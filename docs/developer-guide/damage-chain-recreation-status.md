# 伤害链式反应复现｜工作状态基线（2026-10-18）

> 本记录用于防止跨聊天重复取证。若与旧聊天摘要、旧 ZIP 检查或 Attack Replay 文档冲突，以本记录与用户最新明确指示为准。

## 当前任务边界

目标是尽可能复现用户提供的视频中可观察到的“伤害链式反应”效果。当前研究路线优先分析并交叉比较仓库内 `5.6.1/` 与 `5.6.1_new/` 的原生代码、类型、函数、参数与配置结构，寻找能实现视频效果的原生机制；旧 DLL 分析优先级暂时后置，作为后续补充/交叉验证材料，而不是当前第一步。不是默认复现 Attack Replay，也不是技能重放、模拟按键或动画重放。视频定义目标表现，但不能单独证明内部机制；Dump 声明也不能单独证明某路径实际被使用。不把当前任务目标套用到未来插件项目。

## 权威材料位置（已纠正）

1. **当前第一优先级：原生 Dump：** 先分析仓库根目录 `./5.6.1/` 与 `./5.6.1_new/` 的原生代码并比较差异，围绕视频表现识别候选原生机制、调用关系、参数和配置。
2. **旧 DLL（暂缓优先）：** 两份存根位于仓库内 `plugin/time...` 文件夹。待 Dump 路线形成候选机制后，再定位并记录两份文件的确切路径、文件名、大小、SHA-256，并将可还原的控制流证据用于交叉验证。
3. **Anomaly：** 当前指定仓库 `Rabb1t79/Anomaly-main`，工作分支 `fix/restore-attack-work-and-repair-20261016`。用仓库源码、公开 SDK/ABI、Profile、validator 与 capability policy 核验桥接边界。
4. **ZIP 已彻底弃用为本任务的证据来源。** 不再扫描、比对或依赖 ZIP 来定位旧 DLL 或 Dump；不再用 ZIP 的文件列表推断当前仓库内容。

## 已完结的旁支：Vehicle 载具插件

用户已明确 Vehicle 载具相关插件工作完结。它不再属于本任务的进行中工作，不应继续列入本任务的后续步骤、待办或进度汇报。保留仓库既有历史代码/文档不等于继续维护该任务；除非用户重新提出，不主动重开 Vehicle 工作。

## 当前执行顺序（用户最新确认）

1. 先读并比较 `5.6.1/` 与 `5.6.1_new/` 原生代码，找出尽可能复现视频效果的候选类、函数、触发时序、配置数据和生命周期关系。
2. 将候选路径逐项标注为“直接证实 / 有证据支持但未验证 / 未知”；不能因名称相似或字段存在就断言实际路径。
3. 检查 Anomaly 现有 ABI/Core 能力是否能合规承载候选原生机制。必要时可按证据最小扩展公开 ABI/Core，不得把“当前没有现成接口”误判为禁止桥接。
4. 再分析两份 `plugin/time...` 旧 DLL 存根，作为机制交叉验证与缺口调查；不把它们作为当前首个调查对象。
5. UI 需求按下文固定执行，不重复向用户询问已经给出的设计。

## 伤害链式反应 UI 既定要求

- 插件/UI/Manifest 统一名称：`NTE 伤害链式反应 / Damage Chain Reaction`。
- 提供启用控制，标签为 `√ 启用`。
- 提供可编辑数字项 `伤害重复次数`，初始默认值为 `1`；用户曾手动输入 `100`，不得误当作旧插件默认值。
- 显示 `绑定=就绪 / 等待` 状态与 `触发=战斗中` 类战斗触发状态。
- 独立统计并显示：`伤害=…次 失败=…次 重放=…次`。
- 隐藏调试诊断信息；详细日志写入插件目录。
- `input`/`input_uint32` 是否需要保留尚未定论：如果后续 Dump、视频行为与实现证据证明它与目标功能无关，就直接移除，不为保留控件而保留。无论是否保留数字输入控件，都必须删除旧 `DT_AbilityInput`、InputID、按下/释放接口路线；不模拟按键。
- 用户描述的目标行为是实际伤害触发后的持续链式效果，目标绑定原敌人；不直接改 HP，不重放技能/动画。此前记录过“2 秒内无匹配新伤害或手动关闭即停止”的规则；在确认与视频表现及 Dump 原生机制一致前，把 2 秒停止条件视为待验证的行为约束，不擅自硬编码为已证实机制。

## 本轮已经执行

