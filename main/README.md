# Universal Modbus Inverter Gateway (AOHAI Profile)

This component is one universal ESP32-S3 firmware for standalone, native three-phase, and multi-inverter installations. Its behavior is selected from a persistent NVS profile instead of by flashing different binaries.

The gateway framework, networking, MQTT API, topology management, and Modbus RTU transport are vendor-neutral. The bundled inverter register profile currently implements AOHAI Modbus RTU V2.14. A different Modbus inverter can reuse the framework but must provide a matching register profile because Modbus does not standardize inverter register addresses or meanings.

## Implemented runtime

Version 1.0.8 separates the OTA TLS connection budget (64,000 bytes free / 32,768
largest block) from the reserve after TLS opens and during transfer (32,768 /
8,192 bytes). The firmware card shows failure-time available and required memory
separately from current memory, captured before cleanup. These configurable
reserves need hardware validation with MQTT and OTA running together. The current
memory-blocked device needs a physical 1.0.8 baseline before OTA validation;
uploading the new binary alone cannot repair its installed updater.

Version 1.0.7 adds bounded HTTP chunked firmware downloads and local last-update
failure diagnostics. The firmware card separates the last failed step/error/HTTP
response from the current blocker and current memory. OTA scheduling, admission,
rollback, MQTT live fields and inverter polling are unchanged. Original pre-fix
1.0.5/1.0.6 updaters need a USB baseline or a response with a correct Content-Length
to install this fix; see [OTA updates](../doc/OTA_Updates.md).

Version 1.0.6 adds USB/Modbus receive-overflow rejection, eight local communication
health counters, and stale-queue protection for the separate coordinator UART
link. Routing failures explicitly distinguish not-sent from uncertain commands;
writes are never automatically retried. MQTT live payloads remain unchanged.
See [1.0.6 reliability notes](../doc/Firmware_Reliability_1.0.6.md), including
the local five-second command lifetime and remaining end-to-end limitations.
The proposed broader OTA retry rewrite remains unintegrated. Version 1.0.8 changes
memory admission and diagnostics; the schedule, write protection, rollback,
HTTP/TLS settings, live payloads and polling are unchanged. Mocked host tests do
not replace hardware tests.

Version 1.0.5 fixes OTA starvation from ordinary MQTT getters and enables Octal
PSRAM for the stated N16R8 board. Accepted writes remain protected through
completion and a quiet period; restart waits for a bus transaction boundary.
The bottom firmware card shows current memory, current blocker and last-failure diagnostics.
See [OTA updates](../doc/OTA_Updates.md) for scheduling and the one-time recovery
path for an older updater stuck before downloading. No fleet capacity claim is made.

Latest optimization audit: [runtime review](../doc/Code_Review_2026-09-11.md).
It reduces queue memory, bounds result priority, prevents incomplete live JSON on
allocation failure, fixes low-memory scan handling, and avoids repeated NVS writes
for unchanged ESP-NOW peers. Live topics, fields and power calculations are unchanged;
additional failure/defer/internal-heap counters are available on `/health`.

Live JSON uses a reserved, reusable serialization buffer (48 KiB in available
PSRAM, otherwise 32 KiB internal RAM). Startup logs the actual allocation. This
avoids allocating a large contiguous output block after constructing the JSON
tree. MQTT fields and polling intervals are unchanged; oversized payloads are
rejected with a capacity warning, never truncated.

Offline inverter commands are rejected instead of accumulating until reconnect.
This allows scheduled OTA on a provisioned standalone device without an inverter
attached; task health, network, memory and transaction-boundary guards still apply.

Opt-in standalone OTA and first USB migration: [OTA updates](../doc/OTA_Updates.md).
This checkout enables downloads with `ota_enabled = true`; review that setting
before release. Parallel OTA and release signing are not implemented. The live
payload format is unchanged; hardware validation is still required.

Production status and endurance-test procedure: [Production readiness](../doc/Production_Readiness.md). The firmware has reliability improvements but has not yet passed the release gates. Gateway `/health` telemetry reports memory, stack and queue state every 30 seconds.

