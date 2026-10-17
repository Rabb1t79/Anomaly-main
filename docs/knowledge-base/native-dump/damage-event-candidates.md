# 伤害事件候选：`AHTAbilityCharacter::OnDamaged`

> 来源：同一仓库内的 `5.6.1/` 与 LFS 转换前 `5.6.1-new/` Dump 正文。该类段、参数文件及相关结构段在两版中逐字一致。这里只识别候选事件，不宣称它是旧插件的实际路径。

## 1. 函数声明与反射元数据

`HTGame_classes.hpp` 中的声明：

`void OnDamaged(float DamageAmount, const FHitResult& HitInfo, const FGameplayTagContainer& DamageTags, const UHTGamePlayEffect* DamageGameplayEffect, AHTAbilityCharacter* InstigatorCharacter, AActor* DamageCauser);`

同源 `Dumpspace/FunctionsInfo.json` 将 owner 记录为 `AHTAbilityCharacter`，flags 为：

`Native|Event|Public|HasOutParams|BlueprintEvent`

**直接证实：** 该类型公开声明一个带伤害量、命中信息、伤害标签、伤害 GameplayEffect、施加者角色和伤害来源 Actor 参数的 Native/Event/BlueprintEvent 函数。

**未知：** 它是否在每次敌人受击时触发、触发时点在扣血前还是后、是否在所有可控角色上共用、是否涵盖非点伤害、是否可被 Anomaly 的已支持回调机制安全订阅。

## 2. 参数布局

来源：`HTGame_parameters.hpp`，blob `b839b1a326e90ad3ac1c598f6bbca6b6eee465e1`。该文件在两版中的 blob SHA 相同。

`HTAbilityCharacter_OnDamaged` 参数结构总大小为 `0x148`：

| 参数 | 偏移 / 大小 | 说明 |
|---|---|---|
| `DamageAmount` | `0x000 / 0x004` | float 伤害量。 |
| padding | `0x004 / 0x004` | 对齐空间。 |
| `HitInfo` | `0x008 / 0x108` | `FHitResult` 引用参数。 |
| `DamageTags` | `0x110 / 0x020` | `FGameplayTagContainer` 引用参数。 |
| `DamageGameplayEffect` | `0x130 / 0x008` | `const UHTGamePlayEffect*`。 |
| `InstigatorCharacter` | `0x138 / 0x008` | `AHTAbilityCharacter*`。 |
| `DamageCauser` | `0x140 / 0x008` | `AActor*`。 |

这是参数结构布局，不等于直接函数调用约定。实际桥接前仍需按 Anomaly 的公开 ABI、反射验证与 Game 线程规则核验。

## 3. 关联结构

### `FHTDamageEvent`
- 继承 `FPointDamageEvent`，总大小注释为 `0x170`。
- `DamageGEDef` 为 `TWeakObjectPtr<UHTGamePlayEffect>`，偏移 `0x138`。
- `DamageTags` 为 `FGameplayTagContainer`，偏移 `0x148`、大小 `0x20`。
- 两版该结构段逐字一致。

### `FHTGameplayEffectContext`
- 继承 `FGameplayEffectContext`。
- `Target` 为 `TWeakObjectPtr<AActor>`，偏移 `0x80`。
- 两版该结构段逐字一致。
- 它提示效果上下文中存在目标引用字段，但不能据此证明每个伤害事件都会带有可用的该上下文。

## 4. 对复现路线的意义

- **有证据支持但未验证：** `AHTAbilityCharacter::OnDamaged` 比直接重放技能/输入更接近“观察原生伤害事件”的候选入口，因为其参数直接携带伤害量、HitInfo、DamageTags、伤害效果、InstigatorCharacter 和 DamageCauser。
- **仍未知：** 监听这个事件是否足以识别“玩家对敌人造成的有效伤害”；它是否在效果应用之后触发；回调能否安全地在其内部继续触发伤害；是否会产生递归/重复事件；车辆或环境伤害如何区分。
- 不应直接从此函数推导出“再次调用 OnDamaged 就能造成伤害”。该函数目前只被证实为事件声明/反射包装，不是伤害计算实现。
- 另有 `UAbilityTask_JumpSection::OnAttackedCallback` / `OnDamagedCallback`，其 owner 是能力任务类，不应仅因名字相似就视为通用伤害入口。

## 5. 当前判断

该事件值得列入后续候选路线，但在确认真实调用时序、owner、receiver、可用 ABI 和实际伤害结果之前，不做实现选择。旧 DLL 仍按计划后置，先继续从 Dump 和用户视频建立证据链。
