# 原生 Dump 知识库

## 用途与边界
用于检索仓库中的 NTE 原生 Dump。这里是源码导航与证据记录，不替代原始 Dump，也不把路径/命名推断当成已证实机制。

## 来源快照
- 仓库：`Rabb1t79/Anomaly-main`
- 当前 main 提交：`3da936234db490b88e2fe7a1529840a48136ba90`，新版目录以 Git LFS 指针形式存储。
- 新版真实正文从同仓库 LFS 转换前提交 `5a16a377c2133428d4ccf6f9c0d8770455f9a588` 读取；未使用仓库外资料。
- `5.6.1/`：6,861 个文件；`5.6.1-new/`：7,283 个文件。

## 读取限制
- 路径索引覆盖两个目录内全部 14,144 个文件，但通用条目是路径初注，不表示每个文件正文已逐句审阅。
- 当前 main 的新版路径返回 Git LFS 指针；为做源码对照，读取同仓库历史提交中 LFS 转换前的实体 blob。
- 不从外部 SDK、通用 UE 文档或未获用户授权的资料补齐结论；不以 ZIP 文件作为本轮证据。

## 注释等级
1. **路径初注**：根据文件名、目录、扩展名说明用途，未读正文。
2. **正文已读**：实际读取源码后对声明/内容摘要。
3. **同源交叉核验**：由参数结构、函数包装或 Dumpspace 元数据支持。
4. **版本对照已核验**：对两个版本对应源码段/元数据直接比较。
5. **运行时未证实**：Dump 声明存在不等于目标功能使用它，也不等于它是受支持插件接口。

## 导航
- `work-state.md`：当前进度、证据状态与下一步。
- `file-index-*.md`：全量路径索引，含扩展名、Git blob size、SHA 和路径初注。
- `damage-chain-candidates.md`：周期伤害候选机制摘要。
- `damage-trigger-source-notes.md`：DamageTrigger 类、结构、参数与生成包装的逐段注释。
- `damage-event-candidates.md`：`AHTAbilityCharacter::OnDamaged` 事件与参数布局候选。
- `related-spawn-modifiers.md`：生成 Actor、数值修改器和投射物/激光相关声明。
- `version-diff.md`：两版对照结果与未决项。
