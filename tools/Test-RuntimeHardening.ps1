# Read-only source/config checks. Not a build, unit test or hardware test.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
function Read-Source([string]$relative) {
    Get-Content -LiteralPath (Join-Path $projectRoot $relative) -Raw
}
function Require-Text([string]$source, [string]$expected, [string]$label) {
    if (-not $source.Contains($expected)) { throw "Missing invariant: $label" }
}
$mqtt = Read-Source 'main/src/network/mqtt_bridge.cpp'
$main = Read-Source 'main/main.cpp'
$web = Read-Source 'main/src/network/wifi_provisioning.cpp'
$ota = Read-Source 'main/src/app/ota_update.cpp'
$mesh = Read-Source 'main/src/network/espnow_mesh.cpp'
$cmake = Read-Source 'main/CMakeLists.txt'

Require-Text $mqtt 'xQueueCreate(32, sizeof(TelemetryMessage))' 'typed telemetry queue'
Require-Text $mqtt 'xQueueCreate(8, sizeof(RoutedCommandResult))' 'typed result queue'
Require-Text $mqtt 'xQueueSend(publish_queue_, &message, 0)' 'telemetry queue send'
Require-Text $mqtt 'xQueueSend(result_queue_, &message, 0)' 'result queue send'
Require-Text $mqtt 'xQueueReceive(self->publish_queue_, &message,' 'telemetry queue receive'
Require-Text $mqtt 'xQueueReceive(self->result_queue_, &command_result,' 'result queue receive'
Require-Text $mqtt 'burst < 2' 'bounded result burst'
if ($mqtt -match 'OutboundMessage|OutboundType') { throw 'Old combined queue storage remains' }

Require-Text $mqtt 'json != nullptr && allocations.ok() &&' 'live tree OOM guard'
Require-Text $mqtt 'cJSON_PrintPreallocated(root, json, static_cast<int>(live_json_capacity_), false)' 'bounded live output'
if ($mqtt -match 'capacity\s*<=\s*live_json_capacity_') { throw 'Pessimistic estimate rejects fitting live JSON' }
Require-Text $mqtt 'app::MemoryWorkGuard memory_work;' 'live memory admission'
Require-Text $ota 'MemoryWorkGuard memory_work(pdMS_TO_TICKS(500));' 'bounded OTA memory admission'
Require-Text $ota 'MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT' 'OTA internal RAM checks'
Require-Text $ota 'memory_work.release();' 'OTA releases gate before download'
Require-Text $ota 'CheckOutcome::local_deferred' 'local OTA deferral'

if ($main -notmatch '(?s)extern "C" void app_main\(void\)\s*\{\s*inverter_gateway::app::initialize_json_memory\(\);') {
    throw 'JSON allocator must initialize first in app_main'
}
Require-Text $main 'if (write_attempted) poller->invalidate_state_after_write();' 'post-write state invalidation'
Require-Text $main 'if (accepted && command_is_write(command)) ++pending_write_count_;' 'only accepted writes postpone OTA'
Require-Text $main 'runtime.complete_command(command);' 'completed writes release pending count'
Require-Text $main 'last_write_completed_ms_' 'quiet interval from completion'
if ($main -match 'last_command_ms_') { throw 'Getters must not reset an all-command OTA timer' }
Require-Text $ota 'callbacks.download_blocker(callbacks.context) != nullptr' 'explicit nullable blocker interpretation'
Require-Text $ota 'snapshot.internal_free_bytes' 'current-memory diagnostics'
$jsonMemory = Read-Source 'main/src/app/json_memory.cpp'
Require-Text $jsonMemory 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' 'PSRAM-preferred JSON allocator'
$defaults = Read-Source 'sdkconfig.defaults'
foreach ($setting in @('CONFIG_SPIRAM=y','CONFIG_SPIRAM_MODE_OCT=y','CONFIG_SPIRAM_SPEED_40M=y','CONFIG_SPIRAM_MEMTEST=y')) {
    Require-Text $defaults $setting 'N16R8 PSRAM configuration'
}
Require-Text $web 'new (std::nothrow) wifi_ap_record_t[count]' 'nonthrowing scan allocation'
Require-Text $web 'esp_wifi_clear_ap_list()' 'scan result cleanup'
Require-Text $web '503 Service Unavailable' 'explicit low-memory response'
Require-Text $mesh 'sequence_.fetch_add(' 'atomic transmit sequence allocation'
Require-Text $mesh 'now - last_announce >= announce_interval' 'announcements independent of receive idleness'
foreach ($sourceFile in @('json_memory', 'memory_budget', 'ota_update')) {
    Require-Text $cmake "src/app/$sourceFile.cpp" "registered $sourceFile source"
}

