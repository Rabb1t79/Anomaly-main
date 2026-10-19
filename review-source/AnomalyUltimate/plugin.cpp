// Anomaly Ultimate —— 四合一功能插件
// =============================================================================
// 1. Chronos（时间暂停）：把世界时间膨胀写入极小值（近似冻结），定时 / 无限两种模式。
//    复用 TimeAccel 已验证的寻址链：ProcessEvent 锚定 PlayerController
//      → AActor::OuterPrivate(+0x20) → ULevel::WorldSettings(+0x2B0) → HTWorldSettings，
//    写其私有成员 +0x5E0（游戏每帧同步给引擎 TimeDilation）。
// 2. Damage Replay（伤害链式反应）：订阅 anomaly.ue5.process-event 捕获伤害函数
//    调用，读 UObject vtable 槽位 0x4C(=76 ProcessEvent) 用同一份 object/function/params
//    重放 replayCount 次，形成「打一下出很多次伤害」的链式反应（非模拟按键）。
// 3. Volleyball Win（排球自动胜利）：热键切换，经 anomaly.interop.signature 解析
//    GetRallyState（返回 EHTVolleyballRallyState），hook 后识别对局并请求胜利。
// 4. Developer GM（开发者控制台）：热键切换，经 signature+patch 把 IsGM /
//    CanShowGMConsole 补成恒返回 true。
//
// 安全设计：写只在 on_update（Game 域）做；热键/UI 只置请求标志；内存读写用 SEH 兜底；
//          服务缺失只降级对应功能，不崩溃。所有偏移均为 dump 反射证据或已注明待运行时
//          签名解析，不硬编码绝对地址。

#include "anomaly/sdk/cpp.hpp"

#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>

namespace {

using anomaly::sdk::Host;
using anomaly::sdk::StringView;

// UI 尾部字段存在性检查（ABI 兼容）。
#define HAS(ui, field)                                                        \
    ((ui) != nullptr &&                                                       \
     (ui)->struct_size >=                                                     \
         offsetof(AnomalyUiServiceV1, field) + sizeof((ui)->field) &&         \
     (ui)->field != nullptr)

// UObject 布局（反射证据）。
constexpr std::uintptr_t kClassPrivateOffset = 0x10;
constexpr std::uintptr_t kNamePrivateOffset = 0x18;
constexpr std::uintptr_t kOuterPrivateOffset = 0x20;
constexpr std::uintptr_t kLevelWorldSettingsOffset = 0x2B0;  // ULevel::WorldSettings
constexpr std::uintptr_t kWorldDilationOffset = 0x5E0;       // HTWorldSettings 世界倍率

// Chronos：暂停即把倍率压到最小（近似冻结，与游戏自身下限一致）。
constexpr float kPausedDilation = 0.0001F;
constexpr float kNormalDilation = 1.0F;

constexpr char kControllerClassName[] = "BP_PlayerControllerBase_C";

// 伤害链式反应：重放的目标函数 AHTAbilityCharacter::OnDamaged（Dump 证据，UFunction）。
constexpr char kDamageFunctionName[] = "OnDamaged";
// ProcessEvent 在 UObject vtable 中的槽位（Dump INDEX_PROCESSEVENT = 76 = 0x4C）。
constexpr std::uintptr_t kProcessEventVtableIndex = 76;
// UFunction::ParmsSize 偏移（Profile layout ufunction.parmsSize = 182，uint16）。
constexpr std::uintptr_t kFunctionParmsSizeOffset = 182;

constexpr int kConfVersion = 1;
constexpr const char* kConfFile = "anomaly-ultimate.conf";

struct State {
    // ---- Chronos 指针链 ----
    std::uintptr_t controller{};
    std::uint32_t controller_class_name_id{};
    std::uintptr_t ws{};            // HTWorldSettings
    int find_utf8_tried{};
    int walk_done{};
    std::uint32_t walk_total{};
    std::uint32_t walk_cursor{};

    // ---- Chronos 配置 ----
    double duration{10.0};          // 定时暂停秒数
    std::uint32_t key_timed{0};     // 0 = 未绑定
    std::uint32_t key_infinite{0};
    std::uint32_t mod_timed{};
    std::uint32_t mod_infinite{};

    // ---- Chronos 运行态 ----
    int paused{};
    int infinite{};
    double remaining{};
    double keep_accum{};
    float observed{1.0F};

