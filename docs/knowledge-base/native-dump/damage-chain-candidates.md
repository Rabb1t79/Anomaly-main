# 伤害链候选原生机制：阶段性结论

> 本页只记录 Dump 声明证据和当前候选方向；不证明旧插件实际使用了某条路径。

## 已直接证实的原生声明

在 `5.6.1/` 与同仓库历史提交中可读取的 `5.6.1-new/` 真实正文之间，以下候选符号段逐字一致：

- `ADamageTriggerActor`：继承 `AHTAbilitySpawnActor`；有 `OverlapTriggerLinkTimeCharacters` 目标数组，以及 `RuntimeTriggerPeriod`、`RuntimeTriggerDuration`、`RuntimeTriggerCountPerPeriod`、`RuntimeMaxTriggerCount` 字段。
- `FHTGameplayEffectContainer`：包含目标类型选择字段和 `TargetGameplayEffectClasses`。
- `FSpawnDamageTriggerActor` / `FSpawnDamageTriggerActorArray`：描述生成 Actor、生成条件/位置/旋转/缩放、配置 ID、效果容器、随机生成数与目标类型。
- `FDamageTriggerActorStaticData`：有延迟生成、持续时间、延迟触发、链接持续时间、周期和最大次数字段。
- `UHTAbilitySpawnDamageTriggerActor`：继承 `UHTAbilitySpawnActorEffect`，持有 `SpawnDamageTriggerActorsInfo`。
- `UFindSpawnLocationObj_Target`：包含目标位置偏移、方向、胶囊和预位置时间相关字段。
- `AHTAbilityCharacter::OnDamaged`：公开事件声明携带 `DamageAmount`、`HitInfo`、`DamageTags`、`DamageGameplayEffect`、`InstigatorCharacter` 和 `DamageCauser`。

详细逐段说明见：
- [DamageTrigger 源码注释](damage-trigger-source-notes.md)
- [伤害事件候选](damage-event-candidates.md)
- [生成修改器和周边类型](related-spawn-modifiers.md)
- [版本对照](version-diff.md)

## 候选路线（未选定实现）

1. **周期触发 Actor 路线**：调查 `ADamageTriggerActor` 的实际触发函数、目标集合维护、时序字段写入点和生成配置消费者。
2. **原生伤害事件观察路线**：调查 `AHTAbilityCharacter::OnDamaged` 的真实调用时序和事件覆盖范围，判断是否能识别有效的 player→enemy 伤害。
3. **生成参数配置路线**：调查 `UHTSpawnActorNumericParamModifier` 及专用 DamageTrigger modifiers 的应用阶段和具体消费者。
4. **调用桥接路线**：只有在确定原生候选路径后，才核验 Anomaly 的公开 ABI 是否有可用且合规的入口；缺少接口时再评估最小 ABI/Core 扩展。

## 仍未证实

- 旧插件是否使用上述任意候选类型。
- 首次伤害后是否自动重复同一原生伤害效果。
- 多个敌人是否独立计时、不互相重置。
- 目标离开重叠范围、死亡或对象失效时的清理行为。
- 哪些函数在真实战斗路径中被调用，以及是否可以通过受支持的 Anomaly ABI 桥接。
- 用户视频中的可观察效果与以上原生候选是否对应。

## 证据顺序

先以两个 Dump 版本的类/结构/参数/反射元数据建立候选；再对照用户视频的可观察行为；旧 DLL 控制流和常量分析按当前计划后置，用于交叉验证而非先验决定实现。
