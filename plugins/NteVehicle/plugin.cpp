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
#include <vector>

namespace {

struct Snapshot {
    std::uint32_t flags{};
    double speed_kmh{};
    float top_speed_ratio{1.0F};
    bool friction{true};
};

struct Context {
    const AnomalyNteVehicleServiceV1* vehicle{};
    const AnomalyUiServiceV1* ui{};
    std::mutex mutex;
    Snapshot snapshot{};
    std::string status{"正在等待游戏进程中的 DT_VehicleData"};
    std::string current_vehicle_class;
    std::string selected_vehicle_id;
    std::string pending_vehicle_id;
    std::vector<std::string> vehicle_ids;

    std::atomic_bool refresh_catalog{true};
    std::atomic_bool apply_speed{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool friction_toggle{};
    std::atomic_bool started{};

    float speed_ratio{1.0F};
    bool friction_enabled{true};
} g_context;

constexpr AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
const Service* QueryService(
    const AnomalyHostApiV1* host,
    const std::string_view id,
    const std::uint32_t version) noexcept {
    if (host == nullptr || host->query_service == nullptr) return nullptr;
    const void* table{};
    const auto result = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), version, &table);
    if (result.code != ANOMALY_STATUS_V1_OK || table == nullptr) return nullptr;
    return static_cast<const Service*>(table);
}

template <typename Struct, typename Field>
bool HasField(
    const Struct* value,
    const std::size_t offset) noexcept {
    return value != nullptr &&
        value->struct_size >= offset + sizeof(Field);
}

void SetStatus(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.status = std::move(text);
}

void DrawText(const std::string_view text) {
    if (g_context.ui != nullptr && g_context.ui->text != nullptr) {
        g_context.ui->text(
            g_context.ui->user, anomaly::sdk::StringView(text));
    }
}

void RefreshCatalogOnGameThread() {
    const auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr || vehicle->vehicle_id_count == nullptr ||
        vehicle->vehicle_id_at == nullptr) {
        return;
    }

    std::uint32_t count{};
    const auto count_status = vehicle->vehicle_id_count(
        vehicle->user, &count);
    if (count_status.code != ANOMALY_STATUS_V1_OK) {
        SetStatus("读取 DT_VehicleData 失败");
        return;
    }

    std::vector<std::string> ids;
    ids.reserve(count);
    for (std::uint32_t index{}; index < count; ++index) {
        std::size_t size = ANOMALY_NTE_VEHICLE_V1_ID_MAX_BYTES + 1U;
        std::string value(size, '\0');
        const auto status = vehicle->vehicle_id_at(
            vehicle->user, index, value.data(), &size);
        if (status.code != ANOMALY_STATUS_V1_OK ||
            size == 0 || size > value.size()) {
            continue;
        }
        value.resize(size - 1U);
        if (!value.empty()) ids.push_back(std::move(value));
    }

    {
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids = std::move(ids);

        if (g_context.vehicle_ids.empty()) {
            g_context.selected_vehicle_id.clear();
            g_context.status =
                "已定位 DT_VehicleData，但当前没有读取到包含 Vehicle 的行";
        } else {
            const auto selected = std::ranges::find(
                g_context.vehicle_ids, g_context.selected_vehicle_id);
            if (selected == g_context.vehicle_ids.end()) {
                g_context.selected_vehicle_id =
                    g_context.vehicle_ids.front();
            }
            g_context.status =
                "已读取 DT_VehicleData：" +
                std::to_string(g_context.vehicle_ids.size()) + " 条";
        }
    }
}

void UpdateCurrentVehicleName() {
    const auto* vehicle = g_context.vehicle;
    if (vehicle == nullptr ||
        !HasField<
            AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::current_vehicle_class_name_utf8)>(
            vehicle,
            offsetof(
                AnomalyNteVehicleServiceV1,
                current_vehicle_class_name_utf8)) ||
        vehicle->current_vehicle_class_name_utf8 == nullptr) {
        std::scoped_lock lock(g_context.mutex);
        g_context.current_vehicle_class.clear();
        return;
    }

    std::size_t size = 128U;
    std::string class_name(size, '\0');
    const auto status = vehicle->current_vehicle_class_name_utf8(
        vehicle->user, class_name.data(), &size);

    std::scoped_lock lock(g_context.mutex);
    if (status.code == ANOMALY_STATUS_V1_OK && size > 0 && size <= class_name.size()) {
        class_name.resize(size - 1U);
        g_context.current_vehicle_class = std::move(class_name);
    } else {
        g_context.current_vehicle_class.clear();
    }
}

