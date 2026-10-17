# 伤害链候选原生机制：Dump 正文初读

> 来源：`5.6.1/CppSDK/SDK/HTGame_classes.hpp`（blob `526091ab6679ac6833634e5a6d08d807e90dbf0c`）与 `HTGame_structs.hpp`（blob `cc6781d70b782a0e3ec6cf06dfbd75f323f04a53`）。本页记录 Dump 声明，不证明旧插件采用此路径。

## 已读声明：`ADamageTriggerActor`

**直接证实（Dump 声明）**
- `class ADamageTriggerActor : public AHTAbilitySpawnActor`。
- `OverlapTriggerLinkTimeCharacters` 为 `TArray<AHTPlayerCharacter*>`，偏移 `0x05A8`。
- 私有瞬态字段：`RuntimeTriggerPeriod`（float，`0x05E8`）、`RuntimeTriggerDuration`（float，`0x05EC`）、`RuntimeTriggerCountPerPeriod`（int32，`0x05F0`）、`RuntimeMaxTriggerCount`（int32，`0x05F4`）。
- 类注释总大小 `0x0618`，基类布局大小 `0x04A8`，派生类增量 `0x0170`。
- 函数声明包括：`K2_OnTriggerPeriodGamePlayEffect`、`OnCharacterDeadChanged`、`OnPlayerBeginOverlap`、`OnPlayerEndOverlap`、`StartTriggerGamePlayEffect`、`TriggerAbsorbBegin/End`、`TriggerPeriodGamePlayEffectEvent`。
- 这些声明支持“有重叠目标集合及周期/持续时间/每周期次数/最大次数状态”的结论；函数名不能证明其内部逻辑。

## 已读声明：`FHTGameplayEffectContainer`
- 总大小 `0x28`。
- `TargetType`、`AddTargetType`、`NotFindToDoTargetType` 是三个 `TSubclassOf<UHTGATargetType>`，偏移分别为 `0x00`、`0x08`、`0x10`。
- `TargetGameplayEffectClasses` 为 `TArray<TSubclassOf<UGameplayEffect>>`，偏移 `0x18`、大小 `0x10`。
- 证明存在描述目标类型与 GameplayEffect 类集合的数据结构；不能推出应用顺序、应用者或实际目标。

## 已读声明：`FSpawnDamageTriggerActor`
- 总大小 `0x70`。
- `SapwnActorClass`（保留 Dump 原始拼写）为 `TSubclassOf<AHTAbilitySpawnActor>`，偏移 `0x00`。
- 生成条件数组、生成位置/旋转/缩放对象位于 `0x08`、`0x18`、`0x20`、`0x28`。
- `DamageTriggerConfigID` 在 `0x34`；`EffectContainer` 在 `0x40`、大小 `0x28`；`RandomCountCurve` 在 `0x68`。
- 证明存在“生成伤害触发 Actor + 效果容器”的数据描述，但不证明它与旧插件有关。

## 已读声明：`FDamageTriggerActorStaticData`
- 继承 `FTableRowBase`，总大小 `0x58`（含基类布局）。
- `DelaySpawnTime`（`0x38`）、`duration`（`0x3C`）、`DelayDestoryTime`（`0x40`）、`TriggerOnSpawn`（`0x44`）、`DelayTriggerTime`（`0x48`）、`TriggerLinkTimeDuration`（`0x4C`）、`Period`（`0x50`）、`MaxTriggerCount`（`0x54`）。
- 字段名支持存在延迟生成、延迟触发、持续时间、周期和最大次数配置；真实语义、单位、消费者和字段关系仍需核验。

## 继续读取
- 完整核验 `UHTAbilitySpawnDamageTriggerActor`、`FSpawnDamageTriggerActorArray`、`UFindSpawnLocationObj_Target`。
- 读取相关 `HTGame_parameters.hpp`、`HTGame_functions.cpp` 和 `Dumpspace/FunctionsInfo.json`，核对 owner、参数 ABI、返回值布局。
- 新版真实 LFS 正文仍未取得，故不做字段差异结论。

## 状态
- **直接证实**：Dump 存在周期触发 Actor、目标集合、GameplayEffect 类容器和周期/延迟/最大次数字段。
- **有证据支持但未验证**：这些类型是周期伤害机制的研究候选。
- **未知**：旧插件是否使用此路线、触发条件、目标绑定、独立实例语义，以及能否由受支持 Anomaly ABI 安全桥接。
