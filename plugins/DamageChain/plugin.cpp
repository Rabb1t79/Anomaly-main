/*
 * Damage Chain Reaction
 *
 * Captures original CharacterOnDamaged events and asks Core to dispatch a sequence
 * of individually validated native GameplayEffect applications. The count is the
 * number of additional applications; rate controls spacing and is not a duration.
 * The UI consumes only Core snapshots; all queue mutation is requested from Update
 * in the Game callback domain.
 */
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kVisibleChainRows = ANOMALY_NTE_DAMAGE_CHAIN_V1_MAX_ACTIVE_CHAINS;
constexpr std::size_t kMaximumEventsDrainedPerUpdate = 64;
constexpr std::uint32_t kLogProgressStep = 10;

struct ChainRow {
    std::uint64_t id{};
    std::uint64_t root_damage_sequence{};
    std::uint32_t state{ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_EMPTY};
    std::uint32_t requested{};
    std::uint32_t dispatched{};
    std::uint32_t observed{};
    std::uint32_t rate{};
    std::uint32_t last_status{ANOMALY_STATUS_V1_OK};
    std::uint32_t last_logged_dispatch{};
    bool terminal_logged{};
    std::array<char, ANOMALY_NTE_DAMAGE_CHAIN_V1_STATUS_MESSAGE_CAPACITY> message{};
};

struct Context {
    std::mutex mutex;
    const AnomalyCoreServiceV1* core{};
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteDamageChainServiceV1* chain{};
    const AnomalyUiServiceV1* ui{};
    std::wstring log_path;
    std::string log_error;

    bool enabled{};
    bool was_enabled{};
    std::uint32_t repeat_count{100};
    std::uint32_t rate_per_second{100};
    std::uint64_t damage_cursor{};
    std::uint64_t latest_sequence{};
    std::array<ChainRow, kVisibleChainRows> rows{};
    std::string status{"持续监测原生伤害；链式反应尚未启用"};
    std::uint32_t last_start_failure_code{ANOMALY_STATUS_V1_OK};
    std::uint32_t consecutive_start_failures{};
    std::atomic_bool export_snapshot_requested{false};
};

AnomalyStatusV1 Status(
    const std::uint32_t code, const std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool SameHandle(
    const AnomalyGenerationHandleV1 a,
    const AnomalyGenerationHandleV1 b) noexcept {
    return a.id == b.id && a.generation == b.generation;
}

bool CombatReady(const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
               decltype(AnomalyNteCombatServiceV1::participant_path_utf8)>(
               service, offsetof(AnomalyNteCombatServiceV1, participant_path_utf8)) &&
        service->current_combatant != nullptr &&
        service->latest_damage_sequence != nullptr &&
        service->next_damage_event != nullptr &&
        service->participant_path_utf8 != nullptr;
}

bool ChainReady(const AnomalyNteDamageChainServiceV1* service) noexcept {
    return HasField<AnomalyNteDamageChainServiceV1,
               decltype(AnomalyNteDamageChainServiceV1::cancel_chain)>(
               service, offsetof(AnomalyNteDamageChainServiceV1, cancel_chain)) &&
        service->start_chain != nullptr &&
        service->snapshot != nullptr &&
        service->cancel_chain != nullptr;
}

bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return HasField<AnomalyUiServiceV1,
               decltype(AnomalyUiServiceV1::input_uint32)>(
               service, offsetof(AnomalyUiServiceV1, input_uint32)) &&
        service->begin_window != nullptr &&
        service->end_window != nullptr &&
        service->text != nullptr &&
        service->checkbox != nullptr &&
        service->button != nullptr &&
        service->input_uint32 != nullptr;
}

const char* StatusName(const std::uint32_t code) noexcept {
    switch (code) {
    case ANOMALY_STATUS_V1_OK: return "OK";
    case ANOMALY_STATUS_V1_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "UNAVAILABLE";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "NOT_FOUND";
    case ANOMALY_STATUS_V1_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case ANOMALY_STATUS_V1_FAILED: return "FAILED";
    case ANOMALY_STATUS_V1_TIMEOUT: return "TIMEOUT";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ANOMALY_STATUS_V1_CONFLICT: return "CONFLICT";
    case ANOMALY_STATUS_V1_CANCELLED: return "CANCELLED";
    default: return "UNKNOWN_STATUS";
    }
}

