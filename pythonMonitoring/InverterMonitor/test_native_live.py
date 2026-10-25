"""Live normalization and realistic PI30 streaming tests; no device I/O."""
import unittest

from native_live import (normalize_native_live, native_measurement_received_at,
                         _command_kind, _kind)
from native_protocols import NativeTelemetryCollector, PROFILES
from test_native_protocols import chunk


def pi30_fields():
    # Firmware-decoded QPIGS observations, including all bit fields (30 total).
    data = [
        ('ac_input_voltage', 'V', 230.4), ('ac_input_frequency', 'Hz', 50.0),
        ('ac_output_voltage', 'V', 230.0), ('ac_output_frequency', 'Hz', 50.0),
        ('ac_output_apparent_power', 'VA', 600), ('ac_output_active_power', 'W', 450),
        ('ac_output_load', '%', 9), ('bus_voltage', 'V', 400),
        ('battery_voltage', 'V', 52.4), ('battery_charging_current', 'A', 5),
        ('battery_capacity', '%', 84), ('inverter_heat_sink_temperature', 'C', 35),
        ('pv_input_current_for_battery', 'A', 6.0), ('pv_input_voltage', 'V', 310.4),
        ('battery_voltage_from_scc', 'V', 52.4), ('battery_discharge_current', 'A', 0),
        ('is_sbu_priority_version_added', 'bool', 0), ('is_configuration_changed', 'bool', 0),
        ('is_scc_firmware_updated', 'bool', 0), ('is_load_on', 'bool', 1),
        ('is_battery_voltage_to_steady_w_83d87da5', 'bool', 0),
        ('is_charging_on', 'bool', 1), ('is_scc_charging_on', 'bool', 1),
        ('is_ac_charging_on', 'bool', 0), ('rsv1', 'A', 0), ('rsv2', 'A', 0),
        ('pv_input_power', 'W', 1862), ('is_charging_to_float', 'bool', 0),
        ('is_switched_on', 'bool', 1), ('is_reserved', 'bool', 0)]
    return {name: {'unit': unit, 'value': value} for name, unit, value in data}


def atomic(fields=None, **changes):
    data = chunk(fields=fields or pi30_fields())
    for key in ('chunk_index', 'chunk_count', 'final_chunk'):
        del data[key]
    data['record_complete'] = True
    data.update(changes)
    return data


