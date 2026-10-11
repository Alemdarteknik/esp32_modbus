param(
    [string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe'
)

# Compiles a Windows host test executable, NOT the ESP firmware. No hardware,
# sockets, broker, upload endpoint or real flash APIs are used by this test.
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
    throw "Host C++ compiler not found: $Compiler"
}
$projectDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('esp32-ota-host-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null
$testExecutable = Join-Path $testDirectory 'ota_host_tests.exe'
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $savedPath
    $arguments = @(
        '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O0', '-g',
        '-D_GLIBCXX_ASSERTIONS',
        '-I', (Join-Path $PSScriptRoot 'ota_test_stubs'),
        '-I', (Join-Path $projectDirectory 'main/include'),
        (Join-Path $PSScriptRoot 'ota_host_tests.cpp'),
        '-o', $testExecutable
    )
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Offline host harness compilation failed ($LASTEXITCODE)" }
    Write-Output "Host executable: $testExecutable"
    & $testExecutable
    if ($LASTEXITCODE -ne 0) { throw "Offline host harness failed ($LASTEXITCODE)" }
} finally {
    $env:PATH = $savedPath
}