- 更新根目录 `AGENTS.md`，提交 `d59ff26d637dd6ef776403e5a7560bdd9cd36ea9`。新增“伤害链式反应旧插件复现”任务级强制规则，并修正 Dump 证据源描述。
- 更新后的规则明确：旧 DLL 从仓库 `plugin/time...` 定位；Dump 从 `./5.6.1/` 与 `./5.6.1_new/` 核验；允许公开 ABI 在 Dump 明确支持且已核验的原生接口上建立桥接，但需检查 owner/参数布局/receiver/线程/对象代际/生命周期/capability/失败处理。
- 读取当前分支的 `AGENTS.md`、`.agents/architecture.md`、`.agents/tooling.md` 与 `docs/developer-guide/attack-replay-legacy-port.md`。后者描述的是 Attack Replay 路线，只能作为旁支背景，不能当作旧 DLL 机制证据。
- 对当前分支直接尝试读取 `5.6.1_new/CppSDK/SDK/HTGame_classes.hpp` 返回 404。此结果只表示该精确路径在该 ref 下未取到文件，**不能推断 `5.6.1_new/` 整目录不存在**；接下来应先定位仓库树/目录实际路径，再取文件。
- 本轮未修改实现源码、Core、ABI、Profile 或 Attack 插件；未构建、未运行 DLL、未做实机验证。

## 旧结论的有效性边界

此前对 Library/ZIP 材料中几个 DLL 的哈希和 PE 结构分析，尚未与用户所指的仓库 `plugin/time...` 内两份存根建立文件身份对应关系。**不得把那些旧样本哈希、22.57 MB 高熵节观察或 6.28 MB 样本字符串结果直接套用到目标旧 DLL。** 它们只能作为已完成但与目标身份未关联的历史分析，不能用于决定实现路线。

此前 Dump 交叉索引曾确认 `ADamageTriggerActor` 包含周期触发字段和相关函数，`FSpawnDamageTriggerActorArray` / `FDamageTriggerActorStaticData` 包含伤害触发 Actor 配置数据。这些只是 Dump 中存在相应声明的证据，不能证明旧 DLL 实际使用了它们。必须在两份目标 DLL 的真实代码/控制流中建立关联后才能形成实现结论。

此前对 Anomaly 服务表的检查表明公开的 UE5 ProcessEvent 服务为订阅/取消订阅，对象服务为快照与查找，不等于公开通用调用/创建入口。Core 内部有受具体反射绑定约束的调用逻辑。**当前策略不是禁止 ABI 桥接**：若 Dump 与旧 DLL 证据支持某个原生接口，可以按项目稳定 ABI、capability、Game 线程、对象有效性/代际和生命周期规则评估最小桥接。不得仅因内部存在 invoker 就开放不受约束的通用调用接口。

## 当前证据状态

| 结论 | 状态 | 说明 |
|---|---|---|
| 旧 DLL 两份存根在仓库 `plugin/time...` | **用户明确提供** | 尚需在仓库树中解析精确路径并记录哈希 |
| 原生 Dump 位于 `./5.6.1/` 与 `./5.6.1_new/` | **用户明确提供** | 需要从当前 ref 核验实际目录与文件 |
| `ADamageTriggerActor` 等 Dump 类型有周期触发相关声明 | **直接证实（旧 Dump 的已读文件）** | 不证明目标 DLL 使用了这些类型 |
| 旧 DLL 实际使用 DamageTriggerActor | **未知** | 需目标 DLL 控制流证据 |
| 公开 ABI 可以桥接 Dump 中的原生接口 | **用户明确规则** | 每条具体桥接仍需核验并遵守 ABI/线程/生命周期约束 |
| 旧 DLL 内部机制已解密或还原 | **未证实** | 不得由字符串扫描、PE 元数据或高熵本身推断 |
| 功能已构建、加载或实机生效 | **未完成** | 本轮未修改/构建/运行 |

## 下一步（按当前优先级执行，避免重复）

1. 定位并检查当前分支内 `5.6.1/` 与 `5.6.1_new/` 的实际目录树，先比较相关原生代码，不因单文件 404 推断目录缺失。
2. 围绕视频表现，整理候选原生调用路径、类型关系、配置结构、触发条件、持续/停止条件和目标绑定语义；每项标注证据等级与文件/行号。
3. 将候选机制与 Anomaly 当前 SDK/ABI/Core 的线程、对象有效性、生命周期与 capability 边界对照，评估最小合规桥接。
4. Dump 路线有可检验候选后，再定位两份 `plugin/time...` DLL，记录身份并分析控制流，作为补充交叉验证。
5. 实现前形成“视频表现 → Dump 证据 → ABI/Core 桥接 → 验证方式”的可追溯链；构建、加载、真实功能生效、实机验证分别报告。

## 防重复规则

- 不再问用户 ZIP 在哪里、不再重新扫描 ZIP、不再把 ZIP 当作目标 DLL 或 Dump 的替代来源。
- 当前阶段不得把旧 DLL 分析排在原生 Dump 分析之前；先从 `5.6.1/` 与 `5.6.1_new/` 找候选机制，再做旧 DLL 交叉验证。
- 不得重复询问已明确的伤害链 UI 项目：启用、默认 1 的“伤害重复次数”、绑定/触发状态、伤害/失败/重放分项统计、隐藏诊断并把日志写入插件目录。
- `input`/`input_uint32` 不属于必须保留的既定需求；后续证据若证明无关，应直接移除。
- Vehicle 载具插件已完结，不再列入本任务的后续计划或进度汇报，除非用户明确重新开启。
- 不再重复“5.6.1_new 精确路径 404，所以目录不存在”的错误推断。
- 不把 Attack Replay 目前的 GameplayEffect 重放模型自动当作旧插件机制。
- 不把 Dump 声明等同于实际调用路径，不把 ABI 可桥接等同于接口已实现。
- 每轮先读本记录和 `AGENTS.md`，只报告新增证据、明确阻塞与下一项具体动作。


