param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$testDirectory = New-Item -ItemType Directory -Path (
    [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(),
        'esp32-energy-config-' + [guid]::NewGuid().ToString('N')))
$testExecutable = Join-Path $testDirectory.FullName 'energy_config_migration.exe'
$originalSearchPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    & $Compiler -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers -static `
        ('-I' + (Join-Path $PSScriptRoot 'tests/energy_transport_stubs')) `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        (Join-Path $PSScriptRoot 'tests/energy_config_migration.cpp') `
        (Join-Path $projectRoot 'main/src/app/system_config.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/energy_protocol.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/pi_protocols.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/bms_protocols.cpp') -o $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
    & $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Host assertions failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $originalSearchPath
    if (Test-Path -LiteralPath $testExecutable) { Remove-Item -LiteralPath $testExecutable -Force }
    Remove-Item -LiteralPath $testDirectory.FullName
}
