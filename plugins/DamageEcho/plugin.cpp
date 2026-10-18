#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ue5.h"

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
#include <cstring>
#include <iterator>
#include <type_traits>

namespace {

constexpr std::uintptr_t kApplyDamageOffset = 0x42D5E50; // Supplied candidate; Dump/signature identity is not yet verified.
constexpr std::uint32_t kDefaultEchoIntervalMs = 200;
constexpr std::uint32_t kMinEchoIntervalMs = 50;
constexpr std::uint32_t kMaxEchoIntervalMs = 2000;
constexpr std::size_t kMaxTasks = 64;
constexpr std::size_t kLogCapacity = 64;
constexpr std::size_t kLogLineSize = 256;

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
std::atomic_bool g_TaskCSInitialized{false};
std::atomic_bool g_PluginEnabled{false};
std::atomic<std::uint32_t> g_EchoIntervalMs{kDefaultEchoIntervalMs};
std::atomic<std::uint64_t> g_TotalCapturedTargets{0};
std::atomic<std::uint64_t> g_TotalReplays{0};
std::atomic<std::uint64_t> g_TaskOverflowCount{0};
std::atomic_bool g_OffsetPageChecked{false};
std::atomic_bool g_LastUpdateOnGameThread{false};
DWORD g_ReplayTlsIndex = TLS_OUT_OF_INDEXES;
int g_WindowOpen = 1;
const AnomalyUe5FrameworkServiceV1* g_FrameworkService = nullptr;
char g_LogRing[kLogCapacity][kLogLineSize]{};
std::size_t g_LogWriteIndex = 0;
std::size_t g_LogCount = 0;
char g_LastStatus[192] = "等待玩家攻击";
char g_LastExportPathUtf8[512]{};

bool IsReplayingOnCurrentThread() noexcept {
    return g_ReplayTlsIndex != TLS_OUT_OF_INDEXES &&
           TlsGetValue(g_ReplayTlsIndex) != nullptr;
}

void AppendLogLine(const char* line) noexcept {
    if (line == nullptr || !g_TaskCSInitialized.load(std::memory_order_acquire)) {
        return;
    }
    EnterCriticalSection(&g_TaskCS);
    (void)strncpy_s(g_LogRing[g_LogWriteIndex], kLogLineSize, line, _TRUNCATE);
    g_LogWriteIndex = (g_LogWriteIndex + 1) % kLogCapacity;
    if (g_LogCount < kLogCapacity) ++g_LogCount;
    LeaveCriticalSection(&g_TaskCS);
}

void SetStatusText(const char* status) noexcept {
    if (status == nullptr || !g_TaskCSInitialized.load(std::memory_order_acquire)) {
        return;
    }
    EnterCriticalSection(&g_TaskCS);
    (void)strncpy_s(g_LastStatus, sizeof(g_LastStatus), status, _TRUNCATE);
    LeaveCriticalSection(&g_TaskCS);
}

void LogDamageEvent(const char* eventName, void* target, float damage) noexcept {
    char line[kLogLineSize]{};
    (void)std::snprintf(
        line, sizeof(line), "[%llu] %s target=%p damage=%.3f",
        static_cast<unsigned long long>(GetTickCount64()),
        eventName != nullptr ? eventName : "event",
        target, static_cast<double>(damage));
    AppendLogLine(line);
}

bool IsGameThread() noexcept {
    const auto* service = g_FrameworkService;
    constexpr std::size_t requiredSize =
        offsetof(AnomalyUe5FrameworkServiceV1, is_game_thread) +
        sizeof(decltype(AnomalyUe5FrameworkServiceV1::is_game_thread));
    return service != nullptr &&
           service->struct_size >= requiredSize &&
           service->is_game_thread != nullptr &&
           service->is_game_thread(service->user) != 0;
}

class ReplayScope final {
public:
    ReplayScope() noexcept {
        if (g_ReplayTlsIndex == TLS_OUT_OF_INDEXES) {
            return;
        }
        index_ = g_ReplayTlsIndex;
        previous_ = TlsGetValue(index_);
        active_ = TlsSetValue(index_, reinterpret_cast<void*>(static_cast<std::uintptr_t>(1))) != 0;
    }

