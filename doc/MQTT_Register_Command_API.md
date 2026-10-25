# MQTT Streams, Getters and Setters - Complete Mosquitto CLI Reference

## Communication reliability: version 1.0.6

Command request formats and MQTT `live` payloads are unchanged. Version 1.0.6
adds local USB/Modbus health counters and explicit coordinator-routing failure
metadata. See [1.0.6 release notes](Firmware_Reliability_1.0.6.md).
Version 1.0.9 changes OTA admission and retry scheduling as described below.

## ESP firmware OTA

Version 1.0.9 uses a PSRAM-aware TLS allocator and a 24 KiB contiguous internal
block guard while retaining the 64,000-byte total-free guard and streaming
reserves. Initial checks occur after 30–120 seconds; local blockers retry in
5–10 minutes, and transient failures back off from 5–10 to 30–60 minutes.
Completed checks and permanent rejections retain the 6–7 hour interval.
`firmware_next_check_in_ms` in local HTTP status supplies the webpage countdown.
An old installed updater that never passes admission needs one USB installation
of the new baseline; uploading a new server binary cannot change its running guard.

The ESP also publishes its running firmware identity once per MQTT connection:

- Topic: `<prefix>/<device-id>/esp/ESP-<station-MAC>/firmware`.
- QoS 1, retained; refreshed after reconnect, with failed enqueue retried.
- Payload: `{"device_id":"ESP-44B176CF20EC","project":"esp32_modbus","version":"1.0.9"}`.

This reports the Internet-facing ESP's application version, separately from
inverter firmware fields. It is not an update command. Native live snapshots use
the existing `sections_in_native_records:true` compact form to avoid duplicated
measurements; complete observations, power summaries, age and settings remain.

Monitoring reads current native settings every 120 seconds and after writes;
immutable charging-current choices are read once per connection. Factory-default
`QDI`/`DI` queries remain manual diagnostics. Core electrical readings, PV2 and
fault/status checks retain their cadence. Complete `/live` snapshots remain every
five seconds; detailed `/protocol/COMMAND` records publish initially, on changes,
and after reconnect. Unchanged manual reads still receive their result immediately
and update the next live snapshot. AOHAI retained setting snapshots are also
change-only, while explicit command results and write confirmations remain immediate.

Version 1.0.8 separates connection and streaming memory reserves and adds four
local HTTP failure-memory fields. MQTT command/live formats, retry timing, write
protection and polling remain unchanged. A device whose installed updater keeps
failing the memory guard needs a physical current baseline; publishing a newer
binary alone does not fix that installed updater.

