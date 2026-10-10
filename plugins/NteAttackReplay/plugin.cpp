/*
 * NTE Attack Replay 1.6.0
 *
 * 录制：使用 Host 捕获的原生 HTDamageEvent，只保存已经发生的玩家对敌人伤害。
 * 重放：把原生伤害事件序号交给独立的 damage-replay Host 服务，由 Host 直接
 * 调用 dump 验证过的 HTAbilityCharacter.SetHP，再读回 HP 确认伤害确实生效。
 * 不触发玩家输入、技能、动画，也不重新执行原 GameplayEffect/攻击前中后链。
 */
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct Context final {
    std::mutex mutex;
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteDamageReplayServiceV1* replay{};
    const AnomalyUiServiceV1* ui{};

    // 自动记录光标只遍历已经发生的原生伤害记录。
    std::uint64_t cursor{};
    AnomalyGenerationHandleV1 player{};
    AnomalyGenerationHandleV1 captured_target{};
    std::uint64_t captured_damage_sequence{};
    std::int64_t captured_damage{};
    std::string captured_target_path;
    std::string captured_source_name;

    bool enabled{};
    bool captured{};
    bool replaying{};
    std::uint32_t rate{10};
    std::uint32_t replay_done{};
    std::uint32_t replay_target_count{};
    AnomalyNteDamageReplayResultV1 last_result{};
    std::atomic_bool replay_requested{false};
    std::atomic_bool stop_requested{false};
    std::string status{"自动记录已启动，等待玩家造成下一次有效伤害"};
};

template <typename Struct, typename Field>
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyStatusV1 Status(std::uint32_t code, std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

bool SameHandle(AnomalyGenerationHandleV1 left,
                AnomalyGenerationHandleV1 right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

bool CombatReady(const AnomalyNteCombatServiceV1* combat) noexcept {
    return combat != nullptr &&
        HasField<AnomalyNteCombatServiceV1,
            decltype(AnomalyNteCombatServiceV1::current_combatant)>(
                combat, offsetof(AnomalyNteCombatServiceV1, current_combatant)) &&
        HasField<AnomalyNteCombatServiceV1,
            decltype(AnomalyNteCombatServiceV1::latest_damage_sequence)>(
                combat, offsetof(AnomalyNteCombatServiceV1, latest_damage_sequence)) &&
        HasField<AnomalyNteCombatServiceV1,
            decltype(AnomalyNteCombatServiceV1::next_damage_event)>(
                combat, offsetof(AnomalyNteCombatServiceV1, next_damage_event)) &&
        combat->current_combatant != nullptr &&
        combat->latest_damage_sequence != nullptr &&
        combat->next_damage_event != nullptr;
}

bool ReplayReady(const AnomalyNteDamageReplayServiceV1* replay) noexcept {
    return replay != nullptr &&
        HasField<AnomalyNteDamageReplayServiceV1,
            decltype(AnomalyNteDamageReplayServiceV1::replay_event)>(
                replay, offsetof(AnomalyNteDamageReplayServiceV1, replay_event)) &&
        replay->replay_event != nullptr;
}

bool UiReady(const AnomalyUiServiceV1* ui) noexcept {
    return ui != nullptr &&
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::begin_window)>(
                ui, offsetof(AnomalyUiServiceV1, begin_window)) &&
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::end_window)>(
                ui, offsetof(AnomalyUiServiceV1, end_window)) &&
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::text)>(
                ui, offsetof(AnomalyUiServiceV1, text)) &&
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::button)>(
                ui, offsetof(AnomalyUiServiceV1, button)) &&
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::checkbox)>(
                ui, offsetof(AnomalyUiServiceV1, checkbox)) &&
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::input_uint32)>(
                ui, offsetof(AnomalyUiServiceV1, input_uint32)) &&
        ui->begin_window != nullptr && ui->end_window != nullptr &&
        ui->text != nullptr && ui->button != nullptr &&
        ui->checkbox != nullptr && ui->input_uint32 != nullptr;
}

