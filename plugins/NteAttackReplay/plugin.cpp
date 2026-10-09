// 中文维护说明：此插件只捕获玩家真实命中的伤害数值，并通过 Host 的直接 HP 服务重复该数值。
// 不重放普通攻击输入、不重新激活技能、不播放玩家动作。所有游戏服务调用只发生在 Game Update。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

struct DamageSample {
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 attacker{};
    AnomalyGenerationHandleV1 victim{};
    float damage{};
    std::uint64_t sequence{};
    std::string target_name;
};

struct Context {
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteDamageReplayServiceV1* direct_damage{};
    std::atomic_bool started{false};
    std::atomic<int> enabled_request{-1};
    std::atomic_bool rate_dirty{false};
    std::atomic<std::uint32_t> requested_rate{10};

    std::mutex mutex;
    DamageSample sample{};
    bool captured{};
    bool enabled{};
    std::uint32_t rate_per_second{10};
    std::uint64_t successful_repeat_count{};
    float last_hp_before{};
    float last_hp_after{};
    float last_damage_applied{};
    std::string status{"自动记录已就绪：等待玩家命中目标"};
    bool cursor_initialized{};
    std::uint64_t damage_cursor{};
    Clock::time_point next_replay{};

    // Damage events with the same source/target that arrive in one contiguous burst
    // are treated as one attack chain; the first event in that burst is retained.
    std::uint64_t last_chain_source{};
    AnomalyGenerationHandleV1 last_chain_target{};
    Clock::time_point chain_until{};
} g_context;

constexpr AnomalyStatusV1 Status(const std::uint32_t code,
                                 const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::char_traits<char>::length(message)}};
}

template <typename Service>
const Service* QueryService(
    const AnomalyHostApiV1* host, const std::string_view id,
    const std::uint32_t version) noexcept {
    if (host == nullptr || host->query_service == nullptr) return nullptr;
    const void* table{};
    const auto result = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), version, &table);
    if (result.code != ANOMALY_STATUS_V1_OK || table == nullptr) return nullptr;
    return static_cast<const Service*>(table);
}

std::string MessageOf(const AnomalyStatusV1& status, const std::string_view fallback) {
    if (status.message.data != nullptr && status.message.size != 0) {
        return std::string(status.message.data, status.message.size);
    }
    return std::string(fallback);
}

std::string ParticipantName(
    const AnomalyNteCombatServiceV1* combat,
    const AnomalyGenerationHandleV1 participant) {
    if (combat == nullptr || combat->participant_display_name_utf8 == nullptr ||
        participant.id == 0) return "目标";
    std::string name(256, '\0');
    std::size_t size = name.size();
    const auto status = combat->participant_display_name_utf8(
        combat->user, participant, name.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > name.size()) return "目标";
    name.resize(size - 1U);
    return name.empty() ? "目标" : name;
}

void SetStatus(std::string message) {
    std::scoped_lock lock(g_context.mutex);
    g_context.status = std::move(message);
}

void CaptureEvents(
    const AnomalyNteCombatantSnapshotV1& player,
    const Clock::time_point now) {
    const auto* combat = g_context.combat;
    if (combat == nullptr || combat->next_damage_event == nullptr) return;

    for (std::uint32_t drained = 0; drained < 64; ++drained) {
        AnomalyNteDamageEventV1 event{};
        event.struct_size = sizeof(event);
        const auto status = combat->next_damage_event(
            combat->user, g_context.damage_cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) break;
        if (event.sequence <= g_context.damage_cursor) break;
        g_context.damage_cursor = event.sequence;

        if (event.attacker.id == 0 || event.victim.id == 0 ||
            event.attacker.id != player.character.id ||
            event.attacker.generation != player.character.generation ||
            event.victim.id == player.character.id ||
            event.victim.generation != player.character.generation ||
            event.world.id != player.world.id ||
            event.world.generation != player.world.generation) {
            continue;
        }

        const std::int64_t raw_damage = event.final_damage > 0
            ? event.final_damage
            : (event.basic_damage > 0 ? event.basic_damage : event.display_damage);
        if (raw_damage <= 0 || raw_damage > 1000000000LL) continue;

        bool enabled{};
        {
            std::scoped_lock lock(g_context.mutex);
            enabled = g_context.enabled;
        }
        if (enabled) continue;

        // Keep the first damage event from a contiguous same-source/same-target burst,
        // rather than replacing it with later ticks from the same multi-hit chain.
        const bool same_chain =
            event.source_id == g_context.last_chain_source &&
            event.victim.id == g_context.last_chain_target.id &&
            event.victim.generation == g_context.last_chain_target.generation &&
            now < g_context.chain_until;
        if (same_chain) continue;

        g_context.last_chain_source = event.source_id;
        g_context.last_chain_target = event.victim;
        g_context.chain_until = now + std::chrono::milliseconds(250);

        DamageSample next;
        next.world = event.world;
        next.attacker = event.attacker;
        next.victim = event.victim;
        next.damage = static_cast<float>(raw_damage);
        next.sequence = event.sequence;
        next.target_name = ParticipantName(combat, event.victim);
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.sample = std::move(next);
            g_context.captured = true;
            g_context.last_hp_before = 0.0F;
            g_context.last_hp_after = 0.0F;
            g_context.last_damage_applied = 0.0F;
            g_context.status = "已捕获首个伤害事件：" +
                std::to_string(raw_damage) + " 点；启用后直接重复扣除目标 HP";
        }
    }
}

