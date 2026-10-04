param(
    [string]$SdkRoot = "",
    [string]$LlvmRoot = "",
    [string]$CMake = "",
    [string]$Generator = "MinGW Makefiles"
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ToolRoot = $PSScriptRoot
$OutRoot = Join-Path $ToolRoot "out"
$BuildRoot = Join-Path $OutRoot "build"
$PackageRoot = Join-Path $OutRoot "NteVehicle"

function Find-Existing([string[]]$Candidates) {
    foreach ($p in $Candidates) {
        if ($p -and (Test-Path $p)) { return (Resolve-Path $p).Path }
    }
    return ""
}

if (-not $CMake) { $CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $CMake) {
    $CMake = Find-Existing @(
        (Join-Path $RepoRoot "cmake\bin\cmake.exe"),
        "C:\Program Files\CMake\bin\cmake.exe")
}
if (-not $CMake) { throw "找不到 cmake.exe，请用 -CMake 指定。" }

if (-not $LlvmRoot) {
    $LlvmRoot = Find-Existing @(
        (Join-Path $RepoRoot "llvm-mingw"),
        (Join-Path $RepoRoot "third_party\llvm-mingw"),
        "I:\llvm-mingw",
        "I:\tools\llvm-mingw",
        "C:\llvm-mingw")
}
$Clang = ""
$Clangxx = ""
if ($LlvmRoot) {
    $Clang = Join-Path $LlvmRoot "bin\clang.exe"
    $Clangxx = Join-Path $LlvmRoot "bin\clang++.exe"
}
if (-not (Test-Path $Clangxx)) {
    $x = Get-Command clang++.exe -ErrorAction SilentlyContinue
    if ($x) { $Clangxx = $x.Source }
}
if (-not (Test-Path $Clang)) {
    $x = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($x) { $Clang = $x.Source }
}
if (-not (Test-Path $Clangxx) -or -not (Test-Path $Clang)) {
    throw "找不到 LLVM/Clang，请用 -LlvmRoot 指向 llvm-mingw 根目录。"
}

if (-not $SdkRoot) {
    $SdkRoot = Find-Existing @(
        (Join-Path $RepoRoot "sdk"),
        (Join-Path $RepoRoot "AnomalySDK"),
        (Join-Path $RepoRoot "out\sdk"),
        "I:\AnomalySDK",
        "I:\AnomalyRuntimeProfiler\AnomalySDK")
}
if (-not $SdkRoot) { throw "找不到 AnomalySDK，请用 -SdkRoot 指定。" }

$SdkConfig = Get-ChildItem -Path $SdkRoot -Recurse -Filter "AnomalySDKConfig.cmake" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $SdkConfig) { throw "SDK 内找不到 AnomalySDKConfig.cmake。" }
$SdkPrefix = $SdkConfig.Directory.Parent.Parent.Parent.FullName

Write-Host "NTE Vehicle NoVS build"
Write-Host "Repo : $RepoRoot"
Write-Host "CMake: $CMake"
Write-Host "Clang: $Clangxx"
Write-Host "SDK  : $SdkPrefix"

if (Test-Path $BuildRoot) { Remove-Item $BuildRoot -Recurse -Force }
if (Test-Path $PackageRoot) { Remove-Item $PackageRoot -Recurse -Force }
New-Item -ItemType Directory -Force $BuildRoot | Out-Null

$env:CC = $Clang
$env:CXX = $Clangxx
$env:LLVM_MINGW_ROOT = $LlvmRoot

$Toolchain = Join-Path $ToolRoot "llvm-mingw-toolchain.cmake"
$Args = @(
    "-S", $ToolRoot,
    "-B", $BuildRoot,
    "-G", $Generator,
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DCMAKE_PREFIX_PATH=$SdkPrefix",
    "-DAnomalySDK_DIR=$($SdkConfig.Directory.FullName)",
    "-DANOMALY_SOURCE_ROOT=$RepoRoot"
)
& $CMake @Args
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败。" }

& $CMake --build $BuildRoot --config Release --target anomaly_nte_vehicle --parallel
if ($LASTEXITCODE -ne 0) { throw "NTE Vehicle 编译失败。" }

if (-not (Test-Path (Join-Path $PackageRoot "plugin.dll"))) {
    throw "编译命令成功，但没有找到 plugin.dll。"
}

Copy-Item (Join-Path $RepoRoot "plugins\NteVehicle\manifest.json") (Join-Path $PackageRoot "manifest.json") -Force

$Locale = Join-Path $RepoRoot "plugins\NteVehicle\locales"
if (Test-Path $Locale) {
    Copy-Item $Locale (Join-Path $PackageRoot "locales") -Recurse -Force
}

Write-Host ""
Write-Host "BUILD OK"
Write-Host "Package: $PackageRoot"
