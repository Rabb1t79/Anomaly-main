# DamageTrigger 候选源码逐段注释（5.6.1 与 5.6.1-new）

> 来源：`Rabb1t79/Anomaly-main` 仓库两个 Dump 目录。新版真实正文从同仓库 LFS 转换前提交 `5a16a377c2133428d4ccf6f9c0d8770455f9a588` 读取。本文是原生声明摘要，不证明旧插件采用该路径。

## 1. `ADamageTriggerActor` 类与字段

旧版 classes blob：`526091ab6679ac6833634e5a6d08d807e90dbf0c`；新版历史 classes blob：`7b56987f6db8efe91f66273f3e8d136de45485a3`。候选类段逐字一致。旧版源码位置：HTGame_classes.hpp 第 20,942–21,014 行；结构位置：HTGame_structs.hpp 第 21,548–21,575 行、第 78,757–78,824 行。

| 字段 / 偏移 | 简要注释 | 证据状态 |
|---|---|---|
| `class ADamageTriggerActor : public AHTAbilitySpawnActor` | 继承 `AHTAbilitySpawnActor`；类头注明基类布局大小 `0x04A8`。 | 直接证实 |
| `SpawnConfigID` @ `0x04D0` | 字符串生成配置 ID；具体查找来源未知。 | 字段直接证实 |
| `fTriggerEffectRadius` @ `0x04E0` | float 触发半径字段；实际判定用途未知。 | 字段直接证实 |
| `OverlapTriggerAbsorbCharacters` @ `0x0588` | `TArray<FHTAbsorbAbilityData>`，名称指向吸收重叠角色。 | 字段直接证实；语义未验证 |
| `bAffectedLifeTime` @ `0x0598` | 生命周期相关布尔标志；控制逻辑未知。 | 字段直接证实 |
| `OverlapTriggerLinkTimeCharacters` @ `0x05A8` | 私有 `TArray<AHTPlayerCharacter*>`；是持续链接目标集合的线索，但不能证明它是伤害目标列表。 | 字段直接证实；用途未验证 |
| `OverlapTriggerLinkTimeComp` @ `0x05C8` | 私有 `UShapeComponent*`，名字指向持续链接重叠组件。 | 字段直接证实 |
| `OverlapTriggerAbsorbComp` @ `0x05D8` | 私有 `UShapeComponent*`，与 LinkTime 组件分列。 | 字段直接证实 |
| `RuntimeTriggerPeriod` @ `0x05E8` | 私有瞬态 float，名称指向触发周期；单位/更新规则未知。 | 字段直接证实 |
| `RuntimeTriggerDuration` @ `0x05EC` | 私有瞬态 float，名称指向持续时间；单位/更新规则未知。 | 字段直接证实 |
| `RuntimeTriggerCountPerPeriod` @ `0x05F0` | 私有瞬态 int32，名称指向每周期次数；是已触发、剩余还是计划次数未知。 | 字段直接证实 |
| `RuntimeMaxTriggerCount` @ `0x05F4` | 私有瞬态 int32，名称指向最大触发次数；与前项关系未知。 | 字段直接证实 |

类大小注释为 `0x0618`，派生字段增量 `0x0170`。这些字段说明类中存在周期/时长/次数状态，但不揭示更新或触发算法。

## 2. 函数声明逐项注释

源码位置：HTGame_classes.hpp 第 20,989–20,997 行。

两版该类均有相同的 17 个函数，函数名、返回类型、参数列表与 flags 在 `Dumpspace/FunctionsInfo.json` 中规范化后相同。

