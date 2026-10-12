"""Legacy AOHAI settings keep command/result correlation for every member."""
import json
import unittest
from types import SimpleNamespace

from settings_selection import setting_member_matches


class MemberRoutingTests(unittest.TestCase):
    def test_strict_topic_member_matching(self):
        for member in range(10):
            monitor = SimpleNamespace(mqtt_topic=f'inverter/INV-TEST/inverter/{member}/live')
            self.assertTrue(setting_member_matches(monitor, dict(member_id=member)))
            self.assertFalse(setting_member_matches(monitor, dict(member_id=(member + 1) % 10)))
            self.assertFalse(setting_member_matches(monitor, dict(member_id=True)))
            self.assertFalse(setting_member_matches(monitor, dict(member_id=str(member))))
        self.assertFalse(setting_member_matches(SimpleNamespace(mqtt_topic='bad-topic'), dict(member_id=0)))

    def test_buzzer_result_member_nine(self):
        from test_buzzer_settings import BuzzerTests
        fixture = BuzzerTests()
        fixture.setUp()
        setting = fixture.setting
        setting.monitor.mqtt_topic = setting.monitor.mqtt_topic.replace('/0/live', '/9/live')
        base = setting.monitor.mqtt_topic.rsplit('/', 1)[0]
        setting.request('get')
        result = fixture.result(0)
        setting.receive(base + '/result', result)
        self.assertIsNotNone(setting.pending)
        result['member_id'] = 9
        setting.receive(base + '/result', result)
        self.assertIsNone(setting.pending)
        setting.value.set.assert_called_with('Disable')

    def test_time_result_member_nine(self):
        from test_time_settings import TimeTests
        fixture = TimeTests()
        fixture.setUp()
        setting = fixture.s
        setting.monitor.mqtt_topic = setting.monitor.mqtt_topic.replace('/0/live', '/9/live')
        base = setting.monitor.mqtt_topic.rsplit('/', 1)[0]
        setting.request('get')
        cid, operation, name, _ = setting.pending
        result = dict(member_id=0, command_id=cid, operation=operation,
                      name=name, value=2026, outcome='confirmed')
        setting.receive(base + '/result', result)
        self.assertEqual(setting.read_values, {})
        result['member_id'] = 9
        setting.receive(base + '/result', result)
        self.assertEqual(setting.read_values[name], 2026)

    def test_catalog_result_member_nine(self):
        from test_settings_catalog import CatalogTests
        CatalogTests.setUpClass()
        fixture = CatalogTests()
        item = fixture.item('BatteryType')
        setting = fixture.make_ui(item)
        setting.monitor.mqtt_topic = setting.monitor.mqtt_topic.replace('/0/live', '/9/live')
        base = setting.monitor.mqtt_topic.rsplit('/', 1)[0]
        setting.request('get', key=item.key)
        cid = setting.pending[0]
        result = dict(member_id=0, command_id=cid, operation='get', name=item.name,
                      outcome='confirmed', value=1)
        setting.receive(base + '/result', result)
        self.assertIsNotNone(setting.pending)
        result['member_id'] = 9
        setting.receive(base + '/result', result)
        self.assertIsNone(setting.pending)
        self.assertEqual(setting.cache[item.key], 1)
        self.assertEqual(setting.monitor.mqtt_client.publish.call_args.args[0], base + '/command')


if __name__ == '__main__':
    unittest.main()
