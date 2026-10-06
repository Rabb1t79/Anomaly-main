/*
 * 中文维护说明：本插件
 * - 本文件是该插件的主要实现入口，后续维护时优先在这里说明新增、修改和删除的行为。
 * - 当前代码逻辑保持不变；本次仅补充中文维护注释，便于后续逆向、排错和功能回溯。
 * - 不把未经验证的猜测写成实现依据；涉及游戏调用、偏移、签名或 ABI 时应注明实际证据来源。
 */
// Movement-hold probe: an on-screen panel that flips the Host's character hold, so the pin the
// teleport arrival window uses can be exercised on a live client without going through a
// teleport and without claiming a keyboard key. The panel reports the live gravity scale, velocity
// and movement mode; the mode is read-only there, because the game rewrites it every frame and the
// hold does not try to own it any more.

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/ui.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string_view>

namespace {

struct Context final {
  const AnomalyHostApiV1 *host{};
  const AnomalyCoreServiceV1 *core{};
  const AnomalyNtePlayerServiceV1 *player{};
  AnomalyNtePlayerHoldSnapshotV1 snapshot{sizeof(AnomalyNtePlayerHoldSnapshotV1)};
  // What the panel last asked for. The snapshot stays the truth: a render-thread request is
  // queued for the Game tick, so the call that made it cannot report its own outcome.
  bool requested_hold{};
  bool snapshot_valid{};
  double report_accumulator{};
};

// Held 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool Held(const Context &context) noexcept {
  return context.snapshot_valid &&
         (context.snapshot.flags & ANOMALY_NTE_PLAYER_HOLD_V1_HELD) != 0;
}

// Refused 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool Refused(const Context &context) noexcept {
  return context.snapshot_valid &&
         (context.snapshot.flags & ANOMALY_NTE_PLAYER_HOLD_V1_REFUSED) != 0;
}

AnomalyStatusV1 Status(const std::uint32_t code,
                       const std::string_view message = {}) noexcept {
  return {code, 0, {message.data(), message.size()}};
}

template <typename Service>
const Service *Query(const AnomalyHostApiV1 *host, const char *id,
                     const std::uint32_t version) noexcept {
  return anomaly::sdk::Host(host).Query<Service>(id, version).get();
}

void Log(Context &context, const std::uint32_t level,
         const std::string_view message) noexcept {
  if (context.core != nullptr && context.core->log != nullptr)
    context.core->log(context.core->user, level,
                      anomaly::sdk::StringView(message));
}

// The hold entry points live on the player service, so they are present only when the Host
// published a struct_size that covers them.
// HoldReady 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
bool HoldReady(const AnomalyNtePlayerServiceV1 *service) noexcept {
  return service != nullptr &&
         service->struct_size >=
             offsetof(AnomalyNtePlayerServiceV1, hold_snapshot) +
                 sizeof(service->hold_snapshot) &&
         service->hold_engage != nullptr && service->hold_release != nullptr &&
         service->hold_snapshot != nullptr;
}

// ResolvePlayer 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ResolvePlayer(Context &context) noexcept {
  if (HoldReady(context.player)) return;
  context.player = Query<AnomalyNtePlayerServiceV1>(
      context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
      ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
}

// CaptureSnapshot 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void CaptureSnapshot(Context &context) noexcept {
  context.snapshot_valid = false;
  if (!HoldReady(context.player)) return;
  AnomalyNtePlayerHoldSnapshotV1 snapshot{sizeof(snapshot)};
  if (context.player->hold_snapshot(context.player->user, &snapshot).code !=
      ANOMALY_STATUS_V1_OK) {
    return;
  }
  context.snapshot = snapshot;
  context.snapshot_valid = true;
}

// Toggle 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void Toggle(Context &context) noexcept {
  ResolvePlayer(context);
  if (!HoldReady(context.player)) {
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        "movement hold refused: the player hold entry points are unavailable");
    return;
  }
  const AnomalyStatusV1 status =
      context.requested_hold
          ? context.player->hold_release(context.player->user)
          : context.player->hold_engage(context.player->user);
  if (status.code != ANOMALY_STATUS_V1_OK) {
    char detail[192]{};
    std::snprintf(detail, sizeof(detail), "movement hold refused: %.*s",
                  static_cast<int>(status.message.size),
                  status.message.data == nullptr ? "" : status.message.data);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, detail);
    return;
  }
  context.requested_hold = !context.requested_hold;
  Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
      context.requested_hold ? "movement hold requested"
                             : "movement hold release requested");
  CaptureSnapshot(context);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1 *host,
                                  void **plugin_context) {
  if (host == nullptr || plugin_context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *plugin_context = nullptr;
  auto *context = new (std::nothrow) Context();
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);
  context->host = host;
  context->core = Query<AnomalyCoreServiceV1>(
      host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
  if (context->core == nullptr) {
    delete context;
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "the core service is unavailable");
  }
  // The player service is published by the game bridge, which only comes up once a player
  // exists. Reporting UNAVAILABLE here would park the plugin in the manager's
  // waiting-for-service state for the rest of the session, so the panel loads and picks the
  // service up on its own.
  ResolvePlayer(*context);
  *plugin_context = context;
  if (!HoldReady(context->player)) {
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "movement hold panel loaded: the player hold entry points are not available yet");
  }
  return anomaly::sdk::Ok();
}