Version 1.0.7 fixes bounded HTTP chunked firmware downloads and adds local
last-failure diagnostics. MQTT command/live formats, scheduling and write
admission are unchanged. Original pre-fix 1.0.5/1.0.6 builds require a USB baseline or a
server response with the exact positive Content-Length to obtain this fix; see
[OTA bootstrap requirements](OTA_Updates.md#devices-stuck-on-an-older-updater).

Version 1.0.5: getters no longer postpone scheduled update checks. Accepted
semantic setters/register writes remain protected until completion/rejection
plus a 30-second quiet interval. At activation, new requests and queued getters
are rejected while an in-flight transaction is allowed to finish. Confirmation
delivery is best-effort; never blindly retry a write. Live fields, command
schemas, power calculations and polling intervals are unchanged.
The local status API additionally exposes `firmware_update_reason`,
`firmware_internal_free_bytes`, and `firmware_internal_largest_block_bytes`.
These describe current conditions, not the exact historical failed preflight.

Version 1.0.7 adds `firmware_failure_stage`, `firmware_failure_code`,
`firmware_failure_http_status` and `firmware_failure_error` to the read-only local
HTTP `/api/status` response, not MQTT `live`. They describe the last failed update
attempt, separate from the current blocker. Stage/error are empty and code/HTTP
are zero when cleared. A new actual check or reboot clears them; a locally
postponed check can retain the previous failure. HTTP 200 is response context and
can accompany a later image/read/flash error. The webpage labels these as
**Last update error** and shows unavailable when older firmware omits the fields.
No MQTT check/update trigger or persistent failure history is added.

Version 1.0.8 adds `firmware_failure_internal_free_bytes`,
`firmware_failure_internal_largest_block_bytes`,
`firmware_failure_required_free_bytes` and
`firmware_failure_required_largest_block_bytes` to the same local `/api/status`
response. The card labels these **Internal memory at last error** and compares
available free/largest-block bytes with the active requirements before cleanup.
Current memory can be higher after TLS is released. The values describe the
active budget even when a nonmemory error caused the failure; they do not by
themselves identify its cause. They clear to zero with the failure record at a
new actual check or reboot and survive local postponements. Missing older fields
show unavailable, not zero; no recorded stage displays **None recorded**.

When the local inverter is unavailable, commands return `STATE_UNAVAILABLE`
(semantic) or `state_unavailable` (register commands). They are not queued for
later execution and do not extend the OTA quiet period. USB connection alone is
not sufficient: a recent successful poll is required. Pending commands are
discarded with rejection results when the USB session ends. MQTT delivery remains
best-effort; never blindly retry a write whose result was lost.

[OTA updates](OTA_Updates.md) are an opt-in standalone HTTPS feature, not an
inverter setter. No new MQTT update command is defined in this version. Existing
live payloads are unchanged. During the short activation window, new local
commands are rejected without sending an inverter write (`rejected` for semantic
commands, `state_not_allowed` for register commands); delivery of a result before
reboot is best-effort. Never blindly replay a write after a missing confirmation.

## Power timing correction

### Last-known display values

Existing numeric keys are preserved. New top-level `power_quality` contains an entry for each PV/grid/inverter/load/battery power and load percentage: `available`, `stale`, `state` (`valid`, `last_known`, or `unavailable`), and `since_valid_ms`. The latter is time since the result last passed validation, NOT measurement age. A previously valid value stays visible if the next computation is unavailable; it is marked stale and never used as a calculation input. Section entries restored this way have `source: last_known`, `stale: true`, and `since_valid_ms`. Never-successful readings remain null. History clears on detected member restart or gateway restart. When all live messages stop, the app must also use its own receive-time timeout.

This supersedes earlier statements that every timing failure makes summary values null: only never-valid values remain null; held values require checking `power_quality`. Existing source/basis fields describe current computations and may be absent/unavailable during holdover. Grid interpretation remains provisional R320, even when its data passes timing validation.

Priority polling attempts four-register reads for adjacent EPS apparent/active pairs (R332, R336, R340) and battery discharge/charge (R349). A failed combined read disables batching for that pair for the current adapter session and retries individual pairs. The 850 ms bus gap is unchanged. Support and achieved timing must be verified on hardware.

The scheduler repeats the essential power group (grid/PV electrical readings, PV total, inverter active total, EPS active/apparent phases, battery charge/discharge), interleaves due critical status reads, and grants one rotating background read per pass. This replaces simple full-plan reordering. The bus gap is unchanged; failures or three-phase reads can still exceed calculation age/skew limits and must produce unavailable values. No whole-payload simultaneity guarantee is made. Validate timing on hardware.

`grid_kw` temporarily uses R320-R321 (unsigned 32-bit, high word first, 0.1 W / 1000 = kW), as explicitly approved for validation. Its grid-intake interpretation is UNVERIFIED: the original workbook only says total AC power to user. The grid direction fields have been removed from live telemetry; the grid section source is `unverified_R320_register`, and its `power_factor` is null because PF is not used. R320 is included in priority polling. Missing or timing-invalid reads still yield null; no V*I*PF fallback is used. Topics and keys remain unchanged.

MQTT topics and field names are unchanged. Power calculations require source samples at most 15000 ms old relative to the newest received source timestamp, gateway cache age at most 15000 ms, and at most 5000 ms between contributing samples. Configure these limits in `project_config.hpp`. Essential power requests are grouped, but remain sequential with status reads interleaved; missing/late inputs yield null, not zero. This does not guarantee that all independent summary fields describe one simultaneous snapshot. Transport delay before gateway receipt remains unmeasured.

`grid_kw` temporarily uses R320-R321 (unsigned 32-bit, high word first, 0.1 W / 1000 = kW), as explicitly approved for validation. Its grid-intake interpretation is UNVERIFIED: the original workbook only says total AC power to user. The grid direction fields have been removed from live telemetry; the grid section source is `unverified_R320_register`, and its `power_factor` is null because PF is not used. R320 is included in priority polling. Missing or timing-invalid reads still yield null; no V*I*PF fallback is used. Topics and keys remain unchanged.

This document lists every possible MQTT getter that reads data from the inverter and every permitted MQTT setter that writes a setting or command to the inverter. Each executable catalog row has its own `mosquitto_pub` command. The safety classes remain defined in `AOHAI_Modbus_RTU_V2.14_Register_Classes.md`.

## Document map

Runtime optimization keeps the live payload and command formats unchanged.
Health reports add `live_json_failures`, `live_publish_deferrals`,
`result_queue_drops`, `json_allocation_failures`, `heap_internal_free_bytes`,
`heap_internal_largest_block_bytes`, and `heap_psram_free_bytes`. Their meanings
and limitations are in [Production readiness](Production_Readiness.md#health-subscription).
Version 1.0.6 also adds `usb_rx_dropped_bytes`, `usb_rx_overflow_events`,
`usb_disconnect_count`, `usb_reconnect_count`, `modbus_crc_errors`,
`modbus_timeouts`, `modbus_incomplete_responses`, and
`modbus_rx_overflow_failures`. These are per-boot, saturating uint32 counters
for the Internet-facing ESP's own USB/Modbus path, not per-slave counters.
After an attempted inverter write, the cached operating state is invalidated;
a following guarded command may report state unavailable until a fresh status
read succeeds. Do not blindly retry a write after a missing result.

Production hardening details and remaining release gates: [Production_Readiness.md](Production_Readiness.md). The Internet-facing gateway also publishes `<prefix>/<device-id>/inverter/<member-id>/health` every 30 seconds (QoS 0, not retained), independent of inverter-read success. Subscribe using `mosquitto_sub -v -t 'inverter/INV-44B176CDC70C/inverter/0/health'`. Its memory, stack, queue and age fields are documented in that guide.

Retained command deliveries and payloads above 2048 bytes are rejected. During MQTT outages telemetry is drained/dropped rather than queued indefinitely. Cached input values become stale after 120 seconds (slow counters: 900 seconds), checked when another input message arrives; a member uptime rollback invalidates its old input cache. Last-known register values remain visible but stale inputs are excluded from calculations. The server must still detect missing messages and uncertain write outcomes. Deduplication remains RAM-only and is not an exactly-once guarantee.

### Live measurement validity and power meanings

- Live uses only grid, inverter and load AC power groups. `load_kw` and `load_apparent_kva` are EPS-terminal active/apparent power; `sections.load` replaces `sections.output`. Duplicate output/local-load/power-to-user summary fields and raw power-register entries are removed from live; named register inquiries remain available.
- `grid_kw` remains an estimate using voltage, signed current and reported power factor. Its sign is not yet verified as import/export for every inverter model.
- Select rated output power on the webpage: 3, 4, 5, 6, 8, 12, 15 or 20 kW. Local load percentage uses the saved rating, or the requested 12 kW default for an unset legacy rating. `rated_power_kw` and `rating_source` expose the effective choice. This is a configured assumption, not automatic nameplate detection. Remote-member rating forwarding is still pending. This setting never writes to the inverter.
- `bms_configured` replaces the old configuration meaning of `bms_connected`; `bms_connected` is now null (unknown), not inferred from all-zero BMS registers. `battery_installed` and `generator_installed` remain configuration choices.
- Raw register entries include `age_ms` measured since gateway cache receipt and `stale`. Display stale values as last-known, not live. Samples are sequential, not simultaneous, and transport delay is not included. `core_status_fresh` covers only the status block. `power_summary.freshness_limit_ms` describes the cache-age ceiling and `sampling` is `sequential_cached_readings`. Expiry of a previously received primary power register produces null rather than silently selecting a fallback source. On startup a source can still change as primary registers are first discovered.
- Battery subtraction requires both charge and discharge readings. Calculated PV total requires all configured PV inputs. Unknown power is null, not a fabricated zero.

1. MQTT topic tree, subscriptions, retention and polling timing.
2. Terminus/Linux and Windows CLI quick-start.
3. Getter and setter payloads, lifecycle and returned results.
4. Complete configuration getter catalog.
5. Complete input-data getter catalog.
6. Complete executable setter catalog.
7. Choice-specific commands for every documented multi-option setter.
8. Workbook-writable rows that firmware intentionally blocks.

The internal catalog accounts for all 687 documented rows: 265 configuration items and 422 input-data items. The public API exposes 223 uniquely named configuration getters, 288 uniquely named input-data getters, and 201 named executable setters. The reference includes 166 choice-specific commands across 58 settings, plus individual setter/confirmation recipes for all 201 setters. All 204 writable map rows are accounted for: three are blocked by the current firmware policy. Low-word, reserved, unnamed, and ambiguous catalog rows are handled internally or omitted because they do not have a public name. See [Detailed setter and confirmation recipes](#detailed-setter-and-confirmation-recipes) for commands using your device ID and automatically generated request IDs.

## RGB debug LED

Configure the single addressable WS2812 LED in `main/include/inverter_gateway/app/project_config.hpp`:

```cpp
inline constexpr bool rgb_debug_enabled = true;
inline constexpr gpio_num_t rgb_debug_gpio = GPIO_NUM_48;
inline constexpr std::uint8_t rgb_debug_brightness = 16;
inline constexpr bool rgb_debug_grb_order = true;
inline constexpr std::uint32_t rgb_debug_tick_ms = 250;
inline constexpr std::uint32_t rgb_debug_data_timeout_ms = 15000;
```

Check the actual board schematic before selecting the pin: N16R8 describes flash/RAM, not LED wiring. GPIO 48 is the default assumption; some boards use another GPIO. This driver supports a single WS2812 RGB pixel with GRB or RGB byte order, not a discrete three-channel RGB LED or RGBW strip. Avoid pins already used by USB, BOOT or the coordinator UART. Set brightness to 0 or disable the feature for no light. Disabling the feature creates no LED task and allocates no RMT channel. LED driver failures are logged without stopping inverter communication. No additional downloaded LED component is required.

| Colour | Pattern | Meaning |
|---|---|---|
| Blue | Solid | Setup AP is active (including AP+station mode) |
| Yellow | Blinking | This role needs MQTT/Wi-Fi, but station IP is not ready |
| Magenta | Blinking | Station IP is ready, but MQTT is disconnected or failed to start |
| Cyan | Blinking | ESP-NOW member has no discovered coordinator, or required coordinator UART link is disconnected |
| Red | Blinking | No successful relevant inverter telemetry within 15 seconds, or waiting for the first reading |
| Orange | Blinking | Connections/data are otherwise ready, but JSON allocation, MQTT enqueue or telemetry queue submission failed within the last 15 seconds |
| Green | Solid | Required connections are ready and relevant inverter telemetry was received within 15 seconds |
| White | Solid | Driver fallback when no status provider is supplied; normal firmware supplies a provider |

Blinking defaults to 500 ms on / 500 ms off. Priority follows the table from Blue through Orange, then Green. The LED task starts after the network services have been initialized; it is not an early-boot crash indicator. It works independently of serial-monitor printing. Setup mode deliberately remains blue even if connections are incomplete. A webpage available only on the LAN does not force blue.

For inverter-attached ESPs, freshness means telemetry from their own inverter. For a separate Internet Gateway, it means telemetry arriving from any downstream inverter. ESP-NOW members do not need Wi-Fi/MQTT themselves. Green is not a guarantee that every parallel member is online, that every register is valid, or that the broker acknowledged the last live message. Cyan on an ESP-NOW member reflects discovery state, not a radio acknowledgement on every packet. These colours diagnose the ESP gateway, not inverter warning/fault codes. The LED does not issue inverter writes or introduce an MQTT LED setter.

## Individual feature-flag controls

Each control changes only its flag in `flcdEn`, preserving all other bits through an internal read-modify-write and full read-back. These semantic operations require a fresh standby/stopped inverter state, unlike the generic whole-field `hot_edit_guarded` setter. Send numeric `1` to enable or `0` to disable; the semantic API currently expects numbers, not JSON booleans. Bits 8 through 15 remain untouched. Feature names come from the map; support and behaviour depend on the inverter model. In particular, split-phase, generator and grid-feedback options must match the installation.

Start this confirmation subscription in another terminal before publishing:

```bash
mosquitto_sub -v -t 'inverter/INV-44B176CDC70C/inverter/0/result'
```

Match `command_id` in the result. These semantic replies use uppercase `outcome`: `CONFIRMED` means read-back matched; `STATE_NOT_ALLOWED`, `STATE_UNAVAILABLE`, `COMMUNICATION_ERROR`, or `ADJUSTED` are not confirmed success. These operations currently do not refresh the setting snapshot automatically. To explicitly read the combined current flags, use the existing configuration getter for `flcdEn`. Update all ESP nodes that execute these new operations; older firmware will not recognize the added operations.

### ECOMode_En

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_eco_mode_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_eco_mode_enabled\",\"value\":0}"
```

### OverLoad_RestartEn

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_overload_restart_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_overload_restart_enabled\",\"value\":0}"
```

### OverTemp_RestartEn

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_overtemperature_restart_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_overtemperature_restart_enabled\",\"value\":0}"
```

### InputChange_RemEn

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_input_change_reminder_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_input_change_reminder_enabled\",\"value\":0}"
```

### OPSplit_PhaseEn

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_output_split_phase_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_output_split_phase_enabled\",\"value\":0}"
```

### Generator_AutoIPEn

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_generator_auto_input_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_generator_auto_input_enabled\",\"value\":0}"
```

### DualChannel_LoadEn

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_dual_channel_load_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_dual_channel_load_enabled\",\"value\":0}"
```

### ubridFeedBackEn

Enable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_grid_feedback_enabled\",\"value\":1}"
```

Disable:

```bash
mosquitto_pub -q 1 -t 'inverter/INV-44B176CDC70C/inverter/0/command' -m "{\"command_id\":\"$(date +%s%N)\",\"operation\":\"set_grid_feedback_enabled\",\"value\":0}"
```


## Public device identity

Live delivery note: any successful input-register update can trigger a live message, limited to the configured fast interval. A failed core-status block no longer suppresses other successful measurements. `core_status_fresh` and `core_status_age_ms` report the age of the cached core status; they do not certify freshness of every measurement. If all inverter reads stop succeeding, no new live measurement is generated.

Telemetry, snapshots, alerts and command results use the MQTT task's asynchronous outbox. It is limited to 64 KiB; an allocation or capacity failure is logged and that message is not queued. An alert rejected by the queue is retried when its value is next polled. Clients must still check matching command results; absence of a result is not confirmation and must not trigger an automatic write retry.

The public MQTT API does not use a user-created site ID. Each installation is addressed by a stable `device-id` generated automatically from the coordinator or gateway ESP32 hardware MAC address. This prevents installers from accidentally assigning the same identity to different installations.

For a standalone inverter, use member `0`. For a parallel installation, the same hardware-derived `device-id` groups the installation and `member-id` selects the individual inverter: coordinator/master `0`, then members `1...N`.

The server never needs a Modbus register address. Getters and setters use the documented English `name`; Modbus function codes, address spaces, register numbers, raw words, scaling and read-back addresses are private firmware details. Any MQTT payload attempting to provide those internal fields is rejected.

## Command origin and routing

MQTT is the only external inverter-command source. The provisioning web page changes ESP, topology, peer, Wi-Fi and MQTT configuration only; it cannot read or write inverter registers on demand.

The MQTT bridge accepts catalog access only through the named `get` and `set` operations. Raw address-based read/write operations are rejected, as are named commands containing address, space, count, raw-word, scaling, or expected-register fields. It marks every accepted command with an origin field; the coordinator, UART and ESP-NOW links preserve that field, and every inverter node rejects a command without MQTT origin. This is a software routing boundary, not cryptographic proof; transport encryption/authentication is still required for production.

```text
MQTT -> gateway/coordinator -> optional UART -> optional ESP-NOW
     -> selected inverter ESP -> one serialized Modbus worker
     -> result returns to MQTT over the reverse route
```

The separate gateway/coordinator UART queue rejects commands while offline and
rechecks an observed-link generation and five-second local queue age before
forwarding. Queued commands invalidated by an observed disconnect, local expiry,
or generation change are not replayed into a later link session. The transfer
has a bounded deadline; partial transmission or failed completion is uncertain.
Neither case automatically retries a write.

This does not add a remote execution deadline, peer boot/session nonce, durable
result delivery or exactly-once execution. A short unobserved peer restart may
not invalidate the local generation. Commands already forwarded cannot be recalled;
normal results still arrive asynchronously. Treat a missing result as unknown.

### Routing failure result fields

`routing_status` is optional and appears for routing failures only. Existing
successful result fields are unchanged; do not require this field on every result.

| `routing_status` | Semantic operation result | Named register getter/setter result |
|---|---|---|
| `not_sent` | `outcome: "REJECTED"`, `write_sent: false` | `outcome: "state_not_allowed"`, `write_guard: null` |
| `uncertain` | `outcome: "UNKNOWN"`, `write_sent: null` | `outcome: "communication_error"`, `write_guard: null` |

`write_sent: null` means the gateway cannot establish whether the remote inverter
wrote; it must not be interpreted as false. The register-result null guard means
the gateway did not obtain a remote guard evaluation; it does not grant permission.
These routing failures do not fabricate `modbus_status` or `modbus_exception`.

Example semantic result for uncertain forwarding:

```json
{
  "member_id": 1,
  "command_id": "920010",
  "operation": "set_buzzer_enabled",
  "outcome": "UNKNOWN",
  "routing_status": "uncertain",
  "write_sent": null
}
```

For local USB admission, client status `receive_busy` is numeric 12. It means
the RX drain overlapped incoming data and no write was transmitted. Semantic
results set `write_sent: false`; catalog results can include `modbus_status: 12`.
This is a client diagnostic, not an inverter exception. A receive overflow after
transmission is different: the write may have happened, but its confirmation is
rejected. Never automatically repeat a setter to compensate for either a missing
confirmation or an uncertain result; first obtain current state and reassess.

Command topic:

```text
<prefix>/<device-id>/inverter/<member-id>/command
```

Published topics:

```text
<prefix>/<device-id>/inverter/<member-id>/live
<prefix>/<device-id>/inverter/<member-id>/alert/<alert-name>
<prefix>/<device-id>/inverter/<member-id>/result
<prefix>/<device-id>/inverter/<member-id>/snapshot/<domain>/<name>/<description>
<prefix>/<device-id>/inverter/<member-id>/availability
<prefix>/<device-id>/inverter/<member-id>/health
```

`member-id` is `0` for the standalone/coordinator inverter and `1...N` for parallel members.
In a parallel installation, the Internet-facing ESP publishes every inverter under its own `member-id`; a `+` member wildcard therefore receives the master and every configured slave without opening additional MQTT connections.

| Stream | Purpose | Publish policy |
|---|---|---|
| `live` | Repeated voltage, current, instantaneous power, PV, battery, load, temperature, and status samples | QoS 0, not retained |
| `alert/#` | Inverter faults, warnings, state transitions, derating and BMS faults/warnings | QoS 1, retained per named alert; published only when its value changes |
| `result` | Correlated response to a named getter or setter | QoS 1, not retained |
| `snapshot/#` | Serial/model/firmware and current configuration | QoS 1, retained per named item; refreshed after a confirmed setter |
| `availability` | Device `online`/`offline` state | QoS 1, retained |
| `health` | Internet-facing ESP memory, queue, timing and local USB/Modbus diagnostics | QoS 0, not retained; every 30 seconds while connected |

`mosquitto_pub` sends a getter or setter. `mosquitto_sub` receives the asynchronous streams and the correlated result. A retained snapshot or alert-state message is delivered immediately when a new subscriber connects.

### What is published where

| Data category | MQTT destination | When it is published |
|---|---|---|
| Grid, inverter, PV, battery, BMS, load, generator and instantaneous power measurements | `live` | One consolidated state after each completed fast polling cycle |
| Direct inverter energy counters, when supported by the inverter | Explicit getter response | Excluded from `live`; the app calculates energy |
| Inverter operating status, fault words, main/sub fault codes, warning codes and derating state | `alert/#` | Initial retained state and then only when changed |
| BMS status, error words, warning words and protected-pack identity | `alert/#` | Initial retained state and then only when changed |
| Serial number, device identity, firmware, protocol version and configuration | `snapshot/#` | Once after inverter communication starts; retained by broker |
| A named getter response | `result` | Once for the matching `command_id` |
| A named setter confirmation or rejection | `result` | Once for the matching `command_id` |
| Setting read-back after a confirmed setter | `snapshot/#` | Immediately after confirmation; replaces that retained setting |

### Polling and delivery timing

| Poll class | Interval | Typical content |
|---|---:|---|
| Critical | 1 second | Operating state and inverter fault/warning data |
| Fast | 5 seconds | Grid, EPS, PV, battery/BMS, power flow and parallel data |
| Normal | 30 seconds | Extended PV, generator, fan/runtime and normal health data |
| Slow | 300 seconds | Direct inverter energy counters, when the inverter supports them |
| Boot/setup snapshot | Once per inverter communication session | Serial/model/firmware and configuration |

An alert cannot be emitted faster than the poll containing its source value. Inverter fault changes are normally detected within about one second; BMS fault/warning changes are normally detected within about five seconds.

The `live` payload is assembled from the newest successful values held by the ESP. Fast measurements update every fast cycle, while other included values remain in the payload with their most recently read value. A subscriber therefore receives a complete current state and does not need to combine separate Modbus-block messages. Every topology uses the same JSON structure. `sections.status`, `sections.inverter`, `sections.pv`, `sections.grid`, `sections.output`, `sections.battery`, `sections.bms`, `sections.generator`, `sections.parallel`, and `sections.other` are always present; a section is an empty array when it does not apply or no successful value is available yet.

`power_summary` is the recommended field for the app/server to consume when it needs live power values. The ESP publishes instantaneous power only. It does not integrate energy over time and it does not create daily/monthly/yearly energy counters. The app/server should calculate energy from `power_summary` samples and its own timestamps, so a power cut or ESP restart can be handled by the server history.

`sections.load` and `sections.energy` are omitted. Load measurements, including load percentage, are grouped under `sections.output`; grid-related measurements remain under `sections.grid`. The app/server calculates energy. Explicit energy getters remain available when supported by the inverter.

Live decimal measurements and calculated powers are serialized with two decimal places (`%.2f`), for example `3.52` or `41.50`, as JSON numbers. Integer identifiers, counters and status codes remain integers. Apps may need their own two-decimal display formatting after parsing JSON because numeric parsing does not preserve trailing zeros. Calculations use full precision before formatting.

`power_summary.load_percentage_source` is `register` when input register R344 is available. If R344 is unavailable, the ESP reports `calculated` only when it has valid EPS phase voltage/current measurements and the inverter rated power. The fallback uses the sum of phase apparent power divided by rated power. The same calculated value is also added to `sections.output`.

Signed battery power is published as `power_summary.battery_kw`: positive means the battery is discharging into the inverter/load, negative means the battery is charging, and zero means no battery power flow or no detected current.

`power_summary` may contain these keys when their source values are available:

| Key | Unit | Meaning |
|---|---|---|
| `pv_kw` | kW | Total PV input power; direct inverter register preferred, otherwise calculated from connected PV voltage/current |
| `grid_kw` | kW | Signed grid active-power estimate from complete phase voltage/current readings and valid grid power factor; otherwise null |
| `grid_apparent_kva` | kVA | Sum of phase voltage times current magnitude; null if any required phase is missing |
| `inverter_kw` | kW | AC active-power total (ActPacAll), otherwise the sum of required ActPac phase readings; null if unavailable |
| `inverter_apparent_kva` | kVA | Sum of inverter phase voltage times current magnitude, published independently of active power; otherwise null |
| `output_kw` | kW | Sum of EPS phase active-power readings; otherwise null |
| `output_apparent_kva` | kVA | Sum of EPS phase apparent-power readings, otherwise calculated from output voltage/current |
| `load_kw` | kW | Direct total AC power to user, otherwise EPS active power; null if neither is available |
| `load_apparent_kva` | kVA | EPS/output apparent power, available alongside load active power; otherwise null |
| `load_kva` | kVA | Fallback load apparent power calculated from output voltage/current |
| `load_percentage` | % | Direct R344 load percentage if available, otherwise calculated from output apparent power and rated inverter power |
| `battery_kw` | kW | Signed battery power; positive discharging, negative charging |

This contract applies independently to member `0` and every parallel member topic. A native three-phase member contains its available R/S/T values. A grouped three-phase member contains the locally connected inverter's values plus its configured `phase_1`, `phase_2`, or `phase_3` assignment. A separate Internet Gateway forwards the same structure without changing the originating member's applicability profile.

Active power is published in `kW`, apparent power in `kVA`, and reactive power in `kvar`. Values are already converted; clients must not apply the source-register scale again.

Grid, inverter and Output/Load expose active and apparent power independently. Voltage times current provides apparent power, not active power. Missing grid power factor is not assumed to be 1. Active power is null if its required readings are unavailable; null is not zero and must not be integrated as a zero sample. Grid active power remains an estimate using the reported grid PF, including in three-phase mode. AC active power and system generation are separate readings. Load active power may cover total power to user while load apparent power describes EPS output, so these are not guaranteed to be the same measurement boundary. Each native three-phase sum requires all three phases; parallel ESP members publish their own local readings. Load measurements remain in `sections.output`; no separate Load or Energy section is created.

Polling covers the complete plan, with a compile-time capacity check. Due requests rotate fairly, with critical status given priority between other transactions. Failed boot/setup reads retry at the normal interval instead of blocking later requests. AC, EPS and battery power pairs are requested separately; load percentage is not grouped with system generation. Configured polling intervals are eligibility intervals, not guaranteed full-sweep times: the 850 ms Modbus spacing, response timeouts and number of enabled readings limit actual update frequency.

Live data is filtered by the setup profile of the ESP attached to that inverter:

- `connected_pv_inputs` limits PV voltage, current, and individual power fields to PV1 through the configured count; unused PV-only Modbus blocks are skipped or shortened where the address layout permits.
- Battery, BMS, and generator fields are published only when that equipment is selected during setup. Dedicated unused Modbus blocks are skipped.
- Standalone and parallel single-phase inverters omit S/T and other three-phase-only fields.
- A grouped three-phase installation treats each member as its assigned single-phase unit and reports its assignment as `phase_1`, `phase_2`, or `phase_3`.
- Native three-phase inverters retain R/S/T measurements.
- Standalone systems omit parallel-only data; parallel systems retain it.
- Mixed Modbus blocks may still be read to obtain applicable values, but irrelevant fields from those blocks are not published.
- `name` is the user-facing measurement name, `register_name` preserves the protocol name for diagnostics, and `unit` is the final engineering unit because `value` is already scaled.

### Terminus/Linux subscriptions

Subscribe to all published streams for one device installation using one MQTT connection:

```bash
mosquitto_sub -h "<broker>" -q 1 -v \
  -t "<prefix>/<device-id>/inverter/+/live" \
  -t "<prefix>/<device-id>/inverter/+/alert/#" \
  -t "<prefix>/<device-id>/inverter/+/result" \
  -t "<prefix>/<device-id>/inverter/+/snapshot/#" \
  -t "<prefix>/<device-id>/inverter/+/availability"
```

The equivalent compact one-device wildcard is:

```bash
mosquitto_sub -h "<broker>" -q 1 -v -t "<prefix>/<device-id>/inverter/#"
```

This compact testing wildcard also displays command messages published under the same inverter tree. Use the explicit stream filters above when the subscriber must receive ESP output only.

Separate terminal subscriptions:

```bash
# Repeated live measurements
mosquitto_sub -h "<broker>" -q 1 -v -t "<prefix>/<device-id>/inverter/+/live"

# Current and changing warnings/faults
mosquitto_sub -h "<broker>" -q 1 -v -t "<prefix>/<device-id>/inverter/+/alert/#"

# Getter responses and setter confirmations
mosquitto_sub -h "<broker>" -q 1 -v -t "<prefix>/<device-id>/inverter/+/result"

# Retained serial/model/firmware/configuration data
mosquitto_sub -h "<broker>" -q 1 -v -t "<prefix>/<device-id>/inverter/+/snapshot/#"
```

For a central server handling every device installation, use one persistent wildcard subscription rather than one connection per inverter:

```bash
mosquitto_sub -h "<broker>" -q 1 -v \
  -t "<prefix>/+/inverter/+/live" \
  -t "<prefix>/+/inverter/+/alert/#" \
  -t "<prefix>/+/inverter/+/result" \
  -t "<prefix>/+/inverter/+/snapshot/#" \
  -t "<prefix>/+/inverter/+/availability"
```

MQTT wildcard meaning:

- `+` matches exactly one topic level, such as one device installation or one inverter member.
- `#` matches the remaining topic levels, including all snapshot and alert subtopics.
- Add `-u "<username>" -P "<password>"` when broker authentication is enabled.
- Add `--cafile <ca-file>` and use the configured TLS listener for a production broker.
- Add `-R` when testing only future events and you intentionally do not want retained snapshot/alert state.

For more than one thousand inverters, use one long-lived MQTT client connection with wildcard subscriptions. Do not start one `mosquitto_sub` process or one broker connection per inverter; the CLI commands here are for commissioning and testing.

### Live subscriptions by installation scenario

Live topics use this structure:

```text
<prefix>/<device-id>/inverter/<member-id>/live
```

The examples below use broker `192.168.106.45`, port `1883`, default prefix `inverter`, and example device ID `INV-AABBCCDDEEFF`.

#### Standalone single-phase inverter

A standalone inverter is always member `0`:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/INV-AABBCCDDEEFF/inverter/0/live'
```

#### Standalone native three-phase inverter

A native three-phase inverter is also member `0`. Its R/S/T phase measurements are carried in the member's live messages:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/INV-AABBCCDDEEFF/inverter/0/live'
```

#### Complete parallel installation

Use a single-level `+` wildcard to receive the master and every parallel member:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/INV-AABBCCDDEEFF/inverter/+/live'
```

The member topics are:

```text
inverter/INV-AABBCCDDEEFF/inverter/0/live    coordinator/master
inverter/INV-AABBCCDDEEFF/inverter/1/live    parallel member 1
inverter/INV-AABBCCDDEEFF/inverter/2/live    parallel member 2
inverter/INV-AABBCCDDEEFF/inverter/N/live    parallel member N
```

#### Parallel master only

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/INV-AABBCCDDEEFF/inverter/0/live'
```

#### One parallel member only

For example, subscribe to member `2`:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/INV-AABBCCDDEEFF/inverter/2/live'
```

#### Parallel three-phase groups

Subscribe to all members. Each member remains individually identifiable, and its telemetry includes the configured phase assignment:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/INV-AABBCCDDEEFF/inverter/+/live'
```

#### Separate Internet-gateway arrangement

The coordinator-to-gateway cable does not change the public MQTT tree. Subscribe exactly as for the corresponding standalone or parallel installation. The Internet-facing ESP publishes every routed inverter under its assigned member ID:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/INV-AABBCCDDEEFF/inverter/+/live'
```

#### Every installation on a central server

Use one persistent wildcard subscription for all device IDs and all inverter members:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v \
  -t 'inverter/+/inverter/+/live'
```

The first `+` selects any hardware-derived device ID. The second `+` selects any inverter member.

#### Discover device IDs

Subscribe to availability records:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -q 1 -v \
  -t 'inverter/+/inverter/+/availability'
```

For commissioning only, inspect every topic under the default prefix:

```bash
mosquitto_sub -h 192.168.106.45 -p 1883 -v -t 'inverter/#'
```

Add `-u '<username>' -P '<password>'` to any example when broker authentication is enabled. Replace the broker, port, prefix, device ID and member ID with the values used by the installation.

### Stream message illustrations

Example live message:

```json
{
  "member_id": 0,
  "sequence": 42,
  "uptime_ms": 180500,
  "connected_pv_inputs": 2,
  "topology": "standalone_single_phase",
  "battery_installed": true,
  "bms_configured": true,
  "bms_connected": null,
  "generator_installed": false,
  "phase": "none",
  "operating_status": {
    "value": 1,
    "running": true,
    "running_status": "On-grid",
    "operating_mode_value": 2,
    "operating_mode": "SBU",
    "energy_priority": "Load first"
  },
  "power_summary": {
    "pv_kw": 3.3671,
    "pv_source": "register",
    "grid_kw": 1.1142,
    "grid_phase_count": 1,
    "inverter_kw": 3.52,
    "inverter_source": "register",
    "output_apparent_kva": 3.515,
    "output_phase_count": 1,
    "load_kva": 3.515,
    "load_source": "calculated_apparent_power",
    "load_percentage": 29.29,
    "load_percentage_source": "calculated",
    "battery_kw": -0.18,
    "battery_direction": "positive=discharging, negative=charging",
    "battery_source": "charge_discharge_registers"
  },
  "sections": {
    "pv": [
      {"name": "PV Total Input Power", "register_name": "PpvAll H", "description": "PV Total Input Power", "domain": "PV", "unit": "kW", "value": 0.042}
    ],
    "grid": [
      {"name": "Grid Frequency", "register_name": "Fac", "description": "Grid Frequency", "domain": "GRID", "unit": "Hz", "value": 49.96}
    ],
    "output": [
      {"name": "Output apparent power", "register_name": "", "description": "Calculated from EPS output voltage/current", "domain": "EPS", "unit": "kVA", "value": 3.515, "source": "calculated"},
      {"name": "Output load percentage", "register_name": "", "description": "Calculated from EPS output power and rated inverter power", "domain": "EPS", "unit": "%", "value": 29.29, "source": "calculated"},
      {"name": "Total AC power to user", "register_name": "PactouserTotal H", "description": "Total AC power to user", "domain": "LOAD", "unit": "kW", "value": 3.52}
    ],
    "battery": [
      {"name": "Battery Voltage", "register_name": "Vbat", "description": "Battery Voltage", "domain": "BATTERY", "unit": "V", "value": 41.5},
      {"name": "Battery power", "register_name": "", "description": "Signed battery power: positive discharging, negative charging", "domain": "BATTERY", "unit": "kW", "value": -0.18, "source": "charge_discharge_registers"}
    ]
  }
}
```

For dashboards and history logic, read `power_summary` first. The section arrays are useful for detailed screens and diagnostics. The app/server is responsible for calculated energy totals; there is no Energy or separate Load section in `live`.

Example retained warning/fault state:

```json
{
  "member_id": 0,
  "sequence": 43,
  "uptime_ms": 181000,
  "severity": "warning",
  "state": "active",
  "name": "InvMainWarnCode",
  "description": "Inverter Main Warning Code",
  "previous_value": 0,
  "value": 12
}
```

When its value returns to zero, the same retained alert topic publishes `"state":"clear"`. Inverter system-fault words, main/sub fault and warning codes, derating state, BMS status, BMS errors and BMS warnings are monitored for changes.

Example retained serial-number snapshot:

```json
{
  "member_id": 0,
  "sequence": 2,
  "uptime_ms": 5200,
  "name": "SerialNumber",
  "description": "Inverter serial number",
  "domain": "IDENTITY",
  "value": "EXAMPLE123456789"
}
```

The serial-number words are assembled inside the ESP. Other model, firmware and configuration values are retained as individually named snapshot messages.

After a confirmed setter, `result` first reports the correlated command result and the corresponding retained snapshot is refreshed with `updated_by_command_id`:

```json
{
  "member_id": 0,
  "updated_by_command_id": "920001",
  "name": "DspBeepOnOff",
  "description": "Buzzer switch",
  "domain": "SYSTEM",
  "value": 1
}
```

## Getter command: read data from the inverter

### Normal named inquiry

The server does not need the Modbus address. It sends the exact English catalog variable name:

```json
{
  "command_id": "20001",
  "operation": "get",
  "category": "input_data",
  "name": "Vbat"
}
```

The MQTT gateway resolves `Vbat` to its input-register descriptor before routing the compact command to the inverter ESP. Names are case-sensitive. If the workbook contains the same name more than once in one register space, add its catalog `description` and/or `domain`:

```json
{
  "command_id": "20002",
  "operation": "get",
  "category": "input_data",
  "name": "SOC",
  "description": "Battery SOC",
  "domain": "battery"
}
```

The domain distinguishes local battery `SOC` from parallel aggregate `SOC`. Valid domain strings are `identity`, `communications`, `system`, `inverter`, `grid`, `eps`, `pv`, `generator`, `battery`, `bms`, `load`, `power`, `energy`, `parallel`, `diagnostics`, and `reserved`.

A named 32-bit item is assembled internally. MQTT never supplies its address, high word, low word or scale.

A successful result contains the resolved catalog identity and decoded application value. It does not expose a Modbus address or raw register word:

```json
{
  "member_id": 0,
  "command_id": "20001",
  "operation": "get",
  "outcome": "confirmed",
  "name": "Vbat",
  "description": "Battery Voltage",
  "domain": "BATTERY",
  "unit": "0.1V",
  "value": 41.5
}
```

Getter lifecycle:

```text
server publishes get on command
-> selected ESP reads the inverter
-> ESP attempts to publish a result with the same command_id
```

A getter response is not retained and delivery is best-effort, not exactly-once. Subscribe to `result` before publishing the getter. Use `snapshot/#` when the server needs retained identity/configuration immediately after connecting; use a getter when it needs a fresh on-demand read.

## Setter command: write a setting or command to the inverter

### Normal named setter

```json
{
  "command_id": "20004",
  "operation": "set",
  "name": "DspBeepOnOff",
  "value": true,
  "confirmed": true
}
```

The setter contains the desired application value, never a Modbus address or expected raw register word. The ESP resolves the setting, converts engineering units or boolean values to the inverter encoding, reads the current value itself, writes through the applicable guard and verifies the result.

The service performs:

```text
resolve setting name -> encode application value -> write-guard/state check
-> read current inverter value
-> FC06 for one word or FC10 for multiple words
-> read back -> decode application value -> publish result
```

Setter lifecycle:

```text
server publishes set on command
-> ESP validates name, value, state and required interlock
-> ESP reads the current value
-> ESP writes only when a change is needed
-> ESP reads back the setting
-> ESP publishes result with the same command_id
-> if confirmed, ESP refreshes the retained snapshot for that setting
```

The requester should treat `result` as the command acknowledgement. The refreshed `snapshot/#` message is the durable current configuration broadcast for all subscribers and for clients that connect later.

Rules:

- Every setting name must resolve uniquely to a documented writable setting.
- `confirmed: true` is mandatory.
- The MQTT bridge remembers the most recent 32 command IDs and rejects duplicates during the current boot. Use a new nonzero ID for every deliberate request.
- Boolean settings accept `true` or `false`; numeric settings accept engineering values; two-character ASCII cells accept a string.
- The ESP handles signed encoding, scale, and complete multiword writes internally.
- The server must still validate documented ranges, enums and dependencies before requesting a safety-sensitive change.

## Write-guard requirements

| Guard | MQTT behavior |
|---|---|
| `read_only` | Rejected. |
| `blocked_unvalidated` | Rejected. |
| `hot_edit_confirmed` | Confirmation and internal read-back required. |
| `hot_edit_guarded` | Confirmation, documented dependency validation and `"guarded_interlock": true` required. |
| `runtime_command` | Requires fresh non-fault/non-update state, except an inverter-off request; unvalidated H0 values are rejected. |
| `standby_required` | Requires fresh `standby` or `stopped` state. |
| `commissioning_only` | Requires fresh standby/stopped state and `"commissioning_interlock": true`. |
| `service_only` | Requires fresh standby/stopped state and `"service_interlock": true`. |

Example commissioning setter:

```json
{
  "command_id": "20007",
  "operation": "set",
  "name": "fGridVoltLow1EE",
  "value": 180.0,
  "confirmed": true,
  "commissioning_interlock": true
}
```

Do not use sample values as an installation prescription. Parallel, battery/BMS, grid-code, protection, reset and firmware-update settings require the exact inverter model documentation and qualified commissioning procedure.

## Outcomes

| Outcome | Meaning |
|---|---|
| `confirmed` | Read completed, a persistent setting already matched, or setting read-back matched. Legacy OnOffSet register-only confirmation does not verify operating state. |
| `completed` | OnOffSet write was accepted and actual operating state was verified; `operating_state_confirmed:true`. |
| `transition_pending` | OnOffSet write was accepted, but its requested operating state is not yet verified. |
| `readback_mismatch` | An actual setting readback differs from the requested value. |
| `sent_unverifiable` | A write-only service action received a valid Modbus response but cannot be read back. Never retry automatically. |
| `not_documented` | The requested name is absent from the catalog. |
| `not_readable` | The requested name identifies a write-only command. |
| `read_only` | The requested name is not writable. |
| `blocked_unvalidated` | The catalog/model policy intentionally forbids this write. |
| `confirmation_required` | `confirmed: true` was missing. |
| `state_unavailable` | Fresh operating state is unavailable. |
| `state_not_allowed` | Current operating state does not satisfy the guard. |
| `guarded_interlock_required` | A guarded runtime-policy acknowledgement is missing. |
| `commissioning_interlock_required` | Protected commissioning acknowledgement is missing. |
| `service_interlock_required` | Protected service acknowledgement is missing. |
| `communication_error` | Modbus transmit, response, CRC, exception, echo or read-back failed. |
| `invalid_request` | Count/range, pair completeness or write-only grouping is invalid. |

## Coverage and limitations

The internal catalog contains 265 configuration rows and 422 input-data rows. Every uniquely named, readable application item has a public getter; catalog rows that are only the second half of a value are assembled internally. Every named writable setting allowed by policy reaches the common guard engine. This does not override its safety class: read-only and blocked items remain non-writable, and the inverter may reject unsupported model-specific features.

The current transport security is not production-complete: ESP-NOW payload encryption, verified MQTT TLS certificate provisioning, user authorization levels, replay protection that survives reboot and hardware validation on every writable register remain required before unattended deployment.

## Illustrated Mosquitto workflow

```text
operator/backend                  Mosquitto                     target ESP/inverter
       |                             |                                  |
       |-- publish get/set --------->|                                  |
       |                             |-- route device/member command -->|
       |                             |                                  |-- Modbus read/write
       |                             |                                  |-- verify/read back
       |                             |<-- publish correlated result ----|
       |<-- subscribed result -------|                                  |
```

### Windows PowerShell illustration

```powershell
$Broker = 'mqtt.example.com'
$Prefix = 'inverter'
$DeviceId = 'INV-AABBCCDDEEFF'
$Member = 0
$CommandTopic = "$Prefix/$DeviceId/inverter/$Member/command"
$ResultTopic = "$Prefix/$DeviceId/inverter/$Member/result"

mosquitto_sub.exe -h $Broker -q 1 -v `
  -t "$Prefix/$DeviceId/inverter/+/live" `
  -t "$Prefix/$DeviceId/inverter/+/alert/#" `
  -t "$Prefix/$DeviceId/inverter/+/result" `
  -t "$Prefix/$DeviceId/inverter/+/snapshot/#" `
  -t "$Prefix/$DeviceId/inverter/+/availability"
```

In a second PowerShell window, read battery voltage:

```powershell
$Payload = '{"command_id":"910001","operation":"get","category":"input_data","name":"Vbat"}'
mosquitto_pub.exe -h $Broker -q 1 -t $CommandTopic -m $Payload
```

Enable the buzzer:

```powershell
$Payload = '{"command_id":"920001","operation":"set","name":"DspBeepOnOff","value":true,"confirmed":true}'
mosquitto_pub.exe -h $Broker -q 1 -t $CommandTopic -m $Payload
```

### Linux/macOS shell illustration

Set the connection values and copy the hardware-generated device ID reported by the ESP32:

```bash
BROKER="mqtt.example.com"
PREFIX="inverter"
DEVICE_ID="INV-AABBCCDDEEFF"
MEMBER="0"
COMMAND_TOPIC="$PREFIX/$DEVICE_ID/inverter/$MEMBER/command"
RESULT_TOPIC="$PREFIX/$DEVICE_ID/inverter/$MEMBER/result"
```

Keep all stream subscriptions running:

```bash
mosquitto_sub -h "$BROKER" -q 1 -v \
  -t "$PREFIX/$DEVICE_ID/inverter/+/live" \
  -t "$PREFIX/$DEVICE_ID/inverter/+/alert/#" \
  -t "$PREFIX/$DEVICE_ID/inverter/+/result" \
  -t "$PREFIX/$DEVICE_ID/inverter/+/snapshot/#" \
  -t "$PREFIX/$DEVICE_ID/inverter/+/availability"
```

Read grid voltage:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"910001","operation":"get","category":"input_data","name":"Vac-R"}'
```

Read battery voltage:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"910002","operation":"get","category":"input_data","name":"Vbat"}'
```

Read local battery SOC, distinguished from parallel SOC:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"910003","operation":"get","category":"input_data","name":"SOC","domain":"battery"}'
```

Read PV1 voltage and PV1 power. `Ppv1 H` is a 32-bit point, so the ESP automatically includes its low word:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"910004","operation":"get","category":"input_data","name":"Vpv1"}'

mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"910005","operation":"get","category":"input_data","name":"Ppv1 H"}'
```

Enable the buzzer:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"920001","operation":"set","name":"DspBeepOnOff","value":true,"confirmed":true}'
```

