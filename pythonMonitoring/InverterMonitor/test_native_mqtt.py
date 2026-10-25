"""Native MQTT compatibility tests; no broker or inverter is contacted."""
import json
import time
import unittest
from unittest.mock import Mock, patch

from mqtt_backend import MonitorSession
from native_protocols import PROTOCOL_IDS
from test_native_live import atomic


def chunk(index=0, count=1, sequence=42, **changes):
    data = dict(member_id=0, protocol='PI30', command='QPIGS', sequence=sequence,
                timestamp_ms=123456, chunk_index=index, chunk_count=count,
                final_chunk=index == count - 1,
                fields={'battery_voltage': {'unit': 'V', 'value': 57.5}})
    data.update(changes)
    return data


class NativeBackendTests(unittest.TestCase):
    def setUp(self):
        self.m = MonitorSession()
        self.m.select_protocol('PI30')
        self.m.base = 'inverter/INV-TEST/inverter/0'
        self.m.client = Mock()
        self.m.client.publish.return_value.rc = 0
        self.m.ready = True

    def result(self, **changes):
        pending = self.m.native_pending
        data = dict(member_id=self.m.member_id, operation='protocol', protocol='PI30',
                    command_id=pending['command_id'], command=pending['command'],
                    outcome='acknowledged' if pending['write'] else 'completed',
                    write_sent=pending['write'], detail='ok')
        data.update(changes)
        self.m.receive(self.m.base + '/result', data)

    def test_all_26_protocol_selections(self):
        self.assertEqual(len(PROTOCOL_IDS), 26)
        for profile in PROTOCOL_IDS:
            self.m.select_protocol(profile)
            self.assertEqual(self.m.snapshot()['protocol'], profile)
        with self.assertRaises(ValueError):
            self.m.select_protocol('unknown')
        self.m.active = True
        with self.assertRaises(ValueError):
            self.m.select_protocol('PI30MAX')

    def test_subscriptions_preserve_native_atomic_qos_one_and_health_qos_zero(self):
        self.m.on_connect(self.m.client, None, None, 0)
        subscriptions = self.m.client.subscribe.call_args.args[0]
        self.assertIn((self.m.base + '/protocol/#', 1), subscriptions)
        self.assertIn((self.m.base + '/result', 1), subscriptions)
        self.assertIn((self.m.base + '/live', 1), subscriptions)
        self.assertIn((self.m.health_topic, 0), subscriptions)
        self.assertNotIn((self.m.base + '/snapshot/#', 1), subscriptions)

    def test_aohai_live_health_qos_unchanged(self):
        self.m.select_protocol('AOHAI_MODBUS_RTU')
        self.m.on_connect(self.m.client, None, None, 0)
        subscriptions = self.m.client.subscribe.call_args.args[0]
        self.assertIn((self.m.base + '/live', 0), subscriptions)
        self.assertIn((self.m.health_topic, 0), subscriptions)
        self.assertIn((self.m.base + '/result', 1), subscriptions)
        self.assertIn((self.m.base + '/snapshot/#', 1), subscriptions)
        self.assertFalse(any(topic.startswith(self.m.base + '/protocol') for topic, _ in subscriptions))

    def test_native_mode_does_not_send_201_aohai_reads(self):
        for _ in range(5):
            self.m.tick()
            self.m.snapshot()
        self.m.client.publish.assert_not_called()
        key = next(iter(self.m.catalog))
        with self.assertRaises(ValueError):
            self.m.send('get', key)
        with self.assertRaises(ValueError):
            self.m.write(key, '0', True)

    def test_out_of_order_chunks_need_every_index(self):
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(1, 2))
        self.assertEqual(self.m.snapshot()['native_records'], [])
        self.assertIsNone(self.m.live_at)
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(0, 2,
            fields={'ac_output_active_power': {'unit': 'W', 'value': 119}}))
        state = self.m.snapshot()
        self.assertEqual(len(state['native_records']), 1)
        self.assertEqual(state['native_records'][0]['fields']['ac_output_active_power']['value'], 119)
        self.assertEqual(state['native_records'][0]['fields']['battery_voltage']['unit'], 'V')
        self.assertIsNotNone(self.m.live_at)

    def test_retained_wrong_member_and_wrong_topic_do_not_update(self):
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(), retained=True)
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(member_id=1))
        self.m.receive(self.m.base + '/protocol/QMOD', chunk())
        self.assertEqual(self.m.snapshot()['native_records'], [])
        self.assertIsNone(self.m.live_at)

    def test_profile_mismatch_blocks_commands(self):
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(protocol='PI30MAX'))
        self.assertEqual(self.m.snapshot()['protocol_mismatch'], 'PI30MAX')
        with self.assertRaises(ValueError):
            self.m.send_native('QPIGS')
        self.m.client.publish.assert_not_called()

    def test_query_payload_shared_topic_and_one_transmission(self):
        cid = self.m.send_native('QPIGS')
        args = self.m.client.publish.call_args
        self.assertEqual(args.args[0], self.m.base + '/command')
        self.assertEqual(json.loads(args.args[1]), dict(command_id=cid, operation='protocol',
                         protocol='PI30', command='QPIGS'))
        self.assertEqual(args.kwargs, dict(qos=0, retain=False))
        for _ in range(5):
            self.m.tick()
        self.assertEqual(self.m.client.publish.call_count, 1)
        self.result()
        self.assertEqual(self.m.native_result['outcome'], 'completed')
        self.assertIsNone(self.m.native_pending)

    def test_set_requires_explicit_true_confirmation_and_valid_range(self):
        for confirmation in (False, None, 1, 'yes'):
            with self.assertRaises(ValueError):
                self.m.send_native('POP02', confirmed=confirmation)
        for invalid in ('POP03', 'POP02\rQPIGS', 'raw_write', 'POP02 '):
            with self.assertRaises(ValueError):
                self.m.send_native(invalid, confirmed=True)
        self.m.client.publish.assert_not_called()
        self.m.send_native('POP02', confirmed=True)
        self.assertTrue(json.loads(self.m.client.publish.call_args.args[1])['confirmed'])

    def test_ack_acceptance_is_not_readback_equality(self):
        self.m.send_native('POP02', confirmed=True)
        self.result()
        self.assertIn('accepted', self.m.notice)
        self.assertIn('separately', self.m.notice)
        self.assertNotIn('Confirmed:', self.m.notice)
        self.assertEqual(self.m.cache, {})
        self.assertFalse(self.m.native_write_blocked)

    def test_verified_setter_requires_explicit_fresh_readback_flag(self):
        self.m.send_native('POP02', confirmed=True)
        for verified in (None, False, 1, 'true'):
            self.result(outcome='completed', setting_verified=verified)
            self.assertIsNotNone(self.m.native_pending)
        self.result(outcome='completed', setting_verified=True)
        self.assertIsNone(self.m.native_pending)
        self.assertIn('verified by', self.m.notice)

    def test_acknowledged_readback_mismatch_is_visible(self):
        self.m.send_native('POP02', confirmed=True)
        self.result(setting_verified=False)
        self.assertIn('did not match', self.m.notice)
        self.assertNotIn('verified by', self.m.notice)
        self.assertEqual(self.m.client.publish.call_count, 1)

    def test_query_cannot_claim_setting_verification(self):
        self.m.send_native('QPIGS')
        self.result(setting_verified=True)
        self.assertIsNotNone(self.m.native_pending)
        self.result(setting_verified=None)
        self.assertIsNone(self.m.native_pending)

    def test_result_requires_matching_member_id_profile_command_and_operation(self):
        self.m.send_native('POP02', confirmed=True)
        for invalid in (dict(command_id='other'), dict(member_id=1), dict(protocol='PI30MAX'),
                        dict(command='POP01'), dict(operation='set'), dict(write_sent=False),
                        dict(write_sent=1), dict(outcome='confirmed'), dict(outcome='completed')):
            self.result(**invalid)
            self.assertIsNotNone(self.m.native_pending)
        self.result()
        self.assertIsNone(self.m.native_pending)

    def test_retained_result_does_not_acknowledge(self):
        cid = self.m.send_native('POP02', confirmed=True)
        self.m.receive(self.m.base + '/result', dict(member_id=0, command_id=cid, operation='protocol',
            protocol='PI30', command='POP02', outcome='acknowledged', write_sent=True), retained=True)
        self.assertIsNotNone(self.m.native_pending)

    def test_uncertain_route_blocks_settings_but_queries_continue(self):
        self.m.send_native('POP02', confirmed=True)
        self.result(outcome='uncertain', write_sent=None)
        self.assertTrue(self.m.native_write_blocked)
        with self.assertRaises(ValueError):
            self.m.send_native('POP01', confirmed=True)
        self.m.send_native('QPIGS')
        self.result()
        self.assertTrue(self.m.native_write_blocked)
        self.assertEqual(self.m.client.publish.call_count, 2)

    def test_setting_timeout_never_retries_and_late_ack_cannot_clear_block(self):
        cid = self.m.send_native('POP02', confirmed=True)
        self.m.native_pending['deadline'] = 0
        self.m.tick()
        self.assertTrue(self.m.native_write_blocked)
        self.m.receive(self.m.base + '/result', dict(member_id=0, command_id=cid,
            operation='protocol', protocol='PI30', command='POP02', outcome='acknowledged', write_sent=True))
        for _ in range(5):
            self.m.tick()
        self.assertTrue(self.m.native_write_blocked)
        self.assertEqual(self.m.client.publish.call_count, 1)
        self.assertEqual(self.m.native_result['outcome'], 'uncertain')

    def test_not_sent_setting_does_not_block_next_setting(self):
        self.m.send_native('POP02', confirmed=True)
        self.result(outcome='not_sent', write_sent=False)
        self.assertFalse(self.m.native_write_blocked)
        self.m.send_native('POP01', confirmed=True)
        self.assertEqual(self.m.client.publish.call_count, 2)

    def test_close_marks_pending_setting_uncertain(self):
        self.m.send_native('POP02', confirmed=True)
        self.m.close()
        self.assertTrue(self.m.stop.is_set())
        self.assertFalse(self.m.ready)
        self.assertIsNone(self.m.native_pending)
        self.assertTrue(self.m.native_write_blocked)
        self.m.tick()
        self.assertEqual(self.m.client.publish.call_count, 1)

    def test_member_9_routing_and_strict_integer_validation(self):
        self.m.ready = False
        with patch('mqtt_backend.mqtt.Client'), patch('mqtt_backend.threading.Thread'):
            self.m.connect('ABCDEF123456', protocol='PI30', member_id=9)
        self.assertEqual(self.m.base, 'inverter/INV-ABCDEF123456/inverter/9')
        self.m.on_connect(self.m.client, None, None, 0)
        subscriptions = self.m.client.subscribe.call_args.args[0]
        self.assertIn(('inverter/INV-ABCDEF123456/inverter/0/health', 0), subscriptions)
        self.assertNotIn((self.m.base + '/health', 0), subscriptions)
        self.m.receive(self.m.health_topic, dict(telemetry_seen=True))
        self.assertIsNotNone(self.m.health_at)
        self.assertTrue(self.m.health['telemetry_seen'])
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(member_id=9))
        self.assertEqual(len(self.m.snapshot()['native_records']), 1)
        for invalid in (-1, 10, True, 1.5, '2'):
            self.m.active = False
            with self.assertRaises(ValueError):
                self.m.connect('ABCDEF123456', member_id=invalid)

    def test_invalid_native_numeric_measurement_does_not_crash_or_publish(self):
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(
            fields={'battery_voltage': {'unit': 'V', 'value': float('nan')}}))
        self.assertEqual(self.m.snapshot()['native_records'], [])
        self.m.client.publish.assert_not_called()

    def test_atomic_complete_live_snapshot_populates_without_manual_commands(self):
        with patch('mqtt_backend.time.monotonic', return_value=100):
            data = dict(protocol='PI30', member_id=0, sequence=1, timestamp_ms=1000,
                        native_records=[atomic(), atomic(command='QMOD', sequence=2,
                            timestamp_ms=2000, fields={'device_mode': {'unit': '', 'value': 'Standby'}})])
            self.m.receive(self.m.base + '/live', data)
            state = self.m.snapshot()
        self.assertEqual(state['native_live']['power_summary']['pv_kw'], 1.862)
        self.assertEqual(state['native_live']['power_summary']['load_kw'], .45)
        self.assertEqual(state['native_live']['operating_status']['device_mode'], 'Standby')
        self.assertTrue(state['native_live']['telemetry_available'])
        self.assertEqual(state['native_raw_live'], data)
        state['native_raw_live']['native_records'][0]['fields'].clear()
        self.assertTrue(self.m.snapshot()['native_raw_live']['native_records'][0]['fields'])
        self.assertEqual(self.m.snapshot(include_records=False)['native_raw_live'], {})
        self.m.close()
        self.assertEqual(self.m.snapshot()['native_raw_live'], {})
        self.m.client.publish.assert_not_called()

    def test_periodic_cached_live_and_duplicate_atomic_do_not_refresh_sample(self):
        with patch('mqtt_backend.time.monotonic', return_value=100):
            self.m.receive(self.m.base + '/protocol/QPIGS', atomic())
        with patch('mqtt_backend.time.monotonic', return_value=120):
            self.m.receive(self.m.base + '/live', dict(protocol='PI30', member_id=0,
                native_records=[atomic(sample_age_ms=20000)]))
            self.m.receive(self.m.base + '/protocol/QMOD', atomic(command='QMOD', sequence=2,
                timestamp_ms=2000, fields={'device_mode': {'unit': '', 'value': 'Standby'}}))
            state = self.m.snapshot()
        self.assertEqual(state['live_at'], 100)
        self.assertFalse(state['native_live']['telemetry_available'])
        self.assertTrue(state['native_live']['groups']['status']['fresh'])
        self.assertEqual(state['native_drops'], 0)
        self.assertEqual(state['native_diagnostics']['duplicate_messages'], 1)

    def test_new_subscriber_preserves_cached_sample_age_and_identity(self):
        with patch('mqtt_backend.time.monotonic', return_value=100):
            self.m.receive(self.m.base + '/live', dict(protocol='PI30', member_id=0,
                native_records=[atomic(sample_age_ms=20000), atomic(command='QID', sequence=2,
                    timestamp_ms=2000, sample_age_ms=50000,
                    fields={'serial_number': {'unit': '', 'value': '92832207100756'}})]))
            state = self.m.snapshot()
        self.assertFalse(state['native_live']['telemetry_available'])
        self.assertEqual(state['native_live']['received_at'], 80)
        self.assertEqual(state['native_live']['groups']['identity']['fields']['serial_number']['value'],
                         '92832207100756')
        self.assertEqual(state['native_live']['groups']['identity']['age_seconds'], 50)

    def test_retained_cross_source_member_and_profile_live_snapshots_are_ignored(self):
        data = dict(protocol='PI30', member_id=0, native_records=[atomic()])
        self.m.receive(self.m.base + '/live', data, retained=True)
        self.m.receive('inverter/INV-OTHER/inverter/0/live', data)
        self.m.receive(self.m.base + '/live', dict(data, member_id=1))
        self.m.receive(self.m.base + '/live', dict(data, native_records=[atomic(member_id=1)]))
        self.m.receive(self.m.base + '/live', dict(data, native_records=[atomic(protocol='PI30MAX')]))
        self.assertEqual(self.m.snapshot()['native_records'], [])
        self.m.receive(self.m.base + '/live', dict(data, protocol='PI30MAX'))
        self.assertEqual(self.m.protocol_mismatch, 'PI30MAX')

    def test_mode_and_identity_cannot_claim_numeric_live_freshness(self):
        self.m.receive(self.m.base + '/protocol/QMOD', atomic(command='QMOD', fields={
            'device_mode': {'unit': '', 'value': 'Standby'}}))
        state = self.m.snapshot()
        self.assertIsNone(state['live_at'])
        self.assertFalse(state['native_live']['telemetry_available'])
        self.assertEqual(state['native_live']['operating_status']['device_mode'], 'Standby')

    def test_broker_reconnect_clears_partial_history_and_preserves_drop_counters(self):
        self.m.receive(self.m.base + '/protocol/QPIGS', atomic())
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(sequence=50, index=0, count=2))
        self.m.receive(self.m.base + '/protocol/QPIGS', chunk(sequence=51,
            fields={'x': {'unit': '12345678', 'value': 1}}))
        self.assertEqual(self.m.snapshot()['native_drops'], 1)
        self.m.on_disconnect(self.m.client, None, 1)
        state = self.m.snapshot()
        self.assertEqual(state['native_records'], [])
        self.assertEqual(state['native_incomplete'], 0)
        self.assertEqual(state['native_drops'], 1)
        self.assertIsNone(state['live_at'])
        self.m.on_connect(self.m.client, None, None, 0)
        self.m.receive(self.m.base + '/protocol/QPIGS', atomic(sequence=1))
        self.assertEqual(len(self.m.snapshot()['native_records']), 1)
        self.assertEqual(self.m.snapshot()['native_drops'], 1)

    def test_disconnect_pending_write_stays_uncertain_and_is_never_retried(self):
        self.m.send_native('POP02', confirmed=True)
        self.m.on_disconnect(self.m.client, None, 1)
        self.assertTrue(self.m.native_write_blocked)
        self.assertEqual(self.m.native_result['outcome'], 'uncertain')
        self.m.on_connect(self.m.client, None, None, 0)
        self.m.on_subscribe(self.m.client, None, 1, [0])
        with self.assertRaises(ValueError):
            self.m.send_native('POP01', confirmed=True)
        self.assertEqual(self.m.client.publish.call_count, 1)


if __name__ == '__main__':
    unittest.main()
