PI command definitions and attribution
====================================

Source: https://github.com/jblance/mpp-solar
Revision: eafdd4328c77f516cf82430c5bcb6fa042c45d3a
License: MIT; the complete notice is preserved in LICENSE beside this file.

main/src/protocol/pi_protocol_catalog.generated.inc is derived from the
upstream mppsolar/protocols/pi*.py command dictionaries. The generator reads
literal Python syntax with ast.literal_eval and resolves inheritance without
importing or executing upstream code. A normal firmware build uses the checked
in generated catalog and needs neither an upstream checkout nor Python.

Regeneration from a local reference checkout:

  python tools/generate_pi_protocol_catalog.py --source PATH_TO_MPP_SOLAR

Native codec validation:

  powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-PiProtocols.ps1

Shared MQTT API validation with real ESP-IDF cJSON and firmware codecs:

  powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-MqttNativeProtocols.ps1

The MQTT fixture extracts production function definitions unchanged at build
time and replaces only the queue, client, clock and ESP event boundaries. It
exercises command parsing, retained/fragment guards, write confirmation, shared
ID deduplication, telemetry/result JSON, outbox priorities and OTA draining.
Set -IdfPath if the local ESP-IDF installation differs from the script default.

Catalog coverage (queries/settings command patterns):

  PI16          13 /  0
  PI17          25 / 27
  PI17M058      25 / 27
  PI17INFINI    25 / 27
  PI18          13 / 11
  PI18LVX       20 / 26
  PI18SV        20 / 26
  PI30          19 / 26
  PI30MAX       35 / 32
  PI30MST       35 / 32
  PI30REVO       7 /  2
  PI30M044      36 / 32
  PI30M045      35 / 32
  PI41          21 / 26

This is the coverage defined by that upstream revision, not every command ever
implemented by every manufacturer firmware. PI16 has no upstream setters.
Protocols with shared framing retain distinct inherited/overridden response
layouts and setter patterns. Serial/UART and USB HID use the same PI codec.

Wire differences:

* PI16 sends plain CR-terminated requests, with a decimal ASCII byte-sum on
  historical energy queries. Replies may have their documented optional CRC.
* PI17 uses ^P/^S length prefixes and no transmit CRC; historical energy queries
  add a decimal byte-sum. Receive CRC uses unescaped XModem bytes, so framing
  uses the declared length even when a CRC byte equals CR.
* PI18 uses the catalog prefixes and escaped XModem CRC. PI18LVX defaults to
  transmit without CRC; PI18SV and individual commands have explicit policies.
  Omitting transmit CRC does not omit reply validation.
* PI30 and PI41 use escaped XModem CRC. REVO has CRC and byte-sum-plus-one
  commands; PSET omits CR. Its response reader supports CR/LF, including a
  checksum byte that equals either delimiter.

Known source corrections and bounds:

* PI16 QEH/QEM/QEY dictionary regexes accidentally start with QED; their actual
  documented command prefixes are restored.
* Double escaped regex digits and periods are normalized to match the command
  examples. Expressions compile into a bounded NFA; no runtime eval or general
  regular-expression engine is used.
* Setter packets reject control characters, unsupported patterns, invalid DAT
  calendar values and unknown/repeated PI30 feature flag letters. The native
  catalog can establish syntax and documented enum ranges; model-dependent
  battery/current limits still require the correct model/firmware selection.
* Missing numeric values are omitted instead of reported as zero. Encoded units
  such as 0.1V are converted to V while scaling the reading.
* Shared telemetry field strings are bounded to 31 bytes. Longer manufacturer
  option descriptions use 28 bytes plus "...". Names remain stable normalized
  keys; values may be numeric or textual.
* An ACK reports acceptance. Optional readback requests observe current settings
  without claiming that a generic multi-setting write was proved equal. A sent
  setting with a missing reply must remain uncertain; it must not be retried
  automatically.

tools/tests/pi_reference_vectors.generated.json preserves upstream examples
for reference. Some original examples contain fake checksums, malformed length
headers or encoding artifacts; they are not all valid wire packets. Native
tests use independent checksums and known correct source/manufacturer vectors.
Physical HID/serial devices still need model-specific hardware validation.
USB HID now discovers compatible bridges automatically without a factory VID/PID
restriction. Zero installer IDs mean automatic detection; optional nonzero IDs
are device filters under Advanced settings. Automatic mode requires nonboot,
vendor-defined, unnumbered eight-byte input/output HID reports. Explicit VID+PID
can allow atypical application usages while retaining the report checks. Actual
detected IDs are logged. The selected wire protocol is still tested by normal
readonly polling. Setting ACKs are command results, not monitoring observations.

Shared MQTT interface
=====================

The existing MQTT client, publisher task, topic prefix/site/member routing,
command-ID deduplication, outbox limit and OTA drain guard serve all profiles.
The working standalone AOHAI configuration does not allocate the extra native
telemetry queue. Routed roles and a native local profile allocate it when MQTT
starts; the small native result queue remains available for command rejections.

Request topic:

  PREFIX/SITE/inverter/MEMBER/command

Native monitoring request:

  {"command_id":"1001","operation":"protocol","protocol":"PI30","command":"QPIGS"}

Native settings request:

  {"command_id":"1002","operation":"protocol","protocol":"PI30","command":"POP02","confirmed":true}

The selected profile validates the complete native command; setter status is
inferred from its catalog. If an optional "write" boolean is included it must
match that inferred status. Every settings request requires confirmed:true.
The MQTT event handler rejects retained, fragmented or oversized requests.
Duplicate command IDs do not run again. A request to the wrong selected profile
is handled by the existing runtime routing/backend policy.

Result topic:

  PREFIX/SITE/inverter/MEMBER/result

Native results include command_id as a decimal string, member_id,
operation:"protocol", protocol, command, outcome, detail and write_sent.
write_sent:null means the route could not establish whether the remote write
was transmitted. Outcomes include completed, acknowledged, rejected,
unsupported, unavailable, not_sent, uncertain and invalid_response. An
acknowledged settings result is not an equality confirmation.

Typed telemetry topic:

  PREFIX/SITE/inverter/MEMBER/protocol/COMMAND

Commands with characters outside letters/digits/underscore use /protocol.
Each unretained QoS 1 message carries a complete reading, up to 96 fields:

  {"member_id":0,"protocol":"PI30","command":"QPIGS","sequence":42,
   "timestamp_ms":123456,"record_complete":true,"sample_age_ms":0,
   "fields":{"battery_voltage":{"unit":"V","value":57.5},
             "ac_output_active_power":{"unit":"W","value":119}}}

The gateway collects all internal USB/radio/cable chunks before publication.
Missing/conflicting pieces never update the live cache. Consumers deduplicate
by member + protocol + command + sequence. sample_age_ms describes the age of
the observation at publication, including cached records.

PREFIX/SITE/inverter/MEMBER/live publishes normal PV/grid/battery/output/load
sections, operating status, power summaries and native_records every five
seconds. Missing power is absent and stale summary power is null. Only actual
primary measurements establish core_status_fresh; identity and a mode reply
cannot make electrical data fresh. The monitor accepts legacy chunked MQTT
records as well as the new complete observations. Queue/outbox pressure and
outages can still lose readings; health reports protocol_queue_depth and
protocol_queue_drops. Results retain their existing QoS 1 priority.
