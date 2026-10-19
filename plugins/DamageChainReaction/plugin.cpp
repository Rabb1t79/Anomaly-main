#include "anomaly/sdk/cpp.hpp"
#include "plugins/DamageChainReaction/rate_queue.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kDefaultExtraCount = 1;
constexpr float kDefaultRate = 100.0F;
constexpr std::uint32_t kMinimumExtraCount = 1;
constexpr std::uint32_t kMaximumExtraCount = 1000000;
constexpr float kMinimumRate = 0.1F;
constexpr float kMaximumRate = 1000.0F;
constexpr std::size_t kMaximumLogEntries = 200;
constexpr std::size_t kMaximumChainsKept = 96;
constexpr std::size_t kMaximumEventsDrainedPerUpdate = 64;
constexpr auto kRetryDelay = std::chrono::milliseconds(250);
constexpr char kLogPath[] = "damage-chain-reaction.log";

using Clock = anomaly::damage_chain::RateQueue::Clock;

struct ChainItem {
    AnomalyGenerationHandleV1 handle{};
    AnomalyGenerationHandleV1 target{};
    std::uint64_t root_sequence{};
    std::uint32_t requested_count{};
    std::uint32_t confirmed_seen{};
    std::uint32_t dispatch_seen{};
    std::uint32_t core_state{ANOMALY_NTE_DAMAGE_CHAIN_REACTION_STATE_V1_UNKNOWN};
    anomaly::damage_chain::RateQueue queue;
    bool awaiting_confirmation{};
    bool stop_sent{};
    bool terminal{};
    bool failure_counted{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};
    std::mutex mutex;
    bool enabled{};
    bool export_requested{};
    bool export_in_flight{};
    AnomalyGenerationHandleV1 export_task{};
    bool cursor_initialized{};
    std::uint32_t extra_count{kDefaultExtraCount};
    float rate{kDefaultRate};
    std::uint64_t damage_cursor{};
    std::uint64_t root_hit_count{};
    std::uint64_t confirmed_extra_count{};
    std::uint64_t failure_count{};
    std::uint64_t dispatch_count{};
    std::string status{"等待 Anomaly 原生伤害服务"};
    std::string export_status{"尚未导出"};
    std::vector<ChainItem> chains;
    std::vector<std::string> log;
} g;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr &&
        value->struct_size >= offset + sizeof(Field);
}

bool SameHandle(
    const AnomalyGenerationHandleV1 left,
    const AnomalyGenerationHandleV1 right) noexcept {
    return left.id != 0 && left.generation != 0 &&
        left.id == right.id && left.generation == right.generation;
}