## 2026-10-18 新增：Dump 知识索引与候选机制正文核验

- 已读取当前分支完整递归 Git tree，确认 `5.6.1/` 下共有 **6,861 个文件、5 个目录**。文件分布主要为 5,729 个 `.hpp`、1,121 个 `.cpp`、2 个 `.inl`、5 个 `.json`，另有 `.txt`、`.idmap`、`.usmap`。
- 已在 `docs/knowledge-base/native-dump/` 建立分层文件索引：README + 14 个文件清单分片，覆盖该目录树中的全部 6,861 个文件，记录路径、字节数、Git blob SHA 和简要导航注释。**这是目录/文件级初始索引，不代表 6,861 份正文都已逐一阅读；注释已明确标记待正文核验。**
- 已新增 `docs/knowledge-base/native-dump/damage-chain-candidates.md`，对 `ADamageTriggerActor`、`AHTAbilitySpawnActor`、`UHTAbilitySpawnDamageTriggerActor`、`FHTGameplayEffectContainer`、`FSpawnDamageTriggerActor`、`FSpawnDamageTriggerActorArray`、`FDamageTriggerActorStaticData` 与相关参数布局的字段、偏移、大小、关联和证据限制做了正文级整理。
- **直接证实（声明层）**：`ADamageTriggerActor` 有周期触发相关函数和 Runtime period/duration/count/max-count 字段；保存了 `TArray<AHTPlayerCharacter*>` 类型的 `OverlapTriggerLinkTimeCharacters`；`FSpawnDamageTriggerActor` 配置包含 Actor 子类、spawn condition/location/rotation/scale 对象、`DamageTriggerConfigID` 与 GameplayEffect 容器；`FDamageTriggerActorStaticData` 包含延迟、持续时间、周期和最大触发次数字段。
- **重要限制**：`HTGame_functions.cpp` 中的生成函数包装通过反射 `ProcessEvent` 调用，不是原生业务实现体；目前不能由它证明周期计时的真实行为、原始受击目标绑定、自动启动关系或多目标独立性。候选机制仍是“有证据支持但未验证”，不是已确认的复现路线。
- **第二套 Dump 阻塞已进一步核验**：当前分支树未出现 `5.6.1_new/`；在 `main`、`recreation/damage-chain-native-20261017`、`recovery/analyze-old-plugin-dll-20261017`、`experiment/damage-chain-legacy-ui-20261017`、`feat/damage-chain-reference-ui-20261017` 这五个相关分支/提交树中也未找到。此结论仅限这些已检查的仓库树，不推断其他仓库、用户本地或未检查来源不存在。
- 本次只新增知识文档与状态记录，没有改插件/Core/ABI 源码；未构建、未加载、未做实机验证。


## 2026-10-18 新增：计时状态与新版 Dump 实际可读性复核

### 本轮来源身份
- 仓库：`Rabb1t79/Anomaly-main`
- 工作分支：`fix/restore-attack-work-and-repair-20261016`
- 本轮核验了 Dump 与 Anomaly 接口，并更新本状态记录和 `AGENTS.md`；没有修改功能源码、Core、ABI、Profile，没有构建、运行 DLL 或实机测试。

### 新版 Dump 路径纠偏
- **直接证实：** 当前工作分支根目录内容列表含 `5.6.1/`，不含 `5.6.1_new/`，也不含 `5.6.1-new/`。
- **直接证实：** 仓库 `main` 分支存在目录 `5.6.1-new/`（连字符，不是下划线）。
- **直接证实：** `main:5.6.1-new/CppSDK/SDK/HTGame_classes.hpp`、`HTGame_structs.hpp`、`HTGame_parameters.hpp` 与 `HTGame_functions.cpp` 经 GitHub Contents API 读取时返回 Git LFS 指针，而不是源文件正文。指针分别记录的对象 SHA-256/大小为：classes `7112b6e93246fb56622db8acf258559d29087f73879e46204947ea3ca92cba8e` / 15,466,064 bytes；structs `c85b2ca5d5375b25ea8aa6c5db46e14d46164a7051df6f470cba6f47909a4d39` / 6,966,999 bytes；parameters `272ba3b8b3fc2a8f07bed7e94f94edfce252161b878439d30bec71aa1506934d` / 10,749,434 bytes；functions `b922ed74bf23c37e3e72a476a46efcbc742c559a1795a19109c5c0134d8ddac2` / 18,542,548 bytes。
- **结论边界：** 不能据此说新版 Dump 不存在；也不能把 LFS 指针当作已读的新版 SDK 正文。当前仍未完成 `5.6.1/` 与新版正文的差异比较。后续先解决 LFS 对象的可读性/确认正确分支路径，不要重复把一次 404 当成目录不存在。

