param(
    [string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe',
    [string]$IdfPath = 'C:\Users\Administrator\esp\v5.5.1\esp-idf',
    [string]$ExportJsonl = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$testBuildPath = Join-Path $projectRoot 'build/mqtt_native_protocols_test'
New-Item -ItemType Directory -Force -Path $testBuildPath | Out-Null
$jsonPath = Join-Path $IdfPath 'components/json/cJSON'
if (-not (Test-Path -LiteralPath (Join-Path $jsonPath 'cJSON.c'))) {
    throw 'Set -IdfPath to the installed ESP-IDF checkout containing cJSON.c.'
}
$originalPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler -Parent) + [IO.Path]::PathSeparator + $env:PATH
    python (Join-Path $PSScriptRoot 'tests/extract_mqtt_native_production.py') `
        --root $projectRoot --output (Join-Path $testBuildPath 'mqtt_native_production.generated.inc')
    if ($LASTEXITCODE -ne 0) { throw 'Production MQTT extraction failed.' }
    $cCompiler = Join-Path (Split-Path $Compiler -Parent) 'gcc.exe'
    $jsonObject = Join-Path $testBuildPath 'cJSON.o'
    & $cCompiler -std=c11 -Wall -Wextra -Werror -c (Join-Path $jsonPath 'cJSON.c') -o $jsonObject
    if ($LASTEXITCODE -ne 0) { throw 'Host cJSON compilation failed.' }
    $outputPath = Join-Path $testBuildPath ('mqtt_native_test_' + [Guid]::NewGuid().ToString('N') + '.exe')
    & $Compiler -std=c++20 -Wall -Wextra -Werror -Wno-misleading-indentation -static '-Wl,--stack,16777216' `
        ('-I' + (Join-Path $PSScriptRoot 'tests/mqtt_native_stubs')) `
        ('-I' + (Join-Path $PSScriptRoot 'tests/routing_stubs')) `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        ('-I' + $jsonPath) ('-I' + $testBuildPath) `
        (Join-Path $projectRoot 'main/src/inverter/inverter_command.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/register_command.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/register_value.cpp') `
        (Join-Path $projectRoot 'main/src/inverter/aohai_register_catalog.generated.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/energy_protocol.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/pi_protocols.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/bms_protocols.cpp') `
        (Join-Path $PSScriptRoot 'tests/mqtt_native_protocols_test.cpp') $jsonObject -o $outputPath
    if ($LASTEXITCODE -ne 0) { throw 'Native MQTT host compilation failed.' }
    if ($ExportJsonl) {
        & $outputPath $ExportJsonl (Join-Path $PSScriptRoot 'tests/pi_reference_vectors.generated.json')
    } else {
        & $outputPath '' (Join-Path $PSScriptRoot 'tests/pi_reference_vectors.generated.json')
    }
    if ($LASTEXITCODE -ne 0) { throw 'Native MQTT host tests failed.' }
} finally {
    $env:PATH = $originalPath
}
