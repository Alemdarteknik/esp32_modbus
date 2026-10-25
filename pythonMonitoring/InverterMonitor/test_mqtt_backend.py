import json
import unittest
from datetime import datetime
from unittest.mock import Mock, patch
from mqtt_backend import MonitorSession


class BackendTests(unittest.TestCase):
    def test_priority_refresh_does_not_starve_other_settings(self):
        keys = list(self.m.catalog)
        focus, other = keys[:2]
        self.m.set_focus(focus, self.m.catalog[focus].domain)
        self.m.last_read[focus] = 10
        self.assertEqual(self.m.next_read([focus, other], 100), focus)
        self.assertEqual(self.m.next_read([focus, other], 100), other)

    def test_physical_change_get_updates_cache(self):
        self.m.cache[self.key] = 'old'
        self.m.send('get', self.key)
        self.result(self.item.choices[self.label], operation='get')
        self.assertEqual(self.m.cache[self.key], self.item.choices[self.label])
        self.assertIn(self.key, self.m.value_at)
        self.assertEqual(self.m.read_status[self.key], 'Read confirmed')
        self.m.receive(self.m.base + '/snapshot/x', dict(member_id=0, name=self.item.name,
            domain=self.item.domain, description=self.item.description, value='old'), retained=True)
        self.assertEqual(self.m.cache[self.key], self.item.choices[self.label])

    def setUp(self):
        self.m = MonitorSession()
        self.m.base = 'inverter/INV-TEST/inverter/0'
        self.m.ready = True
        self.m.client = Mock()
        self.m.client.publish.return_value.rc = 0
        self.key = next(k for k, s in self.m.catalog.items() if s.choices and not s.blocked and s.name != 'OnOffSet')
        self.item = self.m.catalog[self.key]
        self.label = next(iter(self.item.choices))

    def result(self, value, **extra):
        data = dict(member_id=0, command_id=self.m.pending['command_id'],
                    operation='set', name=self.m.catalog[self.m.pending['key']].name,
                    outcome='confirmed', value=value)
        data.update(extra)
        self.m.receive(self.m.base + '/result', data)

    def test_write_only_once_and_confirm(self):
        self.m.write(self.key, self.label, True)
        self.m.tick()
        for _ in range(5):
            self.m.tick()
            self.m.snapshot()
        self.assertEqual(self.m.client.publish.call_count, 1)
        call = self.m.client.publish.call_args
        self.assertFalse(call.kwargs['retain'])
        self.assertEqual(call.kwargs['qos'], 0)
        self.assertTrue(json.loads(call.args[1])['confirmed'])
        self.result(self.item.choices[self.label])
        self.assertTrue(self.m.notice.startswith('Confirmed:'))

    def test_ack_required(self):
        with self.assertRaises(ValueError):
            self.m.write(self.key, self.label, False)
        self.m.client.publish.assert_not_called()

    def test_mismatched_id_not_confirmation(self):
        self.m.write(self.key, self.label, True)
        self.m.tick()
        self.result(0, command_id='wrong')
        self.assertIsNotNone(self.m.pending)

    def test_timeout_does_not_retry(self):
        self.m.write(self.key, self.label, True)
        self.m.tick()
        self.m.pending['deadline'] = 0
        self.m.tick()
        self.assertIn('not confirmed', self.m.notice)
        self.assertEqual(self.m.client.publish.call_count, 1)
        self.assertFalse(self.m.jobs)

    def test_time_order_and_abort(self):
        self.m.write_time(datetime(2026, 9, 8, 12, 30, 20), True)
        self.assertEqual([self.m.catalog[k].name for _, k, _ in self.m.jobs],
                         ['Year', 'Month', 'Day', 'Hour', 'Minute', 'Second'])
        self.m.tick()
        self.result(0)
        self.assertFalse(self.m.jobs)
        self.assertIn('Not confirmed', self.m.notice)

    def test_retained_live_not_fresh(self):
        self.m.receive(self.m.base + '/live', {'member_id': 0}, True)
        self.assertIsNone(self.m.live_at)
        self.m.receive(self.m.base + '/health', {'telemetry_seen': False})
        self.assertIsNotNone(self.m.health_at)

    def test_close_stops_dispatch(self):
        self.m.write(self.key, self.label, True)
        self.m.close()
        self.m.tick()
        self.m.client.publish.assert_not_called()

    def fresh_settings(self, now=100):
        """A firmware startup snapshot supplies the complete current catalog."""
        with patch('mqtt_backend.time.monotonic', return_value=now):
            for item in self.m.catalog.values():
                if item.readable:
                    self.m.receive(self.m.base + '/snapshot/setting', dict(member_id=0,
                        name=item.name, domain=item.domain, description=item.description, value=1))

    def test_fresh_firmware_snapshots_suppress_redundant_catalog_reads(self):
        self.fresh_settings()
        for now in (100, 101, 130, 699.9):
            with patch('mqtt_backend.time.monotonic', return_value=now):
                self.m.tick()
        self.m.client.publish.assert_not_called()
        with patch('mqtt_backend.time.monotonic', return_value=700):
            self.m.tick()
        self.assertEqual(self.m.pending['operation'], 'get')
        self.assertEqual(self.m.client.publish.call_count, 1)

    def test_hour_without_changed_snapshots_has_bounded_fallback_and_covers_catalog(self):
        # Firmware publishes settings only when changed. A short fallback for
        # all 199 readable entries would otherwise saturate the command loop.
        now = [10000]
        self.fresh_settings(now[0])
        observed = set()
        with patch('mqtt_backend.time.monotonic', side_effect=lambda: now[0]):
            for second in range(3600):
                now[0] = 10000 + second
                self.m.tick()
                if self.m.pending is not None:
                    observed.add(self.m.pending['key'])
                    self.result(1, operation='get')
        self.assertEqual(observed, {key for key, item in self.m.catalog.items() if item.readable})
        self.assertLess(self.m.client.publish.call_count, 1200)

    def test_retained_snapshot_still_needs_validation_and_cannot_replace_new_read(self):
        self.fresh_settings()
        self.m.read_policy.fresh_at.pop(self.key)
        data = dict(member_id=0, name=self.item.name, domain=self.item.domain,
                    description=self.item.description, value='cached')
        with patch('mqtt_backend.time.monotonic', return_value=101):
            self.m.receive(self.m.base + '/snapshot/setting', data, retained=True)
            self.m.tick()
            self.assertEqual(self.m.pending['key'], self.key)
            self.result(0, operation='get')
            self.m.receive(self.m.base + '/snapshot/setting', data, retained=True)
        self.assertEqual(self.m.cache[self.key], 0)

    def test_focus_refreshes_on_selection_once_then_every_thirty_seconds(self):
        self.fresh_settings()
        self.m.set_focus(self.key, self.item.domain)
        with patch('mqtt_backend.time.monotonic', return_value=101):
            self.m.tick()
            self.assertEqual(self.m.pending['key'], self.key)
            self.result(0, operation='get')
        for now in (102, 110, 130.9):
            self.m.set_focus(self.key, self.item.domain)
            with patch('mqtt_backend.time.monotonic', return_value=now):
                self.m.tick()
        self.assertEqual(self.m.client.publish.call_count, 1)
        with patch('mqtt_backend.time.monotonic', return_value=131):
            self.m.tick()
        self.assertEqual(self.m.client.publish.call_count, 2)

    def test_getter_timeout_backs_off_without_blocking_other_settings(self):
        self.fresh_settings()
        self.m.read_policy.fresh_at.pop(self.key)
        with patch('mqtt_backend.time.monotonic', return_value=101):
            self.m.tick()
        with patch('mqtt_backend.time.monotonic', return_value=112):
            self.m.tick()
        self.assertIsNone(self.m.pending)
        self.assertFalse(self.m.aohai_write_blocked)
        with patch('mqtt_backend.time.monotonic', return_value=141.9):
            self.m.tick()
        self.assertEqual(self.m.client.publish.call_count, 1)
        with patch('mqtt_backend.time.monotonic', return_value=142):
            self.m.tick()
        self.assertEqual(self.m.pending['key'], self.key)
        self.assertEqual(self.m.client.publish.call_count, 2)

    def test_invalid_numeric_snapshots_neither_replace_nor_freshen_readbacks(self):
        item = next(item for item in self.m.catalog.values() if item.name == 'BatteryType')
        data = dict(member_id=0, name=item.name, domain=item.domain,
                    description=item.description, value=1)
        with patch('mqtt_backend.time.monotonic', return_value=100):
            self.m.receive(self.m.base + '/snapshot/setting', data)
        for invalid in ('garbage', None, float('nan'), float('inf'), True):
            with self.subTest(value=invalid), patch('mqtt_backend.time.monotonic', return_value=110):
                self.m.receive(self.m.base + '/snapshot/setting', dict(data, value=invalid))
                self.assertEqual(self.m.cache[item.key], 1)
                self.assertEqual(self.m.read_policy.fresh_at[item.key], 100)
        with patch('mqtt_backend.time.monotonic', return_value=120):
            self.m.receive(self.m.base + '/snapshot/setting', dict(data, value=999))
        self.assertEqual(self.m.cache[item.key], 999)
        self.assertEqual(self.m.read_policy.fresh_at[item.key], 120)

    def test_snapshot_isolation_and_native_staleness_advance_without_new_packets(self):
        from test_native_live import atomic
        self.m.select_protocol('PI30')
        with patch('mqtt_backend.time.monotonic', return_value=100):
            self.m.receive(self.m.base + '/protocol/QPIGS', atomic(
                fields={'battery_voltage': {'value': 52, 'unit': 'V'}}))
            state = self.m.snapshot()
        state['native_records'][0]['fields']['battery_voltage']['value'] = 999
        state['native_live']['groups']['battery']['fields']['battery_voltage']['value'] = 999
        with patch('mqtt_backend.time.monotonic', return_value=116):
            stale = self.m.snapshot()
        self.assertEqual(stale['native_records'][0]['fields']['battery_voltage']['value'], 52)
        field = stale['native_live']['groups']['battery']['fields']['battery_voltage']
        self.assertEqual(field['value'], 52)
        self.assertTrue(field['stale'])
        self.assertFalse(stale['native_live']['telemetry_available'])
        with patch('mqtt_backend.time.monotonic', return_value=116):
            summary = self.m.snapshot(include_records=False)
        self.assertEqual(summary['native_records'], [])
        self.assertEqual(summary['native_live'], stale['native_live'])


