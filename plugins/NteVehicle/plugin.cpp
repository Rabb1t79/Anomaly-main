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
#include <cctype>
#include <string_view>
#include <vector>

namespace {

struct Snapshot {
    std::uint32_t flags{};
    double speed_kmh{};
    float top_speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    bool friction{true};
    std::uint64_t vehicle_entity_id{};
    std::string vehicle_class_name;
    std::string vehicle_snapshot_status{"Host 尚未返回当前驾驶载具状态"};
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
    std::string catalog_filter;
    std::string last_summon{};
    std::vector<std::string> vehicle_ids;
    std::vector<std::string> vehicle_labels;
    std::uint64_t catalog_tick{};
    std::atomic_bool refresh_catalog_requested{};
    std::atomic_bool apply_speed{};
    std::atomic_bool apply_torque{};
    std::atomic_bool apply_other_speed{};
    std::atomic_bool apply_other_torque{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool friction_toggle{};
    std::atomic_bool started{};
    float speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    float other_speed_ratio{1.0F};
    float other_speed_floor_ratio{0.6024F};
    float other_torque_ratio{1.0F};
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
    if (ui == nullptr) return;
    const auto wrapped_offset = offsetof(AnomalyUiServiceV1, text_wrapped);
    if (ui->struct_size >= wrapped_offset +
            sizeof(decltype(AnomalyUiServiceV1::text_wrapped)) &&
        ui->text_wrapped != nullptr) {
        ui->text_wrapped(ui->user, anomaly::sdk::StringView(text));
    } else if (ui->text != nullptr) {
        ui->text(ui->user, anomaly::sdk::StringView(text));
    }
}

std::string StatusDescription(std::string_view prefix, const AnomalyStatusV1& status) {
    std::string result(prefix);
    if (status.message.data != nullptr && status.message.size != 0) {
        result += "：";
        result.append(status.message.data, status.message.size);
    } else {
        result += "（Host 状态码 ";
        result += std::to_string(status.code);
        result += "）";
    }
    return result;
}

void SetLastSummon(std::string text) {
    std::scoped_lock lock(g_context.mutex);
    g_context.last_summon = std::move(text);
}

void QueueSelectionOffset(const std::vector<std::string>& ids,
                          const std::string& selected, int offset) {
    if (ids.empty()) return;
    auto found = std::ranges::find(ids, selected);
    std::size_t index = found == ids.end() ? 0U : static_cast<std::size_t>(found - ids.begin());
    const auto count = static_cast<long long>(ids.size());
    auto next = (static_cast<long long>(index) + offset) % count;
    if (next < 0) next += count;
    std::scoped_lock lock(g_context.mutex);
    g_context.pending_vehicle_id = ids[static_cast<std::size_t>(next)];
}

// 目录内容来自当前 Host 已核验的实时 FNamePool；读取前后序号相同才发布完整目录。
void RefreshCatalogOnGameThread() {
    const auto* vehicle = g_context.vehicle;
    if (!vehicle || !vehicle->catalog_snapshot || !vehicle->vehicle_id_at ||
        !vehicle->catalog_label_at) {
        SetStatus("目录读取失败：当前 Host 没有发布 Vehicle V3 的目录标签接口");
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids.clear();
        g_context.vehicle_labels.clear();
        return;
    }

    AnomalyNteVehicleCatalogSnapshotV1 before{};
    before.struct_size = sizeof(before);
    const auto before_status = vehicle->catalog_snapshot(vehicle->user, &before);
    if (before_status.code != ANOMALY_STATUS_V1_OK ||
        (before.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        before.entry_count > 8192U) {
        SetStatus(StatusDescription("目录读取失败", before_status));
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids.clear();
        g_context.vehicle_labels.clear();
        return;
    }

    std::vector<std::string> ids;
    std::vector<std::string> labels;
    ids.reserve(before.entry_count);
    labels.reserve(before.entry_count);
    for (std::uint32_t i = 0; i < before.entry_count; ++i) {
        std::array<char, ANOMALY_NTE_VEHICLE_ID_MAX_UTF8_BYTES + 1U> id_buffer{};
        std::array<char, ANOMALY_NTE_VEHICLE_ID_MAX_UTF8_BYTES * 2U + 8U> label_buffer{};
        std::size_t id_size = id_buffer.size();
        const auto id_status = vehicle->vehicle_id_at(vehicle->user, i, id_buffer.data(), &id_size);
        if (id_status.code != ANOMALY_STATUS_V1_OK || id_size <= 1U || id_size > id_buffer.size()) {
            SetStatus(StatusDescription("读取 RowName 失败（索引 " + std::to_string(i) + "）", id_status));
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }

        std::size_t label_size = label_buffer.size();
        const auto label_status = vehicle->catalog_label_at(
            vehicle->user, i, label_buffer.data(), &label_size);
        if (label_status.code != ANOMALY_STATUS_V1_OK ||
            label_size <= 1U || label_size > label_buffer.size()) {
            SetStatus(StatusDescription("读取载具显示名称失败（索引 " + std::to_string(i) + "）", label_status));
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }
        ids.emplace_back(id_buffer.data(), id_size - 1U);
        labels.emplace_back(label_buffer.data(), label_size - 1U);
        if (ids.back().empty() || labels.back().empty()) {
            SetStatus("目录读取失败：存在空 RowName 或显示标签");
            std::scoped_lock lock(g_context.mutex);
            g_context.vehicle_ids.clear();
            g_context.vehicle_labels.clear();
            return;
        }
    }

    AnomalyNteVehicleCatalogSnapshotV1 after{};
    after.struct_size = sizeof(after);
    const auto after_status = vehicle->catalog_snapshot(vehicle->user, &after);
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        after.sequence != before.sequence || after.entry_count != ids.size()) {
        SetStatus(StatusDescription("读取目录期间 DataTable 发生变化", after_status));
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids.clear();
        g_context.vehicle_labels.clear();
        return;
    }

    std::string selected_id;
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.vehicle_ids = std::move(ids);
        g_context.vehicle_labels = std::move(labels);
        g_context.catalog_tick = after.sequence;
        if (std::ranges::find(g_context.vehicle_ids, g_context.selected_vehicle_id) ==
            g_context.vehicle_ids.end()) {
            g_context.selected_vehicle_id = g_context.vehicle_ids.empty()
                ? std::string{} : g_context.vehicle_ids.front();
        }
        selected_id = g_context.selected_vehicle_id;
        g_context.status = "已读取 " + std::to_string(g_context.vehicle_ids.size()) +
            " 条 DT_VehicleData 记录";
    }

