/*
 * 中文维护说明：本插件
 * - 本文件是该插件的主要实现入口，后续维护时优先在这里说明新增、修改和删除的行为。
 * - 当前代码逻辑保持不变；本次仅补充中文维护注释，便于后续逆向、排错和功能回溯。
 * - 不把未经验证的猜测写成实现依据；涉及游戏调用、偏移、签名或 ABI 时应注明实际证据来源。
 */
/*
 * 中文维护说明：
 * 1. 本文件是 NTE Attack Replay 插件的实现；本分支不修改 Anomaly 宿主核心。
 * 2. 插件始终监听玩家对目标产生的真实 DamageEvent，并记录攻击上下文。
 * 3. 普通攻击不依赖技能句柄；其重放路径依据 HTGame 中已确认的
 *    DT_AbilityInput / HTAbilityInputRow / MeleeAtack / InputID / Param
 *    以及 ActivateAbilityFromID、ReleaseAbilityFromID 反射函数执行。
 * 4. 技能攻击继续通过 Anomaly NTE skill-invocation 服务执行。
 * 5. Draw 只产生请求；真正的游戏调用在 Update 的 Game 域执行，避免跨线程
 *    直接操作游戏对象。
 * 6. 每次重放只有在之后观察到新的“玩家 -> 非玩家目标”DamageEvent 后，
 *    才计为一次成功重放；仅收到 accepted=1 不作为成功依据。
 * 7. 本文件中的原生偏移、函数名、DataTable 字段和 ProcessEvent 调用链，
 *    均对应当前项目/HTGame 证据；禁止把未经验证的猜测写入这里。
 *
 * 本次改动行为说明：
 * - 新增/完善普通攻击原生输入绑定与重放路径。
 * - 增加对 DataTable 行结构、属性类型、参数大小的运行时校验。
 * - 增加重放后的真实 DamageEvent 验证。
 * - 保留自动捕获、技能重放和 UI 请求/游戏线程分离行为。
 */
// Record the first player->target DAMAGE event; normal attacks do not require a skill.
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/interop.h"

#include <Windows.h>
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
#include <utility>
#include <vector>

