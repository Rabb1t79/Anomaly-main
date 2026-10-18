#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/ui.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr std::uintptr_t kApplyDamageOffset = 0x42D5E50;
constexpr std::size_t kHookSize = 14;
constexpr float kMinEchoRate = 1.0F;
constexpr float kMaxEchoRate = 20.0F;

struct AActor;
struct AController;
struct UClass;

using ApplyDamageFn = float (*)(AActor*, float, AController*, AActor*, UClass*);

ApplyDamageFn g_OriginalApplyDamage = nullptr;
thread_local bool g_IsReplaying = false;

struct EchoTask {
    AActor* Target{};
    float Damage{};
    AController* Instigator{};
    AActor* Causer{};
    UClass* DamageTypeClass{};
    std::chrono::steady_clock::time_point NextTriggerTime{};
};

std::mutex g_TaskMutex;
std::vector<EchoTask> g_EchoTasks;
std::atomic<float> g_EchoRate{5.0F};
std::atomic_bool g_PluginEnabled{true};

void* g_Trampoline = nullptr;
void* g_TargetAddr = nullptr;
std::uint8_t g_OriginalBytes[kHookSize]{};
bool g_HookInstalled = false;

#define HAS_UI(ui, field) \
    ((ui) != nullptr && \
     (ui)->struct_size >= offsetof(AnomalyUiServiceV1, field) + sizeof((ui)->field) && \
     (ui)->field != nullptr)

