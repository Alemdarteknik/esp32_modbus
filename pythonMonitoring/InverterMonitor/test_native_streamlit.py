"""Exercise the native-protocol UI without opening an MQTT connection."""
import copy
import json
import time
import unittest
from pathlib import Path
from unittest.mock import Mock

from streamlit.testing.v1 import AppTest
from native_protocols import PROFILES, classify_command
from mqtt_backend import MonitorSession
from native_live import normalize_native_live


class NativeMonitorStub:
    def __init__(self, protocol='PI30', active=True):
        self.connections = []
        self.commands = []
        self.state = {
            'protocol': protocol, 'member_id': 0, 'active': active,
            'ready': active, 'status': 'Connected' if active else 'Disconnected',
            'notice': '', 'live': {}, 'health': {}, 'live_at': None,
            'health_at': None, 'connected_at': time.monotonic(),
            'native_records': [], 'native_pending': None, 'native_result': None,
            'native_incomplete': 0, 'native_drops': 0,
        }

    def snapshot(self):
        state = copy.deepcopy(self.state)
        for record in state['native_records']:
            record.setdefault('member_id', state['member_id'])
        state['native_live'] = normalize_native_live(state['native_records'], state['protocol'],
                                                     state['member_id'], time.monotonic())
        return state

    def select_protocol(self, protocol):
        if self.state['active']:
            raise ValueError('Disconnect before changing protocol.')
        self.state['protocol'] = protocol

    def connect(self, device, protocol=None, member_id=0):
        self.connections.append((device, protocol, member_id))
        self.state.update(protocol=protocol, member_id=member_id,
                          active=True, ready=True, status='Connected')

    def close(self):
        self.state['active'] = self.state['ready'] = False

    def send_native(self, command, confirmed=False):
        match = classify_command(self.state['protocol'], command)
        if match['setter'] and not confirmed:
            raise ValueError('Confirm the setting before sending it.')
        self.commands.append((command, confirmed))
        self.state['native_pending'] = {'command_id': '9', 'command': command,
                                        'write': match['setter'], 'deadline': None}
        return '9'


