param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$outputDirectory = New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'build/aohai_settings_host_tests')
$executable = Join-Path $outputDirectory.FullName ('aohai_settings_test_' + [guid]::NewGuid().ToString('N') + '.exe')
$oldPath = $env:PATH
try {
    $env:PATH = (Split-Path $compilerPath -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    & $compilerPath -std=c++20 -Wall -Wextra -Werror -static `
        ('-I' + (Join-Path $PSScriptRoot 'tests/usb_modbus_stubs')) `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        (Join-Path $projectRoot 'main/src/inverter/command_service.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/register_command_service.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/register_command.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/inverter_command.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/register_value.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/aohai_register_catalog.generated.cpp') `
        (Join-Path $PSScriptRoot 'tests/aohai_settings_test.cpp') -o $executable
    if ($LASTEXITCODE -ne 0) { throw "AOHAI settings host compilation failed: $LASTEXITCODE" }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw "AOHAI settings host assertions failed: $LASTEXITCODE" }
} finally { $env:PATH = $oldPath }
