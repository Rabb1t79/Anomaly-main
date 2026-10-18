#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kDefaultIntervalMs = 200;
constexpr uint32_t kMinimumIntervalMs = 100;
constexpr uint32_t kMaximumIntervalMs = 1000;
constexpr size_t kMaximumTargetChains = 16;
constexpr size_t kMaximumEventsPerUpdate = 128;
constexpr uint32_t kMaximumReplayFailures = 3;
constexpr auto kChainIdleTimeout = std::chrono::seconds(2);
constexpr auto kConfirmationTimeout = std::chrono::milliseconds(900);
constexpr auto kRetryDelay = std::chrono::milliseconds(250);

struct ChainTask {
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 attacker{};
    AnomalyGenerationHandleV1 victim{};
    uint64_t source_id{};
    uint64_t root_damage_sequence{};
    uint64_t last_seen_damage_sequence{};
    std::string target_label;
    Clock::time_point last_damage_at{};
    Clock::time_point next_trigger_at{};
    Clock::time_point confirmation_deadline{};
    uint64_t confirmed_echoes{};
    uint32_t consecutive_failures{};
    bool waiting_for_confirmation{};
};

struct Context final {
    std::mutex mutex;
    const AnomalyCoreServiceV1* core{};
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteDamageReplayServiceV1* damage_replay{};
    const AnomalyUiServiceV1* ui{};

    std::atomic<int32_t> requested_enabled{-1};
    std::atomic<uint32_t> requested_interval_ms{kDefaultIntervalMs};
    std::atomic<bool> export_requested{false};

    bool enabled{};
    uint32_t interval_ms{kDefaultIntervalMs};
    uint64_t damage_cursor{};
    std::vector<ChainTask> chains;

    uint64_t root_count{};
    uint64_t replay_attempts{};
    uint64_t confirmed_echoes{};
    uint64_t replay_failures{};
    std::string status{"等待玩家造成下一次有效伤害"};
    std::string last_error;
    std::string last_target_label;
    std::string last_export_path;
    std::wstring log_path;
};

template <typename T>
bool HasField(const T* value, size_t field_end) noexcept {
    return value != nullptr && value->struct_size >= field_end;
}

bool ValidHandle(const AnomalyGenerationHandleV1& value) noexcept {
    return value.id != 0 && value.generation != 0;
}

bool SameHandle(
    const AnomalyGenerationHandleV1& left,
    const AnomalyGenerationHandleV1& right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

AnomalyStatusV1 MakeStatus(uint32_t code) noexcept {
    return {code, 0, {nullptr, 0}};
}

std::string StatusMessage(const AnomalyStatusV1& status) {
    if (status.message.data != nullptr && status.message.size != 0) {
        return std::string(status.message.data, status.message.size);
    }
    return "Anomaly status code=" + std::to_string(status.code);
}

void ReportCoreLog(Context& context, std::string_view message) noexcept {
    constexpr size_t kLogFieldEnd =
        offsetof(AnomalyCoreServiceV1, log) + sizeof(AnomalyCoreServiceV1::log);
    if (!HasField(context.core, kLogFieldEnd) || context.core->log == nullptr) return;
    context.core->log(
        context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
        anomaly::sdk::StringView(message));
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty() || value.size() > 32760) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required) != required) {
        return {};
    }
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty() || value.size() > 32760) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required,
            nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

