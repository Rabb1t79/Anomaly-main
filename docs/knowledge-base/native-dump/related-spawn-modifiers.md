# DamageTrigger 周边原生类型注释

> 来源：`5.6.1/CppSDK/SDK/HTGame_classes.hpp`，blob `526091ab6679ac6833634e5a6d08d807e90dbf0c`。这些是相关类型的声明摘要，不是旧插件使用它们的证据。

## 1. `AHTAbilitySpawnActor`

该类继承 `AHTActor`，相关字段：

| 字段 | 偏移 / 大小 | 简注 |
|---|---|---|
| `OverrideEffectContainerInfo` | `0x378 / 0x30` | 效果容器覆盖信息，标记为网络复制/RepNotify。 |
| `EffectContainer` | `0x3A8 / 0x28` | `FHTGameplayEffectContainer`。 |
| `EventTag` | `0x3D0 / 0x08` | 暴露生成的 GameplayTag。 |
| `EventData` | `0x3D8 / 0xB0` | `FGameplayEventData`，标记为 ExposeOnSpawn。 |
| `TriggerAbilityHandle` | `0x488 / 0x04` | 触发能力句柄。 |
| `SavedTriggerSkillCDO` | `0x490 / 0x08` | `UHTGameplayAbility*`。 |
| `CachedTriggerAbilityInstance` | `0x498 / 0x08` | 弱引用能力实例。 |
| `InheritedSkillLevel` | `0x4A0 / 0x04` | 继承技能等级。 |

声明的函数包括 `OnAbilityEnded`、`OnRep_EffectContainer` 和 `OverrideEffectContainer`。这说明基类带有效果容器、事件数据及能力关联状态；它本身不证明具体伤害触发流程或可用的公开插件 API。

## 2. 数值参数修改器

### `UHTSpawnActorParamModifier`
继承 `UObject`，字段：
- `ParamId` @ `0x28`：`FGameplayTag` 参数 ID。
- `ApplyPhase` @ `0x30`：`EHTSpawnActorParamPhase` 阶段枚举。
- `SpawnActorClass` @ `0x38`：目标 Actor 类。
- `EventTags` @ `0x40`：GameplayTag 集合。
- `SpawnConfigIDs` @ `0x60`：`TSet<FName>` 配置 ID 集合。

### `UHTSpawnActorNumericParamModifier`
继承上面的参数修改器，新增：
- `MinValue` @ `0xB0`、`MaxValue` @ `0xB4`。
- `bHasOverride` @ `0xB8`。
- `OverrideValue` @ `0xBC`、`AdditiveValue` @ `0xC0`、`Multiplier` @ `0xC4`。
- `Revision` @ `0xC8`。

这些字段表明存在数值参数的范围、覆盖、加法、倍率和修订状态，但不能仅凭字段名断定应用顺序或具体更新逻辑。

### 专用 DamageTrigger 修改器
以下类均继承 `UHTSpawnActorNumericParamModifier`：
- `UHTDamageTriggerCountModifier`：额外字段 `MaxSpawnCount` @ `0xD0`。
- `UHTDamageTriggerCountPerPeriodModifier`。
- `UHTDamageTriggerDurationModifier`。
- `UHTDamageTriggerMaxTriggerCountModifier`。
- `UHTDamageTriggerPeriodModifier`。
- `UHTDamageTriggerRadiusModifier`。

这些类名和继承关系直接支持存在针对生成伤害触发器的次数、周期、持续时间、最大次数和半径参数修改机制；它们是否参与目标旧插件、如何与 `ADamageTriggerActor` 的 Runtime 字段关联，仍未知。

## 3. 数值修改入口与枚举（新增正文核验）

来源：HTGame_structs.hpp 第 11,439–11,455 行；HTGame_classes.hpp 第 15,266 行；HTGame_parameters.hpp 第 15,231 行。

- EHTSpawnActorParamPhase：PreSpawn = 0、PostSpawn = 1。此枚举明确只有生成前/生成后两个有效阶段值。
- EHTNumericParamOperation：Set = 0、Add = 1、Multiply = 2。
- UHTAbilitySystemBlueprintLibrary 声明静态函数：

  ModifySpawnActorNumericParam(UHTGameplayAbility* AbilityInstance, const FGameplayTag& ParamId, EHTNumericParamOperation Operation, float Value) -> bool

- FunctionsInfo.json flags：Final|Native|Static|Public|BlueprintCallable。
- 参数结构 HTAbilitySystemBlueprintLibrary_ModifySpawnActorNumericParam 总大小 0x20：AbilityInstance @ 0x00（8 字节）、ParamId @ 0x08（8 字节）、Operation @ 0x10（1 字节）、padding @ 0x11–0x13、Value @ 0x14（4 字节）、ReturnValue @ 0x18（1 字节）、尾部 padding @ 0x19–0x1F。
- 两版 HTGame_parameters.hpp 和 HTGame_functions.cpp 的 blob SHA 相同；因此该参数布局和生成反射包装文件逐字一致。

**边界：** 这是游戏 Dump 中声明的静态 BlueprintCallable 函数，不等于 Anomaly 插件已有对应公开 ABI。它以 UHTGameplayAbility 实例、ParamId、操作类型和值为输入；不能据此断言它直接修改已生成 Actor 的 Runtime 字段、能创建 Actor，或就是旧插件机制。生成的函数包装仍只是 ProcessEvent wrapper，不是函数原生实现体。

## 4. 其他持有生成伤害触发配置的效果类

### `UProjectileSpawnDamageActorsEffect`
继承 `UHTProjectileFunc`：
- `SpawnDamageActorArray` @ `0x38`，类型 `TArray<FSpawnDamageTriggerActor>`。
- `bDoSpawnOnHitWater` @ `0x48`。
- 从类名和字段名看，它是投射物相关的伤害触发 Actor 生成效果候选；实际调用时序需进一步核验。

### `ULaserSpawnDamageActorsEffect`
继承 `UHTLaserEffectFunObject`：
- `SpawnDamageActorArray` @ `0x38`。
- `TriggerInterval` @ `0x48`，float。
- `MaxSpawnCount` @ `0x4C`，int32。
- 这说明激光效果类也有生成伤害触发 Actor 数组和间隔/最大生成数配置；不证明与旧插件有关。

## 5. 目标类型候选

`FHTGameplayEffectContainer` 中引用 `UHTGATargetType` 子类。Dump 声明存在：
- `UHTGATargetType_UseEventData`
- `UHTGATargetType_UseOwner`
- 以及其他 `UHTGATargetType` 派生类型。

仅凭名字不能确定目标选择结果；需继续检查这些类型的函数元数据、父类方法和实际调用者。

## 6. 与目标需求的关系

- **直接证实：** 原生 SDK 声明有伤害触发 Actor、目标/效果容器、参数修改器、生成配置，以及投射物/激光效果持有该配置的结构。
- **有证据支持但未验证：** 游戏可能通过配置化的 SpawnActor effect 来构造周期伤害触发实例。
- **未知：** 是否就是旧插件实际机制；这些类在目标行为中的调用路径；能否用公开 Anomaly ABI 创建/配置/销毁相应对象。
- 这组线索不应被转译为任意内存写入或未经验证的私有调用；任何桥接仍需核验 ABI、对象有效性、Game 线程和生命周期。
