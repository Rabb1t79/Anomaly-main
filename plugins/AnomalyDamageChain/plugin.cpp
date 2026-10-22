#include "anomaly/sdk/cpp.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using anomaly::sdk::Host;
using anomaly::sdk::StringView;

constexpr std::uint32_t kCondFirstUseEver = 4u;

// ---------------------------------------------------------------- 配置

struct Config {
    int enabled{1};
    int rate{15};  // 每秒重放次数
};
Config g_conf{};
double g_now_ms{};

// ---------------------------------------------------------------- 服务

const AnomalyHostApiV1* g_host{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyUiServiceV1* g_ui{};
const AnomalySignatureServiceV1* g_signature{};
const AnomalyHookServiceV1* g_hook{};
const AnomalyNteDamageChainServiceV1* g_damage_chain{};

// ---------------------------------------------------------------- 目标

// 外层：每次伤害施加都会被调用（RVA 0x7682FF0，经虚表槽 0x318 分发）。
// 内层：判定函数，返回本次是否真正施加伤害（RVA 0x767F0D0）。
// 签名取自 Tokky 的 AOB 表 pair2，已在当前版本验证唯一命中。
constexpr const char* kBurstPattern =
    "48 85 D2 0F 84 41 01 00 00 57 48 83 EC 60 48 8B F9 4D 85 C0 0F 84 2B 01 00 00 "
    "48 89 5C 24 70 48";
constexpr const char* kGatePattern =
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A9 "
    "48 81 EC D0";

// ---------------------------------------------------------------- 参数抓取

// Win64 入口布局：[rsp]=返回地址，[rsp+8..0x28)=寄存器参数影子空间，
// 真正的栈参数从 [rsp+0x28) 开始。
constexpr int kStackArgCount = 16;

struct Capture {
    std::uintptr_t entry_rsp;              // 0x00
    std::uintptr_t ret_addr;               // 0x08
    std::uintptr_t reg[4];                 // 0x10
    std::uintptr_t stack[kStackArgCount];  // 0x30
    unsigned char xmm[6][16];              // 0xB0：XMM0-XMM5
};
constexpr std::size_t kCaptureXmmOffset = 0xB0;

Capture g_capture{};       // 外层参数，补发使用
Capture g_gate_capture{};  // 判定参数，每次调用前重新捕获

AnomalyGenerationHandleV1 g_burst_lease{};  // 外层 detour 的回调契约
AnomalyGenerationHandleV1 g_gate_lease{};   // 判定 detour 的回调契约
DWORD g_game_thread{};                      // Start 时记录的线程，回调需比对

void* g_original{};
void* g_gate_original{};
std::uintptr_t g_gate_result{};

void* g_stub{};
void* g_replay{};
void* g_gate_stub{};
std::size_t g_stub_bytes{};

AnomalyGenerationHandleV1 g_burst_hook{};
AnomalyGenerationHandleV1 g_gate_hook{};

// ---------------------------------------------------------------- 运行状态

bool g_running{};
bool g_in_replay{};  // 重放期间不把 Gate 结果当新链   // Start 后为真，Stop 后为假：判定回调只在运行期生效
bool g_armed{};
double g_period_ms{1000.0 / 15.0};
double g_next_due{};
double g_last_live_ms{-1.0e9};

// 每条链独立的补发计数（重新命中时归零），上限防止异常情况下无限循环。
constexpr unsigned long long kMaxPulsesPerChain = 600;
unsigned long long g_chain_pulses{};

unsigned long long g_hits{};
unsigned long long g_pulses{};
unsigned long long g_faults{};
unsigned long long g_calls{};
unsigned long long g_calls_valid{};

std::uintptr_t g_last_rdx{};
std::uintptr_t g_last_r8{};
std::uintptr_t g_last_target{};
int g_last_gate{-1};

// ---------------------------------------------------------------- 基础工具

void LogInfo(const char* text) {
    if (g_core == nullptr || g_core->log == nullptr || text == nullptr) return;
    g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(text));
}

void LogWarn(const char* text) {
    if (g_core == nullptr || g_core->log == nullptr || text == nullptr) return;
    g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, StringView(text));
}

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

// ---------------------------------------------------------------- 回调前置声明

extern "C" void OnBurstEnter();
extern "C" void OnBurstExit();
extern "C" void OnGateEnter();
extern "C" void OnGateExit();

// ---------------------------------------------------------------- stub 生成

