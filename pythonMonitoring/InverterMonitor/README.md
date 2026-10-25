# Inverter Monitor

Streamlit browser UI and Tkinter desktop UI with live telemetry, gateway health, and protocol-specific device commands. AOHAI retains its existing live display and all 201 catalog settings. Both interfaces also support the 25 PI, JK, Daly and VE.Direct profiles added to the ESP firmware. The browser entry point is `app.py`; the desktop entry point is `inverter_monitor.py`.
Keep `MQTT_Register_Command_API.md` beside the Python modules: the settings catalog reads it at runtime.
Keep `settings_results.py` beside them as well; both interfaces share its write-result validation.
Keep `native_protocols.py`, `native_live.py` and `protocol_catalog.json` beside them for the native profiles. No additional Python packages are required for these profiles.

## Run with the existing shared environment

From this folder in PowerShell:

```powershell
& ..\.venv\Scripts\python.exe -m pip install -r reqirement.txt
& ..\.venv\Scripts\python.exe -m streamlit run app.py
```

## Standalone setup

Use Python 3.10 or newer (3.12 recommended). Tkinter is not required for the browser app. Copy the entire folder, including `.streamlit`, but do not copy a Windows virtual environment to another computer.

Mac, from this folder:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r reqirement.txt
python -m streamlit run app.py
```

Windows:

```powershell
py -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -r reqirement.txt
& .\.venv\Scripts\python.exe -m streamlit run app.py
```

Open `http://localhost:8501` if the browser does not open automatically. Enter the installation's inverter ID, select the same protocol as the ESP installer, choose the logical member (0 through 9), and press Connect. Member 0 is the coordinator's local inverter; choose the assigned member number for an ESP-NOW inverter member. Gateway health is read from member 0 for every selected inverter.

### Broker connection errors

The default endpoint is `wss://mqtt-esp.alemdarteknik.com:443/mqtt`. A WebSocket handshake error happens before MQTT connects. After an upgrade failure, the app makes a bounded diagnostic HTTP upgrade request and displays the server response without sending inverter commands.

The WebSocket client identifies itself with `User-Agent: InverterMonitor/1.0`. The broker's proxy may challenge clients that omit this header; Paho's default handshake omits it. The diagnostic uses the same User-Agent, Host and Origin headers as the app.

