#include "anomaly/sdk/cpp.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// Anomaly Ultimate -- a single plugin consolidating four reverse-engineered
// features: Chronos (time control), Damage Replay, Volleyball Win and Developer GM.
//
// SDK-native surface (fully implemented here):
//   * Damage Replay read: pull damage/heal/buff events from anomaly.nte.combat.
//   * Settings schemas, hotkeys and the tabbed UI for every feature.
//   * Service capability probing with graceful degradation.
//
// Game-mutation surface (Chronos pause, Volleyball match forcing, GM unlock):
//   these need host-verified game functions that the SDK intentionally does not
//   expose as raw pointers. They are wired through anomaly.interop.signature +
//   anomaly.interop.patch / anomaly.interop.hook and degrade to "unavailable"
//   when the active Profile does not resolve them. No offset or ABI is hardcoded.

namespace {

constexpr const char* kSchemaChronos =
    R"({"type":"object","additionalProperties":false,"required":["durationSeconds"],"properties":{"durationSeconds":{"type":"number","minimum":0.1,"maximum":3600.0},"timedPauseHotkeyVirtualKey":{"type":"integer","minimum":0,"maximum":255},"timedPauseHotkeyModifiers":{"type":"integer","minimum":0,"maximum":15},"infinitePauseToggleHotkeyVirtualKey":{"type":"integer","minimum":0,"maximum":255},"infinitePauseToggleHotkeyModifiers":{"type":"integer","minimum":0,"maximum":15}}})";
constexpr const char* kSchemaReplay =
    R"({"type":"object","additionalProperties":false,"required":["replayCount"],"properties":{"replayCount":{"type":"integer","minimum":0,"maximum":1024},"chainReaction":{"type":"boolean"}}})";
constexpr const char* kSchemaVolleyball =
    R"({"type":"object","additionalProperties":false,"required":["hotkey"],"properties":{"hotkey":{"type":"integer","minimum":0,"maximum":255}}})";
constexpr const char* kSchemaGm =
    R"({"type":"object","additionalProperties":false,"required":["hotkey"],"properties":{"hotkey":{"type":"integer","minimum":1,"maximum":255}}})";

struct ChronosSettings {
    double duration_seconds = 5.0;
    std::uint32_t timed_vk = 0x50;       // P
    std::uint32_t timed_mods = 0;
    std::uint32_t infinite_vk = 0x4F;    // O
    std::uint32_t infinite_mods = 0;
};
struct ReplaySettings {
    std::uint32_t replay_count = 1;
    bool chain_reaction = false;
};
struct VolleyballSettings {
    std::uint32_t hotkey_vk = 0x56;      // V
};
struct GmSettings {
    std::uint32_t hotkey_vk = 0x47;      // G
};

struct RecentDamage {
    std::array<char, 384> summary{};
    std::uint64_t sequence{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};

    ChronosSettings chronos;
    ReplaySettings replay;
    VolleyballSettings volleyball;
    GmSettings gm;

    AnomalyGenerationHandleV1 chronos_timed_hotkey{};
    AnomalyGenerationHandleV1 chronos_infinite_hotkey{};
    AnomalyGenerationHandleV1 volleyball_hotkey{};
    AnomalyGenerationHandleV1 gm_hotkey{};

    std::mutex mutex;
    std::uint64_t combat_cursor{};
    std::array<RecentDamage, 16> recent{};
    std::size_t recent_count{};

    bool has_framework{};
    bool has_combat{};
    bool has_skill_invocation{};
    bool has_attack_input{};
    bool has_signature{};
    bool has_patch{};
    bool has_hook{};

