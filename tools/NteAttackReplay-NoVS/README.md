# NTE Attack Replay - No VS / LLVM-MinGW build

Windows x64 only. Builds the plugin against AnomalySDK without Visual Studio.

Run from repository root:
powershell -ExecutionPolicy Bypass -File .\tools\NteAttackReplay-NoVS\build.ps1

Optional:
powershell -ExecutionPolicy Bypass -File .\tools\NteAttackReplay-NoVS\build.ps1 -SdkRoot "I:\AnomalySDK" -LlvmRoot "I:\llvm-mingw"

Output:
tools\NteAttackReplay-NoVS\out\NteAttackReplay
