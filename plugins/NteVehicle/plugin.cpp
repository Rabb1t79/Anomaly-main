// 中文维护说明：UI 线程只提交意图并读取快照；所有 NTE 车辆服务调用仅在 Game Update 中发生。
// 不保存 Draw 回调传入的 AnomalyUiServiceV1*，避免跨帧保留 UI 生命周期对象。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct VehicleView {
    std::uint32_t flags{};
    double speed_kmh{};
    float speed_ratio{1.0F};
    bool wheel_friction{true};
    bool catalog_valid{};
    std::string selected_id;
    std::string current_class;
    std::string catalog_status{"等待 Host 验证 DT_VehicleData"};
    std::string operation_status{"尚无操作"};
    std::string summon_status{"尚无召唤请求"};
    std::vector<std::string> ids;
};

struct Context {
    const AnomalyNteVehicleServiceV1* vehicle{};
    std::atomic_bool started{false};
    std::atomic_bool refresh_requested{true};
    std::atomic_bool summon_requested{false};
    std::atomic_bool speed_apply_requested{false};
    std::atomic_bool reset_requested{false};
    std::atomic<int> friction_request{-1};

    std::mutex mutex;
    VehicleView view{};
    std::string pending_selection;
    float requested_speed_ratio{1.0F};
    std::uint64_t update_count{};
} g_context;

constexpr AnomalyStatusV1 Status(
    const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::char_traits<char>::length(message)}};
}

template <typename Service>
const Service* QueryService(
    const AnomalyHostApiV1* host, const std::string_view id,
    const std::uint32_t version) noexcept {
    if (host == nullptr || host->query_service == nullptr) return nullptr;
    const void* table{};
    const auto status = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), version, &table);
    if (status.code != ANOMALY_STATUS_V1_OK || table == nullptr) return nullptr;
    return static_cast<const Service*>(table);
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

std::string MessageOf(const AnomalyStatusV1& status, const std::string_view fallback) {
    if (status.message.data != nullptr && status.message.size != 0)
        return std::string(status.message.data, status.message.size);
    return std::string(fallback);
}

void SetOperationStatus(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.view.operation_status = std::move(text);
}

void RefreshCatalogOnGameThread() {
    const auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr || vehicle->vehicle_id_count == nullptr ||
        vehicle->vehicle_id_at == nullptr) return;

    std::uint32_t count{};
    const auto count_status = vehicle->vehicle_id_count(vehicle->user, &count);
    if (count_status.code != ANOMALY_STATUS_V1_OK || count == 0 || count > 4096U) {
        const std::string message = MessageOf(
            count_status, count == 0 ? "DT_VehicleData 未通过验证或没有有效行" : "车辆表条目数异常");
        {
            std::scoped_lock lock(g_context.mutex);
            if (g_context.view.ids.empty()) g_context.view.catalog_valid = false;
            g_context.view.catalog_status = message;
        }
        return;
    }

    std::vector<std::string> ids;
    ids.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::size_t size = ANOMALY_NTE_VEHICLE_V1_ID_MAX_BYTES + 1U;
        std::string value(size, '\0');
        const auto status = vehicle->vehicle_id_at(
            vehicle->user, index, value.data(), &size);
        if (status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > value.size()) {
            SetOperationStatus("读取车辆表第 " + std::to_string(index) +
                " 行失败；拒绝把不完整列表标记为已验证");
            return;
        }
        value.resize(size - 1U);
        if (value.empty()) {
            SetOperationStatus("车辆表中出现空 VehicleID；拒绝不完整列表");
            return;
        }
        ids.push_back(std::move(value));
    }

    std::ranges::sort(ids);
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        SetOperationStatus("Host 返回重复 VehicleID；拒绝将该列表标记为已验证");
        return;
    }

    std::string selected;
    {
        std::scoped_lock lock(g_context.mutex);
        selected = g_context.view.selected_id;
    }
    if (std::ranges::find(ids, selected) == ids.end()) selected = ids.front();
    const auto select_status = vehicle->set_summon_vehicle_id(
        vehicle->user, anomaly::sdk::StringView(selected));

    {
        std::scoped_lock lock(g_context.mutex);
        g_context.view.ids = std::move(ids);
        g_context.view.selected_id = selected;
        g_context.view.catalog_valid = select_status.code == ANOMALY_STATUS_V1_OK;
        g_context.view.catalog_status = select_status.code == ANOMALY_STATUS_V1_OK
            ? "已通过实时 DataTable / DataTable 类名 / VehicleData 行结构 / VehicleID 字段校验，共 " +
                std::to_string(g_context.view.ids.size()) + " 条"
            : "表内容已读取，但 Host 拒绝当前选择：" + MessageOf(select_status, "选择校验失败");
    }
}

