#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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
    std::vector<std::string> vehicles;
    std::uint64_t catalog_generation{};
    float selected_index{};
    std::string status{"正在读取 NTE Vehicle 列表"};
    std::string last_summon;
    std::atomic_bool apply_speed{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool friction_toggle{};
    std::atomic_bool started{};
    float speed_ratio{1.0F};
    bool friction_enabled{true};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}
AnomalyStatusV1 Status(std::uint32_t code) noexcept { return {code, 0, {}}; }

template <typename Service>
const Service* QueryService(const AnomalyHostApiV1* host, std::string_view id, std::uint32_t version) noexcept {
    if (!host || !host->query_service) return nullptr;
    const void* table{};
    const auto result = host->query_service(host->host_context, anomaly::sdk::StringView(id), version, &table);
    return result.code == ANOMALY_STATUS_V1_OK && table ? static_cast<const Service*>(table) : nullptr;
}
void SetStatus(std::string text) { std::scoped_lock lock(g_context.mutex); g_context.status = std::move(text); }
void DrawText(std::string_view text) {
    if (g_context.ui && g_context.ui->text) g_context.ui->text(g_context.ui->user, anomaly::sdk::StringView(text));
}

void Draw() {
    const auto* ui = g_context.ui;
    if (!ui || !ui->begin_window || !ui->end_window || !ui->text || !ui->button || !ui->slider_float) return;
    int open = 1;
    if (!ui->begin_window(ui->user, anomaly::sdk::StringView("NTE Vehicle"), &open, 0)) return;

    Snapshot snap;
    std::string status;
    std::string selected;
    std::string last;
    float index{};
    float ratio{};
    bool friction{};
    std::size_t count{};
    {
        std::scoped_lock lock(g_context.mutex);
        snap = g_context.snapshot;
        status = g_context.status;
        last = g_context.last_summon;
        index = g_context.selected_index;
        ratio = g_context.speed_ratio;
        friction = g_context.friction_enabled;
        count = g_context.vehicles.size();
        if (!g_context.vehicles.empty()) {
            const auto i = (std::min)(static_cast<std::size_t>((std::max)(0.0F, index)), g_context.vehicles.size() - 1U);
            selected = g_context.vehicles[i];
        }
    }

    DrawText("NTE 载具控制");
    DrawText(status);
    const std::string list_text = "已读取 Vehicle 项：" + std::to_string(count);
    DrawText(list_text);

    if (count != 0) {
        const float max_index = static_cast<float>(count - 1U);
        if (ui->slider_float(ui->user, anomaly::sdk::StringView("车辆索引"), &index, 0.0F, max_index)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.selected_index = (std::clamp)(index, 0.0F, max_index);
        }
        if (!selected.empty()) DrawText("当前选择：" + selected);
        if (ui->button(ui->user, anomaly::sdk::StringView("召唤当前车辆"), 0.0F, 0.0F)) {
            g_context.summon.store(true, std::memory_order_release);
        }
    } else {
        DrawText("尚未发现名称包含 Vehicle 的车辆对象。");
    }

    if (!last.empty()) DrawText("最近生成：" + last);

    if ((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0) {
        DrawText("当前驾驶速度：" + std::to_string(snap.speed_kmh) + " km/h");
        DrawText("最高速度倍率：" + std::to_string(snap.top_speed_ratio) + "x");
        if (ui->slider_float(ui->user, anomaly::sdk::StringView("最高速度倍率"), &ratio, 0.05F, 20.0F)) {
            std::scoped_lock lock(g_context.mutex); g_context.speed_ratio = ratio;
        }
        if (ui->button(ui->user, anomaly::sdk::StringView("应用速度倍率"), 0.0F, 0.0F))
            g_context.apply_speed.store(true, std::memory_order_release);
        if (ui->button(ui->user, anomaly::sdk::StringView("恢复 1.0x"), 0.0F, 0.0F))
            g_context.reset.store(true, std::memory_order_release);
        DrawText(std::string("车轮摩擦：") + (friction ? "开启" : "关闭"));
        if (ui->button(ui->user, anomaly::sdk::StringView("切换车轮摩擦"), 0.0F, 0.0F))
            g_context.friction_toggle.store(true, std::memory_order_release);
    }

    ui->end_window(ui->user);
}

void UpdateCatalog() {
    const auto* v = g_context.vehicle;
    if (!v || v->struct_size < offsetof(AnomalyNteVehicleServiceV1, catalog_generation) + sizeof(v->catalog_generation) ||
        !v->catalog_generation || !v->catalog_count || !v->catalog_name_utf8) return;
    const auto generation = v->catalog_generation(v->user);
    const auto count = v->catalog_count(v->user);
    std::vector<std::string> next;
    next.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        char name[512]{};
        std::size_t size = sizeof(name);
        const auto st = v->catalog_name_utf8(v->user, generation, i, name, &size);
        if (st.code == ANOMALY_STATUS_V1_OK && name[0] != '\0') next.emplace_back(name);
    }
    std::scoped_lock lock(g_context.mutex);
    if (generation != g_context.catalog_generation || next != g_context.vehicles) {
        g_context.catalog_generation = generation;
        g_context.vehicles = std::move(next);
        if (g_context.selected_index >= static_cast<float>(g_context.vehicles.size()))
            g_context.selected_index = g_context.vehicles.empty() ? 0.0F : static_cast<float>(g_context.vehicles.size() - 1U);
    }
}

