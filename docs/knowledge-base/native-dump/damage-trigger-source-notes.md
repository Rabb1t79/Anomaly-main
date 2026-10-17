# DamageTrigger 候选源码逐段注释（5.6.1）

> 本文只记录仓库中可读取的 `5.6.1/` Dump 原文。注释依据：类声明、结构体声明、参数布局和生成的函数包装。它们证明原生 SDK 中存在这些声明，不证明旧插件使用了它们，也不证明这些调用可直接作为 Anomaly 插件接口。

## 1. `ADamageTriggerActor` 类声明与字段

来源：`5.6.1/CppSDK/SDK/HTGame_classes.hpp`，Git blob `526091ab6679ac6833634e5a6d08d807e90dbf0c`。

| 原文声明 / 偏移 | 简要注释 | 证据等级 |
|---|---|---|
| `class ADamageTriggerActor : public AHTAbilitySpawnActor` | 此类继承 `AHTAbilitySpawnActor`；基类完整布局大小由类头注释显示为 `0x04A8`。 | 直接证实：声明 |
| `SpawnConfigID` @ `0x04D0` | 字符串型生成配置 ID；字段带 `ExposeOnSpawn`、`Net` 等生成标记。字段名不能确定 ID 的查找位置。 | 直接证实：字段 |
| `fTriggerEffectRadius` @ `0x04E0` | 浮点触发效果半径字段。具体由何种碰撞/判定使用，未知。 | 直接证实：字段 |
| `OverlapTriggerAbsorbCharacters` @ `0x0588` | 数组元素为 `FHTAbsorbAbilityData`；从名称看与吸收重叠对象有关，但实际语义需函数/实现核验。 | 声明直接证实；语义未验证 |
| `bAffectedLifeTime` @ `0x0598` | 布尔生命周期相关标志。何种生命周期行为受其影响，未知。 | 直接证实：字段 |
| `OverlapTriggerLinkTimeCharacters` @ `0x05A8` | 私有数组，元素为 `AHTPlayerCharacter*`。它是“持续链接/重叠目标集合”的强线索，但不能证明数组更新策略或它就是伤害目标列表。 | 声明直接证实；用途有证据支持但未验证 |
| `OverlapTriggerLinkTimeComp` @ `0x05C8` | 私有 `UShapeComponent*`，名字指向持续链接重叠组件。 | 声明直接证实；实际触发条件未知 |
| `OverlapTriggerAbsorbComp` @ `0x05D8` | 私有 `UShapeComponent*`，名字指向吸收重叠组件；与 LinkTime 组件是不同字段。 | 直接证实 |
| `RuntimeTriggerPeriod` @ `0x05E8` | 私有瞬态 float 运行状态字段，名字表示触发周期。单位和更新逻辑未知。 | 直接证实 |
| `RuntimeTriggerDuration` @ `0x05EC` | 私有瞬态 float 运行状态字段，名字表示触发持续时间。单位和更新逻辑未知。 | 直接证实 |
| `RuntimeTriggerCountPerPeriod` @ `0x05F0` | 私有瞬态 int32 字段，名字表示每周期触发计数。是否为计划次数、剩余次数或已触发次数未知。 | 直接证实 |
| `RuntimeMaxTriggerCount` @ `0x05F4` | 私有瞬态 int32 字段，名字表示最大触发计数。与上一个字段的关系未知。 | 直接证实 |

### 函数声明列表逐项注释

