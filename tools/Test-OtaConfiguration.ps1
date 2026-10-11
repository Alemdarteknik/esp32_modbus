# Read-only static checks; does not build, flash, or contact a device/server.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$entries = @(Get-Content (Join-Path $projectRoot 'partitions.csv') |
    Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') } |
    ForEach-Object {
        $fields = $_.Split(',')
        [pscustomobject]@{
            Name = $fields[0].Trim()
            Offset = [Convert]::ToInt64($fields[3].Trim().Substring(2), 16)
            Size = [Convert]::ToInt64($fields[4].Trim().Substring(2), 16)
        }
    })
$previousEnd = 0x9000
foreach ($entry in $entries) {
    if ($entry.Offset -lt $previousEnd -or $entry.Size -le 0) { throw 'Partition overlap/size error' }
    if (($entry.Offset % 4096) -ne 0) { throw 'Partition alignment error' }
    if ($entry.Name.StartsWith('ota_') -and ($entry.Offset % 65536) -ne 0) { throw 'App alignment error' }
    $previousEnd = $entry.Offset + $entry.Size
}
if ($previousEnd -gt 16MB) { throw 'Partitions exceed 16 MB' }
$nvs = $entries | Where-Object Name -eq 'nvs'
if ($nvs.Offset -ne 0x9000 -or $nvs.Size -ne 0x6000) { throw 'Legacy NVS moved' }
$phy = $entries | Where-Object Name -eq 'phy_init'
if ($phy.Offset -ne 0xf000 -or $phy.Size -ne 0x1000) { throw 'Legacy PHY moved' }
$slots = @($entries | Where-Object { $_.Name.StartsWith('ota_') })
if ($slots.Count -ne 2 -or $slots[0].Offset -ne 0x10000 -or $slots[0].Size -ne $slots[1].Size) { throw 'OTA slot mismatch' }
$defaults = Get-Content (Join-Path $projectRoot 'sdkconfig.defaults') -Raw
foreach ($required in @('CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y', 'CONFIG_PARTITION_TABLE_CUSTOM=y', 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y')) {
    if (-not $defaults.Contains($required)) { throw "Missing $required" }
}
$cmake = Get-Content (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'set\(PROJECT_VER "[0-9]{1,5}\.[0-9]{1,5}\.[0-9]{1,5}"\)') { throw 'Invalid OTA version format' }
Write-Output 'PASS: partition bounds/alignment, preserved NVS/PHY, equal OTA slots, defaults and release-version format.'
