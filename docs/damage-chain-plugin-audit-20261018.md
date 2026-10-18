# Damage Chain Plugin Audit — 2026-10-18

## Scope

- Repository: `Rabb1t79/Anomaly-main`
- Dump source branch: `fix/restore-attack-work-and-repair-20261016`
- Plugin repair branch: `fix/damage-chain-plugin-ui-guards-20261018`
- Plugin path: `plugins/DamageEcho/plugin.cpp`
- This branch is an entry-guarded diagnostic build, not a verified recreation of the recovered legacy DLL.

## Dump cross-check for the hard-coded address

The user-supplied code tried to hook `HTGame.exe + 0x42D5E50` as if it were a direct C++ function with the five-argument `ApplyDamage` signature.

### What the repository's 5.6.1 Dump does confirm

1. `5.6.1/CppSDK/SDK/Engine_classes.hpp`, Blob SHA `d8e3399fc3e723159a9a4c86672c51150d467164`, declares:
   `UGameplayStatics::ApplyDamage(AActor* DamagedActor, float BaseDamage, AController* EventInstigator, AActor* DamageCauser, TSubclassOf<UDamageType> DamageTypeClass)` returning `float`.
2. `5.6.1/CppSDK/SDK/Engine_parameters.hpp`, Blob SHA `44869f6561b6a8af1a7d3ccb6f2887f4c0fd104c`, defines the reflected parameter layout:
   - `DamagedActor`: offset `0x00`
   - `BaseDamage`: offset `0x08`
   - padding: offset `0x0C`
   - `EventInstigator` (`AController*`): offset `0x10`
   - `DamageCauser` (`AActor*`): offset `0x18`
   - `DamageTypeClass`: offset `0x20`
   - `ReturnValue`: offset `0x28`
3. `5.6.1/CppSDK/SDK/Engine_functions.cpp`, Blob SHA `65714c47053096aeeb1da5fb51641834bea75584`, shows the generated wrapper resolving the reflected function with `StaticClass()->GetFunction("GameplayStatics", "ApplyDamage")`, filling the parameter struct, invoking `GetDefaultObj()->ProcessEvent(Func, &Parms)`, restoring function flags, and returning `Parms.ReturnValue`.
4. `5.6.1/Dumpspace/OffsetsInfo.json`, Blob SHA `32f04a7dc01a06eef454e11c0e53bea286c70446`, lists global offsets for GObjects, AppendString, GNames, GWorld and ProcessEvent plus the ProcessEvent index. It does not list a machine-code RVA for `UGameplayStatics::ApplyDamage`.

### What remains unverified

- No inspected 5.6.1 Dump entry maps `0x42D5E50` to `UGameplayStatics::ApplyDamage`.
- The SDK's generated wrapper signature is not evidence that `HTGame.exe + 0x42D5E50` has that callable ABI. Checking whether the address lies on an executable page cannot establish function identity or calling convention.
- The generated SDK wrapper is a reflected `ProcessEvent` call; the inspected public `AnomalyUe5ProcessEventServiceV1` only exposes subscribe/unsubscribe callbacks, not a general invocation function. The reviewed public NTE actor/combat services are observation/snapshot-oriented and do not expose a DamageTriggerActor spawn/configure entry point.
- `ADamageTriggerActor` and its periodic GameplayEffect-related fields/method declarations exist in the 5.6.1 Dump, but the old DLL's relationship to that system, its configuration values, and its exact first-target/multi-target semantics remain unproven.

## Code correction

The previous implementation was unsafe to treat as a working feature. The code directly called the unverified address using a guessed function-pointer type, retained raw UObject pointers in a repeat task queue, and could keep rescheduling damage without confirming that the target function and ABI matched the game.

The repaired source now:
- Does not install a hook on `0x42D5E50` and does not make direct damage calls.
- Performs only a memory-page sanity check on that candidate, clearly labelled as such; page executability is not treated as proof of function identity.
- Keeps a Chinese UI for entry status, a preset interval, a blocked-start diagnostic, and bounded log export. The interval control is informational until a supported/verified invocation route exists.
- Exports logs to the Windows temporary directory and records why the hook was not installed.
- Aligns source and manifest version at `1.5.0`, with only the UI capability declared because this guarded build does not use the Hook service.

