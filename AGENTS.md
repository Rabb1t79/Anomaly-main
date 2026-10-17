# Anomaly Agent 协作约定

本文件是面向所有 AI 编码 Agent 的协作约定，遵循通用的 `AGENTS.md` 规范。多数 Agent
（Codex、Claude Code、Cursor、Gemini CLI 等）会自动读取层级中的 `AGENTS.md`；不自动加载的
Agent 请在会话开始时手动引用本文件。它是仓库唯一的自动加载入口，`.agents/` 仅存放由本文件
显式引用的项目资料，不依赖目录自动发现。

在修改源码、构建、Profile、部署或发布资料前，先阅读本文件。它补充产品文档，不能覆盖
更高优先级的平台指令或用户请求。

## 信息来源

- `README.md`：项目概览、已支持能力与快速上手。
- `docs/user-guide/`：面向使用者的安装、配置、内建插件与诊断文档。
- `docs/developer-guide/`：从源码构建、架构概览、插件开发与贡献流程。
- `docs/api-reference/`：完整的纯 C ABI v1 契约。
- `.agents/architecture.md`：实现时使用的模块归属、依赖和线程边界速查。
- `.agents/tooling.md`：构建、测试、发布和实时进程工具的命令与权限规则。
- `CMakePresets.json`：主仓库唯一受支持的构建配置。

`.build/`、`data/`、`runtime/` 都是本机或生成状态，不能作为源码输入。不要修改生成的
CMake 文件、构建产物或正在运行的游戏文件来掩盖源码问题。

## NTE V1 基底与原生 Dump 证据规则（强制）

- 当前主框架基底是用户提供的 `Anomaly-2.3.0-alpha.1.zip`：它是已修缮的 V1 源码，不能用旧 Git 分支整目录覆盖。后续工作必须在此基底上做最小合并，保留 ZIP 内已有的修复；此前新增的 SDK ABI、服务表、Profile、validator、宿主桥接、ABI snapshot 与文档，要逐项迁移并验证，不得假定它们已存在。
- NTE / UE 原生类、UFunction 所属模块及类、函数名、参数顺序/大小/偏移、继承和对象布局，必须以当前任务明确指定的原生 Dump 为证据。对于旧插件伤害链复现，权威路径是仓库根目录 `./5.6.1/` 与 `./5.6.1_new/`；不得再以 ZIP 作为本任务的旧 DLL 或 Dump 来源。优先核对 `CppSDK/SDK/HTGame_*`、相应插件模块的 `*_classes.hpp`、`*_parameters.hpp`、`*_functions.cpp` 及 `Dumpspace` 反射 JSON。不要用通用 Unreal 文档、其他游戏版本、无关仓库/旧插件或名称相似性替代 Dump。
- 每个原生绑定必须核实完整路径/owner、参数 ABI、receiver、所属模块及必要布局。若 Dump 没有足够证据，标注为未验证、关闭该能力并给出可诊断状态；禁止以猜测实现写入、调用或宣称成功。
- 公开 C ABI 变更必须同步更新 `include/anomaly/sdk/`、`apps/abi_snapshot/`、`abi/anomaly-sdk-v1-windows-x64.json`、服务能力/Manifest/Schema/Profile/validator 与文档；使用 `struct_size` 保护可选尾字段，并验证旧消费者前缀布局。
- Attack Replay 必须以真实且相关联的 player→enemy DamageEvent 验证每次额外攻击；`ProcessEvent` 被调用或一个伤害回调返回并不等于实际扣血。Vehicle 必须按 Dump 的真函数 owner 分别调用车速、运动组件与召唤接口，并核验召唤实体、位置与 Owner。
- 交付前必须对当前 V1 合并树执行受影响的 Windows x64 构建、ABI/Schema 验证和包内容检查。插件独立编译成功不能替代宿主构建；源码检查不能替代 DLL 构建；构建成功不能冒充真实游戏内验证。未完成上述必要检查时，不得标记为可交付。

详见 `docs/developer-guide/nte-dump-contract.md`。

## 伤害链式反应旧插件复现（任务级强制规则）

本节只约束旧插件“伤害链式反应”复现任务，不把该任务目标扩展为整个插件项目的唯一目标。

当前进度基线与防重复清单见 [docs/developer-guide/damage-chain-recreation-status.md](docs/developer-guide/damage-chain-recreation-status.md)。每次恢复该任务时先读这两份文件，再继续查证。

