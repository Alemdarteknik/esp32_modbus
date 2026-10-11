# Firmware reliability changes: 1.0.6

This release hardens communication failures and exposes diagnostics. It does not
change MQTT `live` fields, power calculations, Modbus frame interpretation, or
the configured 850 ms minimum command gap. It is not fleet or hardware certification.

## USB/Modbus receive integrity

USB receive overflow is signalled before bytes from the affected callback are
published to the stream. The bus owner drains stale input and snapshots receive
integrity before transmission, then checks again after receiving bytes and before
accepting a read or write confirmation. A seemingly valid frame is rejected if
the transaction was affected by overflow. The next transaction drains/resynchronizes
without resetting the stream underneath its producer. Draining is bounded by the
occupancy observed when it starts.

`receive_busy` is append-only local client status 12: receive activity overlapped
the pre-transmission drain, so no write bytes were transmitted. Semantic results
report `write_sent: false`; catalog results can expose `modbus_status: 12`. This
is an ESP client status, not an inverter Modbus exception code.

An overflow during a transmitted write is different: the inverter may already
have applied the write, although its confirmation cannot be trusted. The firmware
does not automatically retry that write. Read the current state before deciding
whether another command is appropriate.

## Added health counters

Subscribe to the Internet-facing ESP's normal health topic:

```bash
mosquitto_sub -v -t 'inverter/INV-44B176CDC70C/inverter/0/health'
```

These eight counters are local to that ESP, cumulative since boot, and saturate
at 4,294,967,295 rather than wrap. They are independent atomic reads, not one
simultaneous snapshot. They are not forwarded health counters for each slave;
an Internet-only gateway can report zeros while remote inverters are operating.

| Field | Meaning |
|---|---|
| `usb_rx_dropped_bytes` | Bytes rejected by bounded USB receive admission. |
| `usb_rx_overflow_events` | USB receive callbacks that dropped at least one byte. |
| `usb_disconnect_count` | USB device-disconnected notifications. |
| `usb_reconnect_count` | Successful USB opens after the first successful open, including reopen after configuration failure. |
| `modbus_crc_errors` | Complete matched response frames with an invalid CRC, across reads/writes. |
| `modbus_timeouts` | Response deadline reached without a matched response prefix. |
| `modbus_incomplete_responses` | A matched partial response remained at the deadline. |
| `modbus_rx_overflow_failures` | Transactions/admissions invalidated by observed receive overflow; counted at most once per transaction/admission. |

Compare successive reports with serial warnings; one event is not necessarily
one physical cable fault. These fields supplement the existing JSON, MQTT, heap
and queue diagnostics. Delivery remains best-effort. The health JSON worst-case
format is 1005 bytes plus its terminator, within the existing 1024-byte buffer.

## Separate gateway/coordinator UART commands

Commands are not accepted into the coordinator transmit queue while the local
link is offline. Accepted queued commands carry a local observed-link generation
and a five-second queue age limit (`command_queue_max_age_ms = 5000`). Eligibility
is checked before transmitting and during the bounded FIFO transfer. An observed
link loss, generation change, expiry, or local transmit failure prevents replay
of an old queued command on a later connection. The link timeout remains 10 seconds.

UART command transfer is bounded by a one-second shared transfer deadline; it is
not a remote execution deadline. Failure after possible partial transmission is
uncertain. Successful UART transfer only means forwarding occurred, not that the
inverter executed anything. Its normal correlated result still returns asynchronously.
There is no automatic write retry.

Routing failures add optional `routing_status` to the existing `result` topic:

| Status | Semantic result | Catalog getter/setter result |
|---|---|---|
| `not_sent` | `outcome: "REJECTED"`, `write_sent: false` | `outcome: "state_not_allowed"`, `write_guard: null` |
| `uncertain` | `outcome: "UNKNOWN"`, `write_sent: null` | `outcome: "communication_error"`, `write_guard: null` |

The null guard means the receiving inverter's guard was not evaluated here; it
does not mean unrestricted permission. Routing markers are not serialized as
invented `modbus_status`/`modbus_exception` values. Existing command IDs still
correlate responses. Lost results and duplicate requests remain possible.

This is local stale-queue protection, not an end-to-end session protocol. There
is no recipient-enforced command deadline, peer boot/session nonce, durable result
journal, or exactly-once execution guarantee. A sufficiently short unobserved
peer restart can escape local link-loss detection. Bytes already forwarded cannot
be recalled, and a five-second queue lifetime does not expire an executing remote
command. The server must treat missing/uncertain results as unknown and must not
blindly resend setters.

## OTA scope remains unchanged

The proposed OTA worker/retry rewrite was blocked and is not integrated. Actual
scheduling remains: initial check after 1-61 minutes, local preflight postponements
after 5-10 minutes, and network attempts (including failures) after 6-7 hours.
There is no new exponential backoff, Retry-After handling, exact next-check
countdown, pending downloaded-image cache, or download-progress API.

`tools/ota_retry_policy.hpp` is an offline proposal only; it is not firmware code.
The actual worker's 54 mocked host tests pass and characterize the current behavior,
including its existing failure/retry limitations. They do not repair those behaviors
or validate real TLS, flash, rollback, power cuts, or simultaneous live/OTA memory use.
See [OTA updates](OTA_Updates.md) and [host OTA test scope](../tools/OTA_Host_Tests.md).

## Validation and deployment limits

ESP-IDF 5.5.1 ESP32-S3 build passed. The generated
`build/esp32_modbus.bin` contains project `esp32_modbus`, version `1.0.6`, and is
1,435,440 bytes; the 3 MiB app slot retains 54% free space. Runtime source/health
capacity checks and partition/version checks passed. Coordinator host tests passed
for deadlines, wraparound, link generations, blocked/partial FIFO transmission,
completion stalls and byte offsets. These are local build/offline test results,
not deployment approval. No device was flashed or server firmware replaced.

The USB/Modbus host suite compiles the actual transport/parser with deterministic
SDK stubs and tests valid/segmented/partial/CRC/timeout responses, overflow before
publication and after the final byte, resynchronization, pre-TX busy rejection,
write-no-retry behavior, counters, and reconnects. Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Test-UsbModbusDiagnostics.ps1
```

Host checks do not prove actual FreeRTOS SMP timing, electrical/USB reliability,
UART/ESP-NOW hardware behavior, flash persistence, or a thousand-device deployment.
Bench disconnect/overflow/command-loss tests, long-duration monitoring, and rollout
approval remain required. This change does not itself flash a device, upload a
release, change broker credentials, or provision signing/security keys.
