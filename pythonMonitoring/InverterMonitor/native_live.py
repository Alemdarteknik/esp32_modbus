"""Protocol-independent live view from complete, validated device observations.

The command names remain source metadata for diagnostics.  A rating/setting or
identity response cannot make a live sensor appear fresh.  Unsupported values
are omitted. DC battery power and AC apparent power can use coherent measured
voltage/current pairs; battery power is positive discharge, negative charge.
"""
from __future__ import annotations

import copy
import math

GROUPS = ('pv', 'grid', 'battery', 'output', 'load', 'status',
          'identity', 'settings', 'energy', 'other')
_IDENTITY = {'QPI', 'PI', 'QID', 'QSID', 'ID', 'getInfo', 'QVFW', 'QVFW2',
             'QVFW3', 'VERFW', 'VFW', 'VFW2', 'QGMN', 'QMN', 'MD', 'QBOOT',
             'getDeviceInfo', 'getVersion', 'getModel'}
_SETTINGS = {'QPIRI', 'PIRI', 'QFLAG', 'FLAG', 'BATS', 'PQSE', 'getAdditionalInfo',
             'QDI', 'DI', 'QMCHGCR', 'QMUCHGCR', 'MCHGCR', 'MUCHGCR',
             'QOPM', 'QOPPT', 'QCHPT', 'QBEQI', 'QLED', 'QDOP', 'GPMP', 'GPMP0',
             'HECS', 'ACCT', 'ACLT', 'PRI', 'QRI', 'QPOP', 'QPCP', 'QF',
             'getSettings', 'getConfig', 'getParameters', 'batteryCapacity'}
_STATUS = {'QMOD', 'MOD', 'QPIWS', 'WS', 'FWS', 'mosStatus', 'batteryFailureStatus',
           'status', 'cellBalanceState', 'QT', 'T', 'QMD', 'QWS'}
_ENERGY = {'ET', 'QET', 'QLT', 'QEH', 'QED', 'QEM', 'QEY', 'QLY', 'QLM',
           'QLD', 'EY', 'EM', 'ED', 'EH'}
_BMS = {'JK02', 'JK02_32', 'JK04', 'JK232', 'JK485', 'JKPB', 'JKV11',
        'JKSERIAL', 'DALY', 'DALY40'}

_IDENTITY.add('DM')
_IDENTITY_FIELDS = {'serial_number', 'protocol_id', 'protocol_version', 'firmware_version',
                    'hardware_version', 'software_version', 'model', 'model_code',
                    'device_model', 'device_name', 'production_date', 'manufacturing_date',
                    'up_time', 'power_on_times'}
_SETTING_FIELDS = {'battery_bulk_charging_voltage_from_bms',
                   'battery_float_charging_voltage_from_bms', 'battery_cut_off_voltage_from_bms',
                   'battery_max_charging_current', 'battery_max_discharge_current',
                   'battery_max_charge_voltage', 'battery_min_discharge_voltage'}
_PV_CHANNELS = {
    'PI16': ('pv1_input_power', 'pv2_input_power', 'pv3_input_power'),
    **{profile: ('solar_input_power_1', 'solar_input_power_2')
       for profile in ('PI17', 'PI17M058', 'PI17INFINI')},
    **{profile: ('pv1_input_power', 'pv2_input_power')
       for profile in ('PI18', 'PI18LVX', 'PI18SV')},
    **{profile: ('pv1_charging_power', 'pv2_charging_power')
       for profile in ('PI30MAX', 'PI30M044', 'PI30M045')},
    'PI30MST': ('pv1_charging_power', 'pv2_charging_power', 'pv3_charging_power'),
}
_PRIMARY_NAMES = {
    'pv': ('pv_total_charging_power', 'pv_input_power', 'pv_power', 'ppv',
           'pv1_charging_power', 'pv1_input_power', 'solar_input_power_1',
           'pv_input_voltage', 'pv1_input_voltage', 'solar_input_voltage_1', 'vpv'),
    'grid': ('ac_input_total_active_power', 'grid_active_power', 'ac_input_active_power',
             'ac_input_voltage', 'grid_voltage', 'l1_ac_input_voltage', 'ac_input_voltage_r'),
    'battery': ('battery_power', 'total_battery_power', 'p', 'battery_voltage',
                'total_battery_voltage', 'battery_bank_voltage', 'v', 'percent_remain',
                'remaining_battery', 'battery_capacity', 'soc', 'average_cell_voltage',
                'voltage_cell01', 'battery_current', 'total_current', 'current', 'i'),
    'output': ('ac_output_total_active_power', 'ac_output_active_power', 'output_power',
               'l1_ac_output_active_power', 'ac_output_voltage', 'ac_output_voltage_r',
               'l1_ac_output_voltage', 'ac_out_v', 'ac_out_s'),
    'load': ('ac_output_power_percentage', 'output_load_percent', 'ac_output_load',
             'load_percentage', 'l1_ac_output_load', 'il'),
}


