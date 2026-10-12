import queue
import time
import tkinter as tk
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

from inverter_monitor import InverterMonitor, freshness_text


class LiveMonitorTests(unittest.TestCase):
    def test_offline_health_display_and_disconnect_clear(self):
        root = tk.Tk()
        root.withdraw()
        try:
            monitor = InverterMonitor(root)
            monitor.connection_active = True
            monitor.data_queue.put(("health", ({"telemetry_seen": True,
                "telemetry_age_ms": 0, "outbox_bytes": 0}, time.monotonic(), False)))
            monitor.process_queue()
            self.assertEqual(len(monitor.trees["Gateway Health"].get_children()), 3)
            monitor.clear_display()
            self.assertFalse(monitor.trees["Gateway Health"].get_children())
            self.assertIsNone(monitor.last_health_received)
            monitor.running = False
        finally:
            root.destroy()

    def test_freshness(self):
        self.assertEqual(freshness_text(None, 100, 90, 0), "MISSING")
        self.assertEqual(freshness_text(None, 10, 90, 0), "Waiting")
        self.assertEqual(freshness_text(95, 100, 15), "Fresh")
        self.assertEqual(freshness_text(50, 100, 15), "STALE (50s)")

    def test_health_routing(self):
        monitor = InverterMonitor.__new__(InverterMonitor)
        monitor.mqtt_topic = "inverter/INV-TEST/inverter/0/live"
        monitor.data_queue = queue.Queue()
        monitor.on_message(None, None, SimpleNamespace(
            topic=monitor.mqtt_topic.replace("/live", "/health"),
            payload=b'{"telemetry_seen":false}', retain=False))
        kind, (data, timestamp, retained) = monitor.data_queue.get_nowait()
        self.assertEqual(kind, "health")
        self.assertFalse(data["telemetry_seen"])
        self.assertGreater(timestamp, 0)
        self.assertFalse(retained)

    def test_power_sources(self):
        monitor = InverterMonitor.__new__(InverterMonitor)
        monitor.trees = {"Power Summary": Mock()}
        monitor.clear_tree = Mock()
        monitor.insert_value = Mock()
        monitor.insert_record = Mock()
        monitor.update_power_summary({"load_kw": 0.13, "load_source": "register",
                                      "load_apparent_kva": 2.82, "system_generation_kw": 2.37})
        records = {call.args[1]: call.args[2] for call in monitor.insert_record.call_args_list}
        self.assertEqual(records["Load active power"]["source"], "register")
        self.assertEqual(records["Load apparent power"]["source"], "")
        self.assertEqual(records["System generation"]["unit"], "kW")


if __name__ == "__main__":
    unittest.main()
