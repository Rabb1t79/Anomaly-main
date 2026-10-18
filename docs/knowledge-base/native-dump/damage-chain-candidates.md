# 伤害链式反应：Dump 候选机制笔记（5.6.1）

> 来源限定：仅依据仓库 `5.6.1/CppSDK/SDK/HTGame_classes.hpp`、`HTGame_structs.hpp`、`HTGame_functions.cpp` 当前内容。本文记录声明级证据，不宣称目标旧插件已经使用此机制。所有行号均针对本次读取的 blob；对应 SHA 见索引。

## 初步结论

**候选 A：`ADamageTriggerActor` 的周期 GameplayEffect 触发与目标重叠跟踪。**

- 类声明：`5.6.1/CppSDK/SDK/HTGame_classes.hpp:20942-21012`。
- 基类：`AHTAbilitySpawnActor`，声明大小 `0x0170`，总大小 `0x0618`。
- 与周期/时序相关的运行字段：
  - `RuntimeTriggerPeriod`，offset `0x05E8`，float。
  - `RuntimeTriggerDuration`，offset `0x05EC`，float。
  - `RuntimeTriggerCountPerPeriod`，offset `0x05F0`，int32。
  - `RuntimeMaxTriggerCount`，offset `0x05F4`，int32。
- 与目标/重叠跟踪相关的字段：
  - `OverlapTriggerLinkTimeCharacters`，`TArray<AHTPlayerCharacter*>`，offset `0x05A8`。
  - `OverlapTriggerLinkTimeComp`，`UShapeComponent*`，offset `0x05C8`。
- 相关函数声明：`K2_OnTriggerPeriodGamePlayEffect`、`OnCharacterDeadChanged`、`OnPlayerBeginOverlap`、`OnPlayerEndOverlap`、`StartTriggerGamePlayEffect`、`TriggerPeriodGamePlayEffectEvent`。
- 生成包装函数位置：`HTGame_functions.cpp:63067`（周期触发事件）、`:63121`（死亡状态变化）、`:63146`（开始重叠）、`:63181`（结束重叠）、`:63212`（StartTriggerGamePlayEffect）、`:63271`（周期事件）。
- 重要限制：这些 `*_functions.cpp` 方法是生成的反射调用包装，内部调用 `ProcessEvent`；它们不包含游戏原生函数的真实业务实现。因此不能从包装器推导真实定时器如何启动、命中时怎样选目标、何时停止。`OverlapTriggerLinkTimeCharacters` 的元素类型是 `AHTPlayerCharacter*`，可能表示进入触发区域的玩家角色；不能将它直接解释成“原始受击敌人列表”。这使候选 A 更像具备周期/区域触发能力的 Actor，而不是已证实的伤害后续链式机制。

**候选 B：`UHTAbilitySpawnDamageTriggerActor` 与配置数组组合。**

- 类声明：`HTGame_classes.hpp:35020-35037`。
- 字段 `SpawnDamageTriggerActorsInfo` 类型为 `FSpawnDamageTriggerActorArray`，offset `0x28`，大小 `0x28`。
- 结构定义位于 `HTGame_structs.hpp` 的 `FSpawnDamageTriggerActor`（`HTGame_structs.hpp:21560`）与 `FSpawnDamageTriggerActorArray`（`HTGame_structs.hpp:78759`）。结构包含 Actor 子类、spawn condition/location/rotation/scale 对象、`DamageTriggerConfigID`、`FHTGameplayEffectContainer`、`RandomCountCurve`；数组还包含 `SpawnArray`、`nRandomSpawnCount`、`SelectTargetType`、`SecondSelectTargetType`。
- `FHTGameplayEffectContainer` 大小 `0x28`，成员包括 `TargetType`、`AddTargetType`、`NotFindToDoTargetType` 与 `TargetGameplayEffectClasses`（`HTGame_structs.hpp:21548`）。
- `FDamageTriggerActorStaticData`（`HTGame_structs.hpp:78809`；继承 `FTableRowBase`，总大小 `0x58`）包含 `DelaySpawnTime`、`duration`、`DelayDestoryTime`、`TriggerOnSpawn`、`DelayTriggerTime`、`TriggerLinkTimeDuration`、`Period`、`MaxTriggerCount`，并含 spawn offset/rotation。
- 这些配置字段与“延迟生成、持续时长、周期触发、最大触发次数、按目标选择”的目标模型有结构上的对应关系，但**目前没有证据证明它们就是视频所示插件采用的机制**。

