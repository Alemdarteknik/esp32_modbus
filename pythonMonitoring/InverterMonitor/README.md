# Inverter Monitor

Streamlit browser UI and Tkinter desktop UI with live telemetry, gateway health, and protocol-specific device commands. AOHAI retains its existing live display and all 201 catalog settings. Both interfaces also support the 25 PI, JK, Daly and VE.Direct profiles added to the ESP firmware. The browser entry point is `app.py`; the desktop entry point is `inverter_monitor.py`.
Keep `MQTT_Register_Command_API.md` beside the Python modules: the settings catalog reads it at runtime.
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

Open `http://localhost:8501` if the browser does not open automatically. Enter the installation's inverter ID, select the same protocol as the ESP installer, choose the logical member (0 through 9), and press Connect. Member 0 is the coordinator's local inverter; choose the assigned member number for an ESP-NOW inverter member. Gateway health is read from member 0 for every selected inverter. Broker configuration is in `mqtt_backend.py`.

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

Primary readings are polled about every two seconds; secondary/status readings
every five seconds. Identity is read once per device connection. Configuration
and supported energy totals are read every 60 seconds, with changed settings
published automatically. The normal MQTT `/live` snapshot refreshes every five
seconds and includes cached identity/settings with their sample ages.

Current firmware publishes complete native observations on
`inverter/INV-ID/inverter/MEMBER/protocol/COMMAND`, with `record_complete:true`
and `sample_age_ms`. Native live snapshots also carry `native_records`. The
monitor subscribes to these observations with QoS 1, deduplicates redelivery,
and never treats cached publication as a fresh sensor reading. Older chunked
telemetry is also supported: every chunk is required before measurements
update, and partial assemblies expire after five seconds. Advanced diagnostics
show raw records and reasons for dropped/expired/conflicting records.

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
shown as **accepted** and never promoted to confirmed readback equality. A
sent setting with an uncertain response, a lost result, or a timeout remains
uncertain and blocks further settings in that monitor session; reads remain
available. Check the actual device setting and resolve the connection before
reconnecting the monitor. The ESP also blocks further uncertain writes until
the device transport reconnects (direct UART requires restarting its ESP).
Neither interface retries setting commands automatically.

Select `AOHAI_MODBUS_RTU` for the existing Mexxsun/AOHAI installation; its
get/set API, guarded settings and live payload handling are preserved.
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
- AOHAI settings reads rotate at most once per second. Initial loading may take several minutes; unsupported getters are skipped. Native monitoring is polled by the ESP, with optional manual queries in the app.
- An unsubmitted selection survives refreshes unless a different current value arrives.
- Set requires explicit acknowledgement. Browser and native-profile commands are non-retained, under 2048 bytes, and use QoS 0 to avoid transport redelivery. Only a matching inverter result and read-back confirms an AOHAI write; native ACKs mean accepted. Lost commands/results remain unconfirmed or uncertain; writes are never automatically retried.
- AOHAI time changes write six fields sequentially, seconds last. Failure stops remaining writes; partial changes are possible. Native time settings use only the documented command available in their own profile.
- Reserved/unvalidated settings remain blocked, including the combined `flcdEn` bitfield.
- Disconnect clears displayed data. Network failures require manual reconnect. A browser inactive for 90 seconds loses its connection. Avoid multiple controllers for one inverter.
- The server binds to `127.0.0.1`. It has no web authentication and can control equipment: do not expose it publicly without adding access controls. The server computer needs MQTT network access.

## Tests

```powershell
& ..\.venv\Scripts\python.exe -m unittest discover -p "test_*.py" -q
```

Tests use mocked MQTT and do not send inverter commands. The complete suite covers AOHAI regression, native catalog validation, complete telemetry assembly, command/result matching, confirmation, uncertain-write handling, slave routing, and both interfaces. Desktop tests require Tkinter and use withdrawn test windows.