class NativeAppTests(unittest.TestCase):
    def app(self, session):
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        return app

    def widget(self, collection, label):
        return next(widget for widget in collection if widget.label == label)

    def choose(self, app, protocol, key, setter=False):
        entries = [(False, entry) for entry in PROFILES[protocol]['queries']]
        entries.extend((True, entry) for entry in PROFILES[protocol]['setters'])
        index = next(index for index, (write, entry) in enumerate(entries)
                     if entry['key'] == key and write == setter)
        self.widget(app.selectbox, 'Documented command').select(index).run()

    def test_connection_carries_profile_and_member(self):
        session = NativeMonitorStub('AOHAI_MODBUS_RTU', active=False)
        app = self.app(session)
        self.widget(app.selectbox, 'Protocol').select('PI30').run()
        self.assertEqual(session.state['protocol'], 'PI30')
        self.widget(app.number_input, 'Inverter member').set_value(3).run()
        self.widget(app.text_input, 'Inverter ID').input('AABBCCDDEEFF').run()
        self.widget(app.button, 'Connect').click().run()
        self.assertFalse(app.exception)
        self.assertEqual(session.connections, [('AABBCCDDEEFF', 'PI30', 3)])
        self.assertTrue(self.widget(app.selectbox, 'Protocol').disabled)
        self.assertTrue(self.widget(app.number_input, 'Inverter member').disabled)
        self.assertFalse(session.commands)

    def test_complete_native_measurements_preserve_units_and_freshness(self):
        session = NativeMonitorStub()
        now = time.monotonic()
        session.state['native_records'] = [
            {'protocol': 'PI30', 'command': 'QPIGS', 'sequence': 12,
             'timestamp_ms': 42000, 'received_at': now,
             'fields': {'pv_power': {'value': 830, 'unit': 'W'},
                        'battery_current': {'value': 0, 'unit': 'A'},
                        'device_status': {'value': '01001000', 'unit': ''}}},
            {'protocol': 'PI30', 'command': 'QMOD', 'sequence': 11,
             'timestamp_ms': 41000, 'received_at': now - 20,
             'fields': {'mode': {'value': 'B', 'unit': ''}}},
        ]
        session.state['native_incomplete'] = 1
        session.state['native_drops'] = 2
        app = self.app(session)
        frames = [frame.value for frame in app.dataframe if 'name' in frame.value.columns]
        self.assertEqual(sum(len(frame) for frame in frames), 4)
        measured = frames[0].set_index('name')
        self.assertEqual(measured.loc['pv_power', 'value'], '830')
        self.assertEqual(measured.loc['pv_power', 'unit'], 'W')
        self.assertEqual(measured.loc['battery_current', 'value'], '0')
        self.assertEqual(measured.loc['device_status', 'value'], '01001000')
        power = next(frame.value for frame in app.dataframe if 'Circuit' in frame.value.columns)
        self.assertEqual(list(power['Circuit']), ['Grid', 'Load'])
        self.assertFalse(any(widget.label == 'Category' for widget in app.selectbox))
        captions = [caption.value for caption in app.caption]
        self.assertTrue(any('Sequence 12' in text and 'Fresh' in text for text in captions))
        self.assertTrue(any('Sequence 11' in text and 'Stale' in text for text in captions))
        self.assertTrue(any('1 partial telemetry' in text for text in captions))
        self.assertFalse(session.commands)

    def test_setter_requires_confirmation_and_sends_once(self):
        session = NativeMonitorStub()
        app = self.app(session)
        self.choose(app, 'PI30', 'POP', setter=True)
        self.widget(app.text_input, 'Full protocol command').input('POP02').run()
        self.widget(app.button, 'Send setting').click().run()
        self.assertTrue(any('Confirm' in message.value for message in app.error))
        self.assertFalse(session.commands)
        self.widget(app.checkbox, 'I have checked this setting command and confirm sending it to the inverter.').check().run()
        self.widget(app.button, 'Send setting').click().run()
        self.assertFalse(app.exception)
        self.assertEqual(session.commands, [('POP02', True)])
        self.assertTrue(self.widget(app.button, 'Send setting').disabled)
        self.assertFalse(self.widget(app.checkbox, 'I have checked this setting command and confirm sending it to the inverter.').value)
        app.run()
        self.assertEqual(session.commands, [('POP02', True)])

    def test_editing_setting_parameter_requires_new_confirmation(self):
        session = NativeMonitorStub()
        app = self.app(session)
        self.choose(app, 'PI30', 'POP', setter=True)
        self.widget(app.text_input, 'Full protocol command').input('POP01').run()
        self.widget(app.checkbox, 'I have checked this setting command and confirm sending it to the inverter.').check().run()
        self.assertTrue(self.widget(app.checkbox, 'I have checked this setting command and confirm sending it to the inverter.').value)
        self.widget(app.text_input, 'Full protocol command').input('POP02').run()
        self.assertFalse(self.widget(app.checkbox, 'I have checked this setting command and confirm sending it to the inverter.').value)
        self.widget(app.button, 'Send setting').click().run()
        self.assertTrue(any('Confirm' in message.value for message in app.error))
        self.assertFalse(session.commands)
        self.widget(app.checkbox, 'I have checked this setting command and confirm sending it to the inverter.').check().run()
        self.widget(app.button, 'Send setting').click().run()
        self.assertEqual(session.commands, [('POP02', True)])

    def test_selected_command_and_full_pattern_are_enforced(self):
        session = NativeMonitorStub()
        app = self.app(session)
        self.choose(app, 'PI30', 'QPIGS')
        self.widget(app.text_input, 'Full protocol command').input('QMOD').run()
        self.widget(app.button, 'Send query').click().run()
        self.assertTrue(app.error)
        self.assertFalse(session.commands)
        self.widget(app.text_input, 'Full protocol command').input('QPIGS').run()
        self.widget(app.button, 'Send query').click().run()
        self.assertEqual(session.commands, [('QPIGS', False)])
        app.run()
        self.assertEqual(session.commands, [('QPIGS', False)])

    def test_acknowledgement_is_accepted_not_equality_confirmation(self):
        session = NativeMonitorStub()
        session.state['native_result'] = {'command_id': '10', 'command': 'POP02',
                                          'outcome': 'acknowledged', 'write_sent': True,
                                          'detail': 'Device acknowledgement received'}
        app = self.app(session)
        self.assertTrue(any('accepted' in message.value and 'does not confirm' in message.value
                            for message in app.success))
        session.state['native_result']['outcome'] = 'uncertain'
        session.state['native_result']['write_sent'] = None
        app.run()
        self.assertFalse(app.exception)
        self.assertTrue(any('outcome uncertain' in message.value for message in app.warning))
        self.assertFalse(session.commands)

    def test_read_only_profile_and_bms_are_native_views(self):
        for protocol in ('PI16', 'JK02', 'VED'):
            with self.subTest(protocol=protocol):
                session = NativeMonitorStub(protocol)
                app = self.app(session)
                self.assertTrue(any(heading.value == 'Live telemetry' for heading in app.subheader))
                if not PROFILES[protocol]['setters']:
                    self.assertTrue(any('no documented settings' in message.value for message in app.info))
                documented = next(frame.value for frame in app.dataframe if 'Command' in frame.value.columns)
                self.assertEqual(len(documented), len(PROFILES[protocol]['queries']) + len(PROFILES[protocol]['setters']))
                self.assertFalse(session.commands)

    def test_real_session_assembles_chunks_and_publishes_one_query(self):
        session = MonitorSession()
        session.select_protocol('PI30')
        session.active = session.ready = True
        session.member_id = 2
        session.base = 'inverter/INV-AABBCCDDEEFF/inverter/2'
        session.client = Mock()
        session.client.publish.return_value.rc = 0
        session.connected_at = time.monotonic()
        metadata = dict(member_id=2, protocol='PI30', command='QPIGS',
                        sequence=80, timestamp_ms=9000, chunk_count=2)
        session.receive(session.base + '/protocol/QPIGS',
                        dict(metadata, chunk_index=0, final_chunk=False,
                             fields={'battery_voltage': {'value': 52.4, 'unit': 'V'}}))
        app = self.app(session)
        self.assertFalse(any('name' in frame.value.columns for frame in app.dataframe))
        session.receive(session.base + '/protocol/QPIGS',
                        dict(metadata, chunk_index=1, final_chunk=True,
                             fields={'load_power': {'value': 2400, 'unit': 'W'}}))
        app.run()
        measured = next(frame.value for frame in app.dataframe if 'name' in frame.value.columns).set_index('name')
        self.assertEqual(measured.loc['battery_voltage', 'value'], '52.4')
        self.assertEqual(measured.loc['load_power', 'unit'], 'W')
        session.client.publish.assert_not_called()
        self.choose(app, 'PI30', 'QPIGS')
        self.widget(app.button, 'Send query').click().run()
        session.client.publish.assert_called_once()
        topic, encoded = session.client.publish.call_args.args
        payload = json.loads(encoded)
        self.assertEqual(topic, session.base + '/command')
        self.assertEqual(payload['protocol'], 'PI30')
        self.assertEqual(payload['command'], 'QPIGS')
        self.assertEqual(payload['operation'], 'protocol')
        self.assertNotIn('confirmed', payload)
        session.receive(session.base + '/result',
                        dict(member_id=2, command_id=payload['command_id'],
                             command='QPIGS', protocol='PI30', operation='protocol',
                             outcome='completed', write_sent=False, detail='Read complete'))
        app.run()
        self.assertFalse(app.exception)
        self.assertTrue(any('Query completed' in message.value for message in app.success))
        session.client.publish.assert_called_once()

    def test_uncertain_setting_blocks_only_setters_and_shows_mismatch(self):
        session = NativeMonitorStub()
        session.state['native_write_blocked'] = True
        session.state['protocol_mismatch'] = 'PI30MAX'
        app = self.app(session)
        self.assertTrue(any('Device reports PI30MAX' in message.value for message in app.warning))
        self.assertTrue(self.widget(app.button, 'Send query').disabled)
        session.state['protocol_mismatch'] = ''
        app.run()
        self.choose(app, 'PI30', 'POP', setter=True)
        self.assertTrue(self.widget(app.button, 'Send setting').disabled)
        self.choose(app, 'PI30', 'QPIGS')
        self.assertFalse(self.widget(app.button, 'Send query').disabled)
        self.assertFalse(session.commands)

    def test_live_groups_are_first_and_commands_are_collapsed_diagnostics(self):
        session = NativeMonitorStub()
        now = time.monotonic()
        session.state['native_records'] = [dict(protocol='PI30', command='QPIGS', sequence=1,
            timestamp_ms=1, received_at=now, fields={
                'pv_power': {'value': 830, 'unit': 'W'},
                'battery_voltage': {'value': 52.4, 'unit': 'V'},
                'ac_output_active_power': {'value': 1500, 'unit': 'W'},
                'ac_output_load': {'value': 25, 'unit': '%'}})]
        app = self.app(session)
        self.assertEqual(app.tabs[0].label, 'Live data')
        self.assertEqual([metric.label for metric in app.metric], ['PV', 'Grid', 'Battery', 'Output', 'Load'])
        headings = [heading.value for heading in app.tabs[0].get('subheader')]
        self.assertIn('PV', headings)
        self.assertIn('Battery', headings)
        self.assertNotIn('QPIGS', headings)
        self.assertNotIn('QPI', headings)
        for label in ('Manual commands and settings', 'Protocol records and delivery diagnostics'):
            expander = next(item for item in app.expander if item.label == label)
            self.assertFalse(expander.proto.expanded)
        self.assertFalse(session.commands)

    def test_identity_and_standby_do_not_replace_missing_or_stale_power(self):
        session = NativeMonitorStub()
        now = time.monotonic()
        session.state['native_records'] = [dict(protocol='PI30', command='QMOD', sequence=1,
            timestamp_ms=1, received_at=now, fields={'device_mode': {'value': 'Standby', 'unit': ''}}),
            dict(protocol='PI30', command='QPI', sequence=2, timestamp_ms=2, received_at=now,
                 fields={'protocol_id': {'value': 'PI30', 'unit': ''}})]
        app = self.app(session)
        self.assertTrue(all(metric.value == 'Unavailable' for metric in app.metric))
        self.assertTrue(any('Waiting for fresh' in item.value for item in app.info))
        status = next(frame.value for frame in app.dataframe if 'Measurement' in frame.value.columns)
        self.assertEqual(status.iloc[0]['Value'], 'Standby')
        self.assertTrue(status.iloc[0]['Updated'].startswith('Fresh'))
        session.state['native_records'].insert(0, dict(protocol='PI30', command='QPIGS', sequence=0,
            timestamp_ms=0, received_at=now - 60,
            fields={'pv_power': {'value': 830, 'unit': 'W'}}))
        app.run()
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'PV'), 'Stale')
        self.assertTrue(any('Live: Stale' in caption.value for caption in app.caption))
        self.assertFalse(session.commands)

    def test_real_zero_is_distinct_from_missing_and_stale(self):
        session = NativeMonitorStub()
        now = time.monotonic()
        session.state['native_records'] = [dict(protocol='PI30', command='QPIGS', sequence=1,
            timestamp_ms=1, received_at=now,
            fields={'pv_power': {'value': 0, 'unit': 'W'},
                    'battery_voltage': {'value': 0, 'unit': 'V'}})]
        app = self.app(session)
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'PV'), '0.0 kW')
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Battery'), 'Unavailable')
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Grid'), 'Unavailable')
        self.assertFalse(any('Waiting for fresh' in item.value for item in app.info))
        self.assertFalse(session.commands)

    def test_power_view_distinguishes_active_apparent_and_signed_battery(self):
        session = NativeMonitorStub()
        record = dict(protocol='PI30', command='QPIGS', sequence=1,
            timestamp_ms=1, received_at=time.monotonic(), fields={
                'grid_active_power': {'value': 900, 'unit': 'W'},
                'grid_apparent_power': {'value': 1000, 'unit': 'VA'},
                'ac_output_active_power': {'value': 1200, 'unit': 'W'},
                'ac_output_apparent_power': {'value': 1500, 'unit': 'VA'},
                'battery_voltage': {'value': 50, 'unit': 'V'},
                'battery_discharge_current': {'value': 4, 'unit': 'A'},
                'battery_charging_current': {'value': 2, 'unit': 'A'}})
        session.state['native_records'] = [record]
        app = self.app(session)
        power = next(frame.value for frame in app.dataframe if 'Circuit' in frame.value.columns).set_index('Circuit')
        self.assertEqual(power.loc['Grid', 'Active · kW'], '0.9')
        self.assertEqual(power.loc['Grid', 'Apparent · kVA'], '1.0')
        self.assertEqual(power.loc['Load', 'Active · kW'], '1.2')
        self.assertEqual(power.loc['Load', 'Apparent · kVA'], '1.5')
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Battery'), '+0.1 kW')
        self.assertTrue(any('+ discharge / − charge' in caption.value for caption in app.caption))
        record['fields']['battery_charging_current']['value'] = 6
        app.run()
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Battery'), '-0.1 kW')
        record['fields']['battery_charging_current']['value'] = 4
        app.run()
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Battery'), '+0.0 kW')
        record['received_at'] = time.monotonic() - 60
        app.run()
        power = next(frame.value for frame in app.dataframe if 'Circuit' in frame.value.columns)
        self.assertTrue(all(value == 'Stale' for value in power['Active · kW']))
        self.assertTrue(all(value == 'Stale' for value in power['Apparent · kVA']))
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Battery'), 'Stale')
        self.assertFalse(session.commands)

    def test_unsigned_battery_power_and_grid_voltage_do_not_replace_known_power(self):
        session = NativeMonitorStub('PI17')
        session.state['native_records'] = [dict(protocol='PI17', command='PS', sequence=1,
            timestamp_ms=1, received_at=time.monotonic(), fields={
                'grid_voltage': {'value': 230, 'unit': 'V'},
                'battery_power': {'value': 500, 'unit': 'W'}})]
        app = self.app(session)
        power = next(frame.value for frame in app.dataframe if 'Circuit' in frame.value.columns)
        self.assertTrue(all(value == 'Unavailable' for value in power['Active · kW']))
        self.assertTrue(all(value == 'Unavailable' for value in power['Apparent · kVA']))
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Grid'), 'Unavailable')
        self.assertEqual(next(metric.value for metric in app.metric if metric.label == 'Battery'), 'Unavailable')
        measurements = [frame.value for frame in app.dataframe if 'Measurement' in frame.value.columns]
        self.assertTrue(any('Battery power' in set(frame['Measurement']) for frame in measurements))
        self.assertFalse(session.commands)

    def test_bms_shows_pack_current_soc_temperature_and_cells_in_normal_view(self):
        session = NativeMonitorStub('JK02')
        session.state['native_records'] = [dict(protocol='JK02', command='getCellData', sequence=1,
            timestamp_ms=1, received_at=time.monotonic(), fields={
                'voltage_cell01': {'value': 3.25, 'unit': 'V'},
                'battery_voltage': {'value': 52, 'unit': 'V'},
                'battery_current': {'value': -3, 'unit': 'A'},
                'percent_remain': {'value': 75, 'unit': '%'},
                'mos_temp': {'value': -5, 'unit': 'C'},
                'delta_cell_voltage': {'value': .01, 'unit': 'V'}})]
        app = self.app(session)
        self.assertEqual([metric.label for metric in app.metric],
                         ['Battery', 'Current', 'State of charge', 'Temperature', 'Cell difference'])
        self.assertEqual([metric.value for metric in app.metric], ['+0.156 kW', '-3 A', '75 %', '-5 C', '0.01 V'])
        headings = [heading.value for heading in app.tabs[0].get('subheader')]
        self.assertNotIn('PV', headings)
        self.assertNotIn('Grid', headings)
        self.assertNotIn('getCellData', headings)
        battery = next(frame.value for frame in app.dataframe
                       if 'Measurement' in frame.value.columns and 'Voltage cell01' in set(frame.value['Measurement']))
        self.assertEqual(len(battery), 6)
        self.assertFalse(session.commands)


if __name__ == '__main__':
    unittest.main()