- First-installation Wi-Fi AP and English web configuration page
- Two-stage validation before leaving AP mode
- Persistent NVS role, topology, connected-PV count, battery/BMS/generator presence, Wi-Fi, MQTT, Modbus, phase, and peer configuration
- ESP-NOW discovery with saved MAC/member associations
- Standalone, coordinator, member, separate gateway, and combined coordinator/gateway roles
- MQTT telemetry for documented polling-plan register blocks
- MQTT-only semantic and catalog-backed register commands routed to the appropriate inverter
- Guarded FC06/FC10 Modbus writes with compare-before-write and read-back confirmation
- Class-based polling for identity, setup, critical, fast, normal, and slow data
- Complete generated catalog for 265 holding and 422 input registers
- Configurable WS2812 RGB debug LED for setup, connectivity, data freshness and publishing errors

The old `DiagnosticScanner` remains available as a diagnostic module, but `app_main` now uses `ProductionPoller`.

The project now selects 16 MiB flash and a custom dual-OTA layout with two 3 MiB application slots. Existing build artifacts may still contain the former 2 MiB factory-only layout until reconfigured. Follow the first USB migration procedure above; an application-only OTA upload cannot migrate the partition table or bootloader.

## Source layout

```text
main/
  main.cpp                                  Role-based application orchestration
  include/inverter_gateway/
    app/
      device_profile.hpp                    Roles and installation topologies
      project_config.hpp                    Central firmware defaults
      monitor_output.hpp                    Stored serial-monitor preference
      rgb_debug.hpp                         RGB status LED interface
      system_config.hpp                     Validated persistent configuration
    inverter/
      register_catalog.hpp                  Complete register metadata/classes
      polling_plan.hpp                      Class-based production requests
      production_poller.hpp                 Due-time polling and telemetry blocks
      command_service.hpp                   Guarded semantic write service
      register_command_service.hpp          Catalog-backed read/write command service
    network/
      runtime_messages.hpp                  Transport-neutral envelopes
      wifi_provisioning.hpp                 Wi-Fi and web setup
      espnow_mesh.hpp                       Discovery and inverter-node routing
      mqtt_bridge.hpp                       MQTT telemetry and commands
    protocol/modbus_rtu.hpp                 Modbus RTU functions and CRC
    transport/usb_rs485.hpp                 CH340/CH341 USB-RS485 transport
```

Network callbacks never call Modbus directly. Commands enter a FreeRTOS queue and are executed by the single local inverter loop.

## Provisioning

On an empty NVS, the ESP32 creates:

- SSID: `INVERTER-xxxxxx`
- Password: `inverter-setup`
- Page: `http://192.168.4.1`

The access point advertises a captive-portal address and answers local DNS requests, so supported phones and computers can open the setup page automatically after joining it. If the operating system does not display its captive-portal window, open `http://192.168.4.1` manually.

Saving the form stores a draft and restarts into discovery mode. Finalization stays blocked until all applicable checks pass:

- the local inverter has answered Modbus, when this ESP has an inverter;
- the configured number of parallel members has been discovered;
- a separate gateway has discovered its coordinator;
- Wi-Fi and MQTT are connected for an MQTT-capable role.

The setup page presents only settings relevant to the selected installation and device role. Each inverter-facing ESP records how many PV inputs are physically connected and whether a battery, BMS connection, and generator are installed. The poller omits unused equipment blocks where possible, and live MQTT output suppresses unconfigured equipment, three-phase-only values on single-phase inverters, and parallel-only values on standalone systems. Consolidated live MQTT payloads group status, inverter, PV, grid, output, load, battery/BMS, generator, energy, and parallel information. Power is expressed in kW/kVA/kvar and operating codes include readable text. It scans nearby Wi-Fi networks, fills the selected SSID automatically, treats MQTT username/password as optional, derives parallel member counts, and hides ESP-NOW controls outside applicable parallel inverter roles. The master generates a stable installation ID from its ESP32 MAC address; slaves learn it wirelessly and a separate Internet Gateway learns it through the coordinator cable. An ID override, Modbus address, and MQTT prefix remain under Advanced settings.