## Verification boundary

- A successful DLL build validates compilation and packaging only.
- This guarded build intentionally does not perform chain damage. It must not be presented as a working legacy-mechanism recreation.
- The next implementation step needs either a 5.6.1/profile-validated function entry with a matching ABI, or a supported Anomaly Core service that safely resolves and invokes the appropriate reflected/game-native behavior on the Game thread. Do not invent a signature pattern or re-enable the raw offset based only on an executable-page check.


## Build/package check

- The dedicated `Build Damage Echo MSVC` workflow completed successfully: run `39091653214`, source commit `27b50906b1b45a18a171718d665deec3bb498bd2`. This verifies compilation and artifact creation for the guarded source/manifest; it is not an in-game feature test.
- The downloaded artifact was normalized to a flat plugin package containing only `plugin.dll` and `manifest.json`; ZIP integrity passed, the manifest parses as JSON at version `1.5.0`, the DLL is PE32+ x64, and it exports `AnomalyPluginEntryV1`.
- Flat package SHA-256: `09a69b67f2999f73d1391e5b6e273d96ec538b341ecd9f5502e9556d7f720290`.
- DLL SHA-256: `cdaf1142c976a8a95d5885cc753e74154f5dd1bfdf6a402e4af91161fda77310`.
- No in-game test has been run; the entry guard intentionally prevents the chain-damage loop from starting.


## 2026-10-18 深入核验：DamageTriggerActor 配置链与受支持调用边界

### Dump 直接证据

来源：当前分支 `5.6.1/CppSDK/SDK/` 中实际读取的 Dump 文件。此处结论只涉及类型、字段与包装声明，不等同于旧插件实际使用该路线。

- `HTGame_classes.hpp`：`ADamageTriggerActor : public AHTAbilitySpawnActor`，类大小 `0x618`。重要字段包括：
  - `SpawnConfigID` 位于 `0x4D0`，`fTriggerEffectRadius` 位于 `0x4E0`。
  - `OverlapTriggerLinkTimeCharacters` 位于 `0x5A8`，类型为 `TArray<AHTPlayerCharacter*>`。
  - `RuntimeTriggerPeriod`、`RuntimeTriggerDuration`、`RuntimeTriggerCountPerPeriod`、`RuntimeMaxTriggerCount` 分别位于 `0x5E8`、`0x5EC`、`0x5F0`、`0x5F4`。
  - 声明 `OnPlayerBeginOverlap`、`OnPlayerEndOverlap`、`StartTriggerGamePlayEffect`、`K2_OnTriggerPeriodGamePlayEffect` 与 `TriggerPeriodGamePlayEffectEvent`。
- 同一文件：父类 `AHTAbilitySpawnActor` 持有 `EffectContainer`（`0x3A8`）、`EventTag`（`0x3D0`）、`EventData`（`0x3D8`）、`TriggerAbilityHandle`（`0x488`）和能力相关缓存。触发器的生效配置因此不能只根据周期字段推断。
- 同一文件：`UHTAbilitySpawnDamageTriggerActor` 继承 `UHTAbilitySpawnActorEffect`，其中 `SpawnDamageTriggerActorsInfo` 是 `FSpawnDamageTriggerActorArray`。该结构包含 spawn array、随机生成数量以及主/次目标类型。
- `HTGame_structs.hpp`：`FSpawnDamageTriggerActor` 包含 Actor 子类、生成条件、位置/旋转/缩放对象、`bForceSpawn`、`bClientPredictionSpawn`、`DamageTriggerConfigID`、随机半径、GameplayEffect 容器和随机数量曲线。关联的 `FDamageTriggerActorStaticData` 包含 `DelaySpawnTime`、`duration`、`DelayDestoryTime`、`TriggerOnSpawn`、`DelayTriggerTime`、`TriggerLinkTimeDuration`、`Period`、`MaxTriggerCount`。
- `BP_DamageTriggerActorBase_classes.hpp` 中的 `ABP_DamageTriggerActorBase_C` 只声明继承自 `ADamageTriggerActor`，未增加成员或方法。此头文件并未提供蓝图资产的默认属性值或事件图实现。
- `HTGame_functions.cpp` 中 `StartTriggerGamePlayEffect`、`K2_OnTriggerPeriodGamePlayEffect` 等生成包装使用反射 `ProcessEvent`；这些包装没有显示周期触发的实际业务实现、具体效果容器、目标选择或目标丢失后的行为。

