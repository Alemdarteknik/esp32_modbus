# Host-only C++ regression tests. Never builds/flashes ESP firmware or opens hardware/network.
param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $Compiler) {
    $foundCompiler = Get-Command g++ -ErrorAction SilentlyContinue
    if ($foundCompiler) { $Compiler = $foundCompiler.Source }
    elseif (Test-Path -LiteralPath 'C:\msys64\mingw64\bin\g++.exe') {
        $Compiler = 'C:\msys64\mingw64\bin\g++.exe'
    } else { throw 'Install a native C++20 compiler or pass -Compiler with its path.' }
}
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$testDirectory = New-Item -ItemType Directory -Path (
    [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(),
        'esp32-modbus-host-' + [guid]::NewGuid().ToString('N')))
$testExecutable = Join-Path $testDirectory.FullName 'usb_modbus_diagnostics.exe'
$originalSearchPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    & $Compiler -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -static `
        ('-I' + (Join-Path $PSScriptRoot 'tests/usb_modbus_stubs')) `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        (Join-Path $PSScriptRoot 'tests/usb_modbus_diagnostics.cpp') -o $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
    & $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Host assertions failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $originalSearchPath
    # Only our generated executable and now-empty unique directory are removed.
    if (Test-Path -LiteralPath $testExecutable) { Remove-Item -LiteralPath $testExecutable -Force }
    Remove-Item -LiteralPath $testDirectory.FullName
}
