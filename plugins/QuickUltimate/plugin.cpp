/*
 * 中文维护说明：本插件
 * - 本文件是该插件的主要实现入口，后续维护时优先在这里说明新增、修改和删除的行为。
 * - 当前代码逻辑保持不变；本次仅补充中文维护注释，便于后续逆向、排错和功能回溯。
 * - 不把未经验证的猜测写成实现依据；涉及游戏调用、偏移、签名或 ABI 时应注明实际证据来源。
 */
#include "../common/localization.hpp"
#include "anomaly/sdk/cpp.hpp"
#include "sequence.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>

namespace {

using anomaly::plugins::quick_ultimate::Command;
using anomaly::plugins::quick_ultimate::Phase;
using anomaly::plugins::quick_ultimate::Sequencer;

constexpr std::uint32_t kSlotCount = 4;
constexpr std::uint32_t kFirstSlotKey = '1';
constexpr std::uint32_t kUltimateKey = 'Q';

struct Context;

struct HotkeyEndpoint final {
    Context* context{};
    std::uint32_t slot{};
};

enum class LastResult : std::uint8_t {
    None,
    Running,
    Sent,
    WindowUnavailable,
    SendFailed,
};

struct Context final {
    anomaly::plugins::Localizer localizer;
    const AnomalyInputServiceV1* input{};
    const AnomalyUiServiceV1* ui{};
    std::array<AnomalyGenerationHandleV1, kSlotCount> hotkeys{};
    std::array<HotkeyEndpoint, kSlotCount> endpoints{};
    std::atomic<std::uint32_t> pending_slot{};
    std::atomic<std::uint32_t> active_slot{};
    std::atomic<std::uint32_t> last_slot{};
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<LastResult> last_result{LastResult::None};
    Sequencer sequencer;
    HWND sequence_window{};
    bool digit_down{};
    bool ultimate_down{};
    bool alt_cleared{};
};

template <typename Struct, typename Field>
// 中文说明：HasField() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyStatusV1 Status(
    const std::uint32_t code,
    const std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

// 中文说明：InputReady() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool InputReady(const AnomalyInputServiceV1* service) noexcept {
    return HasField<AnomalyInputServiceV1, decltype(AnomalyInputServiceV1::release_hotkey)>(
               service, offsetof(AnomalyInputServiceV1, release_hotkey)) &&
        service->register_hotkey != nullptr && service->release_hotkey != nullptr;
}

// 中文说明：UiReady() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_window)>(
               service, offsetof(AnomalyUiServiceV1, end_window)) &&
        service->begin_window != nullptr && service->end_window != nullptr &&
        service->text != nullptr;
}

// 中文说明：IsOwnedWindow() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool IsOwnedWindow(const HWND window) noexcept {
    if (window == nullptr) return false;
    DWORD process_id{};
    return GetWindowThreadProcessId(window, &process_id) != 0 &&
        process_id == GetCurrentProcessId();
}

// 中文说明：ResolveGameWindow() 负责执行这里的具体处理；保持现有调用关系与行为不变。
HWND ResolveGameWindow() noexcept {
    const HWND foreground = GetForegroundWindow();
    if (!IsOwnedWindow(foreground)) return nullptr;

    const HWND unreal = FindWindowW(L"UnrealWindow", nullptr);
    if (IsOwnedWindow(unreal) &&
        (unreal == foreground || GetAncestor(foreground, GA_ROOT) == unreal)) {
        return unreal;
    }
    return foreground;
}

// 中文说明：ExtendedKey() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool ExtendedKey(const std::uint32_t virtual_key) noexcept {
    switch (virtual_key) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
        return true;
    default:
        return false;
    }
}

bool PostKey(
    const HWND window,
    const std::uint32_t virtual_key,
    const bool down,
    const bool system_key) noexcept {
    if (!IsWindow(window)) return false;

    const UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    LPARAM lparam = 1;
    lparam |= static_cast<LPARAM>(scan_code & 0xFFU) << 16U;
    if (ExtendedKey(virtual_key)) lparam |= 1LL << 24U;
    if (!down) lparam |= 1LL << 30U | 1LL << 31U;

    const UINT message = system_key
        ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
        : (down ? WM_KEYDOWN : WM_KEYUP);
    return PostMessageW(
               window, message, static_cast<WPARAM>(virtual_key), lparam) != FALSE;
}