class SettingsReadPolicyTests(unittest.TestCase):
    def test_failure_backoff_is_bounded_and_a_new_observation_clears_it(self):
        from settings_polling import SettingsReadPolicy
        policy = SettingsReadPolicy()
        for attempt, delay in enumerate((30, 60, 120, 240, 300, 300)):
            now = attempt * 1000
            policy.failed('key', now)
            self.assertFalse(policy.due('key', now + delay - 0.01))
            self.assertTrue(policy.due('key', now + delay))
        policy.received('key', 10000)
        self.assertNotIn('key', policy.failures)
        self.assertNotIn('key', policy.retry_at)
        self.assertFalse(policy.due('key', 10599.9))
        self.assertTrue(policy.due('key', 10600))

    def test_ascii_and_unknown_enum_read_shapes_do_not_use_write_ranges(self):
        from settings_polling import valid_setting_read
        from settings_catalog import load_catalog
        catalog = load_catalog()
        ascii_item = next(item for item in catalog.values() if item.value_type == 'ascii')
        numeric_item = next(item for item in catalog.values() if item.name == 'BatteryType')
        for valid in ('AB', 0, 0x4142, 65535, 65535.0):
            self.assertTrue(valid_setting_read(ascii_item, valid))
        for invalid in ('ABC', 'A', '\x00A', 'çA', True, -1, 65536, 1.5, None):
            self.assertFalse(valid_setting_read(ascii_item, invalid))
        self.assertTrue(valid_setting_read(numeric_item, 999))
        self.assertTrue(valid_setting_read(numeric_item, -999))
