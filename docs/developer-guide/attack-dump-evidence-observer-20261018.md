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
- 原始 Dump 位于仓库目录 `5.6.1` 和 `5.6.1_new`。本轮尚未逐项重新核验这两个目录中的原始文件；因此以下 Dump 结论继承既有审计记录，不声称本轮已完成逐文件复核。
- 当前源码可直接确认：Core 现有可重放捕获绑定只接受四类已验证入口；`CombatExecFunctionTargets()` 也只从这四个绑定中取 Native Exec 目标。其余候选路径目前没有进入这套捕获/回放绑定。
- 既有审计记录确认：之前的实机日志曾出现候选调用无法同时匹配 `DamageGEDef`、攻击者/目标 ASC 与时间窗口的情况。它不能证明现有路径正确，也不能证明某个未观测候选就是正确路径。

## Dump 审计记录中已经列出的原生结构与入口

### 伤害事件

既有审计记录指出，`FHTDamageEvent` 的大小为 `0x170`，`Damage` 位于 `0x10`，`DamageGEDef` 位于 `0x138`，`DamageTags` 位于 `0x148`。`HTAbilityCharacter.OnDamaged` 的反射参数结构大小为 `0x148`，包含 `DamageGameplayEffect`、`InstigatorCharacter` 和 `DamageCauser`。此外，`CharacterOnDamaged` 委托与反射函数 `OnDamaged` 是不同载体，不能混为同一接口。

### 当前已绑定的四类 GameplayEffect 入口

1. `/Script/HTGame.HTAbilitySystemComponent.K2_ApplyGameEffectWithClassByActor`
2. `/Script/HTGame.HTAbilitySystemComponent.K2_ApplyGameplayEffectToTargetWithClass`
3. `/Script/HTGame.HTAbilityCharacter.K2_ActivateGameEffectsWithClassByActor`
4. `/Script/HTGame.HTAbilityCharacter.ServerActivateGameEffectsWithClassByActor`

这些入口在当前 Core 中有各自的反射布局校验。它们的参数语义不同，尤其 Server RPC 的 `GameplayEffect` 是 UObject 引用而非 ClassProperty；不得通过放宽校验来混用参数解释。

### 值得优先只读观测、但尚未获准作为回放根的候选

- `/Script/HTGame.HTAbilityCharacter.K2_ActivateGameEffectsWithClass`
- `/Script/HTGame.HTAbilitySystemComponent.ApplyEffectContainerSpec`
- `/Script/HTGame.HTAbilitySystemBlueprintLibrary.ApplyExternalEffectContainerSpec`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectSpecToSelf`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectSpecToTarget`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectToSelf`
- `/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectToTarget`
- `/Script/HTGame.HTAbilitySystemComponent.ServerApplyTraceProjectileResults`

这些名称/签名来自既有 Dump 审计记录；本轮尚未从仓库目录 `5.6.1` 和 `5.6.1_new` 中逐项复核。它们是观测候选，不是已确认的攻击调用路径。特别是 Effect Container、Spec Handle、Context Handle 可能包含间接数据或生命周期约束，不能把原始参数缓冲区当作可跨帧复制的安全对象。

## 下一项实现任务：只读观测，不扩展回放白名单

1. 在 Core 的反射发现阶段按完整路径解析上述候选函数。
2. 对每个候选做 Dump 对照的函数所属类、参数数量、参数大小、属性类型和字段布局校验；任何字段无法核验时只记录“未验证”，不安装观测 hook。
3. 为通过精确路径解析的候选增加有界、只计数的入口观测：Game Tick、函数路径、receiver 类、原始调用入口类型，以及可安全读取的 Effect/Source/Target 身份字段。对包含未知指针或容器的字段只记录是否存在，不解引用、不跨帧保存。
4. 将观测结果与同一伤害事件的 `DamageGEDef`、攻击者、目标、ASC 和 Tick 关联，输出候选命中入口计数，但不读取参数、不触发回放。
5. 保持现有四类回放入口的校验不变。只有观测证据能证明具体路径、目标语义和安全参数重建方式后，才另行设计最小回放适配。
6. 用当前 Anomaly 的 Core/SDK/ABI 构建与测试验证新增观测；构建成功只证明代码可构建。游戏内链式伤害仍须由运行日志与目标 HP/新 DamageEvent 实测确认。

## 验证分层

- Dump 声明/布局：部分继承既有审计记录；仍需直接检查仓库目录 `5.6.1` 和 `5.6.1_new` 中对应的原始文件，逐项完成复核。
- 当前源码路径：已读取分支文件。
- 观测 hook 实现：已提交只计数的候选入口观测；尚未构建或实机验证。
- Core/SDK/ABI 构建：本轮未运行。
- 插件构建/加载：本轮未验证。
- 真实 GameplayEffect 追加伤害和多目标独立链：未验证。

## 约束

不使用外部 SDK/教程/论坛/搜索结果作为技术决策依据；不使用硬编码跨版本地址；不放宽对象、ASC、Effect 身份或世代校验；不把键位模拟、技能激活或动画重播当成伤害链实现；不将 Dump 原件打入 Runtime/SDK/Tools/发布包。
