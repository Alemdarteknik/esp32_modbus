"""Check actual C++ MQTT output end-to-end through the Python monitor.

Run after Test-MqttNativeProtocols.ps1 has exported its PI30 JSONL fixture:
    python check_firmware_native_payloads.py PATH_TO_pi30_actual_payloads.jsonl
The path is explicit; this check has no firmware build or network side effects.
"""
import json
from pathlib import Path
from unittest.mock import patch

from mqtt_backend import MonitorSession
from native_protocols import PROTOCOL_IDS
from native_live import _kind


def check_firmware_payloads(path):
    envelopes = [json.loads(line) for line in Path(path).read_text(encoding='utf-8').splitlines()
                 if line.strip()]
    if len(envelopes) < 2:
        raise AssertionError('Expected both atomic protocol and consolidated live MQTT payloads.')
    parsed = [(envelope['topic'], json.loads(envelope['payload'])) for envelope in envelopes]
    protocols = [(topic, data) for topic, data in parsed if topic.endswith('/protocol/QPIGS')]
    lives = [(topic, data) for topic, data in parsed if topic.endswith('/live')]
    if not protocols or not lives:
        raise AssertionError('Missing real QPIGS or live payload.')
    base = protocols[0][0].rsplit('/protocol/', 1)[0]
    member = protocols[0][1]['member_id']
    session = MonitorSession()
    session.select_protocol('PI30')
    session.member_id = member
    session.base = base
    with patch('mqtt_backend.time.monotonic', return_value=100):
        for topic, data in parsed:
            session.receive(topic, data)
        state = session.snapshot()
    expected = {'pv_kw': .856, 'load_kw': .119, 'load_apparent_kva': .161,
                'load_percentage': 3, 'battery_kw': -.690}
    if state['native_live']['power_summary'] != expected:
        raise AssertionError(f'Wrong normalized production telemetry: {state["native_live"]["power_summary"]!r}')
    fields = state['native_records'][0]['fields']
    if len(fields) != 30 or fields['battery_voltage']['value'] != 57.5:
        raise AssertionError('Production PI30 response was incomplete or incorrectly decoded.')
    if fields['ac_input_voltage']['value'] != 0 or fields['battery_discharge_current']['value'] != 0:
        raise AssertionError('Valid zero-valued measurements were lost.')
    if state['native_drops'] or not state['native_live']['telemetry_available']:
        raise AssertionError('Complete firmware telemetry was rejected or marked missing.')
    if 'grid_kw' in state['native_live']['power_summary'] or 'grid_apparent_kva' in state['native_live']['power_summary']:
        raise AssertionError('Unreported power measurements were invented.')
    session.native_telemetry.reset()
    with patch('mqtt_backend.time.monotonic', return_value=100):
        session.receive(*lives[0])
        live_only = session.snapshot()
    if live_only['native_live']['power_summary'] != expected or live_only['native_drops']:
        raise AssertionError('Standalone consolidated live publication did not populate monitoring.')
    with patch('mqtt_backend.time.monotonic', return_value=116):
        stale = session.snapshot()
    if stale['native_live']['telemetry_available'] or not stale['native_live']['groups']['pv']['stale']:
        raise AssertionError('Stale production readings remained fresh.')
    return {'messages': len(parsed), 'pi30_fields': len(fields), 'power_summary': expected}