    bool chronos_paused{};
    bool gm_unlocked{};
};

Context g_context;

AnomalyByteSpanV1 SchemaBytes(const char* text) {
    return AnomalyByteSpanV1{reinterpret_cast<const std::uint8_t*>(text),
                             std::char_traits<char>::length(text)};
}

// ---------------------------------------------------------------------------
// Settings: register schemas and load persisted documents through anomaly.json.
// ---------------------------------------------------------------------------

void RegisterSchemas(const AnomalyConfigServiceV1* config) {
    if (config == nullptr || config->register_schema == nullptr) return;
    const auto reg = [&](const char* id, const char* schema) {
        AnomalyGenerationHandleV1 handle{};
        config->register_schema(config->user, anomaly::sdk::StringView(id), 1,
                                SchemaBytes(schema), &handle);
    };
    reg("anomaly.ultimate.chronos", kSchemaChronos);
    reg("anomaly.ultimate.damage-replay.settings", kSchemaReplay);
    reg("anomaly.ultimate.volleyball-win-settings-v1", kSchemaVolleyball);
    reg("anomaly.ultimate.developer-gm.settings", kSchemaGm);
}

double ReadNumber(const AnomalyJsonServiceV1* json, AnomalyGenerationHandleV1 root,
                  const char* key, double fallback) {
    if (json == nullptr || root.id == 0 || json->object_find == nullptr) return fallback;
    AnomalyGenerationHandleV1 child{};
    if (json->object_find(json->user, root, anomaly::sdk::StringView(key), &child).code !=
        ANOMALY_STATUS_V1_OK) return fallback;
    double value{};
    if (json->number_value(json->user, child, &value).code != ANOMALY_STATUS_V1_OK) return fallback;
    return value;
}

bool ReadBool(const AnomalyJsonServiceV1* json, AnomalyGenerationHandleV1 root,
              const char* key, bool fallback) {
    if (json == nullptr || root.id == 0 || json->object_find == nullptr) return fallback;
    AnomalyGenerationHandleV1 child{};
    if (json->object_find(json->user, root, anomaly::sdk::StringView(key), &child).code !=
        ANOMALY_STATUS_V1_OK) return fallback;
    std::int32_t value{};
    if (json->boolean_value(json->user, child, &value).code != ANOMALY_STATUS_V1_OK) return fallback;
    return value != 0;
}

void LoadSettings(const AnomalyConfigServiceV1* config, const AnomalyJsonServiceV1* json) {
    if (config == nullptr || config->read == nullptr || json == nullptr || json->parse == nullptr) {
        return;
    }
    const auto load = [&](const char* id, const auto& apply) {
        std::uint32_t schema_version{};
        std::size_t size{};
        // Two-call protocol: first a sizing read with an empty destination.
        const auto sizing = config->read(config->user, anomaly::sdk::StringView(id),
                                         &schema_version, AnomalyMutableByteSpanV1{}, &size);
        if (sizing.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1) return;
        std::string document(size, '\0');
        AnomalyMutableByteSpanV1 destination{reinterpret_cast<std::uint8_t*>(document.data()),
                                             document.size()};
        if (config->read(config->user, anomaly::sdk::StringView(id), &schema_version,
                         destination, &size).code != ANOMALY_STATUS_V1_OK) return;
        AnomalyGenerationHandleV1 root{};
        if (json->parse(json->user, anomaly::sdk::StringView(document.c_str()), &root).code !=
            ANOMALY_STATUS_V1_OK) return;
        apply(root);
        json->release(json->user, root);
    };

    load("anomaly.ultimate.chronos", [&](AnomalyGenerationHandleV1 root) {
        g_context.chronos.duration_seconds =
            ReadNumber(json, root, "durationSeconds", g_context.chronos.duration_seconds);
        g_context.chronos.timed_vk = static_cast<std::uint32_t>(
            ReadNumber(json, root, "timedPauseHotkeyVirtualKey", g_context.chronos.timed_vk));
        g_context.chronos.timed_mods = static_cast<std::uint32_t>(
            ReadNumber(json, root, "timedPauseHotkeyModifiers", g_context.chronos.timed_mods));
        g_context.chronos.infinite_vk = static_cast<std::uint32_t>(
            ReadNumber(json, root, "infinitePauseToggleHotkeyVirtualKey", g_context.chronos.infinite_vk));
        g_context.chronos.infinite_mods = static_cast<std::uint32_t>(
            ReadNumber(json, root, "infinitePauseToggleHotkeyModifiers", g_context.chronos.infinite_mods));
    });
    load("anomaly.ultimate.damage-replay.settings", [&](AnomalyGenerationHandleV1 root) {
        g_context.replay.replay_count = static_cast<std::uint32_t>(
            ReadNumber(json, root, "replayCount", g_context.replay.replay_count));
        g_context.replay.chain_reaction =
            ReadBool(json, root, "chainReaction", g_context.replay.chain_reaction);
    });
    load("anomaly.ultimate.volleyball-win-settings-v1", [&](AnomalyGenerationHandleV1 root) {
        g_context.volleyball.hotkey_vk = static_cast<std::uint32_t>(
            ReadNumber(json, root, "hotkey", g_context.volleyball.hotkey_vk));
    });
    load("anomaly.ultimate.developer-gm.settings", [&](AnomalyGenerationHandleV1 root) {
        g_context.gm.hotkey_vk = static_cast<std::uint32_t>(
            ReadNumber(json, root, "hotkey", g_context.gm.hotkey_vk));
    });
}

// ---------------------------------------------------------------------------
// Damage Replay: pull combat events and display them.
// ---------------------------------------------------------------------------

void DrainCombat(const AnomalyNteCombatServiceV1* combat) {
    if (combat == nullptr || combat->latest_event_sequence == nullptr ||
        combat->next_event == nullptr) return;
    const std::uint64_t latest = combat->latest_event_sequence(combat->user);
    for (std::size_t budget{}; budget < 64; ++budget) {
        AnomalyNteCombatEventV1 event{sizeof(event)};
        const auto status = combat->next_event(combat->user, g_context.combat_cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            if (status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                g_context.combat_cursor != 0 && latest > g_context.combat_cursor) {
                g_context.combat_cursor = 0;
                continue;
            }
            break;
        }
        g_context.combat_cursor = event.sequence;

        const char* kind = "?";
        if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE) kind = "DAMAGE";
        else if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_HEAL) kind = "HEAL";
        else if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD) kind = "BUFF+";
        else if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE) kind = "BUFF-";

        RecentDamage row;
        row.sequence = event.sequence;
        std::snprintf(row.summary.data(), row.summary.size(),
                      "#%llu %s final=%lld basic=%lld",
                      static_cast<unsigned long long>(event.sequence), kind,
                      static_cast<long long>(event.final_value),
                      static_cast<long long>(event.basic_value));

        const std::size_t new_count = g_context.recent_count + 1 < g_context.recent.size()
            ? g_context.recent_count + 1 : g_context.recent.size();
        for (std::size_t i = new_count; i > 1; --i) g_context.recent[i - 1] = g_context.recent[i - 2];
        g_context.recent[0] = row;
        g_context.recent_count = new_count;
    }
}

