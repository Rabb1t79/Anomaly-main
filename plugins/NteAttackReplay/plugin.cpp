#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {
constexpr std::uint32_t kDefaultRate = 10;
constexpr std::uint32_t kMinimumRate = 1;
constexpr std::uint32_t kMaximumRate = 100;
constexpr double kMaximumDeltaSeconds = 0.25;
constexpr double kFailureRetrySeconds = 0.5;
constexpr std::size_t kNameCapacity = 1024;

struct ViewSnapshot {
    AnomalyNteDamageEventV1 captured{sizeof(captured)};
    std::uint64_t frame{};
    std::uint64_t successful_replays{};
    std::uint64_t attempts{};
    std::uint32_t rate_per_second{kDefaultRate};
    float last_damage_applied{};
    std::string source_name;
    std::string status{"自动记录已启动：等待玩家对敌人造成有效伤害"};
    bool captured_valid{};
    bool replay_enabled{};
};

struct UiIntent {
    bool enable_pending{};
    bool enable_value{};
    bool rate_pending{};
    std::uint32_t rate_per_second{kDefaultRate};
};

struct Context {
    // Game-domain mutable state. UI only sees a copied snapshot and posts intents.
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteDamageReplayServiceV1* damage_replay{};
    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    AnomalyNteDamageEventV1 captured{sizeof(captured)};
    std::uint64_t event_cursor{};
    std::uint64_t frame{};
    std::uint64_t successful_replays{};
    std::uint64_t attempts{};
    std::uint32_t rate_per_second{kDefaultRate};
    double replay_accumulator{};
    double retry_cooldown_seconds{};
    float last_damage_applied{};
    std::string source_name;
    std::string status{"自动记录已启动：等待玩家对敌人造成有效伤害"};
    bool captured_valid{};
    bool replay_enabled{};
    int window_open{1};

    std::atomic_bool running{};
    std::atomic_bool reset_for_start{};
    std::mutex ui_mutex;
    ViewSnapshot view;
    UiIntent intents;
} g;

