# Runtime optimization review - 2026-09-11

Scope: MQTT publication and memory, OTA admission, web configuration/scans,
ESP-NOW discovery, and local command/polling interaction. This is a targeted
source review, not proof that every code path or hardware profile is correct.
No build, flash, inverter write, network load test or endurance test was performed.

## Changes made

- Split queued storage by actual message type: 32 telemetry entries and 8 command
  results no longer each reserve both payloads. Result processing is capped at
  two entries before checking telemetry, rather than draining results forever.
- Retain the existing reserved live output buffer and use the bounded printer,
  not an intentionally inflated estimate, to decide whether a payload fits.
  No truncated serialized live JSON is sent.
- Track cJSON allocation failures from boot. A failed allocation during live-tree
  construction suppresses that live publication. This is conservative across
  concurrent tasks and is not a guarantee for every non-live JSON path.
- Defer live-tree construction during OTA TLS opening or when the outbox already
  exceeds 8 KiB. Input caches continue updating; there is no durable replay.
- Gate OTA TLS opening against live-tree construction, measure internal RAM, and
  retry local preflight deferrals in 5-10 minutes. Actual network attempts retain
  the configured fleet interval. Downloads do not hold the shared memory gate.
- Handle Wi-Fi scan allocation failures without a throwing allocation; release
  completed scan results on error/empty paths. JSON response failures return 503.
- Stop rewriting NVS for unchanged ESP-NOW peers; keep discovery announcements
  periodic under traffic; allocate transmit sequence values atomically.
- Invalidate operating-state cache after an attempted local inverter write,
  including uncertain outcomes, and make the critical status poll due.
- Add diagnostic counters to `/health` and correct stale README claims about
  flash layout and whether automatic downloads are enabled in this checkout.

Existing live field names, numeric formatting, power mappings, MQTT command
schema, write interlocks and the configured Modbus bus gap are unchanged.
Health fields are additive. These changes do not authorize unsolicited writes.

## Important unresolved risks

1. **Internal-RAM pressure remains possible.** Current `sdkconfig` disables
   PSRAM and places TLS allocations in internal RAM. The gate does not reserve
   memory against every HTTP request, scan or MQTT reconnect. Verify the exact
   board and its PSRAM configuration before enabling it; measure worst-case heap
   and stack headroom with live publishing, scans and OTA together.
2. **Parallel routed-command lifetime is not solved.** The separate UART
   coordinator transmit queue can retain commands over a disconnect. Define
   expiry/session identity, rejection and uncertain-result handling before
   unattended writable deployments. Local USB-session rejection is insufficient.
3. **Background refresh can be too slow.** One background request is admitted
   per essential-power sweep. Many enabled requests/timeouts can exceed freshness
   ceilings. Measure all supported phase/member configurations; never substitute
   old readings as fresh just to remove nulls.
4. **Security/fleet release gates remain.** ESP-NOW peer authentication, signed
   OTA releases, setup access control, per-device broker ACLs, staged server
   rollout and durable command/result semantics require additional work.
   The generic Modbus framework does not verify every inverter register profile.
5. **Runtime shutdown needs a separate design.** Current MQTT cleanup is used
   only on startup failure. Calling its force-delete cleanup on an active
   publisher could abandon allocations or an owned mutex. Any future live
   stop/restart feature must use cooperative task shutdown first.

The cJSON allocation hook also disables the library's default realloc shortcut
for ordinary dynamic JSON printing. The reserved/preallocated live printer does
not use that path; stress-test the smaller HTTP/snapshot/result paths too.

## Verification and next bench run

Read-only checks:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-OtaConfiguration.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-RuntimeHardening.ps1
git diff --check
```

These check source/configuration invariants, not compiled firmware behavior.
After building and flashing a test device, capture serial warnings and:

```bash
mosquitto_sub -v -t 'inverter/INV-44B176CDC70C/inverter/0/health'
```

Compare counter changes at each amber event. `live_json_failures` indicates
live JSON failure; `enqueue_failures` indicates an outbox/enqueue rejection.
`live_publish_deferrals` alone is admission control and does not trigger amber.
Track `live_enqueue_age_ms` and server receipt times to detect persistent deferral.
Green after amber only means the recent-error window has expired.

Test Wi-Fi/broker outages, USB/RS485 loss, repeated web scans, command bursts,
maximum live payloads and concurrent OTA on controlled hardware. Verify bounded
memory, continuing task progress, safe command rejection and recovery, then run
the endurance and security gates in [Production readiness](Production_Readiness.md).