bool InitializeLogPath(Context& context) noexcept {
    try {
        constexpr size_t kDirectoryFieldEnd =
            offsetof(AnomalyCoreServiceV1, plugin_directory) +
            sizeof(AnomalyCoreServiceV1::plugin_directory);

        if (HasField(context.core, kDirectoryFieldEnd) &&
            context.core->plugin_directory != nullptr) {
            size_t required{};
            const auto size_status = context.core->plugin_directory(
                context.core->user, nullptr, &required);
            if ((size_status.code == ANOMALY_STATUS_V1_OK ||
                 size_status.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) &&
                required >= 2 && required <= 32768) {
                std::vector<char> utf8_path(required, '\0');
                size_t capacity = utf8_path.size();
                const auto path_status = context.core->plugin_directory(
                    context.core->user, utf8_path.data(), &capacity);
                if (path_status.code == ANOMALY_STATUS_V1_OK &&
                    capacity >= 2 && capacity <= utf8_path.size() &&
                    utf8_path[capacity - 1] == '\0') {
                    const std::wstring directory =
                        Utf8ToWide(std::string_view(utf8_path.data(), capacity - 1));
                    if (!directory.empty()) {
                        const auto path = std::filesystem::path(directory) /
                            L"damage-chain-reaction.log";
                        std::filesystem::create_directories(path.parent_path());
                        context.log_path = path.wstring();
                        return true;
                    }
                }
            }
        }

        wchar_t temp_path[MAX_PATH + 1]{};
        const DWORD length = GetTempPathW(
            static_cast<DWORD>(std::size(temp_path)), temp_path);
        if (length != 0 && length < std::size(temp_path)) {
            const auto path = std::filesystem::path(temp_path) /
                L"Anomaly" / L"NteDamageChainReaction" /
                L"damage-chain-reaction.log";
            std::filesystem::create_directories(path.parent_path());
            context.log_path = path.wstring();
            return true;
        }
    } catch (...) {
    }

    context.log_path.clear();
    return false;
}

void LogLine(Context& context, std::string_view message) noexcept {
    try {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char timestamp[48]{};
        std::snprintf(
            timestamp, sizeof(timestamp),
            "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            static_cast<unsigned>(now.wYear),
            static_cast<unsigned>(now.wMonth),
            static_cast<unsigned>(now.wDay),
            static_cast<unsigned>(now.wHour),
            static_cast<unsigned>(now.wMinute),
            static_cast<unsigned>(now.wSecond),
            static_cast<unsigned>(now.wMilliseconds));

        std::string line;
        line.reserve(message.size() + 64);
        line += '[';
        line += timestamp;
        line += "] ";
        line.append(message.data(), message.size());
        line += "\r\n";

        if (context.log_path.empty()) {
            ReportCoreLog(context, line);
            return;
        }

        HANDLE file = CreateFileW(
            context.log_path.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            ReportCoreLog(context, "Damage Chain Reaction: cannot open diagnostic log");
            return;
        }

        DWORD written{};
        const DWORD bytes = static_cast<DWORD>(
            (std::min)(line.size(), static_cast<size_t>(0xFFFFFFFFU)));
        const BOOL okay = bytes == 0 ||
            (WriteFile(file, line.data(), bytes, &written, nullptr) != 0 &&
             written == bytes);
        if (okay) FlushFileBuffers(file);
        CloseHandle(file);

        if (!okay) {
            ReportCoreLog(context, "Damage Chain Reaction: cannot write diagnostic log");
        }
    } catch (...) {
        ReportCoreLog(context, "Damage Chain Reaction: exception while writing diagnostic log");
    }
}