def check_all_firmware_payloads(path):
    """Validate every native profile using actual decoded firmware JSON.

    The C++ exporter supplies complete observations and their consolidated live
    publication. Comparisons cover every reported field, every group, freshness
    and independently serialized totals; no network connection is opened.
    """
    envelopes = [json.loads(line) for line in Path(path).read_text(encoding='utf-8').splitlines()
                 if line.strip()]
    by_profile = {}
    for envelope in envelopes:
        data = json.loads(envelope['payload'])
        profile = data.get('protocol')
        by_profile.setdefault(profile, []).append((envelope['topic'], data, envelope))
    expected_profiles = set(PROTOCOL_IDS) - {'AOHAI_MODBUS_RTU'}
    if set(by_profile) != expected_profiles:
        raise AssertionError(f'Profile coverage mismatch: {set(by_profile) ^ expected_profiles}')
    result = {'messages': len(envelopes), 'profiles': {}}
    for profile in PROTOCOL_IDS:
        if profile not in by_profile:
            continue
        parsed = by_profile[profile]
        lives = [(topic, data, metadata) for topic, data, metadata in parsed if topic.endswith('/live')]
        atomic_records = [data for topic, data, _ in parsed if '/protocol/' in topic]
        if not lives or not atomic_records:
            raise AssertionError(f'{profile}: missing atomic or live publication.')
        topic, data, metadata = lives[-1]
        session = MonitorSession()
        session.select_protocol(profile)
        session.member_id = data['member_id']
        session.base = topic[:-len('/live')]
        with patch('mqtt_backend.time.monotonic', return_value=100):
            session.receive(topic, data)
            state = session.snapshot()
        if state['native_drops']:
            raise AssertionError(f'{profile}: rejected production payload: {state["native_diagnostics"]}')
        expected_records = {record['command']: record['fields'] for record in data['native_records']}
        actual_records = {record['command']: record['fields'] for record in state['native_records']}
        if actual_records != expected_records:
            raise AssertionError(f'{profile}: complete decoded readings were lost.')
        live = state['native_live']
        fixture_fields = metadata.get('expected_fields')
        if not fixture_fields:
            raise AssertionError(f'{profile}: exporter omitted independently checked decoder fields.')
        expected_groups = {}
        for command, fields in fixture_fields.items():
            if command[:15] not in actual_records:
                raise AssertionError(f'{profile}: complete decoded command {command} was lost.')
            for name, field in fields.items():
                expected_groups.setdefault(field['group'], {})[name] = {
                    'unit': field['unit'], 'value': field['value']}
        actual_groups = {name: {key: {'unit': field['unit'], 'value': field['value']}
                               for key, field in group['fields'].items()}
                         for name, group in live['groups'].items()}
        for name in set(expected_groups) | set(actual_groups):
            if expected_groups.get(name, {}) != actual_groups.get(name, {}):
                actual, expected = actual_groups.get(name, {}), expected_groups.get(name, {})
                differences = {key: {'monitor': actual.get(key), 'firmware': expected.get(key)}
                               for key in set(actual) | set(expected) if actual.get(key) != expected.get(key)}
                raise AssertionError(f'{profile}: {name} field grouping differs between firmware and monitor: '
                                     f'{differences}')
        fresh_summary = {key: value for key, value in live['power_summary'].items()
                         if live['power_quality'][key]['fresh']}
        firmware_summary = {key: value for key, value in data['power_summary'].items()
                            if type(value) in (int, float)}
        if set(fresh_summary) != set(firmware_summary) or any(
                abs(value - firmware_summary[key]) > 0.000001 for key, value in fresh_summary.items()):
            raise AssertionError(f'{profile}: normalized totals differ: {fresh_summary} vs {firmware_summary}')
        if not live['telemetry_available']:
            raise AssertionError(f'{profile}: actual live sensors are missing.')
        with patch('mqtt_backend.time.monotonic', return_value=116):
            stale = session.snapshot()['native_live']
        if stale['telemetry_available'] or any(flags['fresh'] for flags in stale['power_quality'].values()):
            raise AssertionError(f'{profile}: expired electrical readings remained fresh.')
        # Atomic subscriptions must yield identical observations independently.
        session.native_telemetry.reset()
        with patch('mqtt_backend.time.monotonic', return_value=100):
            for record in atomic_records:
                session.receive(session.base + '/protocol/' + record['command'], record)
            atomic_state = session.snapshot()
        if atomic_state['native_drops'] or not atomic_state['native_live']['telemetry_available']:
            raise AssertionError(f'{profile}: complete query subscriptions cannot populate the monitor.')
        result['profiles'][profile] = {'fields': sum(map(len, actual_records.values())),
                                      'commands': len(actual_records), 'power_summary': fresh_summary}
    return result


def check_background_poll_catalog(path):
    """Compare monitor metadata classes with the real firmware API export."""
    catalog = json.loads(Path(path).read_text(encoding='utf-8'))
    count = 0
    classes = {'live': None, 'configuration': 'settings', 'status': 'status',
               'identity': 'identity', 'energy': 'energy'}
    for profile in catalog['profiles']:
        for query in profile['queries']:
            actual = _kind({'protocol': profile['protocol'], 'command': query['command'][:15]})
            expected = classes[query['class']]
            if actual != expected:
                raise AssertionError(f'{profile["protocol"]} {query["command"]}: '
                                     f'firmware class {expected!r} differs from monitor {actual!r}.')
            count += 1
    return count


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('jsonl_path')
    parser.add_argument('--all-profiles', action='store_true', help='Check all 25 native profiles instead of the PI30 regression.')
    parser.add_argument('--poll-catalog', help='Also compare the exported firmware background polling classes.')
    arguments = parser.parse_args()
    checker = check_all_firmware_payloads if arguments.all_profiles else check_firmware_payloads
    result = checker(arguments.jsonl_path)
    if arguments.poll_catalog:
        result['background_queries'] = check_background_poll_catalog(arguments.poll_catalog)
    print(json.dumps(result, sort_keys=True))
