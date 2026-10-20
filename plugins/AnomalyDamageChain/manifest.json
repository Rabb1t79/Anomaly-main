// Anomaly Damage Chain Reaction
// =============================================================================
// 效果：玩家操控角色命中怪物后，对同一次伤害连续重复判定 N 次（可调），
//       形成「打一下、连续掉血多次」的链式反应。
//
// 线程与重入纪律（很关键：上一版在 ProcessEvent 钩子内调用宿主服务，导致游戏卡死）：
//   * ProcessEvent 回调里只允许「读内存 + 查本地缓存」，绝不调用任何宿主服务；
//   * 所有宿主调用（names.resolve_utf8 / storage / scheduler）只在
//     Update（Game 线程、钩子外）或 Start/Stop/Unload（生命周期线程）执行；
//   * Draw（Render 线程）只读 Update 预先构建的快照，不做宿主调用、不做文件 I/O。
//
// 为什么重放走 anomaly.ue5.process-event：anomaly.nte.combat 只给事件数据
// （attacker/victim/数值），不提供「可再次调用」的入口；重放必须拿到
// (object, function, parameters)。
//
// 为什么不用 names.find_utf8：它是 names 服务 v2 才有的字段，本机宿主只有 v1，
// 反查恒失败（运行时日志 names=0）。改为「resolve_utf8 得到名字后与候选表比对」，
// 并把解析延迟到 Update，避开钩子重入。
//
// 关键布局（game dump 5.6.1-new 证据）：
//   UObject::NamePrivate        = +0x18（FName index 在 +0）
//   UObject::ClassPrivate       = +0x10
//   UObject vtable ProcessEvent = slot 0x4C (76)
//   UFunction::ParmsSize        = +0xB6 (182)，uint16
//
// 伤害入口（来自实战 ProcessEvent 捕获，非猜测）：捕获 203 个函数名里，
// 只有 ServerActivateGameEffectsWithClassByActor 是真正施加 GameplayEffect 的入口；
// OnCharacterAttackedCallBack / OnOwningCharacterAttacked / HandleOnHpAttributeChange2 /
// HandlePlayerCharacterHPChanged 都是通知，重放不扣血。捕获里根本没有 OnDamaged /
// ReceiveDamage / TakeDamage，旧候选表永远命中不了。

#include "anomaly/sdk/cpp.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using anomaly::sdk::Host;
using anomaly::sdk::StringView;

#define HAS(ui, field)                                                        \
    ((ui) != nullptr &&                                                       \
     (ui)->struct_size >=                                                     \
         offsetof(AnomalyUiServiceV1, field) + sizeof((ui)->field) &&         \
     (ui)->field != nullptr)

// ---- 布局（dump 证据） ----
constexpr std::uintptr_t kNamePrivateOffset = 0x18;
constexpr std::uintptr_t kClassPrivateOffset = 0x10;
constexpr std::uintptr_t kProcessEventVtableIndex = 76;   // 0x4C
constexpr std::uintptr_t kFunctionParmsSizeOffset = 182;  // 0xB6

// ---- 配置 ----
constexpr int kConfVersion = 8;  // v3: 目标改为技能激活 RPC
constexpr const char* kConfFile = "damage-chain.conf";
constexpr const char* kLogFile = "damage-chain-capture.txt";
constexpr const char* kGeFunctionName = "ServerActivateGameEffectsWithClassByActor";

// 反射遍历：用怪物对象的 Class 找 OnDamaged 这个 UFunction（不依赖 RVA/特征码）。
// Profile layout 证据：object.class=0x10、object.nameOffset=0x18、ufield.next=0x28、
// ufunction.execFunction=0xD8。UStruct::Children 取 UE5 标准 0x38。
constexpr std::uintptr_t kUStructChildrenOffset = 0x38;
constexpr std::uintptr_t kUFieldNextOffset = 0x28;
constexpr std::uintptr_t kUFunctionExecOffset = 216;
constexpr const char* kOnDamagedName = "OnDamaged";

// AHTAbilityCharacter::OnDamaged 参数区（dump: ParmsSize 0x148）
// (float DamageAmount, FHitResult&, FGameplayTagContainer&, UHTGamePlayEffect*, AHTAbilityCharacter*, AActor*)
constexpr std::size_t kOnDamagedParamsSize = 0x148;
constexpr std::size_t kOnDamagedAmountOffset = 0x00;
constexpr std::size_t kOnDamagedGeOffset = 0x130;
constexpr std::size_t kOnDamagedInstigatorOffset = 0x138;
constexpr std::size_t kOnDamagedCauserOffset = 0x140;
// FHTDamageEvent 里 FPointDamageEvent::Damage 位于 +0x08
constexpr std::size_t kDamageEventAmountOffset = 0x08;
constexpr std::uint32_t kMaxRepeat = 512;
constexpr std::size_t kMaxParams = 1024;
constexpr std::size_t kMaxDisplayRows = 12;

// 重放目标：攻击技能激活 RPC。
// 依据：怪物 HP 由服务端计算并复制（OnRep_HPCurrent 为 RepNotify，且游戏连着远程
// 服务器 43.144.211.30:30031 / 113.250.24.242:10012），客户端没有任何 UFunction
// 会施加怪物伤害；唯一能让服务端重新执行一次伤害判定的客户端动作，是重新激活攻击技能。
// 实测 ServerActivateGameEffectsWithClassByActor 只承载 Buff_*（玩家增益），不是伤害链路。
// 伤害检测：这两个函数的参数里带完整 FHTDamageEvent（含 DamageGEDef 与目标角色）。
// dump 证据（5.6.1-new/CppSDK/SDK/HTGame_parameters.hpp:146887）：
//   struct HTGameplayEffectTask_CharacterAttacked_OnCharacterAttackedCallBack {
//       FHTDamageEvent DamageEvent;            // +0x000 (0x170)
//       AHTAbilityCharacter* Instigator;       // +0x170
//       AHTAbilityCharacter* DamagedCharacter; // +0x178
//   };  ParmsSize 0x180
// FHTDamageEvent::DamageGEDef 位于 +0x138（dump 布局）。
constexpr const char* kDefaultDamageNames[] = {
    "OnCharacterAttackedCallBack",
    "OnOwningCharacterAttacked",
};

// 伤害事件参数布局
constexpr std::size_t kDamageEventGeOffset = 0x138;   // FHTDamageEvent::DamageGEDef
constexpr std::size_t kDamageEventInstigatorOffset = 0x170;
constexpr std::size_t kDamageEventVictimOffset = 0x178;