// 生成三段位置无关的转发代码。约定：
//   - 所有 stub 保存并恢复 RSI/RDI（Win64 非易失）。
//   - 入口转发额外保存 XMM0-XMM5（易失浮点参数寄存器），保证原函数看到原值。
//   - 栈参数按 [rsp+0x28] 读取，构造调用帧时放到 [rsp+0x20]。
// 动态代码需要注册展开信息，否则异常穿过手写帧时展开器会失败。
using RtlAddTableFn = unsigned char (*)(void*, unsigned long, unsigned long long);
using RtlDeleteTableFn = unsigned char (*)(void*);
RtlAddTableFn g_rtl_add{};
RtlDeleteTableFn g_rtl_delete{};
void* g_function_table{};

void* g_stub_mem{};

void ResolveUnwindApi() {
    if (g_rtl_add != nullptr) return;
    const HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32 == nullptr) return;
    g_rtl_add = reinterpret_cast<RtlAddTableFn>(
        reinterpret_cast<void*>(GetProcAddress(k32, "RtlAddFunctionTable")));
    g_rtl_delete = reinterpret_cast<RtlDeleteTableFn>(
        reinterpret_cast<void*>(GetProcAddress(k32, "RtlDeleteFunctionTable")));
}

bool BuildStubs() {
    if (g_stub != nullptr) return true;

    constexpr std::size_t kSlot = 1024;
    constexpr std::size_t kSlots = 3;
    constexpr std::size_t kUnwindBytes = 32;
    constexpr std::size_t kFuncEntry = 12;
    const std::size_t total = kSlot * kSlots + kUnwindBytes * kSlots + kFuncEntry * kSlots;
    auto* mem = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (mem == nullptr) return false;

    std::uint8_t* const s1 = mem;
    std::uint8_t* const s2 = mem + kSlot;
    std::uint8_t* const s3 = mem + kSlot * 2;

    auto imm64 = [](std::uint8_t* d, std::size_t& i, std::uint64_t v) {
        std::memcpy(d + i, &v, sizeof(v));
        i += sizeof(v);
    };
    auto rax_imm = [&](std::uint8_t* d, std::size_t& i, const void* addr) {
        d[i++] = 0x48; d[i++] = 0xB8;
        imm64(d, i, reinterpret_cast<std::uint64_t>(addr));
    };
    auto ld8 = [](std::uint8_t* d, std::size_t& i, std::uint8_t rex, std::uint8_t modrm,
                  std::uint8_t disp) {
        d[i++] = rex; d[i++] = 0x8B; d[i++] = modrm; d[i++] = disp;
    };
    auto movsq = [](std::uint8_t* d, std::size_t& i) {
        d[i++] = 0xB9; d[i++] = static_cast<std::uint8_t>(kStackArgCount);
        d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;
        d[i++] = 0xF3; d[i++] = 0x48; d[i++] = 0xA5;
    };
    // 0xA8 帧：[rsp+0x00..0x20) 影子空间，[rsp+0x80] 契约槽，[rsp+0x88] 返回值槽
    constexpr std::uint32_t kFrame = 0xA8;
    constexpr std::uint8_t kLeaseLo = 0x80;
    constexpr std::uint8_t kRetLo = 0x88;

    auto prologue = [&](std::uint8_t* d, std::size_t& i) {
        d[i++] = 0x49; d[i++] = 0x89; d[i++] = 0xE3;      // mov r11,rsp
        d[i++] = 0x57;                                    // push rdi
        d[i++] = 0x56;                                    // push rsi
        d[i++] = 0x48; d[i++] = 0x81; d[i++] = 0xEC;      // sub rsp,0xA8
        d[i++] = static_cast<std::uint8_t>(kFrame & 0xFF);
        d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;
    };
    auto epilogue = [&](std::uint8_t* d, std::size_t& i) {
        d[i++] = 0x48; d[i++] = 0x81; d[i++] = 0xC4;      // add rsp,0xA8
        d[i++] = static_cast<std::uint8_t>(kFrame & 0xFF);
        d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;
        d[i++] = 0x5E;                                    // pop rsi
        d[i++] = 0x5F;                                    // pop rdi
    };
    auto capture = [&](std::uint8_t* d, std::size_t& i, Capture* dst) {
        rax_imm(d, i, dst);
        d[i++] = 0x4C; d[i++] = 0x89; d[i++] = 0x18;
        d[i++] = 0x4D; d[i++] = 0x8B; d[i++] = 0x13;
        d[i++] = 0x4C; d[i++] = 0x89; d[i++] = 0x50; d[i++] = 0x08;
        d[i++] = 0x48; d[i++] = 0x89; d[i++] = 0x48; d[i++] = 0x10;
        d[i++] = 0x48; d[i++] = 0x89; d[i++] = 0x50; d[i++] = 0x18;
        d[i++] = 0x4C; d[i++] = 0x89; d[i++] = 0x40; d[i++] = 0x20;
        d[i++] = 0x4C; d[i++] = 0x89; d[i++] = 0x48; d[i++] = 0x28;
        d[i++] = 0x49; d[i++] = 0x8D; d[i++] = 0x73; d[i++] = 0x28;
        d[i++] = 0x48; d[i++] = 0x8D; d[i++] = 0x78; d[i++] = 0x30;
        movsq(d, i);
    };
    auto xmm_io = [&](std::uint8_t* d, std::size_t& i, Capture* base, std::uint8_t opcode) {
        rax_imm(d, i, base);
        for (int x = 0; x < 6; ++x) {
            d[i++] = 0xF3; d[i++] = 0x0F; d[i++] = opcode;
            d[i++] = static_cast<std::uint8_t>(0x80 | (x << 3));
            const std::uint32_t off = static_cast<std::uint32_t>(kCaptureXmmOffset + x * 16);
            d[i++] = static_cast<std::uint8_t>(off & 0xFF);
            d[i++] = static_cast<std::uint8_t>((off >> 8) & 0xFF);
            d[i++] = static_cast<std::uint8_t>((off >> 16) & 0xFF);
            d[i++] = static_cast<std::uint8_t>((off >> 24) & 0xFF);
        }
    };
    auto load_regs = [&](std::uint8_t* d, std::size_t& i, Capture* src) {
        rax_imm(d, i, src);
        ld8(d, i, 0x48, 0x48, 0x10);
        ld8(d, i, 0x48, 0x50, 0x18);
        ld8(d, i, 0x4C, 0x40, 0x20);
        ld8(d, i, 0x4C, 0x48, 0x28);
    };
    auto place_args = [&](std::uint8_t* d, std::size_t& i, Capture* src) {
        rax_imm(d, i, src);
        d[i++] = 0x48; d[i++] = 0x8D; d[i++] = 0x70; d[i++] = 0x30;
        d[i++] = 0x48; d[i++] = 0x8D; d[i++] = 0x7C; d[i++] = 0x24; d[i++] = 0x20;
        movsq(d, i);
    };
    // 调用一个无参回调
    auto call0 = [&](std::uint8_t* d, std::size_t& i, const void* fn) {
        rax_imm(d, i, fn);
        d[i++] = 0xFF; d[i++] = 0xD0;
    };
    // 调原函数并保存返回值，随后归还契约、返回原值
    auto call_original = [&](std::uint8_t* d, std::size_t& i, void** original,
                             const void* exit_fn) {
        rax_imm(d, i, original);
        d[i++] = 0x48; d[i++] = 0x8B; d[i++] = 0x00;      // mov rax,[rax]
        d[i++] = 0x48; d[i++] = 0x85; d[i++] = 0xC0;      // test rax,rax
        d[i++] = 0x74; const std::size_t jz = i; d[i++] = 0x00;
        d[i++] = 0xFF; d[i++] = 0xD0;                     // call rax
        d[i++] = 0x48; d[i++] = 0x89; d[i++] = 0x84; d[i++] = 0x24;
        d[i++] = kRetLo; d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;  // [rsp+0x88]=rax
        d[i++] = 0xEB; const std::size_t jmp = i; d[i++] = 0x00;
        const std::size_t no_orig = i;
        d[i++] = 0x48; d[i++] = 0xC7; d[i++] = 0x84; d[i++] = 0x24;
        d[i++] = kRetLo; d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;
        d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;    // [rsp+0x88]=0
        const std::size_t finish = i;
        call0(d, i, exit_fn);                             // 归还契约
        d[i++] = 0x48; d[i++] = 0x8B; d[i++] = 0x84; d[i++] = 0x24;
        d[i++] = kRetLo; d[i++] = 0x00; d[i++] = 0x00; d[i++] = 0x00;  // rax=[rsp+0x88]
        d[jz] = static_cast<std::uint8_t>(no_orig - (jz + 1));
        d[jmp] = static_cast<std::uint8_t>(finish - (jmp + 1));
    };

    // ===== s1：外层 detour（领契约 → 调原函数 → 还契约）=====
    std::size_t i = 0;
    prologue(s1, i);
    capture(s1, i, &g_capture);
    xmm_io(s1, i, &g_capture, 0x7F);          // 保存 XMM
    call0(s1, i, &OnBurstEnter);              // 领契约
    xmm_io(s1, i, &g_capture, 0x6F);          // 恢复 XMM
    load_regs(s1, i, &g_capture);
    call_original(s1, i, &g_original, &OnBurstExit);
    epilogue(s1, i);
    s1[i++] = 0xC3;

    // ===== s2：重放（不领契约，由 Update 侧保证）=====
    std::size_t j = 0;
    prologue(s2, j);
    place_args(s2, j, &g_capture);
    load_regs(s2, j, &g_capture);
    xmm_io(s2, j, &g_capture, 0x6F);
    rax_imm(s2, j, &g_original);
    s2[j++] = 0x48; s2[j++] = 0x8B; s2[j++] = 0x00;
    s2[j++] = 0x48; s2[j++] = 0x85; s2[j++] = 0xC0;
    s2[j++] = 0x74; const std::size_t s2_jz = j; s2[j++] = 0x00;
    s2[j++] = 0xFF; s2[j++] = 0xD0;
    epilogue(s2, j);
    s2[j++] = 0xC3;
    const std::size_t s2_fail = j;
    epilogue(s2, j);
    s2[j++] = 0xC3;
    s2[s2_jz] = static_cast<std::uint8_t>(s2_fail - (s2_jz + 1));

    // ===== s3：判定 detour（领契约 → 调原函数 → 记录结果 → 还契约）=====
    std::size_t k = 0;
    prologue(s3, k);
    capture(s3, k, &g_gate_capture);
    xmm_io(s3, k, &g_gate_capture, 0x7F);
    call0(s3, k, &OnGateEnter);               // 领契约
    place_args(s3, k, &g_gate_capture);
    load_regs(s3, k, &g_gate_capture);
    xmm_io(s3, k, &g_gate_capture, 0x6F);
    // 记录返回值到 g_gate_result，供 OnGateExit 判定
    rax_imm(s3, k, &g_gate_original);
    s3[k++] = 0x48; s3[k++] = 0x8B; s3[k++] = 0x00;
    s3[k++] = 0x48; s3[k++] = 0x85; s3[k++] = 0xC0;
    s3[k++] = 0x74; const std::size_t s3_jz = k; s3[k++] = 0x00;
    s3[k++] = 0xFF; s3[k++] = 0xD0;                     // call rax
    s3[k++] = 0x49; s3[k++] = 0x89; s3[k++] = 0xC2;     // mov r10,rax
    s3[k++] = 0xEB; const std::size_t s3_jmp = k; s3[k++] = 0x00;
    const std::size_t s3_no_orig = k;
    s3[k++] = 0x45; s3[k++] = 0x31; s3[k++] = 0xD2;     // xor r10d,r10d
    const std::size_t s3_store = k;
    rax_imm(s3, k, &g_gate_result);
    s3[k++] = 0x4C; s3[k++] = 0x89; s3[k++] = 0x10;     // mov [rax],r10
    call0(s3, k, &OnGateExit);                          // 归还契约 + 判定
    rax_imm(s3, k, &g_gate_result);
    s3[k++] = 0x48; s3[k++] = 0x8B; s3[k++] = 0x00;
    epilogue(s3, k);
    s3[k++] = 0xC3;
    s3[s3_jz] = static_cast<std::uint8_t>(s3_no_orig - (s3_jz + 1));
    s3[s3_jmp] = static_cast<std::uint8_t>(s3_store - (s3_jmp + 1));

    // ===== 展开信息：三个 stub 序言一致（mov r11,rsp / push rdi / push rsi / sub rsp,0xA8）=====
    ResolveUnwindApi();
    std::uint8_t* unwind = mem + kSlot * kSlots;
    auto* funcs = reinterpret_cast<std::uint32_t*>(unwind + kUnwindBytes * kSlots);
    auto write_unwind = [&](std::size_t index, std::size_t code_size) {
        std::uint8_t* u = unwind + index * kUnwindBytes;
        const std::uint32_t units = kFrame / 8;
        u[0] = 0x01;                       // Version=1
        u[1] = 0x0C;                       // SizeOfProlog = 12
        u[2] = 0x04;                       // CountOfCodes
        u[3] = 0x00;
        u[4] = 0x0C; u[5] = 0x01;          // UWOP_ALLOC_LARGE, OpInfo=0
        u[6] = static_cast<std::uint8_t>(units & 0xFF);
        u[7] = static_cast<std::uint8_t>((units >> 8) & 0xFF);
        u[8] = 0x05; u[9] = 0x06;          // push rsi
        u[10] = 0x04; u[11] = 0x07;        // push rdi
        std::uint32_t* f = funcs + index * 3;
        f[0] = static_cast<std::uint32_t>(index * kSlot);
        f[1] = static_cast<std::uint32_t>(index * kSlot + code_size);
        f[2] = static_cast<std::uint32_t>((unwind + index * kUnwindBytes) - mem);
    };
    write_unwind(0, i);
    write_unwind(1, j);
    write_unwind(2, k);

    if (g_rtl_add == nullptr ||
        g_rtl_add(funcs, 3, reinterpret_cast<unsigned long long>(mem)) == 0) {
        LogWarn("damage-chain: RtlAddFunctionTable failed, stub disabled");
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }
    g_function_table = funcs;

    FlushInstructionCache(GetCurrentProcess(), mem, total);
    g_stub = s1;
    g_replay = s2;
    g_gate_stub = s3;
    g_stub_bytes = total;
    g_stub_mem = mem;
    return true;
}