void Update() {
    if (!g_context.started.load(std::memory_order_acquire)) return;
    const auto* combat = g_context.combat;
    const auto* direct = g_context.direct_damage;
    if (combat == nullptr || direct == nullptr ||
        combat->current_combatant == nullptr ||
        combat->latest_damage_sequence == nullptr ||
        combat->next_damage_event == nullptr ||
        direct->apply_damage == nullptr) return;

    const auto now = Clock::now();
    const int enable_request = g_context.enabled_request.exchange(-1, std::memory_order_acq_rel);
    if (enable_request >= 0) {
        std::scoped_lock lock(g_context.mutex);
        const bool requested = enable_request != 0;
        if (requested != g_context.enabled) {
            g_context.enabled = requested;
            if (requested) {
                const auto interval = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / static_cast<double>(g_context.rate_per_second)));
                g_context.next_replay = now + interval;
                g_context.status = g_context.captured
                    ? "效果已启用：直接重复已捕获伤害；次数无上限"
                    : "效果已启用：等待下一次玩家命中事件";
            } else {
                g_context.status = "效果已禁用；自动记录仍继续运行";
            }
        }
    }
    if (g_context.rate_dirty.exchange(false, std::memory_order_acq_rel)) {
        const auto requested = (std::clamp)(g_context.requested_rate.load(std::memory_order_acquire), 1U, 1000U);
        std::scoped_lock lock(g_context.mutex);
        g_context.rate_per_second = requested;
        g_context.next_replay = now + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(requested)));
        if (g_context.enabled) g_context.status = "重放速率已更新为每秒 " + std::to_string(requested) + " 次";
    }

    AnomalyNteCombatantSnapshotV1 player{};
    player.struct_size = sizeof(player);
    const auto current_status = combat->current_combatant(combat->user, &player);
    if (current_status.code != ANOMALY_STATUS_V1_OK ||
        player.character.id == 0 || player.world.id != 1) {
        SetStatus("当前玩家/世界快照不可用；直接伤害暂停");
        return;
    }

    if (!g_context.cursor_initialized) {
        g_context.damage_cursor = combat->latest_damage_sequence(combat->user);
        g_context.cursor_initialized = true;
    } else {
        CaptureEvents(player, now);
    }

    DamageSample sample;
    bool captured{}, enabled{};
    std::uint32_t rate{};
    {
        std::scoped_lock lock(g_context.mutex);
        sample = g_context.sample;
        captured = g_context.captured;
        enabled = g_context.enabled;
        rate = g_context.rate_per_second;
        if (enabled && !captured) {
            g_context.status = "效果已启用：等待下一次玩家命中事件";
        }
    }
    if (!enabled || !captured || now < g_context.next_replay) return;

    // Exactly one application per Game update. A rate of 10 means one direct HP
    // application every 100 ms; there is no replay-count limit.
    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(rate)));
    g_context.next_replay = now + interval;

    AnomalyNteDamageReplayRequestV1 request{};
    request.struct_size = sizeof(request);
    request.world = sample.world;
    request.attacker = sample.attacker;
    request.victim = sample.victim;
    request.damage = sample.damage;

    AnomalyNteDamageReplayResultV1 result{};
    result.struct_size = sizeof(result);
    const auto status = direct->apply_damage(direct->user, &request, &result);
    if (status.code != ANOMALY_STATUS_V1_OK ||
        (result.flags & ANOMALY_NTE_DAMAGE_REPLAY_V1_VALID) == 0 ||
        result.damage_applied <= 0.0F ||
        !std::isfinite(result.hp_before) || !std::isfinite(result.hp_after)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.enabled = false;
        g_context.status = "直接伤害未通过 Host/HP 回读验证，效果已停止：" +
            MessageOf(status, "伤害服务返回无效结果");
        return;
    }

    std::scoped_lock lock(g_context.mutex);
    if (g_context.successful_repeat_count != UINT64_MAX) ++g_context.successful_repeat_count;
    g_context.last_hp_before = result.hp_before;
    g_context.last_hp_after = result.hp_after;
    g_context.last_damage_applied = result.damage_applied;
    g_context.status = "直接伤害已确认：目标 HP " +
        std::to_string(result.hp_before) + " -> " + std::to_string(result.hp_after);
    if ((result.flags & ANOMALY_NTE_DAMAGE_REPLAY_V1_TARGET_ALIVE) == 0) {
        g_context.enabled = false;
        g_context.status = "目标 HP 已归零；效果自动停止";
    }
}

