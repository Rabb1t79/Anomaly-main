/*
 * 中文维护说明：本插件
 * - 本文件是该插件的主要实现入口，后续维护时优先在这里说明新增、修改和删除的行为。
 * - 当前代码逻辑保持不变；本次仅补充中文维护注释，便于后续逆向、排错和功能回溯。
 * - 不把未经验证的猜测写成实现依据；涉及游戏调用、偏移、签名或 ABI 时应注明实际证据来源。
 */
#include "anomaly/sdk/cpp.hpp"
#include "../common/localization.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kStatusMessageCapacity = 192;

enum class RequestKind : std::uint8_t {
    None,
    Move,
    Stop,
};

struct PendingRequest final {
    RequestKind kind{};
    std::array<double, 3> destination{};
};

struct Context final {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    std::array<double, 3> destination{};
    PendingRequest pending{};
    std::array<char, kStatusMessageCapacity> result_message{};
    std::uint32_t result_code{ANOMALY_STATUS_V1_UNAVAILABLE};
    bool has_result{};
    bool started{};
    std::mutex mutex;
} g_context;

template <typename Struct, typename Field>
// HasField 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

// StatusCode 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
constexpr AnomalyStatusV1 StatusCode(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
struct ServiceQuery final {
    const Service* service{};
    AnomalyStatusV1 status{StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};

// bool 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
    [[nodiscard]] explicit operator bool() const noexcept { return service != nullptr; }
};

template <typename Service>
ServiceQuery<Service> QueryService(
    const AnomalyHostApiV1* host,
    const std::string_view id,
    const std::uint32_t minimum_version) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->api_major != ANOMALY_PLUGIN_API_V1_MAJOR ||
        host->query_service == nullptr) {
        return {};
    }

    const void* table{};
    const AnomalyStatusV1 status = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), minimum_version, &table);
    if (status.code != ANOMALY_STATUS_V1_OK || table == nullptr) {
        return {nullptr, status};
    }
    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t kServicePrefix = offsetof(Service, user) + sizeof(void*);
    if (service->struct_size < kServicePrefix ||
        service->service_version < minimum_version) {
        return {};
    }
    return {service, StatusCode(ANOMALY_STATUS_V1_OK)};
}

// UiReady 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return HasField<AnomalyUiServiceV1,
               decltype(AnomalyUiServiceV1::input_double)>(
               service, offsetof(AnomalyUiServiceV1, input_double)) &&
        service->service_version >= ANOMALY_UI_SERVICE_V1_VERSION &&
        service->begin_window != nullptr && service->end_window != nullptr &&
        service->text != nullptr && service->button != nullptr &&
        service->input_double != nullptr;
}

// NavigationReady 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
bool NavigationReady(const AnomalyNteNavigationServiceV1* service) noexcept {
    return HasField<AnomalyNteNavigationServiceV1,
               decltype(AnomalyNteNavigationServiceV1::stop_movement)>(
               service, offsetof(AnomalyNteNavigationServiceV1, stop_movement)) &&
        service->service_version >= ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION &&
        service->move_to_location != nullptr && service->stop_movement != nullptr;
}

// StatusName 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
const char* StatusName(const std::uint32_t code) noexcept {
    switch (code) {
    case ANOMALY_STATUS_V1_OK: return "OK";
    case ANOMALY_STATUS_V1_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "UNAVAILABLE";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "NOT_FOUND";
    case ANOMALY_STATUS_V1_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case ANOMALY_STATUS_V1_FAILED: return "FAILED";
    case ANOMALY_STATUS_V1_TIMEOUT: return "TIMEOUT";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ANOMALY_STATUS_V1_CONFLICT: return "CONFLICT";
    case ANOMALY_STATUS_V1_CANCELLED: return "CANCELLED";
    default: return "UNKNOWN_STATUS";
    }
}

// RecordResult 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
void RecordResult(const AnomalyStatusV1 status) noexcept {
    std::scoped_lock lock(g_context.mutex);
    g_context.has_result = true;
    g_context.result_code = status.code;
    g_context.result_message.fill('\0');
    if (status.message.data == nullptr || status.message.size == 0) return;
    const std::size_t count = (std::min)(
        status.message.size, g_context.result_message.size() - 1U);
    std::memcpy(g_context.result_message.data(), status.message.data, count);
}

// QueueMove 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
void QueueMove(const std::array<double, 3>& destination) noexcept {
    if (!std::isfinite(destination[0]) || !std::isfinite(destination[1]) ||
        !std::isfinite(destination[2])) {
        RecordResult(StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT));
        return;
    }
    std::scoped_lock lock(g_context.mutex);
    if (!g_context.started) return;
    g_context.pending = {RequestKind::Move, destination};
    g_context.has_result = false;
    g_context.result_message.fill('\0');
}

// QueueStop 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
void QueueStop() noexcept {
    std::scoped_lock lock(g_context.mutex);
    if (!g_context.started) return;
    g_context.pending = {RequestKind::Stop, {}};
    g_context.has_result = false;
    g_context.result_message.fill('\0');
}