- `BlackHoleBeginOverlap` / `BlackHoleEndOverlap`：组件/Actor 重叠回调；与目标伤害的关系未知。
- `ClientTriggerHideInGameEvent`：客户端隐藏相关事件名；实现未知。
- `GetLifeSpanSinceCreationWithDelay(bool)`、`GetSpanLifeTime()`：生命周期时间查询候选；返回值单位未知。
- `GetMeshComponent()`：返回 `UMeshComponent*`。
- `K2_GetTriggerEffectRadius()`：返回 float，名字指向触发半径查询。
- `K2_OnTriggerPeriodGamePlayEffect()`：无参数、Native、Public、BlueprintCallable；名字指向周期 GameplayEffect 入口。
- `OnAbsorbBeginOverlap(...)`：吸收重叠回调候选。
- `OnCharacterDeadChanged(bool IsDead)`：死亡状态变化回调；死亡后清理逻辑未知。
- `OnPlayerBeginOverlap(..., bool bFromSweep, const FHitResult& SweepResult)`：进入重叠区域回调。
- `OnPlayerEndOverlap(...)`：离开重叠区域回调。
- `StartTriggerGamePlayEffect()`：无参数、Native、Public；名字指向启动触发 GameplayEffect。
- `TriggerAbsorbBegin/End(AHTAbilityCharacter*)`：接收能力角色指针的事件。
- `TriggerPeriodGamePlayEffectEvent()`：无参数 BlueprintEvent，名字指向周期触发事件。
- `TriggerStartShowActorEvent()`：显示 Actor 相关事件。

函数名和 flags 是反射元数据，不是调用图；不证明函数在目标场景中实际运行。

## 3. 效果容器与生成数据结构

两版的以下结构段逐字一致；旧版 structs blob 为 `cc6781d70b782a0e3ec6cf06dfbd75f323f04a53`，新版历史 blob 为 `28b7cb52252fc062bcbb7d23ccfc9ddf52621079`。

### `FHTGameplayEffectContainer`（大小 `0x28`）
- `TargetType` @ `0x00`、`AddTargetType` @ `0x08`、`NotFindToDoTargetType` @ `0x10`：三个 `TSubclassOf<UHTGATargetType>`。
- `TargetGameplayEffectClasses` @ `0x18`，大小 `0x10`：`TArray<TSubclassOf<UGameplayEffect>>`。
- 直接结论：它是目标类型与 GameplayEffect 类数组的数据容器；不能推出应用顺序、实际目标或调用者。

### `FSpawnDamageTriggerActor`（大小 `0x70`）
- `SapwnActorClass`（保留原拼写）@ `0x00`：`TSubclassOf<AHTAbilitySpawnActor>`。
- `SpawnConditionObjs` @ `0x08`；`SpawnLocationObj` @ `0x18`；`SpawnRotationObj` @ `0x20`；`SpawnScaleObj` @ `0x28`。
- `bForceSpawn` @ `0x30`；`bClientPredictionSpawn` @ `0x31`。
- `DamageTriggerConfigID` @ `0x34`；`RandomRadius` @ `0x3C`；`EffectContainer` @ `0x40`（大小 `0x28`）；`RandomCountCurve` @ `0x68`。

### `FSpawnDamageTriggerActorArray`（大小 `0x28`）
- `SpawnArray` @ `0x00`；`nRandomSpawnCount` @ `0x10`；`SelectTargetType` @ `0x18`；`SecondSelectTargetType` @ `0x20`。
- `UHTAbilitySpawnDamageTriggerActor` 继承 `UHTAbilitySpawnActorEffect`，在 `0x28` 处持有 `SpawnDamageTriggerActorsInfo`，大小 `0x28`。
- 这些声明将生成 Actor、生成条件/位置和 GameplayEffect 容器关联到一个配置结构中，但不证明旧插件采用它。

## 4. 时序与目标位置字段

源码位置：FDamageTriggerActorStaticData 在 HTGame_structs.hpp 第 78,809 行起；UFindSpawnLocationObj_Target 在 HTGame_classes.hpp 第 29,846 行起；UHTAbilitySpawnDamageTriggerActor 在 HTGame_classes.hpp 第 35,020 行起。