// ---------------------------------------------------------------- 回调
// 按框架约定：detour 必须在 begin_callback / end_callback 范围内执行，
// release 依赖这些契约等待在途调用退出。

extern "C" void OnBurstEnter() {
    g_burst_lease = AnomalyGenerationHandleV1{};
    if (!g_running || g_hook == nullptr || g_burst_hook.id == 0) return;
    if (g_game_thread != 0 && GetCurrentThreadId() != g_game_thread) return;
    static_cast<void>(g_hook->begin_callback(g_hook->user, g_burst_hook, &g_burst_lease));
    ++g_calls;
    if (g_capture.reg[1] != 0 && g_capture.reg[2] != 0) {
        ++g_calls_valid;
        g_last_rdx = g_capture.reg[1];
        g_last_r8 = g_capture.reg[2];
        g_last_target = ReadField(reinterpret_cast<const void*>(g_capture.reg[1]), 0xa8);
    }
}

extern "C" void OnBurstExit() {
    if (g_burst_lease.id != 0 && g_hook != nullptr && g_hook->end_callback != nullptr) {
        static_cast<void>(g_hook->end_callback(g_hook->user, g_burst_lease));
    }
    g_burst_lease = AnomalyGenerationHandleV1{};
}

extern "C" void OnGateEnter() {
    g_gate_lease = AnomalyGenerationHandleV1{};
    if (!g_running || g_hook == nullptr || g_gate_hook.id == 0) return;
    if (g_game_thread != 0 && GetCurrentThreadId() != g_game_thread) return;
    static_cast<void>(g_hook->begin_callback(g_hook->user, g_gate_hook, &g_gate_lease));
}