- **证据位置已纠正：本任务彻底弃用 ZIP 作为旧 DLL / 原生 Dump 的取证来源。** 不要重复搜索或重新分析旧 ZIP，也不要根据 ZIP 的目录结构判断仓库当前材料是否存在。
- **旧 DLL 首要对象：** 用户已明确，两份旧 DLL 存根位于当前仓库的 `plugin/time...` 文件夹中。开始分析时先检查当前分支的仓库文件树，定位这两个实际文件，记录相对路径、大小、SHA-256 与可复现的分析结果。不得拿其他插件 DLL、同名样本或先前对话中的猜测替代。若 GitHub 连接器无法列目录，应使用可用的仓库文件搜索/提交树信息继续定位，不要退回 ZIP。
- **原生 Dump 权威路径：** 当前仓库根目录的 `./5.6.1/` 与 `./5.6.1_new/`。二者都应作为本仓库材料核验；对同一类型/函数/布局逐项比较版本差异。不能因为某个文件接口返回 404 就推断整个目录不存在；先检查树/提交/文件路径并记录可验证结果。
- **ABI 桥接原则：** Anomaly 公开 ABI 可以在游戏原生 Dump 明确拥有、且证据足以核验的原生接口上建立桥接。不得将“允许桥接”误解为任意调用都已受支持：仍需核验真实 owner/路径、参数与结构布局、receiver、调用约定、Game 线程、对象代际、生命周期、capability、失败处理，并按需最小扩展 ABI/Core；不得仅凭名称相似、固定地址或“看起来能调用”作实现依据。
- **复现对象优先级：** 旧 DLL 的可还原控制流/行为证据优先决定复现对象；Dump 用于核验原生含义；Anomaly 源码与 ABI 用于核验桥接边界；视频/截图只核对可观察行为，不单独证明内部机制。Attack Replay 分支和 `docs/developer-guide/attack-replay-legacy-port.md` 是另一条实现路线，不能覆盖旧 DLL 证据，也不得未经授权合并。
- **状态记录规则：** 每轮只在真实执行后更新状态；明确区分“直接证实 / 有证据支持但未验证 / 未知”，记录实际路径、哈希、引用位置、验证等级与阻塞。不要重复已经完成的 ZIP 扫描或把同一结论当成新进展。
- 本任务中 Dump 与 DLL 分析材料不得打入 Runtime、SDK、Tools 或发布包，除非用户另行明确授权。

## 必经交付流程

1. 开始前执行 `git status --short --branch`，保留与当前任务无关的已修改和未跟踪文件。
2. 编辑前确定功能所属模块和公开边界。以最小改动完成需求，不夹带无关重构或格式化。
3. 行为变化必须在所属层补充或更新验证。验证范围以实际改动和依赖关系为准：构建直接受影响的
   target，运行对应的单元、契约或集成验证；仅在跨共享模块边界、修改构建图/公开 ABI/发布打包，
   或用户明确要求时扩大范围。不得仅因改动属于 Runtime、Launcher、CMake 或工具等类别就默认扩大
   验证范围。具体命令见 `.agents/tooling.md`。
4. 完成前执行 `git diff --check`，检查暂存差异，并说明实际执行的验证或未执行原因。
5. `.local/` 仅用于本机临时 fixture；其中的源码、测试代码和构建脚本禁止暂存、提交或通过
   `git add -f` 强制加入。需要纳入版本控制的实现必须移到受版本管理的模块目录。

提交不是每项修改的必需步骤。只有用户或任务流程明确要求提交时才创建 Git commit；需要提交时，
使用 Conventional Commit 风格，例如 `fix(plugin): drain callbacks before unload`、
`build(cmake): unify the local preset entry point`。只暂存明确文件，禁止 `git add -A`；不要改写
他人的提交，也不要用 `reset --hard` 或强制 checkout 隐藏工作。

除非用户明确要求，不生成额外的审计记录、补丁副本或回滚脚本；验证结果直接在任务回复中说明。
禁止生成回滚副本：不要创建 `.bak`、带时间戳的 DLL/manifest 副本或其他回滚文件；热更新直接覆盖运行包，验证结果直接在任务回复中说明。

## 实现与测试纪律

- 禁止过度防御代码。只为已知合同、明确故障模型或可复现问题增加校验；同一事实在同一同步调用中
  不得反复验证，不得为纯假设竞态堆叠状态机、重试、缓存或分支。新增防御分支时，必须能说明它
  防止的具体故障、所属边界以及为何现有边界不能处理。
- 禁止构建自圆其说的测试。测试必须从公开合同或独立观察到的行为出发，并能在实现错误时失败；
  不得复制实现算法、为测试专门开放生产路径，或用与实现相同的假设同时构造夹具和预期结果。
  优先验证用户可见结果、公开 ABI 和真实解析输入，避免只证明 mock 按预设返回 mock 值。