bool InstallHook(void* target, void* detour) {
    if (target == nullptr || detour == nullptr || g_HookInstalled) {
        return false;
    }

    void* trampoline = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (trampoline == nullptr) {
        return false;
    }

    // Keep the original bytes for restoration and construct the trampoline.
    std::memcpy(g_OriginalBytes, target, kHookSize);
    std::memcpy(trampoline, target, kHookSize);

    auto* trampolineEnd = static_cast<std::uint8_t*>(trampoline) + kHookSize;
    trampolineEnd[0] = 0xFF;
    trampolineEnd[1] = 0x25;
    *reinterpret_cast<std::uint32_t*>(trampolineEnd + 2) = 0;
    *reinterpret_cast<std::uint64_t*>(trampolineEnd + 6) =
        reinterpret_cast<std::uint64_t>(target) + kHookSize;

    DWORD oldProtect{};
    if (!VirtualProtect(target, kHookSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        std::memset(g_OriginalBytes, 0, sizeof(g_OriginalBytes));
        return false;
    }

    g_TargetAddr = target;
    g_Trampoline = trampoline;
    g_OriginalApplyDamage = reinterpret_cast<ApplyDamageFn>(trampoline);

    auto* entry = static_cast<std::uint8_t*>(target);
    entry[0] = 0xFF;
    entry[1] = 0x25;
    *reinterpret_cast<std::uint32_t*>(entry + 2) = 0;
    *reinterpret_cast<std::uint64_t*>(entry + 6) =
        reinterpret_cast<std::uint64_t>(detour);

    FlushInstructionCache(GetCurrentProcess(), trampoline, kHookSize + 14);
    FlushInstructionCache(GetCurrentProcess(), target, kHookSize);

    DWORD ignored{};
    VirtualProtect(target, kHookSize, oldProtect, &ignored);
    g_HookInstalled = true;
    return true;
}

void UninstallHook() {
    bool restored = false;

    if (g_TargetAddr != nullptr && g_HookInstalled) {
        DWORD oldProtect{};
        if (VirtualProtect(g_TargetAddr, kHookSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(g_TargetAddr, g_OriginalBytes, kHookSize);
            FlushInstructionCache(GetCurrentProcess(), g_TargetAddr, kHookSize);
            DWORD ignored{};
            restored = VirtualProtect(g_TargetAddr, kHookSize, oldProtect, &ignored) != 0;
            g_HookInstalled = !restored;
        }
    }

    // Do not release the trampoline if restoring the entry failed.
    if (restored || !g_HookInstalled) {
        if (g_Trampoline != nullptr) {
            VirtualFree(g_Trampoline, 0, MEM_RELEASE);
        }
        g_Trampoline = nullptr;
        g_TargetAddr = nullptr;
        g_OriginalApplyDamage = nullptr;
        std::memset(g_OriginalBytes, 0, sizeof(g_OriginalBytes));
    }
}

// Keep SEH in a function without C++ objects that require stack unwinding (MSVC C2712).
bool InvokeOriginalSafely(EchoTask* task) noexcept {
    if (task == nullptr || g_OriginalApplyDamage == nullptr) {
        return false;
    }

    __try {
        g_OriginalApplyDamage(task->Target, task->Damage, task->Instigator,
                              task->Causer, task->DamageTypeClass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

float Hooked_ApplyDamage(AActor* damagedActor, float baseDamage,
                         AController* eventInstigator, AActor* damageCauser,
                         UClass* damageTypeClass) {
    const auto original = g_OriginalApplyDamage;
    if (original == nullptr) {
        return 0.0F;
    }

    const float result =
        original(damagedActor, baseDamage, eventInstigator, damageCauser, damageTypeClass);

    if (g_IsReplaying || !g_PluginEnabled.load(std::memory_order_relaxed)) {
        return result;
    }

    if (baseDamage > 0.0F && eventInstigator != nullptr && damagedActor != nullptr) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_TaskMutex);

        for (auto& task : g_EchoTasks) {
            if (task.Target == damagedActor) {
                task.Damage = baseDamage;
                task.Instigator = eventInstigator;
                task.Causer = damageCauser;
                task.DamageTypeClass = damageTypeClass;
                return result;
            }
        }

        g_EchoTasks.push_back({
            damagedActor,
            baseDamage,
            eventInstigator,
            damageCauser,
            damageTypeClass,
            now
        });
    }

    return result;
}

void UpdateEchoTasks() {
    if (!g_PluginEnabled.load(std::memory_order_relaxed) || g_OriginalApplyDamage == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float configuredRate = g_EchoRate.load(std::memory_order_relaxed);
    const float rate = std::clamp(configuredRate, kMinEchoRate, kMaxEchoRate);
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(rate)));

    std::lock_guard<std::mutex> lock(g_TaskMutex);
    for (auto it = g_EchoTasks.begin(); it != g_EchoTasks.end();) {
        if (it->Target == nullptr || it->Instigator == nullptr) {
            it = g_EchoTasks.erase(it);
            continue;
        }

        if (now >= it->NextTriggerTime) {
            g_IsReplaying = true;
            const bool callSucceeded = InvokeOriginalSafely(&*it);
            g_IsReplaying = false;

            if (!callSucceeded) {
                it = g_EchoTasks.erase(it);
                continue;
            }

            it->NextTriggerTime = now + interval;
        }
        ++it;
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    (void)host;
    if (context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("HTGame.exe"));
    if (base == 0) {
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }

    auto* target = reinterpret_cast<void*>(base + kApplyDamageOffset);
    if (!InstallHook(target, reinterpret_cast<void*>(&Hooked_ApplyDamage))) {
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }

    *context = &g_EchoTasks;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    g_PluginEnabled.store(true, std::memory_order_relaxed);
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    g_PluginEnabled.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_TaskMutex);
    g_EchoTasks.clear();
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

void ANOMALY_CALL Unload(void*) {
    g_PluginEnabled.store(false, std::memory_order_relaxed);
    UninstallHook();

    std::lock_guard<std::mutex> lock(g_TaskMutex);
    g_EchoTasks.clear();
}

void ANOMALY_CALL Update(void*, double) {
    UpdateEchoTasks();
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (!HAS_UI(ui, begin_window) || !HAS_UI(ui, end_window) || !HAS_UI(ui, text)) {
        return;
    }

    if (HAS_UI(ui, set_next_window_size)) {
        // First-use condition used by the Anomaly UI service.
        ui->set_next_window_size(ui->user, 420.0F, 250.0F, 4u);
    }

    if (ui->begin_window(ui->user, anomaly::sdk::StringView("伤害回响 (Damage Echo)"),
                         nullptr, 0u) != 0) {
        int enabled = g_PluginEnabled.load(std::memory_order_relaxed) ? 1 : 0;
        if (HAS_UI(ui, checkbox) &&
            ui->checkbox(ui->user, anomaly::sdk::StringView("启用伤害回响"), &enabled) != 0) {
            g_PluginEnabled.store(enabled != 0, std::memory_order_relaxed);
        }

        if (HAS_UI(ui, separator)) {
            ui->separator(ui->user);
        }

        float rate = g_EchoRate.load(std::memory_order_relaxed);
        if (HAS_UI(ui, slider_float) &&
            ui->slider_float(ui->user, anomaly::sdk::StringView("每秒触发次数"),
                             &rate, kMinEchoRate, kMaxEchoRate) != 0) {
            g_EchoRate.store(std::clamp(rate, kMinEchoRate, kMaxEchoRate),
                             std::memory_order_relaxed);
        }

        char status[128]{};
        std::snprintf(status, sizeof(status), "运行状态：%s",
                      g_PluginEnabled.load(std::memory_order_relaxed) ? "已启用" : "已暂停");
        ui->text(ui->user, anomaly::sdk::StringView(status));

        const float currentRate = g_EchoRate.load(std::memory_order_relaxed);
        std::snprintf(status, sizeof(status), "当前频率：%.1f 次/秒",
                      static_cast<double>(currentRate));
        ui->text(ui->user, anomaly::sdk::StringView(status));

        std::size_t activeTargets{};
        {
            std::lock_guard<std::mutex> lock(g_TaskMutex);
            activeTargets = g_EchoTasks.size();
        }
        std::snprintf(status, sizeof(status), "持续回响目标：%zu", activeTargets);
        ui->text(ui->user, anomaly::sdk::StringView(status));
        ui->text(ui->user, anomaly::sdk::StringView("频率设置按每个已登记目标分别生效。"));
    }

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
        anomaly::sdk::StringView("伤害回响 (Damage Echo)"),
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