def _kind(record):
    command = record.get('command', '')
    # MQTT command aliases are bounded to fifteen bytes. Resolve fixed and
    # parameterized query names before applying the firmware polling classes.
    from native_protocols import PROFILES
    definition = None
    for candidate in PROFILES.get(record.get('protocol'), {}).get('queries', []):
        if (candidate['key'][:15] == command or (candidate.get('pattern') is not None
                                                and command.startswith(candidate['key'][:15]))):
            definition = candidate
            command = candidate['key']
            break
    if command in _IDENTITY or command.startswith('QVFW'):
        return 'identity'
    if record.get('protocol') == 'JKPB' and command == 'getBalancerData':
        return 'identity'
    if command in _SETTINGS:
        return 'settings'
    if command in _STATUS:
        return 'status'
    if command in _ENERGY:
        return 'energy'
    # Labels supplement known fixed queries for the many native variants.
    if definition:
        label = definition.get('label', '').casefold()
        if any(word in label for word in ('rated information', 'rating information',
                                         'default setting', 'configuration', 'settings')):
            return 'settings'
        if any(word in label for word in ('serial number', 'firmware version',
                                         'model name', 'protocol id', 'device information')):
            return 'identity'
    return None


def _group(name, field, protocol, kind):
    if kind:
        return kind
    unit = field.get('unit', '')
    if name in _SETTING_FIELDS:
        return 'settings'
    if name in _IDENTITY_FIELDS:
        return 'identity'
    if unit in ('Wh', 'kWh') or 'energy' in name:
        return 'energy'
    if (unit == 'bool' or name.startswith('is_')
            or any(word in name for word in ('fault', 'warning', 'alarm', 'protection',
                                            'status', 'enabled', 'error', 'mode',
                                            'direction', 'connection', 'flag'))
            or name in ('setting_change_bit', 'setting_value_configuration_state',
                        'load_connection', 'balancer_active')):
        return 'status'
    if protocol == 'VED':
        if name in ('pid', 'fw', 'ser', 'ser_'):
            return 'identity'
        if name in ('ppv', 'vpv'):
            return 'pv'
        if name.startswith('ac_out_'):
            return 'output'
        if name in ('cs', 'err', 'alarm', 'ar', 'relay', 'or', 'hsds', 'mppt'):
            return 'status'
        if name.startswith('h') and name[1:].isdigit():
            return 'energy'
        if name in ('v', 'v2', 'v3', 'vs', 'vm', 'i', 'i2', 'i3', 'p', 'soc',
                    't', 'ce', 'ttg'):
            return 'battery'
        if name in ('load', 'il'):
            return 'load'
    if protocol in _BMS:
        return 'battery'
    # Phase prefixes do not change which physical circuit a sensor observes.
    circuit_name = name[3:] if name.startswith(('l1_', 'l2_', 'l3_')) else name
    if (('battery' in circuit_name and not circuit_name.startswith(('pv_', 'pv1_', 'pv2_', 'pv3_')))
            or any(word in circuit_name for word in (
            'cell', 'capacity', 'percent_remain', 'soc', 'balance', 'mos_temp'))):
        return 'battery'
    if circuit_name.startswith(('ac_input_', 'input_current', 'grid_', 'utility_', 'line_')):
        return 'grid'
    if circuit_name == 'solar_feed_to_grid_power':
        return 'grid'
    if name == 'actpvpow':
        return 'pv'
    if name == 'actfeedpow':
        return 'grid'
    if circuit_name in ('ac_output_load', 'load_percentage', 'total_ac_output_percentage',
                        'ac_output_power_percentage', 'output_load_percent',
                        'total_output_load_percent'):
        return 'load'
    if (circuit_name.startswith(('pv_', 'pv1_', 'pv2_', 'pv3_', 'solar_'))
            or any(word in circuit_name for word in ('mppt', 'scc_'))):
        return 'pv'
    if 'output' in circuit_name and not circuit_name.endswith('_mode'):
        return 'output'
    if circuit_name.startswith('load'):
        return 'load'
    if 'temperature' in circuit_name or 'heat_sink' in circuit_name:
        return 'status'
    return 'other'