const char* ChainStateName(const std::uint32_t state) noexcept {
    switch (state) {
    case ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_RUNNING: return "运行中";
    case ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_COMPLETED: return "已派发完";
    case ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_FAILED: return "失败停止";
    case ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_CANCELLED: return "已取消";
    default: return "未知";
    }
}

std::string StatusMessage(const AnomalyStatusV1 status) {
    if (status.message.data != nullptr && status.message.size != 0) {
        return std::string(status.message.data, status.message.size);
    }
    return StatusName(status.code);
}

std::string TimestampUtc() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char value[40]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds));
    return value;
}

void ReportCoreError(Context& context, const std::string_view message) noexcept {
    const auto* core = context.core;
    constexpr std::size_t field_end =
        offsetof(AnomalyCoreServiceV1, log) + sizeof(AnomalyCoreServiceV1::log);
    if (core == nullptr || core->struct_size < field_end || core->log == nullptr) return;
    try {
        core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            anomaly::sdk::StringView(message));
    } catch (...) {
    }
}

bool InitializeLog(Context& context) noexcept {
    try {
        constexpr std::size_t field_end =
            offsetof(AnomalyCoreServiceV1, plugin_directory) +
            sizeof(AnomalyCoreServiceV1::plugin_directory);
        if (context.core == nullptr || context.core->struct_size < field_end ||
            context.core->plugin_directory == nullptr) {
            context.log_error = "Core plugin_directory API is unavailable";
            ReportCoreError(context, context.log_error);
            return false;
        }
        std::size_t required{};
        const auto sizing = context.core->plugin_directory(
            context.core->user, nullptr, &required);
        if (sizing.code != ANOMALY_STATUS_V1_OK || required < 2 || required > 32768) {
            context.log_error = "Cannot query plugin package directory; Core status=" +
                std::to_string(sizing.code);
            ReportCoreError(context, context.log_error);
            return false;
        }
        std::vector<char> utf8(required, '\0');
        std::size_t capacity = utf8.size();
        const auto read = context.core->plugin_directory(
            context.core->user, utf8.data(), &capacity);
        if (read.code != ANOMALY_STATUS_V1_OK || capacity < 2 ||
            capacity > utf8.size() || utf8[capacity - 1] != '\0') {
            context.log_error = "Core plugin package directory read failed; status=" +
                std::to_string(read.code);
            ReportCoreError(context, context.log_error);
            return false;
        }
        const int wide_count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), -1, nullptr, 0);
        if (wide_count < 2 || wide_count > 32768) {
            context.log_error = "Core plugin package directory is not valid UTF-8";
            ReportCoreError(context, context.log_error);
            return false;
        }
        std::wstring directory(static_cast<std::size_t>(wide_count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), -1,
                directory.data(), wide_count) != wide_count) {
            context.log_error = "Failed to convert the plugin package directory to UTF-16";
            ReportCoreError(context, context.log_error);
            return false;
        }
        directory.resize(static_cast<std::size_t>(wide_count - 1));
        while (!directory.empty() &&
            (directory.back() == L'\\' || directory.back() == L'/')) {
            directory.pop_back();
        }
        context.log_path = directory + L"\\damage-chain.log";
        return true;
    } catch (...) {
        context.log_error = "Exception while initializing the damage-chain log path";
        ReportCoreError(context, context.log_error);
        return false;
    }
}