// DrawText 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
void DrawText(const AnomalyUiServiceV1* ui, const std::string_view text) {
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

// DrawStatus 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
void DrawStatus(const AnomalyUiServiceV1* ui) {
    RequestKind pending{};
    bool has_result{};
    std::uint32_t result_code{};
    std::array<char, kStatusMessageCapacity> result_message{};
    {
        std::scoped_lock lock(g_context.mutex);
        pending = g_context.pending.kind;
        has_result = g_context.has_result;
        result_code = g_context.result_code;
        result_message = g_context.result_message;
    }

    if (pending == RequestKind::Move) {
        DrawText(ui, g_context.localizer.Text(
            "status.move_queued", "Navigation: MOVE QUEUED"));
        return;
    }
    if (pending == RequestKind::Stop) {
        DrawText(ui, g_context.localizer.Text(
            "status.stop_queued", "Navigation: STOP QUEUED"));
        return;
    }
    if (!has_result) {
        DrawText(ui, g_context.localizer.Text("status.idle", "Navigation: IDLE"));
        return;
    }

    const std::string_view status = StatusName(result_code);
    if (result_message[0] == '\0') {
        const std::array arguments{status};
        DrawText(ui, g_context.localizer.Format(
            "status.result", "Navigation: {0}", arguments));
    } else {
        const std::array arguments{
            status, std::string_view(result_message.data())};
        DrawText(ui, g_context.localizer.Format(
            "status.result_detail", "Navigation: {0} - {1}", arguments));
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host,
    void** plugin_context) {
    if (plugin_context == nullptr) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const auto ui = QueryService<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui) return ui.status;
    const auto navigation = QueryService<AnomalyNteNavigationServiceV1>(
        host, ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID,
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION);
    if (!navigation) return navigation.status;
    if (!UiReady(ui.service) || !NavigationReady(navigation.service)) {
        return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    {
        std::scoped_lock lock(g_context.mutex);
        g_context.host = host;
        g_context.localizer = anomaly::plugins::Localizer(host);
        g_context.destination = {};
        g_context.pending = {};
        g_context.result_message.fill('\0');
        g_context.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_context.has_result = false;
        g_context.started = false;
    }
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

// Start 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    std::scoped_lock lock(g_context.mutex);
    g_context.pending = {};
    g_context.result_message.fill('\0');
    g_context.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
    g_context.has_result = false;
    g_context.started = true;
    return anomaly::sdk::Ok();
}

// Stop 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    std::scoped_lock lock(g_context.mutex);
    g_context.started = false;
    g_context.pending = {};
    return anomaly::sdk::Ok();
}

// Unload 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    std::scoped_lock lock(g_context.mutex);
    g_context.host = nullptr;
    g_context.localizer = {};
    g_context.pending = {};
    g_context.started = false;
}

// Update 根据函数体中的具体游戏对象、Anomaly 服务和运行时状态完成当前位置/导航/拾取/传送流程；函数的返回值或状态字段直接反映目标对象不存在、服务不可用或调用成功等结果。
void ANOMALY_CALL Update(void* plugin_context, double) {
    if (plugin_context != &g_context) return;

    PendingRequest pending;
    const AnomalyHostApiV1* host{};
    {
        std::scoped_lock lock(g_context.mutex);
        if (!g_context.started || g_context.pending.kind == RequestKind::None) return;
        pending = g_context.pending;
        g_context.pending = {};
        host = g_context.host;
    }
    if (host == nullptr) {
        RecordResult(StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE));
        return;
    }

    const auto navigation = QueryService<AnomalyNteNavigationServiceV1>(
        host, ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID,
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION);
    if (!navigation || !NavigationReady(navigation.service)) {
        RecordResult(
            navigation ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : navigation.status);
        return;
    }
    const AnomalyStatusV1 result = pending.kind == RequestKind::Move
        ? navigation.service->move_to_location(
              navigation.service->user, pending.destination.data())
        : navigation.service->stop_movement(navigation.service->user);
    RecordResult(result);
}

void ANOMALY_CALL Draw(
    void* plugin_context,
    const AnomalyUiServiceV1* ui) {
    if (plugin_context != &g_context) return;

    const AnomalyHostApiV1* host{};
    std::array<double, 3> destination{};
    {
        std::scoped_lock lock(g_context.mutex);
        host = g_context.host;
        destination = g_context.destination;
    }
    if (host == nullptr) return;

    const AnomalyUiServiceV1* input_ui = UiReady(ui) ? ui : nullptr;
    if (input_ui == nullptr) {
        const auto queried = QueryService<AnomalyUiServiceV1>(
            host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
        input_ui = queried.service;
    }
    if (!UiReady(input_ui)) return;

    int open = 1;
    const std::string title = g_context.localizer.Label(
        "window.title", "Navigation", "navigation");
    anomaly::sdk::UiWindow window(input_ui, title, &open);
    if (!window) return;

    input_ui->input_double(
        input_ui->user, anomaly::sdk::StringView("X"), &destination[0], 0.0, 0.0);
    input_ui->input_double(
        input_ui->user, anomaly::sdk::StringView("Y"), &destination[1], 0.0, 0.0);
    input_ui->input_double(
        input_ui->user, anomaly::sdk::StringView("Z"), &destination[2], 0.0, 0.0);
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.destination = destination;
    }

    const std::string move = g_context.localizer.Label(
        "action.move", "Start navigation", "start-navigation");
    if (input_ui->button(
            input_ui->user, anomaly::sdk::StringView(move), 0.0F, 0.0F) != 0) {
        QueueMove(destination);
    }
    const std::string stop = g_context.localizer.Label(
        "action.stop", "Stop navigation", "stop-navigation");
    if (input_ui->button(
            input_ui->user, anomaly::sdk::StringView(stop), 0.0F, 0.0F) != 0) {
        QueueStop();
    }
    DrawStatus(input_ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-navmesh-demo"),
        anomaly::sdk::StringView("Navigation"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.2.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