    ~ReplayScope() {
        if (active_ && index_ != TLS_OUT_OF_INDEXES) {
            (void)TlsSetValue(index_, previous_);
        }
    }

    ReplayScope(const ReplayScope&) = delete;
    ReplayScope& operator=(const ReplayScope&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    DWORD index_{TLS_OUT_OF_INDEXES};
    void* previous_{};
    bool active_{};
};

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

bool FrameworkServiceReady(const AnomalyUe5FrameworkServiceV1* service) noexcept {
    constexpr std::size_t requiredSize =
        offsetof(AnomalyUe5FrameworkServiceV1, is_game_thread) +
        sizeof(decltype(AnomalyUe5FrameworkServiceV1::is_game_thread));
    return service != nullptr &&
           service->struct_size >= requiredSize &&
           service->service_version >= ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION &&
           service->is_game_thread != nullptr;
}

bool IsExecutableImageAddress(std::uintptr_t address, HMODULE module) noexcept {
    if (address == 0 || module == nullptr) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.AllocationBase != module || info.State != MEM_COMMIT || info.Type != MEM_IMAGE) {
        return false;
    }
    if ((info.Protect & PAGE_GUARD) != 0) return false;
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
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
    if (!g_TaskCSInitialized.load(std::memory_order_acquire)) {
        return;
    }

    EnterCriticalSection(&g_TaskCS);
    for (auto& task : g_EchoTasks) {
        task.Active = false;
    }
    LeaveCriticalSection(&g_TaskCS);
}

bool ExportDiagnosticLog() noexcept {
    char snapshot[kLogCapacity][kLogLineSize]{};
    std::size_t count = 0;
    if (!g_TaskCSInitialized.load(std::memory_order_acquire)) return false;

    EnterCriticalSection(&g_TaskCS);
    count = g_LogCount;
    const std::size_t first = (g_LogWriteIndex + kLogCapacity - count) % kLogCapacity;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t slot = (first + i) % kLogCapacity;
        (void)strncpy_s(snapshot[i], kLogLineSize, g_LogRing[slot], _TRUNCATE);
    }
    LeaveCriticalSection(&g_TaskCS);