// 中文说明：EnsureWindow() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool EnsureWindow(Context& context) noexcept {
    if (IsWindow(context.sequence_window)) return true;
    context.sequence_window = ResolveGameWindow();
    return context.sequence_window != nullptr;
}

// 中文说明：SlotKey() 负责执行这里的具体处理；保持现有调用关系与行为不变。
std::uint32_t SlotKey(const std::uint32_t slot) noexcept {
    return kFirstSlotKey + slot - 1U;
}

// 中文说明：ComboHeld() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool ComboHeld(const std::uint32_t slot) noexcept {
    if (slot < 1U || slot > kSlotCount) return false;
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
        (GetAsyncKeyState(static_cast<int>(SlotKey(slot))) & 0x8000) != 0;
}

// 中文说明：RestoreAltIfHeld() 负责执行这里的具体处理；保持现有调用关系与行为不变。
void RestoreAltIfHeld(Context& context) noexcept {
    if (!context.alt_cleared) return;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0 && IsWindow(context.sequence_window)) {
        static_cast<void>(
            PostKey(context.sequence_window, VK_MENU, true, true));
    }
    context.alt_cleared = false;
}

// 中文说明：ApplyCommand() 负责执行这里的具体处理；保持现有调用关系与行为不变。
bool ApplyCommand(Context& context, const std::uint32_t slot, const Command command) noexcept {
    if (!EnsureWindow(context)) return false;

    switch (command) {
    case Command::ClearAlt:
        if (!PostKey(context.sequence_window, VK_MENU, false, true)) return false;
        context.alt_cleared = true;
        return true;
    case Command::DigitDown:
        if (!PostKey(context.sequence_window, SlotKey(slot), true, false)) return false;
        context.digit_down = true;
        return true;
    case Command::DigitUp:
        if (!PostKey(context.sequence_window, SlotKey(slot), false, false)) return false;
        context.digit_down = false;
        return true;
    case Command::UltimateDown:
        if (!PostKey(context.sequence_window, kUltimateKey, true, false)) return false;
        context.ultimate_down = true;
        return true;
    case Command::UltimateUp:
        if (!PostKey(context.sequence_window, kUltimateKey, false, false)) return false;
        context.ultimate_down = false;
        return true;
    case Command::RestoreAlt:
        RestoreAltIfHeld(context);
        return true;
    }
    return false;
}

// 中文说明：CleanupPostedKeys() 负责执行这里的具体处理；保持现有调用关系与行为不变。
void CleanupPostedKeys(Context& context) noexcept {
    if (EnsureWindow(context)) {
        if (context.ultimate_down) {
            static_cast<void>(
                PostKey(context.sequence_window, kUltimateKey, false, false));
        }
        if (context.digit_down && context.active_slot.load() != 0) {
            static_cast<void>(PostKey(
                context.sequence_window,
                SlotKey(context.active_slot.load()), false, false));
        }
    }
    context.ultimate_down = false;
    context.digit_down = false;
    RestoreAltIfHeld(context);
    context.sequence_window = nullptr;
}

// 中文说明：ReleaseHotkeys() 负责执行这里的具体处理；保持现有调用关系与行为不变。
void ReleaseHotkeys(Context& context) noexcept {
    if (!InputReady(context.input)) {
        context.hotkeys.fill({});
        return;
    }
    for (auto& hotkey : context.hotkeys) {
        if (hotkey.id != 0) {
            static_cast<void>(context.input->release_hotkey(context.input->user, hotkey));
        }
        hotkey = {};
    }
}