    // ---- Damage Replay（倍攻连锁反应） ----
    int feature_enabled{1};          // 倍攻连锁反应 总开关
    int enabled{1};                  // 启用
    std::uint32_t repeat_count{100}; // 伤害重复次数
    std::uint32_t damage_function_name_id{};
    int find_damage_tried{};
    // 重放队列（逐 tick 错开重放，避免同帧叠加多重伤害数字）。
    std::uintptr_t replay_object{};
    std::uintptr_t replay_function{};
    std::array<std::uint8_t, 512> replay_params{};
    std::size_t replay_params_size{};
    std::uint32_t replay_remaining{};
    int replaying{};                 // 守卫：重放进入的调用不再计数/捕获
    std::uint64_t damage_seen{};     // 伤害 计数
    std::uint64_t fail_count{};      // 失败 计数
    std::uint64_t replay_done{};     // 重放 计数
    int in_combat{};                 // 触发=战斗中

    // ---- 请求标志（热键/UI → Update） ----
    int req_chronos_timed{};
    int req_chronos_infinite{};
    int req_chronos_stop{};
    int req_volleyball{};
    int req_gm{};
    int req_save{};

    // ---- 功能开关 ----
    int volleyball_enabled{};
    int gm_enabled{};

    // ---- 玩家门禁 ----
    int player_valid{-1};
    double gate_accum{};

