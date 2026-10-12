import json
import unittest
from unittest.mock import MagicMock

from buzzer_settings import BuzzerSettings


class BuzzerTests(unittest.TestCase):
    def setUp(self):
        self.setting = BuzzerSettings.__new__(BuzzerSettings)
        s = self.setting
        s.monitor = MagicMock()
        s.monitor.disconnecting = False
        s.monitor.mqtt_topic = 'inverter/INV-AABBCCDDEEFF/inverter/0/live'
        s.monitor.mqtt_client.publish.return_value.rc = 0
        s.ready, s.current, s.target, s.pending = True, None, None, None
        for name in ('value', 'combo', 'button', 'status'):
            setattr(s, name, MagicMock())
        self.base = s.monitor.mqtt_topic.rsplit('/', 1)[0]

    def result(self, value, outcome='confirmed', command_id=None):
        s = self.setting
        return dict(member_id=0, name=s.NAME, command_id=command_id or s.pending[0],
                    operation=s.pending[1], outcome=outcome, value=value)

    def test_read_select_write_and_readback(self):
        s = self.setting
        s.request('get')
        call = s.monitor.mqtt_client.publish.call_args
        self.assertEqual(call.args[0], self.base + '/command')
        payload = json.loads(call.args[1])
        self.assertEqual(payload['category'], 'configuration')
        self.assertEqual(call.kwargs, dict(qos=1, retain=False))
        s.receive(self.base + '/result', self.result(0))
        s.value.set.assert_called_with('Disable')
        s.value.get.return_value = 'Enable'
        s.value.set.reset_mock()
        s.select()
        s.value.set.assert_not_called()
        self.assertTrue(s.target)
        s.write()
        payload = json.loads(s.monitor.mqtt_client.publish.call_args.args[1])
        self.assertIs(payload['value'], True)
        self.assertIs(payload['confirmed'], True)
        wrong = self.result(1, command_id='wrong')
        s.receive(self.base + '/result', wrong)
        self.assertIsNotNone(s.pending)
        # Snapshot is current state, not the correlated setter acknowledgement.
        s.receive(self.base + '/snapshot/SYSTEM/DspBeepOnOff/Buzzer', dict(member_id=0, name=s.NAME, value=1))
        s.value.set.assert_called_with('Enable')
        self.assertIsNotNone(s.pending)
        s.receive(self.base + '/result', self.result(1))
        self.assertIsNone(s.pending)
        s.status.configure.assert_called_with(text='Confirmed: Enable')

    def test_snapshot_updates_current_automatically(self):
        s = self.setting
        for value, label in ((0, 'Disable'), (1, 'Enable')):
            s.receive(self.base + '/snapshot/SYSTEM/' + s.NAME + '/setting',
                      dict(member_id=0, name=s.NAME, value=value))
            s.value.set.assert_called_with(label)
            s.status.configure.assert_called_with(text=f'Current: {label}')
        s.monitor.mqtt_client.publish.assert_not_called()

    def test_external_change_overrides_old_selection(self):
        s = self.setting
        s.current = 0
        s.target = False
        s.receive(self.base + '/snapshot/SYSTEM/' + s.NAME + '/setting',
                  dict(member_id=0, name=s.NAME, value=1))
        s.value.set.assert_called_with('Enable')
        self.assertIsNone(s.target)

    def test_automatic_reads_detect_changes_and_preserve_new_selection(self):
        s = self.setting
        s.current = 0
        s.target = True
        s.refresh_tick()
        payload = json.loads(s.monitor.mqtt_client.publish.call_args.args[1])
        self.assertEqual(payload['operation'], 'get')
        s.receive(self.base + '/result', self.result(0))
        self.assertTrue(s.target)
        s.value.set.assert_not_called()
        s.refresh_tick()
        s.receive(self.base + '/result', self.result(1))
        s.value.set.assert_called_with('Enable')
        self.assertIsNone(s.target)
        s.reset()
        s.refresh_tick()
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 2)

    def test_click_during_background_read_submits_once(self):
        s = self.setting
        s.current = 0
        s.request('get', background=True)
        old_result = self.result(0)
        s.combo.configure.assert_called_with(state='readonly')
        s.value.get.return_value = 'Enable'
        s.select()
        s.button.configure.assert_called_with(state='normal')
        s.write()
        self.assertEqual(s.pending[1], 'set')
        s.receive(self.base + '/result', old_result)
        self.assertEqual(s.pending[1], 'set')
        s.write()
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 2)

    def test_timeout_never_retries_and_old_timer_is_ignored(self):
        s = self.setting
        s.request('set', False)
        s.timeout('old')
        self.assertIsNotNone(s.pending)
        s.timeout(s.pending[0])
        self.assertIsNone(s.pending)
        s.value.set.assert_called_with('Unknown')
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 1)

    def test_mismatch_rejection_and_disconnect(self):
        s = self.setting
        s.request('set', True)
        s.receive(self.base + '/result', self.result(0))
        self.assertIn('mismatch', s.status.configure.call_args.kwargs['text'])
        s.request('set', True)
        s.receive(self.base + '/result', self.result(None, 'communication_error'))
        self.assertIn('Not confirmed', s.status.configure.call_args.kwargs['text'])
        s.reset()
        s.request('set', True)
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 2)
        s.combo.configure.assert_called_with(state='disabled')


