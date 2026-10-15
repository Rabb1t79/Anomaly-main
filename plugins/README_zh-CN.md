# NTE Vehicle / Attack Replay 源码

`NteVehicle` 与 `NteAttackReplay` 是两个独立插件，均使用顶层 `plugin.dll` + `manifest.json` 包结构。

两个插件依赖配套主框架新增的 `anomaly.nte.vehicle` v2 与 `anomaly.nte.attack-input` v1 接口。插件 DLL 已做 Windows x64 PE / 导出 / 导入表检查；尚未在游戏内运行验证。