extern "C" void OnGateExit() {
    const bool applied = (g_gate_result & 0xFFu) != 0;
    g_last_gate = applied ? 1 : 0;
    if (g_running && !g_in_replay && applied) {
        g_armed = true;
        g_chain_pulses = 0;
        g_next_due = g_now_ms + g_period_ms;
        ++g_hits;
    }
    if (g_gate_lease.id != 0 && g_hook != nullptr && g_hook->end_callback != nullptr) {
        static_cast<void>(g_hook->end_callback(g_hook->user, g_gate_lease));
    }
    g_gate_lease = AnomalyGenerationHandleV1{};
}

int ReplayGuarded() {
    __try {
        reinterpret_cast<void (*)()>(g_replay)();
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void Pump() {
    if (!g_running || g_replay == nullptr || !g_armed) return;
    if (g_now_ms < g_next_due) return;

    if (g_chain_pulses >= kMaxPulsesPerChain) {
        g_armed = false;
        LogInfo("damage-chain: per-chain pulse limit reached, chain stopped");
        return;
    }

    if (!LooksLikeLiveObject(reinterpret_cast<const void*>(g_capture.reg[1])) ||
        !LooksLikeLiveObject(reinterpret_cast<const void*>(g_capture.reg[2])) ||
        ReadField(reinterpret_cast<const void*>(g_capture.reg[1]), 0xa8) == 0) {
        g_armed = false;
        return;
    }

    g_in_replay = true;
    const int replay_ok = ReplayGuarded();
    g_in_replay = false;
    if (replay_ok == 0) {
        g_armed = false;
        ++g_faults;
        LogWarn("damage-chain: replay hit invalid state, chain stopped");
        return;
    }
    ++g_chain_pulses;
    g_next_due = g_now_ms + g_period_ms;
    ++g_pulses;
}

// ---------------------------------------------------------------- 挂钩

bool InstallHook(const char* pattern, void* detour, const char* label,
                 AnomalyGenerationHandleV1* handle, void** original) {
    if (handle->id != 0) return true;
    if (g_hook == nullptr || detour == nullptr) return false;

    // 优先使用 Core 依据 Profile 解析并通过校验的地址；服务不可用时退回本地签名，
    // 这样游戏版本变化会先在 Profile 校验阶段暴露，而不是等到钩错地址才崩。
    std::uintptr_t target = 0;
    if (g_damage_chain != nullptr && label != nullptr) {
        if (std::strcmp(label, "burst") == 0 && g_damage_chain->apply_address != nullptr) {
            static_cast<void>(g_damage_chain->apply_address(g_damage_chain->user, &target));
        } else if (std::strcmp(label, "gate") == 0 &&
                   g_damage_chain->gate_address != nullptr) {
            static_cast<void>(g_damage_chain->gate_address(g_damage_chain->user, &target));
        }
    }
    if (target < 0x10000 && g_signature != nullptr) {
        static_cast<void>(g_signature->resolve(g_signature->user, StringView("HTGame.exe"),
                                               StringView(".text"), StringView(pattern),
                                               &target));
    }
    if (target < 0x10000) {
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), "damage-chain: %s address unresolved", label);
        LogWarn(buf);
        return false;
    }

    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = detour;
    request.label = StringView(label);

    std::uintptr_t resolved_original = 0;
    AnomalyGenerationHandleV1 created{};
    const AnomalyStatusV1 status =
        g_hook->create(g_hook->user, &request, &resolved_original, &created);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), "damage-chain: %s hook create failed code=%u", label,
                      static_cast<unsigned>(status.code));
        LogWarn(buf);
        return false;
    }
    *original = reinterpret_cast<void*>(resolved_original);
    *handle = created;

    const HMODULE mod = GetModuleHandleA("HTGame.exe");
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mod);
    char buf[200]{};
    std::snprintf(buf, sizeof(buf), "damage-chain: %s hook rva=0x%llX", label,
                  static_cast<unsigned long long>(base != 0 ? target - base : target));
    LogInfo(buf);
    return true;
}

