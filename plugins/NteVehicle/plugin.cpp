#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

#include <algorithm>
#include <array>
#include <cmath>
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
    float engine_torque_ratio{1.0F};
    bool friction{true};
};

struct Context {
    const AnomalyNteVehicleServiceV1* vehicle{};
    const AnomalyNtePlayerServiceV1* player{};
    AnomalyNtePlayerSnapshotV1 player_snapshot{};
    bool player_position_valid{};
    std::mutex mutex;
    Snapshot snapshot{};
    std::string status{"正在读取游戏进程中的 Vehicle 载具数据"};
    std::string selected_vehicle_id{"Vehicle007"};
    std::string pending_vehicle_id;
    std::vector<std::string> vehicle_ids;
    std::uint64_t catalog_tick{};
    std::atomic_bool apply_speed{};
    std::atomic_bool apply_torque{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool friction_toggle{};
    std::atomic_bool started{};
    float speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    bool friction_enabled{true};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

constexpr AnomalyStatusV1 Status(std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
const Service* QueryService(const AnomalyHostApiV1* host,
                            std::string_view id,
                            std::uint32_t version) noexcept {
    if (!host || !host->query_service) return nullptr;
    const void* table{};
    const auto result = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), version, &table);
    if (result.code != ANOMALY_STATUS_V1_OK || !table) return nullptr;
    return static_cast<const Service*>(table);
}

void SetStatus(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.status = std::move(text);
}

void DrawText(const AnomalyUiServiceV1* ui, std::string_view text) {
    if (ui != nullptr && ui->text != nullptr) {
        ui->text(ui->user, anomaly::sdk::StringView(text));
    }
}

// 目录内容来自当前 Host 已核验的实时 FNamePool；读取前后序号相同才发布完整目录。
void RefreshCatalogOnGameThread() {
    const auto* vehicle = g_context.vehicle;
    if (!vehicle || !vehicle->catalog_snapshot || !vehicle->vehicle_id_at) return;

    AnomalyNteVehicleCatalogSnapshotV1 before{};
    before.struct_size = sizeof(before);
    const auto before_status = vehicle->catalog_snapshot(vehicle->user, &before);
    if (before_status.code != ANOMALY_STATUS_V1_OK ||
        (before.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        before.entry_count > 8192U) {
        SetStatus("Host 尚未提供有效的载具目录；召唤保持禁用");
        return;
    }

    std::vector<std::string> ids;
    ids.reserve(before.entry_count);
    for (std::uint32_t i = 0; i < before.entry_count; ++i) {
        std::array<char, ANOMALY_NTE_VEHICLE_ID_MAX_UTF8_BYTES + 1U> buffer{};
        std::size_t size = buffer.size();
        const auto status = vehicle->vehicle_id_at(vehicle->user, i, buffer.data(), &size);
        if (status.code != ANOMALY_STATUS_V1_OK || size <= 1U || size > buffer.size()) {
            SetStatus("载具目录未能完整读取；选择与召唤保持禁用");
            return;
        }
        ids.emplace_back(buffer.data(), size - 1U);
        if (ids.back().empty()) {
            SetStatus("载具目录包含空 VehicleID；选择与召唤保持禁用");
            return;
        }
    }

    AnomalyNteVehicleCatalogSnapshotV1 after{};
    after.struct_size = sizeof(after);
    const auto after_status = vehicle->catalog_snapshot(vehicle->user, &after);
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        after.sequence != before.sequence || after.entry_count != ids.size()) {
        SetStatus("读取期间载具目录已变化；稍后重新读取");
        return;
    }

    std::string select_after_refresh;
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids = std::move(ids);
        g_context.catalog_tick = after.sequence;
        if (std::ranges::find(g_context.vehicle_ids, g_context.selected_vehicle_id) ==
            g_context.vehicle_ids.end()) {
            g_context.selected_vehicle_id = g_context.vehicle_ids.empty()
                ? std::string{} : g_context.vehicle_ids.front();
            select_after_refresh = g_context.selected_vehicle_id;
        }
        g_context.status = g_context.vehicle_ids.empty()
            ? "Host 目录有效，但没有符合 Vehicle 条件的条目"
            : "已完整读取 Host 验证的载具目录";
    }
    if (!select_after_refresh.empty()) {
        const auto selected = vehicle->set_summon_vehicle_id(
            vehicle->user, anomaly::sdk::StringView(select_after_refresh));
        if (selected.code != ANOMALY_STATUS_V1_OK)
            SetStatus("目录已读取，但 Host 未接受默认 VehicleID；请手动选择");
    }
}

