# Protocol capability and command reference

The installer selects one protocol and one connection for each inverter member.
Wi-Fi, MQTT, ESP-NOW, the coordinator cable, OTA, provisioning and device roles
remain shared services. Changing the profile changes its request framing,
checksum, response layout and command allowlist.

This firmware includes the existing AOHAI Modbus RTU backend and all 25 named
profiles from the referenced `mpp-solar` catalog. A profile name describes a
wire protocol; it does not establish that every inverter bearing a particular
brand implements that variant. Select the model's documented protocol and
physical interface. Tomatech USB HID needs the matching PI variant.

## Coverage

The PI counts below are native query definitions and settings patterns, not
individual parameter values. The exact patterns and readback queries appear
later in this document. Variant response layouts remain distinct even when
their command names are shared.

| Installer profile | Monitoring | Settings | Usual connection and starting baud |
| --- | --- | --- | --- |
| `AOHAI_MODBUS_RTU` | Existing V2.14 register catalog | Existing guarded semantic/catalog writes | USB RS485, 9600 |
| `PI16` | 13 query definitions | None in source catalog | USB HID or model's serial interface, 2400 |
| `PI17` | 25 query definitions | 27 patterns | USB HID or model's serial interface, 2400 |
| `PI17M058` | 25 query definitions; M058 layout | 27 patterns | USB HID or model's serial interface, 2400 |
| `PI17INFINI` | 25 query definitions; Infini layout | 27 patterns | USB HID or model's serial interface, 2400 |
| `PI18` | 13 query definitions | 11 patterns | USB HID or model's serial interface, 2400 |
| `PI18LVX` | 20 query definitions; LVX framing policy | 26 patterns | USB HID or model's serial interface, 2400 |
| `PI18SV` | 20 query definitions; SV framing policy | 26 patterns | USB HID or model's serial interface, 2400 |
| `PI30` | 19 query definitions | 26 patterns | USB HID or model's serial interface, 2400 |
| `PI30MAX` | 35 query definitions | 32 patterns | USB HID or model's serial interface, 2400 |
| `PI30MST` | 35 query definitions; MST layout | 32 patterns | USB HID or model's serial interface, 2400 |
| `PI30REVO` | 7 query definitions; REVO framing | 2 patterns | USB HID or model's serial interface, 2400 |
| `PI30M044` | 36 query definitions; M044 layout | 32 patterns | USB HID or model's serial interface, 2400 |
| `PI30M045` | 35 query definitions; M045 layout | 32 patterns | USB HID or model's serial interface, 2400 |
| `PI41` | 21 query definitions; phase-specific layouts | 26 patterns | USB HID or model's serial interface, 2400 |
| `JK02` | 24-cell BLE status and device information | Four charge/discharge switches | Bluetooth, service FFE0 / characteristic FFE1 |
| `JK02_32` | 32-cell BLE status and device information | None allowlisted | Bluetooth, FFE0 / FFE1 |
| `JK04` | Float cell-voltage BLE layout and information | None allowlisted | Bluetooth, FFE0 / FFE1 |
| `JK232` | JBD-compatible basic status, cells, information | None allowlisted | Device's serial interface, commonly 9600 |
| `JK485` | RS485 active balancer status, up to 24 cells | Cell count and balancing settings | RS485, 9600 |
| `JKPB` | JK inverter BMS status, settings readout, information | None allowlisted | RS485, commonly 115200 |
| `JKV11` | V11 BLE status layout and information | None allowlisted | Bluetooth, FFE0 / FFE1 |
| `JKSERIAL` | Tagged JK serial status with variable cell count | None allowlisted | Device's TTL serial interface, 115200 |
| `DALY` | Nine Daly queries, address 0x80 | None allowlisted | Device's UART/RS485 interface, 9600 |
| `DALY40` | Same Daly packet family, request address 0x40 | None allowlisted | Device's UART/RS485 interface, 9600 |
| `VED` | Passive VE.Direct TEXT; battery-capacity HEX query | BMV/SmartShunt battery capacity | VE.Direct TTL serial, 19200 |

Baud rates are installer starting values. Use the model's actual rate. UART
RS232, UART RS485 and UART TTL selections describe different electrical
interfaces and need their matching transceiver/cable. A USB serial selection
uses the supported USB VCP driver; it does not convert a TTL port into RS232
or RS485. Native USB HID is available to PI profiles. JK BLE profiles require
Bluetooth; VE.Direct uses its TTL interface.

USB HID discovers a compatible device automatically. The installer selects
the protocol and USB HID connection; no USB vendor/product ID is required.
The ESP reads the device IDs and checks its configuration and report
descriptors. Automatic discovery accepts nonboot HID interfaces with a
vendor-defined application and unnumbered eight-byte input/output reports;
keyboard, mouse and gamepad applications and incompatible report layouts
are rejected. Normal readonly polling must return a valid response for the
selected protocol before device commands are admitted. A setting ACK does
not count as a monitoring observation.

Completed input transfers may contain fewer than eight bytes. The driver
uses only the bytes received and ignores empty reports. Transient input
errors receive at most two recovery attempts before the third consecutive
failure closes the connection. STALL recovery clears the device endpoint
and recreates the host pipe after both callbacks have drained. Recovery
keeps the same connection session and never replays a settings write.
Warnings report transfer status, actual length, endpoint and detected IDs.

