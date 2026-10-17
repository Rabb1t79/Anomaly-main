# 伤害事件候选：`AHTAbilityCharacter::OnDamaged`

> 来源：同一仓库的 `5.6.1/` 与 LFS 转换前 `5.6.1-new/` Dump 正文。类段、参数文件及关联结构段在两版中逐字一致。本页识别候选事件，不宣称它是旧插件的实际路径。

## 1. 函数声明与反射元数据

`HTGame_classes.hpp` 声明：

`void OnDamaged(float DamageAmount, const FHitResult& HitInfo, const FGameplayTagContainer& DamageTags, const UHTGamePlayEffect* DamageGameplayEffect, AHTAbilityCharacter* InstigatorCharacter, AActor* DamageCauser);`

同源 `Dumpspace/FunctionsInfo.json` 将 owner 记为 `AHTAbilityCharacter`，flags 为 `Native|Event|Public|HasOutParams|BlueprintEvent`。

**直接证实：** 此函数声明携带伤害量、命中信息、伤害标签、伤害 GameplayEffect、施加者角色和伤害来源 Actor 参数。

**未知：** 它是否每次敌人受击都会触发、触发时点在扣血前还是后、是否覆盖所有可控角色/伤害类型，以及是否可由 Anomaly 已支持的回调机制安全订阅。

## 2. 参数布局

来源：`HTGame_parameters.hpp`，两版均为 blob `b839b1a326e90ad3ac1c598f6bbca6b6eee465e1`。

`HTAbilityCharacter_OnDamaged` 参数结构总大小 `0x148`：

| 参数 | 偏移 / 大小 | 注释 |
|---|---|---|
| `DamageAmount` | `0x000 / 0x004` | float 伤害量。 |
| padding | `0x004 / 0x004` | 对齐空间。 |
| `HitInfo` | `0x008 / 0x108` | `FHitResult`。 |
| `DamageTags` | `0x110 / 0x020` | `FGameplayTagContainer`。 |
| `DamageGameplayEffect` | `0x130 / 0x008` | `const UHTGamePlayEffect*`。 |
| `InstigatorCharacter` | `0x138 / 0x008` | `AHTAbilityCharacter*`。 |
| `DamageCauser` | `0x140 / 0x008` | `AActor*`。 |

这只是参数结构布局，不等于可直接调用的 ABI。桥接前仍需按 Anomaly 公开 ABI、反射验证和 Game 线程规则核验。

## 3. 关联结构与交叉证据

### `FHTDamageEvent`
- 继承 `FPointDamageEvent`，总大小注释为 `0x170`。
- `DamageGEDef` 为 `TWeakObjectPtr<UHTGamePlayEffect>`，偏移 `0x138`。
- `DamageTags` 为 `FGameplayTagContainer`，偏移 `0x148`、大小 `0x20`。
- 两版结构段逐字一致。

### `FHTGameplayEffectContext`
- 继承 `FGameplayEffectContext`。
- `Target` 为 `TWeakObjectPtr<AActor>`，偏移 `0x80`。
- 两版结构段逐字一致。字段存在不证明每次伤害都会带有可用上下文。

### `GObjects-Dump.txt`
- 对象清单中出现 `Function HTGame.HTAbilityCharacter.OnDamaged`、`ScriptStruct HTGame.DamageTriggerActorStaticData` 和 `Class HTGame.HTAbilitySpawnDamageTriggerActor`。
- 这对反射对象名称的存在提供第二份同源佐证；对象清单中的地址是该 Dump 快照的值，不能作为跨版本稳定 ABI 或硬编码调用地址。

## 4. 对复现路线的意义

- **有证据支持但未验证：** `AHTAbilityCharacter::OnDamaged` 比直接重放技能/输入更接近“观察原生伤害事件”的候选入口，因为参数携带伤害量、HitInfo、DamageTags、伤害效果、InstigatorCharacter 和 DamageCauser。
- **未知：** 是否足以识别有效的 player→enemy 伤害；是否在效果应用之后触发；回调内部继续触发伤害是否会递归；车辆/环境伤害如何区分。
- 不应推导“再次调用 OnDamaged 就能造成伤害”。目前只证实事件声明和反射包装，不是伤害计算实现。
- `UAbilityTask_JumpSection::OnAttackedCallback` / `OnDamagedCallback` 的 owner 是能力任务类，不应因名称相似而当作通用伤害入口。

## 5. 当前判断

该事件值得保留为候选，但在确认真实调用时序、owner、receiver、可用 ABI 和实际伤害结果之前，不选定实现。旧 DLL 仍按计划后置，先继续从 Dump 和用户视频建立证据链。