    // Always synchronize the current UI selection with Host state. Otherwise the
    // default item can look selected while selected_summon_vehicle_id is still empty.
    if (!selected_id.empty()) {
        const auto selected_status = vehicle->set_summon_vehicle_id(
            vehicle->user, anomaly::sdk::StringView(selected_id));
        if (selected_status.code != ANOMALY_STATUS_V1_OK)
            SetStatus(StatusDescription("目录已读取，但 Host 未接受当前 RowName", selected_status));
    }
}

void Update() {
    if (!g_context.vehicle || !g_context.started.load(std::memory_order_acquire)) return;

    // Explicit refresh runs on Game thread; a failed scan waits for another user request
    // instead of rescanning UObject/DataTable every frame.
    if (g_context.refresh_catalog_requested.exchange(false, std::memory_order_acq_rel)) {
        if (!g_context.vehicle->refresh_catalog) {
            SetStatus("目录刷新失败：Host 没有发布 Vehicle V3 refresh_catalog 接口");
        } else {
            const auto refresh_status = g_context.vehicle->refresh_catalog(g_context.vehicle->user);
            if (refresh_status.code == ANOMALY_STATUS_V1_OK) {
                RefreshCatalogOnGameThread();
            } else {
                {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.vehicle_ids.clear();
                    g_context.vehicle_labels.clear();
                }
                SetStatus(StatusDescription("目录刷新失败", refresh_status));
            }
        }
    }

    // Render only writes pending_vehicle_id under mutex; consume it under the same mutex.
    std::string pending_vehicle_id;
    {
        std::scoped_lock lock(g_context.mutex);
        pending_vehicle_id = std::move(g_context.pending_vehicle_id);
        g_context.pending_vehicle_id.clear();
    }
    if (!pending_vehicle_id.empty()) {
        const auto status = g_context.vehicle->set_summon_vehicle_id(
            g_context.vehicle->user, anomaly::sdk::StringView(pending_vehicle_id));
        if (status.code == ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(g_context.mutex);
            g_context.selected_vehicle_id = pending_vehicle_id;
            g_context.status = "已选择载具：" + pending_vehicle_id;
        } else {
            SetStatus(StatusDescription("载具选择失败", status));
        }
    }

    bool catalog_loaded{};
    {
        std::scoped_lock lock(g_context.mutex);
        catalog_loaded = !g_context.vehicle_ids.empty();
    }
    if (catalog_loaded && g_context.vehicle->catalog_snapshot) {
        AnomalyNteVehicleCatalogSnapshotV1 catalog{};
        catalog.struct_size = sizeof(catalog);
        const auto catalog_status = g_context.vehicle->catalog_snapshot(
            g_context.vehicle->user, &catalog);
        if (catalog_status.code == ANOMALY_STATUS_V1_OK &&
            (catalog.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) != 0) {
            bool need_refresh{};
            {
                std::scoped_lock lock(g_context.mutex);
                need_refresh = catalog.sequence != g_context.catalog_tick;
            }
            if (need_refresh) RefreshCatalogOnGameThread();
        } else {
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.vehicle_ids.clear();
                g_context.vehicle_labels.clear();
            }
            SetStatus(StatusDescription("载具目录失效", catalog_status) +
                "；请点击「重新整理」重试");
        }
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
            SetStatus("当前车速与最高车速上限均已应用：" + std::to_string(ratio) + "x");
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

    if (g_context.apply_other_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        float floor_ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.other_speed_ratio;
            floor_ratio = g_context.other_speed_floor_ratio;
        }
        const bool floor_available =
            HasField<AnomalyNteVehicleServiceV1,
                decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_min_speed_ratio)>(
                    g_context.vehicle,
                    offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_min_speed_ratio)) &&
            g_context.vehicle->set_other_vehicles_min_speed_ratio != nullptr;
        if (!floor_available) {
            SetStatus("非玩家车辆最低车速倍率不可用：Host Runtime 未发布扩展接口");
        } else {
            const auto floor_status = g_context.vehicle->set_other_vehicles_min_speed_ratio(
                g_context.vehicle->user, floor_ratio);
            if (floor_status.code != ANOMALY_STATUS_V1_OK) {
                SetStatus(StatusDescription("非玩家车辆最低车速倍率设置失败", floor_status));
            } else {
                const bool available =
                    HasField<AnomalyNteVehicleServiceV1,
                        decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_top_speed_ratio)>(
                            g_context.vehicle,
                            offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_top_speed_ratio)) &&
                    g_context.vehicle->set_other_vehicles_top_speed_ratio != nullptr;
                if (!available) {
                    SetStatus("非玩家车辆速率倍率不可用：Host Runtime 未发布 Vehicle V4 接口");
                } else {
                    const auto status = g_context.vehicle->set_other_vehicles_top_speed_ratio(
                        g_context.vehicle->user, ratio);
                    SetStatus(status.code == ANOMALY_STATUS_V1_OK
                        ? StatusDescription("非玩家车辆速率与下限修改完成", status)
                        : StatusDescription("非玩家车辆速率修改未完全成功", status));
                }
            }
        }
    }

    if (g_context.apply_other_torque.exchange(false, std::memory_order_acq_rel)) {
        float ratio;
        {
            std::scoped_lock lock(g_context.mutex);
            ratio = g_context.other_torque_ratio;
        }
        const bool available =
            HasField<AnomalyNteVehicleServiceV1,
                decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_engine_torque_ratio)>(
                    g_context.vehicle,
                    offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_engine_torque_ratio)) &&
            g_context.vehicle->set_other_vehicles_engine_torque_ratio != nullptr;
        if (!available) {
            SetStatus("非玩家车辆扭矩倍率不可用：Host Runtime 未发布 Vehicle V4 接口");
        } else {
            const auto status = g_context.vehicle->set_other_vehicles_engine_torque_ratio(
                g_context.vehicle->user, ratio);
            SetStatus(status.code == ANOMALY_STATUS_V1_OK
                ? StatusDescription("非玩家车辆扭矩修改完成", status)
                : StatusDescription("非玩家车辆扭矩修改未完全成功", status));
        }
    }

    if (g_context.summon.exchange(false, std::memory_order_acq_rel)) {
        AnomalyNteVehicleSummonRequestV1 request{};
        request.struct_size = sizeof(request);
        request.flags = ANOMALY_NTE_VEHICLE_SUMMON_V1_HAS_POSITION |
            ANOMALY_NTE_VEHICLE_SUMMON_V1_SET_OWNER_TO_PLAYER;
        std::string selected_id;
        std::string selected_label;
        bool position_valid{};
        {
            std::scoped_lock lock(g_context.mutex);
            selected_id = g_context.selected_vehicle_id;
            position_valid = g_context.player_position_valid;
            const auto selected_index = std::ranges::find(g_context.vehicle_ids, selected_id);
            if (selected_index != g_context.vehicle_ids.end()) {
                const auto index = static_cast<std::size_t>(selected_index - g_context.vehicle_ids.begin());
                selected_label = index < g_context.vehicle_labels.size()
                    ? g_context.vehicle_labels[index] : selected_id;
            }
            if (position_valid) {
                // The requested offset stays close to the player: X - 500, Y + 500, Z + 500.
                request.world_position[0] = g_context.player_snapshot.position[0] - 500.0;
                request.world_position[1] = g_context.player_snapshot.position[1] + 500.0;
                request.world_position[2] = g_context.player_snapshot.position[2] + 500.0;
            }
        }
        bool catalog_has_selection{};
        {
            std::scoped_lock lock(g_context.mutex);
            catalog_has_selection = !selected_id.empty() &&
                std::ranges::find(g_context.vehicle_ids, selected_id) != g_context.vehicle_ids.end();
        }
        if (!position_valid || !catalog_has_selection) {
            const std::string reason = !position_valid
                ? "召唤失败：玩家坐标当前无效"
                : "召唤失败：当前选择未同步到 Host 载具目录";
            SetStatus(reason);
            SetLastSummon(reason);
        } else {
            const auto status = g_context.vehicle->summon_vehicle(
                g_context.vehicle->user, &request);
            if (status.code == ANOMALY_STATUS_V1_OK) {
                const std::string label = selected_label.empty() ? selected_id : selected_label;
                const std::string detail = status.message.data != nullptr && status.message.size != 0
                    ? std::string(status.message.data, status.message.size)
                    : std::string{};
                constexpr std::string_view verified_prefix = "召唤已实际生成并通过后置验证：";
                const bool postcondition_verified =
                    !detail.empty() && detail.find(verified_prefix) == 0;
                std::string result;
                if (postcondition_verified) {
                    // Successful summons stay silent; only failure or unverified states show a message.
                    SetStatus({});
                    SetLastSummon({});
                } else {
                    // An OK status without the Core's explicit postcondition marker is not
                    // proof that a new vehicle exists; keep this diagnostic visible.
                    result = "召唤状态未验证：" + label + " (" + selected_id +
                        ")；Host 返回 OK，但没有明确的实际生成/后置验证标记";
                    if (!detail.empty()) result += "；Host 信息：" + detail;
                    SetStatus(result);
                    SetLastSummon(result);
                }
            } else {
                const auto reason = StatusDescription("召唤失败", status);
                SetStatus(reason);
                SetLastSummon(reason);
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
        if (snapshot.struct_size >= offsetof(AnomalyNteVehicleSnapshotV1, vehicle_entity_id) +
                sizeof(snapshot.vehicle_entity_id))
            g_context.snapshot.vehicle_entity_id = snapshot.vehicle_entity_id;
        else
            g_context.snapshot.vehicle_entity_id = 0;
        if (snapshot.struct_size >= offsetof(AnomalyNteVehicleSnapshotV1, vehicle_class_name) +
                sizeof(snapshot.vehicle_class_name)) {
            g_context.snapshot.vehicle_class_name.assign(
                snapshot.vehicle_class_name,
                std::find(snapshot.vehicle_class_name,
                    snapshot.vehicle_class_name + sizeof(snapshot.vehicle_class_name), '\0'));
        } else {
            g_context.snapshot.vehicle_class_name.clear();
        }
        g_context.snapshot.vehicle_snapshot_status.clear();
    } else {
        g_context.snapshot.flags = 0;
        g_context.snapshot.vehicle_entity_id = 0;
        g_context.snapshot.vehicle_class_name.clear();
        g_context.snapshot.vehicle_snapshot_status =
            status.message.data != nullptr && status.message.size != 0
                ? std::string(status.message.data, status.message.size)
                : "Host 载具快照状态码 " + std::to_string(status.code);
    }
}

bool AsciiContainsInsensitive(std::string_view text, std::string_view filter) noexcept {
    if (filter.empty()) return true;
    if (filter.size() > text.size()) return false;
    const auto equal_at = [&](std::size_t offset) {
        for (std::size_t i = 0; i < filter.size(); ++i) {
            const auto left = static_cast<unsigned char>(text[offset + i]);
            const auto right = static_cast<unsigned char>(filter[i]);
            if (std::tolower(left) != std::tolower(right)) return false;
        }
        return true;
    };
    for (std::size_t i = 0; i + filter.size() <= text.size(); ++i) {
        if (equal_at(i)) return true;
    }
    return false;
}

void DrawDivider(const AnomalyUiServiceV1* ui) {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::separator)>(
            ui, offsetof(AnomalyUiServiceV1, separator)) && ui->separator) {
        ui->separator(ui->user);
    }
}

