param(
    [string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe',
    [switch]$MeasureBaseline
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$outputDirectory = New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'build/aohai_polling_host_tests')
$executable = Join-Path $outputDirectory.FullName ('aohai_poller_' + [guid]::NewGuid().ToString('N') + '.exe')
$poller = Join-Path $projectRoot 'main/src/inverter/production_poller.cpp'
$plan = Join-Path $projectRoot 'main/src/inverter/polling_plan.cpp'
$defines = @()
if ($MeasureBaseline) {
    $poller = Join-Path $projectRoot 'build/aohai_production_poller_before.cpp'
    $plan = Join-Path $projectRoot 'build/aohai_polling_plan_before.cpp'
    $defines += '-DAOHAI_BASELINE_MEASURE'
}
$oldPath = $env:PATH
try {
    $env:PATH = (Split-Path $compilerPath -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    & $compilerPath -std=c++20 -Wall -Wextra -Werror -static @defines `
        ('-I' + (Join-Path $PSScriptRoot 'tests/aohai_poller_stubs')) `
        ('-I' + (Join-Path $PSScriptRoot 'tests/usb_modbus_stubs')) `
        ('-I' + (Join-Path $PSScriptRoot 'tests/energy_transport_stubs')) `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        $poller $plan `
        (Join-Path $projectRoot 'main/src/inverter/register_value.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/aohai_register_catalog.generated.cpp') `
        (Join-Path $PSScriptRoot 'tests/aohai_poller_test.cpp') -o $executable
    if ($LASTEXITCODE -ne 0) { throw "AOHAI poller host compilation failed: $LASTEXITCODE" }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw "AOHAI poller host assertions failed: $LASTEXITCODE" }
} finally { $env:PATH = $oldPath }