    wchar_t tempPath[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (tempLength == 0 || tempLength >= std::size(tempPath)) {
        SetStatusText("日志导出失败：无法获取临时目录");
        return false;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t outputPath[MAX_PATH]{};
    const int pathLength = swprintf_s(
        outputPath, std::size(outputPath),
        L"%lsAnomaly-DamageChain-%04u%02u%02u-%02u%02u%02u.log",
        tempPath, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    if (pathLength <= 0 || static_cast<std::size_t>(pathLength) >= std::size(outputPath)) {
        SetStatusText("日志导出失败：输出路径过长");
        return false;
    }

    HANDLE file = CreateFileW(outputPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        char status[192]{};
        (void)std::snprintf(status, sizeof(status), "日志导出失败：Win32=%lu",
                            static_cast<unsigned long>(GetLastError()));
        SetStatusText(status);
        return false;
    }

    const char header[] =
        "Damage Chain diagnostic log\r\n"
        "Note: HTGame offset 0x42D5E50 has only a memory-page check; function identity/signature are not Dump-verified.\r\n";
    DWORD written = 0;
    bool ok = WriteFile(file, header, static_cast<DWORD>(sizeof(header) - 1), &written, nullptr) != 0 &&
              written == sizeof(header) - 1;
    for (std::size_t i = 0; ok && i < count; ++i) {
        const DWORD length = static_cast<DWORD>(std::strlen(snapshot[i]));
        ok = WriteFile(file, snapshot[i], length, &written, nullptr) != 0 && written == length;
        if (ok) {
            constexpr char newline[] = "\r\n";
            ok = WriteFile(file, newline, 2, &written, nullptr) != 0 && written == 2;
        }
    }
    const DWORD closeError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        char status[192]{};
        (void)std::snprintf(status, sizeof(status), "日志导出失败：Win32=%lu",
                            static_cast<unsigned long>(closeError));
        SetStatusText(status);
        return false;
    }

    (void)WideCharToMultiByte(CP_UTF8, 0, outputPath, -1,
                              g_LastExportPathUtf8,
                              static_cast<int>(sizeof(g_LastExportPathUtf8)), nullptr, nullptr);
    SetStatusText("诊断日志已导出到系统临时目录");
    AppendLogLine("[ui] diagnostic log exported");
    return true;
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
        IsReplayingOnCurrentThread() ||
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
    bool addedTask = false;
    bool taskTableFull = false;
    for (auto& task : g_EchoTasks) {
        if (task.Active && task.Target == damagedActor) {
            // Keep this target's cadence: a new hit updates damage, never resets
            // NextTriggerTime. Targets are kept in separate task slots.
            task.Damage = baseDamage;
            found = true;
            break;
        }
    }

    if (!found) {
        for (auto& task : g_EchoTasks) {
            if (!task.Active) {
                task = {damagedActor, baseDamage, eventInstigator, damageCauser,
                        damageTypeClass, now, true};
                addedTask = true;
                break;
            }
        }
        taskTableFull = !addedTask;
    }

    LeaveCriticalSection(&g_TaskCS);
    if (addedTask) {
        g_TotalCapturedTargets.fetch_add(1, std::memory_order_relaxed);
        LogDamageEvent("target-locked", damagedActor, baseDamage);
    } else if (found) {
        LogDamageEvent("same-target-hit-cadence-kept", damagedActor, baseDamage);
    } else if (taskTableFull) {
        g_TaskOverflowCount.fetch_add(1, std::memory_order_relaxed);
        AppendLogLine("[warn] target slots full; new target was not registered");
    }
    return result;
}

void UpdateEchoTasks() {
    if (!g_PluginEnabled.load(std::memory_order_relaxed) ||
        g_OriginalAddress == 0 ||
        !g_TaskCSInitialized.load(std::memory_order_acquire)) {
        return;
    }

    // Repeat damage only on the framework-confirmed Game thread. If the host
    // does not report the Game thread, skip this update rather than invoke game code.
    const bool gameThread = IsGameThread();
    g_LastUpdateOnGameThread.store(gameThread, std::memory_order_relaxed);
    if (!gameThread) return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG intervalMs = g_EchoIntervalMs.load(std::memory_order_relaxed);

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
                task.NextTriggerTime = now + intervalMs;
                shouldTrigger = true;
            }
        }
        LeaveCriticalSection(&g_TaskCS);

        if (!shouldTrigger ||
            !g_PluginEnabled.load(std::memory_order_relaxed)) {
            continue;
        }

        float ignoredResult{};
        bool succeeded = false;
        {
            ReplayScope replayScope;
            if (!replayScope.active()) {
                // Dynamic TLS is unavailable on this thread: skip this tick
                // rather than call damage without recursion suppression.
                continue;
            }
            succeeded = InvokeOriginalSafely(
                reinterpret_cast<ApplyDamageFn>(g_OriginalAddress),
                snapshot.Target,
                snapshot.Damage,
                snapshot.Instigator,
                snapshot.Causer,
                snapshot.DamageTypeClass,
                &ignoredResult);
        }

        if (succeeded) {
            g_TotalReplays.fetch_add(1, std::memory_order_relaxed);
        } else {
            EnterCriticalSection(&g_TaskCS);
            auto& current = g_EchoTasks[i];
            if (current.Active &&
                current.Target == snapshot.Target &&
                current.Instigator == snapshot.Instigator) {
                current.Active = false;
            }
            LeaveCriticalSection(&g_TaskCS);
            AppendLogLine("[error] original damage call faulted; target task removed");
        }
    }
}