Enable Bluetooth:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"920002","operation":"set","name":"ubBluetoothEn","value":true,"confirmed":true}'
```

Turn the inverter off:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"920003","operation":"set","name":"OnOffSet","value":false,"confirmed":true}'
```

Change battery type only after the inverter reports a fresh standby/stopped state. This example selects documented mode `0`; it is an API illustration, not a battery recommendation:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"920004","operation":"set","name":"BatteryType","value":0,"confirmed":true}'
```

Change a guarded operating-policy setting after validating its dependencies:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"920005","operation":"set","name":"OverloadToBypass","value":true,"confirmed":true,"guarded_interlock":true}'
```

A commissioning-only grid setting additionally requires a fresh standby/stopped inverter and the commissioning interlock:

```bash
mosquitto_pub -h "$BROKER" -q 1 -t "$COMMAND_TOPIC" -m \
'{"command_id":"920006","operation":"set","name":"fGridVoltLow1EE","value":180.0,"confirmed":true,"commissioning_interlock":true}'
```

Typical confirmed setter result:

```json
{
  "member_id": 0,
  "command_id": "920001",
  "operation": "set",
  "outcome": "confirmed",
  "name": "DspBeepOnOff",
  "description": "Buzzer switch",
  "domain": "SYSTEM",
  "write_guard": "HOT_EDIT_CONFIRMED",
  "previous_value": 0,
  "value": 1
}
```

<!-- BEGIN GENERATED COMPLETE COMMAND CATALOG -->
## Complete generated Mosquitto command catalog

This section is generated from the firmware catalog and checked against every writable row of `AOHAI_Modbus_RTU_V2.14_Register_Map.md`. Replace `<broker>`, `<prefix>`, `<device-id>`, and `<member-id>`. Use a fresh positive numeric command ID for each request. Replace `<new-value>` with the intended application value. The detailed setter recipes below include concrete examples, confirmation subscriptions and source limitations. Commands do not contain Modbus addresses.

Add `-u "<username>" -P "<password>"` when authentication is enabled. Subscribe to command results before publishing. Use the `live`, `alert/#`, and `snapshot/#` subscriptions documented above for asynchronous data:

```bash
mosquitto_sub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/result"
```

### GETTERS - read data from the inverter

These public commands contain only an application category and catalog data name. Modbus address, function, scale conversion and word layout remain inside the ESP firmware.

#### Configuration getters (current setup, identity and command state)

| Name | Description | Domain | Type/unit | Read class | Mosquitto getter command |
|---|---|---|---|---|---|
| OnOffSet | Switch on/off | system | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"OnOffSet"}'` |
| SystemSetBit | System Enable Flag Bit | system | unknown | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SystemSetBit"}'` |
| DeviceType | Device Type | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"DeviceType"}'` |
| DeviceID | Device Correspondence Address | communications | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"DeviceID"}'` |
| BaudrateSet | Select Baud Rate | communications | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BaudrateSet"}'` |
| Serial NO.1 | Serial number 1 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.1"}'` |
| Serial NO.2 | Serial number 2 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.2"}'` |
| Serial NO.3 | Serial number 3 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.3"}'` |
| Serial NO.4 | Serial numbe 4 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.4"}'` |
| Serial NO.5 | Serial number 5 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.5"}'` |
| Serial NO.6 | Serial number 6 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.6"}'` |
| Serial NO.7 | Serial numbe 7 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.7"}'` |
| Serial NO.8 | Serial numbe 8 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.8"}'` |
| Serial NO.9 | Serial number 9 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.9"}'` |
| Serial NO.10 | Serial number 10 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.10"}'` |
| Serial NO.11 | Serial number 11 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.11"}'` |
| Serial NO.12 | Serial number 12 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.12"}'` |
| Serial NO.13 | Serial number 13 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.13"}'` |
| Serial NO.14 | Serial number 14 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.14"}'` |
| Serial NO.15 | Serial numbe 15 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Serial NO.15"}'` |
| Rs485WorkMode | 485 Operating Mode Selection | communications | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Rs485WorkMode"}'` |
| Year | System Time - Year | system | uint16; Year | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Year"}'` |
| Month | System Time - Month | system | uint16; Month | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Month"}'` |
| Day | System Time - Day | system | uint16; Day | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Day"}'` |
| Hour | System Time - Hours | system | uint16; Hour | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Hour"}'` |
| Minute | System Time - Minutes | system | uint16; Minutes | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Minute"}'` |
| Second | System Time - Seconds | system | uint16; Seconds | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Second"}'` |
| Weekday | System Week | system | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Weekday"}'` |
| SoftVersion_Attest | Firmware Version (High) | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SoftVersion_Attest"}'` |
| SoftVersion_Monitor | Arm Firmware Version Name | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SoftVersion_Monitor"}'` |
| SoftVersion_Control | DSP Software Version Name (TJ) | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SoftVersion_Control"}'` |
| MasterDSPTestVersion | DSP1 software debug version number | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"MasterDSPTestVersion"}'` |
| SlaveDSPTestVersion | DSP2 software debug version number | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SlaveDSPTestVersion"}'` |
| ARM TestVersion | Arm debug version number | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ARM TestVersion"}'` |
| DSP Hard version | Dashboard Hardware Version | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"DSP Hard version"}'` |
| ARM Hard version | Dashboard Hardware Version | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ARM Hard version"}'` |
| Manufacturer Info 8 | Manufacturer Information 8 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info 8"}'` |
| Manufacturer Info 7 | Manufacturer Information 7 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info 7"}'` |
| Manufacturer Info 6 | Manufacturer Information 6 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info 6"}'` |
| Manufacturer Info 5 | Manufacturer Information 5 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info 5"}'` |
| Manufacturer Info 4 | Manufacturer Information 4 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info 4"}'` |
| Manufacturer Info3 | Manufacturer Information 3 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info3"}'` |
| Manufacturer Info 2 | Manufacturer Info 2 | identity | ascii; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info 2"}'` |
| Manufacturer Info 1 | Manufacturer Info 1 | identity | ascii_or_uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Manufacturer Info 1"}'` |
| ModbusVersion | Modbus Version | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ModbusVersion"}'` |
| Module 4 | Inverter Mode (4) | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Module 4"}'` |
| Module 3 | Inverter Mode (3) | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Module 3"}'` |
| Module 2 | Inverter Mode (2) | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Module 2"}'` |
| Module 1 | Inverter Mode (1) | identity | uint16; - | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Module 1"}'` |
| MaxInvPower | Rated power (high) | identity | uint16; 0.1W | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"MaxInvPower"}'` |
| UpdateState | Firmware update progress | system | uint16; - | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"UpdateState"}'` |
| ubScreenType | Screen Type | system | uint8; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubScreenType"}'` |
| SafetyId | Safety regulation number | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SafetyId"}'` |
| ReactivePowerDelayTIme | Reactive Percent Response Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ReactivePowerDelayTIme"}'` |
| ActiveOverloadEnable | Active Power Overload Enabled | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ActiveOverloadEnable"}'` |
| AcChargePowerRate | Inverter Max Take-Off Power Percentage | grid | uint16; 1%Pn | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"AcChargePowerRate"}'` |
| ActivePowerSlope | Active power rate of change (N4105) | grid | uint16; 0.1Pn/min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ActivePowerSlope"}'` |
| ActivePowerRate | Inverter Max Output Active Power Percentage | grid | uint16; 1%Pn | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ActivePowerRate"}'` |
| ReactivePowerRate | Inverter Max Output Reactive Power Percentage | grid | sint16; 1%Pn | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ReactivePowerRate"}'` |
| PowerFactorSet | Inverter output power factor 10,000 times | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PowerFactorSet"}'` |
| PVModelSelect | MPPT Mode | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PVModelSelect"}'` |
| PvStartVoltage | PV start voltage | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PvStartVoltage"}'` |
| FastDerating_en | Fast Down Enable | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"FastDerating_en"}'` |
| Island_en | Island Enabling | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Island_en"}'` |
| VFRT_en | High and low penetration enable | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"VFRT_en"}'` |
| DRMS_en | DRMS Enabled | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"DRMS_en"}'` |
| NLineConnectMode | N-line enable | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"NLineConnectMode"}'` |
| NToGNDDetect | Zero Earth Detection | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"NToGNDDetect"}'` |
| ZeroPowerOutputEnable | Zero power output enabled | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ZeroPowerOutputEnable"}'` |
| FastMpptEnable | Fast mppt enable | grid | uint16; - | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"FastMpptEnable"}'` |
| CtRatioSet | CT Rheological Ratio Setting | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"CtRatioSet"}'` |
| CTMode | Using CT Mode | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"CTMode"}'` |
| LocalAntiBackflowEnable | Anti-reflux enable | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"LocalAntiBackflowEnable"}'` |
| BackflowMeterPowerLimit_R | Anti-reflux Reflux Power Limit | grid | sint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BackflowMeterPowerLimit_R"}'` |
| BackflowMeterPowerLimit_S | Anti-reflux Reflux Power Limit | grid | sint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BackflowMeterPowerLimit_S"}'` |
| BackflowMeterPowerLimit_T | Anti-reflux Reflux Power Limit | grid | sint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BackflowMeterPowerLimit_T"}'` |
| BackflowHostNoResponseFlag | Anti-reflux host failure flag bit | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BackflowHostNoResponseFlag"}'` |
| BackflowFaultTime | Anti-reflux failure time | grid | uint16; 1s | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BackflowFaultTime"}'` |
| BackflowFaultPowerRate | Percentage of active anti-reflux failure | grid | sint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BackflowFaultPowerRate"}'` |
| Power on config falg | Boot navigation flags | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Power on config falg"}'` |
| Vac low C | Grid Low Voltage Restrictions Connected to the Grid | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Vac low C"}'` |
| Vac high C | Grid High Voltage Restriction Connected to Grid | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Vac high C"}'` |
| Fac low C | Grid Low Frequency Restrictions Connected to the Grid | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Fac low C"}'` |
| Fac high C | Grid High Frequency Restrictions Connected to the Grid | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Fac high C"}'` |
| NominalEPSVolt | Optional rated off-grid voltage | eps | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"NominalEPSVolt"}'` |
| NominalEPSFre | Optional rated off-grid frequency | eps | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"NominalEPSFre"}'` |
| EPS_En | Off-grid functionality enabled | eps | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"EPS_En"}'` |
| Bypass_En | Bypass mode enabled | eps | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Bypass_En"}'` |
| UPS_En | UPS Function Enabled | eps | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"UPS_En"}'` |
| ubN_PE_RelayCMD | Zero earth relay control enabled | eps | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubN_PE_RelayCMD"}'` |
| CheckFanCmd | Fan Detection | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"CheckFanCmd"}'` |
| unAFCICtrlReg | AFCI Self-Test Command | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"unAFCICtrlReg"}'` |
| uwAFCIThresholdValue | AFCI error threshold | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwAFCIThresholdValue"}'` |
| AFCIClrFaultCmd | AFCI Error Clear Command | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"AFCIClrFaultCmd"}'` |
| uwRestartVoltL | Fault reconnect grid low voltage | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwRestartVoltL"}'` |
| uwRestartVoltH | Fault reconnect grid high voltage | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwRestartVoltH"}'` |
| uwRestartFreqL | Fault Reconnect Grid Low Frequency | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwRestartFreqL"}'` |
| uwRestartFreqH | Fault Reconnect Grid High Frequency | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwRestartFreqH"}'` |
| BatteryType | Battery type | battery | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatteryType"}'` |
| BatteryCompanySet | Battery communication protocol selection | bms | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatteryCompanySet"}'` |
| BatMdlSerialNum | Number of batteries in series - high-voltage batteries; | battery | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatMdlSerialNum"}'` |
| BatMdlParallNum | Number of battery parallel joints; | battery | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatMdlParallNum"}'` |
| ChargeCurrentLimit | Charge Limit Current | battery | uint16; 0.01A | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ChargeCurrentLimit"}'` |
| VbatStopForDischarge | Discharge cut-off voltage | battery | uint16; 0.01V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"VbatStopForDischarge"}'` |
| Vbat constant charge | Charge cut-off voltage | battery | uint16; 0.01V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Vbat constant charge"}'` |
| ubHighVoltBatRatedVolt | Battery nominal voltage | battery | uint16; 1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubHighVoltBatRatedVolt"}'` |
| ChargeRate | Charging power | battery | uint16; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ChargeRate","domain":"battery","description":"Charging power"}'` |
| ChargeRate | Discharge power | battery | uint16; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ChargeRate","domain":"battery","description":"Discharge power"}'` |
| BatFirstStopSOC | Charge cut-off SOC | battery | uint16; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatFirstStopSOC"}'` |
| OnLineStopSOC | Grid-connected discharge cut-off SOC | battery | uint16; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"OnLineStopSOC"}'` |
| OffLineStopSoc | Off-grid Discharge Cutoff SOC | battery | uint16; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"OffLineStopSoc"}'` |
| Gen Charge En | Total Diesel Charging Enabled | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Gen Charge En"}'` |
| AcCharge_En | AC Charge Enabled | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"AcCharge_En"}'` |
| LoadFirstDischargeDisableFlag | Load Preferred Discharge Prohibit Sign | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"LoadFirstDischargeDisableFlag"}'` |
| RemotePowerControl | One-click charging and discharging | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"RemotePowerControl"}'` |
| NumberTimePeriods | Number of charging and discharging periods - number of currently valid periods, starting with period 1 | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"NumberTimePeriods"}'` |
| PeriodTimeStart1 | Period 1 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart1"}'` |
| PeriodTimeEnd1 | Period 1 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd1","domain":"system","description":"Period 1 End Time"}'` |
| PeriodTimeRate1 | Period 1 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority"}'` |
| PeriodTimeStart2 | Period 2 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart2"}'` |
| PeriodTimeEnd2 | Period 2 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd2"}'` |
| PeriodTimeRate2 | Period 2 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate2"}'` |
| PeriodTimeStart3 | Period 3 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart3"}'` |
| PeriodTimeEnd3 | Slot 3 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd3"}'` |
| PeriodTimeRate3 | Period 3 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate3"}'` |
| PeriodTimeStart4 | Period 4 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart4"}'` |
| PeriodTimeEnd4 | Period 4 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd4"}'` |
| PeriodTimeRate4 | Period 4 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate4"}'` |
| PeriodTimeStart5 | Period 5 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart5"}'` |
| PeriodTimeEnd5 | Slot 5 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd5"}'` |
| PeriodTimeRate5 | Period 5 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate5"}'` |
| PeriodTimeStart6 | Period 6 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart6"}'` |
| PeriodTimeEnd1 | Slot 6 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd1","domain":"system","description":"Slot 6 End Time"}'` |
| PeriodTimeRate1 | Period 6 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority"}'` |
| PeriodTimeStart7 | Period 7 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart7"}'` |
| PeriodTimeEnd1 | Segment 7 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd1","domain":"system","description":"Segment 7 End Time"}'` |
| PeriodTimeRate1 | Period 7 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority"}'` |
| PeriodTimeStart8 | Period 8 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart8"}'` |
| PeriodTimeEnd1 | Slot 8 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd1","domain":"system","description":"Slot 8 End Time"}'` |
| PeriodTimeRate1 | Period 8 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority"}'` |
| PeriodTimeStart9 | Period 9 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart9"}'` |
| PeriodTimeEnd1 | Period 9 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd1","domain":"system","description":"Period 9 End Time"}'` |
| PeriodTimeRate1 | Period 9 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority"}'` |
| PeriodTimeStart10 | Period 10 Start Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeStart10"}'` |
| PeriodTimeEnd10 | Slot 10 End Time | system | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeEnd10"}'` |
| PeriodTimeRate10 | Period 10 Priority | system | uint16; 1Pn% | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PeriodTimeRate10"}'` |
| ChargeSourcePriority | Charge Priority | system | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ChargeSourcePriority"}'` |
| SourcePriority | Energy Priority | system | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SourcePriority"}'` |
| uwCC_DisChrLead_100T | Lead Acid Discharge Current | battery | uint16; 0.01A | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwCC_DisChrLead_100T"}'` |
| ubOP_RecoverDischargeSOC | Grid-connected stop discharge recovery SOC | battery | uint8; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubOP_RecoverDischargeSOC"}'` |
| ubOffGrid_RecoverDischargeSOC | Off-grid Stop Discharge Recovery SOC | battery | uint8; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubOffGrid_RecoverDischargeSOC"}'` |
| BatUnderVol | Battery Low Voltage Shutdown Voltage | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatUnderVol"}'` |
| AcChargingCurrent | AC Charge Limit Current | battery | uint16; 0.1A | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"AcChargingCurrent"}'` |
| uwFloatV_Lead_100T | Floating Charge Voltage | battery | uint16; 0.01V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwFloatV_Lead_100T"}'` |
| BAT2AC_Volt | Battery-to-market voltage | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BAT2AC_Volt"}'` |
| AC2BAT_Volt | Mains to Battery Voltage | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"AC2BAT_Volt"}'` |
| ubLeadAcid_BatSubType | Subtype of lead-acid battery | battery | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubLeadAcid_BatSubType"}'` |
| IncreChar_MaxTim | Improved charging time | battery | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"IncreChar_MaxTim"}'` |
| BatUnderVolt_Point | Battery undervoltage alarm point | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatUnderVolt_Point"}'` |
| Equalization | Equalization Mode Enabled | battery | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Equalization"}'` |
| EQBatteryTime | Equalize charging time | battery | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"EQBatteryTime"}'` |
| EQBatteryTimeout | Equalize Charge Delay Time | battery | uint16; 1min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"EQBatteryTimeout"}'` |
| EqualizationCycle | Balanced charge interval | battery | uint8; 1DAY | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"EqualizationCycle"}'` |
| EqualizationImmediately | Turn on Balanced Charging now | battery | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"EqualizationImmediately"}'` |
| flcdEn | LCD setting enable bit | system | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"flcdEn"}'` |
| BatLVBreak_RestartVolt | Low Voltage Disconnect Battery Recovery Point | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatLVBreak_RestartVolt"}'` |
| BatNeedChr_Volt | Battery Recharge Recovery Point | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatNeedChr_Volt"}'` |
| NonCriticlLoad_BatDisConVolt | Non-critical load disconnect battery voltage | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"NonCriticlLoad_BatDisConVolt"}'` |
| BatHighVolt_DisConPoint | Overvoltage disconnection voltage | battery | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatHighVolt_DisConPoint"}'` |
| BatOverDisCharge_Delay | Battery Overdischarge Delay Time | battery | uint16; 1s | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"BatOverDisCharge_Delay"}'` |
| DspBeepOnOff | Buzzer switch | system | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"DspBeepOnOff"}'` |
| OverloadToBypass | Overload transfer bypass enable | eps | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"OverloadToBypass"}'` |
| AcInputType | Off-grid output mode | grid | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"AcInputType"}'` |
| EQBatteryVoltage_100T | Balanced Charging Voltage | battery | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"EQBatteryVoltage_100T"}'` |
| Parallel_Mode | Parallel Mode | parallel | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Parallel_Mode"}'` |
| ubParallelDeviveID | Parallel can communication address | parallel | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubParallelDeviveID"}'` |
| ubParallelDeviveType | Parallel Device Type | parallel | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubParallelDeviveType"}'` |
| ubBMSWorkMode | BMS communication method | bms | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubBMSWorkMode"}'` |
| uwGridPowerCompensation | Grid Power Compensation | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"uwGridPowerCompensation"}'` |
| Gen Port Work Mode | Diesel Port Function Selection | generator | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Gen Port Work Mode"}'` |
| Gen Charge Curr Limit | Diesel Charging Current Limit | generator | uint16; 1A | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Gen Charge Curr Limit"}'` |
| Gen Input Rated Power | Generator Input Rated Power | generator | uint16; 10W | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Gen Input Rated Power"}'` |
| SecEPS ON SOC/Vbat | (Lithium) Start SOC | generator | uint16; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SecEPS ON SOC/Vbat"}'` |
| SecEPS ON Vbat | (Lead acid) Starting battery voltage | generator | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SecEPS ON Vbat"}'` |
| SecEPS OFF SOC/Vbat | (Lithium) Shutdown SOC | generator | uint16; 0.01 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SecEPS OFF SOC/Vbat"}'` |
| SecEPS OFF Vbat | (Lead Acid) Shutdown Battery Voltage | generator | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SecEPS OFF Vbat"}'` |
| SecEPS  On PV Power Min | Minimum power of photovoltaic startup smart load | generator | uint16; 10W | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"SecEPS  On PV Power Min"}'` |
| ubBluetoothEn | Bluetooth Enabled | system | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubBluetoothEn"}'` |
| upgrade notification | Data update notification, wifi re-submit 0304 data to the server | communications | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"upgrade notification"}'` |
| Datalogger Restart | wifi factory settings restored, server domain name and escalation time modified | communications | uint8 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Datalogger Restart"}'` |
| ubConnectServer | Collector network status | communications | uint8 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubConnectServer"}'` |
| ubDatalogAndArmCommunication | Collector and inverter communication status | communications | uint8 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"ubDatalogAndArmCommunication"}'` |
| LocalSafetyCmd | User Safety Selection Instructions | grid | uint16 | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"LocalSafetyCmd"}'` |
| fGridVoltLow1EE | Low Grid Voltage Protection Tier 1 | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fGridVoltLow1EE"}'` |
| fGridVoltHigh1EE | High Grid Voltage Protection Tier 1 | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fGridVoltHigh1EE"}'` |
| fFreqLow1EE | Grid Low Frequency Protection First Order | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fFreqLow1EE"}'` |
| fFreqHigh1EE | Grid High Frequency Protection First Order | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fFreqHigh1EE"}'` |
| fGridVoltLow2EE | Low Grid Voltage Protection Level 2 | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fGridVoltLow2EE"}'` |
| fGridVoltHigh2EE | High Grid Voltage Protection Level 2 | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fGridVoltHigh2EE"}'` |
| fFreqLow2EE | Grid Low Frequency Protection Second Order | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fFreqLow2EE"}'` |
| fFreqHigh2EE | Grid High Frequency Protection Second Order | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fFreqHigh2EE"}'` |
| fGridVoltLow3EE | Low Grid Voltage Protection Level 3 | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fGridVoltLow3EE"}'` |
| fGridVoltHigh3EE | High Grid Voltage Protection Level 3 | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fGridVoltHigh3EE"}'` |
| fFreqLow3EE | Grid Low Frequency Protection Level 3 | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fFreqLow3EE"}'` |
| fFreqHigh3EE | Grid High Frequency Protection Level 3 | grid | uint16; 0.01Hz | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"fFreqHigh3EE"}'` |
| wVLowCutTime1EE | Low Grid Voltage First Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"wVLowCutTime1EE"}'` |
| wVHighCutTime1EE | High grid voltage first-order protection time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"wVHighCutTime1EE"}'` |
| udFLowCutTime1EE | Low Grid Frequency First Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"udFLowCutTime1EE"}'` |
| udFHighCutTime1EE | High Grid Frequency First Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"udFHighCutTime1EE"}'` |
| wVLowCutTime2EE | Low Grid Voltage Second Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"wVLowCutTime2EE"}'` |
| wVHighCutTime2EE | High Grid Voltage Second Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"wVHighCutTime2EE"}'` |
| udFLowCutTime2EE | Low Grid Frequency Second Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"udFLowCutTime2EE"}'` |
| udFHighCutTime2EE | High Grid Frequency Second Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"udFHighCutTime2EE"}'` |
| wVLowCutTime3EE | Low grid voltage third-order protection time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"wVLowCutTime3EE"}'` |
| wVHighCutTime3EE | High grid voltage third-order protection time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"wVHighCutTime3EE"}'` |
| udFLowCutTime3EE | Low Grid Frequency Third Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"udFLowCutTime3EE"}'` |
| udFHighCutTime3EE | High Grid Frequency Third Order Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"udFHighCutTime3EE"}'` |
| 10MinAVLimit | Voltage protection for ten minutes | grid | uint16; 0.1V | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"10MinAVLimit"}'` |
| U10minTime | 10 Minute Average Voltage Protection Time | grid | uint16; 20ms | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"U10minTime"}'` |
| Time start | Connection time | grid | uint16; 1s | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"Time start"}'` |
| RestartDelayTime | Reconnection time | grid | uint16; 1s | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"RestartDelayTime"}'` |
| PowerStartSlope | Load rate | grid | uint16; 0.1Pn%/min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PowerStartSlope"}'` |
| PowerRestartSlopeEE | Restart Load Rate | grid | uint16; 0.1Pn%/min | setup_snapshot | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"configuration","name":"PowerRestartSlopeEE"}'` |

