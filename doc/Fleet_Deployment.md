# Fleet deployment and device replacement

Firmware hardening is not a 1,000-device capacity certification. No broker load test has been performed.

## Automatic firmware updates

See [OTA updates](OTA_Updates.md). Version 1.0.9 retains automatic HTTPS updates
for provisioned standalone Wi-Fi devices; a responding inverter is not required.
The first check is 30–120 seconds after updater startup. Local postponements
retry in 5–10 minutes; transient errors back off from 5–10 to 30–60 minutes.
Completed checks and permanent rejections retain the 6–7 hour interval. The
webpage displays the next scheduled check without forcing a request.
Connection admission requires 64,000 free internal bytes and a 24,576-byte largest
block; streaming requires 32,768 free bytes and an 8,192-byte largest block.
The TLS allocator can place large buffers in PSRAM. Failure-time memory remains
separate from current memory. These guards and host tests require hardware
validation with concurrent MQTT/OTA traffic.

This is not a fleet rollout controller. The shared latest-file endpoint has no
per-device cohorts, kill switch or signed-release policy. Verify server TLS/download
capacity, protect upload access, and use a controlled pilot before publishing to
an endpoint used by all customers. ESP-NOW/parallel/gateway roles still use USB
updates. Original pre-framing-fix 1.0.5/1.0.6 builds reject chunked responses: they
need a one-time USB baseline or an HTTP 200 binary response with an exact positive
Content-Length to download the fix at their next eligible scheduled check.
Locally rebuilt firmware may retain an older version label while containing the
framing fix; identify installed code from build provenance and diagnostics.
An old updater permanently blocked by its memory guard needs a physical 1.0.9 baseline.
Changing the served binary or response framing cannot replace an updater that
cannot pass its memory guard. Actual hardware OTA and rollback tests remain
required after installation; no device flash or endpoint upload is included here.

## Connection and queue behavior

- MQTT client ID is `ESP-` followed by the full station MAC. Topic identity remains the saved installation ID (internally `site_id`); existing topics do not change.
- Startup delay is randomized from 0 to 15 seconds. Reconnect interval is 5 to 35 seconds, selected once per client initialization, not exponential backoff or new jitter on every attempt.
- Clean sessions are explicit. Retained command rejection remains in place. Never retain command requests.
- Live publication defaults to 5 seconds through `mqtt_live_publish_interval_ms`, independent of Modbus polling cadence. Payload contents are unchanged.
- Command results have a separate bounded eight-entry RAM queue, serviced before queued telemetry when connected. The 64 KiB outbox reserves 8 KiB from ordinary publishes for result messages. This is priority, NOT durable or guaranteed result delivery. Full result queues, enqueue/allocation failures and resets can still lose results.
- Existing health fields and snapshot messages are preserved. Version 1.0.6 appends eight local USB/Modbus counters to health; see [reliability notes](Firmware_Reliability_1.0.6.md). No new retained hardware/installation association topic was added.
- Separate coordinator-UART command queues reject offline admission, observed-generation changes and entries older than five seconds. They do not replay stale queued commands after an observed reconnect. Routing failures are not-sent or uncertain and do not automatically retry writes. This is not a recipient deadline, peer boot/session nonce, durable result journal or exactly-once guarantee; forwarded bytes cannot be recalled.

## Production security

Set `mqtt_require_secure_credentials = true` in `project_config.hpp` before fleet deployment. This requires a TLS URI (`mqtts://` or `wss://`) plus nonempty MQTT username/password in setup and connection initialization. Certificate verification remains enabled for TLS.

The switch defaults to false to preserve existing optional-auth installations. Supply individual credentials and broker topic ACLs; the firmware does not create users or enforce server ACLs. Secure provisioning, credential storage, OTA, and ESP-NOW protections still require review. Do not deploy anonymous inverter command access to an untrusted network.

## Replace an ESP

1. Record the existing installation ID, member ID, role, rating and topology. Keep server history linked to the installation, not just a MAC address.
2. Disconnect the old ESP and revoke its broker credentials. The firmware cannot fence it or detect duplicate installation assignments by itself.
3. Configure the new ESP with new device credentials and the old permanent installation ID. This preserves the topic path while the MQTT client ID changes with hardware.
4. On the master/standalone setup page, select the replacement checkbox to clear saved radio peers, save, and repeat verification. Re-pair parallel devices through commissioning. Member/gateway identities continue to follow the existing coordinator workflow.
5. On the server, associate the replacement with the existing installation, discard pending commands to the old hardware, and refresh identity/settings. Clear or disregard obsolete retained snapshots until replacement reads arrive. Never automatically replay old setters.

The existing persisted installation-ID field is reused; no configuration schema migration or factory reset is necessary. Its original MAC-derived default becomes the permanent installation ID when reused on a replacement. This is manual restoration, not a server-managed ownership-transfer service.

## Replace an inverter only

Keep the ESP and installation ID. Review the supported register profile, rating, PV count and topology; save/restart to clear volatile caches and repeat verification. Confirm the new inverter identity/settings before issuing setters. No automatic old-setting replay is implemented.

## Validation before deployment

Verify unique client IDs, restore an ID on a spare ESP with the original disconnected, check peer rediscovery, test incorrect credentials/certificates, and exercise full result queues and broker outages. Simulate at least 1,000 connections using realistic payload sizes and subscriber fan-out, including mass reconnects. Check reverse-proxy/WebSocket limits, broker RAM, bandwidth, file descriptors and ACL isolation. The version 1.0.5 ESP-IDF build and source/configuration checks pass; device tests and load tests remain outstanding.