void CleanupFailedLoad() noexcept {
    g_PluginEnabled.store(false, std::memory_order_relaxed);
    g_OriginalAddress = 0;
    g_HookHandle = {};
    g_HookService = nullptr;
    g_FrameworkService = nullptr;
    g_OffsetPageChecked.store(false, std::memory_order_relaxed);

    if (g_ReplayTlsIndex != TLS_OUT_OF_INDEXES) {
        (void)TlsFree(g_ReplayTlsIndex);
        g_ReplayTlsIndex = TLS_OUT_OF_INDEXES;
    }
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

    // Use Windows dynamic TLS, not C++ thread_local: the host rejects images
    // that require loader-managed static TLS in a plugin DLL.
    g_ReplayTlsIndex = TlsAlloc();
    if (g_ReplayTlsIndex == TLS_OUT_OF_INDEXES) {
        CleanupFailedLoad();
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }

    anomaly::sdk::Host hostApi(host);
    const auto hookRef = hostApi.Query<AnomalyHookServiceV1>(
        ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION);
    if (!hookRef || !HookServiceReady(hookRef.get())) {
        CleanupFailedLoad();
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_HookService = hookRef.get();

    const auto frameworkRef = hostApi.Query<AnomalyUe5FrameworkServiceV1>(
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION);
    if (!frameworkRef || !FrameworkServiceReady(frameworkRef.get())) {
        CleanupFailedLoad();
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_FrameworkService = frameworkRef.get();

    const auto module = GetModuleHandleA("HTGame.exe");
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    if (base == 0) {
        CleanupFailedLoad();
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }
    const std::uintptr_t targetAddress = base + kApplyDamageOffset;
    if (!IsExecutableImageAddress(targetAddress, module)) {
        CleanupFailedLoad();
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0,
                anomaly::sdk::StringView("damage hook offset is not in executable HTGame image memory")};
    }
    // This is only an executable-image-page sanity check. It does not verify
    // the function identity or ABI signature against the Dump.
    g_OffsetPageChecked.store(true, std::memory_order_relaxed);

    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = targetAddress;
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
    SetStatusText("已加载：目标地址仅通过可执行页检查，函数签名仍待 Dump 核验");
    AppendLogLine("[load] Hook registered; function identity/signature is not Dump-verified");
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    ClearTasks();
    g_TotalCapturedTargets.store(0, std::memory_order_relaxed);
    g_TotalReplays.store(0, std::memory_order_relaxed);
    g_TaskOverflowCount.store(0, std::memory_order_relaxed);
    g_PluginEnabled.store(true, std::memory_order_relaxed);
    SetStatusText("已启用：等待玩家命中目标");
    AppendLogLine("[lifecycle] started; waiting for player damage");
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    g_PluginEnabled.store(false, std::memory_order_relaxed);
    ClearTasks();
    SetStatusText("已暂停：链式目标已清除");
    AppendLogLine("[lifecycle] stopped; all target tasks cleared");
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
    g_FrameworkService = nullptr;
    g_OriginalAddress = 0;

    if (g_ReplayTlsIndex != TLS_OUT_OF_INDEXES) {
        (void)TlsFree(g_ReplayTlsIndex);
        g_ReplayTlsIndex = TLS_OUT_OF_INDEXES;
    }
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
        ui->set_next_window_size(ui->user, 500.0F, 330.0F, 4u);
    }

    {
        anomaly::sdk::UiWindow window(
            ui, "伤害链式反应 (实验)", &g_WindowOpen);
        if (!window) return;

        ui->text(ui->user, anomaly::sdk::StringView(
            "策略：每个目标独立；同一目标再次受伤只更新伤害，不重置计时。"));
        ui->text(ui->user, anomaly::sdk::StringView(
            "注意：当前入口使用给定偏移；可执行页检查不代表函数签名已由 Dump 证实。"));

        int enabled = g_PluginEnabled.load(std::memory_order_relaxed) ? 1 : 0;
        if (HAS_UI(ui, checkbox) &&
            ui->checkbox(ui->user,
                         anomaly::sdk::StringView("启用链式伤害"),
                         &enabled) != 0) {
            const bool wasEnabled = g_PluginEnabled.exchange(
                enabled != 0, std::memory_order_relaxed);
            if (wasEnabled != (enabled != 0)) {
                if (enabled == 0) {
                    ClearTasks();
                    SetStatusText("已暂停：链式目标已清除");
                    AppendLogLine("[ui] disabled; active targets cleared");
                } else {
                    SetStatusText("已启用：等待玩家命中目标");
                    AppendLogLine("[ui] enabled; waiting for player damage");
                }
            }
        }

        float interval = static_cast<float>(
            g_EchoIntervalMs.load(std::memory_order_relaxed));
        if (HAS_UI(ui, slider_float) &&
            ui->slider_float(ui->user,
                             anomaly::sdk::StringView("触发间隔 (毫秒)"),
                             &interval,
                             static_cast<float>(kMinEchoIntervalMs),
                             static_cast<float>(kMaxEchoIntervalMs)) != 0) {
            if (interval < static_cast<float>(kMinEchoIntervalMs)) {
                interval = static_cast<float>(kMinEchoIntervalMs);
            }
            if (interval > static_cast<float>(kMaxEchoIntervalMs)) {
                interval = static_cast<float>(kMaxEchoIntervalMs);
            }
            g_EchoIntervalMs.store(
                static_cast<std::uint32_t>(interval + 0.5F),
                std::memory_order_relaxed);
        }

        if (HAS_UI(ui, separator)) ui->separator(ui->user);

        std::size_t activeCount = 0;
        char status[kLogLineSize]{};
        if (g_TaskCSInitialized.load(std::memory_order_acquire)) {
            EnterCriticalSection(&g_TaskCS);
            for (const auto& task : g_EchoTasks) {
                if (task.Active) ++activeCount;
            }
            (void)strncpy_s(status, sizeof(status), g_LastStatus, _TRUNCATE);
            LeaveCriticalSection(&g_TaskCS);
        }

        char line[192]{};
        (void)std::snprintf(line, sizeof(line), "当前独立目标：%zu / %zu",
                            activeCount, kMaxTasks);
        ui->text(ui->user, anomaly::sdk::StringView(line));

        (void)std::snprintf(line, sizeof(line), "已登记目标总数：%llu",
                            static_cast<unsigned long long>(
                                g_TotalCapturedTargets.load(std::memory_order_relaxed)));
        ui->text(ui->user, anomaly::sdk::StringView(line));

        (void)std::snprintf(line, sizeof(line), "已触发次数：%llu",
                            static_cast<unsigned long long>(
                                g_TotalReplays.load(std::memory_order_relaxed)));
        ui->text(ui->user, anomaly::sdk::StringView(line));

        (void)std::snprintf(line, sizeof(line), "目标槽位满时丢弃数：%llu",
                            static_cast<unsigned long long>(
                                g_TaskOverflowCount.load(std::memory_order_relaxed)));
        ui->text(ui->user, anomaly::sdk::StringView(line));

        ui->text(ui->user, anomaly::sdk::StringView("运行状态："));
        ui->text(ui->user, anomaly::sdk::StringView(status));

        ui->text(ui->user, anomaly::sdk::StringView(
            g_LastUpdateOnGameThread.load(std::memory_order_relaxed)
                ? "线程检查：最近一次 Update 确认在游戏线程"
                : "线程检查：尚未确认游戏线程；不会调用重复伤害函数"));

        if (HAS_UI(ui, separator)) ui->separator(ui->user);

        if (HAS_UI(ui, button) &&
            ui->button(ui->user,
                       anomaly::sdk::StringView("清除所有链式目标"),
                       180.0F, 0.0F) != 0) {
            ClearTasks();
            SetStatusText("已清除：当前所有链式目标已移除");
            AppendLogLine("[ui] all target tasks manually cleared");
        }

        if (HAS_UI(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
        if (HAS_UI(ui, button) &&
            ui->button(ui->user,
                       anomaly::sdk::StringView("导出诊断日志"),
                       150.0F, 0.0F) != 0) {
            (void)ExportDiagnosticLog();
        }

        if (g_LastExportPathUtf8[0] != '\0') {
            ui->text(ui->user, anomaly::sdk::StringView("最近日志文件："));
            ui->text(ui->user, anomaly::sdk::StringView(g_LastExportPathUtf8));
        }
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
static_assert(std::is_same_v<decltype(AnomalyUe5FrameworkServiceV1::is_game_thread),
                             int (ANOMALY_CALL *)(void*)>);
static_assert(std::is_same_v<decltype(AnomalyUiServiceV1::checkbox),
                             int (ANOMALY_CALL *)(void*, AnomalyStringViewV1, int*)>);
static_assert(std::is_same_v<decltype(AnomalyUiServiceV1::slider_float),
                             int (ANOMALY_CALL *)(void*, AnomalyStringViewV1, float*, float, float)>);
static_assert(std::is_same_v<decltype(AnomalyUiServiceV1::button),
                             int (ANOMALY_CALL *)(void*, AnomalyStringViewV1, float, float)>);
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
        anomaly::sdk::StringView("1.4.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw
    };
    return anomaly::sdk::Ok();
}
