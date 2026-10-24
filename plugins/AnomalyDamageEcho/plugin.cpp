#include "anomaly/sdk/anomaly_sdk.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kMaxTicksPerSecond = 10;
constexpr std::size_t kMaxTargets = 64;
constexpr std::size_t kMaxEventsPerUpdate = 128;
constexpr std::size_t kMaxApplicationsPerUpdate = 8;

constexpr char kPluginId[] = "anomaly.local.damage-echo";
constexpr char kPluginName[] = "Damage Echo";
constexpr char kPluginAuthor[] = "Anomaly";
constexpr char kPluginVersion[] = "1.0.0";

struct EchoTarget {
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 victim{};
    std::int64_t damage{};
    std::uint32_t damage_type{};
    std::uint32_t display_type{};
    std::uint32_t reaction_type{};
    std::uint32_t reaction_display_type{};
    std::uint32_t flags{};
    double accumulator{};
    std::uint32_t failures{};
};

struct SharedState {
    std::mutex mutex;
    bool enabled{true};
    std::uint32_t mode{};  // 0 = exact HP + damage text, 1 = native DoT GE.
    std::uint32_t ticks_per_second{4};
    std::unordered_map<std::uint64_t, EchoTarget> targets;
    std::string status{"等待 Game 域伤害事件"};
    std::uint64_t cursor{};
    std::uint64_t last_world_generation{};
    std::uint64_t last_player_id{};
    std::uint64_t last_player_generation{};
};

struct PluginContext {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteDamageEchoServiceV1* echo{};
    SharedState shared;
    double service_retry{};
    int window_open{1};
};

[[nodiscard]] AnomalyStringViewV1 View(const char* text) noexcept {
    return {text, text != nullptr ? std::strlen(text) : 0};
}

template <typename Service>
const Service* QueryService(const AnomalyHostApiV1* host, const char* id) noexcept {
    if (host == nullptr || host->query_service == nullptr) return nullptr;
    const void* table{};
    const AnomalyStringViewV1 service_id{
        id, std::strlen(id)};
    if (host->query_service(host->host_context, service_id, 1, &table).code !=
            ANOMALY_STATUS_V1_OK ||
        table == nullptr) {
        return nullptr;
    }
    const auto* service = static_cast<const Service*>(table);
    if (service->struct_size < sizeof(Service) || service->service_version < 1) return nullptr;
    return service;
}

void SetStatus(SharedState& shared, const char* text) {
    shared.status = text != nullptr ? text : "";
}

bool IsPlayer(const AnomalyNteCombatantSnapshotV1& player,
              const AnomalyGenerationHandleV1 participant) noexcept {
    return player.character.id != 0 && participant.id != 0 &&
        player.character.id == participant.id &&
        player.character.generation == participant.generation;
}

std::uint32_t RequestFlags(const AnomalyNteCombatEventV1& event) noexcept {
    std::uint32_t flags = ANOMALY_NTE_DAMAGE_ECHO_V1_SHOW_DAMAGE;
    if ((event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL) != 0) {
        flags |= ANOMALY_NTE_DAMAGE_ECHO_V1_CRITICAL;
    }
    if ((event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT) != 0) {
        flags |= ANOMALY_NTE_DAMAGE_ECHO_V1_HEAD_HIT;
    }
    if ((event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE) != 0) {
        flags |= ANOMALY_NTE_DAMAGE_ECHO_V1_WEAK_UNBALANCE;
    }
    return flags;
}

void FillRequest(const EchoTarget& target, AnomalyNteDamageEchoRequestV1& request) noexcept {
    request = {};
    request.struct_size = sizeof(request);
    request.flags = target.flags;
    request.world = target.world;
    request.victim = target.victim;
    request.damage = target.damage;
    request.damage_type = target.damage_type;
    request.display_type = target.display_type;
    request.reaction_type = target.reaction_type;
    request.reaction_display_type = target.reaction_display_type;
}

