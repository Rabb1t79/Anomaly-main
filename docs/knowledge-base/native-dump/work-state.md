# 当前知识库工作状态（2026-10-18）

## 任务
建立可持续检索的原生 Dump 知识库，比较 5.6.1/ 与 5.6.1-new/，再根据 Dump 和用户视频筛选伤害链机制候选；旧 DLL 控制流分析后置。

## 仓库状态
- 仓库：Rabb1t79/Anomaly-main
- 知识库工作分支：research/native-dump-knowledge-base-20261018
- 已登记 Dump 文件：14,144 个；索引分片 57 份，每份最多 250 条。
- 知识库目录：docs/knowledge-base/native-dump/
- 普遍注释为路径/扩展名初注，不表示每个文件正文已逐句审阅。

## 新版 LFS 读取方式
当前 main 提交 3da936234db490b88e2fe7a1529840a48136ba90 的 5.6.1-new/ 以 LFS 指针保存。真实正文可从同仓库 LFS 转换前提交 5a16a377c2133428d4ccf6f9c0d8770455f9a588 的普通 Git blob 读取。索引记录当前 main 的指针元数据；专题文档记录历史实体 blob SHA。

## 已完成的正文分析
- ADamageTriggerActor 的字段、函数声明、重叠目标数组和周期/时长/计数状态。
- FHTGameplayEffectContainer、FSpawnDamageTriggerActor、FSpawnDamageTriggerActorArray、FDamageTriggerActorStaticData。
- UHTAbilitySpawnDamageTriggerActor、UFindSpawnLocationObj_Target。
- AHTAbilitySpawnActor 的效果容器、EventData 和能力关联字段。
- UHTSpawnActorParamModifier / UHTSpawnActorNumericParamModifier 及专用 DamageTrigger 数值修改器。
- AProjectileBase 的重叠伤害间隔/最大次数/命中列表/追踪目标字段。
- AHTLaserActor 的 TriggerDamageInterval、PerTargetDamageInterval、TriggerDuration、MaxHitCount 与重叠回调。
- AHTAbilityCharacter::OnDamaged 的反射签名和参数布局。
- 区分反射函数 OnDamaged、CharacterOnDamaged 多播委托与 Anomaly profile 内部 BroadcastCharacterOnDamaged 符号。
- GObjects-Dump.txt 已逐行读取并核对：DamageTriggerActorDataTable 的 Package/DataTable 名称；HTAbilitySpawnDamageTriggerActor 出现在 74 行；存在 GA_Lacrimosa_PerfectEvade、GA_Lacrimosa_SwitchSkill、GA_Shinku_UltraSkill_Rage_Pre 等对象路径。
- 两版候选类/结构段直接比较；HTGame_parameters.hpp 与 HTGame_functions.cpp 两版 blob SHA 相同；ADamageTriggerActor 的 17 个函数签名在反射元数据中规范化后相同。

## 视频行为证据（仅可观察行为）
此前用户提供的 Bilibili 参考链接为 https://www.bilibili.com/video/BV1Sib46eEx5/。既有聊天记录的观察摘要是：约视频 18 秒起，敌人持续出现伤害数字并掉血，玩家基本停留附近；这支持“首次命中后可观察到后续伤害持续发生”的行为目标。它不能证明内部是周期 Actor、Projectile/Laser、重放 GameplayEffect 或其他机制；本轮网页读取该链接返回 Cache miss，未重新播放验证。

## 关键结论分级
- **直接证实**：Dump 中存在周期触发 Actor、Projectile/Laser 的间隔伤害字段、目标重叠数组、GameplayEffect 类容器、生成 Actor 配置和 OnDamaged 事件声明；已检查候选符号在两版中相同。
- **有证据支持但未验证**：这些类型/事件是持续伤害链研究候选。
- **未知**：旧插件是否使用这些候选；真实触发时序、目标关系、独立实例语义、有效伤害验证路径及合规 ABI 桥接方法。

## 阻塞
- GObjects-Dump-WithProperties.txt 约 67.9 MB，单次 Blob 响应超过连接器 64 MiB 解码上限；无法据此读取具体 DataTable 属性值。不能猜测 Period、MaxTriggerCount、TriggerLinkTimeDuration 的实际资产取值。
- Dump 声明和生成函数包装不是原生实现体；它们不能证明真实调用图。
- 当前没有把旧 DLL 的可解释控制流与这些候选建立直接交集。

## 下一步
1. 继续逐目录给重要文件建立正文摘要，保留路径初注与正文核验的区分。
2. 尝试通过可用仓库接口分块获取 GObjects-Dump-WithProperties.txt；若无法获取，明确保持属性值未知。
3. 继续核对 DamageTriggerActorDataTable、配置 ID 和相关资产对象的同源关联，不把对象路径当成执行证据。
4. 依据用户指定 Anomaly 分支检查公开 ABI/service 边界；不因 Attack 现有 damage-replay 服务存在就默认复现路线。
5. Dump 与视频候选链更清楚后，再分析旧 DLL；未建立证据交集前不修改 Attack 分支、不新增 ABI、不改 Core、不构建。
