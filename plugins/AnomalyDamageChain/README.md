# Damage Chain Reaction（伤害链式反应）

玩家操控任意角色命中任意怪物后，对**同一次伤害**连续重复判定 N 次（可调），
形成「打一下、连续掉血多次」的链式反应。

## 关于 anomaly.nte.combat 的更正

先前版本 README 曾断言 `anomaly.nte.combat` 不可用，**那是错的**。核对了框架运行日志的
`profile.ready` 记录，实际情况是：

```json
{"id":"nte.player","available":false,"diagnostics":["nte-player-layout-v1: world pointer is null"]}
{"id":"nte.combat","available":false,"unavailableDependencies":["nte.player"]}
diagnostics: [..., "service anomaly.nte.player initially unavailable: nte.player feature unavailable"]
```

`nte.combat` 不可用**只是因为启动时还在主菜单、`nte.player` 的世界指针为空**（诊断措辞是
"initially"）。进入世界后 `nte.player` / `nte.combat` 会就绪——这也是 NTE Combat Demo 能用的原因。

因此本插件把 `anomaly.nte.combat` 作为**可选服务**接回来了，用于：

- 界面显示 `战斗流=就绪/未就绪` 与 `战斗事件=N`（确认框架伤害流真的在跑）；
- 与 process-event 的命中数互相印证（两条独立信号）。

**但重放仍然走 `ue5.process-event`**：`nte.combat` 只给事件数据（attacker/victim/伤害值），
不提供可重放的调用入口；要"再判定一次"，必须拿到 `(object, function, parameters)`。

## 为什么重放走 ProcessEvent

本版游戏里 `nte.player` / `nte.attack-input` / `nte.skill-invocation` / `nte.skills` 在主菜单时
均不可用，且 `nte.combat` 只读事件、无重放入口。唯一能拿到"可再次调用目标"的通道是
`anomaly.ue5.process-event`（钩 `UObject::ProcessEvent`，给出 object/function/parameters）。

## 机制

```
ProcessEvent(object, function, parameters)
  → 读 function + 0x18 得 FName index（缓存，dump 证据 kNamePrivateOffset=0x18）
  → names.resolve_utf8(index) 得函数名
  → 命中候选伤害名 且 未在重放 且 无活动链
      → 读 UFunction::ParmsSize（+0xB6 = 182）复制参数区
      → 每 Game tick 重放一次：vtable[0x4C]（=76，INDEX_PROCESSEVENT）调 ProcessEvent
      → replaying 守卫防递归；链跑完自动结束
```

默认候选伤害函数名（任一命中即触发）：

```
OnDamaged, ReceiveDamage, TakeDamage, ApplyDamage, BPOnTakeDamage,
ReceiveOnTakeDamage, OnCharacterAttacked, OnCharacterAttackedCallBack,
OnHandleDamageInfo, NetMulticast_OnHandleDamageInfo,
ClientOnSendHandleDamageInfoToDamaged, CharacterOnDamaged,
K2_ApplyDamage, ApplyDamageToTarget
```

## 界面

```
[√] 启用
重复次数：N   [-] [+]
Hook=就绪  事件=N 命中=N 重放=N 失败=N
匹配名=x/14  参数拒绝=N
<状态行>
------ 捕获诊断（列出实战函数名） ------
[√] 捕获诊断（列出实战函数名）
[导出捕获日志]
函数名1  (次数)
函数名2  (次数)
...
```

- `事件`：经过 ProcessEvent 的调用总数（证明钩子在工作）。
- `命中`：命中候选伤害名的次数。
- `重放`：实际重放成功次数。
- `失败`：vtable/参数失效导致的失败。
- **捕获诊断**：实时列出实战中经过 ProcessEvent 的函数名与出现次数（按次数降序）。
  如果默认候选名没命中，打一次怪后看这个列表，就能找到真实的伤害函数名。

## 构建与部署

CI 已接入：`.github/workflows/windows.yml` 会构建 `AnomalyDamageChain` 目标，产物为
`out/AnomalyDamageChain/plugin.dll`，并与 `manifest.json` 一起暂存进
`.build/ci-artifacts/runtime/Anomaly/plugins/AnomalyDamageChain/`。

手动部署：把 `plugin.dll` + `manifest.json` 放到
`<游戏目录>\Anomaly\plugins\AnomalyDamageChain\`（与 `AnomalyUltimate-v1.2` 同级）。
插件默认 `loadPhase=game-ready`，进游戏后按 `K` 或用界面开关启用。

## 调参建议

- `重复次数` 先设 `3~5`，确认有效果再加大；过大可能触发服务器校验或让怪物瞬间死亡。
- 链是**逐 tick 错开**的，不会在同一帧堆出多个伤害数字。
- 若 `命中>0` 但血量不变：说明命中的是「通知型」函数（如 `OnDamaged`），
  需要换真正的结算入口——把捕获列表发给我，我改 `kDefaultDamageNames` 即可。
