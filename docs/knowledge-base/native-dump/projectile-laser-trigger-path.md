# Projectile / Laser 持续伤害候选源码注释

> 来源：HTGame_classes.hpp、HTGame_parameters.hpp、Dumpspace/FunctionsInfo.json。AProjectileBase、AHTLaserActor 及两个 SpawnDamageActorsEffect 类在 5.6.1 与同仓库历史 5.6.1-new 源码段中逐字一致。该证据支持存在原生持续/周期伤害候选，但不证明旧插件使用这些类。

## 1. AProjectileBase：重叠伤害参数

源码位置：HTGame_classes.hpp 第 4,478–4,572 行；关键字段在第 4,500–4,504 行，重叠回调/生成触发 Actor 方法在第 4,560–4,572 行。

类声明：AProjectileBase 继承 AHTAbilitySpawnActor。以下字段均为 Dump 中的对象字段偏移，不是函数参数偏移。

| 字段 | 偏移 / 大小 | 注释 |
|---|---|---|
| OverlapComponent | 0x500 / 0x08 | 重叠组件引用。 |
| OverlapTriggerLinkTimeComp | 0x508 / 0x08 | 持续链接重叠组件引用。 |
| bOverlapHitDamage | 0x549 / 0x01 | 与重叠命中伤害有关的布尔配置。 |
| bOverlapDamage | 0x560 / 0x01 | 重叠伤害开关候选。 |
| OverlapDamageInterval | 0x564 / 0x04 | float，名称指向重叠伤害间隔。 |
| bOverlapedCheckDamage | 0x568 / 0x01 | 名称指向重叠伤害检查。 |
| OverlapDamageMaxCount | 0x56C / 0x04 | int32，名称指向重叠伤害最大次数。 |
| bUseSelfAsDamageSourceObj | 0x570 / 0x01 | 名称指向是否以自身作为伤害来源对象。 |
| bCheckFriendly / bCheckTeammate | 0x66C / 0x66D | 友方/队友检查配置候选。 |
| bOpenHitNotify | 0x66E | 命中通知开关候选。 |
| bOnlyAttackTrackingTarget | 0x66F | 仅攻击追踪目标配置候选。 |
| CircleAroundCenterActor | 0x764 / 0x08 | 弱引用 Actor。 |
| CurrentTrackActor | 0x770 / 0x08 | 当前追踪 Actor。 |
| bForceTrackActor | 0x778 / 0x01 | 强制追踪配置候选。 |
| fDelayTrackTime | 0x77C / 0x04 | 追踪延迟时间候选。 |
| HitActorList | 0x788 / 0x10 | FHitActorInfo 数组。 |

相关函数声明：
- AfterApplyEffectContainerSpec()
- BoxComponentBeginOverlap(...) / BoxComponentEndOverlap(...)
- FinishSpawningDamageTriggerActor(AActor* SpawnDamageActor)
- GetTrackActor()
- K2_ServerProjectileActorHit(...)
- K2_NextTickDestroySelf(float fDelayTime)

这些字段直接表明 Projectile 类中有重叠伤害间隔/最大次数、命中列表、追踪目标和触发器生成相关状态。不能据此推出其在用户目标场景中的配置值、调用顺序或持续时长。

## 2. AHTLaserActor：逐目标间隔与命中次数

源码位置：HTGame_classes.hpp 第 14,947 行起；关键间隔/次数字段在第 14,971–14,980 行，FinishSpawningDamageTriggerActor 与 LinkTime 重叠回调在第 15,006–15,008 行。

AHTLaserActor 继承 AHTAbilitySpawnActor。相关字段：