# Extract the real health format and check a maximal-size rendering against its buffer.
# This does NOT compile-check the variadic arguments; they also require C++ compilation.
$health = [regex]::Match($mqtt, '(?s)void MqttBridge::publish_health\(\).*?(?=void MqttBridge::publisher_task)').Value
$bufferMatch = [regex]::Match($health, 'char payload\[(\d+)\]')
$formatMatch = [regex]::Match($health, '(?s)std::snprintf\(payload, sizeof\(payload\),\s*(.*?)\s*,\s*static_cast<unsigned long long>')
if (-not $bufferMatch.Success -or -not $formatMatch.Success) { throw 'Cannot extract health format/buffer' }
$format = ([regex]::Matches($formatMatch.Groups[1].Value, '"(?:\\.|[^"\\])*"') |
    ForEach-Object { $_.Value.Substring(1, $_.Value.Length - 2).Replace('\"', '"') }) -join ''
$rendered = $format.Replace('%llu', '18446744073709551615').Replace('%u', '4294967295').
    Replace('%d', '-2147483648').Replace('%s', 'false')
if ($rendered.Contains('%')) { throw 'Unknown health format conversion' }
if ($rendered.Length + 1 -gt [int]$bufferMatch.Groups[1].Value) { throw 'Health buffer too small' }
$healthObject = $rendered | ConvertFrom-Json
foreach ($key in @('live_json_failures', 'live_publish_deferrals', 'result_queue_drops',
                  'json_allocation_failures', 'heap_internal_free_bytes',
                  'heap_internal_largest_block_bytes', 'heap_psram_free_bytes',
                  'usb_rx_dropped_bytes', 'usb_rx_overflow_events',
                  'usb_disconnect_count', 'usb_reconnect_count',
                  'modbus_crc_errors', 'modbus_timeouts',
                  'modbus_incomplete_responses', 'modbus_rx_overflow_failures')) {
    if ($null -eq $healthObject.PSObject.Properties[$key]) { throw "Missing health field $key" }
}

Require-Text $mqtt '"routing_status", route_uncertain ? "uncertain" : "not_sent"' 'explicit routing result'
Require-Text $mqtt 'if (route_uncertain) cJSON_AddNullToObject(root, "write_sent")' 'uncertain remote write is not falsely unsent'
Require-Text $mqtt 'if (route_uncertain || route_not_sent) cJSON_AddNullToObject(root, "write_guard")' 'unevaluated remote guard stays unknown'

# Syntax-only check of the existing embedded webpage JavaScript, if Node is installed.
$scriptMatch = [regex]::Match($web, '(?s)<script>(.*?)</script>')
if (-not $scriptMatch.Success) { throw 'Embedded webpage script not found' }
if (Get-Command node -ErrorAction SilentlyContinue) {
    $scriptMatch.Groups[1].Value | node --check -
    if ($LASTEXITCODE -ne 0) { throw 'Embedded webpage JavaScript syntax error' }
} else {
    Write-Output 'SKIP: JavaScript syntax check (Node not installed).'
}
Write-Output "PASS: runtime source invariants and health capacity ($($rendered.Length) bytes plus terminator)."
