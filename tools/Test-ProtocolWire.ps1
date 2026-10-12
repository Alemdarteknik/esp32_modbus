param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$outputDirectory = New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'build/protocol_wire_tests')
$oldPath = $env:PATH
try {
    $env:PATH = (Split-Path $compilerPath -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    foreach ($variant in @('mesh','coordinator')) {
        $executable = Join-Path $outputDirectory.FullName ($variant + '_' + [guid]::NewGuid().ToString('N') + '.exe')
        $defines = @()
        if ($variant -eq 'coordinator') { $defines += '-DTEST_COORDINATOR_WIRE' }
        & $compilerPath -std=c++20 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -static `
            '-Wl,--gc-sections' @defines `
            ('-I' + (Join-Path $PSScriptRoot 'tests/routing_stubs')) `
            ('-I' + (Join-Path $PSScriptRoot 'tests/usb_modbus_stubs')) `
            ('-I' + (Join-Path $projectRoot 'main/include')) `
            (Join-Path $PSScriptRoot 'tests/protocol_wire_validation_test.cpp') -o $executable
        if ($LASTEXITCODE -ne 0) { throw "Protocol wire host compilation failed: $LASTEXITCODE" }
        & $executable
        if ($LASTEXITCODE -ne 0) { throw "Protocol wire host assertions failed: $LASTEXITCODE" }
    }
} finally { $env:PATH = $oldPath }
