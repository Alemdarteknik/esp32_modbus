# OTA updates: version 1.0.8

Version 1.0.8 separates the internal-memory budget needed to open TLS from the
reserve needed after TLS is established and throughout the download. Earlier
versions reapplied the connection budget after TLS had already allocated memory,
which could reject a connection at `post_tls_memory` despite enough remaining
memory for the configured streaming reserve. The local firmware card now records
available and required memory at the failed step, before resources are cleaned up.
The new reserves still require measurement with simultaneous MQTT/OTA on hardware;
they do not guarantee a successful update or deployment capacity.

Version 1.0.7 fixes downloading a firmware image returned with HTTP chunked
transfer encoding and adds last-failure diagnostics to the local firmware card.
Known-length downloads remain supported. Image size, complete HTTP reception and
image verification are required before activation. This is a narrow download
compatibility fix, not the proposed larger OTA worker/retry rewrite.

Version 1.0.6 added [communication hardening](Firmware_Reliability_1.0.6.md)
without changing OTA download behavior. Scheduling remains unchanged in 1.0.8:
startup checks 1-61 minutes, local deferral retries 5-10 minutes, and network
attempts including failures 6-7 hours apart. `tools/ota_retry_policy.hpp` is an
offline proposal only, not firmware. Actual-worker mocked host tests cover
streaming framing, memory admission and failure diagnostics; they do not
establish hardware safety.
See the current results and limits in [host test scope](../tools/OTA_Host_Tests.md).

Status: implemented for standalone Wi-Fi devices, configurable and not hardware-tested.
This checkout currently sets `ota_enabled = true` in
`main/include/inverter_gateway/app/project_config.hpp`. Review that setting before
every release; it is not approval for fleet rollout. Complete the tests below on
spare hardware first. Host tests and a successful build are not hardware update
validation. No device was flashed, no server file was uploaded, and no hardware
update/rollback or fleet load test was performed as part of this change.

## Devices stuck on an older updater

The installed updater decides whether it can download the replacement. Original,
pre-framing-fix 1.0.5/1.0.6 builds reject chunked/unknown-length responses.
Uploading a newer binary to that same chunked endpoint cannot bootstrap those
clients: either USB-flash the current baseline once, or make the server return
HTTP 200 with the exact positive
`Content-Length` of the application binary so the old updater can download it at
its next eligible scheduled check. This change does not configure the server or
flash devices.

The embedded version label alone does not prove which updater code is installed:
a local rebuild can retain an older version string while including newer fixes.
Use the build/source provenance and observed failure stage to identify the issue.

An older updater that never passes memory/runtime admission also cannot repair
itself just because a newer file is uploaded. Temporarily stop app-generated
getters if using a pre-1.0.5 updater and allow its scheduled check to run. If it
still cannot proceed, USB-flash the baseline. Preserve NVS; do not erase the chip
or burn eFuses.

For the currently memory-blocked device, install the 1.0.8 baseline physically
before validating OTA. An installed updater that repeatedly stops at
`post_tls_memory` cannot receive this memory-budget fix merely by changing the
served binary or its Content-Length. HTTP 200 confirms a server response, not a
completed download or a verified image. No physical installation is performed by
this source change.

Version 1.0.5 no longer treats ordinary getters as writes for OTA admission.
Accepted semantic setters and register writes remain protected while queued or
executing; the 30-second quiet interval starts at completion/rejection by the bus
owner, not the earlier enqueue time. Existing Modbus polling and live payloads
are unchanged. The firmware version is embedded from root `PROJECT_VER`.

The proposed larger retry/worker rewrite remains unintegrated. Version 1.0.7
changed streaming framing and added diagnostics; 1.0.8 splits the connection and
streaming memory budgets and adds failure-time memory. The existing staggered
schedule, write admission and rollback flow remain.
There is no new exponential retry, Retry-After support, exact next-check countdown,
download-progress reporting or caching of a verified image while awaiting restart.

## First USB installation

This layout is for the stated ESP32-S3 N16R8 (16 MB flash). Verify the actual
hardware capacity. The old configuration selected 2 MB flash.

Use the complete normal ESP-IDF USB flash operation, including the new bootloader,
partition table, OTA initialization data, and application. Do not upload only
the application for this initial migration. Do not perform a full-chip erase
if preserving configuration is required. Back up important settings separately.

