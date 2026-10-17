/*
 * 中文维护说明：
 * 1. 本插件记录真实的 CharacterOnDamaged 事件，并把原始 DamageEvent 序列交给 Core 的 damage-replay 服务。
 * 2. 当前 ReplayOnce 的实际重放路径是 Core 重新调用捕获并验证过的原生 GameplayEffect 应用，不重放按键/动画，也不直接写目标 HP。
 * 3. 技能目录仅用于 DamageSource 与技能显示名称的只读交叉核对。
 * 4. Draw 只修改 UI 状态/提交请求；真正的 Core 重放请求由 Update 在 Game 域执行。
 * 5. 重放成功的判据是之后观察到同一攻击者、目标和伤害来源的新 CharacterOnDamaged 事件，调用返回成功不等于实际命中。
 * 6. Dump 的函数签名与布局只能证明 ABI 形状；覆盖率、因果匹配和重复应用语义仍须由运行时诊断与受控实验验证。
 */
// Record real player damage; replay is handled by Core's native GameplayEffect replay service.
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/core.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <new>
#include <mutex>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Context final {
    // Draw belongs to Render while Update belongs to Game; serialize their shared state.
    std::mutex state_mutex;
    const AnomalyCoreServiceV1* core{};
    std::wstring replay_log_path;
    bool replay_log_ready{};
    std::string replay_log_error;
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteDamageReplayServiceV1* damage_replay{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyUe5FrameworkServiceV1* framework{};

    // The plugin is always armed: this cursor is advanced from the last observed
    // combat event, so only a damage event that occurs after startup can be captured.
    uint64_t combat_cursor{};
    // Game-thread snapshot shown in the UI to distinguish "no Core events" from
    // events received by Core but filtered as non-player/non-target damage.
    uint64_t latest_damage_sequence_snapshot{};

    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 character{};

    // Optional skill metadata is retained only for source-name/UI cross-checks.
    AnomalyGenerationHandleV1 captured_ability{};
    AnomalyGenerationHandleV1 captured_target{};
    uint64_t captured_damage_sequence{};
    uint64_t captured_damage_source_id{};
    uint64_t captured_tick_sequence{};
    int64_t captured_damage_value{};
    int64_t captured_basic_value{};
    int64_t captured_final_value{};
    uint32_t captured_damage_type{};
    uint32_t captured_reaction_type{};
    std::string captured_ability_path;
    std::string captured_target_path;
    // Keep the Host-resolved DamageSource for cross-checking against the reflected skill.
    std::string captured_damage_source_name;

    bool enabled{};
    bool captured{};
    bool replaying{};
    // 可选的受控实验模式：一次真实新伤害事件确认后自动停用连锁。
    bool proof_single_replay{};
    bool proof_single_replay_completed{};
    // The current matcher only sees its four validated capture families. This is a
    // diagnostic guard, not proof that no unobserved native route exists.
    bool proof_single_replay_candidate_count_available{};
    bool proof_single_replay_single_candidate{};
    uint32_t proof_single_replay_candidate_count{};
    bool proof_single_replay_hp_result_available{};
    bool proof_single_replay_hp_reduced{};
    bool proof_single_replay_native_exec_result_available{};
    bool proof_single_replay_native_exec_seen{};
    // Draw runs in the Render domain. Replay/stop requests are posted atomically
    // and executed by Update() in the Game callback domain required by the ABI.
    std::atomic_bool replay_requested{false};
    std::atomic_bool stop_requested{false};
    // UI value is a repeat rate (times per second), not a total replay limit.
    uint32_t replay_rate_per_second{1}; // Internal cadence; not exposed in the final compact UI.
    uint32_t repeat_count{1};
    uint64_t damage_event_count{};
    uint64_t replay_failure_count{};
    uint64_t replay_attempt_count{};
    uint32_t replay_done{};
    std::chrono::steady_clock::time_point next_replay_at{};
    std::string status{"自动等待玩家下一次攻击"};


    // A replay is not successful when the bridge merely returns accepted=1.
    // Wait for a newer player->target DamageEvent before counting the replay.
    uint64_t replay_damage_cursor{};
    std::chrono::steady_clock::time_point replay_damage_deadline{};
    bool waiting_for_damage{};
};

// Resolve the package folder via Anomaly's Core service. The DLL can be staged in
// a shadow/cache folder, so its module path is not the user's actual plugin directory.
void ReportReplayLogError(Context& context, std::string_view message) noexcept {
    const auto* core = context.core;
    constexpr std::size_t log_field_end =
        offsetof(AnomalyCoreServiceV1, log) + sizeof(AnomalyCoreServiceV1::log);
    if (core == nullptr || core->struct_size < log_field_end || core->log == nullptr) return;
    try {
        core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            anomaly::sdk::StringView(message));
    } catch (...) {
    }
}

