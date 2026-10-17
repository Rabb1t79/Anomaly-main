# Attack 原生伤害链：Dump 证据与观测阶段工作记录

日期：2026-10-18  
仓库：`Rabb1t79/Anomaly-main`  
基线分支：`fix/restore-attack-work-and-repair-20261016`  
本工作分支：`work/attack-dump-evidence-observer-20261018`

## 本轮范围

旧 DLL 解密/密钥恢复降为次要任务。当前主线是依据用户提供的 NTE 5.6.1 原生 Dump、Anomaly 当前接口与 Attack 插件源码，验证伤害事件到 GameplayEffect 执行的真实关系。本文只记录可追溯证据与观测计划；不把文档里的候选函数视为已经发生的实机调用，也不将其直接加入可重放白名单。

## 证据来源和状态

- 已读取仓库分支 `docs/developer-guide/attack-replay-legacy-port.md`、`profiles/nte/nte-current.json`、`src/game/ue5/ue5_nte_adapter.cpp`、`plugins/NteAttackReplay/plugin.cpp`。
- 已读取项目 Library 中既有审计记录 `5.6.1-Native-SDK-Path-Audit.md` 和 `5.6.1全量遍历_内容核验进展_20261017.md`。既有审计记录声明输入 Dump SHA-256 为 `3fdcd63483cf40967dcb701d04f2c58bf6889cb4e5dd75c8e0cdbc55e407b779`。
- 本轮已直接读取仓库 `5.6.1/CppSDK/SDK/HTGame_classes.hpp`、`HTGame_structs.hpp`、`HTGame_parameters.hpp`，以及 `GameplayAbilities_classes.hpp`、`GameplayAbilities_parameters.hpp`，逐项复核下列候选的声明和参数结构。PR 的基线分支与当前工作分支根目录均只有 `5.6.1`，未发现名为 `5.6.1_new` 的目录。默认分支 `main` 另有 `5.6.1-new`（连字符）目录，但本轮读取到其中相关文件是 Git LFS 指针文本，而非完整 Dump 内容，因此没有把它当作可用的第二份 Dump。
- 当前源码可直接确认：Core 现有可重放捕获绑定只接受四类已验证入口；`CombatExecFunctionTargets()` 也只从这四个绑定中取 Native Exec 目标。其余候选路径目前没有进入这套捕获/回放绑定。
- 既有审计记录确认：之前的实机日志曾出现候选调用无法同时匹配 `DamageGEDef`、攻击者/目标 ASC 与时间窗口的情况。它不能证明现有路径正确，也不能证明某个未观测候选就是正确路径。

## Dump 审计记录中已经列出的原生结构与入口

### 伤害事件

`5.6.1/CppSDK/SDK/HTGame_structs.hpp` 直接显示 `FHTDamageEvent` 总大小为 `0x170`，基类为 `FPointDamageEvent`，`DamageGEDef` 位于 `0x138`（大小 `0x8`），`DamageTags` 位于 `0x148`（大小 `0x20`）。`5.6.1/CppSDK/SDK/Engine_structs.hpp` 直接显示基类 `FPointDamageEvent::Damage` 位于 `0x10`（大小 `0x4`），因此 `Damage`、`DamageGEDef`、`DamageTags` 三个字段的偏移本轮均已直接核验。`5.6.1/CppSDK/SDK/HTGame_parameters.hpp` 直接显示 `HTAbilityCharacter.OnDamaged` 参数块大小为 `0x148`：`DamageAmount` `0x00/0x04`、`HitInfo` `0x08/0x108`、`DamageTags` `0x110/0x20`、`DamageGameplayEffect` `0x130/0x08`、`InstigatorCharacter` `0x138/0x08`、`DamageCauser` `0x140/0x08`。同一类声明中还存在 `CharacterOnDamaged` 多播委托；它与反射函数 `OnDamaged` 是不同载体，不能混为同一接口。

### 当前已绑定的四类 GameplayEffect 入口

1. `/Script/HTGame.HTAbilitySystemComponent.K2_ApplyGameEffectWithClassByActor`
2. `/Script/HTGame.HTAbilitySystemComponent.K2_ApplyGameplayEffectToTargetWithClass`
3. `/Script/HTGame.HTAbilityCharacter.K2_ActivateGameEffectsWithClassByActor`
4. `/Script/HTGame.HTAbilityCharacter.ServerActivateGameEffectsWithClassByActor`

