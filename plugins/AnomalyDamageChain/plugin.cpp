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
#include <intrin.h>

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
constexpr std::uintptr_t kUStructSuperOffset = 0x40;   // Profile: ustruct.superStruct = 64
constexpr std::uintptr_t kUStructChildrenOffset = 0x48;  // Profile: ustruct.children = 72
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
// FHTDamageEvent 继承链：FDamageEvent 是多态结构（有虚析构与 GetBestHitInfo），
// 因此 +0x00 是 vtable、+0x08 是 TSubclassOf<UDamageType>、+0x10 才是 float Damage。
// 用 dump 字段尺寸反推可对上 DamageGEDef=0x138：
//   0x18 + FHitResult(0x108) = 0x120, +0x0C ShotDirection = 0x12C,
//   +0x08 BoneName = 0x134, 对齐到 0x138 ✓
constexpr std::size_t kDamageEventAmountOffset = 0x10;
constexpr std::size_t kDamageEventHitInfoOffset = 0x18;
constexpr std::size_t kDamageEventHitInfoSize = 0x108;
constexpr std::size_t kDamageEventTagsOffset = 0x148;
constexpr std::size_t kDamageEventTagsSize = 0x20;
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
std::unordered_map<std::uintptr_t, std::uintptr_t> g_class_fn_cache;

// 原生代码扫描（B 方案）：找 HTGame.exe 里 call 到 OnDamaged 原生入口的位置。
std::mutex g_scan_mutex;
bool g_scan_requested{};
bool g_scan_done{};
std::uintptr_t g_scan_target{};
std::uintptr_t g_exec_starts[8]{};
std::uintptr_t g_exec_ends[8]{};
int g_exec_count{};
// 记录 (function name_id -> 首次调用方 rva)
struct StackCandidate {
    std::uintptr_t return_va{};
    std::uintptr_t call_target{};
};

std::unordered_map<std::uint32_t, std::uintptr_t> g_caller_rva;
std::uintptr_t g_process_event_addr{};
bool g_caller_dirty{};
std::vector<StackCandidate> g_caller_candidates;
std::uintptr_t g_scan_hits[16]{};
int g_scan_hit_count{};
int g_scan_scanned_ranges{};
std::uint64_t g_scan_bytes{};
// thunk 内部分解结果：thunk 里每个 call/jmp 目标，以及各自被调用的次数
std::uintptr_t g_scan_targets[8]{};
int g_scan_target_count{};
int g_scan_target_hits[8]{};
std::uintptr_t g_scan_target_first[8][3]{};
AnomalyGenerationHandleV1 g_scan_task{};
bool g_dump_done{};
bool g_scan_logged{};
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

// 兜底：在类对象自身的内存里扫描"像 UField"的指针，再沿 Next 链找目标名字。
// 不依赖 Children/SuperStruct 的具体偏移，能容忍布局变化。
std::uintptr_t ScanClassForFunction(std::uintptr_t cls, const char* wanted) {
    if (cls < 0x10000 || wanted == nullptr) return 0;
    for (std::uintptr_t off = 0; off < 0x600; off += sizeof(void*)) {
        std::uintptr_t cand = 0;
        if (!SafeReadPtr(cls + off, &cand) || cand < 0x10000) continue;
        std::uint32_t id = 0;
        if (!SafeReadU32(cand + kNamePrivateOffset, &id) || id == 0) continue;
        const char* n = ResolveName(id);
        if (n == nullptr || n[0] == '\0') continue;
        if (std::strcmp(n, wanted) == 0) return cand;
        std::uintptr_t cur = cand;
        for (int guard = 0; guard < 4096; ++guard) {
            std::uintptr_t next = 0;
            if (!SafeReadPtr(cur + kUFieldNextOffset, &next) || next < 0x10000) break;
            cur = next;
            std::uint32_t nid = 0;
            if (!SafeReadU32(cur + kNamePrivateOffset, &nid) || nid == 0) continue;
            const char* nn = ResolveName(nid);
            if (nn != nullptr && nn[0] != '\0' && std::strcmp(nn, wanted) == 0) return cur;
        }
    }
    return 0;
}