// 中文说明：RegisterHotkeys() 负责执行这里的具体处理；保持现有调用关系与行为不变。
AnomalyStatusV1 RegisterHotkeys(Context& context) noexcept {
    ReleaseHotkeys(context);
    for (std::uint32_t slot = 1; slot <= kSlotCount; ++slot) {
        AnomalyHotkeySpecV1 spec{sizeof(spec)};
        spec.modifiers = ANOMALY_INPUT_MODIFIER_V1_ALT;
        spec.virtual_key = SlotKey(slot);
        spec.flags = ANOMALY_HOTKEY_V1_NONE;
        const std::string id = "quick-ultimate-alt-" + std::to_string(slot);
        spec.id = anomaly::sdk::StringView(id);

        const AnomalyStatusV1 status = context.input->register_hotkey(
            context.input->user, &spec, [](void* user, AnomalyGenerationHandleV1,
                                           const AnomalyInputSnapshotV1* snapshot) noexcept {
                auto* endpoint = static_cast<HotkeyEndpoint*>(user);
                if (endpoint == nullptr || endpoint->context == nullptr ||
                    snapshot == nullptr ||
                    snapshot->struct_size <
                        offsetof(AnomalyInputSnapshotV1, modifiers) + sizeof(snapshot->modifiers) ||
                    (snapshot->modifiers & ANOMALY_INPUT_MODIFIER_V1_ALT) == 0) {
                    return;
                }
                endpoint->context->pending_slot.store(
                    endpoint->slot, std::memory_order_release);
            },
            &context.endpoints[slot - 1], &context.hotkeys[slot - 1]);
        if (status.code != ANOMALY_STATUS_V1_OK || context.hotkeys[slot - 1].id == 0) {
            ReleaseHotkeys(context);
            return status.code == ANOMALY_STATUS_V1_OK
                ? Status(ANOMALY_STATUS_V1_FAILED, "hotkey registration returned no handle")
                : status;
        }
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host,
    void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *plugin_context = nullptr;

    auto context = std::unique_ptr<Context>(new (std::nothrow) Context());
    if (!context) return Status(ANOMALY_STATUS_V1_FAILED);

    const anomaly::sdk::Host sdk_host(host);
    context->localizer = anomaly::plugins::Localizer(host);
    const auto input = sdk_host.Query<AnomalyInputServiceV1>(
        ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    const auto ui = sdk_host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    context->input = input.get();
    context->ui = ui.get();
    if (!InputReady(context->input) || !UiReady(context->ui)) {
        return Status(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "quick ultimate requires the input and UI services");
    }

    for (std::uint32_t index = 0; index < kSlotCount; ++index) {
        context->endpoints[index] = {context.get(), index + 1U};
    }
    *plugin_context = context.release();
    return anomaly::sdk::Ok();
}

// 中文说明：Start() 负责执行这里的具体处理；保持现有调用关系与行为不变。
AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);

    context->pending_slot.store(0, std::memory_order_release);
    context->active_slot.store(0, std::memory_order_release);
    context->last_slot.store(0, std::memory_order_release);
    context->phase.store(Phase::Idle, std::memory_order_release);
    context->last_result.store(LastResult::None, std::memory_order_release);
    context->sequencer.Cancel();
    CleanupPostedKeys(*context);
    return RegisterHotkeys(*context);
}

// 中文说明：Stop() 负责执行这里的具体处理；保持现有调用关系与行为不变。
AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);

    ReleaseHotkeys(*context);
    context->pending_slot.store(0, std::memory_order_release);
    CleanupPostedKeys(*context);
    context->sequencer.Cancel();
    context->active_slot.store(0, std::memory_order_release);
    context->phase.store(Phase::Idle, std::memory_order_release);
    return anomaly::sdk::Ok();
}

// 中文说明：Unload() 负责执行这里的具体处理；保持现有调用关系与行为不变。
void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    static_cast<void>(Stop(context, 0));
    delete context;
}

// 中文说明：Update() 负责执行这里的具体处理；保持现有调用关系与行为不变。
void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;

    const auto now = Sequencer::Clock::now();
    const std::uint32_t pending =
        context->pending_slot.exchange(0, std::memory_order_acq_rel);
    if (pending != 0 && context->sequencer.Queue(pending)) {
        context->last_slot.store(pending, std::memory_order_release);
        context->last_result.store(LastResult::Running, std::memory_order_release);
    }

    for (std::uint32_t step = 0; step < 8; ++step) {
        const auto state = context->sequencer.State();
        if (state.phase != Phase::Idle && !EnsureWindow(*context)) {
            context->sequencer.Fail();
            context->last_result.store(
                LastResult::WindowUnavailable, std::memory_order_release);
            break;
        }
        const auto command = context->sequencer.Poll(
            now, ComboHeld(state.active_slot));
        if (!command) break;

        if (!ApplyCommand(*context, state.active_slot, *command)) {
            context->sequencer.Fail();
            context->last_result.store(LastResult::SendFailed, std::memory_order_release);
            break;
        }
        if (*command == Command::RestoreAlt) {
            context->last_result.store(LastResult::Sent, std::memory_order_release);
        }
    }

    const std::uint32_t latest =
        context->pending_slot.exchange(0, std::memory_order_acq_rel);
    if (latest != 0 && context->sequencer.Queue(latest)) {
        context->last_slot.store(latest, std::memory_order_release);
        context->last_result.store(LastResult::Running, std::memory_order_release);
    }

    const auto state = context->sequencer.State();
    context->phase.store(state.phase, std::memory_order_release);
    context->active_slot.store(state.active_slot, std::memory_order_release);
}

