#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/ui.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

namespace {

constexpr std::uintptr_t APPLY_DAMAGE_OFFSET = 0x42D5E50;
constexpr std::size_t kHookSize = 14;
constexpr float kMinEchoRate = 1.0F;
constexpr float kMaxEchoRate = 20.0F;

struct AActor;
struct AController;
struct UClass;

using ApplyDamageFn = float (*)(AActor*, float, AController*, AActor*, UClass*);

ApplyDamageFn g_OriginalApplyDamage = nullptr;
void* g_HookTarget = nullptr;
void* g_Trampoline = nullptr;
std::uint8_t g_OriginalBytes[kHookSize]{};
std::atomic_bool g_HookInstalled{false};
std::atomic_bool g_PluginEnabled{true};
std::atomic<float> g_EchoRate{5.0F};
thread_local bool g_IsReplaying = false;

struct EchoTask {
    AActor* target{};
    float damage{};
    AController* instigator{};
    AActor* causer{};
    UClass* damage_type_class{};
    std::chrono::steady_clock::time_point next_trigger_time{};
};

std::mutex g_TaskMutex;
std::vector<EchoTask> g_EchoTasks;

constexpr bool HasUiField(const AnomalyUiServiceV1* ui, std::size_t offset,
                          std::size_t field_size) noexcept {
    return ui != nullptr && ui->struct_size >= offset + field_size;
}

#define UI_HAS(ui, field) \
    (HasUiField((ui), offsetof(AnomalyUiServiceV1, field), sizeof((ui)->field)) && \
     (ui)->field != nullptr)

bool WriteAbsoluteJump(void* destination, const void* target) noexcept {
    if (destination == nullptr || target == nullptr) return false;
    auto* bytes = static_cast<std::uint8_t*>(destination);
    bytes[0] = 0xFF;
    bytes[1] = 0x25;
    *reinterpret_cast<std::uint32_t*>(bytes + 2) = 0;
    *reinterpret_cast<std::uint64_t*>(bytes + 6) =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(target));
    return true;
}