// 沿 SuperStruct 继承链，在每个类的 Children 链里按名字找 UFunction。
// 必须走继承链：原生函数在父类（AHTAbilityCharacter）上，蓝图子类本身没有。
std::uintptr_t FindFunctionInClass(std::uintptr_t cls, const char* wanted) {
    if (cls < 0x10000 || wanted == nullptr) return 0;
    auto cached = g_class_fn_cache.find(cls);
    if (cached != g_class_fn_cache.end()) return cached->second;
    for (int depth = 0; depth < 64 && cls >= 0x10000; ++depth) {
        std::uintptr_t field = 0;
        if (SafeReadPtr(cls + kUStructChildrenOffset, &field)) {
            for (int guard = 0; guard < 8192 && field >= 0x10000; ++guard) {
                const std::uint32_t name_id = ObjectNameId(field);
                if (name_id != 0) {
                    const char* name = ResolveName(name_id);
                    if (name != nullptr && name[0] != '\0' && std::strcmp(name, wanted) == 0) {
                        return field;
                    }
                }
                std::uintptr_t next = 0;
                if (!SafeReadPtr(field + kUFieldNextOffset, &next)) break;
                field = next;
            }
        }
        const std::uintptr_t scanned = ScanClassForFunction(cls, wanted);
        if (scanned >= 0x10000) {
            if (g_class_fn_cache.size() < 4096) g_class_fn_cache.emplace(cls, scanned);
            return scanned;
        }
        std::uintptr_t super = 0;
        if (!SafeReadPtr(cls + kUStructSuperOffset, &super)) break;
        cls = super;
    }
    return 0;
}

