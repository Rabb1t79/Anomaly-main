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
#include <utility>

namespace {

// 非 DEBUG 状态下的倍率策略：固定 10x；所有精细倍率路径均在调用 Host 前再次校验。
constexpr float kLockedSpeedRatio = 10.0F;
constexpr float kFineSpeedRatioMin = 0.50F;
constexpr float kFineSpeedRatioMax = 10.0F;

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

struct CustomizationOption {
    std::string id;
    std::string label;
    std::uint32_t kind{};
    std::uint32_t module_type{};
    bool is_default{};
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
    bool customization_page{};
    std::string customization_vehicle_id;
    std::string customization_skin_id;
    std::string customization_status{"尚未读取当前车型的改装数据"};
    std::vector<CustomizationOption> customization_options;
    std::vector<std::pair<std::uint32_t, std::string>> selected_module_overrides;
    std::uint64_t customization_catalog_tick{};
    std::atomic_bool refresh_customization_requested{};
    std::atomic_bool refresh_catalog_requested{};
    std::atomic_bool apply_speed{};
    std::atomic_bool apply_torque{};
    std::atomic_bool apply_other_speed{};
    std::atomic_bool apply_other_torque{};
    std::atomic_bool reset{};
    std::atomic_bool summon{};
    std::atomic_bool summon_customized{};
    std::atomic_bool friction_toggle{};
    std::atomic_bool started{};
    float speed_ratio{1.0F};
    float engine_torque_ratio{1.0F};
    float other_speed_ratio{1.0F};
    float other_speed_floor_ratio{0.6024F};
    float other_torque_ratio{1.0F};
    bool friction_enabled{true};
    bool debug_unlocked{};
    bool fine_speed_tuning_enabled{};
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

// 使用可移植 SHA-256 校验 DEBUG 解锁密钥；插件二进制只保存摘要，不保存密钥明文。
std::array<std::uint8_t, 32> Sha256(std::string_view input) {
    constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    const auto rotr = [](std::uint32_t v, unsigned n) noexcept {
        return (v >> n) | (v << (32U - n));
    };
    std::vector<std::uint8_t> data;
    data.reserve(input.size() + 72U);
    for (const unsigned char c : input) data.push_back(c);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) data.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8)
        data.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));

    std::array<std::uint32_t, 8> h{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    for (std::size_t block = 0; block < data.size(); block += 64U) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16U; ++i) {
            const std::size_t p = block + i * 4U;
            w[i] = (static_cast<std::uint32_t>(data[p]) << 24U) |
                   (static_cast<std::uint32_t>(data[p + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(data[p + 2U]) << 8U) |
                   static_cast<std::uint32_t>(data[p + 3U]);
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
            const std::uint32_t s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
            w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
        }
        auto a = h[0]; auto b = h[1]; auto c = h[2]; auto d = h[3];
        auto e = h[4]; auto f = h[5]; auto g = h[6]; auto hh = h[7];
        for (std::size_t i = 0; i < 64U; ++i) {
            const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < digest.size(); ++i)
        digest[i] = static_cast<std::uint8_t>((h[i / 4U] >> (24U - (i % 4U) * 8U)) & 0xffU);
    return digest;
}

bool IsDebugKeyValid(std::string_view candidate) {
    if (candidate.size() != 64U) return false;
    // Stable DEBUG key digest. Preserve unchanged across maintenance releases unless the user explicitly requests rotation.
    constexpr std::array<std::uint8_t, 32> expected{
        0xe7U, 0x1aU, 0xdeU, 0x1aU, 0xcfU, 0xf9U, 0xc7U, 0x39U,
        0xccU, 0x17U, 0x59U, 0x72U, 0xfaU, 0xf6U, 0x0eU, 0x2bU,
        0xbaU, 0x75U, 0xa7U, 0xdaU, 0x04U, 0xfaU, 0xbaU, 0xfdU,
        0x2bU, 0xafU, 0x8cU, 0x94U, 0x23U, 0xb7U, 0x6fU, 0x85U
    };
    const auto actual = Sha256(candidate);
    std::uint8_t difference = 0U;
    for (std::size_t i = 0; i < actual.size(); ++i)
        difference = static_cast<std::uint8_t>(difference | (actual[i] ^ expected[i]));
    return difference == 0U;
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

void RefreshCustomizationCatalogOnGameThread() {
    const auto* vehicle = g_context.vehicle;
    std::string selected_id;
    {
        std::scoped_lock lock(g_context.mutex);
        selected_id = g_context.selected_vehicle_id;
    }
    const bool callbacks_available =
        vehicle != nullptr &&
        HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_catalog_snapshot)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_catalog_snapshot)) &&
        vehicle->customization_catalog_snapshot != nullptr &&
        HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_option_at)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_option_at)) &&
        vehicle->customization_option_at != nullptr;
    if (!callbacks_available || selected_id.empty()) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id.clear();
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status = selected_id.empty()
            ? "请先从召唤清单中选择车型"
            : "当前 Core 未发布 V7 改装目录接口；请更新 Runtime";
        return;
    }

    AnomalyNteVehicleCustomizationCatalogSnapshotV1 before{};
    before.struct_size = sizeof(before);
    const auto before_status = vehicle->customization_catalog_snapshot(
        vehicle->user, anomaly::sdk::StringView(selected_id), &before);
    if (before_status.code != ANOMALY_STATUS_V1_OK ||
        (before.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        before.entry_count > 8192U) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status = StatusDescription("读取当前车型改装目录失败", before_status);
        return;
    }

    std::vector<CustomizationOption> options;
    options.reserve(before.entry_count);
    for (std::uint32_t index = 0; index < before.entry_count; ++index) {
        AnomalyNteVehicleCustomizationOptionV1 native_option{};
        native_option.struct_size = sizeof(native_option);
        const auto option_status = vehicle->customization_option_at(
            vehicle->user, anomaly::sdk::StringView(selected_id), index, &native_option);
        if (option_status.code != ANOMALY_STATUS_V1_OK ||
            (native_option.flags & ANOMALY_NTE_VEHICLE_CUSTOMIZATION_OPTION_V1_VALID) == 0) {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_options.clear();
            g_context.customization_vehicle_id = selected_id;
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = StatusDescription(
                "读取改装目录选项失败（索引 " + std::to_string(index) + "）", option_status);
            return;
        }
        const auto id_end = std::find(
            std::begin(native_option.option_id), std::end(native_option.option_id), '\0');
        const auto label_end = std::find(
            std::begin(native_option.label), std::end(native_option.label), '\0');
        if (id_end == std::end(native_option.option_id) ||
            label_end == std::end(native_option.label) ||
            id_end == std::begin(native_option.option_id) ||
            (native_option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE &&
             native_option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN) ||
            (native_option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE &&
             (native_option.module_type == 0 || native_option.module_type >= 27)) ||
            (native_option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN &&
             native_option.module_type != 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_options.clear();
            g_context.customization_vehicle_id = selected_id;
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = "改装目录包含无效 ID、类型或未终止的 ABI 字符串";
            return;
        }
        CustomizationOption option{};
        option.id.assign(native_option.option_id, static_cast<std::size_t>(
            id_end - std::begin(native_option.option_id)));
        option.label.assign(native_option.label, static_cast<std::size_t>(
            label_end - std::begin(native_option.label)));
        option.kind = native_option.kind;
        option.module_type = native_option.module_type;
        option.is_default =
            (native_option.flags & ANOMALY_NTE_VEHICLE_CUSTOMIZATION_OPTION_V1_DEFAULT) != 0;
        options.push_back(std::move(option));
    }

    AnomalyNteVehicleCustomizationCatalogSnapshotV1 after{};
    after.struct_size = sizeof(after);
    const auto after_status = vehicle->customization_catalog_snapshot(
        vehicle->user, anomaly::sdk::StringView(selected_id), &after);
    if (after_status.code != ANOMALY_STATUS_V1_OK ||
        (after.flags & ANOMALY_NTE_VEHICLE_CATALOG_V1_VALID) == 0 ||
        after.sequence != before.sequence || after.entry_count != options.size()) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_options.clear();
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id.clear();
        g_context.selected_module_overrides.clear();
        g_context.customization_status =
            "读取改装目录期间车型数据发生变化；请重新打开「改造清单」";
        return;
    }

    std::string default_skin;
    for (const auto& option : options) {
        if (option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN &&
            option.is_default) {
            default_skin = option.id;
            break;
        }
    }
    {
        std::scoped_lock lock(g_context.mutex);
        if (selected_id != g_context.selected_vehicle_id) return;
        g_context.customization_options = std::move(options);
        g_context.customization_vehicle_id = selected_id;
        g_context.customization_skin_id = std::move(default_skin);
        g_context.selected_module_overrides.clear();
        g_context.customization_catalog_tick = after.sequence;
        const auto module_count = std::ranges::count_if(
            g_context.customization_options, [](const CustomizationOption& option) {
                return option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE;
            });
        const auto skin_count = std::ranges::count_if(
            g_context.customization_options, [](const CustomizationOption& option) {
                return option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN;
            });
        g_context.customization_status = "已读取该车型实时改装选项：模块 " +
            std::to_string(module_count) + " 项，涂装 " + std::to_string(skin_count) +
            " 项；未选择的模块槽位沿用游戏数据表默认配置";
        if (g_context.customization_skin_id.empty()) {
            g_context.customization_status += "；该车型没有可自动识别的默认涂装，请手动选择涂装后召唤";
        }
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
            if (g_context.customization_page) {
                g_context.customization_vehicle_id.clear();
                g_context.customization_options.clear();
                g_context.customization_skin_id.clear();
                g_context.selected_module_overrides.clear();
                g_context.refresh_customization_requested.store(true, std::memory_order_release);
            }
        } else {
            SetStatus(StatusDescription("载具选择失败", status));
        }
    }

    if (g_context.refresh_customization_requested.exchange(false, std::memory_order_acq_rel)) {
        RefreshCustomizationCatalogOnGameThread();
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
        const bool has_core_switch =
            HasField<AnomalyNteVehicleServiceV1,
                decltype(AnomalyNteVehicleServiceV1::set_fine_speed_tuning_enabled)>(
                    g_context.vehicle,
                    offsetof(AnomalyNteVehicleServiceV1, set_fine_speed_tuning_enabled)) &&
            g_context.vehicle->set_fine_speed_tuning_enabled != nullptr;
        if (has_core_switch) {
            static_cast<void>(g_context.vehicle->set_fine_speed_tuning_enabled(
                g_context.vehicle->user, 0U));
        }
        const auto status = g_context.vehicle->reset(g_context.vehicle->user);
        {
            std::scoped_lock lock(g_context.mutex);
            // Reset 不能成为绕过锁定倍率的后门；重置后关闭精细模式并恢复固定 10x。
            g_context.fine_speed_tuning_enabled = false;
            g_context.speed_ratio = kLockedSpeedRatio;
        }
        g_context.apply_speed.store(true, std::memory_order_release);
        SetStatus(status.code == ANOMALY_STATUS_V1_OK
            ? "已恢复默认载具设置；车速倍率策略恢复为固定 10.0x"
            : "恢复失败；仍将重新应用固定车速倍率");
    }

    if (g_context.apply_speed.exchange(false, std::memory_order_acq_rel)) {
        float ratio = kLockedSpeedRatio;
        {
            std::scoped_lock lock(g_context.mutex);
            // Fail closed: UI 状态、DEBUG 解锁状态和精细调整开关必须同时满足。
            if (g_context.debug_unlocked && g_context.fine_speed_tuning_enabled) {
                ratio = std::clamp(g_context.speed_ratio,
                    kFineSpeedRatioMin, kFineSpeedRatioMax);
                g_context.speed_ratio = ratio;
            } else {
                g_context.speed_ratio = kLockedSpeedRatio;
            }
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

    const bool summon_requested = g_context.summon.exchange(false, std::memory_order_acq_rel);
    const bool summon_customized_requested = g_context.summon_customized.exchange(false, std::memory_order_acq_rel);
    if (summon_requested || summon_customized_requested) {
        AnomalyNteVehicleSummonRequestV1 request{};
        request.struct_size = sizeof(request);
        request.flags = ANOMALY_NTE_VEHICLE_SUMMON_V1_HAS_POSITION |
            ANOMALY_NTE_VEHICLE_SUMMON_V1_SET_OWNER_TO_PLAYER;

        std::string selected_id;
        std::string selected_label;
        std::string selected_skin_id;
        std::string customization_vehicle_id;
        std::vector<std::pair<std::uint32_t, std::string>> module_overrides;
        std::vector<AnomalyNteVehicleModuleSelectionV1> module_selections;
        bool position_valid{};
        {
            std::scoped_lock lock(g_context.mutex);
            selected_id = g_context.selected_vehicle_id;
            selected_skin_id = g_context.customization_skin_id;
            customization_vehicle_id = g_context.customization_vehicle_id;
            module_overrides = g_context.selected_module_overrides;
            position_valid = g_context.player_position_valid;
            const auto selected_index = std::ranges::find(g_context.vehicle_ids, selected_id);
            if (selected_index != g_context.vehicle_ids.end()) {
                const auto index = static_cast<std::size_t>(selected_index - g_context.vehicle_ids.begin());
                selected_label = index < g_context.vehicle_labels.size()
                    ? g_context.vehicle_labels[index] : selected_id;
            }
            if (position_valid) {
                // Keep the existing verified spawn position unchanged.
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

        std::string validation_error;
        if (summon_customized_requested) {
            if (customization_vehicle_id != selected_id) {
                validation_error = "改装召唤失败：改造清单与当前选择的车型不一致，请重新打开清单";
            } else if (selected_skin_id.empty()) {
                validation_error = "改装召唤失败：尚未选择有效涂装";
            } else {
                request.flags |= ANOMALY_NTE_VEHICLE_SUMMON_V1_APPLY_CUSTOM_CONFIGURATION;
                request.skin_id = anomaly::sdk::StringView(std::string_view(
                    selected_skin_id.data(), selected_skin_id.size()));
                module_selections.reserve(module_overrides.size());
                for (const auto& override_entry : module_overrides) {
                    AnomalyNteVehicleModuleSelectionV1 selection{};
                    selection.struct_size = sizeof(selection);
                    selection.module_type = override_entry.first;
                    selection.module_id = anomaly::sdk::StringView(std::string_view(
                        override_entry.second.data(), override_entry.second.size()));
                    module_selections.push_back(selection);
                }
                request.module_selections = module_selections.empty()
                    ? nullptr : module_selections.data();
                request.module_selection_count =
                    static_cast<std::uint32_t>(module_selections.size());
            }
        }

        if (!validation_error.empty()) {
            SetStatus(validation_error);
            SetLastSummon(validation_error);
        } else if (!position_valid || !catalog_has_selection) {
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
                if (postcondition_verified && summon_customized_requested) {
                    result = "改装载具召唤成功：" + detail;
                    SetStatus(result);
                    SetLastSummon(result);
                } else if (postcondition_verified) {
                    SetStatus({});
                    SetLastSummon({});
                } else {
                    result = "召唤状态未验证：" + label + " (" + selected_id +
                        ")；Host 返回 OK，但没有明确的实际生成/后置验证标记";
                    if (!detail.empty()) result += "；Host 信息：" + detail;
                    SetStatus(result);
                    SetLastSummon(result);
                }
            } else {
                const auto reason = StatusDescription(
                    summon_customized_requested ? "改装载具召唤失败" : "召唤失败", status);
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
    const auto label_at = [&labels, &ids](std::size_t i) -> std::string_view {
        return i < labels.size() && !labels[i].empty()
            ? std::string_view(labels[i]) : std::string_view(ids[i]);
    };
    const bool has_selected = std::ranges::find(ids, selected) != ids.end();
    if (!last_summon.empty()) DrawText(ui, "上次召唤：" + last_summon);

    std::string current_filter;
    {
        std::scoped_lock lock(g_context.mutex);
        current_filter = g_context.catalog_filter;
    }
    const auto filter_ids = [&](std::string_view filter) {
        std::vector<std::size_t> matches_out;
        matches_out.reserve(ids.size());
        const bool can_host_filter =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, filter_match),
                sizeof(decltype(AnomalyUiServiceV1::filter_match))) && ui->filter_match;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto label = label_at(i);
            bool matches = filter.empty();
            if (!matches && can_host_filter) {
                matches = ui->filter_match(ui->user, anomaly::sdk::StringView(filter),
                    anomaly::sdk::StringView(label)) != 0;
                if (!matches) {
                    matches = ui->filter_match(ui->user, anomaly::sdk::StringView(filter),
                        anomaly::sdk::StringView(ids[i])) != 0;
                }
            } else if (!matches) {
                matches = AsciiContainsInsensitive(label, filter) ||
                    AsciiContainsInsensitive(ids[i], filter);
            }
            if (matches) matches_out.push_back(i);
        }
        return matches_out;
    };

    // 列表不创建可滚动子窗口；分页在渲染阶段限制为最多五行，因此不会显示旧版滚动条。
    constexpr std::size_t kVehiclesPerPage = 5U;
    static std::size_t current_page = 0U;
    auto filtered = filter_ids(current_filter);
    std::size_t page_count = filtered.empty()
        ? 1U : (filtered.size() + kVehiclesPerPage - 1U) / kVehiclesPerPage;
    if (current_page >= page_count) current_page = page_count - 1U;
    const std::size_t page_begin = current_page * kVehiclesPerPage;
    const std::size_t page_end = (std::min)(filtered.size(), page_begin + kVehiclesPerPage);
    std::vector<std::size_t> visible_indices;
    if (page_begin < page_end) {
        visible_indices.assign(filtered.begin() + static_cast<std::ptrdiff_t>(page_begin),
            filtered.begin() + static_cast<std::ptrdiff_t>(page_end));
    }

    if (ids.empty()) {
        DrawText(ui, "载具清单为空。");
    } else if (filtered.empty()) {
        DrawText(ui, "没有匹配项。");
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
            DrawText(ui, "载具");
            ui->table_next_column(ui->user);
            DrawText(ui, "选择");
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
                    std::string(current ? "当前  " : "选择  ") +
                    std::string(label_at(index)) + "##" + id;
                if (ui->button(ui->user, anomaly::sdk::StringView(button_label), 0.0F, 0.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.pending_vehicle_id = id;
                }
            }
        }
    }

    // 筛选标签与搜索框放在载具选项列表下方、分页控件上方；这里不创建滚动容器。
    DrawDivider(ui);
    const bool has_input_text =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, input_text),
            sizeof(decltype(AnomalyUiServiceV1::input_text))) && ui->input_text;
    std::array<char, 128> filter_buffer{};
    const std::size_t filter_length = (std::min)(current_filter.size(), filter_buffer.size() - 1U);
    std::copy_n(current_filter.data(), filter_length, filter_buffer.data());
    if (has_input_text) {
        DrawText(ui, "筛选");
        if (ui->input_text(ui->user, anomaly::sdk::StringView("搜索载具名称或编号"),
                filter_buffer.data(), filter_buffer.size(), 0U)) {
            std::string updated_filter(filter_buffer.data());
            if (updated_filter != current_filter) {
                current_filter = std::move(updated_filter);
                std::scoped_lock lock(g_context.mutex);
                g_context.catalog_filter = current_filter;
                current_page = 0U;
            }
        }
    }

    // 输入变化后立即重算匹配数量和分页；列表行在下一次绘制时同步更新。
    filtered = filter_ids(current_filter);
    page_count = filtered.empty()
        ? 1U : (filtered.size() + kVehiclesPerPage - 1U) / kVehiclesPerPage;
    if (current_page >= page_count) current_page = page_count - 1U;
    DrawText(ui, "匹配 " + std::to_string(filtered.size()) + " 项 / 共 " +
        std::to_string(ids.size()) + " 辆");
    if (DrawButtonEnabled(ui, "上一页", current_page > 0U)) --current_page;
    DrawInlineGap(ui);
    if (DrawButtonEnabled(ui, "下一页", current_page + 1U < page_count)) ++current_page;
    DrawText(ui, "第 " + std::to_string(current_page + 1U) + " / " +
        std::to_string(page_count) + " 页");

    const bool can_summon = player_position_valid && has_selected;
    if (DrawButtonEnabled(ui, "召唤", can_summon)) {
        g_context.summon.store(true, std::memory_order_release);
    }
    DrawInlineGap(ui);
    if (DrawButtonEnabled(ui, "改造清单", has_selected)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_page = true;
            g_context.customization_vehicle_id.clear();
            g_context.customization_options.clear();
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = "正在读取 " + selected + " 的实时改装目录";
        }
        g_context.refresh_customization_requested.store(true, std::memory_order_release);
    }
    DrawText(ui, "改造清单会读取所选车型实际可用的模块与涂装；未手动覆盖的模块槽位保留游戏默认配置。");
}