`partitions.csv` preserves NVS at 0x9000 with size 0x6000 and PHY at 0xf000.
The first application remains at 0x10000. It now occupies a 3 MB OTA slot;
the second 3 MB slot is at 0x310000, with OTA metadata at 0x610000.
Reconfigure older checkouts and check generated flash arguments before the first
physical flash.

`sdkconfig.defaults` records the new defaults for fresh configurations. Existing
checkouts with a saved sdkconfig must also select custom partitions, 16 MB flash,
and bootloader rollback; defaults do not override saved settings. The current
local sdkconfig has been updated, but this file is ignored by Git.

This N16R8 profile enables 8 MB Octal PSRAM at 40 MHz, including its boot memory
test. JSON allocations prefer PSRAM, with normal-heap fallback; the reserved live
output buffer also prefers PSRAM. TLS and task stacks remain internal. Missing
PSRAM is allowed to boot using internal RAM, but that fallback may still defer
OTA. A failing memory test is not ignored. Confirm detection on spare hardware
before deployment. No flash voltage, eFuse or PSRAM execution-in-place change is made.
See the [Espressif N16R8 specification](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf).

## Release workflow

### Local firmware status

The ESP firmware card on `http://esp32.local` shows the running embedded version,
any newer version offered, the OTA state and time since the last check attempt.
It refreshes through the existing read-only `/api/status` endpoint. Opening the
page does not start a download. Disabled, not-yet-checked, failed, incompatible,
blocked, and unsupported-role states are never reported as up to date. “Up to
date at the last check” means the server's compatible version matched the running
version at that attempt, not that the server is continuously monitored.
Status history resets on reboot. MQTT live payloads are unchanged.

The card additionally shows current internal free/largest-block bytes and a
current blocker assessment while deferred. API fields: `firmware_update_reason`,
`firmware_internal_free_bytes`, `firmware_internal_largest_block_bytes`.
These are measured when the page requests status, not a record of the earlier
failed preflight. `awaiting_scheduled_retry` means current runtime/memory checks
look ready; the previous blocker or transient memory-gate contention may have
cleared. Opening the page does not change the updater schedule or admission.

The separate **Last update error** display comes from these additional read-only
`/api/status` fields:

- `firmware_failure_stage`: failed step, or an empty string if none is recorded.
- `firmware_failure_code`: numeric ESP-IDF error, or zero when cleared.
- `firmware_failure_http_status`: HTTP response status if known, otherwise zero.
- `firmware_failure_error`: `esp_err_to_name` text, empty when no stage is recorded.
- `firmware_failure_internal_free_bytes`: internal free memory at the failed step.
- `firmware_failure_internal_largest_block_bytes`: largest internal block at that step.
- `firmware_failure_required_free_bytes`: active minimum free-memory budget at that step.
- `firmware_failure_required_largest_block_bytes`: active minimum largest-block budget.

The four memory fields were added in 1.0.8. **Internal memory at last error**
compares available memory with the active requirements. These values are captured
before HTTP/TLS, buffer and OTA cleanup, so they can differ substantially from
**Current internal memory** sampled when the page refreshes. A nonmemory failure
also records its active budget; those numbers alone do not identify the cause of
that failure. With no recorded stage, the memory fields are zero and the card
shows **None recorded**. Missing fields from an older updater show **Unavailable**,
not zero. These fields are local HTTP diagnostics; MQTT live payloads are unchanged.

These describe the last failed attempt, not the current blocker. They clear when
a new actual check starts and on reboot; local postponements can retain the
previous failure. HTTP 200 can accompany a later image/read/flash failure and does
not by itself mean the update succeeded. Missing fields from older firmware show
as unavailable. An empty failure record does not imply that an update check ran
or that the device is up to date. There is no persistent error history.

1. On a test device, set `ota_enabled = true` before its initial USB build.
2. Increment `PROJECT_VER` in root `CMakeLists.txt` for every release. The first
   version is `1.0.0`. Use exactly numeric `major.minor.patch`, without suffixes.
3. Build the same project for ESP32-S3, with this OTA layout and rollback code.
4. Upload ONLY the application `esp32_modbus.bin` through your existing upload
   page. Do not upload a merged flash image, bootloader, or partition-table binary.
5. The device checks `https://stock.alemdarteknik.com/download` automatically.