### 旧 Dump 的计时与目标状态线索
来源均为当前工作分支 `5.6.1/` 的实际 Blob 原文：
- `CppSDK/SDK/HTGame_classes.hpp`，Blob SHA `526091ab6679ac6833634e5a6d08d807e90dbf0c`：`ADamageTriggerActor` 在第 20,942 行；`OverlapTriggerLinkTimeCharacters` 是 `TArray<AHTPlayerCharacter*>`，偏移 `0x05A8`；`RuntimeTriggerPeriod`、`RuntimeTriggerDuration`、`RuntimeTriggerCountPerPeriod`、`RuntimeMaxTriggerCount` 分别位于 `0x05E8`、`0x05EC`、`0x05F0`、`0x05F4`；同一类声明 `OnPlayerBeginOverlap`、`OnPlayerEndOverlap`、`StartTriggerGamePlayEffect`、`TriggerPeriodGamePlayEffectEvent`。
- 同文件第 29,846–29,857 行的 `UFindSpawnLocationObj_Target` 声明 `LocationOffset`、`TargetLocationOffset`、`UseToTargetDirection`、`IncludeTargetCapsule`、`PreLocationTime`。
- 同文件第 35,020–35,023 行的 `UHTAbilitySpawnDamageTriggerActor` 持有 `FSpawnDamageTriggerActorArray` 配置。
- `CppSDK/SDK/HTGame_structs.hpp`，Blob SHA `cc6781d70b782a0e3ec6cf06dfbd75f323f04a53`：`FSpawnDamageTriggerActor` 包含 Actor class、生成条件/位置/旋转/缩放对象、`DamageTriggerConfigID`、`FHTGameplayEffectContainer` 与 `RandomCountCurve`；`FSpawnDamageTriggerActorArray` 包含 `SpawnArray`、`nRandomSpawnCount`、`SelectTargetType`、`SecondSelectTargetType`；`FDamageTriggerActorStaticData` 包含 `DelaySpawnTime`、`duration`、`DelayDestoryTime`、`TriggerOnSpawn`、`DelayTriggerTime`、`TriggerLinkTimeDuration`、`Period`、`MaxTriggerCount`。
- `5.6.1/GObjects-Dump.txt`，Blob SHA `4ea559ff564c3c68bcba226368e8839b4dee0034`：`DamageTriggerActorDataTable.DamageTriggerActorDataTable` 对象位于第 199,415 行；找到 74 条 `HTAbilitySpawnDamageTriggerActor` 文本命中，其中 21 条为具有父资产路径的对象实例/默认对象，例如 `GA_Lacrimosa_MeleeModA`、`GA_Lacrimosa_PerfectEvade`、`GA_Shinku_Skill_Rage`、`GA_Female051_Melee` 等。该清单只能证明对象/资产引用存在，不证明其与目标视频的关系。

### 计时/重置结论边界
- **直接证实：** Dump 声明了 Actor 级运行计时/计数变量、目标角色数组、重叠开始/结束事件、周期触发函数，以及数据配置中的周期、持续时间、最大次数和延迟字段。
- **有证据支持但未验证：** 原生机制可能由能力配置生成一个或多个 `ADamageTriggerActor`，并由 Actor 的周期状态驱动 GameplayEffect 触发；目标位置可由 `UFindSpawnLocationObj_Target` 配置计算。GObjects 中多个能力资产确实引用了此生成组件。
- **未知：** 周期变量是否每个 Actor 独立递增、同一 Actor 内多个目标是否有独立计时器、重叠结束是否重置计数、死亡/离开范围/销毁时的停止规则、目标位置是生成时锁定还是每次触发重新计算，以及视频中的链式效果是否使用此路线。Dump 只展示字段和声明，没有给出这些函数的原生实现体；生成的 `*_functions.cpp` 包装不能证明蓝图/业务逻辑。
- **下一步：** 继续核对可读的原生类、配置/资产引用和 Anomaly 当前合法桥接边界；在新版 LFS 正文未可读前，不宣称完成版本差异对照，不改实现源码，不新增 ABI。

- 协作规则清理：已从 `AGENTS.md` 的活动规则中移除 Vehicle 载具实现要求，仅保留“Vehicle 已完结、除非用户重新提出否则不重开”的状态说明。


### Anomaly 合法桥接路径复核（本轮新增）

