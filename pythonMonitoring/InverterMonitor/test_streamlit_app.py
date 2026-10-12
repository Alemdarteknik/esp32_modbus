import unittest
from pathlib import Path
from unittest.mock import Mock
from streamlit.testing.v1 import AppTest
from mqtt_backend import MonitorSession


class AppTests(unittest.TestCase):
    def test_startup_unavailable_then_readings_arrive(self):
        session = MonitorSession()
        session.active = True
        unavailable = {'available': False, 'stale': True, 'state': 'unavailable', 'since_valid_ms': None}
        session.live = {'core_status_fresh': True,
                        'power_summary': {'grid_kw': None, 'load_kw': None, 'inverter_kw': None,
                                          'inverter_apparent_kva': 0.1, 'pv_kw': None, 'battery_kw': None},
                        'power_quality': {'grid_kw': dict(unavailable), 'pv_kw': dict(unavailable)},
                        'sections': {'grid': [], 'pv': [], 'battery': []}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        power = app.dataframe[0].value.set_index('Circuit')
        self.assertEqual(power.loc['Grid', 'Active · kW'], 'Unavailable')
        self.assertEqual(power.loc['Inverter', 'Apparent · kVA'], '0.1')
        session.live['power_summary']['grid_kw'] = 2.88
        session.live['power_quality']['grid_kw'] = {'available': True, 'stale': False, 'state': 'valid', 'since_valid_ms': 0}
        session.live['sections']['grid'] = [{'name': 'Grid active power', 'value': 2.88, 'unit': 'kW'}]
        app.run()
        self.assertFalse(app.exception)
        self.assertEqual(app.dataframe[0].value.set_index('Circuit').loc['Grid', 'Active · kW'], '2.88')
        self.assertTrue(any('name' in frame.value.columns for frame in app.dataframe))
        session.live['power_summary']['grid_kw'] = None
        session.live['power_quality']['grid_kw'] = unavailable
        session.live['sections']['grid'] = []
        app.run()
        self.assertFalse(app.exception)
        self.assertEqual(app.dataframe[0].value.set_index('Circuit').loc['Grid', 'Active · kW'], 'Unavailable')
        self.assertFalse(any('name' in frame.value.columns for frame in app.dataframe))

    def test_latest_payload_without_grid_direction(self):
        session = MonitorSession()
        session.active = True
        session.live = {'power_summary': {'grid_kw': 0.02, 'grid_apparent_kva': 1.46,
                        'inverter_kw': 3.2, 'inverter_apparent_kva': 3.29,
                        'load_kw': 3.29, 'load_apparent_kva': 3.69, 'pv_kw': 3.17,
                        'battery_kw': 0, 'load_percentage': 27.38, 'rated_power_kw': 12},
                        'sections': {'grid': [{'name': 'Grid active power', 'value': 0.02,
                            'unit': 'kW', 'source': 'unverified_R320_register',
                            'description': 'R320 total AC power to user; grid-intake interpretation UNVERIFIED'}]}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        self.assertFalse(any('UNVERIFIED' in w.value for w in app.warning))
        power = app.dataframe[0].value.set_index('Circuit')
        self.assertEqual(list(power.index), ['Grid', 'Inverter', 'Load'])
        self.assertEqual(power.loc['Grid', 'Active · kW'], '0.02')
        metrics = app.dataframe[1].value
        self.assertFalse(any('System generation' in x for x in metrics['Field']))
        grid = next(f.value for f in app.dataframe if 'name' in f.value.columns)
        self.assertEqual(grid.iloc[0]['source'], 'unverified_R320_register')

    def test_section_since_valid_and_percentage_without_source(self):
        session = MonitorSession()
        session.active = True
        session.live = {'power_summary': {'load_kw': 3.51, 'battery_kw': 0,
                        'load_percentage': 29.22, 'load_percentage_basis': 'unavailable'},
                        'power_quality': {'load_kw': {'available': True, 'stale': True, 'state': 'last_known'}},
                        'sections': {'battery': [{'name': 'battery_kw', 'value': 0, 'unit': 'kW',
                            'description': 'Last valid result; not a new measurement',
                            'source': 'last_known', 'stale': True, 'since_valid_ms': 5013}]}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        power = app.dataframe[0].value.set_index('Circuit')
        self.assertEqual(power.loc['Load', 'Active · kW'], '3.51')
        frame = next(f.value for f in app.dataframe if 'name' in f.value.columns)
        self.assertEqual(frame.iloc[0]['since_valid_ms'], '5013')
        self.assertEqual(frame.iloc[0]['value'], '0')
        self.assertLess(list(frame.columns).index('since_valid_ms'), list(frame.columns).index('description'))
        self.assertTrue(any('Basis: unavailable' in c.value for c in app.caption))

    def test_power_quality_last_known_and_unverified_grid(self):
        session = MonitorSession()
        session.active = True
        session.live = {'power_summary': {'inverter_kw': 3.41, 'grid_kw': 0.91,
                        'load_kw': 3.5, 'battery_kw': 0,
                        'grid_direction': 'unverified_grid_intake_candidate_R320'},
                        'power_quality': {'inverter_kw': {'available': True, 'stale': True,
                        'state': 'last_known', 'since_valid_ms': 5950},
                        'load_kw': {'available': False, 'state': 'unavailable'},
                        'battery_kw': {'available': True, 'stale': False, 'state': 'valid'}}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        power = app.dataframe[0].value.set_index('Circuit')
        self.assertEqual(power.loc['Inverter', 'Active · kW'], '3.41')
        self.assertEqual(power.loc['Load', 'Active · kW'], 'Unavailable')
        self.assertTrue(any('UNVERIFIED' in w.value for w in app.warning))
        quality = next(f.value for f in app.dataframe if 'since_valid_ms' in f.value.columns)
        self.assertEqual(quality.iloc[0]['since_valid_ms'], '5950')

    def test_categories_and_physical_change_update_dropdown(self):
        session = MonitorSession()
        session.active = session.ready = True
        session.client = Mock()
        session.client.publish.return_value.rc = 0
        session.base = 'inverter/INV-TEST/inverter/0'
        key = next(k for k, s in session.catalog.items() if s.name == 'ChargeSourcePriority')
        item = session.catalog[key]
        session.cache[key] = 0
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        category = next(w for w in app.selectbox if w.label == 'Category')
        category.select(item.domain).run()
        choices = next(w for w in app.selectbox if w.label == 'Edit setting')
        self.assertTrue(all(session.catalog[k].domain == item.domain for k in choices.options
                            if k in session.catalog))
        choices.select(key).run()
        self.assertEqual(session.focus_key, key)
        self.assertEqual(next(w for w in app.selectbox if w.label == 'Value').value, 'CSO - Solar first')
        session.send('get', key)
        session.receive(session.base + '/result', dict(member_id=0, command_id=session.pending['command_id'],
                        operation='get', name=item.name, outcome='confirmed', value=2))
        app.run()
        self.assertFalse(app.exception)
        self.assertEqual(next(w for w in app.selectbox if w.label == 'Value').value, 'OSO - Solar charging only')
        tables = [f.value for f in app.dataframe if 'Current' in f.value.columns]
        row = next(frame[frame['Name'] == item.name].iloc[0] for frame in tables if item.name in list(frame['Name']))
        self.assertEqual(row['Current'], 'OSO - Solar charging only')
        self.assertEqual(row['Read status'], 'Read confirmed')

    def test_new_load_format_and_rating(self):
        session = MonitorSession()
        session.active = True
        session.live = {'power_summary': {'load_kw': 1.59, 'load_apparent_kva': 1.83,
                        'rated_power_kw': 12, 'rating_source': 'default_12_kw',
                        'load_percentage': 13.22, 'load_percentage_source': 'calculated',
                        'load_percentage_basis': 'active_power'},
                        'sections': {'load': [{'name': 'Load active power', 'value': 1.59, 'unit': 'kW'}]}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        power = app.dataframe[0].value.set_index('Circuit')
        self.assertNotIn('Output', power.index)
        self.assertEqual(power.loc['Load', 'Active · kW'], '1.59')
        metrics = app.dataframe[1].value.set_index('Field')
        self.assertEqual(metrics.loc['Load · %', 'Value'], '13.22')
        self.assertEqual(metrics.loc['Reported rating · kW', 'Value'], '12')
        self.assertNotIn('Local load · kW', metrics.index)
        self.assertTrue(any('default_12_kw' in c.value for c in app.caption))
        session.live['power_summary']['output_kw'] = 2.39
        app.run()
        self.assertIn('Output', list(app.dataframe[0].value['Circuit']))

    def test_sequential_sampling_and_stale_flags(self):
        session = MonitorSession()
        session.active = True
        session.live = {'power_summary': {'sampling': 'sequential_cached_readings', 'freshness_limit_ms': 120000},
                        'sections': {'pv': [
                            {'name': 'PV voltage', 'value': 184.1, 'unit': 'V', 'age_ms': 130000, 'stale': True},
                            {'name': 'PV current', 'value': 0, 'unit': 'A', 'age_ms': 0, 'stale': False},
                            {'name': 'PV power', 'value': 1.92, 'unit': 'kW'}]}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        frame = next(f.value for f in app.dataframe if 'stale' in f.value.columns)
        self.assertEqual(list(frame.columns)[:5], ['name', 'value', 'unit', 'age_ms', 'stale'])
        self.assertEqual(list(frame['stale']), ['True', 'False', ''])
        self.assertEqual(frame.iloc[0]['value'], '184.1')
        self.assertTrue(any('1 readings are marked stale' in w.value for w in app.warning))
        self.assertTrue(any('sampled at different times' in c.value for c in app.caption))

    def test_all_live_sections_and_columns_visible(self):
        session = MonitorSession()
        session.active = True
        session.live = {'sections': {
            'pv': [{'name': 'PV1 Voltage', 'description': 'PV voltage', 'value': 268.4, 'unit': 'V'}],
            'battery': [{'name': 'SOC', 'value': 0, 'unit': '%'}],
            'bms': [{'name': 'Battery temperature', 'value': None, 'unit': 'C'}],
            'future_section': [{'name': 'New metric', 'value': False, 'source': 'register'}],
            'generator': []}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        frames = [frame.value for frame in app.dataframe if 'name' in frame.value.columns]
        self.assertEqual(sum(len(frame) for frame in frames), 4)
        self.assertEqual(list(frames[0].columns)[:3], ['name', 'value', 'unit'])
        self.assertEqual(frames[1].iloc[0]['value'], '0')
        self.assertEqual(frames[2].iloc[0]['value'], 'Unavailable')
        self.assertEqual(frames[3].iloc[0]['value'], 'False')
        self.assertFalse(any(w.label == 'Measurements' for w in app.selectbox))

    def test_nullable_power_and_reading_age(self):
        session = MonitorSession()
        session.active = True
        session.live = {'bms_configured': True, 'bms_connected': None,
                        'power_summary': {'inverter_kw': None, 'load_kva': 3.35, 'local_load_kw': None,
                                          'power_to_user_kw': None, 'battery_kw': 0},
                        'sections': {'inverter': [{'name': 'Voltage', 'description': 'Voltage',
                                                  'value': 247, 'unit': 'V', 'age_ms': 1672}]}}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        power = app.dataframe[0].value.set_index('Circuit')
        self.assertEqual(power.loc['Inverter', 'Active · kW'], 'Unavailable')
        self.assertEqual(power.loc['Load', 'Apparent · kVA'], '3.35')
        measurements = next(f.value for f in app.dataframe if 'age_ms' in f.value.columns)
        self.assertEqual(list(measurements.columns)[:4], ['name', 'value', 'unit', 'age_ms'])
        self.assertTrue(any('connection status is unknown' in c.value for c in app.caption))

    def test_disconnected_start(self):
        app = AppTest.from_file(str(Path(__file__).with_name('app.py'))).run(timeout=20)
        self.assertFalse(app.exception)
        self.assertEqual(app.text_input[0].label, 'Inverter ID')

    def test_connected_payload_and_settings(self):
        session = MonitorSession()
        session.active = session.ready = True
        session.status = 'Connected'
        session.live = {'power_summary': {'grid_kw': 1.26, 'load_kw': 0.13},
                        'sections': {'grid': [{'name': 'Voltage', 'value': 247, 'unit': 'V'}]}}
        session.client = Mock()
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        self.assertFalse(app.exception)
        self.assertTrue(app.text_input[0].disabled)
        self.assertEqual(len(app.tabs), 4)
        app.run()
        session.client.publish.assert_not_called()

    def test_choice_survives_refresh_and_set_is_not_repeated(self):
        session = MonitorSession()
        session.active = session.ready = True
        key = next(k for k, s in session.catalog.items() if s.name == 'ChargeSourcePriority')
        session.cache[key] = 0
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        next(w for w in app.selectbox if w.label == 'Edit setting').select(key).run()
        selector = next(w for w in app.selectbox if w.label == 'Value')
        self.assertEqual(selector.value, 'CSO - Solar first')
        selector.select('OSO - Solar charging only').run()
        app.run()
        self.assertEqual(next(w for w in app.selectbox if w.label == 'Value').value, 'OSO - Solar charging only')
        next(w for w in app.checkbox if w.label.startswith('I have checked')).check().run()
        next(w for w in app.button if w.label == 'Set').click().run()
        self.assertFalse(app.exception)
        self.assertEqual(session.jobs, [('set', key, 2)])
        app.run()
        self.assertEqual(session.jobs, [('set', key, 2)])

    def test_disconnect_clears_ui(self):
        session = MonitorSession()
        session.active = True
        session.live = {'sequence': 123}
        app = AppTest.from_file(str(Path(__file__).with_name('app.py')))
        app.session_state['monitor'] = session
        app.run(timeout=20)
        next(w for w in app.button if w.label == 'Disconnect').click().run()
        self.assertFalse(app.exception)
        self.assertFalse(app.session_state['monitor'].live)
        self.assertFalse(app.text_input[0].disabled)