void Update() {
    if (!g_context.vehicle || !g_context.started.load(std::memory_order_acquire)) return;
    UpdateCatalog();

    if (g_context.summon.exchange(false, std::memory_order_acq_rel)) {
        std::string selected;
        {
            std::scoped_lock lock(g_context.mutex);
            if (!g_context.vehicles.empty()) {
                const auto i = (std::min)(static_cast<std::size_t>((std::max)(0.0F, g_context.selected_index)), g_context.vehicles.size() - 1U);
                selected = g_context.vehicles[i];
            }
        }
        if (selected.empty()) {
            SetStatus("没有可召唤的 Vehicle");
        } else if (g_context.vehicle->summon_selected) {
            const auto st = g_context.vehicle->summon_selected(g_context.vehicle->user, anomaly::sdk::StringView(selected));
            SetStatus(st.code == ANOMALY_STATUS_V1_OK ? "车辆已按 Tokky 原生生成链创建" : std::string("车辆生成失败：") + (st.message.data ? std::string(st.message.data, st.message.size) : ""));
        } else {
            SetStatus("当前 Host 没有新的车辆生成 ABI");
        }
    }

    if (g_context.vehicle->reset && g_context.reset.exchange(false, std::memory_order_acq_rel))
        SetStatus(g_context.vehicle->reset(g_context.vehicle->user).code == ANOMALY_STATUS_V1_OK ? "已恢复默认载具设置" : "恢复失败");

    if (g_context.apply_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio; { std::scoped_lock lock(g_context.mutex); ratio = g_context.speed_ratio; }
        const auto st = g_context.vehicle->set_top_speed_ratio(g_context.vehicle->user, ratio);
        SetStatus(st.code == ANOMALY_STATUS_V1_OK ? "速度倍率已应用" : "速度倍率应用失败");
    }
    if (g_context.friction_toggle.exchange(false, std::memory_order_acq_rel)) {
        bool enabled; { std::scoped_lock lock(g_context.mutex); enabled = !g_context.friction_enabled; }
        const auto st = g_context.vehicle->set_wheel_friction_enabled(g_context.vehicle->user, enabled ? 1u : 0u);
        if (st.code == ANOMALY_STATUS_V1_OK) { std::scoped_lock lock(g_context.mutex); g_context.friction_enabled = enabled; }
        SetStatus(st.code == ANOMALY_STATUS_V1_OK ? (enabled ? "车轮摩擦已开启" : "车轮摩擦已关闭") : "车轮摩擦切换失败");
    }

    AnomalyNteVehicleSnapshotV1 snapshot{sizeof(snapshot)};
    const auto st = g_context.vehicle->snapshot(g_context.vehicle->user, &snapshot);
    std::scoped_lock lock(g_context.mutex);
    if (st.code == ANOMALY_STATUS_V1_OK) {
        g_context.snapshot.flags = snapshot.flags;
        g_context.snapshot.speed_kmh = snapshot.speed_kmh;
        g_context.snapshot.top_speed_ratio = snapshot.top_speed_ratio;
        g_context.snapshot.friction = snapshot.wheel_friction_enabled != 0;
    } else g_context.snapshot.flags = 0;

    if (g_context.vehicle->struct_size >= offsetof(AnomalyNteVehicleServiceV1, last_summon_utf8) + sizeof(g_context.vehicle->last_summon_utf8) && g_context.vehicle->last_summon_utf8) {
        char name[512]{}; std::size_t size = sizeof(name);
        if (g_context.vehicle->last_summon_utf8(g_context.vehicle->user, name, &size).code == ANOMALY_STATUS_V1_OK && name[0]) g_context.last_summon = name;
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!host || !plugin_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto* vehicle = QueryService<AnomalyNteVehicleServiceV1>(host, ANOMALY_NTE_VEHICLE_SERVICE_V1_ID, ANOMALY_NTE_VEHICLE_SERVICE_V1_VERSION);
    const auto* ui = QueryService<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!vehicle || !ui || !vehicle->snapshot || !vehicle->set_top_speed_ratio || !vehicle->summon_vehicle ||
        !vehicle->set_wheel_friction_enabled || !vehicle->reset) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    if (!ui->begin_window || !ui->end_window || !ui->text || !ui->button || !ui->slider_float) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    if (vehicle->struct_size < offsetof(AnomalyNteVehicleServiceV1, summon_selected) + sizeof(vehicle->summon_selected) || !vehicle->summon_selected)
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    { std::scoped_lock lock(g_context.mutex); g_context.vehicle = vehicle; g_context.ui = ui; g_context.vehicles.clear(); g_context.catalog_generation = 0; g_context.selected_index = 0.0F; g_context.status.clear(); g_context.last_summon.clear(); g_context.speed_ratio = 1.0F; g_context.friction_enabled = true; }
    g_context.apply_speed.store(false, std::memory_order_release); g_context.reset.store(false, std::memory_order_release); g_context.summon.store(false, std::memory_order_release); g_context.friction_toggle.store(false, std::memory_order_release); g_context.started.store(false, std::memory_order_release);
    g_context.speed_ratio = 1.0F; g_context.friction_enabled = true;
    g_context.status = "正在读取 NTE Vehicle 列表";
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(true, std::memory_order_release); return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(false, std::memory_order_release); return anomaly::sdk::Ok();
}
void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release); g_context.vehicle = nullptr; g_context.ui = nullptr;
}
void ANOMALY_CALL UpdateCallback(void* plugin_context, double) { if (plugin_context == &g_context) Update(); }
void ANOMALY_CALL DrawCallback(void* plugin_context, const AnomalyUiServiceV1*) { if (plugin_context == &g_context) Draw(); }
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-vehicle"),
        anomaly::sdk::StringView("NTE Vehicle"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("0.7.0"),
        Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}