"""All documented setting paths, using mocked MQTT and no device writes."""
import json
import re
import unittest
from decimal import Decimal
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, MagicMock

from catalog_settings_ui import CatalogSettings
from inverter_monitor import InverterMonitor, native_result_text
from mqtt_backend import MonitorSession
from native_protocols import PROFILES, classify_command
from settings_catalog import load_catalog
from settings_results import (named_write_result, write_problem, begin_write,
                              finish_write, reset_write_session)
import test_settings_catalog


class NamedSettingsAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = load_catalog()

    def item(self, name):
        return next(item for item in self.catalog.values() if item.name == name)

    def monitor(self):
        monitor = MonitorSession()
        monitor.base = 'inverter/INV-TEST/inverter/0'
        monitor.ready = True
        monitor.client = Mock()
        monitor.client.publish.return_value.rc = 0
        return monitor

    def test_all_201_catalog_types_match_firmware_scaling_and_guards(self):
        root = Path(__file__).resolve().parents[2]
        source = root / 'main/src/inverter/aohai_register_catalog.generated.cpp'
        if not source.is_file():
            self.skipTest('Firmware source is only present in the combined repository.')
        token = r'("(?:[^"\\]|\\.)*")'
        pattern = (r'\{RegisterSpace::holding,\s*(\d+),\s*' + token + r',\s*' + token +
                   r',\s*RegisterAccess::(\w+),\s*RegisterDataType::(\w+),\s*' + token +
                   r',\s*([\d.eE+-]+),\s*RegisterReadClass::\w+,\s*RegisterWriteGuard::(\w+)')
        descriptors = {}
        for address, name, description, access, dtype, unit, scale, guard in re.findall(pattern, source.read_text(encoding='utf-8')):
            descriptors[json.loads(name) + ' | ' + json.loads(description)] = (dtype, Decimal(scale), guard)
        self.assertEqual(len(self.catalog), 201)
        for key, item in self.catalog.items():
            with self.subTest(setting=key):
                dtype, scale, guard = descriptors[key]
                self.assertEqual(item.value_type.split(';', 1)[0], dtype)
                if not item.blocked:
                    self.assertEqual(item.guard, guard)
                if not item.choices and not item.value_type.startswith('ascii'):
                    self.assertEqual(item.step, scale)
                if item.blocked:
                    with self.assertRaises(ValueError):
                        item.parse('1')
                elif item.choices:
                    for label, raw in item.choices.items():
                        self.assertEqual(item.parse(label), raw)
                elif item.value_type.startswith('ascii'):
                    self.assertEqual(item.parse('AZ'), 'AZ')
                elif re.fullmatch(r'PeriodTime(?:Start|End)\d+', item.name):
                    self.assertEqual(item.parse('12:34'), 12 * 256 + 34)
                elif item.bounds:
                    for boundary in item.bounds:
                        item.parse(str(boundary))
                else:
                    item.parse('0')

    def test_scaled_ranges_steps_and_encoding_limits(self):
        cases = [('ChargeRate', '1', ('1.01', '25', '-0.01')),
                 ('ReactivePowerDelayTIme', '600000', ('1', '600020', '-20')),
                 ('wVLowCutTime1EE', '100000', ('1', '100020')),
                 ('PowerStartSlope', '0.1', ('0', '3000.1', '0.11')),
                 ('PvStartVoltage', '6553.5', ('6553.6', '-0.1', '0.01')),
                 ('Year', '2026', ('22', '2051')),
                 ('Hour', '23', ('24', '59'))]
        for name, valid, invalid in cases:
            item = self.item(name)
            self.assertIsInstance(item.parse(valid), (int, float))
            for value in invalid:
                with self.subTest(setting=name, invalid=value), self.assertRaises(ValueError):
                    item.parse(value)

    def test_on_off_requires_operating_state_evidence_both_interfaces(self):
        item = self.item('OnOffSet')
        for expected in (0, 1):
            for outcome, verified, confirmed in [('confirmed', None, False),
                                                  ('transition_pending', False, False),
                                                  ('completed', False, False),
                                                  ('completed', True, True)]:
                with self.subTest(expected=expected, outcome=outcome, verified=verified):
                    response = dict(member_id=0, operation='set', name='OnOffSet', value=1 - expected,
                                    outcome=outcome, operating_state_confirmed=verified, write_sent=True)
                    monitor = self.monitor()
                    monitor.send('set', item.key, expected)
                    response['command_id'] = monitor.pending['command_id']
                    monitor.receive(monitor.base + '/result', response)
                    self.assertEqual(monitor.notice.startswith('Confirmed:'), confirmed)
                    ui = test_settings_catalog.CatalogTests.make_ui(self, item)
                    ui.request('set', expected)
                    response['command_id'] = ui.pending[0]
                    ui.receive(ui.monitor.mqtt_topic.rsplit('/', 1)[0] + '/result', response)
                    self.assertEqual(ui.status.configure.call_args.kwargs['text'].startswith('Confirmed:'), confirmed)
                    self.assertEqual(ui.monitor.mqtt_client.publish.call_count, 1)

    def test_timeout_and_disconnect_block_another_write_but_reads_continue(self):
        item = self.item('DspBeepOnOff')
        for disconnect in (False, True):
            monitor = self.monitor()
            monitor.send('set', item.key, True)
            if disconnect:
                monitor.on_disconnect(monitor.client, None, 1)
            else:
                monitor.pending['deadline'] = 0
                monitor.tick()
            self.assertTrue(monitor.aohai_write_blocked)
            monitor.ready = True
            with self.assertRaises(ValueError):
                monitor.write(item.key, next(iter(item.choices)), True)
            monitor.send('get', item.key)
            self.assertEqual(monitor.client.publish.call_count, 2)

    def test_guard_is_shared_between_time_and_catalog_writers(self):
        monitor, first, second = SimpleNamespace(), object(), object()
        begin_write(monitor, first)
        self.assertTrue(write_problem(monitor, second))
        finish_write(monitor, first, uncertain=True)
        self.assertTrue(write_problem(monitor, first))
        reset_write_session(monitor)
        self.assertFalse(write_problem(monitor, second))

    def test_retained_tk_result_is_discarded_before_widget_dispatch(self):
        monitor = InverterMonitor.__new__(InverterMonitor)
        monitor.data_queue = Mock()
        monitor.mqtt_topic = 'inverter/INV-TEST/inverter/0/live'
        monitor.on_message(None, None, SimpleNamespace(topic='inverter/INV-TEST/inverter/0/result',
                           payload=b'{"outcome":"confirmed"}', retain=True))
        monitor.data_queue.put.assert_not_called()

    def test_failure_keeps_modbus_exception_visible(self):
        item = self.item('DspBeepOnOff')
        result = named_write_result(item, True, dict(outcome='communication_error',
                                   modbus_status='exception', modbus_exception=3, value=None))
        self.assertIn('modbus_exception=3', result['message'])
        self.assertFalse(result['confirmed'])

    def test_year_legacy_raw_and_manufacturer_ascii_readbacks(self):
        for name, expected, actual in [('Year', 2026, 26),
                                        ('Manufacturer Info 1', 'AB', 0x4142)]:
            item = self.item(name)
            result = named_write_result(item, expected, dict(outcome='confirmed', value=actual))
            self.assertTrue(result['confirmed'])
        item = self.item('Year')
        monitor = self.monitor()
        monitor.send('set', item.key, 2026)
        monitor.receive(monitor.base + '/result', dict(member_id=0, name='Year', operation='set',
            command_id=monitor.pending['command_id'], outcome='confirmed', value=26))
        self.assertEqual(monitor.cache[item.key], 2026)
        self.assertTrue(monitor.notice.startswith('Confirmed:'))

    def test_definitely_unsent_failure_does_not_block_a_new_setting(self):
        result = named_write_result(self.item('DspBeepOnOff'), True,
                                    dict(outcome='communication_error', write_sent=False))
        self.assertFalse(result['confirmed'])
        self.assertFalse(result['uncertain'])


