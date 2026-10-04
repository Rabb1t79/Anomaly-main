// Record the first player->target DAMAGE event; normal attacks do not require a skill.
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <atomic>
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
    AnomalyGenerationHandleV1 captured_target{};
    int32_t captured_input_id{-1};
    uint64_t captured_damage_sequence{};
    uint64_t captured_tick_sequence{};
    int64_t captured_damage_value{};
    int64_t captured_basic_value{};
    int64_t captured_final_value{};
    uint32_t captured_damage_type{};
    uint32_t captured_reaction_type{};
    bool captured_has_skill{};
    std::string captured_ability_path;
    std::string captured_target_path;
    // Keep the Host-resolved DamageSource for cross-checking against the reflected skill.
    std::string captured_damage_source_name;

    bool enabled{};
    bool captured{};
    bool replaying{};
    // Draw runs in the Render domain. Replay/stop requests are posted atomically
    // and executed by Update() in the Game callback domain required by the ABI.
    std::atomic_bool replay_requested{false};
    std::atomic_bool stop_requested{false};
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
           service->text != nullptr && service->checkbox != nullptr && service->button != nullptr &&
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

std::string ReadDamageSourceName(
    const AnomalyNteCombatServiceV1* combat,
    uint64_t source_id) {
    if (combat == nullptr || combat->source_name_utf8 == nullptr || source_id == 0) return {};
    size_t size = 0;
    const auto sizing = combat->source_name_utf8(combat->user, source_id, nullptr, &size);
    if (sizing.code != ANOMALY_STATUS_V1_OK &&
        sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) return {};
    if (size == 0) return {};
    std::string result(size, '\0');
    if (combat->source_name_utf8(
            combat->user, source_id, result.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::string ReadAbilityDisplayName(
    const AnomalyNteSkillsServiceV1* skills,
    AnomalyGenerationHandleV1 ability) {
    if (skills == nullptr || skills->ability_display_name_utf8 == nullptr || ability.id == 0) return {};
    size_t size = 0;
    const auto sizing = skills->ability_display_name_utf8(
        skills->user, ability, nullptr, &size);
    if (sizing.code != ANOMALY_STATUS_V1_OK &&
        sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) return {};
    if (size == 0) return {};
    std::string result(size, '\0');
    if (skills->ability_display_name_utf8(
            skills->user, ability, result.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

// Refresh the candidate skill before the combat stream is consumed. The Host's skill
// snapshot is immutable for its sequence, so the plugin never walks UE objects itself.
void UpdateSkillCandidate(Context& context) {
    if (!SkillsReady(context.skills)) return;

    AnomalyNteSkillFrameV1 frame{};
    frame.size = sizeof(frame);
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        frame.character.id == 0) {
        return;
    }

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> skills{};
    AnomalyNteSkillPageRequestV1 request{};
    request.size = sizeof(request);
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

    // Clear the previous candidate first. A normal attack must never inherit
    // the skill handle from the preceding attack.
    context.captured_skill = {};
    context.captured_ability = {};
    context.captured_input_id = -1;

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
    AnomalyNteSkillFrameV1 frame{};
    frame.size = sizeof(frame);
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> skills{};
    AnomalyNteSkillPageRequestV1 request{};
    request.size = sizeof(request);
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
    context.captured_target = {};
    context.captured_input_id = -1;
    context.captured_damage_sequence = 0;
    context.captured_tick_sequence = 0;
    context.captured_damage_value = 0;
    context.captured_basic_value = 0;
    context.captured_final_value = 0;
    context.captured_damage_type = 0;
    context.captured_reaction_type = 0;
    context.captured_has_skill = false;
    context.captured_ability_path.clear();
    context.captured_target_path.clear();
    context.captured_damage_source_name.clear();
    context.status = context.enabled ? "自动等待玩家下一次攻击" : "自动记录中：重放功能未启用";

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
            !SameHandle(event.world, combatant.world) ||
            !SameHandle(event.source, combatant.character) ||
            event.target.id == 0 || SameHandle(event.target, combatant.character)) {
            continue;
        }

        context.captured = true;
        context.captured_damage_sequence = event.sequence;
        context.captured_tick_sequence = event.tick_sequence;
        context.captured_damage_value = event.value;
        context.captured_basic_value = event.basic_value;
        context.captured_final_value = event.final_value;
        context.captured_damage_type = event.damage_type;
        context.captured_reaction_type = event.reaction_type;
        context.captured_target = event.target;

        // Cross-check the real DamageEvent source against the skill catalog.
        // Prefer an exact reflected ability display-name match over the skill that
        // merely happened to be active immediately before the hit.
        context.captured_damage_source_name =
            ReadDamageSourceName(context.combat, event.name_id);
        if (!context.captured_damage_source_name.empty() &&
            SkillsReady(context.skills)) {
            AnomalyNteSkillFrameV1 frame{};
    frame.size = sizeof(frame);
            std::array<AnomalyNteSkillSnapshotV1,
                       ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> source_skills{};
            AnomalyNteSkillPageRequestV1 request{};
    request.size = sizeof(request);
            AnomalyNteSkillPageResultV1 result{sizeof(result)};
            if (context.skills->frame(context.skills->user, &frame).code ==
                ANOMALY_STATUS_V1_OK) {
                request.generation = frame.generation;
                request.offset = 0;
                request.capacity = static_cast<uint32_t>(source_skills.size());
                if (context.skills->page(
                        context.skills->user, &request,
                        source_skills.data(), &result).code == ANOMALY_STATUS_V1_OK) {
                    for (uint32_t j = 0; j < result.returned; ++j) {
                        const auto& skill = source_skills[j];
                        if (!SameHandle(skill.character, combatant.character)) continue;
                        const std::string display_name =
                            ReadAbilityDisplayName(context.skills, skill.ability_class);
                        if (!display_name.empty() &&
                            display_name == context.captured_damage_source_name) {
                            context.captured_skill = skill.handle;
                            context.captured_ability = skill.ability_class;
                            context.captured_input_id = skill.input_id;
                            break;
                        }
                    }
                }
            }
        }

        context.captured_has_skill = context.captured_skill.id != 0;
        context.captured_ability_path =
            ReadAbilityPath(context.skills, context.captured_ability);
        if (context.combat->participant_path_utf8 != nullptr) {
            size_t size = 0;
            const auto sizing = context.combat->participant_path_utf8(
                context.combat->user, event.target, nullptr, &size);
            if (sizing.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL && size != 0) {
                context.captured_target_path.resize(size);
                if (context.combat->participant_path_utf8(
                        context.combat->user, event.target,
                        context.captured_target_path.data(), &size).code == ANOMALY_STATUS_V1_OK) {
                    if (!context.captured_target_path.empty() &&
                        context.captured_target_path.back() == '\0') {
                        context.captured_target_path.pop_back();
                    }
                } else {
                    context.captured_target_path.clear();
                }
            }
        }
        context.status = context.captured_has_skill
            ? (context.captured_damage_source_name.empty()
                ? "已自动捕获：技能/普通攻击链的第一次伤害"
                : "已自动捕获：已用真实 DamageSource 交叉匹配技能")
            : "已自动捕获：普通攻击链的第一次伤害（未关联到可调用技能）";
        return true;
    }
    return false;
}

enum class ReplayCallResult : uint32_t {
    Success,
    NoSkill,
    InvalidState,
    ServiceError,
    Rejected,
};

ReplayCallResult ReplayOnce(Context& context, uint32_t* status_code, uint32_t* accepted) {
    if (status_code != nullptr) *status_code = ANOMALY_STATUS_V1_OK;
    if (accepted != nullptr) *accepted = 0;
    if (!InvocationReady(context.invocation) || !context.captured) {
        return ReplayCallResult::InvalidState;
    }
    if (!context.captured_has_skill) {
        return ReplayCallResult::NoSkill;
    }

    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    if (context.combat->current_combatant(
            context.combat->user, &combatant).code != ANOMALY_STATUS_V1_OK ||
        combatant.character.id == 0 || combatant.world.id == 0) {
        return ReplayCallResult::InvalidState;
    }

    context.world = combatant.world;
    context.character = combatant.character;

    AnomalyGenerationHandleV1 skill{};
    if (!ResolveReplaySkill(context, &skill)) return ReplayCallResult::NoSkill;

    AnomalyNteSkillInvocationRequestV1 request{sizeof(request)};
    request.world = context.world;
    request.character = context.character;
    request.skill = skill;

    AnomalyNteSkillInvocationResultV1 result{sizeof(result)};
    const auto status = context.invocation->activate(
        context.invocation->user, &request, &result);
    if (status_code != nullptr) *status_code = status.code;
    if (accepted != nullptr) *accepted = result.accepted;
    if (status.code != ANOMALY_STATUS_V1_OK) return ReplayCallResult::ServiceError;
    if (result.accepted == 0) return ReplayCallResult::Rejected;
    return ReplayCallResult::Success;
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
    context->enabled = false;
    context->replay_count = 10;
    context->replay_rate = 2.0;
    context->replay_done = 0;
    context->replaying = false;
    context->replay_requested.store(false, std::memory_order_release);
    context->stop_requested.store(false, std::memory_order_release);
    ArmForNextAttack(*context);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->replaying = false;
    context->replay_requested.store(false, std::memory_order_release);
    context->stop_requested.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;

    // Recording is always active. The enable switch only gates replay actions.
    if (!context->captured && !context->replaying) {
        CaptureNextAttack(*context);
    }

    if (context->stop_requested.exchange(false, std::memory_order_acq_rel)) {
        context->replaying = false;
        context->status = "已停止重放，继续自动记录";
    }
    if (context->replay_requested.exchange(false, std::memory_order_acq_rel)) {
        if (context->enabled && context->captured && !context->replaying) {
            context->replay_done = 0;
            context->replaying = true;
            context->next_replay = std::chrono::steady_clock::now();
            context->status = "已提交重放，等待 Game 域执行";
        } else {
            context->status = context->captured
                ? "无法开始重放：请先启用重放功能"
                : "无法开始重放：当前没有已捕获攻击";
        }
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

    uint32_t replay_status = ANOMALY_STATUS_V1_OK;
    uint32_t accepted = 0;
    const ReplayCallResult replay_result =
        ReplayOnce(*context, &replay_status, &accepted);
    if (replay_result != ReplayCallResult::Success) {
        context->replaying = false;
        if (replay_result == ReplayCallResult::NoSkill) {
            context->status = context->captured_has_skill
                ? "重放失败：当前捕获技能句柄已失效或无法重新解析"
                : "已记录普通攻击，但当前 ABI 没有直接注入普通攻击事件的接口";
        } else if (replay_result == ReplayCallResult::Rejected) {
            context->status = "重放被游戏拒绝：skill activate accepted=0";
        } else {
            context->status = "重放调用失败：ABI status=" + std::to_string(replay_status);
        }
        ArmForNextAttack(*context);
        return;
    }
    context->status = "游戏已接受技能激活请求（accepted=1）";

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
        int enabled = context->enabled ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView("[启用]"), &enabled) != 0) {
            const bool next_enabled = enabled != 0;
            if (next_enabled != context->enabled) {
                context->enabled = next_enabled;
                if (context->enabled) {
                    // Enabling replay must not reset or interrupt the always-on recorder.
                    context->status = context->captured
                        ? "已启用重放：已保留当前自动捕获"
                        : "已启用重放：自动等待下一次攻击";
                } else {
                    // Disabling replay must not stop recording or discard a captured attack.
                    // Draw is Render-domain code; disable replay through the Game-domain request.
                    context->stop_requested.store(true, std::memory_order_release);
                    context->status = context->captured
                        ? "自动记录中：重放功能未启用（已保留当前捕获）"
                        : "自动记录中：重放功能未启用";
                }
            }
        }

        ui->text(ui->user, anomaly::sdk::StringView(
            context->enabled ? context->status : "自动记录中：重放功能未启用"));

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
                ? "已捕获玩家→目标的攻击/伤害事件"
                : context->captured_ability_path));
        if (!context->captured_damage_source_name.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "DamageSource：" + context->captured_damage_source_name));
        }
        if (!context->captured_target_path.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "目标：" + context->captured_target_path));
        }
        const std::string progress =
            "进度：" + std::to_string(context->replay_done) + "/" +
            std::to_string(context->replay_count);
        ui->text(ui->user, anomaly::sdk::StringView(progress));
    } else {
        ui->text(ui->user, anomaly::sdk::StringView(
            "无需手动录制：插件自动等待下一次玩家攻击"));
    }

    if (context->enabled && !context->replaying && context->captured) {
        if (ui->button(
                ui->user, anomaly::sdk::StringView("开始重放"), 0.0F, 0.0F) != 0) {
            // Draw only posts the request; Update() executes the skill activation in Game domain.
            context->replay_requested.store(true, std::memory_order_release);
            context->status = "已提交重放请求";
        }
    } else if (context->replaying) {
        if (ui->button(
                ui->user, anomaly::sdk::StringView("停止重放"), 0.0F, 0.0F) != 0) {
            // Draw only posts the stop request; Update() applies it in Game domain.
            context->stop_requested.store(true, std::memory_order_release);
        }
    }

        ui->end_window(ui->user);
    } else {
        // end_window must also be called when begin_window returned false.
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
        anomaly::sdk::StringView("1.1.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw,
    };
    return anomaly::sdk::Ok();
}