| 字段 | 偏移 / 大小 | 注释 |
|---|---|---|
| OverlapTriggerLinkTimeComp | 0x4A8 / 0x08 | 持续链接重叠组件。 |
| LaserEffectFunObjects | 0x530 / 0x10 | UHTLaserEffectFunObject 数组。 |
| LaserLengthMax / LaserSpeed | 0x540 / 0x544 | 激光长度与速度配置。 |
| TriggerRadius | 0x548 / 0x04 | 触发半径。 |
| TargetHitCount | 0x550 / 0x04 | int32，目标命中计数候选。 |
| TriggerDuration | 0x554 / 0x04 | float，触发持续时间候选。 |
| TriggerDamageInterval | 0x55C / 0x04 | float，伤害触发间隔候选。 |
| PerTargetDamageInterval | 0x560 / 0x04 | float，逐目标伤害间隔候选。 |
| bTriggerDamageImmediately | 0x564 / 0x01 | 是否立即触发伤害的布尔配置候选。 |
| MaxHitCount | 0x568 / 0x04 | int32，最大命中次数。 |
| DelayActiveTime | 0x56C / 0x04 | 延迟激活时间候选。 |
| TriggerDamageObjectTypes | 0x570 / 0x10 | 对象类型查询数组。 |
| HitEndObjectTypes | 0x580 / 0x10 | 结束命中对象类型数组。 |

相关函数：
- FinishSpawningDamageTriggerActor(AActor* SpawnDamageActor)
- OnPlayerTriggerLinkTimeBeginOverlap(..., bool bFromSweep, const FHitResult& SweepResult)
- OnPlayerTriggerLinkTimeEndOverlap(...)

反射元数据显示两个 FinishSpawningDamageTriggerActor 都是 Event|Public|BlueprintEvent，参数只有 AActor* SpawnDamageActor（参数结构大小 0x08）。Laser 的 BeginOverlap 是 Final|Native|Private|HasOutParams，参数布局包括组件、OtherActor、OtherComp、OtherBodyIndex、bFromSweep 和 SweepResult；EndOverlap 包含前四项。

这些字段提供了“激光持续期间按间隔对每个目标触发伤害，并限制命中次数”的强命名线索，但实际语义、单位、目标重置行为和运行时调用路径仍未知。

## 3. SpawnDamageActorsEffect 配置类

HTGame_classes.hpp 中 UProjectileSpawnDamageActorsEffect 位于第 71,380 行，ULaserSpawnDamageActorsEffect 位于第 49,462 行。

### UProjectileSpawnDamageActorsEffect
- 继承 UHTProjectileFunc。
- SpawnDamageActorArray @ 0x38，类型 TArray<FSpawnDamageTriggerActor>。
- bDoSpawnOnHitWater @ 0x48。

### ULaserSpawnDamageActorsEffect
- 继承 UHTLaserEffectFunObject。
- SpawnDamageActorArray @ 0x38。
- TriggerInterval @ 0x48，float。
- MaxSpawnCount @ 0x4C，int32。

这两个配置类把前文 FSpawnDamageTriggerActor 数组连接到 Projectile/Laser 功能对象，但不能证明目标插件复用了该配置。

## 4. 两版对照

已读取 5.6.1 的 HTGame_classes.hpp（blob 526091ab6679ac6833634e5a6d08d807e90dbf0c）与 5.6.1-new 的历史实体 blob（7b56987f6db8efe91f66273f3e8d136de45485a3）。AProjectileBase、AHTLaserActor、UProjectileSpawnDamageActorsEffect、ULaserSpawnDamageActorsEffect 四个类段逐字一致。HTGame_parameters.hpp 两版 blob SHA 相同。

## 5. 对伤害链目标的意义

- **直接证实：** 原生 Projectile/Laser 类型存在重叠伤害、间隔、最大次数、逐目标间隔、持续链接重叠组件及生成 DamageTriggerActor 的声明。
- **有证据支持但未验证：** 这是另一条原生周期伤害候选，且 AHTLaserActor 的 PerTargetDamageInterval 比普通的技能重放更接近“同一目标持续受到后续伤害”的外观。
- **未知：** 旧插件是否创建/调用 Projectile、Laser 或 DamageTriggerActor；这些字段在目标资产中的实际值；逐目标计时是否会因新命中重置；是否可通过受支持 Anomaly ABI 安全创建/配置这些对象。
- 不能因字段名看似匹配就选定路线；应结合视频行为、资产对象路径、原生调用链和后续旧 DLL 证据。