void Draw(const AnomalyUiServiceV1* ui) {
    if (ui == nullptr || ui->begin_window == nullptr ||
        ui->end_window == nullptr || ui->text == nullptr ||
        ui->checkbox == nullptr || ui->input_uint32 == nullptr) return;

    int open = 1;
    if (ui->begin_window(ui->user, anomaly::sdk::StringView("NTE Attack Replay"), &open, 0) == 0) return;

    bool enabled{};
    bool captured{};
    std::uint32_t rate{};
    std::uint64_t count{};
    float damage{};
    float hp_before{}, hp_after{}, applied{};
    std::string target, status;
    {
        std::scoped_lock lock(g_context.mutex);
        enabled = g_context.enabled;
        captured = g_context.captured;
        rate = g_context.rate_per_second;
        count = g_context.successful_repeat_count;
        damage = g_context.sample.damage;
        target = g_context.sample.target_name;
        hp_before = g_context.last_hp_before;
        hp_after = g_context.last_hp_after;
        applied = g_context.last_damage_applied;
        status = g_context.status;
    }

    int enabled_value = enabled ? 1 : 0;
    if (ui->checkbox(ui->user, anomaly::sdk::StringView("启用直接伤害重放"), &enabled_value) != 0) {
        g_context.enabled_request.store(enabled_value != 0 ? 1 : 0, std::memory_order_release);
    }
    std::uint32_t editable_rate = rate;
    if (ui->input_uint32(ui->user, anomaly::sdk::StringView("每秒重复次数"), &editable_rate, 1U, 10U) != 0) {
        g_context.requested_rate.store((std::clamp)(editable_rate, 1U, 1000U), std::memory_order_release);
        g_context.rate_dirty.store(true, std::memory_order_release);
    }
    ui->text(ui->user, anomaly::sdk::StringView("次数：无上限"));
    ui->text(ui->user, anomaly::sdk::StringView(captured
        ? "捕获状态：已锁定一条真实玩家伤害"
        : "捕获状态：等待玩家造成伤害"));
    ui->text(ui->user, anomaly::sdk::StringView(
        "伤害值：" + (captured ? std::to_string(damage) : std::string("—"))));
    ui->text(ui->user, anomaly::sdk::StringView("目标：" + (target.empty() ? std::string("—") : target)));
    ui->text(ui->user, anomaly::sdk::StringView(
        "成功重复次数：" + std::to_string(count)));
    if (count != 0) {
        ui->text(ui->user, anomaly::sdk::StringView(
            "最近一次 HP：" + std::to_string(hp_before) + " -> " +
            std::to_string(hp_after) + "（实际扣除 " + std::to_string(applied) + "）"));
    }
    ui->text(ui->user, anomaly::sdk::StringView(status));
    ui->end_window(ui->user);
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr)
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);

    const auto* combat = QueryService<AnomalyNteCombatServiceV1>(
        host, ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);
    const auto* direct = QueryService<AnomalyNteDamageReplayServiceV1>(
        host, ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_ID, ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_VERSION);
    const auto* ui = QueryService<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (combat == nullptr || direct == nullptr || ui == nullptr ||
        combat->current_combatant == nullptr || combat->latest_damage_sequence == nullptr ||
        combat->next_damage_event == nullptr || direct->apply_damage == nullptr ||
        ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr ||
        ui->checkbox == nullptr || ui->input_uint32 == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            "combat, direct-damage, or UI service is missing");
    }

    g_context.combat = combat;
    g_context.direct_damage = direct;
    g_context.started.store(false, std::memory_order_release);
    g_context.enabled_request.store(-1, std::memory_order_release);
    g_context.rate_dirty.store(false, std::memory_order_release);
    g_context.requested_rate.store(10, std::memory_order_release);
    g_context.cursor_initialized = false;
    g_context.damage_cursor = 0;
    g_context.next_replay = {};
    g_context.last_chain_source = 0;
    g_context.last_chain_target = {};
    g_context.chain_until = {};
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.sample = {};
        g_context.captured = false;
        g_context.enabled = false;
        g_context.rate_per_second = 10;
        g_context.successful_repeat_count = 0;
        g_context.last_hp_before = 0.0F;
        g_context.last_hp_after = 0.0F;
        g_context.last_damage_applied = 0.0F;
        g_context.status = "自动记录已就绪：等待玩家命中目标";
    }
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(false, std::memory_order_release);
    std::scoped_lock lock(g_context.mutex);
    g_context.enabled = false;
    g_context.status = "插件已停止；直接伤害效果已关闭";
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    g_context.combat = nullptr;
    g_context.direct_damage = nullptr;
}

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) {
    if (plugin_context == &g_context) Update();
}

void ANOMALY_CALL DrawCallback(
    void* plugin_context, const AnomalyUiServiceV1* ui) {
    if (plugin_context == &g_context) Draw(ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-attack-replay"),
        anomaly::sdk::StringView("NTE Attack Replay"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.5.0"),
        Load,
        Start,
        Stop,
        Unload,
        UpdateCallback,
        DrawCallback};
    return anomaly::sdk::Ok();
}