AnomalyStatusV1 Status(std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

bool SameHandle(AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) noexcept {
    return a.id != 0 && a.id == b.id && a.generation == b.generation;
}

bool ReadSourceName(const AnomalyNteCombatServiceV1* service,
                    std::uint64_t source, std::string& output) {
    if (service == nullptr || service->source_name_utf8 == nullptr || source == 0) return false;
    std::size_t size{};
    auto result = service->source_name_utf8(service->user, source, nullptr, &size);
    if ((result.code != ANOMALY_STATUS_V1_OK &&
         result.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) ||
        size <= 1 || size > kNameCapacity) return false;
    std::string value(size, '\0');
    result = service->source_name_utf8(service->user, source, value.data(), &size);
    if (result.code != ANOMALY_STATUS_V1_OK) return false;
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    if (value.empty()) return false;
    output = std::move(value);
    return true;
}

void PublishView() {
    ViewSnapshot next;
    next.captured = g.captured;
    next.frame = g.frame;
    next.successful_replays = g.successful_replays;
    next.attempts = g.attempts;
    next.rate_per_second = g.rate_per_second;
    next.last_damage_applied = g.last_damage_applied;
    next.source_name = g.source_name;
    next.status = g.status;
    next.captured_valid = g.captured_valid;
    next.replay_enabled = g.replay_enabled;
    std::scoped_lock lock(g.ui_mutex);
    g.view = std::move(next);
}

struct PublishViewOnExit {
    ~PublishViewOnExit() { PublishView(); }
};

void ClearCapture(const char* message) {
    g.captured = {sizeof(g.captured)};
    g.captured_valid = false;
    g.source_name.clear();
    g.replay_accumulator = 0.0;
    g.retry_cooldown_seconds = 0.0;
    g.status = message;
}

void Update(void*, double delta_seconds) {
    if (!g.running.load(std::memory_order_acquire)) return;
    PublishViewOnExit publish_view;

    UiIntent intent;
    {
        std::scoped_lock lock(g.ui_mutex);
        intent = g.intents;
        g.intents = {};
    }

    if (g.reset_for_start.exchange(false, std::memory_order_acq_rel)) {
        g.replay_enabled = false;
        g.frame = 0;
        g.successful_replays = 0;
        g.attempts = 0;
        g.last_damage_applied = 0.0F;
        g.rate_per_second = kDefaultRate;
        g.replay_accumulator = 0.0;
        g.retry_cooldown_seconds = 0.0;
        if (g.combat != nullptr && g.combat->latest_damage_sequence != nullptr)
            g.event_cursor = g.combat->latest_damage_sequence(g.combat->user);
        ClearCapture("自动记录已启动：等待玩家对敌人造成有效伤害");
    }

    // The checkbox is a direct effect-enable value. Recording remains automatic
    // whether it is checked or not; there is no hidden toggle/replay-start mode.
    if (intent.enable_pending) {
        g.replay_enabled = intent.enable_value;
        g.replay_accumulator = 0.0;
        g.status = !g.replay_enabled
            ? "直接伤害效果已禁用；自动记录继续运行"
            : (g.captured_valid
                ? "直接伤害效果已启用；将对已记录目标重复应用伤害"
                : "直接伤害效果已启用；等待记录到首个有效玩家伤害事件");
    }
    if (intent.rate_pending) {
        g.rate_per_second = (std::clamp)(intent.rate_per_second, kMinimumRate, kMaximumRate);
        g.status = "重复伤害速率已更新：" + std::to_string(g.rate_per_second) + " 次/秒";
    }

    ++g.frame;
    if (g.combat == nullptr || g.damage_replay == nullptr ||
        g.combat->current_combatant == nullptr ||
        g.combat->next_damage_event == nullptr ||
        g.combat->latest_damage_sequence == nullptr ||
        g.damage_replay->apply_damage == nullptr) {
        g.status = "Core 战斗事件或直接伤害服务不可用；效果开关状态保持不变";
        return;
    }

    AnomalyNteCombatantSnapshotV1 current{sizeof(current)};
    const auto current_status = g.combat->current_combatant(g.combat->user, &current);
    const bool player_valid = current_status.code == ANOMALY_STATUS_V1_OK &&
        (current.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 &&
        current.character.id != 0 && current.world.id != 0;
    if (!player_valid) {
        g.status = "等待有效的本地玩家/世界快照；效果开关状态保持不变";
        g.replay_accumulator = 0.0;
        return;
    }
    g.combatant = current;

    // Captures are tied to the player and world generation where they occurred.
    // A world transition cannot accidentally replay an old actor handle.
    if (g.captured_valid &&
        (!SameHandle(g.captured.attacker, g.combatant.character) ||
         !SameHandle(g.captured.world, g.combatant.world))) {
        ClearCapture("世界或玩家代际已变化；自动等待新的玩家伤害事件");
    }

    // Automatic recording is independent of the effect checkbox. Capture only a
    // real player -> other-character event with a usable, positive final damage.
    for (std::size_t drained = 0; drained < 32; ++drained) {
        AnomalyNteDamageEventV1 event{sizeof(event)};
        const auto status = g.combat->next_damage_event(
            g.combat->user, g.event_cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) break;
        if (event.sequence <= g.event_cursor) break;
        g.event_cursor = event.sequence;

        if (g.captured_valid ||
            (event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) == 0 ||
            !SameHandle(event.attacker, g.combatant.character) ||
            SameHandle(event.victim, g.combatant.character) ||
            event.victim.id == 0 ||
            !SameHandle(event.world, g.combatant.world) ||
            event.final_damage <= 0 ||
            static_cast<double>(event.final_damage) > 1.0e9) continue;

        g.captured = event;
        g.captured_valid = true;
        g.source_name.clear();
        static_cast<void>(ReadSourceName(g.combat, event.source_id, g.source_name));
        g.last_damage_applied = 0.0F;
        g.replay_accumulator = 0.0;
        g.retry_cooldown_seconds = 0.0;
        g.status = "已自动记录首个有效伤害事件；可独立启用/禁用直接重复伤害";
    }

    if (!g.replay_enabled || !g.captured_valid) {
        g.replay_accumulator = 0.0;
        return;
    }

    double dt = std::isfinite(delta_seconds) && delta_seconds > 0.0
        ? (std::min)(delta_seconds, kMaximumDeltaSeconds) : 0.0;
    if (g.retry_cooldown_seconds > 0.0) {
        g.retry_cooldown_seconds = (std::max)(0.0, g.retry_cooldown_seconds - dt);
        return;
    }

    const double interval_seconds = 1.0 / static_cast<double>(g.rate_per_second);
    g.replay_accumulator += dt;
    // Unlimited repeats: the captured event stays active until disabled, its
    // target becomes invalid, or the game rejects the damage request.
    for (std::uint32_t emitted = 0;
         g.replay_accumulator >= interval_seconds && emitted < 32;
         ++emitted) {
        AnomalyNteDamageReplayRequestV1 request{};
        request.struct_size = sizeof(request);
        request.world = g.captured.world;
        request.attacker = g.captured.attacker;
        request.victim = g.captured.victim;
        request.damage = static_cast<float>(g.captured.final_damage);

        AnomalyNteDamageReplayResultV1 replay_result{sizeof(replay_result)};
        const auto result = g.damage_replay->apply_damage(
            g.damage_replay->user, &request, &replay_result);
        ++g.attempts;
        g.replay_accumulator -= interval_seconds;

        if (result.code == ANOMALY_STATUS_V1_OK &&
            (replay_result.flags & ANOMALY_NTE_DAMAGE_REPLAY_V1_VALID) != 0 &&
            std::isfinite(replay_result.damage_applied) &&
            replay_result.damage_applied > 0.0F) {
            ++g.successful_replays;
            g.last_damage_applied = replay_result.damage_applied;
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "直接伤害已确认：本次实际扣除 %.1f HP；成功重复 %llu 次",
                static_cast<double>(replay_result.damage_applied),
                static_cast<unsigned long long>(g.successful_replays));
            g.status = message;

            if ((replay_result.flags & ANOMALY_NTE_DAMAGE_REPLAY_V1_TARGET_ALIVE) == 0) {
                ClearCapture("目标已死亡；效果开关保持原值，自动等待下一次有效玩家伤害事件");
                break;
            }
        } else if (result.code == ANOMALY_STATUS_V1_CONFLICT ||
                   result.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            ClearCapture("目标或对象代际已失效；效果开关保持原值，自动等待下一次有效玩家伤害事件");
            break;
        } else {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "Core 未确认本次伤害（状态码 %u）；效果仍保持启用，稍后重试",
                result.code);
            g.status = message;
            g.replay_accumulator = 0.0;
            g.retry_cooldown_seconds = kFailureRetrySeconds;
            break;
        }
    }
}