void ReleaseHook(AnomalyGenerationHandleV1* handle, const char* label) {
    if (handle->id == 0) return;
    if (g_hook != nullptr && g_hook->release != nullptr) {
        const AnomalyStatusV1 status = g_hook->release(g_hook->user, *handle);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            char buf[192]{};
            std::snprintf(buf, sizeof(buf), "damage-chain: %s hook release failed code=%u",
                          label, static_cast<unsigned>(status.code));
            LogWarn(buf);
            return;  // 保留句柄，避免丢失对残留钩子的跟踪
        }
    }
    *handle = AnomalyGenerationHandleV1{};
}

// ---------------------------------------------------------------- 生命周期

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
    g_damage_chain = view
                         .Query<AnomalyNteDamageChainServiceV1>(
                             ANOMALY_NTE_DAMAGE_CHAIN_SERVICE_V1_ID,
                             ANOMALY_NTE_DAMAGE_CHAIN_SERVICE_V1_VERSION)
                         .get();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    const int rate = g_conf.rate > 0 ? g_conf.rate : 1;
    g_period_ms = 1000.0 / static_cast<double>(rate);
    g_chain_pulses = 0;
    g_armed = false;
    g_running = true;
    g_game_thread = GetCurrentThreadId();  // 回调只在该线程上生效
    if (!BuildStubs()) {
        LogWarn("damage-chain: stub allocation failed");
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    g_running = false;
    g_armed = false;
    // 停止即解除钩子，避免禁用状态下仍然拦截游戏的每次伤害调用。
    // 注意：不在此处清空原函数指针——解除过程中可能仍有在途调用，
    // 清空会让它们直接返回 0，导致游戏本身跳过这一次伤害。
    ReleaseHook(&g_gate_hook, "gate");
    ReleaseHook(&g_burst_hook, "burst");
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    g_running = false;
    g_armed = false;
    // 先让 stub 失效，再卸载钩子。
    g_original = nullptr;
    g_gate_original = nullptr;

    ReleaseHook(&g_gate_hook, "gate");
    ReleaseHook(&g_burst_hook, "burst");

    // 两个钩子都成功释放后才回收动态代码；释放失败时保留内存，
    // 避免残留钩子跳进已释放区域。
    if (g_burst_hook.id == 0 && g_gate_hook.id == 0 && g_stub_mem != nullptr) {
        if (g_function_table != nullptr && g_rtl_delete != nullptr) {
            g_rtl_delete(g_function_table);
            g_function_table = nullptr;
        }
        VirtualFree(g_stub_mem, 0, MEM_RELEASE);
        g_stub_mem = nullptr;
        g_stub_bytes = 0;
    }
    g_stub = nullptr;
    g_replay = nullptr;
    g_gate_stub = nullptr;
    g_hook = nullptr;
    g_signature = nullptr;
    g_ui = nullptr;
    g_core = nullptr;
    g_host = nullptr;
}

