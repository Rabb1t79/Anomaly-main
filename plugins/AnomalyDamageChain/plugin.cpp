#include "anomaly/sdk/cpp.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using anomaly::sdk::Host;
using anomaly::sdk::StringView;

// ---- 服务 ----
const AnomalyHostApiV1* g_host{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyUiServiceV1* g_ui{};
const AnomalySignatureServiceV1* g_signature{};
const AnomalyHookServiceV1* g_hook{};

// ---- 配置 ----
struct Config {
    int enabled{1};
    int rate{15};  // 每秒次数（速率），不是总次数
};
Config g_conf{};
double g_now_ms{};

// ---- 多段伤害 ----
// 依据：Tokky AOB 表中的 pair2，已在本版本游戏代码中验证唯一命中 RVA 0x7682FF0。
// 该函数带栈参数，detour 必须是汇编 stub：保存入口 rsp、抓取寄存器与栈参数，
// 再原样跳回原函数，全程不移动栈。stub 在运行时生成，避免改动构建系统。
constexpr const char* kBurstPattern =
    "48 85 D2 0F 84 41 01 00 00 57 48 83 EC 60 48 8B F9 4D 85 C0 0F 84 2B 01 00 00 "
    "48 89 5C 24 70 48";

constexpr int kStackArgCount = 16;
struct Capture {
    std::uintptr_t entry_rsp;
    std::uintptr_t reg[4];
    std::uintptr_t stack[kStackArgCount];
};
Capture g_capture{};
void* g_original{};
void* g_stub{};
void* g_replay{};
AnomalyGenerationHandleV1 g_hook_handle{};

double g_period_ms{1000.0 / 15.0};
double g_next_due{};
bool g_armed{};
unsigned long long g_hits{};
unsigned long long g_pulses{};
unsigned long long g_faults{};

void LogInfo(const char* text) {
    if (g_core == nullptr || g_core->log == nullptr || text == nullptr) return;
    g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
}

// 判断指针是否仍像一个有效的 UObject：首 8 字节应是落在游戏映像内的虚表指针。
bool LooksLikeLiveObject(const void* p) {
    if (p == nullptr) return false;
    const HMODULE mod = GetModuleHandleA("HTGame.exe");
    if (mod == nullptr) return false;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
    std::uintptr_t vtbl = 0;
    __try {
        vtbl = *reinterpret_cast<const std::uintptr_t*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return vtbl >= base && vtbl < base + 0x12000000ull;
}

extern "C" void OnBurstHit() {
    // 原函数自身会跳过 rdx/r8 为空的调用；这类调用不能进重放队列，
    // 否则补发时会在 [rdx+0xa8] 处访问空指针。
    if (g_capture.reg[1] == 0 || g_capture.reg[2] == 0) return;
    g_armed = true;
    g_next_due = g_now_ms + g_period_ms;
    ++g_hits;
}

// 生成两条 stub：入口转发、重放调用。
bool BuildStubs() {
    constexpr std::size_t kSize = 512;
    auto* mem = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (mem == nullptr) return false;
    std::uint8_t* p = mem;
    std::uint8_t* q = mem + kSize;

    auto put64 = [](std::uint8_t* d, std::uint64_t v, std::size_t& i) {
        std::memcpy(d + i, &v, sizeof(v));
        i += sizeof(v);
    };

    // ---- stub1：入口转发 ----
    std::size_t i = 0;
    // mov rax, &g_capture
    p[i++] = 0x48; p[i++] = 0xB8;
    put64(p, reinterpret_cast<std::uint64_t>(&g_capture), i);
    // mov [rax+0x00], rsp
    p[i++] = 0x48; p[i++] = 0x89; p[i++] = 0x20;
    // mov [rax+0x08], rcx
    p[i++] = 0x48; p[i++] = 0x89; p[i++] = 0x48; p[i++] = 0x08;
    // mov [rax+0x10], rdx
    p[i++] = 0x48; p[i++] = 0x89; p[i++] = 0x50; p[i++] = 0x10;
    // mov [rax+0x18], r8
    p[i++] = 0x4C; p[i++] = 0x89; p[i++] = 0x40; p[i++] = 0x18;
    // mov [rax+0x20], r9
    p[i++] = 0x4C; p[i++] = 0x89; p[i++] = 0x48; p[i++] = 0x20;
    // lea rsi, [rsp+8]
    p[i++] = 0x48; p[i++] = 0x8D; p[i++] = 0x74; p[i++] = 0x24; p[i++] = 0x08;
    // lea rdi, [rax+0x28]
    p[i++] = 0x48; p[i++] = 0x8D; p[i++] = 0x78; p[i++] = 0x28;
    // mov ecx, kStackArgCount
    p[i++] = 0xB9;
    p[i++] = static_cast<std::uint8_t>(kStackArgCount);
    p[i++] = 0; p[i++] = 0; p[i++] = 0;
    // rep movsq
    p[i++] = 0xF3; p[i++] = 0x48; p[i++] = 0xA5;
    // mov rax, &OnBurstHit
    p[i++] = 0x48; p[i++] = 0xB8;
    put64(p, reinterpret_cast<std::uint64_t>(&OnBurstHit), i);
    // sub rsp, 0x28
    p[i++] = 0x48; p[i++] = 0x83; p[i++] = 0xEC; p[i++] = 0x28;
    // call rax
    p[i++] = 0xFF; p[i++] = 0xD0;
    // add rsp, 0x28
    p[i++] = 0x48; p[i++] = 0x83; p[i++] = 0xC4; p[i++] = 0x28;
    // rep movsq 已把 RCX 清零，跳回原函数前必须把四个参数寄存器全部还原，
    // 否则原函数拿到的是 rcx=0。
    // mov rax, &g_capture
    p[i++] = 0x48; p[i++] = 0xB8;
    put64(p, reinterpret_cast<std::uint64_t>(&g_capture), i);
    // mov rcx, [rax+0x08]
    p[i++] = 0x48; p[i++] = 0x8B; p[i++] = 0x48; p[i++] = 0x08;
    // mov rdx, [rax+0x10]
    p[i++] = 0x48; p[i++] = 0x8B; p[i++] = 0x50; p[i++] = 0x10;
    // mov r8, [rax+0x18]
    p[i++] = 0x4C; p[i++] = 0x8B; p[i++] = 0x40; p[i++] = 0x18;
    // mov r9, [rax+0x20]
    p[i++] = 0x4C; p[i++] = 0x8B; p[i++] = 0x48; p[i++] = 0x20;
    // mov rsp, [rax]
    p[i++] = 0x48; p[i++] = 0x8B; p[i++] = 0x20;
    // mov rax, &g_original
    p[i++] = 0x48; p[i++] = 0xB8;
    put64(p, reinterpret_cast<std::uint64_t>(&g_original), i);
    // jmp qword [rax]
    p[i++] = 0xFF; p[i++] = 0x20;

    // ---- stub2：重放 ----
    // 关键顺序：先做 rep movsq 拷贝栈参数，最后才装 rcx/rdx/r8/r9。
    // rep movsq 会把 RCX 清零，若先装参数再拷贝，第一个参数必丢。
    std::size_t j = 0;
    // mov rax, &g_capture
    q[j++] = 0x48; q[j++] = 0xB8;
    put64(q, reinterpret_cast<std::uint64_t>(&g_capture), j);
    // sub rsp, 0x88（16 字节对齐）
    q[j++] = 0x48; q[j++] = 0x81; q[j++] = 0xEC;
    q[j++] = 0x88; q[j++] = 0x00; q[j++] = 0x00; q[j++] = 0x00;
    // lea rsi, [rax+0x28]
    q[j++] = 0x48; q[j++] = 0x8D; q[j++] = 0x70; q[j++] = 0x28;
    // mov rdi, rsp
    q[j++] = 0x48; q[j++] = 0x89; q[j++] = 0xE7;
    // mov ecx, kStackArgCount
    q[j++] = 0xB9;
    q[j++] = static_cast<std::uint8_t>(kStackArgCount);
    q[j++] = 0; q[j++] = 0; q[j++] = 0;
    // rep movsq（拷贝到 [rsp+0..]，即原函数看到的栈参数位置）
    q[j++] = 0xF3; q[j++] = 0x48; q[j++] = 0xA5;
    // 现在再装寄存器参数（rep movsq 已结束，不再破坏 RCX）
    // mov rcx, [rax+0x08]
    q[j++] = 0x48; q[j++] = 0x8B; q[j++] = 0x48; q[j++] = 0x08;
    // mov rdx, [rax+0x10]
    q[j++] = 0x48; q[j++] = 0x8B; q[j++] = 0x50; q[j++] = 0x10;
    // mov r8, [rax+0x18]
    q[j++] = 0x4C; q[j++] = 0x8B; q[j++] = 0x40; q[j++] = 0x18;
    // mov r9, [rax+0x20]
    q[j++] = 0x4C; q[j++] = 0x8B; q[j++] = 0x48; q[j++] = 0x20;
    // mov rax, &g_original
    q[j++] = 0x48; q[j++] = 0xB8;
    put64(q, reinterpret_cast<std::uint64_t>(&g_original), j);
    // call qword [rax]
    q[j++] = 0xFF; q[j++] = 0x10;
    // add rsp, 0x88
    q[j++] = 0x48; q[j++] = 0x81; q[j++] = 0xC4;
    q[j++] = 0x88; q[j++] = 0x00; q[j++] = 0x00; q[j++] = 0x00;
    // ret
    q[j++] = 0xC3;

    FlushInstructionCache(GetCurrentProcess(), mem, kSize * 2);
    g_stub = p;
    g_replay = q;
    return true;
}

// 安全读：只用于诊断探针。
bool SafeRead(std::uintptr_t src, void* dst, std::size_t size) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 探针：确认 ue5.ProcessEvent 的原始签名能否解析，以及 0x18AACF0 处的实际字节。
// 目的：区分「签名与构建不符」和「函数入口已被运行时改写」。
void ProbeProcessEvent() {
    if (g_signature == nullptr || g_signature->resolve == nullptr) {
        LogInfo("damage-chain: probe skipped (no signature service)");
        return;
    }
    constexpr const char* kOriginal =
        "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 "
        "48 8D 6C 24 30 48 89 9D 28 01 00 00";
    char buf[260]{};
    std::uintptr_t resolved = 0;
    const bool ok = g_signature
                        ->resolve(g_signature->user, StringView("HTGame.exe"),
                                  StringView(".text"), StringView(kOriginal), &resolved)
                        .code == ANOMALY_STATUS_V1_OK &&
                    resolved >= 0x10000;

    const HMODULE mod = GetModuleHandleA("HTGame.exe");
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
    unsigned char head[16]{};
    bool read_ok = false;
    if (base != 0) read_ok = SafeRead(base + 0x18AACF0u, head, sizeof(head));

    int n = std::snprintf(buf, sizeof(buf), "damage-chain: probe original_pattern=%s",
                          ok ? "MATCH" : "MISS");
    if (ok) {
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                           " rva=0x%llX",
                           static_cast<unsigned long long>(resolved - base));
    }
    if (read_ok && n > 0 && static_cast<std::size_t>(n) + 64 < sizeof(buf)) {
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                           " head@18AACF0=");
        for (int i = 0; i < 8 && static_cast<std::size_t>(n) + 4 < sizeof(buf); ++i) {
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n), "%02X",
                               head[i]);
        }
    }
    LogInfo(buf);
}