def _battery_metrics(fields):
    metrics = {}
    choices = {
        'current': ('battery_current', 'total_current', 'current', 'i', 'balance_current'),
        'state_of_charge': ('percent_remain', 'soc', 'remaining_battery', 'battery_capacity'),
        'temperature': ('mos_temp', 'battery_temperature', 'battery_t1', 'battery_t01', 'temperature',
                        'maximum_cell_temperature', 'ntc_01', 't'),
        'cell_difference': ('delta_cell_voltage', 'voltage_difference'),
    }
    for metric, names in choices.items():
        available = [(name, fields[name]) for name in names if name in fields
                     and type(fields[name]['value']) in (int, float)]
        fresh = [pair for pair in available if pair[1]['fresh']]
        if fresh or available:
            name, item = (fresh or available)[0]
            metrics[metric] = dict(name=name, **copy.deepcopy(item))
    return metrics


def _reading(value, unit, record, now, limit):
    age = max(0.0, now - record['received_at'])
    return {'value': copy.deepcopy(value), 'unit': unit,
            'received_at': record['received_at'], 'age_seconds': age,
            'fresh': age <= limit, 'stale': age > limit,
            'source_command': record['command'], 'sequence': record['sequence'],
            'timestamp_ms': record['timestamp_ms']}


def _coherent(*items):
    return bool(items) and len({(item['source_command'], item['sequence'], item['timestamp_ms'],
                                 item['received_at']) for item in items}) == 1


def _calculated_power(groups, protocol):
    calculated = {}

    def fields(group, names, unit):
        available = groups[group]['fields']
        for name in names:
            item = available.get(name)
            if item and item['unit'] == unit and type(item['value']) in (int, float):
                return name, item
        return None

    def derived(key, components, value, unit):
        if any(part is None for part in components):
            return
        items = [item for _, item in components]
        if not _coherent(*items):
            return
        oldest = min(items, key=lambda item: item['received_at'])
        calculated[key] = (tuple(name for name, _ in components), dict(oldest,
            value=0.0 if value == 0 else value, unit=unit,
            fresh=all(item['fresh'] for item in items), stale=any(item['stale'] for item in items)))

    voltage = fields('battery', ('battery_voltage', 'total_battery_voltage', 'battery_bank_voltage',
                                 'total_voltage', 'v' if protocol == 'VED' else ''), 'V')
    charge_positive = protocol in {'JK02', 'JK02_32', 'JKV11', 'JKPB', 'JK232', 'JKSERIAL', 'VED'}
    if charge_positive or protocol in {'DALY', 'DALY40'}:
        current = fields('battery', ('battery_current', 'total_current', 'current',
                                     'i' if protocol == 'VED' else ''), 'A')
        if voltage and current and voltage[1]['value'] > 0:
            derived('battery_kw', (voltage, current),
                    voltage[1]['value'] * current[1]['value'] * (-1 if charge_positive else 1) / 1000, 'kW')
    else:
        charge = fields('battery', ('battery_charging_current', 'battery_charge_current'), 'A')
        discharge = fields('battery', ('battery_discharge_current', 'battery_discharging_current'), 'A')
        if voltage and charge and discharge and voltage[1]['value'] > 0 and min(
                charge[1]['value'], discharge[1]['value']) >= 0:
            derived('battery_kw', (voltage, charge, discharge),
                    voltage[1]['value'] * (discharge[1]['value'] - charge[1]['value']) / 1000, 'kW')
        if protocol == 'PI30REVO' and ('battery_kw' not in calculated or not calculated['battery_kw'][1]['fresh']):
            voltage = fields('battery', ('battery_voltage_from_bms',), 'V')
            charge = fields('battery', ('battery_charging_current_from_bms',), 'A')
            discharge = fields('battery', ('battery_discharge_current_from_bms',), 'A')
            if voltage and charge and discharge and voltage[1]['value'] > 0 and min(
                    charge[1]['value'], discharge[1]['value']) >= 0:
                derived('battery_kw', (voltage, charge, discharge),
                        voltage[1]['value'] * (discharge[1]['value'] - charge[1]['value']) / 1000, 'kW')

    components, apparent = [], 0.0
    phases = ('r', 's', 't') if protocol in {'PI17', 'PI17M058', 'PI17INFINI'} else ('',)
    for phase in phases:
        suffix = '_' + phase if phase else ''
        voltage = fields('grid', ('ac_input_voltage' + suffix, 'grid_voltage' if not phase else ''), 'V')
        current = fields('grid', ('ac_input_current' + suffix, 'grid_current' if not phase else ''), 'A')
        if not voltage or not current or voltage[1]['value'] < 0:
            break
        components.extend((voltage, current))
        apparent += voltage[1]['value'] * abs(current[1]['value']) / 1000
    else:
        derived('grid_apparent_kva', components, apparent, 'kVA')
    return calculated