// 中文说明：PhaseMessageKey() 负责执行这里的具体处理；保持现有调用关系与行为不变。
std::string_view PhaseMessageKey(const Phase phase) noexcept {
    switch (phase) {
    case Phase::Idle:
        return "state.ready";
    case Phase::ClearAlt:
    case Phase::DigitDown:
    case Phase::DigitPressed:
        return "state.switching";
    case Phase::UltimateDown:
    case Phase::UltimatePressed:
    case Phase::RepeatWait:
        return "state.holding_ultimate";
    case Phase::RestoreAlt:
    case Phase::Cooldown:
        return "state.finishing";
    case Phase::Cleanup:
        return "state.cleanup";
    }
    return "state.ready";
}

// 中文说明：PhaseFallback() 负责执行这里的具体处理；保持现有调用关系与行为不变。
std::string_view PhaseFallback(const Phase phase) noexcept {
    switch (phase) {
    case Phase::Idle:
        return "Ready";
    case Phase::ClearAlt:
    case Phase::DigitDown:
    case Phase::DigitPressed:
        return "Switching character";
    case Phase::UltimateDown:
    case Phase::UltimatePressed:
    case Phase::RepeatWait:
        return "Holding ultimate; release the shortcut to stop";
    case Phase::RestoreAlt:
    case Phase::Cooldown:
        return "Finishing";
    case Phase::Cleanup:
        return "Cleaning up";
    }
    return "Ready";
}

// 中文说明：Draw() 负责执行这里的具体处理；保持现有调用关系与行为不变。
void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    if (ui == nullptr) ui = context->ui;
    if (!UiReady(ui)) return;

    const Phase phase = context->phase.load(std::memory_order_acquire);
    const LastResult result = context->last_result.load(std::memory_order_acquire);
    const std::uint32_t slot = context->last_slot.load(std::memory_order_acquire);

    const std::string title = context->localizer.Text(
        "window.title", "Quick Ultimate");
    int open = 1;
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::set_next_window_size)>(
            ui, offsetof(AnomalyUiServiceV1, set_next_window_size)) &&
        ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 390.0F, 0.0F, 4U);
    }

    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    const std::string bindings = context->localizer.Text(
        "hotkeys.bindings", "Alt+1 / Alt+2 / Alt+3 / Alt+4");
    ui->text(ui->user, anomaly::sdk::StringView(bindings));

    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::separator)>(
            ui, offsetof(AnomalyUiServiceV1, separator)) &&
        ui->separator != nullptr) {
        ui->separator(ui->user);
    }

    const std::string state = context->localizer.Text(
        PhaseMessageKey(phase), PhaseFallback(phase));
    ui->text(ui->user, anomaly::sdk::StringView(state));

    std::string result_text;
    switch (result) {
    case LastResult::None:
        result_text = context->localizer.Text(
            "status.waiting", "Waiting for a shortcut");
        break;
    case LastResult::Running: {
        const std::string slot_text = std::to_string(slot);
        const std::array<std::string_view, 1> arguments{slot_text};
        result_text = context->localizer.Format(
            "status.running", "Triggered slot {0}", arguments);
        break;
    }
    case LastResult::Sent: {
        const std::string slot_text = std::to_string(slot);
        const std::array<std::string_view, 1> arguments{slot_text};
        result_text = context->localizer.Format(
            "status.sent", "Slot {0}: switch and ultimate key sent", arguments);
        break;
    }
    case LastResult::WindowUnavailable:
        result_text = context->localizer.Text(
            "status.window_unavailable", "Foreground game window is unavailable");
        break;
    case LastResult::SendFailed:
        result_text = context->localizer.Text(
            "status.send_failed", "The game window rejected a key message");
        break;
    }
    ui->text(ui->user, anomaly::sdk::StringView(result_text));
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-quick-ultimate"),
        anomaly::sdk::StringView("Quick Ultimate"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.0.2"),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw,
    };
    return anomaly::sdk::Ok();
}