After setup, MQTT-capable devices continue serving the configuration page at `http://esp32.local` and at their assigned LAN IP address. The phone or computer must be connected to the same local Wi-Fi network. The setup access point remains disabled during normal operation.

Advanced settings also contains **Show detailed ESP serial-monitor output**. The choice is saved separately in NVS and controls information logs, Modbus TX/RX frames, and successful CRC messages. Warnings and errors always remain visible. Firmware defaults—including this setting, polling times, setup AP name/password, Modbus timing, BOOT-button timing, and coordinator-link settings—are centralized in `include/inverter_gateway/app/project_config.hpp`. ESP-IDF menuconfig can additionally force serial-monitor output on with `CONFIG_INVERTER_GATEWAY_MONITOR_OUTPUT_DEFAULT`.

Hold the ESP32-S3 BOOT button (GPIO0 by default) for three seconds either during startup or while the device is operating normally. Outside setup mode, the device saves a setup-mode request and restarts into its access point without erasing the existing configuration.

## RGB debug LED

Configure the LED in [`include/inverter_gateway/app/project_config.hpp`](include/inverter_gateway/app/project_config.hpp):

```cpp
inline constexpr bool rgb_debug_enabled = true;
inline constexpr gpio_num_t rgb_debug_gpio = GPIO_NUM_48;
inline constexpr std::uint8_t rgb_debug_brightness = 16; // 0..255
inline constexpr bool rgb_debug_grb_order = true;
inline constexpr std::uint32_t rgb_debug_tick_ms = 250;
inline constexpr std::uint32_t rgb_debug_data_timeout_ms = 15000;
```

The driver supports one WS2812 addressable RGB pixel using ESP-IDF RMT. GPIO 48 is a configurable assumption: check your board schematic, since N16R8 specifies memory capacity rather than LED wiring. Use GRB or RGB byte order to match the LED. Discrete three-channel RGB LEDs and RGBW strips are not supported. Avoid GPIOs already used by USB, BOOT or the coordinator UART.

| Colour | Pattern | Meaning |
|---|---|---|
| Blue | Solid | Setup AP is active, including AP+station mode |
| Yellow | Blinking | Waiting for Wi-Fi station IP on an MQTT-capable role |
| Magenta | Blinking | Wi-Fi is ready but MQTT is disconnected or failed to start |
| Cyan | Blinking | ESP-NOW member has no discovered coordinator, or required coordinator UART link is disconnected |
| Red | Blinking | Waiting for inverter telemetry, or no relevant telemetry received for 15 seconds |
| Orange | Blinking | Otherwise ready, but a JSON allocation, MQTT enqueue or telemetry queue submission failed within the last 15 seconds |
| Green | Solid | Required connections are ready and relevant inverter telemetry is recent |
| White | Solid | Driver fallback when no status provider is supplied; normal firmware supplies one |

The first applicable condition wins, from Blue through Orange, then Green. Default blinking is 500 ms on and 500 ms off. On an inverter-attached ESP, data freshness refers to its local inverter. On a separate Internet Gateway, it refers to any downstream inverter. Green does not guarantee every parallel member is online, every register is valid, or MQTT delivery was acknowledged. These are gateway debug states, not inverter warning/fault codes.

The LED task starts after network-service initialization and works independently of serial-monitor printing. A LAN-only configuration webpage does not force blue. Set `rgb_debug_enabled = false` to avoid allocating the LED task and RMT channel; brightness `0` also turns the light off. Driver failure is logged without stopping the gateway. LED indication never writes to the inverter.