void ANOMALY_CALL Update(void*, double delta_seconds) {
    g_now_ms += (delta_seconds > 0.0 ? delta_seconds : 0.0) * 1000.0;

    if (g_stub == nullptr) BuildStubs();
    if (g_running) {
        InstallHook(kBurstPattern, g_stub, "burst", &g_burst_hook, &g_original);
        InstallHook(kGatePattern, g_gate_stub, "gate", &g_gate_hook, &g_gate_original);
        if (g_conf.enabled != 0) {
            Pump();
        } else {
            g_armed = false;
        }
    } else {
        g_armed = false;
    }

    if (g_armed && (g_now_ms - g_last_live_ms) >= 250.0) {
        g_last_live_ms = g_now_ms;
        char mon[380]{};
        std::snprintf(mon, sizeof(mon),
                      "damage-chain: live hits=%llu pulses=%llu chain=%llu faults=%llu "
                      "calls=%llu valid=%llu rdx=0x%llX r8=0x%llX tgt=0x%llX gate=%d",
                      g_hits, g_pulses, g_chain_pulses, g_faults, g_calls, g_calls_valid,
                      static_cast<unsigned long long>(g_last_rdx),
                      static_cast<unsigned long long>(g_last_r8),
                      static_cast<unsigned long long>(g_last_target), g_last_gate);
        LogInfo(mon);
    }
}