    std::uint64_t events{};
    char status[512]{};
};

State g_state;
std::recursive_mutex g_state_mutex;
int g_window_open = 1;

const AnomalyUiServiceV1* g_ui{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyUe5NamesServiceV1* g_names{};
const AnomalyUe5ObjectsServiceV1* g_objects{};
const AnomalyUe5ProcessEventServiceV1* g_process_event{};
const AnomalyInputServiceV1* g_input{};
const AnomalyStorageServiceV1* g_storage{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyNtePlayerServiceV1* g_player{};
const AnomalyNteCombatServiceV1* g_combat{};
const AnomalySignatureServiceV1* g_signature{};
const AnomalyPatchServiceV1* g_patch{};
const AnomalyHookServiceV1* g_hook{};

AnomalyGenerationHandleV1 g_events_handle{};
AnomalyGenerationHandleV1 g_hotkey_timed{};
AnomalyGenerationHandleV1 g_hotkey_infinite{};
AnomalyGenerationHandleV1 g_hotkey_volleyball{};
AnomalyGenerationHandleV1 g_hotkey_gm{};
AnomalyGenerationHandleV1 g_patch_gm_handle{};

void SetError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(g_state.status, sizeof(g_state.status), format, args);
    va_end(args);
}

// --------------------------- 安全内存读写（SEH） ------------------------------
bool SafeReadPtr(std::uintptr_t address, std::uintptr_t* out) {
    if (out == nullptr) return false;
    __try {
        *out = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t SafeReadU32(std::uintptr_t address) {
    __try {
        return *reinterpret_cast<const std::uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SafeReadF32(std::uintptr_t address, float* out) {
    if (out == nullptr) return false;
    __try {
        *out = *reinterpret_cast<const volatile float*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LooksLikePointer(std::uintptr_t v) {
    return v > 0x10000ULL && v < 0x800000000000ULL;
}

// 写世界倍率：优先 anomaly.core::write_memory，失败退回直接写（SEH 兜底）。
bool WriteWorldDilation(float value) {
    if (!LooksLikePointer(g_state.ws)) return false;
    const std::uintptr_t address = g_state.ws + kWorldDilationOffset;
    bool ok = false;
    if (g_core != nullptr && g_core->write_memory != nullptr) {
        AnomalyByteSpanV1 span{};
        span.data = reinterpret_cast<const std::uint8_t*>(&value);
        span.size = sizeof(value);
        ok = g_core->write_memory(g_core->user, address, span).code == ANOMALY_STATUS_V1_OK;
    }
    if (!ok) {
        __try {
            *reinterpret_cast<volatile float*>(address) = value;
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!ok) return false;
    float back = 0.0F;
    if (SafeReadF32(address, &back) && back == value) {
        g_state.observed = value;
        return true;
    }
    return false;
}

void RestoreDilation() { WriteWorldDilation(kNormalDilation); }

void StopPause(const char* reason) {
    const bool was = (g_state.paused != 0) || (g_state.infinite != 0);
    g_state.paused = 0;
    g_state.infinite = 0;
    g_state.remaining = 0.0;
    if (was) RestoreDilation();
    if (reason != nullptr) SetError("%s", reason);
}

// --------------------------- ProcessEvent 锚定控制器 --------------------------
bool ClassNameMatches(std::uintptr_t object, std::uint32_t name_id) {
    if (object == 0 || name_id == 0) return false;
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(object + kClassPrivateOffset, &cls) || cls == 0) return false;
    return SafeReadU32(cls + kNamePrivateOffset) == name_id;
}

// 读取 UFunction 的 FName id（UObject::NamePrivate 在 +0x18）。
std::uint32_t FunctionNameId(std::uintptr_t function) {
    if (!LooksLikePointer(function)) return 0;
    return SafeReadU32(function + kNamePrivateOffset);
}

// 通过 UObject vtable 槽位调用 ProcessEvent（重放伤害，非模拟输入）。
// UObject::ProcessEvent(UFunction* Function, void* Parms) —— x64 下 rcx=this, rdx=Function, r8=Parms。
bool CallProcessEvent(std::uintptr_t object, std::uintptr_t function, void* parameters) {
    if (!LooksLikePointer(object) || !LooksLikePointer(function)) return false;
    std::uintptr_t vtable = 0;
    if (!SafeReadPtr(object, &vtable) || !LooksLikePointer(vtable)) return false;
    std::uintptr_t pe = 0;
    if (!SafeReadPtr(vtable + kProcessEventVtableIndex * 8, &pe) || !LooksLikePointer(pe)) return false;
    using ProcessEventFn = void (*)(void*, void*, void*);
    reinterpret_cast<ProcessEventFn>(pe)(reinterpret_cast<void*>(object),
                                         reinterpret_cast<void*>(function), parameters);
    return true;
}

// 读取 UFunction 的参数区大小（UFunction::ParmsSize，uint16，偏移 kFunctionParmsSizeOffset）。
std::size_t ReadFunctionParmsSize(std::uintptr_t function) {
    if (!LooksLikePointer(function)) return 0;
    __try {
        const std::uint16_t size = *reinterpret_cast<const std::uint16_t*>(
            function + kFunctionParmsSizeOffset);
        return static_cast<std::size_t>(size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void ANOMALY_CALL OnProcessEvent(void*, std::uintptr_t object, std::uintptr_t function,
                                 void* parameters) {
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    State& s = g_state;
    ++s.events;
    if (object == 0) return;

    // Chronos：锚定控制器。
    if (s.controller_class_name_id != 0 && ClassNameMatches(object, s.controller_class_name_id)) {
        s.controller = object;
    }

    // Damage Replay：倍攻连锁反应（多次真实伤害，非模拟输入）。
    if (!s.feature_enabled || !s.enabled) return;
    if (s.repeat_count == 0 || s.damage_function_name_id == 0) return;
    if (FunctionNameId(function) != s.damage_function_name_id) return;
    // 重放进入的调用直接返回：不再计数、不再捕获，避免无限递归。
    if (s.replaying) return;

    ++s.damage_seen;
    // 捕获本次伤害：复制参数区 + 记录 object/function，交给 Update 逐 tick 错开重放。
    const std::size_t parms_size = ReadFunctionParmsSize(function);
    if (parms_size == 0 || parms_size > s.replay_params.size() || parameters == nullptr) {
        ++s.fail_count;
        return;
    }
    s.replay_object = object;
    s.replay_function = function;
    std::memcpy(s.replay_params.data(), parameters, parms_size);
    s.replay_params_size = parms_size;
    s.replay_remaining = s.repeat_count;
}

void ResolveDamageFunctionNameId() {
    State& s = g_state;
    if (s.damage_function_name_id != 0) return;
    if (s.find_damage_tried != 0) return;
    s.find_damage_tried = 1;
    if (g_names == nullptr || g_names->find_utf8 == nullptr) return;
    std::uint32_t id = 0;
    if (g_names->find_utf8(g_names->user, StringView(kDamageFunctionName), &id).code ==
        ANOMALY_STATUS_V1_OK) {
        s.damage_function_name_id = id;
    } else {
        SetError("未找到伤害函数 %s（游戏版本可能变了）", kDamageFunctionName);
    }
}

void ResolveWorldSettings() {
    State& s = g_state;
    if (!LooksLikePointer(s.controller)) { s.ws = 0; return; }
    std::uintptr_t level = 0;
    if (!SafeReadPtr(s.controller + kOuterPrivateOffset, &level) || !LooksLikePointer(level)) {
        s.ws = 0; return;
    }
    std::uintptr_t ws = 0;
    if (!SafeReadPtr(level + kLevelWorldSettingsOffset, &ws) || !LooksLikePointer(ws)) {
        s.ws = 0; return;
    }
    s.ws = ws;
}

void ResolveControllerClassNameId() {
    State& s = g_state;
    if (s.controller_class_name_id != 0) return;
    if (s.find_utf8_tried == 0) {
        s.find_utf8_tried = 1;
        if (g_names != nullptr && g_names->find_utf8 != nullptr) {
            std::uint32_t id = 0;
            if (g_names->find_utf8(g_names->user, StringView(kControllerClassName), &id).code ==
                ANOMALY_STATUS_V1_OK) {
                s.controller_class_name_id = id;
                return;
            }
        }
    }
    if (s.walk_done != 0) return;
    if (g_objects == nullptr || g_objects->snapshot_at == nullptr || g_objects->count == nullptr ||
        g_names == nullptr || g_names->resolve_utf8 == nullptr) {
        s.walk_done = 1;
        return;
    }
    if (s.walk_total == 0) s.walk_total = g_objects->count(g_objects->user);
    std::uint32_t budget = 2048;
    while (budget-- > 0 && s.walk_cursor < s.walk_total) {
        AnomalyUe5ObjectSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        if (g_objects->snapshot_at(g_objects->user, s.walk_cursor, &snapshot).code ==
            ANOMALY_STATUS_V1_OK) {
            char name[128]{};
            std::size_t size = sizeof(name);
            if (g_names->resolve_utf8(g_names->user, snapshot.name_id, name, &size).code ==
                    ANOMALY_STATUS_V1_OK &&
                std::strcmp(name, kControllerClassName) == 0) {
                s.controller_class_name_id = snapshot.name_id;
                s.walk_done = 1;
                return;
            }
        }
        ++s.walk_cursor;
    }
    if (s.walk_cursor >= s.walk_total) s.walk_done = 1;
}

// --------------------------- 玩家门禁 ----------------------------------------
void PollPlayerGate(double dt) {
    State& s = g_state;
    if (g_player == nullptr || g_player->snapshot == nullptr) { s.player_valid = -1; return; }
    s.gate_accum += dt;
    if (s.player_valid >= 0 && s.gate_accum < 0.5) return;
    s.gate_accum = 0.0;
    AnomalyNtePlayerSnapshotV1 snap{};
    snap.struct_size = sizeof(snap);
    if (g_player->snapshot(g_player->user, &snap).code != ANOMALY_STATUS_V1_OK) {
        s.player_valid = 0; return;
    }
    s.player_valid = ((snap.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
                      (snap.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0) ? 1 : 0;
}

bool PlayerGateOpen() { return g_state.player_valid == 1; }

bool DeveloperModeEnabled() {
    if (g_ui == nullptr || !HAS(g_ui, developer_mode_enabled)) return false;
    return g_ui->developer_mode_enabled(g_ui->user) != 0;
}

// --------------------------- 战斗状态（触发=战斗中） ---------------------------
void PollCombatState() {
    if (g_combat == nullptr || g_combat->current_combatant == nullptr) {
        g_state.in_combat = 0;
        return;
    }
    AnomalyNteCombatantSnapshotV1 snap{};
    snap.struct_size = sizeof(snap);
    if (g_combat->current_combatant(g_combat->user, &snap).code != ANOMALY_STATUS_V1_OK) {
        g_state.in_combat = 0;
        return;
    }
    g_state.in_combat = (snap.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 ? 1 : 0;
}

// --------------------------- Volleyball / GM（interop）------------------------
// 通过签名服务解析函数地址。签名特征码需运行时匹配当前游戏 build；特征码为空时
// 该能力不可用（不硬编码绝对地址）。
uintptr_t ResolveBySignature(const char* pattern) {
    if (g_signature == nullptr || g_signature->resolve == nullptr || pattern == nullptr ||
        pattern[0] == '\0') return 0;
    uintptr_t address = 0;
    const auto status = g_signature->resolve(g_signature->user, StringView("HTGame.exe"),
                                              StringView(".text"), StringView(pattern), &address);
    return status.code == ANOMALY_STATUS_V1_OK ? address : 0;
}

void ApplyGmPatch() {
    if (g_patch_gm_handle.id != 0) return;  // 已打补丁
    if (g_patch == nullptr || g_patch->apply == nullptr) {
        SetError("patch 服务不可用，无法解锁 GM");
        return;
    }
    // IsGM / CanShowGMConsole 均返回 bool（Dump 证据），补成 "mov al,1; ret" 即返回 true。
    // 地址由 signature 服务运行时解析，特征码由宿主 Profile 提供。
    const uintptr_t is_gm = ResolveBySignature("");     // TODO: IsGM 特征码（需游戏二进制）
    const uintptr_t can_show = ResolveBySignature("");  // TODO: CanShowGMConsole 特征码
    if (is_gm == 0 && can_show == 0) {
        SetError("未解析到 IsGM/CanShowGMConsole（特征码未配置），GM 解锁不可用");
        return;
    }
    const std::uint8_t patch[] = {0xB0, 0x01, 0xC3};  // mov al,1; ret
    AnomalyByteSpanV1 span{};
    span.data = patch;
    span.size = sizeof(patch);
    if (is_gm != 0) {
        AnomalyGenerationHandleV1 handle{};
        g_patch->apply(g_patch->user, is_gm, span, StringView("ultimate.IsGM"), &handle);
    }
    if (can_show != 0) {
        AnomalyGenerationHandleV1 handle{};
        g_patch->apply(g_patch->user, can_show, span, StringView("ultimate.CanShowGMConsole"), &handle);
        g_patch_gm_handle = handle;
    }
}

// --------------------------- 订阅与热键 --------------------------------------
void SubscribeEvents() {
    if (g_process_event == nullptr || g_process_event->subscribe == nullptr) return;
    if (g_events_handle.id != 0) return;
    g_process_event->subscribe(g_process_event->user, &OnProcessEvent, nullptr, &g_events_handle);
}

void UnsubscribeEvents() {
    if (g_process_event != nullptr && g_process_event->unsubscribe != nullptr &&
        g_events_handle.id != 0) {
        g_process_event->unsubscribe(g_process_event->user, g_events_handle);
    }
    g_events_handle = AnomalyGenerationHandleV1{};
}

void ANOMALY_CALL OnHotkey(void*, AnomalyGenerationHandleV1 hotkey, const AnomalyInputSnapshotV1*) {
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    State& s = g_state;
    if (hotkey.id == g_hotkey_timed.id) s.req_chronos_timed = 1;
    else if (hotkey.id == g_hotkey_infinite.id) s.req_chronos_infinite = 1;
    else if (hotkey.id == g_hotkey_volleyball.id) s.req_volleyball = 1;
    else if (hotkey.id == g_hotkey_gm.id) s.req_gm = 1;
}

void RegisterHotkeys() {
    if (g_input == nullptr || g_input->register_hotkey == nullptr) return;
    const auto reg = [&](std::uint32_t vk, std::uint32_t mods, const char* id,
                         AnomalyGenerationHandleV1* out) {
        if (vk == 0) return;  // 未绑定
        AnomalyHotkeySpecV1 spec{};
        spec.struct_size = sizeof(spec);
        spec.modifiers = mods;
        spec.virtual_key = vk;
        spec.flags = 0u;
        spec.id = StringView(id);
        g_input->register_hotkey(g_input->user, &spec, &OnHotkey, nullptr, out);
    };
    reg(g_state.key_timed, g_state.mod_timed, "ultimate.chronos.timed", &g_hotkey_timed);
    reg(g_state.key_infinite, g_state.mod_infinite, "ultimate.chronos.infinite", &g_hotkey_infinite);
    reg('V', 0, "ultimate.volleyball", &g_hotkey_volleyball);
    reg('G', 0, "ultimate.gm", &g_hotkey_gm);
}

void ReleaseHotkeys() {
    if (g_input == nullptr || g_input->release_hotkey == nullptr) return;
    const auto rel = [&](AnomalyGenerationHandleV1 h) {
        if (h.id != 0) g_input->release_hotkey(g_input->user, h);
    };
    rel(g_hotkey_timed); rel(g_hotkey_infinite);
    rel(g_hotkey_volleyball); rel(g_hotkey_gm);
    g_hotkey_timed = g_hotkey_infinite = AnomalyGenerationHandleV1{};
    g_hotkey_volleyball = g_hotkey_gm = AnomalyGenerationHandleV1{};
}

// --------------------------- 配置持久化 --------------------------------------
std::size_t FormatConfig(char* text, std::size_t capacity) {
    State& s = g_state;
    const int written = std::snprintf(text, capacity,
        "conf_version=%d\nduration=%.4f\nfeature_enabled=%d\nenabled=%d\nrepeat_count=%u\n"
        "key_timed=%u\nkey_infinite=%u\nmod_timed=%u\nmod_infinite=%u\n",
        kConfVersion, s.duration, s.feature_enabled, s.enabled, s.repeat_count,
        s.key_timed, s.key_infinite, s.mod_timed, s.mod_infinite);
    if (written <= 0) return 0;
    const std::size_t size = static_cast<std::size_t>(written);
    return (size < capacity) ? size : capacity - 1;
}

AnomalyStatusV1 SaveConfig() {
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    if (g_storage == nullptr || g_storage->write_atomic == nullptr) {
        return AnomalyStatusV1{static_cast<std::uint32_t>(ANOMALY_STATUS_V1_UNAVAILABLE), 0, {}};
    }
    char text[512]{};
    const std::size_t size = FormatConfig(text, sizeof(text));
    AnomalyByteSpanV1 span{};
    span.data = reinterpret_cast<const std::uint8_t*>(text);
    span.size = size;
    return g_storage->write_atomic(g_storage->user, StringView(kConfFile), span);
}

void ANOMALY_CALL PersistTask(void*, AnomalyGenerationHandleV1) {
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    SaveConfig();
}

void LoadConfig() {
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    State& s = g_state;
    if (g_storage == nullptr || g_storage->read == nullptr) return;
    char text[512]{};
    AnomalyMutableByteSpanV1 span{};
    span.data = reinterpret_cast<std::uint8_t*>(text);
    span.size = sizeof(text) - 1;
    std::size_t size = span.size;
    if (g_storage->read(g_storage->user, StringView(kConfFile), span, &size).code !=
            ANOMALY_STATUS_V1_OK || size == 0) return;
    text[size < sizeof(text) ? size : sizeof(text) - 1] = '\0';

    int conf_version = 0;
    char* line = text;
    while (line != nullptr && *line != '\0') {
        char* next = std::strchr(line, '\n');
        if (next != nullptr) *next = '\0';
        char* eq = std::strchr(line, '=');
        if (eq != nullptr) {
            *eq = '\0';
            const char* key = line;
            const char* value = eq + 1;
            if (std::strcmp(key, "conf_version") == 0) conf_version = std::atoi(value);
            else if (std::strcmp(key, "duration") == 0) s.duration = std::atof(value);
            else if (std::strcmp(key, "feature_enabled") == 0)
                s.feature_enabled = std::atoi(value) != 0;
            else if (std::strcmp(key, "enabled") == 0)
                s.enabled = std::atoi(value) != 0;
            else if (std::strcmp(key, "repeat_count") == 0)
                s.repeat_count = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            else if (std::strcmp(key, "key_timed") == 0)
                s.key_timed = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            else if (std::strcmp(key, "key_infinite") == 0)
                s.key_infinite = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            else if (std::strcmp(key, "mod_timed") == 0)
                s.mod_timed = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            else if (std::strcmp(key, "mod_infinite") == 0)
                s.mod_infinite = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        }
        line = (next != nullptr) ? next + 1 : nullptr;
    }
    if (conf_version != kConfVersion) {
        s.duration = 10.0;
        s.feature_enabled = 1;
        s.enabled = 1;
        s.repeat_count = 100;
    }
    if (s.duration < 0.5) s.duration = 0.5;
    if (s.duration > 3600.0) s.duration = 3600.0;
    if (s.repeat_count > 1024) s.repeat_count = 1024;
}

// --------------------------- 绘制 --------------------------------------------
void Text(const AnomalyUiServiceV1* ui, const char* utf8) {
    if (HAS(ui, text)) ui->text(ui->user, StringView(utf8));
}

void DrawChronosTab(const AnomalyUiServiceV1* ui) {
    State& s = g_state;
    char line[256];

    Text(ui, "【Chronos 时间暂停】");
    if (HAS(ui, button) && ui->button(ui->user, StringView("定时暂停##chr-once"), 120.0F, 0.0F)) {
        s.req_chronos_timed = 1;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    if (HAS(ui, button) &&
        ui->button(ui->user, StringView(s.infinite ? "停止无限暂停##chr-tgl" : "无限暂停##chr-tgl"),
                   140.0F, 0.0F)) {
        s.req_chronos_infinite = 1;
    }
    if (s.paused) {
        std::snprintf(line, sizeof(line), "状态：已暂停（剩余 %.1f 秒）", s.remaining);
    } else if (s.infinite) {
        std::snprintf(line, sizeof(line), "状态：无限暂停中");
    } else {
        std::snprintf(line, sizeof(line), "状态：运行中（当前 %.4f 倍）",
                      static_cast<double>(s.observed));
    }
    Text(ui, line);
    if (HAS(ui, button) && ui->button(ui->user, StringView("保存设置##save-config"), 110.0F, 0.0F)) {
        s.req_save = 1;
    }
}

void DrawDamageTab(const AnomalyUiServiceV1* ui) {
    State& s = g_state;
    char line[256];

    Text(ui, "【伤害链式反应】");
    if (HAS(ui, checkbox)) {
        ui->checkbox(ui->user, StringView("启用伤害链总开关"), &s.feature_enabled);
        ui->checkbox(ui->user, StringView("启用重放"), &s.enabled);
    }

    char value_text[24]{};
    std::snprintf(value_text, sizeof(value_text), "%u", s.repeat_count);
    Text(ui, "每次命中后追加的重放次数");
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("-##cnt-dn"), 28.0F, 0.0F)) {
        if (s.repeat_count > 0) --s.repeat_count;
        s.req_save = 1;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    Text(ui, value_text);
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("+##cnt-up"), 28.0F, 0.0F)) {
        if (s.repeat_count < 1024) ++s.repeat_count;
        s.req_save = 1;
    }

    std::snprintf(line, sizeof(line), "OnDamaged 名称匹配：%s；战斗状态：%s",
                  s.damage_function_name_id != 0 ? "已解析（仅名称匹配）" : "未解析",
                  s.in_combat ? "战斗中" : "未知/非战斗");
    Text(ui, line);
    Text(ui, "说明：计数表示事件观察/调用尝试，不等同于已确认的实际伤害。");
    std::snprintf(line, sizeof(line), "观测事件=%llu 伤害事件匹配=%llu 重放调用成功=%llu 失败=%llu",
                  static_cast<unsigned long long>(s.events),
                  static_cast<unsigned long long>(s.damage_seen),
                  static_cast<unsigned long long>(s.replay_done),
                  static_cast<unsigned long long>(s.fail_count));
    Text(ui, line);
}

void DrawDeveloperTab(const AnomalyUiServiceV1* ui) {
    State& s = g_state;
    char line[256];

    if (HAS(ui, button) &&
        ui->button(ui->user, StringView(s.gm_enabled ? "关闭 GM##gm-tgl" : "解锁 GM##gm-tgl"),
                   130.0F, 0.0F)) {
        s.req_gm = 1;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    std::snprintf(line, sizeof(line), "Developer GM：%s", s.gm_enabled ? "已请求开启" : "关闭");
    Text(ui, line);
    Text(ui, "注意：原代码的签名特征码为空，未配置时该功能无法真正生效。");

    if (HAS(ui, separator)) ui->separator(ui->user);
    if (HAS(ui, button) &&
        ui->button(ui->user, StringView(s.volleyball_enabled ? "关闭排球自动赢##vb-tgl" : "开启排球自动赢##vb-tgl"),
                   170.0F, 0.0F)) {
        s.req_volleyball = 1;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    std::snprintf(line, sizeof(line), "排球自动胜利：%s", s.volleyball_enabled ? "已请求开启" : "关闭");
    Text(ui, line);
    Text(ui, "注意：原代码尚无已验证的 GetRallyState 签名与完整胜利处理逻辑。");
}

void DrawMain(const AnomalyUiServiceV1* ui) {
    if (!DeveloperModeEnabled()) {
        Text(ui, "需要开发者模式（平台设置 > 高级 > 开发者模式）");
        if (HAS(ui, separator)) ui->separator(ui->user);
    }

    const bool tabs_available = HAS(ui, begin_tab_bar) && HAS(ui, end_tab_bar) &&
                                HAS(ui, begin_tab_item) && HAS(ui, end_tab_item);
    if (tabs_available && ui->begin_tab_bar(ui->user, StringView("ultimate-tabs"), 0u) != 0) {
        if (ui->begin_tab_item(ui->user, StringView("时间暂停"), nullptr,
                               ANOMALY_UI_TAB_ITEM_V1_NONE, 1) != 0) {
            DrawChronosTab(ui);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, StringView("伤害链式反应"), nullptr,
                               ANOMALY_UI_TAB_ITEM_V1_NONE, 1) != 0) {
            DrawDamageTab(ui);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, StringView("开发者功能"), nullptr,
                               ANOMALY_UI_TAB_ITEM_V1_NONE, 1) != 0) {
            DrawDeveloperTab(ui);
            ui->end_tab_item(ui->user);
        }
        ui->end_tab_bar(ui->user);
    } else {
        DrawChronosTab(ui);
        if (HAS(ui, separator)) ui->separator(ui->user);
        DrawDamageTab(ui);
        if (HAS(ui, separator)) ui->separator(ui->user);
        DrawDeveloperTab(ui);
    }

    if (g_state.status[0] != '\0') {
        if (HAS(ui, separator)) ui->separator(ui->user);
        Text(ui, g_state.status);
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui_v1) {
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    const AnomalyUiServiceV1* ui = (ui_v1 != nullptr) ? ui_v1 : g_ui;
    if (!HAS(ui, begin_window) || !HAS(ui, end_window) || g_window_open == 0) return;
    if (ui->begin_window(ui->user, StringView("Anomaly Ultimate"), &g_window_open, 0u) != 0) {
        DrawMain(ui);
    }
    ui->end_window(ui->user);
}

// --------------------------- 生命周期 ----------------------------------------
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const Host view(host);

    const auto ui = view.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID,
                                                   ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || !HAS(ui.get(), text) || !HAS(ui.get(), begin_window)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_ui = ui.get();

    g_core = view.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
                                              ANOMALY_CORE_SERVICE_V1_VERSION).get();
    g_names = view.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID,
                                                   ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    g_objects = view.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
                                                       ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    g_process_event = view.Query<AnomalyUe5ProcessEventServiceV1>(
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID,
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION).get();
    g_input = view.Query<AnomalyInputServiceV1>(ANOMALY_INPUT_SERVICE_V1_ID,
                                                ANOMALY_INPUT_SERVICE_V1_VERSION).get();
    g_storage = view.Query<AnomalyStorageServiceV1>(ANOMALY_STORAGE_SERVICE_V1_ID,
                                                    ANOMALY_STORAGE_SERVICE_V1_VERSION).get();
    g_scheduler = view.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID,
                                                        ANOMALY_SCHEDULER_SERVICE_V1_VERSION).get();
    g_player = view.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
                                                     ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    g_combat = view.Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
                                                     ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();
    g_signature = view.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID,
                                                        ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
    g_patch = view.Query<AnomalyPatchServiceV1>(ANOMALY_PATCH_SERVICE_V1_ID,
                                                ANOMALY_PATCH_SERVICE_V1_VERSION).get();
    g_hook = view.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID,
                                              ANOMALY_HOOK_SERVICE_V1_VERSION).get();

    {
        std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
        g_state = State{};
        g_window_open = 1;
    }
    *context = &g_state;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    {
        std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
        LoadConfig();
    }
    RegisterHotkeys();
    SubscribeEvents();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    UnsubscribeEvents();
    ReleaseHotkeys();
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    StopPause(nullptr);
    SaveConfig();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    UnsubscribeEvents();
    ReleaseHotkeys();
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    StopPause(nullptr);
    g_ui = nullptr; g_core = nullptr; g_names = nullptr; g_objects = nullptr;
    g_process_event = nullptr; g_input = nullptr; g_storage = nullptr; g_scheduler = nullptr;
    g_player = nullptr; g_combat = nullptr;
    g_signature = nullptr; g_patch = nullptr; g_hook = nullptr;
    g_state = State{};
    g_window_open = 1;
}

void ANOMALY_CALL Update(void*, double delta_seconds) {
    std::lock_guard<std::recursive_mutex> guard(g_state_mutex);
    State& s = g_state;
    const double dt = (delta_seconds > 0.0) ? delta_seconds : 0.0;

    if (s.controller_class_name_id == 0) ResolveControllerClassNameId();
    if (s.damage_function_name_id == 0) ResolveDamageFunctionNameId();
    SubscribeEvents();
    ResolveWorldSettings();
    PollPlayerGate(dt);

    // 采样当前倍率（界面自检）。
    if (s.ws != 0) {
        float v = 1.0F;
        if (SafeReadF32(s.ws + kWorldDilationOffset, &v)) s.observed = v;
    }

    // ---- Chronos 请求处理 ----
    if (s.req_chronos_stop) {
        s.req_chronos_stop = 0;
        StopPause("已恢复");
    } else if (s.req_chronos_timed || s.req_chronos_infinite) {
        const bool infinite = s.req_chronos_infinite != 0;
        s.req_chronos_timed = 0;
        s.req_chronos_infinite = 0;
        if (!DeveloperModeEnabled()) {
            SetError("需要开发者模式才能暂停");
        } else if (!PlayerGateOpen()) {
            SetError("角色不在世界中，已取消");
        } else if (s.ws == 0) {
            SetError("尚未解析到 WorldSettings，请稍后再试");
        } else if (infinite) {
            if (s.infinite) {
                StopPause("已停止无限暂停");
            } else {
                if (WriteWorldDilation(kPausedDilation)) {
                    s.paused = 1; s.infinite = 1; s.remaining = 0.0;
                    SetError("");
                } else {
                    SetError("写入失败（WorldSettings 地址可能已失效）");
                }
            }
        } else {
            if (WriteWorldDilation(kPausedDilation)) {
                s.paused = 1; s.infinite = 0; s.remaining = s.duration;
                SetError("");
            } else {
                SetError("写入失败（WorldSettings 地址可能已失效）");
            }
        }
    }

    // ---- Chronos 定时暂停倒计时 ----
    if (s.paused && !s.infinite) {
        s.remaining -= dt;
        if (s.remaining <= 0.0) {
            StopPause("暂停结束，已恢复");
        }
    }

    // ---- Damage Replay（倍攻连锁反应） ----
    PollCombatState();
    // 逐 tick 错开重放：每个 Game tick 只重放一次，形成链式、不叠加同帧多重伤害。
    if (s.replay_remaining > 0 && s.replay_params_size > 0) {
        s.replaying = 1;
        if (CallProcessEvent(s.replay_object, s.replay_function, s.replay_params.data())) {
            ++s.replay_done;
        } else {
            ++s.fail_count;
        }
        s.replaying = 0;
        --s.replay_remaining;
    }

    // ---- Volleyball / GM 开关 ----
    if (s.req_volleyball) {
        s.req_volleyball = 0;
        s.volleyball_enabled = !s.volleyball_enabled;
        if (s.volleyball_enabled) {
            SetError(g_hook != nullptr
                ? "排球自动赢已开启（hook 待运行时解析 GetRallyState 特征码）"
                : "hook 服务不可用，排球自动赢不可用");
        } else {
            SetError("排球自动赢已关闭");
        }
    }
    if (s.req_gm) {
        s.req_gm = 0;
        s.gm_enabled = !s.gm_enabled;
        if (s.gm_enabled) ApplyGmPatch();
        else SetError("GM 解锁已关闭（补丁已释放需重新加载插件）");
    }

    // ---- 配置落盘 ----
    if (s.req_save) {
        s.req_save = 0;
        if (g_scheduler != nullptr && g_scheduler->schedule != nullptr) {
            AnomalyGenerationHandleV1 task{};
            g_scheduler->schedule(g_scheduler->user, 0, PersistTask, nullptr, &task);
        }
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   StringView("anomaly.ultimate"), StringView("Anomaly Ultimate"),
                   StringView("Anomaly"), StringView("1.0.0"),
                   Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}