- `include/anomaly/sdk/services/ue5.h`，Blob SHA `8ed83052b06928ae584a684060e7520d1c95c6ea`：公开 `anomaly.ue5.process-event` 服务只提供订阅/取消订阅；`anomaly.ue5.objects` 提供对象快照与 `find_exact`；Framework 服务提供 Game 线程 ID、Tick 序号与线程检查。这里没有公开通用 `ProcessEvent` 调用或 Actor 创建函数。
- `include/anomaly/sdk/services/nte.h`，Blob SHA `0833833bcb4b1d5ce9dc2ac8bbf8a4c49908e22a`：NTE Actors 服务提供缓存帧、快照和受限属性读取；Combat 服务提供伤害事件读取。现有 `anomaly.nte.damage-replay` 明确属于已捕获 GameplayEffect 调用的重放服务，本任务不把它预设为实现路线。
- `src/game/ue5/ue5_process_event.cpp`，Blob SHA `5d7a0a1025b55360fd4b26f7ebd52ef37595e643`：Core 内部存在 `CreateUe5ProcessEventInvoker`，前置条件包括 `ValidateProcessEventAbi`。
- `src/game/ue5/ue5_nte_adapter.cpp`，Blob SHA `a7c0c7158f15d2cfa8e1d986a35933da5d00f001`，第 11,547 行附近：Core 内部存在经过反射参数布局校验的 `GameplayStatics.BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` 调用路径；该具体函数最终要求 spawned object 通过 Vehicle 类型和 Owner 后置条件。它证明 Core 内部有受约束的原生反射调用/延迟生成先例，但不等于插件已有可调用的通用 Actor spawn ABI，也不证明可直接用于 `ADamageTriggerActor`。
- **当前桥接结论：** 公开 ABI 没有现成的通用 Actor 创建/函数调用入口；Core 内部有经过 Profile/反射 ABI 校验的调用原语与专用生成实现。若 Dump 后续足以证实 DamageTriggerActor 路线，应优先评估新增窄范围、具名、capability 受限的 Core/ABI 服务，逐项验证类/函数 owner、参数布局、SpawnConfigID/数据表读取、目标绑定、Game 线程、对象代际、生命周期和失败后置条件；不开放无约束的通用反射调用，不新增 ABI，直到上述路线证据充分。
- 本轮只核验并记录接口，不改 Core/ABI/插件源码，不构建。Vehicle 已完结；其既有内部代码只作为 Core 结构证据，不重新开启 Vehicle 任务。

## 2026-10-18 当前工作分支同步（以本节覆盖较早的路径/验证状态记录）

### 当前分支与 Dump 证据
- 当前工作分支：`work/nte-damage-chain-reaction-independent-20261018`；本轮开始时 head 为 `2e073e95d1a27d9d8aeec610b3975482a1746a0f`。所有本轮修改仍只提交到该工作分支。
- 已纠正此前的 LFS 判断：`main` 上的 `5.6.1-new/` 使用 Git LFS，但工作流 `nte-dump-native-evidence.yml`（运行 [39117662599](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39117662599)）已成功物化所选文件并生成 `nte-native-damage-dump-evidence` 报告。
- 报告记录 `HTGame_parameters.hpp` 的 SHA-256 在两版相同；`HTGame_classes.hpp`、`HTGame_structs.hpp` 和 `GObjects-Dump.txt` 的版本哈希不同。聚焦声明 diff 对 `FHTDamageEvent`、`FBufferData`、`UHTAbilitySystemComponent`、`AHTAbilityCharacter`、`UHTAbilitySpawnDamageTriggerActor`、两个 ASC GameplayEffect 应用入口及 `OnDamaged` 参数块均报告 “No textual difference in extracted block”。这不是全文件完全相同的证明，也不是运行时安全调用的证明。
- 报告只作仓库证据记录；它不会自动被复制进运行时包，也不应把其中的 GObjects 运行时地址当成固定 RVA。

### 当前实现与验证边界
- Core 当前 `anomaly.nte.damage-chain-reaction` 的 `apply_next` 走 `DamageTriggerActor` 周期事件，不再把捕获的 ASC 参数缓冲区作为后续伤害调用。该路由仍需实机证明每次调用是否产生一条匹配的真实伤害。
- 最近一次已完成的 Core ABI 校验：运行 [39124127839](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39124127839)，结果成功。相同 head 的完整 Windows 工作流 [39124127821](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39124127821) 在最近一次读取时仍为运行中；不得把较早提交的通过结果当作该 head 的结果。
- 发现独立构建工作流之前指向旧的 `plugins/NteDamageChainReaction`，不是当前目标 `plugins/DamageChainReaction`。本次同步修正了工作流源文件、包含目录、manifest/产物名称，并加入 `RateQueue` 确定性测试。待新工作流运行成功后才记录为构建验证通过。
- `RateQueue` 修正只影响调度安全性：非有限速率值回退到 100 次/秒；时间间隔向上取整，避免因时钟精度截断而让调度快于设定速率。没有改变“首次原生命中不计数、每次额外真实命中逐一确认、停顿后不突发补发、成功数以真实伤害事件为准”的行为合同。
- 尚未完成：实机确认 1 次正常攻击 + N 次额外伤害严格成立；确认原生 Actor 的周期事件对原目标真实结算；确认无递归扩增、多目标独立、长时间停顿后继续完成队列。不得把 ABI 编译通过称为功能已生效。

### 同步修正：次数输入范围与路线文案
- 本轮复查发现 UI 将“额外结算次数”硬编码限制为 1–10，而插件/Core 的数量上限为 1,000,000，因此无法通过 UI 设置要求示例中的 100 次。已改用 `kMinimumExtraCount` / `kMaximumExtraCount`，默认值仍为 1。
- UI 提示和 manifest 原本还写着“直接 ASC GameplayEffect 应用上下文”，与已切换的 `DamageTriggerActor` / `EffectContainer` 路线不一致；已同步修正。此项只是界面/描述准确性修复，不是实机伤害验证。
- 修改文件：`plugins/DamageChainReaction/plugin.cpp`、`plugins/DamageChainReaction/manifest.json`，并在同一提交更新两份进度/行为合同 MD。后续由本分支 Windows/Core/独立插件工作流验证。

