"""Normal live telemetry across every native profile; no network/device I/O."""
import unittest

from native_live import normalize_native_live, native_measurement_received_at
from native_protocols import NativeTelemetryCollector, PROTOCOL_IDS


def observation(protocol, command, values, sequence=1, received_at=100):
    return dict(protocol=protocol, member_id=0, command=command[:15], sequence=sequence,
                timestamp_ms=sequence * 1000, received_at=received_at,
                fields={name: {'value': value, 'unit': unit} for name, unit, value in values})


def profile_samples():
    """Use actual decoded field spellings/units, including distinct PV channels."""
    samples = {'PI16': [observation('PI16', 'QPIGS', [
        ('grid_voltage', 'V', 230), ('output_power', 'W', 1200),
        ('output_load_percent', '%', 24), ('positive_battery_voltage', 'V', 52),
        ('pv1_input_power', 'W', 500), ('pv2_input_power', 'W', 0), ('pv3_input_power', 'W', 700)])]}
    for profile in ('PI17', 'PI17M058', 'PI17INFINI'):
        samples[profile] = [observation(profile, 'GS', [
            ('solar_input_voltage_1', 'V', 300), ('solar_input_voltage_2', 'V', 310),
            ('battery_voltage', 'V', 52), ('battery_current', 'A', -3),
            ('ac_input_voltage_r', 'V', 230), ('ac_output_voltage_r', 'V', 230)]),
            observation(profile, 'PS', [('solar_input_power_1', 'W', 500),
                ('solar_input_power_2', 'W', 700), ('battery_power', 'W', -156),
                ('battery_power_direction', '', 'Charging'),
                ('ac_input_total_active_power', 'W', -300), ('ac_output_total_active_power', 'W', 1200),
                ('ac_output_total_apparent_power', 'VA', 1500), ('ac_output_power_percentage', '%', 24)], 2)]
    for profile in ('PI18', 'PI18LVX', 'PI18SV'):
        samples[profile] = [observation(profile, 'GS', [
            ('pv1_input_power', 'W', 500), ('pv2_input_power', 'W', 700),
            ('battery_voltage', 'V', 52), ('ac_output_active_power', 'W', 1200),
            ('ac_output_apparent_power', 'VA', 1500), ('output_load_percent', '%', 24)])]
    for profile in ('PI30', 'PI30REVO'):
        samples[profile] = [observation(profile, 'QPIGS', [
            ('pv_input_power', 'W', 1200), ('battery_voltage', 'V', 52),
            ('ac_output_active_power', 'W', 1200), ('ac_output_apparent_power', 'VA', 1500),
            ('ac_output_load', '%', 24)])]
    for profile in ('PI30MAX', 'PI30M044', 'PI30M045', 'PI30MST'):
        samples[profile] = [observation(profile, 'QPIGS', [
            ('pv1_charging_power', 'W', 500), ('battery_voltage', 'V', 52),
            ('ac_output_active_power', 'W', 1200), ('ac_output_apparent_power', 'VA', 1500),
            ('ac_output_load', '%', 24)]),
            observation(profile, 'QPIGS2', [('pv2_charging_power', 'W', 700)], 2)]
        if profile == 'PI30MST':
            samples[profile][1]['fields'].update(pv3_charging_power={'unit': 'W', 'value': 300},
                                               pv_total_charging_power={'unit': 'W', 'value': 1500})
    samples['PI41'] = [observation('PI41', 'QPIGS', [
        ('l1_ac_input_voltage', 'V', 120), ('l1_ac_output_active_power', 'W', 500),
        ('l1_ac_output_apparent_power', 'VA', 600), ('l1_ac_output_load', '%', 20),
        ('battery_voltage', 'V', 52), ('pv_input_power', 'W', 1200)]),
        observation('PI41', 'QPIGS2', [('l2_ac_input_voltage', 'V', 120),
            ('l2_ac_output_active_power', 'W', 700), ('l2_ac_output_apparent_power', 'VA', 900),
            ('l2_ac_output_load', '%', 30)], 2)]
    for profile in ('JK02', 'JK02_32', 'JKV11', 'JKPB'):
        samples[profile] = [observation(profile, 'getCellData', [
            ('voltage_cell01', 'V', 3.25), ('battery_voltage', 'V', 52),
            ('battery_power', 'W', 156), ('battery_current', 'A', -3),
            ('percent_remain', '%', 75), ('mos_temp', 'C', 28), ('delta_cell_voltage', 'V', .01)])]
    samples['JK04'] = [observation('JK04', 'getCellData', [
        ('voltage_cell01', 'V', 3.25), ('resistance_cell01', 'Ohm', .002),
        ('average_cell_voltage', 'V', 3.26), ('delta_cell_voltage', 'V', .01)])]
    samples['JK232'] = [observation('JK232', 'getBalancerData', [
        ('total_battery_voltage', 'V', 52), ('total_current', 'A', -3),
        ('remaining_battery', '%', 75), ('ntc_01', 'C', 28)])]
    samples['JK485'] = [observation('JK485', 'getBalancerData', [
        ('voltage_cell01', 'V', 3.25), ('total_battery_voltage', 'V', 52),
        ('balance_current', 'A', .5), ('voltage_difference', 'V', .01), ('temperature', 'C', 28)])]
    samples['JKSERIAL'] = [observation('JKSERIAL', 'getBalancerData', [
        ('voltage_cell01', 'V', 3.25), ('battery_voltage', 'V', 52),
        ('battery_current', 'A', -3), ('percent_remain', '%', 75), ('mos_temp', 'C', 28)])]
    for profile in ('DALY', 'DALY40'):
        samples[profile] = [observation(profile, 'SOC', [
            ('battery_bank_voltage', 'V', 52), ('acquisition_voltage', 'V', 52),
            ('current', 'A', -3), ('soc', '%', 75)]),
            observation(profile, 'cellMinMaxTemps', [('maximum_cell_temperature', 'C', 28),
                ('minimum_cell_temperature', 'C', 24)], 2)]
    samples['VED'] = [observation('VED', 'vedtext', [
        ('pid', '', '0xA053'), ('v', 'V', 52), ('i', 'A', -3), ('p', 'W', -156),
        ('soc', '%', 75), ('vpv', 'V', 300), ('ppv', 'W', 1200),
        ('ac_out_v', 'V', 230), ('ac_out_s', 'VA', 1500), ('mppt', '', '2'),
        ('h19', 'kWh', 24), ('il', 'A', 1)])]
    return samples