#### Input-register getters (status, measurements, power, energy and faults)

| Name | Description | Domain | Type/unit | Read class | Mosquitto getter command |
|---|---|---|---|---|---|
| Inverter Status | Inverter operating status | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Inverter Status"}'` |
| StartDelayTime | Grid-connected countdown | inverter | uint16; 1s | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"StartDelayTime"}'` |
| INV_VolR | Inverter Voltage | inverter | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"INV_VolR"}'` |
| INV_VolS | Inverter Voltage | inverter | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"INV_VolS"}'` |
| INV_VolT | Inverter Voltage | inverter | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"INV_VolT"}'` |
| INV_CurrR | Inverter Current | inverter | sint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"INV_CurrR"}'` |
| INV_CurrS | Inverter Current | inverter | sint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"INV_CurrS"}'` |
| INV_CurrT | Inverter Current | inverter | sint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"INV_CurrT"}'` |
| Bus1 Voltage | Bus1 Internal Voltage | inverter | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Bus1 Voltage","domain":"inverter"}'` |
| Bus2 Voltage | Bus2 Internal Voltage | inverter | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Bus2 Voltage"}'` |
| Inv_Temp | Inverter Temperature | inverter | sint16; 0.1C deg | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Inv_Temp"}'` |
| Boost_Temp | Inverter Internal IPM Temperature | inverter | sint16; 0.1C deg | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Boost_Temp"}'` |
| LLC _Temp | LLC Radiator Temperature | inverter | sint16; 0.1C deg | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"LLC _Temp"}'` |
| Bat_Temp | Battery temperature | inverter | sint16; 0.1C deg | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Bat_Temp"}'` |
| TA_Temp | Ambient temperature | inverter | sint16; 0.1C deg | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"TA_Temp"}'` |
| DCV-R | R-phase DC voltage component | inverter | sint16; 1mV | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DCV-R"}'` |
| DCV-S | S-phase DC voltage component | inverter | sint16; 1mV | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DCV-S"}'` |
| DCV-T | T-phase DC voltage component | inverter | sint16; 1mV | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DCV-T"}'` |
| DCI-R | R-phase DC current component | inverter | sint16; 1mA | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DCI-R"}'` |
| DCI-S | S-phase DC current component | inverter | sint16; 1mA | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DCI-S"}'` |
| DCI-T | T-phase DC current component | inverter | sint16; 1mA | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DCI-T"}'` |
| ISO Resistance | ISO Resistance | inverter | uint16; 1kohm | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ISO Resistance"}'` |
| GFCI | Leakage current | inverter | uint16; 1mA | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"GFCI"}'` |
| HistoryEventCnt | Number of historical event records | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"HistoryEventCnt"}'` |
| Systemfault word0 | System Failure word0 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word0"}'` |
| Systemfault word1 | System fault word1 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word1"}'` |
| Systemfault word2 | System fault word2 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word2"}'` |
| Systemfault word3 | System failure word3 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word3"}'` |
| Systemfault word4 | System failure word4 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word4"}'` |
| Systemfault word5 | System Failure word5 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word5"}'` |
| Systemfault word6 | System Failure word6 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word6"}'` |
| Systemfault word7 | System Failure word7 | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Systemfault word7"}'` |
| InvMainErrorCode | Inverter Master Fault Code | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"InvMainErrorCode"}'` |
| InvMainWarnCode | Inverter Main Warning Code | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"InvMainWarnCode"}'` |
| InvErrorSubCode | Inverter sub-fault code | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"InvErrorSubCode"}'` |
| InvWarnSubCode | Inverter sub-warning code | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"InvWarnSubCode"}'` |
| DeviceType | Device Type | inverter | uint16 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DeviceType"}'` |
| DeratingModeFlag | Download Mode Flag | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DeratingModeFlag"}'` |
| PowerCosFlag | Leading Lag Flag | inverter | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PowerCosFlag"}'` |
| Bus1 Voltage | Positive Bus Voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Bus1 Voltage","domain":"grid","description":"Positive Bus Voltage"}'` |
| Bus1 Voltage | Negative Bus Voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Bus1 Voltage","domain":"grid","description":"Negative Bus Voltage"}'` |
| Vac-R | Three Phase Grid Voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vac-R"}'` |
| Iac-R | Three-phase grid output current | grid | sint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Iac-R"}'` |
| Vac-S | Three Phase Grid Voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vac-S"}'` |
| Iac-S | Three-phase grid output current | grid | sint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Iac-S"}'` |
| Vac-T | Three Phase Grid Voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vac-T"}'` |
| Iac-T | Three-phase grid output current | grid | sint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Iac-T"}'` |
| Vac_RS | Three-phase grid line voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vac_RS"}'` |
| Vac_ST | Three-phase grid line voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vac_ST"}'` |
| Vac_TR | Three-phase grid line voltage | grid | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vac_TR"}'` |
| Fac | Grid Frequency | grid | uint16; 0.01Hz | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Fac"}'` |
| PF | Power Factor | grid | sint16; 1E-4 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PF"}'` |
| RealOPPercent | Actual Output Power Percentage | grid | uint16; 0.01 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"RealOPPercent"}'` |
| EPS Fac | Off-grid frequency | eps | uint16; 0.01Hz | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Fac"}'` |
| EPS Vac1 | Off-grid R phase output voltage | eps | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Vac1"}'` |
| EPS Iac1 | Off-grid R phase output current | eps | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Iac1"}'` |
| EPS Vac2 | Off-grid S phase output voltage | eps | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Vac2"}'` |
| EPS Iac2 | Off-grid S phase output current | eps | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Iac2"}'` |
| EPS Vac3 | Off-grid T-phase output voltage | eps | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Vac3"}'` |
| EPS Iac3 | Off-grid T-phase output current | eps | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Iac3"}'` |
| PvNum | PV Paths | pv | uint16 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PvNum"}'` |
| Vpv1 | PV1 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv1"}'` |
| PV1Curr | PV1 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV1Curr"}'` |
| Vpv2 | PV2 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv2"}'` |
| PV2Curr | PV2 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV2Curr"}'` |
| Vpv3 | PV3 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv3"}'` |
| PV3Curr | PV3 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV3Curr"}'` |
| Vpv4 | PV4 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv4"}'` |
| PV4Curr | PV4 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV4Curr"}'` |
| Vpv5 | PV5 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv5"}'` |
| PV5Curr | PV5 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV5Curr"}'` |
| Vpv6 | PV6 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv6"}'` |
| PV6Curr | PV6 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV6Curr"}'` |
| Vpv7 | PV7 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv7"}'` |
| PV7Curr | PV7 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV7Curr"}'` |
| Vpv8 | PV8 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv8"}'` |
| PV8Curr | PV8 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV8Curr"}'` |
| Vpv9 | PV9 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv9"}'` |
| PV9Curr | PV9 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV9Curr"}'` |
| Vpv10 | PV10 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv10"}'` |
| PV10Curr | PV10 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV10Curr"}'` |
| Vpv11 | PV11 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv11"}'` |
| PV11Curr | PV11 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV11Curr"}'` |
| Vpv12 | PV12 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv12"}'` |
| PV12Curr | PV12 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV12Curr"}'` |
| Vpv13 | PV13 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv13"}'` |
| PV13Curr | PV13 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV13Curr"}'` |
| Vpv14 | PV14 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv14"}'` |
| PV14Curr | PV14 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV14Curr"}'` |
| Vpv15 | PV15 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv15"}'` |
| PV15Curr | PV15 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV15Curr"}'` |
| Vpv16 | PV16 Voltage | pv | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vpv16"}'` |
| PV16Curr | PV16 Input Current | pv | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PV16Curr"}'` |
| uwGEN_v_R | Diesel generator voltage R | generator | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwGEN_v_R"}'` |
| uwGEN_i_R | Diesel generator current R | generator | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwGEN_i_R"}'` |
| uwGEN_v_S | Diesel generator voltage S | generator | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwGEN_v_S"}'` |
| uwGEN_i_S | Diesel generator current S | generator | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwGEN_i_S"}'` |
| uwGEN_v_T | Diesel generator voltage T | generator | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwGEN_v_T"}'` |
| uwGEN_i_T | Diesel generator current T | generator | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwGEN_i_T"}'` |
| uwGEN_Freq | Diesel Generator Frequency | generator | uint16; 0.01Hz | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwGEN_Freq"}'` |
| fan_speed | Fan speed | system | uint16; rpm | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"fan_speed"}'` |
| Time total H | Total working hours | system | uint32; 0.5min | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Time total H"}'` |
| Debuge1 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge1"}'` |
| Debuge2 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge2"}'` |
| Debuge3 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge3"}'` |
| Debuge4 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge4"}'` |
| Debuge5 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge5"}'` |
| Debuge6 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge6"}'` |
| Debuge7 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge7"}'` |
| Debuge8 | Main DSP debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge8"}'` |
| Debuge9 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge9"}'` |
| Debuge10 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge10"}'` |
| Debuge11 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge11"}'` |
| Debuge12 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge12"}'` |
| Debuge13 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge13"}'` |
| Debuge14 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge14"}'` |
| Debuge15 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge15"}'` |
| Debuge16 | Debug parameters from DSP | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Debuge16"}'` |
| Priority | Current Battery Priority | battery | uint16 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Priority"}'` |
| Battery Type | Battery type | battery | uint16 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Battery Type"}'` |
| Vbat | Battery Voltage | battery | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Vbat"}'` |
| SOC | Battery SOC | battery | uint16; 0.01 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"SOC","domain":"battery"}'` |
| BatVolt_DSP | DSP Sampling Battery Voltage | battery | uint16; 0.1V | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatVolt_DSP"}'` |
| BMS_Status | Battery status | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_Status"}'` |
| BMS_Error1 | Battery error message 1 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_Error1"}'` |
| BMS_Error2 | Battery error message 2 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_Error2"}'` |
| BMS_Error3 | Battery error message 3 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_Error3"}'` |
| BMS_Error4 | Battery error message 4 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_Error4"}'` |
| BMS_WarnInfo1 | Battery Alarm 1 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_WarnInfo1"}'` |
| BMS_WarnInfo2 | Battery Alarm 2 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_WarnInfo2"}'` |
| BMS_WarnInfo3 | Battery Alarm 3 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_WarnInfo3"}'` |
| BMS_WarnInfo4 | Battery Alarm 4 | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_WarnInfo4"}'` |
| BMS_BatteryCurr | Battery current | bms | sint16; 0.01A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_BatteryCurr"}'` |
| BMS_BatteryTemp | Battery temperature | bms | sint16; 0.1C deg | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_BatteryTemp"}'` |
| BMS_MaxChargeCurr | Maximum allowable charging current of the battery | bms | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_MaxChargeCurr"}'` |
| BMS_MaxDischargeCurr | Maximum allowable discharge current of the battery | bms | uint16; 0.1A | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_MaxDischargeCurr"}'` |
| BMS_GaugeFCC | Battery rated capacity | bms | uint16; 0.1Ah | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_GaugeFCC"}'` |
| BMS_GaugeRM | Real-Time Battery Capacity | bms | uint16; 0.1Ah | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_GaugeRM"}'` |
| BMS_SoftVersion_Major | Battery Upload Software Major Version | bms | uint16 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_SoftVersion_Major"}'` |
| BMS_SoftVersion_Minor | Battery Upload Software Minor Version | bms | uint16 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_SoftVersion_Minor"}'` |
| BMS_HardVersion | Battery Upload Hardware Version | bms | uint16 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_HardVersion"}'` |
| BMS_DeltaVolt | Battery unit pressure difference | bms | uint16; 0.001V | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_DeltaVolt"}'` |
| BMS_CycleCnt | Battery Cycles | bms | uint16 | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_CycleCnt"}'` |
| BMS_SOH | SOH | bms | uint16 | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_SOH"}'` |
| BMS_ConstantVolt | Battery Recommended Charging Voltage | bms | uint16; 0.1V | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_ConstantVolt"}'` |
| uwLVVoltage_Pack | LV Voltage | bms | uint16; 0.1V | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwLVVoltage_Pack"}'` |
| BMS_BMSInfo | BMS Information | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_BMSInfo"}'` |
| BMS_PackInfo | Pack Info | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMS_PackInfo"}'` |
| MaxCellVol | Battery maximum cell voltage | bms | uint16; 0.001V | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MaxCellVol"}'` |
| MinCellVol | Battery minimum cell voltage | bms | uint16; 0.001V | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MinCellVol"}'` |
| ModuleNum | Number of batteries in parallel | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ModuleNum"}'` |
| CellNum | Number of battery cells | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"CellNum"}'` |
| MaxVoltCellNo | Highest voltage unit number | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MaxVoltCellNo"}'` |
| MinVoltCellNo | Minimum Voltage Cell Number | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MinVoltCellNo"}'` |
| MaxTemprCell_10T | Maximum monomer temperature | bms | sint16; 0.1C deg | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MaxTemprCell_10T"}'` |
| MinTemprCell_10T | Minimum monomer temperature | bms | sint16; 0.1C deg | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MinTemprCell_10T"}'` |
| MaxTemprCellNo | Maximum Voltage Temperature Number | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MaxTemprCellNo"}'` |
| MinTemprCellNo | Minimum Voltage Temperature Number | bms | uint16 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MinTemprCellNo"}'` |
| Protect pack ID | Faulty Battery Address | bms | uint16 | critical_status | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Protect pack ID"}'` |
| MaxSOC | Parallel Maximum SOC | bms | uint16; 0.01 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MaxSOC"}'` |
| MinSOC | Parallel Minimum SOC | bms | uint16; 0.01 | live_health | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"MinSOC"}'` |
| BMSCompany | Battery manufacturer information | bms | uint16 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BMSCompany"}'` |
| PowerPackSn | Throughput Display Battery Pack Number | battery | uint16 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PowerPackSn"}'` |
| DisChargPower H | Cumulative discharge | battery | uint32; 0.1kwh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"DisChargPower H"}'` |
| ChargPower H | Cumulative Charges | battery | uint32; 0.1kwh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ChargPower H"}'` |
| BatDebuge1 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge1"}'` |
| BatDebuge2 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge2"}'` |
| BatDebuge3 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge3"}'` |
| BatDebuge4 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge4"}'` |
| BatDebuge5 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge5"}'` |
| BatDebuge6 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge6"}'` |
| BatDebuge7 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge7"}'` |
| BatDebuge8 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge8"}'` |
| BatDebuge9 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge9"}'` |
| BatDebuge10 | Battery BMS debug parameters | diagnostics | uint16 | diagnostic | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"BatDebuge10"}'` |
| PpvAll H | PV Total Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PpvAll H"}'` |
| Ppv1 H | PV1 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv1 H"}'` |
| Ppv2 H | PV2 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv2 H"}'` |
| Ppv3 H | PV3 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv3 H"}'` |
| Ppv4 H | PV4 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv4 H"}'` |
| Ppv5 H | PV5 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv5 H"}'` |
| Ppv6 H | PV6 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv6 H"}'` |
| Ppv7 H | PV7 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv7 H"}'` |
| Ppv8 H | PV8 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv8 H"}'` |
| Ppv9 H | PV9 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv9 H"}'` |
| Ppv10 H | PV10 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv10 H"}'` |
| Ppv11 H | PV11 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv11 H"}'` |
| Ppv12 H | PV12 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv12 H"}'` |
| Ppv13 H | PV13 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv13 H"}'` |
| Ppv14 H | PV14 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv14 H"}'` |
| Ppv15 H | PV15 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv15 H"}'` |
| Ppv16 H | PV16 Input Power | pv | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Ppv16 H"}'` |
| SPacAll H | Three-phase output apparent power all | power | uint32; 0.1VA | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"SPacAll H"}'` |
| ActPacAll H | Three-phase output power all | power | sint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ActPacAll H"}'` |
| ReActPacAll H | Three-phase output reactive power all | power | sint32; 0.1var | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ReActPacAll H"}'` |
| SPac_R H | Three-phase output apparent power R | power | uint32; 0.1VA | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"SPac_R H"}'` |
| ActPac_R H | Three-phase output power R | power | sint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ActPac_R H"}'` |
| ReActPac_R H | Three-phase output reactive power R | power | sint32; 0.1var | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ReActPac_R H"}'` |
| SPac_S H | Three-phase output apparent power S | power | uint32; 0.1VA | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"SPac_S H"}'` |
| ActPac_S H | Three-phase output power S | power | sint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ActPac_S H"}'` |
| ReActPac_S H | Three-phase output reactive power S | power | sint32; 0.1var | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ReActPac_S H"}'` |
| SPac_T H | Three-phase output apparent power T | power | uint32; 0.1VA | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"SPac_T H"}'` |
| ActPac_T H | Three-phase output power T | power | sint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ActPac_T H"}'` |
| ReActPac_T H | Three-phase output reactive power T | power | sint32; 0.1var | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ReActPac_T H"}'` |
| Pactouser R   H | R phase to user power | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pactouser R   H"}'` |
| Pactouser S   H | S phase to user power | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pactouser S   H"}'` |
| Pactouser T   H | T phase to user power | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pactouser T   H"}'` |
| PactouserTotal H | Total AC power to user | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PactouserTotal H"}'` |
| Pac to grid R  H | AC Side to Grid Power R | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pac to grid R  H"}'` |
| Pactogrid S  H | AC Side to Grid Power S | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pactogrid S  H"}'` |
| Pactogrid T H | AC Side to Grid Power T | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pactogrid T H"}'` |
| Pactogrid total H | Total AC Side to Grid Power | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pactogrid total H"}'` |
| PLocalLoad total H | Inverter power to local load total | load | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PLocalLoad total H"}'` |
| EPS Pac_R H | Off-grid R phase output power | power | uint32; 0.1VA | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Pac_R H"}'` |
| EPS ActPac_R H | Off-grid R phase output active power | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS ActPac_R H"}'` |
| EPS Pac_S H | Off-grid S phase output power | power | uint32; 0.1VA | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Pac_S H"}'` |
| EPS ActPac_S H | Off-grid S phase output active power | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS ActPac_S H"}'` |
| EPS Pac_T H | Off-grid T-phase output power | power | uint32; 0.1VA | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS Pac_T H"}'` |
| EPS ActPac_T H | Off-grid T-phase output active power | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPS ActPac_T H"}'` |
| Loadpercent | Load percentage | power | uint16; 0.01 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Loadpercent"}'` |
| PSystem H | System Power Generation | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PSystem H"}'` |
| PSelf H | Spontaneous self-consumption power | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"PSelf H"}'` |
| Pdischarge H | Discharge power | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pdischarge H"}'` |
| Pcharge H | Charging power | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Pcharge H"}'` |
| AC charge Power_H | AC charging power | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"AC charge Power_H"}'` |
| Extra AC Power to  grid_H | Additional inverter AC power to grid H | power | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Extra AC Power to  grid_H"}'` |
| udGEN_ApparentP_R   H | Diesel generator R apparent power | generator | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udGEN_ApparentP_R   H"}'` |
| udGEN_ApparentP_S   H | Diesel generator S-phase apparent power | generator | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udGEN_ApparentP_S   H"}'` |
| udGEN_ApparentP_T   H | Diesel generator T-phase apparent power | generator | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udGEN_ApparentP_T   H"}'` |
| udGEN_ActiveP_R   H | Diesel Generator R Phase Active Power | generator | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udGEN_ActiveP_R   H"}'` |
| udGEN_ActiveP_S   H | Diesel Generator S Phase Active Power | generator | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udGEN_ActiveP_S   H"}'` |
| udGEN_ActiveP_T   H | Diesel Generator T-Phase Active Power | generator | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udGEN_ActiveP_T   H"}'` |
| Eactoday H | Daily Power Generation | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eactoday H"}'` |
| Eac total H | Total Power Generation | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eac total H"}'` |
| EPVAll_Today H | PV Daily Power Generation | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EPVAll_Today H"}'` |
| Epv_total H | PV Total Energy | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Epv_total H"}'` |
| EChargeToday H | Daily Charge | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EChargeToday H"}'` |
| EChargeTotal H | Total Charges | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EChargeTotal H"}'` |
| EDischargeToday H | Daily Discharge Volume | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EDischargeToday H"}'` |
| EDischargeTotal H | Total Discharge | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EDischargeTotal H"}'` |
| EACharge_Today_H | AC Daily Charge | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EACharge_Today_H"}'` |
| EACharge_Total_H | Total AC Charging | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EACharge_Total_H"}'` |
| Eextra_today H | External grid-connected inversion energy on the same day | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eextra_today H"}'` |
| Eextra_total H | Total external grid-connected inverter energy | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eextra_total H"}'` |
| Esystem_today H | System Daily Power Generation | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Esystem_today H"}'` |
| Esystem_total H | Total system power generation | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Esystem_total H"}'` |
| Eself_today H | Spontaneous Daily Electricity Generation | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eself_today H"}'` |
| Eself_total H | Total spontaneous self-consumption | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eself_total H"}'` |
| Eload_today H | Load Power Consumption Day | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eload_today H"}'` |
| Eload_total H | Total Power Consumed by Load | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"Eload_total H"}'` |
| EtoGrid_today H | Infeed Grid Battery Days | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EtoGrid_today H"}'` |
| EtoGrid_total H | Total power fed into the grid | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EtoGrid_total H"}'` |
| EfromGrid_today H | Grid Intake Day | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EfromGrid_today H"}'` |
| EfromGrid_total H | Total grid intake | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"EfromGrid_total H"}'` |
| dEpvToGridTodayEE | Electricity on the Internet (electricity sold) day | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"dEpvToGridTodayEE","domain":"energy","description":"Electricity on the Internet (electricity sold) day"}'` |
| dEpvToGridTotalEE | Total on-line electricity (electricity sold) | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"dEpvToGridTotalEE","domain":"energy","description":"Total on-line electricity (electricity sold)"}'` |
| dEGridToLoadTodayEE | Date of Purchase of Electricity (Buy Electricity) | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"dEGridToLoadTodayEE","domain":"energy","description":"Date of Purchase of Electricity (Buy Electricity)"}'` |
| dEGridToLoadTotalEE | Total Electricity Purchased (Bought) | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"dEGridToLoadTotalEE","domain":"energy","description":"Total Electricity Purchased (Bought)"}'` |
| dESelfToLoadTodayEE | Self-Sufficiency Battery Day | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"dESelfToLoadTodayEE","domain":"energy","description":"Self-Sufficiency Battery Day"}'` |
| dESelfToLoadTotalEE | Total Self-Sufficiency Battery | energy | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"dESelfToLoadTotalEE","domain":"energy","description":"Total Self-Sufficiency Battery"}'` |
| uwParallelType | Parallel Type | parallel | uint16 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"uwParallelType"}'` |
| HostSerialNum5 | Host serial number 9 ~ 10 characters, indicating the year of machine production | parallel | ascii | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"HostSerialNum5"}'` |
| HostSerialNum6 | Host serial number 11-12 characters, indicating the week in which the machine was manufactured | parallel | ascii | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"HostSerialNum6"}'` |
| HostSerialNum7 | Host Serial Number 13-14 characters | parallel | ascii | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"HostSerialNum7"}'` |
| HostSerialNum8 | Host Serial Number 15-16 characters | parallel | ascii | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"HostSerialNum8"}'` |
| ubParallelDeviceID | Parallel Device ID | parallel | uint8 | boot_identity | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"ubParallelDeviceID"}'` |
| udwParallelPVPower-H | Total Parallel PV Power | parallel | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallelPVPower-H"}'` |
| sdwParallelGridPower-H | Total power of parallel electromechanical grids | parallel | sint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"sdwParallelGridPower-H"}'` |
| udwParallelLoadPower-H | Total Parallel Load Power | parallel | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallelLoadPower-H"}'` |
| sdwParallelBatPower-H | Total Parallel Battery Power | parallel | sint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"sdwParallelBatPower-H"}'` |
| udwParallelSelfPower-H | Parallel machine self-consumption power | parallel | uint32; 0.1W | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallelSelfPower-H"}'` |
| udwParallel_EPVToady_H | Parallel PV Daily Power Generation | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EPVToady_H"}'` |
| udwParallel_EPVTotal_H | Shutdown PV Total Energy | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EPVTotal_H"}'` |
| udwParallel_ESelfToday_H | Parallel machine spontaneous daily power generation | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_ESelfToday_H"}'` |
| udwParallel_ESelfTotal_H | Total amount of electricity generated by the parallel machine itself | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_ESelfTotal_H"}'` |
| udwParallel_ELoadToday_H | Parallel Load Power Consumption Day | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_ELoadToday_H","domain":"parallel","description":"Parallel Load Power Consumption Day"}'` |
| udwParallel_ELoadToday_H | Total power consumption of parallel loads | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_ELoadToday_H","domain":"parallel","description":"Total power consumption of parallel loads"}'` |
| udwParallel_EPVtoGridToday_H | On-line electricity consumption (electricity sold) days | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EPVtoGridToday_H"}'` |
| udwParallel_EPVtoGridTotal_H | Total on-line electricity (electricity sold) | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EPVtoGridTotal_H"}'` |
| udwParallel_EGridtoLoadToday_H | Parallel Purchase of Electricity (Purchase of Electricity) Day | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EGridtoLoadToday_H"}'` |
| udwParallel_EGridtoLoadTotal_H | Total amount of electricity purchased (purchased) in parallel machines | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EGridtoLoadTotal_H"}'` |
| udwParallel_ESelftoLoadToday_H | Parallel Self-Sufficiency Battery Day | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_ESelftoLoadToday_H"}'` |
| udwParallel_ESelftoLoadTotal_H | Parallel Self-Sufficiency Total Power | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_ESelftoLoadTotal_H"}'` |
| SOC | Battery SOC | parallel | uint16; 0.01 | live_flow | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"SOC","domain":"parallel"}'` |
| udwParallel_EBatChrToday_H | Parallel Battery Charge Battery Day | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EBatChrToday_H"}'` |
| udwParallel_EBatChrTotal_H | Total charge amount of parallel battery | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EBatChrTotal_H"}'` |
| udwParallel_EBatDisChrToday_H | Parallel battery discharge amount day | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EBatDisChrToday_H"}'` |
| udwParallel_EBatDisChrTotal_H | Total discharge capacity of parallel battery | parallel | uint32; 0.1kWh | slow_counter | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"get","category":"input_data","name":"udwParallel_EBatDisChrTotal_H"}'` |

