#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <cstring>
#include <string>
#include <string_view>

namespace {

struct Context final {
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* invocation{};
    const AnomalyUiServiceV1* ui{};

    // The plugin is always armed: this cursor is advanced from the last observed
    // combat event, so only a damage event that occurs after startup can be captured.
    uint64_t combat_cursor{};

    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 character{};

    // These identify the skill that was active around the captured damage event.
    AnomalyGenerationHandleV1 captured_skill{};
    AnomalyGenerationHandleV1 captured_ability{};
    int32_t captured_input_id{-1};
    uint64_t captured_damage_sequence{};
    uint64_t captured_tick_sequence{};
    std::string captured_ability_path;

    bool captured{};
    bool replaying{};
    uint32_t replay_count{10};
    uint32_t replay_done{};
    double replay_rate{2.0};
    std::chrono::steady_clock::time_point next_replay{};
    std::string status{"自动等待玩家下一次攻击"};

    uint64_t last_skill_generation{};
    uint64_t last_skill_sequence{};
};

template <typename Struct, typename Field>
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool CombatReady(const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
                    decltype(AnomalyNteCombatServiceV1::next_event)>(
               service, offsetof(AnomalyNteCombatServiceV1, next_event)) &&
           service->latest_event_sequence != nullptr &&
           service->next_event != nullptr &&
           service->current_combatant != nullptr;
}

bool SkillsReady(const AnomalyNteSkillsServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillsServiceV1,
                    decltype(AnomalyNteSkillsServiceV1::page)>(
               service, offsetof(AnomalyNteSkillsServiceV1, page)) &&
           service->frame != nullptr && service->page != nullptr;
}

bool InvocationReady(const AnomalyNteSkillInvocationServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillInvocationServiceV1,
                    decltype(AnomalyNteSkillInvocationServiceV1::activate)>(
               service, offsetof(AnomalyNteSkillInvocationServiceV1, activate)) &&
           service->activate != nullptr;
}

bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return HasField<AnomalyUiServiceV1,
                    decltype(AnomalyUiServiceV1::end_window)>(
               service, offsetof(AnomalyUiServiceV1, end_window)) &&
           service->begin_window != nullptr && service->end_window != nullptr &&
           service->text != nullptr && service->button != nullptr &&
           service->input_uint32 != nullptr && service->input_double != nullptr;
}