bool ExportDiagnosticLog(Context& context, std::wstring& exported_path) noexcept {
    try {
        if (context.log_path.empty() || GetFileAttributesW(context.log_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return false;
        }

        PWSTR documents = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(
            FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents);
        if (FAILED(hr) || documents == nullptr) {
            if (documents != nullptr) CoTaskMemFree(documents);
            return false;
        }

        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t file_name[96]{};
        std::swprintf(
            file_name, std::size(file_name),
            L"damage-chain-reaction-%04u%02u%02u-%02u%02u%02u.log",
            static_cast<unsigned>(now.wYear),
            static_cast<unsigned>(now.wMonth),
            static_cast<unsigned>(now.wDay),
            static_cast<unsigned>(now.wHour),
            static_cast<unsigned>(now.wMinute),
            static_cast<unsigned>(now.wSecond));

        const auto destination = std::filesystem::path(documents) /
            L"Anomaly" / L"NteDamageChainReaction" / file_name;
        std::filesystem::create_directories(destination.parent_path());
        CoTaskMemFree(documents);

        if (!CopyFileW(context.log_path.c_str(), destination.c_str(), FALSE)) {
            return false;
        }
        exported_path = destination.wstring();
        return true;
    } catch (...) {
        return false;
    }
}

bool CombatServiceReady(const AnomalyNteCombatServiceV1* service) noexcept {
    constexpr size_t kCurrentFieldEnd =
        offsetof(AnomalyNteCombatServiceV1, current_combatant) +
        sizeof(AnomalyNteCombatServiceV1::current_combatant);
    constexpr size_t kLatestDamageFieldEnd =
        offsetof(AnomalyNteCombatServiceV1, latest_damage_sequence) +
        sizeof(AnomalyNteCombatServiceV1::latest_damage_sequence);
    constexpr size_t kNextDamageFieldEnd =
        offsetof(AnomalyNteCombatServiceV1, next_damage_event) +
        sizeof(AnomalyNteCombatServiceV1::next_damage_event);
    return HasField(service, kCurrentFieldEnd) &&
        HasField(service, kLatestDamageFieldEnd) &&
        HasField(service, kNextDamageFieldEnd) &&
        service->current_combatant != nullptr &&
        service->latest_damage_sequence != nullptr &&
        service->next_damage_event != nullptr;
}

bool DamageReplayServiceReady(
    const AnomalyNteDamageReplayServiceV1* service) noexcept {
    constexpr size_t kReplayFieldEnd =
        offsetof(AnomalyNteDamageReplayServiceV1, replay_damage) +
        sizeof(AnomalyNteDamageReplayServiceV1::replay_damage);
    return HasField(service, kReplayFieldEnd) && service->replay_damage != nullptr;
}

bool UiServiceReady(const AnomalyUiServiceV1* ui) noexcept {
    constexpr size_t kBeginEnd =
        offsetof(AnomalyUiServiceV1, begin_window) +
        sizeof(AnomalyUiServiceV1::begin_window);
    constexpr size_t kEndEnd =
        offsetof(AnomalyUiServiceV1, end_window) +
        sizeof(AnomalyUiServiceV1::end_window);
    constexpr size_t kTextEnd =
        offsetof(AnomalyUiServiceV1, text) + sizeof(AnomalyUiServiceV1::text);
    constexpr size_t kButtonEnd =
        offsetof(AnomalyUiServiceV1, button) + sizeof(AnomalyUiServiceV1::button);
    constexpr size_t kCheckboxEnd =
        offsetof(AnomalyUiServiceV1, checkbox) + sizeof(AnomalyUiServiceV1::checkbox);
    constexpr size_t kSliderEnd =
        offsetof(AnomalyUiServiceV1, slider_float) +
        sizeof(AnomalyUiServiceV1::slider_float);
    return HasField(ui, kBeginEnd) && HasField(ui, kEndEnd) &&
        HasField(ui, kTextEnd) && HasField(ui, kButtonEnd) &&
        HasField(ui, kCheckboxEnd) && HasField(ui, kSliderEnd) &&
        ui->begin_window != nullptr && ui->end_window != nullptr &&
        ui->text != nullptr && ui->button != nullptr &&
        ui->checkbox != nullptr && ui->slider_float != nullptr;
}

std::string ResolveTargetLabel(
    const AnomalyNteCombatServiceV1* service,
    AnomalyGenerationHandleV1 participant) {
    constexpr size_t kPathFieldEnd =
        offsetof(AnomalyNteCombatServiceV1, participant_path_utf8) +
        sizeof(AnomalyNteCombatServiceV1::participant_path_utf8);
    const std::string fallback = "目标 #" + std::to_string(participant.id);
    if (!HasField(service, kPathFieldEnd) ||
        service->participant_path_utf8 == nullptr) {
        return fallback;
    }

    size_t required{};
    const auto size_status = service->participant_path_utf8(
        service->user, participant, nullptr, &required);
    if ((size_status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL &&
         size_status.code != ANOMALY_STATUS_V1_OK) ||
        required < 2 || required > 4096) {
        return fallback;
    }

    std::vector<char> buffer(required, '\0');
    size_t capacity = buffer.size();
    const auto path_status = service->participant_path_utf8(
        service->user, participant, buffer.data(), &capacity);
    if (path_status.code != ANOMALY_STATUS_V1_OK ||
        capacity < 1 || capacity > buffer.size()) {
        return fallback;
    }

    const size_t text_size = capacity > 0 && buffer[capacity - 1] == '\0'
        ? capacity - 1 : capacity;
    if (text_size == 0) return fallback;
    return std::string(buffer.data(), text_size);
}

bool CurrentCombatant(
    Context& context, AnomalyNteCombatantSnapshotV1& snapshot) noexcept {
    snapshot = {};
    snapshot.struct_size = sizeof(snapshot);
    const auto status = context.combat->current_combatant(
        context.combat->user, &snapshot);
    return status.code == ANOMALY_STATUS_V1_OK &&
        snapshot.struct_size >= sizeof(snapshot) &&
        (snapshot.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 &&
        ValidHandle(snapshot.world) && ValidHandle(snapshot.character);
}

ChainTask* FindChainForTarget(
    Context& context, const AnomalyNteDamageEventV1& event) noexcept {
    for (auto& chain : context.chains) {
        if (SameHandle(chain.world, event.world) &&
            SameHandle(chain.attacker, event.attacker) &&
            SameHandle(chain.victim, event.victim)) {
            return &chain;
        }
    }
    return nullptr;
}

void ProcessDamageEvent(
    Context& context,
    const AnomalyNteDamageEventV1& event,
    const AnomalyNteCombatantSnapshotV1& current,
    const bool current_valid,
    const Clock::time_point now) {
    if (event.struct_size < sizeof(event) ||
        (event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) == 0 ||
        event.final_damage <= 0 ||
        !ValidHandle(event.world) ||
        !ValidHandle(event.attacker) ||
        !ValidHandle(event.victim) ||
        event.attacker.id == event.victim.id) {
        return;
    }

    if (ChainTask* existing = FindChainForTarget(context, event)) {
        if (existing->waiting_for_confirmation &&
            event.sequence > existing->last_seen_damage_sequence &&
            event.source_id == existing->source_id) {
            existing->waiting_for_confirmation = false;
            existing->last_seen_damage_sequence = event.sequence;
            existing->last_damage_at = now;
            existing->next_trigger_at =
                now + std::chrono::milliseconds(context.interval_ms);
            existing->consecutive_failures = 0;
            context.status = "伤害链运行中";
            context.last_error.clear();
            ++existing->confirmed_echoes;
            ++context.confirmed_echoes;
            if (context.confirmed_echoes == 1 ||
                context.confirmed_echoes % 10 == 0) {
                LogLine(context,
                    "confirmed additional damage; root_sequence=" +
                    std::to_string(existing->root_damage_sequence) +
                    " observed_sequence=" + std::to_string(event.sequence) +
                    " target=" + existing->target_label +
                    " total_confirmed=" + std::to_string(context.confirmed_echoes));
            }
        } else if (!existing->waiting_for_confirmation) {
            // A fresh hit on the same target does not reset the root, interval, or chain.
            // It only refreshes the inactivity clock; target tasks remain independent.
            existing->last_damage_at = now;
            existing->last_seen_damage_sequence =
                (std::max)(existing->last_seen_damage_sequence, event.sequence);
        }
        return;
    }

    if (!context.enabled || !current_valid ||
        !SameHandle(event.world, current.world) ||
        !SameHandle(event.attacker, current.character) ||
        event.source_id == 0) {
        return;
    }

    if (context.chains.size() >= kMaximumTargetChains) {
        context.last_error =
            "目标链数量已达上限，未创建新的目标实例；请停用或等待现有实例结束";
        context.status = "目标实例已达上限";
        LogLine(context,
            "chain capacity reached; skipped damage_sequence=" +
            std::to_string(event.sequence) + " target_id=" +
            std::to_string(event.victim.id));
        return;
    }

    ChainTask chain{};
    chain.world = event.world;
    chain.attacker = event.attacker;
    chain.victim = event.victim;
    chain.source_id = event.source_id;
    chain.root_damage_sequence = event.sequence;
    chain.last_seen_damage_sequence = event.sequence;
    chain.target_label = ResolveTargetLabel(context.combat, event.victim);
    chain.last_damage_at = now;
    chain.next_trigger_at =
        now + std::chrono::milliseconds(context.interval_ms);

    context.last_target_label = chain.target_label;
    context.chains.push_back(std::move(chain));
    ++context.root_count;
    context.status = "已捕获真实伤害，正在维持目标链";
    LogLine(context,
        "chain started; damage_sequence=" + std::to_string(event.sequence) +
        " source_id=" + std::to_string(event.source_id) +
        " attacker_id=" + std::to_string(event.attacker.id) +
        " target=" + context.chains.back().target_label +
        " active_targets=" + std::to_string(context.chains.size()));
}

void ApplyUiRequests(Context& context) {
    const int32_t requested = context.requested_enabled.exchange(
        -1, std::memory_order_acq_rel);
    if (requested != -1) {
        const bool next_enabled = requested != 0;
        if (next_enabled != context.enabled) {
            if (next_enabled) {
                context.damage_cursor = context.combat->latest_damage_sequence(
                    context.combat->user);
                context.enabled = true;
                context.status = "已启用，等待下一次玩家伤害";
                context.last_error.clear();
                LogLine(context, "feature enabled; cursor=" +
                    std::to_string(context.damage_cursor));
            } else {
                LogLine(context, "feature disabled; cleared active target chains=" +
                    std::to_string(context.chains.size()));
                context.enabled = false;
                context.chains.clear();
                context.status = "已停用";
            }
        }
    }

    context.interval_ms = (std::clamp)(
        context.requested_interval_ms.load(std::memory_order_acquire),
        kMinimumIntervalMs, kMaximumIntervalMs);

    if (context.export_requested.exchange(false, std::memory_order_acq_rel)) {
        std::wstring exported;
        if (ExportDiagnosticLog(context, exported)) {
            context.last_export_path = WideToUtf8(exported);
            context.status = "诊断日志已导出";
            context.last_error.clear();
            LogLine(context, "diagnostic log exported to=" + context.last_export_path);
        } else {
            context.last_error = "诊断日志导出失败；请检查日志文件是否已生成，以及文档目录是否可写";
            context.status = "诊断日志导出失败";
            LogLine(context, "diagnostic log export failed");
        }
    }
}

void UpdateChains(Context& context, const Clock::time_point now) {
    for (auto it = context.chains.begin(); it != context.chains.end();) {
        if (now - it->last_damage_at >= kChainIdleTimeout) {
            LogLine(context,
                "chain stopped by inactivity; root_sequence=" +
                std::to_string(it->root_damage_sequence) +
                " confirmed_echoes=" + std::to_string(it->confirmed_echoes) +
                " target=" + it->target_label);
            if (context.chains.size() == 1) {
                context.status = "等待下一次玩家伤害";
            }
            it = context.chains.erase(it);
            continue;
        }

        if (it->waiting_for_confirmation) {
            if (now >= it->confirmation_deadline) {
                ++context.replay_failures;
                context.last_error =
                    "Core 已接受原生效果调用，但超时未观察到匹配的新伤害事件；已停止该目标链";
                context.status = "未确认追加伤害，已停止该目标链";
                LogLine(context,
                    "confirmation timeout; root_sequence=" +
                    std::to_string(it->root_damage_sequence) +
                    " target=" + it->target_label +
                    " expected_source_id=" + std::to_string(it->source_id));
                it = context.chains.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        if (now < it->next_trigger_at) {
            ++it;
            continue;
        }

        AnomalyNteDamageReplayRequestV1 request{};
        request.struct_size = sizeof(request);
        request.flags = 0;
        request.damage_sequence = it->root_damage_sequence;

        ++context.replay_attempts;
        // The cursor floor is sampled immediately before dispatch so a queued hit that
        // predates this invocation cannot be mistaken for its confirmation.
        it->last_seen_damage_sequence = context.combat->latest_damage_sequence(
            context.combat->user);
        const auto replay_status = context.damage_replay->replay_damage(
            context.damage_replay->user, &request);
        if (replay_status.code == ANOMALY_STATUS_V1_OK) {
            // Do not count the call return as a hit. The task pauses until a newer
            // matching CharacterOnDamaged event confirms actual additional damage.
            it->waiting_for_confirmation = true;
            it->confirmation_deadline = now + kConfirmationTimeout;
            ++it;
            continue;
        }

        ++context.replay_failures;
        ++it->consecutive_failures;
        context.last_error = StatusMessage(replay_status);
        context.status = "原生伤害链调用被 Core 拒绝";
        LogLine(context,
            "replay rejected; status_code=" + std::to_string(replay_status.code) +
            " root_sequence=" + std::to_string(it->root_damage_sequence) +
            " target=" + it->target_label +
            " consecutive_failures=" + std::to_string(it->consecutive_failures) +
            " reason=" + context.last_error);

        if (it->consecutive_failures >= kMaximumReplayFailures) {
            LogLine(context,
                "chain stopped after repeated Core rejection; root_sequence=" +
                std::to_string(it->root_damage_sequence) +
                " target=" + it->target_label);
            it = context.chains.erase(it);
            continue;
        }

        it->next_trigger_at = now + kRetryDelay;
        ++it;
    }

    if (context.enabled && context.chains.empty() &&
        context.status != "原生伤害链调用被 Core 拒绝" &&
        context.status != "未确认追加伤害，已停止该目标链" &&
        context.status != "目标实例已达上限" &&
        context.status != "诊断日志导出失败") {
        context.status = "等待下一次玩家伤害";
    } else if (!context.chains.empty() && context.status == "等待下一次玩家伤害") {
        context.status = "伤害链运行中";
    }
}

void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;

    try {
        std::scoped_lock lock(context->mutex);
        ApplyUiRequests(*context);
        if (!context->enabled) return;

        AnomalyNteCombatantSnapshotV1 current{};
        const bool current_valid = CurrentCombatant(*context, current);
        const auto now = Clock::now();

        for (size_t i = 0; i < kMaximumEventsPerUpdate; ++i) {
            AnomalyNteDamageEventV1 event{};
            event.struct_size = sizeof(event);
            const uint64_t previous_cursor = context->damage_cursor;
            const auto event_status = context->combat->next_damage_event(
                context->combat->user, previous_cursor, &event);

            if (event_status.code == ANOMALY_STATUS_V1_NOT_FOUND) break;
            if (event_status.code != ANOMALY_STATUS_V1_OK) {
                context->last_error = StatusMessage(event_status);
                context->status = "读取原生伤害事件失败";
                LogLine(*context, "next_damage_event failed; cursor=" +
                    std::to_string(previous_cursor) +
                    " reason=" + context->last_error);
                break;
            }
            if (event.struct_size < sizeof(event) ||
                event.sequence <= previous_cursor) {
                context->last_error = "原生伤害事件的结构长度或序列号无效";
                context->status = "伤害事件校验失败";
                LogLine(*context, "invalid damage event returned by combat service");
                break;
            }

            context->damage_cursor = event.sequence;
            ProcessDamageEvent(*context, event, current, current_valid, now);
        }

        UpdateChains(*context, now);
    } catch (...) {
        try {
            std::scoped_lock lock(context->mutex);
            context->last_error = "插件 Update 发生未处理异常，已记录诊断日志";
            context->status = "插件更新异常";
            LogLine(*context, "exception escaped Update processing");
        } catch (...) {
        }
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return MakeStatus(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }

    auto* context = new (std::nothrow) Context{};
    if (context == nullptr) return MakeStatus(ANOMALY_STATUS_V1_FAILED);

    const anomaly::sdk::Host sdk_host(host);
    context->core = sdk_host.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
    context->combat = sdk_host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
        ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    context->damage_replay = sdk_host.Query<AnomalyNteDamageReplayServiceV1>(
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_ID,
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_VERSION).get();
    context->ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();

    InitializeLogPath(*context);
    LogLine(*context, "plugin load; initial state created");

    if (!CombatServiceReady(context->combat) ||
        !DamageReplayServiceReady(context->damage_replay) ||
        !UiServiceReady(context->ui)) {
        LogLine(*context,
            "load failed: required NTE combat, native damage-replay, or UI service unavailable");
        delete context;
        return MakeStatus(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    context->chains.reserve(kMaximumTargetChains);
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return MakeStatus(ANOMALY_STATUS_V1_INVALID_ARGUMENT);

    std::scoped_lock lock(context->mutex);
    context->enabled = false;
    context->chains.clear();
    context->root_count = 0;
    context->replay_attempts = 0;
    context->confirmed_echoes = 0;
    context->replay_failures = 0;
    context->last_error.clear();
    context->last_target_label.clear();
    context->last_export_path.clear();
    context->interval_ms = kDefaultIntervalMs;
    context->requested_interval_ms.store(kDefaultIntervalMs, std::memory_order_release);
    context->export_requested.store(false, std::memory_order_release);
    context->requested_enabled.store(1, std::memory_order_release);
    context->status = "正在启用，等待玩家伤害";
    LogLine(*context, "plugin start requested");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return MakeStatus(ANOMALY_STATUS_V1_INVALID_ARGUMENT);

    std::scoped_lock lock(context->mutex);
    context->requested_enabled.store(0, std::memory_order_release);
    context->enabled = false;
    context->chains.clear();
    context->status = "已停用";
    LogLine(*context, "plugin stopped; active chains cleared");
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    LogLine(*context, "plugin unload");
    delete context;
}

void ANOMALY_CALL Draw(
    void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || !UiServiceReady(ui)) return;

    std::scoped_lock lock(context->mutex);
    if (ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 390.0F, 0.0F, 4U);
    }

    int open = 1;
    anomaly::sdk::UiWindow window(ui, "伤害链式反应", &open);
    if (!window) return;

    int enabled = context->enabled ? 1 : 0;
    if (ui->checkbox(ui->user, anomaly::sdk::StringView("启用"), &enabled) != 0) {
        context->requested_enabled.store(enabled != 0 ? 1 : 0, std::memory_order_release);
    }

    float interval = static_cast<float>(context->interval_ms);
    if (ui->slider_float(
            ui->user, anomaly::sdk::StringView("连锁间隔（毫秒）"),
            &interval, static_cast<float>(kMinimumIntervalMs),
            static_cast<float>(kMaximumIntervalMs)) != 0) {
        const auto value = static_cast<uint32_t>(
            (std::clamp)(interval, static_cast<float>(kMinimumIntervalMs),
                static_cast<float>(kMaximumIntervalMs)) + 0.5F);
        context->requested_interval_ms.store(value, std::memory_order_release);
    }

    ui->text(ui->user, anomaly::sdk::StringView("状态："));
    ui->text(ui->user, anomaly::sdk::StringView(context->status));
    ui->text(ui->user, anomaly::sdk::StringView(
        "独立目标实例：" + std::to_string(context->chains.size())));
    ui->text(ui->user, anomaly::sdk::StringView(
        "已确认追加伤害：" + std::to_string(context->confirmed_echoes)));
    ui->text(ui->user, anomaly::sdk::StringView(
        "原生调用尝试：" + std::to_string(context->replay_attempts)));
    ui->text(ui->user, anomaly::sdk::StringView(
        "调用失败次数：" + std::to_string(context->replay_failures)));

    if (!context->last_target_label.empty()) {
        ui->text(ui->user, anomaly::sdk::StringView("最近目标：" + context->last_target_label));
    }
    if (!context->last_export_path.empty()) {
        ui->text(ui->user, anomaly::sdk::StringView("日志副本：" + context->last_export_path));
    }

    if (ui->button(ui->user,
            anomaly::sdk::StringView("导出诊断日志"),
            0.0F, 0.0F) != 0) {
        context->export_requested.store(true, std::memory_order_release);
    }

    bool show_diagnostics = false;
    constexpr size_t kDeveloperModeFieldEnd =
        offsetof(AnomalyUiServiceV1, developer_mode_enabled) +
        sizeof(AnomalyUiServiceV1::developer_mode_enabled);
    if (HasField(ui, kDeveloperModeFieldEnd) &&
        ui->developer_mode_enabled != nullptr) {
        show_diagnostics = ui->developer_mode_enabled(ui->user) != 0;
    }
    if (show_diagnostics) {
        if (ui->separator != nullptr) ui->separator(ui->user);
        ui->text(ui->user, anomaly::sdk::StringView("调试诊断（开发者模式）"));
        ui->text(ui->user, anomaly::sdk::StringView(
            "最近伤害序列：" + std::to_string(context->damage_cursor)));
        ui->text(ui->user, anomaly::sdk::StringView(
            "已捕获链根数：" + std::to_string(context->root_count)));
        if (!context->last_error.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView("最近错误：" + context->last_error));
        }
        if (!context->log_path.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "日志位置：" + WideToUtf8(context->log_path)));
        }
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return MakeStatus(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }

    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.tools.nte-damage-chain-reaction"),
        anomaly::sdk::StringView("NTE Damage Chain Reaction"),
        anomaly::sdk::StringView("Rabb1t79"),
        anomaly::sdk::StringView("0.1.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw
    };
    return anomaly::sdk::Ok();
}