bool WriteReplayLogBytes(Context& context, std::string_view line) noexcept {
    if (context.replay_log_path.empty()) {
        ReportReplayLogError(context, context.replay_log_error.empty()
            ? "NTE Damage Chain Reaction: plugin log path is not initialized"
            : context.replay_log_error);
        return false;
    }
    HANDLE file = CreateFileW(
        context.replay_log_path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        context.replay_log_ready = false;
        context.replay_log_error = "NTE Damage Chain Reaction: cannot open attack-replay.log; Win32 error=" +
            std::to_string(error);
        ReportReplayLogError(context, context.replay_log_error);
        return false;
    }

    DWORD written{};
    const auto bytes_to_write = static_cast<DWORD>(
        (std::min)(line.size(), static_cast<std::size_t>(0xFFFFFFFFU)));
    const BOOL okay = bytes_to_write == 0 ||
        (WriteFile(file, line.data(), bytes_to_write, &written, nullptr) != 0 &&
            written == bytes_to_write);
    const DWORD error = okay ? ERROR_SUCCESS : GetLastError();
    if (okay) {
        FlushFileBuffers(file);
    }
    CloseHandle(file);
    if (!okay) {
        context.replay_log_ready = false;
        context.replay_log_error = "NTE Damage Chain Reaction: cannot write attack-replay.log; Win32 error=" +
            std::to_string(error);
        ReportReplayLogError(context, context.replay_log_error);
        return false;
    }
    context.replay_log_ready = true;
    return true;
}

std::string ReplayLogTimestampUtc() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char timestamp[40]{};
    std::snprintf(
        timestamp, sizeof(timestamp),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds));
    return timestamp;
}

// Create the log during plugin load, so the file exists even before the first error.
// The directory comes from the Core API and refers to the real plugin package folder.
bool InitializeReplayDiagnosticLog(Context& context) noexcept {
    try {
        constexpr std::size_t directory_field_end =
            offsetof(AnomalyCoreServiceV1, plugin_directory) +
            sizeof(AnomalyCoreServiceV1::plugin_directory);
        if (context.core == nullptr ||
            context.core->struct_size < directory_field_end ||
            context.core->plugin_directory == nullptr) {
            context.replay_log_error =
                "NTE Damage Chain Reaction: Core plugin_directory API is unavailable";
            ReportReplayLogError(context, context.replay_log_error);
            return false;
        }

        std::size_t required{};
        const auto size_status = context.core->plugin_directory(
            context.core->user, nullptr, &required);
        if (size_status.code != ANOMALY_STATUS_V1_OK || required < 2 || required > 32768) {
            context.replay_log_error =
                "NTE Damage Chain Reaction: cannot query package directory; Core status=" +
                std::to_string(size_status.code);
            ReportReplayLogError(context, context.replay_log_error);
            return false;
        }
        std::vector<char> utf8_path(required, '\0');
        std::size_t capacity = utf8_path.size();
        const auto path_status = context.core->plugin_directory(
            context.core->user, utf8_path.data(), &capacity);
        if (path_status.code != ANOMALY_STATUS_V1_OK || capacity < 2 ||
            capacity > utf8_path.size() || utf8_path[capacity - 1] != '\0') {
            context.replay_log_error =
                "NTE Damage Chain Reaction: Core package directory read failed; status=" +
                std::to_string(path_status.code);
            ReportReplayLogError(context, context.replay_log_error);
            return false;
        }

        const int wide_required = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(), -1, nullptr, 0);
        if (wide_required < 2 || wide_required > 32768) {
            context.replay_log_error =
                "NTE Damage Chain Reaction: Core package directory is not valid UTF-8";
            ReportReplayLogError(context, context.replay_log_error);
            return false;
        }
        std::wstring directory(static_cast<std::size_t>(wide_required), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(), -1,
                directory.data(), wide_required) != wide_required) {
            context.replay_log_error =
                "NTE Damage Chain Reaction: failed to convert Core package directory to UTF-16";
            ReportReplayLogError(context, context.replay_log_error);
            return false;
        }
        directory.resize(static_cast<std::size_t>(wide_required - 1));
        while (!directory.empty() &&
            (directory.back() == L'\\' || directory.back() == L'/')) {
            directory.pop_back();
        }
        if (directory.empty()) {
            context.replay_log_error = "NTE Damage Chain Reaction: Core returned an empty package directory";
            ReportReplayLogError(context, context.replay_log_error);
            return false;
        }

        context.replay_log_path = directory + L"\\attack-replay.log";
        const std::string startup_line =
            "[" + ReplayLogTimestampUtc() + "] plugin=anomaly.builtin.nte-attack-replay "
            "version=1.9.9 log=initialized\r\n";
        if (!WriteReplayLogBytes(context, startup_line)) return false;
        return true;
    } catch (...) {
        context.replay_log_error =
            "NTE Damage Chain Reaction: unexpected failure while initializing attack-replay.log";
        ReportReplayLogError(context, context.replay_log_error);
        return false;
    }
}

