# NTE V1 基底与 `5.6.1-0+UE5-HT` Dump 契约

## Source of truth

1. **宿主源码基底**：用户交付的 `Anomaly-2.3.0-alpha.1.zip`。此 ZIP 是 V1 修缮后的代码基底，不包含后续单独新增的 NTE ABI/服务物件；合并时保留其既有修复，再把新增的公开合同和实现逐项迁移回来。不能直接用旧远程分支覆盖它。
2. **游戏原生接口证据**：用户交付的 `5.6.1-0+UE5-HT.zip`。UE/NTE 反射信息、模块、类、继承、函数 owner、参数 ABI、对象布局和偏移必须来自该包里的 Dump。其他游戏构建、通用引擎文档、名字近似的无关源码都不是替代依据。
3. **公共合同证据**：当前仓库的 SDK C ABI、schema、ABI snapshot、Profile validator 和文档。此类项目内部合同要与源文件同步，但不能反过来证明一个游戏函数在目标 Dump 中存在。

## 已从目标 Dump 核实的 Vehicle 路径

- `HTPlayerController.BP_GetCurrentDriveVehicle()`：当前驾驶载具返回路径，receiver 为本地 PlayerController。
- `HTPlayerCharacter.BP_GetCurrentDirvingVehicle()`：Dump 中备用路径，拼写为 `Dirving`，receiver 为本地 Player Pawn。
- `HTWheeledVehicleBase.SetTopSpeedRatio(float SpeedRatio)`：单个 4-byte float 参数，用于车速倍率。
- `HTVehicleMovementComponent.GetForwardSpeedKmH()`、`SetEnableWheelFriction(const bool& Enable)`、`GetExternalTorqueRatio()`、`SetExternalTorqueRatio(const float& ExternalTorqueRate)`：全部属于运动组件。外部扭矩比优先使用游戏自己的 `SetExternalTorqueRatio`，不能与车速倍率混为一谈。
- `HTCheatManager.CheatSpawnVehicle(const FName& VehicleID)`：召唤函数属于 CheatManager，不能按 Controller 方法调用。召唤后应验证新 Actor、位置和 `Owner`。
- `HTWheeledVehicleBase.HTVehicleMovementComponent` 字段偏移为 `0x378`。备用扭矩路径只有在 dump-declared 外部扭矩比函数无法通过 ABI 验证时才用 `SetMaxEngineTorque(float Torque)`，其反射 owner 为 `ChaosVehicles.ChaosWheeledVehicleMovementComponent`。备用基线偏移必须与 dump/Profile 中 `EngineSetup.MaxTorque` 对齐；不能每帧累乘倍率。

## 已从目标 Dump 核实的 Attack Input 路径

- `HTPlayerController.ActivateAbilityFromID(ESkillInputIDType InputID, int32 Param)` 和 `ReleaseAbilityFromID(ESkillInputIDType InputID, int32 Param)` 是游戏自身的输入入口。参数块总长 8 bytes；枚举参数 offset 0，`int32 Param` offset 4。
- 重放控制要以有效输入驱动连击；每次重放之后必须等待新的、player 为 source 且目标与本次捕获一致的 DamageEvent，才计入实际成功次数。调用被接受、动画/回调被调用或 UI 显示不等价于已造成伤害。
- 禁止仅凭 `HTAbilityCharacter.OnDamaged` 的回调声明推断可以直接制造伤害；除非目标 Dump 与实际验证证明了真正施加伤害的路径，否则不得将其当作“重打伤害”机制。

## 每次变更必需的核查

- 原生调用：精确路径、所属类、模块、参数类型/次序/大小/偏移、接收对象类型与游戏线程门禁。
- 布局读取：Profile key、该 key 的布局 validator、读取范围检查以及失败时的不可用状态。未经 dump 验证的偏移不能在插件内硬编码。
- 公共 ABI：版本与 `struct_size`、可选尾字段检查、ABI snapshot 及 JSON schema。
- 线程和反馈：UI/render 回调只发意图；Game 域执行操作；实际行为以可观测游戏事件/状态核验。
- 交付：从合并后的 V1 树构建 Windows x64 Runtime 与独立插件，检查 ZIP 内的 `plugin.dll`、`manifest.json` 路径和服务版本。仅源码语法检查或另一个分支的 CI 不能宣称当前包已构建。
