# 伤害事件候选：三个不同的“受伤通知”表面

> 来源：Dump 与用户指定 Anomaly 分支内的 profile。不要把反射函数、委托和内部 hook 混成一个入口。

## 1. 反射函数 AHTAbilityCharacter::OnDamaged

HTGame_classes.hpp 第 1,445 行声明：

void OnDamaged(float DamageAmount, const FHitResult& HitInfo, const FGameplayTagContainer& DamageTags, const UHTGamePlayEffect* DamageGameplayEffect, AHTAbilityCharacter* InstigatorCharacter, AActor* DamageCauser);

Dumpspace/FunctionsInfo.json 将 owner 记为 AHTAbilityCharacter，flags 为 Native|Event|Public|HasOutParams|BlueprintEvent。

参数结构 HTAbilityCharacter_OnDamaged 总大小 0x148，位于 HTGame_parameters.hpp 第 2,385 行附近，来自两版 SHA 相同的文件：

| 参数 | 偏移 / 大小 | 注释 |
|---|---|---|
| DamageAmount | 0x000 / 0x004 | float 伤害量。 |
| padding | 0x004 / 0x004 | 对齐。 |
| HitInfo | 0x008 / 0x108 | FHitResult。 |
| DamageTags | 0x110 / 0x020 | FGameplayTagContainer。 |
| DamageGameplayEffect | 0x130 / 0x008 | const UHTGamePlayEffect*。 |
| InstigatorCharacter | 0x138 / 0x008 | AHTAbilityCharacter*。 |
| DamageCauser | 0x140 / 0x008 | AActor*。 |

**直接证实：** 这是一个有完整反射签名和参数缓冲布局的 Native/Event/BlueprintEvent 函数声明。

**未知：** 是否每次有效伤害都会触发、触发在伤害结算前还是后、是否覆盖所有角色/伤害类型。生成的 HTGame_functions.cpp 包装只展示 ProcessEvent 调用，不是原生伤害计算实现。

## 2. CharacterOnDamaged 多播委托

HTGame_classes.hpp 另有一个名为 CharacterOnDamaged 的 TMulticastInlineDelegate 属性。它的参数形状是：
- const FHTDamageEvent&
- 被伤害角色
- 施加者角色
- DamageCauser

这与上面的反射函数不同：反射函数接收 float DamageAmount、FHitResult、DamageTags 和 UHTGamePlayEffect*；委托接收 FHTDamageEvent 以及双方角色指针。不能把两者视为同一个签名或入口。

## 3. Anomaly profile 中的内部广播符号

在用户指定仓库分支 fix/restore-attack-work-and-repair-20261016 的 profiles/nte/nte-current.json 中，存在：
- 符号键：nte.HTAbilityCharacter.BroadcastCharacterOnDamaged
- 模块：HTGame.exe
- section：.text
- 解析方式：direct
- validators：address-in-module、executable
- capability 使用者：anomaly.nte.combat

它是通过 profile 中的 .text 特征模式解析的内部符号，不是 FunctionsInfo.json 中的反射 UFunction，也不是 CharacterOnDamaged 委托本身。现有 src/game/ue5/ue5_damage_function_hook.cpp 使用 character_on_damaged target 安装独立 native hook；这只说明当前 Anomaly 分支存在内部观察路线，不证明旧插件采用它，也不决定复现必须沿用该路线。

## 4. 关联结构

### FHTDamageEvent
来源：HTGame_structs.hpp 第 23,127 行起。
- 继承 FPointDamageEvent，总大小 0x170。
- DamageGEDef：TWeakObjectPtr<UHTGamePlayEffect>，偏移 0x138。
- DamageTags：FGameplayTagContainer，偏移 0x148，大小 0x20。
- 两版结构段逐字一致。

### FHTGameplayEffectContext
来源：HTGame_structs.hpp 第 59,869 行起。
- 继承 FGameplayEffectContext。
- Target：TWeakObjectPtr<AActor>，偏移 0x80。
- 字段存在不证明每次伤害都带有有效上下文。

### 同源对象清单
GObjects-Dump.txt 中存在 Function HTGame.HTAbilityCharacter.OnDamaged、ScriptStruct HTGame.DamageTriggerActorStaticData 和 Class HTGame.HTAbilitySpawnDamageTriggerActor。这确认反射对象名称出现在 Dump 清单里，但不证明调用顺序。清单地址是 Dump 快照值，不是稳定 ABI。

## 5. 对复现路线的意义

- **有证据支持但未验证：** 这三个表面都可能提供伤害事件观察线索，但签名和调用机制不同，需要分别核验。
- **未知：** 哪一个是游戏对每次有效命中的真实广播点；委托是否可从受支持接口订阅；内部 hook 是否适用于目标版本；是否能识别玩家对敌人造成的有效伤害；回调内继续触发伤害是否会递归。
- 不应推导“再次调用 OnDamaged 就能造成伤害”。它是事件声明，不是伤害计算实现。
- UAbilityTask_JumpSection::OnAttackedCallback / OnDamagedCallback 的 owner 是能力任务类，不应因名称相似而当作通用伤害入口。

## 6. 当前判断

保留这三个表面作为不同候选，先通过 Dump 参数布局、profile 绑定和可读的原生调用链建立对应关系，再决定是否需要观测/桥接。旧 DLL 仍按计划后置；不因当前 Anomaly 分支存在某个 hook 就推定旧插件使用同一机制。