The supported server contract is HTTP 200 with the application binary directly,
using either a correct positive `Content-Length` or HTTP chunked transfer encoding.
Redirects, interactive login and HTTP errors are not accepted as firmware.
Streaming acceptance relies on ESP-IDF's parser-completion result and inactive-slot
bounds; the SDK's chunked flag alone does not prove a Transfer-Encoding header was
present. Incomplete reception, slot overflow and known-length mismatches reject
the update. The old installed-updater limitation above still applies until a
build containing the framing fix is installed.

Historical observation only, not current endpoint validation: on 2026-09-12 the
endpoint returned HTTP 200 and embedded version 1.0.4 matching the then-local
binary hash. That earlier check does not describe the currently served release
or establish uptime or capacity.
Use atomic file replacement on the server, never rewrite the served file in place.
The firmware uses one GET, without cross-request Range resumption.

Initial checks occur after 1 minute plus 0-60 minutes of random delay. Subsequent
network attempts, including failed attempts, occur after 6 hours plus 0-60 minutes.
Local preflight deferrals (busy/unhealthy runtime or insufficient memory) retry
after 5-10 minutes without contacting the server during the deferred check.
There is no tight retry loop. This spreads traffic but is not a server-enforced rollout.
Same/older versions and incompatible project/chip headers are rejected before
erasing the inactive slot. Rebuilding a file under the same version is not an update.

## Safety and operation

Internal-memory budgets in `project_config.hpp` are expressed in bytes:

| Phase | Minimum free memory | Minimum largest block | Configuration |
| --- | ---: | ---: | --- |
| Before opening TLS | 64,000 | 32,768 | `ota_minimum_free_heap`, `ota_minimum_largest_block` |
| After TLS opens and during transfer | 32,768 | 8,192 | `ota_stream_minimum_free_heap`, `ota_stream_minimum_largest_block` |

The connection budget is unchanged. The streaming budget accounts for an already
allocated TLS session and remains enforced after opening TLS and during transfer.
Either free memory or fragmentation can still reject an attempt. These defaults
are operational guards, not a reservation against concurrent allocation or
proof of adequate real-device headroom.

- OTA TLS opening and live JSON construction share a transient-memory admission
  gate. OTA waits at most 500 ms to acquire it, checks internal-RAM free/largest
  blocks before and after opening TLS, and releases it before the download.
  Live cache updates continue while publication is deferred. The gate does not
  cover Modbus transactions, HTTP scans/status requests or MQTT reconnects.
  Established TLS sessions still consume memory; OTA can remain deferred or
  abort when internal memory is insufficient. Hardware measurement is required.
- OTA runs at priority 1, below the priority-4 inverter and MQTT publisher tasks.
- Reads/writes use at most 4096-byte chunks with 50 ms yielding. Flash erasure is
  incremental. The live MQTT payload schema and polling intervals are unchanged.
- Downloading is deferred while an accepted write is queued/executing or completed in the last
  30 seconds, the publisher/bus heartbeat is unhealthy, MQTT is disconnected,
  or the configured heap headroom is unavailable. Headroom is conservative and
  must be measured on hardware while MQTT TLS and OTA TLS coexist.
- MQTT now permanently reserves its live serialization buffer before starting
  the publisher. OTA cannot consume that reservation. On internal-RAM-only
  builds, the remaining heap may be below the OTA threshold and updates will
  show as deferred. Do not lower thresholds blindly; verify/configure the
  board's PSRAM and measure simultaneous MQTT/OTA memory usage. Version 1.0.5
  enables the N16R8 PSRAM profile. Version 1.0.8 keeps the connection thresholds
  and uses the separate streaming thresholds listed above during transfer.
- An absent inverter does not block OTA on a provisioned standalone ESP. The
  bus task must still be alive and the existing network/memory checks must pass.
  Commands are accepted only after a successful local poll in the current USB
  session, within `command_link_max_age_ms` (default 15 seconds). A USB adapter
  alone is insufficient. Silent RS485 loss is detected when that window expires;
  a USB disconnect invalidates readiness immediately.
- Unavailable-inverter commands return `STATE_UNAVAILABLE` (semantic) or
  `state_unavailable` (register commands), without joining the execution queue
  or extending the OTA quiet period. Queued commands are rechecked before
  execution and rejected on disconnect, never replayed into a new USB session.
  Rejection delivery remains best-effort under a command flood or network outage.
- A command can arrive during a chunk already in progress. Flash operations may
  briefly stall execution: zero polling jitter is NOT guaranteed.
- A 15-minute deadline is checked between reads/chunks; each HTTP operation uses
  a 5-second timeout. This is not a hard real-time deadline inside SDK calls.
