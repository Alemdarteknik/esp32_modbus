"""Withdrawn Tk tests; every MQTT session and network worker is mocked."""
import time
import json
import tkinter as tk
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

from inverter_monitor import AOHAI_PROTOCOL, InverterMonitor, native_result_text
from native_protocols import PROTOCOL_IDS
from mqtt_backend import MonitorSession
from native_live import normalize_native_live


class NativeTkTests(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.monitor = InverterMonitor(self.root)

    def tearDown(self):
        self.monitor.running = False
        self.root.destroy()

    def select_native(self, protocol="PI30"):
        self.monitor.protocol_id.set(protocol)
        self.monitor.select_protocol()

    def ready(self, **extra):
        snapshot = dict(active=True, ready=True, status="Connected", notice="",
                        protocol="PI30", member_id=0, native_records=[],
                        native_pending=None, native_result=None, native_incomplete=0,
                        native_drops=0, native_write_blocked=False,
                        health={}, health_at=None, connected_at=time.monotonic())
        snapshot.update(extra)
        for record in snapshot['native_records']:
            record.setdefault('member_id', snapshot['member_id'])
        snapshot['native_live'] = normalize_native_live(snapshot['native_records'], snapshot['protocol'],
            snapshot['member_id'], time.monotonic(),
            connected_pv_inputs=snapshot.get('native_connected_pv_inputs'))
        self.monitor.native_snapshot = snapshot
        self.monitor.native_session = Mock()
        self.monitor.native_session.snapshot.return_value = snapshot
        self.monitor.native_session.send_native.return_value = "101"
        return snapshot

    def test_default_aohai_and_all_protocol_and_member_selectors(self):
        monitor = self.monitor
        self.assertEqual(monitor.protocol_id.get(), AOHAI_PROTOCOL)
        self.assertEqual(tuple(monitor.protocol_selector.cget("values")), PROTOCOL_IDS)
        self.assertEqual(tuple(monitor.member_selector.cget("values")), tuple(map(str, range(10))))
        for frame in monitor.native_frames:
            self.assertEqual(monitor.notebook.tab(frame, "state"), "hidden")
        self.assertEqual(monitor.notebook.tab(monitor.settings_outer, "state"), "normal")

    def test_native_hides_all_legacy_settings_and_does_not_poll(self):
        self.select_native()
        monitor = self.monitor
        for frame in monitor.aohai_frames:
            self.assertEqual(monitor.notebook.tab(frame, "state"), "hidden")
        for setting in monitor.setting_controls:
            setting.request = Mock()
        monitor.data_queue.put(("settings_ready", True))
        monitor.process_queue()
        for setting in monitor.setting_controls:
            setting.request.assert_not_called()
            self.assertFalse(setting.ready)

    def test_native_connect_uses_shared_session_only_and_selected_member(self):
        self.select_native()
        monitor = self.monitor
        monitor.inverter_id.set("INV-AABBCCDDEEFF")
        monitor.member_id.set("9")
        with patch("inverter_monitor.MonitorSession") as session, patch("inverter_monitor.threading.Thread") as thread:
            monitor.connect()
            session.return_value.connect.assert_called_once_with("AABBCCDDEEFF", protocol="PI30", member_id=9)
            thread.assert_not_called()
        self.assertTrue(monitor.connection_active)
        self.assertEqual(str(monitor.protocol_selector.cget("state")), "disabled")
        self.assertEqual(str(monitor.member_selector.cget("state")), "disabled")

    def test_aohai_connect_retains_original_worker_and_default_topic(self):
        monitor = self.monitor
        monitor.inverter_id.set("AABBCCDDEEFF")
        with patch("inverter_monitor.MonitorSession") as session, patch("inverter_monitor.threading.Thread") as thread:
            monitor.connect()
            thread.assert_called_once()
            thread.return_value.start.assert_called_once()
            session.assert_not_called()
        self.assertEqual(monitor.mqtt_topic, "inverter/INV-AABBCCDDEEFF/inverter/0/live")

    def test_aohai_slave_subscribes_gateway_health_at_member_zero(self):
        monitor = self.monitor
        monitor.inverter_id.set("AABBCCDDEEFF")
        monitor.member_id.set("9")
        with patch("inverter_monitor.threading.Thread"):
            monitor.connect()
        client = Mock()
        monitor.on_connect(client, None, None, 0)
        subscriptions = dict(client.subscribe.call_args.args[0])
        self.assertIn("inverter/INV-AABBCCDDEEFF/inverter/9/live", subscriptions)
        self.assertIn("inverter/INV-AABBCCDDEEFF/inverter/0/health", subscriptions)
        self.assertNotIn("inverter/INV-AABBCCDDEEFF/inverter/9/health", subscriptions)
        while not monitor.data_queue.empty():
            monitor.data_queue.get_nowait()
        monitor.on_message(client, None, SimpleNamespace(
            topic="inverter/INV-AABBCCDDEEFF/inverter/0/health",
            payload=b'{"telemetry_seen":true}', retain=False))
        self.assertEqual(monitor.data_queue.get_nowait()[0], "health")

    def test_setter_requires_confirmation_and_edit_revokes_it(self):
        self.select_native()
        self.ready()
        monitor = self.monitor
        monitor.native_command.set("POP02")
        self.assertEqual(str(monitor.native_send_button.cget("state")), "disabled")
        monitor.native_confirmed.set(True)
        monitor.update_native_controls()
        self.assertEqual(str(monitor.native_send_button.cget("state")), "normal")
        monitor.send_native_command()
        monitor.native_session.send_native.assert_called_once_with("POP02", confirmed=True)
        self.assertFalse(monitor.native_confirmed.get())
        monitor.native_command.set("POP01")
        self.assertEqual(str(monitor.native_send_button.cget("state")), "disabled")

    def test_uncertain_write_block_leaves_reads_available(self):
        self.select_native()
        self.ready(native_write_blocked=True)
        monitor = self.monitor
        monitor.native_command.set("POP02")
        monitor.native_confirmed.set(True)
        monitor.update_native_controls()
        self.assertEqual(str(monitor.native_send_button.cget("state")), "disabled")
        self.assertIn("uncertain", monitor.native_command_hint.cget("text"))
        monitor.native_command.set("QPIGS")
        self.assertEqual(str(monitor.native_send_button.cget("state")), "normal")
        monitor.send_native_command()
        monitor.native_session.send_native.assert_called_once_with("QPIGS", confirmed=False)

    def test_invalid_and_pending_commands_are_disabled(self):
        self.select_native()
        snapshot = self.ready()
        monitor = self.monitor
        monitor.native_command.set("POP999")
        self.assertEqual(str(monitor.native_send_button.cget("state")), "disabled")
        monitor.native_command.set("QPIGS")
        self.assertEqual(str(monitor.native_send_button.cget("state")), "normal")
        snapshot["native_pending"] = {"command_id": "101", "command": "QPIGS"}
        monitor.update_native_controls()
        self.assertEqual(str(monitor.native_send_button.cget("state")), "disabled")

    def test_native_records_keep_units_and_complete_command_identity(self):
        self.select_native()
        self.ready(native_records=[dict(protocol="PI30", command="QPIGS", sequence=5,
            timestamp_ms=123, received_at=time.monotonic(), fields={
                "battery_voltage": {"value": 52.4, "unit": "V"},
                "output_active_power": {"value": 1500, "unit": "W"}})],
            native_result={"command_id": "101", "command": "POP02", "outcome": "acknowledged"},
            native_incomplete=1, native_drops=2)
        monitor = self.monitor
        monitor.notebook.select(monitor.native_frames[2])
        monitor.refresh_native()
        rows = [monitor.native_tree.item(row, "values") for row in monitor.native_tree.get_children()]
        self.assertEqual(rows[0][:5], ("battery_voltage", "52.4", "V", "QPIGS", "PI30"))
        self.assertEqual(rows[1][1:3], ("1500", "W"))
        self.assertIn("Accepted by device", monitor.native_result_label.cget("text"))
        self.assertIn("Incomplete records: 1", monitor.native_delivery_label.cget("text"))
        self.assertIn("Dropped: 2", monitor.native_delivery_label.cget("text"))
        self.assertFalse(monitor.trees["Power Summary"].get_children())

    def test_each_command_keeps_its_own_freshness(self):
        self.select_native()
        now = time.monotonic()
        self.ready(native_records=[dict(protocol="PI30", command="QPIGS", sequence=1,
            timestamp_ms=1, received_at=now - 60,
            fields={"battery_voltage": {"value": 52.123456, "unit": "V"}}),
            dict(protocol="PI30", command="QID", sequence=2, timestamp_ms=2, received_at=now,
            fields={"serial_number": {"value": "123456", "unit": ""}})])
        self.monitor.notebook.select(self.monitor.native_frames[2])
        self.monitor.refresh_native()
        rows = [self.monitor.native_tree.item(row, "values") for row in self.monitor.native_tree.get_children()]
        self.assertEqual(rows[0][1], "52.123456")
        self.assertTrue(rows[0][6].startswith("STALE"))
        self.assertEqual(rows[1][6], "Fresh")

    def test_reported_protocol_mismatch_disables_commands(self):
        self.select_native()
        self.ready(protocol_mismatch="PI18")
        self.monitor.native_command.set("QPIGS")
        self.assertEqual(str(self.monitor.native_send_button.cget("state")), "disabled")
        self.assertIn("Device reports PI18", self.monitor.native_command_hint.cget("text"))

    def test_real_shared_backend_assembles_chunks_then_routes_setting_result(self):
        self.select_native()
        monitor = self.monitor
        session = MonitorSession()
        session.select_protocol("PI30")
        session.base = "inverter/INV-AABBCCDDEEFF/inverter/2"
        session.member_id = 2
        session.active = session.ready = True
        session.status = "Connected"
        session.connected_at = time.monotonic()
        session.client = Mock()
        session.client.publish.return_value.rc = 0
        monitor.native_session = session
        monitor.notebook.select(monitor.native_frames[2])
        chunk = dict(member_id=2, protocol="PI30", command="QPIGS", sequence=10,
                     timestamp_ms=5000, chunk_count=2, chunk_index=0, final_chunk=False,
                     fields={"battery_voltage": {"value": 52.4, "unit": "V"},
                             "output_voltage": {"value": 230, "unit": "V"}})
        session.receive(session.base + "/protocol/QPIGS", chunk)
        monitor.refresh_native()
        self.assertFalse(monitor.native_tree.get_children())
        self.assertIn("Incomplete records: 1", monitor.native_delivery_label.cget("text"))
        session.receive(session.base + "/protocol/QPIGS", dict(chunk, chunk_index=1,
            final_chunk=True, fields={"output_active_power": {"value": 1500, "unit": "W"}}))
        monitor.refresh_native()
        self.assertEqual(len(monitor.native_tree.get_children()), 3)
        monitor.native_command.set("POP02")
        monitor.native_confirmed.set(True)
        monitor.send_native_command()
        pending = session.native_pending
        self.assertTrue(pending["write"])
        session.receive(session.base + "/result", dict(member_id=2,
            command_id=pending["command_id"], command="POP02", protocol="PI30",
            operation="protocol", outcome="acknowledged", write_sent=True, detail="ACK"))
        monitor.refresh_native()
        self.assertIn("Accepted by device", monitor.native_result_label.cget("text"))
        self.assertFalse(session.jobs)
        self.assertEqual(session.client.publish.call_count, 1)

    def test_native_disconnect_and_finished_unlock_selectors(self):
        self.select_native()
        snapshot = self.ready()
        monitor = self.monitor
        monitor.connection_active = True
        monitor.disconnect()
        monitor.native_session.close.assert_called_once()
        self.assertTrue(monitor.disconnecting)
        snapshot.update(active=False, ready=False, status="Disconnected")
        monitor.refresh_native()
        self.assertFalse(monitor.connection_active)
        self.assertEqual(str(monitor.protocol_selector.cget("state")), "readonly")
        self.assertEqual(str(monitor.member_selector.cget("state")), "readonly")

    def test_all_native_profiles_have_a_readable_catalog(self):
        for protocol in PROTOCOL_IDS:
            if protocol == AOHAI_PROTOCOL:
                continue
            self.select_native(protocol)
            self.assertTrue(self.monitor.native_template_entries, protocol)
            self.assertEqual(str(self.monitor.native_send_button.cget("state")), "disabled")

    def test_changing_disconnected_profile_detaches_old_session_and_results(self):
        self.select_native()
        self.ready(native_result={"command": "POP02", "outcome": "uncertain"})
        self.monitor.refresh_native()
        self.assertIn("uncertain", self.monitor.native_result_label.cget("text"))
        self.select_native("JK02")
        self.assertIsNone(self.monitor.native_session)
        self.assertEqual(self.monitor.native_result_label.cget("text"), "No command result yet.")
        self.assertFalse(self.monitor.native_tree.get_children())

    def test_result_text_does_not_call_ack_verified(self):
        self.assertIn("check the readback", native_result_text({"outcome": "acknowledged"}))
        self.assertIn("verify the device", native_result_text({"outcome": "uncertain"}))
        self.assertIn("Read completed", native_result_text({"outcome": "completed"}))

    def test_live_groups_are_first_and_manual_commands_are_separate(self):
        self.select_native()
        monitor = self.monitor
        tabs = [monitor.notebook.tab(frame, 'text') for frame in monitor.notebook.tabs()
                if monitor.notebook.tab(frame, 'state') != 'hidden']
        self.assertEqual(tabs[0], 'Live data')
        self.assertIn('Manual settings', tabs)
        self.assertIn('Advanced diagnostics', tabs)
        self.assertEqual(monitor.notebook.select(), str(monitor.native_frames[0]))
        self.assertEqual(tuple(monitor.native_metric_labels), ('pv', 'grid', 'battery', 'output', 'load'))
        self.assertNotEqual(monitor.native_tree.master, monitor.native_frames[0])
        self.assertEqual(monitor.native_send_button.master, monitor.native_frames[1])

    def test_live_missing_readings_standby_and_identity_are_not_power(self):
        self.select_native()
        now = time.monotonic()
        self.ready(native_records=[dict(protocol='PI30', command='QMOD', sequence=1,
            timestamp_ms=1, received_at=now,
            fields={'device_mode': {'value': 'Standby', 'unit': ''}}),
            dict(protocol='PI30', command='QPI', sequence=2, timestamp_ms=2, received_at=now,
                 fields={'protocol_id': {'value': 'PI30', 'unit': ''}})])
        monitor = self.monitor
        monitor.refresh_native()
        for key, label in monitor.native_metric_labels.items():
            self.assertEqual(label.cget('text'), 'Active: Unavailable\nApparent: Unavailable'
                             if key in ('grid', 'output') else 'Unavailable')
        self.assertIn('Waiting for fresh electrical', monitor.native_live_notice.cget('text'))
        self.assertIn('Standby', monitor.native_mode_label.cget('text'))
        self.assertIn('Fresh', monitor.native_mode_label.cget('text'))
        self.assertNotIn('Standby', monitor.connection_label.cget('text'))
        self.assertIn('Waiting', monitor.freshness_label.cget('text'))
        monitor.native_session.send_native.assert_not_called()

    def test_live_real_zero_and_stale_readings_are_distinct(self):
        self.select_native()
        now = time.monotonic()
        self.ready(native_records=[dict(protocol='PI30', command='QPIGS', sequence=1,
            timestamp_ms=1, received_at=now,
            fields={'pv_power': {'value': 0, 'unit': 'W'},
                    'battery_voltage': {'value': 0, 'unit': 'V'}}),
            dict(protocol='PI30', command='QPIGS2', sequence=2, timestamp_ms=2, received_at=now - 60,
                 fields={'grid_voltage': {'value': 230, 'unit': 'V'}})])
        monitor = self.monitor
        monitor.refresh_native()
        self.assertEqual(monitor.native_metric_labels['pv'].cget('text'), '0.0 kW')
        self.assertEqual(monitor.native_metric_labels['battery'].cget('text'), 'Unavailable')
        self.assertEqual(monitor.native_metric_labels['grid'].cget('text'), 'Active: Unavailable\nApparent: Unavailable')
        self.assertEqual(monitor.native_metric_labels['output'].cget('text'), 'Active: Unavailable\nApparent: Unavailable')
        grid = monitor.native_group_trees['grid']
        row = grid.item(grid.get_children()[0], 'values')
        self.assertEqual(row[1:3], ('230', 'V'))
        self.assertTrue(row[3].startswith('Stale'))
        monitor.native_session.send_native.assert_not_called()

    def test_pi30_second_tracker_is_visible_and_a_missing_tracker_is_not_a_total(self):
        from test_native_pv2 import trackers
        self.select_native()
        records = trackers()
        now = time.monotonic()
        for record in records:
            record['received_at'] = now
        self.ready(native_records=records[:1], native_connected_pv_inputs=2)
        self.monitor.refresh_native()
        self.assertEqual(self.monitor.native_metric_labels['pv'].cget('text'), 'Unavailable')
        self.ready(native_records=records, native_connected_pv_inputs=2)
        self.monitor.refresh_native()
        self.assertEqual(self.monitor.native_metric_labels['pv'].cget('text'), '1.905 kW')
        tree = self.monitor.native_group_trees['pv']
        values = [tree.item(child, 'values') for child in tree.get_children()]
        self.assertTrue(any(row[:3] == ('Pv2 input voltage', '327.3', 'V') for row in values))
        self.assertTrue(any(row[:3] == ('Pv2 input current', '3.1', 'A') for row in values))
        self.assertTrue(any(row[:3] == ('Pv2 charging power', '1049', 'W') for row in values))
        records[1]['received_at'] = now - 60
        self.ready(native_records=records, native_connected_pv_inputs=2)
        self.monitor.refresh_native()
        self.assertEqual(self.monitor.native_metric_labels['pv'].cget('text'), 'Stale')
        self.monitor.native_session.send_native.assert_not_called()

    def test_power_cards_show_active_apparent_and_signed_battery(self):
        self.select_native()
        record = dict(protocol='PI30', command='QPIGS', sequence=1,
            timestamp_ms=1, received_at=time.monotonic(), fields={
                'grid_active_power': {'value': 900, 'unit': 'W'},
                'grid_apparent_power': {'value': 1000, 'unit': 'VA'},
                'ac_output_active_power': {'value': 1200, 'unit': 'W'},
                'ac_output_apparent_power': {'value': 1500, 'unit': 'VA'},
                'battery_voltage': {'value': 50, 'unit': 'V'},
                'battery_discharge_current': {'value': 4, 'unit': 'A'},
                'battery_charging_current': {'value': 2, 'unit': 'A'}})
        self.ready(native_records=[record])
        monitor = self.monitor
        monitor.refresh_native()
        self.assertEqual(monitor.native_metric_labels['grid'].cget('text'), 'Active: 0.9 kW\nApparent: 1.0 kVA')
        self.assertEqual(monitor.native_metric_labels['output'].cget('text'), 'Active: 1.2 kW\nApparent: 1.5 kVA')
        self.assertEqual(monitor.native_metric_frames['output'].cget('text'), 'Load power')
        self.assertEqual(monitor.native_metric_labels['battery'].cget('text'), '+0.1 kW')
        self.assertIn('+ discharge / − charge', monitor.native_power_caption.cget('text'))
        record['fields']['battery_charging_current']['value'] = 6
        self.ready(native_records=[record])
        monitor.refresh_native()
        self.assertEqual(monitor.native_metric_labels['battery'].cget('text'), '-0.1 kW')
        record['fields']['battery_charging_current']['value'] = 4
        self.ready(native_records=[record])
        monitor.refresh_native()
        self.assertEqual(monitor.native_metric_labels['battery'].cget('text'), '+0.0 kW')
        record['received_at'] = time.monotonic() - 60
        self.ready(native_records=[record])
        monitor.refresh_native()
        self.assertEqual(monitor.native_metric_labels['grid'].cget('text'), 'Active: Stale\nApparent: Stale')
        self.assertEqual(monitor.native_metric_labels['output'].cget('text'), 'Active: Stale\nApparent: Stale')
        self.assertEqual(monitor.native_metric_labels['battery'].cget('text'), 'Stale')
        monitor.native_session.send_native.assert_not_called()

    def test_unsigned_battery_power_is_not_a_signed_power_card(self):
        self.select_native('PI17')
        self.ready(protocol='PI17', native_records=[dict(protocol='PI17', command='PS', sequence=1,
            timestamp_ms=1, received_at=time.monotonic(), fields={
                'grid_voltage': {'value': 230, 'unit': 'V'},
                'battery_power': {'value': 500, 'unit': 'W'}})])
        monitor = self.monitor
        monitor.refresh_native()
        self.assertEqual(monitor.native_metric_labels['battery'].cget('text'), 'Unavailable')
        self.assertEqual(monitor.native_metric_labels['grid'].cget('text'), 'Active: Unavailable\nApparent: Unavailable')
        battery = monitor.native_group_trees['battery']
        rows = [battery.item(row, 'values') for row in battery.get_children()]
        self.assertIn(('Battery power', '500', 'W'), [row[:3] for row in rows])
        monitor.native_session.send_native.assert_not_called()

    def test_stale_electrical_readings_not_refreshed_by_identity_or_mode(self):
        self.select_native()
        now = time.monotonic()
        self.ready(native_records=[dict(protocol='PI30', command='QPIGS', sequence=1,
            timestamp_ms=1, received_at=now - 60,
            fields={'pv_power': {'value': 830, 'unit': 'W'}}),
            dict(protocol='PI30', command='QMOD', sequence=2, timestamp_ms=2, received_at=now,
                 fields={'device_mode': {'value': 'Standby', 'unit': ''}})])
        self.monitor.refresh_native()
        self.assertEqual(self.monitor.native_metric_labels['pv'].cget('text'), 'Stale')
        self.assertIn('STALE', self.monitor.freshness_label.cget('text'))
        self.assertIn('Standby', self.monitor.native_mode_label.cget('text'))
        self.assertIn('Fresh', self.monitor.native_mode_label.cget('text'))
        self.assertIn('Waiting for fresh electrical', self.monitor.native_live_notice.cget('text'))

    def test_bms_normal_view_shows_pack_current_soc_temperature_and_cells(self):
        self.select_native('JK02')
        self.ready(protocol='JK02', native_records=[dict(protocol='JK02', command='getCellData', sequence=1,
            timestamp_ms=1, received_at=time.monotonic(), fields={
                'voltage_cell01': {'value': 3.25, 'unit': 'V'},
                'battery_voltage': {'value': 52, 'unit': 'V'},
                'battery_current': {'value': -3, 'unit': 'A'},
                'percent_remain': {'value': 75, 'unit': '%'},
                'mos_temp': {'value': -5, 'unit': 'C'},
                'delta_cell_voltage': {'value': .01, 'unit': 'V'}})])
        monitor = self.monitor
        monitor.refresh_native()
        for key, title, value in [('battery', 'Battery', '+0.156 kW'), ('pv', 'Current', '-3 A'),
                                  ('grid', 'State of charge', '75 %'), ('output', 'Temperature', '-5 C'),
                                  ('load', 'Cell difference', '0.01 V')]:
            self.assertEqual(monitor.native_metric_frames[key].cget('text'), title)
            self.assertEqual(monitor.native_metric_labels[key].cget('text'), value)
        battery = monitor.native_group_trees['battery']
        self.assertEqual(len(battery.get_children()), 6)
        self.assertEqual(int(battery.master.grid_info()['columnspan']), 2)
        self.assertFalse(monitor.native_group_trees['grid'].master.grid_info())
        monitor.native_session.send_native.assert_not_called()

    def test_native_live_updates_preserve_rows_selection_and_stale_transitions(self):
        self.select_native()
        now = time.monotonic()
        records = [dict(protocol='PI30', command='QPIGS', sequence=1,
                        timestamp_ms=1, received_at=now,
                        fields={'pv_power': {'value': 830, 'unit': 'W'}})]
        snapshot = self.ready(native_records=records, health={'telemetry_seen': True})
        monitor = self.monitor
        monitor.refresh_native()
        tree = monitor.native_group_trees['pv']
        rows = tree.get_children()
        tree.selection_set(rows[0])
        with patch.object(tree, 'insert', wraps=tree.insert) as insert, \
                patch.object(tree, 'delete', wraps=tree.delete) as delete, \
                patch.object(tree, 'item', wraps=tree.item) as item, \
                patch.object(monitor, 'clear_tree', wraps=monitor.clear_tree) as clear:
            for _ in range(10):
                monitor.refresh_native()
            insert.assert_not_called()
            delete.assert_not_called()
            item.assert_not_called()
            clear.assert_not_called()
            snapshot['native_live'] = normalize_native_live(records, 'PI30', 0, now + 60)
            monitor.refresh_native()
            self.assertEqual(tree.get_children(), rows)
            self.assertEqual(tree.selection(), (rows[0],))
            self.assertEqual(monitor.native_metric_labels['pv'].cget('text'), 'Stale')
            self.assertTrue(tree.item(rows[0], 'values')[3].startswith('Stale'))
            insert.assert_not_called()
            delete.assert_not_called()
        monitor.native_session.snapshot.assert_called_with(include_records=False)
        monitor.native_session.send_native.assert_not_called()

    def test_diagnostics_and_raw_json_load_only_when_selected(self):
        self.select_native()
        self.ready(native_records=[dict(protocol='PI30', command='QPIGS', sequence=1,
            timestamp_ms=1, received_at=time.monotonic(),
            fields={'pv_power': {'value': 830, 'unit': 'W'}})])
        monitor = self.monitor
        with patch('inverter_monitor.json.dumps', wraps=json.dumps) as dumps:
            monitor.refresh_native()
            monitor.native_session.snapshot.assert_called_with(include_records=False)
            self.assertFalse(monitor.native_tree.get_children())
            dumps.assert_not_called()
            monitor.notebook.select(monitor.native_frames[2])
            monitor.native_tab_changed()
            monitor.native_session.snapshot.assert_called_with(include_records=True)
            rows = monitor.native_tree.get_children()
            self.assertEqual(len(rows), 1)
            monitor.native_tree.selection_set(rows[0])
            dumps.assert_not_called()
            monitor.notebook.select(monitor.raw_frame)
            monitor.native_tab_changed()
            dumps.assert_called_once()
            self.assertIn('pv_power', monitor.raw_text.get('1.0', 'end'))
            monitor.notebook.select(monitor.native_frames[0])
            monitor.native_tab_changed()
            monitor.native_session.snapshot.assert_called_with(include_records=False)
            self.assertEqual(monitor.native_tree.get_children(), rows)
            self.assertEqual(monitor.native_tree.selection(), (rows[0],))
            dumps.assert_called_once()

    def test_native_timer_refreshes_once_per_second(self):
        self.select_native()
        self.ready()
        monitor = self.monitor
        with patch.object(monitor, 'refresh_native') as refresh, \
                patch.object(monitor.root, 'after'), \
                patch('inverter_monitor.time.monotonic', side_effect=[100 + n / 10 for n in range(11)]):
            for _ in range(11):
                monitor.process_queue()
        self.assertEqual(refresh.call_count, 2)


if __name__ == "__main__":
    unittest.main()