Implementation: [`src/app/rgb_debug.cpp`](src/app/rgb_debug.cpp). The same colour reference is in [`MQTT_Register_Command_API.md`](../doc/MQTT_Register_Command_API.md#rgb-debug-led).

## MQTT interface

Topic root:

```text
<prefix>/<device_id>/inverter/<member_id>/
```

Published topics are non-retained `live`, non-retained command `result`, retained `snapshot/<domain>/<name>/<description>`, retained `alert/<name>`, and retained `availability` (`online`/`offline`). Confirmed named setters refresh the corresponding retained snapshot. The command subscription is:

```text
<prefix>/<device_id>/inverter/+/command
```

Example semantic command:

```json
{
  "command_id": "10027",
  "operation": "set_buzzer_enabled",
  "value": 1
}
```

Allowed operations are `set_buzzer_enabled`, `set_bluetooth_enabled`, `set_inverter_enabled`, `set_overload_restart_enabled`, `set_overload_to_bypass_enabled`, and `set_battery_type`. Battery-type commissioning also requires `"commissioning_interlock": true` and a fresh standby/stopped inverter state.

Normal applications can address a catalog entry by name, without knowing its Modbus address:

```json
{
  "command_id": "10028",
  "operation": "get",
  "space": "input",
  "name": "Vbat"
}
```

```json
{
  "command_id": "10029",
  "operation": "set",
  "name": "DspBeepOnOff",
  "value": 1,
  "expected_value": 0,
  "confirmed": true
}
```

Names come from the English register catalog and are case-sensitive. If the workbook repeats a name, include its exact `description` to disambiguate it. A named 32-bit high-word entry automatically reads/writes its adjacent low word.

Every catalog entry can also be requested explicitly in blocks of at most 24 words. For example:

```json
{
    "command_id": "10030",
  "operation": "read_registers",
  "space": "input",
  "address": 125,
  "count": 20
}
```

A raw holding-register write uses compare-before-write protection:

```json
{
  "command_id": "10031",
  "operation": "write_registers",
  "space": "holding",
  "address": 207,
  "count": 1,
  "values": [1],
  "expected_values": [0],
  "confirmed": true
}
```

Writes use raw 16-bit register words. They are accepted only for documented writable holding registers and remain subject to the catalog write guard. Standby, commissioning, and service operations require the corresponding inverter state/interlock. Read-only, reserved, unsupported, and `blocked_unvalidated` entries are rejected. See `../doc/MQTT_Register_Command_API.md` for the complete contract.

Telemetry contains the register space, starting address, count, sequence, boot-relative uptime, and an array with address, catalog name, raw value, engineering value, and unit. Large Modbus reads are divided into ESP-NOW-safe blocks without splitting 32-bit values.

## ESP-NOW routing

- Member IDs start at 1; coordinator/standalone uses ID 0.
- Members announce every five seconds until learned.
- ESP-NOW packets carry a site hash, so nearby installations using the same channel are not accidentally paired.
- Coordinators save member MAC addresses in NVS and only accept data from the saved association.
- A separate Internet Gateway exchanges framed, CRC-checked telemetry, commands, results, and heartbeats with the coordinator over UART1 (TX GPIO17, RX GPIO18, 115200 8N1).
- All inverter-node ESP32s must share one 2.4 GHz ESP-NOW channel. The separate UART gateway is not part of that ESP-NOW radio group.

ESP-NOW application encryption is not enabled yet. MQTT may use credentials, but certificate provisioning for verified TLS is also not implemented. Treat this networking layer as commissioning/test firmware until both are added.

## Safety boundary

The complete register catalog does not make every register writable. MQTT can read documented readable input/holding registers and can request catalog-marked writable holding registers. Read-only, reserved, unknown and `blocked_unvalidated` entries stay blocked; guarded classes require confirmation, fresh state, compare-before-write, interlocks where applicable, and read-back verification. Write-only service actions cannot be read back and are reported as `sent_unverifiable`.

No build or flash is performed by the source-generation workflow.
# Dedicated inverter task

For fleet settings, MQTT result prioritization and the replacement commissioning procedure, see [Fleet deployment](../doc/Fleet_Deployment.md). Existing topic identities are preserved; a replacement ESP restores the saved installation ID manually. Enable the documented TLS/authentication policy before fleet deployment.

Inverter USB connection, Modbus reads and MQTT-origin command execution run in the single `inverter_bus` FreeRTOS task. MQTT serialization/publication remains separate. Automatic core scheduling is retained; no tasks are newly pinned. Configure `inverter_task_stack_bytes` (8192), `inverter_task_priority` (4), and `inverter_task_diagnostics_ms` (30000) in `project_config.hpp`. Stack-headroom reports use INFO logging. MQTT payloads and Modbus timing are unchanged. No additional inverter-write entry point is introduced.
