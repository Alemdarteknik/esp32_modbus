param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$testDirectory = New-Item -ItemType Directory -Path (
    [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(),
        'esp32-command-history-' + [guid]::NewGuid().ToString('N')))
$testExecutable = Join-Path $testDirectory.FullName 'protocol_command_history.exe'
$originalSearchPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    & $Compiler -std=c++20 -Wall -Wextra -Werror -static `
        ('-I' + (Join-Path $PSScriptRoot 'tests/energy_transport_stubs')) `
        ('-I' + (Join-Path $PSScriptRoot 'tests/usb_modbus_stubs')) `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        (Join-Path $PSScriptRoot 'tests/protocol_command_history_test.cpp') -o $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
    & $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Host assertions failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $originalSearchPath
    if (Test-Path -LiteralPath $testExecutable) { Remove-Item -LiteralPath $testExecutable -Force }
    Remove-Item -LiteralPath $testDirectory.FullName
}