AnomalyStatusV1 Status(uint32_t code, std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

bool SameHandle(AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) noexcept {
    return a.id == b.id && a.generation == b.generation;
}

std::string ReadAbilityPath(
    const AnomalyNteSkillsServiceV1* skills,
    AnomalyGenerationHandleV1 ability) {
    if (skills == nullptr || skills->ability_path_utf8 == nullptr) return {};
    size_t size = 0;
    const auto sizing = skills->ability_path_utf8(skills->user, ability, nullptr, &size);
    if (sizing.code != ANOMALY_STATUS_V1_OK &&
        sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) {
        return {};
    }
    std::string result(size, '\0');
    if (size == 0) return {};
    if (skills->ability_path_utf8(
            skills->user, ability, result.data(), &size).code != ANOMALY_STATUS_V1_OK) {
        return {};
    }
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

// Refresh the candidate skill before the combat stream is consumed. The Host's skill
// snapshot is immutable for its sequence, so the plugin never walks UE objects itself.
void UpdateSkillCandidate(Context& context) {
    if (!SkillsReady(context.skills)) return;

    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        frame.character.id == 0) {
        return;
    }

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> skills{};
    AnomalyNteSkillPageRequestV1 request{sizeof(request)};
    request.generation = frame.generation;
    request.offset = 0;
    request.capacity = static_cast<uint32_t>(skills.size());
    AnomalyNteSkillPageResultV1 result{sizeof(result)};

    if (context.skills->page(
            context.skills->user, &request, skills.data(), &result).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }

    context.last_skill_generation = frame.generation;
    context.last_skill_sequence = frame.sequence;

    // Prefer an explicitly pressed skill, then an active skill. This creates a
    // deterministic correlation window around the next player damage event.
    const AnomalyNteSkillSnapshotV1* selected = nullptr;
    for (uint32_t i = 0; i < result.returned; ++i) {
        if ((skills[i].flags & ANOMALY_NTE_SKILL_V1_INPUT_PRESSED) != 0) {
            selected = &skills[i];
            break;
        }
    }
    if (selected == nullptr) {
        for (uint32_t i = 0; i < result.returned; ++i) {
            if ((skills[i].flags & ANOMALY_NTE_SKILL_V1_ACTIVE) != 0) {
                selected = &skills[i];
                break;
            }
        }
    }
    if (selected != nullptr) {
        context.captured_skill = selected->handle;
        context.captured_ability = selected->ability_class;
        context.captured_input_id = selected->input_id;
    }
}

bool ResolveReplaySkill(Context& context, AnomalyGenerationHandleV1* skill_out) {
    if (!SkillsReady(context.skills) || skill_out == nullptr) return false;

    // A handle from the capture can remain valid through the replay. Check it first.
    if (context.skills->snapshot_by_handle != nullptr && context.captured_skill.id != 0) {
        AnomalyNteSkillSnapshotV1 snapshot{sizeof(snapshot)};
        if (context.skills->snapshot_by_handle(
                context.skills->user, context.captured_skill, &snapshot).code ==
            ANOMALY_STATUS_V1_OK) {
            if (SameHandle(snapshot.character, context.character)) {
                *skill_out = snapshot.handle;
                return true;
            }
        }
    }

    // Character/skill generations may refresh after the original attack. In that case,
    // remap by ability class and input id instead of using a stale opaque handle.
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> skills{};
    AnomalyNteSkillPageRequestV1 request{sizeof(request)};
    request.generation = frame.generation;
    request.offset = 0;
    request.capacity = static_cast<uint32_t>(skills.size());
    AnomalyNteSkillPageResultV1 result{sizeof(result)};
    if (context.skills->page(
            context.skills->user, &request, skills.data(), &result).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }

    for (uint32_t i = 0; i < result.returned; ++i) {
        const auto& skill = skills[i];
        if (!SameHandle(skill.character, context.character)) continue;
        if (context.captured_ability.id != 0 &&
            SameHandle(skill.ability_class, context.captured_ability)) {
            *skill_out = skill.handle;
            return true;
        }
    }
    for (uint32_t i = 0; i < result.returned; ++i) {
        const auto& skill = skills[i];
        if (SameHandle(skill.character, context.character) &&
            skill.input_id == context.captured_input_id) {
            *skill_out = skill.handle;
            return true;
        }
    }
    return false;
}

void ArmForNextAttack(Context& context) {
    context.captured = false;
    context.replaying = false;
    context.replay_done = 0;
    context.captured_skill = {};
    context.captured_ability = {};
    context.captured_input_id = -1;
    context.captured_damage_sequence = 0;
    context.captured_tick_sequence = 0;
    context.captured_ability_path.clear();
    context.status = "自动等待玩家下一次攻击";

    // Starting at the current tail prevents an old combat event from being mistaken
    // for the next attack after a world change or plugin restart.
    if (context.combat != nullptr) {
        context.combat_cursor = context.combat->latest_event_sequence(
            context.combat->user);
    }
}

bool CaptureNextAttack(Context& context) {
    if (!CombatReady(context.combat)) return false;

    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    if (context.combat->current_combatant(
            context.combat->user, &combatant).code != ANOMALY_STATUS_V1_OK ||
        combatant.character.id == 0) {
        return false;
    }

    context.world = combatant.world;
    context.character = combatant.character;

    // Keep the candidate skill fresh immediately before consuming the combat stream.
    UpdateSkillCandidate(context);

    for (uint32_t i = 0; i < 16; ++i) {
        AnomalyNteCombatEventV1 event{sizeof(event)};
        const auto status = context.combat->next_event(
            context.combat->user, context.combat_cursor, &event);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            // A stale cursor can happen when the Host replaces the combat ring or world.
            // Rebase to the current tail; old events must never be replayed as a new attack.
            context.combat_cursor = context.combat->latest_event_sequence(
                context.combat->user);
            return false;
        }
        if (status.code != ANOMALY_STATUS_V1_OK) {
            return false;
        }
        context.combat_cursor = event.sequence;

        if (event.kind != ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE ||
            !SameHandle(event.source, combatant.character)) {
            continue;
        }

        context.captured = true;
        context.captured_damage_sequence = event.sequence;
        context.captured_tick_sequence = event.tick_sequence;
        context.captured_ability_path =
            ReadAbilityPath(context.skills, context.captured_ability);
        context.status = "已自动捕获下一次玩家攻击";
        return true;
    }
    return false;
}

