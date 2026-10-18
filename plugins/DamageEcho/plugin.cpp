#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/interop.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

constexpr std::uintptr_t kApplyDamageOffset = 0x42D5E50;
constexpr ULONGLONG kEchoIntervalMs = 200;
constexpr std::size_t kMaxTasks = 64;

struct EchoTask {
    void* Target{};
    float Damage{};
    void* Instigator{};
    void* Causer{};
    void* DamageTypeClass{};
    ULONGLONG NextTriggerTime{};
    bool Active{};
};

EchoTask g_EchoTasks[kMaxTasks]{};
CRITICAL_SECTION g_TaskCS{};
bool g_TaskCSInitialized = false;
std::atomic_bool g_PluginEnabled{false};
thread_local bool g_IsReplaying = false;
int g_WindowOpen = 1;

const AnomalyHookServiceV1* g_HookService = nullptr;
AnomalyGenerationHandleV1 g_HookHandle{};
std::uintptr_t g_OriginalAddress = 0;

using ApplyDamageFn = float (ANOMALY_CALL *)(void*, float, void*, void*, void*);

#define HAS_UI(ui, field) \
    ((ui) != nullptr && \
     (ui)->struct_size >= offsetof(AnomalyUiServiceV1, field) + sizeof((ui)->field) && \
     (ui)->field != nullptr)

bool HookServiceReady(const AnomalyHookServiceV1* service) noexcept {
    constexpr std::size_t requiredSize =
        offsetof(AnomalyHookServiceV1, end_callback) +
        sizeof(decltype(AnomalyHookServiceV1::end_callback));

    return service != nullptr &&
           service->struct_size >= requiredSize &&
           service->service_version >= ANOMALY_HOOK_SERVICE_V1_VERSION &&
           service->create != nullptr &&
           service->release != nullptr &&
           service->begin_callback != nullptr &&
           service->end_callback != nullptr;
}

class HookCallbackLease final {
public:
    HookCallbackLease() noexcept {
        const auto* service = g_HookService;
        if (service == nullptr ||
            g_HookHandle.id == 0 ||
            g_HookHandle.generation == 0 ||
            service->begin_callback == nullptr ||
            service->end_callback == nullptr) {
            return;
        }

        const auto status =
            service->begin_callback(service->user, g_HookHandle, &lease_);
        if (status.code == ANOMALY_STATUS_V1_OK && lease_.id != 0) {
            service_ = service;
            acquired_ = true;
        }
    }

    ~HookCallbackLease() {
        if (acquired_ && service_ != nullptr) {
            (void)service_->end_callback(service_->user, lease_);
        }
    }

    HookCallbackLease(const HookCallbackLease&) = delete;
    HookCallbackLease& operator=(const HookCallbackLease&) = delete;

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    const AnomalyHookServiceV1* service_{};
    AnomalyGenerationHandleV1 lease_{};
    bool acquired_{};
};