bool InstallHook(void* target, void* detour) noexcept {
    if (target == nullptr || detour == nullptr) return false;
    if (g_HookInstalled.load(std::memory_order_acquire)) return true;

    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) return false;

    std::memcpy(g_OriginalBytes, target, kHookSize);
    std::memcpy(trampoline, target, kHookSize);
    if (!WriteAbsoluteJump(trampoline + kHookSize,
                           static_cast<std::uint8_t*>(target) + kHookSize)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kHookSize, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    const bool jump_written = WriteAbsoluteJump(target, detour);
    FlushInstructionCache(GetCurrentProcess(), target, kHookSize);

    DWORD ignored = 0;
    VirtualProtect(target, kHookSize, old_protect, &ignored);
    if (!jump_written) {
        DWORD restore_protect = 0;
        if (VirtualProtect(target, kHookSize, PAGE_EXECUTE_READWRITE, &restore_protect)) {
            std::memcpy(target, g_OriginalBytes, kHookSize);
            FlushInstructionCache(GetCurrentProcess(), target, kHookSize);
            VirtualProtect(target, kHookSize, restore_protect, &ignored);
        }
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    g_HookTarget = target;
    g_Trampoline = trampoline;
    g_OriginalApplyDamage = reinterpret_cast<ApplyDamageFn>(trampoline);
    g_HookInstalled.store(true, std::memory_order_release);
    return true;
}

void RemoveHook() noexcept {
    if (!g_HookInstalled.exchange(false, std::memory_order_acq_rel)) return;

    if (g_HookTarget != nullptr) {
        DWORD old_protect = 0;
        if (VirtualProtect(g_HookTarget, kHookSize, PAGE_EXECUTE_READWRITE, &old_protect)) {
            std::memcpy(g_HookTarget, g_OriginalBytes, kHookSize);
            FlushInstructionCache(GetCurrentProcess(), g_HookTarget, kHookSize);
            DWORD ignored = 0;
            VirtualProtect(g_HookTarget, kHookSize, old_protect, &ignored);
        }
    }

    if (g_Trampoline != nullptr) {
        VirtualFree(g_Trampoline, 0, MEM_RELEASE);
    }
    g_HookTarget = nullptr;
    g_Trampoline = nullptr;
    g_OriginalApplyDamage = nullptr;
}

float CallOriginalGuarded(AActor* target, float damage, AController* instigator,
                          AActor* causer, UClass* damage_type_class,
                          bool* raised_exception) noexcept {
    if (raised_exception != nullptr) *raised_exception = false;
    if (g_OriginalApplyDamage == nullptr) return 0.0F;

    // Keep SEH in a leaf wrapper so MSVC can compile the surrounding STL code.
    __try {
        return g_OriginalApplyDamage(target, damage, instigator, causer, damage_type_class);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (raised_exception != nullptr) *raised_exception = true;
        return 0.0F;
    }
}

float Hooked_ApplyDamage(AActor* damaged_actor, float base_damage,
                         AController* event_instigator, AActor* damage_causer,
                         UClass* damage_type_class) {
    const float result = CallOriginalGuarded(damaged_actor, base_damage, event_instigator,
                                             damage_causer, damage_type_class, nullptr);

    if (g_IsReplaying || !g_PluginEnabled.load(std::memory_order_relaxed)) {
        return result;
    }

    if (damaged_actor == nullptr || event_instigator == nullptr ||
        !(base_damage > 0.0F)) {
        return result;
    }

    std::lock_guard<std::mutex> lock(g_TaskMutex);
    for (auto& task : g_EchoTasks) {
        if (task.target == damaged_actor) {
            task.damage = base_damage;
            task.instigator = event_instigator;
            task.causer = damage_causer;
            task.damage_type_class = damage_type_class;
            return result;
        }
    }

    g_EchoTasks.push_back(EchoTask{
        damaged_actor,
        base_damage,
        event_instigator,
        damage_causer,
        damage_type_class,
        std::chrono::steady_clock::now()
    });
    return result;
}

void UpdateEchoTasks() {
    if (!g_PluginEnabled.load(std::memory_order_relaxed) ||
        g_OriginalApplyDamage == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float rate = (g_EchoRate.load(std::memory_order_relaxed) >= kMinEchoRate)
        ? g_EchoRate.load(std::memory_order_relaxed) : kMinEchoRate;
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(rate)));

    std::lock_guard<std::mutex> lock(g_TaskMutex);
    for (auto it = g_EchoTasks.begin(); it != g_EchoTasks.end();) {
        if (it->target == nullptr || it->instigator == nullptr) {
            it = g_EchoTasks.erase(it);
            continue;
        }

        if (now >= it->next_trigger_time) {
            g_IsReplaying = true;
            bool raised_exception = false;
            (void)CallOriginalGuarded(it->target, it->damage, it->instigator,
                                      it->causer, it->damage_type_class,
                                      &raised_exception);
            g_IsReplaying = false;

            if (raised_exception) {
                it = g_EchoTasks.erase(it);
                continue;
            }
            it->next_trigger_time = now + interval;
        }
        ++it;
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr || host == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;

    // Require the UI service up front so the plugin never loads without a usable control panel.
    const anomaly::sdk::Host host_view(host);
    const auto ui_ref = host_view.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    const auto* ui = ui_ref.get();
    if (!UI_HAS(ui, begin_window) || !UI_HAS(ui, end_window) ||
        !UI_HAS(ui, text) || !UI_HAS(ui, checkbox) ||
        !UI_HAS(ui, slider_float) || !UI_HAS(ui, button)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    const auto module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("HTGame.exe"));
    if (module_base == 0) {
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }

    g_HookTarget = reinterpret_cast<void*>(module_base + APPLY_DAMAGE_OFFSET);
    *context = &g_EchoTasks;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    (void)context;
    if (g_HookInstalled.load(std::memory_order_acquire)) {
        g_PluginEnabled.store(true, std::memory_order_release);
        return anomaly::sdk::Ok();
    }

    if (g_HookTarget == nullptr) {
        const auto module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("HTGame.exe"));
        if (module_base == 0) return {ANOMALY_STATUS_V1_FAILED, 0, {}};
        g_HookTarget = reinterpret_cast<void*>(module_base + APPLY_DAMAGE_OFFSET);
    }

    if (!InstallHook(g_HookTarget, reinterpret_cast<void*>(&Hooked_ApplyDamage))) {
        g_HookTarget = nullptr;
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }

    g_PluginEnabled.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t deadline_milliseconds) {
    (void)context;
    (void)deadline_milliseconds;
    g_PluginEnabled.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_TaskMutex);
    g_EchoTasks.clear();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* context) {
    (void)context;
    g_PluginEnabled.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_TaskMutex);
        g_EchoTasks.clear();
    }
    RemoveHook();
}