void EnsureHook() {
    if (g_hook_handle.id != 0) return;
    if (g_stub == nullptr || g_hook == nullptr || g_signature == nullptr) return;

    std::uintptr_t target = 0;
    if (g_signature
            ->resolve(g_signature->user, StringView("HTGame.exe"), StringView(".text"),
                      StringView(kBurstPattern), &target)
            .code != ANOMALY_STATUS_V1_OK ||
        target < 0x10000) {
        return;
    }

    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = g_stub;
    request.label = StringView("damage-chain.burst");

    std::uintptr_t original = 0;
    AnomalyGenerationHandleV1 handle{};
    if (g_hook->create(g_hook->user, &request, &original, &handle).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }
    g_original = reinterpret_cast<void*>(original);
    g_hook_handle = handle;

    const HMODULE mod = GetModuleHandleA("HTGame.exe");
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
    char buf[220]{};
    std::snprintf(buf, sizeof(buf), "damage-chain: burst hook rva=0x%llX rate=%d/s",
                  static_cast<unsigned long long>(base != 0 ? target - base : target),
                  g_conf.rate);
    LogInfo(buf);
}

// 补发调用的护栏：延迟调用可能踩到游戏已清理的容器（实测 TArray.Data 为空但 Num>0）。
// 用一个 SEH 帧就地拦截，失败即停止循环，不再让异常穿透到游戏。
// 注意：本函数内不能有需要析构的局部对象，否则 __try 不可用。
int CallReplayGuarded() {
    __try {
        reinterpret_cast<void (*)()>(g_replay)();
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void Pump() {
    if (!g_armed || g_replay == nullptr || g_original == nullptr) return;
    if (g_now_ms < g_next_due) return;
    // 补发前确认对象仍然有效：对象可能已被销毁，直接调用会访问悬空指针。
    if (!LooksLikeLiveObject(reinterpret_cast<const void*>(g_capture.reg[1])) ||
        !LooksLikeLiveObject(reinterpret_cast<const void*>(g_capture.reg[2]))) {
        g_armed = false;  // 目标已不存在，停止循环
        return;
    }
    if (CallReplayGuarded() == 0) {
        // 目标状态已失效：停止循环，避免反复触发同一处失效访问。
        g_armed = false;
        ++g_faults;
        LogInfo("damage-chain: replay hit invalid state, chain stopped");
        return;
    }
    g_next_due = g_now_ms + g_period_ms;
    ++g_pulses;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context != nullptr) *context = nullptr;
    g_host = host;
    const Host view(host);
    g_core = view
                 .Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
                                             ANOMALY_CORE_SERVICE_V1_VERSION)
                 .get();
    g_ui = view.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID,
                                          ANOMALY_UI_SERVICE_V1_VERSION)
               .get();
    g_signature = view
                      .Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID,
                                                        ANOMALY_SIGNATURE_SERVICE_V1_VERSION)
                      .get();
    g_hook = view.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID,
                                              ANOMALY_HOOK_SERVICE_V1_VERSION)
                 .get();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    g_period_ms = 1000.0 / static_cast<double>(g_conf.rate > 0 ? g_conf.rate : 1);
    if (!BuildStubs()) {
        LogInfo("damage-chain: stub allocation failed");
    }
    ProbeProcessEvent();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    g_armed = false;
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    g_armed = false;
    if (g_hook_handle.id != 0 && g_hook != nullptr && g_hook->release != nullptr) {
        g_hook->release(g_hook->user, g_hook_handle);
    }
    g_hook_handle = AnomalyGenerationHandleV1{};
    g_original = nullptr;
    g_stub = nullptr;
    g_replay = nullptr;
    g_hook = nullptr;
    g_signature = nullptr;
    g_ui = nullptr;
    g_core = nullptr;
    g_host = nullptr;
    g_hits = 0;
    g_pulses = 0;
}