namespace {

struct NormalAttackBinding { uintptr_t world{}; uintptr_t controller{}; uintptr_t triggered{}; uintptr_t completed{}; std::array<uint8_t,8> pressed{}; std::array<uint8_t,8> released{}; };

struct Context final {
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* invocation{};
    const AnomalyUiServiceV1* ui{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNteAttackInputServiceV1* attack_input{};

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
    uint64_t captured_replay_id{};
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
    uint64_t replay_last_tick{};
    uint64_t replay_target_count{};
    std::string status{"自动等待玩家下一次攻击"};

    uint64_t last_skill_generation{};
    uint64_t last_skill_sequence{};

    // A replay is not successful when the bridge merely returns accepted=1.
    // Wait for a newer player->target DamageEvent before counting the replay.
    uint64_t replay_damage_cursor{};
    std::chrono::steady_clock::time_point replay_damage_deadline{};
    bool waiting_for_damage{};
    NormalAttackBinding normal_attack{};
};

template <typename Struct, typename Field>
// HasField 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

// 检查战斗服务结构长度以及 latest_event_sequence、next_event、current_combatant 等回调是否存在；只有事件序列、事件读取和当前战斗角色查询同时可用时，攻击捕获逻辑才继续运行。
bool CombatReady(const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
                    decltype(AnomalyNteCombatServiceV1::next_event)>(
               service, offsetof(AnomalyNteCombatServiceV1, next_event)) &&
           service->latest_event_sequence != nullptr &&
           service->next_event != nullptr &&
           service->current_combatant != nullptr;
}

// SkillsReady 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool SkillsReady(const AnomalyNteSkillsServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillsServiceV1,
                    decltype(AnomalyNteSkillsServiceV1::page)>(
               service, offsetof(AnomalyNteSkillsServiceV1, page)) &&
           service->frame != nullptr && service->page != nullptr;
}

// InvocationReady 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool InvocationReady(const AnomalyNteSkillInvocationServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillInvocationServiceV1,
                    decltype(AnomalyNteSkillInvocationServiceV1::activate)>(
               service, offsetof(AnomalyNteSkillInvocationServiceV1, activate)) &&
           service->activate != nullptr;
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


/* 中文说明：以下常量和函数实现已从当前 HTGame/Anomaly 证据中确定的普通攻击原生调用链。 */
constexpr ptrdiff_t kWorldGameInstanceOffset = 560;
constexpr ptrdiff_t kGameInstanceLocalPlayersOffset = 56;
constexpr ptrdiff_t kLocalPlayerControllerOffset = 48;
constexpr ptrdiff_t kObjectClassOffset = 16;
constexpr ptrdiff_t kObjectNameOffset = 24;
constexpr ptrdiff_t kUStructChildrenOffset = 72;
constexpr ptrdiff_t kUStructSuperStructOffset = 64;
constexpr ptrdiff_t kUFieldNextOffset = 40;
constexpr ptrdiff_t kUFunctionNumParmsOffset = 180;
constexpr ptrdiff_t kUFunctionParmsSizeOffset = 182;
constexpr ptrdiff_t kUStructPropertyLinkOffset = 112;
constexpr ptrdiff_t kFFieldNameOffset = 32;
constexpr ptrdiff_t kFFieldClassOffset = 8;
constexpr ptrdiff_t kFPropertyElementSizeOffset = 52;
constexpr ptrdiff_t kFPropertyOffsetInternalOffset = 68;
constexpr ptrdiff_t kFPropertyPropertyLinkNextOffset = 72;
constexpr ptrdiff_t kDataTableRowMapOffset = 48;
constexpr size_t kDataTableRowStride = 24;
constexpr size_t kMaximumNameBytes = 1024;
constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";

// NativeRead 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool NativeRead(const void* address, void* destination, size_t size) noexcept {
    if (!address || !destination || size == 0) return false;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), address, destination, size, &copied) != 0 &&
        copied == size;
}
template <typename T>
// NativeRead 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool NativeRead(const void* address, T& value) noexcept {
    return NativeRead(address, &value, sizeof(value));
}
// NativePointer 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void* NativePointer(const void* address) noexcept {
    uintptr_t value{};
    return NativeRead(address, value) ? reinterpret_cast<void*>(value) : nullptr;
}

// NativeName 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
std::string NativeName(const Context& c, uint32_t id) {
    if (!c.names || !c.names->resolve_utf8 || id == 0) return {};
    size_t size{};
    if (c.names->resolve_utf8(c.names->user, id, nullptr, &size).code != ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > kMaximumNameBytes) return {};
    std::string value(size, '\0');
    if (c.names->resolve_utf8(c.names->user, id, value.data(), &size).code != ANOMALY_STATUS_V1_OK)
        return {};
    value.resize(size - 1);
    return value;
}
// NativeObjectName 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
std::string NativeObjectName(const Context& c, uintptr_t object) {
    uint32_t id{};
    if (!NativeRead(reinterpret_cast<const void*>(object + kObjectNameOffset), id)) return {};
    return NativeName(c, id);
}
// NativeFName 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
std::string NativeFName(const Context& c, uint32_t cmp, uint32_t number) {
    auto value = NativeName(c, cmp);
    if (number != 0) value += "_" + std::to_string(number - 1);
    return value;
}