void ANOMALY_CALL Update(void* context, double delta_seconds) {
    (void)context;
    (void)delta_seconds;
    UpdateEchoTasks();
}

void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui) {
    (void)context;
    if (!UI_HAS(ui, begin_window) || !UI_HAS(ui, end_window) ||
        !UI_HAS(ui, text)) {
        return;
    }

    if (UI_HAS(ui, set_next_window_size_constraints)) {
        ui->set_next_window_size_constraints(ui->user, 420.0F, 310.0F, 900.0F, 900.0F);
    }
    if (UI_HAS(ui, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 520.0F, 390.0F, 4u);
    }

    int open = 1;
    const int visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView("伤害回响"), &open, 0u);
    if (visible != 0) {
        ui->text(ui->user, anomaly::sdk::StringView("对首次命中的目标建立独立回响任务。"));
        ui->text(ui->user, anomaly::sdk::StringView("运行模式：持续触发；关闭插件后停止并清空任务。"));

        if (UI_HAS(ui, separator)) ui->separator(ui->user);

        int enabled = g_PluginEnabled.load(std::memory_order_relaxed) ? 1 : 0;
        if (UI_HAS(ui, checkbox) &&
            ui->checkbox(ui->user, anomaly::sdk::StringView("启用伤害回响"), &enabled) != 0) {
            g_PluginEnabled.store(enabled != 0, std::memory_order_release);
            if (enabled == 0) {
                std::lock_guard<std::mutex> lock(g_TaskMutex);
                g_EchoTasks.clear();
            }
        }

        float rate = g_EchoRate.load(std::memory_order_relaxed);
        if (UI_HAS(ui, slider_float) &&
            ui->slider_float(ui->user, anomaly::sdk::StringView("每秒触发次数"),
                             &rate, kMinEchoRate, kMaxEchoRate) != 0) {
            if (rate < kMinEchoRate) rate = kMinEchoRate;
            if (rate > kMaxEchoRate) rate = kMaxEchoRate;
            g_EchoRate.store(rate, std::memory_order_relaxed);
        }

        if (UI_HAS(ui, separator)) ui->separator(ui->user);

        std::size_t task_count = 0;
        {
            std::lock_guard<std::mutex> lock(g_TaskMutex);
            task_count = g_EchoTasks.size();
        }
        ui->text(ui->user, anomaly::sdk::StringView(
            enabled != 0 ? "状态：已启用" : "状态：已暂停"));
        char rate_line[96]{};
        std::snprintf(rate_line, sizeof(rate_line), "当前频率：%.1f 次/秒", 
                      static_cast<double>(g_EchoRate.load(std::memory_order_relaxed)));
        ui->text(ui->user, anomaly::sdk::StringView(rate_line));
        char tasks_line[96]{};
        std::snprintf(tasks_line, sizeof(tasks_line), "活动目标任务：%zu", task_count);
        ui->text(ui->user, anomaly::sdk::StringView(tasks_line));

        if (UI_HAS(ui, button) &&
            ui->button(ui->user, anomaly::sdk::StringView("清空所有回响任务"), 0.0F, 0.0F) != 0) {
            std::lock_guard<std::mutex> lock(g_TaskMutex);
            g_EchoTasks.clear();
        }
    }
    // The UI ABI requires end_window even when begin_window returns zero.
    ui->end_window(ui->user);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }

    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("com.rabb1t79.damage-echo"),
        anomaly::sdk::StringView("伤害回响"),
        anomaly::sdk::StringView("Rabb1t79"),
        anomaly::sdk::StringView("1.0.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw
    };
    return anomaly::sdk::Ok();
}
