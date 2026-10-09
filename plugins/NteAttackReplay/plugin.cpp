#include "anomaly/sdk/cpp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>

namespace {
constexpr std::uint32_t kInputNormal = 0;
constexpr std::uint32_t kInputSkill = 2;
constexpr std::uint32_t kInputUltimate = 3;
constexpr std::uint64_t kDamageTimeoutFrames = 120;
constexpr std::uint32_t kNameCapacity = 1024;

struct ViewSnapshot {
    AnomalyNteDamageEventV1 captured{sizeof(captured)};
    std::uint64_t frame{};
    std::uint64_t confirmed_hits{};
    std::uint64_t attempts{};
    std::uint32_t interval_frames{1};
    std::uint32_t input_id{kInputNormal};
    std::string source_name;
    std::string status{"自动记录已启动：等待玩家对敌人造成伤害"};
    bool captured_valid{};
    bool replay_enabled{};
};
struct UiIntent {
    bool enable_pending{};
    bool enable_value{};
    bool interval_pending{};
    std::uint32_t interval_frames{1};
    bool input_pending{};
    std::uint32_t input_id{kInputNormal};
};
struct Context {
    // Game-domain state. UI code never reads or writes these members directly.
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteAttackInputServiceV1* input{};
    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    AnomalyNteDamageEventV1 captured{sizeof(captured)};
    AnomalyNteAttackInputRequestV1 input_request{sizeof(input_request), 0, kInputNormal, 0};
    std::uint64_t event_cursor{};
    std::uint64_t frame{};
    std::uint64_t replay_start_sequence{};
    std::uint64_t pending_deadline{};
    std::uint64_t next_press_frame{};
    std::uint64_t confirmed_hits{};
    std::uint64_t attempts{};
    std::uint32_t interval_frames{1};
    std::string source_name;
    std::string status{"自动记录已启动：等待玩家对敌人造成伤害"};
    bool captured_valid{};
    bool input_overridden{};
    bool replay_enabled{};
    bool button_pressed{};
    bool awaiting_hit{};
    bool auto_input_matched{};

    // Lifecycle flags are atomic; on_stop never invokes a Game-thread-only service.
    std::atomic_bool running{};
    std::atomic_bool reset_replay_on_start{};