### CI 状态补记（源代码提交 `9a53edc3b398e559ca469bdd55c507b74216d3e2`）
- 独立插件工作流 [39125202472](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202472) 已成功：构建当前 `plugins/DamageChainReaction`、RateQueue 确定性测试 1/1 通过、测试包已上传。
- 测试包 artifact：`DamageChainReaction-windows-x64`，SHA-256 `9e08a8a74ebc3489a6d9efd48c3ffc6767dacd8263fe461492196dfb9564d06a`。这是可用于进一步验证的测试包，不是已实机确认的最终成品。
- 本次 MD 更新时，全量 Windows 工作流 [39125202479](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202479) 与 Core/ABI 工作流 [39125202470](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202470) 仍在运行；后续以 GitHub 实际结果为准。
- 实机尚未证明 DamageTriggerActor 周期事件每次产生恰好一次额外真实伤害，或 100 次配置能在游戏内完整兑现。构建和队列测试不能替代该验证。

### Dump 取证覆盖补强（2026-10-18）
- 扩展后的 LFS 取证运行 [39125616938](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125616938) 已成功完成，读取源仍是 `main` 的两版 Dump，未修改 `main`。
- 自动化报告成功提取 15 个聚焦声明块，缺失标记数为 0；15/15 块在两版中均报告文本一致。新增覆盖：`ADamageTriggerActor`、`AHTAbilitySpawnActor`、`FHTGameplayEffectContainer`、`FSpawnDamageTriggerActor`、`FSpawnDamageTriggerActorArray`、`FDamageTriggerActorStaticData` 及 `HTAbilitySpawnActor_OverrideEffectContainer`。
- 读到的 Dump 声明：周期 Actor 的运行字段为 `RuntimeTriggerPeriod=0x5E8`、`RuntimeTriggerDuration=0x5EC`、`RuntimeTriggerCountPerPeriod=0x5F0`、`RuntimeMaxTriggerCount=0x5F4`；父类 `AHTAbilitySpawnActor` 的 `EffectContainer=0x3A8`、`EventData=0x3D8`；`OverrideEffectContainer` 参数 `InEffectContainer` 为 `FHTGameplayEffectContainer`，大小 `0x28`。
- 以上只证明两个 Dump 对应声明块一致。完整 classes、structs 和 GObjects 文件哈希仍然不同；没有证据可从声明直接推断手动调用周期事件必然造成一次真实伤害，实机伤害确认仍未完成。

### CI 结果刷新（2026-10-18）
- 已确认源代码提交 `9a53edc3b398e559ca469bdd55c507b74216d3e2` 的三项检查全部成功：独立插件构建 + RateQueue 测试/打包 [39125202472](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202472)、全量 Windows 框架 Runtime/打包 [39125202479](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202479)、Core/ABI 校验 [39125202470](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202470)。
- 随后的提交 `eed4357560b63c8a6b5619465102861713080d2f` 只修改取证脚本、取证工作流和文档；扩展取证运行 [39125616938](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125616938) 已成功，15 个聚焦声明块均提取成功且两版文本一致。
- 当前所有已通过的 CI 只证明源码编译、SDK/ABI 校验和队列测试通过，不证明游戏中伤害已生效。剩余核心验证是：一次原生命中后，N 次周期触发必须产生恰好 N 次针对原目标的新真实伤害事件。

### 追加核验：EventData 内层结构布局（2026-10-18）
- 继续审查 Core 的原生触发适配器时，发现还需要对照 `GameplayAbilities` 包内的 `FGameplayEventData` 声明：适配器验证外层 `AHTAbilitySpawnActor.EventData` 大小为 `0xB0`，并要求内层 `Instigator` / `Target` 两个对象属性分别位于 `0x08` / `0x10`。旧版 Dump 声明支持这些值，但之前自动报告没有包含这个单独的结构头文件，因此不能说这一处已完成双版本核验。
- 本轮把 `5.6.1/CppSDK/SDK/GameplayAbilities_structs.hpp` 与 `5.6.1-new/CppSDK/SDK/GameplayAbilities_structs.hpp` 纳入 LFS 取证输入，并新增 `FGameplayEventData` 聚焦块。工作流在脚本或自身改动时会自动运行。
- 本次只增加证据覆盖，没有更改 Core 的硬编码布局或调用逻辑。需等待新报告确认两版的 `FGameplayEventData` 块是否一致，再决定后续实现动作。
### 最新证据任务结果（2026-10-18）
- `FGameplayEventData` 双版本取证运行 [39126184168](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39126184168) 已完成且成功。两份 `GameplayAbilities_structs.hpp` 均由 Git LFS 成功物化，文件大小 150,574 bytes、SHA-256 同为 `992b87956ae12d195bb8d1a3b492e4b9c5ef520b5bf5acedadcff6496fa6a556`。
- 最新聚焦报告共 16 个声明块：16/16 成功提取、缺失标记 0、不可用 diff 0、16/16 块文本一致。`FGameplayEventData` 布局为大小 `0xB0`、`Instigator` offset `0x08`（8 字节对象属性）、`Target` offset `0x10`（8 字节对象属性），与当前适配器布局验证条件一致。
- 最新报告位置 `/mnt/data/NTE-damage-Dump-evidence-5.6.1-vs-5.6.1-new.md`，SHA-256 `4c74d25bc53d3ccf2e3574aeb65011c3d2735f8eae3c5878654c61f802eb57e2`。两版本的完整 HTGame classes、HTGame structs 和 GObjects 文件仍存在哈希差异；聚焦声明一致不能代替原生调用的实机伤害验证。

