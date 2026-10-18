#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/ui.h"

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
#include <mutex>
#include <new>

namespace {

// This is only the user-supplied candidate offset. The 5.6.1 Dump confirms the
// reflected UGameplayStatics::ApplyDamage signature, but does not map this RVA
// to that function. Deliberately never install a hook at this candidate address.
constexpr std::uintptr_t kCandidateApplyDamageOffset = 0x42D5E50;
constexpr std::uint32_t kDefaultEchoIntervalMs = 200;
constexpr std::uint32_t kMinEchoIntervalMs = 50;
constexpr std::uint32_t kMaxEchoIntervalMs = 2000;
constexpr std::size_t kLogCapacity = 64;
constexpr std::size_t kLogLineSize = 256;

struct Context {
    std::mutex mutex;
    bool enabled{};
    bool candidateModuleFound{};
    bool candidatePageExecutable{};
    std::uintptr_t candidateAddress{};
    std::uint32_t echoIntervalMs{kDefaultEchoIntervalMs};
    std::uint64_t totalCapturedTargets{};
    std::uint64_t totalReplays{};
    std::uint64_t droppedTargets{};
    int windowOpen{1};
    char status[192]{"入口待验证：链式伤害已安全停用"};
    char exportPathUtf8[512]{};
    char log[kLogCapacity][kLogLineSize]{};
    std::size_t logWriteIndex{};
    std::size_t logCount{};
};

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

void SetStatus(Context* ctx, const char* status) noexcept {
    if (ctx == nullptr || status == nullptr) return;
    std::lock_guard lock(ctx->mutex);
    (void)strncpy_s(ctx->status, sizeof(ctx->status), status, _TRUNCATE);
}

void AddLog(Context* ctx, const char* message) noexcept {
    if (ctx == nullptr || message == nullptr) return;

    std::lock_guard lock(ctx->mutex);
    auto& line = ctx->log[ctx->logWriteIndex];
    (void)std::snprintf(line, sizeof(line), "[%llu] %s",
                        static_cast<unsigned long long>(GetTickCount64()), message);
    ctx->logWriteIndex = (ctx->logWriteIndex + 1) % kLogCapacity;
    if (ctx->logCount < kLogCapacity) ++ctx->logCount;
}

bool ExportDiagnosticLog(Context* ctx) noexcept {
    if (ctx == nullptr) return false;

    char snapshot[kLogCapacity][kLogLineSize]{};
    std::size_t count = 0;
    std::uintptr_t candidateAddress = 0;
    bool pageExecutable = false;

    {
        std::lock_guard lock(ctx->mutex);
        count = ctx->logCount;
        const std::size_t first = (ctx->logWriteIndex + kLogCapacity - count) % kLogCapacity;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t slot = (first + i) % kLogCapacity;
            (void)strncpy_s(snapshot[i], kLogLineSize, ctx->log[slot], _TRUNCATE);
        }
        candidateAddress = ctx->candidateAddress;
        pageExecutable = ctx->candidatePageExecutable;
    }

    wchar_t tempPath[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (tempLength == 0 || tempLength >= std::size(tempPath)) {
        SetStatus(ctx, "日志导出失败：无法获取系统临时目录");
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
        SetStatus(ctx, "日志导出失败：输出路径过长");
        return false;
    }

    HANDLE file = CreateFileW(outputPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        char status[192]{};
        (void)std::snprintf(status, sizeof(status), "日志导出失败：Win32=%lu",
                            static_cast<unsigned long>(GetLastError()));
        SetStatus(ctx, status);
        return false;
    }

    char header[512]{};
    const int headerLength = std::snprintf(
        header, sizeof(header),
        "Damage Chain diagnostic log\r\n"
        "Mode: entry-guarded; chain execution is intentionally disabled.\r\n"
        "Candidate offset: HTGame.exe + 0x%llX\r\n"
        "Candidate page executable: %s\r\n"
        "Dump finding: Engine.GameplayStatics.ApplyDamage is a reflected wrapper; "
        "the Dump does not prove the candidate offset is its machine-code entry.\r\n\r\n",
        static_cast<unsigned long long>(kCandidateApplyDamageOffset),
        pageExecutable ? "yes" : "no");
    DWORD written = 0;
    bool ok = headerLength > 0 &&
              static_cast<std::size_t>(headerLength) < sizeof(header) &&
              WriteFile(file, header, static_cast<DWORD>(headerLength), &written, nullptr) != 0 &&
              written == static_cast<DWORD>(headerLength);

    for (std::size_t i = 0; ok && i < count; ++i) {
        const DWORD length = static_cast<DWORD>(std::strlen(snapshot[i]));
        ok = WriteFile(file, snapshot[i], length, &written, nullptr) != 0 && written == length;
        if (ok) {
            constexpr char newline[] = "\r\n";
            ok = WriteFile(file, newline, 2, &written, nullptr) != 0 && written == 2;
        }
    }

    const DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        char status[192]{};
        (void)std::snprintf(status, sizeof(status), "日志导出失败：Win32=%lu",
                            static_cast<unsigned long>(writeError));
        SetStatus(ctx, status);
        return false;
    }

