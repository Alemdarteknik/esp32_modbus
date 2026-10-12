"""Signed DC power and separate AC active/apparent power; no device I/O."""
import unittest

from native_live import normalize_native_live
from test_native_all_profiles import observation


class NativePowerTests(unittest.TestCase):
    def live(self, profile, command, values, now=100):
        return normalize_native_live([observation(profile, command, values)], profile, 0, now)

    def test_split_battery_currents_use_discharge_positive_charge_negative(self):
        for profile, command, charge_key in (
            ('PI18', 'GS', 'battery_charging_current'),
            ('PI18LVX', 'GS', 'battery_charge_current'),
            ('PI18SV', 'GS', 'battery_charge_current'),
            *((p, 'QPIGS', 'battery_charging_current') for p in
              ('PI30', 'PI30MAX', 'PI30MST', 'PI30M044', 'PI30M045', 'PI41')),
        ):
            for charge, discharge, expected in ((2, 4, .1), (4, 2, -.1), (0, 0, 0)):
                with self.subTest(profile=profile, charge=charge, discharge=discharge):
                    live = self.live(profile, command, [('battery_voltage', 'V', 50),
                        (charge_key, 'A', charge), ('battery_discharge_current', 'A', discharge)])
                    self.assertEqual(live['power_summary']['battery_kw'], expected)
                    self.assertTrue(live['power_quality']['battery_kw']['fresh'])

    def test_bms_signed_current_conventions_and_unsigned_power_are_normalized(self):
        profiles = (
            *((p, 'getCellData', 'battery_voltage', 'battery_current', -1) for p in
              ('JK02', 'JK02_32', 'JKV11', 'JKPB')),
            ('JK232', 'getBalancerData', 'total_battery_voltage', 'total_current', -1),
            ('JKSERIAL', 'getBalancerData', 'battery_voltage', 'battery_current', -1),
            ('DALY', 'SOC', 'battery_bank_voltage', 'current', 1),
            ('DALY40', 'SOC', 'battery_bank_voltage', 'current', 1),
            ('VED', 'vedtext', 'v', 'i', -1),
        )
        for profile, command, voltage, current, sign in profiles:
            for amps in (-3, 0, 3):
                with self.subTest(profile=profile, current=amps):
                    values = [(voltage, 'V', 52), (current, 'A', amps)]
                    if profile in {'JK02', 'JK02_32', 'JKV11', 'JKPB'}:
                        values.append(('battery_power', 'W', 999))
                    live = self.live(profile, command, values)
                    self.assertAlmostEqual(live['power_summary']['battery_kw'], sign * 52 * amps / 1000)
                    if profile in {'JK02', 'JK02_32', 'JKV11', 'JKPB'}:
                        self.assertEqual(live['groups']['battery']['fields']['battery_power']['value'], 999)
        for watts in (-1000, 0, 1000):
            live = self.live('VED', 'vedtext', [('p', 'W', watts), ('v', 'V', 52), ('i', 'A', 3)])
            self.assertEqual(live['power_summary']['battery_kw'], -watts / 1000)

    def test_pi17_power_magnitude_requires_matching_direction(self):
        for profile in ('PI17', 'PI17M058', 'PI17INFINI'):
            for direction, expected in (('Charging', -.1), ('Discharging', .1)):
                for magnitude in (-100, 100):
                    with self.subTest(profile=profile, direction=direction, magnitude=magnitude):
                        live = self.live(profile, 'PS', [('battery_power', 'W', magnitude),
                            ('battery_power_direction', '', direction)])
                        self.assertEqual(live['power_summary']['battery_kw'], expected)
            self.assertNotIn('battery_kw', self.live(profile, 'PS', [('battery_power', 'W', 100)])['power_summary'])
            self.assertEqual(self.live(profile, 'PS', [('battery_power', 'W', 0)])['power_summary']['battery_kw'], 0)
        records = [observation('PI17', 'PS', [('battery_power', 'W', 100)]),
                   observation('PI17', 'PS', [('battery_power_direction', '', 'Charging')], 2)]
        self.assertNotIn('battery_kw', normalize_native_live(records, 'PI17', 0, 100)['power_summary'])

    def test_revo_bms_fallback_and_qall_use_only_measured_currents(self):
        for command, names in (
            ('QLITH0', ('battery_voltage_from_bms', 'battery_charging_current_from_bms',
                       'battery_discharge_current_from_bms')),
            ('QALL', ('battery_voltage', 'battery_charging_current', 'battery_discharging_current')),
        ):
            live = self.live('PI30REVO', command, list(zip(names, ('V', 'A', 'A'), (50, 2, 4))))
            self.assertEqual(live['power_summary']['battery_kw'], .1)
        self.assertNotIn('battery_kw', self.live('PI30REVO', 'QPIGS', [
            ('battery_voltage', 'V', 50), ('battery_charging_current', 'A', 2),
            ('daily_energy_consumed', 'kWh', 4)])['power_summary'])

    def test_grid_three_phase_apparent_never_becomes_active(self):
        values = [(f'ac_input_voltage_{p}', 'V', v) for p, v in zip('rst', (230, 231, 232))]
        values += [(f'ac_input_current_{p}', 'A', i) for p, i in zip('rst', (1, -2, 3))]
        live = self.live('PI17', 'GS', values)
        self.assertAlmostEqual(live['power_summary']['grid_apparent_kva'], 1.388)
        self.assertNotIn('grid_kw', live['power_summary'])
        self.assertNotIn('grid_apparent_kva', self.live('PI17', 'GS', values[:-1])['power_summary'])
        split = [observation('PI17', 'GS', values[:3]), observation('PI17', 'GS', values[3:], 2)]
        self.assertNotIn('grid_apparent_kva', normalize_native_live(split, 'PI17', 0, 100)['power_summary'])

    def test_reported_active_and_apparent_power_keep_distinct_units(self):
        live = self.live('PI30', 'QPIGS', [('ac_output_active_power', 'W', 900),
            ('ac_output_apparent_power', 'VA', 1200), ('grid_active_power', 'kW', .7),
            ('grid_apparent_power', 'kVA', 1)])
        self.assertEqual(live['power_summary']['load_kw'], .9)
        self.assertEqual(live['power_summary']['load_apparent_kva'], 1.2)
        self.assertEqual(live['power_summary']['grid_kw'], .7)
        self.assertEqual(live['power_summary']['grid_apparent_kva'], 1)

    def test_incomplete_mixed_and_rated_battery_inputs_remain_unavailable(self):
        complete = [('battery_voltage', 'V', 50), ('battery_charging_current', 'A', 2),
                    ('battery_discharge_current', 'A', 4)]
        for missing in range(3):
            self.assertNotIn('battery_kw', self.live('PI30', 'QPIGS',
                complete[:missing] + complete[missing + 1:])['power_summary'])
        for bad in (('battery_charging_current', 'A', -2), ('battery_voltage', 'V', 0),
                    ('battery_discharge_current', 'VA', 4)):
            values = [part for part in complete if part[0] != bad[0]] + [bad]
            self.assertNotIn('battery_kw', self.live('PI30', 'QPIGS', values)['power_summary'])
        split = [observation('PI30', 'QPIGS', complete[:1]), observation('PI30', 'QPIGS', complete[1:], 2)]
        self.assertNotIn('battery_kw', normalize_native_live(split, 'PI30', 0, 100)['power_summary'])
        self.assertEqual(self.live('PI30', 'QPIRI', complete + [
            ('ac_input_voltage', 'V', 230), ('ac_input_current', 'A', 10)])['power_summary'], {})
        for profile, command in (('JK04', 'getCellData'), ('JK485', 'getBalancerData')):
            live = self.live(profile, command, [('total_battery_voltage', 'V', 50), ('balance_current', 'A', 4)])
            self.assertNotIn('battery_kw', live['power_summary'])

    def test_calculated_power_expires_and_preserves_its_signed_last_value(self):
        live = self.live('PI30', 'QPIGS', [('battery_voltage', 'V', 50),
            ('battery_charging_current', 'A', 2), ('battery_discharge_current', 'A', 0)], now=116)
        self.assertEqual(live['power_summary']['battery_kw'], -.1)
        self.assertTrue(live['power_quality']['battery_kw']['stale'])
        self.assertEqual(live['power_quality']['battery_kw']['age_seconds'], 16)


if __name__ == '__main__':
    unittest.main()