- `BlackHoleBeginOverlap(...)` / `BlackHoleEndOverlap(...)`：以组件和 Actor 等作为参数的重叠回调；Dump 声明本身不证明与目标伤害功能相关。
- `ClientTriggerHideInGameEvent()`：事件名涉及客户端隐藏状态；具体行为未知。
- `GetLifeSpanSinceCreationWithDelay(bool)`、`GetSpanLifeTime()`：生命周期时间相关查询函数；返回值和单位需结合参数声明/元数据进一步核验。
- `GetMeshComponent()`：返回 `UMeshComponent*`。
- `K2_GetTriggerEffectRadius()`：返回 float，名字表示查询触发效果半径。
- `K2_OnTriggerPeriodGamePlayEffect()`：周期 GameplayEffect 相关的 K2 事件入口声明；未从此声明获知其内部实现。
- `OnAbsorbBeginOverlap(...)`：吸收组件的重叠回调候选。
- `OnCharacterDeadChanged(bool IsDead)`：接收死亡状态布尔值的回调声明。它提示生命周期/目标死亡可能被观察，但不证明死亡后如何清理。
- `OnPlayerBeginOverlap(..., bool bFromSweep, const FHitResult& SweepResult)` / `OnPlayerEndOverlap(...)`：进入/离开重叠区域回调声明；是否筛选敌我、是否更新 `OverlapTriggerLinkTimeCharacters`，单凭声明未知。
- `StartTriggerGamePlayEffect()`：无参数函数声明，名字指向启动触发 GameplayEffect 的行为；内部效果未知。
- `TriggerAbsorbBegin(AHTAbilityCharacter*)` / `TriggerAbsorbEnd(AHTAbilityCharacter*)`：接收能力角色指针的开始/结束事件。
- `TriggerPeriodGamePlayEffectEvent()`：无参数周期触发事件声明。
- `TriggerStartShowActorEvent()`：无参数显示 Actor 相关事件声明。

## 2. `FHTGameplayEffectContainer` 字段布局

来源：`5.6.1/CppSDK/SDK/HTGame_structs.hpp`，Git blob `cc6781d70b782a0e3ec6cf06dfbd75f323f04a53`。

| 字段 | 偏移 / 大小 | 注释 |
|---|---|---|
| `TargetType` | `0x00 / 0x08` | `TSubclassOf<UHTGATargetType>`。 |
| `AddTargetType` | `0x08 / 0x08` | 另一 `UHTGATargetType` 子类引用。 |
| `NotFindToDoTargetType` | `0x10 / 0x08` | 第三个目标类型引用；条件语义不能仅从名字确定。 |
| `TargetGameplayEffectClasses` | `0x18 / 0x10` | `TArray<TSubclassOf<UGameplayEffect>>`。 |
| 总大小 | `0x28` | 与字段布局一致。 |

直接结论：Dump 定义了一个同时包含目标类型选择与 GameplayEffect 类数组的数据容器。它不说明应用哪个目标、由谁调用、如何处理效果，也不证明旧插件使用它。

## 3. `FSpawnDamageTriggerActor` 与数组配置

来源同上。

- `FSpawnDamageTriggerActor` 总大小 `0x70`。
- `SapwnActorClass`（保留原始拼写）@ `0x00`：`TSubclassOf<AHTAbilitySpawnActor>`。
- `SpawnConditionObjs` @ `0x08`：生成条件对象数组。
- `SpawnLocationObj` @ `0x18`、`SpawnRotationObj` @ `0x20`、`SpawnScaleObj` @ `0x28`：位置/旋转/缩放对象引用。
- `bForceSpawn` @ `0x30`、`bClientPredictionSpawn` @ `0x31`：两个布尔配置字段；行为需实现/元数据核验。
- `DamageTriggerConfigID` @ `0x34`、`RandomRadius` @ `0x3C`。
- `EffectContainer` @ `0x40`，占 `0x28`，与 `FHTGameplayEffectContainer` 布局吻合。
- `RandomCountCurve` @ `0x68`：`UCurveFloat*`。

`FSpawnDamageTriggerActorArray` 总大小 `0x28`：
- `SpawnArray` @ `0x00`：`TArray<FSpawnDamageTriggerActor>`。
- `nRandomSpawnCount` @ `0x10`：int32。
- `SelectTargetType` @ `0x18` 与 `SecondSelectTargetType` @ `0x20`：两个 `TSubclassOf<UHTGATargetType>`。
- `UHTAbilitySpawnDamageTriggerActor` 继承 `UHTAbilitySpawnActorEffect`，在 `0x28` 处持有 `FSpawnDamageTriggerActorArray SpawnDamageTriggerActorsInfo`（字段大小 `0x28`）。这将“生成 Actor 效果类”与上面的生成数组数据结构直接连接起来，但尚未证明它被伤害链旧插件调用。

## 4. `FDamageTriggerActorStaticData` 时序配置

- 继承 `FTableRowBase`，总大小 `0x58`。
- `DelaySpawnTime` @ `0x38`：名字指向延迟生成。
- `duration` @ `0x3C`：持续时间候选字段。
- `DelayDestoryTime` @ `0x40`：保留 Dump 拼写，名字指向延迟销毁。
- `TriggerOnSpawn` @ `0x44`：生成时触发的布尔配置候选。
- `DelayTriggerTime` @ `0x48`：延迟触发候选字段。
- `TriggerLinkTimeDuration` @ `0x4C`：持续链接时间候选字段。
- `Period` @ `0x50`：周期候选字段。
- `MaxTriggerCount` @ `0x54`：最大触发次数候选字段。

