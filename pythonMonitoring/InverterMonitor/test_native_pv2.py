"""PI30 tracker 2 is a separate observation; no MQTT or device connection."""
import unittest
from unittest.mock import patch

from mqtt_backend import MonitorSession
from native_live import normalize_native_live
from test_native_all_profiles import observation


def trackers(pv1=856, pv2=1049, second_received_at=100):
    return [observation('PI30', 'QPIGS', [
        ('pv_input_voltage', 'V', 310), ('pv_input_power', 'W', pv1)], 1),
        observation('PI30', 'QPIGS2', [
            ('pv2_input_current', 'A', 3.1), ('pv2_input_voltage', 'V', 327.3),
            ('pv2_charging_power', 'W', pv2)], 2, second_received_at)]


def wire(record, **extra):
    data = {key: value for key, value in record.items() if key != 'received_at'}
    data.update(record_complete=True, sample_age_ms=0, **extra)
    return data


class Pi30SecondTrackerTests(unittest.TestCase):
    def test_dual_tracker_total_infers_second_tracker_from_report(self):
        live = normalize_native_live(trackers(), 'PI30', 0, 100)
        self.assertEqual(live['power_summary']['pv_kw'], 1.905)
        self.assertTrue(live['power_quality']['pv_kw']['fresh'])
        self.assertEqual(live['power_quality']['pv_kw']['source_fields'],
                         ['pv_input_power', 'pv2_charging_power'])
        self.assertEqual(live['groups']['pv']['fields']['pv2_input_voltage']['value'], 327.3)
        self.assertEqual(live['groups']['pv']['fields']['pv2_input_current']['unit'], 'A')
        self.assertTrue(live['pv_total_requires_channels'])

    def test_selected_dual_tracker_waits_for_missing_second_power(self):
        for records in (trackers()[:1], trackers()):
            if len(records) > 1:
                del records[1]['fields']['pv2_charging_power']
            with self.subTest(records=len(records)):
                live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=2)
                self.assertNotIn('pv_kw', live['power_summary'])
                self.assertTrue(live['pv_total_requires_channels'])
                self.assertEqual(live['groups']['pv']['fields']['pv_input_power']['value'], 856)

    def test_second_tracker_voltage_alone_does_not_make_pv1_a_total(self):
        records = trackers()
        del records[1]['fields']['pv2_charging_power']
        live = normalize_native_live(records, 'PI30', 0, 100)
        self.assertNotIn('pv_kw', live['power_summary'])

    def test_real_zero_survives_for_either_or_both_trackers(self):
        for pv1, pv2 in ((0, 1049), (856, 0), (0, 0)):
            with self.subTest(pv1=pv1, pv2=pv2):
                live = normalize_native_live(trackers(pv1, pv2), 'PI30', 0, 100,
                                             connected_pv_inputs=2)
                self.assertEqual(live['power_summary']['pv_kw'], (pv1 + pv2) / 1000)
                self.assertTrue(live['power_quality']['pv_kw']['fresh'])

    def test_three_trackers_need_all_three_or_an_explicit_reported_total(self):
        records = trackers()
        live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=3)
        self.assertNotIn('pv_kw', live['power_summary'])
        records[1]['fields']['pv3_charging_power'] = dict(unit='W', value=300)
        for count in (None, 3):
            live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=count)
            self.assertEqual(live['power_summary']['pv_kw'], 2.205)
        records[1]['fields']['pv_total_charging_power'] = dict(unit='W', value=2200)
        live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=3)
        self.assertEqual(live['power_summary']['pv_kw'], 2.2)
        self.assertEqual(live['power_quality']['pv_kw']['source_field'], 'pv_total_charging_power')

    def test_unsupported_extra_tracker_cannot_turn_partial_channels_into_a_total(self):
        records = trackers()
        records[1]['fields']['pv3_charging_power'] = dict(unit='W', value=300)
        live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=4)
        self.assertNotIn('pv_kw', live['power_summary'])

    def test_reported_whole_device_total_does_not_require_a_pv1_observation(self):
        record = trackers()[1]
        record['fields'].update(pv3_charging_power=dict(unit='W', value=620),
                                pv_total_charging_power=dict(unit='W', value=2525))
        live = normalize_native_live([record], 'PI30', 0, 100, connected_pv_inputs=3)
        self.assertEqual(live['power_summary']['pv_kw'], 2.525)

    def test_unknown_tracker_count_uses_reported_total_without_individual_trackers(self):
        record = observation('PI30', 'QPIGS2', [('pv_total_charging_power', 'W', 2525)])
        live = normalize_native_live([record], 'PI30', 0, 100)
        self.assertEqual(live['power_summary']['pv_kw'], 2.525)
        self.assertEqual(live['power_quality']['pv_kw']['source_field'], 'pv_total_charging_power')
        self.assertTrue(live['power_quality']['pv_kw']['fresh'])

    def test_explicit_single_tracker_uses_pv1_even_if_extra_diagnostics_are_present(self):
        records = trackers()
        records[1]['fields']['pv_total_charging_power'] = dict(unit='W', value=2525)
        live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=1)
        self.assertEqual(live['power_summary']['pv_kw'], .856)
        self.assertFalse(live['pv_total_requires_channels'])
        for count in (0, 4):
            live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=count)
            self.assertNotIn('pv_kw', live['power_summary'])
            self.assertTrue(live['pv_total_requires_channels'])

    def test_old_second_tracker_keeps_total_stale_while_pv1_remains_fresh(self):
        live = normalize_native_live(trackers(second_received_at=80), 'PI30', 0, 100,
                                     connected_pv_inputs=2)
        self.assertEqual(live['power_summary']['pv_kw'], 1.905)
        self.assertEqual(live['power_quality']['pv_kw']['age_seconds'], 20)
        self.assertFalse(live['power_quality']['pv_kw']['fresh'])
        self.assertTrue(live['power_quality']['pv_kw']['stale'])
        self.assertTrue(live['groups']['pv']['fields']['pv_input_power']['fresh'])

    def test_single_tracker_and_older_firmware_keep_existing_first_tracker_total(self):
        for count in (None, 1):
            with self.subTest(count=count):
                live = normalize_native_live(trackers()[:1], 'PI30', 0, 100,
                                             connected_pv_inputs=count)
                self.assertEqual(live['power_summary']['pv_kw'], .856)
                self.assertFalse(live['pv_total_requires_channels'])

    def test_invalid_units_and_non_numeric_power_do_not_make_a_total(self):
        for unit, value in (('VA', 1049), ('W', '1049'), ('W', None)):
            records = trackers()
            records[1]['fields']['pv2_charging_power'] = dict(unit=unit, value=value)
            live = normalize_native_live(records, 'PI30', 0, 100, connected_pv_inputs=2)
            self.assertNotIn('pv_kw', live['power_summary'])

    def test_invalid_configured_count_is_rejected(self):
        for count in (-1, 17, True, 2.0, '2'):
            with self.subTest(count=count), self.assertRaises(ValueError):
                normalize_native_live(trackers(), 'PI30', 0, 100, connected_pv_inputs=count)