### 对目标效果的判定

| 项目 | 证据级别 | 结论 |
|---|---|---|
| 原生类具备周期 GameplayEffect 相关状态和方法 | 直接证实（声明层） | 是可继续研究的候选 |
| 原生体系存在数据驱动的生成配置、目标类型和效果容器 | 直接证实（声明层） | 不能把它简化为只创建 Actor 并填写计时字段 |
| 一个实例可以重复命中初次受伤的同一目标 | 未证实 | 需要确认目标选择类、生成位置策略和触发事件实现 |
| 每个受击目标拥有独立计时/计数，互不重置 | 未证实 | Actor 有实例级字段不等于已证明其计时/多目标语义 |
| 旧插件确实通过此原生路径实现视频中的效果 | 未知 | 需要与目标 DLL 控制流或实机调用记录交叉验证 |

### ABI/Core 桥接判定

- `include/anomaly/sdk/services/ue5.h` 中公开的 `anomaly.ue5.process-event` 是订阅/取消订阅接口；`anomaly.ue5.objects` 是快照和精确查找接口。它们没有向插件开放通用反射调用或 Actor 创建。
- `src/game/ue5/ue5_process_event.cpp` 中的 `CreateUe5ProcessEventInvoker` 依赖 `ValidateProcessEventAbi`，说明 Core 有已校验的内部调用原语；但这个内部函数本身不是可由插件直接调用的 ABI。
- 仓库其他 Core 路径中的延迟 Actor 生成是带类型和后置条件的专用调用实例，不应被视为任意 Actor 都能安全生成的通用入口。

### 实现门禁

在继续启用功能前，至少需要有证据确定：要生成的准确类及其配置 ID、`EffectContainer` 和 TargetType 的来源、如何把初次受击目标传给原生触发器、生成/周期回调的 Game 线程和对象生命周期，以及关闭/死亡/离开目标范围后的停止规则。随后才能设计窄范围、具名且有 capability 的 Core/ABI 桥接。

当前插件继续保持 Entry Guarded：不启用 `HTGame.exe + 0x42D5E50`，不通过手写偏移改写 Runtime 计时字段，也不因发现 `ADamageTriggerActor` 就直接创建实例或调用其周期方法。此决定不否定原生候选，只避免在类配置和目标语义未验证时制造伪实现。


## 2026-10-18 原生能力生成链候选：EventData 与 SpawnActorMaps

本节在上一节的 `ADamageTriggerActor` 声明检查之上，补充更具体的“能力配置 → 生成 Actor 效果 → 目标 EventData”候选连接。仍然是 Dump/对象清单证据，不证明旧插件使用该路径。

### 直接可核验的连接