### SETTERS - write settings or commands to the inverter (201)

These commands reach the common setter only after MQTT-origin validation. The ESP resolves the name, converts the application value, reads the current inverter value internally, checks operating state and interlocks, writes the complete value, and verifies it by read-back. Never retry a `service_only` command automatically.

| Setter guard | Count | Required behavior |
|---|---:|---|
| hot_edit_confirmed | 2 | Confirmed internal read-before-write and read-back. |
| hot_edit_guarded | 51 | Also requires guarded_interlock and validated dependencies. |
| runtime_command | 2 | Fresh valid runtime state; transition/status monitoring. |
| standby_required | 47 | Fresh standby or stopped state. |
| commissioning_only | 71 | Fresh standby/stopped state and commissioning_interlock. |
| service_only | 28 | Fresh standby/stopped state and service_interlock; never auto-retry. |

| Setting | Description | Domain | Application value type/unit | Guard | Documented meaning | Mosquitto setter command template |
|---|---|---|---|---|---|---|
| OnOffSet | Switch on/off | system | uint16 | runtime_command | On: 1; Off: 0; Restart: 0xA5 (TBD) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OnOffSet","value":"<new-value>","confirmed":true}'` |
| DeviceID | Device Correspondence Address | communications | uint16 | commissioning_only | Communication ID | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"DeviceID","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| BaudrateSet | Select Baud Rate | communications | uint16 | commissioning_only | 0:9600bps; 1:38400bps | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BaudrateSet","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Serial NO.1 | Serial number 1 | identity | ascii | service_only | Product serial number, valid minimum 16 digits | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.1","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.2 | Serial number 2 | identity | ascii | service_only | Serial number 2 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.2","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.3 | Serial number 3 | identity | ascii | service_only | Serial number 3 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.3","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.4 | Serial numbe 4 | identity | ascii | service_only | Serial numbe 4 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.4","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.5 | Serial number 5 | identity | ascii | service_only | Serial number 5 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.5","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.6 | Serial number 6 | identity | ascii | service_only | Serial number 6 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.6","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.7 | Serial numbe 7 | identity | ascii | service_only | Serial numbe 7 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.7","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.8 | Serial numbe 8 | identity | ascii | service_only | Serial numbe 8 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.8","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.9 | Serial number 9 | identity | ascii | service_only | Serial number 9 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.9","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.10 | Serial number 10 | identity | ascii | service_only | Serial number 10 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.10","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.11 | Serial number 11 | identity | ascii | service_only | Serial number 11 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.11","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.12 | Serial number 12 | identity | ascii | service_only | Serial number 12 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.12","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.13 | Serial number 13 | identity | ascii | service_only | Serial number 13 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.13","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.14 | Serial number 14 | identity | ascii | service_only | Serial number 14 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.14","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Serial NO.15 | Serial numbe 15 | identity | ascii | service_only | Serial numbe 15 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Serial NO.15","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Rs485WorkMode | 485 Operating Mode Selection | communications | uint16 | commissioning_only | 0: Slave mode (ate); 1: Host mode (BMS485); 2: Host mode (Meter485) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Rs485WorkMode","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Year | System Time - Year | system | uint16; Year | hot_edit_guarded | System Time - Year | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Year","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Month | System Time - Month | system | uint16; Month | hot_edit_guarded | System Time - Month | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Month","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Day | System Time - Day | system | uint16; Day | hot_edit_guarded | System Time - Day | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Day","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Hour | System Time - Hours | system | uint16; Hour | hot_edit_guarded | System Time - Hours | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Hour","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Minute | System Time - Minutes | system | uint16; Minutes | hot_edit_guarded | System Time - Minutes | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Minute","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Second | System Time - Seconds | system | uint16; Seconds | hot_edit_guarded | Set time to seconds to take effect | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Second","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Weekday | System Week | system | uint16 | hot_edit_guarded | System Week | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Weekday","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Manufacturer Info 1 | Manufacturer Info 1 | identity | ascii_or_uint16 | service_only | Optional vendor name when configuring | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Manufacturer Info 1","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Module 4 | Inverter Mode (4) | identity | uint16 | service_only | Inverter Mode (4) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Module 4","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Module 3 | Inverter Mode (3) | identity | uint16 | service_only | Inverter Mode (3) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Module 3","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Module 2 | Inverter Mode (2) | identity | uint16 | service_only | Inverter Mode (2) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Module 2","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Module 1 | Inverter Mode (1) | identity | uint16 | service_only | Inverter Mode (1) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Module 1","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Reset to factory | Factory reset | system | uint16 | service_only | 0x01: Restore user data; 0xA0: Restore factory settings | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Reset to factory","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| UpdateFileType | Upgrade File Type | system | uint16 | service_only | 0x01: bin upgrade; 0x10: Hex upgrade | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"UpdateFileType","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| ubScreenType | Screen Type | system | uint8 | commissioning_only | 0: Small screen; 1: Large screen | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubScreenType","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| SafetyId | Safety regulation number | grid | uint16 | commissioning_only | Safety regulation number | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SafetyId","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| ReactivePowerDelayTIme | Reactive Percent Response Time | grid | uint16; 20ms | commissioning_only | Reactive Percent Response Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ReactivePowerDelayTIme","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| ActiveOverloadEnable | Active Power Overload Enabled | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ActiveOverloadEnable","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| AcChargePowerRate | Inverter Max Take-Off Power Percentage | grid | uint16; 1%Pn | commissioning_only | Grid Intake | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcChargePowerRate","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| ActivePowerSlope | Active power rate of change (N4105) | grid | uint16; 0.1Pn/min | commissioning_only | 0: Not Enabled; Other: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ActivePowerSlope","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| ActivePowerRate | Inverter Max Output Active Power Percentage | grid | uint16; 1%Pn | commissioning_only | Power Generation to the Grid | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ActivePowerRate","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| ReactivePowerRate | Inverter Max Output Reactive Power Percentage | grid | sint16; 1%Pn | commissioning_only | Inverter Max Output Reactive Power Percentage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ReactivePowerRate","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| PowerFactorSet | Inverter output power factor 10,000 times | grid | uint16 | commissioning_only | PF=(X-10000)/10000 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PowerFactorSet","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| PVModelSelect | MPPT Mode | grid | uint16 | commissioning_only | 0: Independent MPPT mode; 1: DC source mode; 2: Parallel MPPT mode | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PVModelSelect","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| PvStartVoltage | PV start voltage | grid | uint16; 0.1V | commissioning_only | PV start voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PvStartVoltage","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| FastDerating_en | Fast Down Enable | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"FastDerating_en","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Island_en | Island Enabling | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Island_en","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| VFRT_en | High and low penetration enable | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"VFRT_en","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| DRMS_en | DRMS Enabled | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"DRMS_en","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| NLineConnectMode | N-line enable | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NLineConnectMode","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| NToGNDDetect | Zero Earth Detection | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NToGNDDetect","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| ZeroPowerOutputEnable | Zero power output enabled | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ZeroPowerOutputEnable","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| FastMpptEnable | Fast mppt enable | grid | uint16 | commissioning_only | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"FastMpptEnable","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| CtRatioSet | CT Rheological Ratio Setting | grid | uint16 | commissioning_only | Reserved | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"CtRatioSet","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| CTMode | Using CT Mode | grid | uint16 | commissioning_only | 0:METER; 1:CT | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"CTMode","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| LocalAntiBackflowEnable | Anti-reflux enable | grid | uint16 | commissioning_only | 0: Disable; 1: Total anti-reflux enable; 2: Three-phase independent anti-reflux enable | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalAntiBackflowEnable","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| BackflowMeterPowerLimit_R | Anti-reflux Reflux Power Limit | grid | sint16; 1Pn% | commissioning_only | Anti-reflux Reflux Power Limit | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BackflowMeterPowerLimit_R","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| BackflowMeterPowerLimit_S | Anti-reflux Reflux Power Limit | grid | sint16; 1Pn% | commissioning_only | Anti-reflux Reflux Power Limit | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BackflowMeterPowerLimit_S","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| BackflowMeterPowerLimit_T | Anti-reflux Reflux Power Limit | grid | sint16; 1Pn% | commissioning_only | Anti-reflux Reflux Power Limit | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BackflowMeterPowerLimit_T","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| BackflowFaultTime | Anti-reflux failure time | grid | uint16; 1s | commissioning_only | Anti-reflux failure time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BackflowFaultTime","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| BackflowFaultPowerRate | Percentage of active anti-reflux failure | grid | sint16; 1Pn% | commissioning_only | Machine operates at given power after anti-reverse flow failure | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BackflowFaultPowerRate","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Power on config falg | Boot navigation flags | system | uint16 | commissioning_only | 0x00: default; 0x01: configuring (no storage, shutdown); 0x55: configuration complete (save) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Power on config falg","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Vac low C | Grid Low Voltage Restrictions Connected to the Grid | grid | uint16; 0.1V | commissioning_only | Grid-connected conditions | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Vac low C","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Vac high C | Grid High Voltage Restriction Connected to Grid | grid | uint16; 0.1V | commissioning_only | Grid High Voltage Restriction Connected to Grid | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Vac high C","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Fac low C | Grid Low Frequency Restrictions Connected to the Grid | grid | uint16; 0.01Hz | commissioning_only | Grid Low Frequency Restrictions Connected to the Grid | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Fac low C","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Fac high C | Grid High Frequency Restrictions Connected to the Grid | grid | uint16; 0.01Hz | commissioning_only | Grid High Frequency Restrictions Connected to the Grid | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Fac high C","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| NominalEPSVolt | Optional rated off-grid voltage | eps | uint16 | standby_required | 0: 230V; 1: 240V; 2: 208V; 3: 220V (off-grid); 4: 200V (off-grid) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSVolt","value":"<new-value>","confirmed":true}'` |
| NominalEPSFre | Optional rated off-grid frequency | eps | uint16 | standby_required | 0:50Hz; 1:60Hz | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSFre","value":"<new-value>","confirmed":true}'` |
| EPS_En | Off-grid functionality enabled | eps | uint16 | standby_required | Off-grid functionality enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EPS_En","value":"<new-value>","confirmed":true}'` |
| Bypass_En | Bypass mode enabled | eps | uint16 | standby_required | Bypass mode enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Bypass_En","value":"<new-value>","confirmed":true}'` |
| UPS_En | UPS Function Enabled | eps | uint16 | standby_required | TBC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"UPS_En","value":"<new-value>","confirmed":true}'` |
| ubN_PE_RelayCMD | Zero earth relay control enabled | eps | uint16 | commissioning_only | Attendance Functional Requirements | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubN_PE_RelayCMD","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| unAFCICtrlReg | AFCI Self-Test Command | system | uint16 | service_only | Bit0: SelfCheckCmd;; Bit1: ClrFaultCmd;; Bit2~7:reserved; | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"unAFCICtrlReg","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| uwAFCIThresholdValue | AFCI error threshold | system | uint16 | service_only | AFCI error threshold | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwAFCIThresholdValue","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| AFCIClrFaultCmd | AFCI Error Clear Command | system | uint16 | service_only | AFCI Error Clear Command | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AFCIClrFaultCmd","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| uwRestartVoltL | Fault reconnect grid low voltage | grid | uint16; 0.1V | commissioning_only | Fault reconnection and grid connection conditions | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwRestartVoltL","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| uwRestartVoltH | Fault reconnect grid high voltage | grid | uint16; 0.1V | commissioning_only | Fault reconnect grid high voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwRestartVoltH","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| uwRestartFreqL | Fault Reconnect Grid Low Frequency | grid | uint16; 0.01Hz | commissioning_only | Fault Reconnect Grid Low Frequency | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwRestartFreqL","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| uwRestartFreqH | Fault Reconnect Grid High Frequency | grid | uint16; 0.01Hz | commissioning_only | Fault Reconnect Grid High Frequency | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwRestartFreqH","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| BatteryType | Battery type | battery | uint16 | standby_required | 0: Lead acid; 1: Lithium battery; 2: Lithium battery without communication (off-grid); 3: User 2 (off-grid); 4: User 3 (off-grid); 5: NO Bat | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryType","value":"<new-value>","confirmed":true}'` |
| BatteryCompanySet | Battery communication protocol selection | bms | uint16 | standby_required | 0: null; 1: Protocol 1; 2: Protocol 2; 3: Protocol 3; 4: Protocol 4 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryCompanySet","value":"<new-value>","confirmed":true}'` |
| BatMdlSerialNum | Number of batteries in series - high-voltage batteries; | battery | uint16 | standby_required | Lead-acid batteries (for high-voltage battery systems) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatMdlSerialNum","value":"<new-value>","confirmed":true}'` |
| BatMdlParallNum | Number of battery parallel joints; | battery | uint16 | standby_required | Lead-acid batteries (reserved, unused) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatMdlParallNum","value":"<new-value>","confirmed":true}'` |
| ChargeCurrentLimit | Charge Limit Current | battery | uint16; 0.01A | standby_required | When the charging current needs to be lower than this value, enter the floating charging CC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ChargeCurrentLimit","value":"<new-value>","confirmed":true}'` |
| VbatStopForDischarge | Discharge cut-off voltage | battery | uint16; 0.01V | standby_required | Discharge cut-off voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"VbatStopForDischarge","value":"<new-value>","confirmed":true}'` |
| Vbat constant charge | Charge cut-off voltage | battery | uint16; 0.01V | standby_required | CV voltage (lead-acid battery) | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Vbat constant charge","value":"<new-value>","confirmed":true}'` |
| ubHighVoltBatRatedVolt | Battery nominal voltage | battery | uint16; 1V | standby_required | Battery nominal voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubHighVoltBatRatedVolt","value":"<new-value>","confirmed":true}'` |
| ChargeRate | Charging power | battery | uint16; 0.01 | standby_required | Charging power | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ChargeRate","domain":"battery","description":"Charging power","value":"<new-value>","confirmed":true}'` |
| ChargeRate | Discharge power | battery | uint16; 0.01 | standby_required | Discharge power | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ChargeRate","domain":"battery","description":"Discharge power","value":"<new-value>","confirmed":true}'` |
| BatFirstStopSOC | Charge cut-off SOC | battery | uint16; 0.01 | standby_required | Charge cut-off SOC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatFirstStopSOC","value":"<new-value>","confirmed":true}'` |
| OnLineStopSOC | Grid-connected discharge cut-off SOC | battery | uint16; 0.01 | standby_required | Grid-connected discharge cut-off SOC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OnLineStopSOC","value":"<new-value>","confirmed":true}'` |
| OffLineStopSoc | Off-grid Discharge Cutoff SOC | battery | uint16; 0.01 | standby_required | Off-grid Discharge Cutoff SOC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OffLineStopSoc","value":"<new-value>","confirmed":true}'` |
| Gen Charge En | Total Diesel Charging Enabled | system | uint16 | hot_edit_guarded | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Charge En","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| AcCharge_En | AC Charge Enabled | system | uint16 | hot_edit_guarded | 0: Disabled; 1: Enabled | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcCharge_En","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| LoadFirstDischargeDisableFlag | Load Preferred Discharge Prohibit Sign | system | uint16 | hot_edit_guarded | TBC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LoadFirstDischargeDisableFlag","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| RemotePowerControl | One-click charging and discharging | system | uint16 | runtime_command | 255: Fail; 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"RemotePowerControl","value":"<new-value>","confirmed":true}'` |
| NumberTimePeriods | Number of charging and discharging periods - number of currently valid periods, starting with period 1 | system | uint16 | hot_edit_guarded | 0 ~ 10: Number of time periods; 0xA5: Number of time periods reset, all time period setting values reset | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NumberTimePeriods","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart1 | Period 1 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart1","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd1 | Period 1 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Period 1 End Time","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate1 | Period 1 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart2 | Period 2 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart2","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd2 | Period 2 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd2","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate2 | Period 2 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate2","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart3 | Period 3 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart3","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd3 | Slot 3 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd3","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate3 | Period 3 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate3","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart4 | Period 4 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart4","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd4 | Period 4 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd4","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate4 | Period 4 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate4","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart5 | Period 5 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart5","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd5 | Slot 5 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd5","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate5 | Period 5 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate5","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart6 | Period 6 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart6","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd1 | Slot 6 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Slot 6 End Time","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate1 | Period 6 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart7 | Period 7 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart7","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd1 | Segment 7 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Segment 7 End Time","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate1 | Period 7 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart8 | Period 8 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart8","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd1 | Slot 8 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Slot 8 End Time","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate1 | Period 8 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart9 | Period 9 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart9","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd1 | Period 9 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Period 9 End Time","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate1 | Period 9 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeStart10 | Period 10 Start Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeStart10","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeEnd10 | Slot 10 End Time | system | uint16; 1min | hot_edit_guarded | H: hour 0 ~ 23; L: minute 0 ~ 59 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeEnd10","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| PeriodTimeRate10 | Period 10 Priority | system | uint16; 1Pn% | hot_edit_guarded | 0: Load first; 1: Battery first; 2: Grid first | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate10","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| ChargeSourcePriority | Charge Priority | system | uint8 | hot_edit_guarded | 0:CSO; 1:SNU; 2:OSO | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ChargeSourcePriority","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| SourcePriority | Energy Priority | system | uint8 | hot_edit_guarded | 0: Sol; 1: UTI; 2: SBU; 10: Grid connected output mode | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SourcePriority","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| uwCC_DisChrLead_100T | Lead Acid Discharge Current | battery | uint16; 0.01A | standby_required | Lead Acid Discharge Current | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwCC_DisChrLead_100T","value":"<new-value>","confirmed":true}'` |
| ubOP_RecoverDischargeSOC | Grid-connected stop discharge recovery SOC | battery | uint8; 0.01 | standby_required | Grid-connected stop discharge recovery SOC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubOP_RecoverDischargeSOC","value":"<new-value>","confirmed":true}'` |
| ubOffGrid_RecoverDischargeSOC | Off-grid Stop Discharge Recovery SOC | battery | uint8; 0.01 | standby_required | Off-grid Stop Discharge Recovery SOC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubOffGrid_RecoverDischargeSOC","value":"<new-value>","confirmed":true}'` |
| BatUnderVol | Battery Low Voltage Shutdown Voltage | battery | uint16; 0.1V | standby_required | Battery Low Voltage Shutdown Voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatUnderVol","value":"<new-value>","confirmed":true}'` |
| AcChargingCurrent | AC Charge Limit Current | battery | uint16; 0.1A | standby_required | AC Charge Limit Current | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcChargingCurrent","value":"<new-value>","confirmed":true}'` |
| uwFloatV_Lead_100T | Floating Charge Voltage | battery | uint16; 0.01V | standby_required | Floating Charge Voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwFloatV_Lead_100T","value":"<new-value>","confirmed":true}'` |
| BAT2AC_Volt | Battery-to-market voltage | battery | uint16; 0.1V | standby_required | Battery-to-market voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BAT2AC_Volt","value":"<new-value>","confirmed":true}'` |
| AC2BAT_Volt | Mains to Battery Voltage | battery | uint16; 0.1V | standby_required | Mains to Battery Voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AC2BAT_Volt","value":"<new-value>","confirmed":true}'` |
| ubLeadAcid_BatSubType | Subtype of lead-acid battery | battery | uint8 | standby_required | 0:USE; 1:SLD; 2:FLD; 3:GEL; 4:L14; 5:L15; 6:L16; 7:N13; 8:N14; 9:LIT | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":"<new-value>","confirmed":true}'` |
| IncreChar_MaxTim | Improved charging time | battery | uint16; 1min | standby_required | Improved charging time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"IncreChar_MaxTim","value":"<new-value>","confirmed":true}'` |
| BatUnderVolt_Point | Battery undervoltage alarm point | battery | uint16; 0.1V | standby_required | Battery undervoltage alarm point | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatUnderVolt_Point","value":"<new-value>","confirmed":true}'` |
| Equalization | Equalization Mode Enabled | battery | uint8 | standby_required | 0: Disable; 1: Enable | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Equalization","value":"<new-value>","confirmed":true}'` |
| EQBatteryTime | Equalize charging time | battery | uint16; 1min | standby_required | Equalize charging time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EQBatteryTime","value":"<new-value>","confirmed":true}'` |
| EQBatteryTimeout | Equalize Charge Delay Time | battery | uint16; 1min | standby_required | Equalize Charge Delay Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EQBatteryTimeout","value":"<new-value>","confirmed":true}'` |
| EqualizationCycle | Balanced charge interval | battery | uint8; 1DAY | standby_required | Balanced charge interval | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EqualizationCycle","value":"<new-value>","confirmed":true}'` |
| EqualizationImmediately | Turn on Balanced Charging now | battery | uint16 | service_only | 0: Disable; 1: Enable | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EqualizationImmediately","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| flcdEn | LCD setting enable bit | system | uint16 | hot_edit_guarded | 0: ECOMode_En (Energy Saving Mode Enabled); 1: OverLoad_RestartEn; 2: OverTemp_RestartEn; 3: InputChange_RemEn; 4: OPSplit_PhaseEn; 5: Generator_AutoIPEn; 6: DualChannel_LoadEn; 7: ubridFeedBackEn; 8 ~ 15: unused | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"flcdEn","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| BatLVBreak_RestartVolt | Low Voltage Disconnect Battery Recovery Point | battery | uint16; 0.1V | standby_required | Low Voltage Disconnect Battery Recovery Point | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatLVBreak_RestartVolt","value":"<new-value>","confirmed":true}'` |
| BatNeedChr_Volt | Battery Recharge Recovery Point | battery | uint16; 0.1V | standby_required | Battery Recharge Recovery Point | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatNeedChr_Volt","value":"<new-value>","confirmed":true}'` |
| NonCriticlLoad_BatDisConVolt | Non-critical load disconnect battery voltage | battery | uint16; 0.1V | standby_required | Non-critical load disconnect battery voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NonCriticlLoad_BatDisConVolt","value":"<new-value>","confirmed":true}'` |
| BatHighVolt_DisConPoint | Overvoltage disconnection voltage | battery | uint16; 0.1V | standby_required | Overvoltage disconnection voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatHighVolt_DisConPoint","value":"<new-value>","confirmed":true}'` |
| BatOverDisCharge_Delay | Battery Overdischarge Delay Time | battery | uint16; 1s | standby_required | Battery Overdischarge Delay Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatOverDisCharge_Delay","value":"<new-value>","confirmed":true}'` |
| DspBeepOnOff | Buzzer switch | system | uint8 | hot_edit_confirmed | 0: Disable; 1: Enable | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"DspBeepOnOff","value":"<new-value>","confirmed":true}'` |
| OverloadToBypass | Overload transfer bypass enable | eps | uint8 | hot_edit_guarded | 0: Disable; 1: Enable | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OverloadToBypass","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| AcInputType | Off-grid output mode | grid | uint8 | standby_required | 0:APL; 1:UPS | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcInputType","value":"<new-value>","confirmed":true}'` |
| EQBatteryVoltage_100T | Balanced Charging Voltage | battery | uint16 | standby_required | Balanced Charging Voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EQBatteryVoltage_100T","value":"<new-value>","confirmed":true}'` |
| Parallel_Mode | Parallel Mode | parallel | uint8 | standby_required | 0:SIG; 1:PAL; 2:3P1; 3:3P2; 4:3P3 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Parallel_Mode","value":"<new-value>","confirmed":true}'` |
| ubParallelDeviveID | Parallel can communication address | parallel | uint8 | standby_required | Parallel can communication address | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubParallelDeviveID","value":"<new-value>","confirmed":true}'` |
| ubParallelDeviveType | Parallel Device Type | parallel | uint8 | standby_required | 0: Host; 1: Slave | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubParallelDeviveType","value":"<new-value>","confirmed":true}'` |
| ubBMSWorkMode | BMS communication method | bms | uint8 | standby_required | 0: Disable; 1: can communication; 2: 485 communication | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubBMSWorkMode","value":"<new-value>","confirmed":true}'` |
| uwGridPowerCompensation | Grid Power Compensation | grid | uint16 | hot_edit_guarded | Grid Power Compensation | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"uwGridPowerCompensation","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| Gen Port Work Mode | Diesel Port Function Selection | generator | uint16 | standby_required | 0.Default; 1.Generator En; 2.Gen Force; 3.SmartLoad Output; 4.On Grid always on; 5.Off Grid immediately off; 6.AC Couple on SecEPS side | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":"<new-value>","confirmed":true}'` |
| Gen Charge Curr Limit | Diesel Charging Current Limit | generator | uint16; 1A | standby_required | Diesel Charging Current Limit | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Charge Curr Limit","value":"<new-value>","confirmed":true}'` |
| Gen Input Rated Power | Generator Input Rated Power | generator | uint16; 10W | standby_required | Generator Input Rated Power | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Input Rated Power","value":"<new-value>","confirmed":true}'` |
| SecEPS ON SOC/Vbat | (Lithium) Start SOC | generator | uint16; 0.01 | hot_edit_guarded | (Lithium) Start SOC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SecEPS ON SOC/Vbat","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| SecEPS ON Vbat | (Lead acid) Starting battery voltage | generator | uint16; 0.1V | hot_edit_guarded | (Lead acid) Starting battery voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SecEPS ON Vbat","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| SecEPS OFF SOC/Vbat | (Lithium) Shutdown SOC | generator | uint16; 0.01 | hot_edit_guarded | (Lithium) Shutdown SOC | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SecEPS OFF SOC/Vbat","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| SecEPS OFF Vbat | (Lead Acid) Shutdown Battery Voltage | generator | uint16; 0.1V | hot_edit_guarded | (Lead Acid) Shutdown Battery Voltage | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SecEPS OFF Vbat","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| SecEPS  On PV Power Min | Minimum power of photovoltaic startup smart load | generator | uint16; 10W | hot_edit_guarded | Minimum power of photovoltaic startup smart load | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SecEPS  On PV Power Min","value":"<new-value>","confirmed":true,"guarded_interlock":true}'` |
| ubBluetoothEn | Bluetooth Enabled | system | uint8 | hot_edit_confirmed | 1: Bluetooth On; 0: Bluetooth Off | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubBluetoothEn","value":"<new-value>","confirmed":true}'` |
| upgrade notification | Data update notification, wifi re-submit 0304 data to the server | communications | uint8 | service_only | 0: Invalid default; 1: Trigger update | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"upgrade notification","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| Datalogger Restart | wifi factory settings restored, server domain name and escalation time modified | communications | uint8 | service_only | 0: Invalid by default; 1: Trigger a factory reset | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Datalogger Restart","value":"<new-value>","confirmed":true,"service_interlock":true}'` |
| LocalSafetyCmd | User Safety Selection Instructions | grid | uint16 | commissioning_only | TBD; 0: Regional Standard Safety Code; 1: Subscriber Wide Range; 2: Grid Company Safety Code | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalSafetyCmd","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fGridVoltLow1EE | Low Grid Voltage Protection Tier 1 | grid | uint16; 0.1V | commissioning_only | Low Grid Voltage Protection Tier 1 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fGridVoltLow1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fGridVoltHigh1EE | High Grid Voltage Protection Tier 1 | grid | uint16; 0.1V | commissioning_only | High Grid Voltage Protection Tier 1 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fGridVoltHigh1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fFreqLow1EE | Grid Low Frequency Protection First Order | grid | uint16; 0.01Hz | commissioning_only | Grid Low Frequency Protection First Order | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fFreqLow1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fFreqHigh1EE | Grid High Frequency Protection First Order | grid | uint16; 0.01Hz | commissioning_only | Grid High Frequency Protection First Order | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fFreqHigh1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fGridVoltLow2EE | Low Grid Voltage Protection Level 2 | grid | uint16; 0.1V | commissioning_only | Low Grid Voltage Protection Level 2 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fGridVoltLow2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fGridVoltHigh2EE | High Grid Voltage Protection Level 2 | grid | uint16; 0.1V | commissioning_only | High Grid Voltage Protection Level 2 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fGridVoltHigh2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fFreqLow2EE | Grid Low Frequency Protection Second Order | grid | uint16; 0.01Hz | commissioning_only | Grid Low Frequency Protection Second Order | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fFreqLow2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fFreqHigh2EE | Grid High Frequency Protection Second Order | grid | uint16; 0.01Hz | commissioning_only | Grid High Frequency Protection Second Order | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fFreqHigh2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fGridVoltLow3EE | Low Grid Voltage Protection Level 3 | grid | uint16; 0.1V | commissioning_only | Low Grid Voltage Protection Level 3 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fGridVoltLow3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fGridVoltHigh3EE | High Grid Voltage Protection Level 3 | grid | uint16; 0.1V | commissioning_only | High Grid Voltage Protection Level 3 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fGridVoltHigh3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fFreqLow3EE | Grid Low Frequency Protection Level 3 | grid | uint16; 0.01Hz | commissioning_only | Grid Low Frequency Protection Level 3 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fFreqLow3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| fFreqHigh3EE | Grid High Frequency Protection Level 3 | grid | uint16; 0.01Hz | commissioning_only | Grid High Frequency Protection Level 3 | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"fFreqHigh3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| wVLowCutTime1EE | Low Grid Voltage First Order Protection Time | grid | uint16; 20ms | commissioning_only | Low Grid Voltage First Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"wVLowCutTime1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| wVHighCutTime1EE | High grid voltage first-order protection time | grid | uint16; 20ms | commissioning_only | High grid voltage first-order protection time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"wVHighCutTime1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| udFLowCutTime1EE | Low Grid Frequency First Order Protection Time | grid | uint16; 20ms | commissioning_only | Low Grid Frequency First Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"udFLowCutTime1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| udFHighCutTime1EE | High Grid Frequency First Order Protection Time | grid | uint16; 20ms | commissioning_only | High Grid Frequency First Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"udFHighCutTime1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| wVLowCutTime2EE | Low Grid Voltage Second Order Protection Time | grid | uint16; 20ms | commissioning_only | Low Grid Voltage Second Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"wVLowCutTime2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| wVHighCutTime2EE | High Grid Voltage Second Order Protection Time | grid | uint16; 20ms | commissioning_only | High Grid Voltage Second Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"wVHighCutTime2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| udFLowCutTime2EE | Low Grid Frequency Second Order Protection Time | grid | uint16; 20ms | commissioning_only | Low Grid Frequency Second Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"udFLowCutTime2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| udFHighCutTime2EE | High Grid Frequency Second Order Protection Time | grid | uint16; 20ms | commissioning_only | High Grid Frequency Second Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"udFHighCutTime2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| wVLowCutTime3EE | Low grid voltage third-order protection time | grid | uint16; 20ms | commissioning_only | Low grid voltage third-order protection time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"wVLowCutTime3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| wVHighCutTime3EE | High grid voltage third-order protection time | grid | uint16; 20ms | commissioning_only | High grid voltage third-order protection time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"wVHighCutTime3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| udFLowCutTime3EE | Low Grid Frequency Third Order Protection Time | grid | uint16; 20ms | commissioning_only | Low Grid Frequency Third Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"udFLowCutTime3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| udFHighCutTime3EE | High Grid Frequency Third Order Protection Time | grid | uint16; 20ms | commissioning_only | High Grid Frequency Third Order Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"udFHighCutTime3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| 10MinAVLimit | Voltage protection for ten minutes | grid | uint16; 0.1V | commissioning_only | Voltage protection for ten minutes | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"10MinAVLimit","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| U10minTime | 10 Minute Average Voltage Protection Time | grid | uint16; 20ms | commissioning_only | 10 Minute Average Voltage Protection Time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"U10minTime","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| Time start | Connection time | grid | uint16; 1s | commissioning_only | Connection time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Time start","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| RestartDelayTime | Reconnection time | grid | uint16; 1s | commissioning_only | Reconnection time | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"RestartDelayTime","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| PowerStartSlope | Load rate | grid | uint16; 0.1Pn%/min | commissioning_only | Load rate | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PowerStartSlope","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |
| PowerRestartSlopeEE | Restart Load Rate | grid | uint16; 0.1Pn%/min | commissioning_only | Restart Load Rate | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PowerRestartSlopeEE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'` |