- The running image is never overwritten. Complete image validation, including
  its appended SHA-256, happens before choosing the next boot slot.
- Activation closes local command admission and waits up to 10 seconds for the
  bus worker to park at a transaction boundary. Admission cannot close while an
  accepted write is pending. The in-flight transaction finishes; queued getters
  are rejected instead of delaying restart. Parking failure aborts activation.
  A fixed two-second publisher grace follows parking, not an unbounded wait for
  an empty outbox. This does not guarantee the server received a result.
- Late commands are rejected without writing the inverter; rejection results
  are best-effort. Queue emptiness is not proof the server received a QoS-0
  confirmation. The server must handle missing results/unknown outcomes and
  must never blindly replay an inverter write after a reconnect.
- Reboot creates a monitoring gap. No inverter settings are changed by OTA.
- An attempted version is saved before boot selection so a failed trial does
  not repeatedly reinstall itself. A power cut in that small window can also
  suppress an unbooted version. Publish a NEW version to retry.
- An OTA trial boot gets 180 seconds to confirm 60 continuous seconds of local
  bus-worker and publisher progress. The timer is armed before loading settings.
  Failed initialization, entering setup instead of normal operation, or missing
  progress leaves the image unconfirmed and triggers a reboot/bootloader rollback.
- These health probes test task progress, not register correctness or delivery.
  They do not require a working inverter or broker during the trial. Idle-task
  watchdog panic is enabled, but it is not a universal deadlock detector.
- Wi-Fi/MQTT settings are not erased or migrated by OTA. Future configuration
  migrations must remain readable by the previous firmware to preserve rollback.

## Boundaries before fleet deployment

Only `standalone_combined` is enabled (single-phase and native three-phase).
Parallel coordinators, ESP-NOW members, and separate Internet gateways continue
using USB updates. Their routed-command quiescence and firmware forwarding need
a separate implementation. No remote fleet-update command or OTA status topic
is provided yet. Errors use the `ota_update` serial log tag and the local webpage's
last-failure fields; no fleet-wide progress reporting is added.

HTTPS verifies the server certificate. The image checksum detects corruption,
but firmware signing/key provisioning is NOT implemented. A compromised upload
account/server could distribute malicious firmware. Protect upload access and
add signed-release verification before customer fleet deployment. No secure-boot
eFuses are changed by these modifications.

The single latest-file endpoint provides no staged cohort selection, kill switch,
immutable release identity, or centralized progress report. A thousand-device
load test and server-side rollout control are still needed. Future OTA releases
must keep this partition layout and boot-validation logic; firmware alone does
not safely migrate partitions or the bootloader.

## Required bench tests

- Install baseline via USB; verify saved Wi-Fi/MQTT/installation ID and partition offsets.
- Test newer, identical, older, wrong-chip/project, oversized, HTML and corrupt images.
- Test both positive-Content-Length and chunked downloads, including fragmented
  reads, truncated responses, incomplete chunk framing, size mismatches and bodies
  larger than the inactive slot. Verify a failed download never selects a new boot
  slot. Host fakes do not validate the real SDK HTTP parser or flash hardware.
- Disconnect Wi-Fi and power during download; confirm the old image still boots.
- Boot an intentionally failing TEST image on a spare ESP; verify rollback and no reinstall loop.
- Test a broker outage during trial; verify local progress can still confirm the image.
- Send commands during download and at activation; verify no write is interrupted,
  duplicates are not replayed, and the server handles missing/rejected results.
- Measure polling gaps, free/largest heap blocks, watchdog behaviour, and task stacks.
- Confirm flash interruption at activation is recoverable, with physical recovery available.
- Run representative concurrent-download load tests before expanding beyond test devices.
- Keep app getters running during a scheduled check/update; verify they do not
  defer it. Exercise queued and long-running writes, including uncertain outcomes
  and disconnects, and verify no accepted/in-flight write is cancelled by OTA.
- Confirm PSRAM detection, boot memory test, allocator log and internal-memory
  headroom with maximum live payloads, web scans and both TLS sessions together.
- Verify memory below the connection budget but above both streaming thresholds
  can continue after TLS opens. Under controlled pressure, exercise free-memory
  and largest-block failures after TLS and during transfer. Confirm the last-error
  snapshot precedes cleanup, current memory can recover independently, local
  postponements retain the failure and the next actual check clears it.
