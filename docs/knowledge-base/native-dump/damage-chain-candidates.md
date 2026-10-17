# 伤害链候选原生机制：阶段性结论

> 本页记录 Dump 声明证据与候选方向；不证明旧插件实际使用了某条路径。

## 已直接证实的原生声明

在 5.6.1/ 与同仓库历史提交中可读取的 5.6.1-new/ 真实正文之间，以下候选符号段逐字一致：

- ADamageTriggerActor：继承 AHTAbilitySpawnActor；有 OverlapTriggerLinkTimeCharacters 数组，以及 RuntimeTriggerPeriod、RuntimeTriggerDuration、RuntimeTriggerCountPerPeriod、RuntimeMaxTriggerCount 字段。
- FHTGameplayEffectContainer：目标类型字段和 TargetGameplayEffectClasses 数组。
- FSpawnDamageTriggerActor / FSpawnDamageTriggerActorArray：生成 Actor、条件/位置/旋转/缩放、配置 ID、效果容器、随机生成数与目标类型。
- FDamageTriggerActorStaticData：延迟生成、持续时间、延迟触发、链接持续时间、周期和最大次数。
- UHTAbilitySpawnDamageTriggerActor：继承 UHTAbilitySpawnActorEffect，持有 SpawnDamageTriggerActorsInfo。
- UFindSpawnLocationObj_Target：目标位置偏移、方向、目标胶囊和预位置时间相关字段。
- AProjectileBase：存在 OverlapDamageInterval、OverlapDamageMaxCount、HitActorList、OverlapTriggerLinkTimeComp 和 FinishSpawningDamageTriggerActor。
- AHTLaserActor：存在 TriggerDamageInterval、PerTargetDamageInterval、MaxHitCount、TriggerDuration、目标对象类型数组和 FinishSpawningDamageTriggerActor。
- AHTAbilityCharacter::OnDamaged：事件声明携带 DamageAmount、HitInfo、DamageTags、DamageGameplayEffect、InstigatorCharacter、DamageCauser。

## GObjects 对象路径交叉证据

来源：5.6.1/GObjects-Dump.txt，blob 4ea559ff564c3c68bcba226368e8839b4dee0034，共 383,234 行。

- 第 192,829 行：Package DamageTriggerActorDataTable。
- 第 199,415 行：DataTable DamageTriggerActorDataTable.DamageTriggerActorDataTable。
- HTAbilitySpawnDamageTriggerActor 出现在 74 行（96 次字符串出现，部分行多次出现）。
- 第 181,500–181,504 行：XL_map_bigworld_test 的对象路径中，GA_Lacrimosa_PerfectEvade_C 下有 HTAbilitySpawnDamageTriggerActor_0，并列出 FindSpawnRotationObj_Owner 与 FindSpawnLocationObj_Target 子对象。
- 第 181,552–181,554 行：GA_Lacrimosa_SwitchSkill_C 下有对应 HTAbilitySpawnDamageTriggerActor_0，并列出 Owner 位置/旋转对象。
- 第 226,794–226,799 行：GA_Shinku_UltraSkill_Rage_Pre 路径下有两个 HTAbilitySpawnDamageTriggerActor 对象和条件对象。
- 其他默认对象路径还包括 GA_Lacrimosa_MeleeModA、GA_Lacrimosa_UltraSkillModA、GA_Trigger_Kill_50/100/150、GA_Accessory_11 等。

**证据边界：** 这些条目证明对象清单中有 DataTable 与相关能力/资产对象路径，不证明它们在目标战斗中实际执行，也不证明旧 DLL 使用了这些资产。对象清单地址仅是 Dump 快照值，不得作为跨版本稳定地址。

## 候选路线（尚未选定实现）

1. **周期触发 Actor 路线**：调查 ADamageTriggerActor 的实际触发函数、目标集合维护、时序字段写入点和生成配置消费者。
2. **Projectile/Laser 持续伤害路线**：调查 AProjectileBase 的重叠伤害间隔/最大次数，以及 AHTLaserActor 的逐目标伤害间隔、持续时间和命中计数。详见 projectile-laser-trigger-path.md。
3. **原生伤害事件观察路线**：区分并调查 AHTAbilityCharacter::OnDamaged 反射事件、CharacterOnDamaged 多播委托和 Anomaly profile 中的内部广播符号；不能把它们视为同一个入口。
4. **生成参数配置路线**：调查 UHTSpawnActorNumericParamModifier 及专用 DamageTrigger modifiers 的应用阶段与消费者。
5. **调用桥接路线**：只有确定原生候选路径后，才核验 Anomaly 公开 ABI；缺少接口时再评估最小合规 ABI/Core 扩展。

## 仍未证实
- 旧插件是否使用上述任意候选类型。
- 首次伤害后是否自动重复同一原生伤害效果。
- 多个敌人是否独立计时、不互相重置。
- 目标离开重叠范围、死亡或对象失效时的清理行为。
- 哪些函数在真实战斗路径中被调用，以及是否可通过受支持 Anomaly ABI 桥接。
- 视频中的可观察效果与这些原生候选是否对应。

## 重要读取限制
GObjects-Dump-WithProperties.txt 约 67.9 MB，单次 Blob 响应超过当前连接器的 64 MiB 解码上限。完整属性内容尚未读取，因此不能声称已确认 Period、MaxTriggerCount 或 TriggerLinkTimeDuration 的实际数据表值。

## 证据顺序
先以两个 Dump 版本的类/结构/参数/反射元数据建立候选，再对照用户视频的可观察行为；旧 DLL 控制流与常量分析后置，用于交叉验证，而不是先验决定实现。