class BluetoothTests(BuzzerTests):
    def setUp(self):
        super().setUp()
        self.setting.NAME = 'ubBluetoothEn'
        self.setting.TITLE = 'Bluetooth'

    def test_buzzer_results_do_not_confirm_bluetooth(self):
        s = self.setting
        s.request('set', True)
        payload = json.loads(s.monitor.mqtt_client.publish.call_args.args[1])
        self.assertEqual(payload['name'], 'ubBluetoothEn')
        wrong = self.result(1)
        wrong['name'] = 'DspBeepOnOff'
        s.receive(self.base + '/result', wrong)
        self.assertIsNotNone(s.pending)
        s.receive(self.base + '/result', self.result(1))
        self.assertIsNone(s.pending)


class PriorityTests(BuzzerTests):
    def test_battery_types_and_standby_rejection(self):
        s = self.setting
        s.NAME = 'BatteryType'
        s.CHOICES = {0: 'Lead acid', 1: 'Lithium battery',
                     2: 'Lithium without communication (off-grid)',
                     3: 'User 2 (off-grid)', 4: 'User 3 (off-grid)', 5: 'No battery'}
        for value, label in s.CHOICES.items():
            s.request('set', value)
            payload = json.loads(s.monitor.mqtt_client.publish.call_args.args[1])
            self.assertEqual(payload['name'], 'BatteryType')
            self.assertIs(type(payload['value']), int)
            self.assertEqual(payload['value'], value)
            self.assertNotIn('guarded_interlock', payload)
            s.receive(self.base + '/result', self.result(value))
            s.value.set.assert_called_with(label)
        s.request('set', 0)
        s.receive(self.base + '/result', self.result(None, 'state_not_allowed'))
        self.assertIn('state_not_allowed', s.status.configure.call_args.kwargs['text'])
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 7)

    def test_priority_numeric_encoding_and_exact_confirmation(self):
        s = self.setting
        s.NAME = 'SourcePriority'
        s.CHOICES = {0: 'Sol', 1: 'UTI', 2: 'SBU', 10: 'Grid connected output mode'}
        s.GUARDED = True
        s.current = 0
        s.value.get.return_value = 'Grid connected output mode'
        s.select()
        s.write()
        payload = json.loads(s.monitor.mqtt_client.publish.call_args.args[1])
        self.assertEqual(payload['value'], 10)
        self.assertIs(type(payload['value']), int)
        self.assertTrue(payload['guarded_interlock'])
        s.receive(self.base + '/result', self.result(2))
        self.assertIn('mismatch', s.status.configure.call_args.kwargs['text'])
        s.request('set', 7)
        self.assertEqual(s.monitor.mqtt_client.publish.call_count, 1)

    def test_charging_priority_zero_is_valid(self):
        s = self.setting
        s.NAME = 'ChargeSourcePriority'
        s.CHOICES = {0: 'CSO', 1: 'SNU', 2: 'OSO'}
        s.GUARDED = True
        s.current = 1
        s.value.get.return_value = 'CSO'
        s.select()
        s.write()
        payload = json.loads(s.monitor.mqtt_client.publish.call_args.args[1])
        self.assertEqual(payload['name'], 'ChargeSourcePriority')
        self.assertIs(type(payload['value']), int)
        self.assertEqual(payload['value'], 0)
        s.receive(self.base + '/result', self.result(0))
        s.status.configure.assert_called_with(text='Confirmed: CSO')


if __name__ == '__main__':
    unittest.main()