void Update() {
    if (!g_context.vehicle || !g_context.started.load(std::memory_order_acquire)) return;

    if (g_context.pending_vehicle_id.size() != 0) {
        std::string id;
        {
            std::scoped_lock lock(g_context.mutex);
            id = g_context.pending_vehicle_id;
            g_context.pending_vehicle_id.clear();
        }
        const auto status = g_context.vehicle->set_summon_vehicle_id(
            g_context.vehicle->user, anomaly::sdk::StringView(id));
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.selected_vehicle_id = id;
            g_context.status = "已选择载具：" + id;
        } else {
            SetStatus("载具选择失败");
        }
    }

    if (g_context.vehicle->catalog_snapshot) {
        AnomalyNteVehicleCatalogSnapshotV1 catalog{};
        catalog.struct_size = sizeof(catalog);
        const auto catalog_status = g_context.vehicle->catalog_snapshot(
            g_context.vehicle->user, &catalog);
        bool need_refresh = catalog_status.code == ANOMALY_STATUS_V1_OK &&
            (catalog.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) != 0;
        {
            std::scoped_lock lock(g_context.mutex);
            need_refresh = need_refresh &&
                (g_context.vehicle_ids.empty() || catalog.sequence != g_context.catalog_tick);
        }
        if (need_refresh) RefreshCatalogOnGameThread();
    }

    // Refresh the live player snapshot every Game update. Summon must not reuse stale coordinates.
    if (g_context.player && g_context.player->snapshot) {
        AnomalyNtePlayerSnapshotV1 player_snapshot{};
        player_snapshot.struct_size = sizeof(player_snapshot);
        const auto player_status = g_context.player->snapshot(
            g_context.player->user, &player_snapshot);
        const bool valid = player_status.code == ANOMALY_STATUS_V1_OK &&
            (player_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
            std::ranges::all_of(player_snapshot.position,
                [](double value) { return std::isfinite(value); });
        std::scoped_lock lock(g_context.mutex);
        g_context.player_position_valid = valid;
        if (valid) g_context.player_snapshot = player_snapshot;
    }

    if (g_context.reset.exchange(false, std::memory_order_acq_rel)) {
        const auto status = g_context.vehicle->reset(g_context.vehicle->user);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK ? "已恢复默认载具设置" : "恢复失败");
    }

    if (g_context.apply_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.speed_ratio;
        }
        const auto status = g_context.vehicle->set_top_speed_ratio(
            g_context.vehicle->user, ratio);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            SetStatus("车速倍率已应用：" + std::to_string(ratio) + "x");
        } else if (status.message.data != nullptr && status.message.size != 0) {
            SetStatus("车速倍率应用失败：" + std::string(status.message.data, status.message.size));
        } else {
            SetStatus("车速倍率应用失败，Host 状态码 " + std::to_string(status.code));
        }
    }

    if (g_context.apply_torque.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.engine_torque_ratio;
        }
        const bool has_torque_ratio_service =
            HasField<AnomalyNteVehicleServiceV1,
                decltype(AnomalyNteVehicleServiceV1::set_engine_torque_ratio)>(
                    g_context.vehicle,
                    offsetof(AnomalyNteVehicleServiceV1, set_engine_torque_ratio)) &&
            g_context.vehicle->set_engine_torque_ratio != nullptr;
        if (!has_torque_ratio_service) {
            SetStatus("发动机扭矩倍率不可用：当前 Host Runtime 未发布该扩展接口");
        } else {
            const auto status = g_context.vehicle->set_engine_torque_ratio(
                g_context.vehicle->user, ratio);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                SetStatus("发动机扭矩倍率已应用：" + std::to_string(ratio) + "x");
            } else if (status.message.data != nullptr && status.message.size != 0) {
                SetStatus("发动机扭矩倍率应用失败：" +
                    std::string(status.message.data, status.message.size));
            } else {
                SetStatus("发动机扭矩倍率应用失败，Host 状态码 " +
                    std::to_string(status.code));
            }
        }
    }

    if (g_context.summon.exchange(false, std::memory_order_acq_rel)) {
        AnomalyNteVehicleSummonRequestV1 request{};
        request.struct_size = sizeof(request);
        request.flags = ANOMALY_NTE_VEHICLE_SUMMON_V1_HAS_POSITION |
            ANOMALY_NTE_VEHICLE_SUMMON_V1_SET_OWNER_TO_PLAYER;
        std::string selected_id;
        bool position_valid{};
        {
            std::scoped_lock lock(g_context.mutex);
            selected_id = g_context.selected_vehicle_id;
            position_valid = g_context.player_position_valid;
            if (position_valid) {
                // The requested offset is signed by axis: X - 2000, Y + 2000, Z + 2000.
                request.world_position[0] = g_context.player_snapshot.position[0] - 2000.0;
                request.world_position[1] = g_context.player_snapshot.position[1] + 2000.0;
                request.world_position[2] = g_context.player_snapshot.position[2] + 2000.0;
            }
        }
        bool catalog_has_selection{};
        {
            std::scoped_lock lock(g_context.mutex);
            catalog_has_selection = !selected_id.empty() &&
                std::ranges::find(g_context.vehicle_ids, selected_id) != g_context.vehicle_ids.end();
        }
        if (!position_valid || !catalog_has_selection) {
            SetStatus("召唤未执行：需要有效玩家坐标和当前目录中的 VehicleID");
        } else {
            const auto status = g_context.vehicle->summon_vehicle(
                g_context.vehicle->user, &request);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                SetStatus("Host 已接受召唤请求；生成与 Actor.Owner 由 Host 后置核验");
            } else if (status.message.data != nullptr && status.message.size != 0) {
                SetStatus("召唤失败：" + std::string(status.message.data, status.message.size));
            } else {
                SetStatus("召唤失败，Host 状态码 " + std::to_string(status.code));
            }
        }
    }

    if (g_context.friction_toggle.exchange(false, std::memory_order_acq_rel)) {
        bool enabled;
        {
            std::scoped_lock lock(g_context.mutex);
            enabled = !g_context.friction_enabled;
        }
        const auto status = g_context.vehicle->set_wheel_friction_enabled(
            g_context.vehicle->user, enabled ? 1u : 0u);
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.friction_enabled = enabled;
            g_context.status = enabled ? "车轮摩擦已开启" : "车轮摩擦已关闭";
        } else {
            SetStatus("车轮摩擦切换失败");
        }
    }

    AnomalyNteVehicleSnapshotV1 snapshot{sizeof(snapshot)};
    const auto status = g_context.vehicle->snapshot(g_context.vehicle->user, &snapshot);
    std::scoped_lock lock(g_context.mutex);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        g_context.snapshot.flags = snapshot.flags;
        g_context.snapshot.speed_kmh = snapshot.speed_kmh;
        g_context.snapshot.top_speed_ratio = snapshot.top_speed_ratio;
        g_context.snapshot.engine_torque_ratio = snapshot.engine_torque_ratio;
        g_context.snapshot.friction = snapshot.wheel_friction_enabled != 0;
        g_context.status = g_context.status;
    } else {
        g_context.snapshot.flags = 0;
    }
}