void DrawInlineGap(const AnomalyUiServiceV1* ui) {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::same_line)>(
            ui, offsetof(AnomalyUiServiceV1, same_line)) && ui->same_line) {
        ui->same_line(ui->user, 0.0F, 12.0F);
    }
}

bool DrawButtonEnabled(
    const AnomalyUiServiceV1* ui, std::string_view label, bool enabled) {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, button_enabled)) && ui->button_enabled) {
        return ui->button_enabled(ui->user, anomaly::sdk::StringView(label),
            0.0F, 0.0F, enabled ? 1 : 0) != 0;
    }
    return enabled && ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0;
}

void DrawSectionTitle(const AnomalyUiServiceV1* ui, std::string_view eyebrow, std::string_view title) {
    DrawText(ui, eyebrow);
    DrawText(ui, title);
    DrawDivider(ui);
}

bool UiFieldAvailable(const AnomalyUiServiceV1* ui, std::size_t offset, std::size_t field_size) {
    return ui != nullptr && ui->struct_size >= offset + field_size;
}

void DrawSummonTab(
    const AnomalyUiServiceV1* ui,
    const std::string& selected,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& labels,
    const std::string& last_summon,
    bool player_position_valid) {
    DrawSectionTitle(ui, "01  /  VEHICLE DEPLOYMENT", "载具召唤");
    const auto label_at = [&labels, &ids](std::size_t i) -> std::string_view {
        return i < labels.size() && !labels[i].empty()
            ? std::string_view(labels[i]) : std::string_view(ids[i]);
    };
    const auto selected_index = std::ranges::find(ids, selected);
    const bool has_selected = selected_index != ids.end();
    if (!last_summon.empty()) DrawText(ui, "上次召唤：" + last_summon);

    std::string current_filter;
    {
        std::scoped_lock lock(g_context.mutex);
        current_filter = g_context.catalog_filter;
    }
    std::array<char, 128> filter_buffer{};
    const std::size_t filter_length = (std::min)(current_filter.size(), filter_buffer.size() - 1);
    std::copy_n(current_filter.data(), filter_length, filter_buffer.data());
    const bool has_input_text =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, input_text),
            sizeof(decltype(AnomalyUiServiceV1::input_text))) && ui->input_text;
    if (has_input_text) {
        DrawText(ui, "筛选");
        if (ui->input_text(ui->user, anomaly::sdk::StringView("搜索名称或 VehicleID"),
                filter_buffer.data(), filter_buffer.size(), 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.catalog_filter.assign(filter_buffer.data());
        }
        current_filter.assign(filter_buffer.data());
    }

    std::vector<std::size_t> filtered;
    filtered.reserve(ids.size());
    const bool can_host_filter =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, filter_match),
            sizeof(decltype(AnomalyUiServiceV1::filter_match))) && ui->filter_match;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto label = label_at(i);
        bool matches = current_filter.empty();
        if (!matches && can_host_filter) {
            matches = ui->filter_match(ui->user, anomaly::sdk::StringView(current_filter),
                anomaly::sdk::StringView(label)) != 0;
            if (!matches) {
                matches = ui->filter_match(ui->user, anomaly::sdk::StringView(current_filter),
                    anomaly::sdk::StringView(ids[i])) != 0;
            }
        } else if (!matches) {
            matches = AsciiContainsInsensitive(label, current_filter) ||
                AsciiContainsInsensitive(ids[i], current_filter);
        }
        if (matches) filtered.push_back(i);
    }

    // Keep the catalog compact: show only five choices per page while preserving filtering.
    constexpr std::size_t kVehiclesPerPage = 5U;
    static std::size_t current_page = 0U;
    const std::size_t page_count = filtered.empty()
        ? 1U : (filtered.size() + kVehiclesPerPage - 1U) / kVehiclesPerPage;
    if (current_page >= page_count) current_page = page_count - 1U;
    DrawText(ui, "数量 / " + std::to_string(filtered.size()) +
        " 项匹配 / " + std::to_string(ids.size()) + " 辆已读取 / 第 " +
        std::to_string(current_page + 1U) + " / " + std::to_string(page_count) + " 页");
    if (has_selected) {
        if (ui->button(ui->user, anomaly::sdk::StringView("上一项"), 0.0F, 0.0F))
            QueueSelectionOffset(ids, selected, -1);
        DrawInlineGap(ui);
        if (ui->button(ui->user, anomaly::sdk::StringView("下一项"), 0.0F, 0.0F))
            QueueSelectionOffset(ids, selected, 1);
    }

    std::vector<std::size_t> visible_indices;
    const std::size_t page_begin = current_page * kVehiclesPerPage;
    const std::size_t page_end = (std::min)(filtered.size(), page_begin + kVehiclesPerPage);
    if (page_begin < page_end) {
        visible_indices.assign(filtered.begin() + static_cast<std::ptrdiff_t>(page_begin),
            filtered.begin() + static_cast<std::ptrdiff_t>(page_end));
    }

    // 按当前页实际条目数设置列表高度，避免 auto-fit child 在不同 UI Host 中保留多余空白。
    const bool has_catalog_child =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, begin_child),
            sizeof(decltype(AnomalyUiServiceV1::begin_child))) &&
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
            sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
        ui->begin_child != nullptr && ui->end_child != nullptr;
    const float catalog_height = static_cast<float>(
        38U + visible_indices.size() * 34U + 8U);
    const bool catalog_child_visible = has_catalog_child
        ? ui->begin_child(ui->user,
            anomaly::sdk::StringView("##vehicle-catalog-scroll"),
            0.0F, catalog_height, 1U) != 0
        : true;

    if (catalog_child_visible) {
        if (ids.empty()) {
            DrawText(ui, "清单是空的。请按「重新整理」重新读取游戏中的载具数据。");
        } else if (filtered.empty()) {
            DrawText(ui, "没有匹配项。请修改筛选条件。");
        } else {
            const bool can_table =
                UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_table),
                    sizeof(decltype(AnomalyUiServiceV1::end_table))) &&
                ui->begin_table && ui->table_next_row && ui->table_next_column && ui->end_table;
            const bool table_open = can_table && ui->begin_table(ui->user,
                anomaly::sdk::StringView("##vehicle-catalog-table"), 2,
                ANOMALY_UI_TABLE_V1_SIZING_STRETCH_PROP, 0.0F, 0.0F) != 0;
            if (table_open) {
                ui->table_next_row(ui->user);
                ui->table_next_column(ui->user);
                DrawText(ui, "名称 (VehicleID)");
                ui->table_next_column(ui->user);
                DrawText(ui, "操作");
                for (const std::size_t index : visible_indices) {
                    const std::string& id = ids[index];
                    const bool current = id == selected;
                    ui->table_next_row(ui->user);
                    ui->table_next_column(ui->user);
                    DrawText(ui, label_at(index));
                    ui->table_next_column(ui->user);
                    const std::string button_label =
                        std::string(current ? "当前##" : "选择##") + id;
                    if (ui->button(ui->user, anomaly::sdk::StringView(button_label), 0.0F, 0.0F)) {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.pending_vehicle_id = id;
                    }
                }
                ui->end_table(ui->user);
            } else {
                for (const std::size_t index : visible_indices) {
                    const std::string& id = ids[index];
                    const bool current = id == selected;
                    const std::string button_label =
                        std::string(current ? "当前  " : "选用  ") +
                        std::string(label_at(index)) + "##" + id;
                    if (ui->button(ui->user, anomaly::sdk::StringView(button_label), 0.0F, 0.0F)) {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.pending_vehicle_id = id;
                    }
                }
            }
        }
    }
    // EndChild is required even when BeginChild returns false.
    if (has_catalog_child && ui->end_child != nullptr)
        ui->end_child(ui->user);

    DrawDivider(ui);
    const bool can_summon = player_position_valid && has_selected;
    DrawText(ui, player_position_valid
        ? "玩家坐标：有效"
        : "玩家坐标：无效  /  召唤按钮保持禁用");
    if (DrawButtonEnabled(ui, "召唤", can_summon)) {
        g_context.summon.store(true, std::memory_order_release);
    }
    DrawText(ui, "载具目录分页");
    if (DrawButtonEnabled(ui, "上一页", current_page > 0U)) --current_page;
    DrawInlineGap(ui);
    if (DrawButtonEnabled(ui, "下一页", current_page + 1U < page_count)) ++current_page;
    DrawText(ui, "当前页 / " + std::to_string(current_page + 1U) +
        " / " + std::to_string(page_count));
    DrawDivider(ui);
    if (ui->button(ui->user, anomaly::sdk::StringView("重新整理"), 0.0F, 0.0F)) {
        g_context.refresh_catalog_requested.store(true, std::memory_order_release);
        SetStatus("已请求重新整理载具目录");
    }
}