void DrawCustomizationTab(
    const AnomalyUiServiceV1* ui,
    const std::string& selected,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& labels,
    const std::vector<CustomizationOption>& options,
    const std::vector<std::pair<std::uint32_t, std::string>>& module_overrides,
    const std::string& customization_vehicle_id,
    const std::string& skin_id,
    const std::string& customization_status,
    bool player_position_valid) {
    const auto selected_index = std::ranges::find(ids, selected);
    const bool has_selected = selected_index != ids.end();
    std::string selected_label = selected;
    if (has_selected) {
        const auto index = static_cast<std::size_t>(selected_index - ids.begin());
        if (index < labels.size() && !labels[index].empty()) selected_label = labels[index];
    }
    DrawText(ui, "改造清单 / " + selected_label);
    if (DrawButtonEnabled(ui, "返回召唤清单", true)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.customization_page = false;
        return;
    }
    DrawInlineGap(ui);
    if (DrawButtonEnabled(ui, "重新读取改装数据", true)) {
        {
            std::scoped_lock lock(g_context.mutex);
            g_context.customization_vehicle_id.clear();
            g_context.customization_options.clear();
            g_context.customization_skin_id.clear();
            g_context.selected_module_overrides.clear();
            g_context.customization_status = "正在重新读取所选车型的改装目录";
        }
        g_context.refresh_customization_requested.store(true, std::memory_order_release);
    }
    DrawText(ui, customization_status);
    if (!has_selected) {
        DrawText(ui, "当前车型不在已读取的载具目录中，无法应用改装。");
        return;
    }
    if (customization_vehicle_id != selected) {
        DrawText(ui, "正在读取当前车型的改装选项；请等待数据加载完成后再召唤。");
        return;
    }
    if (options.empty()) {
        DrawText(ui, "没有取得可用的改装选项。请重新读取；若仍失败，请查看 Core 返回的目录校验诊断。");
        return;
    }

    static constexpr std::array<std::string_view, 27> kModuleTypeNames{
        "无", "发动机", "变速箱", "制动", "悬挂", "车身颜色模块",
        "前包围", "侧裙", "后包围", "尾翼", "贴花", "轮毂",
        "引擎盖", "车牌", "车顶", "轮胎", "附件", "限滑差速器 LSD",
        "车身刚度", "Skin 模块", "内饰皮革颜色", "内饰缝线颜色",
        "车辆铭牌", "轮胎烟雾", "轮胎痕迹", "拖钩", "ECU"
    };
    const auto override_for_type = [&](std::uint32_t module_type) -> std::string_view {
        const auto found = std::ranges::find_if(module_overrides,
            [module_type](const auto& entry) { return entry.first == module_type; });
        return found == module_overrides.end()
            ? std::string_view{} : std::string_view(found->second);
    };
    const auto draw_module_options = [&]() {
        for (std::uint32_t type = 1; type < kModuleTypeNames.size(); ++type) {
            bool has_options = false;
            bool has_default = false;
            for (const auto& option : options) {
                if (option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE &&
                    option.module_type == type) {
                    has_options = true;
                    has_default = has_default || option.is_default;
                }
            }
            if (!has_options) continue;
            DrawDivider(ui);
            DrawText(ui, std::string(kModuleTypeNames[type]));
            const std::string_view selected_option_id = override_for_type(type);
            for (const auto& option : options) {
                if (option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_MODULE ||
                    option.module_type != type) continue;
                const bool explicit_selected = !selected_option_id.empty() &&
                    selected_option_id == option.id;
                const bool default_selected = selected_option_id.empty() && option.is_default;
                std::string button_label;
                if (explicit_selected) button_label = "[已选] ";
                else if (default_selected) button_label = "[默认] ";
                else button_label = "选择 ";
                button_label += option.label.empty() ? option.id : option.label;
                button_label += " (" + option.id + ")##custom-module-" +
                    std::to_string(type) + "-" + option.id;
                if (ui->button(ui->user, anomaly::sdk::StringView(button_label), 0.0F, 0.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    auto& overrides = g_context.selected_module_overrides;
                    if (option.is_default) {
                        std::erase_if(overrides, [type](const auto& entry) {
                            return entry.first == type;
                        });
                    } else {
                        const auto found = std::ranges::find_if(overrides,
                            [type](const auto& entry) { return entry.first == type; });
                        if (found == overrides.end()) overrides.emplace_back(type, option.id);
                        else found->second = option.id;
                    }
                    g_context.customization_status = "已更新 " +
                        std::string(kModuleTypeNames[type]) + " 的选择；召唤时将提交给游戏原生改装组件";
                }
            }
            if (has_default && !selected_option_id.empty()) {
                const std::string reset_label =
                    "恢复默认##custom-reset-module-" + std::to_string(type);
                if (DrawButtonEnabled(ui, reset_label, true)) {
                    std::scoped_lock lock(g_context.mutex);
                    std::erase_if(g_context.selected_module_overrides, [type](const auto& entry) {
                        return entry.first == type;
                    });
                    g_context.customization_status = "已恢复 " +
                        std::string(kModuleTypeNames[type]) + " 的游戏默认模块";
                }
            }
        }
    };
    const auto draw_skin_options = [&]() {
        DrawDivider(ui);
        DrawText(ui, "涂装");
        bool has_default_skin = false;
        for (const auto& option : options) {
            if (option.kind != ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN) continue;
            has_default_skin = has_default_skin || option.is_default;
            const bool current = skin_id == option.id;
            std::string button_label = current ? "[当前] " : (option.is_default ? "[默认] " : "选择 ");
            button_label += option.label.empty() ? option.id : option.label;
            button_label += " (" + option.id + ")##custom-skin-" + option.id;
            if (ui->button(ui->user, anomaly::sdk::StringView(button_label), 0.0F, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.customization_skin_id = option.id;
                g_context.customization_status = "已选择涂装：" + option.label + " (" + option.id + ")";
            }
        }
        if (has_default_skin && !skin_id.empty()) {
            if (DrawButtonEnabled(ui, "恢复默认涂装##custom-reset-skin", true)) {
                const auto found = std::ranges::find_if(options, [](const CustomizationOption& option) {
                    return option.kind == ANOMALY_NTE_VEHICLE_CUSTOMIZATION_KIND_V1_SKIN &&
                        option.is_default;
                });
                if (found != options.end()) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.customization_skin_id = found->id;
                    g_context.customization_status = "已恢复游戏默认涂装：" + found->label;
                }
            }
        }
    };

    const bool child_api_available =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
            sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
        ui->begin_child && ui->end_child;
    if (child_api_available) {
        const bool child_visible = ui->begin_child(ui->user,
            anomaly::sdk::StringView("##vehicle-customization-options"), 0.0F, 350.0F, 0U) != 0;
        if (child_visible) {
            draw_skin_options();
            draw_module_options();
        }
        ui->end_child(ui->user);
    } else {
        draw_skin_options();
        draw_module_options();
    }

    DrawDivider(ui);
    DrawText(ui, "仅手动选择的模块槽位会覆盖默认值；其他槽位使用游戏数据表的默认模块。");
    const bool can_summon_customized = player_position_valid && has_selected &&
        customization_vehicle_id == selected && !skin_id.empty();
    if (DrawButtonEnabled(ui, "召唤改装车辆", can_summon_customized)) {
        g_context.summon_customized.store(true, std::memory_order_release);
    }
}


void DrawPerformanceTab(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    float ratio, bool debug_unlocked, bool fine_speed_tuning_enabled,
    float torque_ratio, float other_speed_ratio, float other_speed_floor_ratio, float other_torque_ratio) {
    if ((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) == 0) {
        DrawText(ui, "当前驾驶载具：未识别");
    } else {
        DrawText(ui, "当前驾驶载具 / " +
            (snap.vehicle_class_name.empty() ? std::string("未知类名") : snap.vehicle_class_name) +
            " (" + (snap.vehicle_entity_id != 0
                ? "#" + std::to_string(snap.vehicle_entity_id) : std::string("#编号未知")) + ")");
        const double display_speed = std::isfinite(snap.speed_kmh)
            ? std::floor(std::fabs(snap.speed_kmh)) : 0.0;
        DrawText(ui, "当前速度  /  " + std::to_string(static_cast<std::uint64_t>(display_speed)) + " km/h");
        DrawText(ui, "当前最高速倍率 / " +
        std::to_string(snap.top_speed_ratio) + "x");
    }

    DrawDivider(ui);
    const bool fine_tuning_active = debug_unlocked && fine_speed_tuning_enabled;
    float slider_ratio = fine_tuning_active
        ? std::clamp(ratio, kFineSpeedRatioMin, kFineSpeedRatioMax)
        : kLockedSpeedRatio;
    bool slider_changed = false;
    const bool has_disabled_slider =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, slider_float_enabled),
            sizeof(decltype(AnomalyUiServiceV1::slider_float_enabled))) &&
        ui->slider_float_enabled != nullptr;
    if (has_disabled_slider) {
        slider_changed = ui->slider_float_enabled(ui->user,
            anomaly::sdk::StringView("速度倍率"), &slider_ratio,
            kFineSpeedRatioMin, kFineSpeedRatioMax,
            fine_tuning_active ? 1 : 0) != 0;
    } else {
        // Older hosts lack a disabled-slider primitive; use a temporary value and never persist edits while locked.
        slider_changed = ui->slider_float(ui->user,
            anomaly::sdk::StringView("速度倍率"), &slider_ratio,
            kFineSpeedRatioMin, kFineSpeedRatioMax) != 0;
    }
    if (fine_tuning_active && slider_changed) {
        slider_ratio = std::clamp(slider_ratio, kFineSpeedRatioMin, kFineSpeedRatioMax);
        std::scoped_lock lock(g_context.mutex);
        if (g_context.debug_unlocked && g_context.fine_speed_tuning_enabled)
            g_context.speed_ratio = slider_ratio;
    } else if (!fine_tuning_active) {
        std::scoped_lock lock(g_context.mutex);
        g_context.speed_ratio = kLockedSpeedRatio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用车速倍率"), 0.0F, 0.0F)) {
        // Actual application path independently enforces the same policy in Update().
        g_context.apply_speed.store(true, std::memory_order_release);
    }

    DrawDivider(ui);
    DrawText(ui, "发动机外部扭矩倍率");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView("发动机扭矩倍率"),
                         &torque_ratio, 0.05F, 20.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.engine_torque_ratio = torque_ratio;
    }
    if (ui->button(ui->user, anomaly::sdk::StringView("应用扭矩倍率"), 0.0F, 0.0F)) {
        g_context.apply_torque.store(true, std::memory_order_release);
    }
    DrawDivider(ui);
    DrawDivider(ui);
    DrawText(ui, "其他载具实例");
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
    if (ui->button(ui->user, anomaly::sdk::StringView("恢复默认设置  /  固定车速倍率 10.0x"), 0.0F, 0.0F)) {
        g_context.reset.store(true, std::memory_order_release);
    }
}