### Enumerated setter choices (58 settings)

Each row below is a complete command illustration for one documented choice. Use a fresh numeric `command_id` for every publication. Boolean enable/disable choices use `true` and `false`; other choices use their documented application mode number. These are requested settings, not Modbus addresses or raw register words.

#### OnOffSet - Switch on/off

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Off | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OnOffSet","value":0,"confirmed":true}'` |
| On | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OnOffSet","value":1,"confirmed":true}'` |
| Restart (TBD in map; not validated) | `165` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OnOffSet","value":165,"confirmed":true}'` |

#### BaudrateSet - Select Baud Rate

| Choice | Application value | Mosquitto command |
|---|---:|---|
| 9600bps | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BaudrateSet","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| 38400bps | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BaudrateSet","value":1,"confirmed":true,"commissioning_interlock":true}'` |

#### Rs485WorkMode - 485 Operating Mode Selection

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Slave mode (ate) | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Rs485WorkMode","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| Host mode (BMS485) | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Rs485WorkMode","value":1,"confirmed":true,"commissioning_interlock":true}'` |
| Host mode (Meter485) | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Rs485WorkMode","value":2,"confirmed":true,"commissioning_interlock":true}'` |

#### Reset to factory - Factory reset

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Restore user data | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Reset to factory","value":1,"confirmed":true,"service_interlock":true}'` |
| Restore factory settings | `160` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Reset to factory","value":160,"confirmed":true,"service_interlock":true}'` |

#### UpdateFileType - Upgrade File Type

| Choice | Application value | Mosquitto command |
|---|---:|---|
| bin upgrade | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"UpdateFileType","value":1,"confirmed":true,"service_interlock":true}'` |
| Hex upgrade | `16` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"UpdateFileType","value":16,"confirmed":true,"service_interlock":true}'` |

#### ubScreenType - Screen Type

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Small screen | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubScreenType","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| Large screen | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubScreenType","value":1,"confirmed":true,"commissioning_interlock":true}'` |

#### ActiveOverloadEnable - Active Power Overload Enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ActiveOverloadEnable","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ActiveOverloadEnable","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### PVModelSelect - MPPT Mode

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Independent MPPT mode | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PVModelSelect","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| DC source mode | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PVModelSelect","value":1,"confirmed":true,"commissioning_interlock":true}'` |
| Parallel MPPT mode | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PVModelSelect","value":2,"confirmed":true,"commissioning_interlock":true}'` |

#### FastDerating_en - Fast Down Enable

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"FastDerating_en","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"FastDerating_en","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### Island_en - Island Enabling

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Island_en","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Island_en","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### VFRT_en - High and low penetration enable

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"VFRT_en","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"VFRT_en","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### DRMS_en - DRMS Enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"DRMS_en","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"DRMS_en","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### NLineConnectMode - N-line enable

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NLineConnectMode","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NLineConnectMode","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### NToGNDDetect - Zero Earth Detection

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NToGNDDetect","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NToGNDDetect","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### ZeroPowerOutputEnable - Zero power output enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ZeroPowerOutputEnable","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ZeroPowerOutputEnable","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### FastMpptEnable - Fast mppt enable

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"FastMpptEnable","value":false,"confirmed":true,"commissioning_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"FastMpptEnable","value":true,"confirmed":true,"commissioning_interlock":true}'` |

#### CTMode - Using CT Mode

| Choice | Application value | Mosquitto command |
|---|---:|---|
| METER | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"CTMode","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| CT | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"CTMode","value":1,"confirmed":true,"commissioning_interlock":true}'` |

#### LocalAntiBackflowEnable - Anti-reflux enable

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disable | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalAntiBackflowEnable","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| Total anti-reflux enable | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalAntiBackflowEnable","value":1,"confirmed":true,"commissioning_interlock":true}'` |
| Three-phase independent anti-reflux enable | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalAntiBackflowEnable","value":2,"confirmed":true,"commissioning_interlock":true}'` |

#### Power on config falg - Boot navigation flags

| Choice | Application value | Mosquitto command |
|---|---:|---|
| default | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Power on config falg","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| configuring (no storage, shutdown) | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Power on config falg","value":1,"confirmed":true,"commissioning_interlock":true}'` |
| configuration complete (save) | `85` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Power on config falg","value":85,"confirmed":true,"commissioning_interlock":true}'` |

#### NominalEPSVolt - Optional rated off-grid voltage

| Choice | Application value | Mosquitto command |
|---|---:|---|
| 230V | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSVolt","value":0,"confirmed":true}'` |
| 240V | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSVolt","value":1,"confirmed":true}'` |
| 208V | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSVolt","value":2,"confirmed":true}'` |
| 220V (off-grid) | `3` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSVolt","value":3,"confirmed":true}'` |
| 200V (off-grid) | `4` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSVolt","value":4,"confirmed":true}'` |

#### NominalEPSFre - Optional rated off-grid frequency

| Choice | Application value | Mosquitto command |
|---|---:|---|
| 50Hz | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSFre","value":0,"confirmed":true}'` |
| 60Hz | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"NominalEPSFre","value":1,"confirmed":true}'` |

#### EPS_En - Off-grid functionality enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Value 0 (see description) | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EPS_En","value":0,"confirmed":true}'` |
| Value 1 (see description) | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EPS_En","value":1,"confirmed":true}'` |

#### Bypass_En - Bypass mode enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Value 0 (see description) | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Bypass_En","value":0,"confirmed":true}'` |
| Value 1 (see description) | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Bypass_En","value":1,"confirmed":true}'` |

#### UPS_En - UPS Function Enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Value 0 (see description) | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"UPS_En","value":0,"confirmed":true}'` |
| Value 1 (see description) | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"UPS_En","value":1,"confirmed":true}'` |

#### ubN_PE_RelayCMD - Zero earth relay control enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Value 0 (see description) | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubN_PE_RelayCMD","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| Value 1 (see description) | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubN_PE_RelayCMD","value":1,"confirmed":true,"commissioning_interlock":true}'` |

#### BatteryType - Battery type

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Lead acid | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryType","value":0,"confirmed":true}'` |
| Lithium battery | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryType","value":1,"confirmed":true}'` |
| Lithium battery without communication (off-grid) | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryType","value":2,"confirmed":true}'` |
| User 2 (off-grid) | `3` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryType","value":3,"confirmed":true}'` |
| User 3 (off-grid) | `4` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryType","value":4,"confirmed":true}'` |
| NO Bat | `5` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryType","value":5,"confirmed":true}'` |

#### BatteryCompanySet - Battery communication protocol selection

| Choice | Application value | Mosquitto command |
|---|---:|---|
| null | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryCompanySet","value":0,"confirmed":true}'` |
| Protocol 1 | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryCompanySet","value":1,"confirmed":true}'` |
| Protocol 2 | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryCompanySet","value":2,"confirmed":true}'` |
| Protocol 3 | `3` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryCompanySet","value":3,"confirmed":true}'` |
| Protocol 4 | `4` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatteryCompanySet","value":4,"confirmed":true}'` |

#### BatMdlSerialNum - Number of batteries in series - high-voltage batteries;

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Documented choice 12 | `12` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatMdlSerialNum","value":12,"confirmed":true}'` |
| Documented choice 18 | `18` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatMdlSerialNum","value":18,"confirmed":true}'` |
| Documented choice 24 | `24` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatMdlSerialNum","value":24,"confirmed":true}'` |
| Documented choice 36 | `36` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"BatMdlSerialNum","value":36,"confirmed":true}'` |

#### Gen Charge En - Total Diesel Charging Enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Charge En","value":false,"confirmed":true,"guarded_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Charge En","value":true,"confirmed":true,"guarded_interlock":true}'` |

#### AcCharge_En - AC Charge Enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disabled | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcCharge_En","value":false,"confirmed":true,"guarded_interlock":true}'` |
| Enabled | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcCharge_En","value":true,"confirmed":true,"guarded_interlock":true}'` |

#### LoadFirstDischargeDisableFlag - Load Preferred Discharge Prohibit Sign

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Value 0 (see description) | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LoadFirstDischargeDisableFlag","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Value 1 (see description) | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LoadFirstDischargeDisableFlag","value":1,"confirmed":true,"guarded_interlock":true}'` |

#### RemotePowerControl - One-click charging and discharging

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Fail | `255` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"RemotePowerControl","value":255,"confirmed":true}'` |
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"RemotePowerControl","value":0,"confirmed":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"RemotePowerControl","value":1,"confirmed":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"RemotePowerControl","value":2,"confirmed":true}'` |

#### PeriodTimeRate1 - Period 1 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate2 - Period 2 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate2","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate2","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate2","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate3 - Period 3 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate3","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate3","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate3","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate4 - Period 4 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate4","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate4","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate4","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate5 - Period 5 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate5","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate5","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate5","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate1 - Period 6 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate1 - Period 7 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate1 - Period 8 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate1 - Period 9 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### PeriodTimeRate10 - Period 10 Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Load first | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate10","value":0,"confirmed":true,"guarded_interlock":true}'` |
| Battery first | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate10","value":1,"confirmed":true,"guarded_interlock":true}'` |
| Grid first | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"PeriodTimeRate10","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### ChargeSourcePriority - Charge Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| CSO | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ChargeSourcePriority","value":0,"confirmed":true,"guarded_interlock":true}'` |
| SNU | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ChargeSourcePriority","value":1,"confirmed":true,"guarded_interlock":true}'` |
| OSO | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ChargeSourcePriority","value":2,"confirmed":true,"guarded_interlock":true}'` |

#### SourcePriority - Energy Priority

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Sol | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SourcePriority","value":0,"confirmed":true,"guarded_interlock":true}'` |
| UTI | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SourcePriority","value":1,"confirmed":true,"guarded_interlock":true}'` |
| SBU | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SourcePriority","value":2,"confirmed":true,"guarded_interlock":true}'` |
| Grid connected output mode | `10` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"SourcePriority","value":10,"confirmed":true,"guarded_interlock":true}'` |

#### ubLeadAcid_BatSubType - Subtype of lead-acid battery

| Choice | Application value | Mosquitto command |
|---|---:|---|
| USE | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":0,"confirmed":true}'` |
| SLD | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":1,"confirmed":true}'` |
| FLD | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":2,"confirmed":true}'` |
| GEL | `3` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":3,"confirmed":true}'` |
| L14 | `4` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":4,"confirmed":true}'` |
| L15 | `5` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":5,"confirmed":true}'` |
| L16 | `6` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":6,"confirmed":true}'` |
| N13 | `7` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":7,"confirmed":true}'` |
| N14 | `8` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":8,"confirmed":true}'` |
| LIT | `9` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubLeadAcid_BatSubType","value":9,"confirmed":true}'` |

#### Equalization - Equalization Mode Enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disable | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Equalization","value":false,"confirmed":true}'` |
| Enable | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Equalization","value":true,"confirmed":true}'` |

#### EqualizationImmediately - Turn on Balanced Charging now

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disable | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EqualizationImmediately","value":false,"confirmed":true,"service_interlock":true}'` |
| Enable | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"EqualizationImmediately","value":true,"confirmed":true,"service_interlock":true}'` |

#### DspBeepOnOff - Buzzer switch

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disable | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"DspBeepOnOff","value":false,"confirmed":true}'` |
| Enable | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"DspBeepOnOff","value":true,"confirmed":true}'` |

#### OverloadToBypass - Overload transfer bypass enable

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disable | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OverloadToBypass","value":false,"confirmed":true,"guarded_interlock":true}'` |
| Enable | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"OverloadToBypass","value":true,"confirmed":true,"guarded_interlock":true}'` |

#### AcInputType - Off-grid output mode

| Choice | Application value | Mosquitto command |
|---|---:|---|
| APL | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcInputType","value":0,"confirmed":true}'` |
| UPS | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"AcInputType","value":1,"confirmed":true}'` |

#### Parallel_Mode - Parallel Mode