// ResolveNativeWorld 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool ResolveNativeWorld(const Context& c, uintptr_t& address) noexcept {
    address = 0;
    if (!c.signature || !c.signature->resolve) return false;
    uintptr_t instruction{};
    if (c.signature->resolve(
            c.signature->user, anomaly::sdk::StringView("HTGame.exe"),
            anomaly::sdk::StringView(".text"),
            anomaly::sdk::StringView(kGWorldPattern), &instruction).code != ANOMALY_STATUS_V1_OK)
        return false;
    int32_t displacement{};
    if (!NativeRead(reinterpret_cast<const void*>(instruction + 3), displacement)) return false;
    address = static_cast<uintptr_t>(
        static_cast<intptr_t>(instruction) + 7 + displacement);
    return address != 0;
}
// GetNativeController 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool GetNativeController(const Context& c, uintptr_t& world, uintptr_t& controller) noexcept {
    uintptr_t g_world{};
    if (!ResolveNativeWorld(c, g_world)) return false;
    if (!NativeRead(reinterpret_cast<const void*>(g_world), world) || !world) return false;
    uintptr_t game_instance{}, locals{}, local_player{};
    int32_t count{};
    if (!NativeRead(reinterpret_cast<const void*>(world + kWorldGameInstanceOffset), game_instance) ||
        !game_instance ||
        !NativeRead(reinterpret_cast<const void*>(game_instance + kGameInstanceLocalPlayersOffset), locals) ||
        !locals ||
        !NativeRead(reinterpret_cast<const void*>(game_instance + kGameInstanceLocalPlayersOffset + 8), count) ||
        count < 1 ||
        !NativeRead(reinterpret_cast<const void*>(locals), local_player) ||
        !local_player ||
        !NativeRead(reinterpret_cast<const void*>(local_player + kLocalPlayerControllerOffset), controller) ||
        !controller)
        return false;
    return true;
}

bool FindNativeFunction(const Context& c, uintptr_t cls, std::string_view target,
                        uint8_t num_params, uint16_t params_size,
                        uintptr_t& result) noexcept {
    for (unsigned depth{}; cls && depth < 64; ++depth) {
        uintptr_t field{};
        if (!NativeRead(reinterpret_cast<const void*>(cls + kUStructChildrenOffset), field))
            return false;
        for (unsigned count{}; field && count < 4096; ++count) {
            uintptr_t next{}, field_class{};
            if (!NativeRead(reinterpret_cast<const void*>(field + kUFieldNextOffset), next) ||
                !NativeRead(reinterpret_cast<const void*>(field + kObjectClassOffset), field_class))
                break;
            if (NativeObjectName(c, field_class) == "Function" &&
                NativeObjectName(c, field) == target) {
                uint8_t np{};
                uint16_t ps{};
                NativeRead(reinterpret_cast<const void*>(field + kUFunctionNumParmsOffset), np);
                NativeRead(reinterpret_cast<const void*>(field + kUFunctionParmsSizeOffset), ps);
                if (np == num_params && ps == params_size) {
                    result = field;
                    return true;
                }
            }
            if (!next || next == field) break;
            field = next;
        }
        uintptr_t super{};
        if (!NativeRead(reinterpret_cast<const void*>(cls + kUStructSuperStructOffset), super) ||
            !super || super == cls) break;
        cls = super;
    }
    return false;
}

bool NativePropertyOffset(const Context& c, uintptr_t owner, std::string_view name,
                          std::string_view type, int32_t expected_size,
                          int32_t& offset, uint16_t buffer_size = 0) noexcept {
    uintptr_t property{};
    if (!NativeRead(reinterpret_cast<const void*>(owner + kUStructPropertyLinkOffset), property))
        return false;
    for (unsigned i{}; property && i < 64; ++i) {
        uint32_t name_id{};
        if (!NativeRead(reinterpret_cast<const void*>(property + kFFieldNameOffset), name_id))
            break;
        if (NativeName(c, name_id) == name) {
            uintptr_t field_class{};
            uint32_t type_id{};
            int32_t actual_size{};
            if (!NativeRead(reinterpret_cast<const void*>(property + kFFieldClassOffset), field_class) ||
                !field_class ||
                !NativeRead(reinterpret_cast<const void*>(field_class), type_id) ||
                NativeName(c, type_id) != type ||
                !NativeRead(reinterpret_cast<const void*>(property + kFPropertyElementSizeOffset), actual_size) ||
                actual_size != expected_size ||
                !NativeRead(reinterpret_cast<const void*>(property + kFPropertyOffsetInternalOffset), offset) ||
                offset < 0 ||
                (buffer_size != 0 &&
                 (expected_size > buffer_size ||
                  offset > static_cast<int32_t>(buffer_size - expected_size))))
                return false;
            return true;
        }
        uintptr_t next{};
        if (!NativeRead(reinterpret_cast<const void*>(property + kFPropertyPropertyLinkNextOffset), next) ||
            next == property)
            break;
        property = next;
    }
    return false;
}