void DrawChassisTab(const AnomalyUiServiceV1* ui, bool friction) {
    DrawText(ui, "车轮摩擦");
    DrawInlineGap(ui);
    if (ui->button(ui->user, anomaly::sdk::StringView(friction ? "关闭" : "开启"), 0.0F, 0.0F)) {
        g_context.friction_toggle.store(true, std::memory_order_release);
    }
}

void DrawDebugPanel(
    const AnomalyUiServiceV1* ui, const Snapshot& snap,
    const std::string& status, const std::string& selected,
    std::size_t catalog_size, bool player_position_valid,
    bool fine_speed_tuning_enabled) {
    DrawText(ui, "构建标记：NTE Vehicle 0.10.0 / 2026-10-20-selected-modification-summon");
    DrawText(ui, "构建标记：manifest schema v2 / Plugin API v1 / Vehicle service v7");
    DrawText(ui, "Plugin ID：anomaly.local." + std::string("nte-vehicle"));
    DrawText(ui, "Manifest：schemaVersion=2 / entry=plugin.dll / loadPhase=game-ready / games=nte / builds=nte-*");
    DrawText(ui, "Required services：anomaly.ui v1 / anomaly.nte.vehicle v7 / anomaly.nte.player v1");
    DrawText(ui, "Capabilities：ui / nte-vehicle / nte-player-snapshot / per-vehicle-customization-catalog / selected-modification-summon");
    DrawText(ui, "创建日期：2026-10-14");
    DrawText(ui, "修改状态：已包含车型实时改装目录、模块/涂装选择与自定义召唤链路；游戏内结果待实测");
    DrawText(ui, "Host 状态：" + (status.empty() ? std::string("暂无状态消息") : status));
    // An empty diagnostic string means the Host snapshot callback returned OK.
    // Do not label this success path as "Host 未提供快照状态"; use the validity flag
    // to distinguish a valid current vehicle from a successful-but-empty snapshot.
    const std::string snapshot_status = !snap.vehicle_snapshot_status.empty()
        ? snap.vehicle_snapshot_status
        : (((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0)
            ? std::string("Host 快照正常：已识别当前驾驶载具")
            : std::string("Host 快照调用成功，但当前未识别驾驶载具"));
    DrawText(ui, "载具快照状态：" + snapshot_status);
    DrawText(ui, "驾驶载具对象：" +
        (((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0)
            ? (snap.vehicle_class_name.empty() ? std::string("已识别，类名未知") : snap.vehicle_class_name)
            : std::string("未识别")));
    DrawText(ui, "驾驶载具 Entity ID：" + (snap.vehicle_entity_id != 0
        ? std::to_string(snap.vehicle_entity_id) : std::string("不可用")));
    DrawText(ui, "VehicleID：" + (selected.empty() ? std::string("不可用") : selected));
    DrawText(ui, "TopSpeedRatio：" +
        (((snap.flags & ANOMALY_NTE_VEHICLE_V1_VALID) != 0)
            ? std::to_string(snap.top_speed_ratio) : std::string("不可用")));
    DrawText(ui, "玩家位置快照：" + std::string(player_position_valid ? "VALID" : "INVALID"));
    DrawText(ui, "载具目录：" + std::to_string(catalog_size) + " 项；当前选项：" +
        (selected.empty() ? std::string("无") : selected));

    DrawDivider(ui);
    DrawText(ui, "车速精细调整（0.50x–10.0x）");
    const bool has_checkbox =
        UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, checkbox),
            sizeof(decltype(AnomalyUiServiceV1::checkbox))) && ui->checkbox;
    int fine_enabled = fine_speed_tuning_enabled ? 1 : 0;
    if (has_checkbox) {
        if (ui->checkbox(ui->user,
                anomaly::sdk::StringView("启用车速精细倍率调整"), &fine_enabled) != 0) {
            const bool enable_requested = fine_enabled != 0;
            const bool has_core_switch =
                HasField<AnomalyNteVehicleServiceV1,
                    decltype(AnomalyNteVehicleServiceV1::set_fine_speed_tuning_enabled)>(
                        g_context.vehicle,
                        offsetof(AnomalyNteVehicleServiceV1, set_fine_speed_tuning_enabled)) &&
                g_context.vehicle->set_fine_speed_tuning_enabled != nullptr;
            AnomalyStatusV1 core_status = Status(ANOMALY_STATUS_V1_UNAVAILABLE);
            if (has_core_switch) {
                core_status = g_context.vehicle->set_fine_speed_tuning_enabled(
                    g_context.vehicle->user, enable_requested ? 1U : 0U);
            }
            const bool enable = enable_requested &&
                has_core_switch && core_status.code == ANOMALY_STATUS_V1_OK;
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.fine_speed_tuning_enabled = enable;
                g_context.speed_ratio = enable
                    ? std::clamp(g_context.speed_ratio, kFineSpeedRatioMin, kFineSpeedRatioMax)
                    : kLockedSpeedRatio;
            }
            fine_speed_tuning_enabled = enable;
            if (enable) {
                SetStatus("Core 已启用 DEBUG 车速精细调整；倍率可在 0.50x–10.0x 内调整");
            } else {
                // 关闭开关后 Core 和插件均恢复锁定策略，随后实际应用固定 10x。
                g_context.apply_speed.store(true, std::memory_order_release);
                SetStatus(enable_requested
                    ? "Core 未授权精细调整；倍率保持固定 10.0x"
                    : "DEBUG 车速精细调整已关闭；正在恢复固定 10.0x");
                fine_enabled = 0;
            }
        }
        // 精简界面：移除重复的启用/锁定说明；锁定策略仍由控件及 Core 授权逻辑执行。
    } else {
        DrawText(ui, "当前 UI Host 不支持开关控件；精细调整保持关闭，倍率固定为 10.0x。");
    }
}