    char pathUtf8[512]{};
    (void)WideCharToMultiByte(CP_UTF8, 0, outputPath, -1, pathUtf8,
                              static_cast<int>(sizeof(pathUtf8)), nullptr, nullptr);
    {
        std::lock_guard lock(ctx->mutex);
        (void)strncpy_s(ctx->exportPathUtf8, sizeof(ctx->exportPathUtf8), pathUtf8, _TRUNCATE);
        (void)strncpy_s(ctx->status, sizeof(ctx->status),
                        "诊断日志已导出到系统临时目录", _TRUNCATE);
    }
    AddLog(ctx, "[ui] diagnostic log exported");
    return true;
}

#define HAS_UI(ui, field) \
    ((ui) != nullptr && \
     (ui)->struct_size >= offsetof(AnomalyUiServiceV1, field) + sizeof((ui)->field) && \
     (ui)->field != nullptr)

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** pluginContext) {
    if (host == nullptr || pluginContext == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *pluginContext = nullptr;

    auto* ctx = new (std::nothrow) Context{};
    if (ctx == nullptr) return {ANOMALY_STATUS_V1_FAILED, 0, {}};

    const auto module = GetModuleHandleA("HTGame.exe");
    ctx->candidateModuleFound = module != nullptr;
    if (module != nullptr) {
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        ctx->candidateAddress = base + kCandidateApplyDamageOffset;
        ctx->candidatePageExecutable =
            IsExecutableImageAddress(ctx->candidateAddress, module);
    }

    if (!ctx->candidateModuleFound) {
        (void)strncpy_s(ctx->status, sizeof(ctx->status),
                        "未找到 HTGame.exe；伤害入口未检查", _TRUNCATE);
        AddLog(ctx, "[load] HTGame.exe module not found; no hook installed");
    } else {
        AddLog(ctx, "[load] 5.6.1 Dump matches the ApplyDamage reflected signature, not the raw offset");
        AddLog(ctx, ctx->candidatePageExecutable
                        ? "[check] candidate address is in an executable HTGame image page; function identity remains unverified"
                        : "[check] candidate address is not in an executable HTGame image page");
        AddLog(ctx, "[guard] no hook installed because 0x42D5E50 is not mapped to ApplyDamage by the Dump");
        (void)strncpy_s(ctx->status, sizeof(ctx->status),
                        "安全停用：0x42D5E50 未在 Dump 中对应到伤害函数", _TRUNCATE);
    }

    *pluginContext = ctx;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* pluginContext) {
    auto* ctx = static_cast<Context*>(pluginContext);
    if (ctx == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};

    // The raw function entry and ABI have not been verified. Never enable a
    // damage loop merely because an address happens to be in an executable page.
    {
        std::lock_guard lock(ctx->mutex);
        ctx->enabled = false;
        (void)strncpy_s(ctx->status, sizeof(ctx->status),
                        "启动被阻止：需先验证伤害入口与调用 ABI", _TRUNCATE);
    }
    AddLog(ctx, "[lifecycle] start blocked: raw ApplyDamage entry/signature not Dump-verified");
    return {ANOMALY_STATUS_V1_UNAVAILABLE, 0,
            anomaly::sdk::StringView("伤害入口尚未通过 Dump/ABI 验证；未启用伤害调用")};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* pluginContext, std::uint32_t) {
    auto* ctx = static_cast<Context*>(pluginContext);
    if (ctx == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};

    {
        std::lock_guard lock(ctx->mutex);
        ctx->enabled = false;
        (void)strncpy_s(ctx->status, sizeof(ctx->status), "已暂停；当前版本未安装伤害 Hook", _TRUNCATE);
    }
    AddLog(ctx, "[lifecycle] stopped; no damage hook was installed");
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* pluginContext) {
    auto* ctx = static_cast<Context*>(pluginContext);
    if (ctx == nullptr) return;
    {
        std::lock_guard lock(ctx->mutex);
        ctx->enabled = false;
    }
    delete ctx;
}

void ANOMALY_CALL Update(void*, double) {
    // Deliberately empty: until a Dump-mapped entry or supported invocation
    // bridge is verified, this plugin must not generate damage calls.
}

void ANOMALY_CALL Draw(void* pluginContext, const AnomalyUiServiceV1* ui) {
    auto* ctx = static_cast<Context*>(pluginContext);
    if (ctx == nullptr || !HAS_UI(ui, begin_window) ||
        !HAS_UI(ui, end_window) || !HAS_UI(ui, text)) {
        return;
    }

    if (HAS_UI(ui, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 560.0F, 390.0F, 4u);
    }

    {
        anomaly::sdk::UiWindow window(ui, "伤害链式反应（入口核验版）", &ctx->windowOpen);
        if (!window) return;

        ui->text(ui->user, anomaly::sdk::StringView(
            "当前版本不会挂接未经验证的函数地址，也不会主动调用伤害函数。"));
        ui->text(ui->user, anomaly::sdk::StringView(
            "Dump 已确认 UGameplayStatics::ApplyDamage 的反射签名；未确认 0x42D5E50 是其机器码入口。"));

        char line[256]{};
        {
            std::lock_guard lock(ctx->mutex);
            (void)std::snprintf(line, sizeof(line), "候选入口：HTGame.exe + 0x%llX",
                static_cast<unsigned long long>(kCandidateApplyDamageOffset));
        }
        ui->text(ui->user, anomaly::sdk::StringView(line));

        {
            std::lock_guard lock(ctx->mutex);
            const char* pageState = !ctx->candidateModuleFound
                ? "未检查（未找到 HTGame.exe）"
                : (ctx->candidatePageExecutable
                    ? "可执行内存页（仅地址检查，不代表函数身份正确）"
                    : "未通过可执行内存页检查");
            ui->text(ui->user, anomaly::sdk::StringView("地址检查："));
            ui->text(ui->user, anomaly::sdk::StringView(pageState));
            ui->text(ui->user, anomaly::sdk::StringView("功能状态：安全停用，等待入口/ABI 证据"));
            ui->text(ui->user, anomaly::sdk::StringView(ctx->status));
            (void)std::snprintf(line, sizeof(line), "触发间隔设置：%u ms（当前不执行）",
                                ctx->echoIntervalMs);
        }
        ui->text(ui->user, anomaly::sdk::StringView(line));

        float interval = static_cast<float>(kDefaultEchoIntervalMs);
        {
            std::lock_guard lock(ctx->mutex);
            interval = static_cast<float>(ctx->echoIntervalMs);
        }
        if (HAS_UI(ui, slider_float) &&
            ui->slider_float(ui->user, anomaly::sdk::StringView("预设触发间隔 (毫秒)"),
                             &interval, static_cast<float>(kMinEchoIntervalMs),
                             static_cast<float>(kMaxEchoIntervalMs)) != 0) {
            if (interval < static_cast<float>(kMinEchoIntervalMs)) interval = static_cast<float>(kMinEchoIntervalMs);
            if (interval > static_cast<float>(kMaxEchoIntervalMs)) interval = static_cast<float>(kMaxEchoIntervalMs);
            std::lock_guard lock(ctx->mutex);
            ctx->echoIntervalMs = static_cast<std::uint32_t>(interval + 0.5F);
        }

        if (HAS_UI(ui, separator)) ui->separator(ui->user);
        if (HAS_UI(ui, button) &&
            ui->button(ui->user, anomaly::sdk::StringView("尝试启动链式伤害"), 190.0F, 0.0F) != 0) {
            (void)Start(ctx);
        }
        if (HAS_UI(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
        if (HAS_UI(ui, button) &&
            ui->button(ui->user, anomaly::sdk::StringView("导出诊断日志"), 160.0F, 0.0F) != 0) {
            (void)ExportDiagnosticLog(ctx);
        }

        std::size_t logCount = 0;
        std::uint64_t totalReplays = 0;
        char statusCopy[192]{};
        char pathCopy[512]{};
        {
            std::lock_guard lock(ctx->mutex);
            logCount = ctx->logCount;
            totalReplays = ctx->totalReplays;
            (void)strncpy_s(statusCopy, sizeof(statusCopy), ctx->status, _TRUNCATE);
            (void)strncpy_s(pathCopy, sizeof(pathCopy), ctx->exportPathUtf8, _TRUNCATE);
        }
        (void)std::snprintf(line, sizeof(line), "活动目标：0（尚未启用伤害 Hook）");
        ui->text(ui->user, anomaly::sdk::StringView(line));
        (void)std::snprintf(line, sizeof(line), "已触发次数：%llu",
                            static_cast<unsigned long long>(totalReplays));
        ui->text(ui->user, anomaly::sdk::StringView(line));
        (void)std::snprintf(line, sizeof(line), "保留诊断日志：%zu / %zu", logCount, kLogCapacity);
        ui->text(ui->user, anomaly::sdk::StringView(line));
        if (pathCopy[0] != '\0') {
            ui->text(ui->user, anomaly::sdk::StringView("最近导出路径："));
            ui->text(ui->user, anomaly::sdk::StringView(pathCopy));
        }
    }
}

using UiBeginWindowSignature = int (ANOMALY_CALL *)(
    void*, AnomalyStringViewV1, int*, std::uint32_t);
static_assert(std::is_same_v<decltype(AnomalyUiServiceV1::begin_window), UiBeginWindowSignature>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_load), decltype(&Load)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_start), decltype(&Start)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_stop), decltype(&Stop)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_unload), decltype(&Unload)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_update), decltype(&Update)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_draw), decltype(&Draw)>);

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
        anomaly::sdk::StringView("Damage Chain (Entry Guarded)"),
        anomaly::sdk::StringView("Rabb1t79"),
        anomaly::sdk::StringView("1.5.0"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw
    };
    return anomaly::sdk::Ok();
}