// Keep SEH separate from functions that hold C++ objects (avoids MSVC C2712).
bool InvokeOriginalSafely(
    const ApplyDamageFn original,
    void* target,
    const float damage,
    void* instigator,
    void* causer,
    void* damageTypeClass,
    float* result) noexcept {
    if (original == nullptr || result == nullptr) {
        return false;
    }

    __try {
        *result = original(target, damage, instigator, causer, damageTypeClass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

void ClearTasks() noexcept {
    if (!g_TaskCSInitialized) {
        return;
    }

    EnterCriticalSection(&g_TaskCS);
    for (auto& task : g_EchoTasks) {
        task.Active = false;
    }
    LeaveCriticalSection(&g_TaskCS);
}

float ANOMALY_CALL Hooked_ApplyDamage(
    void* damagedActor,
    float baseDamage,
    void* eventInstigator,
    void* damageCauser,
    void* damageTypeClass) {
    // The official hook service fills the original address and handle before
    // enabling the detour. Acquire the callback lease before plugin-side work.
    HookCallbackLease callbackLease;
    const auto original = reinterpret_cast<ApplyDamageFn>(g_OriginalAddress);
    if (original == nullptr) {
        return 0.0F;
    }

    float result{};
    if (!InvokeOriginalSafely(original, damagedActor, baseDamage, eventInstigator,
                              damageCauser, damageTypeClass, &result)) {
        return 0.0F;
    }

    // If shutdown has started, still pass through the original call but don't
    // access task state unless the host granted the callback lease.
    if (!callbackLease.acquired() ||
        g_IsReplaying ||
        !g_PluginEnabled.load(std::memory_order_relaxed) ||
        baseDamage <= 0.0F ||
        eventInstigator == nullptr ||
        damagedActor == nullptr ||
        !g_TaskCSInitialized) {
        return result;
    }

    const ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_TaskCS);

    bool found = false;
    for (auto& task : g_EchoTasks) {
        if (task.Active && task.Target == damagedActor) {
            task.Damage = baseDamage;
            task.Instigator = eventInstigator;
            task.Causer = damageCauser;
            task.DamageTypeClass = damageTypeClass;
            found = true;
            break;
        }
    }

    if (!found) {
        for (auto& task : g_EchoTasks) {
            if (!task.Active) {
                task = {damagedActor, baseDamage, eventInstigator, damageCauser,
                        damageTypeClass, now, true};
                break;
            }
        }
    }

    LeaveCriticalSection(&g_TaskCS);
    return result;
}

void UpdateEchoTasks() {
    if (!g_PluginEnabled.load(std::memory_order_relaxed) ||
        g_OriginalAddress == 0 ||
        !g_TaskCSInitialized) {
        return;
    }

    const ULONGLONG now = GetTickCount64();

    for (std::size_t i = 0; i < kMaxTasks; ++i) {
        EchoTask snapshot{};
        bool shouldTrigger = false;

        // Don't hold the task lock while invoking game code: damage callbacks
        // may enter the detour concurrently.
        EnterCriticalSection(&g_TaskCS);
        auto& task = g_EchoTasks[i];
        if (task.Active && now >= task.NextTriggerTime) {
            if (task.Target == nullptr || task.Instigator == nullptr) {
                task.Active = false;
            } else {
                snapshot = task;
                task.NextTriggerTime = now + kEchoIntervalMs;
                shouldTrigger = true;
            }
        }
        LeaveCriticalSection(&g_TaskCS);

        if (!shouldTrigger ||
            !g_PluginEnabled.load(std::memory_order_relaxed)) {
            continue;
        }

        float ignoredResult{};
        g_IsReplaying = true;
        const bool succeeded = InvokeOriginalSafely(
            reinterpret_cast<ApplyDamageFn>(g_OriginalAddress),
            snapshot.Target,
            snapshot.Damage,
            snapshot.Instigator,
            snapshot.Causer,
            snapshot.DamageTypeClass,
            &ignoredResult);
        g_IsReplaying = false;

        if (!succeeded) {
            EnterCriticalSection(&g_TaskCS);
            auto& current = g_EchoTasks[i];
            if (current.Active &&
                current.Target == snapshot.Target &&
                current.Instigator == snapshot.Instigator) {
                current.Active = false;
            }
            LeaveCriticalSection(&g_TaskCS);
        }
    }
}

void CleanupFailedLoad() noexcept {
    g_PluginEnabled.store(false, std::memory_order_relaxed);
    g_OriginalAddress = 0;
    g_HookHandle = {};
    g_HookService = nullptr;

    if (g_TaskCSInitialized) {
        DeleteCriticalSection(&g_TaskCS);
        g_TaskCSInitialized = false;
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;

    // IMPORTANT: create() enables the detour before returning, so initialize
    // synchronization and all callback-visible state before requesting a hook.
    if (!InitializeCriticalSectionEx(&g_TaskCS, 0, 0)) {
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }
    g_TaskCSInitialized = true;

    anomaly::sdk::Host hostApi(host);
    const auto hookRef = hostApi.Query<AnomalyHookServiceV1>(
        ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION);
    if (!hookRef || !HookServiceReady(hookRef.get())) {
        CleanupFailedLoad();
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_HookService = hookRef.get();

    const auto base = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleA("HTGame.exe"));
    if (base == 0) {
        CleanupFailedLoad();
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }

    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = base + kApplyDamageOffset;
    request.detour = reinterpret_cast<void*>(&Hooked_ApplyDamage);
    request.label = anomaly::sdk::StringView("ApplyDamageHook");

    // Output storage is persistent global storage, never stack-local. The
    // service populates it before enabling the hook.
    g_OriginalAddress = 0;
    g_HookHandle = {};
    const auto status = g_HookService->create(
        g_HookService->user, &request, &g_OriginalAddress, &g_HookHandle);

    if (status.code != ANOMALY_STATUS_V1_OK ||
        g_OriginalAddress == 0 ||
        g_HookHandle.id == 0 ||
        g_HookHandle.generation == 0) {
        if (g_HookHandle.id != 0 && g_HookService != nullptr) {
            (void)g_HookService->release(g_HookService->user, g_HookHandle);
        }
        CleanupFailedLoad();
        return status.code == ANOMALY_STATUS_V1_OK
            ? AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}}
            : status;
    }

    *context = &g_TaskCS;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    g_PluginEnabled.store(true, std::memory_order_relaxed);
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    g_PluginEnabled.store(false, std::memory_order_relaxed);
    ClearTasks();
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

void ANOMALY_CALL Unload(void*) {
    g_PluginEnabled.store(false, std::memory_order_relaxed);

    bool released = (g_HookHandle.id == 0);
    if (g_HookService != nullptr && g_HookHandle.id != 0) {
        const auto status =
            g_HookService->release(g_HookService->user, g_HookHandle);
        released = status.code == ANOMALY_STATUS_V1_OK;
    }

    ClearTasks();

    // Keep the synchronization object alive if the host reports that callbacks
    // did not drain. Do not clear callback-visible state in that case.
    if (!released) {
        return;
    }

    g_HookHandle = {};
    g_HookService = nullptr;
    g_OriginalAddress = 0;

    if (g_TaskCSInitialized) {
        DeleteCriticalSection(&g_TaskCS);
        g_TaskCSInitialized = false;
    }
}

void ANOMALY_CALL Update(void*, double) {
    UpdateEchoTasks();
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (!HAS_UI(ui, begin_window) ||
        !HAS_UI(ui, end_window) ||
        !HAS_UI(ui, text)) {
        return;
    }

    if (HAS_UI(ui, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 420.0F, 170.0F, 4u);
    }

    {
        anomaly::sdk::UiWindow window(
            ui, "伤害回响 (Damage Echo)", &g_WindowOpen);
        if (!window) {
            return;
        }

        int enabled = g_PluginEnabled.load(std::memory_order_relaxed) ? 1 : 0;
        if (HAS_UI(ui, checkbox) &&
            ui->checkbox(ui->user,
                         anomaly::sdk::StringView("启用伤害回响"),
                         &enabled) != 0) {
            g_PluginEnabled.store(enabled != 0, std::memory_order_relaxed);
        }

        if (HAS_UI(ui, separator)) {
            ui->separator(ui->user);
        }

        ui->text(ui->user, anomaly::sdk::StringView("连击间隔：200 ms"));

        std::size_t activeCount = 0;
        if (g_TaskCSInitialized) {
            EnterCriticalSection(&g_TaskCS);
            for (const auto& task : g_EchoTasks) {
                if (task.Active) {
                    ++activeCount;
                }
            }
            LeaveCriticalSection(&g_TaskCS);
        }

        char status[96]{};
        std::snprintf(status, sizeof(status), "当前回响目标：%zu", activeCount);
        ui->text(ui->user, anomaly::sdk::StringView(status));

        std::snprintf(status, sizeof(status), "运行状态：%s",
                      g_PluginEnabled.load(std::memory_order_relaxed)
                          ? "已启用" : "已暂停");
        ui->text(ui->user, anomaly::sdk::StringView(status));
    }
}

using HookCreateSignature = AnomalyStatusV1 (ANOMALY_CALL *)(
    void*, const AnomalyHookRequestV1*, std::uintptr_t*,
    AnomalyGenerationHandleV1*);
using HookReleaseSignature = AnomalyStatusV1 (ANOMALY_CALL *)(
    void*, AnomalyGenerationHandleV1);
using HookBeginCallbackSignature = AnomalyStatusV1 (ANOMALY_CALL *)(
    void*, AnomalyGenerationHandleV1, AnomalyGenerationHandleV1*);
using HookEndCallbackSignature = AnomalyStatusV1 (ANOMALY_CALL *)(
    void*, AnomalyGenerationHandleV1);
using UiBeginWindowSignature = int (ANOMALY_CALL *)(
    void*, AnomalyStringViewV1, int*, std::uint32_t);

static_assert(std::is_same_v<decltype(AnomalyHookServiceV1::create),
                             HookCreateSignature>);
static_assert(std::is_same_v<decltype(AnomalyHookServiceV1::release),
                             HookReleaseSignature>);
static_assert(std::is_same_v<decltype(AnomalyHookServiceV1::begin_callback),
                             HookBeginCallbackSignature>);
static_assert(std::is_same_v<decltype(AnomalyHookServiceV1::end_callback),
                             HookEndCallbackSignature>);
static_assert(std::is_same_v<decltype(AnomalyUiServiceV1::begin_window),
                             UiBeginWindowSignature>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_load),
                             decltype(&Load)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_start),
                             decltype(&Start)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_stop),
                             decltype(&Stop)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_unload),
                             decltype(&Unload)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_update),
                             decltype(&Update)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_draw),
                             decltype(&Draw)>);

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr ||
        descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }

    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("com.rabb1t79.damage-echo"),
        anomaly::sdk::StringView("Damage Echo (Native)"),
        anomaly::sdk::StringView("Rabb1t79"),
        anomaly::sdk::StringView("1.3.1"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw
    };
    return anomaly::sdk::Ok();
}
