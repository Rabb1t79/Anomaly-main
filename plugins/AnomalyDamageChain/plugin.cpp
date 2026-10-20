// Anomaly Damage Chain Reaction
// =============================================================================
// 效果：玩家操控角色命中怪物后，对同一次伤害连续重复判定 N 次（可调），
//       形成「打一下、连续掉血多次」的链式反应。
//
// 机制（只依赖本版游戏实际可用的服务）：
//   anomaly.ue5.process-event  —— 钩 UObject::ProcessEvent，拿到每次 UFunction 调用的
//                                  (object, function, parameters)
//   anomaly.ue5.names          —— 把 function 的 FName 解析成字符串
//   anomaly.ue5.objects/world  —— 诊断用（对象/世界快照）
//   anomaly.core               —— 日志（向 Core 写入本插件状态，便于相互印证）
//   anomaly.storage/scheduler/input/ui —— 配置、任务、热键、界面
//
// 关于 anomaly.nte.combat：它在主菜单时不可用（依赖 nte.player，世界指针为空），
// 进世界后才会就绪。它只提供伤害事件（attacker/victim/数值），没有"重新判定"的入口，
// 所以本插件把它作为可选的状态/印证信号（战斗流=就绪、战斗事件=N），
// 而真正的重放走 ue5.process-event 拿到可再次调用的 (object, function, parameters)。
//
// 关键布局（game dump 5.6.1-new 证据）：
//   UObject::NamePrivate         = +0x18（FName index 在 +0）
//   UObject vtable ProcessEvent  = slot 0x4C (76)
//   UFunction::ParmsSize         = +0xB6 (182)，uint16
//
// 重放：命中后复制参数区（大小取自 UFunction::ParmsSize），每个 Game tick 重放一次，
//       天然错开、不叠加同帧多重伤害；replaying 守卫防止无限递归。
//
// 诊断：界面「捕获诊断」会实时列出实战中经过 ProcessEvent 的函数名与次数，
//       若默认候选名未命中，可从该列表直接读出真实的伤害函数名。

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
constexpr std::uintptr_t kProcessEventVtableIndex = 76;   // 0x4C
constexpr std::uintptr_t kFunctionParmsSizeOffset = 182;  // 0xB6

// ---- 配置 ----
constexpr int kConfVersion = 1;
constexpr const char* kConfFile = "damage-chain.conf";
constexpr const char* kLogFile = "damage-chain-capture.txt";
constexpr std::uint32_t kMaxRepeat = 512;
constexpr std::size_t kMaxParams = 1024;

// 默认候选伤害函数名（命中任意一个即触发）。
// 说明：不同角色/怪物可能是蓝图子类，伤害入口可能是蓝图事件名（如 BPOnTakeDamage）。
constexpr const char* kDefaultDamageNames[] = {
    "OnDamaged",
    "ReceiveDamage",
    "TakeDamage",
    "ApplyDamage",
    "BPOnTakeDamage",
    "ReceiveOnTakeDamage",
    "OnCharacterAttacked",
    "OnCharacterAttackedCallBack",
    "OnHandleDamageInfo",
    "NetMulticast_OnHandleDamageInfo",
    "ClientOnSendHandleDamageInfoToDamaged",
    "CharacterOnDamaged",
    "K2_ApplyDamage",
    "ApplyDamageToTarget",
};

struct CaptureEntry {
    std::uint32_t name_id{};
    std::uint32_t count{};
};

struct PendingReplay {
    std::uintptr_t object{};
    std::uintptr_t function{};
    std::array<std::uint8_t, kMaxParams> params{};
    std::size_t params_size{};
    std::uint32_t remaining{};
    bool valid{};
};

struct State {
    // 配置
    int enabled{1};
    std::uint32_t repeat_count{8};
    int capture_enabled{1};

    // 统计
    std::uint64_t events{};
    std::uint64_t matched{};
    std::uint64_t replayed{};
    std::uint64_t failed{};
    std::uint64_t params_rejected{};