### 目标选择类型证据补齐（2026-10-18）
- 当前 Core 会把 `HTGATargetType_UseEventData` 设置为 `FHTGameplayEffectContainer.TargetType`，并验证它属于 `HTGATargetType` 子类。上一份 16 块报告覆盖了容器和内层 `FGameplayEventData`，但没有独立提取这两个目标类型类声明。
- 本次扩展双版本 LFS 取证脚本：增加 `UHTGATargetType` 与 `UHTGATargetType_UseEventData` 聚焦类块，并在 GObjects 搜索中加入基类名称。等待下一次报告确认两版都能提取且布局一致；类型名本身不证明目标解析的运行语义。
### 目标类型双版本核验结果（2026-10-18）
- 新增目标类型的取证运行 [39126720146](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39126720146) 已成功。当前报告共有 18 个聚焦声明块，缺失标记 0、不可用 diff 0、18/18 块在两版中均文本一致。
- `UHTGATargetType` 声明 `GetTargets(OwningActor, TargetingActor, FGameplayEventData&, OutTargetData, OutHitResults, OutActors) const`；`UHTGATargetType_UseEventData` 的类大小与父类相同，没有额外字段，继承 `GetTargets`。这证明该目标解析接口接收 `FGameplayEventData` 并可输出目标 Actor/TargetData。
- 这仍不能证明具体实现一定返回 `EventData.Target`，也不能证明调用 `K2_OnTriggerPeriodGamePlayEffect` 必然造成一次真实伤害。下一步需要针对当前游戏版本的只读运行时目标解析探针/事件追踪，或实机伤害确认。
- 最新报告 `/mnt/data/NTE-damage-Dump-evidence-5.6.1-vs-5.6.1-new.md`，SHA-256 `2b7b9b5657b00571dced930f5dfe0a7f2f5e759cc4259421514700efa0ccdc8f`。
### 原生调用嵌套与 Actor 生命周期审查（2026-10-18）
- 静态检查发现，没有明显的同线程重复获取状态主锁的路径：Core 在持状态锁时设置 awaiting-confirmation，随后释放锁调用反射周期事件，调用结束后再重新上锁并排空伤害捕获队列。伤害钩子释放自身短暂管理锁后才调用原始处理函数和回调。因此当前源码没有显示出显而易见的这一路径死锁。
- 临时 chain provenance 在直接调用 `K2_OnTriggerPeriodGamePlayEffect` 前设置、作用域结束后恢复；这结构上允许同步嵌套伤害回调，但是否在捕获回调时把 chain ID 正确写入事件，仍需运行时确认。
- **新识别的生命周期风险，尚未修复：** `SpawnNativeDamageTriggerActorLocked` 先调用 `FinishSpawningActor`，然后 `ConfigureNativeDamageTriggerActorLocked` 才设置 `EventData`、`EffectContainer` 和运行期周期/次数字段。当前 Dump 只证明反射声明，不证明完成生成时是否自动启动计时触发。插件明确会为每个队列项直接调用 `K2_OnTriggerPeriodGamePlayEffect`，没有显式调用 `StartTriggerGamePlayEffect` / `TriggerPeriodGamePlayEffectEvent`，但这不足以证明 Actor 不会自行产生额外伤害。
- 目前不凭一般 UE5 经验或猜测更改 spawn 生命周期/周期字段。下一步需要用项目证据或只读运行时诊断确认 actor 的启动语义和事件是否每次恰好产生一次伤害，再作最小修复。当前 18 块 Dump 对照、CI 和 RateQueue 测试均不能替代实机伤害验证。
### Runtime Probe 工作流错配修正（2026-10-18）
- 发现旧 `.github/workflows/nte-damage-chain-runtime-probe.yml` 实际构建完整 Runtime 后，却校验/打包 `plugins/NteAttackReplay` v1.9.7；并且它没有 `push` 触发器，独立工作分支的更新不会自动触发该探针。这与当前目标链式反应插件不一致。
- 已把探针切换到 `plugins/DamageChainReaction`，验证 manifest ID `anomaly.builtin.nte-damage-chain-reaction`、版本 `0.1.0`，产物命名改为 `DamageChainReactionRuntimeProbe-windows-x64.zip`，并在相关分支 push 时触发。
- 本次修改和进度记录在同一 Git 提交。工作流结果仍只证明 Runtime/Core/插件的构建与打包；不证明游戏中每次周期调用恰好产生一次额外伤害。
### RateQueue 验收测试补强（2026-10-18）
- 新增五秒停顿测试：暂停超过一秒后任务仍有效；恢复执行时只消耗一次已确认任务；下一次执行仍按设定间隔排期，不会把积压任务瞬间补发。
- 新增完整 100 次推进测试：每次成功只消耗一个额外任务，直到第 100 次成功才完成队列。这验证了调度和计数语义，不模拟游戏原生伤害，也不能证明每次 Core 调用都对目标造成伤害。
- 已验证：独立插件构建工作流 [39127512880](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512880) 成功，RateQueue 测试 1/1 通过，测试包已上传。
### 当前提交完整 CI 结果（commit `8dd451f2abb7079a0ec2b275b025305186007888`）
- **独立插件 + 调度器测试** [39127512880](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512880)：成功；RateQueue 测试 1/1 通过，`100% tests passed`。插件测试包 SHA-256：`4b609129500c3db882ccf0bb25a33d7a010486ee2d534a9438ad8b42d4fc8811`。
- **Core / ABI 校验** [39127512883](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512883)：成功；已生成 `nte-damage-chain-core-validation` 诊断工件，SHA-256：`73568e4be73ac5fe38acf61eec272b1d9f3030d37bc28de01ab8daaff26df564`。
- **全量 Windows 构建** [39127513011](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127513011)：成功，包括构建、ABI 快照与打包接口验证。
- **Runtime 探针包** [39127512930](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512930)：成功；它实际校验的是 `plugins/DamageChainReaction`，工件 SHA-256：`59c9a40a63dc44b56ee7196f02a332aeaa769ddcd8f391c6884f50c8107be080`。
- 上述成功只证明该提交的构建、ABI 与包完整性检查通过，不是游戏实机效果证明。周期事件是否确实每次造成一次伤害、100 次是否完全兑现、目标是否稳定、是否存在 Actor 自主触发，仍未验证；生成后才写入容器/周期字段的生命周期风险依然开放。
### 当前可下载测试包校验（2026-10-19）
- 独立插件包： [DamageChainReaction-windows-x64.zip](sandbox:/mnt/data/DamageChainReaction-windows-x64.zip)，SHA-256 `a708006d8fa6bab19e08792985e2f6982c95fc791a0d27ee4025a4b6b112b381`；包内有 `plugin.dll`（48,640 bytes）和 manifest（ID `anomaly.builtin.nte-damage-chain-reaction`、版本 `0.1.0`）。
- Runtime 探针整包： [DamageChainReactionRuntimeProbe-windows-x64.zip](sandbox:/mnt/data/DamageChainReactionRuntimeProbe-windows-x64.zip)，SHA-256 `a7c2d089ddf4e35d8cca788b07a9c851cddbc5198b5ca6d757b56fc7027c998d`；包含同次验证构建的 `Anomaly.Core.dll` 与 `plugins/DamageChainReaction/plugin.dll`（94,720 bytes），manifest ID/版本匹配。已检查整包，不含 `5.6.1`、`CppSDK`、Dump 目录或 `GObjects-Dump.txt`。
- 上述哈希针对从 GitHub artifact 封装中解开的直接 ZIP；GitHub Actions artifact 本身的 digest 是外层包哈希。推荐先使用 Runtime 探针整包进行后续实机测试，因为 Core 与插件来自同一个通过构建/ABI 检查的源码提交。注意它仍是测试包，未完成实机伤害行为验收。

