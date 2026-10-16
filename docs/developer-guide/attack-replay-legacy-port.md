# Attack Replay legacy-runtime port map

本文用于把现存的旧运行时截图转化为当前 Anomaly-main 的移植契约。截图是功能架构参照，不作为排查旧版本故障的任务。

## 截图中可以确认的旧实现结构

旧运行时显示了以下 Hook 类别：

- `anomaly.nte.damage-native / character-on-damaged`：原生角色伤害事件入口。
- `anomaly.nte.gameplay-effect-exec / combat-exec-0..3`：四个 GameplayEffect Native Exec Hook。
- `anomaly.ue5.process-event` 与 `anomaly.ue5.actor-process-event`：反射调用与 Actor 调用入口。
- 服务清单包含 `anomaly.nte.combat v1`、`anomaly.nte.damage-replay v1`、`anomaly.nte.skills v1`、`anomaly.nte.skill-invocation v1`。

旧截图中的控制器按下/释放桥属于已淘汰的实现路线。当前仓库已从插件、Core、Profile、SDK、ABI 快照和 capability policy 中移除该桥；伤害链只允许复用经过校验的原生 GameplayEffect 调用。

截图中的地址只是旧进程的一次运行地址，不得硬编码到新版本。当前移植必须从当前 Profile、反射对象与经过校验的 Native Exec 指针重新建立绑定。

## 当前实现的对应关系

| 旧运行时职责 | 当前代码入口 |
| --- | --- |
| 原生 `CharacterOnDamaged` 事件 | `Ue5NteAdapter::OnDamageEvent` → `EnqueueCharacterDamage` |
| 读取原始 DamageEvent 与 DamageGEDef | `CaptureCharacterDamageBytesLocked` 与当前 Profile 的 `damageEvent.*` 布局 |
| 发现四类 GameplayEffect Native Exec 目标 | `Ue5NteAdapter::CombatExecFunctionTargets` |
| 接收 Native Exec Hook 参数 | `Ue5NteAdapter::OnCombatExecFunction` → `EnqueueCombatProcessEvent` |
| 同时兼容 ProcessEvent/ActorProcessEvent | `OnProcessEventPre`、`OnActorProcessEventPre` |
| 保存原始 GameplayEffect 参数及标签数组 | `CaptureReplayEffectInvocationLocked` |
| 关联原始伤害与实际 GameplayEffect 调用 | `FindReplayContextForDamageLocked` / `ReplayContextMatchesDamageLocked` |
| 排除友方目标 | dump 反射校验的 `HTAbilityCharacter.IsFriendlyTarget(Target)`；校验不可用时拒绝重放 |
| 按 Game Tick 重用原始效果调用 | `AnomalyNteDamageReplayServiceV1` 与 `plugins/NteAttackReplay/plugin.cpp` |

## 链条生命周期与角色切换

- 一条链的根是启用前后由“当前控制角色”产生的一次真实伤害事件；根事件捕获的攻击者、目标、DamageGEDef 和经过验证的 GameplayEffect 调用上下文在本条链中保持固定。
- 切换当前角色不得自动取消已经开始的链，也不得把旧链的攻击者 ASC、目标或 GameplayEffect 参数改绑到新角色。每次追加仍使用同一根事件的固定快照，并要求原对象句柄、World 与 object generation 仍然有效。
- 角色切换只影响后续新链的起点：旧链结束或被明确停止后，捕获器再从新的当前控制角色接收下一次真实伤害。
- 角色切换与对象失效是不同情况。若旧攻击者/目标被销毁或换代、World generation 改变、原生上下文不再能通过身份校验，Core 必须拒绝继续重放；不得为了跨角色延续而跳过代际、目标或 Effect 校验。
- 插件在链进行期间冻结根事件，不能让后续命中自动覆盖根快照。正常完成下一次链式追加的证明仍然是观察到相同 World、原攻击者、原目标和新序号的真实伤害事件，而不是仅凭重放调用返回成功。

## 移植约束

1. 重放根必须来自真实的 `CharacterOnDamaged` 事件；不得以键位、动画、技能激活或动作回放替代。
2. 重放必须复用与该伤害根严格匹配的原生 GameplayEffect 调用，至少约束 Effect identity、攻击者 ASC、目标 ASC/接收对象、函数路径与对象代际。初始关联先完整验证这些身份字段，再仅从伤害事件同一 Tick 或此前最多 8 个 Game Tick 内的调用中选择时间最近者；未来 Tick 的调用不得关联。该窗口仅用于容忍伤害通知延迟，不放宽 Effect/ASC/函数校验，超窗或没有身份匹配的候选仍拒绝重放。
3. `FBufferData` 中的 `FGameplayTagContainer` TArray 必须复制到自有内存；不得在后续 Game Tick 使用原调用栈里的数组指针。
4. 同一次原生调用可能同时经过 ProcessEvent 与 Native Exec Hook。只有原参数缓冲区地址、receiver、function、tick、kind、参数字节以及已复制标签一致，且观测来源互补时，才把它们合并成一条调用记录；相同入口重复触发的独立调用不得被合并。
5. 每次连锁的间隔以真实 Game Tick 计数，间隔为 1 表示每个 Game Tick 至多发起一次原生效果调用。只在之后观察到同 World、同攻击者、同目标、同来源的新 DamageEvent 时计数。
6. 运行时诊断应区分“ProcessEvent 调用已返回”“Native Exec Hook 观察到调用”与“目标产生新伤害”这三个不同事实。
7. 旧截图中的 Hook 地址、旧进程地址与临时 Profile 值仅作参考；不移植为固定地址。5.6.1 CppSDK/dump 只作布局与反射签名参考，不纳入 Runtime、SDK 或 Tools 发布包。

## 当前伤害调用关联规则

- `ReplayContextMatchesDamageLocked` 先验证原始 DamageGEDef、攻击者/目标 ASC、receiver、函数路由、来源 Actor 与对象代际；这些校验失败的候选不会因为 Tick 接近而被接受。
- `FindReplayContextForDamageLocked` 随后限制候选必须在事件 Tick 之前或同 Tick，且不早于 8 个 Game Tick；多个精确匹配时选择时间最近的调用，同 Tick 时沿用新捕获优先的确定性顺序。
- 日志使用 `tick_delta_gt_8`、`future_tick` 和 `nearest_exact_tick_delta` 区分超出时间窗、未来调用和已通过身份校验的最近时间差。

## 目前本轮代码改动

- 对 GameplayEffect 捕获记录增加 ingress 来源与原参数缓冲区标识。
- 合并由 ProcessEvent/ActorProcessEvent 与 Native Exec 对同一调用产生的重复观测；不按相同参数字节单独合并独立调用。
- 返回所选原生调用路由、捕获入口来源、重复观测次数、匹配候选数以及调用前后目标 HP 探针结果。
- 单次验证只在来源 ID 有效且完全一致、目标出现新正伤害事件、候选数为 1、HP 探针显示同步下降时提供支持性证据；Native Exec 命中状态也会独立展示。这不是对全部原生路径已经覆盖的证明，仍须在游戏中用实际日志确认。

## 尚待完成的验收

- Windows Core/Runtime 完整构建与 ABI/SDK staging 检查通过。
- Attack Replay 插件 MSVC 构建通过。
- 运行时产生可读日志，明确显示所选 route、`native_exec_seen`、`matching_candidates`、新伤害事件与 HP 探针。
- 游戏内对单次普通攻击进行一次受控验证，再测试投射物、多段与范围伤害。
- 在没有实际运行时日志时，不把 CI 绿色称作游戏内功能通过。
