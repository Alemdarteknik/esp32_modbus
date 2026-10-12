import json
import unittest
from datetime import datetime
from unittest.mock import Mock
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
        self.key = next(k for k, s in self.m.catalog.items() if s.choices and not s.blocked)
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