void Update() {
    if (g_context.vehicle == nullptr ||
        !g_context.started.load(std::memory_order_acquire)) {
        return;
    }

    if (g_context.refresh_catalog.exchange(false, std::memory_order_acq_rel)) {
        RefreshCatalogOnGameThread();
    }

    std::string pending_id;
    {
        std::scoped_lock lock(g_context.mutex);
        pending_id.swap(g_context.pending_vehicle_id);
    }
    if (!pending_id.empty()) {
        const auto status = g_context.vehicle->set_summon_vehicle_id(
            g_context.vehicle->user, anomaly::sdk::StringView(pending_id));
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.selected_vehicle_id = pending_id;
            g_context.status = "已选择：" + pending_id;
        } else {
            SetStatus("Vehicle 选择失败：该 ID 不在实际 DT_VehicleData 表中");
        }
    }

    if (g_context.reset.exchange(false, std::memory_order_acq_rel)) {
        const auto status = g_context.vehicle->reset(g_context.vehicle->user);
        SetStatus(
            status.code == ANOMALY_STATUS_V1_OK
                ? "已恢复默认载具设置"
                : "恢复载具设置失败");
    }

    if (g_context.apply_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio{};
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.speed_ratio;
        }
        const auto status = g_context.vehicle->set_top_speed_ratio(
            g_context.vehicle->user, ratio);
        SetStatus(
            status.code == ANOMALY_STATUS_V1_OK
                ? "速度倍率已应用"
                : "速度倍率应用失败");
    }

    if (g_context.summon.exchange(false, std::memory_order_acq_rel)) {
        const auto status = g_context.vehicle->summon_vehicle(
            g_context.vehicle->user);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.status =
                "已发送召唤：" + g_context.selected_vehicle_id +
                "；等待新 BP_vehicle 实体绑定 Player";
        } else {
            SetStatus("召唤载具失败：Host 未通过实际召唤 ABI 校验");
        }
    }

    if (g_context.friction_toggle.exchange(false, std::memory_order_acq_rel)) {
        bool enabled{};
        {
            std::scoped_lock lock(g_context.mutex);
            enabled = !g_context.friction_enabled;
        }
        const auto status =
            g_context.vehicle->set_wheel_friction_enabled(
                g_context.vehicle->user, enabled ? 1U : 0U);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.friction_enabled = enabled;
            g_context.status =
                enabled ? "车轮摩擦已开启" : "车轮摩擦已关闭";
        } else {
            SetStatus("车轮摩擦切换失败");
        }
    }

    AnomalyNteVehicleSnapshotV1 snapshot{sizeof(snapshot)};
    const auto snapshot_status =
        g_context.vehicle->snapshot(g_context.vehicle->user, &snapshot);

    {
        std::scoped_lock lock(g_context.mutex);
        if (snapshot_status.code == ANOMALY_STATUS_V1_OK) {
            g_context.snapshot.flags = snapshot.flags;
            g_context.snapshot.speed_kmh = snapshot.speed_kmh;
            g_context.snapshot.top_speed_ratio = snapshot.top_speed_ratio;
            g_context.snapshot.friction =
                snapshot.wheel_friction_enabled != 0;
        } else {
            g_context.snapshot.flags = 0;
        }
    }

    UpdateCurrentVehicleName();
}

