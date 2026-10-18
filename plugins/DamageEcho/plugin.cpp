#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/nte.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
constexpr std::size_t kLogCapacity = 128;
constexpr std::size_t kLogLineSize = 512;
constexpr std::size_t kMaxNameBytes = 2048;
constexpr std::uint32_t kPageCapacity = ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY;
constexpr std::uint64_t kFramePollMs = 350;
constexpr std::uint64_t kPagePollMs = 0;
static_assert(kPageCapacity <= ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY);

template <typename S, typename F>
bool HasField(const S* s, std::size_t off) noexcept {
    return s && s->struct_size >= off + sizeof(F);
}
bool HasUi(const AnomalyUiServiceV1* ui, std::size_t off, std::size_t size) noexcept {
    return ui && ui->struct_size >= off + size;
}
#define HAS_UI(ui, field) (HasUi((ui), offsetof(AnomalyUiServiceV1, field), sizeof((ui)->field)) && (ui)->field)

struct TriggerActor {
    std::uint64_t generation{};
    std::string className;
    std::string actorName;
};
struct Context {
    std::mutex mutex;
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyNteCombatServiceV1* combat{};
    bool actorsAvailable{};
    bool combatAvailable{};
    bool observing{};
    bool resetRequested{};
    bool baselineDone{};
    int windowOpen{1};
    std::uint32_t intervalPreferenceMs{200}; // Display-only preference; never triggers damage.
    std::uint64_t damageCursor{};
    bool damageCursorInitialized{};
    std::uint64_t observedDamageEvents{};
    std::uint64_t lastDamageSequence{};
    std::uint64_t matchingActorCount{};
    std::uint32_t scanTotal{};
    std::uint32_t scanOffset{};
    char status[192]{"只读观察待启动；链式伤害保持禁用"};
    char exportPath[512]{};
    char logs[kLogCapacity][kLogLineSize]{};
    std::size_t logWrite{};
    std::size_t logCount{};

    // Only on the Host's Game-update callback; Draw accesses summaries via mutex.
    std::uint64_t lastFramePollMs{};
    std::uint64_t lastPagePollMs{};
    std::uint64_t lastFrameErrorLogMs{};
    bool frameKnown{};
    bool sweepActive{};
    std::uint64_t frameGeneration{};
    std::uint64_t frameSequence{};
    std::uint32_t pageOffset{};
    std::unordered_map<std::uint64_t, std::string> classNames;
    std::unordered_map<std::uint64_t, TriggerActor> currentTriggers;
    std::unordered_map<std::uint64_t, TriggerActor> previousTriggers;
};