uintptr_t NativeObjectProperty(const Context& c, uintptr_t object,
                               std::string_view property_name) noexcept {
    if (!object) return 0;
    uintptr_t cls{};
    if (!NativeRead(reinterpret_cast<const void*>(object + kObjectClassOffset), cls))
        return 0;
    for (unsigned depth{}; cls && depth < 64; ++depth) {
        uintptr_t property{};
        if (!NativeRead(reinterpret_cast<const void*>(cls + kUStructPropertyLinkOffset), property))
            return 0;
        for (unsigned count{}; property && count < 4096; ++count) {
            uint32_t name_id{};
            if (!NativeRead(reinterpret_cast<const void*>(property + kFFieldNameOffset), name_id))
                return 0;
            if (NativeName(c, name_id) == property_name) {
                uintptr_t field_class{}, value{};
                uint32_t type_id{};
                int32_t size{}, offset{};
                if (!NativeRead(reinterpret_cast<const void*>(property + kFFieldClassOffset), field_class) ||
                    !field_class ||
                    !NativeRead(reinterpret_cast<const void*>(field_class), type_id) ||
                    NativeName(c, type_id) != "ObjectProperty" ||
                    !NativeRead(reinterpret_cast<const void*>(property + kFPropertyElementSizeOffset), size) ||
                    size != 8 ||
                    !NativeRead(reinterpret_cast<const void*>(property + kFPropertyOffsetInternalOffset), offset) ||
                    offset < 0 ||
                    !NativeRead(reinterpret_cast<const void*>(object + offset), value))
                    return 0;
                return value;
            }
            uintptr_t next{};
            if (!NativeRead(reinterpret_cast<const void*>(property + kFPropertyPropertyLinkNextOffset), next) ||
                next == property)
                break;
            property = next;
        }
        uintptr_t super{};
        if (!NativeRead(reinterpret_cast<const void*>(cls + kUStructSuperStructOffset), super) ||
            super == cls)
            break;
        cls = super;
    }
    return 0;
}

bool ReadNativeDataTable(uintptr_t table,
                         std::vector<std::pair<std::array<uint32_t, 2>, uintptr_t>>& rows) noexcept {
    uintptr_t data{};
    int32_t num{}, max{};
    if (!table ||
        !NativeRead(reinterpret_cast<const void*>(table + kDataTableRowMapOffset), data) ||
        !NativeRead(reinterpret_cast<const void*>(table + kDataTableRowMapOffset + 8), num) ||
        !NativeRead(reinterpret_cast<const void*>(table + kDataTableRowMapOffset + 12), max) ||
        !data || num <= 0 || num > 4096 || max < num)
        return false;
    rows.clear();
    rows.reserve(static_cast<size_t>(num));
    for (int32_t i{}; i < num; ++i) {
        const auto at = data + static_cast<size_t>(i) * kDataTableRowStride;
        std::array<uint32_t, 2> key{};
        uintptr_t row{};
        if (NativeRead(reinterpret_cast<const void*>(at), key[0]) &&
            NativeRead(reinterpret_cast<const void*>(at + 4), key[1]) &&
            NativeRead(reinterpret_cast<const void*>(at + 8), row) &&
            key[0] != 0 && row != 0)
            rows.emplace_back(key, row);
    }
    return !rows.empty();
}