void RememberFirstHit(
    SharedState& shared,
    const AnomalyNteCombatEventV1& event,
    const AnomalyNteCombatantSnapshotV1& player) {
    if (event.kind != ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE ||
        !IsPlayer(player, event.source) || event.target.id == 0) {
        return;
    }
    const std::int64_t damage = event.final_value != 0 ? event.final_value : event.value;
    if (damage <= 0) return;
    if (shared.targets.size() >= kMaxTargets &&
        !shared.targets.contains(event.target.id)) {
        SetStatus(shared, "目标数量达到上限，忽略新的回响");
        return;
    }
    const auto [iterator, inserted] = shared.targets.try_emplace(event.target.id);
    if (!inserted) return;
    EchoTarget& target = iterator->second;
    target.world = event.world;
    target.victim = event.target;
    target.damage = damage;
    target.damage_type = event.damage_type;
    target.display_type = event.display_type;
    target.reaction_type = event.reaction_type;
    target.reaction_display_type = event.reaction_display_type;
    target.flags = RequestFlags(event);
    target.accumulator = 0.0;
}

void ApplyTargets(PluginContext& context, double delta_seconds) {
    SharedState& shared = context.shared;
    const std::uint32_t rate = std::clamp<std::uint32_t>(
        shared.ticks_per_second, 1U, kMaxTicksPerSecond);
    const double interval = 1.0 / static_cast<double>(rate);
    std::size_t applications{};
    std::vector<std::uint64_t> remove;
    remove.reserve(shared.targets.size());
    for (auto& [key, target] : shared.targets) {
        (void)key;
        target.accumulator += (std::max)(delta_seconds, 0.0);
        while (target.accumulator >= interval &&
               applications < kMaxApplicationsPerUpdate) {
            target.accumulator -= interval;
            ++applications;
            AnomalyNteDamageEchoRequestV1 request{};
            FillRequest(target, request);
            AnomalyNteDamageEchoResultV1 result{};
            result.struct_size = sizeof(result);
            const auto status = shared.mode == 0
                ? context.echo->apply_manual(context.echo->user, &request, &result)
                : context.echo->apply_native_dot(context.echo->user, &request, &result);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                target.failures = 0;
                if ((result.flags & ANOMALY_NTE_DAMAGE_ECHO_V1_DEAD) != 0 ||
                    result.dead != 0) {
                    remove.push_back(target.victim.id);
                    break;
                }
                continue;
            }
            if (status.code == ANOMALY_STATUS_V1_NOT_FOUND ||
                status.code == ANOMALY_STATUS_V1_INVALID_ARGUMENT) {
                remove.push_back(target.victim.id);
                break;
            }
            if (++target.failures >= 3) {
                remove.push_back(target.victim.id);
                if (shared.mode == 0 && status.code == ANOMALY_STATUS_V1_FAILED) {
                    SetStatus(shared, "A 模式写入被游戏拒绝；可切换到 B 原生 DoT");
                } else if (status.code == ANOMALY_STATUS_V1_PERMISSION_DENIED) {
                    SetStatus(shared, "宿主拒绝了伤害回响服务权限");
                } else {
                    SetStatus(shared, "伤害回响应用失败，已停止该目标");
                }
                break;
            }
        }
    }
    for (const auto id : remove) shared.targets.erase(id);
}

void RefreshServices(PluginContext& context) {
    if (context.combat == nullptr) {
        context.combat = QueryService<AnomalyNteCombatServiceV1>(
            context.host, ANOMALY_NTE_COMBAT_SERVICE_V1_ID);
    }
    if (context.echo == nullptr) {
        context.echo = QueryService<AnomalyNteDamageEchoServiceV1>(
            context.host, ANOMALY_NTE_DAMAGE_ECHO_SERVICE_V1_ID);
    }
}