const char* StatusName(const std::uint32_t status) noexcept {
    switch (status) {
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

std::string StatusDetail(const AnomalyStatusV1& status) {
    if (status.message.data == nullptr || status.message.size == 0) return {};
    return std::string(status.message.data, status.message.size);
}

void LogLocked(std::string message) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    char prefix[48]{};
    std::snprintf(prefix, sizeof(prefix), "[steady-ms=%lld] ",
        static_cast<long long>(elapsed));
    g.log.emplace_back(std::string(prefix) + std::move(message));
    if (g.log.size() > kMaximumLogEntries) {
        g.log.erase(g.log.begin(),
            g.log.begin() + static_cast<std::ptrdiff_t>(
                g.log.size() - kMaximumLogEntries));
    }
}

void Log(std::string message) {
    std::scoped_lock lock(g.mutex);
    LogLocked(std::move(message));
}

void SetStatus(std::string status) {
    std::scoped_lock lock(g.mutex);
    g.status = std::move(status);
}

std::uint32_t ClampExtraCount(const std::uint32_t count) noexcept {
    return (std::clamp)(count, kMinimumExtraCount, kMaximumExtraCount);
}

float ClampRate(const float rate) noexcept {
    if (!std::isfinite(rate)) return kDefaultRate;
    return (std::clamp)(rate, kMinimumRate, kMaximumRate);
}

std::uint32_t RateMilli(const float rate) noexcept {
    return static_cast<std::uint32_t>(std::llround(
        static_cast<double>(ClampRate(rate)) * 1000.0));
}

bool CombatServiceAvailable(const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
               decltype(AnomalyNteCombatServiceV1::participant_display_name_utf8)>(
               service, offsetof(AnomalyNteCombatServiceV1, participant_display_name_utf8)) &&
        service->current_combatant != nullptr &&
        service->latest_damage_sequence != nullptr &&
        service->next_damage_event != nullptr;
}

bool ChainServiceAvailable(
    const AnomalyNteDamageChainReactionServiceV1* service) noexcept {
    return service != nullptr &&
        HasField<AnomalyNteDamageChainReactionServiceV1,
            decltype(AnomalyNteDamageChainReactionServiceV1::stop_chain)>(
            service, offsetof(AnomalyNteDamageChainReactionServiceV1, stop_chain)) &&
        service->start_chain != nullptr &&
        service->apply_next != nullptr &&
        service->snapshot != nullptr &&
        service->stop_chain != nullptr;
}

void AddChain(
    const AnomalyNteDamageChainReactionServiceV1* service,
    const AnomalyNteDamageEventV1& event,
    const std::uint32_t extra_count,
    const float rate) {
    if (!ChainServiceAvailable(service)) return;

    AnomalyNteDamageChainReactionRequestV1 request{};
    request.struct_size = sizeof(request);
    request.flags = 0;
    request.damage_sequence = event.sequence;
    request.extra_count = ClampExtraCount(extra_count);
    request.rate_milli = RateMilli(rate);

    AnomalyGenerationHandleV1 handle{};
    const auto result = service->start_chain(service->user, &request, &handle);
    if (result.code != ANOMALY_STATUS_V1_OK || handle.id == 0) {
        char prefix[192]{};
        std::snprintf(prefix, sizeof(prefix),
            "原始命中 #%llu 未建立链：status=%s(%u)",
            static_cast<unsigned long long>(event.sequence),
            StatusName(result.code), result.code);
        std::string line(prefix);
        const std::string detail = StatusDetail(result);
        if (!detail.empty()) line += " detail=" + detail.substr(0, 384);
        {
            std::scoped_lock lock(g.mutex);
            ++g.failure_count;
            LogLocked(std::move(line));
            g.status = std::string("未启动链：") + StatusName(result.code);
        }
        return;
    }

    ChainItem item{};
    item.handle = handle;
    item.target = event.victim;
    item.root_sequence = event.sequence;
    item.requested_count = request.extra_count;
    item.core_state = ANOMALY_NTE_DAMAGE_CHAIN_REACTION_STATE_V1_ACTIVE;
    item.queue.Start(
        {request.extra_count, static_cast<double>(request.rate_milli) / 1000.0},
        Clock::now());

    {
        std::scoped_lock lock(g.mutex);
        g.chains.push_back(std::move(item));
        char line[256]{};
        std::snprintf(line, sizeof(line),
            "链 #%llu 已建立：原始命中 #%llu，目标句柄=%llu/%llu，额外次数=%u，速率=%.3f 次/秒。",
            static_cast<unsigned long long>(handle.id),
            static_cast<unsigned long long>(event.sequence),
            static_cast<unsigned long long>(event.victim.id),
            static_cast<unsigned long long>(event.victim.generation),
            request.extra_count,
            static_cast<double>(request.rate_milli) / 1000.0);
        LogLocked(line);
        const std::string detail = StatusDetail(result);
        if (!detail.empty()) LogLocked(std::string("Core 链初始化结果：") + detail.substr(0, 384));
        g.status = "已建立链，等待按速率应用并逐次确认";
    }
}

void ObserveSnapshotLocked(
    ChainItem& chain,
    const AnomalyNteDamageChainReactionSnapshotV1& snapshot,
    const Clock::time_point now) {
    if (snapshot.confirmed_extra_count > chain.confirmed_seen) {
        const auto delta = snapshot.confirmed_extra_count - chain.confirmed_seen;
        for (std::uint32_t index{}; index < delta; ++index) {
            chain.queue.CommitSuccess(now);
        }
        chain.confirmed_seen = snapshot.confirmed_extra_count;
        g.confirmed_extra_count += delta;

        char line[256]{};
        std::snprintf(line, sizeof(line),
            "链 #%llu 收到真实 CharacterOnDamaged 确认：%u/%u 次额外伤害已结算。",
            static_cast<unsigned long long>(chain.handle.id),
            chain.confirmed_seen, chain.requested_count);
        LogLocked(line);
    }
    const auto observed_dispatches =
        (std::max)(chain.dispatch_seen, snapshot.dispatch_attempt_count);
    if (observed_dispatches > chain.dispatch_seen) {
        g.dispatch_count += observed_dispatches - chain.dispatch_seen;
        chain.dispatch_seen = observed_dispatches;
    }
    chain.core_state = snapshot.state;
    chain.awaiting_confirmation =
        snapshot.state == ANOMALY_NTE_DAMAGE_CHAIN_REACTION_STATE_V1_AWAITING_CONFIRMATION ||
        (snapshot.flags &
            ANOMALY_NTE_DAMAGE_CHAIN_REACTION_SNAPSHOT_V1_AWAITING_CONFIRMATION) != 0;

    if (snapshot.state == ANOMALY_NTE_DAMAGE_CHAIN_REACTION_STATE_V1_COMPLETED) {
        if (!chain.terminal) {
            char line[160]{};
            std::snprintf(line, sizeof(line), "链 #%llu 已完成：%u/%u 次。",
                static_cast<unsigned long long>(chain.handle.id),
                chain.confirmed_seen, chain.requested_count);
            LogLocked(line);
        }
        chain.terminal = true;
        chain.awaiting_confirmation = false;
        chain.queue.Cancel();
    } else if (snapshot.state == ANOMALY_NTE_DAMAGE_CHAIN_REACTION_STATE_V1_FAILED ||
               snapshot.state == ANOMALY_NTE_DAMAGE_CHAIN_REACTION_STATE_V1_STOPPED) {
        if (snapshot.state == ANOMALY_NTE_DAMAGE_CHAIN_REACTION_STATE_V1_FAILED &&
            !chain.failure_counted) {
            ++g.failure_count;
            chain.failure_counted = true;
        }
        chain.terminal = true;
        chain.queue.Cancel();
    }
}

bool ReadChainSnapshot(
    const AnomalyNteDamageChainReactionServiceV1* service,
    const AnomalyGenerationHandleV1 handle,
    AnomalyNteDamageChainReactionSnapshotV1& snapshot) noexcept {
    if (!ChainServiceAvailable(service)) return false;
    snapshot = {};
    snapshot.struct_size = sizeof(snapshot);
    return service->snapshot(service->user, handle, &snapshot).code ==
        ANOMALY_STATUS_V1_OK;
}

void UpdateOneChain(
    const AnomalyNteDamageChainReactionServiceV1* service,
    const AnomalyGenerationHandleV1 handle,
    const bool enabled,
    const Clock::time_point now) {
    AnomalyNteDamageChainReactionSnapshotV1 snapshot{sizeof(snapshot)};
    if (!ReadChainSnapshot(service, handle, snapshot)) {
        std::scoped_lock lock(g.mutex);
        const auto found = std::find_if(g.chains.begin(), g.chains.end(),
            [&](const ChainItem& item) { return SameHandle(item.handle, handle); });
        if (found != g.chains.end() && !found->terminal) {
            found->terminal = true;
            found->queue.Cancel();
            found->awaiting_confirmation = false;
            LogLocked("链句柄失效或新服务不可用；停止对它发起后续调用。");
        }
        return;
    }

    bool send_stop{};
    bool send_apply{};
    std::uint32_t dispatch_before{};
    {
        std::scoped_lock lock(g.mutex);
        const auto found = std::find_if(g.chains.begin(), g.chains.end(),
            [&](const ChainItem& item) { return SameHandle(item.handle, handle); });
        if (found == g.chains.end()) return;

        ObserveSnapshotLocked(*found, snapshot, now);
        if (!enabled && !found->terminal && !found->stop_sent) {
            found->stop_sent = true;
            found->queue.Cancel();
            send_stop = true;
        } else if (enabled && !found->terminal && !found->stop_sent &&
                   !found->awaiting_confirmation && found->queue.Poll(now)) {
            send_apply = true;
            dispatch_before = snapshot.dispatch_attempt_count;
        }
    }

    if (send_stop) {
        const auto stopped = service->stop_chain(service->user, handle);
        Log(std::string("请求停止链 #") + std::to_string(handle.id) +
            "：status=" + StatusName(stopped.code));
        AnomalyNteDamageChainReactionSnapshotV1 after{sizeof(after)};
        if (ReadChainSnapshot(service, handle, after)) {
            std::scoped_lock lock(g.mutex);
            const auto found = std::find_if(g.chains.begin(), g.chains.end(),
                [&](const ChainItem& item) { return SameHandle(item.handle, handle); });
            if (found != g.chains.end()) ObserveSnapshotLocked(*found, after, Clock::now());
        }
        return;
    }

    if (!send_apply) return;

    const auto applied = service->apply_next(service->user, handle);
    AnomalyNteDamageChainReactionSnapshotV1 after{sizeof(after)};
    if (!ReadChainSnapshot(service, handle, after)) {
        std::scoped_lock lock(g.mutex);
        const auto found = std::find_if(g.chains.begin(), g.chains.end(),
            [&](const ChainItem& item) { return SameHandle(item.handle, handle); });
        if (found != g.chains.end()) {
            found->terminal = true;
            found->queue.Cancel();
            found->awaiting_confirmation = false;
        }
        LogLocked("原生调用后无法读取链快照；已停止重试，防止重复伤害。");
        return;
    }

    bool dispatched{};
    {
        std::scoped_lock lock(g.mutex);
        const auto found = std::find_if(g.chains.begin(), g.chains.end(),
            [&](const ChainItem& item) { return SameHandle(item.handle, handle); });
        if (found == g.chains.end()) return;

        dispatched = after.dispatch_attempt_count > dispatch_before ||
            (after.flags &
                ANOMALY_NTE_DAMAGE_CHAIN_REACTION_SNAPSHOT_V1_AWAITING_CONFIRMATION) != 0;
        ObserveSnapshotLocked(*found, after, Clock::now());
        if (!dispatched && applied.code != ANOMALY_STATUS_V1_OK &&
            !found->terminal && !found->awaiting_confirmation) {
            ++g.failure_count;
            found->queue.Defer(Clock::now() + kRetryDelay);
            char prefix[192]{};
            std::snprintf(prefix, sizeof(prefix),
                "链 #%llu 的本次应用未进入原生调用：status=%s(%u)，稍后重试；未计入成功次数。",
                static_cast<unsigned long long>(handle.id),
                StatusName(applied.code), applied.code);
            std::string line(prefix);
            const std::string detail = StatusDetail(applied);
            if (!detail.empty()) line += " detail=" + detail.substr(0, 384);
            LogLocked(std::move(line));
        } else if (dispatched && found->awaiting_confirmation) {
            char line[224]{};
            std::snprintf(line, sizeof(line),
                "链 #%llu 已调用原生 K2_OnTriggerPeriodGamePlayEffect，等待真实伤害事件；调用不等于成功。",
                static_cast<unsigned long long>(handle.id));
            LogLocked(line);
        }
    }
}

void DrainDamageEvents(
    const AnomalyNteCombatServiceV1* combat,
    const AnomalyNteDamageChainReactionServiceV1* chain_service,
    const bool enabled,
    const std::uint32_t extra_count,
    const float rate,
    const AnomalyGenerationHandleV1 player_character) {
    if (!CombatServiceAvailable(combat)) return;

    for (std::size_t drained{}; drained < kMaximumEventsDrainedPerUpdate; ++drained) {
        std::uint64_t cursor{};
        {
            std::scoped_lock lock(g.mutex);
            cursor = g.damage_cursor;
        }

        AnomalyNteDamageEventV1 event{sizeof(event)};
        const auto result = combat->next_damage_event(combat->user, cursor, &event);
        if (result.code != ANOMALY_STATUS_V1_OK) {
            const auto latest = combat->latest_damage_sequence(combat->user);
            if (latest > cursor && result.code == ANOMALY_STATUS_V1_NOT_FOUND) {
                {
                    std::scoped_lock lock(g.mutex);
                    g.damage_cursor = latest;
                    LogLocked("伤害事件游标已落后于保留窗口；跳过已丢失记录，从最新序号继续。");
                }
            }
            break;
        }
        if (event.sequence == 0 || event.sequence <= cursor) break;
        {
            std::scoped_lock lock(g.mutex);
            g.damage_cursor = event.sequence;
        }

        if ((event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) == 0 ||
            (event.flags & ANOMALY_NTE_DAMAGE_V1_CHAIN_GENERATED) != 0 ||
            event.final_damage <= 0 ||
            !SameHandle(event.attacker, player_character) ||
            event.victim.id == 0 || event.victim.id == event.attacker.id) {
            continue;
        }
        if (!enabled) continue;
        {
            std::scoped_lock lock(g.mutex);
            ++g.root_hit_count;
        }

        // Core independently verifies HTAbilityCharacter identity and hostility, resolves
        // DamageGEDef to a concrete Dump-derived effect class, and binds a native
        // DamageTriggerActor's EffectContainer to this original target.
        AddChain(chain_service, event, extra_count, rate);
    }
}

bool BuildExportText(const AnomalyGenerationHandleV1 task, std::string& output) {
    std::scoped_lock lock(g.mutex);
    if (!g.export_in_flight) return false;
    if (g.export_task.id == 0) g.export_task = task;
    if (!SameHandle(g.export_task, task)) return false;

    const auto generated_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto active_chains = std::count_if(g.chains.begin(), g.chains.end(),
        [](const ChainItem& chain) { return !chain.terminal; });

    output.clear();
    output.reserve(4096);
    output.append("Damage Chain Reaction - diagnostic report\n");
    output.append("report_schema=1\n");
    output.append("plugin_id=anomaly.builtin.nte-damage-chain-reaction\n");
    output.append("plugin_version=0.1.3\n");
    output.append("report_path=damage-chain-reaction.log\n");
    output.append("generated_epoch_ms=");
    output.append(std::to_string(generated_epoch_ms));
    output.append("\nfeature_enabled=");
    output.append(g.enabled ? "true" : "false");
    output.append("\nconfigured_extra_count=");
    output.append(std::to_string(g.extra_count));
    output.append("\nconfigured_rate_per_second=");
    output.append(std::to_string(static_cast<double>(g.rate)));
    output.append("\neligible_root_hit_count=");
    output.append(std::to_string(g.root_hit_count));
    output.append("\nconfirmed_extra_damage_count=");
    output.append(std::to_string(g.confirmed_extra_count));
    output.append("\nnative_trigger_dispatch_count=");
    output.append(std::to_string(g.dispatch_count));
    output.append("\nfailure_count=");
    output.append(std::to_string(g.failure_count));
    output.append("\ntracked_chain_count=");
    output.append(std::to_string(g.chains.size()));
    output.append("\nactive_chain_count=");
    output.append(std::to_string(active_chains));
    output.append("\nretained_log_entry_count=");
    output.append(std::to_string(g.log.size()));
    output.append("\ncurrent_status=");
    output.append(g.status);
    output.append("\nexport_status=");
    output.append(g.export_status);
    output.append("\n\nNotes:\n");
    output.append("- The original hit is not included in configured_extra_count.\n");
    output.append("- confirmed_extra_damage_count increments only after a matching real damage event.\n");
    output.append("- native_trigger_dispatch_count records dispatch attempts, not confirmed damage.\n");
    output.append("- Rate controls cadence only; elapsed time does not expire a pending chain.\n");
    output.append("- The event log below is bounded to the latest 200 entries.\n\nEvent log:\n");
    for (const auto& line : g.log) {
        output.append(line);
        output.push_back('\n');
    }
    return true;
}

void CompleteExport(std::string status, std::string message) {
    std::scoped_lock lock(g.mutex);
    g.export_in_flight = false;
    g.export_task = {};
    g.export_status = std::move(status);
    LogLocked(std::move(message));
}

void CompleteExportTask(
    const AnomalyGenerationHandleV1 task,
    std::string status,
    std::string message) {
    std::scoped_lock lock(g.mutex);
    if (!g.export_in_flight || !SameHandle(g.export_task, task)) return;
    g.export_in_flight = false;
    g.export_task = {};
    g.export_status = std::move(status);
    LogLocked(std::move(message));
}

void ANOMALY_CALL ExportTask(
    void* callback_user,
    const AnomalyGenerationHandleV1 task) {
    const auto* host = static_cast<const AnomalyHostApiV1*>(callback_user);
    {
        std::scoped_lock lock(g.mutex);
        if (!g.export_in_flight) return;
        if (g.export_task.id == 0) g.export_task = task;
        if (!SameHandle(g.export_task, task)) return;
    }
    if (host == nullptr) {
        CompleteExportTask(task, "日志导出失败：Host 不可用", "后台导出失败：Host 不可用。");
        return;
    }

    try {
        const anomaly::sdk::Host sdk_host(host);
        const auto storage = sdk_host.Query<AnomalyStorageServiceV1>(
            ANOMALY_STORAGE_SERVICE_V1_ID, ANOMALY_STORAGE_SERVICE_V1_VERSION);
        if (!storage || storage->write_atomic == nullptr) {
            CompleteExportTask(
                task,
                "日志导出失败：存储服务不可用",
                "后台导出失败：anomaly.storage 服务不可用。");
            return;
        }

        // This callback runs on anomaly.scheduler's worker, away from the
        // Game/Render callbacks where synchronous state I/O is prohibited.
        std::string contents;
        if (!BuildExportText(task, contents)) return;
        const auto path = anomaly::sdk::StringView(kLogPath);
        const AnomalyByteSpanV1 bytes{
            reinterpret_cast<const std::uint8_t*>(contents.data()),
            contents.size()};
        const auto status = storage->write_atomic(storage->user, path, bytes);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            CompleteExportTask(
                task,
                std::string("日志已导出：") + kLogPath,
                std::string("诊断报告已导出到插件存储文件：") + kLogPath);
        } else {
            const std::string detail = StatusDetail(status);
            std::string user_status = std::string("日志导出失败：") + StatusName(status.code);
            std::string message = std::string("后台报告写入失败：status=") +
                StatusName(status.code);
            if (!detail.empty()) {
                user_status += "（" + detail + "）";
                message += " reason=" + detail;
            }
            CompleteExportTask(task, std::move(user_status), std::move(message));
        }
    } catch (...) {
        CompleteExportTask(
            task,
            "日志导出失败：后台任务异常",
            "后台诊断报告导出任务发生未处理异常。");
    }
}

