#include "anomaly/sdk/cpp.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using anomaly::sdk::Host;
using anomaly::sdk::StringView;

// ImGui Cond_FirstUseEver：窗口尺寸只在首次出现时应用。
constexpr std::uint32_t kCondFirstUseEver = 4u;

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
    std::uintptr_t entry_rsp;              // 0x00
    std::uintptr_t ret_addr;               // 0x08
    std::uintptr_t reg[4];                 // 0x10
    std::uintptr_t stack[kStackArgCount];  // 0x30，从第 5 个参数开始
};
Capture g_capture{};      // 外层施加函数的参数（补发用）
Capture g_gate_capture{}; // 判定函数的参数（不能与外层共用，否则互相覆盖）
void* g_original{};
void* g_stub{};
void* g_replay{};
AnomalyGenerationHandleV1 g_hook_handle{};

// 内层判定函数（gate）：返回 bool，决定本次攻击是否真正施加伤害。
// 它只带 4 个寄存器参数、无栈参数，因此可以用普通 C detour 安全挂钩。
// 签名取自游戏代码 0x767F0D0 前 32 字节，已在该版本验证唯一命中。
constexpr const char* kGatePattern =
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A9 "
    "48 81 EC D0";
using GateFn = int (*)(void*, void*, void*, void*);
void* g_gate_original{};
AnomalyGenerationHandleV1 g_gate_hook{};
void* g_gate_stub{};
std::uintptr_t g_gate_result{};

double g_period_ms{1000.0 / 15.0};
double g_next_due{};
bool g_armed{};
// 被钩函数的调用方返回地址（stub 入口的 [rsp]），用于静态定位调用链。
std::uintptr_t g_burst_caller{};
bool g_burst_caller_logged{};
// 诊断：统计所有进入被钩函数的调用，而不只是通过过滤的。
unsigned long long g_call_total{};
unsigned long long g_call_null_args{};
unsigned long long g_call_no_target{};
unsigned long long g_call_valid{};
std::uintptr_t g_sample_rdx{};
std::uintptr_t g_sample_r8{};
std::uintptr_t g_sample_target{};
std::uintptr_t g_sample_caller{};
bool g_sample_ready{};

// 链式判定期间持有的对象引用。UE 用 GC 而非引用计数，阻止回收的手段是
// 在 UObject::ObjectFlags（Profile: object.flags = 8）上置 RF_MarkAsRootSet。
// 值来源：UE ObjectMacros.h 的 EObjectFlags 枚举，RF_MarkAsRootSet = 0x80。
constexpr std::uintptr_t kObjectFlagsOffset = 8;
constexpr std::uint32_t kRfMarkAsRootSet = 0x80u;
void* g_rooted[4]{};
int g_rooted_count{};
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