void Draw(void*, const AnomalyUiServiceV1* ui) {
    if (!g.running.load(std::memory_order_acquire) || ui == nullptr ||
        ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr || ui->checkbox == nullptr ||
        ui->slider_float == nullptr) return;

    ViewSnapshot view;
    {
        std::scoped_lock lock(g.ui_mutex);
        view = g.view;
    }

    int open = g.window_open;
    anomaly::sdk::UiWindow window(ui, "NTE Attack Replay", &open, 0);
    g.window_open = open;
    if (!window) return;

    ui->text(ui->user, anomaly::sdk::StringView(
        "自动记录始终运行；启用只控制直接重复伤害，不发送玩家攻击输入。"));
    ui->text(ui->user, anomaly::sdk::StringView("重复次数：无上限"));

    int enabled = view.replay_enabled ? 1 : 0;
    if (ui->checkbox(ui->user, anomaly::sdk::StringView("启用直接伤害效果"), &enabled)) {
        std::scoped_lock lock(g.ui_mutex);
        g.intents.enable_pending = true;
        g.intents.enable_value = enabled != 0;
    }

    float rate = static_cast<float>(view.rate_per_second);
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("重复伤害速率（次/秒）"),
                         &rate, static_cast<float>(kMinimumRate),
                         static_cast<float>(kMaximumRate))) {
        std::scoped_lock lock(g.ui_mutex);
        g.intents.rate_pending = true;
        g.intents.rate_per_second = static_cast<std::uint32_t>(
            (std::clamp)(static_cast<int>(std::lround(rate)),
                         static_cast<int>(kMinimumRate), static_cast<int>(kMaximumRate)));
    }

    ui->text(ui->user, anomaly::sdk::StringView(
        view.captured_valid ? "录制状态：已捕获玩家伤害事件" : "录制状态：等待玩家对敌人造成伤害"));
    if (view.captured_valid) {
        char line[240]{};
        std::snprintf(line, sizeof(line),
            "事件 #%llu | 原始伤害 %lld | 最近实际扣血 %.1f HP | 来源 ID %llu",
            static_cast<unsigned long long>(view.captured.sequence),
            static_cast<long long>(view.captured.final_damage),
            static_cast<double>(view.last_damage_applied),
            static_cast<unsigned long long>(view.captured.source_id));
        ui->text(ui->user, anomaly::sdk::StringView(line));
        if (!view.source_name.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView(
                std::string("伤害来源：") + view.source_name));
        }
    }

    char stats[192]{};
    std::snprintf(stats, sizeof(stats),
        "速率：%u 次/秒 | 调用次数：%llu | 已确认重复：%llu",
        view.rate_per_second,
        static_cast<unsigned long long>(view.attempts),
        static_cast<unsigned long long>(view.successful_replays));
    ui->text(ui->user, anomaly::sdk::StringView(stats));
    ui->text(ui->user, anomaly::sdk::StringView(view.status));
    g.window_open = open;
}

