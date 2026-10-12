param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$outputDirectory = New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'build/energy_poll_tests')
$executable = Join-Path $outputDirectory.FullName ('energy_poll_schedule_' + [guid]::NewGuid().ToString('N') + '.exe')
$oldPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler -Parent) + [IO.Path]::PathSeparator + $env:PATH
    & $Compiler -std=c++20 -Wall -Wextra -Werror -Wno-misleading-indentation -static `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        (Join-Path $projectRoot 'main/src/protocol/energy_protocol.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/pi_protocols.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/bms_protocols.cpp') `
        (Join-Path $PSScriptRoot 'tests/energy_poll_schedule_test.cpp') -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'Energy poll schedule host compilation failed.' }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw 'Energy poll schedule assertions failed.' }
} finally { $env:PATH = $oldPath }