// 读一个指针字段，越界时返回 0 而不是抛异常。
std::uintptr_t ReadField(const void* base, std::size_t offset) {
    if (base == nullptr) return 0;
    std::uintptr_t value = 0;
    __try {
        value = *reinterpret_cast<const std::uintptr_t*>(
            static_cast<const unsigned char*>(base) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return value;
}

extern "C" void OnBurstHit() {
    ++g_call_total;

    const std::uintptr_t caller =
        g_capture.entry_rsp != 0
            ? ReadField(reinterpret_cast<const void*>(g_capture.entry_rsp), 0)
            : 0;
    if (g_burst_caller == 0 && caller != 0) g_burst_caller = caller;

    // 第一次调用时留一份样本，供离线分析参数形态。
    if (!g_sample_ready) {
        g_sample_ready = true;
        g_sample_rdx = g_capture.reg[1];
        g_sample_r8 = g_capture.reg[2];
        g_sample_target = ReadField(reinterpret_cast<const void*>(g_capture.reg[1]), 0xa8);
        g_sample_caller = caller;
    }

    if (g_capture.reg[1] == 0 || g_capture.reg[2] == 0) {
        ++g_call_null_args;
        return;
    }
    if (ReadField(reinterpret_cast<const void*>(g_capture.reg[1]), 0xa8) == 0) {
        ++g_call_no_target;
        return;
    }

    // 只统计：是否真正施加伤害由判定函数（gate）的返回值决定。
    ++g_call_valid;
}

// 由判定函数确认"已施加伤害"后调用。
void ArmChain() {
    g_armed = true;
    g_next_due = g_now_ms + g_period_ms;
    ++g_hits;
}

// 生成两条 stub：入口转发、重放调用。
bool BuildStubs() {
    constexpr std::size_t kSlot = 512;
    auto* mem = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kSlot * 3, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (mem == nullptr) return false;
    std::uint8_t* p = mem;
    std::uint8_t* q = mem + kSlot;
    std::uint8_t* g = mem + kSlot * 2;

    auto put64 = [](std::uint8_t* d, std::uint64_t v, std::size_t& i) {
        std::memcpy(d + i, &v, sizeof(v));
        i += sizeof(v);
    };
    auto mov_rax_imm = [&](std::uint8_t* d, std::size_t& i, const void* addr) {
        d[i++] = 0x48; d[i++] = 0xB8;
        put64(d, reinterpret_cast<std::uint64_t>(addr), i);
    };
    auto load_from_rax = [&](std::uint8_t* d, std::size_t& i, std::uint8_t rex,
                             std::uint8_t modrm, std::uint8_t disp) {
        d[i++] = rex; d[i++] = 0x8B; d[i++] = modrm; d[i++] = disp;
    };
    auto rep_movsq = [&](std::uint8_t* d, std::size_t& i) {
        d[i++] = 0xB9; d[i++] = static_cast<std::uint8_t>(kStackArgCount);
        d[i++] = 0; d[i++] = 0; d[i++] = 0;
        d[i++] = 0xF3; d[i++] = 0x48; d[i++] = 0xA5;
    };
    // 保存四个寄存器参数与栈参数到 g_capture
    auto capture_all = [&](std::uint8_t* d, std::size_t& i, Capture* dst) {
        mov_rax_imm(d, i, dst);
        d[i++] = 0x48; d[i++] = 0x89; d[i++] = 0x20;                 // [rax]=rsp
        d[i++] = 0x4C; d[i++] = 0x8B; d[i++] = 0x14; d[i++] = 0x24;  // r10=[rsp]
        d[i++] = 0x4C; d[i++] = 0x89; d[i++] = 0x50; d[i++] = 0x08;
        d[i++] = 0x48; d[i++] = 0x89; d[i++] = 0x48; d[i++] = 0x10;
        d[i++] = 0x48; d[i++] = 0x89; d[i++] = 0x50; d[i++] = 0x18;
        d[i++] = 0x4C; d[i++] = 0x89; d[i++] = 0x40; d[i++] = 0x20;
        d[i++] = 0x4C; d[i++] = 0x89; d[i++] = 0x48; d[i++] = 0x28;
        d[i++] = 0x48; d[i++] = 0x8D; d[i++] = 0x74; d[i++] = 0x24; d[i++] = 0x28;
        d[i++] = 0x48; d[i++] = 0x8D; d[i++] = 0x78; d[i++] = 0x30;
        rep_movsq(d, i);
    };
    // 用 g_capture 构造合法调用帧并装载参数
    auto build_frame = [&](std::uint8_t* d, std::size_t& i, Capture* src) {
        mov_rax_imm(d, i, src);
        d[i++] = 0x48; d[i++] = 0x81; d[i++] = 0xEC;
        d[i++] = 0xA8; d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;
        d[i++] = 0x48; d[i++] = 0x8D; d[i++] = 0x70; d[i++] = 0x30;
        d[i++] = 0x48; d[i++] = 0x8D; d[i++] = 0x7C; d[i++] = 0x24; d[i++] = 0x20;
        rep_movsq(d, i);
        mov_rax_imm(d, i, src);
        load_from_rax(d, i, 0x48, 0x48, 0x10);
        load_from_rax(d, i, 0x48, 0x50, 0x18);
        load_from_rax(d, i, 0x4C, 0x40, 0x20);
        load_from_rax(d, i, 0x4C, 0x48, 0x28);
    };

    // ---- stub1：入口转发（尾跳，空指针保护）----
    std::size_t i = 0;
    capture_all(p, i, &g_capture);
    mov_rax_imm(p, i, &OnBurstHit);
    p[i++] = 0x48; p[i++] = 0x83; p[i++] = 0xEC; p[i++] = 0x28;
    p[i++] = 0xFF; p[i++] = 0xD0;
    p[i++] = 0x48; p[i++] = 0x83; p[i++] = 0xC4; p[i++] = 0x28;
    mov_rax_imm(p, i, &g_capture);
    load_from_rax(p, i, 0x48, 0x48, 0x10);
    load_from_rax(p, i, 0x48, 0x50, 0x18);
    load_from_rax(p, i, 0x4C, 0x40, 0x20);
    load_from_rax(p, i, 0x4C, 0x48, 0x28);
    mov_rax_imm(p, i, &g_original);
    p[i++] = 0x48; p[i++] = 0x8B; p[i++] = 0x00;   // rax=[g_original]
    p[i++] = 0x48; p[i++] = 0x85; p[i++] = 0xC0;   // test
    p[i++] = 0x74; const std::size_t pjz = i; p[i++] = 0x00;  // jz（回填）
    p[i++] = 0xFF; p[i++] = 0xE0;                  // jmp rax
    const std::size_t pfg = i;
    p[i++] = 0x31; p[i++] = 0xC0;                  // xor eax,eax
    p[i++] = 0xC3;                                 // ret
    p[pjz] = static_cast<std::uint8_t>(pfg - (pjz + 1));

    // ---- stub2：重放 ----
    std::size_t j = 0;
    build_frame(q, j, &g_capture);
    mov_rax_imm(q, j, &g_original);
    q[j++] = 0x48; q[j++] = 0x8B; q[j++] = 0x00;
    q[j++] = 0x48; q[j++] = 0x85; q[j++] = 0xC0;
    q[j++] = 0x74; const std::size_t qjz = j; q[j++] = 0x00;  // jz（回填）
    q[j++] = 0xFF; q[j++] = 0xD0;                  // call rax
    q[j++] = 0x48; q[j++] = 0x81; q[j++] = 0xC4;
    q[j++] = 0xA8; q[j++] = 0x00; q[j++] = 0x00; q[j++] = 0x00;
    q[j++] = 0xC3;                                 // ret
    const std::size_t qfg = j;
    q[qjz] = static_cast<std::uint8_t>(qfg - (qjz + 1));
    q[j++] = 0x48; q[j++] = 0x81; q[j++] = 0xC4;   // fail:
    q[j++] = 0xA8; q[j++] = 0x00; q[j++] = 0x00; q[j++] = 0x00;
    q[j++] = 0x31; q[j++] = 0xC0;
    q[j++] = 0xC3;

    // ---- stub3：判定函数（带返回值）----
    std::size_t k = 0;
    capture_all(g, k, &g_gate_capture);
    build_frame(g, k, &g_gate_capture);
    mov_rax_imm(g, k, &g_gate_original);
    g[k++] = 0x48; g[k++] = 0x8B; g[k++] = 0x00;   // rax=[g_gate_original]
    g[k++] = 0x48; g[k++] = 0x85; g[k++] = 0xC0;   // test
    g[k++] = 0x74; g[k++] = 0x00;                  // jz fail（偏移稍后回填）
    const std::size_t jz_at = k - 1;
    g[k++] = 0xFF; g[k++] = 0xD0;                  // call rax
    g[k++] = 0x49; g[k++] = 0x89; g[k++] = 0xC2;   // mov r10,rax
    g[k++] = 0xEB; g[k++] = 0x00;                  // jmp store（偏移稍后回填）
    const std::size_t jmp_at = k - 1;
    const std::size_t fail_label = k;
    g[k++] = 0x45; g[k++] = 0x31; g[k++] = 0xD2;   // xor r10d,r10d
    const std::size_t store_label = k;
    mov_rax_imm(g, k, &g_gate_result);
    g[k++] = 0x4C; g[k++] = 0x89; g[k++] = 0x10;   // mov [rax],r10
    mov_rax_imm(g, k, &OnGateResult);
    g[k++] = 0x48; g[k++] = 0x83; g[k++] = 0xEC; g[k++] = 0x20;
    g[k++] = 0xFF; g[k++] = 0xD0;
    g[k++] = 0x48; g[k++] = 0x83; g[k++] = 0xC4; g[k++] = 0x20;
    mov_rax_imm(g, k, &g_gate_result);
    g[k++] = 0x48; g[k++] = 0x8B; g[k++] = 0x00;   // mov rax,[rax]
    g[k++] = 0x48; g[k++] = 0x81; g[k++] = 0xC4;
    g[k++] = 0xA8; g[k++] = 0x00; g[k++] = 0x00; g[k++] = 0x00;
    g[k++] = 0xC3;

    g[jz_at] = static_cast<std::uint8_t>(fail_label - (jz_at + 1));
    g[jmp_at] = static_cast<std::uint8_t>(store_label - (jmp_at + 1));

    FlushInstructionCache(GetCurrentProcess(), mem, kSlot * 3);
    g_stub = p;
    g_replay = q;
    g_gate_stub = g;
    return true;
}

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

// 判定函数 detour：只有它返回真，才说明这次攻击真的施加了伤害。
// gate stub 调用完原函数后回调这里（无参数，结果在 g_gate_result）。
extern "C" void OnGateResult() {
    if (g_gate_result != 0) ArmChain();
}

void EnsureGateHook() {
    if (g_gate_hook.id != 0) return;
    if (g_hook == nullptr || g_signature == nullptr) return;

    std::uintptr_t target = 0;
    if (g_signature
            ->resolve(g_signature->user, StringView("HTGame.exe"), StringView(".text"),
                      StringView(kGatePattern), &target)
            .code != ANOMALY_STATUS_V1_OK ||
        target < 0x10000) {
        return;
    }
    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    if (g_gate_stub == nullptr) return;
    request.detour = g_gate_stub;
    request.label = StringView("damage-chain.gate");

    std::uintptr_t original = 0;
    AnomalyGenerationHandleV1 handle{};
    if (g_hook->create(g_hook->user, &request, &original, &handle).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }
    g_gate_original = reinterpret_cast<void*>(original);
    g_gate_hook = handle;
    const HMODULE mod = GetModuleHandleA("HTGame.exe");
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
    char buf[200]{};
    std::snprintf(buf, sizeof(buf), "damage-chain: gate hook rva=0x%llX",
                  static_cast<unsigned long long>(base != 0 ? target - base : target));
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

// 给对象打上/清除 RF_MarkAsRootSet 标记。仅在确认是 UObject（虚表在游戏映像内）时操作。
bool SetRootFlag(void* object, bool on) {
    if (!LooksLikeLiveObject(object)) return false;
    __try {
        auto* flags = reinterpret_cast<std::uint32_t*>(
            static_cast<unsigned char*>(object) + kObjectFlagsOffset);
        if (on) {
            *flags |= kRfMarkAsRootSet;
        } else {
            *flags &= ~kRfMarkAsRootSet;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClearRoots() {
    for (int i = 0; i < g_rooted_count; ++i) {
        if (g_rooted[i] != nullptr) SetRootFlag(g_rooted[i], false);
        g_rooted[i] = nullptr;
    }
    g_rooted_count = 0;
}

// 把参与本次判定的 4 个参数对象钉住，避免补发时已被回收。
void HoldArgs() {
    ClearRoots();
    std::uintptr_t args[4] = {g_capture.reg[0], g_capture.reg[1], g_capture.reg[2],
                              g_capture.reg[3]};
    for (int i = 0; i < 4; ++i) {
        auto* p = reinterpret_cast<void*>(args[i]);
        if (p == nullptr) continue;
        if (SetRootFlag(p, true)) {
            g_rooted[g_rooted_count++] = p;
        }
    }
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
    if (ReadField(reinterpret_cast<const void*>(g_capture.reg[1]), 0xa8) == 0) {
        g_armed = false;  // 命中目标已被清空（目标死亡/离开），停止循环
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

    {
        static int tick = 0;
        if ((tick++ % 300) == 0 && g_call_total != 0) {
            const HMODULE m2 = GetModuleHandleA("HTGame.exe");
            const std::uintptr_t b2 = reinterpret_cast<std::uintptr_t>(m2);
            char t2[320]{};
            std::snprintf(t2, sizeof(t2),
                          "damage-chain: calls total=%llu valid=%llu nullargs=%llu "
                          "notarget=%llu caller=0x%llX",
                          g_call_total, g_call_valid, g_call_null_args, g_call_no_target,
                          static_cast<unsigned long long>(b2 != 0 && g_burst_caller != 0
                                                              ? g_burst_caller - b2
                                                              : 0));
            LogInfo(t2);
        }
    }

    if (!g_burst_caller_logged && g_burst_caller != 0) {
        g_burst_caller_logged = true;
        const HMODULE mod = GetModuleHandleA("HTGame.exe");
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), "damage-chain: burst caller rva=0x%llX",
                      static_cast<unsigned long long>(base != 0 ? g_burst_caller - base
                                                                : g_burst_caller));
        LogInfo(buf);
    }
    if (g_stub == nullptr) BuildStubs();
    EnsureHook();
    if (g_gate_hook.id == 0) EnsureGateHook();
    if (g_conf.enabled != 0) {
        Pump();
    } else if (g_armed) {
        g_armed = false;
    }
}

#define HAS(service, member) \
    ((service) != nullptr && (service)->member != nullptr)

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui_v1) noexcept {
    const AnomalyUiServiceV1* ui = (ui_v1 != nullptr) ? ui_v1 : g_ui;
    if (!HAS(ui, begin_window) || !HAS(ui, end_window)) return;

    if (HAS(ui, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 612.0F, 172.0F, kCondFirstUseEver);
    }

    // 生命周期：end_window 必须无条件调用，即使 begin_window 返回 0，
    // 否则框架的 UI 栈不平衡，后续帧会失效。
    int open = 1;
    if (ui->begin_window(ui->user, anomaly::sdk::StringView("倍攻连锁反应"), &open, 0u) != 0) {
        char buf[192]{};

        int enabled = g_conf.enabled;
        if (HAS(ui, checkbox) &&
            ui->checkbox(ui->user, anomaly::sdk::StringView("倍攻连锁反应"), &enabled) != 0) {
            g_conf.enabled = enabled;
        }

        int chain = g_conf.enabled;
        if (HAS(ui, checkbox) &&
            ui->checkbox(ui->user, anomaly::sdk::StringView("启用"), &chain) != 0) {
            g_conf.enabled = chain;
        }

        std::uint32_t rate = static_cast<std::uint32_t>(g_conf.rate > 0 ? g_conf.rate : 1);
        if (HAS(ui, input_uint32) &&
            ui->input_uint32(ui->user, anomaly::sdk::StringView("伤害重复次数"), &rate, 1u,
                             10u) != 0) {
            if (rate < 1u) rate = 1u;
            if (rate > 200u) rate = 200u;
            g_conf.rate = static_cast<int>(rate);
            g_period_ms = 1000.0 / static_cast<double>(rate);
        }

        std::snprintf(buf, sizeof(buf), "绑定=%s  触发=%s",
                      g_hook_handle.id != 0 ? "就绪" : "未就绪",
                      g_armed ? "战斗中" : "待机");
        if (HAS(ui, text)) ui->text(ui->user, anomaly::sdk::StringView(buf));

        std::snprintf(buf, sizeof(buf), "伤害=%llu  失败=%llu  重放=%llu", g_hits, g_faults,
                      g_pulses);
        if (HAS(ui, text)) ui->text(ui->user, anomaly::sdk::StringView(buf));
    }
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
                   anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("3.6.1"),
                   Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
