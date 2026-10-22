#include <windows.h>
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ue5.h"
#include "ue_reflection.hpp"
#include "anomaly/sdk/services/ue5.h"
#include "ue_reflection.hpp"
// AnomalyUltimate — 依据本次逆向证据重建（不沿用旧实现）
// 证据来源：_re/最终验收报告.md、_re/四功能源码还原.md
//  - 描述符字符串：原始 DLL 运行期调用 AnomalyPluginEntryV1 的返回值（RVA 0x495F0/0x49588/0x49608/0x49610）
//  - 服务查询顺序：set_trace 抓到的 on_load 序列
//  - 功能 id/版本：原始描述符填充（RVA 0x22E30/0x29C50/0x32380/0x3C0A0）
//  - 配置 schema / 键：.rdata 明文 + 去混淆字符串
#include "anomaly/sdk/plugin.h"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/platform.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace {

constexpr const char* kId = "anomaly.local.ultimate";
constexpr const char* kName = "AnomalyUltimate";
constexpr const char* kAuthor = "Anomaly";
constexpr const char* kVersion = "1.0.0";

struct FeatureDesc {
    const char* id;
    const char* name;
    const char* version;
    const char* schema_id;
    const char* schema_json;
    void (*on_load)(const AnomalyHostApiV1*, void**);
    void (*on_start)(void*);
    void (*on_stop)(void*);
    void (*on_unload)(void*);
};

AnomalyStatusV1 ok() { AnomalyStatusV1 s{}; s.code = ANOMALY_STATUS_V1_OK; return s; }
AnomalyStatusV1 fail(uint32_t code, const char* msg) {
    AnomalyStatusV1 s{}; s.code = code; s.message.data = msg;
    s.message.size = msg ? std::strlen(msg) : 0; return s;
}

const void* Query(const AnomalyHostApiV1* host, const char* id, uint32_t min_version) {
    if (!host || !host->query_service) return nullptr;
    const void* service = nullptr;
    AnomalyStringViewV1 view{id, std::strlen(id)};
    if (host->query_service(host->host_context, view, min_version, &service).code !=
        ANOMALY_STATUS_V1_OK)
        return nullptr;
    return service;
}

// 配置 schema（明文来自 .rdata：0x4C660 / 0x4D200 / 0x4D6D0 / 0x4F2C8）
constexpr const char* kChronosSchema =
    R"({"type":"object","additionalProperties":false,"required":["durationSeconds"],)"
    R"("properties":{"durationSeconds":{"type":"integer","minimum":1},)"
    R"("timedPauseHotkeyVirtualKey":{"type":"integer"},"timedPauseHotkeyModifiers":{"type":"integer"},)"
    R"("infinitePauseToggleHotkeyVirtualKey":{"type":"integer"},)"
    R"("infinitePauseToggleHotkeyModifiers":{"type":"integer"}}})";
constexpr const char* kDamageSchema =
    R"({"type":"object","additionalProperties":false,"required":["replayCount"],)"
    R"("properties":{"replayCount":{"type":"integer","minimum":0,"maximum":1024},)"
    R"("chainReaction":{"type":"boolean"}}})";
constexpr const char* kVolleyballSchema =
    R"({"type":"object","additionalProperties":false,"properties":)"
    R"({"hotkey":{"type":"string"},"anomaly-ultimate-tabs":{"type":"string"}}})";
constexpr const char* kGmSchema =
    R"({"type":"object","additionalProperties":false,"properties":)"
    R"({"hotkey":{"type":"string"}}})";

// 每个功能在 on_load 中先校验依赖，缺失时以带原因的 UNAVAILABLE 降级（不猜测绑定）
struct Ctx {
    const AnomalyHostApiV1* host;
    const void* core;
    const void* config;
    const void* signature;
    const void* ue5_framework;
    const void* ue5_names;
    const void* ue5_objects;
    const void* ui;
    const void* nte_combat;
    const void* patch;
    const void* process_event;
    const void* hook;
    const void* damage_chain;
};

Ctx* g_ctx = nullptr;
const void* LazyService(const void** slot, const char* id, uint32_t min_version);
void TryQueryIsGM();
void ANOMALY_CALL OnProcessEvent(void*, uintptr_t, uintptr_t, void*);
static int g_pe_subscribed = 0;
static int g_patch_rc = -1;
static int g_hook_ok = 0;
static int g_dmg_chain_ok = 0;
static int g_hook2_ok = 0;
int GmToggleOn();
static int g_sig_ok = 0;
static int g_load_count = 0;
static unsigned long long g_probe = 0;
static int g_find_rc = -1;
static unsigned int g_id_pc = 0, g_id_asc = 0, g_id_vb = 0;
static unsigned long long g_cls_pc = 0;
static unsigned long long g_cls_asc = 0;
static unsigned long long g_cls_vb = 0;
static unsigned int g_probe_nid = 0;
static unsigned int g_want_id = 0;
static int g_draw_count = 0;
static int g_names_ok = 0;
void TickFeatures();
bool InitUe(const void* signature_service, const void* names_service);