// Append every failed request's complete Core reason in the actual package folder;
// the in-game UI only displays a short status.
void AppendReplayDiagnosticLog(
    Context& context, const uint32_t status_code,
    const std::string_view detail) noexcept {
    try {
        std::string line;
        line.reserve(detail.size() + context.captured_target_path.size() +
            context.captured_damage_source_name.size() + 256U);
        line += "[";
        line += ReplayLogTimestampUtc();
        line += "] replay=failed status_code=";
        line += std::to_string(status_code);
        line += " damage_sequence=";
        line += std::to_string(context.captured_damage_sequence);
        line += " rate_per_second=";
        line += std::to_string(context.replay_rate_per_second);
        line += " target=";
        line += context.captured_target_path.empty()
            ? "(unknown)" : context.captured_target_path;
        line += " damage_source=";
        line += context.captured_damage_source_name.empty()
            ? "(unknown)" : context.captured_damage_source_name;
        line += "\r\nreason=";
        line.append(detail.data(), detail.size());
        line += "\r\n\r\n";
        WriteReplayLogBytes(context, line);
    } catch (...) {
        ReportReplayLogError(context,
            "NTE Damage Chain Reaction: exception while writing replay failure diagnostic");
    }
}

// 单次路线验证的正向证据：记录实际调用的 Core 路由，以及调用之后出现的新原生伤害事件。
void AppendReplayInvocationEvidenceLog(
    Context& context, const AnomalyStatusV1& status) noexcept {
    try {
        std::string line;
        line.reserve(512U + context.captured_target_path.size() +
            context.captured_damage_source_name.size() + status.message.size);
        line += "[";
        line += ReplayLogTimestampUtc();
        line += "] replay=invocation_returned status_code=";
        line += std::to_string(status.code);
        line += " damage_sequence=";
        line += std::to_string(context.captured_damage_sequence);
        line += " source_id=";
        line += std::to_string(context.captured_damage_source_id);
        line += " target=";
        line += context.captured_target_path.empty()
            ? "(unknown)" : context.captured_target_path;
        line += " damage_source=";
        line += context.captured_damage_source_name.empty()
            ? "(unknown)" : context.captured_damage_source_name;
        line += "\r\nroute=";
        if (status.message.data != nullptr && status.message.size != 0) {
            line.append(status.message.data, status.message.size);
        } else {
            line += "(Core did not report the selected route)";
        }
        line += "\r\nnote=ProcessEvent dispatch completion is not proof of applied damage\r\n\r\n";
        WriteReplayLogBytes(context, line);
    } catch (...) {
        ReportReplayLogError(context,
            "NTE Damage Chain Reaction: exception while writing invocation evidence");
    }
}

void AppendReplayObservedDamageEvidenceLog(
    Context& context, const AnomalyNteDamageEventV1& event) noexcept {
    try {
        std::string line;
        line.reserve(512U + context.captured_target_path.size() +
            context.captured_damage_source_name.size());
        line += "[";
        line += ReplayLogTimestampUtc();
        line += "] verification=observed_new_character_damage_event";
        line += " original_damage_sequence=";
        line += std::to_string(context.captured_damage_sequence);
        line += " observed_damage_sequence=";
        line += std::to_string(event.sequence);
        line += " captured_source_id=";
        line += std::to_string(context.captured_damage_source_id);
        line += " observed_source_id=";
        line += std::to_string(event.source_id);
        line += " tick_sequence=";
        line += std::to_string(event.tick_sequence);
        line += " final_damage=";
        line += std::to_string(event.final_damage);
        line += " display_damage=";
        line += std::to_string(event.display_damage);
        line += " target=";
        line += context.captured_target_path.empty()
            ? "(unknown)" : context.captured_target_path;
        line += " damage_source=";
        line += context.captured_damage_source_name.empty()
            ? "(unknown)" : context.captured_damage_source_name;
        line += "\r\nproof=resolved exact source_id, same world, same attacker, same victim, newer sequence, positive final_damage\r\n\r\n";
        WriteReplayLogBytes(context, line);
    } catch (...) {
        ReportReplayLogError(context,
            "NTE Damage Chain Reaction: exception while writing observed-damage evidence");
    }
}

template <typename Struct, typename Field>
// HasField 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

// 只消费 Host 捕获的实际 CharacterOnDamaged 伤害流，不用合并后的 Unified CombatEvent 序列代替伤害序列。
bool CombatReady(const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
                    decltype(AnomalyNteCombatServiceV1::next_damage_event)>(
               service, offsetof(AnomalyNteCombatServiceV1, next_damage_event)) &&
           service->latest_damage_sequence != nullptr &&
           service->next_damage_event != nullptr &&
           service->current_combatant != nullptr &&
           service->source_name_utf8 != nullptr;
}

// SkillsReady 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool SkillsReady(const AnomalyNteSkillsServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillsServiceV1,
                    decltype(AnomalyNteSkillsServiceV1::page)>(
               service, offsetof(AnomalyNteSkillsServiceV1, page)) &&
           service->frame != nullptr && service->page != nullptr;
}

// UiReady 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
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