std::string ReadParticipantPath(const AnomalyNteCombatServiceV1* combat,
                                AnomalyGenerationHandleV1 participant) {
    if (combat == nullptr || participant.id == 0 ||
        !HasField<AnomalyNteCombatServiceV1,
            decltype(AnomalyNteCombatServiceV1::participant_path_utf8)>(
                combat, offsetof(AnomalyNteCombatServiceV1, participant_path_utf8)) ||
        combat->participant_path_utf8 == nullptr) return {};
    std::size_t size{};
    const auto sizing = combat->participant_path_utf8(
        combat->user, participant, nullptr, &size);
    if (sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL ||
        size < 2 || size > 4096) return {};
    std::string value(size, '\0');
    const auto status = combat->participant_path_utf8(
        combat->user, participant, value.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK) return {};
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

std::string ReadDamageSourceName(const AnomalyNteCombatServiceV1* combat,
                                 std::uint64_t source_id) {
    if (combat == nullptr || source_id == 0 ||
        !HasField<AnomalyNteCombatServiceV1,
            decltype(AnomalyNteCombatServiceV1::source_name_utf8)>(
                combat, offsetof(AnomalyNteCombatServiceV1, source_name_utf8)) ||
        combat->source_name_utf8 == nullptr) return {};
    std::size_t size{};
    const auto sizing = combat->source_name_utf8(combat->user, source_id, nullptr, &size);
    if (sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL ||
        size < 2 || size > 1024) return {};
    std::string value(size, '\0');
    const auto status = combat->source_name_utf8(
        combat->user, source_id, value.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK) return {};
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

// 自动记录真实发生过的原生伤害；不扫描技能或输入链，也不发起新攻击。
bool CaptureNextDamage(Context& context) {
    if (!CombatReady(context.combat) || !ReplayReady(context.replay)) return false;

    AnomalyNteCombatantSnapshotV1 combatant{};
    combatant.struct_size = sizeof(combatant);
    const auto combatant_status = context.combat->current_combatant(
        context.combat->user, &combatant);
    if (combatant_status.code != ANOMALY_STATUS_V1_OK ||
        combatant.character.id == 0 || combatant.world.generation == 0) {
        return false;
    }
    context.player = combatant.character;

    for (std::uint32_t index = 0; index < 32; ++index) {
        AnomalyNteDamageEventV1 event{};
        event.struct_size = sizeof(event);
        const auto status = context.combat->next_damage_event(
            context.combat->user, context.cursor, &event);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            // If the ring cursor expired, rebase to the Host tail; do not guess an old hit.
            const auto latest = context.combat->latest_damage_sequence(context.combat->user);
            if (latest > context.cursor) context.cursor = latest;
            return false;
        }
        if (status.code != ANOMALY_STATUS_V1_OK) {
            context.status = "自动记录暂不可用：Host 读取原生伤害事件失败";
            return false;
        }
        context.cursor = event.sequence;

        if ((event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) == 0 ||
            !SameHandle(event.attacker, combatant.character) ||
            event.victim.id == 0 ||
            SameHandle(event.victim, combatant.character) ||
            event.world.generation != combatant.world.generation ||
            event.final_damage <= 0) {
            continue;
        }

        context.captured = true;
        context.replaying = false;
        context.captured_damage_sequence = event.sequence;
        context.captured_target = event.victim;
        context.captured_damage = event.final_damage;
        context.captured_target_path = ReadParticipantPath(context.combat, event.victim);
        context.captured_source_name = ReadDamageSourceName(context.combat, event.source_id);
        context.replay_done = 0;
        context.replay_target_count = 0;
        context.last_result = {};
        context.status = "已记录真实伤害：" + std::to_string(event.final_damage) +
            "；重放会直接提交相同伤害值，不重新执行攻击";
        return true;
    }
    return false;
}

void ClearCapturedDamage(Context& context) {
    context.captured = false;
    context.captured_target = {};
    context.captured_damage_sequence = 0;
    context.captured_damage = 0;
    context.captured_target_path.clear();
    context.captured_source_name.clear();
    context.replay_done = 0;
    context.replay_target_count = 0;
}

void FinishReplay(Context& context, std::string message) {
    context.replaying = false;
    context.status = std::move(message);
    if (CombatReady(context.combat)) {
        // SetHP can trigger game notifications; skip any related records from this replay.
        context.cursor = context.combat->latest_damage_sequence(context.combat->user);
    }
    ClearCapturedDamage(context);
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    auto* context = new (std::nothrow) Context{};
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);

    const anomaly::sdk::Host sdk_host(host);
    context->combat = sdk_host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
        ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    context->replay = sdk_host.Query<AnomalyNteDamageReplayServiceV1>(
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_ID,
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_VERSION).get();
    context->ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();

    if (!CombatReady(context->combat) || !ReplayReady(context->replay) ||
        !UiReady(context->ui)) {
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            "direct damage replay needs UI, NTE combat-read and NTE damage-replay services");
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(context->mutex);
    context->enabled = false;
    context->replaying = false;
    context->replay_requested.store(false, std::memory_order_release);
    context->stop_requested.store(false, std::memory_order_release);
    ClearCapturedDamage(*context);
    context->last_result = {};
    context->cursor = context->combat->latest_damage_sequence(context->combat->user);
    context->status = "自动记录已启动，等待玩家造成下一次有效伤害";
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->stop_requested.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    std::scoped_lock lock(context->mutex);

    if (context->stop_requested.exchange(false, std::memory_order_acq_rel)) {
        context->replaying = false;
        context->status = "已停止直接伤害重放；记录的伤害仍保留";
    }

    if (context->replay_requested.exchange(false, std::memory_order_acq_rel)) {
        if (context->enabled && context->captured && !context->replaying) {
            context->replaying = true;
            context->replay_done = 0;
            // 延续已约定速率：X 对同一原生伤害追加 X+1 次直接重放；每帧一次。
            context->replay_target_count = context->rate + 1U;
            context->status = "直接伤害重放已启动：每个 Game 更新帧最多一次";
        } else {
            context->status = !context->enabled
                ? "无法重放：请先启用重放功能"
                : "无法重放：尚未捕获玩家对敌人的有效伤害";
        }
    }

    if (!context->captured && !context->replaying) {
        CaptureNextDamage(*context);
    }
    if (!context->replaying) return;

    AnomalyNteDamageReplayRequestV1 request{};
    request.struct_size = sizeof(request);
    request.damage_sequence = context->captured_damage_sequence;
    AnomalyNteDamageReplayResultV1 result{};
    result.struct_size = sizeof(result);
    const auto status = context->replay->replay_event(
        context->replay->user, &request, &result);
    if (status.code != ANOMALY_STATUS_V1_OK ||
        (result.flags & ANOMALY_NTE_DAMAGE_REPLAY_V1_APPLIED) == 0 ||
        result.damage_sequence != context->captured_damage_sequence ||
        !SameHandle(result.target, context->captured_target) ||
        result.applied_damage <= 0.0F ||
        result.hp_after >= result.hp_before) {
        const std::string detail =
            status.message.data != nullptr && status.message.size != 0
                ? std::string(status.message.data, status.message.size)
                : ("Host status " + std::to_string(status.code));
        context->replaying = false;
        context->status = "直接伤害重放失败：" + detail;
        return;
    }

    context->last_result = result;
    ++context->replay_done;
    context->status = "已直接重放伤害 " + std::to_string(context->replay_done) +
        "/" + std::to_string(context->replay_target_count) +
        "；本次实际扣血 " + std::to_string(result.applied_damage);
    if (context->replay_done >= context->replay_target_count) {
        const auto last_result = context->last_result;
        FinishReplay(*context,
            "直接伤害重放完成；最近一次扣血 " +
            std::to_string(last_result.applied_damage) + "，HP " +
            std::to_string(last_result.hp_before) + " → " +
            std::to_string(last_result.hp_after) +
            "；自动记录将等待下一次玩家伤害");
        context->last_result = last_result;
    }
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || !UiReady(ui)) return;
    std::scoped_lock lock(context->mutex);

    int open = 1;
    if (ui->set_next_window_size != nullptr &&
        HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::set_next_window_size)>(
                ui, offsetof(AnomalyUiServiceV1, set_next_window_size))) {
        ui->set_next_window_size(ui->user, 440.0F, 0.0F, 4U);
    }
    const int visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView("NTE Attack Replay | 直接伤害重放"), &open, 0);
    if (visible != 0) {
        int enabled = context->enabled ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView("[启用]"), &enabled) != 0) {
            context->enabled = enabled != 0;
            context->status = context->enabled
                ? (context->captured ? "直接重放已启用：已保留记录伤害"
                                     : "直接重放已启用：自动等待玩家伤害")
                : "自动记录持续运行；直接重放功能已关闭";
            if (!context->enabled) {
                context->stop_requested.store(true, std::memory_order_release);
            }
        }
        ui->text(ui->user, anomaly::sdk::StringView(context->status));
        if (ui->separator != nullptr) ui->separator(ui->user);

        ui->input_uint32(
            ui->user, anomaly::sdk::StringView("追加直接伤害次数 X（每次命中追加 X+1 次）"),
            &context->rate, 1U, 10U);
        context->rate = std::clamp(context->rate, 0U, 9999U);

        if (context->captured) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "已捕获伤害值：" + std::to_string(context->captured_damage)));
            if (!context->captured_target_path.empty()) {
                ui->text(ui->user, anomaly::sdk::StringView(
                    "目标：" + context->captured_target_path));
            }
            if (!context->captured_source_name.empty()) {
                ui->text(ui->user, anomaly::sdk::StringView(
                    "原伤害来源：" + context->captured_source_name));
            }
            ui->text(ui->user, anomaly::sdk::StringView(
                "重放进度：" + std::to_string(context->replay_done) + "/" +
                std::to_string(context->replay_target_count != 0
                    ? context->replay_target_count : context->rate + 1U)));
        } else {
            ui->text(ui->user, anomaly::sdk::StringView(
                "自动等待下一次玩家对敌人的有效伤害，无需手动录制。"));
        }
        if ((context->last_result.flags & ANOMALY_NTE_DAMAGE_REPLAY_V1_APPLIED) != 0) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "最近一次 HP：" + std::to_string(context->last_result.hp_before) +
                " → " + std::to_string(context->last_result.hp_after) +
                "（实际扣除 " + std::to_string(context->last_result.applied_damage) + "）"));
        }
        ui->text(ui->user, anomaly::sdk::StringView(
            "模式：直接重放已造成的伤害数值；不重放攻击输入、技能、动画或原效果链。"));

        if (context->enabled && context->captured && !context->replaying) {
            if (ui->button(ui->user, anomaly::sdk::StringView("开始直接重放"), 0.0F, 0.0F) != 0) {
                context->replay_requested.store(true, std::memory_order_release);
                context->status = "已提交直接伤害重放请求";
            }
        } else if (context->replaying) {
            if (ui->button(ui->user, anomaly::sdk::StringView("停止重放"), 0.0F, 0.0F) != 0) {
                context->stop_requested.store(true, std::memory_order_release);
            }
        }
    }
    // 即使窗口内容被裁剪，BeginWindow/EndWindow 也必须始终配对。
    ui->end_window(ui->user);
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-attack-replay"),
        anomaly::sdk::StringView("NTE Attack Replay"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.6.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw,
    };
    return anomaly::sdk::Ok();
}