// 普通攻击调用只经过 Host 已验证的 attack-input ABI；插件不直接调用 UObject::ProcessEvent。
bool InvokeNativeNormalAttack(Context& c) {
    if (c.attack_input == nullptr || c.attack_input->activate_melee == nullptr) {
        return false;
    }
    return c.attack_input->activate_melee(c.attack_input->user).code ==
        ANOMALY_STATUS_V1_OK;
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
    const AnomalyNteCombatEventV1& event) {
    // The unified combat stream exposes the event's DamageSource name through
    // event_name_utf8(event). name_id is not the source_id accepted by
    // source_name_utf8, so passing name_id to source_name_utf8 can silently
    // lose the skill correlation and incorrectly fall back to normal input.
    if (combat == nullptr || combat->event_name_utf8 == nullptr || event.name_id == 0) {
        return {};
    }
    size_t size = 0;
    const auto sizing = combat->event_name_utf8(
        combat->user, &event, nullptr, &size);
    if (sizing.code != ANOMALY_STATUS_V1_OK &&
        sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) {
        return {};
    }
    if (size == 0) return {};
    std::string result(size, '\0');
    if (combat->event_name_utf8(
            combat->user, &event, result.data(), &size).code != ANOMALY_STATUS_V1_OK) {
        return {};
    }
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
// UpdateSkillCandidate 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void UpdateSkillCandidate(Context& context) {
    if (!SkillsReady(context.skills)) return;

    AnomalyNteSkillFrameV1 frame{};
    frame.struct_size = sizeof(frame);
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        frame.character.id == 0) {
        return;
    }

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> skills{};
    AnomalyNteSkillPageRequestV1 request{};
    request.struct_size = sizeof(request);
    request.generation = frame.generation;
    request.offset = 0;
    request.capacity = static_cast<uint32_t>(skills.size());
    AnomalyNteSkillPageResultV1 result{};
    result.struct_size = sizeof(result);

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

