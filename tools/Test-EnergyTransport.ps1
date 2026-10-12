param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$testDirectory = New-Item -ItemType Directory -Path (
    [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(),
        'esp32-energy-transport-' + [guid]::NewGuid().ToString('N')))
$testExecutables = @()
$originalSearchPath = $env:PATH
try {
    $env:PATH = (Split-Path $Compiler -Parent) + [System.IO.Path]::PathSeparator + $env:PATH
    foreach ($testName in @('energy_transport_contract', 'energy_usb_serial', 'energy_usb_hid_discovery')) {
        $testExecutable = Join-Path $testDirectory.FullName ($testName + '.exe')
        $testExecutables += $testExecutable
        $additionalIncludes = @()
        if ($testName -eq 'energy_usb_hid_discovery') {
            $additionalIncludes += '-I' + (Join-Path $PSScriptRoot 'tests/energy_hid_stubs')
        }
        & $Compiler -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -static `
            @additionalIncludes `
            ('-I' + (Join-Path $PSScriptRoot 'tests/energy_transport_stubs')) `
            ('-I' + (Join-Path $PSScriptRoot 'tests/usb_modbus_stubs')) `
            ('-I' + (Join-Path $projectRoot 'main/include')) `
            (Join-Path $PSScriptRoot ('tests/' + $testName + '.cpp')) -o $testExecutable
        if ($LASTEXITCODE -ne 0) { throw "Host compilation failed: $LASTEXITCODE" }
        & $testExecutable
        if ($LASTEXITCODE -ne 0) { throw "Host assertions failed: $LASTEXITCODE" }
    }
} finally {
    $env:PATH = $originalSearchPath
    foreach ($testExecutable in $testExecutables) {
        if (Test-Path -LiteralPath $testExecutable) { Remove-Item -LiteralPath $testExecutable -Force }
    }
    Remove-Item -LiteralPath $testDirectory.FullName
}