## 继承/配置连接线索

```text
UHTAbilitySpawnDamageTriggerActor
  └─ SpawnDamageTriggerActorsInfo : FSpawnDamageTriggerActorArray
       ├─ SpawnArray : FSpawnDamageTriggerActor[]
       │    ├─ SapwnActorClass : AHTAbilitySpawnActor subclass
       │    ├─ SpawnConditionObjs[]
       │    ├─ SpawnLocationObj / SpawnRotationObj / SpawnScaleObj
       │    ├─ DamageTriggerConfigID
       │    └─ EffectContainer : FHTGameplayEffectContainer
       │         ├─ TargetType / AddTargetType / NotFindToDoTargetType
       │         └─ TargetGameplayEffectClasses[]
       ├─ nRandomSpawnCount
       ├─ SelectTargetType
       └─ SecondSelectTargetType

ADamageTriggerActor : AHTAbilitySpawnActor
  ├─ EffectContainer : FHTGameplayEffectContainer (继承自基类，offset 0x03A8)
  ├─ EventTag / EventData / TriggerAbilityHandle
  ├─ OverlapTriggerLinkTimeCharacters[]
  └─ RuntimeTriggerPeriod / Duration / CountPerPeriod / MaxTriggerCount
```

## 对目标表现的匹配度（暂定）

| 目标观察 | Dump 支持程度 | 限制 |
|---|---|---|
| 伤害相关效果可按周期触发 | **直接证实：存在字段/函数声明** | 未证实其触发实现与视频一致 |
| 可保存/跟踪一组重叠的玩家角色对象 | **直接证实：字段声明** | 字段名写的是 `AHTPlayerCharacter*`，不等于确认绑定的是最初受击敌人；语义要从蓝图/原生实现进一步确认 |
| 触发次数/持续时间可配置 | **直接证实：字段声明** | 运行字段和静态数据字段不一定直接一一对应 |
| 按目标类型构造 GameplayEffect 容器 | **直接证实：结构声明** | 未证实当前目标插件使用这些配置 |
| 多个目标实例相互独立、不互相重置 | **未知** | 需要实例所有权/生成调用/生命周期证据 |
| 伤害事件会自动启动该 Actor | **未知** | 当前类声明不包含事件订阅关系证明 |
| 该机制就是旧 DLL 的实际机制 | **未知** | 旧 DLL 按用户要求暂时后置 |

## 下一步取证

1. 读取 `HTGame_parameters.hpp` 中以上函数的精确参数结构，核对反射参数布局。
2. 在 `HTGame_classes.hpp` / `HTGame_functions.cpp` 中检索 `HTAbilitySpawnDamageTriggerActor`、`DamageTriggerConfigID`、`TriggerLinkTimeDuration` 与调用它们的相关类，建立声明关系图。
3. 查找是否存在可读的蓝图资产/配置表或其他随 Dump 提供的元数据，确认静态数据实际如何被消费；若当前仓库不含实现体，明确列为证据缺口。
4. 检查 `5.6.1_new/` 的实际来源。当前指定分支递归树没有此目录；也在 `main`、`recreation/damage-chain-native-20261017`、`recovery/analyze-old-plugin-dll-20261017`、`experiment/damage-chain-legacy-ui-20261017` 与 `feat/damage-chain-reference-ui-20261017` 的树中未找到它。不能因此推断用户本地或其他来源不存在。
5. 在两套版本的代码与配置都核验之前，不开始改实现。


## 参数布局补充（5.6.1）

来源文件：`5.6.1/CppSDK/SDK/HTGame_parameters.hpp`。

