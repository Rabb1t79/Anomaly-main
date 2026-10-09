#include "anomaly/sdk/cpp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

struct RenderSnapshot {
    AnomalyNteVehicleSnapshotV1 vehicle{sizeof(vehicle)};
    std::vector<std::string> vehicle_ids;
    std::uint64_t catalog_sequence{};
    std::uint32_t count{};
    std::uint32_t page{};
    std::string selected_id;
    std::string status{"等待 Host 验证游戏载具数据表"};
    bool catalog_ready{};
    bool player_position_valid{};
    bool driving_vehicle_valid{};
};

struct UiIntent {
    bool previous_page{};
    bool next_page{};
    bool select{};
    std::uint32_t select_index{};
    std::uint64_t select_sequence{};
    bool summon{};
    bool speed_ratio_pending{};
    float speed_ratio{1.0F};
};

struct Context {
    // These fields are owned by on_update (Game domain). Draw never reads them.
    const AnomalyNteVehicleServiceV1* vehicle_service{};
    const AnomalyNtePlayerServiceV1* player_service{};
    AnomalyNteVehicleCatalogSnapshotV1 catalog{sizeof(catalog)};
    AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
    AnomalyNteVehicleSnapshotV1 vehicle{sizeof(vehicle)};
    std::vector<std::string> vehicle_ids;
    std::string selected_id;
    std::string status{"等待 Host 验证游戏载具数据表"};
    std::uint32_t page{};
    bool catalog_ready{};
    bool player_position_valid{};
    bool driving_vehicle_valid{};
    // Persist the close button state for the current plugin generation.
    int window_open{1};

    // Cross-domain state only carries Render intents and immutable Render snapshots.
    std::mutex intent_mutex;
    UiIntent intents;
    std::atomic_bool running{};
    std::atomic_bool reset_for_start{};
    std::atomic<std::shared_ptr<const RenderSnapshot>> render_snapshot;
} g;