class NativeSettingsAudit(unittest.TestCase):
    def test_every_query_and_setter_dispatches_once_with_truthful_results(self):
        monitor = MonitorSession()
        monitor.base = 'inverter/INV-TEST/inverter/0'
        monitor.ready = True
        monitor.client = Mock()
        monitor.client.publish.return_value.rc = 0
        total = setters = 0
        for protocol, profile in PROFILES.items():
            if protocol == 'AOHAI_MODBUS_RTU':
                continue
            monitor.select_protocol(protocol)
            for group in ('queries', 'setters'):
                write = group == 'setters'
                for entry in profile[group]:
                    with self.subTest(protocol=protocol, command=entry['example']):
                        before = monitor.client.publish.call_count
                        cid = monitor.send_native(entry['example'], confirmed=write)
                        call = monitor.client.publish.call_args
                        payload = json.loads(call.args[1])
                        self.assertEqual(payload['protocol'], protocol)
                        self.assertEqual(payload.get('confirmed'), True if write else None)
                        self.assertEqual(call.kwargs, dict(qos=0, retain=False))
                        result = dict(member_id=0, operation='protocol', protocol=protocol,
                                      command_id=cid, command=entry['example'], write_sent=write,
                                      outcome='acknowledged' if write else 'completed')
                        monitor.receive(monitor.base + '/result', result)
                        self.assertIsNone(monitor.native_pending)
                        self.assertEqual(monitor.client.publish.call_count, before + 1)
                        if write:
                            self.assertIn('accepted', monitor.notice)
                            self.assertNotIn('verified by', monitor.notice)
                            cid = monitor.send_native(entry['example'], confirmed=True)
                            result.update(command_id=cid, outcome='completed', setting_verified=True)
                            monitor.receive(monitor.base + '/result', result)
                            self.assertIsNone(monitor.native_pending)
                            self.assertIn('verified by', monitor.notice)
                            self.assertIn('verified by', native_result_text(result))
                            cid = monitor.send_native(entry['example'], confirmed=True)
                            result.update(command_id=cid, outcome='rejected', setting_verified=None)
                            monitor.receive(monitor.base + '/result', result)
                            self.assertFalse(monitor.native_write_blocked)
                            self.assertNotIn('verified by', monitor.notice)
                            setters += 1
                        total += 1
        self.assertEqual(total, 705)
        self.assertEqual(setters, 336)

    def test_peer_unit_readback_never_uses_local_settings(self):
        for protocol, command, expected in [('PI30', 'MCHGC130', 'QPGS1'),
                                             ('PI30MAX', 'POPM10', 'QPGS1'),
                                             ('PI18LVX', 'MCHGC1,030', 'PRI1'),
                                             ('PI18SV', 'MCHGC1,030', 'PRI1'),
                                             ('PI18', 'PCP1,1', None),
                                             ('PI18', 'MCHGC1,030', None)]:
            with self.subTest(protocol=protocol, command=command):
                self.assertEqual(classify_command(protocol, command)['readback'], expected)

    def test_revo_calendar_is_real_and_bms_numbers_are_integer_only(self):
        for entry in PROFILES['PI30REVO']['setters']:
            self.assertTrue(classify_command('PI30REVO', entry['example'])['setter'])
            parts = entry['example'].split(' ')
            parts[8:10] = ['02', '30']
            with self.assertRaises(ValueError):
                classify_command('PI30REVO', ' '.join(parts))
        for protocol, profile in PROFILES.items():
            for entry in profile['setters']:
                if entry.get('numeric'):
                    for suffix in ('.0', '.', 'e0'):
                        with self.subTest(protocol=protocol, command=entry['example'] + suffix), self.assertRaises(ValueError):
                            classify_command(protocol, entry['example'] + suffix)


if __name__ == '__main__':
    unittest.main()