// 建立 AttackReplay 的运行状态并启动事件/输入轮询，使后续 Update 可以捕获并重放攻击；重复启动不会重新创建已经存在的状态。
AnomalyStatusV1 ANOMALY_CALL Start(void *plugin_context) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
      "movement hold panel ready: use the panel button to toggle the hold");
  return anomaly::sdk::Ok();
}

// 停止 AttackReplay 的事件捕获和重放状态，并清除待验证的攻击上下文，防止插件停止后继续消费战斗事件。
AnomalyStatusV1 ANOMALY_CALL Stop(void *plugin_context, std::uint32_t) {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  // A stop must never leave the character with frozen gravity behind.
  if (context->requested_hold && HoldReady(context->player)) {
    static_cast<void>(context->player->hold_release(context->player->user));
    context->requested_hold = false;
  }
  return anomaly::sdk::Ok();
}

// Unload 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ANOMALY_CALL Unload(void *plugin_context) {
  delete static_cast<Context *>(plugin_context);
}

// Update 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ANOMALY_CALL Update(void *plugin_context, const double delta_seconds) noexcept {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return;
  try {
    context->report_accumulator += delta_seconds;
    if (context->report_accumulator < 1.0) return;
    context->report_accumulator = 0.0;
    ResolvePlayer(*context);
    CaptureSnapshot(*context);
    if (!context->requested_hold && !Held(*context) && !Refused(*context)) return;
    char detail[224]{};
    std::snprintf(detail, sizeof(detail),
                  "movement hold held=%u refused=%u mode=%u gravity=%.3f "
                  "velocity=(%.2f, %.2f, %.2f)",
                  Held(*context) ? 1u : 0u, Refused(*context) ? 1u : 0u,
                  context->snapshot.movement_mode, context->snapshot.gravity_scale,
                  context->snapshot.velocity[0], context->snapshot.velocity[1],
                  context->snapshot.velocity[2]);
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, detail);
  } catch (...) {
  }
}

// Draw 根据函数体中的具体对象、服务和状态字段执行当前插件流程；返回值/状态字段用于把实际执行结果交给调用方。
void ANOMALY_CALL Draw(void *plugin_context, const AnomalyUiServiceV1 *ui) noexcept {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr || ui == nullptr) return;
  try {
    if (ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->button == nullptr || ui->text == nullptr ||
        ui->struct_size <
            offsetof(AnomalyUiServiceV1, end_window) + sizeof(ui->end_window)) {
      return;
    }
    if (ui->begin_window(ui->user,
                         anomaly::sdk::StringView("Movement Hold"),
                         nullptr, 0) != 0) {
      ResolvePlayer(*context);
      CaptureSnapshot(*context);
      if (context->snapshot_valid) {
        char detail[256]{};
        std::snprintf(detail, sizeof(detail),
                      "held=%u refused=%u mode=%u\ngravity=%.3f  velocity=(%.2f, %.2f, %.2f)",
                      Held(*context) ? 1u : 0u, Refused(*context) ? 1u : 0u,
                      context->snapshot.movement_mode, context->snapshot.gravity_scale,
                      context->snapshot.velocity[0], context->snapshot.velocity[1],
                      context->snapshot.velocity[2]);
        ui->text(ui->user, anomaly::sdk::StringView(detail));
        // Mode 3 is MOVE_Falling. The hold pins the character without owning its state, so the
        // mode is reported as it is rather than as a verdict: what matters for a teleport is
        // *where* the character was when the fall started, not which mode it is in.
        const char *state = "not held";
        if (Refused(*context)) {
          state = "the Host refused the hold (see the host log)";
        } else if (Held(*context)) {
          state = context->snapshot.movement_mode == 3
              ? "held; the game owns the mode and says MOVE_Falling"
              : "held; the game says the character is not falling";
        } else if (context->requested_hold) {
          state = "requested, not engaged yet";
        }
        ui->text(ui->user, anomaly::sdk::StringView(state));
      } else {
        ui->text(ui->user, anomaly::sdk::StringView(
                               "the player hold entry points are not available yet"));
      }
      const char *label = context->requested_hold ? "Release" : "Hold the character";
      if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0) {
        Toggle(*context);
      }
    }
    // The host accounts a window from the begin call itself, so the end call must follow it
    // even when the window was not visible this frame.
    ui->end_window(ui->user);
  } catch (...) {
  }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1 *descriptor) {
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
                 ANOMALY_PLUGIN_API_V1_MINOR,
                 anomaly::sdk::StringView("anomaly.local.nte.movement-hold-probe"),
                 anomaly::sdk::StringView("Movement Hold Probe"),
                 anomaly::sdk::StringView("Anomaly"),
                 anomaly::sdk::StringView("1.0.0"), Load, Start, Stop, Unload,
                 Update, Draw};
  return anomaly::sdk::Ok();
}
