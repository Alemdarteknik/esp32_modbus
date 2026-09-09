# Fleet deployment and device replacement

Firmware hardening is not a 1,000-device capacity certification. No broker load test has been performed.

## Connection and queue behavior

- MQTT client ID is `ESP-` followed by the full station MAC. Topic identity remains the saved installation ID (internally `site_id`); existing topics do not change.
- Startup delay is randomized from 0 to 15 seconds. Reconnect interval is 5 to 35 seconds, selected once per client initialization, not exponential backoff or new jitter on every attempt.
- Clean sessions are explicit. Retained command rejection remains in place. Never retain command requests.
- Live publication defaults to 5 seconds through `mqtt_live_publish_interval_ms`, independent of Modbus polling cadence. Payload contents are unchanged.
- Command results have a separate bounded eight-entry RAM queue, serviced before queued telemetry when connected. The 64 KiB outbox reserves 8 KiB from ordinary publishes for result messages. This is priority, NOT durable or guaranteed result delivery. Full result queues, enqueue/allocation failures and resets can still lose results.
- Existing startup health and snapshot messages remain unchanged. No new retained hardware/installation association topic was added.

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

Verify unique client IDs, restore an ID on a spare ESP with the original disconnected, check peer rediscovery, test incorrect credentials/certificates, and exercise full result queues and broker outages. Simulate at least 1,000 connections using realistic payload sizes and subscriber fan-out, including mass reconnects. Check reverse-proxy/WebSocket limits, broker RAM, bandwidth, file descriptors and ACL isolation. Compilation, device tests and load tests remain outstanding.
