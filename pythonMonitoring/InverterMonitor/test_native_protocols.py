"""Contract tests for real firmware command forms and telemetry envelopes."""
import copy
import math
import unittest

from native_protocols import (CATALOG_SOURCE, PROFILES, PROTOCOL_IDS,
                              NativeTelemetryCollector, classify_command)


def chunk(sequence=1, index=0, count=1, fields=None, *, command='QPIGS',
          protocol='PI30', timestamp=1000, member=0):
    return {'member_id': member, 'protocol': protocol, 'command': command,
            'sequence': sequence, 'timestamp_ms': timestamp, 'chunk_index': index,
            'chunk_count': count, 'final_chunk': index == count - 1,
            'fields': fields if fields is not None else {'battery_voltage': {'unit': 'V', 'value': 52.4}}}


class NativeCommandTests(unittest.TestCase):
    def test_all_profiles_and_every_catalog_example(self):
        self.assertEqual(len(PROTOCOL_IDS), 26)
        self.assertEqual(len(set(PROTOCOL_IDS)), 26)
        self.assertEqual(set(PROFILES), set(PROTOCOL_IDS))
        self.assertEqual(PROTOCOL_IDS[0], 'AOHAI_MODBUS_RTU')
        self.assertEqual(CATALOG_SOURCE['revision'], 'eafdd4328c77f516cf82430c5bcb6fa042c45d3a')
        self.assertIn('Permission is hereby granted', CATALOG_SOURCE['permission_notice'])
        total = 0
        for protocol, profile in PROFILES.items():
            for group in ('queries', 'setters'):
                for definition in profile[group]:
                    with self.subTest(protocol=protocol, key=definition['key']):
                        actual = classify_command(protocol, definition['example'])
                        self.assertEqual(actual['setter'], group == 'setters')
                        self.assertEqual(actual['readback'], definition['readback'])
                        if definition['readback']:
                            self.assertFalse(classify_command(protocol, definition['readback'])['setter'])
                        total += 1
            for command in profile['polls']:
                self.assertFalse(classify_command(protocol, command)['setter'])
        self.assertEqual(total, 704)

    def test_variant_coverage_and_allowlist(self):
        counts = {'PI16': (13, 0), 'PI17': (25, 27), 'PI18': (13, 11),
                  'PI18LVX': (20, 26), 'PI30': (19, 26), 'PI30MAX': (35, 32),
                  'PI30REVO': (7, 2), 'PI41': (21, 26)}
        for protocol, expected in counts.items():
            self.assertEqual((len(PROFILES[protocol]['queries']), len(PROFILES[protocol]['setters'])), expected)
        self.assertTrue(classify_command('PI30', 'POP02')['setter'])
        self.assertEqual(classify_command('PI30', 'POP02')['readback'], 'QPIRI')
        for protocol, command in [('PI30', 'POP09'), ('PI30', 'QPIGSx'),
                                  ('PI30', 'qpigs'), ('PI16', 'POP02'),
                                  ('JK02_32', 'setChargingOn'), ('DALY', 'setChargingOn')]:
            with self.assertRaises(ValueError):
                classify_command(protocol, command)

    def test_command_input_limits(self):
        for protocol, command in [('INVALID', 'QPIGS'), ('AOHAI_MODBUS_RTU', 'QPIGS'),
                                  (None, 'QPIGS'), ([], 'QPIGS'), ('PI30', None),
                                  ('PI30', ''), ('PI30', 'QPIGS\r'), ('PI30', 'QPIGS\0'),
                                  ('PI30', 'QPIé'), ('PI30', 'Q' * 80)]:
            with self.assertRaises(ValueError):
                classify_command(protocol, command)

    def test_date_and_feature_checks_match_firmware(self):
        for profile in ('PI17', 'PI18LVX', 'PI30MAX'):
            date = next(x for x in PROFILES[profile]['setters'] if x['key'] == 'DAT')
            good = date['example']
            self.assertTrue(classify_command(profile, good)['setter'])
            bad = 'DAT' + ('20260230000000' if len(good) == 17 else '260230000000')
            with self.assertRaises(ValueError):
                classify_command(profile, bad)
        for key in ('PE', 'PD'):
            item = next(x for x in PROFILES['PI30']['setters'] if x['key'] == key)
            self.assertTrue(classify_command('PI30', item['example'])['setter'])
            with self.assertRaises(ValueError):
                classify_command('PI30', key + item['flags'][0] * 2)
            with self.assertRaises(ValueError):
                classify_command('PI30', key + '#')

    def test_documented_bms_numeric_bounds_and_fixed_toggles(self):
        for profile, key, low, high in [('JK485', 'setCellCount', 2, 24),
                                       ('JK485', 'setBalanceTrigger', 2, 1000),
                                       ('JK485', 'setMaxBalanceCurrent', 30, 1000),
                                       ('VED', 'setBatteryCapacity', 1, 9999)]:
            for value in (str(low), str(high), str(low) + '.0', str(low) + '.'):
                self.assertTrue(classify_command(profile, key + value)['setter'])
            for value in (str(low - 1), str(high + 1), str(low) + '.5', '+' + str(low),
                          str(low) + 'e0', str(low) + 'x', ' ' + str(low), '.'):
                with self.assertRaises(ValueError):
                    classify_command(profile, key + value)
        for key in ('setChargingOn', 'setChargingOff', 'setDischargingOn', 'setDischargingOff'):
            self.assertTrue(classify_command('JK02', key)['setter'])