void Draw(const AnomalyUiServiceV1* ui) {
    if (!ui || !ui->begin_window || !ui->end_window || !ui->text ||
        !ui->button || !ui->slider_float) return;

    int open = 1;
    anomaly::sdk::UiWindow window(ui, "载具控制", &open, 0);
    if (!window) return;

    Snapshot snap;
    std::string status;
    std::string selected;
    std::string last_summon;
    std::vector<std::string> ids;
    std::vector<std::string> labels;
    std::vector<CustomizationOption> customization_options;
    std::vector<std::pair<std::uint32_t, std::string>> module_overrides;
    std::string customization_vehicle_id;
    std::string customization_skin_id;
    std::string customization_status;
    bool customization_page{};
    float ratio;
    float torque_ratio;
    float other_speed_ratio;
    float other_speed_floor_ratio;
    float other_torque_ratio;
    bool friction;
    bool player_position_valid{};
    bool debug_unlocked{};
    bool fine_speed_tuning_enabled{};
    {
        std::scoped_lock lock(g_context.mutex);
        snap = g_context.snapshot;
        status = g_context.status;
        selected = g_context.selected_vehicle_id;
        last_summon = g_context.last_summon;
        ids = g_context.vehicle_ids;
        labels = g_context.vehicle_labels;
        customization_options = g_context.customization_options;
        module_overrides = g_context.selected_module_overrides;
        customization_vehicle_id = g_context.customization_vehicle_id;
        customization_skin_id = g_context.customization_skin_id;
        customization_status = g_context.customization_status;
        customization_page = g_context.customization_page;
        ratio = g_context.speed_ratio;
        torque_ratio = g_context.engine_torque_ratio;
        other_speed_ratio = g_context.other_speed_ratio;
        other_speed_floor_ratio = g_context.other_speed_floor_ratio;
        other_torque_ratio = g_context.other_torque_ratio;
        friction = g_context.friction_enabled;
        player_position_valid = g_context.player_position_valid;
        debug_unlocked = g_context.debug_unlocked;
        fine_speed_tuning_enabled = g_context.fine_speed_tuning_enabled;
    }

    static int active_tab = 0;
    static std::array<char, 129> debug_key_buffer{};
    static std::string debug_unlock_feedback;
    if (!debug_unlocked && active_tab == 3) active_tab = 0;

    const auto nav_button = [&](std::string_view label, int tab) {
        const std::string shown = active_tab == tab
            ? "[" + std::string(label) + "]" : std::string(label);
        if (ui->button(ui->user, anomaly::sdk::StringView(shown), 0.0F, 0.0F))
            active_tab = tab;
        DrawInlineGap(ui);
    };
    nav_button("召唤载具", 0);
    nav_button("性能调校", 1);
    nav_button("车轮摩擦", 2);

    if (UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, button_enabled),
            sizeof(decltype(AnomalyUiServiceV1::button_enabled))) && ui->button_enabled) {
        if (ui->button_enabled(ui->user, anomaly::sdk::StringView("Debug"),
                0.0F, 0.0F, debug_unlocked ? 1 : 0) != 0 && debug_unlocked) {
            active_tab = 3;
        }
    } else {
        const std::string debug_label = debug_unlocked ? "Debug" : "Debug（锁定）";
        if (ui->button(ui->user, anomaly::sdk::StringView(debug_label), 0.0F, 0.0F) && debug_unlocked)
            active_tab = 3;
    }
    DrawInlineGap(ui);
    if (ui->button(ui->user, anomaly::sdk::StringView("刷新载具列表"), 0.0F, 0.0F)) {
        g_context.refresh_catalog_requested.store(true, std::memory_order_release);
        SetStatus("已请求重新整理载具目录");
    }
    DrawDivider(ui);

    if (!debug_unlocked) {
        const bool has_input_text =
            UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, input_text),
                sizeof(decltype(AnomalyUiServiceV1::input_text))) && ui->input_text;
        if (has_input_text) {
            DrawText(ui, "Debug 未解锁");
            ui->input_text(ui->user, anomaly::sdk::StringView("输入 Debug 密钥"),
                debug_key_buffer.data(), debug_key_buffer.size(), ANOMALY_UI_TEXT_INPUT_V1_PASSWORD);
            DrawInlineGap(ui);
            if (ui->button(ui->user, anomaly::sdk::StringView("解锁 Debug"), 0.0F, 0.0F)) {
                // Own the entered text for the complete local/Core validation call. The UI
                // buffer is cleared only after the Core callback returns, and the same
                // byte range is passed to both checks.
                const std::string candidate_owned(debug_key_buffer.data());
                const bool valid = IsDebugKeyValid(candidate_owned);
                bool core_key_accepted = false;
                std::uint32_t core_status_code = ANOMALY_STATUS_V1_FAILED;
                std::string core_rejection_message;
                const bool has_core_authorizer =
                    HasField<AnomalyNteVehicleServiceV1,
                        decltype(AnomalyNteVehicleServiceV1::authorize_fine_speed_tuning)>(
                            g_context.vehicle,
                            offsetof(AnomalyNteVehicleServiceV1, authorize_fine_speed_tuning)) &&
                    g_context.vehicle->authorize_fine_speed_tuning != nullptr;
                if (valid && has_core_authorizer) {
                    const auto core_status = g_context.vehicle->authorize_fine_speed_tuning(
                        g_context.vehicle->user,
                        anomaly::sdk::StringView(std::string_view(
                            candidate_owned.data(), candidate_owned.size())));
                    core_status_code = core_status.code;
                    core_key_accepted = core_status.code == ANOMALY_STATUS_V1_OK;
                    if (!core_key_accepted && core_status.message.data != nullptr &&
                        core_status.message.size != 0) {
                        core_rejection_message.assign(
                            core_status.message.data, core_status.message.size);
                    }
                }
                std::fill(debug_key_buffer.begin(), debug_key_buffer.end(), '\0');
                if (valid && core_key_accepted) {
                    {
                        std::scoped_lock lock(g_context.mutex);
                        g_context.debug_unlocked = true;
                        // Both local and Core fine-tuning switches remain off until the checkbox is enabled.
                        g_context.fine_speed_tuning_enabled = false;
                        g_context.speed_ratio = kLockedSpeedRatio;
                    }
                    debug_unlocked = true;
                    fine_speed_tuning_enabled = false;
                    ratio = kLockedSpeedRatio;
                    debug_unlock_feedback = "Core 密钥验证通过；精细车速调整仍默认关闭，可在 Debug 中单独启用";
                } else if (valid && !has_core_authorizer) {
                    debug_unlock_feedback = "当前 Core 缺少 V5 授权接口；精细倍率保持锁定，请更新 Runtime";
                } else if (valid) {
                    debug_unlock_feedback = core_rejection_message.empty()
                        ? ("Core 拒绝密钥（状态码 " + std::to_string(core_status_code) +
                            "）；精细倍率保持锁定")
                        : ("Core 拒绝密钥：" + core_rejection_message +
                            "；精细倍率保持锁定");
                } else {
                    debug_unlock_feedback = "密钥无效";
                }
            }
        } else {
            DrawText(ui, "当前 UI Host 未提供密钥输入接口，DEBUG 保持锁定。");
        }
        if (!debug_unlock_feedback.empty()) DrawText(ui, debug_unlock_feedback);
        DrawDivider(ui);
    }

    if (active_tab == 0) {
        if (customization_page) {
            DrawCustomizationTab(ui, selected, ids, labels, customization_options,
                module_overrides, customization_vehicle_id, customization_skin_id,
                customization_status, player_position_valid);
        } else {
            DrawSummonTab(ui, selected, ids, labels, last_summon, player_position_valid);
        }
    } else if (active_tab == 1) {
        DrawPerformanceTab(ui, snap, ratio, debug_unlocked, fine_speed_tuning_enabled,
            torque_ratio, other_speed_ratio, other_speed_floor_ratio, other_torque_ratio);
    } else if (active_tab == 2) {
        DrawChassisTab(ui, friction);
    } else if (active_tab == 3 && debug_unlocked) {
        DrawDebugPanel(ui, snap, status, selected, ids.size(), player_position_valid,
            fine_speed_tuning_enabled);
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
            decltype(AnomalyNteVehicleServiceV1::authorize_fine_speed_tuning)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, authorize_fine_speed_tuning)) ||
        !vehicle->authorize_fine_speed_tuning ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::set_fine_speed_tuning_enabled)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, set_fine_speed_tuning_enabled)) ||
        !vehicle->set_fine_speed_tuning_enabled ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::revoke_fine_speed_authorization)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, revoke_fine_speed_authorization)) ||
        !vehicle->revoke_fine_speed_authorization ||
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
        !vehicle->set_other_vehicles_engine_torque_ratio ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_catalog_snapshot)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_catalog_snapshot)) ||
        !vehicle->customization_catalog_snapshot ||
        !HasField<AnomalyNteVehicleServiceV1,
            decltype(AnomalyNteVehicleServiceV1::customization_option_at)>(
                vehicle, offsetof(AnomalyNteVehicleServiceV1, customization_option_at)) ||
        !vehicle->customization_option_at || !player->snapshot) {
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
    g_context.summon_customized.store(false, std::memory_order_release);
    g_context.friction_toggle.store(false, std::memory_order_release);
    g_context.refresh_catalog_requested.store(false, std::memory_order_release);
    g_context.refresh_customization_requested.store(false, std::memory_order_release);
    g_context.speed_ratio = kLockedSpeedRatio;
    g_context.engine_torque_ratio = 1.0F;
    g_context.other_speed_ratio = 1.0F;
    g_context.other_speed_floor_ratio = 0.6024F;
    g_context.other_torque_ratio = 1.0F;
    g_context.friction_enabled = true;
    g_context.debug_unlocked = false;
    g_context.fine_speed_tuning_enabled = false;
    g_context.snapshot = {};
    g_context.selected_vehicle_id = "Vehicle007";
    g_context.pending_vehicle_id.clear();
    g_context.catalog_filter.clear();
    g_context.vehicle_ids.clear();
    g_context.vehicle_labels.clear();
    g_context.customization_page = false;
    g_context.customization_vehicle_id.clear();
    g_context.customization_skin_id.clear();
    g_context.customization_status = "尚未读取当前车型的改装数据";
    g_context.customization_options.clear();
    g_context.selected_module_overrides.clear();
    g_context.customization_catalog_tick = 0;
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
    // Each plugin start re-applies the locked 10x policy until DEBUG + fine-tune switch are both enabled.
    g_context.apply_speed.store(true, std::memory_order_release);
    g_context.started.store(true, std::memory_order_release);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    if (plugin_context != &g_context) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.started.store(false, std::memory_order_release);
    if (g_context.vehicle) {
        if (g_context.vehicle->set_fine_speed_tuning_enabled)
            static_cast<void>(g_context.vehicle->set_fine_speed_tuning_enabled(
                g_context.vehicle->user, 0U));
        if (g_context.vehicle->revoke_fine_speed_authorization)
            static_cast<void>(g_context.vehicle->revoke_fine_speed_authorization(
                g_context.vehicle->user));
    }
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.debug_unlocked = false;
        g_context.fine_speed_tuning_enabled = false;
        g_context.speed_ratio = kLockedSpeedRatio;
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    if (plugin_context != &g_context) return;
    g_context.started.store(false, std::memory_order_release);
    if (g_context.vehicle) {
        if (g_context.vehicle->set_fine_speed_tuning_enabled)
            static_cast<void>(g_context.vehicle->set_fine_speed_tuning_enabled(
                g_context.vehicle->user, 0U));
        if (g_context.vehicle->revoke_fine_speed_authorization)
            static_cast<void>(g_context.vehicle->revoke_fine_speed_authorization(
                g_context.vehicle->user));
    }
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
        anomaly::sdk::StringView("载具控制"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.10.0"), Load, Start, Stop, Unload, UpdateCallback, DrawCallback};
    return anomaly::sdk::Ok();
}