For PC logs while the inverter uses native USB, connect through the board's
USB-to-UART programming port or a separate UART adapter. The ESP32-S3 USB
host and native USB Serial/JTAG share the internal PHY, so switching it to
host mode can disconnect a PC monitor on native USB. See Espressif's
[USB Host hardware guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_host.html#external-phy-configuration).

Advanced settings contain optional USB vendor and product filters. Zero
means any ID; each nonzero field must match. Explicitly setting both IDs
also permits an atypical HID application usage, while retaining the
eight-byte report checks. Saved nonzero filters are preserved. Clear both
to zero to enable automatic discovery. Detected actual IDs appear in the
ESP logs. This identifies a compatible USB connection; the installer still
selects the device's documented protocol variant.

Each ESP owns one local USB inverter connection. With multiple compatible
devices on a hub, the first compatible bridge is selected; there is no
automatic search across bridges for a protocol response. Use a direct
connection for a normal installation or explicit filters to choose the
intended bridge. UART, Bluetooth and the existing CH340/CH341 serial path
do not use these HID filters. USB device descriptors are exposed by the
[ESP-IDF USB Host library](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/usb_host.html).

## Requests and results

Native commands use the shared MQTT request topic:

```text
PREFIX/SITE/inverter/MEMBER/command
```

For example, query the selected PI30 member:

```json
{"command_id":"1001","operation":"protocol","protocol":"PI30","command":"QPIGS"}
```

An explicitly confirmed native settings request:

```json
{"command_id":"1002","operation":"protocol","protocol":"PI30","command":"POP02","confirmed":true}
```

The profile validates the complete command and infers whether it is a setter.
If `write` is supplied, it must match that inference. Settings require
`confirmed:true`. Requests for another selected profile, unknown commands,
invalid values, retained requests and duplicate command IDs do not execute.
AOHAI continues to use its existing semantic and register request forms.

Results use `PREFIX/SITE/inverter/MEMBER/result`. Native outcomes are
`completed`, `acknowledged`, `rejected`, `unsupported`, `unavailable`,
`not_sent`, `uncertain` or `invalid_response`. An ACK establishes that the
device accepted the command. A subsequent readback reports observed settings;
it does not automatically establish equality for a multi-setting command.
`write_sent:null` means the route could not establish whether the remote
write was transmitted. A sent setting with a missing valid reply remains
uncertain and is not automatically retried.

A sent setting with an uncertain response blocks further settings on that
connection, so its delayed ACK cannot acknowledge a different write. Monitoring
continues. A new USB/Bluetooth connection session clears the block; direct UART
requires restarting the node after resolving the device connection. A successful
read alone does not clear it. The local command owner also keeps the last 16
admitted IDs: pending requests cannot be evicted, and completed duplicate requests
return the original result. This history is bounded and does not survive reboot.
Use unique command IDs and resolve uncertain writes before issuing replacements.

Native telemetry, commands and results add wire message types 5, 6 and 7 while
preserving the existing Modbus packet layouts. Update the member, coordinator and
separate gateway firmware together when using these profiles; older firmware
does not route the new message types. Each node selects one local device profile. Native frames are bounded to 1024 bytes and decoded records to 96 fields; the firmware rejects responses beyond those limits. Longer text values end in "...".

Normal monitoring uses `PREFIX/SITE/inverter/MEMBER/live` for native profiles
as well as AOHAI. Native snapshots contain PV, grid, battery, output, load,
status, identity, settings and energy sections where reported by the device.
Missing measurements remain absent; stale power summaries are null. The
native monitor displays these sections directly and keeps raw commands under
advanced diagnostics. A valid mode reply such as Standby has its own age and
does not establish that electrical telemetry is fresh.

This display and polling behavior applies to every installer profile. The
normal view uses the device's available readings:

| Profile family | Normal live readings |
| --- | --- |
| PI inverter variants | PV channels, grid phases, battery, output phases, load, temperatures and operating status where reported |
| JK BMS profiles | Pack voltage/current/power, SOC, physical cell readings, balancing, temperatures and protection status where reported |
| Daly profiles | Pack voltage/current/SOC, cell voltage and temperature extrema, MOS state, balancing and protection status |
| VE.Direct | Battery, PV or AC output readings according to the connected Victron product, plus status and energy counters |

Explicit device totals take precedence. A channel or phase total can also be
formed from a complete set of reported active or apparent power readings;
missing channels do not count as zero. Active power and apparent power remain
separate: grid and load active power use kW, and apparent power uses kVA.
Batteries use signed DC kW, positive for discharge and negative for charge.
Where the protocol supplies measured DC voltage and current in one complete
observation, battery power can be calculated with the documented current sign.
Split-current PI readings use `V * (discharge A - charge A) / 1000`.
JK current is charge-positive and is inverted; Daly A5 current is already
discharge-positive. VE.Direct reported battery power is charge-positive and
is inverted, with voltage/current used only as a fallback. Raw unsigned JK
power is retained in diagnostics but is not used as signed battery power.
PI17 reported battery power requires its direction from the same observation.

Calculated values require coherent components, take the oldest component's
age and expire with live measurements. PI17 grid apparent power sums all
three measured phase voltage/current pairs. AC voltage/current never supplies
active power without a reported active-power measurement. PI30's QPIGS has
no grid current or grid power, so both grid power values remain unavailable;
QPIRI current is a configuration rating and cannot supply a live grid value.
BMS-only profiles show battery information without unsupported PV/grid/load
cards. Wi-Fi, MQTT, ESP-NOW, OTA and roles remain shared across all profiles.

Current signs follow the [JK protocol implementation](https://github.com/syssi/esphome-jk-bms),
[Daly A5 implementation](https://github.com/dreadnought/python-daly-bms/blob/main/dalybms/daly_bms.py)
and [Victron VE.Direct protocol](https://www.victronenergy.com/upload/documents/VE.Direct-Protocol-3.33.pdf).

Background polling reads primary measurements about every two seconds and
secondary/status measurements every five seconds. Identity and firmware are
read once per device connection; configuration and supported energy totals
are read every 60 seconds. Optional rejected queries stop for that session;
transient failures back off. Live MQTT snapshots refresh every five seconds.
Identity, configuration and status command records publish when changed,
with a 60-second refresh for unchanged polled records. Cached identity is also
included in live snapshots for newly connected monitors, with its actual age.

Typed diagnostics use `PREFIX/SITE/inverter/MEMBER/protocol/COMMAND`.
Each nonretained QoS 1 message contains one complete observation, up to 96
fields, with `record_complete:true` and `sample_age_ms`. The gateway assembles
and validates all internal radio/cable chunks before publishing; partial
records never update the live cache. Live snapshots also carry these complete
records in `native_records`. Sequence deduplication handles MQTT redelivery,
and cached sample ages prevent old data from becoming fresh. The Python
monitor accepts the previous chunked MQTT format for older firmware. See
[the shared MQTT reference](../third_party/mpp-solar/README.txt) for complete
payload examples, limits and queue counters.

The cache retains up to 32 distinct query observations, enough for every fixed
background query in each profile. If a large snapshot exceeds the 32 KiB
internal-RAM JSON buffer, it omits duplicated metadata section arrays and sets
`metadata_in_native_records:true`. A further compact form omits the duplicated
`sections` object and sets `sections_in_native_records:true`. Both forms retain
every complete observation in `native_records`; the monitor reconstructs the
same groups and values. The 48 KiB PSRAM buffer fits the full tested profile
snapshots. Neither form publishes partial observations or drops physical zero.

## AOHAI Modbus RTU

The existing backend retains register decoding, FC03/FC04 monitoring,
FC06/FC10 writes, CRC checking, inverter-state checks, expected-value checks
and catalog write guards. It is not passed through an ASCII protocol codec.

Semantic operations include buzzer, Bluetooth, inverter enable, overload
restart, overload bypass, battery type, ECO mode, overtemperature restart,
input-change reminder, split phase, generator input, dual-channel load and
grid feedback. Some settings require fresh standby/stopped state; battery
type also requires the commissioning interlock. Catalog-backed operations
retain documented access/ranges and guarded/commissioning/service
interlocks. Readback can report confirmed, adjusted or pending outcomes.
The [existing command examples](../main/README.md) describe the AOHAI JSON
forms and guarded register operations.

## JK, Daly and VE.Direct commands

Daly cell voltage and temperature groups poll automatically after a valid
status reply supplies their respective counts. Each read uses that count to
require the complete frame group and omit unused trailing slots. Reconnecting
forgets the old counts; a missing count does not create an empty or zero-filled
cell observation. The normal monitor exposes these physical cells directly.

Commands and parameters below are case-sensitive. No raw register writer,
password writer or undocumented setting is exposed by these codecs.

| Profiles | Poll commands in order | Additional reads / behavior |
| --- | --- | --- |
| `JK02`, `JK02_32`, `JK04`, `JKV11` | `getInfo`, `getCellData` | `getInfo` starts the notification stream after connecting; the four response layouts are decoded separately. |
| `JK232` | `getBalancerData`, `getCellData`, `getInfo` | `getStatus` aliases basic status. DD/77 packets use the documented two's-complement checksum. |
| `JK485` | `getBalancerData` | 55AA request / EB90 response family; this is the balancer protocol, not the tagged JK BMS frame. |
| `JKPB` | `getCellData`, `getAdditionalInfo`, `getBalancerData` | `getInfo` aliases device information. Modbus FC10 triggers reads; the 300-byte record and Modbus echo are checked separately. |
| `JKSERIAL` | `getBalancerData` | `getCellData` aliases the same read. 4E57 tagged reply; cell count comes from the payload and the current-encoding version is respected. |
| `DALY`, `DALY40` | `SOC`, `cellMinMaxVoltages`, `cellMinMaxTemps`, `mosStatus`, `status`, `cellBalanceState`, `batteryFailureStatus` | `cellVoltages`, `cellTemperatures` require group counts obtained from `status`; the whole ordered group must arrive. |
| `VED` | `vedtext` | Passive read sends no fake command. `batteryCapacity` performs a documented HEX read on supported BMV/SmartShunt products. |

JK BLE publishes cell voltages/resistance, pack measurements, temperatures,
SOC/capacity/cycles and charge/discharge states where the chosen layout
defines them. JK04 publishes its float cell-voltage/balancer layout. Device
information does not publish authentication passwords. JKPB publishes only
grounded status/settings offsets; unidentified record bytes are not turned
into invented fields. Daly publishes cell extrema, pack voltage/current/SOC,
MOS state, counts, cell balancing/fault fields and optional cell/temperature
groups. VE.Direct TEXT emits recognized numeric fields with their physical
units and preserves other printable records as text.

| Profile | Setter syntax | Accepted parameter | Readback |
| --- | --- | --- | --- |
| `JK02` | `setChargingOn`, `setChargingOff` | Fixed switches | `getCellData` |
| `JK02` | `setDischargingOn`, `setDischargingOff` | Fixed switches | `getCellData` |
| `JK485` | `setCellCount<N>` | Integer 2..24 cells | `getBalancerData` |
| `JK485` | `setBalanceTrigger<N>` | Integer 2..1000 mV | `getBalancerData` |
| `JK485` | `setMaxBalanceCurrent<N>` | Integer 30..1000 mA | `getBalancerData` |
| `JK485` | `setBalanceOn`, `setBalanceOff` | Fixed switches | `getBalancerData` |
| `VED` | `setBatteryCapacity<N>` | Integer 1..9999 Ah | `batteryCapacity` |

For example, `setBalanceTrigger10` sets 10 mV and `setBatteryCapacity200`
requests 200 Ah. Signs, exponent notation, whitespace and trailing junk are
rejected. The JK485 write echo must match the requested value. JK02 ACKs
use the captured response format. VE.Direct checks the HEX reply flags and
returned register/value. Battery-capacity register 0x1000 belongs to the
documented BMV/SmartShunt protocol; another VE.Direct product may reject it.
JK02_32/JKV11 numerical writes, JK232/JKSERIAL password-authenticated settings,
JKPB writes and Daly writes have no validated setter contract here and remain
unsupported.

## Framing and model restrictions

* PI16 uses plain CR-terminated requests and the documented decimal byte-sum
  on historical energy queries; replies support the documented optional CRC.
* PI17 uses length-prefixed ^P/^S frames. Declared lengths keep an embedded CR
  checksum byte from terminating the response early.
* PI18 respects command/variant transmit-CRC policy. PI18LVX's default omission
  of transmit CRC still requires a valid reply checksum.
* PI30/PI41 use escaped XModem CRC. REVO has CRC and byte-sum variants and a
  PSET packet without CR. Native framing handles checksum delimiter bytes.
* JK BLE uses fragmented notifications, an additive packet checksum and a
  configured peer MAC. FFE0/FFE1 are the defaults; UUID properties and the
  notification descriptor are discovered. UART emulation is not a replacement
  for the BLE profiles.
* Daly responses use 13-byte additive-checksum packets. Address 0x80 and 0x40
  are different request variants. Cell/temperature packet groups cannot be
  considered complete after their first packet.
* VE.Direct TEXT checksum covers the complete block, including its terminal
  binary checksum byte. Interleaved HEX messages are excluded. HEX frames use
  their own 0x55 additive checksum and little-endian registers/values.

## Validation and sources

Run the focused host checks from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-PiProtocols.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-BmsProtocols.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-EnergyTransport.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-EnergyConfiguration.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-ProtocolWire.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-ProtocolCommandHistory.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-EnergyPollSchedule.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-MqttNativeProtocols.ps1
node tools/tests/inverter_selection_ui_test.js
```

Host codec tests cover request framing, actual variant layouts, checksums,
fragmentation/truncation, setter allowlists, invalid values, ACK/rejection,
readback metadata and VE.Direct official examples. The BMS suite checks 27
valid recorded packets and rejects three truncated source packets. Native
mesh/coordinator tests cover malformed typed messages, bounded chunk metadata,
packet sizes and not-sent/uncertain failure attribution. These are software
checks; they do not establish physical HID enumeration, UART electrical
compatibility, Bluetooth connection behavior or settings acceptance on every
inverter/BMS firmware. Each installation needs its own device validation.

The reference catalog is [jblance/mpp-solar](https://github.com/jblance/mpp-solar),
revision `eafdd4328c77f516cf82430c5bcb6fa042c45d3a`. The
[upstream MIT notice](../third_party/mpp-solar/LICENSE) covers derived command
metadata and recorded fixtures. Manufacturer JK/JBD and Daly communication
documents are preserved in the upstream
[protocol documents](https://github.com/jblance/mpp-solar/tree/eafdd4328c77f516cf82430c5bcb6fa042c45d3a/docs/protocols).
Additional primary implementation references are
[the JK BLE protocol implementation](https://github.com/syssi/esphome-jk-bms/blob/main/components/jk_bms_ble/jk_bms_ble.cpp)
and [the JK-PB protocol author's notes](https://github.com/mr-manuel/venus-os_dbus-serialbattery/blob/master/bms-docs/JKBMS-PB.md).
VE.Direct follows Victron's
[VE.Direct protocol specification](https://www.victronenergy.com/upload/documents/VE.Direct-Protocol-3.34.pdf),
[BMV HEX protocol](https://www.victronenergy.com/upload/documents/BMV-7xx-HEX-Protocol.pdf)
and [official interface FAQ](https://www.victronenergy.com/live/vedirect_protocol:faq).

## PI query and settings catalog

The following catalog is resolved from the same upstream constants as the
checked-in native metadata. Query names with parameters show their full
matching expressions. Setter patterns use regular-expression syntax:
`\d` is a decimal digit; `[0-2]` restricts an enum; parentheses group values;
`|` selects alternatives; `$` ends the command. Send the actual matching
command, not the pattern text. Control characters are always rejected. DAT
also needs a valid calendar date, and PE/PD feature letters must be recognized
and unrepeated. Numeric regex bounds establish documented syntax; they do
not establish the right voltage/current for every battery or inverter model.

Readback entries name an observation request. A dash means the upstream
catalog provides no relevant readback mapping in this implementation.

### PI16 monitoring

Automatic poll: `QPIGS`, `QMOD`, `QPIRI`, `QPI`, `QID`, `QVFW`, `QVFW2`, `QGMN`, `QPIBI`.

Available native queries: `QEH(\d\d\d\d\d\d\d\d\d\d)$`, `QED(\d\d\d\d\d\d\d\d)$`, `QEM(\d\d\d\d\d\d)$`, `QEY(\d\d\d\d)$`, `QID`, `QGMN`, `QMOD`, `QPIBI`, `QPIGS`, `QPI`, `QPIRI`, `QVFW`, `QVFW2`.

### PI17 monitoring

Automatic poll: `GS`, `PS`, `MOD`, `WS`, `BATS`, `FLAG`, `PIRI`, `PI`, `ID`, `VFW`, `VFW2`, `MD`, `DM`, `INGS`, `EMINFO`, `BMS`, `GPMP`, `HECS`, `DI`, `ET`, `T`.

Available native queries: `GPMP`, `PI`, `ID`, `VFW`, `VFW2`, `MD`, `DI`, `DM`, `INGS`, `EMINFO`, `PIRI`, `GS`, `PS`, `MOD`, `WS`, `FLAG`, `T`, `ET`, `BATS`, `HECS`, `EY(\d\d\d\d)$`, `EM(\d\d\d\d\d\d)$`, `ED(\d\d\d\d\d\d\d\d)$`, `EH(\d\d\d\d\d\d\d\d\d\d)$`, `BMS`.

### PI17M058 monitoring

Automatic poll: `GS`, `PS`, `MOD`, `WS`, `BATS`, `FLAG`, `PIRI`, `PI`, `ID`, `VFW`, `VFW2`, `MD`, `DM`, `INGS`, `EMINFO`, `BMS`, `GPMP`, `HECS`, `DI`, `ET`, `T`.

Available native queries: `GPMP`, `PI`, `ID`, `VFW`, `VFW2`, `MD`, `DI`, `DM`, `INGS`, `EMINFO`, `PIRI`, `GS`, `PS`, `MOD`, `WS`, `FLAG`, `T`, `ET`, `BATS`, `HECS`, `EY(\d\d\d\d)$`, `EM(\d\d\d\d\d\d)$`, `ED(\d\d\d\d\d\d\d\d)$`, `EH(\d\d\d\d\d\d\d\d\d\d)$`, `BMS`.

### PI17INFINI monitoring

Automatic poll: `GS`, `PS`, `MOD`, `WS`, `BATS`, `FLAG`, `PIRI`, `PI`, `ID`, `VFW`, `VFW2`, `MD`, `DM`, `INGS`, `EMINFO`, `BMS`, `GPMP`, `HECS`, `DI`, `ET`, `T`.

Available native queries: `GPMP`, `PI`, `ID`, `VFW`, `VFW2`, `MD`, `DI`, `DM`, `INGS`, `EMINFO`, `PIRI`, `GS`, `PS`, `MOD`, `WS`, `FLAG`, `T`, `ET`, `BATS`, `HECS`, `EY(\d\d\d\d)$`, `EM(\d\d\d\d\d\d)$`, `ED(\d\d\d\d\d\d\d\d)$`, `EH(\d\d\d\d\d\d\d\d\d\d)$`, `BMS`.

### PI18 monitoring

Automatic poll: `GS`, `MOD`, `PIRI`, `FLAG`, `PI`, `ID`, `VFW`, `MCHGCR`, `MUCHGCR`, `ET`.

Available native queries: `PI`, `ET`, `EY(\d\d\d\d)$`, `EM(\d\d\d\d\d\d)$`, `ED(\d\d\d\d\d\d\d\d)$`, `ID`, `VFW`, `PIRI`, `GS`, `MOD`, `FLAG`, `MCHGCR`, `MUCHGCR`.

### PI18LVX monitoring

Automatic poll: `GS`, `MOD`, `FWS`, `PIRI`, `FLAG`, `PI`, `ID`, `VFW`, `DI`, `MCHGCR`, `MUCHGCR`, `ACCT`, `ACLT`, `ET`, `T`.

Available native queries: `PI`, `T`, `ET`, `EY(\d\d\d\d)$`, `EM(\d\d\d\d\d\d)$`, `ED(\d\d\d\d\d\d\d\d)$`, `ID`, `VFW`, `MCHGCR`, `MUCHGCR`, `PRI(\d)$`, `PGS(\d)$`, `FWS`, `DI`, `PIRI`, `GS`, `MOD`, `FLAG`, `ACCT`, `ACLT`.

### PI18SV monitoring

Automatic poll: `GS`, `MOD`, `FWS`, `PIRI`, `FLAG`, `PI`, `ID`, `VFW`, `DI`, `MCHGCR`, `MUCHGCR`, `ACCT`, `ACLT`, `ET`, `T`.

Available native queries: `PI`, `T`, `ET`, `EY(\d\d\d\d)$`, `EM(\d\d\d\d\d\d)$`, `ED(\d\d\d\d\d\d\d\d)$`, `ID`, `VFW`, `MCHGCR`, `MUCHGCR`, `PRI(\d)$`, `PGS(\d)$`, `FWS`, `DI`, `PIRI`, `GS`, `MOD`, `FLAG`, `ACCT`, `ACLT`.

### PI30 monitoring

Automatic poll: `QPIGS`, `QMOD`, `QPIWS`, `QPIRI`, `QFLAG`, `QPI`, `QID`, `QVFW`, `QVFW2`, `QGMN`, `QMN`, `QBOOT`, `Q1`, `QBMS`, `QDI`, `QMCHGCR`, `QMUCHGCR`, `QOPM`.

Available native queries: `Q1`, `QBOOT`, `QDI`, `QFLAG`, `QID`, `QMCHGCR`, `QMOD`, `QMN`, `QGMN`, `QMUCHGCR`, `QOPM`, `QPGS(\d+)$`, `QPI`, `QPIGS`, `QPIRI`, `QPIWS`, `QVFW`, `QVFW2`, `QBMS`.

### PI30MAX monitoring

Automatic poll: `QPIGS`, `QPIGS2`, `QMOD`, `QPIWS`, `QPIRI`, `QFLAG`, `QPI`, `QSID`, `QVFW`, `QVFW3`, `VERFW`, `QGMN`, `QMN`, `QBOOT`, `Q1`, `QBMS`, `QDI`, `QMCHGCR`, `QMUCHGCR`, `QOPM`, `QOPPT`, `QCHPT`, `QBEQI`, `QLED`, `QET`, `QLT`, `QT`.

Available native queries: `Q1`, `QBOOT`, `QDI`, `QFLAG`, `QID`, `QMCHGCR`, `QMOD`, `QMN`, `QGMN`, `QMUCHGCR`, `QOPM`, `QPGS(\d+)$`, `QPI`, `QPIGS`, `QPIRI`, `QPIWS`, `QVFW`, `QBMS`, `QSID`, `QVFW3`, `VERFW`, `QPIGS2`, `QOPPT`, `QCHPT`, `QT`, `QBEQI`, `QET`, `QEY(\d\d\d\d)$`, `QEM(\d\d\d\d\d\d)$`, `QED(\d\d\d\d\d\d\d\d)$`, `QLT`, `QLY(\d\d\d\d)$`, `QLM(\d\d\d\d\d\d)$`, `QLD(\d\d\d\d\d\d\d\d)$`, `QLED`.

### PI30MST monitoring

Automatic poll: `QPIGS`, `QPIGS2`, `QMOD`, `QPIWS`, `QPIRI`, `QFLAG`, `QPI`, `QSID`, `QVFW`, `QVFW3`, `VERFW`, `QGMN`, `QMN`, `QBOOT`, `Q1`, `QBMS`, `QDI`, `QMCHGCR`, `QMUCHGCR`, `QOPM`, `QOPPT`, `QCHPT`, `QBEQI`, `QLED`, `QET`, `QLT`, `QT`.

Available native queries: `Q1`, `QBOOT`, `QDI`, `QFLAG`, `QID`, `QMCHGCR`, `QMOD`, `QMN`, `QGMN`, `QMUCHGCR`, `QOPM`, `QPGS(\d+)$`, `QPI`, `QPIGS`, `QPIRI`, `QPIWS`, `QVFW`, `QBMS`, `QSID`, `QVFW3`, `VERFW`, `QPIGS2`, `QOPPT`, `QCHPT`, `QT`, `QBEQI`, `QET`, `QEY(\d\d\d\d)$`, `QEM(\d\d\d\d\d\d)$`, `QED(\d\d\d\d\d\d\d\d)$`, `QLT`, `QLY(\d\d\d\d)$`, `QLM(\d\d\d\d\d\d)$`, `QLD(\d\d\d\d\d\d\d\d)$`, `QLED`.

### PI30REVO monitoring

Automatic poll: `QPIGS`, `QALL`, `QMOD`, `QPIRI`, `PQSE`, `QPI`, `QLITH0`.

Available native queries: `PQSE`, `QALL`, `QMOD`, `QPI`, `QPIGS`, `QPIRI`, `QLITH0`.

### PI30M044 monitoring

Automatic poll: `QPIGS`, `QPIGS2`, `QMOD`, `QPIWS`, `QPIRI`, `QFLAG`, `QPI`, `QSID`, `QVFW`, `QVFW3`, `VERFW`, `QGMN`, `QMN`, `QBOOT`, `Q1`, `QBMS`, `QDI`, `QMCHGCR`, `QMUCHGCR`, `QOPM`, `QOPPT`, `QCHPT`, `QBEQI`, `QLED`, `QDOP`, `QET`, `QLT`, `QT`.

Available native queries: `Q1`, `QBOOT`, `QDI`, `QFLAG`, `QID`, `QMCHGCR`, `QMOD`, `QMN`, `QGMN`, `QMUCHGCR`, `QOPM`, `QPGS(\d+)$`, `QPI`, `QPIGS`, `QPIRI`, `QPIWS`, `QVFW`, `QBMS`, `QSID`, `QVFW3`, `VERFW`, `QPIGS2`, `QOPPT`, `QCHPT`, `QT`, `QBEQI`, `QET`, `QEY(\d\d\d\d)$`, `QEM(\d\d\d\d\d\d)$`, `QED(\d\d\d\d\d\d\d\d)$`, `QLT`, `QLY(\d\d\d\d)$`, `QLM(\d\d\d\d\d\d)$`, `QLD(\d\d\d\d\d\d\d\d)$`, `QLED`, `QDOP`.

### PI30M045 monitoring

Automatic poll: `QPIGS`, `QPIGS2`, `QMOD`, `QPIWS`, `QPIRI`, `QFLAG`, `QPI`, `QSID`, `QVFW`, `QVFW3`, `VERFW`, `QGMN`, `QMN`, `QBOOT`, `Q1`, `QBMS`, `QDI`, `QMCHGCR`, `QMUCHGCR`, `QOPM`, `QOPPT`, `QCHPT`, `QBEQI`, `QLED`, `QET`, `QLT`, `QT`.

Available native queries: `Q1`, `QBOOT`, `QDI`, `QFLAG`, `QID`, `QMCHGCR`, `QMOD`, `QMN`, `QGMN`, `QMUCHGCR`, `QOPM`, `QPGS(\d+)$`, `QPI`, `QPIGS`, `QPIRI`, `QPIWS`, `QVFW`, `QBMS`, `QSID`, `QVFW3`, `VERFW`, `QPIGS2`, `QOPPT`, `QCHPT`, `QT`, `QBEQI`, `QET`, `QEY(\d\d\d\d)$`, `QEM(\d\d\d\d\d\d)$`, `QED(\d\d\d\d\d\d\d\d)$`, `QLT`, `QLY(\d\d\d\d)$`, `QLM(\d\d\d\d\d\d)$`, `QLD(\d\d\d\d\d\d\d\d)$`, `QLED`.

### PI41 monitoring

Automatic poll: `QPIGS`, `QPIGS2`, `QMOD`, `QPIWS`, `QPIRI`, `QFLAG`, `QPI`, `QID`, `QVFW`, `QVFW2`, `QGMN`, `QMN`, `QBOOT`, `Q1`, `QBMS`, `QDI`, `QMCHGCR`, `QMUCHGCR`, `QOPM`.

Available native queries: `Q1`, `QBOOT`, `QDI`, `QFLAG`, `QID`, `QMCHGCR`, `QMOD`, `QMN`, `QGMN`, `QMUCHGCR`, `QOPM`, `QPGS(\d)$`, `QPI`, `QPIGS`, `QPIRI`, `QPIWS`, `QVFW`, `QVFW2`, `QBMS`, `QP2GS(\d)$`, `QPIGS2`.

### PI16 settings

No settings commands are defined in the referenced upstream catalog.

### PI17, PI17M058 settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `GPMP0` | `GPMP(0[10]\d\d\d\d)$` | `GPMP` |
| `LON` | `LON([01])$` | `PS` |
| `PA` | `PA([01])$` | `FLAG` |
| `PB` | `PB([01])$` | `FLAG` |
| `PC` | `PC([01])$` | `FLAG` |
| `PD` | `PD([A-Z])$` | `FLAG` |
| `PE` | `PE([A-Z])$` | `FLAG` |
| `PF` | `PF([01])$` | - |
| `DAT` | `DAT(\d\d\d\d\d\d\d\d\d\d\d\d)$` | `T` |
| `LST` | `LST((0[123])\|([12]0))$` | - |
| `EDA` | `EDA([01])$` | - |
| `EDB` | `EDB([01])$` | - |
| `EDC` | `EDC([01])$` | - |
| `EDD` | `EDD([01])$` | - |
| `EDE` | `EDE([01])$` | - |
| `EDF` | `EDF([01])$` | - |
| `EDG` | `EDG([01])$` | - |
| `BT` | `BT([01])$` | `BATS` |
| `MCHGV` | `MCHGV(0[45]\d\d,0[45]\d\d)$` | `BATS` |
| `ACCT` | `ACCT((2[0-3]\|[01]?[0-9])([0-5]?[0-9])-(2[0-3]\|[01]?[0-9])([0-5]?[0-9]))$` | - |
| `ACCB` | `ACCB([01],0[456]\d\d)$` | `BATS` |
| `MCHGC` | `MCHGC([012]\d\d\d)$` | `BATS` |
| `MUCHGC` | `MUCHGC([012]\d\d\d)$` | `BATS` |
| `SEP` | `SEP(0[012])$` | `BATS` |
| `BDCM` | `BDCM(0[012]\d\d)$` | `BATS` |
| `BATDV` | `BATDV(0[45]\d\d,0[45]\d\d,0[45]\d\d,0[45]\d\d)$` | `BATS` |
| `BCA` | `BCA(0\d\d\d,0\d\d,0\d\d\d)$` | `BATS` |

### PI17INFINI settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `GPMP0` | `GPMP(0[10]\d\d\d\d)$` | `GPMP` |
| `LON` | `LON([01])$` | `PS` |
| `PC` | `PC([01])$` | `FLAG` |
| `PD` | `PD([A-Z])$` | `FLAG` |
| `PE` | `PE([A-Z])$` | `FLAG` |
| `PF` | `PF([01])$` | - |
| `DAT` | `DAT(\d\d\d\d\d\d\d\d\d\d\d\d)$` | `T` |
| `LST` | `LST((0[123])\|([12]0))$` | - |
| `EDA` | `EDA([01])$` | - |
| `EDB` | `EDB([01])$` | - |
| `EDC` | `EDC([01])$` | - |
| `EDD` | `EDD([01])$` | - |
| `EDE` | `EDE([01])$` | - |
| `EDF` | `EDF([01])$` | - |
| `EDG` | `EDG([01])$` | - |
| `BT` | `BT([01])$` | `BATS` |
| `MCHGV` | `MCHGV(0[45]\d\d,0[45]\d\d)$` | `BATS` |
| `ACCT` | `ACCT((2[0-3]\|[01]?[0-9])([0-5]?[0-9]),(2[0-3]\|[01]?[0-9])([0-5]?[0-9]),(2[0-3]\|[01]?[0-9])([0-5]?[0-9]),(2[0-3]\|[01]?[0-9])([0-5]?[0-9]))$` | - |
| `ACCB` | `ACCB([01],0[456]\d\d)$` | `BATS` |
| `MCHGC` | `MCHGC([012]\d\d\d)$` | `BATS` |
| `MUCHGC` | `MUCHGC([012]\d\d\d)$` | `BATS` |
| `SEP` | `SEP(0[012])$` | `BATS` |
| `BDCM` | `BDCM(0[012]\d\d)$` | `BATS` |
| `BATDV` | `BATDV(0[45]\d\d,0[45]\d\d,0[45]\d\d,0[45]\d\d)$` | `BATS` |
| `BCA` | `BCA(0\d\d\d,0\d\d,0\d\d\d)$` | `BATS` |
| `PEA` | `PEA` | `FLAG` |
| `PDA` | `PDA` | `FLAG` |

### PI18 settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `POP` | `POP([01])$` | `PIRI` |
| `PSP` | `PSP([01])$` | `PIRI` |
| `PEI` | `PEI` | `PIRI` |
| `PDI` | `PDI` | `PIRI` |
| `PCP` | `PCP([0-9],[012])$` | `PIRI` |
| `MCHGC` | `MCHGC([0-9],0[1-8]0)$` | `PIRI` |
| `MUCHGC` | `MUCHGC([0-9]),(002\|0[1-8]0)$` | `PIRI` |
| `PBT` | `PBT([012])$` | `PIRI` |
| `MCHGV` | `MCHGV(4[8-9][0-9]\|5[0-7][0-9]\|58[0-5]),(4[8-9][0-9]\|5[0-7][0-9]\|58[0-4])$` | `PIRI` |
| `PSDV` | `PSDV(4[0-7][0-9]\|480)$` | `PIRI` |
| `BUCD` | `BUCD((4[4-9]0\|5[0-1]0),(000\|4[8-9]0\|5[0-8]0))$` | `PIRI` |

### PI18LVX, PI18SV settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `LON` | `LON([01])$` | `GS` |
| `PEA` | `PEA` | `FLAG` |
| `PDA` | `PDA` | `FLAG` |
| `PEB` | `PEB` | `FLAG` |
| `PDB` | `PDB` | `FLAG` |
| `PEC` | `PEC` | `FLAG` |
| `PDC` | `PDC` | `FLAG` |
| `PED` | `PED` | `FLAG` |
| `PDD` | `PDD` | `FLAG` |
| `PEE` | `PEE` | `FLAG` |
| `PDE` | `PDE` | `FLAG` |
| `PEF` | `PEF` | `FLAG` |
| `PDF` | `PDF` | `FLAG` |
| `PEG` | `PEG` | `FLAG` |
| `PDG` | `PDG` | `FLAG` |
| `PEH` | `PEH` | `FLAG` |
| `PDH` | `PDH` | `FLAG` |
| `PEI` | `PEI` | `PIRI` |
| `PDI` | `PDI` | `PIRI` |
| `PF` | `PF` | `PIRI` |
| `MCHGC` | `MCHGC(\d,\d\d\d)$` | `PIRI` |
| `MUCHGC` | `MUCHGC(\d,\d\d\d)$` | `PIRI` |
| `MCHGV` | `MCHGV(\d\d\d,\d\d\d)$` | `PIRI` |
| `DAT` | `DAT(\d\d\d\d\d\d\d\d\d\d\d\d)$` | `T` |
| `POP` | `POP(\d)$` | `PIRI` |
| `BUCD` | `BUCD(\d\d\d,\d\d\d)$` | `PIRI` |

### PI30, PI41 settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `F` | `F([56]0)$` | `QPIRI` |
| `MCHGC` | `MCHGC(\d\d\d)$` | `QPIRI` |
| `MNCHGC` | `MNCHGC(\d\d\d\d)$` | `QPIRI` |
| `MUCHGC` | `MUCHGC(\d\d\d)$` | `QPIRI` |
| `PBCV` | `PBCV(\d\d\.\d)$` | `QPIRI` |
| `PBDV` | `PBDV(\d\d\.\d)$` | `QPIRI` |
| `PBFT` | `PBFT(\d\d\.\d)$` | `QPIRI` |
| `PBT` | `PBT(0[012])$` | `QPIRI` |
| `PCP` | `PCP(0[0123])$` | `QPIRI` |
| `PCVV` | `PCVV(\d\d\.\d)$` | `QPIRI` |
| `PE` | `PE(.+)$` | `QFLAG` |
| `PD` | `PD(.+)$` | `QFLAG` |
| `PF` | `PF` | `QPIRI` |
| `PGR` | `PGR(0[01])$` | `QPIRI` |
| `POP` | `POP(0[012])$` | `QPIRI` |
| `POPLG` | `POPLG(0[012])$` | `QPIRI` |
| `POPM` | `POPM(\d[01234])$` | `QPIRI` |
| `PPCP` | `PPCP(\d0[0123])$` | `QPIRI` |
| `PPVOKC` | `PPVOKC([01])$` | `QPIRI` |
| `PSDV` | `PSDV(\d\d\.\d)$` | `QPIRI` |
| `PSPB` | `PSPB([01])$` | `QPIRI` |
| `PBATCD` | `PBATCD([01][01][01])$` | `QBMS` |
| `DAT` | `DAT(\d\d\d\d\d\d\d\d\d\d\d\d\d\d)$` | - |
| `PBATMAXDISC` | `PBATMAXDISC([01]\d\d)$` | `QPIRI` |
| `BTA` | `BTA([-+]0\d)$` | - |
| `PSAVE` | `PSAVE` | - |

### PI30MAX, PI30MST settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `F` | `F([56]0)$` | `QPIRI` |
| `MCHGC` | `MCHGC(\d\d\d)$` | `QPIRI` |
| `MNCHGC` | `MNCHGC(\d\d\d\d)$` | `QPIRI` |
| `MUCHGC` | `MUCHGC(\d\d\d)$` | `QPIRI` |
| `PBCV` | `PBCV(\d\d\.\d)$` | `QPIRI` |
| `PBDV` | `PBDV(\d\d\.\d)$` | `QPIRI` |
| `PBFT` | `PBFT(\d\d\.\d)$` | `QPIRI` |
| `PBT` | `PBT(0[012])$` | `QPIRI` |
| `PCP` | `PCP(0[0123])$` | `QPIRI` |
| `PCVV` | `PCVV(\d\d\.\d)$` | `QPIRI` |
| `PE` | `PE(.+)$` | `QFLAG` |
| `PD` | `PD(.+)$` | `QFLAG` |
| `PF` | `PF` | `QPIRI` |
| `PGR` | `PGR(0[01])$` | `QPIRI` |
| `POP` | `POP(0[012])$` | `QPIRI` |
| `POPLG` | `POPLG(0[012])$` | `QPIRI` |
| `POPM` | `POPM(\d[01234])$` | `QPIRI` |
| `PPCP` | `PPCP(\d0[0123])$` | `QPIRI` |
| `PPVOKC` | `PPVOKC([01])$` | `QPIRI` |
| `PSDV` | `PSDV(\d\d\.\d)$` | `QPIRI` |
| `PSPB` | `PSPB([01])$` | `QPIRI` |
| `PBATCD` | `PBATCD([01][01][01])$` | `QBMS` |
| `DAT` | `DAT(\d\d\d\d\d\d\d\d\d\d\d\d\d\d)$` | `QT` |
| `PBATMAXDISC` | `PBATMAXDISC([01]\d\d)$` | `QPIRI` |
| `BTA` | `BTA([-+]0\d)$` | - |
| `PSAVE` | `PSAVE` | - |
| `PLEDE` | `PLEDE([01])$` | `QLED` |
| `PLEDS` | `PLEDS([012])$` | `QLED` |
| `PLEDM` | `PLEDM([0123])$` | `QLED` |
| `PLEDB` | `PLEDB([123456789])$` | `QLED` |
| `PLEDT` | `PLEDT([123])$` | `QLED` |
| `PLEDC` | `PLEDC(\d\d\d\d\d\d\d\d\d\d)$` | `QLED` |

### PI30REVO settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `PSET` | `PSET(\d+ \d+[.]\d+ \d+[.]\d+ \d+[.]\d+ \d+[.]\d+ \d+ \d+ \d+ \d+ \d+ \d+ \d+.*)$` | `PQSE` |
| `_PSET` | `_PSET(\d+ \d+[.]\d+ \d+[.]\d+ \d+[.]\d+ \d+[.]\d+ \d+ \d+ \d+ \d+ \d+ \d+ \d+.*)$` | `PQSE` |

### PI30M044 settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `F` | `F([56]0)$` | `QPIRI` |
| `MCHGC` | `MCHGC(\d\d\d)$` | `QPIRI` |
| `MNCHGC` | `MNCHGC(\d\d\d\d)$` | `QPIRI` |
| `MUCHGC` | `MUCHGC(\d\d\d)$` | `QPIRI` |
| `PBCV` | `PBCV(\d\d\.\d)$` | `QPIRI` |
| `PBDV` | `PBDV(\d\d\.\d)$` | `QPIRI` |
| `PBFT` | `PBFT(\d\d\.\d)$` | `QPIRI` |
| `PBT` | `PBT(0[0-9])$` | `QPIRI` |
| `PCP` | `PCP(0[0123])$` | `QPIRI` |
| `PCVV` | `PCVV(\d\d\.\d)$` | `QPIRI` |
| `PE` | `PE(.+)$` | `QFLAG` |
| `PD` | `PD(.+)$` | `QFLAG` |
| `PF` | `PF` | `QPIRI` |
| `PGR` | `PGR(0[01])$` | `QPIRI` |
| `POP` | `POP(0[012])$` | `QPIRI` |
| `POPLG` | `POPLG(0[012])$` | `QPIRI` |
| `POPM` | `POPM(\d[01234])$` | `QPIRI` |
| `PPCP` | `PPCP(\d0[0123])$` | `QPIRI` |
| `PPVOKC` | `PPVOKC([01])$` | `QPIRI` |
| `PSDV` | `PSDV(\d\d\.\d)$` | `QPIRI` |
| `PSPB` | `PSPB([01])$` | `QPIRI` |
| `PBATCD` | `PBATCD([01][01][01])$` | `QBMS` |
| `DAT` | `DAT(\d\d\d\d\d\d\d\d\d\d\d\d\d\d)$` | `QT` |
| `PBATMAXDISC` | `PBATMAXDISC([01]\d\d)$` | `QPIRI` |
| `BTA` | `BTA([-+]0\d)$` | - |
| `PSAVE` | `PSAVE` | - |
| `PLEDE` | `PLEDE([01])$` | `QLED` |
| `PLEDS` | `PLEDS([012])$` | `QLED` |
| `PLEDM` | `PLEDM([0123])$` | `QLED` |
| `PLEDB` | `PLEDB([123456789])$` | `QLED` |
| `PLEDT` | `PLEDT([123])$` | `QLED` |
| `PLEDC` | `PLEDC(\d\d\d\d\d\d\d\d\d\d)$` | `QLED` |

### PI30M045 settings

| Command family | Accepted complete command pattern | Readback |
| --- | --- | --- |
| `F` | `F([56]0)$` | `QPIRI` |
| `MCHGC` | `MCHGC(\d\d\d)$` | `QPIRI` |
| `MNCHGC` | `MNCHGC(\d\d\d\d)$` | `QPIRI` |
| `MUCHGC` | `MUCHGC(\d[01]\d[02])$` | `QPIRI` |
| `PBCV` | `PBCV(\d\d\.\d)$` | `QPIRI` |
| `PBDV` | `PBDV(\d\d\.\d)$` | `QPIRI` |
| `PBFT` | `PBFT(\d\d\.\d)$` | `QPIRI` |
| `PBT` | `PBT(0[012])$` | `QPIRI` |
| `PCP` | `PCP(0[0123])$` | `QPIRI` |
| `PCVV` | `PCVV(\d\d\.\d)$` | `QPIRI` |
| `PE` | `PE(.+)$` | `QFLAG` |
| `PD` | `PD(.+)$` | `QFLAG` |
| `PF` | `PF` | `QPIRI` |
| `PGR` | `PGR(0[01])$` | `QPIRI` |
| `POP` | `POP(0[012])$` | `QPIRI` |
| `POPLG` | `POPLG(0[012])$` | `QPIRI` |
| `POPM` | `POPM(\d[01234])$` | `QPIRI` |
| `PPCP` | `PPCP(\d0[0123])$` | `QPIRI` |
| `PPVOKC` | `PPVOKC([01])$` | `QPIRI` |
| `PSDV` | `PSDV(\d\d\.\d)$` | `QPIRI` |
| `PSPB` | `PSPB([01])$` | `QPIRI` |
| `PBATCD` | `PBATCD([01][01][01])$` | `QBMS` |
| `DAT` | `DAT(\d\d\d\d\d\d\d\d\d\d\d\d)$` | `QT` |
| `PBATMAXDISC` | `PBATMAXDISC([01]\d\d)$` | `QPIRI` |
| `BTA` | `BTA([-+]0\d)$` | - |
| `PSAVE` | `PSAVE` | - |
| `PLEDE` | `PLEDE([01])$` | `QLED` |
| `PLEDS` | `PLEDS([012])$` | `QLED` |
| `PLEDM` | `PLEDM([0123])$` | `QLED` |
| `PLEDB` | `PLEDB([123456789])$` | `QLED` |
| `PLEDT` | `PLEDT([123])$` | `QLED` |
| `PLEDC` | `PLEDC(\d\d\d\d\d\d\d\d\d\d)$` | `QLED` |