这些入口在当前 Core 中有各自的反射布局校验。它们的参数语义不同，尤其 Server RPC 的 `GameplayEffect` 是 UObject 引用而非 ClassProperty；不得通过放宽校验来混用参数解释。

### 值得优先只读观测、但尚未获准作为回放根的候选

- `/Script/HTGame.HTAbilityCharacter.K2_ActivateGameEffectsWithClass`
- `/Script/HTGame.HTGameplayAbility.ApplyEffectContainerSpec`
- `/Script/HTGame.HTAbilitySystemBlueprintLibrary.ApplyExternalEffectContainerSpec`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectSpecToSelf`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectSpecToTarget`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectToSelf`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectToTarget`
- `/Script/HTGame.HTAbilitySystemComponent.ServerApplyTraceProjectileResults`

本轮已从 Dump 参数文件逐项确认候选的声明和参数块大小：

| 候选函数 | Dump 参数块大小 | 关键参数布局（偏移相对参数块） |
|---|---:|---|
| `HTAbilityCharacter.K2_ActivateGameEffectsWithClass` | `0x68` | `WorldContextObject` `0x00/0x08`；`GameplayEffect` 类引用 `0x08/0x08`；`FBufferData ModifyData` `0x10/0x58` |
| `HTGameplayAbility.ApplyEffectContainerSpec` | `0x48` | `FHTGameplayEffectContainerSpec ContainerSpec` `0x00/0x38`；返回 `TArray<FActiveGameplayEffectHandle>` `0x38/0x10` |
| `HTAbilitySystemBlueprintLibrary.ApplyExternalEffectContainerSpec` | `0x48` | 同为 `ContainerSpec` `0x00/0x38` 和返回数组 `0x38/0x10` |
| `AbilitySystemComponent.BP_ApplyGameplayEffectSpecToSelf` | `0x18` | `FGameplayEffectSpecHandle` `0x00/0x10`；返回句柄 `0x10/0x08` |
| `AbilitySystemComponent.BP_ApplyGameplayEffectSpecToTarget` | `0x20` | Spec Handle `0x00/0x10`；目标 ASC 指针 `0x10/0x08`；返回句柄 `0x18/0x08` |
| `AbilitySystemComponent.BP_ApplyGameplayEffectToSelf` | `0x30` | GE 类 `0x00/0x08`；Level `0x08/0x04`；Context Handle `0x10/0x18`；返回句柄 `0x28/0x08` |
| `AbilitySystemComponent.BP_ApplyGameplayEffectToTarget` | `0x38` | GE 类 `0x00/0x08`；目标 ASC 指针 `0x08/0x08`；Level `0x10/0x04`；Context Handle `0x18/0x18`；返回句柄 `0x30/0x08` |
| `HTAbilitySystemComponent.ServerApplyTraceProjectileResults` | `0xE8` | Spec Handle `0x00/0x04`；Tag `0x04/0x08`；`FGameplayEventData` `0x10/0xB0`；`ShotID` `0xC0/0x04`；`RandomSeed` `0xC4/0x04`；TraceResults 数组 `0xC8/0x10`；PredictionKey `0xD8/0x10` |

Dump 还显示 `FHTGameplayEffectContainerSpec` 大小为 `0x38`，其中 `TargetData` 为 `0x28`，`TargetGameplayEffectSpecs` 数组为 `0x10`。这些是 Dump 声明和静态参数布局的直接证据，**不证明这些函数实际走在玩家攻击路径中**，也不证明当前运行时 UFunction 的 `ParmsSize` 与 Dump 一致。现有观测仅按精确路径解析并计数，不读取候选参数、不为候选单独安装 hook、不把候选加入回放白名单。Effect Container、Spec Handle、Context Handle 和带实例引用的数组仍不能当作可跨帧复制的安全对象。

## 下一项实现任务：只读观测，不扩展回放白名单