| Choice | Application value | Mosquitto command |
|---|---:|---|
| SIG | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Parallel_Mode","value":0,"confirmed":true}'` |
| PAL | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Parallel_Mode","value":1,"confirmed":true}'` |
| 3P1 | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Parallel_Mode","value":2,"confirmed":true}'` |
| 3P2 | `3` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Parallel_Mode","value":3,"confirmed":true}'` |
| 3P3 | `4` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Parallel_Mode","value":4,"confirmed":true}'` |

#### ubParallelDeviveType - Parallel Device Type

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Host | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubParallelDeviveType","value":0,"confirmed":true}'` |
| Slave | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubParallelDeviveType","value":1,"confirmed":true}'` |

#### ubBMSWorkMode - BMS communication method

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Disable | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubBMSWorkMode","value":0,"confirmed":true}'` |
| can communication | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubBMSWorkMode","value":1,"confirmed":true}'` |
| 485 communication | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubBMSWorkMode","value":2,"confirmed":true}'` |

#### Gen Port Work Mode - Diesel Port Function Selection

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Default | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":0,"confirmed":true}'` |
| Generator En | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":1,"confirmed":true}'` |
| Gen Force | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":2,"confirmed":true}'` |
| SmartLoad Output | `3` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":3,"confirmed":true}'` |
| On Grid always on | `4` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":4,"confirmed":true}'` |
| Off Grid immediately off | `5` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":5,"confirmed":true}'` |
| AC Couple on SecEPS side | `6` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Gen Port Work Mode","value":6,"confirmed":true}'` |

#### ubBluetoothEn - Bluetooth Enabled

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Bluetooth On | `true` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubBluetoothEn","value":true,"confirmed":true}'` |
| Bluetooth Off | `false` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"ubBluetoothEn","value":false,"confirmed":true}'` |

#### upgrade notification - Data update notification, wifi re-submit 0304 data to the server

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Invalid default | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"upgrade notification","value":0,"confirmed":true,"service_interlock":true}'` |
| Trigger update | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"upgrade notification","value":1,"confirmed":true,"service_interlock":true}'` |

#### Datalogger Restart - wifi factory settings restored, server domain name and escalation time modified

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Invalid by default | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Datalogger Restart","value":0,"confirmed":true,"service_interlock":true}'` |
| Trigger a factory reset | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"Datalogger Restart","value":1,"confirmed":true,"service_interlock":true}'` |

#### LocalSafetyCmd - User Safety Selection Instructions

| Choice | Application value | Mosquitto command |
|---|---:|---|
| Regional Standard Safety Code | `0` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalSafetyCmd","value":0,"confirmed":true,"commissioning_interlock":true}'` |
| Subscriber Wide Range | `1` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalSafetyCmd","value":1,"confirmed":true,"commissioning_interlock":true}'` |
| Grid Company Safety Code | `2` | `mosquitto_pub -h "<broker>" -q 1 -t "<prefix>/<device-id>/inverter/<member-id>/command" -m '{"command_id":"<unique-command-id>","operation":"set","name":"LocalSafetyCmd","value":2,"confirmed":true,"commissioning_interlock":true}'` |

### NON-EXECUTABLE SETTERS - workbook-writable rows blocked by firmware (3)

These rows are included for protocol coverage but are not published as setter commands. Model validation and a future catalog-policy change are required before use.

| Name | Description | Domain | Access | Guard | Reason/notes |
|---|---|---|---|---|---|
| SystemSetBit | System Enable Flag Bit | system | read_write | blocked_unvalidated | Single and Triple Camera Use |
| ubConnectServer | Collector network status | communications | read_write | read_only | Low 8-bit indicates networking status:; 0x0055: Networking exception; 0x00AA: Networking normal; High 8-bit indicates collector type:; 0x0100: Wifi-U; 0x0200: 4G-U |
| ubDatalogAndArmCommunication | Collector and inverter communication status | communications | read_write | read_only | 0x55: Communication error; 0xAA: Communication OK |

## Detailed setter and confirmation recipes

Coverage: 204 writable map rows; 201 named setters allowed by firmware; 3 blocked rows listed above. Each allowed setter has a recipe below. Availability still depends on the inverter model and operating state.

These are individual examples, not a script to execute in sequence. A documented default is an illustration, not a recommended installation setting. Where the map supplies no usable value, the example deliberately retains `<new-value>`; it must be replaced after model-specific validation. Number ranges are not discrete lists: choose a valid value within the documented range. Values described as TBD, reserved, unused or unsupported retain those limitations even when firmware exposes their name.

### Terminal setup and confirmation meaning

In each Linux/Terminus terminal, set these variables. These examples use your local broker and inverter; change MEMBER for a parallel slave. MQTT authentication options can be added to both commands if required.

```bash
BROKER=localhost
PORT=1883
DEVICE=INV-44B176CDC70C
MEMBER=0
BASE="inverter/$DEVICE/inverter/$MEMBER"
```

Start the confirmation subscription in terminal 1 BEFORE publishing from terminal 2. Match the returned `command_id` with the request. `confirmed:true` authorizes the request. Ordinary settings require `outcome:"confirmed"` and a matching readback. OnOffSet requires `outcome:"completed"` with `operating_state_confirmed:true`; its H0 command latch alone does not prove operating state. `transition_pending` means the write was accepted but the requested state is not verified. `readback_mismatch` means the observed setting differs. `sent_unverifiable` remains an action without verified readback. Missing or uncertain results must not trigger an automatic write retry. Malformed or duplicate requests may only be logged by firmware and produce no result.

Illustrative confirmed result (additional fields may be present):

```json
{"member_id":0,"command_id":"123456789","operation":"set","name":"DspBeepOnOff","outcome":"confirmed","value":1}
```

The setting snapshot is also refreshed after a confirmed setter and contains `updated_by_command_id`. To watch it:

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/snapshot/#"
```

Commands below use `date +%s%N` to generate numeric IDs for manual Linux testing. Production servers should allocate IDs uniquely across concurrent clients. Do not publish setter commands as retained messages.

### Setter 1 - OnOffSet: Switch on/off

Operating requirement: runtime_command. Fresh valid runtime state; transition/status monitoring.
Applicable models: Single-phase/three-phase/off-grid.
Map comments: On: 1; Off: 0; Restart: 0xA5 (TBD).
Map default: 1; map range: 0 ~ 1 or 0xA5; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Off (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"OnOffSet","value":0,"confirmed":true}'
```

Setter example - On (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"OnOffSet","value":1,"confirmed":true}'
```

Setter example - Restart (TBD in map; not validated) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"OnOffSet","value":165,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `OnOffSet` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 2 - DeviceID: Device Correspondence Address

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: Communication ID.
Map default: 1; map range: 1~245; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"DeviceID","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `DeviceID` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 3 - BaudrateSet: Select Baud Rate

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0:9600bps; 1:38400bps.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - 9600bps (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BaudrateSet","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - 38400bps (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BaudrateSet","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `BaudrateSet` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 4 - Serial NO.1: Serial number 1

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map comments: Product serial number, valid minimum 16 digits.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.1","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 5 - Serial NO.2: Serial number 2

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.2","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.2` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 6 - Serial NO.3: Serial number 3

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.3","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.3` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 7 - Serial NO.4: Serial numbe 4

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.4","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.4` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 8 - Serial NO.5: Serial number 5

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.5","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.5` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 9 - Serial NO.6: Serial number 6

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.6","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.6` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 10 - Serial NO.7: Serial numbe 7

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.7","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.7` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 11 - Serial NO.8: Serial numbe 8

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.8","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.8` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 12 - Serial NO.9: Serial number 9

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.9","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.9` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 13 - Serial NO.10: Serial number 10

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.10","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.10` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 14 - Serial NO.11: Serial number 11

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.11","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.11` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 15 - Serial NO.12: Serial number 12

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.12","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.12` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 16 - Serial NO.13: Serial number 13

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.13","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.13` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 17 - Serial NO.14: Serial number 14

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.14","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.14` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 18 - Serial NO.15: Serial numbe 15

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Illustrative two-character fragment AB (service operation) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Serial NO.15","value":"AB","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Serial NO.15` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 19 - Rs485WorkMode: 485 Operating Mode Selection

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Slave mode (ate); 1: Host mode (BMS485); 2: Host mode (Meter485).
Map default: 0; map range: 0~2; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Slave mode (ate) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Rs485WorkMode","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Host mode (BMS485) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Rs485WorkMode","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Host mode (Meter485) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Rs485WorkMode","value":2,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Rs485WorkMode` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 20 - Year: System Time - Year

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 2022; map range: Read: 2022 ~ 2050; Write: 22 ~ 50; map unit: Year. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example calendar year 2026 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Year","value":2026,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Year` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 21 - Month: System Time - Month

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 1; map range: 1~12; map unit: Month. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Month","value":1,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Month` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 22 - Day: System Time - Day

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 1; map range: 1~31; map unit: Day. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Day","value":1,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Day` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 23 - Hour: System Time - Hours

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 0; map range: 0~59; map unit: Hour. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Hour","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Hour` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 24 - Minute: System Time - Minutes

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 0; map range: 0~59; map unit: Minutes. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Minute","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Minute` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 25 - Second: System Time - Seconds

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: Set time to seconds to take effect.
Map default: 0; map range: 0~59; map unit: Seconds. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Second","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Second` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 26 - Weekday: System Week

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 1; map range: 1~7; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Weekday","value":1,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Weekday` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 27 - Manufacturer Info 1: Manufacturer Info 1

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map comments: Optional vendor name when configuring.
Map default: -; map range: 0~4; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Manufacturer Info 1","value":0,"confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Manufacturer Info 1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 28 - Module 4: Inverter Mode (4)

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Module 4","value":"<new-value>","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Module 4` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 29 - Module 3: Inverter Mode (3)

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Module 3","value":"<new-value>","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Module 3` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 30 - Module 2: Inverter Mode (2)

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Module 2","value":"<new-value>","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Module 2` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 31 - Module 1: Inverter Mode (1)

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Module 1","value":"<new-value>","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Module 1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 32 - Reset to factory: Factory reset

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map comments: 0x01: Restore user data; 0xA0: Restore factory settings.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Restore user data (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Reset to factory","value":1,"confirmed":true,"service_interlock":true}'
```