这些名字与前述运行状态字段形成可研究的时间/次数线索，但单位、数据表读取点、运行时赋值关系及计数更新规则均未证实。

## 5. `UFindSpawnLocationObj_Target` 目标定位字段

- 类继承 `UFindSpawnLocationObj`。
- `NotTargetSpawnBySelfLocation` @ `0x50`：布尔字段，名字对“目标位置 vs 自身位置”有约束意味，具体条件未知。
- `LocationOffset` @ `0x58` 与 `TargetLocationOffset` @ `0x70`：两个 FVector 偏移字段。
- `CustomTargetLocationOffset` @ `0x88`：自定义目标位置偏移对象。
- `UseToTargetDirection` @ `0x90`：布尔方向配置候选。
- `IncludeTargetCapsule` @ `0x91`：布尔目标胶囊相关配置候选。
- `PreLocationTime` @ `0x94`：float，名字提示预先位置时间；单位与用途未知。

## 6. 参数布局与生成包装的直接证据

来源：`5.6.1/CppSDK/SDK/HTGame_parameters.hpp`（blob `b839b1a326e90ad3ac1c598f6bbca6b6eee465e1`）与 `HTGame_functions.cpp`（blob `c0a5f1dfa3540ab656d18578950d02e4de522ef2`）。

- `DamageTriggerActor_OnPlayerBeginOverlap` 参数结构大小为 `0x128`：`OverlappedComponent` @ `0x00`，`OtherActor` @ `0x08`，`OtherComp` @ `0x10`，`OtherBodyIndex` @ `0x18`，`bFromSweep` @ `0x1C`，`SweepResult` @ `0x20`、大小 `0x108`。
- `DamageTriggerActor_OnPlayerEndOverlap` 参数结构大小为 `0x20`：前三个指针字段 @ `0x00/0x08/0x10`，`OtherBodyIndex` @ `0x18`，尾部 padding 4 字节。
- `DamageTriggerActor_OnCharacterDeadChanged` 参数结构大小为 `0x01`，只有 `bool IsDead` @ `0x00`。
- `DamageTriggerActor_TriggerAbsorbBegin/End` 各有一个 `AHTAbilityCharacter*` 参数 @ `0x00`，结构大小 `0x08`。
- 参数文件里没有单独的 `DamageTriggerActor_StartTriggerGamePlayEffect` 或 `DamageTriggerActor_TriggerPeriodGamePlayEffectEvent` 参数结构；这与类声明中二者均为无参数函数相符，但还需用函数元数据确认其反射布局。
- 生成包装 `ADamageTriggerActor::StartTriggerGamePlayEffect()` 的代码：静态缓存 `UFunction*`；首次时通过 `Class->GetFunction("DamageTriggerActor", "StartTriggerGamePlayEffect")` 查找；保存 `FunctionFlags`；将标志与 `0x400` 按位或；调用 `UObject::ProcessEvent(Func, nullptr)`；最后恢复原标志。
- 这段 `HTGame_functions.cpp` 代码是生成的反射调用包装，不是 `StartTriggerGamePlayEffect` 的原生实现体。它没有展示内部触发循环、计时、目标筛选或效果应用逻辑。不能仅凭这个 wrapper 断言调用安全、受 Anomaly 支持，或等同于旧插件的真实机制。

## 7. 当前机制假设分级

- **直接证实**：旧版 Dump 有周期触发 Actor；其字段包含目标重叠数组、周期/持续时间/计数状态；另有 GameplayEffect 类容器、触发 Actor 生成数据和时序配置。
- **有证据支持但未验证**：这些原生声明可作为“持续/周期伤害触发”候选研究方向。
- **未知**：目标插件是否使用这条路径；它是否自动重复同一个效果；首次受击后如何建立目标关系；多个目标是否有独立实例/计时；目标死亡/离开重叠区时如何清理；哪些函数由游戏实际调用；Anomaly 是否有可安全桥接的已支持接口。
- **阻塞**：`5.6.1-new/` 的 LFS 实体正文目前未读取，故不能声称两版已完成差异比较。