void DrawPerformanceTab(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    float ratio, float torque_ratio, float other_speed_ratio, float other_speed_floor_ratio, float other_torque_ratio) {
    DrawSectionTitle(ui, "02  /  PERFORMANCE TUNING", "速度与发动机");
    if ((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) == 0) {
        DrawText(ui, "驾驶状态：未检测到当前驾驶载具");
    } else {
        DrawText(ui, "驾驶状态：已识别当前载具");
        DrawText(ui, "当前驾驶载具 / " +
            (snap.vehicle_class_name.empty() ? std::string("未知类名") : snap.vehicle_class_name) +
            " (" + (snap.vehicle_entity_id != 0
                ? "#" + std::to_string(snap.vehicle_entity_id) : std::string("#编号未知")) + ")");
        const double display_speed = std::isfinite(snap.speed_kmh)
            ? std::floor(std::fabs(snap.speed_kmh)) : 0.0;
        DrawText(ui, "当前速度  /  " + std::to_string(static_cast<std::uint64_t>(display_speed)) + " km/h");
        DrawText(ui, "实际 TopSpeedRatio（相对载具原始最高速）/ " +
        std::to_string(snap.top_speed_ratio) + "x");
    }

    DrawDivider(ui);
    DrawText(ui, "目标最高速 = 70 km/h × 速度因子");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("速度倍率"),
                         &ratio, 0.6024F, 10.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.speed_ratio = ratio;
    }
    DrawText(ui, "应用范围：当前玩家正在驾驶的载具");
    if (ui->button(ui->user, anomaly::sdk::StringView("应用车速倍率"), 0.0F, 0.0F)) {
        g_context.apply_speed.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, "发动机外部扭矩倍率");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("发动机扭矩倍率"),
                         &torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.engine_torque_ratio = torque_ratio;
    }
    DrawText(ui, "倍率以 Host 已验证的车辆原生参数为基准");
    if (ui->button(ui->user, anomaly::sdk::StringView("应用扭矩倍率"), 0.0F, 0.0F)) {
        g_context.apply_torque.store(true, std::memory_order_release);
    }
    DrawDivider(ui);
    DrawSectionTitle(ui, "02B  /  OTHER VEHICLE INSTANCES", "其他载具实例");
    DrawText(ui, "应用范围：当前已加载关卡中的 HTWheeledVehicleBase 实例；排除玩家当前驾驶载具");
    DrawText(ui, "目标最高速 = 70 km/h × 速度因子");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("其他车辆速度倍率"),
                         &other_speed_ratio, 0.05F, 10.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_speed_ratio = other_speed_ratio;
    }
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("其他车辆最低车速倍率"),
                         &other_speed_floor_ratio, 0.05F, 10.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_speed_floor_ratio = other_speed_floor_ratio;
    }
    DrawText(ui, "应用倍率低于此值时按下限执行；仅影响非玩家载具实例");
    if (ui->button(ui->user, anomaly::sdk::StringView("应用其他车辆倍率与下限"), 0.0F, 0.0F)) {
        g_context.apply_other_speed.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, "非玩家车辆发动机外部扭矩倍率");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("其他车辆扭矩倍率"),
                         &other_torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.other_torque_ratio = other_torque_ratio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用其他车辆扭矩倍率"), 0.0F, 0.0F)) {
        g_context.apply_other_torque.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    if (ui->button(ui->user, anomaly::sdk::StringView("恢复默认倍率  /  1.0x"), 0.0F, 0.0F)) {
        g_context.reset.store(true, std::memory_order_release);
    }
}