void Draw() {
    const auto* ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr ||
        ui->end_window == nullptr || ui->text == nullptr ||
        ui->button == nullptr || ui->checkbox == nullptr ||
        ui->slider_float == nullptr) {
        return;
    }

    int open = 1;
    // UI contract: once begin_window() is called, end_window() is mandatory even
    // when begin_window() returns 0. Keep the false-return path explicit so this
    // lifecycle rule is impossible to miss during later UI changes.
    const int window_visible = ui->begin_window(
        ui->user, anomaly::sdk::StringView("NTE Vehicle"), &open, 0);
    if (!window_visible) {
        ui->end_window(ui->user);
        return;
    }

    Snapshot snapshot;
    std::string status;
    std::string current_class;
    std::string selected;
    std::vector<std::string> ids;
    float ratio{};
    bool friction{};
    {
        std::scoped_lock lock(g_context.mutex);
        snapshot = g_context.snapshot;
        status = g_context.status;
        current_class = g_context.current_vehicle_class;
        selected = g_context.selected_vehicle_id;
        ids = g_context.vehicle_ids;
        ratio = g_context.speed_ratio;
        friction = g_context.friction_enabled;
    }

    DrawText("NTE 载具控制");
    DrawText(status);

    if (ui->button(
            ui->user, anomaly::sdk::StringView("刷新 DT_VehicleData"),
            0.0F, 0.0F)) {
        g_context.refresh_catalog.store(true, std::memory_order_release);
    }

    DrawText(
        "Vehicle 表条目：" + std::to_string(ids.size()) +
        "；当前选择：" +
        (selected.empty() ? std::string("<未选择>") : selected));

    for (std::size_t index{}; index < ids.size(); ++index) {
        int checked = ids[index] == selected ? 1 : 0;
        const std::string checkbox_id =
            "选择##vehicle_row_" + std::to_string(index);
        if (ui->checkbox(
                ui->user, anomaly::sdk::StringView(checkbox_id),
                &checked) && checked != 0) {
            std::scoped_lock lock(g_context.mutex);
            if (g_context.selected_vehicle_id != ids[index]) {
                g_context.pending_vehicle_id = ids[index];
            }
        }
        DrawText(ids[index]);
    }

    if (ui->button(
            ui->user, anomaly::sdk::StringView("召唤当前选择"),
            0.0F, 0.0F)) {
        g_context.summon.store(true, std::memory_order_release);
    }

    if ((snapshot.flags & ANOMALY_NTE_VEHICLE_V1_VALID) == 0) {
        DrawText("当前载具：未检测到");
    } else {
        DrawText(
            "当前载具：" +
            (current_class.empty() ? std::string("已检测") : current_class));
        DrawText(
            "当前速度：" + std::to_string(snapshot.speed_kmh) + " km/h");
        DrawText(
            "最高速度倍率：" +
            std::to_string(snapshot.top_speed_ratio) + "x");

        if (ui->slider_float(
                ui->user, anomaly::sdk::StringView("倍率"),
                &ratio, 0.05F, 20.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.speed_ratio = ratio;
        }
        if (ui->button(
                ui->user, anomaly::sdk::StringView("应用速度倍率"),
                0.0F, 0.0F)) {
            g_context.apply_speed.store(true, std::memory_order_release);
        }
        if (ui->button(
                ui->user, anomaly::sdk::StringView("恢复 1.0x"),
                0.0F, 0.0F)) {
            g_context.reset.store(true, std::memory_order_release);
        }

        DrawText(
            std::string("车轮摩擦：") +
            (friction ? "开启" : "关闭"));
        if (ui->button(
                ui->user, anomaly::sdk::StringView("切换车轮摩擦"),
                0.0F, 0.0F)) {
            g_context.friction_toggle.store(true, std::memory_order_release);
        }
    }

}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }

    const auto* vehicle = QueryService<AnomalyNteVehicleServiceV1>(
        host,
        ANOMALY_NTE_VEHICLE_SERVICE_V1_ID,
        ANOMALY_NTE_VEHICLE_SERVICE_V1_VERSION);
    const auto* ui = QueryService<AnomalyUiServiceV1>(
        host,
        ANOMALY_UI_SERVICE_V1_ID,
        ANOMALY_UI_SERVICE_V1_VERSION);
    if (vehicle == nullptr || ui == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    if (vehicle->snapshot == nullptr ||
        vehicle->set_top_speed_ratio == nullptr ||
        vehicle->summon_vehicle == nullptr ||
        vehicle->set_wheel_friction_enabled == nullptr ||
        vehicle->reset == nullptr ||
        vehicle->vehicle_id_count == nullptr ||
        vehicle->vehicle_id_at == nullptr ||
        vehicle->set_summon_vehicle_id == nullptr ||
        ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr || ui->button == nullptr ||
        ui->checkbox == nullptr || ui->slider_float == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    g_context.vehicle = vehicle;
    g_context.ui = ui;
    g_context.started.store(false, std::memory_order_release);
    g_context.refresh_catalog.store(true, std::memory_order_release);
    g_context.apply_speed.store(false, std::memory_order_release);
    g_context.reset.store(false, std::memory_order_release);
    g_context.summon.store(false, std::memory_order_release);
    g_context.friction_toggle.store(false, std::memory_order_release);
    g_context.snapshot = {};
    g_context.current_vehicle_class.clear();
    g_context.selected_vehicle_id.clear();
    g_context.pending_vehicle_id.clear();
    g_context.vehicle_ids.clear();
    g_context.speed_ratio = 1.0F;
    g_context.friction_enabled = true;
    g_context.status = "正在读取游戏进程中的 DT_VehicleData";
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    g_context.started.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(
    void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    g_context.started.store(false, std::memory_order_release);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    g_context.vehicle = nullptr;
    g_context.ui = nullptr;
}

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) {
    if (plugin_context == &g_context) Update();
}

void ANOMALY_CALL DrawCallback(
    void* plugin_context, const AnomalyUiServiceV1*) {
    if (plugin_context == &g_context) Draw();
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
        anomaly::sdk::StringView("anomaly.local.nte-vehicle"),
        anomaly::sdk::StringView("NTE Vehicle"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.8.1"),
        Load,
        Start,
        Stop,
        Unload,
        UpdateCallback,
        DrawCallback};
    return anomaly::sdk::Ok();
}
