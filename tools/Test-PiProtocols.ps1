param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$outputPath = Join-Path $projectRoot 'build/pi_protocols_test.exe'
$originalPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler -Parent) + [IO.Path]::PathSeparator + $env:PATH
    & $Compiler -std=c++20 -Wall -Wextra -Werror -Wno-misleading-indentation -static `
        ('-I' + (Join-Path $projectRoot 'main/include')) `
        (Join-Path $projectRoot 'main/src/protocol/energy_protocol.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/pi_protocols.cpp') `
        (Join-Path $projectRoot 'main/src/protocol/bms_protocols.cpp') `
        (Join-Path $PSScriptRoot 'tests/pi_protocols_test.cpp') -o $outputPath
    if ($LASTEXITCODE -ne 0) { throw 'PI protocol host compilation failed.' }
    & $outputPath
    if ($LASTEXITCODE -ne 0) { throw 'PI protocol host tests failed.' }
} finally {
    $env:PATH = $originalPath
}