1. 已在 Core 反射发现阶段按完整路径解析上述候选，并增加有界入口计数。
2. 当前计数只代表从已有 ProcessEvent/Native Exec 观测入口看到的次数；不同入口可能对同一逻辑调用重复计数。
3. 已新增只读运行时布局核验：按 Dump 预期检查 UFunction owner/name、`NumParms`、`ParmsSize`、`ReturnValueOffset`、字段反射类型/大小/偏移、结构体身份、ClassProperty 的 meta-class、目标对象类型，以及数组元素结构类型/大小；只有全部通过的候选才进入计数绑定。验证失败原因附加到 Core 的入口诊断信息中。
4. 最新提交 `5fe56ac2c06b1096fd98f6e01244a294f241c70c` 的 Windows 构建、Core/ABI validation、Runtime probe package 三项 CI 均已成功。即使运行时布局通过，也只证明当前 UFunction 元数据与 Dump 对得上，不证明它实际走在攻击路径中。后续再把可验证的调用与同一伤害事件的 `DamageGEDef`、攻击者、目标、ASC 和 Tick 关联；不触发回放。
5. 保持现有四类回放入口的校验不变。只有观测证据能证明具体路径、目标语义和安全参数重建方式后，才另行设计最小回放适配。
6. 用当前 Anomaly 的 Core/SDK/ABI 构建与测试验证新增观测；构建成功只证明代码可构建。游戏内链式伤害仍须由运行日志与目标 HP/新 DamageEvent 实测确认。


## Attack UI 最终定板（1.9.9）

- 窗口标题：`伤害链式反应`；目标宽度：`360`。
- 控件顺序：`启用`复选框 → `伤害重复次数`（初始值 `1`，允许 `1–10`）→ `绑定=就绪/等待` 与 `触发=战斗中/等待战斗` → `伤害=N次 失败=N次 重放=N次`。
- 不得把日志、单次路线验证按钮、目标/技能路径或实验诊断重新放回主界面；详细失败原因只写入插件目录 `attack-replay.log`。
- 每个可见/折叠窗口帧都必须配对调用 `begin_window` / `end_window`。
- `伤害重复次数`表示原始命中之后需要确认的重复伤害事件数；只有观察到同一世界、同一攻击者、同一目标及同一伤害来源的新事件才计为完成。达到配置次数后停止当前链并重新等待下一次真实伤害。
- 当前实现的重放间隔仍是内部固定值，不在最终 UI 暴露速率控制；若以后要改变节奏，须作为单独需求处理，不得偷偷扩展定板界面。

## 验证分层

- Dump 声明/参数布局：8 个候选的声明和参数块大小已从 `5.6.1` 对应 `*_classes.hpp` / `*_parameters.hpp` 直接核验；尚未核验运行时反射布局是否一致。
- `FHTDamageEvent` 大小，以及 `Damage`、`DamageGEDef`、`DamageTags` 三个字段偏移均已从对应结构定义直接核验。
- 当前源码路径：已读取分支文件。
- 观测实现：候选先经过运行时 UFunction 布局校验，再只计数；不读取调用参数缓冲区、不增加回放白名单。最新 Windows 构建、Core/ABI validation、Runtime probe package 三项 CI 均成功。此前针对旧提交 `6c9a91866b690a989a9cd08f5d4042b44ae11058` 的失败构建不能代表最新提交；最新 CI 只证明代码和 ABI 验证通过，不证明游戏内加载或功能生效。
- 插件界面：最终定板为版本 1.9.9、宽度 360 的紧凑面板。已核对当前工作分支 `plugins/NteAttackReplay/plugin.cpp`：只显示“启用”复选框、“伤害重复次数”输入（初始值 1，范围 1–10）、绑定/触发状态，以及“伤害=次数、失败=次数、重放=次数”；不显示日志、单次路线验证控件、目标路径或实验诊断；详细诊断写入插件目录的 `attack-replay.log`。对应代码已包含在最新通过 CI 的提交中；界面定板锁定，不再随本轮技术观测改动。仍未完成游戏内实际显示验证。
- 真实 GameplayEffect 追加伤害和多目标独立链：未验证。

## 约束

不使用外部 SDK/教程/论坛/搜索结果作为技术决策依据；不使用硬编码跨版本地址；不放宽对象、ASC、Effect 身份或世代校验；不把键位模拟、技能激活或动画重播当成伤害链实现；不将 Dump 原件打入 Runtime/SDK/Tools/发布包。
