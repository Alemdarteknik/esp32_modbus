# Production readiness and release gates

## Power timing correction

### Last-known display values

Existing numeric keys are preserved. New top-level `power_quality` contains an entry for each PV/grid/inverter/load/battery power and load percentage: `available`, `stale`, `state` (`valid`, `last_known`, or `unavailable`), and `since_valid_ms`. The latter is time since the result last passed validation, NOT measurement age. A previously valid value stays visible if the next computation is unavailable; it is marked stale and never used as a calculation input. Section entries restored this way have `source: last_known`, `stale: true`, and `since_valid_ms`. Never-successful readings remain null. History clears on detected member restart or gateway restart. When all live messages stop, the app must also use its own receive-time timeout.

This supersedes earlier statements that every timing failure makes summary values null: only never-valid values remain null; held values require checking `power_quality`. Existing source/basis fields describe current computations and may be absent/unavailable during holdover. Grid interpretation remains provisional R320, even when its data passes timing validation.

Priority polling attempts four-register reads for adjacent EPS apparent/active pairs (R332, R336, R340) and battery discharge/charge (R349). A failed combined read disables batching for that pair for the current adapter session and retries individual pairs. The 850 ms bus gap is unchanged. Support and achieved timing must be verified on hardware.

The scheduler repeats the essential power group (grid/PV electrical readings, PV total, inverter active total, EPS active/apparent phases, battery charge/discharge), interleaves due critical status reads, and grants one rotating background read per pass. This replaces simple full-plan reordering. The bus gap is unchanged; failures or three-phase reads can still exceed calculation age/skew limits and must produce unavailable values. No whole-payload simultaneity guarantee is made. Validate timing on hardware.

`grid_kw` temporarily uses R320-R321 (unsigned 32-bit, high word first, 0.1 W / 1000 = kW), as explicitly approved for validation. Its grid-intake interpretation is UNVERIFIED: the original workbook only says total AC power to user. The grid direction fields have been removed from live telemetry; the grid section source is `unverified_R320_register`, and its `power_factor` is null because PF is not used. R320 is included in priority polling. Missing or timing-invalid reads still yield null; no V*I*PF fallback is used. Topics and keys remain unchanged.

MQTT topics and field names are unchanged. Power calculations require source samples at most 15000 ms old relative to the newest received source timestamp, gateway cache age at most 15000 ms, and at most 5000 ms between contributing samples. Configure these limits in `project_config.hpp`. Essential power requests are grouped, but remain sequential with status reads interleaved; missing/late inputs yield null, not zero. This does not guarantee that all independent summary fields describe one simultaneous snapshot. Transport delay before gateway receipt remains unmeasured.

`grid_kw` temporarily uses R320-R321 (unsigned 32-bit, high word first, 0.1 W / 1000 = kW), as explicitly approved for validation. Its grid-intake interpretation is UNVERIFIED: the original workbook only says total AC power to user. The grid direction fields have been removed from live telemetry; the grid section source is `unverified_R320_register`, and its `power_factor` is null because PF is not used. R320 is included in priority polling. Missing or timing-invalid reads still yield null; no V*I*PF fallback is used. Topics and keys remain unchanged.

Status: hardening in progress; not certified or endurance-tested. No firmware build, flash, physical inverter write or long-duration test was performed during this audit. Do not interpret source checks as evidence of multi-year operation.

## Implemented hardening

- USB-RS485 connection, polling and queued MQTT-command execution share one dedicated `inverter_bus` FreeRTOS task. It uses automatic core affinity, priority 4 and an 8192-byte stack by default, configured in `project_config.hpp`. The main task stays alive to preserve configuration/runtime lifetimes. MQTT publication remains a separate task.
- Modbus stack minimum-free bytes are logged at INFO level every 30 seconds while the adapter is connected. Enable INFO output for this diagnostic. Task creation failure is logged and leaves inverter communication stopped; it does not reset credentials. This change does not shorten the 850 ms bus gap, verify register meanings, or guarantee polling deadlines under timeout/command load.

- Polling capacity is checked at compile time; due requests rotate instead of starving later blocks.
- Power-register pairs are read independently; failed setup reads retry with a delay.
- Poller state uses heap rather than the small main-task stack.
- Live JSON uses one bounded-size output allocation; the tree is freed before enqueue copies the payload.
- MQTT has a bounded 64 KiB outbox and asynchronous sends. Explicit network timeout is 10 seconds and reconnect delay is 5 seconds.
- Retained commands and commands over 2048 bytes are rejected. Never retain commands at the broker.
- Live readings become stale after 120 seconds; slow counters after 900 seconds. The former 10-second cutoff was shorter than the observed roughly 35-second polling sweep with an 850 ms transaction gap. These are configurable ceilings, not guaranteed refresh intervals. Staleness is evaluated on the next input message; absence of all telemetry must be detected using health data/server receive times.
- Last-known register values remain visible with `age_ms` and `stale`; stale inputs are excluded from calculations. Once a primary power register has been received, its expiry does not silently select another measurement source. A reboot clears this history. Calculations still use sequential samples, not an atomic measurement: cache age does not include transport delay. Multiword decoding requires matching receipt timestamps for both words.
- Live uses only grid, inverter and load AC power groups. `load_kw` and `load_apparent_kva` are EPS-terminal active/apparent power; `sections.load` replaces `sections.output`. Duplicate output/local-load/power-to-user summary fields and raw power-register entries are removed from live; named register inquiries remain available.
- Select rated output power on the webpage: 3, 4, 5, 6, 8, 12, 15 or 20 kW. Local load percentage uses the saved rating, or the requested 12 kW default for an unset legacy rating. `rated_power_kw` and `rating_source` expose the effective choice. This is a configured assumption, not automatic nameplate detection. Remote-member rating forwarding is still pending. This setting never writes to the inverter.
- Battery register subtraction requires both charge and discharge readings; PV voltage/current fallback requires every configured input. Missing values are not assumed zero.
- `bms_configured` is a setup choice; `bms_connected` is null until a verified communication-state mapping exists. Installation flags are not physical presence detection.
- A member uptime rollback clears its input cache, cached mode/rating validity and alert history. This is not a cryptographic boot/session identity.
- The publisher drains telemetry during MQTT outages instead of holding one old sample indefinitely. Dropped data is counted. There is no durable telemetry replay guarantee.
- Gateway health is published every 30 seconds without waiting for inverter telemetry, using a small fixed serialization buffer.