class Pi30TrackerMqttTests(unittest.TestCase):
    def setUp(self):
        self.session = MonitorSession()
        self.session.select_protocol('PI30')
        self.session.base = 'inverter/INV-TEST/inverter/0'

    def test_atomic_and_live_snapshots_make_the_same_complete_total(self):
        results = []
        for live_only in (False, True):
            self.setUp()
            with patch('mqtt_backend.time.monotonic', return_value=100):
                records = [wire(record, connected_pv_inputs=2) for record in trackers()]
                if live_only:
                    self.session.receive(self.session.base + '/live', dict(protocol='PI30', member_id=0,
                        connected_pv_inputs=2, native_records=records))
                else:
                    for record in records:
                        self.session.receive(self.session.base + '/protocol/' + record['command'], record)
                results.append(self.session.snapshot()['native_live'])
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0]['power_summary']['pv_kw'], 1.905)
        self.assertEqual(results[0]['connected_pv_inputs'], 2)

    def test_selected_dual_tracker_metadata_blocks_partial_total_for_both_transports(self):
        for live_only in (False, True):
            self.setUp()
            with self.subTest(live_only=live_only), patch('mqtt_backend.time.monotonic', return_value=100):
                record = wire(trackers()[0], connected_pv_inputs=2)
                if live_only:
                    self.session.receive(self.session.base + '/live', dict(protocol='PI30', member_id=0,
                        connected_pv_inputs=2, native_records=[record]))
                else:
                    self.session.receive(self.session.base + '/protocol/QPIGS', record)
                live = self.session.snapshot()['native_live']
                self.assertNotIn('pv_kw', live['power_summary'])
                self.assertEqual(live['connected_pv_inputs'], 2)

    def test_retained_wrong_member_and_wrong_protocol_cannot_change_tracker_count(self):
        with patch('mqtt_backend.time.monotonic', return_value=100):
            record = wire(trackers()[0], connected_pv_inputs=2)
            for changes, retained in (({}, True), ({'member_id': 1}, False), ({'protocol': 'PI30MAX'}, False)):
                self.session.receive(self.session.base + '/protocol/QPIGS', dict(record, **changes), retained)
                self.assertIsNone(self.session.native_connected_pv_inputs)

    def test_connection_epoch_resets_configured_count(self):
        with patch('mqtt_backend.time.monotonic', return_value=100):
            self.session.receive(self.session.base + '/protocol/QPIGS',
                                 wire(trackers()[0], connected_pv_inputs=2))
            self.assertEqual(self.session.native_connected_pv_inputs, 2)
            self.session.close()
            self.assertIsNone(self.session.native_connected_pv_inputs)
            self.assertFalse(self.session.snapshot()['native_records'])


if __name__ == '__main__':
    unittest.main()