def _power_summary(groups, protocol):
    # These are reported totals or explicitly documented independent channels.
    # AC volts times amps can establish apparent power only. DC current sign
    # is normalized per protocol and components must share one observation.
    specifications = (
        ('pv_kw', 'pv', ('pv_total_charging_power',),
         ('pv_input_power', 'pv_power', 'solar_power', 'ppv'),
         ('W', 'kW'), _PV_CHANNELS.get(protocol, ())),
        ('grid_kw', 'grid', ('ac_input_total_active_power',),
         ('grid_active_power', 'ac_input_active_power', 'grid_power'), ('W', 'kW'), ()),
        ('grid_apparent_kva', 'grid', ('ac_input_total_apparent_power',),
         ('grid_apparent_power', 'ac_input_apparent_power'), ('VA', 'kVA'), ()),
        ('load_kw', 'output', ('ac_output_total_active_power', 'total_output_active_power',
                             'total_ac_output_active_power'),
         ('ac_output_active_power', 'output_active_power', 'output_power'), ('W', 'kW'),
         ('l1_ac_output_active_power', 'l2_ac_output_active_power') if protocol == 'PI41' else ()),
        ('load_apparent_kva', 'output', ('ac_output_total_apparent_power',
                                       'total_output_apparent_power', 'total_ac_output_apparent_power'),
         ('ac_output_apparent_power', 'output_apparent_power', 'ac_out_s'), ('VA', 'kVA'),
         ('l1_ac_output_apparent_power', 'l2_ac_output_apparent_power') if protocol == 'PI41' else ()),
        ('load_percentage', 'load', (),
         ('ac_output_power_percentage', 'output_load_percent', 'ac_output_load',
          'load_percentage', 'total_output_load_percent', 'total_ac_output_percentage'), ('%',), ()),
        ('battery_kw', 'battery', ('total_battery_power',),
         ('battery_power', 'p'), ('W', 'kW'), ()),
    )
    summary, quality = {}, {}
    calculated = _calculated_power(groups, protocol)
    for key, group_name, total_names, aliases, units, channels in specifications:
        fields = groups[group_name]['fields']
        def candidates(names):
            return [(name, fields[name]) for name in names if name in fields
                    and fields[name]['unit'] in units
                    and type(fields[name]['value']) in (int, float)]
        totals, direct = candidates(total_names), candidates(aliases)
        if key == 'battery_kw':
            if protocol in {'JK02', 'JK02_32', 'JKV11', 'JKPB'}:
                totals, direct = [], []
            elif protocol in {'PI17', 'PI17M058', 'PI17INFINI'}:
                direction = groups['status']['fields'].get('battery_power_direction')
                def signed(choices):
                    result = []
                    for name, item in choices:
                        if item['value'] == 0:
                            result.append((name, item))
                        elif direction and _coherent(item, direction) and direction['value'] in ('Charging', 'Discharging'):
                            result.append((name, dict(item,
                                value=abs(item['value']) * (-1 if direction['value'] == 'Charging' else 1),
                                fresh=item['fresh'] and direction['fresh'], stale=item['stale'] or direction['stale'])))
                    return result
                totals, direct = signed(totals), signed(direct)
            elif protocol == 'VED':
                totals = [(name, dict(item, value=-item['value'])) for name, item in totals]
                direct = [(name, dict(item, value=-item['value'])) for name, item in direct]
        channel_fields = candidates(channels)
        aggregate = None
        if channels and len(channel_fields) == len(channels):
            # Oldest component determines the aggregate's freshness.
            oldest = min((item for _, item in channel_fields), key=lambda item: item['received_at'])
            aggregate = (channels, dict(oldest,
                value=sum(item['value'] / 1000.0 if item['unit'] in ('W', 'VA') else item['value']
                          for _, item in channel_fields),
                unit='kVA' if 'VA' in units else 'kW',
                fresh=all(item['fresh'] for _, item in channel_fields),
                stale=any(item['stale'] for _, item in channel_fields),
                source_command=' + '.join(dict.fromkeys(item['source_command'] for _, item in channel_fields))))
        if aggregate is None:
            aggregate = calculated.get(key)
        selected = None
        for choices in (totals, direct):
            fresh = [(name, item) for name, item in choices if item['fresh']]
            if fresh:
                selected = max(fresh, key=lambda pair: pair[1]['received_at'])
                break
        if selected is None and aggregate and aggregate[1]['fresh']:
            selected = aggregate
        if selected is None:
            for choices in (totals, direct):
                if choices:
                    selected = max(choices, key=lambda pair: pair[1]['received_at'])
                    break
        if selected is None:
            selected = aggregate
        if selected is None:
            continue
        name, item = selected
        summary[key] = item['value'] / 1000.0 if item['unit'] in ('W', 'VA') else item['value']
        quality[key] = {k: item[k] for k in ('received_at', 'age_seconds', 'fresh', 'stale', 'source_command')}
        quality[key]['source_field'] = ' + '.join(name) if isinstance(name, tuple) else name
        if isinstance(name, tuple):
            quality[key]['source_fields'] = list(name)
    return summary, quality