AnomalyStatusV1 Status(const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

void PublishSnapshot() {
    RenderSnapshot view;
    view.vehicle = g.vehicle;
    view.vehicle_ids = g.vehicle_ids;
    view.catalog_sequence = g.catalog.sequence;
    view.count = g.catalog.entry_count;
    view.page = g.page;
    view.selected_id = g.selected_id;
    view.status = g.status;
    view.catalog_ready = g.catalog_ready;
    view.player_position_valid = g.player_position_valid;
    view.driving_vehicle_valid = g.driving_vehicle_valid;
    g.render_snapshot.store(std::make_shared<const RenderSnapshot>(std::move(view)),
                            std::memory_order_release);
}

bool ReadVehicleIdOnGameThread(const std::uint32_t index, std::string& output) {
    if (g.vehicle_service == nullptr || g.vehicle_service->vehicle_id_at == nullptr) return false;
    std::array<char, ANOMALY_NTE_VEHICLE_ID_MAX_UTF8_BYTES> buffer{};
    std::size_t size = buffer.size();
    const auto result = g.vehicle_service->vehicle_id_at(
        g.vehicle_service->user, index, buffer.data(), &size);
    if (result.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > buffer.size()) return false;
    output.assign(buffer.data(), size - 1U);
    return !output.empty();
}

UiIntent TakeUiIntents() {
    std::scoped_lock lock(g.intent_mutex);
    UiIntent intent = g.intents;
    g.intents = {};
    return intent;
}

void Update(void*, double) {
    if (!g.running.load(std::memory_order_acquire)) return;
    const UiIntent intent = TakeUiIntents();

    if (g.reset_for_start.exchange(false, std::memory_order_acq_rel)) {
        g.catalog_ready = false;
        g.page = 0;
        g.selected_id.clear();
        g.status = "插件已启动，正在重新验证游戏载具数据表";
    }

    if (g.vehicle_service != nullptr && g.vehicle_service->catalog_snapshot != nullptr) {
        AnomalyNteVehicleCatalogSnapshotV1 next{sizeof(next)};
        const auto result = g.vehicle_service->catalog_snapshot(
            g.vehicle_service->user, &next);
        if (result.code == ANOMALY_STATUS_V1_OK &&
            (next.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) != 0) {
            const bool refresh = !g.catalog_ready || next.sequence != g.catalog.sequence ||
                next.entry_count != g.vehicle_ids.size();
            g.catalog = next;
            if (refresh) {
                std::vector<std::string> ids;
                ids.reserve(next.entry_count);
                bool complete = true;
                for (std::uint32_t index = 0; index < next.entry_count; ++index) {
                    std::string id;
                    if (!ReadVehicleIdOnGameThread(index, id)) {
                        complete = false;
                        break;
                    }
                    ids.push_back(std::move(id));
                }
                AnomalyNteVehicleCatalogSnapshotV1 after{sizeof(after)};
                const auto verify = g.vehicle_service->catalog_snapshot(
                    g.vehicle_service->user, &after);
                if (complete && ids.size() == next.entry_count &&
                    verify.code == ANOMALY_STATUS_V1_OK &&
                    after.sequence == next.sequence && after.entry_count == next.entry_count &&
                    (after.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) != 0) {
                    g.vehicle_ids = std::move(ids);
                    g.catalog = after;
                    g.catalog_ready = true;
                    const std::uint32_t max_page = g.catalog.entry_count == 0
                        ? 0U : (g.catalog.entry_count - 1U) / 6U;
                    g.page = (std::min)(g.page, max_page);
                    if (!g.selected_id.empty() &&
                        std::ranges::find(g.vehicle_ids, g.selected_id) == g.vehicle_ids.end()) {
                        g.selected_id.clear();
                        g.status = "目录已刷新，原 VehicleID 不再有效，请重新选择";
                    } else {
                        g.status = g.vehicle_ids.empty()
                            ? "数据表有效，但没有匹配 Vehicle 字段的行"
                            : "DT_VehicleData 已完整读取";
                    }
                } else {
                    g.catalog_ready = false;
                    g.status = "目录读取期间发生变化或行读取失败；已禁用选择和召唤";
                }
            }
        } else {
            g.catalog_ready = false;
            g.status = "Host 当前未验证 DT_VehicleData；召唤保持禁用";
        }
    }

    // Refresh the live player snapshot before processing summon intent, so a click never
    // reuses the prior frame's coordinates.
    if (g.player_service != nullptr && g.player_service->snapshot != nullptr) {
        AnomalyNtePlayerSnapshotV1 next{sizeof(next)};
        const auto result = g.player_service->snapshot(g.player_service->user, &next);
        g.player_position_valid = result.code == ANOMALY_STATUS_V1_OK &&
            (next.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
            std::ranges::all_of(next.position, [](double value) { return std::isfinite(value); });
        if (g.player_position_valid) g.player = next;
    }

    if (g.vehicle_service != nullptr && g.vehicle_service->snapshot != nullptr) {
        AnomalyNteVehicleSnapshotV1 next{sizeof(next)};
        const auto result = g.vehicle_service->snapshot(g.vehicle_service->user, &next);
        g.driving_vehicle_valid = result.code == ANOMALY_STATUS_V1_OK &&
            (next.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0;
        if (g.driving_vehicle_valid) g.vehicle = next;
    }

    // Slider changes are queued from Render and applied only on the Game domain.
    if (intent.speed_ratio_pending) {
        if (!g.driving_vehicle_valid || g.vehicle_service == nullptr ||
            g.vehicle_service->set_top_speed_ratio == nullptr) {
            g.status = "车速倍率未应用：当前没有有效的驾驶载具或 Host setter";
        } else {
            const float requested = (std::clamp)(intent.speed_ratio, 0.05F, 20.0F);
            const auto result = g.vehicle_service->set_top_speed_ratio(
                g.vehicle_service->user, requested);
            if (result.code == ANOMALY_STATUS_V1_OK) {
                g.vehicle.top_speed_ratio = requested;
                g.status = "车速倍率已提交到 Host：" + std::to_string(requested) + "x";
            } else if (result.message.data != nullptr && result.message.size != 0) {
                g.status = "车速倍率设置失败：" +
                    std::string(result.message.data, result.message.size);
            } else {
                g.status = "车速倍率设置失败，状态码 " + std::to_string(result.code);
            }
        }
    }

    const std::uint32_t pages = g.catalog.entry_count == 0
        ? 1U : (g.catalog.entry_count + 5U) / 6U;
    if (intent.previous_page && g.page > 0) --g.page;
    if (intent.next_page && g.page + 1U < pages) ++g.page;

    if (intent.select) {
        if (intent.select_sequence != g.catalog.sequence || !g.catalog_ready ||
            intent.select_index >= g.vehicle_ids.size() || g.vehicle_service == nullptr ||
            g.vehicle_service->set_summon_vehicle_id == nullptr) {
            g.status = "选择请求已过期；请从当前载具目录重新选择";
        } else {
            const std::string& id = g.vehicle_ids[intent.select_index];
            const AnomalyStringViewV1 requested{id.data(), id.size()};
            const auto result = g.vehicle_service->set_summon_vehicle_id(
                g.vehicle_service->user, requested);
            if (result.code == ANOMALY_STATUS_V1_OK) {
                g.selected_id = id;
                g.status = "已选择有效 VehicleID：" + id;
            } else {
                g.status = "Host 拒绝该 VehicleID；未更改召唤目标";
            }
        }
    }

    if (intent.summon) {
        if (!g.catalog_ready || !g.player_position_valid || g.selected_id.empty() ||
            g.vehicle_service == nullptr || g.vehicle_service->summon_vehicle == nullptr) {
            g.status = "召唤未执行：需要有效数据表、已选 VehicleID 和有效玩家坐标";
        } else {
            AnomalyNteVehicleSummonRequestV1 request{};
            request.struct_size = sizeof(request);
            request.flags = ANOMALY_NTE_VEHICLE_SUMMON_V1_HAS_POSITION |
                ANOMALY_NTE_VEHICLE_SUMMON_V1_SET_OWNER_TO_PLAYER;
            request.world_position[0] = g.player.position[0] - 2000.0;
            request.world_position[1] = g.player.position[1] + 2000.0;
            request.world_position[2] = g.player.position[2] + 2000.0;
            const auto result = g.vehicle_service->summon_vehicle(
                g.vehicle_service->user, &request);
            g.status = result.code == ANOMALY_STATUS_V1_OK
                ? "Host 已确认召唤请求；坐标及 Actor.Owner 由 Host 后置核验"
                : "召唤失败或后置核验未通过；不会显示为成功";
        }
    }

    PublishSnapshot();
}

void Draw(void*, const AnomalyUiServiceV1* ui) {
    if (!g.running.load(std::memory_order_acquire) || ui == nullptr ||
        ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr || ui->button == nullptr) return;

    // Immutable snapshot load is non-blocking; no Game service call or shared-state lock in Render.
    const auto view = g.render_snapshot.load(std::memory_order_acquire);
    if (!view) return;

    int open = g.window_open;
    anomaly::sdk::UiWindow window(ui, "NTE Vehicle Catalog", &open, 0);
    g.window_open = open;
    if (!window) return; // UiWindow calls end_window even when begin_window returns false.

    ui->text(ui->user, anomaly::sdk::StringView(
        "数据源：Host 从 HTGame DT_VehicleData 验证后的 VehicleID 目录"));
    char info[192]{};
    std::snprintf(info, sizeof(info), "符合 Vehicle 条件的目录条目：%u", view->count);
    ui->text(ui->user, anomaly::sdk::StringView(info));
    ui->text(ui->user, anomaly::sdk::StringView(view->status));
    if (!view->catalog_ready || view->count == 0) {
        ui->text(ui->user, anomaly::sdk::StringView("目录验证并完整读取前，选择与召唤均不可用。"));
        return;
    }

    const std::uint32_t pages = (view->count + 5U) / 6U;
    if (ui->button(ui->user, anomaly::sdk::StringView("上一页"), 90.0F, 0.0F) != 0 && view->page > 0) {
        std::scoped_lock lock(g.intent_mutex);
        g.intents.previous_page = true;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("下一页"), 90.0F, 0.0F) != 0 && view->page + 1U < pages) {
        std::scoped_lock lock(g.intent_mutex);
        g.intents.next_page = true;
    }
    std::snprintf(info, sizeof(info), "页码：%u / %u", view->page + 1U, pages);
    ui->text(ui->user, anomaly::sdk::StringView(info));

    for (std::uint32_t row = 0; row < 6; ++row) {
        const std::uint32_t index = view->page * 6U + row;
        if (index >= view->count || index >= view->vehicle_ids.size()) break;
        const auto& id = view->vehicle_ids[index];
        const std::string label = (id == view->selected_id ? "[已选] " : "选择 ") + id;
        if (ui->button(ui->user, anomaly::sdk::StringView(label), 280.0F, 0.0F) != 0) {
            std::scoped_lock lock(g.intent_mutex);
            g.intents.select = true;
            g.intents.select_index = index;
            g.intents.select_sequence = view->catalog_sequence;
        }
    }

    ui->text(ui->user, anomaly::sdk::StringView(
        view->selected_id.empty() ? "召唤目标：未选择" : "召唤目标：" + view->selected_id));
    if (ui->button(ui->user, anomaly::sdk::StringView("召唤到玩家附近 (X-2000, Y+2000, Z+2000)"), 300.0F, 0.0F) != 0) {
        std::scoped_lock lock(g.intent_mutex);
        g.intents.summon = true;
    }
    if (view->driving_vehicle_valid) {
        std::snprintf(info, sizeof(info), "当前驾驶速度：%.1f km/h", view->vehicle.speed_kmh);
        ui->text(ui->user, anomaly::sdk::StringView(info));
        if (ui->slider_float != nullptr) {
            float ratio = view->vehicle.top_speed_ratio;
            if (!std::isfinite(ratio) || ratio < 0.05F || ratio > 20.0F) ratio = 1.0F;
            if (ui->slider_float(ui->user,
                    anomaly::sdk::StringView("车速倍率（发动机扭矩）"),
                    &ratio, 0.05F, 20.0F) != 0) {
                std::scoped_lock lock(g.intent_mutex);
                g.intents.speed_ratio_pending = true;
                g.intents.speed_ratio = ratio;
            }
        } else {
            ui->text(ui->user, anomaly::sdk::StringView("当前 Host UI 未提供 slider_float"));
        }
    } else {
        ui->text(ui->user, anomaly::sdk::StringView("当前未检测到正在驾驶的载具"));
    }
    if (view->player_position_valid) {
        ui->text(ui->user, anomaly::sdk::StringView("玩家坐标快照有效"));
    }
    g.window_open = open;
}

AnomalyStatusV1 Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr)
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const anomaly::sdk::Host api(host);
    const auto vehicle = api.Query<AnomalyNteVehicleServiceV1>(
        ANOMALY_NTE_VEHICLE_SERVICE_V1_ID, ANOMALY_NTE_VEHICLE_SERVICE_V1_VERSION);
    const auto player = api.Query<AnomalyNtePlayerServiceV1>(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!vehicle || !player || !vehicle->catalog_snapshot || !vehicle->vehicle_id_at ||
        !vehicle->set_summon_vehicle_id || !vehicle->summon_vehicle || !vehicle->snapshot ||
        !vehicle->set_top_speed_ratio || !player->snapshot) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "required vehicle-v2/player service missing");
    }

    g.vehicle_service = vehicle.get();
    g.player_service = player.get();
    g.catalog = {sizeof(g.catalog), 0, 0, 0, 0};
    g.player = {sizeof(g.player)};
    g.vehicle = {sizeof(g.vehicle)};
    g.vehicle_ids.clear();
    g.selected_id.clear();
    g.page = 0;
    g.catalog_ready = false;
    g.player_position_valid = false;
    g.driving_vehicle_valid = false;
    g.status = "等待 Host 验证游戏载具数据表";
    {
        std::scoped_lock lock(g.intent_mutex);
        g.intents = {};
    }
    g.render_snapshot.store(std::make_shared<const RenderSnapshot>(), std::memory_order_release);
    g.running.store(false, std::memory_order_release);
    g.window_open = 1;
    g.reset_for_start.store(false, std::memory_order_release);
    *plugin_context = &g;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 Start(void* plugin_context) {
    if (plugin_context != &g) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g.window_open = 1;
    g.reset_for_start.store(true, std::memory_order_release);
    g.running.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g.running.store(false, std::memory_order_release);
    std::scoped_lock lock(g.intent_mutex);
    g.intents = {};
    return anomaly::sdk::Ok();
}

void Unload(void* plugin_context) {
    if (plugin_context != &g) return;
    g.running.store(false, std::memory_order_release);
    g.vehicle_service = nullptr;
    g.player_service = nullptr;
    g.render_snapshot.store({}, std::memory_order_release);
    std::scoped_lock lock(g.intent_mutex);
    g.intents = {};
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-vehicle"),
        anomaly::sdk::StringView("NTE Vehicle"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.8.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