- 实现复杂度应与当前需求成比例。未经需求或证据支持，不提前实现兼容层、未来版本、恢复策略或
  极端场景框架；发现这些代码时应先删除或收敛，再继续叠加功能。

## 架构边界

`RuntimeSession` 是进程级生命周期所有者，`src/runtime/core_main.cpp` 是生产组合根。
依赖从 Bootstrap 指向 Runtime 抽象，再经由声明接口到平台服务和外部能力。Runtime 内核不能
包含 NTE 专用头文件，插件不能包含宿主内部头文件，跨线程域工作必须通过 Dispatcher，不能直接
触碰其他组件的所有者。

以下规则不可突破：

- `src/bootstrap/dwmapi/` 必须在 Loader Lock 外工作：仅转发系统导出并异步启动 Core，不放入
  配置、Hook、UI、扫描或插件加载。
- `src/runtime/` 负责 Session 状态、停止传播、服务生命周期和 Dispatcher 所有权。部分启动失败时，
  已启动服务也必须按依赖反序停止。
- `src/platform/windows/`、`src/services/`、`src/diagnostics/` 保持游戏无关，不能依赖 NTE 布局
  或发布游戏专用策略。
- `src/game/ue5/` 负责经过验证的通用 UE 符号和快照；`src/game/nte/` 负责按来源优先级选择活动
  Profile 和 NTE Feature Gate。Profile 不包含或匹配 Build identity；新增偏移或签名必须有 Profile、validator 与降级行为，不能硬编码在插件
  或 UI 中。
- `src/plugin/` 负责 Catalog、包、generation、Scope、热重载和故障隔离。每个插件可见资源都要
  记录在 `PluginScope`，回调不能超出其 generation lease。
- `src/render/dx12/` 与 `src/ui/` 负责 Present/Resize/输入/UI 生命周期。Render 回调不得扫描
  包、加载 DLL、等待生命周期任务或同步执行文件/网络 I/O。
- `include/anomaly/sdk/` 是公开 DLL 边界：仅纯 C ABI、定宽类型、opaque handle、清晰所有权以及
  version/`struct_size` 检查。不得暴露 STL、异常、RTTI 对象、裸 UE 对象、`ImGuiContext*` 或内部头文件。

完整的模块、线程和变更影响图见 `.agents/architecture.md`。

## 构建与测试约定

主仓库统一使用 `windows-vs2022` 配置预设与 `windows-relwithdebinfo` 构建预设。
`build.cmd` 只是 CI 同一命令序列的便捷包装。禁止新建临时 NMake、Ninja、`build/` 或按阶段命名的
主构建树。默认构建只产出 Runtime、Tools、SDK 三个发布组件；开发夹具、诊断探针与 PDB 不属于
发布内容，需要 `build.cmd fixtures|probes|symbols` 显式开启。精确命令与产物路径见
`.agents/tooling.md`。

## 冗余清理约定

清理冗余时：
删除必须有调用图或链接级证据，不能仅凭“看起来没用”移除公开合同、schema getter、Retry API、
旧 ABI table accessor 或测试专用 parser；集中安全原语后，reparse、原子性、超时、rollback 与
quarantine 测试不得弱化；ABI `struct_size`/version 检查、PluginScope generation/lease/ledger、
ABI/线程/回调最外层 `catch (...)`、Dispatcher 锁外销毁 callback 等属于有意设计，不得为缩减行数删除。

## 实时目标与数据安全

实时进程工具只用于明确的诊断任务，不能作为日常验证。`anomaly-cli write`、`patch`、`protect`、
`alloc`、`free` 会修改其他进程，必须得到用户明确授权，并且目标只能是自有测试进程。Profile
重扫和实体扫描即使以读取为主，也需要明确请求和活动 Profile 上下文。后台 tracker 必须在
结束前使用配套 stop 脚本停止。日志、dump、Profile 数据和诊断包都可能含敏感信息，分享前必须审查
并脱敏。

## 快速导航

| 目标 | 首先阅读 |
| --- | --- |
| Runtime 启动与生命周期 | `src/bootstrap/dwmapi/proxy_main.cpp`、`src/runtime/core_main.cpp`、`src/runtime/runtime_session.cpp` |
| 服务与所有权 | `src/runtime/service_graph.cpp`、`src/runtime/runtime_dispatchers.cpp` |
| 插件 ABI 与热重载 | `include/anomaly/sdk/`、`src/plugin/`、`examples/` |
| UE5/NTE 兼容 | `src/game/ue5/`、`src/game/nte/`、`profiles/nte/` |
| 渲染与 UI | `src/render/dx12/`、`src/ui/` |
| 验证与发布门禁 | `CMakeLists.txt`、`.agents/tooling.md` |