    // Cross-domain UI contract: on_draw copies ViewSnapshot in a short lock and queues intents.
    std::mutex ui_mutex;
    ViewSnapshot view;
    UiIntent intents;
} g;

void PublishView() {
    ViewSnapshot next;
    next.captured = g.captured;
    next.frame = g.frame;
    next.confirmed_hits = g.confirmed_hits;
    next.attempts = g.attempts;
    next.interval_frames = g.interval_frames;
    next.input_id = g.input_request.input_id;
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

AnomalyStatusV1 Status(std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}
bool SameHandle(AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) noexcept {
    return a.id != 0 && a.id == b.id && a.generation == b.generation;
}
bool ReadName(const AnomalyNteCombatServiceV1* service, const std::uint64_t source, std::string& output) {
    if (!service || !service->source_name_utf8 || source == 0) return false;
    std::size_t size{};
    auto result = service->source_name_utf8(service->user, source, nullptr, &size);
    if ((result.code != ANOMALY_STATUS_V1_OK && result.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) ||
        size <= 1 || size > kNameCapacity) return false;
    std::string value(size, '\0');
    result = service->source_name_utf8(service->user, source, value.data(), &size);
    if (result.code != ANOMALY_STATUS_V1_OK) return false;
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    output = std::move(value);
    return !output.empty();
}
bool ReadAbilityName(const AnomalyNteSkillsServiceV1* service,
                     AnomalyGenerationHandleV1 ability, std::string& output) {
    if (!service || !service->ability_display_name_utf8 || ability.id == 0) return false;
    std::size_t size{};
    auto result = service->ability_display_name_utf8(service->user, ability, nullptr, &size);
    if ((result.code != ANOMALY_STATUS_V1_OK && result.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) ||
        size <= 1 || size > kNameCapacity) return false;
    std::string value(size, '\0');
    result = service->ability_display_name_utf8(service->user, ability, value.data(), &size);
    if (result.code != ANOMALY_STATUS_V1_OK) return false;
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    output = std::move(value);
    return !output.empty();
}
bool MatchSkillInput(const std::string& source_name, std::int32_t& input_id) {
    if (!g.skills || !g.skills->frame || !g.skills->snapshot_at || source_name.empty()) return false;
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (g.skills->frame(g.skills->user, &frame).code != ANOMALY_STATUS_V1_OK || frame.skill_count == 0) return false;
    const std::uint32_t count = (std::min)(frame.skill_count, 128U);
    for (std::uint32_t i = 0; i < count; ++i) {
        AnomalyNteSkillSnapshotV1 skill{sizeof(skill)};
        if (g.skills->snapshot_at(g.skills->user, frame.generation, i, &skill).code != ANOMALY_STATUS_V1_OK) continue;
        std::string name;
        if (ReadAbilityName(g.skills, skill.ability_class, name) && name == source_name && skill.input_id >= 0 && skill.input_id <= 76) {
            input_id = skill.input_id;
            return true;
        }
    }
    return false;
}
void ReleaseIfPressed() noexcept {
    if (g.button_pressed && g.input && g.input->release) {
        static_cast<void>(g.input->release(g.input->user, &g.input_request));
    }
    g.button_pressed = false;
}
void Update(void*, double) {
    if (!g.running.load(std::memory_order_acquire)) return;
    PublishViewOnExit publish_view;

    UiIntent intent;
    {
        std::scoped_lock lock(g.ui_mutex);
        intent = g.intents;
        g.intents = {};
    }
    if (g.reset_replay_on_start.exchange(false, std::memory_order_acq_rel)) {
        g.replay_enabled = false;
        g.awaiting_hit = false;
        g.button_pressed = false;
        g.status = "自动记录已启动：等待玩家对敌人造成伤害";
    }
    if (intent.enable_pending) {
        g.replay_enabled = intent.enable_value;
        g.awaiting_hit = false;
        if (!g.replay_enabled) {
            g.status = "重放已关闭；自动记录仍继续";
        } else if (!g.captured_valid) {
            g.status = "重放已开启，但尚未录到玩家对敌人的命中事件";
        } else {
            g.next_press_frame = g.frame;
            g.status = "重放循环已开启，等待真实伤害事件确认";
        }
    }
    if (intent.interval_pending) g.interval_frames = (std::clamp)(intent.interval_frames, 1U, 10U);
    if (intent.input_pending) {
        g.input_request.input_id = intent.input_id;
        g.input_overridden = true;
        g.status = intent.input_id == kInputNormal ? "输入 ID 已设为普通攻击"
            : intent.input_id == kInputSkill ? "输入 ID 已设为技能" : "输入 ID 已设为大招";
    }

    ++g.frame;
    if (!g.combat || !g.combat->current_combatant || !g.combat->next_damage_event || !g.combat->latest_damage_sequence) return;
    AnomalyNteCombatantSnapshotV1 now{sizeof(now)};
    const auto combatant_status = g.combat->current_combatant(g.combat->user, &now);
    const bool player_valid = combatant_status.code == ANOMALY_STATUS_V1_OK &&
        (now.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 && now.character.id != 0;
    if (player_valid) g.combatant = now;

    // Drain only new events. Recording is automatic and independent of the replay checkbox.
    for (std::size_t drained = 0; drained < 24; ++drained) {
        AnomalyNteDamageEventV1 event{sizeof(event)};
        const auto status = g.combat->next_damage_event(g.combat->user, g.event_cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) break;
        if (event.sequence <= g.event_cursor) break;
        g.event_cursor = event.sequence;
        if (!player_valid || (event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) == 0 ||
            !SameHandle(event.attacker, g.combatant.character) || SameHandle(event.victim, g.combatant.character) ||
            event.victim.id == 0) continue;
        if (!g.captured_valid) {
            g.captured = event;
            g.captured_valid = true;
            g.source_name.clear();
            static_cast<void>(ReadName(g.combat, event.source_id, g.source_name));
            std::int32_t matched = -1;
            g.auto_input_matched = MatchSkillInput(g.source_name, matched);
            if (!g.input_overridden) {
                g.input_request.input_id = g.auto_input_matched ? static_cast<std::uint32_t>(matched) : kInputNormal;
            }
            g.status = g.auto_input_matched
                ? "已自动记录玩家命中事件，并按 DamageSource 匹配到技能输入"
                : "已自动记录玩家命中事件；技能名未能精确匹配，默认普通攻击，可手动切换输入类型";
        } else if (g.replay_enabled && g.awaiting_hit && event.sequence > g.replay_start_sequence &&
                   SameHandle(event.attacker, g.combatant.character) &&
                   SameHandle(event.victim, g.captured.victim) &&
                   (g.captured.source_id == 0 || event.source_id == g.captured.source_id)) {
            ++g.confirmed_hits;
            g.awaiting_hit = false;
            g.status = "已确认重放产生新的匹配 DamageEvent；继续循环";
            g.next_press_frame = g.frame + g.interval_frames;
        }
    }

    if (!g.replay_enabled || !g.captured_valid) {
        ReleaseIfPressed();
        return;
    }
    if (g.button_pressed) {
        const auto result = g.input->release(g.input->user, &g.input_request);
        g.button_pressed = false;
        if (result.code != ANOMALY_STATUS_V1_OK) {
            g.replay_enabled = false;
            g.awaiting_hit = false;
            g.status = "攻击输入 Release 失败，已自动停止以避免输入卡住";
            return;
        }
    }
    if (g.awaiting_hit) {
        if (g.frame > g.pending_deadline) {
            g.awaiting_hit = false;
            g.replay_enabled = false;
            g.status = "超时：未观察到新的匹配 DamageEvent，已停止；不将输入调用当成重放成功";
        }
        return;
    }
    if (g.frame < g.next_press_frame) return;
    g.replay_start_sequence = g.combat->latest_damage_sequence(g.combat->user);
    const auto result = g.input->press(g.input->user, &g.input_request);
    if (result.code != ANOMALY_STATUS_V1_OK) {
        g.replay_enabled = false;
        g.status = "游戏攻击输入调用失败，已停止";
        return;
    }
    ++g.attempts;
    g.button_pressed = true;
    // Keep input state inside one Game callback. on_stop runs in Lifecycle after the
    // callback barrier, so it must never be responsible for releasing a held game input.
    auto release = g.input->release(g.input->user, &g.input_request);
    if (release.code != ANOMALY_STATUS_V1_OK) {
        // One immediate retry is safer than carrying a pressed input into Stop/Unload.
        release = g.input->release(g.input->user, &g.input_request);
    }
    if (release.code != ANOMALY_STATUS_V1_OK) {
        g.replay_enabled = false;
        g.awaiting_hit = false;
        g.status = "攻击输入 Release 失败，已停止重放；请确认游戏输入状态";
        return;
    }
    g.button_pressed = false;
    g.awaiting_hit = true;
    g.pending_deadline = g.frame + kDamageTimeoutFrames;
    g.status = "输入已发送，等待同目标/同来源的新 DamageEvent 确认";
}

void Draw(void*, const AnomalyUiServiceV1* ui) {
    if (!g.running.load(std::memory_order_acquire) || ui == nullptr ||
        ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr || ui->checkbox == nullptr ||
        ui->button == nullptr || ui->slider_float == nullptr) return;

    ViewSnapshot view;
    {
        std::scoped_lock lock(g.ui_mutex);
        view = g.view;
    }

    int open = 1;
    anomaly::sdk::UiWindow window(ui, "NTE Attack Replay", &open, 0);
    if (!window) return; // RAII pairs EndWindow even when BeginWindow returns false.

    ui->text(ui->user, anomaly::sdk::StringView("录制始终自动运行；此复选框只控制重放循环。"));
    int enabled = view.replay_enabled ? 1 : 0;
    if (ui->checkbox(ui->user, anomaly::sdk::StringView("启用重放"), &enabled)) {
        std::scoped_lock lock(g.ui_mutex);
        g.intents.enable_pending = true;
        g.intents.enable_value = enabled != 0;
    }

    float interval = static_cast<float>(view.interval_frames);
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("两次输入间隔（游戏帧）"), &interval, 1.0F, 10.0F)) {
        std::scoped_lock lock(g.ui_mutex);
        g.intents.interval_pending = true;
        g.intents.interval_frames = static_cast<std::uint32_t>((std::clamp)(interval, 1.0F, 10.0F) + 0.5F);
    }
    const auto queue_input = [](const std::uint32_t input_id) {
        std::scoped_lock lock(g.ui_mutex);
        g.intents.input_pending = true;
        g.intents.input_id = input_id;
    };
    if (ui->button(ui->user, anomaly::sdk::StringView("普通攻击输入"), 120.0F, 0.0F)) queue_input(kInputNormal);
    if (ui->button(ui->user, anomaly::sdk::StringView("技能输入"), 100.0F, 0.0F)) queue_input(kInputSkill);
    if (ui->button(ui->user, anomaly::sdk::StringView("大招输入"), 100.0F, 0.0F)) queue_input(kInputUltimate);

    ui->text(ui->user, anomaly::sdk::StringView(view.captured_valid ? "录制状态：已捕获" : "录制状态：等待首次命中"));
    if (view.captured_valid) {
        char line[240]{};
        std::snprintf(line, sizeof(line), "DamageEvent #%llu | Source=%llu | Damage=%lld | InputID=%u",
            static_cast<unsigned long long>(view.captured.sequence),
            static_cast<unsigned long long>(view.captured.source_id),
            static_cast<long long>(view.captured.final_damage), view.input_id);
        ui->text(ui->user, anomaly::sdk::StringView(line));
        ui->text(ui->user, anomaly::sdk::StringView("目标已固定：重复事件必须来自本地玩家、同一目标且 DamageSource 匹配。"));
        if (!view.source_name.empty()) ui->text(ui->user, anomaly::sdk::StringView(std::string("DamageSource：") + view.source_name));
    }
    char stats[160]{};
    std::snprintf(stats, sizeof(stats), "输入尝试：%llu | 已确认新命中：%llu | 单次超时：%llu 帧",
        static_cast<unsigned long long>(view.attempts), static_cast<unsigned long long>(view.confirmed_hits),
        static_cast<unsigned long long>(kDamageTimeoutFrames));
    ui->text(ui->user, anomaly::sdk::StringView(stats));
    ui->text(ui->user, anomaly::sdk::StringView(view.status));
}

AnomalyStatusV1 Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!host || !plugin_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const anomaly::sdk::Host view(host);
    const auto combat = view.Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);
    const auto skills = view.Query<AnomalyNteSkillsServiceV1>(ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION);
    const auto input = view.Query<AnomalyNteAttackInputServiceV1>(ANOMALY_NTE_ATTACK_INPUT_SERVICE_V1_ID, ANOMALY_NTE_ATTACK_INPUT_SERVICE_V1_VERSION);
    if (!combat || !input || !combat->current_combatant || !combat->next_damage_event ||
        !combat->latest_damage_sequence || !input->press || !input->release)
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "required combat/attack-input service missing");
    g.combat = combat.get(); g.skills = skills.get(); g.input = input.get();
    g.event_cursor = g.combat->latest_damage_sequence(g.combat->user);
    g.captured = {sizeof(g.captured)};
    g.combatant = {sizeof(g.combatant)};
    g.frame = g.replay_start_sequence = g.pending_deadline = g.next_press_frame = 0;
    g.confirmed_hits = g.attempts = 0;
    g.interval_frames = 1;
    g.source_name.clear();
    g.status = "自动记录已启动：等待玩家对敌人造成伤害";
    g.captured_valid = g.input_overridden = g.replay_enabled = false;
    g.button_pressed = g.awaiting_hit = g.auto_input_matched = false;
    g.input_request = {sizeof(g.input_request), 0, kInputNormal, 0};
    g.running.store(false, std::memory_order_release);
    g.reset_replay_on_start.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(g.ui_mutex);
        g.intents = {};
        g.view = {};
    }
    PublishView();
    *plugin_context = &g;
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 Start(void*) {
    g.reset_replay_on_start.store(true, std::memory_order_release);
    g.running.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 Stop(void*, std::uint32_t) {
    // Stop runs on Lifecycle after normal callbacks drain. It only flips lifecycle state;
    // game-thread-only input calls are never made here.
    g.running.store(false, std::memory_order_release);
    g.reset_replay_on_start.store(true, std::memory_order_release);
    std::scoped_lock lock(g.ui_mutex);
    g.intents = {};
    g.view.replay_enabled = false;
    g.view.status = "插件已停止；未在 Lifecycle 线程调用游戏输入服务";
    return anomaly::sdk::Ok();
}
void Unload(void*) {
    g.running.store(false, std::memory_order_release);
    g.combat = nullptr;
    g.skills = nullptr;
    g.input = nullptr;
    {
        std::scoped_lock lock(g.ui_mutex);
        g.intents = {};
        g.view = {};
    }
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-attack-replay"), anomaly::sdk::StringView("NTE Attack Replay"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.4.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