## 2026-10-19：实机日志定位 capability 注册缺口

- 收到 Runtime 日志：`anomaly.builtin.nte-damage-chain-reaction` 被 `unknown-capability` 与 `required-service-mapping-missing` 拒绝，触发插件启用失败。
- 对照 `src/plugin/plugin_capability_policy.cpp` 后确认原因：Core 只登记了旧的 `nte-damage-chain` / `anomaly.nte.damage-chain`，没有登记当前独立实现使用的 `nte-damage-chain-reaction` / `anomaly.nte.damage-chain-reaction`。现已补齐新 capability 和一对一服务映射，不借用旧 service，也不回退到 damage-replay。
- 新增回归测试，直接调用实际 `ResolvePluginCapabilityGrant`：当前 DCR manifest 必须通过；只带旧 capability 的 manifest 必须继续被拒绝。插件工作流会同时编译和执行此测试。
- 同步核对时发现 `abi/anomaly-sdk-v1-windows-x64.json` 漏掉了 `apps/abi_snapshot/main.cpp` 已输出的 DCR 新请求/快照/服务结构、状态枚举、标志位与服务登记。下一步从 Windows CI 生成的快照中取得完整基线后再同步；不要凭手写偏移覆盖真实 ABI 快照。
- 同一日志中的其他插件 `missing-cxx-throw-bridge` / `static-tls` 属于独立加载兼容问题，本次不声称已修复。DCR 实机伤害次数、目标绑定及 Actor 是否自主触发仍待游戏内验证。