// ---------------------------------------------------------------- 界面

#define HAS(service, member) ((service) != nullptr && (service)->member != nullptr)

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui_v1) noexcept {
    const AnomalyUiServiceV1* ui = (ui_v1 != nullptr) ? ui_v1 : g_ui;
    if (!HAS(ui, begin_window) || !HAS(ui, end_window)) return;

    if (HAS(ui, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 612.0F, 172.0F, kCondFirstUseEver);
    }

    // end_window 必须无条件调用，否则框架 UI 栈不平衡。
    int open = 1;
    if (ui->begin_window(ui->user, anomaly::sdk::StringView("倍攻连锁反应"), &open, 0u) != 0) {
        char buf[192]{};

        int enabled = g_conf.enabled;
        if (HAS(ui, checkbox) &&
            ui->checkbox(ui->user, anomaly::sdk::StringView("启用"), &enabled) != 0) {
            g_conf.enabled = enabled;
            if (enabled == 0) g_armed = false;
        }

        std::uint32_t rate = static_cast<std::uint32_t>(g_conf.rate > 0 ? g_conf.rate : 1);
        if (HAS(ui, input_uint32) &&
            ui->input_uint32(ui->user, anomaly::sdk::StringView("每秒重放次数"), &rate, 1u,
                             10u) != 0) {
            if (rate < 1u) rate = 1u;
            if (rate > 200u) rate = 200u;
            g_conf.rate = static_cast<int>(rate);
            g_period_ms = 1000.0 / static_cast<double>(rate);
        }

        const bool both = g_burst_hook.id != 0 && g_gate_hook.id != 0;
        std::snprintf(buf, sizeof(buf), "绑定=%s  触发=%s",
                      both ? "就绪" : (g_running ? "等待" : "未运行"),
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
                   anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("6.1.0"),
                   Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