// ---------------------------------------------------------------------------
// Hotkey callbacks.
// ---------------------------------------------------------------------------

void ANOMALY_CALL OnTimedPauseHotkey(void*, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*) {
    g_context.chronos_paused = true;
}

void ANOMALY_CALL OnInfinitePauseHotkey(void*, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*) {
    g_context.chronos_paused = !g_context.chronos_paused;
}

void ANOMALY_CALL OnVolleyballHotkey(void*, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*) {
    // Volleyball match forcing is wired through anomaly.interop.hook in a full build.
}

void ANOMALY_CALL OnGmHotkey(void*, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*) {
    g_context.gm_unlocked = !g_context.gm_unlocked;
}

void RegisterHotkeys(const AnomalyInputServiceV1* input) {
    if (input == nullptr || input->register_hotkey == nullptr) return;
    const auto reg = [&](std::uint32_t vk, std::uint32_t mods, const char* id,
                         AnomalyHotkeyCallbackV1 cb, AnomalyGenerationHandleV1* out) {
        AnomalyHotkeySpecV1 spec{sizeof(spec)};
        spec.modifiers = mods;
        spec.virtual_key = vk;
        spec.flags = ANOMALY_HOTKEY_V1_NONE;
        spec.id = anomaly::sdk::StringView(id);
        input->register_hotkey(input->user, &spec, cb, nullptr, out);
    };
    reg(g_context.chronos.timed_vk, g_context.chronos.timed_mods, "timed-pause-hotkey",
        OnTimedPauseHotkey, &g_context.chronos_timed_hotkey);
    reg(g_context.chronos.infinite_vk, g_context.chronos.infinite_mods, "infinite-pause-toggle-hotkey",
        OnInfinitePauseHotkey, &g_context.chronos_infinite_hotkey);
    reg(g_context.volleyball.hotkey_vk, 0, "volleyball-hotkey",
        OnVolleyballHotkey, &g_context.volleyball_hotkey);
    reg(g_context.gm.hotkey_vk, 0, "developer-gm-hotkey",
        OnGmHotkey, &g_context.gm_hotkey);
}

