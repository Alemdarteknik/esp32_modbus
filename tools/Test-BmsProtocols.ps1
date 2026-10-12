param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$outputDirectory = New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'build/bms_host_tests')
$executable = Join-Path $outputDirectory.FullName ('bms_protocols_test_' + [guid]::NewGuid().ToString('N') + '.exe')
$oldPath = $env:PATH
try {
    $env:PATH = (Split-Path $compilerPath -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    & $compilerPath -std=c++20 -Wall -Wextra -Werror -static -DENERGY_BMS_STANDALONE `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        (Join-Path $projectRoot 'main/src/protocol/bms_protocols.cpp') `
        (Join-Path $PSScriptRoot 'tests/bms_protocols_test.cpp') -o $executable
    if ($LASTEXITCODE -ne 0) { throw "BMS host compilation failed: $LASTEXITCODE" }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw "BMS host assertions failed: $LASTEXITCODE" }
} finally { $env:PATH = $oldPath }
