# 原生 Dump 知识索引

> 状态：第一阶段目录/文件清单已建立；不是已完成逐文件语义阅读。索引注释严格标明当前仅根据仓库树、文件名、扩展名与已知生成文件命名规律归类的内容。凡未阅读正文的项目，均不得当作已证实的实现语义。

## 来源与版本

- 仓库：`Rabb1t79/Anomaly-main`
- 分支：`fix/restore-attack-work-and-repair-20261016`
- 本次读取的递归 Git tree commit：`3054f3a21526211b453aedcdf39b1e98770fe3c8`
- 可见 Dump 根目录：`5.6.1/`
- 本次目录树内文件数：**6861**（不含目录）
- 目录树内 `5.6.1_new/`：当前分支递归树中未出现。该事实只描述当前分支，不推断其他分支、其他仓库或用户本地材料不存在。

## 目录职责速览

- `5.6.1/CppSDK/SDK/`: 生成的 C++ SDK 文件；包含按模块拆分的 `*_classes.hpp`、`*_structs.hpp`、`*_parameters.hpp`、`*_functions.cpp` 等。
- `5.6.1/CppSDK/`: SDK 生成辅助文件与汇总入口。
- `5.6.1/Dumpspace/`: 类、函数、结构、枚举与偏移等反射元数据。
- `5.6.1/GObjects-Dump.txt`、`GObjects-Dump-WithProperties.txt`: 对象/属性转储，体积较大，应按关键词检索而不是整份塞进上下文。
- `5.6.1/Mappings/` 与 `5.6.1/IDAMappings/`: 映射类辅助材料；需先核验格式与来源用途。

## 使用规则

1. 文件存在不等于其声明走在目标功能路径中。
2. 函数/字段名、偏移或封装存在不等于它是受支持的插件接口。
3. 关键结论要记录完整文件路径、类型/函数名、行号或字段偏移、Dump 版本、必要的 ABI 参数/receiver/线程/生命周期核验。
4. 当前索引中的每条“简注”是导航线索。若标注“待正文核验”，不能用于直接实现或作为功能路径已证实的证据。
5. 每个分片按仓库路径排序；SHA 与字节数来自当前分支递归树，可用于核对文件身份。

## 分片索引

- [文件索引 01](./file-index-01.md)（500 个文件）
- [文件索引 02](./file-index-02.md)（500 个文件）
- [文件索引 03](./file-index-03.md)（500 个文件）
- [文件索引 04](./file-index-04.md)（500 个文件）
- [文件索引 05](./file-index-05.md)（500 个文件）
- [文件索引 06](./file-index-06.md)（500 个文件）
- [文件索引 07](./file-index-07.md)（500 个文件）
- [文件索引 08](./file-index-08.md)（500 个文件）
- [文件索引 09](./file-index-09.md)（500 个文件）
- [文件索引 10](./file-index-10.md)（500 个文件）
- [文件索引 11](./file-index-11.md)（500 个文件）
- [文件索引 12](./file-index-12.md)（500 个文件）
- [文件索引 13](./file-index-13.md)（500 个文件）
- [文件索引 14](./file-index-14.md)（361 个文件）

## 语义研究笔记

- [伤害链式反应候选机制（5.6.1）](./damage-chain-candidates.md)：当前已阅读的周期触发 Actor、配置结构与证据边界。

## 伤害链研究入口

后续正文阅读优先围绕 `ADamageTriggerActor`、`AHTAbilitySpawnActor`、`UHTAbilitySpawnDamageTriggerActor`、`FHTGameplayEffectContainer`、`FSpawnDamageTriggerActor`、`FDamageTriggerActorStaticData` 等已知候选符号，并对完整 owner、函数参数、调用关系、时序、目标绑定和生命周期逐项取证。这些候选名称本身不构成机制已被确认的结论。