void ChronosLoad(const AnomalyHostApiV1*, void** out) {
    if (!g_ctx || !g_ctx->signature) { *out = nullptr; return; }
    *out = g_ctx;  // 目标解析（PausedGameExcludePlayerByTimer）需运行时签名，见 manifest
}
void DamageLoad(const AnomalyHostApiV1*, void** out) {
    if (!g_ctx || !g_ctx->nte_combat) { *out = nullptr; return; }
    *out = g_ctx;
}
void VolleyLoad(const AnomalyHostApiV1*, void** out) {
    if (!g_ctx || !g_ctx->ue5_framework) { *out = nullptr; return; }
    *out = g_ctx;
}
void GmLoad(const AnomalyHostApiV1*, void** out) {
    if (!g_ctx || !g_ctx->signature) { *out = nullptr; return; }
    *out = g_ctx;
}
void Noop(void*) {}

AnomalyStatusV1 ANOMALY_CALL on_load(const AnomalyHostApiV1* host, void** out) {
    ++g_load_count;
    if (!host || !out) return fail(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host is invalid");
    *out = nullptr;

    auto* ctx = new Ctx{};
    ctx->host = host;
    // 原始 on_load 的服务查询顺序（set_trace 实测）
    ctx->config       = Query(host, "anomaly.config", 1);
    ctx->core         = Query(host, "anomaly.core", 1);
    ctx->signature    = Query(host, "anomaly.interop.signature", 1);
    ctx->ue5_framework= Query(host, "anomaly.ue5.framework", 1);
    ctx->ue5_names    = Query(host, "anomaly.ue5.names", 2);
    ctx->ue5_objects  = Query(host, "anomaly.ue5.objects", 1);
    ctx->ui           = Query(host, "anomaly.ui", 1);
    ctx->nte_combat   = Query(host, "anomaly.nte.combat", 1);
    ctx->patch        = Query(host, "anomaly.interop.patch", 1);
    ctx->process_event= Query(host, "anomaly.ue5.process-event", 1);
    g_ctx = ctx;
    InitUe(ctx->signature, ctx->ue5_names);

    if (!ctx->config) { delete ctx; g_ctx = nullptr;
        return fail(ANOMALY_STATUS_V1_UNAVAILABLE, "config service is unavailable"); }

    void* unused = nullptr;
    ChronosLoad(host, &unused);
    DamageLoad(host, &unused);
    VolleyLoad(host, &unused);
    GmLoad(host, &unused);

    *out = ctx;
    return ok();
}

AnomalyStatusV1 ANOMALY_CALL on_start(void* ctx) { return ctx ? ok() : fail(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is invalid"); }
AnomalyStatusV1 ANOMALY_CALL on_stop(void* ctx, uint32_t) { if (!ctx) return ok(); return ok(); }
void ANOMALY_CALL on_unload(void* ctx) { if (ctx == nullptr) return; if (ctx == g_ctx) g_ctx = nullptr; delete static_cast<Ctx*>(ctx); }
void ANOMALY_CALL on_update(void*, double) { static int n = 0; if ((++n % 30) == 0) TickFeatures(); }
// 窗口与控件状态（可持久化到 anomaly.config）
struct UiState {
    int chronos = 0;
    int damage = 0;
    int chain = 0;
    int volley = 0;
    int gm = 0;
    float replay = 32.0f;
};
UiState g_ui_state;
int GmToggleOn() { return g_ui_state.gm; }
bool g_ui_dirty = false;

constexpr const char* kWindowTitle = "AnomalyUltimate##";
constexpr const char* kDamageSchemaId = "anomaly.ultimate.damage-replay.settings";

void SaveDamageConfig() {
    if (!g_ctx || !g_ctx->config) return;
    const auto* cfg = static_cast<const AnomalyConfigServiceV1*>(g_ctx->config);
    if (!cfg->write_atomic) return;
    char doc[192];
    int n = snprintf(doc, sizeof(doc), "{\"replayCount\":%d,\"chainReaction\":%s}",
                     static_cast<int>(g_ui_state.replay),
                     g_ui_state.chain ? "true" : "false");
    if (n <= 0) return;
    AnomalyStringViewV1 id{kDamageSchemaId, std::strlen(kDamageSchemaId)};
    AnomalyByteSpanV1 span{reinterpret_cast<const std::uint8_t*>(doc),
                           static_cast<std::size_t>(n)};
    cfg->write_atomic(cfg->user, id, 1, span);
    g_ui_dirty = false;
}


static ue::Reader g_ue;
int g_sig_svc = 0;
int g_sig_rc = -1;
static unsigned long long g_gworld_instr = 0;
static unsigned long long g_pe_addr = 0;
static unsigned long long g_gworld_ptr = 0;
unsigned long long g_sig_addr = 0;
void TickFeatures();

// 每帧解析四个功能的目标 UClass/UFunction（结果反映到 UI 状态行）。
// 解析使用 GObjects 访问器（签名）+ anomaly.ue5.names 的 name_id 比对。
struct ResolveState {
    int    tried = 0;
    int    sig_ok = 0;
    int    names_ok = 0;
    int    chronos = 0;
    int    damage = 0;
    int    volley = 0;
    int    gm = 0;
};
ResolveState g_resolve;


// 用宿主 objects 服务按对象路径取 name_id（不依赖 find_utf8）
unsigned int FindNameIdByPath(const char* path) {
    if (!g_ctx || !g_ctx->ue5_objects || !path) return 0;
    const auto* objs = static_cast<const AnomalyUe5ObjectsServiceV1*>(g_ctx->ue5_objects);
    if (!objs->find_exact || !objs->snapshot_by_handle) return 0;
    // BetterPose 同款：先校验结构体确实包含这些字段（避免旧版服务结构越界）
    if (objs->struct_size <
        offsetof(AnomalyUe5ObjectsServiceV1, find_exact) + sizeof(objs->find_exact)) return 0;
    if (objs->struct_size <
        offsetof(AnomalyUe5ObjectsServiceV1, snapshot_by_handle) +
            sizeof(objs->snapshot_by_handle)) return 0;
    AnomalyGenerationHandleV1 handle{};
    AnomalyStringViewV1 view{path, std::strlen(path)};
    g_find_rc = (int)objs->find_exact(objs->user, view, &handle).code;
    if (g_find_rc != ANOMALY_STATUS_V1_OK) return 0;
    AnomalyUe5ObjectSnapshotV1 snap{};
    snap.struct_size = sizeof(snap);
    g_find_rc = (int)objs->snapshot_by_handle(objs->user, handle, &snap).code;
    if (g_find_rc != ANOMALY_STATUS_V1_OK) return 0;
    return snap.name_id;
}


// 用 names 服务的 resolve_utf8 把 FName id 解析成文本，用于核对编码
void DecodeName(unsigned int id, char* out, unsigned long long cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!g_ctx || !g_ctx->ue5_names) return;
    const auto* n = static_cast<const AnomalyUe5NamesServiceV1*>(g_ctx->ue5_names);
    if (!n->resolve_utf8) return;
    std::size_t size = static_cast<std::size_t>(cap);
    n->resolve_utf8(n->user, id, out, &size);
    if (size < cap) out[size] = 0;
}


// 遍历 UClass 的 Children（UStruct+0x48 / UField+0x28），用 resolve_utf8 解码名字比对，
// 不依赖 find_utf8（该接口在本宿主上不可用）。
void* FindFuncByName(void* uclass, const char* fname) {
    if (!uclass || !fname) return nullptr;
    std::uintptr_t child = 0;
    if (!ue::Reader::SafePtr(reinterpret_cast<std::uintptr_t>(uclass) + 0x48, &child)) return nullptr;
    char buf[128];
    for (int guard = 0; guard < 8192 && child != 0; ++guard) {
        unsigned int id = 0;
        if (ue::Reader::SafeU32(child + 0x18, &id)) {
            DecodeName(id, buf, sizeof(buf));
            if (buf[0] != 0 && std::strcmp(buf, fname) == 0) return reinterpret_cast<void*>(child);
        }
        std::uintptr_t next = 0;
        if (!ue::Reader::SafePtr(child + 0x28, &next)) return nullptr;
        child = next;
    }
    return nullptr;
}


// 在 GObjects 中按 Class 指针找实例（跳过 CDO "Default__*"）
using PE_FN = void (*)(void*, void*, void*);
PE_FN ReadProcessEvent(void* object);

void* FindInstanceOfClass(void* cls) {
    if (!cls) return nullptr;
    char buf[128];
    const unsigned int limit = g_ue.object_limit < 400000u ? g_ue.object_limit : 400000u;
    for (unsigned int i = 0; i < limit; ++i) {
        void* obj = g_ue.ObjectAt(i);
        if (!obj) continue;
        std::uintptr_t c = 0;
        if (!ue::Reader::SafePtr(reinterpret_cast<std::uintptr_t>(obj) + 0x10, &c)) continue;
        if (reinterpret_cast<void*>(c) != cls) continue;
        unsigned int id = 0;
        if (ue::Reader::SafeU32(reinterpret_cast<std::uintptr_t>(obj) + 0x18, &id)) {
            DecodeName(id, buf, sizeof(buf));
            if (std::strncmp(buf, "Default__", 9) == 0) continue;
        }
        return obj;
    }
    return nullptr;
}

void* g_pc_inst = nullptr;
void* g_isgm_fn = nullptr;
int   g_isgm_result = -1;
int   g_isgm_called = 0;

void TryQueryIsGM() {
    if (g_isgm_called || !g_pc_inst || !g_isgm_fn) return;
    if (!g_ctx || !g_ctx->ue5_framework) return;
    const auto* fw = static_cast<const AnomalyUe5FrameworkServiceV1*>(g_ctx->ue5_framework);
    if (!fw->is_game_thread || !fw->is_game_thread(fw->user)) return;
    g_isgm_called = 1;
    struct { unsigned char ret; } params{0};
    auto process = g_pe_addr != 0
        ? reinterpret_cast<PE_FN>(g_pe_addr)
        : ReadProcessEvent(g_pc_inst);
    if (process) {
        process(g_pc_inst, g_isgm_fn, &params);
        g_isgm_result = params.ret ? 1 : 0;
    }
}

static int g_scan_budget = 0;

// 引用链（偏移来自 5.6.1/CppSDK/SDK/Engine_classes.hpp）：
//   UWorld::OwningGameInstance 0x230
//   UGameInstance::LocalPlayers 0x38 (TArray: Data@0x38, Num@0x40)
//   UPlayer::PlayerController 0x30
void* PlayerControllerFromWorld() {
    if (g_gworld_ptr == 0) return nullptr;
    __try {
        void* instance = *reinterpret_cast<void**>(static_cast<std::uintptr_t>(g_gworld_ptr) + 0x230);
        if (instance == nullptr) return nullptr;
        void** data = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(instance) + 0x38);
        const int num = *reinterpret_cast<const int*>(reinterpret_cast<std::uintptr_t>(instance) + 0x40);
        if (data == nullptr || num <= 0) return nullptr;
        void* local_player = data[0];
        if (local_player == nullptr) return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(local_player) + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}


// Chronos：世界时间倍率（偏移来自 5.6.1/CppSDK/SDK/Engine_classes.hpp）
//   UWorld::PersistentLevel 0x30 → ULevel::WorldSettings 0x2B0 → HTWorldSettings +0x5E0 (float)
constexpr std::uintptr_t kPersistentLevel = 0x30;
constexpr std::uintptr_t kWorldSettings = 0x2B0;
constexpr std::uintptr_t kWorldDilation = 0x5E0;

float* WorldDilationSlot() {
    if (g_gworld_ptr == 0) return nullptr;
    __try {
        void* level = *reinterpret_cast<void**>(static_cast<std::uintptr_t>(g_gworld_ptr) + kPersistentLevel);
        if (level == nullptr) return nullptr;
        void* settings = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(level) + kWorldSettings);
        if (settings == nullptr) return nullptr;
        return reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(settings) + kWorldDilation);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void ProbeHook() {
    static int done = 0;
    if (done || !g_ctx) return;
    done = 1;
    const void* hook = LazyService(&g_ctx->hook, "anomaly.interop.hook", 1);
    g_hook_ok = hook ? 1 : 0;
    const void* dmg = LazyService(&g_ctx->damage_chain, "anomaly.nte.damage-chain", 1);
    g_dmg_chain_ok = dmg ? 1 : 0;
}

void EnsureSubscription() {
    if (g_pe_subscribed || !g_ctx) return;
    const void* pe_service = LazyService(&g_ctx->process_event, "anomaly.ue5.process-event", 1);
    if (!pe_service) return;
    const auto* pe = static_cast<const AnomalyUe5ProcessEventServiceV1*>(pe_service);
    if (!pe->subscribe) return;
    AnomalyGenerationHandleV1 h{};
    if (pe->subscribe(pe->user, OnProcessEvent, nullptr, &h).code == ANOMALY_STATUS_V1_OK)
        g_pe_subscribed = 1;
}

void TickChronos() {
    static int applied = 0;
    float* slot = WorldDilationSlot();
    if (slot == nullptr) return;
    if (g_ui_state.chronos && !applied) {
        float current = 1.0f;
        __try { current = *slot; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (current > 0.001f && current < 10.0f) {
            __try { *slot = 0.0001f; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
            applied = 1;
        }
    } else if (!g_ui_state.chronos && applied) {
        __try { *slot = 1.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        applied = 0;
    }
}


// GM：把 IsGM 的原生 ExecFunction（UFunction::ExecFunction 0xD8）改成 "mov al,1; ret"
int g_gm_patched = 0;

// on_load 阶段服务可能尚未注册，这里按帧惰性补查
const void* LazyService(const void** slot, const char* id, uint32_t min_version) {
    if (*slot != nullptr) return *slot;
    if (!g_ctx || !g_ctx->host || !g_ctx->host->query_service) return nullptr;
    const void* service = nullptr;
    AnomalyStringViewV1 view{id, std::strlen(id)};
    if (g_ctx->host->query_service(g_ctx->host->host_context, view, min_version, &service).code ==
        ANOMALY_STATUS_V1_OK) {
        *slot = service;
    }
    return *slot;
}


// GM 命令清单：patch 成功后，从 HTCheatManager 类的 Children 链枚举 UFunction 名
static char g_gm_cmd_names[32][64];
static int  g_gm_cmd_count = 0;

void BuildGmCommandList() {
    static int built = 0;
    if (built || !g_gm_patched) return;
    built = 1;
    unsigned int cls_id = FindNameIdByPath("/Script/HTGame.HTCheatManager");
    if (cls_id == 0) return;
    void* cls = g_ue.FindClassById(cls_id);
    if (cls == nullptr) return;
    std::uintptr_t child = 0;
    if (!ue::Reader::SafePtr(reinterpret_cast<std::uintptr_t>(cls) + 0x48, &child)) return;
    char name[64];
    for (int guard = 0; guard < 4096 && child != 0 && g_gm_cmd_count < 32; ++guard) {
        unsigned int id = 0;
        if (ue::Reader::SafeU32(child + 0x18, &id)) {
            DecodeName(id, name, sizeof(name));
            if (name[0] != 0) {
                std::snprintf(g_gm_cmd_names[g_gm_cmd_count], sizeof(g_gm_cmd_names[0]), "%s", name);
                ++g_gm_cmd_count;
            }
        }
        std::uintptr_t next = 0;
        if (!ue::Reader::SafePtr(child + 0x28, &next)) break;
        child = next;
    }
}

void ApplyGmPatch() {
    if (g_gm_patched) return;
    if (!g_ctx) { g_patch_rc = -201; return; }

    const void* core_service = LazyService(&g_ctx->core, "anomaly.core", 1);
    if (!core_service) { g_patch_rc = -202; return; }
    const auto* core = static_cast<const AnomalyCoreServiceV1*>(core_service);
    if (!core->read_memory) { g_patch_rc = -203; return; }

    const void* patch_service = LazyService(&g_ctx->patch, "anomaly.interop.patch", 1);
    if (!patch_service) { g_patch_rc = -204; return; }
    const auto* patcher = static_cast<const AnomalyPatchServiceV1*>(patch_service);
    if (!patcher->apply) { g_patch_rc = -205; return; }

    if (!g_isgm_fn && g_cls_pc)
        g_isgm_fn = FindFuncByName(reinterpret_cast<void*>(g_cls_pc), "IsGM");
    if (!g_isgm_fn) { g_patch_rc = -206; return; }

    unsigned char pointer_bytes[8] = {0};
    AnomalyMutableByteSpanV1 read_span{pointer_bytes, 8};
    if (core->read_memory(core->user,
                          reinterpret_cast<std::uintptr_t>(g_isgm_fn) + 0xD8,
                          read_span).code != ANOMALY_STATUS_V1_OK) {
        g_patch_rc = -207;
        return;
    }
    std::uintptr_t exec = 0;
    std::memcpy(&exec, pointer_bytes, 8);
    if (exec == 0) { g_patch_rc = -208; return; }

    unsigned char body[3] = {0xB0, 0x01, 0xC3};   // mov al,1 ; ret
    AnomalyByteSpanV1 body_span{body, 3};
    AnomalyStringViewV1 label{"ultimate.gm.isgm", 16};
    AnomalyGenerationHandleV1 handle{};
    g_patch_rc = static_cast<int>(patcher->apply(patcher->user, exec, body_span, label, &handle).code);
    if (g_patch_rc == 0) g_gm_patched = 1;
}

// Damage：在 process-event 回调（游戏线程）里重放同一调用
void* g_dmg_fn = nullptr;
int   g_replay_budget = 0;
void ANOMALY_CALL OnProcessEvent(void* /*user*/, uintptr_t object, uintptr_t function, void* parameters) {
    if (!g_ui_state.damage || function == 0 || function != reinterpret_cast<uintptr_t>(g_dmg_fn)) return;
    int count = static_cast<int>(g_ui_state.replay);
    if (count <= 0 || count > 1024) return;
    if (g_replay_budget > 200) return;              // 每帧上限，避免雪崩
    if (!g_ctx || !g_ctx->ue5_framework) return;
    const auto* fw = static_cast<const AnomalyUe5FrameworkServiceV1*>(g_ctx->ue5_framework);
    if (!fw->is_game_thread || !fw->is_game_thread(fw->user)) return;
    auto process = g_pe_addr != 0 ? reinterpret_cast<PE_FN>(g_pe_addr) : nullptr;
    if (!process) return;
    for (int i = 0; i < count && i < 32; ++i) {
        process(reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parameters);
        ++g_replay_budget;
    }
}

void TickFeatures() {
    // 安全阀：解析完成且已拿到实例后彻底停止扫描，避免每帧全量遍历
    if (g_resolve.chronos && g_resolve.damage && g_resolve.volley && g_resolve.gm && g_pc_inst)
        return;
    if (g_gworld_instr != 0) {
        __try {
            const int disp = *reinterpret_cast<const int*>((std::uintptr_t)g_gworld_instr + 3);
            const std::uintptr_t target = (std::uintptr_t)g_gworld_instr + 7 + (std::intptr_t)disp;
            g_gworld_ptr = (unsigned long long)*reinterpret_cast<void* const*>(target);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_gworld_ptr = 0; }
    }
    if (!g_pc_inst) g_pc_inst = PlayerControllerFromWorld();
    if (g_scan_budget >= 6) return;
    ++g_scan_budget;
    if (g_resolve.damage && g_resolve.volley && g_resolve.chronos && g_resolve.gm) return;
    if (!g_sig_ok || !g_names_ok) return;
    // retry every frame until resolved (services may appear after first frames)
    void* probe = g_ue.ObjectAt(0);
    g_probe = (unsigned long long)probe;
    if (probe) g_ue.ObjectNameId(probe, &g_probe_nid);
    g_ue.NameId("HTPlayerController", &g_want_id);
    if (g_want_id == 0) g_want_id = FindNameIdByPath("/Script/HTGame.HTPlayerController");
    if (g_id_pc == 0) g_id_pc = g_want_id ? g_want_id : FindNameIdByPath("/Script/HTGame.HTPlayerController");
    unsigned int pc_id = g_id_pc;
    void* pc_cls = g_ue.FindClassById(pc_id);
    g_cls_pc = (unsigned long long)pc_cls;
    if (pc_cls) {
        // DISABLED: full GObjects scan stalls/crashes the game
        if (FindFuncByName(pc_cls, "PausedGameExcludePlayerByTimer")) g_resolve.chronos = 1;
        if (FindFuncByName(pc_cls, "IsGM")) g_resolve.gm = 1;
        if (void* f = FindFuncByName(pc_cls, "CanShowGMConsole")) g_resolve.gm = 1;
        g_isgm_fn = FindFuncByName(pc_cls, "IsGM");
    }
    if (g_id_asc == 0) g_id_asc = FindNameIdByPath("/Script/HTGame.HTAbilitySystemComponent");
    unsigned int asc_id = g_id_asc;
    void* asc_cls = g_ue.FindClassById(asc_id);
    g_cls_asc = (unsigned long long)asc_cls;
    if (asc_cls) {
        if (void* df = FindFuncByName(asc_cls, "K2_ApplyGameplayEffectToTargetWithClass")) {
            g_resolve.damage = 1;
            g_dmg_fn = df;
        }
    }
    if (g_id_vb == 0) g_id_vb = FindNameIdByPath("/Script/HTGame.HTVolleyballGameManager");
    unsigned int vb_id = g_id_vb;
    void* vb_cls = g_ue.FindClassById(vb_id);
    g_cls_vb = (unsigned long long)vb_cls;
    if (vb_cls) {
        if (FindFuncByName(vb_cls, "GetRallyState")) g_resolve.volley = 1;
        if (FindFuncByName(vb_cls, "AwardPoint")) g_resolve.volley = 1;
        if (FindFuncByName(vb_cls, "Test_ForceWin")) g_resolve.volley = 1;
    }
}

void ANOMALY_CALL on_draw(void* ctx, const AnomalyUiServiceV1* ui) {
    if (ctx == nullptr || ui == nullptr || ui->begin_window == nullptr) return;
    ++g_draw_count;
    if (GmToggleOn()) ApplyGmPatch();
    BuildGmCommandList();
    static int open = 1;
    AnomalyStringViewV1 title{kWindowTitle, std::strlen(kWindowTitle)};
    if (ui->begin_window(ui->user, title, &open, 0) != 0) {
        if (ui->checkbox) {
            AnomalyStringViewV1 c1{"Chronos", 8};
            if (ui->checkbox(ui->user, c1, &g_ui_state.chronos)) g_ui_dirty = true;
            AnomalyStringViewV1 c2{"Damage Replay", 13};
            if (ui->checkbox(ui->user, c2, &g_ui_state.damage)) g_ui_dirty = true;
            AnomalyStringViewV1 c3{"  chainReaction", 15};
            if (ui->checkbox(ui->user, c3, &g_ui_state.chain)) g_ui_dirty = true;
            AnomalyStringViewV1 c4{"Volleyball Win", 14};
            if (ui->checkbox(ui->user, c4, &g_ui_state.volley)) g_ui_dirty = true;
            AnomalyStringViewV1 c5{"Developer GM", 12};
            if (ui->checkbox(ui->user, c5, &g_ui_state.gm)) g_ui_dirty = true;
        }
        if (ui->slider_float) {
            AnomalyStringViewV1 s1{"replayCount", 11};
            if (ui->slider_float(ui->user, s1, &g_ui_state.replay, 0.0f, 1024.0f))
                g_ui_dirty = true;
        }
        
        if (ui->button && g_ui_dirty) {
            AnomalyStringViewV1 b1{"Save", 4};
            if (ui->button(ui->user, b1, 60.0f, 0.0f)) SaveDamageConfig();
        }
        if (ui->text) {
            char status[160];
            int n = snprintf(status, sizeof(status),
                             "state: c=%d d=%d chain=%d v=%d gm=%d replay=%.0f | resolved c=%d d=%d v=%d gm=%d",
                             g_ui_state.chronos, g_ui_state.damage, g_ui_state.chain,
                             g_ui_state.volley, g_ui_state.gm, g_ui_state.replay,
                             g_resolve.chronos, g_resolve.damage, g_resolve.volley, g_resolve.gm);
            if (n > 0) {
                AnomalyStringViewV1 view{status, static_cast<std::size_t>(n)};
                ui->text(ui->user, view);
                char diag[96];
                int dn = snprintf(diag, sizeof(diag), "diag: rc=%d find=%d ids=%u/%u/%u world=%llX flags=%d%d", g_sig_rc, g_find_rc, g_id_pc, g_id_asc, g_id_vb,
                             g_gworld_ptr, g_sig_ok, g_names_ok);
                if (dn > 0) {
                    AnomalyStringViewV1 dv{diag, static_cast<std::size_t>(dn)};
                    ui->text(ui->user, dv);
                }
                char dec[160];
                char wn[48] = {0}, nn[48] = {0};
                DecodeName(g_want_id, wn, sizeof(wn));
                DecodeName(g_probe_nid, nn, sizeof(nn));
                int en = snprintf(dec, sizeof(dec), "cls: pc=%llX inst=%llX gm=%d/%d sub=%d hook=%d dmg=%d", g_cls_pc, (unsigned long long)g_pc_inst, g_gm_patched, g_patch_rc, g_pe_subscribed, g_hook_ok, g_dmg_chain_ok);
                if (en > 0) {
                    AnomalyStringViewV1 ev{dec, static_cast<std::size_t>(en)};
                    ui->text(ui->user, ev);
                }
                if (g_gm_cmd_count > 0 && ui->separator) ui->separator(ui->user);
                for (int i = 0; i < g_gm_cmd_count; ++i) {
                    AnomalyStringViewV1 cmd{g_gm_cmd_names[i], std::strlen(g_gm_cmd_names[i])};
                    ui->text(ui->user, cmd);
                }
            }
        }
    }
    ui->end_window(ui->user);
}

const AnomalyPluginDescriptorV1 kDescriptor = {
    sizeof(AnomalyPluginDescriptorV1), 1, 0,
    {kId, 22}, {kName, 15}, {kAuthor, 7}, {kVersion, 5},
    on_load, on_start, on_stop, on_unload, on_update, on_draw,
};


// ---------------------------------------------------------------------------
// 基于 5.6.1 CppSDK dump 的 UFunction 参数布局（HTGame_parameters.hpp 逐字段核对）
// ---------------------------------------------------------------------------
struct FBufData { unsigned char raw[0x58]; };                 // FBufferData (0x58)
struct FActiveGEHandle { unsigned char raw[0x08]; };

struct Params_ApplyGEToTarget {                               // sizeof 0x78
    void*  GameplayEffect;        // 0x00 TSubclassOf<UGameplayEffect>
    void*  TargetAbilityCmp;      // 0x08 UHTAbilitySystemComponent*
    FBufData ModifyData;          // 0x10 FBufferData
    unsigned char BuffEventType;  // 0x68 EBuffEventType
    unsigned char bUsePredictionKey; // 0x69
    unsigned char pad0[2];        // 0x6A
    FActiveGEHandle ReturnValue;  // 0x6C
    unsigned char pad1[4];        // 0x74
};
static_assert(sizeof(Params_ApplyGEToTarget) == 0x78, "dump says 0x78");

struct Params_GetAbilitySystemComponent { void* ReturnValue; };  // 0x00

// UObject::ProcessEvent 位于对象 vtable 的 0x4C 槽（原始插件同款，证据见报告 §8.4）
using ProcessEventFn = PE_FN;

ProcessEventFn ReadProcessEvent(void* object) {
    if (object == nullptr) return nullptr;
    void* vtable = nullptr;
    __try { vtable = *reinterpret_cast<void**>(object); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (vtable == nullptr) return nullptr;
    void* fn = nullptr;
    __try { fn = *reinterpret_cast<void**>(static_cast<unsigned char*>(vtable) + 0x4C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return reinterpret_cast<ProcessEventFn>(fn);
}

// 单次施加 GE：调用方提供已解析的 UFunction 与 GE 类
bool ApplyGameplayEffect(void* asc, void* ufunction_apply, void* ge_class) {
    auto process = ReadProcessEvent(asc);
    if (process == nullptr || ufunction_apply == nullptr) return false;
    Params_ApplyGEToTarget params{};
    params.GameplayEffect = ge_class;
    params.TargetAbilityCmp = asc;
    params.BuffEventType = 0;         // 默认事件类型，调用方按配置覆盖
    params.bUsePredictionKey = 0;
    process(asc, ufunction_apply, &params);
    return true;
}


// 以下参数布局逐字段取自 5.6.1/CppSDK/SDK/HTGame_parameters.hpp
struct Params_GetRallyState { unsigned char ReturnValue; };            // 0x01
struct Params_BoolResult    { unsigned char ReturnValue; };            // IsGM / CanShowGMConsole
struct Params_AwardPoint {                                             // 0x08
    int           TeamIndex;    // 0x00
    unsigned char ScoreType;    // 0x04 EHTVolleyballScoreType
    unsigned char pad[3];       // 0x05
};

// 统一 UFunction 调用：对象 + UFunction + 参数块（走 ProcessEvent slot 0x4C）
bool CallUF(void* object, void* ufunction, void* params) {
    auto process = ReadProcessEvent(object);
    if (process == nullptr || object == nullptr || ufunction == nullptr) return false;
    process(object, ufunction, params);
    return true;
}


// GObjects 访问器特征码：来自插件去混淆字符串，且在 HTGame_dump_20260929 中唯一命中 0x18CEEC9
constexpr const char* kGObjectsSig =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3";
constexpr const char* kGameModule = "HTGame.exe";
constexpr const char* kGameSection = ".text";
// 摘自运行中 Profile 的 ue5.GWorld（resolve: rip-rel32, offset 3, size 7）
// Profile ue5.ProcessEvent（resolve: direct）
constexpr const char* kProcessEventSig =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 "
    "48 89 9D 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 C0 00 00 00";
constexpr const char* kGWorldSig = "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";

// g_ue defined earlier

bool InitUe(const void* signature_service, const void* names_service) {
    g_sig_svc = signature_service ? 1 : 0;
    if (!signature_service || !names_service) { g_sig_rc = -2; return false; }
    const auto* sig = static_cast<const AnomalySignatureServiceV1*>(signature_service);
    if (!sig->resolve) return false;
    std::uintptr_t address = 0;
    AnomalyStringViewV1 mod{kGameModule, std::strlen(kGameModule)};
    AnomalyStringViewV1 sec{kGameSection, std::strlen(kGameSection)};
    AnomalyStringViewV1 pat{kGObjectsSig, std::strlen(kGObjectsSig)};
    g_sig_rc = (int)sig->resolve(sig->user, mod, sec, pat, &address).code;
    if (g_sig_rc != ANOMALY_STATUS_V1_OK) return false;
    g_sig_addr = (unsigned long long)address;
    g_ue.names = static_cast<const AnomalyUe5NamesServiceV1*>(names_service);
    g_ue.get_object = reinterpret_cast<ue::GetObjFn>(address);
    {
        std::uintptr_t gaddr = 0;
        AnomalyStringViewV1 gpat{kGWorldSig, std::strlen(kGWorldSig)};
        if (sig->resolve(sig->user, mod, sec, gpat, &gaddr).code == ANOMALY_STATUS_V1_OK)
            g_gworld_instr = (unsigned long long)gaddr;
        std::uintptr_t peaddr = 0;
        AnomalyStringViewV1 pep{kProcessEventSig, std::strlen(kProcessEventSig)};
        if (sig->resolve(sig->user, mod, sec, pep, &peaddr).code == ANOMALY_STATUS_V1_OK)
            g_pe_addr = (unsigned long long)peaddr;
    }
    g_sig_ok = 1;
    g_names_ok = 1;
    return true;
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor) return fail(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "descriptor is invalid");
    if (descriptor->struct_size < sizeof(AnomalyPluginDescriptorV1)) {
        descriptor->struct_size = sizeof(AnomalyPluginDescriptorV1);
        descriptor->api_major = 1;
        descriptor->api_minor = 0;
        return fail(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "descriptor is too small");
    }
    *descriptor = kDescriptor;
    return ok();
}