    // 战斗流（anomaly.nte.combat，可选；主菜单时可能未就绪，进世界后可用）
    int combat_available{};
    std::uint64_t combat_events{};
    std::uint64_t combat_cursor{};

    // 匹配名 id 集合（启动时由 names 服务解析）
    std::vector<std::uint32_t> damage_name_ids;
    std::uint32_t resolved_names{};

    // 重放
    PendingReplay pending;
    int replaying{};

    // 捕获统计
    std::unordered_map<std::uint32_t, std::uint32_t> capture_counts;
    std::uint64_t function_cache_size{};

    char status[512]{};
};

State g_state;
std::mutex g_mutex;

const AnomalyUiServiceV1* g_ui{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyInputServiceV1* g_input{};
const AnomalyStorageServiceV1* g_storage{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyUe5ProcessEventServiceV1* g_process_event{};
const AnomalyUe5NamesServiceV1* g_names{};
const AnomalyNteCombatServiceV1* g_combat{};

AnomalyGenerationHandleV1 g_events_handle{};
AnomalyGenerationHandleV1 g_hotkey_toggle{};

// function 指针 -> FName index 缓存；name_id -> 字符串缓存
std::unordered_map<std::uintptr_t, std::uint32_t> g_fn_name_cache;
std::unordered_map<std::uint32_t, std::string> g_name_cache;
std::mutex g_name_mutex;  // 保护 g_name_cache（Game/Render 双线程访问）

void SetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_state.status, sizeof(g_state.status), fmt, ap);
    va_end(ap);
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

// 解析 UFunction 的 FName index（带缓存）。
std::uint32_t FunctionNameId(std::uintptr_t function) {
    if (function < 0x10000) return 0;
    auto it = g_fn_name_cache.find(function);
    if (it != g_fn_name_cache.end()) return it->second;
    std::uint32_t id = 0;
    if (!SafeReadU32(function + kNamePrivateOffset, &id)) id = 0;
    if (g_fn_name_cache.size() < 65536) g_fn_name_cache.emplace(function, id);
    return id;
}

// name_id -> 字符串（带缓存）。
const char* NameOf(std::uint32_t name_id) {
    if (name_id == 0 || g_names == nullptr || g_names->resolve_utf8 == nullptr) return "";
    std::scoped_lock lock(g_name_mutex);
    auto it = g_name_cache.find(name_id);
    if (it != g_name_cache.end()) return it->second.c_str();
    if (g_name_cache.size() > 8192) return "";
    char buf[256]{};
    std::size_t size = sizeof(buf);
    if (g_names->resolve_utf8(g_names->user, name_id, buf, &size).code != ANOMALY_STATUS_V1_OK) {
        g_name_cache.emplace(name_id, std::string());
        return "";
    }
    std::string s(buf);  // 宿主保证 null 结尾
    auto inserted = g_name_cache.emplace(name_id, std::move(s));
    return inserted.first->second.c_str();
}

bool IsDamageName(std::uint32_t name_id) {
    if (name_id == 0) return false;
    for (std::uint32_t id : g_state.damage_name_ids) {
        if (id == name_id) return true;
    }
    return false;
}

// 通过 UObject vtable 槽位调用 ProcessEvent（x64: rcx=this, rdx=Function, r8=Parms）。
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

void CaptureName(std::uint32_t name_id) {
    if (!g_state.capture_enabled || name_id == 0) return;
    auto it = g_state.capture_counts.find(name_id);
    if (it == g_state.capture_counts.end()) {
        if (g_state.capture_counts.size() < 4096) g_state.capture_counts.emplace(name_id, 1);
    } else if (it->second < 0xFFFFFFFFu) {
        ++it->second;
    }
}

void ANOMALY_CALL OnProcessEvent(void*, std::uintptr_t object, std::uintptr_t function,
                                 void* parameters) {
    if (!g_state.enabled) return;
    const std::uint32_t name_id = FunctionNameId(function);

    {
        std::scoped_lock lock(g_mutex);
        ++g_state.events;
        CaptureName(name_id);
    }

    if (!IsDamageName(name_id)) return;
    if (g_state.replaying) return;
    if (g_state.pending.valid) return;  // 已有一条链在跑

    std::uint16_t parms_size = 0;
    if (parameters == nullptr || !SafeReadU16(function + kFunctionParmsSizeOffset, &parms_size) ||
        parms_size == 0 || parms_size > kMaxParams) {
        std::scoped_lock lock(g_mutex);
        ++g_state.params_rejected;
        SetStatus("命中 %s，但参数区不可用（ParmsSize=%u）", NameOf(name_id),
                  static_cast<unsigned>(parms_size));
        return;
    }

    {
        std::scoped_lock lock(g_mutex);
        ++g_state.matched;
        g_state.pending.object = object;
        g_state.pending.function = function;
        g_state.pending.params_size = parms_size;
        std::memcpy(g_state.pending.params.data(), parameters, parms_size);
        g_state.pending.remaining = g_state.repeat_count;
        g_state.pending.valid = true;
        SetStatus("命中 %s：开始重放 %u 次", NameOf(name_id), g_state.repeat_count);
    }

    // 向 Core 写入一条可相互印证的事件
    if (g_core != nullptr && g_core->log != nullptr) {
        char line[192]{};
        std::snprintf(line, sizeof(line), "damage-chain: hit %s, replay %u",
                      NameOf(name_id), g_state.repeat_count);
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(line));
    }
}