// 日志用：打印整条继承链以及每级的前几个子函数名。
void LogClassChildren(std::uintptr_t cls, char* out, std::size_t cap) {
    out[0] = '\0';
    std::size_t used = 0;
    for (int depth = 0; depth < 6 && cls >= 0x10000; ++depth) {
        const char* cname = ResolveName(ObjectNameId(cls));
        const int w0 = std::snprintf(out + used, cap - used, "%s[%s:", used ? " " : "",
                                     (cname != nullptr && cname[0]) ? cname : "?");
        if (w0 > 0) used += static_cast<std::size_t>(w0);
        if (used + 4 >= cap) break;

        std::uintptr_t field = 0;
        if (SafeReadPtr(cls + kUStructChildrenOffset, &field)) {
            for (int i2 = 0; i2 < 5 && field >= 0x10000; ++i2) {
                const char* n = ResolveName(ObjectNameId(field));
                if (n != nullptr && n[0] != '\0') {
                    const int w = std::snprintf(out + used, cap - used, "%s%s", i2 ? "," : "", n);
                    if (w > 0) used += static_cast<std::size_t>(w);
                    if (used + 3 >= cap) break;
                }
                std::uintptr_t next = 0;
                if (!SafeReadPtr(field + kUFieldNextOffset, &next)) break;
                field = next;
            }
        }
        const int w2 = std::snprintf(out + used, cap - used, "]");
        if (w2 > 0) used += static_cast<std::size_t>(w2);
        if (used + 4 >= cap) break;

        std::uintptr_t super = 0;
        if (!SafeReadPtr(cls + kUStructSuperOffset, &super)) break;
        cls = super;
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

// 解析模块的可执行段范围（读内存里的 PE 头，不走磁盘）。
int CollectExecRanges(std::uintptr_t base, std::uintptr_t* starts, std::uintptr_t* ends,
                      int capacity) {
    if (base < 0x10000 || capacity <= 0) return 0;
    std::uint32_t lfanew = 0;
    if (!SafeReadU32(base + 0x3C, &lfanew) || lfanew == 0) return 0;
    const std::uintptr_t nt = base + lfanew;
    std::uint16_t num_sections = 0;
    std::uint16_t opt_size = 0;
    if (!SafeReadU16(nt + 0x06, &num_sections) || !SafeReadU16(nt + 0x14, &opt_size)) return 0;
    int count = 0;
    const std::uintptr_t sect = nt + 0x18 + opt_size;
    for (int i = 0; i < num_sections && count < capacity; ++i) {
        const std::uintptr_t hdr = sect + static_cast<std::uintptr_t>(i) * 40;
        std::uint32_t vsize = 0;
        std::uint32_t va = 0;
        std::uint32_t chars = 0;
        if (!SafeReadU32(hdr + 8, &vsize) || !SafeReadU32(hdr + 12, &va) ||
            !SafeReadU32(hdr + 36, &chars)) {
            continue;
        }
        if ((chars & 0x20000000u) == 0 || vsize == 0) continue;  // IMAGE_SCN_MEM_EXECUTE
        starts[count] = base + va;
        ends[count] = base + va + vsize;
        ++count;
    }
    return count;
}

// 收集栈上所有"像返回地址"的候选：值在 HTGame 可执行段内，且前 5 字节是 E8。
// 同时读出那条 call 的目标地址，便于判定它调用的是哪个模块。
int CollectStackCandidates(StackCandidate* out, int capacity) {
    if (g_exec_count <= 0 || capacity <= 0) return 0;
    std::uintptr_t* p = reinterpret_cast<std::uintptr_t*>(_AddressOfReturnAddress());
    int count = 0;
    for (int i = 0; i < 1024 && count < capacity; ++i) {
        std::uintptr_t value = 0;
        if (!SafeReadPtr(reinterpret_cast<std::uintptr_t>(p + i), &value)) break;
        if (value < 0x10000) continue;
        bool in_exec = false;
        for (int r = 0; r < g_exec_count; ++r) {
            if (value >= g_exec_starts[r] && value < g_exec_ends[r]) { in_exec = true; break; }
        }
        if (!in_exec) continue;
        std::uint32_t opcode = 0;
        if (!SafeReadU32(value - 5, &opcode) || (opcode & 0xFFu) != 0xE8u) continue;
        std::int32_t rel = 0;
        if (!SafeReadU32(value - 4, reinterpret_cast<std::uint32_t*>(&rel))) continue;
        out[count].return_va = value;
        out[count].call_target = value + static_cast<std::intptr_t>(rel);
        ++count;
    }
    return count;
}

// 块级安全拷贝：一次 SEH 保护拷整块// 块级安全拷贝：一次 SEH 保护拷整块，之后在本地缓冲里紧凑扫描。
bool SafeCopyBlock(std::uintptr_t src, void* dst, std::size_t size) {
    if (dst == nullptr || size == 0) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 在可执行段里找 call rel32 指向 target 的位置（E8 rel32）。
int ScanDirectCallers(std::uintptr_t target, std::uintptr_t* out, int capacity,
                      std::uintptr_t* ranges_scanned, std::uint64_t* bytes_scanned) {
    const HMODULE mod = GetModuleHandleA("HTGame.exe");
    if (mod == nullptr || target < 0x10000) return 0;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
    std::uintptr_t starts[32]{};
    std::uintptr_t ends[32]{};
    const int ranges = CollectExecRanges(base, starts, ends, 32);
    if (ranges_scanned != nullptr) *ranges_scanned = static_cast<std::uintptr_t>(ranges);

    constexpr std::size_t kChunk = 1U << 20;
    std::vector<std::uint8_t> buffer(kChunk + 8);
    int found = 0;
    std::uint64_t total = 0;
    for (int r = 0; r < ranges; ++r) {
        const std::uintptr_t begin = starts[r];
        const std::uintptr_t end = ends[r];
        if (end <= begin) continue;
        total += static_cast<std::uint64_t>(end - begin);
        for (std::uintptr_t off = 0; off < end - begin; off += kChunk) {
            const std::size_t n = static_cast<std::size_t>(
                (end - begin - off) < kChunk ? (end - begin - off) : kChunk);
            if (n < 5) break;
            if (!SafeCopyBlock(begin + off, buffer.data(), n)) continue;
            const std::uint8_t* data = buffer.data();
            for (std::size_t i = 0; i + 5 <= n; ++i) {
                if (data[i] != 0xE8) continue;
                std::int32_t rel = 0;
                std::memcpy(&rel, data + i + 1, sizeof(rel));
                const std::uintptr_t site = begin + off + i;
                if (site + 5 + static_cast<std::intptr_t>(rel) != target) continue;
                if (found < capacity) out[found] = site;
                ++found;
            }
        }
    }
    if (bytes_scanned != nullptr) *bytes_scanned = total;
    return found;
}

// 把模块的可执行段整段写到磁盘，供离线反汇编分析。
void DumpExecSections() {
    const HMODULE mod = GetModuleHandleA("HTGame.exe");
    if (mod == nullptr) return;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
    std::uintptr_t starts[32]{};
    std::uintptr_t ends[32]{};
    const int ranges = CollectExecRanges(base, starts, ends, 32);
    if (ranges <= 0) return;
    char temp[MAX_PATH]{};
    if (GetTempPathA(MAX_PATH, temp) == 0) return;
    constexpr std::size_t kChunk = 1U << 20;
    std::vector<std::uint8_t> buffer(kChunk);
    for (int r = 0; r < ranges; ++r) {
        if (ends[r] <= starts[r]) continue;
        char path[MAX_PATH]{};
        std::snprintf(path, sizeof(path), "%sanomaly_htgame_%d.bin", temp, r);
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        const std::uintptr_t size = ends[r] - starts[r];
        std::uintptr_t written_total = 0;
        for (std::uintptr_t off = 0; off < size; off += kChunk) {
            const std::size_t n = (size - off) < kChunk
                                      ? static_cast<std::size_t>(size - off)
                                      : kChunk;
            if (!SafeCopyBlock(starts[r] + off, buffer.data(), n)) break;
            DWORD written = 0;
            if (!WriteFile(file, buffer.data(), static_cast<DWORD>(n), &written, nullptr) ||
                written != n) {
                break;
            }
            written_total += n;
        }
        CloseHandle(file);
        if (g_core != nullptr && g_core->log != nullptr) {
            char text[320]{};
            std::snprintf(text, sizeof(text),
                          "damage-chain: dump %s rva=0x%llX size=0x%llX written=0x%llX",
                          path, static_cast<unsigned long long>(starts[r] - base),
                          static_cast<unsigned long long>(size),
                          static_cast<unsigned long long>(written_total));
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
        }
    }
}

// worker 线程执行扫描（不冻结游戏）。
void ANOMALY_CALL ScanTaskRun(void*, AnomalyGenerationHandleV1) {
    std::uintptr_t target = 0;
    {
        std::scoped_lock lock(g_scan_mutex);
        target = g_scan_target;
    }
    if (target == 0) return;

    // 先扫直接调用者（上一版已做，通常为 0：该地址是 ProcessEvent 专用 thunk）。
    std::uintptr_t hits[16]{};
    std::uintptr_t ranges = 0;
    std::uint64_t bytes = 0;
    const int count = ScanDirectCallers(target, hits, 16, &ranges, &bytes);

    // 把 thunk 前 0x400 字节写到文件，供本地用真正的反汇编器分析。
    if (g_storage != nullptr && g_storage->write_atomic != nullptr) {
        std::uint8_t dump[0x400]{};
        if (SafeCopyBlock(target, dump, sizeof(dump))) {
            AnomalyByteSpanV1 span{};
            span.data = dump;
            span.size = sizeof(dump);
            g_storage->write_atomic(g_storage->user, StringView("thunk.bin"), span);
        }
    }
    // 再反汇编 thunk 前 0x200 字节，取出它内部的 call/jmp 目标（真正的 C++ 实现）。
    std::uintptr_t calls[8]{};
    int call_count = 0;
    {
        const HMODULE mod = GetModuleHandleA("HTGame.exe");
        const std::uintptr_t mb = reinterpret_cast<std::uintptr_t>(mod);
        std::uint8_t code[0x200]{};
        if (mb != 0 && SafeCopyBlock(target, code, sizeof(code))) {
            for (std::size_t i = 0; i + 5 <= sizeof(code) && call_count < 8; ++i) {
                if (code[i] != 0xE8 && code[i] != 0xE9) continue;
                std::int32_t rel = 0;
                std::memcpy(&rel, code + i + 1, sizeof(rel));
                const std::uintptr_t dest =
                    target + i + 5 + static_cast<std::intptr_t>(rel);
                if (dest < mb || dest >= mb + 0x11000000ULL) continue;
                bool dup = false;
                for (int k = 0; k < call_count; ++k) {
                    if (calls[k] == dest) { dup = true; break; }
                }
                if (!dup) calls[call_count++] = dest;
            }
        }
    }

    // 对每个 thunk 内部目标扫调用者。
    int target_hits[8]{};
    std::uintptr_t target_first[8][3]{};
    for (int t = 0; t < call_count; ++t) {
        std::uintptr_t sub[16]{};
        std::uintptr_t sub_ranges = 0;
        std::uint64_t sub_bytes = 0;
        const int n = ScanDirectCallers(calls[t], sub, 16, &sub_ranges, &sub_bytes);
        target_hits[t] = n;
        for (int k = 0; k < n && k < 3; ++k) target_first[t][k] = sub[k];
    }

    std::scoped_lock lock(g_scan_mutex);
    g_scan_hit_count = count;
    g_scan_scanned_ranges = static_cast<int>(ranges);
    g_scan_bytes = bytes;
    for (int i = 0; i < 16; ++i) g_scan_hits[i] = hits[i];
    g_scan_target_count = call_count;
    for (int t = 0; t < 8; ++t) {
        g_scan_targets[t] = calls[t];
        g_scan_target_hits[t] = target_hits[t];
        for (int k = 0; k < 3; ++k) g_scan_target_first[t][k] = target_first[t][k];
    }
    g_scan_done = true;

    if (!g_dump_done) {
        g_dump_done = true;
        DumpExecSections();
    }
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
    // 埋点：记录是谁（RVA）调用了这个伤害事件 UFunction。
    if (g_exec_count > 0 && g_caller_rva.find(name_id) == g_caller_rva.end()) {
        StackCandidate cands[6]{};
        const int nc = CollectStackCandidates(cands, 6);
        if (nc > 0) {
            g_caller_candidates.clear();
            for (int k = 0; k < nc; ++k) g_caller_candidates.push_back(cands[k]);
            g_caller_rva.emplace(name_id, cands[0].return_va);
            g_caller_dirty = true;
        }
    }
    g_state.pending = PendingReplay{};
    g_state.pending.mode = 2;  // 待 Update 里解析 OnDamaged 的 UFunction
    g_state.pending.raw_victim = victim;
    g_state.pending.raw_instigator = instigator;
    g_state.pending.raw_ge = ge;
    g_state.pending.params_size = kOnDamagedParamsSize;
    g_state.pending.remaining = g_state.repeat_count;
    g_state.pending.valid = true;
    g_state.last_match_name_id = name_id;

    // 直接按 AHTAbilityCharacter::OnDamaged 的参数布局组包（偏移来自 dump）。
    {
        auto* out = g_state.pending.params.data();
        std::memset(out, 0, kOnDamagedParamsSize);
        float amount = 0.0F;
        if (SafeReadU32(base + kDamageEventAmountOffset,
                        reinterpret_cast<std::uint32_t*>(&amount))) {
            g_state.pending.raw_damage = amount;
            std::memcpy(out + kOnDamagedAmountOffset, &amount, sizeof(amount));
        }
        // HitInfo 与 DamageTags 直接从原始伤害事件拷过来，保持这次判定的真实上下文。
        std::memcpy(out + 0x08, reinterpret_cast<const void*>(base + kDamageEventHitInfoOffset),
                    kDamageEventHitInfoSize);
        std::memcpy(out + 0x110, reinterpret_cast<const void*>(base + kDamageEventTagsOffset),
                    kDamageEventTagsSize);
        std::memcpy(out + kOnDamagedGeOffset, &ge, sizeof(ge));
        std::memcpy(out + kOnDamagedInstigatorOffset, &instigator, sizeof(instigator));
        const std::uintptr_t causer = 0;
        std::memcpy(out + kOnDamagedCauserOffset, &causer, sizeof(causer));
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
    if (fn >= 0x10000) {
        std::uintptr_t exec = 0;
        if (SafeReadPtr(fn + kUFunctionExecOffset, &exec) && exec >= 0x10000) {
            bool need = false;
            {
                std::scoped_lock lock(g_scan_mutex);
                if (!g_scan_requested && !g_scan_done) {
                    g_scan_target = exec;
                    g_scan_requested = true;
                    need = true;
                }
            }
            if (need && g_scheduler != nullptr && g_scheduler->schedule != nullptr) {
                AnomalyGenerationHandleV1 task{};
                if (g_scheduler->schedule(g_scheduler->user, 0, &ScanTaskRun, nullptr, &task).code ==
                    ANOMALY_STATUS_V1_OK) {
                    g_scan_task = task;
                } else {
                    std::scoped_lock lock(g_scan_mutex);
                    g_scan_requested = false;
                }
            }
        }
    }
    if (fn >= 0x10000 && g_state.log_matches && g_core != nullptr && g_core->log != nullptr) {
        // 探测：UFunction 上是否存有指向 HTGame.exe 代码段的原生函数指针。
        const HMODULE mod = GetModuleHandleA("HTGame.exe");
        const std::uintptr_t mb = reinterpret_cast<std::uintptr_t>(mod);
        char text[512]{};
        int n = std::snprintf(text, sizeof(text),
                              "damage-chain: OnDamaged ufunc=0x%llX mod=0x%llX",
                              static_cast<unsigned long long>(fn),
                              static_cast<unsigned long long>(mb));
        for (std::uintptr_t off = 0xC0; off <= 0xF8 && n > 0 &&
                                       static_cast<std::size_t>(n) + 32 < sizeof(text);
             off += 8) {
            std::uintptr_t v = 0;
            SafeReadPtr(fn + off, &v);
            const bool in_mod = mb != 0 && v >= mb && v < mb + 0x11000000ULL;
            n += std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n),
                               " +%llX=0x%llX%s", static_cast<unsigned long long>(off),
                               static_cast<unsigned long long>(v), in_mod ? "*" : "");
        }
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
    }
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
    // 参数区已在 OnProcessEvent 里按 dump 布局组好，这里只补 function/object。
    PendingReplay rebuilt = local;
    rebuilt.mode = 1;
    rebuilt.object = local.raw_victim;
    rebuilt.function = fn;
    rebuilt.params_size = kOnDamagedParamsSize;

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
        std::snprintf(msg, sizeof(msg), "damage-chain: match fn=%s ge=%s obj=%s n=%u ps=%zu dmg=%.1f",
                      (fn != nullptr && fn[0]) ? fn : "?", (ge != nullptr && ge[0]) ? ge : "?",
                      (obj != nullptr && obj[0]) ? obj : "?", g_state.repeat_count,
                      local.params_size, local.raw_damage);
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
    if (g_signature != nullptr && g_signature->resolve != nullptr) {
        std::uintptr_t pe = 0;
        if (g_signature
                ->resolve(g_signature->user, StringView("HTGame.exe"), StringView(".text"),
                          StringView("40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00"),
                          &pe)
                .code == ANOMALY_STATUS_V1_OK) {
            g_process_event_addr = pe;
        }
    }
    if (const HMODULE ht = GetModuleHandleA("HTGame.exe")) {
        g_exec_count = CollectExecRanges(reinterpret_cast<std::uintptr_t>(ht), g_exec_starts,
                                         g_exec_ends, 8);
    }

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

    SetStatus("已加载 v2.0.2：候选名 %d 个，Hook=%s", g_state.candidate_count,
              g_events_handle.id != 0 ? "就绪" : "未就绪");
    if (g_core != nullptr && g_core->log != nullptr) {
        char text[192]{};
        std::snprintf(text, sizeof(text), "damage-chain: start v2.0.2, candidates=%d hook=%d np=%d gf=%d gobj=%d oi=%d iv=%d",
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

    // 调用方埋点：有新记录时写一条日志。
    if (g_caller_dirty) {
        g_caller_dirty = false;
        const HMODULE mod = GetModuleHandleA("HTGame.exe");
        const std::uintptr_t mb = reinterpret_cast<std::uintptr_t>(mod);
        char text[1024]{};
        int n = std::snprintf(text, sizeof(text), "damage-chain: callers");
        for (const auto& kv : g_caller_rva) {
            if (n <= 0 || static_cast<std::size_t>(n) + 96 >= sizeof(text)) break;
            const char* name = ResolveName(kv.first);
            n += std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n),
                               " %s@0x%llX", (name != nullptr && name[0]) ? name : "?",
                               static_cast<unsigned long long>(kv.second - mb));
        }
        // 候选栈帧：返回地址 RVA <- 该 call 的目标 RVA（>0x10D43000 表示目标在别处）
        for (const auto& c : g_caller_candidates) {
            if (n <= 0 || static_cast<std::size_t>(n) + 64 >= sizeof(text)) break;
            n += std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n),
                               " [0x%llX<-0x%llX]",
                               static_cast<unsigned long long>(c.return_va - mb),
                               static_cast<unsigned long long>(c.call_target - mb));
        }
        if (g_core != nullptr && g_core->log != nullptr) {
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
        }
    }

    // 扫描完成后把结果写进日志（只写一次）。
    if (!g_scan_logged) {
        std::uintptr_t hits[16]{};
        int count = 0;
        int ranges = 0;
        std::uint64_t bytes = 0;
        std::uintptr_t target = 0;
        {
            std::scoped_lock lock(g_scan_mutex);
            if (g_scan_done) {
                count = g_scan_hit_count;
                ranges = g_scan_scanned_ranges;
                bytes = g_scan_bytes;
                target = g_scan_target;
                for (int i = 0; i < 16; ++i) hits[i] = g_scan_hits[i];
                g_scan_logged = true;
            }
        }
        if (g_scan_logged && g_core != nullptr && g_core->log != nullptr) {
            const HMODULE mod = GetModuleHandleA("HTGame.exe");
            const std::uintptr_t mb = reinterpret_cast<std::uintptr_t>(mod);
            char text[900]{};
            int n = std::snprintf(text, sizeof(text),
                                  "damage-chain: scan thunk=0x%llX ranges=%d bytes=%llu direct=%d",
                                  static_cast<unsigned long long>(target), ranges,
                                  static_cast<unsigned long long>(bytes), count);
            int tcount = 0;
            std::uintptr_t tgt[8]{};
            int thits[8]{};
            std::uintptr_t tfirst[8][3]{};
            {
                std::scoped_lock lock(g_scan_mutex);
                tcount = g_scan_target_count;
                for (int i = 0; i < 8; ++i) {
                    tgt[i] = g_scan_targets[i];
                    thits[i] = g_scan_target_hits[i];
                    for (int k = 0; k < 3; ++k) tfirst[i][k] = g_scan_target_first[i][k];
                }
            }
            for (int t = 0; t < tcount; ++t) {
                if (n <= 0 || static_cast<std::size_t>(n) + 80 >= sizeof(text)) break;
                n += std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n),
                                   " {impl rva=0x%llX n=%d",
                                   static_cast<unsigned long long>(tgt[t] - mb), thits[t]);
                for (int k = 0; k < 3 && thits[t] > k; ++k) {
                    if (static_cast<std::size_t>(n) + 30 >= sizeof(text)) break;
                    n += std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n),
                                       ",0x%llX",
                                       static_cast<unsigned long long>(tfirst[t][k] - mb));
                }
                n += std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n), "}");
            }
            g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
        }
    }

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
                   StringView("2.0.2"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