- `DamageTriggerActor_OnCharacterDeadChanged`：起始行 20415，参数结构大小 `0x1`；`IsDead` 位于 offset `0x0`，类型 `bool`。
- `DamageTriggerActor_OnPlayerBeginOverlap`：起始行 20424，参数结构大小 `0x128`；`OverlappedComponent` offset `0x0`、`OtherActor` offset `0x8`、`OtherComp` offset `0x10`、`OtherBodyIndex` offset `0x18`、`bFromSweep` offset `0x1C`、`SweepResult` offset `0x20`（大小 `0x108`）。
- `DamageTriggerActor_OnPlayerEndOverlap`：起始行 20439，参数结构大小 `0x20`；包含 OverlappedComponent、OtherActor、OtherComp、OtherBodyIndex。
- `StartTriggerGamePlayEffect`、`K2_OnTriggerPeriodGamePlayEffect` 与 `TriggerPeriodGamePlayEffectEvent` 的类声明为无显式参数函数；声明本身不能证明函数由何种事件调用。
- `FHTAbilityActorFun_SpawnActors` 位于 `HTGame_structs.hpp:39191`，大小 `0x80`（含基类部分），包含 `SpawnCmdScheduler`、`SpawnActorClass`、`DamageTriggerConfigID`、`EventTag`、`EffectContainer`、`bIsRelateToAbility`。这是另一条“技能扩展结构配置生成 Actor”的结构线索，尚未证明它与 `UHTAbilitySpawnDamageTriggerActor` 在实际调用路径上相连。

## 候选等级调整

- **候选 B（生成配置结构）**：与“生成 Actor + 目标类型 + GameplayEffect 容器 + 配置 ID”的结构对应更直接；目前属于**有证据支持但未验证**。
- **候选 A（周期/区域触发 Actor）**：周期字段与目标表现有部分对应，但 overlap 跟踪字段保存的是 `AHTPlayerCharacter*`，且没有原生实现体证明它绑定最初受击的敌人；当前应视为**待验证的邻近机制**，不能优先当作最终方案。
- 要把任一候选提升为实现路线，仍需查清配置数据的实际消费者、目标选取实现与 Actor 创建/销毁生命周期。Dump 的生成函数包装不能替代这些证据。

## 2026-10-18 双版本证据对照结果

- 扩展报告 [39125616938](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125616938) 已成功读取两版实际 LFS 对象。15 个聚焦块均成功提取，缺失标记为 0，15/15 块文本一致。
- 与当前候选路径直接相关的声明包括：`ADamageTriggerActor` 周期字段 `RuntimeTriggerPeriod`/`RuntimeTriggerDuration`/`RuntimeTriggerCountPerPeriod`/`RuntimeMaxTriggerCount` 的偏移分别为 `0x5E8/0x5EC/0x5F0/0x5F4`；基类 `AHTAbilitySpawnActor` 的 `EffectContainer`/`EventData` 偏移为 `0x3A8/0x3D8`；`OverrideEffectContainer` 接收一个大小 `0x28` 的 `FHTGameplayEffectContainer`。
- 完整 classes、structs 与 GObjects 文件哈希在两个版本间仍不同；focused diff 匹配不能证明该周期事件的运行语义，也不能证明手动触发会造成一次真实伤害。仍需 Core 反射读回与实机伤害验证。

## 2026-10-18 补充布局证据：FGameplayEventData

- 当前 Core 适配器还会验证 `AHTAbilitySpawnActor.EventData` 的内层结构：`FGameplayEventData` 总大小 `0xB0`，`Instigator` offset `0x08`，`Target` offset `0x10`。这些值可在 `5.6.1/CppSDK/SDK/GameplayAbilities_structs.hpp` 中找到，但之前自动化双版本报告没有包含该头文件。
- 已将旧版与新版 `GameplayAbilities_structs.hpp` 一并纳入 LFS 取证，并将 `FGameplayEventData` 加入 focused block 对照。新报告返回后需核对提取成功与字段布局差异，不能只依靠旧版声明决定适配器兼容性。
## 最新补充核验结果：FGameplayEventData（2026-10-18）

- 双版本证据任务 [39126184168](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39126184168) 成功；最新版报告共有 16 个聚焦块，16 个全部成功提取且文本一致，零缺失标记。
- `FGameplayEventData` 在两版均为 `0xB0`，`Instigator` offset `0x08`，`Target` offset `0x10`；对应 `GameplayAbilities_structs.hpp` 文件大小及 SHA-256 也完全相同。
- 这些结果支持 Core 当前的内层字段布局检查，但不证明 `K2_OnTriggerPeriodGamePlayEffect` 的运行时业务语义，也不证明其调用一次就会对原始受击目标造成一次真实伤害。
## 2026-10-18 目标类型声明补充

- 当前适配器选择 `HTGATargetType_UseEventData` 作为 GameplayEffect 容器目标类型，且通过反射验证其父类为 `HTGATargetType`。这两个类声明尚未列入前一轮聚焦对照。
- 新增两版 Dump 的目标类型类块提取项；需等后续报告验证它们是否存在以及声明是否相同，再提升对应证据等级。