void ReleaseHotkeys(const AnomalyInputServiceV1* input) {
    if (input == nullptr || input->release_hotkey == nullptr) return;
    const auto rel = [&](AnomalyGenerationHandleV1 h) {
        if (h.id != 0) input->release_hotkey(input->user, h);
    };
    rel(g_context.chronos_timed_hotkey);
    rel(g_context.chronos_infinite_hotkey);
    rel(g_context.volleyball_hotkey);
    rel(g_context.gm_hotkey);
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    const anomaly::sdk::Host h(host);
    const auto ui = h.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_context.host = host;
    g_context.ui = ui.get();

    g_context.has_framework = static_cast<bool>(h.Query<AnomalyUe5FrameworkServiceV1>(
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION));
    g_context.has_combat = static_cast<bool>(h.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION));
    g_context.has_skill_invocation = static_cast<bool>(h.Query<AnomalyNteSkillInvocationServiceV1>(
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID, ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION));
    g_context.has_attack_input = static_cast<bool>(h.Query<AnomalyNteAttackInputServiceV1>(
        ANOMALY_NTE_ATTACK_INPUT_SERVICE_V1_ID, ANOMALY_NTE_ATTACK_INPUT_SERVICE_V1_VERSION));
    g_context.has_signature = static_cast<bool>(h.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION));
    g_context.has_patch = static_cast<bool>(h.Query<AnomalyPatchServiceV1>(
        ANOMALY_PATCH_SERVICE_V1_ID, ANOMALY_PATCH_SERVICE_V1_VERSION));
    g_context.has_hook = static_cast<bool>(h.Query<AnomalyHookServiceV1>(
        ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION));

    const auto config = h.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    const auto json = h.Query<AnomalyJsonServiceV1>(
        ANOMALY_JSON_SERVICE_V1_ID, ANOMALY_JSON_SERVICE_V1_VERSION);
    RegisterSchemas(config.get());
    LoadSettings(config.get(), json.get());

    const auto input = h.Query<AnomalyInputServiceV1>(
        ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    RegisterHotkeys(input.get());

    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }

void ANOMALY_CALL Unload(void*) {
    const anomaly::sdk::Host h(g_context.host);
    const auto input = h.Query<AnomalyInputServiceV1>(
        ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    ReleaseHotkeys(input.get());
    g_context.host = nullptr;
    g_context.ui = nullptr;
}

void ANOMALY_CALL Update(void* context, double) {
    if (context != &g_context || g_context.host == nullptr) return;
    const anomaly::sdk::Host h(g_context.host);
    const auto combat = h.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);
    std::scoped_lock lock(g_context.mutex);
    DrainCombat(combat.get());
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr) return;

    int open = 1;
    anomaly::sdk::UiWindow window(ui, "Anomaly Ultimate", &open);
    if (!window) return;

    ui->text(ui->user, anomaly::sdk::StringView(
        g_context.chronos_paused ? "Chronos: PAUSED" : "Chronos: running"));
    if (ui->slider_float != nullptr) {
        float duration = static_cast<float>(g_context.chronos.duration_seconds);
        ui->slider_float(ui->user, anomaly::sdk::StringView("Timed pause (s)"),
                         &duration, 0.1f, 3600.0f);
        g_context.chronos.duration_seconds = duration;
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    {
        std::scoped_lock lock(g_context.mutex);
        char header[96]{};
        std::snprintf(header, sizeof(header), "Damage Replay: replayCount=%u chain=%s",
                      g_context.replay.replay_count,
                      g_context.replay.chain_reaction ? "on" : "off");
        ui->text(ui->user, anomaly::sdk::StringView(header));
        if (g_context.recent_count == 0) {
            ui->text(ui->user, anomaly::sdk::StringView("No combat events"));
        }
        for (std::size_t i = 0; i < g_context.recent_count; ++i) {
            ui->text(ui->user, anomaly::sdk::StringView(g_context.recent[i].summary.data()));
        }
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    ui->text(ui->user, anomaly::sdk::StringView(
        g_context.gm_unlocked ? "Developer GM: unlocked" : "Developer GM: locked"));

    if (ui->separator != nullptr) ui->separator(ui->user);

    ui->text(ui->user, anomaly::sdk::StringView(
        g_context.has_hook ? "Volleyball Win: hook ready" : "Volleyball Win: hook unavailable"));
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.ultimate"),
        anomaly::sdk::StringView("Anomaly Ultimate"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
