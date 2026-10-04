# NTE Vehicle - No VS / LLVM-MinGW build

Windows x64 only. This wrapper builds only the NTE Vehicle plugin against the installed Anomaly SDK. It does not build the Anomaly runtime and does not require Visual Studio.

Run from repository root:
powershell -ExecutionPolicy Bypass -File .\tools\NteVehicle-NoVS\build.ps1

Optional:
powershell -ExecutionPolicy Bypass -File .\tools\NteVehicle-NoVS\build.ps1 -SdkRoot "I:\AnomalySDK" -LlvmRoot "I:\llvm-mingw"

Output:
tools\NteVehicle-NoVS\out\NteVehicle
