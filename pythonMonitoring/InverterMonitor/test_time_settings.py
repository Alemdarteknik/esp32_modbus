import json
import unittest
from unittest.mock import MagicMock, patch
from time_settings import TimeSettings
from settings_polling import SettingsReadPolicy, CONTROL_REFRESH_SECONDS


class TimeTests(unittest.TestCase):
    def setUp(self):
        self.s = s = TimeSettings.__new__(TimeSettings)
        s.monitor = MagicMock()
        s.monitor.disconnecting = False
        s.monitor.mqtt_topic = 'inverter/INV-AABBCCDDEEFF/inverter/0/live'
        s.monitor.mqtt_client.publish.return_value.rc = 0
        s.ready = True
        s.pending = None
        s.jobs = []
        s.writing = False
        s.dialog = None
        s.values = {}
        s.read_values = {}
        s.read_policy = SettingsReadPolicy(refresh_seconds=CONTROL_REFRESH_SECONDS)
        s.value = MagicMock()
        s.status = MagicMock()
        self.values = dict(zip(s.FIELDS, (2026, 9, 6, 21, 30, 45)))

    def reply(self, value, outcome='confirmed'):
        s = self.s
        cid, op, name, _ = s.pending
        s.receive(s.monitor.mqtt_topic.rsplit('/', 1)[0] + '/result',
                  dict(member_id=0, command_id=cid, operation=op, name=name, value=value, outcome=outcome))

    def test_sequential_write_seconds_last_then_read(self):
        s = self.s
        s.edit_values = {k: MagicMock(get=MagicMock(return_value=str(v))) for k,v in self.values.items()}
        s.apply()
        for name in s.FIELDS:
            call = s.monitor.mqtt_client.publish.call_args
            payload = json.loads(call.args[1])
            self.assertEqual(payload['name'], name)
            self.assertEqual(payload['value'], self.values[name])
            self.assertTrue(payload['confirmed'] and payload['guarded_interlock'])
            self.assertFalse(call.kwargs['retain'])
            self.reply(self.values[name])
        self.assertEqual(s.pending[1], 'get')
        for name in s.FIELDS:
            self.reply(self.values[name])
        s.value.set.assert_called_with('2026-09-06 21:30:45')

    def test_invalid_date_no_write(self):
        s = self.s
        s.edit_status = MagicMock()
        values = dict(self.values, Month=2, Day=30)
        s.edit_values = {k: MagicMock(get=MagicMock(return_value=str(v))) for k,v in values.items()}
        s.apply()
        s.monitor.mqtt_client.publish.assert_not_called()

    def test_legacy_two_digit_year_readback_completes_the_calendar_write(self):
        s = self.s
        s.edit_values = {k: MagicMock(get=MagicMock(return_value=str(v))) for k, v in self.values.items()}
        s.apply()
        for name in s.FIELDS:
            self.reply(26 if name == 'Year' else self.values[name])
        for name in s.FIELDS:
            self.reply(26 if name == 'Year' else self.values[name])
        s.value.set.assert_called_with('2026-09-06 21:30:45')

    def test_failure_stops_sequence(self):
        s = self.s
        s.writing = True
        s.jobs = [('set', k, v) for k,v in self.values.items()]
        s.send_next()
        self.reply(None, 'communication_error')
        self.assertEqual(s.jobs, [])
        self.assertIsNone(s.pending)
        self.assertIn('some fields may have been written', s.status.configure.call_args.kwargs['text'])
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 1)

    def test_periodic_calendar_reads_wait_two_minutes_after_success(self):
        s = self.s
        with patch('time_settings.time.monotonic', return_value=100):
            s.request('get')
            for name in s.FIELDS:
                self.reply(self.values[name])
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 6)
        with patch('time_settings.time.monotonic', return_value=219):
            s.refresh()
        self.assertIsNone(s.pending)
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 6)
        with patch('time_settings.time.monotonic', return_value=220):
            s.refresh()
        self.assertEqual(s.pending[1], 'get')
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 7)


if __name__ == '__main__':
    unittest.main()