void DrainEvents(PluginContext& context, const AnomalyNteCombatantSnapshotV1& player) {
    SharedState& shared = context.shared;
    for (std::size_t count{}; count < kMaxEventsPerUpdate; ++count) {
        AnomalyNteCombatEventV1 event{};
        event.struct_size = sizeof(event);
        const auto status = context.combat->next_event(
            context.combat->user, shared.cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) break;
        if (event.sequence <= shared.cursor) break;
        shared.cursor = event.sequence;
        if (event.world.generation != 0 &&
            shared.last_world_generation != 0 &&
            event.world.generation != shared.last_world_generation) {
            shared.targets.clear();
        }
        if (event.world.generation != 0) {
            shared.last_world_generation = event.world.generation;
        }
        RememberFirstHit(shared, event, player);
    }
}

void Update(PluginContext& context, double delta_seconds) {
    if (context.host == nullptr) return;
    context.service_retry += (std::max)(delta_seconds, 0.0);
    if (context.service_retry >= 0.5) {
        context.service_retry = 0.0;
        RefreshServices(context);
    }
    if (context.combat == nullptr || context.echo == nullptr) return;

    AnomalyNteCombatantSnapshotV1 player{};
    player.struct_size = sizeof(player);
    if (context.combat->current_combatant(context.combat->user, &player).code !=
            ANOMALY_STATUS_V1_OK ||
        (player.flags & ANOMALY_NTE_COMBATANT_V1_VALID) == 0) {
        return;
    }

    std::scoped_lock lock(context.shared.mutex);
    if (!context.shared.enabled) return;
    if (context.shared.cursor == 0) {
        context.shared.cursor = context.combat->latest_event_sequence(context.combat->user);
    }
    if (context.shared.last_player_id != player.character.id ||
        context.shared.last_player_generation != player.character.generation) {
        context.shared.last_player_id = player.character.id;
        context.shared.last_player_generation = player.character.generation;
        context.shared.targets.clear();
    }
    DrainEvents(context, player);
    if (context.shared.targets.empty()) {
        SetStatus(context.shared, "等待玩家对怪物的有效伤害");
        return;
    }
    ApplyTargets(context, delta_seconds);
    char status[160]{};
    std::snprintf(status, sizeof(status), "模式 %s / 目标 %zu / 速率 %u 次每秒",
        context.shared.mode == 0 ? "A 精确回响" : "B 原生 DoT",
        context.shared.targets.size(),
        static_cast<unsigned>(context.shared.ticks_per_second));
    context.shared.status = status;
}