Setter example - Restore factory settings (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Reset to factory","value":160,"confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Reset to factory` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 33 - UpdateFileType: Upgrade File Type

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map comments: 0x01: bin upgrade; 0x10: Hex upgrade.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - bin upgrade (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"UpdateFileType","value":1,"confirmed":true,"service_interlock":true}'
```

Setter example - Hex upgrade (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"UpdateFileType","value":16,"confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `UpdateFileType` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 34 - ubScreenType: Screen Type

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Small screen; 1: Large screen.
Map default: -; map range: -; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Small screen (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubScreenType","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Large screen (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubScreenType","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ubScreenType` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 35 - SafetyId: Safety regulation number

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: 0; map range: 0~200; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SafetyId","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `SafetyId` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 36 - ReactivePowerDelayTIme: Reactive Percent Response Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: 0; map range: 0~30000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ReactivePowerDelayTIme","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ReactivePowerDelayTIme` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 37 - ActiveOverloadEnable: Active Power Overload Enabled

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ActiveOverloadEnable","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ActiveOverloadEnable","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ActiveOverloadEnable` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 38 - AcChargePowerRate: Inverter Max Take-Off Power Percentage

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: Grid Intake.
Map default: 100; map range: 0~100; map unit: 1%Pn. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AcChargePowerRate","value":100,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `AcChargePowerRate` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 39 - ActivePowerSlope: Active power rate of change (N4105)

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map comments: 0: Not Enabled; Other: Enabled.
Map default: 0; map range: 0~30000; map unit: 0.1Pn/min. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Not Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ActivePowerSlope","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ActivePowerSlope` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 40 - ActivePowerRate: Inverter Max Output Active Power Percentage

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: Power Generation to the Grid.
Map default: 100; map range: 0~100; map unit: 1%Pn. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ActivePowerRate","value":100,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ActivePowerRate` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 41 - ReactivePowerRate: Inverter Max Output Reactive Power Percentage

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 100~-100; map unit: 1%Pn. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ReactivePowerRate","value":100,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ReactivePowerRate` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 42 - PowerFactorSet: Inverter output power factor 10,000 times

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: PF=(X-10000)/10000.
Map default: 10000; map range: 0~20000; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PowerFactorSet","value":10000,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PowerFactorSet` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 43 - PVModelSelect: MPPT Mode

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Independent MPPT mode; 1: DC source mode; 2: Parallel MPPT mode.
Map default: 0; map range: 0~2; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Independent MPPT mode (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PVModelSelect","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - DC source mode (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PVModelSelect","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Parallel MPPT mode (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PVModelSelect","value":2,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PVModelSelect` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 44 - PvStartVoltage: PV start voltage

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: ; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PvStartVoltage","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PvStartVoltage` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 45 - FastDerating_en: Fast Down Enable

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"FastDerating_en","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"FastDerating_en","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `FastDerating_en` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 46 - Island_en: Island Enabling

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Island_en","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Island_en","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Island_en` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 47 - VFRT_en: High and low penetration enable

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"VFRT_en","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"VFRT_en","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `VFRT_en` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 48 - DRMS_en: DRMS Enabled

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"DRMS_en","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"DRMS_en","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `DRMS_en` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 49 - NLineConnectMode: N-line enable

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NLineConnectMode","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NLineConnectMode","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `NLineConnectMode` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 50 - NToGNDDetect: Zero Earth Detection

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NToGNDDetect","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NToGNDDetect","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `NToGNDDetect` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 51 - ZeroPowerOutputEnable: Zero power output enabled

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ZeroPowerOutputEnable","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ZeroPowerOutputEnable","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ZeroPowerOutputEnable` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 52 - FastMpptEnable: Fast mppt enable

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: -. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"FastMpptEnable","value":false,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"FastMpptEnable","value":true,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `FastMpptEnable` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 53 - CtRatioSet: CT Rheological Ratio Setting

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: Reserved.
Map default: 2000; map range: 1~3000; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"CtRatioSet","value":2000,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `CtRatioSet` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 54 - CTMode: Using CT Mode

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0:METER; 1:CT.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - METER (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"CTMode","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - CT (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"CTMode","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `CTMode` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 55 - LocalAntiBackflowEnable: Anti-reflux enable

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0: Disable; 1: Total anti-reflux enable; 2: Three-phase independent anti-reflux enable.
Map default: 0; map range: 0~3; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LocalAntiBackflowEnable","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Total anti-reflux enable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LocalAntiBackflowEnable","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Three-phase independent anti-reflux enable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LocalAntiBackflowEnable","value":2,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `LocalAntiBackflowEnable` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 56 - BackflowMeterPowerLimit_R: Anti-reflux Reflux Power Limit

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: 0; map range: 0~100; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BackflowMeterPowerLimit_R","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `BackflowMeterPowerLimit_R` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 57 - BackflowMeterPowerLimit_S: Anti-reflux Reflux Power Limit

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: 0; map range: 0~100; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BackflowMeterPowerLimit_S","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `BackflowMeterPowerLimit_S` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 58 - BackflowMeterPowerLimit_T: Anti-reflux Reflux Power Limit

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: 0; map range: 0~100; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BackflowMeterPowerLimit_T","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `BackflowMeterPowerLimit_T` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 59 - BackflowFaultTime: Anti-reflux failure time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: 30; map range: 30~120; map unit: 1s. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BackflowFaultTime","value":30,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `BackflowFaultTime` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 60 - BackflowFaultPowerRate: Percentage of active anti-reflux failure

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map comments: Machine operates at given power after anti-reverse flow failure.
Map default: 50; map range: 0~100; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BackflowFaultPowerRate","value":50,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `BackflowFaultPowerRate` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 61 - Power on config falg: Boot navigation flags

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: 0x00: default; 0x01: configuring (no storage, shutdown); 0x55: configuration complete (save).
Map default: 0; map range: 0 ~ 1 or 0x55; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - default (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Power on config falg","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - configuring (no storage, shutdown) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Power on config falg","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - configuration complete (save) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Power on config falg","value":85,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Power on config falg` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 62 - Vac low C: Grid Low Voltage Restrictions Connected to the Grid

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: Grid-connected conditions.
Map default: ; map range: 1700~2400; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Vac low C","value":170,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Vac low C` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 63 - Vac high C: Grid High Voltage Restriction Connected to Grid

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 2000~2900; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Vac high C","value":200,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Vac high C` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 64 - Fac low C: Grid Low Frequency Restrictions Connected to the Grid

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:4700~5010; 60Hz:; 5700~6010; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Fac low C","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Fac low C` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 65 - Fac high C: Grid High Frequency Restrictions Connected to the Grid

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:; 4990~5300; 60Hz:; 5990~6300; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Fac high C","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Fac high C` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 66 - NominalEPSVolt: Optional rated off-grid voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0: 230V; 1: 240V; 2: 208V; 3: 220V (off-grid); 4: 200V (off-grid).
Map default: 0; map range: 0~4; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - 230V (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NominalEPSVolt","value":0,"confirmed":true}'
```

Setter example - 240V (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NominalEPSVolt","value":1,"confirmed":true}'
```

Setter example - 208V (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NominalEPSVolt","value":2,"confirmed":true}'
```

Setter example - 220V (off-grid) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NominalEPSVolt","value":3,"confirmed":true}'
```

Setter example - 200V (off-grid) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NominalEPSVolt","value":4,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `NominalEPSVolt` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 67 - NominalEPSFre: Optional rated off-grid frequency

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0:50Hz; 1:60Hz.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - 50Hz (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NominalEPSFre","value":0,"confirmed":true}'
```

Setter example - 60Hz (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NominalEPSFre","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `NominalEPSFre` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 68 - EPS_En: Off-grid functionality enabled

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 1; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Value 0 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EPS_En","value":0,"confirmed":true}'
```

Setter example - Value 1 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EPS_En","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `EPS_En` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 69 - Bypass_En: Bypass mode enabled

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Value 0 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Bypass_En","value":0,"confirmed":true}'
```

Setter example - Value 1 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Bypass_En","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `Bypass_En` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 70 - UPS_En: UPS Function Enabled

Operating requirement: standby_required. Fresh standby or stopped state.
Applicable models: Single-phase temporarily not supported.
Map comments: TBC.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Value 0 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"UPS_En","value":0,"confirmed":true}'
```

Setter example - Value 1 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"UPS_En","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `UPS_En` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 71 - ubN_PE_RelayCMD: Zero earth relay control enabled

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: Attendance Functional Requirements.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Value 0 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubN_PE_RelayCMD","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Value 1 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubN_PE_RelayCMD","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ubN_PE_RelayCMD` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 72 - unAFCICtrlReg: AFCI Self-Test Command

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Applicable models: Single-phase temporarily not supported.
Map comments: Bit0: SelfCheckCmd;; Bit1: ClrFaultCmd;; Bit2~7:reserved;.
Map default: ; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"unAFCICtrlReg","value":"<new-value>","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `unAFCICtrlReg` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 73 - uwAFCIThresholdValue: AFCI error threshold

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Applicable models: Single-phase temporarily not supported.
Map comments: AFCI error threshold.
Map default: ; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwAFCIThresholdValue","value":"<new-value>","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `uwAFCIThresholdValue` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 74 - AFCIClrFaultCmd: AFCI Error Clear Command

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map default: ; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AFCIClrFaultCmd","value":"<new-value>","confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `AFCIClrFaultCmd` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 75 - uwRestartVoltL: Fault reconnect grid low voltage

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: Fault reconnection and grid connection conditions.
Map default: ; map range: 1700~2400; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwRestartVoltL","value":170,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `uwRestartVoltL` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 76 - uwRestartVoltH: Fault reconnect grid high voltage

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 2000~2900; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwRestartVoltH","value":200,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `uwRestartVoltH` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 77 - uwRestartFreqL: Fault Reconnect Grid Low Frequency

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:4700~5010; 60Hz:; 5700~6010; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwRestartFreqL","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `uwRestartFreqL` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 78 - uwRestartFreqH: Fault Reconnect Grid High Frequency

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:; 4990~5300; 60Hz:; 5990~6300; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwRestartFreqH","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `uwRestartFreqH` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 79 - BatteryType: Battery type

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0: Lead acid; 1: Lithium battery; 2: Lithium battery without communication (off-grid); 3: User 2 (off-grid); 4: User 3 (off-grid); 5: NO Bat.
Map default: 1; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Lead acid (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryType","value":0,"confirmed":true}'
```

Setter example - Lithium battery (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryType","value":1,"confirmed":true}'
```

Setter example - Lithium battery without communication (off-grid) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryType","value":2,"confirmed":true}'
```

Setter example - User 2 (off-grid) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryType","value":3,"confirmed":true}'
```

Setter example - User 3 (off-grid) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryType","value":4,"confirmed":true}'
```

Setter example - NO Bat (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryType","value":5,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatteryType` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 80 - BatteryCompanySet: Battery communication protocol selection

Operating requirement: standby_required. Fresh standby or stopped state.
Applicable models: Single-phase temporarily not supported.
Map comments: 0: null; 1: Protocol 1; 2: Protocol 2; 3: Protocol 3; 4: Protocol 4.
Map default: 0; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - null (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryCompanySet","value":0,"confirmed":true}'
```

Setter example - Protocol 1 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryCompanySet","value":1,"confirmed":true}'
```

Setter example - Protocol 2 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryCompanySet","value":2,"confirmed":true}'
```

Setter example - Protocol 3 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryCompanySet","value":3,"confirmed":true}'
```

Setter example - Protocol 4 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatteryCompanySet","value":4,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatteryCompanySet` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 81 - BatMdlSerialNum: Number of batteries in series - high-voltage batteries;

Operating requirement: standby_required. Fresh standby or stopped state.
Applicable models: Single-phase temporarily not supported.
Map comments: Lead-acid batteries (for high-voltage battery systems).
Map default: 36; map range: 12,18,24,36; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Documented choice 12 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatMdlSerialNum","value":12,"confirmed":true}'
```

Setter example - Documented choice 18 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatMdlSerialNum","value":18,"confirmed":true}'
```

Setter example - Documented choice 24 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatMdlSerialNum","value":24,"confirmed":true}'
```

Setter example - Documented choice 36 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatMdlSerialNum","value":36,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatMdlSerialNum` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 82 - BatMdlParallNum: Number of battery parallel joints;

Operating requirement: standby_required. Fresh standby or stopped state.
Applicable models: Single-phase temporarily not supported.
Map comments: Lead-acid batteries (reserved, unused).
Map default: 1; map range: 1~160; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatMdlParallNum","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatMdlParallNum` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 83 - ChargeCurrentLimit: Charge Limit Current

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: When the charging current needs to be lower than this value, enter the floating charging CC.
Map default: 8500; map range: S, O:; 100~12000; T:10~250; map unit: 0.01A. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ChargeCurrentLimit","value":85,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ChargeCurrentLimit` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 84 - VbatStopForDischarge: Discharge cut-off voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 4600; map range: S:4200~5000; T:700~1200; O:4000~5200; map unit: 0.01V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"VbatStopForDischarge","value":46,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `VbatStopForDischarge` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 85 - Vbat constant charge: Charge cut-off voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: CV voltage (lead-acid battery).
Map default: 5800; map range: S, O:; 4800~5920; T:1200~1600; map unit: 0.01V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Vbat constant charge","value":58,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `Vbat constant charge` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 86 - ubHighVoltBatRatedVolt: Battery nominal voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 400; map range: T:100~800; map unit: 1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubHighVoltBatRatedVolt","value":400,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubHighVoltBatRatedVolt` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 87 - ChargeRate: Charging power

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 100; map range: 0~100; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ChargeRate","domain":"battery","description":"Charging power","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ChargeRate` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 88 - ChargeRate: Discharge power

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 100; map range: 0~100; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ChargeRate","domain":"battery","description":"Discharge power","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ChargeRate` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 89 - BatFirstStopSOC: Charge cut-off SOC

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 100; map range: 10~100; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatFirstStopSOC","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatFirstStopSOC` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 90 - OnLineStopSOC: Grid-connected discharge cut-off SOC

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 10; map range: 10~100; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"OnLineStopSOC","value":0.1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `OnLineStopSOC` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 91 - OffLineStopSoc: Off-grid Discharge Cutoff SOC

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 10; map range: 10~100; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"OffLineStopSoc","value":0.1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `OffLineStopSoc` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 92 - Gen Charge En: Total Diesel Charging Enabled

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Disabled; 1: Enabled.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Charge En","value":false,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Charge En","value":true,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Gen Charge En` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 93 - AcCharge_En: AC Charge Enabled

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Disabled; 1: Enabled.
Map default: 1; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AcCharge_En","value":false,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Enabled (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AcCharge_En","value":true,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `AcCharge_En` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 94 - LoadFirstDischargeDisableFlag: Load Preferred Discharge Prohibit Sign

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: TBC.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Value 0 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LoadFirstDischargeDisableFlag","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Value 1 (see description) (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LoadFirstDischargeDisableFlag","value":1,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `LoadFirstDischargeDisableFlag` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 95 - RemotePowerControl: One-click charging and discharging

Operating requirement: runtime_command. Fresh valid runtime state; transition/status monitoring.
Map comments: 255: Fail; 0: Load first; 1: Battery first; 2: Grid first.
Map default: 255; map range: 0~2,255; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Fail (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"RemotePowerControl","value":255,"confirmed":true}'
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"RemotePowerControl","value":0,"confirmed":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"RemotePowerControl","value":1,"confirmed":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"RemotePowerControl","value":2,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `RemotePowerControl` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 96 - NumberTimePeriods: Number of charging and discharging periods - number of currently valid periods, starting with period 1

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0 ~ 10: Number of time periods; 0xA5: Number of time periods reset, all time period setting values reset.
Map default: 0; map range: 0~10; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Number of time periods reset, all time period setting values reset (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NumberTimePeriods","value":165,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `NumberTimePeriods` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 97 - PeriodTimeStart1: Period 1 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart1","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 98 - PeriodTimeEnd1: Period 1 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Period 1 End Time","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 99 - PeriodTimeRate1: Period 1 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 1 Priority","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 100 - PeriodTimeStart2: Period 2 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart2","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart2` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 101 - PeriodTimeEnd2: Period 2 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd2","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd2` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 102 - PeriodTimeRate2: Period 2 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate2","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate2","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate2","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate2` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 103 - PeriodTimeStart3: Period 3 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart3","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart3` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 104 - PeriodTimeEnd3: Slot 3 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd3","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd3` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 105 - PeriodTimeRate3: Period 3 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate3","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate3","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate3","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate3` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 106 - PeriodTimeStart4: Period 4 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart4","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart4` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 107 - PeriodTimeEnd4: Period 4 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd4","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd4` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 108 - PeriodTimeRate4: Period 4 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate4","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate4","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate4","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate4` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 109 - PeriodTimeStart5: Period 5 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart5","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart5` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 110 - PeriodTimeEnd5: Slot 5 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd5","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd5` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 111 - PeriodTimeRate5: Period 5 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate5","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate5","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate5","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate5` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 112 - PeriodTimeStart6: Period 6 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart6","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart6` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 113 - PeriodTimeEnd1: Slot 6 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Slot 6 End Time","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 114 - PeriodTimeRate1: Period 6 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 6 Priority","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 115 - PeriodTimeStart7: Period 7 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart7","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart7` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 116 - PeriodTimeEnd1: Segment 7 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Segment 7 End Time","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 117 - PeriodTimeRate1: Period 7 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 7 Priority","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 118 - PeriodTimeStart8: Period 8 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart8","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart8` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 119 - PeriodTimeEnd1: Slot 8 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Slot 8 End Time","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 120 - PeriodTimeRate1: Period 8 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 8 Priority","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 121 - PeriodTimeStart9: Period 9 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart9","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart9` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 122 - PeriodTimeEnd1: Period 9 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd1","domain":"system","description":"Period 9 End Time","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 123 - PeriodTimeRate1: Period 9 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate1","domain":"system","description":"Period 9 Priority","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate1` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 124 - PeriodTimeStart10: Period 10 Start Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeStart10","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeStart10` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 125 - PeriodTimeEnd10: Slot 10 End Time

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: H: hour 0 ~ 23; L: minute 0 ~ 59.
Map default: 0; map range: 0~0x173B; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.
The map packs hour and minute together. The current named API does not accept an HH:MM string or convert ordinary elapsed minutes. The zero example means midnight only; other times require a validated complete encoding. A friendly time setter is not implemented.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeEnd10","value":0,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeEnd10` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 126 - PeriodTimeRate10: Period 10 Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Load first; 1: Battery first; 2: Grid first.
Map default: 0; map range: 0~2; map unit: 1Pn%. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Load first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate10","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Battery first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate10","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid first (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PeriodTimeRate10","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PeriodTimeRate10` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 127 - ChargeSourcePriority: Charge Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0:CSO; 1:SNU; 2:OSO.
Map default: 1; map range: 0~2; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - CSO (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ChargeSourcePriority","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - SNU (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ChargeSourcePriority","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - OSO (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ChargeSourcePriority","value":2,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `ChargeSourcePriority` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 128 - SourcePriority: Energy Priority

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Sol; 1: UTI; 2: SBU; 10: Grid connected output mode.
Map default: 1; map range: 0~10; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Sol (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SourcePriority","value":0,"confirmed":true,"guarded_interlock":true}'
```

Setter example - UTI (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SourcePriority","value":1,"confirmed":true,"guarded_interlock":true}'
```

Setter example - SBU (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SourcePriority","value":2,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Grid connected output mode (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SourcePriority","value":10,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `SourcePriority` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 129 - uwCC_DisChrLead_100T: Lead Acid Discharge Current

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 13700; map range: 100~13700; map unit: 0.01A. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwCC_DisChrLead_100T","value":137,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `uwCC_DisChrLead_100T` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 130 - ubOP_RecoverDischargeSOC: Grid-connected stop discharge recovery SOC

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 90; map range: 20~100; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubOP_RecoverDischargeSOC","value":0.9,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubOP_RecoverDischargeSOC` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 131 - ubOffGrid_RecoverDischargeSOC: Off-grid Stop Discharge Recovery SOC

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 30; map range: 10~100; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubOffGrid_RecoverDischargeSOC","value":0.3,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubOffGrid_RecoverDischargeSOC` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 132 - BatUnderVol: Battery Low Voltage Shutdown Voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 400; map range: 400~480; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatUnderVol","value":40,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatUnderVol` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 133 - AcChargingCurrent: AC Charge Limit Current

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 600; map range: 10~1000; map unit: 0.1A. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AcChargingCurrent","value":60,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `AcChargingCurrent` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 134 - uwFloatV_Lead_100T: Floating Charge Voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 5520; map range: 4800~5840; map unit: 0.01V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwFloatV_Lead_100T","value":55.2,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `uwFloatV_Lead_100T` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 135 - BAT2AC_Volt: Battery-to-market voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 460; map range: 440~520; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BAT2AC_Volt","value":46,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BAT2AC_Volt` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 136 - AC2BAT_Volt: Mains to Battery Voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 540; map range: 480~580; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AC2BAT_Volt","value":54,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `AC2BAT_Volt` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 137 - ubLeadAcid_BatSubType: Subtype of lead-acid battery

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0:USE; 1:SLD; 2:FLD; 3:GEL; 4:L14; 5:L15; 6:L16; 7:N13; 8:N14; 9:LIT.
Map default: 3; map range: 0~8; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.
Source conflict: the range says 0 through 8, but the comments also list 9 (LIT). That extra choice is documented, not guaranteed accepted by the model or firmware validation.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - USE (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":0,"confirmed":true}'
```

Setter example - SLD (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":1,"confirmed":true}'
```

Setter example - FLD (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":2,"confirmed":true}'
```

Setter example - GEL (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":3,"confirmed":true}'
```

Setter example - L14 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":4,"confirmed":true}'
```

Setter example - L15 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":5,"confirmed":true}'
```

Setter example - L16 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":6,"confirmed":true}'
```

Setter example - N13 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":7,"confirmed":true}'
```

Setter example - N14 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":8,"confirmed":true}'
```

Setter example - LIT (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubLeadAcid_BatSubType","value":9,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubLeadAcid_BatSubType` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 138 - IncreChar_MaxTim: Improved charging time

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 120; map range: 5~900; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"IncreChar_MaxTim","value":120,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `IncreChar_MaxTim` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 139 - BatUnderVolt_Point: Battery undervoltage alarm point

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 440; map range: 400~500; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatUnderVolt_Point","value":44,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatUnderVolt_Point` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 140 - Equalization: Equalization Mode Enabled

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0: Disable; 1: Enable.
Map default: 1; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Equalization","value":false,"confirmed":true}'
```

Setter example - Enable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Equalization","value":true,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `Equalization` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 141 - EQBatteryTime: Equalize charging time

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 120; map range: 5~900; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EQBatteryTime","value":120,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `EQBatteryTime` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 142 - EQBatteryTimeout: Equalize Charge Delay Time

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 120; map range: 5~900; map unit: 1min. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EQBatteryTimeout","value":120,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `EQBatteryTimeout` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 143 - EqualizationCycle: Balanced charge interval

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 30; map range: 0~30; map unit: 1DAY. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EqualizationCycle","value":30,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `EqualizationCycle` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 144 - EqualizationImmediately: Turn on Balanced Charging now

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map comments: 0: Disable; 1: Enable.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EqualizationImmediately","value":false,"confirmed":true,"service_interlock":true}'
```

Setter example - Enable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EqualizationImmediately","value":true,"confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `EqualizationImmediately` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 145 - flcdEn: LCD setting enable bit

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: ECOMode_En (Energy Saving Mode Enabled); 1: OverLoad_RestartEn; 2: OverTemp_RestartEn; 3: InputChange_RemEn; 4: OPSplit_PhaseEn; 5: Generator_AutoIPEn; 6: DualChannel_LoadEn; 7: ubridFeedBackEn; 8 ~ 15: unused.
Map default: ; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.
This is a combined flag field, not a mode selector. The listed numbers are bit positions. A named write replaces the whole flag field; there is currently no named per-flag setter here that preserves other flags automatically. Do not send a bit position as if it enabled that feature. A validated complete value is required.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"flcdEn","value":"<new-value>","confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `flcdEn` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 146 - BatLVBreak_RestartVolt: Low Voltage Disconnect Battery Recovery Point

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 520; map range: 500~580; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatLVBreak_RestartVolt","value":52,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatLVBreak_RestartVolt` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 147 - BatNeedChr_Volt: Battery Recharge Recovery Point

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 520; map range: 500~560; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatNeedChr_Volt","value":52,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatNeedChr_Volt` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 148 - NonCriticlLoad_BatDisConVolt: Non-critical load disconnect battery voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 460; map range: 420~540; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"NonCriticlLoad_BatDisConVolt","value":46,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `NonCriticlLoad_BatDisConVolt` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 149 - BatHighVolt_DisConPoint: Overvoltage disconnection voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 600; map range: 300~600; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatHighVolt_DisConPoint","value":60,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatHighVolt_DisConPoint` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 150 - BatOverDisCharge_Delay: Battery Overdischarge Delay Time

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 5; map range: 5~50; map unit: 1s. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"BatOverDisCharge_Delay","value":5,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `BatOverDisCharge_Delay` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 151 - DspBeepOnOff: Buzzer switch

Operating requirement: hot_edit_confirmed. Confirmed internal read-before-write and read-back.
Map comments: 0: Disable; 1: Enable.
Map default: 1; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"DspBeepOnOff","value":false,"confirmed":true}'
```

Setter example - Enable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"DspBeepOnOff","value":true,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `DspBeepOnOff` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 152 - OverloadToBypass: Overload transfer bypass enable

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map comments: 0: Disable; 1: Enable.
Map default: 1; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"OverloadToBypass","value":false,"confirmed":true,"guarded_interlock":true}'
```

Setter example - Enable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"OverloadToBypass","value":true,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `OverloadToBypass` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 153 - AcInputType: Off-grid output mode

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0:APL; 1:UPS.
Map default: 1; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - APL (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AcInputType","value":0,"confirmed":true}'
```

Setter example - UPS (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"AcInputType","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `AcInputType` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 154 - EQBatteryVoltage_100T: Balanced Charging Voltage

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 5840; map range: 4800~5840; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"EQBatteryVoltage_100T","value":5840,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `EQBatteryVoltage_100T` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 155 - Parallel_Mode: Parallel Mode

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0:SIG; 1:PAL; 2:3P1; 3:3P2; 4:3P3.
Map default: 0; map range: 0~4; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - SIG (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Parallel_Mode","value":0,"confirmed":true}'
```

Setter example - PAL (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Parallel_Mode","value":1,"confirmed":true}'
```

Setter example - 3P1 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Parallel_Mode","value":2,"confirmed":true}'
```

Setter example - 3P2 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Parallel_Mode","value":3,"confirmed":true}'
```

Setter example - 3P3 (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Parallel_Mode","value":4,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `Parallel_Mode` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 156 - ubParallelDeviveID: Parallel can communication address

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 0; map range: 0~9; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubParallelDeviveID","value":0,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubParallelDeviveID` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 157 - ubParallelDeviveType: Parallel Device Type

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0: Host; 1: Slave.
Map default: 0; map range: 0~1; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Host (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubParallelDeviveType","value":0,"confirmed":true}'
```

Setter example - Slave (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubParallelDeviveType","value":1,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubParallelDeviveType` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 158 - ubBMSWorkMode: BMS communication method

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0: Disable; 1: can communication; 2: 485 communication.
Map default: 0; map range: 0~2; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Disable (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubBMSWorkMode","value":0,"confirmed":true}'
```

Setter example - can communication (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubBMSWorkMode","value":1,"confirmed":true}'
```

Setter example - 485 communication (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubBMSWorkMode","value":2,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubBMSWorkMode` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 159 - uwGridPowerCompensation: Grid Power Compensation

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 40; map range: 0~200; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"uwGridPowerCompensation","value":40,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `uwGridPowerCompensation` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 160 - Gen Port Work Mode: Diesel Port Function Selection

Operating requirement: standby_required. Fresh standby or stopped state.
Map comments: 0.Default; 1.Generator En; 2.Gen Force; 3.SmartLoad Output; 4.On Grid always on; 5.Off Grid immediately off; 6.AC Couple on SecEPS side.
Map default: 0; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Default (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Port Work Mode","value":0,"confirmed":true}'
```

Setter example - Generator En (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Port Work Mode","value":1,"confirmed":true}'
```

Setter example - Gen Force (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Port Work Mode","value":2,"confirmed":true}'
```

Setter example - SmartLoad Output (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Port Work Mode","value":3,"confirmed":true}'
```

Setter example - On Grid always on (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Port Work Mode","value":4,"confirmed":true}'
```

Setter example - Off Grid immediately off (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Port Work Mode","value":5,"confirmed":true}'
```

Setter example - AC Couple on SecEPS side (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Port Work Mode","value":6,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `Gen Port Work Mode` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 161 - Gen Charge Curr Limit: Diesel Charging Current Limit

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 100A; map range: ; map unit: 1A. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Charge Curr Limit","value":100,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `Gen Charge Curr Limit` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 162 - Gen Input Rated Power: Generator Input Rated Power

Operating requirement: standby_required. Fresh standby or stopped state.
Map default: 8000W; map range: ; map unit: 10W. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Gen Input Rated Power","value":8000,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `Gen Input Rated Power` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 163 - SecEPS ON SOC/Vbat: (Lithium) Start SOC

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 0.4; map range: ; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SecEPS ON SOC/Vbat","value":0.4,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `SecEPS ON SOC/Vbat` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 164 - SecEPS ON Vbat: (Lead acid) Starting battery voltage

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 45.0V; map range: ; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SecEPS ON Vbat","value":45,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `SecEPS ON Vbat` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 165 - SecEPS OFF SOC/Vbat: (Lithium) Shutdown SOC

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 0.4; map range: ; map unit: 0.01. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SecEPS OFF SOC/Vbat","value":0.4,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `SecEPS OFF SOC/Vbat` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 166 - SecEPS OFF Vbat: (Lead Acid) Shutdown Battery Voltage

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 55.0V; map range: ; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SecEPS OFF Vbat","value":55,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `SecEPS OFF Vbat` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 167 - SecEPS  On PV Power Min: Minimum power of photovoltaic startup smart load

Operating requirement: hot_edit_guarded. Also requires guarded_interlock and validated dependencies.
Map default: 3000W; map range: ; map unit: 10W. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Example using the documented default converted to the current API scale (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"SecEPS  On PV Power Min","value":3000,"confirmed":true,"guarded_interlock":true}'
```

Confirmation: match the printed ID and `name` = `SecEPS  On PV Power Min` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 168 - ubBluetoothEn: Bluetooth Enabled

Operating requirement: hot_edit_confirmed. Confirmed internal read-before-write and read-back.
Map comments: 1: Bluetooth On; 0: Bluetooth Off.
Map default: 1; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Bluetooth On (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubBluetoothEn","value":true,"confirmed":true}'
```

Setter example - Bluetooth Off (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"ubBluetoothEn","value":false,"confirmed":true}'
```

Confirmation: match the printed ID and `name` = `ubBluetoothEn` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 169 - upgrade notification: Data update notification, wifi re-submit 0304 data to the server

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map comments: 0: Invalid default; 1: Trigger update.
Map default: 0; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Invalid default (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"upgrade notification","value":0,"confirmed":true,"service_interlock":true}'
```

Setter example - Trigger update (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"upgrade notification","value":1,"confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `upgrade notification` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 170 - Datalogger Restart: wifi factory settings restored, server domain name and escalation time modified

Operating requirement: service_only. Fresh standby/stopped state and service_interlock; never auto-retry.
Map comments: 0: Invalid by default; 1: Trigger a factory reset.
Map default: 0; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Invalid by default (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Datalogger Restart","value":0,"confirmed":true,"service_interlock":true}'
```

Setter example - Trigger a factory reset (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Datalogger Restart","value":1,"confirmed":true,"service_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Datalogger Restart` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 171 - LocalSafetyCmd: User Safety Selection Instructions

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map comments: TBD; 0: Regional Standard Safety Code; 1: Subscriber Wide Range; 2: Grid Company Safety Code.
Map default: 0; map range: ; map unit: . These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Regional Standard Safety Code (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LocalSafetyCmd","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Subscriber Wide Range (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LocalSafetyCmd","value":1,"confirmed":true,"commissioning_interlock":true}'
```

Setter example - Grid Company Safety Code (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"LocalSafetyCmd","value":2,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `LocalSafetyCmd` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 172 - fGridVoltLow1EE: Low Grid Voltage Protection Tier 1

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 460~2400; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fGridVoltLow1EE","value":46,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fGridVoltLow1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 173 - fGridVoltHigh1EE: High Grid Voltage Protection Tier 1

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 2000~2900; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fGridVoltHigh1EE","value":200,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fGridVoltHigh1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 174 - fFreqLow1EE: Grid Low Frequency Protection First Order

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:4700~5010; 60Hz:; 5700~6010; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fFreqLow1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fFreqLow1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 175 - fFreqHigh1EE: Grid High Frequency Protection First Order

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:; 4990~5300; 60Hz:; 5990~6300; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fFreqHigh1EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fFreqHigh1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 176 - fGridVoltLow2EE: Low Grid Voltage Protection Level 2

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 460~2400; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fGridVoltLow2EE","value":46,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fGridVoltLow2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 177 - fGridVoltHigh2EE: High Grid Voltage Protection Level 2

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 2000~2900; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fGridVoltHigh2EE","value":200,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fGridVoltHigh2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 178 - fFreqLow2EE: Grid Low Frequency Protection Second Order

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:4700~5010; 60Hz:; 5700~6010; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fFreqLow2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fFreqLow2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 179 - fFreqHigh2EE: Grid High Frequency Protection Second Order

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 50Hz:; 4990~5300; 60Hz:; 5990~6300; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fFreqHigh2EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fFreqHigh2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 180 - fGridVoltLow3EE: Low Grid Voltage Protection Level 3

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 460~2400; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fGridVoltLow3EE","value":46,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fGridVoltLow3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 181 - fGridVoltHigh3EE: High Grid Voltage Protection Level 3

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 2000~2900; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fGridVoltHigh3EE","value":200,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fGridVoltHigh3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 182 - fFreqLow3EE: Grid Low Frequency Protection Level 3

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 50Hz:4700~5010; 60Hz:; 5700~6010; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fFreqLow3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fFreqLow3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 183 - fFreqHigh3EE: Grid High Frequency Protection Level 3

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 50Hz:; 4990~5300; 60Hz:; 5990~6300; map unit: 0.01Hz. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"fFreqHigh3EE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `fFreqHigh3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 184 - wVLowCutTime1EE: Low Grid Voltage First Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"wVLowCutTime1EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `wVLowCutTime1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 185 - wVHighCutTime1EE: High grid voltage first-order protection time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"wVHighCutTime1EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `wVHighCutTime1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 186 - udFLowCutTime1EE: Low Grid Frequency First Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"udFLowCutTime1EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `udFLowCutTime1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 187 - udFHighCutTime1EE: High Grid Frequency First Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"udFHighCutTime1EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `udFHighCutTime1EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 188 - wVLowCutTime2EE: Low Grid Voltage Second Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"wVLowCutTime2EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `wVLowCutTime2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 189 - wVHighCutTime2EE: High Grid Voltage Second Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"wVHighCutTime2EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `wVHighCutTime2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 190 - udFLowCutTime2EE: Low Grid Frequency Second Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"udFLowCutTime2EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `udFLowCutTime2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 191 - udFHighCutTime2EE: High Grid Frequency Second Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"udFHighCutTime2EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `udFHighCutTime2EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 192 - wVLowCutTime3EE: Low grid voltage third-order protection time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"wVLowCutTime3EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `wVLowCutTime3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 193 - wVHighCutTime3EE: High grid voltage third-order protection time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"wVHighCutTime3EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `wVHighCutTime3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 194 - udFLowCutTime3EE: Low Grid Frequency Third Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"udFLowCutTime3EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `udFLowCutTime3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 195 - udFHighCutTime3EE: High Grid Frequency Third Order Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"udFHighCutTime3EE","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `udFHighCutTime3EE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 196 - 10MinAVLimit: Voltage protection for ten minutes

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 200~2900; map unit: 0.1V. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"10MinAVLimit","value":20,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `10MinAVLimit` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 197 - U10minTime: 10 Minute Average Voltage Protection Time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 0~5000; map unit: 20ms. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"U10minTime","value":0,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `U10minTime` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 198 - Time start: Connection time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Applicable models: Single-phase temporarily not supported.
Map default: ; map range: 30~900; map unit: 1s. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"Time start","value":30,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `Time start` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 199 - RestartDelayTime: Reconnection time

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 30~900; map unit: 1s. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Range-boundary illustration; not a recommended operating setting (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"RestartDelayTime","value":30,"confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `RestartDelayTime` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 200 - PowerStartSlope: Load rate

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 1-30000; map unit: 0.1Pn%/min. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PowerStartSlope","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PowerStartSlope` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.

### Setter 201 - PowerRestartSlopeEE: Restart Load Rate

Operating requirement: commissioning_only. Fresh standby/stopped state and commissioning_interlock.
Map default: ; map range: 1-30000; map unit: 0.1Pn%/min. These source fields retain the map notation; example payloads use the firmware application scale.

Confirmation command (terminal 1):

```bash
mosquitto_sub -h "$BROKER" -p "$PORT" -q 1 -v -t "$BASE/result"
```

Setter example - Choose a validated value; the map does not define a complete usable example (terminal 2):

```bash
COMMAND_ID=$(date +%s%N)
printf 'Request ID: %s\n' "$COMMAND_ID"
mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$BASE/command" -m '{"command_id":"'"$COMMAND_ID"'","operation":"set","name":"PowerRestartSlopeEE","value":"<new-value>","confirmed":true,"commissioning_interlock":true}'
```

Confirmation: match the printed ID and `name` = `PowerRestartSlopeEE` in `/result`. Inspect `outcome` and the returned `value`; receiving the command at the broker alone is not inverter confirmation.
<!-- END GENERATED COMPLETE COMMAND CATALOG -->
