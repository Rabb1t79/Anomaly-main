param(
    [string]$SdkRoot = "",
    [string]$LlvmRoot = "I:\llvm-mingw-20260922-msvcrt-x86_64",
    [string]$CMake = "",
    [string]$Generator = "MinGW Makefiles"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ToolRoot = $PSScriptRoot
$OutRoot = Join-Path $ToolRoot "out"
$BuildRoot = Join-Path $OutRoot "build"
$PackageRoot = Join-Path $OutRoot "NteAttackReplay"

function Find-Existing([string[]]$Candidates) {
    foreach ($p in $Candidates) {
        if ($p -and (Test-Path $p)) { return (Resolve-Path $p).Path }
    }
    return ""
}

if (-not $CMake) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeCommand) { $CMake = $cmakeCommand.Source }
}
if (-not $CMake) {
    $CMake = Find-Existing @(
        "I:\AnomalyRuntimeProfiler\cmake\bin\cmake.exe",
        (Join-Path $RepoRoot "cmake\bin\cmake.exe"),
        "C:\Program Files\CMake\bin\cmake.exe"
    )
}
if (-not $CMake) { throw "cmake.exe not found. Use -CMake to specify it." }

$LlvmRoot = Find-Existing @($LlvmRoot)
if (-not $LlvmRoot) { throw "LLVM MinGW not found at I:\llvm-mingw-20260922-msvcrt-x86_64." }
$Clang = Join-Path $LlvmRoot "bin\clang.exe"
$Clangxx = Join-Path $LlvmRoot "bin\clang++.exe"
$Make = Join-Path $LlvmRoot "bin\mingw32-make.exe"
if (-not (Test-Path $Make)) {
    $makeCommand = Get-Command mingw32-make.exe -ErrorAction SilentlyContinue
    if ($makeCommand) { $Make = $makeCommand.Source }
}
if (-not (Test-Path $Make)) { throw "mingw32-make.exe not found. MinGW Makefiles requires mingw32-make." }
if (-not (Test-Path $Clang) -or -not (Test-Path $Clangxx)) { throw "clang.exe or clang++.exe is missing from the selected LLVM MinGW directory." }

if (-not $SdkRoot) {
    $SdkRoot = Find-Existing @(
        (Join-Path $RepoRoot "sdk"),
        (Join-Path $RepoRoot "AnomalySDK"),
        (Join-Path $RepoRoot "out\sdk"),
        "I:\AnomalySDK",
        "I:\AnomalyRuntimeProfiler\AnomalySDK"
    )
}
if (-not $SdkRoot) { throw "AnomalySDK not found. Use -SdkRoot to specify the SDK root." }
$SdkConfig = Get-ChildItem -Path $SdkRoot -Recurse -Filter "AnomalySDKConfig.cmake" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $SdkConfig) { throw "AnomalySDKConfig.cmake was not found inside the SDK root." }
$SdkPrefix = $SdkConfig.Directory.Parent.Parent.Parent.FullName

Write-Host "NTE Attack Replay NoVS build"
Write-Host "Repo : $RepoRoot"
Write-Host "CMake: $CMake"
Write-Host "Clang: $Clangxx"
Write-Host "Make : $Make"
Write-Host "SDK  : $SdkPrefix"

if (Test-Path $BuildRoot) { Remove-Item $BuildRoot -Recurse -Force }
if (Test-Path $PackageRoot) { Remove-Item $PackageRoot -Recurse -Force }
New-Item -ItemType Directory -Force $BuildRoot | Out-Null
$env:CC = $Clang
$env:CXX = $Clangxx
$env:LLVM_MINGW_ROOT = $LlvmRoot
$Toolchain = Join-Path $ToolRoot "llvm-mingw-toolchain.cmake"
$CMakeArgs = @(
    "-S", $ToolRoot,
    "-B", $BuildRoot,
    "-G", $Generator,
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DCMAKE_MAKE_PROGRAM=$Make",
    "-DCMAKE_PREFIX_PATH=$SdkPrefix",
    "-DAnomalySDK_DIR=$($SdkConfig.Directory.FullName)",
    "-DANOMALY_SOURCE_ROOT=$RepoRoot"
)
& $CMake @CMakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
& $CMake --build $BuildRoot --config Release --target anomaly_nte_attack_replay --parallel
if ($LASTEXITCODE -ne 0) { throw "NTE Attack Replay build failed." }
$PluginDll = Join-Path $BuildRoot "package\NteAttackReplay\plugin.dll"
if (-not (Test-Path $PluginDll)) { throw "Build completed but plugin.dll was not found at $PluginDll." }
New-Item -ItemType Directory -Force $PackageRoot | Out-Null
Copy-Item $PluginDll (Join-Path $PackageRoot "plugin.dll") -Force
Copy-Item (Join-Path $RepoRoot "plugins\NteAttackReplay\manifest.json") (Join-Path $PackageRoot "manifest.json") -Force
$Locale = Join-Path $RepoRoot "plugins\NteAttackReplay\locales"
if (Test-Path $Locale) { Copy-Item $Locale (Join-Path $PackageRoot "locales") -Recurse -Force }
Write-Host ""
Write-Host "BUILD OK"
Write-Host "Package: $PackageRoot"