void ProcessExport(const AnomalyHostApiV1* host) {
    bool requested{};
    bool in_flight{};
    {
        std::scoped_lock lock(g.mutex);
        requested = g.export_requested;
        g.export_requested = false;
        in_flight = g.export_in_flight;
    }
    if (!requested) return;
    if (in_flight) {
        std::scoped_lock lock(g.mutex);
        g.export_status = "已有后台导出任务正在运行";
        LogLocked("用户再次请求导出，但已有导出任务运行中；未重复排队。");
        return;
    }
    if (host == nullptr) {
        CompleteExport("日志导出失败：Host 不可用", "日志导出失败：Host 不可用。");
        return;
    }

    const anomaly::sdk::Host sdk_host(host);
    const auto scheduler = sdk_host.Query<AnomalySchedulerServiceV1>(
        ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    if (!scheduler || scheduler->schedule == nullptr) {
        CompleteExport(
            "日志导出失败：后台调度服务不可用",
            "日志导出失败：anomaly.scheduler 服务不可用，无法安全地移出 Game/Render 线程。");
        return;
    }

    {
        std::scoped_lock lock(g.mutex);
        g.export_in_flight = true;
        g.export_status = "正在后台导出日志…";
        LogLocked("已将日志导出任务提交给 anomaly.scheduler Worker。");
    }

    AnomalyGenerationHandleV1 task{};
    const auto scheduled = scheduler->schedule(
        scheduler->user, 1U, ExportTask,
        const_cast<AnomalyHostApiV1*>(host), &task);
    if (scheduled.code != ANOMALY_STATUS_V1_OK) {
        const std::string detail = StatusDetail(scheduled);
        std::string user_status = std::string("日志导出任务启动失败：") +
            StatusName(scheduled.code);
        std::string message = std::string("后台任务排队失败：status=") +
            StatusName(scheduled.code);
        if (!detail.empty()) {
            user_status += "（" + detail + "）";
            message += " reason=" + detail;
        }
        CompleteExport(std::move(user_status), std::move(message));
    } else {
        bool cancel_task{};
        {
            std::scoped_lock lock(g.mutex);
            if (g.export_in_flight) {
                if (g.export_task.id == 0) g.export_task = task;
                cancel_task = !SameHandle(g.export_task, task);
            } else {
                cancel_task = true;
            }
        }
        if (cancel_task && scheduler->cancel != nullptr) {
            static_cast<void>(scheduler->cancel(scheduler->user, task));
        }
    }
}

void CancelExportTask(const AnomalyHostApiV1* host) {
    AnomalyGenerationHandleV1 task{};
    {
        std::scoped_lock lock(g.mutex);
        g.export_requested = false;
        g.export_in_flight = false;
        task = g.export_task;
        g.export_task = {};
    }
    if (task.id == 0 || task.generation == 0 || host == nullptr) return;
    try {
        const anomaly::sdk::Host sdk_host(host);
        const auto scheduler = sdk_host.Query<AnomalySchedulerServiceV1>(
            ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
        if (scheduler && scheduler->cancel != nullptr) {
            static_cast<void>(scheduler->cancel(scheduler->user, task));
        }
    } catch (...) {
        // The task checks export_in_flight before reading state. If cancellation
        // races with callback start, the detached generation is ignored.
    }
}

void ResetState() {
    std::scoped_lock lock(g.mutex);
    g.enabled = false;
    g.export_requested = false;
    g.export_in_flight = false;
    g.export_task = {};
    g.export_status = "尚未导出";
    g.cursor_initialized = false;
    g.extra_count = kDefaultExtraCount;
    g.rate = kDefaultRate;
    g.damage_cursor = 0;
    g.root_hit_count = 0;
    g.confirmed_extra_count = 0;
    g.failure_count = 0;
    g.dispatch_count = 0;
    g.status = "等待 Anomaly 原生伤害服务";
    g.chains.clear();
    g.log.clear();
    LogLocked("插件已初始化；默认关闭，首次原生命中不计入额外次数。");
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    const auto ui = anomaly::sdk::Host(host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr || ui->button == nullptr || ui->checkbox == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g.host = host;
    g.ui = ui.get();
    ResetState();
    *context = &g;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    ResetState();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    const auto* host = g.host;
    CancelExportTask(host);
    if (host != nullptr) {
        const anomaly::sdk::Host sdk_host(host);
        const auto chain_service = sdk_host.Query<AnomalyNteDamageChainReactionServiceV1>(
            ANOMALY_NTE_DAMAGE_CHAIN_REACTION_SERVICE_V1_ID,
            ANOMALY_NTE_DAMAGE_CHAIN_REACTION_SERVICE_V1_VERSION);
        if (ChainServiceAvailable(chain_service.get())) {
            std::vector<AnomalyGenerationHandleV1> handles;
            {
                std::scoped_lock lock(g.mutex);
                for (const auto& chain : g.chains) {
                    if (!chain.terminal && !chain.stop_sent) handles.push_back(chain.handle);
                }
            }
            for (const auto handle : handles) {
                static_cast<void>(chain_service->stop_chain(chain_service->user, handle));
            }
        }
    }
    ResetState();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    static_cast<void>(Stop(nullptr, 0));
    g.host = nullptr;
    g.ui = nullptr;
}

void ANOMALY_CALL Update(void* context, double) {
    if (context != &g) return;
    const auto* host = g.host;
    if (host == nullptr) return;

    const anomaly::sdk::Host sdk_host(host);
    const auto combat = sdk_host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);
    const auto chain_service = sdk_host.Query<AnomalyNteDamageChainReactionServiceV1>(
        ANOMALY_NTE_DAMAGE_CHAIN_REACTION_SERVICE_V1_ID,
        ANOMALY_NTE_DAMAGE_CHAIN_REACTION_SERVICE_V1_VERSION);

    if (!CombatServiceAvailable(combat.get()) || !ChainServiceAvailable(chain_service.get())) {
        SetStatus("原生伤害服务尚未通过 Core 验证；当前不会发起任何伤害调用");
        ProcessExport(host);
        return;
    }

    bool enabled{};
    std::uint32_t extra_count{};
    float rate{};
    bool cursor_initialized{};
    {
        std::scoped_lock lock(g.mutex);
        enabled = g.enabled;
        extra_count = g.extra_count;
        rate = g.rate;
        cursor_initialized = g.cursor_initialized;
    }

    const auto latest = combat->latest_damage_sequence(combat->user);
    if (!cursor_initialized) {
        {
            std::scoped_lock lock(g.mutex);
            if (!g.cursor_initialized) {
                g.damage_cursor = latest;
                g.cursor_initialized = true;
                LogLocked("伤害流游标已初始化；仅处理此刻之后的新命中，不对历史事件补开链。");
            }
        }
    }

    AnomalyNteCombatantSnapshotV1 player{sizeof(player)};
    AnomalyGenerationHandleV1 player_character{};
    const auto player_status = combat->current_combatant(combat->user, &player);
    if (player_status.code == ANOMALY_STATUS_V1_OK &&
        player.struct_size >= sizeof(player) &&
        (player.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0) {
        player_character = player.character;
    }

    DrainDamageEvents(
        combat.get(), chain_service.get(), enabled,
        extra_count, rate, player_character);

    std::vector<AnomalyGenerationHandleV1> handles;
    {
        std::scoped_lock lock(g.mutex);
        handles.reserve(g.chains.size());
        for (const auto& chain : g.chains) handles.push_back(chain.handle);
    }
    const auto now = Clock::now();
    for (const auto handle : handles) {
        UpdateOneChain(chain_service.get(), handle, enabled, now);
    }

    {
        std::scoped_lock lock(g.mutex);
        if (g.chains.size() > kMaximumChainsKept) {
            const auto removable = g.chains.size() - kMaximumChainsKept;
            std::size_t removed{};
            auto it = g.chains.begin();
            while (it != g.chains.end() && removed < removable) {
                if (it->terminal && !it->awaiting_confirmation) {
                    it = g.chains.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        if (!g.enabled) {
            g.status = "功能关闭；活动链已请求停止";
        } else if (g.chains.empty()) {
            g.status = "已启用，等待下一次自然伤害命中";
        } else {
            const auto active = std::count_if(g.chains.begin(), g.chains.end(),
                [](const ChainItem& item) { return !item.terminal; });
            g.status = std::string("启用中；活动链数=") + std::to_string(active);
        }
    }

    ProcessExport(host);
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* service) {
    if (service == nullptr) service = g.ui;
    if (service == nullptr || service->begin_window == nullptr ||
        service->end_window == nullptr || service->text == nullptr ||
        service->checkbox == nullptr || service->button == nullptr) {
        return;
    }

    bool enabled{};
    std::uint32_t extra_count{};
    float rate{};
    std::uint64_t root_hit_count{};
    std::uint64_t confirmed_extra_count{};
    std::uint64_t failure_count{};
    std::uint64_t dispatch_count{};
    bool bound{};
    bool trigger_active{};
    std::string status;
    std::string export_status;
    {
        std::scoped_lock lock(g.mutex);
        enabled = g.enabled;
        extra_count = g.extra_count;
        rate = g.rate;
        root_hit_count = g.root_hit_count;
        confirmed_extra_count = g.confirmed_extra_count;
        failure_count = g.failure_count;
        dispatch_count = g.dispatch_count;
        status = g.status;
        export_status = g.export_status;
        for (const auto& item : g.chains) {
            if (!item.terminal && item.target.id != 0 && item.target.generation != 0) {
                bound = true;
                trigger_active = true;
                break;
            }
        }
    }

    int open = 1;
    anomaly::sdk::UiWindow window(service, "伤害链式反应", &open);
    if (!window) return;

    service->text(service->user, anomaly::sdk::StringView(
        "保留首次原生命中；伤害重复次数只统计其后的额外伤害。速率只控制触发间隔，不限制链的总持续时间。"));
    service->text(service->user, anomaly::sdk::StringView(
        "使用已校验的 DamageTriggerActor 原生周期事件；不会模拟按键、重放技能/动画或直接修改 HP。"));

    int enabled_ui = enabled ? 1 : 0;
    if (service->checkbox(service->user,
            anomaly::sdk::StringView("√ 启用"), &enabled_ui) != 0) {
        std::scoped_lock lock(g.mutex);
        g.enabled = enabled_ui != 0;
        LogLocked(g.enabled
            ? "用户启用功能；从下一次新的自然伤害开始创建链。"
            : "用户关闭功能；将请求停止所有活动链。");
    }

    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_uint32)>(
            service, offsetof(AnomalyUiServiceV1, input_uint32)) &&
        service->input_uint32 != nullptr) {
        if (service->input_uint32(service->user,
                anomaly::sdk::StringView("伤害重复次数"), &extra_count,
                kMinimumExtraCount, kMaximumExtraCount) != 0) {
            extra_count = ClampExtraCount(extra_count);
            std::scoped_lock lock(g.mutex);
            g.extra_count = extra_count;
        }
    }
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::slider_float)>(
            service, offsetof(AnomalyUiServiceV1, slider_float)) &&
        service->slider_float != nullptr) {
        if (service->slider_float(service->user,
                anomaly::sdk::StringView("连锁速率（次/秒）"), &rate,
                kMinimumRate, kMaximumRate) != 0) {
            rate = ClampRate(rate);
            std::scoped_lock lock(g.mutex);
            g.rate = rate;
        }
    }

    char setting[160]{};
    std::snprintf(setting, sizeof(setting), "当前设置：伤害重复次数=%u；速率=%.3f 次/秒",
        ClampExtraCount(extra_count), static_cast<double>(ClampRate(rate)));
    service->text(service->user, anomaly::sdk::StringView(setting));

    service->text(service->user, anomaly::sdk::StringView(
        bound ? "绑定=就绪" : "绑定=等待"));
    service->text(service->user, anomaly::sdk::StringView(
        !enabled ? "触发=已停止"
                 : (trigger_active ? "触发=战斗中" : "触发=等待有效命中")));
    service->text(service->user, anomaly::sdk::StringView(status.c_str()));
    service->text(service->user, anomaly::sdk::StringView(export_status.c_str()));

    char statistics[256]{};
    std::snprintf(statistics, sizeof(statistics),
        "原始命中=%llu次 已确认额外伤害=%llu次 触发调用=%llu次 失败=%llu次",
        static_cast<unsigned long long>(root_hit_count),
        static_cast<unsigned long long>(confirmed_extra_count),
        static_cast<unsigned long long>(dispatch_count),
        static_cast<unsigned long long>(failure_count));
    service->text(service->user, anomaly::sdk::StringView(statistics));

    // Detailed records stay out of the main UI. Use this action to export the
    // bounded in-memory diagnostics to the plugin-scoped storage file.
    if (service->button(service->user,
            anomaly::sdk::StringView("导出诊断日志"), 0.0F, 0.0F) != 0) {
        std::scoped_lock lock(g.mutex);
        g.export_requested = true;
        g.export_status = "已请求后台导出";
        LogLocked(std::string("用户请求导出诊断日志到 ") + kLogPath);
    }
}
}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-damage-chain-reaction"),
        anomaly::sdk::StringView("伤害链式反应"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("0.1.3"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