// 每 tick 重放一次（错开，避免同帧多重伤害）。
void PumpReplay() {
    PendingReplay local;
    {
        std::scoped_lock lock(g_mutex);
        if (!g_state.pending.valid || g_state.pending.remaining == 0) return;
        local = g_state.pending;
        --g_state.pending.remaining;
        g_state.replaying = 1;
    }
    const bool ok = InvokeProcessEvent(local.object, local.function, local.params.data());
    {
        std::scoped_lock lock(g_mutex);
        g_state.replaying = 0;
        if (ok) {
            ++g_state.replayed;
        } else {
            ++g_state.failed;
            g_state.pending.remaining = 0;
            SetStatus("重放失败（vtable/参数失效）");
        }
        if (g_state.pending.remaining == 0) {
            g_state.pending.valid = false;
            g_state.pending.object = 0;
            g_state.pending.function = 0;
        }
    }
}

// 战斗流：anomaly.nte.combat 在主菜单时未就绪（nte.player 世界指针为空），进世界后可用。
// 这里只做状态回显与计数，用于确认伤害流是否真的在跑。
void PollCombat() {
    if (g_combat == nullptr) {
        g_state.combat_available = 0;
        return;
    }
    if (g_combat->latest_damage_sequence == nullptr || g_combat->next_damage_event == nullptr) {
        g_state.combat_available = 0;
        return;
    }
    const std::uint64_t latest = g_combat->latest_damage_sequence(g_combat->user);
    if (latest == 0) return;
    g_state.combat_available = 1;
    for (int budget = 0; budget < 32; ++budget) {
        AnomalyNteDamageEventV1 ev{};
        ev.struct_size = sizeof(ev);
        const auto status = g_combat->next_damage_event(g_combat->user, g_state.combat_cursor, &ev);
        if (status.code != ANOMALY_STATUS_V1_OK) break;
        g_state.combat_cursor = ev.sequence;
        ++g_state.combat_events;
    }
}