// 施加 GE 的参数布局（ServerActivateGameEffectsWithClassByActor，ParmsSize 0x80）
constexpr std::size_t kApplySourceOffset = 0x00;
constexpr std::size_t kApplyGeOffset = 0x08;
constexpr std::size_t kApplyBufferOffset = 0x10;   // FBufferData(0x58)
constexpr std::size_t kApplyBufferLevelOffset = 0x10 + 0x28;
constexpr std::size_t kApplyBufferDurationOffset = 0x10 + 0x2C;
constexpr std::size_t kApplyBufferStacksOffset = 0x10 + 0x38;
constexpr std::size_t kApplyEventTypeOffset = 0x68;
constexpr std::size_t kApplyParamsSize = 0x80;
constexpr std::size_t kDefaultDamageNameCount =
    sizeof(kDefaultDamageNames) / sizeof(kDefaultDamageNames[0]);

// ServerActivateGameEffectsWithClassByActor 的 GameplayEffect 指针在参数区 +0x08。
// dump: HTGame_parameters.hpp:2485，ParmsSize 0x80。
constexpr std::size_t kGeParamOffset = 0x08;

// name_match 缓存值
enum : std::uint8_t {
    kNamePending = 0,
    kNameMatch = 1,
    kNameNoMatch = 2,
    kNameMatchGe = 3,  // 命中且是 GE 施加入口（需要按 GE 类名过滤）
};

struct PendingReplay {
    std::uintptr_t object{};          // 重放时的 this
    std::uintptr_t function{};        // 重放时调用的 UFunction
    std::array<std::uint8_t, kMaxParams> params{};
    std::size_t params_size{};
    std::uint32_t remaining{};
    std::uint32_t ge_class_name_id{};
    std::uint32_t object_class_name_id{};
    std::uint32_t victim_class_name_id{};
    int mode{};                       // 0=原样重放 1=GE 施加 2=待解析 OnDamaged
    // mode 2 暂存，等 Update 里解析出 OnDamaged 后再组装 params
    std::uintptr_t raw_victim{};
    std::uintptr_t raw_instigator{};
    std::uintptr_t raw_ge{};
    float raw_damage{};
    bool valid{};
};

struct DisplayRow {
    char name[72]{};
    std::uint32_t count{};
};

struct DisplaySnapshot {
    DisplayRow rows[kMaxDisplayRows]{};
    std::size_t row_count{};
    char status[512]{};
    std::uint64_t events{};
    std::uint64_t matched{};
    std::uint64_t replayed{};
    std::uint64_t failed{};
    std::uint64_t params_rejected{};
    std::uint64_t ge_skipped{};
    std::uint64_t combat_events{};
    int combat_available{};
    int hook_ready{};
    int candidate_count{};
    char last_match[72]{};
    char last_ge[72]{};
};

struct State {
    // 配置
    int enabled{1};
    std::uint32_t repeat_count{15};
    int capture_enabled{1};
    int ge_filter{0};
    int log_matches{1};      // 命中时向 Core 写一条诊断（函数名 + GE 类名）
    int zero_prediction{1};  // 重放 GE 时清零 PredictionKey（+0x6C 起 0x10 字节）
    int apply_on_instigator{1};  // 1=在施害者(玩家,客户端拥有)上调用GE施加，SourceObj=怪物
    int interval_ms{120};    // 每次重放的间隔（毫秒），形成可见的连续伤害
    double replay_accum{0.0};

    // 最近一次命中的函数/GE 类名 id（供界面显示）
    std::uint32_t last_match_name_id{};
    std::uint32_t last_ge_name_id{};

    // 统计
    std::uint64_t events{};
    std::uint64_t matched{};
    std::uint64_t replayed{};
    std::uint64_t failed{};
    std::uint64_t params_rejected{};
    std::uint64_t ge_skipped{};
    int candidate_count{};

    // 重放
    PendingReplay pending;
    int replaying{};

    // 名称匹配缓存 + 待解析队列（仅 Game 线程访问）
    std::unordered_map<std::uint32_t, std::uint8_t> name_match;
    std::vector<std::uint32_t> pending_names;

    // 捕获统计（仅 Game 线程访问）
    std::unordered_map<std::uint32_t, std::uint32_t> capture_counts;

    // 战斗流
    int combat_available{};
    std::uint64_t combat_events{};
    std::uint64_t combat_cursor{};

    char status[512]{};
};

State g_state;
std::mutex g_state_mutex;    // 保护 g_state（钩子与 Update/Draw 共享的统计）
std::mutex g_display_mutex;  // 保护 g_display
DisplaySnapshot g_display;

