import json
import unittest
from unittest.mock import MagicMock, patch

from settings_catalog import load_catalog
from catalog_settings_ui import CatalogSettings
from settings_polling import SettingsReadPolicy, settings_index


class CatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = load_catalog()

    def item(self, name, description=None):
        return next(x for x in self.catalog.values() if x.name == name and
                    (description is None or x.description == description))

    def test_coverage_and_duplicate_names(self):
        self.assertEqual(len(self.catalog), 201)
        self.assertTrue(all(x.notes for x in self.catalog.values()))
        rates = [x for x in self.catalog.values() if x.name == 'ChargeRate']
        self.assertEqual(len(rates), 2)
        self.assertNotEqual(rates[0].key, rates[1].key)
        self.assertEqual(sum(bool(x.choices) for x in self.catalog.values()), 59)

    def test_schedule_encoding_and_reset_choice(self):
        item = self.item('PeriodTimeStart1')
        self.assertEqual(item.parse('23:59'), 0x173B)
        self.assertEqual(item.parse('00:00'), 0)
        for value in ('24:00', '23:60', '60', '-1'):
            with self.assertRaises(ValueError):
                item.parse(value)
        self.assertEqual(self.item('NumberTimePeriods').parse('Reset all time-period settings'), 165)

    def test_every_enum_retains_exact_application_type(self):
        for item in self.catalog.values():
            for label, value in item.choices.items():
                with self.subTest(setting=item.name, label=label):
                    self.assertEqual(item.parse(label), value)
                    self.assertIs(type(item.parse(label)), type(value))

    def test_ranges_nonfinite_ascii_and_unvalidated_restart(self):
        voltage = self.item('fGridVoltLow1EE')
        self.assertEqual(voltage.parse('180.0'), 180)
        for value in ('-1', '300', 'NaN', 'Infinity', '1e9999'):
            with self.assertRaises(ValueError):
                voltage.parse(value)
        serial = self.item('Serial NO.1')
        self.assertEqual(serial.parse('AB'), 'AB')
        for value in ('A', 'ABC', '\nA'):
            with self.assertRaises(ValueError):
                serial.parse(value)
        self.assertNotIn(165, self.item('OnOffSet').choices.values())
        with self.assertRaises(ValueError):
            self.item('CtRatioSet').parse('10')

    def make_ui(self, item):
        ui = CatalogSettings.__new__(CatalogSettings)
        ui.catalog = self.catalog
        ui._settings_index = settings_index(self.catalog)
        ui.selected = item.key
        ui.ready = True
        ui.pending = None
        ui.cache = {}
        ui.unreadable = set()
        ui.dirty = False
        ui.last_read = {}
        ui.read_policy = SettingsReadPolicy(ui.last_read)
        ui.monitor = MagicMock()
        ui.monitor.disconnecting = False
        ui.monitor.mqtt_topic = 'inverter/INV-AABBCCDDEEFF/inverter/0/live'
        ui.monitor.mqtt_client.publish.return_value.rc = 0
        for name in ('status', 'value', 'input', 'button', 'ack', 'frame', 'table', 'count'):
            setattr(ui, name, MagicMock())
        return ui

    def test_background_load_without_selecting_rows(self):
        ui = self.make_ui(self.item('BatteryType'))
        ui.selected = None
        ui.request('get')
        first = ui.pending
        self.assertIsNotNone(first)
        item = ui.catalog[first[2]]
        ui.receive(ui.monitor.mqtt_topic.rsplit('/', 1)[0] + '/result',
                   dict(member_id=0, command_id=first[0], name=item.name,
                        operation='get', outcome='confirmed', value=1))
        self.assertEqual(ui.cache[first[2]], 1)
        self.assertIsNone(ui.selected)
        ui.refresh()
        self.assertNotEqual(ui.pending[2], first[2])
        ui.monitor.disconnecting = True
        ui.pending = None
        ui.refresh()
        self.assertEqual(ui.monitor.mqtt_client.publish.call_count, 2)

    def test_qualified_write_and_correlated_confirmation(self):
        item = self.item('ChargeRate', 'Discharge power')
        ui = self.make_ui(item)
        ui.request('set', 25)
        call = ui.monitor.mqtt_client.publish.call_args
        payload = json.loads(call.args[1])
        self.assertEqual(payload['description'], 'Discharge power')
        self.assertEqual(payload['domain'], 'battery')
        self.assertFalse(call.kwargs['retain'])
        response = dict(member_id=0, operation='set', name=item.name, command_id='wrong', outcome='confirmed', value=25)
        topic = ui.monitor.mqtt_topic.rsplit('/', 1)[0] + '/result'
        ui.receive(topic, response)
        self.assertIsNotNone(ui.pending)
        response['command_id'] = ui.pending[0]
        ui.receive(topic, response)
        self.assertIsNone(ui.pending)
        self.assertEqual(ui.cache[item.key], 25)
        ui.status.configure.assert_called_with(text='Confirmed: 25')

    def test_each_catalog_payload_uses_exact_identity_and_guard(self):
        for item in self.catalog.values():
            with self.subTest(setting=item.key):
                ui = self.make_ui(item)
                ui.request('set', 1)
                if item.blocked:
                    ui.monitor.mqtt_client.publish.assert_not_called()
                    continue
                payload = json.loads(ui.monitor.mqtt_client.publish.call_args.args[1])
                self.assertEqual(payload['name'], item.name)
                self.assertEqual(payload['description'], item.description)
                self.assertEqual(ui.monitor.mqtt_client.publish.call_args.kwargs, dict(qos=0, retain=False))
                if item.guard == 'commissioning_only':
                    self.assertTrue(payload['commissioning_interlock'])
                if item.guard == 'service_only':
                    self.assertTrue(payload['service_interlock'])

    def test_unverifiable_timeout_and_rejection_are_not_success(self):
        item = self.item('Reset to factory')
        for outcome in ('sent_unverifiable', 'state_not_allowed', 'communication_error'):
            ui = self.make_ui(item)
            ui.request('set', 1)
            ui.receive(ui.monitor.mqtt_topic.rsplit('/', 1)[0] + '/result',
                       dict(member_id=0, operation='set', name=item.name, command_id=ui.pending[0], outcome=outcome))
            self.assertNotIn('Confirmed:', ui.status.configure.call_args.kwargs['text'])
            self.assertEqual(ui.monitor.mqtt_client.publish.call_count, 1)
        ui = self.make_ui(item)
        ui.request('set', 1)
        ui.timeout(ui.pending[0])
        self.assertEqual(ui.monitor.mqtt_client.publish.call_count, 1)

    def test_write_requires_ack_and_service_confirmation(self):
        ui = self.make_ui(self.item('Reset to factory'))
        ui.value.get.return_value = next(iter(self.item('Reset to factory').choices))
        ui.ack.get.return_value = False
        ui.write()
        ui.monitor.mqtt_client.publish.assert_not_called()
        ui.ack.get.return_value = True
        with patch('catalog_settings_ui.messagebox.askyesno', return_value=False):
            ui.write()
        ui.monitor.mqtt_client.publish.assert_not_called()

    def test_fresh_snapshots_skip_global_getters_and_selected_row_refreshes_sooner(self):
        item = self.item('BatteryType')
        ui = self.make_ui(item)
        base = ui.monitor.mqtt_topic.rsplit('/', 1)[0]
        with patch('catalog_settings_ui.time.monotonic', return_value=100):
            for setting in ui.catalog.values():
                if setting.readable:
                    ui.receive(base + '/snapshot/setting', dict(member_id=0, name=setting.name,
                        domain=setting.domain, description=setting.description, value=1))
            ui.refresh()
        ui.monitor.mqtt_client.publish.assert_not_called()
        with patch('catalog_settings_ui.time.monotonic', return_value=129.9):
            ui.refresh()
        ui.monitor.mqtt_client.publish.assert_not_called()
        with patch('catalog_settings_ui.time.monotonic', return_value=130):
            ui.refresh()
        self.assertEqual(ui.pending[2], item.key)

    def test_retained_snapshot_does_not_overwrite_a_fresh_observation(self):
        item = self.item('BatteryType')
        ui = self.make_ui(item)
        base = ui.monitor.mqtt_topic.rsplit('/', 1)[0]
        data = dict(member_id=0, name=item.name, domain=item.domain,
                    description=item.description, value=1)
        ui.receive(base + '/snapshot/setting', data)
        ui.receive(base + '/snapshot/setting', dict(data, value=0), retained=True)
        self.assertEqual(ui.cache[item.key], 1)

    def test_invalid_numeric_snapshots_cannot_suppress_fallback_validation(self):
        item = self.item('BatteryType')
        ui = self.make_ui(item)
        base = ui.monitor.mqtt_topic.rsplit('/', 1)[0]
        data = dict(member_id=0, name=item.name, domain=item.domain,
                    description=item.description, value=1)
        for invalid in ('garbage', None, float('nan'), True):
            ui.receive(base + '/snapshot/setting', dict(data, value=invalid))
        self.assertNotIn(item.key, ui.cache)
        self.assertNotIn(item.key, ui.read_policy.fresh_at)
        ui.receive(base + '/snapshot/setting', dict(data, value=999))
        self.assertEqual(ui.cache[item.key], 999)
        self.assertIn(item.key, ui.read_policy.fresh_at)


if __name__ == '__main__':
    unittest.main()