bool ActorServiceReady(const AnomalyNteActorsServiceV1* s) noexcept {
    return HasField<AnomalyNteActorsServiceV1,
        decltype(AnomalyNteActorsServiceV1::entity_name_utf8)>(
            s, offsetof(AnomalyNteActorsServiceV1, entity_name_utf8)) &&
        s->frame && s->page && s->class_name_utf8 && s->entity_name_utf8;
}
bool CombatServiceReady(const AnomalyNteCombatServiceV1* s) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
        decltype(AnomalyNteCombatServiceV1::source_name_utf8)>(
            s, offsetof(AnomalyNteCombatServiceV1, source_name_utf8)) &&
        s->latest_damage_sequence && s->next_damage_event && s->source_name_utf8;
}
void SetStatus(Context* c, const char* value) noexcept {
    if (!c || !value) return;
    std::lock_guard lock(c->mutex);
    strncpy_s(c->status, sizeof(c->status), value, _TRUNCATE);
}
void Log(Context* c, const char* value) noexcept {
    if (!c || !value) return;
    std::lock_guard lock(c->mutex);
    auto& line = c->logs[c->logWrite];
    std::snprintf(line, sizeof(line), "[%llu ms] %s",
        static_cast<unsigned long long>(GetTickCount64()), value);
    c->logWrite = (c->logWrite + 1) % kLogCapacity;
    if (c->logCount < kLogCapacity) ++c->logCount;
}
bool SizingStatus(AnomalyStatusV1 status) noexcept {
    return status.code == ANOMALY_STATUS_V1_OK ||
           status.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL;
}
template <typename Resolver>
bool ReadUtf8(Resolver&& resolver, std::string& result) {
    std::size_t size{};
    if (!SizingStatus(resolver(nullptr, &size)) || size < 2 || size > kMaxNameBytes) return false;
    std::string value(size, '\0');
    if (resolver(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return false;
    const auto end = value.find('\0');
    if (end != std::string::npos) value.resize(end);
    result = std::move(value);
    return true;
}
std::string ClassName(Context* c, std::uint64_t classId) {
    if (!c || !c->actors || !classId) return {};
    auto found = c->classNames.find(classId);
    if (found != c->classNames.end()) return found->second;
    std::string value;
    if (ReadUtf8([&](char* dst, std::size_t* size) {
        return c->actors->class_name_utf8(c->actors->user, classId, dst, size);
    }, value) && c->classNames.size() < 4096)
        c->classNames.emplace(classId, value);
    return value;
}
std::string ActorName(Context* c, std::uint64_t entityId) {
    if (!c || !c->actors || !entityId) return {};
    std::string value;
    (void)ReadUtf8([&](char* dst, std::size_t* size) {
        return c->actors->entity_name_utf8(c->actors->user, entityId, dst, size);
    }, value);
    return value;
}
std::string DamageSource(Context* c, std::uint64_t sourceId) {
    if (!c || !c->combat || !sourceId) return {};
    std::string value;
    (void)ReadUtf8([&](char* dst, std::size_t* size) {
        return c->combat->source_name_utf8(c->combat->user, sourceId, dst, size);
    }, value);
    return value;
}
bool TriggerClass(const std::string& name) noexcept {
    return name.find("DamageTriggerActor") != std::string::npos ||
           name.find("DamageTrigger") != std::string::npos;
}
void DrainDamage(Context* c) {
    const auto* s = c->combat;
    const auto latest = s->latest_damage_sequence(s->user);
    if (!c->damageCursorInitialized) {
        c->damageCursor = latest; // Do not replay history from before observation started.
        c->damageCursorInitialized = true;
        {
            std::lock_guard lock(c->mutex);
            c->lastDamageSequence = latest;
        }
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "[combat] cursor initialized at %llu; earlier events skipped",
            static_cast<unsigned long long>(latest));
        Log(c, line);
        return;
    }
    unsigned drained = 0;
    while (c->damageCursor < latest && drained < 16) {
        AnomalyNteDamageEventV1 e{};
        e.struct_size = sizeof(e);
        const auto st = s->next_damage_event(s->user, c->damageCursor, &e);
        if (st.code != ANOMALY_STATUS_V1_OK) {
            if (st.code == ANOMALY_STATUS_V1_NOT_FOUND && latest > c->damageCursor) {
                char line[192]{};
                std::snprintf(line, sizeof(line),
                    "[combat] event gap after=%llu latest=%llu; cursor resynchronized",
                    static_cast<unsigned long long>(c->damageCursor),
                    static_cast<unsigned long long>(latest));
                Log(c, line);
                c->damageCursor = latest;
                std::lock_guard lock(c->mutex);
                c->lastDamageSequence = latest;
            } else if (st.code != ANOMALY_STATUS_V1_NOT_FOUND) {
                char line[160]{};
                std::snprintf(line, sizeof(line), "[combat] next_damage_event status=%u", st.code);
                Log(c, line);
            }
            break;
        }
        if (e.sequence <= c->damageCursor) {
            Log(c, "[combat] non-advancing event sequence; cursor unchanged");
            break;
        }
        c->damageCursor = e.sequence;
        ++drained;
        const auto src = DamageSource(c, e.source_id);
        char line[kLogLineSize]{};
        std::snprintf(line, sizeof(line),
            "[damage] seq=%llu tick=%llu attacker=%llu:%llu victim=%llu:%llu source_id=%llu final=%lld flags=0x%X source=%s",
            static_cast<unsigned long long>(e.sequence),
            static_cast<unsigned long long>(e.tick_sequence),
            static_cast<unsigned long long>(e.attacker.id),
            static_cast<unsigned long long>(e.attacker.generation),
            static_cast<unsigned long long>(e.victim.id),
            static_cast<unsigned long long>(e.victim.generation),
            static_cast<unsigned long long>(e.source_id),
            static_cast<long long>(e.final_damage), e.flags, src.empty() ? "<unresolved>" : src.c_str());
        Log(c, line);
        {
            std::lock_guard lock(c->mutex);
            ++c->observedDamageEvents;
            c->lastDamageSequence = e.sequence;
        }
    }
}
void BeginSweep(Context* c, const AnomalyNteEntityFrameV1& frame) {
    c->frameKnown = true;
    c->frameGeneration = frame.generation;
    c->frameSequence = frame.sequence;
    c->sweepActive = true;
    c->pageOffset = 0;
    c->lastPagePollMs = 0;
    c->currentTriggers.clear();
    {
        std::lock_guard lock(c->mutex);
        c->scanTotal = frame.entity_count;
        c->scanOffset = 0;
    }
    char line[192]{};
    std::snprintf(line, sizeof(line),
        "[actors] sweep start generation=%llu sequence=%llu frame_count=%u",
        static_cast<unsigned long long>(frame.generation),
        static_cast<unsigned long long>(frame.sequence), frame.entity_count);
    Log(c, line);
}
void FinishSweep(Context* c) {
    if (!c->baselineDone) {
        for (const auto& [id, actor] : c->currentTriggers) {
            char line[kLogLineSize]{};
            std::snprintf(line, sizeof(line), "[actor-baseline] id=%llu:%llu class=%s name=%s",
                static_cast<unsigned long long>(id),
                static_cast<unsigned long long>(actor.generation),
                actor.className.c_str(), actor.actorName.c_str());
            Log(c, line);
        }
        c->baselineDone = true;
        char line[128]{};
        std::snprintf(line, sizeof(line), "[actors] initial baseline complete; matches=%zu",
            c->currentTriggers.size());
        Log(c, line);
    } else {
        for (const auto& [id, actor] : c->currentTriggers) {
            auto old = c->previousTriggers.find(id);
            if (old == c->previousTriggers.end() || old->second.generation != actor.generation) {
                char line[kLogLineSize]{};
                std::snprintf(line, sizeof(line), "[actor-appeared] id=%llu:%llu class=%s name=%s",
                    static_cast<unsigned long long>(id),
                    static_cast<unsigned long long>(actor.generation),
                    actor.className.c_str(), actor.actorName.c_str());
                Log(c, line);
            }
        }
        for (const auto& [id, actor] : c->previousTriggers) {
            auto now = c->currentTriggers.find(id);
            if (now == c->currentTriggers.end() || now->second.generation != actor.generation) {
                char line[kLogLineSize]{};
                std::snprintf(line, sizeof(line), "[actor-disappeared] id=%llu:%llu class=%s name=%s",
                    static_cast<unsigned long long>(id),
                    static_cast<unsigned long long>(actor.generation),
                    actor.className.c_str(), actor.actorName.c_str());
                Log(c, line);
            }
        }
    }
    c->previousTriggers = c->currentTriggers;
    {
        std::lock_guard lock(c->mutex);
        c->matchingActorCount = c->currentTriggers.size();
        c->scanOffset = c->scanTotal;
    }
    c->sweepActive = false;
}
void PollActors(Context* c) {
    const auto* s = c->actors;
    const auto now = GetTickCount64();
    if (!c->sweepActive &&
        (c->lastFramePollMs == 0 || now - c->lastFramePollMs >= kFramePollMs)) {
        c->lastFramePollMs = now;
        AnomalyNteEntityFrameV1 frame{};
        frame.struct_size = sizeof(frame);
        const auto st = s->frame(s->user, &frame);
        if (st.code == ANOMALY_STATUS_V1_OK) {
            if (!c->frameKnown || frame.generation != c->frameGeneration ||
                frame.sequence != c->frameSequence) BeginSweep(c, frame);
        } else if (c->lastFrameErrorLogMs == 0 || now - c->lastFrameErrorLogMs >= 5000) {
            c->lastFrameErrorLogMs = now;
            char line[144]{};
            std::snprintf(line, sizeof(line), "[actors] frame status=%u; retrying", st.code);
            Log(c, line);
        }
    }
    if (!c->sweepActive || (c->lastPagePollMs && now - c->lastPagePollMs < kPagePollMs)) return;
    c->lastPagePollMs = now;
    std::array<AnomalyNteEntitySnapshotV1, kPageCapacity> items{};
    for (auto& item : items) item.struct_size = sizeof(item);
    AnomalyNteEntityPageRequestV1 req{};
    req.struct_size = sizeof(req);
    req.generation = c->frameGeneration;
    req.offset = c->pageOffset;
    req.capacity = kPageCapacity;
    AnomalyNteEntityPageResultV1 result{};
    result.struct_size = sizeof(result);
    const auto st = s->page(s->user, &req, items.data(), &result);
    if (st.code != ANOMALY_STATUS_V1_OK || result.generation != c->frameGeneration) {
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "[actors] page/snapshot failed status=%u; partial sweep discarded", st.code);
        Log(c, line);
        c->frameKnown = false;
        c->sweepActive = false;
        return;
    }
    {
        std::lock_guard lock(c->mutex);
        c->scanTotal = result.total_matches;
    }
    const auto count = (std::min)(result.returned, static_cast<std::uint32_t>(items.size()));
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& item = items[i];
        auto cls = ClassName(c, item.class_id);
        if (!TriggerClass(cls) || item.handle.id == 0) continue;
        c->currentTriggers[item.handle.id] = {
            item.handle.generation, std::move(cls), ActorName(c, item.entity_id)};
    }
    if (result.next_offset <= c->pageOffset && result.next_offset < result.total_matches) {
        Log(c, "[actors] pagination did not advance; partial sweep discarded");
        c->frameKnown = false;
        c->sweepActive = false;
        return;
    }
    c->pageOffset = result.next_offset;
    {
        std::lock_guard lock(c->mutex);
        c->scanOffset = result.next_offset;
        c->scanTotal = result.total_matches;
    }
    if (result.next_offset >= result.total_matches) FinishSweep(c);
}
void ResetObservation(Context* c) {
    c->damageCursorInitialized = false;
    c->baselineDone = false;
    c->frameKnown = false;
    c->sweepActive = false;
    c->lastFramePollMs = 0;
    c->lastPagePollMs = 0;
    c->previousTriggers.clear();
    c->currentTriggers.clear();
    {
        std::lock_guard lock(c->mutex);
        c->observedDamageEvents = 0;
        c->lastDamageSequence = 0;
        c->matchingActorCount = 0;
        c->scanTotal = c->scanOffset = 0;
        c->exportPath[0] = '\0';
    }
    Log(c, "[probe] observation reset; previous game events excluded");
}
bool ExportLog(Context* c) noexcept {
    char snapshot[kLogCapacity][kLogLineSize]{};
    std::size_t count{};
    std::uint64_t events{}, seq{}, actors{};
    std::uint32_t total{}, offset{};
    bool actorReady{}, combatReady{}, observing{};
    {
        std::lock_guard lock(c->mutex);
        count = c->logCount;
        const auto first = (c->logWrite + kLogCapacity - count) % kLogCapacity;
        for (std::size_t i = 0; i < count; ++i)
            strncpy_s(snapshot[i], kLogLineSize, c->logs[(first + i) % kLogCapacity], _TRUNCATE);
        events = c->observedDamageEvents; seq = c->lastDamageSequence;
        actors = c->matchingActorCount; total = c->scanTotal; offset = c->scanOffset;
        actorReady = c->actorsAvailable; combatReady = c->combatAvailable; observing = c->observing;
    }
    wchar_t temp[MAX_PATH]{};
    const DWORD tlen = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    if (!tlen || tlen >= std::size(temp)) { SetStatus(c, "日志导出失败：无法获取临时目录"); return false; }
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t path[MAX_PATH]{};
    const int n = swprintf_s(path, std::size(path),
        L"%lsAnomaly-DamageChain-Observation-%04u%02u%02u-%02u%02u%02u-%03u.log",
        temp, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (n <= 0 || static_cast<std::size_t>(n) >= std::size(path)) {
        SetStatus(c, "日志导出失败：路径过长"); return false;
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { SetStatus(c, "日志导出失败：无法创建文件"); return false; }
    char header[1024]{};
    const int hn = std::snprintf(header, sizeof(header),
        "Damage Chain read-only observation log\r\n"
        "No damage calls, hooks, private writes, or trigger spawning.\r\n"
        "HTGame.exe+0x42D5E50 remains unverified and is never called.\r\n"
        "Actors service=%s; Combat service=%s; observing=%s\r\n"
        "New damage events=%llu; last sequence=%llu; matching actor count=%llu; scan=%u/%u\r\n\r\n",
        actorReady ? "available" : "unavailable", combatReady ? "available" : "unavailable",
        observing ? "yes" : "no", static_cast<unsigned long long>(events),
        static_cast<unsigned long long>(seq), static_cast<unsigned long long>(actors), offset, total);
    DWORD written{};
    bool ok = hn > 0 && static_cast<std::size_t>(hn) < sizeof(header) &&
        WriteFile(file, header, static_cast<DWORD>(hn), &written, nullptr) != 0 &&
        written == static_cast<DWORD>(hn);
    for (std::size_t i = 0; ok && i < count; ++i) {
        const DWORD len = static_cast<DWORD>(std::strlen(snapshot[i]));
        ok = WriteFile(file, snapshot[i], len, &written, nullptr) != 0 && written == len;
        if (ok) {
            constexpr char nl[] = "\r\n";
            ok = WriteFile(file, nl, 2, &written, nullptr) != 0 && written == 2;
        }
    }
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        char msg[128]{}; std::snprintf(msg, sizeof(msg), "日志导出失败：Win32=%lu", static_cast<unsigned long>(err));
        SetStatus(c, msg); return false;
    }
    char utf8[512]{};
    (void)WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    {
        std::lock_guard lock(c->mutex);
        strncpy_s(c->exportPath, sizeof(c->exportPath), utf8, _TRUNCATE);
        strncpy_s(c->status, sizeof(c->status), "只读诊断日志已导出", _TRUNCATE);
    }
    Log(c, "[ui] read-only diagnostic log exported");
    return true;
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** pluginContext) {
    if (!host || !pluginContext) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *pluginContext = nullptr;
    auto* c = new (std::nothrow) Context{};
    if (!c) return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    anomaly::sdk::Host h(host);
    const auto actorsRef = h.Query<AnomalyNteActorsServiceV1>(ANOMALY_NTE_ACTORS_SERVICE_V1_ID);
    const auto combatRef = h.Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID);
    c->actors = actorsRef.get(); c->combat = combatRef.get();
    c->actorsAvailable = ActorServiceReady(c->actors);
    c->combatAvailable = CombatServiceReady(c->combat);
    Log(c, "[load] read-only probe configured; no damage hook/call installed");
    Log(c, c->actorsAvailable ? "[service] anomaly.nte.actors available" : "[service] anomaly.nte.actors unavailable/ABI incomplete");
    Log(c, c->combatAvailable ? "[service] anomaly.nte.combat available" : "[service] anomaly.nte.combat unavailable/ABI incomplete");
    if (!c->actorsAvailable || !c->combatAvailable)
        strncpy_s(c->status, sizeof(c->status), "只读服务不完整；需匹配的 Runtime/Core", _TRUNCATE);
    *pluginContext = c;
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Start(void* pluginContext) {
    auto* c = static_cast<Context*>(pluginContext);
    if (!c) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    {
        std::lock_guard lock(c->mutex);
        c->observing = true; c->resetRequested = true;
        strncpy_s(c->status, sizeof(c->status), "只读观察已启动；链式伤害仍保持禁用", _TRUNCATE);
    }
    Log(c, "[lifecycle] read-only observation requested");
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Stop(void* pluginContext, std::uint32_t) {
    auto* c = static_cast<Context*>(pluginContext);
    if (!c) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    {
        std::lock_guard lock(c->mutex);
        c->observing = false;
        strncpy_s(c->status, sizeof(c->status), "只读观察已停止；链式伤害未启用", _TRUNCATE);
    }
    Log(c, "[lifecycle] read-only observation stopped");
    return anomaly::sdk::Ok();
}
void ANOMALY_CALL Unload(void* pluginContext) {
    auto* c = static_cast<Context*>(pluginContext);
    if (!c) return;
    { std::lock_guard lock(c->mutex); c->observing = false; }
    delete c;
}
void ANOMALY_CALL Update(void* pluginContext, double) {
    auto* c = static_cast<Context*>(pluginContext);
    if (!c) return;
    bool observing{}, reset{};
    {
        std::lock_guard lock(c->mutex);
        observing = c->observing; reset = c->resetRequested; c->resetRequested = false;
    }
    if (!observing) return;
    try {
        if (reset) ResetObservation(c);
        if (c->combatAvailable) DrainDamage(c);
        if (c->actorsAvailable) PollActors(c);
    } catch (...) {
        Log(c, "[probe] exception during read-only observation; no damage call attempted");
        SetStatus(c, "只读观察遇到异常；伤害调用仍保持禁用");
    }
}
void TryStartChain(Context* c) {
    SetStatus(c, "安全阻止：伤害入口与调用 ABI 尚未验证");
    Log(c, "[guard] chain activation rejected; no validated invocation ABI");
}
void ANOMALY_CALL Draw(void* pluginContext, const AnomalyUiServiceV1* ui) {
    auto* c = static_cast<Context*>(pluginContext);
    if (!c || !HAS_UI(ui, begin_window) || !HAS_UI(ui, end_window) || !HAS_UI(ui, text)) return;
    if (HAS_UI(ui, set_next_window_size)) ui->set_next_window_size(ui->user, 620.0F, 470.0F, 4u);
    anomaly::sdk::UiWindow window(ui, "伤害链式反应（只读取证版）", &c->windowOpen);
    if (!window) return;
    ui->text(ui->user, anomaly::sdk::StringView("只读取战斗事件与 Actor 快照；不施加伤害、不生成触发器、不挂原始地址 Hook。"));
    ui->text(ui->user, anomaly::sdk::StringView("观察到 DamageTriggerActor 类对象只是线索，不等于证实旧插件使用该机制。"));
    char line[256]{}, status[192]{}, path[512]{};
    bool observing{}, actorsReady{}, combatReady{};
    std::uint64_t events{}, seq{}, actorCount{};
    std::uint32_t offset{}, total{}, interval{};
    {
        std::lock_guard lock(c->mutex);
        observing = c->observing; actorsReady = c->actorsAvailable; combatReady = c->combatAvailable;
        events = c->observedDamageEvents; seq = c->lastDamageSequence; actorCount = c->matchingActorCount;
        offset = c->scanOffset; total = c->scanTotal; interval = c->intervalPreferenceMs;
        strncpy_s(status, sizeof(status), c->status, _TRUNCATE);
        strncpy_s(path, sizeof(path), c->exportPath, _TRUNCATE);
    }
    ui->text(ui->user, anomaly::sdk::StringView(observing ? "只读观察：运行中" : "只读观察：已停止"));
    std::snprintf(line, sizeof(line), "Actors 服务：%s | Combat 服务：%s",
        actorsReady ? "可用" : "不可用", combatReady ? "可用" : "不可用");
    ui->text(ui->user, anomaly::sdk::StringView(line));
    ui->text(ui->user, anomaly::sdk::StringView(status));
    std::snprintf(line, sizeof(line), "新伤害事件：%llu | 最近序号：%llu",
        static_cast<unsigned long long>(events), static_cast<unsigned long long>(seq));
    ui->text(ui->user, anomaly::sdk::StringView(line));
    std::snprintf(line, sizeof(line), "最近完成扫描的匹配 Actor：%llu | 扫描进度：%u/%u",
        static_cast<unsigned long long>(actorCount), offset, total);
    ui->text(ui->user, anomaly::sdk::StringView(line));
    float slider = static_cast<float>(interval);
    if (HAS_UI(ui, slider_float) &&
        ui->slider_float(ui->user, anomaly::sdk::StringView("预设间隔（仅保存，不会触发伤害）"),
                         &slider, 50.0F, 2000.0F) != 0) {
        slider = std::clamp(slider, 50.0F, 2000.0F);
        std::lock_guard lock(c->mutex);
        c->intervalPreferenceMs = static_cast<std::uint32_t>(slider + 0.5F);
    }
    if (HAS_UI(ui, separator)) ui->separator(ui->user);
    if (HAS_UI(ui, button)) {
        if (ui->button(ui->user, anomaly::sdk::StringView(observing ? "停止只读观察" : "启动只读观察"), 155.0F, 0.0F) != 0) {
            if (observing) (void)Stop(c, 0); else (void)Start(c);
        }
        if (HAS_UI(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
        if (ui->button(ui->user, anomaly::sdk::StringView("启动链式伤害（安全阻止）"), 220.0F, 0.0F) != 0) TryStartChain(c);
        if (ui->button(ui->user, anomaly::sdk::StringView("导出诊断日志"), 160.0F, 0.0F) != 0) (void)ExportLog(c);
    }
    std::size_t count{};
    { std::lock_guard lock(c->mutex); count = c->logCount; }
    std::snprintf(line, sizeof(line), "保留诊断日志：%zu/%zu", count, kLogCapacity);
    ui->text(ui->user, anomaly::sdk::StringView(line));
    if (path[0]) {
        ui->text(ui->user, anomaly::sdk::StringView("最近导出路径："));
        ui->text(ui->user, anomaly::sdk::StringView(path));
    }
}
using UiBeginWindowSignature = int (ANOMALY_CALL *)(void*, AnomalyStringViewV1, int*, std::uint32_t);
static_assert(std::is_same_v<decltype(AnomalyUiServiceV1::begin_window), UiBeginWindowSignature>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_load), decltype(&Load)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_start), decltype(&Start)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_stop), decltype(&Stop)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_unload), decltype(&Unload)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_update), decltype(&Update)>);
static_assert(std::is_same_v<decltype(AnomalyPluginDescriptorV1::on_draw), decltype(&Draw)>);
} // namespace
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("com.rabb1t79.damage-echo"),
        anomaly::sdk::StringView("Damage Chain (Read-only Probe)"),
        anomaly::sdk::StringView("Rabb1t79"), anomaly::sdk::StringView("1.6.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