// SameHandle 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
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
    const AnomalyNteCombatServiceV1* combat, const uint64_t source_id) {
    if (combat == nullptr || combat->source_name_utf8 == nullptr || source_id == 0) {
        return {};
    }
    size_t size = 0;
    const auto sizing = combat->source_name_utf8(
        combat->user, source_id, nullptr, &size);
    if (sizing.code != ANOMALY_STATUS_V1_OK &&
        sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) {
        return {};
    }
    if (size == 0) return {};
    std::string result(size, '\\0');
    if (combat->source_name_utf8(
            combat->user, source_id, result.data(), &size).code != ANOMALY_STATUS_V1_OK) {
        return {};
    }
    if (!result.empty() && result.back() == '\\0') result.pop_back();
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

// Refresh optional active-skill metadata for display/source-name cross-checking only.
// The damage chain executes a captured GameplayEffect call, never an input or skill request.
void UpdateSkillCandidate(Context& context) {
    if (!SkillsReady(context.skills)) return;

    AnomalyNteSkillFrameV1 frame{};
    frame.struct_size = sizeof(frame);
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        frame.character.id == 0) return;

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> skills{};
    AnomalyNteSkillPageRequestV1 request{};
    request.struct_size = sizeof(request);
    request.generation = frame.generation;
    request.offset = 0;
    request.capacity = static_cast<uint32_t>(skills.size());
    AnomalyNteSkillPageResultV1 result{};
    result.struct_size = sizeof(result);
    if (context.skills->page(context.skills->user, &request, skills.data(), &result).code !=
        ANOMALY_STATUS_V1_OK) return;

    context.captured_ability = {};
    for (uint32_t i = 0; i < result.returned; ++i) {
        if ((skills[i].flags & ANOMALY_NTE_SKILL_V1_ACTIVE) != 0 &&
            SameHandle(skills[i].character, frame.character)) {
            context.captured_ability = skills[i].ability_class;
            break;
        }
    }
}

// ArmForNextAttack 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ArmForNextAttack(Context& context) {
    context.proof_single_replay_completed = false;
    context.proof_single_replay_candidate_count_available = false;
    context.proof_single_replay_single_candidate = false;
    context.proof_single_replay_candidate_count = 0;
    context.proof_single_replay_hp_result_available = false;
    context.proof_single_replay_hp_reduced = false;
    context.proof_single_replay_native_exec_result_available = false;
    context.proof_single_replay_native_exec_seen = false;
    context.captured = false;
    context.replaying = false;
    context.replay_done = 0;
    context.captured_ability = {};
    context.captured_target = {};
    context.captured_damage_sequence = 0;
    context.captured_damage_source_id = 0;
    context.captured_tick_sequence = 0;
    context.captured_damage_value = 0;
    context.captured_basic_value = 0;
    context.captured_final_value = 0;
    context.captured_damage_type = 0;
    context.captured_reaction_type = 0;
    context.captured_ability_path.clear();
    context.captured_target_path.clear();
    context.captured_damage_source_name.clear();
    context.replay_damage_cursor = 0;
    context.replay_damage_deadline = {};
    context.waiting_for_damage = false;
    context.status = context.enabled
        ? "自动等待玩家下一次攻击"
        : "已停用：勾选启用后自动连锁";

    // Starting at the current tail prevents an old combat event from being mistaken
    // for the next attack after a world change or plugin restart.
    if (context.combat != nullptr) {
        context.combat_cursor = context.combat->latest_damage_sequence(
            context.combat->user);
        context.latest_damage_sequence_snapshot = context.combat_cursor;
    }
}

