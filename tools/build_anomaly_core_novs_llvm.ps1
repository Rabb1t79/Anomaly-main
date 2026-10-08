param(
    [Parameter(Mandatory=$true)]
    [string]$LlvmMingwRoot,
    [string]$BuildDir = "$PSScriptRoot/../.build/core-novs-llvm"
)

$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path "$PSScriptRoot/..").Path
$LlvmMingwRoot = (Resolve-Path $LlvmMingwRoot).Path
$Clang = Join-Path $LlvmMingwRoot 'bin/clang.exe'
$ClangXX = Join-Path $LlvmMingwRoot 'bin/clang++.exe'
if (!(Test-Path $Clang) -or !(Test-Path $ClangXX)) { throw "LLVM-MinGW clang/clang++ not found under: $LlvmMingwRoot" }

if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }

cmake -S $SourceRoot -B $BuildDir -G 'MinGW Makefiles' `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_C_COMPILER="$Clang" `
  -DCMAKE_CXX_COMPILER="$ClangXX" `
  -DCMAKE_C_COMPILER_TARGET=x86_64-w64-mingw32 `
  -DCMAKE_CXX_COMPILER_TARGET=x86_64-w64-mingw32

cmake --build $BuildDir --target anomaly_core --parallel 2

$dll = Get-ChildItem -Path $BuildDir -Filter 'Anomaly.Core.dll' -Recurse -File | Select-Object -First 1
if ($null -eq $dll) { throw 'Anomaly.Core.dll was not produced' }

$OutDir = Join-Path $SourceRoot 'dist/Core'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $dll.FullName (Join-Path $OutDir 'Anomaly.Core.dll') -Force
Get-FileHash (Join-Path $OutDir 'Anomaly.Core.dll') -Algorithm SHA256
Write-Host "Built: $OutDir\Anomaly.Core.dll"