# Offline OTA worker tests

Run in PowerShell:

```powershell
.\tools\Test-OtaWorkerHost.ps1
```

If the local PowerShell execution policy prevents that command, run the same
offline script in a process with a temporary execution-policy override:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Test-OtaWorkerHost.ps1
```

The default compiler is `C:\msys64\mingw64\bin\g++.exe`; pass `-Compiler` to
select another compatible Windows host compiler. The runner emits its test
executable into a newly created temporary directory and prints that path.
It does not build or modify the ESP application binary.

`ota_host_tests.cpp` includes the actual `main/src/app/ota_update.cpp`, along
with the actual project configuration and OTA header. The implementation is
not copied or translated into a separate behavior model. Only external SDK
services and the memory-admission mutex are replaced with offline fakes.
The fake image-header/descriptor layouts match the fields and byte offsets
used by the worker in ESP-IDF 5.5.1. The fake APIs are deliberately a narrow
subset, not a replacement SDK. They cannot be compiled with `ESP_PLATFORM`.

Current status: **130/130 offline actual-worker cases pass** after reducing
transient allocations and correcting memory admission and retry scheduling. The
suite compiles with `-Wall -Wextra -Werror`. This is host verification only; no
device update, endpoint upload or hardware OTA validation is performed by these
tests.

The suite retains the previous 120 cases (except updating the old network-error
retry expectation) and adds coverage for the reported fragmented heap, PSRAM
fallback and production scheduling policy:

- Runtime and memory preflight deferrals, NVS/client/buffer allocation errors.
- TLS/open failure, post-handshake heap pressure, HTTP status/length/slot errors.
- Partial headers, incompatible chip/project/version, same/older releases,
  previously attempted releases and missing image hash flag.
- Write-blocker changes, download truncation/timeouts, OTA begin/write/end errors.
- Failed activation boundary, NVS set/commit failure, boot-partition failure.
- Byte-for-byte successful image reconstruction, partial HTTP reads, resource
  cleanup, abort/resume and validation-before-activation ordering.
- Actual worker initial delay and local/normal/network-error retry intervals.
- Task-creation failure reporting.
- Known-length and chunked/unknown-length success, including fragmented reads,
  a parser that completes before buffered body bytes are drained, and a final
  zero-byte result needed to consume chunk framing. Read requests must always
  have a positive length; image verification must follow EOF and a completion
  check. Empty and incomplete chunked prefixes cannot start flash writes.
- Incomplete/truncated chunked responses, header/read errors, and missing
  non-chunked length are rejected; known-length bodies also require completed
  HTTP framing.
- Exact-slot images can finish without an extra flash write; oversized known
  or chunked bodies are rejected before any write crosses the inactive slot.
  The fake flash API treats any attempted out-of-bounds write as a test failure.
- Chunked responses still require the existing project/hash checks and successful
  image verification before activation, with unchanged cleanup ordering.
- Failed attempts expose the expected stage, error code and observed HTTP status.
  Existing failure context survives local runtime/memory deferrals; a new actual
  check clears it before NVS access, and a subsequent failure replaces old stage,
  HTTP status and offered-version information. Fresh nonfailure cases contain no
  leftover failure diagnostics.
- Connection admission requires 64,000 internal free bytes and a 24,576-byte
  contiguous block. Exact boundaries pass; one byte below either boundary defers
  before NVS or a network request.
- The reported 89.2 KiB free / 31 KiB largest-block memory sample starts a real
  check instead of remaining permanently deferred by the former 32 KiB guard.
  This tests admission, not actual TLS allocations on the device.
- The download scratch buffer prefers PSRAM, with successful complete updates
  when PSRAM allocation fails and the normal allocator must be used. The HTTP
  configuration is checked for 1,024-byte RX / 512-byte TX buffers.
- Startup is scheduled 30-120 seconds after health confirmation. Runtime and
  local memory deferrals retry after 5-10 minutes. Transient network, HTTP
  429/503, header transport and allocation failures start at 5-10 minutes,
  back off to a maximum of 30-60 minutes, and recover automatically when local
  blockers clear. Permanent HTTP/image errors retain the normal fleet interval.
- The real worker exposes the next scheduled monotonic deadline while sleeping
  and clears it before an actual check. Repeated failure and recovery scenarios
  verify both backoff and deadline behavior.
- Known-length and chunked images activate byte-for-byte with 48,000 internal free
  bytes and a 16,384-byte contiguous block throughout active TLS, below both
  connection reserves. Both framings also succeed at the exact streaming reserves:
  32,768 free bytes and an 8,192-byte contiguous block.
- One byte below either streaming reserve fails at every protected phase:
  after TLS, after headers, before erase, before the first body read, after a body
  read, after a body write, and before image verification. Both response framings
  are checked. Tests assert the failure stage, error, HTTP status, active budget,
  exact bytes already written, abort/cleanup, and absence of activation records.
- Failure heap snapshots precede cleanup recovery. The rejecting sample is retained
  even if memory recovers immediately after that sample and before cleanup. TLS,
  flash and verification errors also report their failure-time heap and active
  phase requirements; these requirements do not imply memory caused those errors.
- Failure heap and requirements survive runtime, memory-gate, low-free-memory and
  fragmentation deferrals. All four fields clear before NVS on the next actual
  check; a subsequent failure replaces the active budget, and a nonfailure leaves
  no retained failure memory.

Before the fix, the chunked regression was run against the original real worker:
the baseline passed 54/54, while the new unknown-length success case failed with
`check_failed`, reproducing the missing-Content-Length compatibility problem.

Fake time makes long waits complete immediately. A fake restart throws a test
signal after recording activation; no PC restart or real device restart occurs.
Heap allocations in the harness use host memory, never ESP memory.
Fake heap samples follow SDK lifecycle events: before open, active TLS, headers,
prefix receipt, prefix write, body receipt, body write, EOF and cleanup. Memory
dips and cleanup recovery are configurable independently; status polling or extra
heap queries do not advance the model. This models pressure and fragmentation
without claiming to reproduce real ESP-IDF allocation sizes.

The separate web-status rendering regressions can be run with:

```powershell
node tools/tests/ota_memory_ui_test.js
```

Those tests cover display of live versus failure-time memory and active reserve
fields, including compatibility with absent optional fields. They do not replace
the actual-worker tests above.

The suite explicitly characterizes existing limitations: HTTPS resources remain
alive during quiesce and unsuccessful quiesce does not retain a pending activation
record. Tests
of these behaviors must be reviewed if those production behaviors change.

Passing host tests is not production certification. These tests do not verify
real image cryptography, TLS certificates or handshake memory consumption,
flash persistence, power-cut recovery, electrical isolation, actual FreeRTOS
SMP/locking, watchdog timing, or a thousand-device deployment. Image validation
is a programmable fake return value; real ESP-IDF validation remains necessary.
HTTP reads supply decoded body bytes and configurable parser-completion signals;
the suite does not implement or independently validate ESP-IDF's HTTP chunk parser.
No test contacts an endpoint, broker, Wi-Fi network, serial port or inverter.

Firmware configuration selects ESP-IDF's default mbedTLS allocator, allowing the
16 KiB incoming record plus overhead to prefer PSRAM under the existing 16 KiB
malloc threshold. Internal fallback remains available if PSRAM is absent. TLS
record limits, certificate checks and image verification are unchanged. The host
suite does not simulate that allocator or certify its real handshake footprint;
check the generated firmware configuration and validate on ESP hardware.