void DrawChassisTab(const AnomalyUiServiceV1* ui, bool friction) {
    DrawSectionTitle(ui, "03  /  CHASSIS & TRACTION", "底盘与车轮");
    DrawText(ui, std::string("车轮摩擦状态  /  ") + (friction ? "开启" : "关闭"));
    if (ui->button(ui->user, anomaly::sdk::StringView(
            friction ? "关闭车轮摩擦" : "开启车轮摩擦"), 0.0F, 0.0F)) {
        g_context.friction_toggle.store(true, std::memory_order_release);
    }
    DrawDivider(ui);
    DrawText(ui, "注：此选项控制车辆运动组件的车轮摩擦状态。");
}

void DrawLegacyLayout(
    const AnomalyUiServiceV1* ui, const Snapshot& snap, const std::string& status,
    const std::string& selected, const std::vector<std::string>& ids,
    const std::vector<std::string>& labels, const std::string& last_summon,
    float ratio, float torque_ratio,
    float other_speed_ratio, float other_speed_floor_ratio, float other_torque_ratio, bool friction, bool player_position_valid) {
    DrawSectionTitle(ui, "NTE  /  VEHICLE CONTROL", "载具控制");
    if (!status.empty()) DrawText(ui, "状态  /  " + status);
    DrawDivider(ui);
    DrawSummonTab(ui, selected, ids, labels, last_summon, player_position_valid);
    DrawDivider(ui);
    DrawPerformanceTab(ui, snap, ratio, torque_ratio, other_speed_ratio, other_speed_floor_ratio, other_torque_ratio);
    DrawDivider(ui);
    DrawChassisTab(ui, friction);
}

