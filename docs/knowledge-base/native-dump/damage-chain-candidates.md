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
- 重要限制：这些 `*_functions.cpp` 方法是生成的反射调用包装，内部调用 `ProcessEvent`；它们不包含游戏原生函数的真实业务实现。因此不能从包装器推导真实定时器如何启动、命中时怎样选目标、何时停止。

**候选 B：`UHTAbilitySpawnDamageTriggerActor` 与配置数组组合。**

- 类声明：`HTGame_classes.hpp:35020-35037`。
- 字段 `SpawnDamageTriggerActorsInfo` 类型为 `FSpawnDamageTriggerActorArray`，offset `0x28`，大小 `0x28`。
- 结构定义位于 `HTGame_structs.hpp` 的 `FSpawnDamageTriggerActor`（约行 21560，需以当前文件定位结果为准）与 `FSpawnDamageTriggerActorArray`（约行 16500? 不使用估算行号；按类型名检索）。结构包含 Actor 子类、spawn condition/location/rotation/scale 对象、`DamageTriggerConfigID`、`FHTGameplayEffectContainer`、`RandomCountCurve`；数组还包含 `SpawnArray`、`nRandomSpawnCount`、`SelectTargetType`、`SecondSelectTargetType`。
- `FHTGameplayEffectContainer` 大小 `0x28`，成员包括 `TargetType`、`AddTargetType`、`NotFindToDoTargetType` 与 `TargetGameplayEffectClasses`（`HTGame_structs.hpp` 中 ScriptStruct 段落）。
- `FDamageTriggerActorStaticData`（继承 `FTableRowBase`，总大小 `0x58`）包含 `DelaySpawnTime`、`duration`、`DelayDestoryTime`、`TriggerOnSpawn`、`DelayTriggerTime`、`TriggerLinkTimeDuration`、`Period`、`MaxTriggerCount`，并含 spawn offset/rotation。
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