class NativeTelemetryTests(unittest.TestCase):
    def setUp(self):
        self.collector = NativeTelemetryCollector()

    def test_all_chunks_required_and_out_of_order_arrival(self):
        first = chunk(index=0, count=3, fields={'grid_voltage': {'unit': 'V', 'value': 231.2}})
        middle = chunk(index=1, count=3, fields={'battery_voltage': {'unit': 'V', 'value': 52.4}})
        final = chunk(index=2, count=3, fields={'load_power': {'unit': 'W', 'value': 450}})
        self.assertIsNone(self.collector.receive(final, 0))
        self.assertEqual(self.collector.records(), [])
        self.assertIsNone(self.collector.receive(first, .1))
        record = self.collector.receive(middle, .2)
        self.assertEqual(record['fields'], dict(first['fields'], **middle['fields'], **final['fields']))
        self.assertEqual(record['received_at'], .2)
        self.assertEqual(self.collector.incomplete_count, 0)
        self.assertEqual(len(self.collector.records()), 1)

    def test_identical_duplicates_do_not_complete_or_change_readings(self):
        first = chunk(index=0, count=2)
        self.assertIsNone(self.collector.receive(first, 0))
        self.assertIsNone(self.collector.receive(first, .1))
        final = chunk(index=1, count=2, fields={'load_power': {'unit': 'W', 'value': 900}})
        record = self.collector.receive(final, .2)
        self.assertEqual(record['fields']['load_power']['value'], 900)
        self.assertIsNone(self.collector.receive(first, .3))
        self.assertIsNone(self.collector.receive(final, .4))
        self.assertEqual(self.collector.records()[0]['received_at'], .2)

    def test_conflicting_duplicate_and_cross_chunk_fields_poison_record(self):
        for conflict in ('value', 'same_field'):
            with self.subTest(conflict=conflict):
                collector = NativeTelemetryCollector()
                collector.receive(chunk(index=0, count=2), 0)
                bad = chunk(index=0 if conflict == 'value' else 1, count=2,
                            fields={'battery_voltage': {'unit': 'V', 'value': 999}})
                self.assertIsNone(collector.receive(bad, .1))
                final = chunk(index=1, count=2, fields={'load_power': {'unit': 'W', 'value': 900}})
                self.assertIsNone(collector.receive(final, .2))
                self.assertEqual(collector.records(), [])
                self.assertGreaterEqual(collector.dropped_count, 1)

    def test_conflicting_metadata_poison_record(self):
        for change in ({'timestamp_ms': 1001}, {'chunk_count': 3, 'final_chunk': False}):
            collector = NativeTelemetryCollector()
            collector.receive(chunk(index=0, count=2), 0)
            final = chunk(index=1, count=2, fields={'load_power': {'unit': 'W', 'value': 900}})
            final.update(change)
            self.assertIsNone(collector.receive(final, .1))
            self.assertEqual(collector.incomplete_count, 0)
            self.assertEqual(collector.records(), [])

    def test_expiry_drops_partial_and_does_not_refresh_on_duplicates(self):
        collector = NativeTelemetryCollector(expiry_seconds=1)
        collector.receive(chunk(index=0, count=2), 0)
        collector.receive(chunk(index=0, count=2), .9)
        collector.expire(1)
        self.assertEqual(collector.incomplete_count, 0)
        self.assertEqual(collector.dropped_count, 1)
        self.assertIsNone(collector.receive(chunk(index=1, count=2), 1.1))

    def test_assembly_and_record_counts_are_bounded(self):
        collector = NativeTelemetryCollector(max_assemblies=2, max_records=2)
        for seq in (1, 2, 3):
            collector.receive(chunk(seq, index=0, count=2), seq / 10)
        self.assertEqual(collector.incomplete_count, 2)
        self.assertEqual(collector.dropped_count, 1)
        for seq, command in enumerate(('QPIGS', 'QMOD', 'QPIRI'), 10):
            collector.receive(chunk(seq, command=command, timestamp=seq * 1000), seq)
        self.assertEqual(len(collector.records()), 2)

    def test_complete_maximum_96_field_record(self):
        for index in reversed(range(48)):
            fields = {f'voltage_cell{index * 2 + j:02}': {'unit': 'V', 'value': 3.3} for j in (0, 1)}
            result = self.collector.receive(chunk(index=index, count=48, fields=fields,
                                                  protocol='JK02_32', command='getCellData'), (48-index) / 100)
        self.assertEqual(len(result['fields']), 96)

    def test_reordered_records_do_not_replace_newer_same_command(self):
        good = self.collector.receive(chunk(5, timestamp=5000), 0)
        self.assertIsNone(self.collector.receive(chunk(4, timestamp=4000), .1))
        self.assertIsNone(self.collector.receive(chunk(5, timestamp=5000), .2))
        self.assertEqual(self.collector.records()[0], good)
        # A different query finishing late remains a useful independent record.
        other = self.collector.receive(chunk(3, command='QMOD', timestamp=3000), .3)
        self.assertIsNotNone(other)

    def test_sequence_wrap_with_increasing_uptime(self):
        self.collector.receive(chunk(0xffffffff, timestamp=10000), 0)
        record = self.collector.receive(chunk(0, timestamp=11000), 1)
        self.assertEqual(record['sequence'], 0)
        self.assertIsNone(self.collector.receive(chunk(0xfffffffe, timestamp=9000), 1.1))

    def test_reboot_requires_two_plausible_records_and_clears_prior_epoch(self):
        self.collector.receive(chunk(500, timestamp=300000), 0)
        self.collector.receive(chunk(501, command='QPIRI', timestamp=301000), 1)
        self.assertIsNone(self.collector.receive(chunk(1, timestamp=1000), 7))
        record = self.collector.receive(chunk(2, timestamp=2000, command='QMOD'), 8)
        self.assertEqual(record['sequence'], 2)
        self.assertEqual({x['command'] for x in self.collector.records()}, {'QPIGS', 'QMOD'})
        self.assertTrue(all(x['timestamp_ms'] <= 2000 for x in self.collector.records()))

    def test_single_old_packet_cannot_reset_epoch(self):
        self.collector.receive(chunk(500, timestamp=300000), 0)
        self.assertIsNone(self.collector.receive(chunk(1, timestamp=1000), 6))
        self.collector.receive(chunk(501, timestamp=301000), 7)
        self.assertEqual(self.collector.records()[0]['sequence'], 501)

    def test_reboot_with_higher_low_sequence_after_lost_early_records(self):
        # Early startup records can be lost, so a restarted node's first seen
        # counter may exceed the last counter observed before its restart.
        self.collector.receive(chunk(10, timestamp=600000), 1)
        self.assertIsNone(self.collector.receive(chunk(11, timestamp=10000), 61))
        record = self.collector.receive(chunk(12, timestamp=11000), 62)
        self.assertEqual(record['sequence'], 12)
        self.assertEqual(record['timestamp_ms'], 11000)
        self.assertEqual(len(self.collector.records()), 1)
        self.assertEqual(self.collector.records()[0]['received_at'], 62)

    def test_peer_protocol_and_member_isolation_even_for_partial_records(self):
        self.collector.receive(chunk(index=0, count=2), 0)
        self.assertIsNone(self.collector.receive(chunk(member=1), .1))
        self.assertIsNone(self.collector.receive(chunk(protocol='PI30MAX'), .2))
        self.collector.reset()
        self.assertIsNotNone(self.collector.receive(chunk(member=1), 0))

    def test_long_bms_query_telemetry_uses_firmware_15_byte_alias(self):
        for protocol, full in [('JKPB', 'getAdditionalInfo'), ('DALY', 'cellMinMaxVoltages'),
                               ('DALY40', 'cellTemperatures'), ('DALY', 'cellBalanceState'),
                               ('DALY', 'batteryFailureStatus')]:
            collector = NativeTelemetryCollector()
            record = collector.receive(chunk(protocol=protocol, command=full[:15]), 0)
            self.assertIsNotNone(record, full)
            self.assertEqual(record['command'], full[:15])
            self.assertFalse(classify_command(protocol, full)['setter'])

    def test_invalid_envelopes_and_nonfinite_values_never_publish(self):
        mutations = [{'protocol': 'UNKNOWN'}, {'protocol': 'AOHAI_MODBUS_RTU'},
                     {'member_id': True}, {'member_id': 10}, {'member_id': -1},
                     {'sequence': True}, {'sequence': -1}, {'sequence': 0x100000000},
                     {'timestamp_ms': -1}, {'timestamp_ms': 1.5}, {'timestamp_ms': True},
                     {'chunk_count': 0}, {'chunk_count': 49}, {'chunk_count': True},
                     {'chunk_index': 1}, {'chunk_index': True}, {'final_chunk': 1},
                     {'final_chunk': False}, {'command': 'QPIGS\0'}, {'command': 'X'*16},
                     {'command': 'POP02'}, {'fields': {}}, {'fields': []},
                     {'fields': {str(i): {'unit': '', 'value': 1} for i in range(3)}},
                     {'fields': {'invalid-key': {'unit': 'V', 'value': 1}}},
                     {'fields': {'x'*40: {'unit': '', 'value': 1}}},
                     {'fields': {'x': {'unit': '12345678', 'value': 1}}},
                     {'fields': {'x': {'unit': '', 'value': 'a'*32}}},
                     {'fields': {'x': {'unit': '', 'value': 'é'*16}}},
                     {'fields': {'x': {'unit': '', 'value': '\n'}}},
                     {'fields': {'x': {'unit': '', 'value': True}}},
                     {'fields': {'x': {'unit': '', 'value': None}}},
                     {'fields': {'x': {'unit': '', 'value': float('nan')}}},
                     {'fields': {'x': {'unit': '', 'value': float('inf')}}},
                     {'fields': {'x': {'unit': '', 'value': 10**400}}},
                     {'fields': {'x': {'unit': '', 'value': 1, 'extra': 5}}}]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                collector = NativeTelemetryCollector()
                data = chunk()
                data.update(mutation)
                self.assertIsNone(collector.receive(data, 0))
                self.assertEqual(collector.records(), [])
                self.assertEqual(collector.dropped_count, 1)

    def test_snapshots_and_inputs_are_copied(self):
        data = chunk()
        result = self.collector.receive(data, 0)
        data['fields']['battery_voltage']['value'] = 900
        result['fields']['battery_voltage']['value'] = 800
        snapshot = self.collector.records()
        self.assertEqual(snapshot[0]['fields']['battery_voltage']['value'], 52.4)
        snapshot[0]['fields'].clear()
        self.assertTrue(self.collector.records()[0]['fields'])

    def test_invalid_limits_and_monotonic_time(self):
        for options in ({'max_assemblies': 0}, {'max_records': True}, {'expiry_seconds': math.inf}):
            with self.assertRaises(ValueError):
                NativeTelemetryCollector(**options)
        self.collector.expire(1)
        with self.assertRaises(ValueError):
            self.collector.expire(.5)
        with self.assertRaises(ValueError):
            self.collector.expire(math.nan)


if __name__ == '__main__':
    unittest.main()