void LogLine(Context& context, const std::string_view message) noexcept {
    if (context.log_path.empty()) {
        ReportCoreError(context, context.log_error.empty()
            ? "Damage Chain: diagnostic log path has not been initialized"
            : context.log_error);
        return;
    }
    try {
        const std::string line = TimestampUtc() + " " + std::string(message) + "\r\n";
        HANDLE file = CreateFileW(context.log_path.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            context.log_error = "Cannot open damage-chain.log; Win32 error=" +
                std::to_string(GetLastError());
            ReportCoreError(context, context.log_error);
            return;
        }
        DWORD written{};
        const auto length = static_cast<DWORD>(
            (std::min)(line.size(), static_cast<std::size_t>(0xFFFFFFFFU)));
        const BOOL success = WriteFile(file, line.data(), length, &written, nullptr) != 0 &&
            written == length;
        const DWORD error = success ? ERROR_SUCCESS : GetLastError();
        if (success) FlushFileBuffers(file);
        CloseHandle(file);
        if (!success) {
            context.log_error = "Cannot write damage-chain.log; Win32 error=" +
                std::to_string(error);
            ReportCoreError(context, context.log_error);
        }
    } catch (...) {
        ReportCoreError(context, "Damage Chain: exception while writing diagnostic log");
    }
}

ChainRow* FindRow(Context& context, const std::uint64_t id) noexcept {
    for (auto& row : context.rows) {
        if (id != 0 && row.id == id) return &row;
    }
    return nullptr;
}

bool IsTerminalState(const std::uint32_t state) noexcept {
    return state == ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_COMPLETED ||
        state == ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_FAILED ||
        state == ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_CANCELLED;
}

ChainRow* FindReusableRow(Context& context) noexcept {
    for (auto& row : context.rows) {
        if (row.id == 0 || IsTerminalState(row.state)) return &row;
    }
    return nullptr;
}

void RefreshChainRows(Context& context) noexcept {
    if (!ChainReady(context.chain)) return;
    for (auto& row : context.rows) {
        if (row.id == 0) continue;
        AnomalyNteDamageChainSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        const auto result = context.chain->snapshot(
            context.chain->user, row.id, &snapshot);
        if (result.code != ANOMALY_STATUS_V1_OK) {
            if (row.state == ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_RUNNING) {
                row.state = ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_FAILED;
                row.last_status = result.code;
                const std::string message = "Core chain snapshot unavailable: " +
                    StatusMessage(result);
                std::memset(row.message.data(), 0, row.message.size());
                std::memcpy(row.message.data(), message.data(),
                    (std::min)(message.size(), row.message.size() - 1U));
                if (!row.terminal_logged) {
                    LogLine(context, "chain_id=" + std::to_string(row.id) +
                        " state=FAILED status=" + message);
                    row.terminal_logged = true;
                }
            }
            continue;
        }
        const auto previous_state = row.state;
        row.state = snapshot.state;
        row.root_damage_sequence = snapshot.root_damage_sequence;
        row.requested = snapshot.requested_repeat_count;
        row.dispatched = snapshot.dispatched_repeat_count;
        row.observed = snapshot.observed_chain_damage_count;
        row.rate = snapshot.rate_per_second;
        row.last_status = snapshot.last_status_code;
        std::memcpy(row.message.data(), snapshot.status_message, row.message.size());
        row.message.back() = '\0';

        if (row.dispatched != row.last_logged_dispatch &&
            (row.dispatched >= row.requested ||
             (row.dispatched != 0 && row.dispatched % kLogProgressStep == 0))) {
            LogLine(context,
                "chain_id=" + std::to_string(row.id) +
                " progress_dispatched=" + std::to_string(row.dispatched) + "/" +
                std::to_string(row.requested) +
                " observed_damage_events=" + std::to_string(row.observed));
            row.last_logged_dispatch = row.dispatched;
        }
        if (IsTerminalState(row.state) && !row.terminal_logged) {
            LogLine(context,
                "chain_id=" + std::to_string(row.id) +
                " state=" + ChainStateName(row.state) +
                " status_code=" + std::to_string(row.last_status) +
                " dispatched=" + std::to_string(row.dispatched) + "/" +
                std::to_string(row.requested) +
                " observed_damage_events=" + std::to_string(row.observed) +
                " message=" + std::string(row.message.data()));
            row.terminal_logged = true;
        }
        if (previous_state != row.state && row.state == ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_RUNNING) {
            row.terminal_logged = false;
        }
    }
}