- `HTGame_classes.hpp`（`UHTGameplayAbility`，约第 33584 行）声明 `SpawnActorMaps: TMap<FGameplayTag, UHTAbilitySpawnActorEffect*>`，字段偏移 `0x5A0`；另有 `EffectContainerMap: TMap<FGameplayTag, FHTGameplayEffectContainer>`，偏移 `0x5F0`。这为按 GameplayTag 索引能力的 Actor 生成效果及 GameplayEffect 容器提供了直接类型证据。
- 同类声明 `K2_SpawnAbilityActorsEffect(const FGameplayTag& ContainerTag, const FGameplayEventData& EventData)`（约第 33695 行）。`HTGame_functions.cpp` 中有对应的反射包装器，使用 `GetFunction("HTGameplayAbility", "K2_SpawnAbilityActorsEffect")` 和 `ProcessEvent`；它是调用包装，不是实际生成逻辑实现。
- `HTGame_parameters.hpp` 为该调用给出 `HTGameplayAbility_K2_SpawnAbilityActorsEffect` 参数结构，总大小 `0xB8`：`ContainerTag` 在 `0x0`，`FGameplayEventData` 在 `0x8`。
- `GameplayAbilities_structs.hpp` 中 `FGameplayEventData` 大小为 `0xB0`，包含 `EventTag`（`0x0`）、`Instigator`（`0x8`）、`Target`（`0x10`）、`ContextHandle`（`0x28`）和 `TargetData`（`0x88`）。因此，该原生 API 的数据模型能够携带目标/攻击者及 TargetData；这并不证明运行时调用会把受伤目标填入这些字段。
- `HTGame_classes.hpp` 中 `UHTGATargetType::GetTargets(OwningActor, TargetingActor, EventData, ...)` 接收 EventData；Dump 还声明 `UHTGATargetType_UseEventData`、`UHTGATargetType_UseOwner`、`UHTGATargetType_AimingResult` 等目标类型，以及 `UFindSpawnLocationObj_Target` 的 `TargetLocationOffset`、`UseToTargetDirection`、`IncludeTargetCapsule` 和 `PreLocationTime`。这是“目标数据参与原生目标解析/生成定位”的直接声明证据，不足以证明它会锁定初次命中的同一目标。
- `GObjects-Dump.txt` 中有实际对象路径证据：`DataTable DamageTriggerActorDataTable.DamageTriggerActorDataTable`（第 199415 行）；`HTAbilitySpawnDamageTriggerActor` 被挂在 `GA_Lacrimosa_MeleeModA`、`GA_Lacrimosa_PerfectEvade`、`GA_Shinku_Skill_Rage`、`GA_Female051_Melee`、`GA_Trigger_Kill_50/100/150` 等能力资产下。部分实例具有 `FindSpawnLocationObj_Target`，部分则是 `FindSpawnLocationObj_Owner`。对象清单只列对象类型和路径，不包含组件属性值、DataTable 行内容或 Blueprint 事件图。

### 这条路径为什么还不能直接执行

`K2_SpawnAbilityActorsEffect` 是能力对象上的可变更调用，按 `ContainerTag` 读取该能力自身的 `SpawnActorMaps`，可能生成不同类型的 Actor。直接调用它不是“只生成 DamageTriggerActor”的保证，更不是普通伤害事件的等价替代。如果容器 Tag 不正确、能力对象不是正确的活动实例、EffectData/TargetData 不完整或其 Owner/World 生命周期已结束，结果都未被当前 Dump 声明所解决。

### 下一步验证方案

在增加任何会生成 Actor 或触发效果的 Core/ABI 之前，先做**只读运行时观测**，用正常战斗观察以下反射函数是否出现、调用顺序和目标数据是否吻合：

1. `HTGameplayAbility.K2_SpawnAbilityActorsEffect`
2. `DamageTriggerActor.StartTriggerGamePlayEffect`
3. `DamageTriggerActor.K2_OnTriggerPeriodGamePlayEffect` / `TriggerPeriodGamePlayEffectEvent`

现有 `anomaly.ue5.process-event` 订阅回调只向插件提供原始 UObject/UFunction 指针和临时参数缓冲，并不提供已解析的函数路径或安全的目标对象句柄；仅在插件内猜读 UObject 内存不是合规替代。因此，观测工具若需要对这些函数按名称筛选并解码 EventData.Target，应新增**只读、具名、白名单式的反射事件观测能力**，由 Core 使用其已具备的 Profile/ABI 校验和对象/属性解析能力完成路径匹配与必要字段解析；不要新增无约束反射调用器。先验证观察到的原生路线，再决定是否有必要建立单独的具名生成桥接。

证据状态更新：
- `HTGameplayAbility` 存在按 Tag 配置的 Actor Effect 与 `K2_SpawnAbilityActorsEffect`：直接证实（声明/包装层）。
- `FGameplayEventData` 能携带 Instigator/Target/TargetData：直接证实（布局层）。
- 普通伤害事件之后旧游戏逻辑会调用该方法，且会选择 `ADamageTriggerActor`：未知。
- 用该方法能实现持续命中同一受击目标并让多目标独立：未知。
- 本轮仍未修改插件/Core/ABI 实现、未构建或实机测试；只在证据文档中进一步收窄了下一步观测路径。