void Draw(const AnomalyUiServiceV1* ui) {
    if (!ui || !ui->begin_window || !ui->end_window || !ui->text ||
        !ui->button || !ui->slider_float) return;

    int open = 1;
    anomaly::sdk::UiWindow window(ui, "NTE Vehicle", &open, 0);
    if (!window) return;

    Snapshot snap;
    std::string status;
    std::string selected;
    std::vector<std::string> ids;
    float ratio;
    bool friction;
    {
        std::scoped_lock lock(g_context.mutex);
        snap = g_context.snapshot;
        status = g_context.status;
        selected = g_context.selected_vehicle_id;
        ids = g_context.vehicle_ids;
        ratio = g_context.speed_ratio;
        friction = g_context.friction_enabled;
    }

    DrawText(ui, "NTE 载具控制");
    DrawText(status);

    DrawText("召唤载具");
    DrawText("当前选择：" + selected);
    if (ids.empty()) {
        DrawText("正在读取运行时 Vehicle 名称...");
    } else {
        const std::size_t limit = (std::min)(ids.size(), std::size_t{96});
        for (std::size_t i = 0; i < limit; ++i) {
            const bool current = ids[i] == selected;
            const std::string label = std::string(current ? "[当前] " : "") + ids[i];
            if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.pending_vehicle_id = ids[i];
            }
        }
        if (ids.size() > limit) {
            DrawText("列表显示前 96 项；Host 目录仍保留全部运行时匹配项。");
        }
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("召唤当前载具"), 0.0F, 0.0F)) {
        g_context.summon.store(true, std::memory_order_release);
    }

    if ((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) == 0) {
        DrawText("当前没有检测到正在驾驶的载具；可先调整倍率，应用时需要有效驾驶载具。");
    } else {
        DrawText("当前速度：" + std::to_string(snap.speed_kmh) + " km/h");
        DrawText("当前倍率：" + std::to_string(snap.top_speed_ratio) + "x");
    }
    // Keep the control visible even when the current vehicle snapshot is temporarily
    // invalid; hiding it made the slider impossible to drag before a valid driving sample.
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("速度倍率"),
                         &ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.speed_ratio = ratio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用车速倍率"), 0.0F, 0.0F))
        g_context.apply_speed.store(true, std::memory_order_release);
    float torque_ratio;
    {
        std::scoped_lock lock(g_context.mutex);
        torque_ratio = g_context.engine_torque_ratio;
    }
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("发动机扭矩倍率"),
                         &torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.engine_torque_ratio = torque_ratio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用扭矩倍率"), 0.0F, 0.0F))
        g_context.apply_torque.store(true, std::memory_order_release);
    if (ui->button(ui->user, anomaly::sdk::StringView("恢复 1.0x"), 0.0F, 0.0F))
        g_context.reset.store(true, std::memory_order_release);
    DrawText(std::string("车轮摩擦：") + (friction ? "开启" : "关闭"));
    if (ui->button(ui->user, anomaly::sdk::StringView("切换车轮摩擦"), 0.0F, 0.0F))
        g_context.friction_toggle.store(true, std::memory_order_release);

}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!host || !plugin_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto* vehicle = QueryService<AnomalyNteVehicleServiceV1>(
        host, ANOMALY_NTE_VEHICLE_SERVICE_V1_ID, ANOMALY_NTE_VEHICLE_SERVICE_V1_VERSION);
    const auto* player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!vehicle || !player) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);

    if (!vehicle->snapshot || !vehicle->set_top_speed_ratio ||
        !vehicle->summon_vehicle || !vehicle->set_wheel_friction_enabled ||
        !vehicle->reset || !vehicle->catalog_snapshot || !vehicle->vehicle_id_at ||
        !vehicle->set_summon_vehicle_id || !player->snapshot) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    g_context.vehicle = vehicle;
    g_context.player = player;
    g_context.started.store(false, std::memory_order_release);
    g_context.apply_speed.store(false, std::memory_order_release);
    g_context.apply_torque.store(false, std::memory_order_release);
    g_context.reset.store(false, std::memory_order_release);
    g_context.summon.store(false, std::memory_order_release);
    g_context.friction_toggle.store(false, std::memory_order_release);
    g_context.speed_ratio = 1.0F;
    g_context.engine_torque_ratio = 1.0F;
    g_context.friction_enabled = true;
    g_context.snapshot = {};
    g_context.selected_vehicle_id = "Vehicle007";
    g_context.pending_vehicle_id.clear();
    g_context.vehicle_ids.clear();
    g_context.status = "正在读取 Host 验证的运行时 Vehicle 目录";
    g_context.player_position_valid = false;
    g_context.player_snapshot = {};
    g_context.catalog_tick = 0;
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
    g_context.player = nullptr;
}

void ANOMALY_CALL UpdateCallback(void* plugin_context, double) {
    if (plugin_context == &g_context) Update();
}

void ANOMALY_CALL DrawCallback(void* plugin_context, const AnomalyUiServiceV1* ui) {
    if (plugin_context == &g_context) Draw(ui);
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-vehicle"),
        anomaly::sdk::StringView("NTE Vehicle"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.8.1"), Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}