bool ReplayOnce(Context& context) {
    if (!InvocationReady(context.invocation) || !context.captured) return false;

    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    if (context.combat->current_combatant(
            context.combat->user, &combatant).code != ANOMALY_STATUS_V1_OK ||
        combatant.character.id == 0 || combatant.world.id == 0) {
        return false;
    }

    context.world = combatant.world;
    context.character = combatant.character;

    AnomalyGenerationHandleV1 skill{};
    if (!ResolveReplaySkill(context, &skill)) return false;

    AnomalyNteSkillInvocationRequestV1 request{sizeof(request)};
    request.world = context.world;
    request.character = context.character;
    request.skill = skill;

    AnomalyNteSkillInvocationResultV1 result{sizeof(result)};
    const auto status = context.invocation->activate(
        context.invocation->user, &request, &result);
    if (status.code != ANOMALY_STATUS_V1_OK || result.accepted == 0) {
        return false;
    }
    return true;
}

void ANOMALY_CALL LoadDummy() {}

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
    context->skills = sdk_host.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID,
        ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
    context->invocation = sdk_host.Query<AnomalyNteSkillInvocationServiceV1>(
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).get();
    context->ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();

    if (!CombatReady(context->combat) || !SkillsReady(context->skills) ||
        !InvocationReady(context->invocation) || !UiReady(context->ui)) {
        delete context;
        return Status(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "attack replay requires NTE combat, skills, skill-invocation and UI services");
    }

    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->replay_count = 10;
    context->replay_rate = 2.0;
    context->replay_done = 0;
    context->replaying = false;
    ArmForNextAttack(*context);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->replaying = false;
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;

    if (!context->captured && !context->replaying) {
        CaptureNextAttack(*context);
    }

    if (!context->replaying) return;

    const auto now = std::chrono::steady_clock::now();
    if (context->replay_done >= context->replay_count) {
        context->replaying = false;
        context->status = "重放完成，继续自动等待下一次攻击";
        ArmForNextAttack(*context);
        return;
    }

    if (now < context->next_replay) return;

    if (!ReplayOnce(*context)) {
        context->replaying = false;
        context->status = "重放失败：当前技能已不可用";
        ArmForNextAttack(*context);
        return;
    }

    ++context->replay_done;
    const double interval_seconds =
        1.0 / std::clamp(context->replay_rate, 0.1, 30.0);
    context->next_replay =
        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(interval_seconds));

    if (context->replay_done >= context->replay_count) {
        context->replaying = false;
        context->status = "重放完成，继续自动等待下一次攻击";
        ArmForNextAttack(*context);
    } else {
        context->status = "正在重放";
    }
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || !UiReady(ui)) return;

    int open = 1;
    if (ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 430.0F, 0.0F, 4U);
    }
    // begin_window/end_window are always paired. The host UI follows ImGui's
    // Begin/End rule: End is required even when Begin returns false.
    const int window_visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView("自动攻击录制/重放"), &open, 0);
    if (window_visible != 0) {
        ui->text(ui->user, anomaly::sdk::StringView(context->status));

    if (ui->separator != nullptr) ui->separator(ui->user);

    if (ui->input_uint32 != nullptr) {
        ui->input_uint32(
            ui->user, anomaly::sdk::StringView("重放次数"),
            &context->replay_count, 1, 10);
        context->replay_count = std::clamp(context->replay_count, 1u, 100000u);
    }

    if (ui->input_double != nullptr) {
        ui->input_double(
            ui->user, anomaly::sdk::StringView("重放速率（次/秒）"),
            &context->replay_rate, 0.1, 1.0);
        context->replay_rate = std::clamp(context->replay_rate, 0.1, 30.0);
    }

    if (context->captured) {
        ui->text(ui->user, anomaly::sdk::StringView(
            context->captured_ability_path.empty()
                ? "已捕获攻击动作"
                : context->captured_ability_path));
        const std::string progress =
            "进度：" + std::to_string(context->replay_done) + "/" +
            std::to_string(context->replay_count);
        ui->text(ui->user, anomaly::sdk::StringView(progress));
    } else {
        ui->text(ui->user, anomaly::sdk::StringView(
            "无需手动录制：插件自动等待下一次玩家攻击"));
    }

    if (!context->replaying && context->captured) {
        if (ui->button(
                ui->user, anomaly::sdk::StringView("开始重放"), 0.0F, 0.0F) != 0) {
            context->replay_done = 0;
            context->replaying = true;
            context->next_replay = std::chrono::steady_clock::now();
            context->status = "正在重放";
        }
    } else if (context->replaying) {
        if (ui->button(
                ui->user, anomaly::sdk::StringView("停止重放"), 0.0F, 0.0F) != 0) {
            context->replaying = false;
            context->status = "已停止重放，继续自动等待下一次攻击";
            ArmForNextAttack(*context);
        }
    }

        ui->end_window(ui->user);
    }

    // end_window must also be called when begin_window returned false.
    if (window_visible == 0) {
        ui->end_window(ui->user);
    }
}

}  // namespace

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