void UpdateCurrentVehicleClass() {
    const auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::current_vehicle_class_name_utf8)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, current_vehicle_class_name_utf8)) ||
        vehicle->current_vehicle_class_name_utf8 == nullptr) return;

    std::string name(256, '\0');
    std::size_t size = name.size();
    const auto status = vehicle->current_vehicle_class_name_utf8(
        vehicle->user, name.data(), &size);
    std::scoped_lock lock(g_context.mutex);
    if (status.code == ANOMALY_STATUS_V1_OK && size > 0 && size <= name.size()) {
        name.resize(size - 1U);
        g_context.view.current_class = std::move(name);
    } else {
        g_context.view.current_class.clear();
    }
}

void UpdateSummonStatus() {
    const auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::summon_status_utf8)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, summon_status_utf8)) ||
        vehicle->summon_status_utf8 == nullptr) return;

    std::string text(512, '\0');
    std::size_t size = text.size();
    const auto status = vehicle->summon_status_utf8(vehicle->user, text.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > text.size()) return;
    text.resize(size - 1U);
    std::scoped_lock lock(g_context.mutex);
    g_context.view.summon_status = std::move(text);
}

void Update() {
    const auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr || !g_context.started.load(std::memory_order_acquire)) return;
    ++g_context.update_count;

    if (g_context.refresh_requested.exchange(false, std::memory_order_acq_rel) ||
        (g_context.update_count % 30U == 0U)) {
        RefreshCatalogOnGameThread();
    }

    std::string pending_selection;
    {
        std::scoped_lock lock(g_context.mutex);
        pending_selection.swap(g_context.pending_selection);
    }
    if (!pending_selection.empty()) {
        const auto status = vehicle->set_summon_vehicle_id(
            vehicle->user, anomaly::sdk::StringView(pending_selection));
        std::scoped_lock lock(g_context.mutex);
        if (status.code == ANOMALY_STATUS_V1_OK &&
            std::ranges::find(g_context.view.ids, pending_selection) != g_context.view.ids.end()) {
            g_context.view.selected_id = pending_selection;
            g_context.view.operation_status = "已选中真实表项：" + pending_selection;
        } else {
            g_context.view.operation_status = "选择被 Host 拒绝：" +
                MessageOf(status, "ID 不在已验证的 DT_VehicleData 列表中");
        }
    }

    if (g_context.reset_requested.exchange(false, std::memory_order_acq_rel)) {
        const auto status = vehicle->reset(vehicle->user);
        SetOperationStatus(status.code == ANOMALY_STATUS_V1_OK
            ? "已请求恢复默认车辆参数" : "恢复车辆参数失败：" + MessageOf(status, "Host 未确认"));
    }

    if (g_context.speed_apply_requested.exchange(false, std::memory_order_acq_rel)) {
        float ratio{};
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.requested_speed_ratio;
        }
        const auto status = vehicle->set_top_speed_ratio(vehicle->user, ratio);
        SetOperationStatus(status.code == ANOMALY_STATUS_V1_OK
            ? "速度倍率调用已被 Host 接受；以回读值为准"
            : "速度倍率未应用：" + MessageOf(status, "当前载具/移动组件不可用"));
    }

    const int friction = g_context.friction_request.exchange(-1, std::memory_order_acq_rel);
    if (friction >= 0) {
        const auto status = vehicle->set_wheel_friction_enabled(
            vehicle->user, friction != 0 ? 1U : 0U);
        SetOperationStatus(status.code == ANOMALY_STATUS_V1_OK
            ? (friction != 0 ? "车轮摩擦已设置为开启" : "车轮摩擦已设置为关闭")
            : "车轮摩擦设置失败：" + MessageOf(status, "当前移动组件不可用"));
    }

    if (g_context.summon_requested.exchange(false, std::memory_order_acq_rel)) {
        std::string selected;
        bool valid{};
        {
            std::scoped_lock lock(g_context.mutex);
            selected = g_context.view.selected_id;
            valid = g_context.view.catalog_valid &&
                std::ranges::find(g_context.view.ids, selected) != g_context.view.ids.end();
        }
        if (!valid || selected.empty()) {
            SetOperationStatus("拒绝召唤：当前没有通过验证的 VehicleID 选择");
        } else {
            const auto select_status = vehicle->set_summon_vehicle_id(
                vehicle->user, anomaly::sdk::StringView(selected));
            if (select_status.code != ANOMALY_STATUS_V1_OK) {
                SetOperationStatus("召唤前选择校验失败：" + MessageOf(select_status, "VehicleID 无效"));
            } else {
                const auto status = vehicle->summon_vehicle(vehicle->user);
                SetOperationStatus(status.code == ANOMALY_STATUS_V1_OK
                    ? "已提交召唤请求；请查看下方 Host 的实体 / Owner / 坐标读回结果"
                    : "载具召唤调用失败：" + MessageOf(status, "原生召唤 ABI 未通过验证"));
            }
        }
    }

    AnomalyNteVehicleSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const auto snapshot_status = vehicle->snapshot(vehicle->user, &snapshot);
    {
        std::scoped_lock lock(g_context.mutex);
        if (snapshot_status.code == ANOMALY_STATUS_V1_OK) {
            g_context.view.flags = snapshot.flags;
            g_context.view.speed_kmh = snapshot.speed_kmh;
            g_context.view.speed_ratio = snapshot.top_speed_ratio;
            g_context.view.wheel_friction = snapshot.wheel_friction_enabled != 0;
        } else {
            g_context.view.flags = 0;
        }
    }
    UpdateCurrentVehicleClass();
    UpdateSummonStatus();
}