// ResolveReplaySkill 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool ResolveReplaySkill(Context& context, AnomalyGenerationHandleV1* skill_out) {
    if (!SkillsReady(context.skills) || skill_out == nullptr) return false;

    // A handle from the capture can remain valid through the replay. Check it first.
    if (context.skills->snapshot_by_handle != nullptr && context.captured_skill.id != 0) {
        AnomalyNteSkillSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
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
    frame.struct_size = sizeof(frame);
    if (context.skills->frame(context.skills->user, &frame).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> skills{};
    AnomalyNteSkillPageRequestV1 request{};
    request.struct_size = sizeof(request);
    request.generation = frame.generation;
    request.offset = 0;
    request.capacity = static_cast<uint32_t>(skills.size());
    AnomalyNteSkillPageResultV1 result{};
    result.struct_size = sizeof(result);
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

// ArmForNextAttack 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ArmForNextAttack(Context& context) {
    context.captured = false;
    context.replaying = false;
    context.replay_done = 0;
    context.captured_skill = {};
    context.captured_ability = {};
    context.captured_target = {};
    context.captured_input_id = -1;
    context.captured_damage_sequence = 0;
    context.captured_replay_id = 0;
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
    context.replay_damage_cursor = 0;
    context.replay_damage_deadline = {};
    context.replay_last_tick = 0;
    context.replay_target_count = 0;
    context.waiting_for_damage = false;
    context.status = context.enabled ? "自动等待玩家下一次攻击" : "自动记录中：重放功能未启用";

    // Starting at the current tail prevents an old combat event from being mistaken
    // for the next attack after a world change or plugin restart.
    if (context.combat != nullptr) {
        context.combat_cursor = context.combat->latest_event_sequence(
            context.combat->user);
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
        AnomalyNteCombatEventV1 event{};
        event.struct_size = sizeof(event);
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

        if (event.replay_id == 0) {
            context.status = "检测到真实伤害，但 Host 未生成可重放 DamageEvent 上下文；已跳过";
            continue;
        }
        context.captured = true;
        context.captured_damage_sequence = event.sequence;
        context.captured_replay_id = event.replay_id;
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
            ReadDamageSourceName(context.combat, event);
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

// 使用 Host 保留的真实 HTDamageEvent 上下文重放；每次提交后必须观察到新的玩家->目标 DamageEvent 才能计数。
ReplayCallResult ReplayOnce(Context& context, uint32_t* status_code, uint32_t* accepted) {
    if (status_code != nullptr) *status_code = ANOMALY_STATUS_V1_OK;
    if (accepted != nullptr) *accepted = 0;
    if (!context.captured || context.captured_replay_id == 0 ||
        context.attack_input == nullptr) {
        return ReplayCallResult::InvalidState;
    }
    const bool replay_service_available =
        HasField<AnomalyNteAttackInputServiceV1,
            decltype(AnomalyNteAttackInputServiceV1::replay_damage_event)>(
                context.attack_input,
                offsetof(AnomalyNteAttackInputServiceV1, replay_damage_event)) &&
        context.attack_input->replay_damage_event != nullptr;
    if (!replay_service_available) {
        if (status_code != nullptr) *status_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        return ReplayCallResult::ServiceError;
    }

    // Reapply the captured native damage context instead of re-triggering the attack input
    // or re-activating an ability. The Host validates target, world, hit data, tags and effect.
    const auto status = context.attack_input->replay_damage_event(
        context.attack_input->user, context.captured_replay_id);
    if (status_code != nullptr) *status_code = status.code;
    if (status.code != ANOMALY_STATUS_V1_OK) return ReplayCallResult::ServiceError;
    if (accepted != nullptr) *accepted = 1;
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
    context->signature = sdk_host.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
    context->names = sdk_host.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    context->framework = sdk_host.Query<AnomalyUe5FrameworkServiceV1>(
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION).get();
    context->attack_input = sdk_host.Query<AnomalyNteAttackInputServiceV1>(
        ANOMALY_NTE_ATTACK_INPUT_SERVICE_V1_ID,
        ANOMALY_NTE_ATTACK_INPUT_SERVICE_V1_VERSION).get();
    context->ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();

    if (!CombatReady(context->combat) || !SkillsReady(context->skills) ||
        !InvocationReady(context->invocation) || !UiReady(context->ui) ||
        context->signature == nullptr || context->names == nullptr || context->framework == nullptr || context->attack_input == nullptr ||
        context->signature->resolve == nullptr || context->names->resolve_utf8 == nullptr ||
        context->framework->tick_sequence == nullptr ||
        !HasField<AnomalyNteAttackInputServiceV1,
            decltype(AnomalyNteAttackInputServiceV1::replay_damage_event)>(
                context->attack_input,
                offsetof(AnomalyNteAttackInputServiceV1, replay_damage_event)) ||
        context->attack_input->replay_damage_event == nullptr) {
        delete context;
        return Status(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "attack replay requires NTE combat, skills, skill-invocation and UI services");
    }

    *plugin_context = context;
    return anomaly::sdk::Ok();
}

// 建立 AttackReplay 的运行状态并启动事件/输入轮询，使后续 Update 可以捕获并重放攻击；重复启动不会重新创建已经存在的状态。
AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->enabled = false;
    context->replay_count = 10;
    context->replay_done = 0;
    context->replay_target_count = 0;
    context->replay_last_tick = 0;
    context->replaying = false;
    context->replay_requested.store(false, std::memory_order_release);
    context->stop_requested.store(false, std::memory_order_release);
    ArmForNextAttack(*context);
    return anomaly::sdk::Ok();
}

// 停止 AttackReplay 的事件捕获和重放状态，并清除待验证的攻击上下文，防止插件停止后继续消费战斗事件。
AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
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
            context->waiting_for_damage = false;
            // x denotes x+1 additional hits after the original event; at most one is
            // submitted per Game tick and the next is gated on a fresh DamageEvent.
            context->replay_target_count = context->replay_count + 1U;
            context->replay_last_tick = context->captured_tick_sequence;
            context->status = "已提交重放，等待原始伤害后的下一游戏帧";
        } else {
            context->status = context->captured
                ? "无法开始重放：请先启用重放功能"
                : "无法开始重放：当前没有已捕获攻击";
        }
    }

    if (!context->replaying) return;

    const auto now = std::chrono::steady_clock::now();
    const uint64_t current_tick = context->framework->tick_sequence(
        context->framework->user);

    // accepted=1 only means the call reached the game. A replay is counted
    // only after a fresh player->target DamageEvent is observed.
    if (context->waiting_for_damage) {
        AnomalyNteCombatEventV1 event{};
        event.struct_size = sizeof(event);
        const auto damage_status = context->combat->next_event(
            context->combat->user, context->replay_damage_cursor, &event);
        if (damage_status.code == ANOMALY_STATUS_V1_OK) {
            context->replay_damage_cursor = event.sequence;
            if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE &&
                SameHandle(event.source, context->character) &&
                event.target.id != 0 &&
                !SameHandle(event.target, context->character)) {
                context->waiting_for_damage = false;
                ++context->replay_done;
                context->replay_last_tick = event.tick_sequence;
                context->status = "已确认新的 DamageEvent，进入下一游戏帧";
            }
        } else if (damage_status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
            context->replaying = false;
            context->waiting_for_damage = false;
            context->status = "重放验证失败：读取新的 DamageEvent 时发生错误";
            ArmForNextAttack(*context);
            return;
        }

        if (context->replaying && context->replay_done >= context->replay_target_count) {
            context->replaying = false;
            context->status = "重放完成，继续自动等待下一次攻击";
            ArmForNextAttack(*context);
            return;
        }
        if (context->waiting_for_damage && now >= context->replay_damage_deadline) {
            context->replaying = false;
            context->waiting_for_damage = false;
            context->status = "重放失败：原生伤害上下文已提交，但未观察到新的 DamageEvent";
            ArmForNextAttack(*context);
            return;
        }
        if (context->waiting_for_damage) return;
    }

    if (context->replay_done >= context->replay_target_count) {
        context->replaying = false;
        context->status = "重放完成，继续自动等待下一次攻击";
        ArmForNextAttack(*context);
        return;
    }

    // on_update() runs once per Game tick. Do not issue a second replay invocation
    // inside the same tick; one accepted attack is allowed to mature into one damage
    // event before the next frame is permitted to submit another attack.
    if (current_tick == 0 || current_tick == context->replay_last_tick) return;

    // Snapshot the event tail immediately before invoking the replay.
    context->replay_damage_cursor = context->combat->latest_event_sequence(
        context->combat->user);
    context->replay_damage_deadline = now + std::chrono::milliseconds(2000);

    uint32_t replay_status = ANOMALY_STATUS_V1_OK;
    uint32_t accepted = 0;
    const ReplayCallResult replay_result =
        ReplayOnce(*context, &replay_status, &accepted);
    if (replay_result != ReplayCallResult::Success) {
        context->replaying = false;
        if (replay_result == ReplayCallResult::NoSkill) {
            context->status = context->captured_has_skill
                ? "重放失败：当前捕获技能句柄已失效或无法重新解析"
                : "已记录普通攻击，但原生 ActivateAbilityFromID/ReleaseAbilityFromID 输入绑定不可用";
        } else if (replay_result == ReplayCallResult::Rejected) {
            context->status = "重放被游戏拒绝：skill activate accepted=0";
        } else {
            context->status = "重放调用失败：ABI status=" + std::to_string(replay_status);
        }
        ArmForNextAttack(*context);
        return;
    }
    context->waiting_for_damage = true;
    context->status = accepted != 0
        ? "原生伤害上下文已提交，等待新的 DamageEvent"
        : "原生伤害重放已提交，等待新的 DamageEvent";
}

// Draw 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
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
            ui->user, anomaly::sdk::StringView("额外合法重击数 x（实际重击 x+1）"),
            &context->replay_count, 1, 10);
        context->replay_count = std::clamp(context->replay_count, 1u, 100000u);
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
            "额外重击进度：" + std::to_string(context->replay_done) + "/" +
            std::to_string(context->replay_target_count != 0
                ? context->replay_target_count : context->replay_count + 1U);
        ui->text(ui->user, anomaly::sdk::StringView(progress));
        ui->text(ui->user, anomaly::sdk::StringView("节拍：每个 Game tick 最多重放一次捕获的原生伤害上下文"));
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
        anomaly::sdk::StringView("1.3.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw,
    };
    return anomaly::sdk::Ok();
}