const AnomalyUiServiceV1* g_ui{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyInputServiceV1* g_input{};
const AnomalyStorageServiceV1* g_storage{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyUe5ProcessEventServiceV1* g_process_event{};
const AnomalyUe5NamesServiceV1* g_names{};
const AnomalyNteCombatServiceV1* g_combat{};

std::uintptr_t g_apply_ufunc{};  // ServerActivateGameEffectsWithClassByActor 的 UFunction
const AnomalySignatureServiceV1* g_signature{};
std::uintptr_t g_objects_accessor{};  // ue5.GObjects 访问器地址
std::uintptr_t g_objects_global{};    // 其中 mov rax,[rip+x] 指向的全局变量地址

// Profile 的 ue5.GObjects 特征码：mov rax,[rip+disp32]; mov rcx,[rax+rcx*8];
// mov rax,[rcx+rdx*8]; ret —— 即 TUObjectArray 的 chunk 查找。
constexpr const char* kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3";
AnomalyGenerationHandleV1 g_events_handle{};
AnomalyGenerationHandleV1 g_hotkey_toggle{};

// 仅 Game 线程使用（Update / Start）
std::unordered_map<std::uintptr_t, std::uint32_t> g_fn_name_cache;
std::unordered_map<std::uint32_t, std::string> g_name_cache;

// 导出：Draw 只置标志，Update 构建内容并调度写入
std::mutex g_export_mutex;
std::string g_export_text;
bool g_export_pending{};
bool g_export_requested{};
AnomalyGenerationHandleV1 g_export_task{};

void SetStatus(const char* fmt, ...) {
    char buffer[512]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    std::scoped_lock lock(g_state_mutex);
    std::memcpy(g_state.status, buffer, sizeof(g_state.status));
}

bool SafeReadU32(std::uintptr_t address, std::uint32_t* out) {
    if (out == nullptr || address < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const std::uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadU16(std::uintptr_t address, std::uint16_t* out) {
    if (out == nullptr || address < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const std::uint16_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadPtr(std::uintptr_t address, std::uintptr_t* out) {
    if (out == nullptr || address < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 只读内存，可在钩子内调用。
std::uint32_t FunctionNameId(std::uintptr_t function) {
    if (function < 0x10000) return 0;
    auto it = g_fn_name_cache.find(function);
    if (it != g_fn_name_cache.end()) return it->second;
    std::uint32_t id = 0;
    if (!SafeReadU32(function + kNamePrivateOffset, &id)) id = 0;
    if (g_fn_name_cache.size() < 65536) g_fn_name_cache.emplace(function, id);
    return id;
}

// 只读内存，可在钩子内调用。
std::uint32_t ObjectNameId(std::uintptr_t object) {
    if (object < 0x10000) return 0;
    std::uint32_t id = 0;
    if (!SafeReadU32(object + kNamePrivateOffset, &id)) return 0;
    return id;
}

// 只在 Game 线程（Update / Start）调用；绝不从钩子或渲染线程调用。
const char* ResolveName(std::uint32_t name_id) {
    if (name_id == 0 || g_names == nullptr || g_names->resolve_utf8 == nullptr) return "";
    auto it = g_name_cache.find(name_id);
    if (it != g_name_cache.end()) return it->second.c_str();
    if (g_name_cache.size() > 8192) return "";
    char buf[256]{};
    std::size_t size = sizeof(buf);
    if (g_names->resolve_utf8(g_names->user, name_id, buf, &size).code != ANOMALY_STATUS_V1_OK) {
        g_name_cache.emplace(name_id, std::string());
        return "";
    }
    auto inserted = g_name_cache.emplace(name_id, std::string(buf));
    return inserted.first->second.c_str();
}

// 在给定 UClass 的 Children 链里按名字找 UFunction。
std::uintptr_t FindFunctionInClass(std::uintptr_t cls, const char* wanted) {
    if (cls < 0x10000 || wanted == nullptr) return 0;
    std::uintptr_t field = 0;
    if (!SafeReadPtr(cls + kUStructChildrenOffset, &field)) return 0;
    for (int guard = 0; guard < 8192 && field >= 0x10000; ++guard) {
        const std::uint32_t name_id = ObjectNameId(field);
        if (name_id != 0) {
            const char* name = ResolveName(name_id);
            if (name != nullptr && name[0] != '\0' && std::strcmp(name, wanted) == 0) return field;
        }
        std::uintptr_t next = 0;
        if (!SafeReadPtr(field + kUFieldNextOffset, &next)) break;
        field = next;
    }
    return 0;
}

// 日志用：列出某个类的前几个子函数名，确认遍历是否正确。
void LogClassChildren(std::uintptr_t cls, char* out, std::size_t cap) {
    out[0] = '\0';
    if (cls < 0x10000) return;
    std::uintptr_t field = 0;
    if (!SafeReadPtr(cls + kUStructChildrenOffset, &field)) return;
    std::size_t used = 0;
    for (int i = 0; i < 8 && field >= 0x10000; ++i) {
        const std::uint32_t id = ObjectNameId(field);
        const char* n = id ? ResolveName(id) : "";
        if (n != nullptr && n[0] != '\0') {
            const int w = std::snprintf(out + used, cap - used, "%s%s", used ? "," : "", n);
            if (w > 0) used += static_cast<std::size_t>(w);
            if (used + 2 >= cap) break;
        }
        std::uintptr_t next = 0;
        if (!SafeReadPtr(field + kUFieldNextOffset, &next)) break;
        field = next;
    }
}

bool ContainsIgnoreCase(const char* text, const char* needle) {
    if (text == nullptr || needle == nullptr || *needle == '\0') return false;
    const std::size_t n = std::strlen(needle);
    for (const char* p = text; *p != '\0'; ++p) {
        std::size_t i = 0;
        while (i < n && p[i] != '\0') {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

// TWeakObjectPtr<T> = { int32 ObjectIndex; int32 ObjectSerialNumber }（8 字节，不是指针）。
// 通过 GUObjectArray 的 chunked 数组解析成真实对象指针；序列号不匹配或读取失败返回 0。
std::uintptr_t ResolveWeakObject(std::int32_t index, std::int32_t serial) {
    if (g_objects_global == 0 || index <= 0) return 0;
    std::uintptr_t chunk_array = 0;
    if (!SafeReadPtr(g_objects_global, &chunk_array) || chunk_array < 0x10000) return 0;
    const std::uint32_t chunk_index = static_cast<std::uint32_t>(index) >> 16;
    const std::uint32_t within = static_cast<std::uint32_t>(index) & 0xFFFFu;
    std::uintptr_t chunk = 0;
    if (!SafeReadPtr(chunk_array + static_cast<std::uintptr_t>(chunk_index) * sizeof(void*),
                     &chunk) ||
        chunk < 0x10000) {
        return 0;
    }
    // FUObjectItem: Object(0x00) Flags(0x08) ClusterRootIndex(0x0C) SerialNumber(0x10)，步长 0x18。
    const std::uintptr_t item = chunk + static_cast<std::uintptr_t>(within) * 0x18u;
    std::uintptr_t object = 0;
    if (!SafeReadPtr(item, &object) || object < 0x10000) return 0;
    if (serial != 0) {
        std::uint32_t item_serial = 0;
        if (!SafeReadU32(item + 0x10, &item_serial)) return 0;
        if (static_cast<std::int32_t>(item_serial) != serial) return 0;
    }
    return object;
}

bool InvokeProcessEvent(std::uintptr_t object, std::uintptr_t function, void* parameters) {
    if (object < 0x10000 || function < 0x10000) return false;
    std::uintptr_t vtable = 0;
    if (!SafeReadPtr(object, &vtable) || vtable < 0x10000) return false;
    std::uintptr_t pe = 0;
    if (!SafeReadPtr(vtable + kProcessEventVtableIndex * sizeof(void*), &pe) || pe < 0x10000) {
        return false;
    }
    using ProcessEventFn = void (*)(void*, void*, void*);
    __try {
        reinterpret_cast<ProcessEventFn>(pe)(
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// ProcessEvent 回调：只读内存 + 本地缓存，不调用任何宿主服务。
// ---------------------------------------------------------------------------
void ANOMALY_CALL OnProcessEvent(void*, std::uintptr_t object, std::uintptr_t function,
                                 void* parameters) {
    if (!g_state.enabled) return;
    std::scoped_lock lock(g_state_mutex);
    ++g_state.events;

    const std::uint32_t name_id = FunctionNameId(function);
    if (name_id == 0) return;

    if (g_state.capture_enabled && g_state.capture_counts.size() < 4096) {
        auto it = g_state.capture_counts.find(name_id);
        if (it == g_state.capture_counts.end()) {
            g_state.capture_counts.emplace(name_id, 1);
        } else if (it->second < 0xFFFFFFFFu) {
            ++it->second;
        }
    }

    auto cached = g_state.name_match.find(name_id);
    if (cached == g_state.name_match.end()) {
        // 首次见到：排队等 Update 解析，本事件丢弃（下一次攻击起可匹配）。
        if (g_state.pending_names.size() < 8192) g_state.pending_names.push_back(name_id);
        g_state.name_match.emplace(name_id, kNamePending);
        return;
    }
    // 观察到 GE 施加函数的 UFunction 指针就记下来，供后面把伤害 GE 施加到目标。
    if (cached->second == kNameMatchGe) {
        g_apply_ufunc = function;
        return;
    }
    if (cached->second != kNameMatch) return;
    if (g_state.replaying || g_state.pending.valid) return;
    if (g_state.repeat_count == 0) return;  // 0 次时不要留下永久 pending
    if (g_apply_ufunc == 0) return;         // 还没见过 GE 施加函数，无法重放

    std::uint16_t parms_size = 0;
    if (parameters == nullptr || !SafeReadU16(function + kFunctionParmsSizeOffset, &parms_size) ||
        parms_size < kDamageEventVictimOffset + sizeof(void*) || parms_size > kMaxParams) {
        ++g_state.params_rejected;
        return;
    }

    // 从伤害事件里取：伤害 GE、施害者、受害者。
    const auto base = reinterpret_cast<std::uintptr_t>(parameters);
    std::uint32_t ge_index = 0;
    std::uint32_t ge_serial = 0;
    std::uintptr_t instigator = 0;
    std::uintptr_t victim = 0;
    if (!SafeReadU32(base + kDamageEventGeOffset, &ge_index) ||
        !SafeReadU32(base + kDamageEventGeOffset + 4, &ge_serial) ||
        !SafeReadPtr(base + kDamageEventInstigatorOffset, &instigator) ||
        !SafeReadPtr(base + kDamageEventVictimOffset, &victim) || victim < 0x10000) {
        ++g_state.params_rejected;
        return;
    }
    // DamageGEDef 是 TWeakObjectPtr，必须经 GUObjectArray 解析成真实对象。
    const std::uintptr_t ge = ResolveWeakObject(static_cast<std::int32_t>(ge_index),
                                                static_cast<std::int32_t>(ge_serial));
    if (ge < 0x10000) {
        ++g_state.params_rejected;
        return;
    }

    ++g_state.matched;
    g_state.pending = PendingReplay{};
    g_state.pending.mode = 2;  // 待 Update 里解析 OnDamaged 再组装 params
    g_state.pending.raw_victim = victim;
    g_state.pending.raw_instigator = instigator;
    g_state.pending.raw_ge = ge;
    g_state.pending.remaining = g_state.repeat_count;
    g_state.pending.valid = true;
    g_state.last_match_name_id = name_id;

    float amount = 0.0F;
    if (SafeReadU32(base + kDamageEventAmountOffset,
                    reinterpret_cast<std::uint32_t*>(&amount))) {
        g_state.pending.raw_damage = amount;
    }

    // 记录受害者/GE 类名（只读内存），用于诊断显示。
    {
        std::uintptr_t cls = 0;
        if (SafeReadPtr(victim + kClassPrivateOffset, &cls) && cls >= 0x10000) {
            g_state.pending.object_class_name_id = ObjectNameId(cls);
        }
        std::uintptr_t ge_cls = 0;
        if (SafeReadPtr(ge + kClassPrivateOffset, &ge_cls) && ge_cls >= 0x10000) {
            g_state.pending.ge_class_name_id = ObjectNameId(ge_cls);
            g_state.last_ge_name_id = g_state.pending.ge_class_name_id;
        }
    }
}

// ---------------------------------------------------------------------------
// Update：Game 线程，钩子之外。所有宿主调用集中在这里。
// ---------------------------------------------------------------------------
void ResolvePendingNames() {
    for (std::size_t round = 0; round < 4; ++round) {
        std::vector<std::uint32_t> batch;
        {
            std::scoped_lock lock(g_state_mutex);
            if (g_state.pending_names.empty()) return;
            batch.swap(g_state.pending_names);
        }
        for (const std::uint32_t name_id : batch) {
            const char* name = ResolveName(name_id);
            std::uint8_t result = kNameNoMatch;
            if (name != nullptr && name[0] != '\0') {
                if (std::strcmp(name, kGeFunctionName) == 0) {
                    result = kNameMatchGe;  // GE 施加入口：只记指针，不触发重放
                } else {
                    for (const char* wanted : kDefaultDamageNames) {
                        if (std::strcmp(name, wanted) == 0) {
                            result = kNameMatch;  // 伤害事件：解析并重放
                            break;
                        }
                    }
                }
            }
            std::scoped_lock lock(g_state_mutex);
            g_state.name_match[name_id] = result;
        }
    }
}

// mode 2 -> 用反射把 OnDamaged 找出来，组装它的参数区，转入正式重放。
void ResolvePendingDamage() {
    PendingReplay local;
    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.pending.valid || g_state.pending.mode != 2) return;
        local = g_state.pending;
    }
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(local.raw_victim + kClassPrivateOffset, &cls) || cls < 0x10000) {
        std::scoped_lock lock(g_state_mutex);
        g_state.pending.valid = false;
        ++g_state.params_rejected;
        return;
    }
    const std::uintptr_t fn = FindFunctionInClass(cls, kOnDamagedName);
    if (fn < 0x10000) {
        char children[320]{};
        LogClassChildren(cls, children, sizeof(children));
        SetStatus("未找到 OnDamaged；类子项=%s", children);
        if (g_core != nullptr && g_core->log != nullptr && g_state.log_matches) {
            char text[384]{};
            std::snprintf(text, sizeof(text), "damage-chain: OnDamaged miss, children=%s", children);
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, StringView(text));
        }
        std::scoped_lock lock(g_state_mutex);
        g_state.pending.valid = false;
        ++g_state.failed;
        return;
    }
    // 组装 AHTAbilityCharacter::OnDamaged 的参数区。
    PendingReplay rebuilt = local;
    rebuilt.mode = 1;
    rebuilt.object = local.raw_victim;
    rebuilt.function = fn;
    rebuilt.params_size = kOnDamagedParamsSize;
    rebuilt.ge_class_name_id = local.ge_class_name_id;
    auto* out = rebuilt.params.data();
    std::memset(out, 0, kOnDamagedParamsSize);
    std::memcpy(out + kOnDamagedAmountOffset, &local.raw_damage, sizeof(local.raw_damage));
    std::memcpy(out + kOnDamagedGeOffset, &local.raw_ge, sizeof(local.raw_ge));
    std::memcpy(out + kOnDamagedInstigatorOffset, &local.raw_instigator,
                sizeof(local.raw_instigator));
    const std::uintptr_t causer = 0;
    std::memcpy(out + kOnDamagedCauserOffset, &causer, sizeof(causer));

    const std::uint32_t fn_id = ObjectNameId(fn);
    {
        std::scoped_lock lock(g_state_mutex);
        rebuilt.remaining = g_state.repeat_count;
        g_state.pending = rebuilt;
        g_state.last_match_name_id = fn_id;
    }
    SetStatus("已定位 OnDamaged（rva 待定），伤害=%.1f 开始重放 %u 次",
              local.raw_damage, g_state.repeat_count);
}

// 判断 pending 里的 GE 是否伤害类（Game 线程调用）。
bool PendingGeIsDamage() {
    const std::uint32_t id = g_state.pending.ge_class_name_id;
    if (id == 0) return true;  // 读不到类名就不过滤，交给用户开关决定
    const char* name = ResolveName(id);
    if (name == nullptr || name[0] == '\0') return true;
    return ContainsIgnoreCase(name, "damage") || ContainsIgnoreCase(name, "dmg") ||
           ContainsIgnoreCase(name, "hurt");
}

void PumpReplay() {
    PendingReplay local;
    bool is_first = false;
    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.pending.valid || g_state.pending.remaining == 0) return;
        local = g_state.pending;
        is_first = (g_state.pending.remaining == g_state.repeat_count);
    }
    // 先写诊断（即使随后被过滤，也要看到函数名与 GE 类名）。
    if (is_first && g_state.log_matches && g_core != nullptr && g_core->log != nullptr) {
        const char* fn = ResolveName(FunctionNameId(local.function));
        const char* ge = local.ge_class_name_id ? ResolveName(local.ge_class_name_id) : "";
        const char* obj = local.object_class_name_id ? ResolveName(local.object_class_name_id) : "";
        char msg[320]{};
        std::snprintf(msg, sizeof(msg), "damage-chain: match fn=%s ge=%s obj=%s n=%u ps=%zu",
                      (fn != nullptr && fn[0]) ? fn : "?", (ge != nullptr && ge[0]) ? ge : "?",
                      (obj != nullptr && obj[0]) ? obj : "?", g_state.repeat_count,
                      local.params_size);
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(msg));
    }
    // 宿主调用（ResolveName）必须在锁外，避免持锁等待宿主。
    if (local.ge_class_name_id != 0 && g_state.ge_filter && !PendingGeIsDamage()) {
        std::scoped_lock lock(g_state_mutex);
        ++g_state.ge_skipped;
        g_state.pending.valid = false;
        g_state.pending.remaining = 0;
        return;
    }
    // 可选：清零 PredictionKey（Server RPC 复用同一 key 时服务端可能去重）。
    if (g_state.zero_prediction && local.ge_class_name_id != 0 && local.params_size >= 0x7Cu) {
        std::memset(local.params.data() + 0x6C, 0, 0x10);
    }
    {
        std::scoped_lock lock(g_state_mutex);
        if (!g_state.pending.valid || g_state.pending.remaining == 0) return;
        --g_state.pending.remaining;
        g_state.replaying = 1;
    }
    const bool ok = InvokeProcessEvent(local.object, local.function, local.params.data());
    {
        std::scoped_lock lock(g_state_mutex);
        g_state.replaying = 0;
        if (ok) {
            ++g_state.replayed;
        } else {
            ++g_state.failed;
            g_state.pending.remaining = 0;
        }
        if (g_state.pending.remaining == 0) {
            g_state.pending.valid = false;
            g_state.pending.object = 0;
            g_state.pending.function = 0;
        }
    }
}

// 战斗流：anomaly.nte.combat 在主菜单时未就绪，进世界后可用。只做状态回显与计数。
void PollCombat() {
    if (g_combat == nullptr || g_combat->latest_damage_sequence == nullptr ||
        g_combat->next_damage_event == nullptr) {
        std::scoped_lock lock(g_state_mutex);
        g_state.combat_available = 0;
        return;
    }
    const std::uint64_t latest = g_combat->latest_damage_sequence(g_combat->user);
    if (latest == 0) return;
    std::uint64_t cursor = 0;
    {
        std::scoped_lock lock(g_state_mutex);
        g_state.combat_available = 1;
        cursor = g_state.combat_cursor;
    }
    for (int budget = 0; budget < 32; ++budget) {
        AnomalyNteDamageEventV1 event{};
        event.struct_size = sizeof(event);
        if (g_combat->next_damage_event(g_combat->user, cursor, &event).code !=
            ANOMALY_STATUS_V1_OK) {
            break;
        }
        cursor = event.sequence;
        std::scoped_lock lock(g_state_mutex);
        g_state.combat_cursor = cursor;
        ++g_state.combat_events;
    }
}

// 构建 Draw 用的快照（Game 线程）。
void BuildDisplay() {
    DisplaySnapshot snapshot;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
    std::uint32_t last_match_id = 0;
    std::uint32_t last_ge_id = 0;
    {
        std::scoped_lock lock(g_state_mutex);
        snapshot.events = g_state.events;
        snapshot.matched = g_state.matched;
        snapshot.replayed = g_state.replayed;
        snapshot.failed = g_state.failed;
        snapshot.params_rejected = g_state.params_rejected;
        snapshot.ge_skipped = g_state.ge_skipped;
        snapshot.combat_events = g_state.combat_events;
        snapshot.combat_available = g_state.combat_available;
        snapshot.candidate_count = g_state.candidate_count;
        std::memcpy(snapshot.status, g_state.status, sizeof(snapshot.status));
        last_match_id = g_state.last_match_name_id;
        last_ge_id = g_state.last_ge_name_id;
        rows.reserve(g_state.capture_counts.size());
        for (const auto& kv : g_state.capture_counts) rows.emplace_back(kv.first, kv.second);
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    snapshot.hook_ready = g_events_handle.id != 0 ? 1 : 0;
    for (const auto& row : rows) {
        if (snapshot.row_count >= kMaxDisplayRows) break;
        const char* name = ResolveName(row.first);
        if (name == nullptr || name[0] == '\0') continue;
        DisplayRow& out = snapshot.rows[snapshot.row_count++];
        std::snprintf(out.name, sizeof(out.name), "%s", name);
        out.count = row.second;
    }
    if (last_match_id != 0) {
        const char* name = ResolveName(last_match_id);
        if (name != nullptr) std::snprintf(snapshot.last_match, sizeof(snapshot.last_match), "%s", name);
    }
    if (last_ge_id != 0) {
        const char* name = ResolveName(last_ge_id);
        if (name != nullptr) std::snprintf(snapshot.last_ge, sizeof(snapshot.last_ge), "%s", name);
    }
    std::scoped_lock lock(g_display_mutex);
    g_display = snapshot;
}

// 导出：Update 构建文本并调度到 worker 线程写盘。
void ANOMALY_CALL ExportTaskRun(void*, AnomalyGenerationHandleV1) {
    std::string text;
    {
        std::scoped_lock lock(g_export_mutex);
        if (!g_export_pending) return;
        text.swap(g_export_text);
        g_export_pending = false;
    }
    if (g_storage == nullptr || g_storage->write_atomic == nullptr) return;
    AnomalyByteSpanV1 span{};
    span.data = reinterpret_cast<const std::uint8_t*>(text.data());
    span.size = text.size();
    g_storage->write_atomic(g_storage->user, StringView(kLogFile), span);
}

void ServiceExport() {
    bool requested = false;
    {
        std::scoped_lock lock(g_export_mutex);
        requested = g_export_requested;
        if (requested) g_export_requested = false;
    }
    if (!requested) return;
    if (g_storage == nullptr || g_storage->write_atomic == nullptr) {
        SetStatus("storage 不可用，无法导出");
        return;
    }
    if (g_scheduler == nullptr || g_scheduler->schedule == nullptr) {
        SetStatus("scheduler 不可用，无法导出");
        return;
    }
    std::string text;
    text.reserve(4096);
    text += "Damage Chain Reaction - ProcessEvent capture\n";
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
        {
            std::scoped_lock lock(g_state_mutex);
            rows.reserve(g_state.capture_counts.size());
            for (const auto& kv : g_state.capture_counts) rows.emplace_back(kv.first, kv.second);
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& row : rows) {
            const char* name = ResolveName(row.first);
            if (name == nullptr || name[0] == '\0') continue;
            char line[320]{};
            std::snprintf(line, sizeof(line), "%-56s %u\n", name, row.second);
            text += line;
        }
    }
    {
        std::scoped_lock lock(g_export_mutex);
        g_export_text.swap(text);
        g_export_pending = true;
    }
    AnomalyGenerationHandleV1 task{};
    if (g_scheduler->schedule(g_scheduler->user, 0, &ExportTaskRun, nullptr, &task).code ==
        ANOMALY_STATUS_V1_OK) {
        g_export_task = task;
        SetStatus("已提交导出任务（%s）", kLogFile);
    } else {
        std::scoped_lock lock(g_export_mutex);
        g_export_pending = false;
        SetStatus("调度导出失败");
    }
}

// --------------------------- 配置 ------------------------------------------
std::size_t FormatConfig(char* text, std::size_t capacity) {
    const int n = std::snprintf(
        text, capacity,
        "conf_version=%d\nenabled=%d\nrepeat_count=%u\ncapture=%d\nge_filter=%d\nlog=%d\nzero_pred=%d\non_instigator=%d\ninterval=%d\n",
        kConfVersion, g_state.enabled, g_state.repeat_count, g_state.capture_enabled,
        g_state.ge_filter, g_state.log_matches, g_state.zero_prediction,
        g_state.apply_on_instigator, g_state.interval_ms);
    if (n <= 0) return 0;
    const std::size_t size = static_cast<std::size_t>(n);
    return size < capacity ? size : capacity - 1;
}

void SaveConfig() {
    if (g_storage == nullptr || g_storage->write_atomic == nullptr) return;
    char text[256]{};
    const std::size_t size = FormatConfig(text, sizeof(text));
    AnomalyByteSpanV1 span{};
    span.data = reinterpret_cast<const std::uint8_t*>(text);
    span.size = size;
    g_storage->write_atomic(g_storage->user, StringView(kConfFile), span);
}

void LoadConfig() {
    if (g_storage == nullptr || g_storage->read == nullptr) return;
    char text[256]{};
    AnomalyMutableByteSpanV1 span{};
    span.data = reinterpret_cast<std::uint8_t*>(text);
    span.size = sizeof(text) - 1;
    std::size_t size = span.size;
    if (g_storage->read(g_storage->user, StringView(kConfFile), span, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size == 0) {
        return;
    }
    text[size < sizeof(text) ? size : sizeof(text) - 1] = '\0';
    int version = 0;
    char* line = text;
    while (line != nullptr && *line != '\0') {
        char* next = std::strchr(line, '\n');
        if (next != nullptr) *next = '\0';
        char* eq = std::strchr(line, '=');
        if (eq != nullptr) {
            *eq = '\0';
            const char* key = line;
            const char* value = eq + 1;
            if (std::strcmp(key, "conf_version") == 0) {
                version = std::atoi(value);
            } else if (std::strcmp(key, "enabled") == 0) {
                g_state.enabled = std::atoi(value) != 0;
            } else if (std::strcmp(key, "repeat_count") == 0) {
                g_state.repeat_count = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            } else if (std::strcmp(key, "capture") == 0) {
                g_state.capture_enabled = std::atoi(value) != 0;
            } else if (std::strcmp(key, "ge_filter") == 0) {
                g_state.ge_filter = std::atoi(value) != 0;
            } else if (std::strcmp(key, "log") == 0) {
                g_state.log_matches = std::atoi(value) != 0;
            } else if (std::strcmp(key, "zero_pred") == 0) {
                g_state.zero_prediction = std::atoi(value) != 0;
            } else if (std::strcmp(key, "on_instigator") == 0) {
                g_state.apply_on_instigator = std::atoi(value) != 0;
            } else if (std::strcmp(key, "interval") == 0) {
                g_state.interval_ms = std::atoi(value);
            }
        }
        line = (next != nullptr) ? next + 1 : nullptr;
    }
    if (version != kConfVersion) {
        g_state.enabled = 1;
        g_state.repeat_count = 15;
        g_state.capture_enabled = 1;
        g_state.ge_filter = 0;
        g_state.log_matches = 1;
        g_state.zero_prediction = 1;
    }
    if (g_state.repeat_count > kMaxRepeat) g_state.repeat_count = kMaxRepeat;
    if (g_state.interval_ms < 16) g_state.interval_ms = 16;
    if (g_state.interval_ms > 5000) g_state.interval_ms = 5000;
}

// --------------------------- 热键 -------------------------------------------
void ANOMALY_CALL OnHotkey(void*, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*) {
    g_state.enabled = g_state.enabled ? 0 : 1;
    SetStatus(g_state.enabled ? "已启用" : "已关闭");
}

void RegisterHotkey() {
    if (g_input == nullptr || g_input->register_hotkey == nullptr) return;
    AnomalyHotkeySpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.modifiers = 0;
    spec.virtual_key = 'K';
    spec.flags = 0u;
    spec.id = StringView("damage-chain.toggle");
    g_input->register_hotkey(g_input->user, &spec, &OnHotkey, nullptr, &g_hotkey_toggle);
}

void ReleaseHotkey() {
    if (g_input != nullptr && g_input->release_hotkey != nullptr && g_hotkey_toggle.id != 0) {
        g_input->release_hotkey(g_input->user, g_hotkey_toggle);
    }
    g_hotkey_toggle = AnomalyGenerationHandleV1{};
}

// --------------------------- 绘制（Render 线程，只读快照） -------------------
void Text(const AnomalyUiServiceV1* ui, const char* s) {
    if (HAS(ui, text) && s != nullptr) ui->text(ui->user, StringView(s));
}

void DrawMain(const AnomalyUiServiceV1* ui) {
    DisplaySnapshot snapshot;
    {
        std::scoped_lock lock(g_display_mutex);
        snapshot = g_display;
    }
    char line[320]{};

    if (HAS(ui, checkbox)) ui->checkbox(ui->user, StringView("启用"), &g_state.enabled);

    if (HAS(ui, text)) {
        std::snprintf(line, sizeof(line), "重复次数：%u", g_state.repeat_count);
        Text(ui, line);
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("-##dc-dn"), 24.0F, 0.0F)) {
        if (g_state.repeat_count > 0) --g_state.repeat_count;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("+##dc-up"), 24.0F, 0.0F)) {
        if (g_state.repeat_count < kMaxRepeat) ++g_state.repeat_count;
    }

    if (HAS(ui, text)) {
        std::snprintf(line, sizeof(line), "间隔：%d ms", g_state.interval_ms);
        Text(ui, line);
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("-##iv-dn"), 24.0F, 0.0F)) {
        g_state.interval_ms -= 20;
        if (g_state.interval_ms < 16) g_state.interval_ms = 16;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("+##iv-up"), 24.0F, 0.0F)) {
        g_state.interval_ms += 20;
        if (g_state.interval_ms > 5000) g_state.interval_ms = 5000;
    }

    std::snprintf(line, sizeof(line), "Hook=%s 事件=%llu 命中=%llu 重放=%llu 失败=%llu",
                  snapshot.hook_ready ? "就绪" : "未就绪",
                  static_cast<unsigned long long>(snapshot.events),
                  static_cast<unsigned long long>(snapshot.matched),
                  static_cast<unsigned long long>(snapshot.replayed),
                  static_cast<unsigned long long>(snapshot.failed));
    Text(ui, line);

    std::snprintf(line, sizeof(line), "候选名=%d 参数拒绝=%llu GE跳过=%llu",
                  snapshot.candidate_count,
                  static_cast<unsigned long long>(snapshot.params_rejected),
                  static_cast<unsigned long long>(snapshot.ge_skipped));
    Text(ui, line);

    std::snprintf(line, sizeof(line), "战斗流=%s 战斗事件=%llu",
                  snapshot.combat_available ? "就绪" : "未就绪",
                  static_cast<unsigned long long>(snapshot.combat_events));
    Text(ui, line);

    if (snapshot.last_match[0] != '\0') {
        std::snprintf(line, sizeof(line), "上次命中：%s", snapshot.last_match);
        Text(ui, line);
    }
    if (snapshot.last_ge[0] != '\0') {
        std::snprintf(line, sizeof(line), "GE 类名：%s", snapshot.last_ge);
        Text(ui, line);
    }

    if (HAS(ui, checkbox))
        ui->checkbox(ui->user, StringView("仅重放伤害类 GE"), &g_state.ge_filter);
    if (HAS(ui, checkbox))
        ui->checkbox(ui->user, StringView("清零 PredictionKey"), &g_state.zero_prediction);
    if (HAS(ui, checkbox))
        ui->checkbox(ui->user, StringView("命中写日志"), &g_state.log_matches);
    if (HAS(ui, checkbox))
        ui->checkbox(ui->user, StringView("在玩家(施害者)上施加"), &g_state.apply_on_instigator);

    if (snapshot.status[0] != '\0') Text(ui, snapshot.status);

    if (HAS(ui, separator)) ui->separator(ui->user);

    if (HAS(ui, checkbox))
        ui->checkbox(ui->user, StringView("捕获诊断（列出实战函数名）"), &g_state.capture_enabled);
    if (HAS(ui, button) && ui->button(ui->user, StringView("导出捕获日志##dc-exp"), 150.0F, 0.0F)) {
        std::scoped_lock lock(g_export_mutex);
        g_export_requested = true;
    }

    for (std::size_t i = 0; i < snapshot.row_count; ++i) {
        std::snprintf(line, sizeof(line), "%s  (%u)", snapshot.rows[i].name,
                      snapshot.rows[i].count);
        Text(ui, line);
    }
    if (snapshot.row_count == 0) Text(ui, "（尚无捕获）");
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui_v1) {
    const AnomalyUiServiceV1* ui = ui_v1 != nullptr ? ui_v1 : g_ui;
    if (!HAS(ui, begin_window) || !HAS(ui, end_window)) return;
    int open = 1;
    if (ui->begin_window(ui->user, StringView("Damage Chain Reaction"), &open, 0u) != 0) {
        DrawMain(ui);
    }
    ui->end_window(ui->user);
}

// --------------------------- 生命周期 ---------------------------------------
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
    g_input = view.Query<AnomalyInputServiceV1>(ANOMALY_INPUT_SERVICE_V1_ID,
                                                ANOMALY_INPUT_SERVICE_V1_VERSION).get();
    g_storage = view.Query<AnomalyStorageServiceV1>(ANOMALY_STORAGE_SERVICE_V1_ID,
                                                    ANOMALY_STORAGE_SERVICE_V1_VERSION).get();
    g_scheduler = view.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID,
                                                        ANOMALY_SCHEDULER_SERVICE_V1_VERSION).get();
    g_process_event = view.Query<AnomalyUe5ProcessEventServiceV1>(
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION).get();
    g_names = view.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1u).get();
    g_signature = view.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID,
                                                      ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
    g_combat = view.Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
                                                     ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();

    g_state = State{};
    *context = &g_state;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    LoadConfig();
    RegisterHotkey();

    g_state.candidate_count = static_cast<int>(kDefaultDamageNameCount);

    // 解析 GUObjectArray 全局（用于 TWeakObjectPtr 解析）。
    if (g_signature != nullptr && g_signature->resolve != nullptr) {
        std::uintptr_t addr = 0;
        if (g_signature
                ->resolve(g_signature->user, StringView("HTGame.exe"), StringView(".text"),
                          StringView(kGObjectsPattern), &addr)
                .code == ANOMALY_STATUS_V1_OK &&
            addr >= 0x10000) {
            g_objects_accessor = addr;
            std::uint32_t disp = 0;
            if (SafeReadU32(addr + 3, &disp)) {
                g_objects_global = addr + 7 + static_cast<std::intptr_t>(
                                                    static_cast<std::int32_t>(disp));
            }
        }
    }

    if (g_process_event != nullptr && g_process_event->subscribe != nullptr &&
        g_events_handle.id == 0) {
        g_process_event->subscribe(g_process_event->user, &OnProcessEvent, nullptr,
                                   &g_events_handle);
    }

    SetStatus("已加载 v1.7.1：候选名 %d 个，Hook=%s", g_state.candidate_count,
              g_events_handle.id != 0 ? "就绪" : "未就绪");
    if (g_core != nullptr && g_core->log != nullptr) {
        char text[192]{};
        std::snprintf(text, sizeof(text), "damage-chain: start v1.7.1, candidates=%d hook=%d np=%d gf=%d gobj=%d oi=%d iv=%d",
                      g_state.candidate_count, g_events_handle.id != 0 ? 1 : 0,
                      g_state.zero_prediction, g_state.ge_filter, g_objects_global != 0 ? 1 : 0,
                      g_state.apply_on_instigator, g_state.interval_ms);
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
    }
    return anomaly::sdk::Ok();
}

void CancelExportTask() {
    if (g_scheduler != nullptr && g_scheduler->cancel != nullptr && g_export_task.id != 0) {
        g_scheduler->cancel(g_scheduler->user, g_export_task);
        g_export_task = AnomalyGenerationHandleV1{};
    }
    std::scoped_lock lock(g_export_mutex);
    g_export_pending = false;
    g_export_requested = false;
    g_export_text.clear();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    {
        std::scoped_lock lock(g_state_mutex);
        g_state.pending.valid = false;
        g_state.pending.remaining = 0;
    }
    CancelExportTask();
    if (g_process_event != nullptr && g_process_event->unsubscribe != nullptr &&
        g_events_handle.id != 0) {
        g_process_event->unsubscribe(g_process_event->user, g_events_handle);
    }
    g_events_handle = AnomalyGenerationHandleV1{};
    ReleaseHotkey();
    SaveConfig();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    CancelExportTask();
    if (g_process_event != nullptr && g_process_event->unsubscribe != nullptr &&
        g_events_handle.id != 0) {
        g_process_event->unsubscribe(g_process_event->user, g_events_handle);
    }
    g_events_handle = AnomalyGenerationHandleV1{};
    ReleaseHotkey();
    g_ui = nullptr;
    g_core = nullptr;
    g_input = nullptr;
    g_storage = nullptr;
    g_scheduler = nullptr;
    g_process_event = nullptr;
    g_names = nullptr;
    g_combat = nullptr;
    g_signature = nullptr;
    g_apply_ufunc = 0;
    g_objects_accessor = 0;
    g_objects_global = 0;
    g_fn_name_cache.clear();
    g_name_cache.clear();
    g_state = State{};
}

void ANOMALY_CALL Update(void*, double delta_seconds) {
    ResolvePendingNames();
    ResolvePendingDamage();
    ServiceExport();

    // 按 interval_ms 间隔重放，形成「连续多次」而不是同帧爆发。
    g_state.replay_accum += (delta_seconds > 0.0 ? delta_seconds : 0.0) * 1000.0;
    const double interval = static_cast<double>(g_state.interval_ms > 0 ? g_state.interval_ms : 0);
    if (g_state.replay_accum >= interval) {
        g_state.replay_accum = 0.0;
        PumpReplay();
    }
    PollCombat();

    // 每 ~30 tick 刷新一次显示快照，控制排序/解析开销；首帧也构建一次。
    static std::uint32_t tick = 0;
    ++tick;
    if (tick == 1u || (tick % 30u) == 0u) BuildDisplay();
    if ((tick % 600u) == 0u && g_state.log_matches && g_core != nullptr &&
        g_core->log != nullptr) {
        char text[320]{};
        std::snprintf(text, sizeof(text),
                      "damage-chain: events=%llu matched=%llu replayed=%llu failed=%llu "
                      "ge_skip=%llu params_rej=%llu",
                      static_cast<unsigned long long>(g_state.events),
                      static_cast<unsigned long long>(g_state.matched),
                      static_cast<unsigned long long>(g_state.replayed),
                      static_cast<unsigned long long>(g_state.failed),
                      static_cast<unsigned long long>(g_state.ge_skipped),
                      static_cast<unsigned long long>(g_state.params_rejected));
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   StringView("anomaly.local.damage-chain"),
                   StringView("Damage Chain Reaction"), StringView("Anomaly"),
                   StringView("1.0.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