// --------------------------- 配置 ------------------------------------------
std::size_t FormatConfig(char* text, std::size_t capacity) {
    const int n = std::snprintf(text, capacity,
                                "conf_version=%d\nenabled=%d\nrepeat_count=%u\ncapture=%d\n",
                                kConfVersion, g_state.enabled, g_state.repeat_count,
                                g_state.capture_enabled);
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
            ANOMALY_STATUS_V1_OK || size == 0) {
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
            if (std::strcmp(key, "conf_version") == 0) version = std::atoi(value);
            else if (std::strcmp(key, "enabled") == 0) g_state.enabled = std::atoi(value) != 0;
            else if (std::strcmp(key, "repeat_count") == 0)
                g_state.repeat_count = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            else if (std::strcmp(key, "capture") == 0)
                g_state.capture_enabled = std::atoi(value) != 0;
        }
        line = (next != nullptr) ? next + 1 : nullptr;
    }
    if (version != kConfVersion) {
        g_state.enabled = 1;
        g_state.repeat_count = 8;
        g_state.capture_enabled = 1;
    }
    if (g_state.repeat_count > kMaxRepeat) g_state.repeat_count = kMaxRepeat;
}

// --------------------------- 捕获诊断 --------------------------------------
// 注意：框架禁止在 Game/Render 回调里做同步 state I/O
// （scoped_platform_services: "plugin state I/O is unavailable from game or render callbacks"）。
// 导出按钮在 Draw（Render 域）触发，因此必须经 anomaly.scheduler 投递到 worker 线程再写。
std::mutex g_export_mutex;
std::string g_export_text;
bool g_export_pending{};
AnomalyGenerationHandleV1 g_export_task{};

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

void ExportCapture() {
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
        std::scoped_lock lock(g_mutex);
        std::vector<CaptureEntry> rows;
        rows.reserve(g_state.capture_counts.size());
        for (const auto& kv : g_state.capture_counts) rows.push_back({kv.first, kv.second});
        std::sort(rows.begin(), rows.end(),
                  [](const CaptureEntry& a, const CaptureEntry& b) { return a.count > b.count; });
        for (const auto& r : rows) {
            const char* name = NameOf(r.name_id);
            if (name == nullptr || name[0] == '\0') continue;
            char line[320]{};
            std::snprintf(line, sizeof(line), "%-56s %u\n", name, r.count);
            text += line;
        }
    }
    {
        std::scoped_lock lock(g_export_mutex);
        g_export_text.swap(text);
        g_export_pending = true;
    }
    AnomalyGenerationHandleV1 task{};
    const auto status = g_scheduler->schedule(g_scheduler->user, 0, &ExportTaskRun, nullptr, &task);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        g_export_task = task;
        SetStatus("已提交导出任务（%s）", kLogFile);
    } else {
        std::scoped_lock lock(g_export_mutex);
        g_export_pending = false;
        SetStatus("调度导出失败（%u）", status.code);
    }
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

// --------------------------- 绘制 -------------------------------------------
void Text(const AnomalyUiServiceV1* ui, const char* s) {
    if (HAS(ui, text) && s != nullptr) ui->text(ui->user, StringView(s));
}

