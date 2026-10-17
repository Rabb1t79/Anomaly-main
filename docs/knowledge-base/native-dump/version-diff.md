# 5.6.1 与 5.6.1-new 对照状态

## 来源快照
- 当前 main：`3da936234db490b88e2fe7a1529840a48136ba90`；新版文件以 Git LFS 指针形式存储。
- 同仓库 LFS 转换前父提交：`5a16a377c2133428d4ccf6f9c0d8770455f9a588`，tree `e82ad85e8138324bd2c27eaa7dfc3bdf474d28a9`，保留新版真实源码 blob。
- 新版对照使用同仓库历史 blob，没有从仓库外获取 Dump。

## 目录规模（当前 main）
- `5.6.1/`：6,861 个文件（.hpp 5,729；.cpp 1,121；.inl 2；.json 5；.txt 2；.idmap 1；.usmap 1）。
- `5.6.1-new/`：7,283 个文件（.hpp 6,076；.cpp 1,196；.inl 2；.json 5；.txt 2；.idmap 1；.usmap 1）。
- 文件数不同不等于 API 语义不同。

## DamageTrigger 候选符号对照（正文已读取）

| 文件 / 符号 | 5.6.1 blob | 5.6.1-new 历史 blob | 结果 |
|---|---|---|---|
| `HTGame_classes.hpp` | `526091ab6679ac6833634e5a6d08d807e90dbf0c` | `7b56987f6db8efe91f66273f3e8d136de45485a3` | 文件大小分别 15,272,896 / 15,273,251 字节；候选类段逐字一致，其他位置仍有差异。 |
| `ADamageTriggerActor` | 上述 classes blob | 上述 classes blob | 类段逐字一致；17 个函数声明、字段和偏移一致。 |
| `UHTAbilitySpawnDamageTriggerActor` | 同上 | 同上 | 类段逐字一致；持有 `FSpawnDamageTriggerActorArray` 的字段位置/大小一致。 |
| `UFindSpawnLocationObj_Target` | 同上 | 同上 | 类段逐字一致；位置、方向、目标胶囊、预位置时间字段一致。 |
| `HTGame_structs.hpp` | `cc6781d70b782a0e3ec6cf06dfbd75f323f04a53` | `28b7cb52252fc062bcbb7d23ccfc9ddf52621079` | 文件大小分别 6,879,049 / 6,879,572 字节；候选结构段逐字一致。 |
| `FHTGameplayEffectContainer` | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| `FSpawnDamageTriggerActor` | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| `FSpawnDamageTriggerActorArray` | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| `FDamageTriggerActorStaticData` | 上述 structs blob | 上述 structs blob | 结构段逐字一致。 |
| `HTGame_parameters.hpp` | `b839b1a326e90ad3ac1c598f6bbca6b6eee465e1` | 同一 SHA | 文件内容逐字相同。 |
| `HTGame_functions.cpp` | `c0a5f1dfa3540ab656d18578950d02e4de522ef2` | 同一 SHA | 文件内容逐字相同。 |
| `Dumpspace/FunctionsInfo.json` | `77f814d20c38455538d2ae3224a02a023c353e7e` | `d920ace94722578035d5659295cffaf38ba2cae1` | 元数据文件不同；两版 `ADamageTriggerActor` 均有 17 个函数，名称、返回类型、参数和 flags 规范化后相同。数值地址/标识不同，不能作为稳定 ABI。 |

## 结论
- **直接证实**：已检查的 DamageTrigger 候选类/结构/参数/包装中，两版对应声明布局与函数签名相同；参数和包装文件具有相同 blob SHA。
- **直接证实**：完整 classes/structs/FunctionsInfo 文件并非整体完全相同；只对已比较的符号段下结论。
- **未知**：未对全部 14,144 个文件做语义级差异分析；也未证明旧插件在运行时使用这些原生候选路径。
- Dump 元数据中的数值地址/标识不是稳定 ABI；禁止硬编码并跨版本复用。

## 后续
1. 继续核验 `Dumpspace/FunctionsInfo.json` 目标函数完整路径和元数据。
2. 检查 `GObjects-Dump-WithProperties.txt` 与静态数据定义，追踪候选配置来源。
3. 将原生声明候选与视频表现对应；之后再用旧 DLL 控制流/常量交叉验证。
4. 最后核验 Anomaly 当前公开 ABI 是否有安全桥接；如不足，再按同源证据评估最小 ABI/Core 扩展。