### `FDamageTriggerActorStaticData`（大小 `0x58`，继承 `FTableRowBase`）
- `SpawnOffset` @ `0x08`，`SpawnOffsetRotation` @ `0x20`。
- `DelaySpawnTime` @ `0x38`、`duration` @ `0x3C`、`DelayDestoryTime` @ `0x40`。
- `TriggerOnSpawn` @ `0x44`、`DelayTriggerTime` @ `0x48`、`TriggerLinkTimeDuration` @ `0x4C`、`Period` @ `0x50`、`MaxTriggerCount` @ `0x54`。
- 字段名支持延迟/持续/周期/最大次数是配置候选；单位、消费者和运行时赋值关系未知。

### `UFindSpawnLocationObj_Target`
- 两版类段逐字一致，继承 `UFindSpawnLocationObj`。
- `NotTargetSpawnBySelfLocation` @ `0x50`；`LocationOffset` @ `0x58`；`TargetLocationOffset` @ `0x70`。
- `CustomTargetLocationOffset` @ `0x88`；`UseToTargetDirection` @ `0x90`；`IncludeTargetCapsule` @ `0x91`；`PreLocationTime` @ `0x94`。
- 名称提示生成位置可能相对目标，但实际算法、坐标系和单位未知。

## 5. 参数布局与生成包装

源码位置：HTAbilityCharacter::OnDamaged 在 HTGame_classes.hpp 第 1,445 行；对应参数结构位于 HTGame_parameters.hpp 第 2,385 行附近。

两版 `HTGame_parameters.hpp` 都是 blob `b839b1a326e90ad3ac1c598f6bbca6b6eee465e1`；两版 `HTGame_functions.cpp` 都是 blob `c0a5f1dfa3540ab656d18578950d02e4de522ef2`，因此两个文件内容逐字相同。

- `DamageTriggerActor_OnPlayerBeginOverlap` 大小 `0x128`：`OverlappedComponent` @ `0x00`、`OtherActor` @ `0x08`、`OtherComp` @ `0x10`、`OtherBodyIndex` @ `0x18`、`bFromSweep` @ `0x1C`、`SweepResult` @ `0x20`，大小 `0x108`。
- `DamageTriggerActor_OnPlayerEndOverlap` 大小 `0x20`：前三个指针 @ `0x00/0x08/0x10`，`OtherBodyIndex` @ `0x18`，尾部 padding 4 字节。
- `DamageTriggerActor_OnCharacterDeadChanged` 大小 `0x01`：`bool IsDead` @ `0x00`。
- `DamageTriggerActor_TriggerAbsorbBegin/End` 各有 `AHTAbilityCharacter*` @ `0x00`，大小 `0x08`。
- `StartTriggerGamePlayEffect()` 与 `TriggerPeriodGamePlayEffectEvent()` 在参数文件中没有独立参数结构，与类声明/反射元数据的无参数签名一致。
- 生成包装 `ADamageTriggerActor::StartTriggerGamePlayEffect()`：静态缓存 `UFunction*`；通过 `Class->GetFunction("DamageTriggerActor", "StartTriggerGamePlayEffect")` 查找；保存原 `FunctionFlags`；对 flags 按位 OR `0x400`；调用 `UObject::ProcessEvent(Func, nullptr)`；最后恢复原 flags。
- 这是生成的反射调用包装，不是原生实现体；没有展示计时、循环、目标筛选或效果应用逻辑。它不证明此调用是受 Anomaly 支持的接口，也不证明旧插件通过此函数工作。

## 6. 机制结论分级
- **直接证实**：Dump 存在周期触发 Actor、重叠目标数组、周期/持续时间/次数状态、GameplayEffect 类容器、Actor 生成配置和目标位置配置；已比较的候选声明在两版中相同。
- **有证据支持但未验证**：这组声明适合作为持续/周期伤害机制的原生候选方向。
- **未知**：旧插件是否使用它；首次伤害如何关联目标；是否自动重复同一效果；多目标是否独立计时；目标离开/死亡时如何清理；哪些函数真实走在目标路径上；如何合规桥接到 Anomaly。