If the error reports `HTTP 403` and a Cloudflare browser challenge, inspect Cloudflare Security Events for this hostname and `/mqtt` path. Adjust the specific challenging rule to permit the MQTT endpoint while keeping broker authentication/access controls. Cloudflare Bot Fight Mode does not support skip rules; see [Cloudflare security interoperability](https://developers.cloudflare.com/waf/feature-interoperability/). A browser challenge cannot be completed by the Python MQTT client.

To use another broker listener, set these environment variables in the terminal before starting Streamlit:

| Variable | Default | Meaning |
| --- | --- | --- |
| `INVERTER_MQTT_HOST` | `mqtt-esp.alemdarteknik.com` | Hostname without scheme/path |
| `INVERTER_MQTT_TRANSPORT` | `websockets` | `websockets` or `tcp`; TLS enabled for both |
| `INVERTER_MQTT_PORT` | `443` for WebSockets, `8883` for TCP | Port of the configured broker listener |
| `INVERTER_MQTT_PATH` | `/mqtt` | WebSocket path; unused for TCP |

For example, `$env:INVERTER_MQTT_HOST = 'your-broker.example.com'` in PowerShell, or `export INVERTER_MQTT_HOST=your-broker.example.com` on macOS/Linux. A raw MQTT/TLS listener must be configured separately on the broker; changing the client transport cannot create it. The browser app and `MonitorSession` callers use these variables; the legacy AOHAI Tkinter transport retains its module constants.

## Additional protocol profiles

Available profiles are `PI16`, `PI17`, `PI17M058`, `PI17INFINI`, `PI18`,
`PI18LVX`, `PI18SV`, `PI30`, `PI30MAX`, `PI30MST`, `PI30REVO`, `PI30M044`,
`PI30M045`, `PI41`, `JK02`, `JK02_32`, `JK04`, `JK232`, `JK485`, `JKPB`,
`JKV11`, `JKSERIAL`, `DALY`, `DALY40`, and `VED`.

The ESP polls the selected device automatically. Live data shows PV, grid,
battery, output and load without entering protocol commands. It preserves
reported values and units; missing readings stay unavailable and actual zero
values remain zero. Each group shows freshness independently. Standby is an
observed operating mode with its own age, not a substitute for electrical data.

The live power view shows grid and load **active power in kW** separately from
**apparent power in kVA**. Battery power uses **+ for discharge and − for charge**,
in kW. A voltage reading does not replace a missing power value. If the profile
does not report enough information to establish power or battery direction,
the power display stays unavailable; stale samples are marked stale.

The same automatic monitoring applies to every listed profile. PI variants
include their additional PV channels, grid/output phases and BMS readings where
supported. JK and Daly views focus on battery, current, SOC, cells, temperatures
and protection status; unsupported inverter groups are hidden. VE.Direct shows
the battery, solar or output measurements the connected Victron product emits.
Device totals are preferred; complete reported power channels/phases may be
summed. A missing channel stays missing, and apparent power is never treated as
active power. Each reading keeps its own update age.

For base PI30, selecting two or three PV inputs in the ESP installer enables
the optional second-tracker query automatically. Supported inverters publish
PV2 current, voltage and power; models with the extended response also publish
PV3 and a measured whole-device PV total. Both interfaces show these readings
in the PV measurements table. The PV power card uses the measured total or all
selected tracker powers. It stays unavailable while a selected tracker has no
power reading, and becomes stale if any component expires. A real zero from
one tracker remains valid. Older single-tracker firmware keeps its existing
PV1 display. A device that rejects the optional query cannot supply PV2 data.

Primary readings are polled about every two seconds; secondary/status readings
every five seconds. Identity and supported charging-current lists are read once
per device connection. Current settings refresh every 120 seconds and after
writes; supported energy totals refresh every 60 seconds. Auxiliary diagnostics
and clock reads refresh every ten seconds. Manufacturer defaults (`QDI`/`DI`)
are manual diagnostics and never replace active settings in the normal display.
The normal MQTT `/live` snapshot refreshes every five seconds and includes cached
identity/settings with their actual sample ages.

Current firmware publishes complete native observations on
`inverter/INV-ID/inverter/MEMBER/protocol/COMMAND`, with `record_complete:true`
and `sample_age_ms`. Detailed records publish initially and on value changes,
with a fresh publication after reconnect. Unchanged manual queries still receive
their command result immediately and fresh values in the next live snapshot.
Native live snapshots also carry `native_records`. The
monitor subscribes to these observations with QoS 1, deduplicates redelivery,
and never treats cached publication as a fresh sensor reading. Older chunked
telemetry is also supported: every chunk is required before measurements
update, and partial assemblies expire after five seconds. Advanced diagnostics
show raw records and reasons for dropped/expired/conflicting records.
Native publications may also carry `connected_pv_inputs` for the gateway's
local inverter. For remote members without this metadata, the monitor infers
additional trackers from their own reported PV2/PV3 fields.

For a native profile, the Commands/Settings view lists its documented queries,
setting formats, examples and readback queries. Enter a complete supported
command and press Send query or explicitly confirm and send a setting.
Examples are command examples, not measured current settings. Commands use
the existing MQTT `/command` topic with `operation:"protocol"`, `protocol`,
`command` and a unique command ID; settings include `confirmed:true`. The
firmware validates the profile and complete command. Unsupported or
undocumented settings are unavailable; some BMS profiles and PI16 provide
monitoring only.

Native `/result` outcomes differ from AOHAI's result schema. A device ACK is
shown as **accepted**. Only a successful fresh device readback comparison is
shown as **verified** (`completed`, `setting_verified:true`). An explicit
readback mismatch is shown separately. Addressed parallel commands use their
addressed unit's documented query, never the local inverter's settings. A
sent setting with an uncertain response, a lost result, or a timeout remains
uncertain and blocks further settings in that monitor session; reads remain
available. Check the actual device setting and resolve the connection before
reconnecting the monitor. The ESP also blocks further uncertain writes until
the device transport reconnects (direct UART requires restarting its ESP).
Neither interface retries setting commands automatically.

Select `AOHAI_MODBUS_RTU` for the existing Mexxsun/AOHAI installation; its
get/set API, guarded settings and live payload handling are preserved.
The AOHAI on/off control always sends the command once. The monitor only shows
**inverter On/Off confirmed** when the ESP has verified its actual operating
state (`completed`, `operating_state_confirmed:true`). A command-register
readback alone is not that verification. Pending transitions remain unverified,
and the command is never automatically retried. This controls the inverter's
operating state; the connected battery/grid may keep its electronics powered.

Selecting a native profile suppresses the automatic AOHAI catalog reads and
hides the AOHAI controls. If the device reports a different native protocol,
commands are blocked until you reconnect with the matching selection.
Update the inverter-facing ESP, coordinator and any separate MQTT gateway
to firmware that understands native protocol messages. Individual device
compatibility still needs hardware validation; no model is inferred from
the inverter's brand name.

The checked-in native catalog follows the ESP firmware's supported commands.
PI command definitions derive from the MIT-licensed `jblance/mpp-solar`
revision `eafdd4328c77f516cf82430c5bcb6fa042c45d3a`; source metadata and the
permission notice are in `protocol_catalog.json` and `LICENSE.mpp-solar.txt`.

## Behavior and safety

- MQTT uses TLS WebSockets at `mqtt-esp.alemdarteknik.com:443/mqtt`, without username/password. Each browser session owns its connection.
- The UI refreshes every second using [Streamlit fragments](https://docs.streamlit.io/develop/api-reference/execution-flow/st.fragment). Live receipt becomes stale after 15 seconds; health after 90 seconds.
- The desktop also refreshes native readings once per second while checking its event queue every 100 ms. Measurement tables reuse their rows, preserving selection and avoiding rebuilds when values are unchanged. Sample ages and stale indicators continue advancing without new packets.
- Raw native records are copied and formatted only when their diagnostics view is open. Recent Streamlit versions load that view when its tab and expander are opened; older versions provide a checkbox. The complete validated observations remain available in the bounded collector.
- AOHAI settings use the ESP's published snapshots. Background getters validate retained values and fill missing or older readings, with a ten-minute interval for inactive settings and a 30-second interval for the selected setting. Selecting a different setting requests its current value; repeated UI refreshes do not repeat that request. Dedicated clock and shortcut controls use a two-minute fallback. Failed getters back off, unsupported getters are skipped, and writes are never retried automatically. Native monitoring is polled by the ESP, with optional manual queries in the app.
- An unsubmitted selection survives refreshes unless a different current value arrives.
- Set requires explicit acknowledgement. Setting commands in both interfaces are non-retained, under 2048 bytes, and use QoS 0 to avoid transport redelivery. Only a matching inverter result and read-back confirms an AOHAI setting; on/off additionally requires operating-state verification. Native ACKs mean accepted until an exact fresh readback verifies the setting. Lost commands/results remain unconfirmed or uncertain and block further settings in that monitor session; reads continue. Verify the device before reconnecting. Writes are never automatically retried.
- Numeric AOHAI inputs use the documented API engineering scale and register step: for example, `ChargeRate` is a fraction from 0 to 1, 20 ms timer settings require multiples of 20, and battery voltage settings retain their documented voltage step. Model-dependent operating requirements remain enforced by the ESP. Calendar readbacks accept both the inverter's two-digit year and its four-digit year.
- AOHAI time changes write six fields sequentially, seconds last. Failure stops remaining writes; partial changes are possible. Native time settings use only the documented command available in their own profile.
- Reserved/unvalidated settings remain blocked, including the combined `flcdEn` bitfield.
- Disconnect clears displayed data. Network failures require manual reconnect. A browser inactive for 90 seconds loses its connection. Avoid multiple controllers for one inverter.
- The server binds to `127.0.0.1`. It has no web authentication and can control equipment: do not expose it publicly without adding access controls. The server computer needs MQTT network access.

## Tests

```powershell
& ..\.venv\Scripts\python.exe -m unittest discover -p "test_*.py" -q
```

Tests use mocked MQTT and do not send inverter commands. The complete suite covers AOHAI regression, native catalog validation, complete telemetry assembly, command/result matching, confirmation, uncertain-write handling, slave routing, and both interfaces. Desktop tests require Tkinter and use withdrawn test windows.