void ExportSnapshot(Context& context) noexcept {
    LogLine(context, "diagnostic_snapshot enabled=" +
        std::string(context.enabled ? "1" : "0") +
        " repeat_count=" + std::to_string(context.repeat_count) +
        " rate_per_second=" + std::to_string(context.rate_per_second) +
        " damage_cursor=" + std::to_string(context.damage_cursor) +
        " latest_damage_sequence=" + std::to_string(context.latest_sequence));
    for (const auto& row : context.rows) {
        if (row.id == 0) continue;
        LogLine(context, "snapshot chain_id=" + std::to_string(row.id) +
            " root_sequence=" + std::to_string(row.root_damage_sequence) +
            " state=" + ChainStateName(row.state) +
            " dispatched=" + std::to_string(row.dispatched) + "/" +
            std::to_string(row.requested) +
            " observed_damage_events=" + std::to_string(row.observed) +
            " rate_per_second=" + std::to_string(row.rate) +
            " last_status=" + std::to_string(row.last_status) +
            " message=" + std::string(row.message.data()));
    }
}

void CancelRunningChains(Context& context) noexcept {
    for (auto& row : context.rows) {
        if (row.id == 0 ||
            row.state != ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_RUNNING) continue;
        const auto result = context.chain->cancel_chain(context.chain->user, row.id);
        LogLine(context, "chain_id=" + std::to_string(row.id) +
            " cancel_status=" + StatusName(result.code) +
            " detail=" + StatusMessage(result));
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "invalid plugin load arguments");
    }
    *plugin_context = nullptr;
    auto* context = new (std::nothrow) Context{};
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED, "allocation failed");

    const anomaly::sdk::Host sdk_host(host);
    context->core = sdk_host.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
    context->combat = sdk_host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    context->chain = sdk_host.Query<AnomalyNteDamageChainServiceV1>(
        ANOMALY_NTE_DAMAGE_CHAIN_SERVICE_V1_ID,
        ANOMALY_NTE_DAMAGE_CHAIN_SERVICE_V1_VERSION).get();
    context->ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();
    InitializeLog(*context);

    if (!CombatReady(context->combat) || !ChainReady(context->chain) || !UiReady(context->ui)) {
        LogLine(*context, "plugin load failed: combat, damage-chain, or UI service ABI unavailable");
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            "Damage Chain requires validated native damage events, the queued damage-chain service, and UI");
    }
    LogLine(*context,
        "plugin loaded; behavior=one real damage capture starts an extra-hit queue; count controls total repeats; rate controls spacing only; no wall-clock expiry");
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(context->mutex);
    context->enabled = false;
    context->was_enabled = false;
    context->repeat_count = (std::clamp)(context->repeat_count, 1U,
        ANOMALY_NTE_DAMAGE_CHAIN_V1_MAX_REPEAT_COUNT);
    context->rate_per_second = (std::clamp)(context->rate_per_second, 1U,
        ANOMALY_NTE_DAMAGE_CHAIN_V1_MAX_RATE_PER_SECOND);
    context->damage_cursor = context->combat->latest_damage_sequence(context->combat->user);
    context->latest_sequence = context->damage_cursor;
    context->status = "持续监测原生伤害；链式反应尚未启用";
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(context->mutex);
    context->enabled = false;
    context->was_enabled = false;
    CancelRunningChains(*context);
    context->status = "插件已停止；未派发的队列已取消";
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    std::scoped_lock lock(context->mutex);

    if (context->combat->latest_damage_sequence != nullptr) {
        context->latest_sequence = context->combat->latest_damage_sequence(context->combat->user);
    }
    // Refresh terminal rows before consuming new native events so a completed chain's
    // diagnostic slot can be reused immediately rather than dropping the next root hit.
    RefreshChainRows(*context);
    if (context->was_enabled && !context->enabled) {
        CancelRunningChains(*context);
        context->status = "链式反应已停用：现有队列已取消，伤害监测继续";
    }
    context->was_enabled = context->enabled;

    if (context->export_snapshot_requested.exchange(false, std::memory_order_acq_rel)) {
        ExportSnapshot(*context);
        context->status = "诊断快照已追加到 damage-chain.log";
    }

    for (std::size_t drained{}; drained < kMaximumEventsDrainedPerUpdate; ++drained) {
        AnomalyNteDamageEventV1 event{};
        event.struct_size = sizeof(event);
        const auto read = context->combat->next_damage_event(
            context->combat->user, context->damage_cursor, &event);
        if (read.code == ANOMALY_STATUS_V1_NOT_FOUND) break;
        if (read.code != ANOMALY_STATUS_V1_OK) {
            context->status = "读取原生伤害事件失败：" + StatusMessage(read);
            LogLine(*context, "damage_event_read_failed status=" + StatusMessage(read));
            break;
        }
        if (event.sequence == 0 || event.sequence <= context->damage_cursor) {
            context->status = "Core 伤害事件序列异常；本轮已停止读取以避免循环";
            LogLine(*context, context->status);
            break;
        }
        context->damage_cursor = event.sequence;
        if (!context->enabled ||
            (event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) == 0 ||
            (event.flags & ANOMALY_NTE_DAMAGE_V1_CHAIN_GENERATED) != 0 ||
            event.attacker.id == 0 || event.victim.id == 0) {
            continue;
        }

        // Core repeats only captured player-control characters; this read-only identity
        // check avoids creating noisy rejected requests for NPC-versus-NPC damage.
        AnomalyNteCombatantSnapshotV1 current{};
        current.struct_size = sizeof(current);
        const auto current_status = context->combat->current_combatant(
            context->combat->user, &current);
        if (current_status.code != ANOMALY_STATUS_V1_OK ||
            (current.flags & ANOMALY_NTE_COMBATANT_V1_VALID) == 0 ||
            !SameHandle(event.attacker, current.character)) {
            continue;
        }

        auto* row = FindReusableRow(*context);
        if (row == nullptr) {
            context->status = "32 个诊断记录槽均被活动链占用；本次未提交新链";
            LogLine(*context, "chain_start_skipped reason=all_plugin_tracking_slots_active root_sequence=" +
                std::to_string(event.sequence));
            continue;
        }
        AnomalyNteDamageChainRequestV1 request{};
        request.struct_size = sizeof(request);
        request.damage_sequence = event.sequence;
        request.repeat_count = context->repeat_count;
        request.rate_per_second = context->rate_per_second;
        std::uint64_t chain_id{};
        const auto started = context->chain->start_chain(
            context->chain->user, &request, &chain_id);
        if (started.code != ANOMALY_STATUS_V1_OK || chain_id == 0) {
            context->status = "未启动链式反应：" + StatusMessage(started);
            if (context->last_start_failure_code != started.code ||
                context->consecutive_start_failures == 0 ||
                context->consecutive_start_failures % 10U == 0) {
                LogLine(*context, "chain_start_failed root_sequence=" +
                    std::to_string(event.sequence) +
                    " requested_repeats=" + std::to_string(request.repeat_count) +
                    " rate_per_second=" + std::to_string(request.rate_per_second) +
                    " status_code=" + std::to_string(started.code) +
                    " detail=" + StatusMessage(started));
            }
            context->last_start_failure_code = started.code;
            ++context->consecutive_start_failures;
            continue;
        }

        *row = {};
        row->id = chain_id;
        row->root_damage_sequence = event.sequence;
        row->state = ANOMALY_NTE_DAMAGE_CHAIN_STATE_V1_RUNNING;
        row->requested = request.repeat_count;
        row->dispatched = 1; // StartChain only succeeds after the first repeat dispatch returns.
        row->observed = 0;
        row->rate = request.rate_per_second;
        row->last_status = ANOMALY_STATUS_V1_OK;
        row->last_logged_dispatch = 1;
        const auto source_message = "链已启动 chain_id=" + std::to_string(chain_id) +
            " root_damage_sequence=" + std::to_string(event.sequence) +
            " original_damage=" + std::to_string(event.final_damage) +
            " repeat_count=" + std::to_string(request.repeat_count) +
            " rate_per_second=" + std::to_string(request.rate_per_second) +
            "; time limit=none";
        std::memcpy(row->message.data(), source_message.data(),
            (std::min)(source_message.size(), row->message.size() - 1U));
        LogLine(*context, source_message);
        context->status = "已捕获一次真实玩家伤害，并已提交独立的后续伤害队列";
        context->consecutive_start_failures = 0;
        context->last_start_failure_code = ANOMALY_STATUS_V1_OK;
    }

}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || !UiReady(ui)) return;
    std::scoped_lock lock(context->mutex);

    int open = 1;
    if (ui->begin_window(ui->user, anomaly::sdk::StringView("伤害链式反应"), &open, 0) != 0) {
        int enabled = context->enabled ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView("启用伤害链式反应"), &enabled) != 0) {
            context->enabled = enabled != 0;
            context->status = context->enabled
                ? "已启用：等待下一次原生玩家伤害事件"
                : "正在停止：Update 将取消尚未派发的队列";
        }

        ui->text(ui->user, anomaly::sdk::StringView(
            "行为：单次真实伤害捕捉 → 排队追加 GameplayEffect → 由游戏原生伤害路径结算"));
        if (ui->input_uint32 != nullptr) {
            ui->input_uint32(ui->user, anomaly::sdk::StringView("额外伤害次数（不含首次原生伤害）"),
                &context->repeat_count, 1U, ANOMALY_NTE_DAMAGE_CHAIN_V1_MAX_REPEAT_COUNT);
            ui->input_uint32(ui->user, anomaly::sdk::StringView("触发速率（次/秒，仅决定间隔）"),
                &context->rate_per_second, 1U,
                ANOMALY_NTE_DAMAGE_CHAIN_V1_MAX_RATE_PER_SECOND);
        }
        context->repeat_count = (std::clamp)(context->repeat_count, 1U,
            ANOMALY_NTE_DAMAGE_CHAIN_V1_MAX_REPEAT_COUNT);
        context->rate_per_second = (std::clamp)(context->rate_per_second, 1U,
            ANOMALY_NTE_DAMAGE_CHAIN_V1_MAX_RATE_PER_SECOND);

        if (ui->separator != nullptr) ui->separator(ui->user);
        ui->text(ui->user, anomaly::sdk::StringView(context->status));
        ui->text(ui->user, anomaly::sdk::StringView(
            "次数控制总量，速率控制节奏；没有 1 秒或其他墙钟期限。游戏线程迟延只积压，不丢弃剩余次数。"));
        ui->text(ui->user, anomaly::sdk::StringView(
            "派发成功不等于伤害已生效；下方分别显示 Core 原生调用派发数和观察到的真实伤害事件数。"));

        const auto latest_line = "Core 伤害事件序列：" +
            std::to_string(context->latest_sequence) +
            "；插件已消费至：" + std::to_string(context->damage_cursor);
        ui->text(ui->user, anomaly::sdk::StringView(latest_line));
        if (ui->button(ui->user, anomaly::sdk::StringView("导出当前诊断快照"), 0.0F, 0.0F) != 0) {
            context->export_snapshot_requested.store(true, std::memory_order_release);
        }
        ui->text(ui->user, anomaly::sdk::StringView(
            context->log_path.empty()
                ? (context->log_error.empty() ? "日志路径尚未准备好" : context->log_error)
                : "诊断日志：damage-chain.log（位于本插件目录）"));

        for (const auto& row : context->rows) {
            if (row.id == 0) continue;
            std::string progress = "链 #" + std::to_string(row.id) +
                "｜额外派发 " + std::to_string(row.dispatched) + "/" +
                std::to_string(row.requested) +
                "｜观察到真实伤害 " + std::to_string(row.observed) +
                "｜速率 " + std::to_string(row.rate) + "/秒｜" +
                ChainStateName(row.state);
            ui->text(ui->user, anomaly::sdk::StringView(progress));
            if (row.message[0] != '\0') {
                ui->text(ui->user, anomaly::sdk::StringView(std::string(row.message.data())));
            }
        }
    }
    ui->end_window(ui->user);
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "invalid plugin descriptor");
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.damage-chain"),
        anomaly::sdk::StringView("NTE Damage Chain"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.0.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw,
    };
    return anomaly::sdk::Ok();
}