class AllNativeProfileTests(unittest.TestCase):
    def test_every_native_profile_has_physical_live_telemetry(self):
        samples = profile_samples()
        self.assertEqual(set(samples), set(PROTOCOL_IDS) - {'AOHAI_MODBUS_RTU'})
        for profile, records in samples.items():
            with self.subTest(protocol=profile):
                collector = NativeTelemetryCollector()
                for record in records:
                    wire = {key: value for key, value in record.items() if key != 'received_at'}
                    wire.update(record_complete=True, sample_age_ms=0)
                    self.assertIsNotNone(collector.receive(wire, 100))
                live = normalize_native_live(collector.records(), profile, 0, 100)
                self.assertEqual(native_measurement_received_at(collector.records_view(), profile, 0),
                                 live['received_at'])
                self.assertTrue(live['telemetry_available'])
                self.assertEqual(collector.dropped_count, 0)
                self.assertEqual(sum(len(group['fields']) for group in live['groups'].values()),
                                 sum(len(record['fields']) for record in records))
                if profile.startswith('PI'):
                    self.assertEqual(live['power_summary']['pv_kw'], 1.5 if profile == 'PI30MST' else 1.2)
                    self.assertEqual(live['power_summary']['load_kw'], 1.2)
                elif profile == 'VED':
                    self.assertEqual(live['power_summary']['pv_kw'], 1.2)
                    self.assertEqual(live['power_summary']['battery_kw'], .156)
                    self.assertNotIn('load_kw', live['power_summary'])
                else:
                    self.assertEqual(live['device_type'], 'bms')
                    self.assertFalse(live['groups']['pv']['supported'])
                    self.assertTrue(live['groups']['battery']['fresh'])
                stale = normalize_native_live(collector.records(), profile, 0, 116)
                self.assertFalse(stale['telemetry_available'])
                self.assertTrue(all(flags['stale'] for flags in stale['power_quality'].values()))

    def test_missing_pv_channel_cannot_be_a_reported_total(self):
        for profile, names in [('PI16', ('pv1_input_power', 'pv2_input_power')),
                               ('PI18', ('pv1_input_power',)),
                               ('PI30MAX', ('pv1_charging_power',)),
                               ('PI30MST', ('pv1_charging_power', 'pv2_charging_power'))]:
            with self.subTest(protocol=profile):
                record = observation(profile, 'GS' if profile == 'PI18' else 'QPIGS',
                                     [(name, 'W', 500) for name in names])
                live = normalize_native_live([record], profile, 0, 100)
                self.assertNotIn('pv_kw', live['power_summary'])
                self.assertTrue(live['telemetry_available'])

    def test_channel_aggregation_uses_oldest_component_and_preserves_zero(self):
        records = [observation('PI30MAX', 'QPIGS', [('pv1_charging_power', 'W', 0)], 1, 80),
                   observation('PI30MAX', 'QPIGS2', [('pv2_charging_power', 'W', 700)], 2, 100)]
        live = normalize_native_live(records, 'PI30MAX', 0, 100)
        self.assertEqual(live['power_summary']['pv_kw'], .7)
        self.assertTrue(live['power_quality']['pv_kw']['stale'])
        self.assertEqual(live['power_quality']['pv_kw']['age_seconds'], 20)
        self.assertTrue(live['telemetry_available'])

    def test_reported_total_preferred_then_fresh_complete_channels(self):
        record = observation('PI30MST', 'QPIGS2', [
            ('pv1_charging_power', 'W', 500), ('pv2_charging_power', 'W', 700),
            ('pv3_charging_power', 'W', 300), ('pv_total_charging_power', 'W', 1490)])
        live = normalize_native_live([record], 'PI30MST', 0, 100)
        self.assertEqual(live['power_summary']['pv_kw'], 1.49)
        old_total = observation('PI30MST', 'QPIGS2', [('pv_total_charging_power', 'W', 1490)], 1, 80)
        fresh_channels = observation('PI30MST', 'QPIGS', [
            ('pv1_charging_power', 'W', 500), ('pv2_charging_power', 'W', 700),
            ('pv3_charging_power', 'W', 300)], 2, 100)
        live = normalize_native_live([old_total, fresh_channels], 'PI30MST', 0, 100)
        self.assertEqual(live['power_summary']['pv_kw'], 1.5)
        self.assertTrue(live['power_quality']['pv_kw']['fresh'])

    def test_pi41_phases_are_circuits_and_only_complete_phases_make_total(self):
        records = profile_samples()['PI41']
        live = normalize_native_live(records[:1], 'PI41', 0, 100)
        self.assertNotIn('load_kw', live['power_summary'])
        self.assertIn('l1_ac_input_voltage', live['groups']['grid']['fields'])
        self.assertIn('l1_ac_output_load', live['groups']['load']['fields'])
        live = normalize_native_live(records, 'PI41', 0, 100)
        self.assertEqual(live['power_summary']['load_kw'], 1.2)
        self.assertEqual(live['power_summary']['load_apparent_kva'], 1.5)
        self.assertNotIn('load_percentage', live['power_summary'])

    def test_bms_pack_reading_preferred_to_first_cell_and_temperature_sign_kept(self):
        records = profile_samples()['JK02']
        records[0]['fields']['mos_temp']['value'] = -5
        live = normalize_native_live(records, 'JK02', 0, 100)
        self.assertEqual(live['groups']['battery']['primary']['name'], 'battery_power')
        self.assertEqual(live['battery_metrics']['current']['value'], -3)
        self.assertEqual(live['battery_metrics']['state_of_charge']['value'], 75)
        self.assertEqual(live['battery_metrics']['temperature']['value'], -5)
        del records[0]['fields']['battery_power']
        self.assertEqual(normalize_native_live(records, 'JK02', 0, 100)
                         ['groups']['battery']['primary']['name'], 'battery_voltage')

    def test_ved_history_and_mppt_state_do_not_become_live_pv_power(self):
        record = observation('VED', 'vedtext', [('pid', '', '0xA053'), ('mppt', '', '2'),
                                               ('h21', 'W', 1400), ('h19', 'kWh', 24)])
        live = normalize_native_live([record], 'VED', 0, 100)
        self.assertFalse(live['telemetry_available'])
        self.assertEqual(live['power_summary'], {})
        self.assertIn('h21', live['groups']['energy']['fields'])
        self.assertIn('mppt', live['groups']['status']['fields'])

    def test_limits_and_model_information_never_freshen_electrical_sensors(self):
        records = [observation('PI30REVO', 'QLITH0', [
            ('battery_max_charging_current', 'A', 50), ('battery_max_charge_voltage', 'V', 54)]),
            observation('PI30REVO', 'DM', [('model_code', '', '0001')], 2)]
        live = normalize_native_live(records, 'PI30REVO', 0, 100)
        self.assertFalse(live['telemetry_available'])
        self.assertEqual(len(live['groups']['settings']['fields']), 2)
        self.assertIn('model_code', live['groups']['identity']['fields'])
        ved = normalize_native_live([observation('VED', 'batteryCapacity', [('battery_capacity', 'Ah', 200)])],
                                    'VED', 0, 100)
        self.assertFalse(ved['telemetry_available'])
        self.assertIn('battery_capacity', ved['groups']['settings']['fields'])


if __name__ == '__main__':
    unittest.main()
