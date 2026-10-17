# 5.6.1 与 5.6.1-new 对照状态

## 来源快照
- 当前 main：3da936234db490b88e2fe7a1529840a48136ba90；新版文件以 Git LFS 指针存储。
- 同仓库 LFS 转换前提交：5a16a377c2133428d4ccf6f9c0d8770455f9a588，保留新版真实源码 blob。
- 新版对照使用同仓库历史 blob，没有从仓库外获取 Dump。

## 目录规模（当前 main）
- 5.6.1/：6,861 个文件（.hpp 5,729；.cpp 1,121；.inl 2；.json 5；.txt 2；.idmap 1；.usmap 1）。
- 5.6.1-new/：7,283 个文件（.hpp 6,076；.cpp 1,196；.inl 2；.json 5；.txt 2；.idmap 1；.usmap 1）。
- 文件数不同不等于 API 语义不同。

## 已读候选符号段对照

| 文件 / 符号 | 5.6.1 blob | 5.6.1-new 历史 blob | 结果 |
|---|---|---|---|
| HTGame_classes.hpp | 526091ab6679ac6833634e5a6d08d807e90dbf0c | 7b56987f6db8efe91f66273f3e8d136de45485a3 | 文件大小分别 15,272,896 / 15,273,251 字节；已检查的候选类段逐字一致，文件其他位置仍有差异。 |
| ADamageTriggerActor | 上述 classes blob | 上述 classes blob | 类段逐字一致；17 个函数声明、字段和偏移一致。 |
| UHTAbilitySpawnDamageTriggerActor | 同上 | 同上 | 类段逐字一致；持有 FSpawnDamageTriggerActorArray 的字段位置/大小一致。 |
| UFindSpawnLocationObj_Target | 同上 | 同上 | 类段逐字一致；目标位置、方向、胶囊和预位置时间字段一致。 |
| AHTAbilityCharacter | 同上 | 同上 | 类段逐字一致。 |
| AProjectileBase | 同上 | 同上 | 类段逐字一致；含 OverlapDamageInterval、OverlapDamageMaxCount、HitActorList 等字段。 |
| AHTLaserActor | 同上 | 同上 | 类段逐字一致；含 TriggerDamageInterval、PerTargetDamageInterval、MaxHitCount 等字段。 |
| UHTAbilitySystemBlueprintLibrary | 同上 | 同上 | 类段逐字一致；ModifySpawnActorNumericParam 签名/flags 相同。 |
| UProjectileSpawnDamageActorsEffect | 同上 | 同上 | 类段逐字一致；持有 FSpawnDamageTriggerActor 数组。 |
| ULaserSpawnDamageActorsEffect | 同上 | 同上 | 类段逐字一致；持有 FSpawnDamageTriggerActor 数组及 TriggerInterval/MaxSpawnCount。 |
| HTGame_structs.hpp | cc6781d70b782a0e3ec6cf06dfbd75f323f04a53 | 28b7cb52252fc062bcbb7d23ccfc9ddf52621079 | 文件大小分别 6,879,049 / 6,879,572 字节；已检查候选结构段逐字一致。 |
| FHTGameplayEffectContainer | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| FSpawnDamageTriggerActor | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| FSpawnDamageTriggerActorArray | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| FDamageTriggerActorStaticData | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| FHTDamageEvent | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| FHTGameplayEffectContext | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| EHTSpawnActorParamPhase / EHTNumericParamOperation | 上述 structs blob | 上述 structs blob | 枚举段逐字一致；PreSpawn/PostSpawn 与 Set/Add/Multiply 数值相同。 |
| HTGame_parameters.hpp | b839b1a326e90ad3ac1c598f6bbca6b6eee465e | 同一 SHA | 文件内容逐字相同。 |
| HTGame_functions.cpp | c0a5f1dfa3540ab656d18578950d02e4de522ef2 | 同一 SHA | 文件内容逐字相同。 |
| HTAbilitySystemBlueprintLibrary_ModifySpawnActorNumericParam | HTGame_parameters.hpp 中的参数结构 | 同一文件 SHA | 参数布局 0x20；两版参数/包装文件逐字相同，FunctionsInfo 的返回值、参数和 flags 规范化后相同。 |
| Dumpspace/FunctionsInfo.json | 77f814d20c38455538d2ae3224a02a023c353e7e | d920ace94722578035d5659295cffaf38ba2cae1 | 元数据文件不同；ADamageTriggerActor 的 17 个函数名称、返回类型、参数和 flags 规范化后相同。数值地址/标识不同，不能作为稳定 ABI。 |

## 直接结论
- **直接证实：** 已检查的 DamageTrigger、Projectile、Laser 和伤害事件候选类/结构段在两版中相同；参数和生成包装文件的 blob SHA 相同。
- **直接证实：** 完整 classes/structs/FunctionsInfo 文件并非整体完全相同；结论只覆盖已比较的符号段。
- **未知：** 未对全部 14,144 个文件做语义级差异分析；也未证明旧插件在运行时使用这些原生候选路径。
- Dump 元数据中的数值地址/标识不是稳定 ABI；禁止硬编码并跨版本复用。

## 后续
1. 核验 GObjects-Dump-WithProperties.txt 或其他可读配置材料中的实际资产参数。
2. 对候选类完整函数路径、参数结构和 flags 建立索引，尤其是目标选择与生成配置消费者。
3. 将候选与用户视频表现对应；旧 DLL 控制流/常量分析后置。
4. 最后核验 Anomaly 当前公开 ABI 是否有安全桥接；如不足，再按同源证据评估最小 ABI/Core 扩展。