void Draw(const AnomalyUiServiceV1* ui) {
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr || ui->button == nullptr || ui->checkbox == nullptr ||
        ui->slider_float == nullptr) return;

    int open = 1;
    if (ui->begin_window(ui->user, anomaly::sdk::StringView("NTE Vehicle"), &open, 0) == 0) return;

    VehicleView view;
    float ratio{};
    {
        std::scoped_lock lock(g_context.mutex);
        view = g_context.view;
        ratio = g_context.requested_speed_ratio;
    }
    const auto text = [ui](const std::string& value) {
        ui->text(ui->user, anomaly::sdk::StringView(value));
    };

    text("NTE 载具控制");
    text("车辆表验证：" + view.catalog_status);
    text("当前选择：" + (view.selected_id.empty() ? std::string("未选择") : view.selected_id));
    if (ui->button(ui->user, anomaly::sdk::StringView("重新验证车辆表"), 0.0F, 0.0F))
        g_context.refresh_requested.store(true, std::memory_order_release);

    if (ui->begin_child != nullptr && ui->end_child != nullptr) {
        if (ui->begin_child(ui->user, anomaly::sdk::StringView("vehicle_catalog"), 0.0F, 220.0F, 0) != 0) {
            for (std::size_t i = 0; i < view.ids.size(); ++i) {
                int selected = view.ids[i] == view.selected_id ? 1 : 0;
                const std::string label = view.ids[i] + "##vehicle_" + std::to_string(i);
                if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &selected) != 0 && selected != 0) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.pending_selection = view.ids[i];
                }
            }
            ui->end_child(ui->user);
        }
    } else {
        for (std::size_t i = 0; i < view.ids.size(); ++i) {
            int selected = view.ids[i] == view.selected_id ? 1 : 0;
            const std::string label = view.ids[i] + "##vehicle_" + std::to_string(i);
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &selected) != 0 && selected != 0) {
                std::scoped_lock lock(g_context.mutex);
                g_context.pending_selection = view.ids[i];
            }
        }
    }

    if (ui->button(ui->user, anomaly::sdk::StringView("召唤已选载具"), 0.0F, 0.0F))
        g_context.summon_requested.store(true, std::memory_order_release);
    text("操作状态：" + view.operation_status);
    text("召唤验证状态：" + view.summon_status);

    if ((view.flags & ANOMALY_NTE_VEHICLE_V1_VALID) == 0) {
        text("当前载具：未检测到");
    } else {
        text("当前载具：" + (view.current_class.empty() ? std::string("类型名未解析") : view.current_class));
        text("当前速率：" + std::to_string(view.speed_kmh) + " km/h");
        text((view.flags & ANOMALY_NTE_VEHICLE_V1_HAS_SPEED) != 0
            ? "速率回读：已验证" : "速率回读：尚未通过有效性验证");
        text("最高速率倍率回读：" + std::to_string(view.speed_ratio) + "x");

        if (ui->slider_float(ui->user, anomaly::sdk::StringView("最高速率倍率"),
                &ratio, 0.05F, 20.0F) != 0) {
            std::scoped_lock lock(g_context.mutex);
            g_context.requested_speed_ratio = ratio;
        }
        if (ui->button(ui->user, anomaly::sdk::StringView("应用倍率"), 0.0F, 0.0F))
            g_context.speed_apply_requested.store(true, std::memory_order_release);
        if (ui->button(ui->user, anomaly::sdk::StringView("恢复 1.0x"), 0.0F, 0.0F))
            g_context.reset_requested.store(true, std::memory_order_release);

        int friction = view.wheel_friction ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView("车轮摩擦"), &friction) != 0)
            g_context.friction_request.store(friction != 0 ? 1 : 0, std::memory_order_release);
    }
    ui->end_window(ui->user);
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr)
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto* vehicle = QueryService<AnomalyNteVehicleServiceV1>(
        host, ANOMALY_NTE_VEHICLE_SERVICE_V1_ID, ANOMALY_NTE_VEHICLE_SERVICE_V1_VERSION);
    const auto* ui = QueryService<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (vehicle == nullptr || ui == nullptr ||
        vehicle->snapshot == nullptr || vehicle->set_top_speed_ratio == nullptr ||
        vehicle->set_wheel_friction_enabled == nullptr || vehicle->reset == nullptr ||
        vehicle->summon_vehicle == nullptr || vehicle->vehicle_id_count == nullptr ||
        vehicle->vehicle_id_at == nullptr || vehicle->set_summon_vehicle_id == nullptr ||
        ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr ||
        ui->button == nullptr || ui->checkbox == nullptr || ui->slider_float == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "vehicle or UI service is incomplete");
    }

    g_context.vehicle = vehicle;
    g_context.started.store(false, std::memory_order_release);
    g_context.refresh_requested.store(true, std::memory_order_release);
    g_context.summon_requested.store(false, std::memory_order_release);
    g_context.speed_apply_requested.store(false, std::memory_order_release);
    g_context.reset_requested.store(false, std::memory_order_release);
    g_context.friction_request.store(-1, std::memory_order_release);
    g_context.update_count = 0;
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.view = {};
        g_context.pending_selection.clear();
        g_context.requested_speed_ratio = 1.0F;
        g_context.view.catalog_status = "等待 Host 验证 DT_VehicleData";
        g_context.view.operation_status = "尚无操作";
        g_context.view.summon_status = "尚无召唤请求";
    }
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(false, std::memory_order_release);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    g_context.vehicle = nullptr;
}

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) {
    if (plugin_context == &g_context) Update();
}

void ANOMALY_CALL DrawCallback(
    void* plugin_context, const AnomalyUiServiceV1* ui) {
    if (plugin_context == &g_context) Draw(ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-vehicle"),
        anomaly::sdk::StringView("NTE Vehicle"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.9.0"),
        Load,
        Start,
        Stop,
        Unload,
        UpdateCallback,
        DrawCallback};
    return anomaly::sdk::Ok();
}