def normalize_native_live(records, protocol, member_id, now, live_limit_seconds=15.0):
    """Return a copied live snapshot; ``now`` and receipts use monotonic seconds.

    Group fields retain stable firmware names.  Power summaries only convert
    explicit W/VA values to kW/kVA. Coherent DC voltage/current readings use
    documented protocol signs; AC voltage/current gives apparent power only.
    Zero is a valid observation, never missing.
    """
    if type(now) not in (int, float) or not math.isfinite(now):
        raise ValueError('now must be a finite monotonic time.')
    groups = {name: {'fields': {}, 'received_at': None, 'age_seconds': None,
                     'fresh': False, 'stale': False, 'supported': None, 'primary': None}
              for name in GROUPS}
    if protocol in _BMS:
        for name in ('pv', 'grid', 'output', 'load'):
            groups[name]['supported'] = False
    for record in records:
        if record.get('protocol') != protocol or record.get('member_id') != member_id:
            continue
        kind = _kind(record)
        for name, field in record.get('fields', {}).items():
            group_name = _group(name, field, protocol, kind)
            group = groups[group_name]
            limit = 300.0 if group_name in ('identity', 'settings') else (
                120.0 if group_name == 'energy' else live_limit_seconds)
            previous = group['fields'].get(name)
            if previous and (record['timestamp_ms'], record['sequence']) <= (
                    previous['timestamp_ms'], previous['sequence']):
                continue
            item = _reading(field['value'], field['unit'], record, now, limit)
            group['fields'][name] = item
            group['supported'] = True
    numeric_receipts = [item['received_at'] for name, group in groups.items()
                        if name in ('pv', 'grid', 'battery', 'output', 'load')
                        for item in group['fields'].values()
                        if type(item['value']) in (int, float) and item['unit'] in (
                            'V', 'A', 'W', 'kW', 'VA', 'kVA', '%', 'Hz', 'Ah', 'C', 'Deg_C')]
    for name, group in groups.items():
        if not group['fields']:
            continue
        group['received_at'] = max(item['received_at'] for item in group['fields'].values())
        group['age_seconds'] = max(0.0, now - group['received_at'])
        group['fresh'] = any(item['fresh'] for item in group['fields'].values())
        group['stale'] = not group['fresh']
        named = [key for key in _PRIMARY_NAMES.get(name, ()) if key in group['fields']
                 and type(group['fields'][key]['value']) in (int, float)]
        preferred = next((key for key in named if group['fields'][key]['fresh']), None)
        if preferred is None:
            preferred = next(iter(named), None)
        if preferred is None:
            preferred = next((key for key, item in group['fields'].items()
                              if type(item['value']) in (int, float) and item['unit'] in ('W', 'kW')), None)
        if preferred is None:
            preferred = next((key for key, item in group['fields'].items()
                              if type(item['value']) in (int, float) and item['unit'] in ('V', '%', 'A')), None)
        if preferred is not None:
            group['primary'] = dict(name=preferred, **copy.deepcopy(group['fields'][preferred]))
    summary, quality = _power_summary(groups, protocol)
    received_at = max(numeric_receipts) if numeric_receipts else None
    return {'protocol': protocol, 'member_id': member_id, 'groups': groups,
            'device_type': 'bms' if protocol in _BMS else 'inverter',
            'battery_metrics': _battery_metrics(groups['battery']['fields']),
            'received_at': received_at,
            'telemetry_available': received_at is not None and now - received_at <= live_limit_seconds,
            'power_summary': summary, 'power_quality': quality,
            'operating_status': {name: copy.deepcopy(item['value'])
                                 for name, item in groups['status']['fields'].items()}}