## Health subscription

```bash
mosquitto_sub -v -t 'inverter/INV-44B176CDC70C/inverter/0/health'
```

The health topic belongs to the Internet-facing ESP's configured member ID. It is QoS 0, not retained. It is not a per-slave hardware-health report. A blocked MQTT task or disconnected network can prevent its delivery; the server must detect missed reports.

Fields: `uptime_ms`, `reset_reason` (ESP-IDF reset enum), `heap_free_bytes`, `heap_minimum_bytes`, `heap_largest_block_bytes`, `publisher_stack_free_bytes`, `outbox_bytes`, `publish_queue_depth`, `telemetry_seen`, `telemetry_age_ms`, `live_enqueued`, `live_enqueue_age_ms`, `mqtt_disconnect_count`, `enqueue_failures`, `telemetry_queue_drops`.

The two age fields are milliseconds measured by the receiving gateway. An age of zero with the corresponding boolean false means never observed, not fresh data. Telemetry age covers any member and input/holding data; it does not certify every field. Live enqueue is not broker acknowledgement. The age counters use unsigned 32-bit subtraction and are meaningful for gaps shorter than approximately 49.7 days. Reboot resets RAM counters. Minimum free heap may decrease during warm-up; a continual decline after warm-up needs investigation.

## Required validation before release

1. Build and run on each supported board configuration. Confirm actual LED pin, flash size, PSRAM mode and USB wiring. Current sdkconfig disables PSRAM; enabling it requires confirming the exact module and testing memory. Do not assume that an N16R8 label alone verifies board configuration.
2. Run a minimum seven-day monitored soak on standalone and the maximum intended parallel setup. Store health and live receipt timestamps on the server. Pass only with no unexpected resets, allocation failures, unexplained queue growth or continued memory decline after warm-up. Exercise the largest supported payload. Set acceptance stack/heap margins from measured worst-case use, not a guessed constant.
3. Repeatedly interrupt Wi-Fi, broker connectivity, USB-RS485 and inverter power. Include long outages, broker restarts and reconnection after address changes. Verify recovery, current data, no stale values treated as new energy samples, and no unsolicited writes. Keep test loads controlled.
4. Test command duplicates, retained commands, oversized/malformed payloads, reboot during write/read-back, and lost results. Verify untouched feature flags remain unchanged. Never automatically retry a write merely because its confirmation is missing.
5. Test every supported role and phase profile. Verify polarity and measurement boundary of each power point against instruments; grid power-factor validity remains model-dependent.

## Remaining release blockers

- Command deduplication is a finite RAM list; there is no persistent exactly-once execution, durable result journal or authenticated command expiry. Design a server command/session contract before unattended writable deployment. MQTT origin tagging alone does not authenticate a command.
- Provisioning/LAN configuration authentication and authorization need review. The setup AP default password is shared. Broker credentials must be provisioned with per-device ACLs and authenticated TLS for deployment; optional anonymous/plaintext support remains for local testing.
- ESP-NOW payload encryption and peer authentication are not complete. A separate transport-security design and hardware tests are required before trusting wireless write routing.
- No verified OTA rollback and signed update process is established. Partition layout, signing keys, secure boot and flash/NVS encryption require a concrete release policy; do not burn eFuses during development.
- Snapshot/result delivery is not durable: queue/outbox rejection can lose a result or a once-read snapshot. A server must be able to request configuration again and distinguish uncertain write outcomes. Alert state is retried if enqueue fails, but queue acceptance is not an end-to-end acknowledgement.
- JSON tree construction still has many individual allocations. The optimized serializer and health reporting reduce/debug risk but do not prove allocation-failure completeness or freedom from fragmentation.
- Per-register timestamps add about 2 KiB per active inverter member. Maximum-parallel memory use must be validated on the selected PSRAM configuration.
- No complete watchdog/stall-recovery policy has been validated; do not automatically reboot during a potentially pending inverter write without a defined recovery contract.
- Power quality, enclosure temperature, RS485 isolation/surge protection, EMC and electrical compliance require hardware assessment.

Release is gated on evidence from these tests and resolving the applicable blockers. Maintain firmware version, configuration schema, release notes and rollback instructions for every deployed version.