void Draw(PluginContext& context, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr || ui->begin_window == nullptr) return;
    bool enabled{};
    std::uint32_t mode{};
    std::uint32_t rate{};
    std::size_t targets{};
    std::string status;
    {
        std::scoped_lock lock(context.shared.mutex);
        enabled = context.shared.enabled;
        mode = context.shared.mode;
        rate = context.shared.ticks_per_second;
        targets = context.shared.targets.size();
        status = context.shared.status;
    }
    const bool visible = ui->begin_window(
        ui->user, View("伤害回响 / Damage Echo"), &context.window_open, 0) != 0;
    if (visible) {
        const auto text = [ui](const char* value) {
            if (value != nullptr) ui->text(ui->user, View(value));
        };
        text("对玩家首次命中的每个怪物，按固定速率重复该次伤害，直到目标死亡。");
        int enabled_int = enabled ? 1 : 0;
        if (ui->checkbox(ui->user, View("启用回响"), &enabled_int) != 0) {
            std::scoped_lock lock(context.shared.mutex);
            context.shared.enabled = enabled_int != 0;
        }
        ui->separator(ui->user);
        text("模式");
        if (ui->button(ui->user, View("A 精确回响（HP + 官方伤害数字）"), 0.0F, 0.0F) != 0) {
            std::scoped_lock lock(context.shared.mutex);
            context.shared.mode = 0;
            context.shared.targets.clear();
            SetStatus(context.shared, "已切换 A 模式；等待新的有效伤害");
        }
        if (ui->button(ui->user, View("B 原生 DoT（游戏 GE 公式）"), 0.0F, 0.0F) != 0) {
            std::scoped_lock lock(context.shared.mutex);
            context.shared.mode = 1;
            context.shared.targets.clear();
            SetStatus(context.shared, "已切换 B 模式；伤害量由游戏 DoT GE 决定");
        }
        ui->separator(ui->user);
        float rate_value = static_cast<float>(rate);
        if (ui->slider_float(ui->user, View("每秒次数"), &rate_value, 1.0F,
                static_cast<float>(kMaxTicksPerSecond)) != 0) {
            const auto next = static_cast<std::uint32_t>(
                std::clamp(std::lround(rate_value), 1L,
                    static_cast<long>(kMaxTicksPerSecond)));
            std::scoped_lock lock(context.shared.mutex);
            context.shared.ticks_per_second = next;
        }
        char line[160]{};
        std::snprintf(line, sizeof(line), "当前模式：%s",
            mode == 0 ? "A 精确回响" : "B 原生 DoT");
        text(line);
        std::snprintf(line, sizeof(line), "活跃目标：%zu", targets);
        text(line);
        std::snprintf(line, sizeof(line), "服务：combat=%s / damage-echo=%s",
            context.combat != nullptr ? "可用" : "不可用",
            context.echo != nullptr ? "可用" : "不可用");
        text(line);
        text(status.c_str());
        if (mode == 1) {
            text("B 模式的跳伤由游戏原生 DoT GE 决定，速率只控制维护/重挂频率。");
        }
        if (ui->button(ui->user, View("清空目标"), 0.0F, 0.0F) != 0) {
            std::scoped_lock lock(context.shared.mutex);
            context.shared.targets.clear();
            SetStatus(context.shared, "已清空目标");
        }
    }
    ui->end_window(ui->user);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr ||
        host->struct_size < sizeof(AnomalyHostApiV1)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    auto* context = new (std::nothrow) PluginContext();
    if (context == nullptr) return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    context->host = host;
    context->ui = QueryService<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID);
    if (context->ui == nullptr) {
        delete context;
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    context->combat = QueryService<AnomalyNteCombatServiceV1>(
        host, ANOMALY_NTE_COMBAT_SERVICE_V1_ID);
    context->echo = QueryService<AnomalyNteDamageEchoServiceV1>(
        host, ANOMALY_NTE_DAMAGE_ECHO_SERVICE_V1_ID);
    *plugin_context = context;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<PluginContext*>(plugin_context);
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    {
        std::scoped_lock lock(context->shared.mutex);
        context->shared.targets.clear();
        context->shared.cursor = 0;
        context->shared.last_world_generation = 0;
        context->shared.last_player_id = 0;
        context->shared.last_player_generation = 0;
        context->shared.enabled = true;
        SetStatus(context->shared, "等待玩家对怪物的有效伤害");
    }
    RefreshServices(*context);
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* context = static_cast<PluginContext*>(plugin_context);
    if (context == nullptr) return {ANOMALY_STATUS_V1_OK, 0, {}};
    std::scoped_lock lock(context->shared.mutex);
    context->shared.targets.clear();
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<PluginContext*>(plugin_context);
    delete context;
}

void ANOMALY_CALL UpdateCallback(void* plugin_context, double delta_seconds) {
    auto* context = static_cast<PluginContext*>(plugin_context);
    if (context != nullptr) Update(*context, delta_seconds);
}

void ANOMALY_CALL DrawCallback(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<PluginContext*>(plugin_context);
    if (context != nullptr) Draw(*context, ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        {kPluginId, std::strlen(kPluginId)},
        {kPluginName, std::strlen(kPluginName)},
        {kPluginAuthor, std::strlen(kPluginAuthor)},
        {kPluginVersion, std::strlen(kPluginVersion)},
        Load,
        Start,
        Stop,
        Unload,
        UpdateCallback,
        DrawCallback};
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}