AnomalyStatusV1 Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr)
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);

    const anomaly::sdk::Host api(host);
    const auto combat = api.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);
    const auto damage_replay = api.Query<AnomalyNteDamageReplayServiceV1>(
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_ID,
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_VERSION);
    if (!combat || !damage_replay ||
        combat->current_combatant == nullptr ||
        combat->next_damage_event == nullptr ||
        combat->latest_damage_sequence == nullptr ||
        damage_replay->apply_damage == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            "required combat-read/direct-damage service missing");
    }

    g.combat = combat.get();
    g.damage_replay = damage_replay.get();
    g.combatant = {sizeof(g.combatant)};
    g.captured = {sizeof(g.captured)};
    g.event_cursor = g.combat->latest_damage_sequence(g.combat->user);
    g.frame = 0;
    g.successful_replays = 0;
    g.attempts = 0;
    g.rate_per_second = kDefaultRate;
    g.replay_accumulator = 0.0;
    g.retry_cooldown_seconds = 0.0;
    g.last_damage_applied = 0.0F;
    g.source_name.clear();
    g.status = "自动记录已启动：等待玩家对敌人造成有效伤害";
    g.captured_valid = false;
    g.replay_enabled = false;
    g.running.store(false, std::memory_order_release);
    g.window_open = 1;
    g.reset_for_start.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(g.ui_mutex);
        g.intents = {};
        g.view = {};
    }
    PublishView();
    *plugin_context = &g;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 Start(void* plugin_context) {
    if (plugin_context != &g)
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g.window_open = 1;
    g.reset_for_start.store(true, std::memory_order_release);
    g.running.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g)
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g.running.store(false, std::memory_order_release);
    g.reset_for_start.store(true, std::memory_order_release);
    std::scoped_lock lock(g.ui_mutex);
    g.intents = {};
    g.view.replay_enabled = false;
    g.view.status = "插件已停止；未从 Lifecycle 线程调用游戏接口";
    return anomaly::sdk::Ok();
}

void Unload(void* plugin_context) {
    if (plugin_context != &g) return;
    g.running.store(false, std::memory_order_release);
    g.combat = nullptr;
    g.damage_replay = nullptr;
    std::scoped_lock lock(g.ui_mutex);
    g.intents = {};
    g.view = {};
}
} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-attack-replay"),
        anomaly::sdk::StringView("NTE Attack Replay"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.5.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
