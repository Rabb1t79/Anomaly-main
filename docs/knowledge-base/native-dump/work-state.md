# 当前知识库工作状态（2026-10-18）

## 任务
建立可持续检索的原生 Dump 知识库，先比较 `5.6.1/` 与 `5.6.1-new/`，再根据 Dump 和用户视频筛选伤害链机制候选；旧 DLL 分析后置。

## 仓库状态
- 仓库：`Rabb1t79/Anomaly-main`
- 知识库工作分支：`research/native-dump-knowledge-base-20261018`
- 当前已提交到该分支的知识库提交：`0f6509a76537a925c77455c8b38a98bc02f4d689`
- 知识库目录：`docs/knowledge-base/native-dump/`
- 已登记 Dump 文件：14,144 个；索引分片 57 份，每份最多 250 条。
- 注意：索引中的普遍注释是路径/扩展名初注，不代表每个文件正文已逐句审阅。

## LFS 读取解决方式
当前 main 提交 `3da936234db490b88e2fe7a1529840a48136ba90` 的 `5.6.1-new/` 以 LFS 指针保存。真实正文可从同仓库 LFS 转换前提交 `5a16a377c2133428d4ccf6f9c0d8770455f9a588` 的普通 Git blob 读取。后续比较必须记录当前指针和历史实体 blob 的来源，不能把指针文本当源码。

## 已完成的源码分析
- `ADamageTriggerActor`：字段、函数声明和周期/时长/计数状态。
- `FHTGameplayEffectContainer`、`FSpawnDamageTriggerActor`、`FSpawnDamageTriggerActorArray`、`FDamageTriggerActorStaticData`。
- `UHTAbilitySpawnDamageTriggerActor`、`UFindSpawnLocationObj_Target`。
- `AHTAbilitySpawnActor` 的效果容器、EventData 和能力关联字段。
- `UHTSpawnActorParamModifier` / `UHTSpawnActorNumericParamModifier` 与专用 DamageTrigger 数值修改器。
- `AHTAbilityCharacter::OnDamaged` 的反射签名与参数布局。
- 两版候选类/结构段直接比较；参数文件 `HTGame_parameters.hpp` 与包装文件 `HTGame_functions.cpp` blob SHA 相同；`ADamageTriggerActor` 的 17 个函数签名在反射元数据中规范化后相同。

## 关键结论分级
- **直接证实**：原生 Dump 中存在周期触发 Actor、目标重叠数组、周期/持续时间/次数字段、GameplayEffect 类容器、生成 Actor 配置和公开的 `OnDamaged` 事件声明；已检查的候选符号在两版中相同。
- **有证据支持但未验证**：这些类型/事件是持续伤害链研究候选。
- **未知**：旧插件是否使用这些候选；真实触发时序、目标关系、独立实例语义、有效伤害验证路径及合规 ABI 桥接方法。

## 下一步
1. 检查 `GObjects-Dump-WithProperties.txt` 与配置/映射材料，查找 `DamageTriggerActorStaticData`、`DamageTriggerConfigID` 的对象和数据关联。
2. 扩大函数元数据索引，记录候选类的完整函数路径、参数结构和 flags，尤其是目标选择和生成配置消费者。
3. 对 `OnDamaged` 追踪可能的调用/包装和时间语义；不能只因函数名就认定它是每次有效伤害的可靠事件。
4. 继续按目录逐步为重要文件建立正文摘要，保持初注与正文核验的明确区分。
5. Dump 候选形成后再分析旧 DLL，最后核验 Anomaly ABI/Core 桥接。