void DrawMain(const AnomalyUiServiceV1* ui) {
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

    std::snprintf(line, sizeof(line), "Hook=%s 事件=%llu 命中=%llu 重放=%llu 失败=%llu",
                  g_events_handle.id != 0 ? "就绪" : "未就绪",
                  static_cast<unsigned long long>(g_state.events),
                  static_cast<unsigned long long>(g_state.matched),
                  static_cast<unsigned long long>(g_state.replayed),
                  static_cast<unsigned long long>(g_state.failed));
    Text(ui, line);

    std::snprintf(line, sizeof(line), "匹配名=%u/%zu  参数拒绝=%llu",
                  g_state.resolved_names,
                  sizeof(kDefaultDamageNames) / sizeof(kDefaultDamageNames[0]),
                  static_cast<unsigned long long>(g_state.params_rejected));
    Text(ui, line);

    std::snprintf(line, sizeof(line), "战斗流=%s 战斗事件=%llu",
                  g_state.combat_available ? "就绪" : "未就绪",
                  static_cast<unsigned long long>(g_state.combat_events));
    Text(ui, line);

    if (g_state.status[0] != '\0') Text(ui, g_state.status);

    if (HAS(ui, separator)) ui->separator(ui->user);

    if (HAS(ui, checkbox))
        ui->checkbox(ui->user, StringView("捕获诊断（列出实战函数名）"), &g_state.capture_enabled);
    if (HAS(ui, button) && ui->button(ui->user, StringView("导出捕获日志##dc-exp"), 150.0F, 0.0F)) {
        ExportCapture();
    }

    // 列出出现次数最多的函数名（帮助定位真实伤害函数）
    std::vector<CaptureEntry> rows;
    {
        std::scoped_lock lock(g_mutex);
        rows.reserve(g_state.capture_counts.size());
        for (const auto& kv : g_state.capture_counts) rows.push_back({kv.first, kv.second});
    }
    std::sort(rows.begin(), rows.end(),
              [](const CaptureEntry& a, const CaptureEntry& b) { return a.count > b.count; });
    std::size_t shown = 0;
    for (const auto& r : rows) {
        if (shown >= 12) break;
        const char* name = NameOf(r.name_id);
        if (name == nullptr || name[0] == '\0') continue;
        std::snprintf(line, sizeof(line), "%s  (%u)", name, r.count);
        Text(ui, line);
        ++shown;
    }
    if (rows.empty()) Text(ui, "（尚无捕获）");
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
    g_combat = view.Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
                                                     ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).get();

    g_state = State{};
    *context = &g_state;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    LoadConfig();
    RegisterHotkey();

    // 解析候选伤害函数名 -> name_id（find_utf8 是 names v2 字段，需按 struct_size 判断）
    const bool has_find =
        g_names != nullptr &&
        g_names->struct_size >=
            offsetof(AnomalyUe5NamesServiceV1, find_utf8) + sizeof(g_names->find_utf8) &&
        g_names->find_utf8 != nullptr;
    if (has_find) {
        for (const char* name : kDefaultDamageNames) {
            std::uint32_t id = 0;
            if (g_names->find_utf8(g_names->user, StringView(name), &id).code ==
                    ANOMALY_STATUS_V1_OK && id != 0) {
                g_state.damage_name_ids.push_back(id);
                ++g_state.resolved_names;
            }
        }
    }

    if (g_process_event != nullptr && g_process_event->subscribe != nullptr &&
        g_events_handle.id == 0) {
        g_process_event->subscribe(g_process_event->user, &OnProcessEvent, nullptr,
                                   &g_events_handle);
    }

    SetStatus("已加载：匹配名 %u 个，Hook=%s", g_state.resolved_names,
              g_events_handle.id != 0 ? "就绪" : "未就绪");
    if (g_core != nullptr && g_core->log != nullptr) {
        char line[192]{};
        std::snprintf(line, sizeof(line), "damage-chain: start, names=%u hook=%d",
                      g_state.resolved_names, g_events_handle.id != 0 ? 1 : 0);
        g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(line));
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    {
        std::scoped_lock lock(g_mutex);
        g_state.pending.valid = false;
        g_state.pending.remaining = 0;
    }
    if (g_scheduler != nullptr && g_scheduler->cancel != nullptr && g_export_task.id != 0) {
        g_scheduler->cancel(g_scheduler->user, g_export_task);
        g_export_task = AnomalyGenerationHandleV1{};
    }
    {
        std::scoped_lock lock(g_export_mutex);
        g_export_pending = false;
        g_export_text.clear();
    }
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
    if (g_scheduler != nullptr && g_scheduler->cancel != nullptr && g_export_task.id != 0) {
        g_scheduler->cancel(g_scheduler->user, g_export_task);
        g_export_task = AnomalyGenerationHandleV1{};
    }
    {
        std::scoped_lock lock(g_export_mutex);
        g_export_pending = false;
        g_export_text.clear();
    }
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
    g_fn_name_cache.clear();
    g_name_cache.clear();
    g_state = State{};
}

void ANOMALY_CALL Update(void*, double) {
    PollCombat();
    PumpReplay();
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