void Draw(const AnomalyUiServiceV1* ui) {
    if (!ui || !ui->begin_window || !ui->end_window || !ui->text ||
        !ui->button || !ui->slider_float) return;

    int open = 1;
    anomaly::sdk::UiWindow window(ui, "NTE Vehicle  /  载具控制", &open, 0);
    if (!window) return;

    Snapshot snap;
    std::string status;
    std::string selected;
    std::string last_summon;
    std::vector<std::string> ids;
    std::vector<std::string> labels;
    float ratio;
    float torque_ratio;
    float other_speed_ratio;
    float other_speed_floor_ratio;
    float other_torque_ratio;
    bool friction;
    bool player_position_valid{};
    {
        std::scoped_lock lock(g_context.mutex);
        snap = g_context.snapshot;
        status = g_context.status;
        selected = g_context.selected_vehicle_id;
        last_summon = g_context.last_summon;
        ids = g_context.vehicle_ids;
        labels = g_context.vehicle_labels;
        ratio = g_context.speed_ratio;
        torque_ratio = g_context.engine_torque_ratio;
        other_speed_ratio = g_context.other_speed_ratio;
        other_speed_floor_ratio = g_context.other_speed_floor_ratio;
        other_torque_ratio = g_context.other_torque_ratio;
        friction = g_context.friction_enabled;
        player_position_valid = g_context.player_position_valid;
    }

    if (!status.empty()) DrawText(ui, "运行状态  /  " + status);
    DrawText(ui, "插件构建标记  /  NTE Vehicle 0.9.6 / 2026-10-13-other-speed-floor");
    if ((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0) {
        DrawText(ui, "当前驾驶载具  /  " +
            (snap.vehicle_class_name.empty() ? std::string("未知类名") : snap.vehicle_class_name) +
            " (" + (snap.vehicle_entity_id != 0
                ? "#" + std::to_string(snap.vehicle_entity_id) : std::string("#编号未知")) + ")");
    } else {
        DrawText(ui, "当前驾驶载具  /  未识别；Host 状态：" +
            (snap.vehicle_snapshot_status.empty()
                ? std::string("当前驾驶载具对象不可用") : snap.vehicle_snapshot_status));
    }
    DrawDivider(ui);

    const bool has_tabs =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_tab_bar),
            sizeof(decltype(AnomalyUiServiceV1::end_tab_bar))) &&
        ui->begin_tab_bar && ui->begin_tab_item && ui->end_tab_item && ui->end_tab_bar;
    bool tab_bar_open = false;
    if (has_tabs) {
        tab_bar_open = ui->begin_tab_bar(ui->user,
            anomaly::sdk::StringView("##nte-vehicle-tabs"), ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN) != 0;
    }
    if (tab_bar_open) {
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("载具召唤"), nullptr,
                ANOMALY_UI_TAB_ITEM_V1_NONE, 1)) {
            DrawSummonTab(ui, selected, ids, labels, last_summon, player_position_valid);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("性能调校"), nullptr,
                ANOMALY_UI_TAB_ITEM_V1_NONE, 1)) {
            DrawPerformanceTab(ui, snap, ratio, torque_ratio, other_speed_ratio, other_speed_floor_ratio, other_torque_ratio);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("底盘设置"), nullptr,
                ANOMALY_UI_TAB_ITEM_V1_NONE, 1)) {
            DrawChassisTab(ui, friction);
            ui->end_tab_item(ui->user);
        }
        ui->end_tab_bar(ui->user);
    } else {
        DrawLegacyLayout(ui, snap, status, selected, ids, labels, last_summon,
            ratio, torque_ratio, other_speed_ratio, other_speed_floor_ratio, other_torque_ratio, friction, player_position_valid);
    }
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
        !vehicle->set_summon_vehicle_id ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::refresh_catalog)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, refresh_catalog)) ||
        !vehicle->refresh_catalog ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::catalog_label_at)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, catalog_label_at)) ||
        !vehicle->catalog_label_at ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_top_speed_ratio)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_top_speed_ratio)) ||
        !vehicle->set_other_vehicles_top_speed_ratio ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_min_speed_ratio)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_min_speed_ratio)) ||
        !vehicle->set_other_vehicles_min_speed_ratio ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::set_other_vehicles_engine_torque_ratio)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, set_other_vehicles_engine_torque_ratio)) ||
        !vehicle->set_other_vehicles_engine_torque_ratio || !player->snapshot) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    g_context.vehicle = vehicle;
    g_context.player = player;
    g_context.started.store(false, std::memory_order_release);
    g_context.apply_speed.store(false, std::memory_order_release);
    g_context.apply_torque.store(false, std::memory_order_release);
    g_context.apply_other_speed.store(false, std::memory_order_release);
    g_context.apply_other_torque.store(false, std::memory_order_release);
    g_context.reset.store(false, std::memory_order_release);
    g_context.summon.store(false, std::memory_order_release);
    g_context.friction_toggle.store(false, std::memory_order_release);
    g_context.refresh_catalog_requested.store(false, std::memory_order_release);
    g_context.speed_ratio = 1.0F;
    g_context.engine_torque_ratio = 1.0F;
    g_context.other_speed_ratio = 1.0F;
    g_context.other_speed_floor_ratio = 0.6024F;
    g_context.other_torque_ratio = 1.0F;
    g_context.friction_enabled = true;
    g_context.snapshot = {};
    g_context.selected_vehicle_id = "Vehicle007";
    g_context.pending_vehicle_id.clear();
    g_context.catalog_filter.clear();
    g_context.vehicle_ids.clear();
    g_context.vehicle_labels.clear();
    g_context.last_summon.clear();
    g_context.status = "正在读取 Host 验证的运行时 Vehicle 目录";
    g_context.player_position_valid = false;
    g_context.player_snapshot = {};
    g_context.catalog_tick = 0;
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.refresh_catalog_requested.store(true, std::memory_order_release);
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
        anomaly::sdk::StringView("0.9.5"), Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}