// 记录当前 DamageEvent 序列号和玩家战斗句柄，然后等待下一条玩家对非玩家目标的新增伤害事件；只有实际观察到事件才把一次攻击认定为成功。
bool CaptureNextAttack(Context& context) {
    if (!CombatReady(context.combat)) return false;

    AnomalyNteCombatantSnapshotV1 combatant{};
    combatant.struct_size = sizeof(combatant);
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
        AnomalyNteDamageEventV1 event{};
        event.struct_size = sizeof(event);
        const auto status = context.combat->next_damage_event(
            context.combat->user, context.combat_cursor, &event);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            // A stale cursor can happen when the Host replaces the combat ring or world.
            // Rebase to the current tail; old events must never be replayed as a new attack.
            context.combat_cursor = context.combat->latest_damage_sequence(
                context.combat->user);
            return false;
        }
        if (status.code != ANOMALY_STATUS_V1_OK) {
            return false;
        }
        context.combat_cursor = event.sequence;

        if ((event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) == 0 ||
            !SameHandle(event.world, combatant.world) ||
            !SameHandle(event.attacker, combatant.character) ||
            event.victim.id == 0 || SameHandle(event.victim, combatant.character)) {
            continue;
        }

        context.captured = true;
        ++context.damage_event_count;
        context.replay_done = 0;
        context.proof_single_replay_completed = false;
        context.proof_single_replay_candidate_count_available = false;
        context.proof_single_replay_single_candidate = false;
        context.proof_single_replay_candidate_count = 0;
        context.proof_single_replay_hp_result_available = false;
        context.proof_single_replay_hp_reduced = false;
        context.proof_single_replay_native_exec_result_available = false;
        context.proof_single_replay_native_exec_seen = false;
        context.captured_damage_sequence = event.sequence;
        context.captured_tick_sequence = event.tick_sequence;
        context.captured_damage_value = event.display_damage;
        context.captured_basic_value = event.basic_damage;
        context.captured_final_value = event.final_damage;
        context.captured_damage_source_id = event.source_id;
        context.captured_damage_type = event.damage_type;
        context.captured_reaction_type = event.reaction_type;
        context.captured_target = event.victim;
        context.captured_target_path.clear();

        // Cross-check the real DamageEvent source against the skill catalog.
        // Prefer an exact reflected ability display-name match over the skill that
        // merely happened to be active immediately before the hit.
        context.captured_damage_source_name =
            ReadDamageSourceName(context.combat, event.source_id);
        // A generic active-skill candidate is not proof that it caused this hit.
        // When the real DamageSource is available, keep only an exact name match.
        if (!context.captured_damage_source_name.empty()) {
                    context.captured_ability = {};
            }
        if (!context.captured_damage_source_name.empty() &&
            SkillsReady(context.skills)) {
            AnomalyNteSkillFrameV1 frame{};
    frame.struct_size = sizeof(frame);
            std::array<AnomalyNteSkillSnapshotV1,
                       ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> source_skills{};
            AnomalyNteSkillPageRequestV1 request{};
    request.struct_size = sizeof(request);
            AnomalyNteSkillPageResultV1 result{};
    result.struct_size = sizeof(result);
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
                            context.captured_ability = skill.ability_class;
                            break;
                        }
                    }
                }
            }
        }

        context.captured_ability_path =
            ReadAbilityPath(context.skills, context.captured_ability);
        if (context.combat->participant_path_utf8 != nullptr) {
            size_t size = 0;
            const auto sizing = context.combat->participant_path_utf8(
                context.combat->user, event.victim, nullptr, &size);
            if (sizing.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL && size != 0) {
                context.captured_target_path.resize(size);
                if (context.combat->participant_path_utf8(
                        context.combat->user, event.victim,
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
        context.status = context.captured_damage_source_name.empty()
            ? "已自动捕获实际玩家伤害事件；重放将使用 Host 捕获的原始 GameplayEffect 调用"
            : "已自动捕获实际玩家伤害事件；DamageSource 已记录，等待重放";
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

// Read the native-exec ingress observation reported by Core for the selected invocation.
bool ReadNativeExecSeen(const AnomalyStatusV1& status, bool& seen) noexcept {
    seen = false;
    if (status.message.data == nullptr || status.message.size == 0) return false;
    const std::string_view message(status.message.data, status.message.size);
    constexpr std::string_view key = "native_exec_seen=";
    const std::size_t key_position = message.find(key);
    if (key_position == std::string_view::npos) return false;
    const std::size_t position = key_position + key.size();
    if (position >= message.size()) return false;
    if (message[position] == '1') { seen = true; return true; }
    if (message[position] == '0') { seen = false; return true; }
    return false;
}

// Read a diagnostic-only HP probe emitted around the native dispatch by Core.
bool ReadTargetHpReduction(const AnomalyStatusV1& status, bool& reduced) noexcept {
    reduced = false;
    if (status.message.data == nullptr || status.message.size == 0) return false;
    const std::string_view message(status.message.data, status.message.size);
    constexpr std::string_view key = "target_hp_reduced=";
    const std::size_t key_position = message.find(key);
    if (key_position == std::string_view::npos) return false;
    const std::size_t position = key_position + key.size();
    if (position >= message.size()) return false;
    if (message[position] == '1') { reduced = true; return true; }
    if (message[position] == '0') { reduced = false; return true; }
    return false;
}

// Parse Core's diagnostic-only matching-candidate field without changing the host ABI.
bool ReadMatchingCandidateCount(
    const AnomalyStatusV1& status, uint32_t& count) noexcept {
    count = 0;
    if (status.message.data == nullptr || status.message.size == 0) return false;
    const std::string_view message(status.message.data, status.message.size);
    constexpr std::string_view key = "matching_candidates=";
    const std::size_t key_position = message.find(key);
    if (key_position == std::string_view::npos) return false;
    std::size_t position = key_position + key.size();
    if (position >= message.size() || message[position] < '0' || message[position] > '9') {
        return false;
    }
    uint32_t parsed = 0;
    while (position < message.size() &&
           message[position] >= '0' && message[position] <= '9') {
        const uint32_t digit = static_cast<uint32_t>(message[position] - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
        ++position;
    }
    count = parsed;
    return true;
}

// 通过 Core damage-replay 服务重用捕获的 GameplayEffect 应用调用；只有观察到新的真实 DamageEvent 才计数。
ReplayCallResult ReplayOnce(Context& context, AnomalyStatusV1* replay_status) {
    if (replay_status != nullptr) {
        *replay_status = Status(ANOMALY_STATUS_V1_OK);
    }
    if (!context.captured || context.captured_damage_sequence == 0 ||
        context.damage_replay == nullptr ||
        context.damage_replay->replay_damage == nullptr) {
        if (replay_status != nullptr) {
            *replay_status = Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                "captured damage/replay service is unavailable");
        }
        return ReplayCallResult::InvalidState;
    }

    // Reapply only the exact GameplayEffect context associated with the recorded DamageEvent.
    // No player input, attack animation, or skill activation request is issued here.
    AnomalyNteDamageReplayRequestV1 request{};
    request.struct_size = sizeof(request);
    request.damage_sequence = context.captured_damage_sequence;
    const auto status = context.damage_replay->replay_damage(
        context.damage_replay->user, &request);
    if (replay_status != nullptr) *replay_status = status;
    return status.code == ANOMALY_STATUS_V1_OK
        ? ReplayCallResult::Success : ReplayCallResult::ServiceError;
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    auto* context = new (std::nothrow) Context{};
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);

    const anomaly::sdk::Host sdk_host(host);
    context->core = sdk_host.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
    InitializeReplayDiagnosticLog(*context);
    context->combat = sdk_host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
        ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    context->damage_replay = sdk_host.Query<AnomalyNteDamageReplayServiceV1>(
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_ID,
        ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_VERSION).get();
    context->skills = sdk_host.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID,
        ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).get();
    context->framework = sdk_host.Query<AnomalyUe5FrameworkServiceV1>(
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION).get();
    context->ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();

    // Damage replay uses the Host's validated captured GameplayEffect call. Skill,
    // invocation, names and signature services are optional enrichers; requiring them
    // prevented replay from loading when those unrelated services were unavailable.
    if (!CombatReady(context->combat) || context->damage_replay == nullptr ||
        context->damage_replay->replay_damage == nullptr || !UiReady(context->ui) ||
        context->framework == nullptr || context->framework->tick_sequence == nullptr) {
        delete context;
        return Status(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "Attack Replay requires raw NTE damage events, the GameplayEffect replay service, framework ticks, and UI");
    }

    *plugin_context = context;
    return anomaly::sdk::Ok();
}

// 建立 AttackReplay 的运行状态并启动真实伤害事件轮询，使后续 Update 可以捕获并重放攻击；重复启动不会重新创建已经存在的状态。
AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock state_lock(context->state_mutex);
    context->enabled = false;
    context->replay_rate_per_second = 1;
    context->repeat_count = 1;
    context->damage_event_count = 0;
    context->replay_failure_count = 0;
    context->replay_attempt_count = 0;
    context->replay_done = 0;
    context->replaying = false;
    context->replay_requested.store(false, std::memory_order_release);
    context->stop_requested.store(false, std::memory_order_release);
    context->proof_single_replay = false;
    context->proof_single_replay_completed = false;
    ArmForNextAttack(*context);
    return anomaly::sdk::Ok();
}

// 停止 AttackReplay 的事件捕获和重放状态，并清除待验证的攻击上下文，防止插件停止后继续消费战斗事件。
AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock state_lock(context->state_mutex);
    context->replaying = false;
    context->replay_requested.store(false, std::memory_order_release);
    context->stop_requested.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

// Unload 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

// Update 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    std::scoped_lock state_lock(context->state_mutex);
    // Sample the host sequence on the Game callback, never from Draw's Render domain.
    if (context->combat != nullptr && context->combat->latest_damage_sequence != nullptr) {
        context->latest_damage_sequence_snapshot =
            context->combat->latest_damage_sequence(context->combat->user);
    }

    // 启用开关只控制重放。停止连锁时保留最新捕获，录制流继续消费后续真实伤害。
    if (context->stop_requested.exchange(false, std::memory_order_acq_rel)) {
        // Disable stops the chain but intentionally preserves the current capture.
        // The recorder remains armed at all times, even while replay is disabled.
        context->replaying = false;
        context->waiting_for_damage = false;
        context->replay_requested.store(false, std::memory_order_release);
        context->status = context->enabled
            ? "已启用：等待下一次真实伤害事件"
            : (context->captured
                ? "伤害链式反应已停用（保留当前捕获，继续监测真实伤害）"
                : "持续监测真实伤害；伤害链式反应未启用");
    }
    // Continuously record the newest real player hit whenever no replay chain is active.
    // Freeze the selected hit during replay so the chain cannot replace its own source context.
    if (!context->replaying && CaptureNextAttack(*context)) {
        if (context->enabled) {
            context->replay_requested.store(true, std::memory_order_release);
        }
    }
    if (!context->enabled) return;
    if (context->replay_requested.exchange(false, std::memory_order_acq_rel)) {
        if (context->enabled && context->captured && !context->replaying) {
            context->replay_done = 0;
            context->replaying = true;
            context->waiting_for_damage = false;
            context->next_replay_at = std::chrono::steady_clock::now();
            context->status = "已自动启动伤害链式反应";
        } else {
            context->status = "自动重放未启动：功能已停用或尚无有效攻击";
        }
    }

    if (!context->replaying) return;

    const auto now = std::chrono::steady_clock::now();

    // accepted=1 only means the call reached the game. A replay is counted
    // only after a fresh player->target DamageEvent is observed.
    if (context->waiting_for_damage) {
        AnomalyNteDamageEventV1 event{};
        event.struct_size = sizeof(event);
        const auto damage_status = context->combat->next_damage_event(
            context->combat->user, context->replay_damage_cursor, &event);
        if (damage_status.code == ANOMALY_STATUS_V1_OK) {
            context->replay_damage_cursor = event.sequence;
            const bool exact_source_identity =
                context->captured_damage_source_id != 0 &&
                event.source_id != 0 &&
                event.source_id == context->captured_damage_source_id;
            bool source_name_fallback_matches = false;
            // 单次路线证明不接受仅显示名称相同的弱匹配；一般连锁监控仍保留历史兼容回退。
            if (!exact_source_identity && !context->proof_single_replay &&
                !context->captured_damage_source_name.empty()) {
                const std::string observed_source_name =
                    ReadDamageSourceName(context->combat, event.source_id);
                source_name_fallback_matches = !observed_source_name.empty() &&
                    observed_source_name == context->captured_damage_source_name;
            }
            if ((event.flags & ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT) != 0 &&
                SameHandle(event.world, context->world) &&
                SameHandle(event.attacker, context->character) &&
                SameHandle(event.victim, context->captured_target) &&
                event.sequence > context->captured_damage_sequence &&
                (exact_source_identity || source_name_fallback_matches) &&
                (!context->proof_single_replay || event.final_damage > 0)) {
                if (context->proof_single_replay) {
                    AppendReplayObservedDamageEvidenceLog(*context, event);
                }
                context->waiting_for_damage = false;
                if (context->damage_event_count != (std::numeric_limits<uint64_t>::max)()) {
                    ++context->damage_event_count;
                }
                ++context->replay_done;
                // Keep the native replay cadence internal; the finalized compact UI
                // exposes a total repeat count rather than a rate control.
                context->next_replay_at = now + std::chrono::milliseconds(
                    (1000U + context->replay_rate_per_second - 1U) /
                    std::clamp(context->replay_rate_per_second, 1U, 100U));
                context->status = "已确认新的伤害事件";
                if (context->proof_single_replay) {
                    // 一次已确认的原生重放结束实验，防止测试过程中连续扣除目标生命。
                    context->enabled = false;
                    context->replaying = false;
                    context->waiting_for_damage = false;
                    context->replay_requested.store(false, std::memory_order_release);
                    context->proof_single_replay_completed = true;
                    if (!context->proof_single_replay_candidate_count_available) {
                        context->status =
                            "单次观察结束：已见新伤害，但 Core 未提供匹配候选数；路线未证实";
                    } else if (!context->proof_single_replay_single_candidate) {
                        context->status =
                            "单次观察结束：已见新伤害，但匹配候选数=" +
                            std::to_string(context->proof_single_replay_candidate_count) +
                            "；存在歧义，不能认定路线正确";
                    } else if (!context->proof_single_replay_hp_result_available) {
                        context->status =
                            "单次观察结束：候选数=1 且观察到新伤害，但目标 HP 探针不可用；路线未证实";
                    } else if (!context->proof_single_replay_hp_reduced) {
                        context->status =
                            "单次观察结束：候选数=1 且观察到新伤害，但同步 GetHP 未下降；需排除护盾或异步结算";
                    } else if (!context->proof_single_replay_native_exec_result_available) {
                        context->status =
                            "单次观测结束：单候选、同来源新伤害且 HP 下降，但缺少 Native Exec 命中证据";
                    } else if (!context->proof_single_replay_native_exec_seen) {
                        context->status =
                            "单次观测结束：伤害已生效，但只观测到 ProcessEvent 入口；原生 Exec 路径尚未确认";
                    } else {
                        context->status =
                            "移植路径得到支持：单候选、Native Exec 命中、同来源新伤害且同步 GetHP 下降";
                    }
                    return;
                }
                if (context->replay_done >= std::clamp(context->repeat_count, 1U, 10U)) {
                    context->replaying = false;
                    context->waiting_for_damage = false;
                    context->replay_requested.store(false, std::memory_order_release);
                    ArmForNextAttack(*context);
                    context->status = "已完成设定重复次数，等待下一次真实伤害";
                    return;
                }
            }
        } else if (damage_status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
            ++context->replay_failure_count;
            context->replaying = false;
            context->waiting_for_damage = false;
            ArmForNextAttack(*context);
            context->status = "伤害链验证失败：读取新的 DamageEvent 时发生错误；已恢复监测，等待下一次真实伤害事件";
            return;
        }

        if (context->waiting_for_damage && now >= context->replay_damage_deadline) {
            ++context->replay_failure_count;
            const bool single_shot = context->proof_single_replay;
            context->replaying = false;
            context->waiting_for_damage = false;
            if (single_shot) {
                context->enabled = false;
                context->replay_requested.store(false, std::memory_order_release);
            }
            ArmForNextAttack(*context);
            context->status = single_shot
                ? "单次验证未通过：2 秒内未观察到同一目标的新 DamageEvent，已自动停用；检查 attack-replay.log"
                : "伤害链停止：2 秒内未确认同一目标的新伤害（目标可能已死亡或追加未生效）；自动等待下一次真实伤害事件";
            return;
        }
        if (context->waiting_for_damage) return;
    }

    // Rate controls cadence only; it never caps the total number of chained hits.
    if (context->next_replay_at.time_since_epoch().count() != 0 &&
        now < context->next_replay_at) return;
    // Snapshot the raw DamageEvent tail immediately before invoking the replay.
    context->replay_damage_cursor = context->combat->latest_damage_sequence(
        context->combat->user);
    context->replay_damage_deadline = now + std::chrono::milliseconds(2000);

    AnomalyStatusV1 replay_status{};
    ++context->replay_attempt_count;
    const ReplayCallResult replay_result = ReplayOnce(*context, &replay_status);
    if (replay_result != ReplayCallResult::Success) {
        std::string failure;
        if (replay_status.message.data != nullptr && replay_status.message.size != 0) {
            failure = "重放请求被 Core 拒绝：" +
                std::string(replay_status.message.data, replay_status.message.size);
        } else {
            failure = "GameplayEffect 重放调用失败：ABI status=" +
                std::to_string(replay_status.code);
        }
        // The UI remains short; the complete Core message and call context go to
        // attack-replay.log in this plugin's own directory.
        AppendReplayDiagnosticLog(*context, replay_status.code, failure);
        ++context->replay_failure_count;
        const bool single_shot = context->proof_single_replay;
        context->replaying = false;
        context->waiting_for_damage = false;
        if (single_shot) {
            context->enabled = false;
            context->replay_requested.store(false, std::memory_order_release);
        }
        ArmForNextAttack(*context);
        context->status = single_shot
            ? "单次验证失败：Core 拒绝此次重放，已自动停用；详见 attack-replay.log"
            : "重放失败；已恢复捕获。详见 attack-replay.log";
        return;
    }
    if (context->proof_single_replay) {
        context->proof_single_replay_candidate_count_available =
            ReadMatchingCandidateCount(replay_status, context->proof_single_replay_candidate_count);
        context->proof_single_replay_single_candidate =
            context->proof_single_replay_candidate_count_available &&
            context->proof_single_replay_candidate_count == 1U;
        context->proof_single_replay_hp_result_available =
            ReadTargetHpReduction(replay_status, context->proof_single_replay_hp_reduced);
        context->proof_single_replay_native_exec_result_available =
            ReadNativeExecSeen(replay_status, context->proof_single_replay_native_exec_seen);
        AppendReplayInvocationEvidenceLog(*context, replay_status);
    }
    context->waiting_for_damage = true;
    context->status = "已提交原生 GameplayEffect 追加调用，等待同一目标的新伤害事件";
}