void ANOMALY_CALL Update(void*, double delta_seconds) {
    g_now_ms += (delta_seconds > 0.0 ? delta_seconds : 0.0) * 1000.0;
    if (g_stub == nullptr) BuildStubs();
    EnsureHook();
    if (g_conf.enabled != 0) Pump();
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) noexcept {
    if (ui == nullptr) ui = g_ui;
    if (ui == nullptr) return;
    // 生命周期：先确认服务表与所需方法齐备，再按 begin/end 成对使用。
    // open 传 nullptr 表示不提供关闭按钮，窗口常驻，避免每帧重建局部标志。
    if (ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr || ui->checkbox == nullptr || ui->input_uint32 == nullptr ||
        ui->struct_size < offsetof(AnomalyUiServiceV1, end_window) +
                              sizeof(ui->end_window)) {
        return;
    }
    if (ui->begin_window(ui->user, anomaly::sdk::StringView("Damage Chain Reaction"),
                         nullptr, 0u) == 0) {
        return;
    }

    char buf[192]{};
    std::snprintf(buf, sizeof(buf), "状态: %s",
                  g_hook_handle.id != 0 ? (g_armed ? "循环中" : "已挂钩") : "等待挂钩");
    ui->text(ui->user, anomaly::sdk::StringView(buf));

    std::uint32_t rate = static_cast<std::uint32_t>(g_conf.rate > 0 ? g_conf.rate : 1);
    if (ui->input_uint32(ui->user, anomaly::sdk::StringView("每秒次数（速率）"), &rate, 1u,
                         5u) != 0) {
        if (rate < 1u) rate = 1u;
        if (rate > 120u) rate = 120u;
        g_conf.rate = static_cast<int>(rate);
        g_period_ms = 1000.0 / static_cast<double>(rate);
    }

    std::snprintf(buf, sizeof(buf), "间隔: %.1f ms", g_period_ms);
    ui->text(ui->user, anomaly::sdk::StringView(buf));

    int enabled = g_conf.enabled;
    if (ui->checkbox(ui->user, anomaly::sdk::StringView("启用多段伤害"), &enabled) != 0) {
        g_conf.enabled = enabled;
    }

    std::snprintf(buf, sizeof(buf), "命中: %llu    已补发: %llu    失效停止: %llu", g_hits,
                  g_pulses, g_faults);
    ui->text(ui->user, anomaly::sdk::StringView(buf));

    ui->end_window(ui->user);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   anomaly::sdk::StringView("anomaly.local.damage-chain"),
                   anomaly::sdk::StringView("Damage Chain Reaction"),
                   anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("3.1.0"),
                   Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