class NativeLiveTests(unittest.TestCase):
    def collect(self, data=None, now=100):
        collector = NativeTelemetryCollector()
        self.assertIsNotNone(collector.receive(data or atomic(), now))
        return collector

    def normalize(self, collector, now=100):
        return normalize_native_live(collector.records(), 'PI30', 0, now)

    def test_complete_pi30_fifteen_chunks_reordered_with_duplicates(self):
        collector = NativeTelemetryCollector()
        values = list(pi30_fields().items())
        messages = [chunk(sequence=267, timestamp=365902, index=i, count=15,
                          fields=dict(values[i * 2:i * 2 + 2])) for i in range(15)]
        # Monotonic receipt order is independent of source chunk order.
        for order, index in enumerate((14, 0, 7, 8, 4, 1, 2, 10, 13, 5, 12, 3, 11, 9)):
            self.assertIsNone(collector.receive(messages[index], 100 + order / 100))
            self.assertIsNone(collector.receive(messages[index], 100 + order / 100 + .001))
        self.assertFalse(self.normalize(collector, 100.2)['telemetry_available'])
        record = collector.receive(messages[6], 100.2)
        self.assertEqual(record['fields'], pi30_fields())
        live = self.normalize(collector, 100.3)
        self.assertEqual(live['power_summary']['pv_kw'], 1.862)
        self.assertEqual(live['power_summary']['load_kw'], .45)
        self.assertEqual(live['power_summary']['load_apparent_kva'], .6)
        self.assertEqual(live['power_summary']['load_percentage'], 9)
        self.assertEqual(live['groups']['grid']['fields']['ac_input_voltage']['value'], 230.4)
        self.assertEqual(live['groups']['battery']['fields']['battery_capacity']['value'], 84)
        self.assertEqual(live['groups']['battery']['fields']['battery_discharge_current']['value'], 0)
        self.assertEqual(live['power_summary']['battery_kw'], -.262)
        self.assertNotIn('grid_kw', live['power_summary'])
        self.assertEqual(collector.dropped_count, 0)
        self.assertEqual(collector.diagnostics()['duplicate_messages'], 14)

    def test_atomic_complete_and_qos_redelivery_do_not_refresh(self):
        data = atomic()
        collector = self.collect(data)
        self.assertIsNone(collector.receive(data, 101))
        self.assertEqual(self.normalize(collector, 101)['received_at'], 100)
        self.assertEqual(collector.dropped_count, 0)
        self.assertEqual(collector.diagnostics()['duplicate_messages'], 1)
        self.assertEqual(collector.diagnostics()['completed_records'], 1)

    def test_atomic_rejects_marker_conflicts_oversize_and_malformed_fields(self):
        bad = []
        for marker in (False, 1, 'true'):
            bad.append(atomic(record_complete=marker))
        for key, value in (('chunk_index', 0), ('chunk_count', 1), ('final_chunk', True)):
            bad.append(atomic(**{key: value}))
        bad.append(atomic(fields={f'x{i}': {'unit': 'V', 'value': 1} for i in range(97)}))
        bad.append(atomic(fields={'x': {'unit': '12345678', 'value': 1}}))
        bad.append(atomic(fields={'x': {'unit': '', 'value': True}}))
        for age in (-1, True, 1.5, float('inf'), '10'):
            bad.append(atomic(sample_age_ms=age))
        for data in bad:
            with self.subTest(data=data):
                collector = NativeTelemetryCollector()
                self.assertIsNone(collector.receive(data, 100))
                self.assertEqual(collector.records(), [])
                self.assertEqual(collector.dropped_count, 1)

    def test_maximum_atomic_96_fields_and_long_valid_unit(self):
        fields = {f'cell_{i}': {'unit': '1234567', 'value': i} for i in range(96)}
        collector = self.collect(atomic(fields=fields))
        self.assertEqual(len(collector.records()[0]['fields']), 96)

    def test_standby_is_separate_observation_not_missing_power_default(self):
        collector = self.collect(atomic(fields={'device_mode': {'unit': '', 'value': 'Standby'}},
                                        command='QMOD'))
        live = self.normalize(collector)
        self.assertEqual(live['operating_status']['device_mode'], 'Standby')
        self.assertTrue(live['groups']['status']['fresh'])
        self.assertFalse(live['telemetry_available'])
        self.assertIsNone(live['received_at'])
        self.assertEqual(live['power_summary'], {})
        self.assertEqual(live['groups']['pv']['fields'], {})
        collector.receive(atomic(sequence=2, timestamp_ms=2000), 101)
        collector.receive(atomic(sequence=3, timestamp_ms=3000, command='QMOD',
            fields={'device_mode': {'unit': '', 'value': 'Line mode'}}), 120)
        live = self.normalize(collector, 120)
        self.assertFalse(live['telemetry_available'])
        self.assertTrue(live['groups']['status']['fresh'])
        self.assertTrue(live['groups']['pv']['stale'])
        self.assertEqual(live['received_at'], 101)

    def test_identity_and_settings_do_not_shadow_live_sensors(self):
        collector = self.collect()
        collector.receive(atomic(sequence=2, timestamp_ms=2000, command='QPIRI', fields={
            'ac_output_active_power': {'unit': 'W', 'value': 5000},
            'battery_voltage': {'unit': 'V', 'value': 48},
            'battery_type': {'unit': '', 'value': 'User'}}), 101)
        collector.receive(atomic(sequence=3, timestamp_ms=3000, command='QID', fields={
            'serial_number': {'unit': '', 'value': '92832207100756'}}), 110)
        live = self.normalize(collector, 117)
        self.assertEqual(live['power_summary']['load_kw'], .45)
        self.assertEqual(live['groups']['battery']['fields']['battery_voltage']['value'], 52.4)
        self.assertEqual(live['groups']['settings']['fields']['battery_voltage']['value'], 48)
        self.assertFalse(live['telemetry_available'])
        self.assertTrue(live['groups']['identity']['fresh'])
        self.assertTrue(live['groups']['settings']['fresh'])
        self.assertEqual(live['received_at'], 100)

    def test_factory_defaults_cannot_replace_current_settings(self):
        for protocol, current, defaults in (('PI30', 'QPIRI', 'QDI'), ('PI18LVX', 'PIRI', 'DI')):
            with self.subTest(protocol=protocol):
                collector = self.collect(atomic(protocol=protocol, command=current, fields={
                    'battery_type': {'unit': '', 'value': 'User'}}))
                collector.receive(atomic(protocol=protocol, command=defaults, sequence=2,
                    timestamp_ms=2000, fields={
                        'battery_type': {'unit': '', 'value': 'AGM'},
                        'battery_voltage': {'unit': 'V', 'value': 48}}), 101)
                live = normalize_native_live(collector.records(), protocol, 0, 101)
                self.assertEqual(live['groups']['settings']['fields']['battery_type']['value'], 'User')
                self.assertNotIn('battery_voltage', live['groups']['settings']['fields'])
                self.assertFalse(live['telemetry_available'])
                self.assertEqual(len(collector.records()), 2)

    def test_defaults_only_remain_diagnostic_without_current_telemetry(self):
        collector = self.collect(atomic(command='QDI', fields={
            'battery_voltage': {'unit': 'V', 'value': 48}}))
        live = self.normalize(collector)
        self.assertFalse(live['telemetry_available'])
        self.assertEqual(live['groups']['settings']['fields'], {})
        self.assertEqual(len(collector.records()), 1)

    def test_zero_and_stale_values_preserved_missing_not_inferred(self):
        collector = self.collect(atomic(fields={'pv_input_power': {'unit': 'W', 'value': 0}}))
        live = self.normalize(collector, 100)
        self.assertEqual(live['power_summary'], {'pv_kw': 0.0})
        self.assertTrue(live['telemetry_available'])
        self.assertIsNone(live['groups']['battery']['received_at'])
        stale = self.normalize(collector, 116)
        self.assertEqual(stale['power_summary']['pv_kw'], 0.0)
        self.assertTrue(stale['power_quality']['pv_kw']['stale'])
        self.assertFalse(stale['telemetry_available'])

    def test_no_apparent_to_active_conversion_or_unreported_grid_direction(self):
        collector = self.collect(atomic(fields={'ac_output_apparent_power': {'unit': 'VA', 'value': 1000},
                                               'ac_input_voltage': {'unit': 'V', 'value': 230}}))
        live = self.normalize(collector)
        self.assertEqual(live['power_summary'], {'load_apparent_kva': 1.0})
        self.assertNotIn('grid_direction', live['power_summary'])

    def test_profiles_and_members_cannot_cross_normalization_boundary(self):
        collector = self.collect()
        for protocol, member in (('PI30MAX', 0), ('PI30', 1)):
            live = normalize_native_live(collector.records(), protocol, member, 100)
            self.assertFalse(live['telemetry_available'])
            self.assertEqual(live['power_summary'], {})

    def test_bms_battery_and_ved_scaling_are_already_device_decoded(self):
        collector = self.collect(atomic(protocol='JK02', command='getCellData', fields={
            'battery_voltage': {'unit': 'V', 'value': 52.4},
            'battery_power': {'unit': 'W', 'value': 1000},
            'battery_current': {'unit': 'A', 'value': -19.0},
            'percent_remain': {'unit': '%', 'value': 75},
            'voltage_cell01': {'unit': 'V', 'value': 3.3}}))
        live = normalize_native_live(collector.records(), 'JK02', 0, 100)
        self.assertEqual(live['power_summary']['battery_kw'], .9956)
        self.assertFalse(live['groups']['grid']['supported'])
        self.assertEqual(live['groups']['battery']['fields']['battery_current']['value'], -19)
        collector = self.collect(atomic(protocol='VED', command='vedtext', fields={
            'ppv': {'unit': 'W', 'value': 500}, 'vpv': {'unit': 'V', 'value': 85.1},
            'v': {'unit': 'V', 'value': 52.4}, 'h19': {'unit': 'kWh', 'value': 20.5}}))
        live = normalize_native_live(collector.records(), 'VED', 0, 100)
        self.assertEqual(live['power_summary']['pv_kw'], .5)
        self.assertEqual(live['groups']['energy']['fields']['h19']['value'], 20.5)

    def test_snapshots_do_not_share_mutable_values(self):
        collector = self.collect()
        first = self.normalize(collector)
        first['groups']['pv']['fields']['pv_input_power']['value'] = 99
        self.assertEqual(self.normalize(collector)['power_summary']['pv_kw'], 1.862)

    def test_readonly_records_match_copied_records_at_freshness_boundaries(self):
        collector = self.collect()
        collector.receive(atomic(command='QPIGS2', sequence=2, timestamp_ms=2000,
            sample_age_ms=2000, fields={'pv2_charging_power': {'unit': 'W', 'value': 0}}), 101)
        for now in (101, 114, 115, 116, 401):
            with self.subTest(now=now):
                copied = normalize_native_live(collector.records(), 'PI30', 0, now,
                                               connected_pv_inputs=2)
                viewed = normalize_native_live(collector.records_view(), 'PI30', 0, now,
                                               connected_pv_inputs=2)
                self.assertEqual(viewed, copied)
                self.assertEqual(native_measurement_received_at(collector.records_view(), 'PI30', 0),
                                 viewed['received_at'])
                viewed['groups']['pv']['fields']['pv_input_power']['value'] = 99
                self.assertEqual(collector.records_view()[0]['fields']['pv_input_power']['value'], 1862)

    def test_catalog_kind_index_preserves_alias_and_pattern_precedence(self):
        for protocol, profile in PROFILES.items():
            commands = {'unknown'}
            for definition in profile['queries']:
                commands.update((definition['key'][:15], definition['example'][:15],
                                 definition['example']))
            for command in commands:
                resolved, matched = command, None
                for definition in profile['queries']:
                    if (definition['key'][:15] == command or
                            (definition.get('pattern') is not None and
                             command.startswith(definition['key'][:15]))):
                        resolved, matched = definition['key'], definition
                        break
                with self.subTest(protocol=protocol, command=command):
                    self.assertEqual(_kind({'protocol': protocol, 'command': command}),
                                     _command_kind(protocol, resolved, matched))

    def test_measurement_receipt_uses_selected_fields_not_message_order(self):
        record = self.collect().records()[0]
        later_sensor = dict(record, command='QPIGS2', sequence=2, timestamp_ms=2000,
                            received_at=101, fields={
                                'pv_input_power': {'unit': 'W', 'value': 0}})
        old_delayed = dict(record, command='QPIGSx', sequence=0, timestamp_ms=900, received_at=120)
        settings = dict(record, command='QPIRI', sequence=10, timestamp_ms=10000,
                        received_at=125)
        defaults = dict(settings, command='QDI', received_at=126)
        records = [settings, later_sensor, defaults, old_delayed, record]
        for selected in (records, list(reversed(records))):
            normalized = normalize_native_live(selected, 'PI30', 0, 126)
            self.assertEqual(native_measurement_received_at(selected, 'PI30', 0),
                             normalized['received_at'])
            self.assertEqual(normalized['received_at'], 101)
        # A newer text value for the same sensor makes that field ineligible;
        # an older numeric value must not keep its receipt alive.
        missing = dict(later_sensor, sequence=3, timestamp_ms=3000, received_at=130,
                       fields={'pv_input_power': {'unit': 'W', 'value': 'Unavailable'}})
        selected = [later_sensor, missing]
        self.assertIsNone(native_measurement_received_at(selected, 'PI30', 0))
        self.assertIsNone(normalize_native_live(selected, 'PI30', 0, 130)['received_at'])
        self.assertIsNone(native_measurement_received_at(records, 'PI30', 1))

    def test_missing_chunk_diagnostic_explains_no_live_numeric_record(self):
        collector = NativeTelemetryCollector()
        fields = list(pi30_fields().items())
        for index in range(14):
            collector.receive(chunk(index=index, count=15, fields=dict(fields[index*2:index*2+2])), index / 100)
        collector.expire(6)
        self.assertFalse(self.normalize(collector, 6)['telemetry_available'])
        self.assertEqual(collector.diagnostics()['drop_reasons'], {'incomplete_record_expired': 1})
        collector.reset(preserve_counters=True)
        self.assertEqual(collector.dropped_count, 1)
        self.assertEqual(collector.incomplete_count, 0)
        collector.reset()
        self.assertEqual(collector.diagnostics()['drop_reasons'], {})

    def test_early_cached_identity_after_live_is_same_boot_not_reboot(self):
        collector = self.collect(atomic(sequence=267, timestamp_ms=365902), 100)
        identity = atomic(command='QID', sequence=5, timestamp_ms=1000, sample_age_ms=364902,
                          fields={'serial_number': {'unit': '', 'value': '92832207100756'}})
        self.assertIsNotNone(collector.receive(identity, 100))
        self.assertEqual({record['command'] for record in collector.records()}, {'QPIGS', 'QID'})
        self.assertTrue(self.normalize(collector, 100)['telemetry_available'])
        self.assertEqual(collector.dropped_count, 0)
        # Periodic cached repeats, even after an idle gap, must not reset live.
        self.assertIsNone(collector.receive(dict(identity, sample_age_ms=370902), 106))
        self.assertEqual(len(collector.records()), 2)

    def test_aged_reboot_candidate_uses_delivery_time_for_plausibility(self):
        collector = self.collect(atomic(sequence=500, timestamp_ms=600000), 100)
        self.assertIsNone(collector.receive(atomic(sequence=1, timestamp_ms=1000,
            sample_age_ms=9000), 110))
        record = collector.receive(atomic(sequence=2, timestamp_ms=11000), 111)
        self.assertIsNotNone(record)
        self.assertEqual(collector.records()[0]['timestamp_ms'], 11000)
        self.assertEqual(collector.records()[0]['received_at'], 111)

    def test_profile_specific_identity_and_configuration_cannot_be_live_ratings(self):
        cases = [('PI30REVO', 'PQSE', 'settings'), ('PI17', 'PIRI', 'settings'),
                 ('PI17', 'BATS', 'settings'), ('JKPB', 'getAdditionalInfo', 'settings'),
                 ('JKPB', 'getBalancerData', 'identity'), ('JK02', 'getInfo', 'identity'),
                 ('JK02_32', 'getInfo', 'identity'), ('JK04', 'getInfo', 'identity'),
                 ('JKV11', 'getInfo', 'identity'), ('PI30', 'QET', 'energy')]
        for protocol, command, group in cases:
            with self.subTest(protocol=protocol, command=command):
                # Numeric rated parameters must not masquerade as sensors,
                # even when their field names and units resemble live data.
                record = dict(protocol=protocol, member_id=0, command=command[:15], sequence=1,
                              timestamp_ms=1000, received_at=100,
                              fields={'battery_voltage': {'unit': 'V', 'value': 48},
                                      'ac_output_active_power': {'unit': 'W', 'value': 5000}})
                live = normalize_native_live([record], protocol, 0, 100)
                self.assertFalse(live['telemetry_available'])
                self.assertEqual(live['power_summary'], {})
                self.assertEqual(set(live['groups'][group]['fields']),
                                 {'battery_voltage', 'ac_output_active_power'})


if __name__ == '__main__':
    unittest.main()