// Final compact UI based on the legacy plugin layout; diagnostic details stay in attack-replay.log.
void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || !UiReady(ui)) return;
    std::scoped_lock state_lock(context->state_mutex);

    int open = 1;
    if (ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 360.0F, 0.0F, 4U);
    }
    const int window_visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView("伤害链式反应"), &open, 0);
    if (window_visible != 0) {
        int enabled = context->enabled ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView("启用"), &enabled) != 0) {
            const bool next_enabled = enabled != 0;
            if (next_enabled != context->enabled) {
                context->enabled = next_enabled;
                if (context->enabled) {
                    context->status = context->captured
                        ? "已启用：伤害链式反应运行中"
                        : "已启用：等待战斗伤害";
                    if (context->captured) {
                        context->replay_requested.store(true, std::memory_order_release);
                    }
                } else {
                    context->stop_requested.store(true, std::memory_order_release);
                    context->status = "已停用";
                }
            }
        }

        if (ui->input_uint32 != nullptr) {
            ui->input_uint32(
                ui->user, anomaly::sdk::StringView("伤害重复次数"),
                &context->repeat_count, 1U, 10U);
            context->repeat_count = std::clamp(context->repeat_count, 1U, 10U);
        }

        const bool binding_ready = CombatReady(context->combat) &&
            context->framework != nullptr && context->framework->tick_sequence != nullptr;
        const std::string binding_line = std::string("绑定=") +
            (binding_ready ? "就绪" : "等待") + " 触发=" +
            (context->captured ? "战斗中" : "等待战斗");
        ui->text(ui->user, anomaly::sdk::StringView(binding_line));

        const std::string counters =
            "伤害=" + std::to_string(context->damage_event_count) +
            "次 失败=" + std::to_string(context->replay_failure_count) +
            "次 重放=" + std::to_string(context->replay_attempt_count) + "次";
        ui->text(ui->user, anomaly::sdk::StringView(counters));

        // No route-validation controls or diagnostic internals are exposed in the final UI.
        ui->end_window(ui->user);
    } else {
        // The host UI requires End even when Begin reports a collapsed/hidden window.
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
        anomaly::sdk::StringView("NTE Damage Chain Reaction"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.9.9"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw,
    };
    return anomaly::sdk::Ok();
}
