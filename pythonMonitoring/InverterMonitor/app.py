"""Run with: python -m streamlit run app.py"""
import json
import inspect
import time
from functools import lru_cache, partial
from datetime import datetime, date

import streamlit as st
from mqtt_backend import MonitorSession, TIME_FIELDS, display_value
from settings_selection import select_setting_row
from native_protocols import PROTOCOL_IDS, PROFILES, classify_command


AOHAI_PROTOCOL = 'AOHAI_MODBUS_RTU'
STATEFUL_CONTAINERS = ('on_change' in inspect.signature(st.tabs).parameters
                       and 'on_change' in inspect.signature(st.expander).parameters)


@lru_cache(maxsize=32)
def native_command_catalog(protocol):
    """Prepare static command labels once, independent of live session data."""
    catalog = PROFILES[protocol]
    entries = tuple((False, entry) for entry in catalog['queries']) + tuple(
        (True, entry) for entry in catalog['setters'])
    rows = tuple({'Command': entry['key'], 'Description': entry['label'],
                  'Kind': 'Setting' if setter else 'Query',
                  'Format': entry.get('pattern') or entry['key'],
                  'Readback query': entry.get('readback') or ''}
                 for setter, entry in entries)
    labels = tuple(f'{entry["key"]} · {entry["label"]} · {"Setting" if setter else "Query"}'
                   for setter, entry in entries)
    return entries, rows, labels


st.set_page_config(page_title='Inverter Monitor', page_icon='☀', layout='wide')
st.markdown('''<style>
.block-container {max-width:1200px; padding-top:2rem;}
h1 {font-size:2rem !important; letter-spacing:-.04em;}
button[kind="primary"] {background:#2563eb; border-color:#2563eb; color:white;}
</style>''', unsafe_allow_html=True)

if 'monitor' not in st.session_state:
    st.session_state.monitor = MonitorSession()
monitor = st.session_state.monitor


def live_value(value):
    if value is None:
        return 'Unavailable'
    return json.dumps(value) if isinstance(value, (dict, list)) else str(value)


def power_value(summary, quality, key):
    value = live_value(summary.get(key))
    flags = quality.get(key, {})
    if not isinstance(flags, dict):
        return value
    if flags.get('available') is False:
        return 'Unavailable'
    return value


def native_power_value(summary, quality, key, signed=False):
    value = summary.get(key)
    flags = quality.get(key, {})
    if value is None or flags.get('available') is False:
        return 'Unavailable'
    if flags.get('stale'):
        return 'Stale'
    if not flags.get('fresh'):
        return 'Unavailable'
    text = live_value(value)
    return '+' + live_value(abs(value)) if signed and isinstance(value, (int, float)) and value >= 0 else text


def table(data, all_rows=False):
    if not data:
        st.caption('No data received.')
        return
    if isinstance(data, dict):
        data = [{'Field': k.replace('_', ' '), 'Value': live_value(v)}
                for k, v in data.items()]
    else:
        data = [{k: live_value(v)
                 for k, v in row.items()} for row in data if isinstance(row, dict)]
        # Keep the reading visible before wide register/description metadata.
        columns = list(dict.fromkeys(k for row in data for k in row))
        order = [k for k in ('name', 'value', 'unit', 'age_ms', 'stale', 'since_valid_ms', 'source') if k in columns]
        order.extend(k for k in columns if k not in order)
        data = [{k: row.get(k, '') for k in order} for row in data]
    st.dataframe(data, hide_index=True, width='stretch',
                 height=max(100, 36 * (len(data) + 1)) if all_rows else 'auto')


def freshness(received, connected, timeout):
    now = time.monotonic()
    if received is None:
        return 'Missing' if connected is not None and now - connected > timeout else 'Waiting'
    age = int(now - received)
    return f'{"Fresh" if age <= timeout else "Stale"} · {age}s ago'


@st.fragment(run_every=1)
def interface():
    include_records = (st.session_state.get('monitor_tab', 'Live data') == 'Raw JSON'
                       or (st.session_state.get('monitor_tab', 'Live data') == 'Advanced diagnostics'
                           and st.session_state.get('native_diagnostics_open', False))) if STATEFUL_CONTAINERS else (
                       st.session_state.get('load_native_diagnostics', False)
                       or st.session_state.get('show_raw_json', False))
    state = monitor.snapshot(include_records=include_records)
    st.title('Inverter Monitor')
    st.caption('Live telemetry & device settings')
    left, right = st.columns([4, 1], vertical_alignment='bottom')
    device = left.text_input('Inverter ID', placeholder='12-character device ID',
                             disabled=state['active'], key='device_id')
    selected_protocol = state.get('protocol', AOHAI_PROTOCOL)
    protocol = st.selectbox('Protocol', PROTOCOL_IDS,
                            index=PROTOCOL_IDS.index(selected_protocol),
                            format_func=lambda key: PROFILES.get(key, {}).get('label', 'AOHAI / Mexxsun Modbus RS485'),
                            disabled=state['active'], key='monitor_protocol')
    member = int(st.number_input('Inverter member', min_value=0, max_value=9,
                                value=state.get('member_id', 0), step=1,
                                disabled=state['active'], key='monitor_member'))
    st.caption('Member 0 is the gateway\'s local inverter; use the configured member number for a remote inverter. Select the same protocol as the ESP installer.')
    if not state['active'] and protocol != selected_protocol:
        monitor.select_protocol(protocol)
        state = monitor.snapshot(include_records=include_records)
    if state['active']:
        if right.button('Disconnect', width='stretch'):
            monitor.close()
            # A new session owns a new MQTT client. The old worker cannot
            # deliver queued packets into the next connection's display.
            replacement = MonitorSession()
            replacement.select_protocol(protocol)
            replacement.member_id = member
            st.session_state.monitor = replacement
            st.rerun()
    elif right.button('Connect', type='primary', width='stretch'):
        try:
            monitor.connect(device, protocol=protocol, member_id=member)
            st.rerun()
        except Exception as error:
            st.error(str(error))
    if state.get('connection_error'):
        st.error(state['status'])
    else:
        st.caption(state['status'])
    if not state['active']:
        st.info('Enter your inverter ID to connect. Data and controls will appear here.')
        return
    native = state.get('protocol', AOHAI_PROTOCOL) != AOHAI_PROTOCOL
    records = state.get('native_records', []) if native else []
    received_at = state.get('native_live', {}).get('received_at') if native else state['live_at']
    st.caption('Live: ' + freshness(received_at, state['connected_at'], 15)
               + '   •   Gateway health: ' + freshness(state['health_at'], state['connected_at'], 90))
    if not native and state['live'].get('core_status_fresh') is False:
        st.warning('The gateway reports stale core inverter status.')
    if state.get('protocol_mismatch'):
        st.warning(f'Device reports {state["protocol_mismatch"]}; disconnect and select its matching protocol.')

    tabs_options = {'key': 'monitor_tab', 'on_change': 'rerun'} if STATEFUL_CONTAINERS else {}
    tabs = st.tabs(
        ['Live data', 'Gateway health', 'Manual settings', 'Raw JSON', 'Advanced diagnostics'] if native
        else ['Live data', 'Gateway health', 'All settings', 'Raw JSON'], **tabs_options)
    live_tab, health_tab, settings_tab, raw_tab = tabs[:4]
    with live_tab:
        if native:
            native_measurements(state)
        else:
            aohai_measurements(state)
    with health_tab:
        st.subheader('Gateway health')
        st.caption('Internet-facing ESP only, not a per-slave hardware-health report. Expected every 30 seconds.')
        health = state['health']
        if health.get('telemetry_seen') is False:
            st.warning('Gateway has not seen inverter telemetry.')
        table(health)
    with settings_tab:
        if native:
            st.caption('Live readings arrive automatically. These controls are only for a deliberate manual read or setting change.')
            with st.expander('Manual commands and settings', expanded=False):
                native_settings(state)
        else:
            settings(state)
    with raw_tab:
        if (not STATEFUL_CONTAINERS or raw_tab.open) and (
                STATEFUL_CONTAINERS or st.checkbox('Show raw JSON', key='show_raw_json')):
            if native:
                payload = state.get('native_raw_live')
                if payload:
                    st.caption('Latest received live JSON payload')
                    st.json(payload)
                else:
                    st.caption('Collected protocol records')
                    st.json({'protocol': state['protocol'], 'member_id': state['member_id'],
                             'records': records})
                if state.get('native_result'):
                    st.caption('Latest command result')
                    st.json(state['native_result'])
            else:
                st.json(state['live'])
    if native:
        diagnostics_tab = tabs[4]
        with diagnostics_tab:
            if STATEFUL_CONTAINERS and not diagnostics_tab.open:
                return
            options = {'key': 'native_diagnostics_open', 'on_change': 'rerun'} if STATEFUL_CONTAINERS else {}
            diagnostics = st.expander('Protocol records and delivery diagnostics', expanded=False, **options)
            with diagnostics:
                load = diagnostics.open if STATEFUL_CONTAINERS else st.checkbox(
                    'Load protocol diagnostics', key='load_native_diagnostics')
                if load:
                    native_diagnostics(state)
                    st.json({'protocol': state['protocol'], 'member_id': state['member_id'],
                             'records': records, 'result': state.get('native_result'),
                             'incomplete_records': state.get('native_incomplete', 0),
                             'dropped_records': state.get('native_drops', 0)})


def aohai_measurements(state):
    live = state['live']
    summary = live.get('power_summary', {})
    quality = live.get('power_quality', {})
    quality = quality if isinstance(quality, dict) else {}
    st.subheader('Power')
    if summary.get('sampling') == 'sequential_cached_readings':
        st.caption('Sequential cached readings: values were sampled at different times, so power totals may not balance within one message.')
    if 'freshness_limit_ms' in summary:
        st.caption(f'Gateway power-summary freshness limit: {live_value(summary["freshness_limit_ms"])} ms. This is separate from live-message receipt freshness.')
    rows = []
    for label, prefix in [('Grid', 'grid'), ('Inverter', 'inverter'), ('Load', 'load'), ('Output', 'output')]:
        # New firmware calls EPS output "load". Keep legacy output
        # readings only when explicitly present; never duplicate load.
        if prefix == 'output' and not any(k in summary for k in ('output_kw', 'output_apparent_kva')):
            continue
        apparent_key = prefix + '_apparent_kva'
        if apparent_key not in summary and prefix == 'load':
            apparent_key = 'load_kva'
        rows.append({'Circuit': label, 'Active · kW': power_value(summary, quality, prefix + '_kw'),
                     'Apparent · kVA': power_value(summary, quality, apparent_key)})
    table(rows)
    st.caption('Unavailable means missing or null in the payload, not zero. Active power is not inferred from apparent power.')
    metrics = {label: power_value(summary, quality, key) for label, key in (
        ('PV · kW', 'pv_kw'), ('Battery · kW (+ discharge / − charge)', 'battery_kw'),
        ('Load · %', 'load_percentage'))}
    for label, key in (('Power to user · kW', 'power_to_user_kw'), ('Local load · kW', 'local_load_kw'),
                       ('Reported rating · kW', 'rated_power_kw')):
        if key in summary:
            metrics[label] = power_value(summary, quality, key)
    table(metrics)
    if 'unverified' in str(summary.get('grid_direction', '')).casefold():
        st.warning('Grid active power uses an UNVERIFIED register interpretation. Do not treat it as confirmed grid import/export power.')
    if quality:
        st.subheader('Power reading quality')
        table([{'Metric': key, **flags} if isinstance(flags, dict) else {'Metric': key, 'Details': flags}
               for key, flags in quality.items()], all_rows=True)
        st.caption('Quality flags are reported by the gateway. since_valid_ms is its reported time since a valid reading, not a live timer. A valid flag does not verify the physical meaning of a register.')
    if 'rated_power_kw' in summary:
        st.caption('Rating source: ' + live_value(summary.get('rating_source'))
                   + '. A default rating is not a confirmed device rating.')
    if 'load_percentage_source' in summary or 'load_percentage_basis' in summary:
        st.caption('Load percentage source: ' + live_value(summary.get('load_percentage_source'))
                   + ' · Basis: ' + live_value(summary.get('load_percentage_basis')))
    if live.get('bms_configured') is True and live.get('bms_connected') is None:
        st.caption('BMS is configured; its connection status is unknown.')
    st.subheader('Operating status')
    table(live.get('operating_status', {}))
    sections = live.get('sections', {})
    if isinstance(sections, dict) and sections:
        count = sum(len(records) for records in sections.values() if isinstance(records, list))
        st.subheader('All measurements')
        st.caption(f'{count} measurements · all sections shown below')
        st.caption('age_ms is the reading age reported by the gateway at publication, in milliseconds; it is not a live-running timer.')
        stale_count = sum(1 for records in sections.values() if isinstance(records, list)
                          for record in records if isinstance(record, dict) and record.get('stale') is True)
        if stale_count:
            st.warning(f'{stale_count} readings are marked stale by the gateway. Their reported values remain visible below.')
        st.caption('stale is the gateway-provided flag. A blank flag means not reported, not confirmed fresh.')
        for section, records in sections.items():
            st.subheader(str(section).replace('_', ' ').title())
            table(records, all_rows=True)
    st.subheader('Payload metadata')
    table({k: v for k, v in live.items() if k not in ('sections', 'operating_status', 'power_summary', 'power_quality')}, all_rows=True)
    st.subheader('Complete power summary & sources')
    table(summary, all_rows=True)


def native_measurements(state):
    live = state.get('native_live', {})
    groups = live.get('groups', {})
    st.subheader('Live telemetry')
    st.caption('The ESP reads the selected device automatically. Live electrical readings refresh in the background; device information and settings may update less often.')
    if not live.get('telemetry_available'):
        st.info('Waiting for fresh PV, grid, battery or output measurements. Device mode and identity alone do not confirm live electrical data.')
    circuits = [('PV', 'pv', 'pv_kw'), ('Grid', 'grid', 'grid_kw'),
                ('Battery', 'battery', 'battery_kw'), ('Output', 'output', 'load_kw'),
                ('Load', 'load', 'load_percentage')]
    bms = live.get('device_type') == 'bms'
    if bms:
        circuits = [('Battery', 'battery', 'battery_kw')]
    summary = live.get('power_summary', {})
    quality = live.get('power_quality', {})
    if not bms:
        st.subheader('Power')
        table([{'Circuit': label,
                'Active · kW': native_power_value(summary, quality, prefix + '_kw'),
                'Apparent · kVA': native_power_value(summary, quality, prefix + '_apparent_kva')}
               for label, prefix in [('Grid', 'grid'), ('Load', 'load')]])
    battery_cards = [('Current', 'current'), ('State of charge', 'state_of_charge'),
                     ('Temperature', 'temperature'), ('Cell difference', 'cell_difference')] if bms else []
    cards = st.columns(len(circuits) + len(battery_cards))
    for card, (label, key, summary_key) in zip(cards, circuits):
        group = groups.get(key, {})
        fields = group.get('fields', {})
        value, unit = 'Unavailable', ''
        if summary_key in summary:
            value = native_power_value(summary, quality, summary_key, signed=summary_key == 'battery_kw')
            if value not in ('Unavailable', 'Stale'):
                unit = '%' if summary_key == 'load_percentage' else 'kW'
        elif fields and (bms or key in ('pv', 'load')) and not (
                key == 'pv' and live.get('pv_total_requires_channels')):
            field = group.get('primary') or {}
            if bms and key == 'battery' and field.get('unit') in ('W', 'kW'):
                field = next((fields[name] for name in ('battery_voltage', 'total_battery_voltage',
                             'total_voltage', 'voltage', 'v') if name in fields), {})
            value = 'Stale' if field.get('stale') else live_value(field.get('value')) if field.get('fresh') else 'Unavailable'
            unit = field.get('unit', '') if field.get('fresh') else ''
        card.metric(label, (value + ' ' + unit).rstrip())
    for card, (label, key) in zip(cards[len(circuits):], battery_cards):
        field = live.get('battery_metrics', {}).get(key, {})
        if key == 'current' and field.get('name') == 'balance_current':
            label = 'Balance current'
        value = 'Stale' if field.get('stale') else (
            live_value(field.get('value')) if field.get('fresh') else 'Unavailable')
        unit = field.get('unit', '') if field.get('fresh') else ''
        card.metric(label, (value + ' ' + unit).rstrip())
    st.caption('Battery power is in kW: + discharge / − charge. Unavailable means power or its direction is not known. Stale means the last reading has expired; neither means zero. Active and apparent power are shown separately.')
    status = groups.get('status', {}).get('fields', {})
    st.subheader('Operating status')
    if status:
        table(native_live_rows(status), all_rows=True)
        st.caption('Operating flags and protections are reported by the BMS; their update ages are shown above.' if bms
                   else 'The operating mode is reported by the inverter. Standby is an observed mode, not the MQTT connection state; its update age is shown above.')
    else:
        st.caption('Operating status unavailable: waiting for the device to report it.')
    for label, key, _ in circuits:
        st.subheader(label)
        fields = groups.get(key, {}).get('fields', {})
        if fields:
            table(native_live_rows(fields), all_rows=True)
        else:
            st.caption('Unavailable: not reported by this profile.' if groups.get(key, {}).get('supported') is False
                       else 'Waiting for measurements: this inverter has not reported these readings yet.')
    for key, label in [('energy', 'Energy totals'), ('other', 'Other measurements')]:
        fields = groups.get(key, {}).get('fields', {})
        if fields:
            st.subheader(label)
            table(native_live_rows(fields), all_rows=True)
    with st.expander('Device information and reported settings', expanded=False):
        for key, label in [('identity', 'Device information'), ('settings', 'Reported settings')]:
            st.subheader(label)
            fields = groups.get(key, {}).get('fields', {})
            if fields:
                table(native_live_rows(fields), all_rows=True)
            else:
                st.caption('Waiting for information from the inverter.')


def native_live_rows(fields):
    rows = []
    for name, field in fields.items():
        age = field.get('age_seconds')
        state = 'Stale' if field.get('stale') else 'Fresh' if field.get('fresh') else 'Unavailable'
        updated = state + (f' · {int(age)}s ago' if age is not None else '')
        rows.append({'Measurement': name.replace('_', ' ').capitalize(),
                     'Value': live_value(field.get('value')), 'Unit': field.get('unit', ''),
                     'Updated': updated})
    return rows


def native_diagnostics(state):
    st.caption('Protocol commands and packet details are diagnostics. Normal monitoring does not require selecting a command.')
    records = state.get('native_records', [])
    if not records:
        st.info('Waiting for a complete protocol telemetry record. The ESP polls the selected inverter automatically.')
    for record in records:
        st.subheader(record['command'])
        st.caption(f'{record["protocol"]} · Sequence {record["sequence"]} · Device timestamp {record["timestamp_ms"]} ms · '
                   + freshness(record['received_at'], state['connected_at'], 15))
        table([{'name': name, 'value': field.get('value'), 'unit': field.get('unit', '')}
               for name, field in record['fields'].items()], all_rows=True)
    if state.get('native_incomplete'):
        st.caption(f'{state["native_incomplete"]} partial telemetry records are awaiting their remaining chunks.')
    if state.get('native_drops'):
        st.caption(f'{state["native_drops"]} incomplete, expired or invalid telemetry records were discarded.')


def native_settings(state):
    st.subheader('Protocol commands and settings')
    profile = state['protocol']
    catalog = PROFILES[profile]
    entries, catalog_rows, command_labels = native_command_catalog(profile)
    st.caption('Queries read the device. Settings require explicit confirmation and are sent once. Values shown in command examples are examples, not current device settings.')
    if state.get('notice'):
        st.info(state['notice'])
    if state.get('native_write_blocked'):
        st.warning('A previous setting has an uncertain outcome. Further settings are blocked in this monitor session. Read the device and verify its state before reconnecting; queries remain available.')
    if not catalog['setters']:
        st.info('This profile has no documented settings commands in the firmware catalog.')
    with st.expander('All documented commands'):
        table(catalog_rows, all_rows=True)
    pending = state.get('native_pending')
    if pending:
        st.info(f'Waiting for command {pending["command_id"]}: {pending["command"]}. No automatic retry.')
    result = state.get('native_result')
    if result:
        outcome = result.get('outcome', 'unknown')
        if outcome == 'acknowledged':
            if result.get('setting_verified') is False:
                st.warning('Setting accepted, but its readback did not match the request. Check the inverter setting; no automatic retry.')
            else:
                st.success('Setting accepted by the device. An acknowledgement does not confirm that a measured value equals the requested value.')
        elif outcome == 'completed':
            st.success('Setting verified by a fresh device readback.' if result.get('setting_verified') is True
                       else 'Query completed. Its measurements appear in Live data when received.')
        elif outcome == 'uncertain':
            st.warning('Command outcome uncertain. It may have reached the device. Check the device before sending another setting; do not retry automatically.')
        elif outcome in ('rejected', 'unsupported', 'invalid_response'):
            st.error('Command result: ' + outcome.replace('_', ' '))
        else:
            st.info('Command result: ' + outcome.replace('_', ' '))
        table({key: result[key] for key in ('command_id', 'command', 'outcome', 'write_sent', 'setting_verified', 'detail') if key in result})
    if not entries:
        return
    disabled = not state['ready'] or bool(pending) or bool(state.get('protocol_mismatch'))
    selected = st.selectbox('Documented command', list(range(len(entries))),
                            format_func=lambda index: command_labels[index],
                            disabled=disabled, key='native_choice:' + profile)
    setter, entry = entries[selected]
    command_disabled = disabled or (setter and state.get('native_write_blocked', False))
    st.caption('Required format: ' + (entry.get('pattern') or entry['key']))
    if entry.get('minimum') is not None and entry.get('maximum') is not None:
        st.caption(f'Allowed value: {entry["minimum"]} to {entry["maximum"]} {entry.get("unit", "")}'.rstrip())
    command = st.text_input('Full protocol command', value=entry.get('example') or entry['key'],
                            disabled=command_disabled, key=f'native_command:{profile}:{selected}')
    try:
        command_readback = classify_command(profile, command).get('readback')
    except ValueError:
        command_readback = None
    if setter:
        st.caption(('The ESP may verify this setting with ' + command_readback + '. Only a matching fresh readback is shown as verified.')
                   if command_readback else 'No documented readback query for this command. An ACK alone is shown as accepted.')
    confirmed = False
    confirmation_revision_key = 'native_confirmation_revision:' + profile
    if setter:
        revision = st.session_state.get(confirmation_revision_key, 0)
        confirmed = st.checkbox('I have checked this setting command and confirm sending it to the inverter.',
                                disabled=command_disabled,
                                key=f'native_confirm:{profile}:{selected}:{revision}:{command}')
    if st.button('Send setting' if setter else 'Send query', type='primary', disabled=command_disabled,
                 key='native_send:' + profile):
        try:
            matched = classify_command(profile, command)
            if matched['key'] != entry['key'] or matched['setter'] != setter:
                raise ValueError('Enter a command matching the selected documented command.')
            if setter and not confirmed:
                raise ValueError('Confirm the setting before sending it to the inverter.')
            monitor.send_native(command, confirmed=confirmed)
            if setter:
                st.session_state[confirmation_revision_key] = revision + 1
            st.rerun()
        except ValueError as error:
            st.error(str(error))


def settings(state):
    st.subheader('All settings')
    st.caption(f'{len(state["cache"])} / {len(monitor.catalog)} values loaded · Reads rotate automatically.')
    if state['notice']:
        st.info(state['notice'])
    busy = bool(state['jobs'] or (state['pending'] and state['pending']['operation'] == 'set'))
    disabled = not state['ready'] or busy or state.get('aohai_write_blocked', False)
    if state.get('aohai_write_blocked', False):
        st.warning('Earlier setting outcome is uncertain. Check the inverter before reconnecting. Automatic reads continue; settings are blocked in this session.')
    with st.expander('System time'):
        values = {s.name: state['cache'].get(k) for k, s in monitor.catalog.items() if s.name in TIME_FIELDS}
        try:
            current = datetime(*(int(values[n]) for n in TIME_FIELDS))
        except (ValueError, TypeError, KeyError):
            current = None
        st.caption('Last read from inverter: ' + (str(current) if current else 'Waiting for all six fields'))
        with st.form('system_time'):
            calendar = st.date_input('Date', value=current.date() if current else None,
                                     min_value=date(2022, 1, 1), max_value=date(2050, 12, 31))
            clock = st.time_input('Time', value=current.time() if current else None, step=1)
            ack_time = st.checkbox('I confirm the guarded time change. Six fields are written sequentially; interruption may leave a partial change.')
            if st.form_submit_button('Set time', type='primary', disabled=disabled):
                try:
                    if calendar is None or clock is None:
                        raise ValueError('Choose both date and time.')
                    monitor.write_time(datetime.combine(calendar, clock), ack_time)
                    st.rerun()
                except ValueError as error:
                    st.error(str(error))
    query = st.text_input('Search settings', placeholder='Buzzer, Bluetooth, battery, priority…')
    categories = {'system': 'System', 'battery': 'Battery & charging', 'grid': 'Grid',
                  'eps': 'Output / EPS', 'generator': 'Generator', 'communications': 'Communications',
                  'parallel': 'Parallel operation', 'bms': 'BMS', 'identity': 'Device identity'}
    domains = list(dict.fromkeys(s.domain for s in monitor.catalog.values()))
    category = st.selectbox('Category', ['All'] + domains,
                            format_func=lambda d: categories.get(d, d), disabled=busy)
    query_text = query.casefold()
    groups = {}
    keys = []
    for key, item in monitor.catalog.items():
        if ((category == 'All' or item.domain == category)
                and query_text in f'{item.name} {item.description} {item.meaning}'.casefold()):
            keys.append(key)
            groups.setdefault(item.domain, []).append(key)
    st.caption('Physical changes appear after the next confirmed device read. Fresh ESP settings snapshots avoid duplicate reads. The selected setting is checked about every 30 seconds; other values are read when missing or older than ten minutes. Timeouts can delay refreshes.')
    st.caption('Click a settings row to open its editor below the tables. Nothing is written until you confirm and press Set.')
    now = time.monotonic()
    for domain in domains:
        group = groups.get(domain, [])
        if not group:
            continue
        st.markdown('#### ' + categories.get(domain, domain))
        table_key = f'settings_rows:{domain}:{query}'
        st.dataframe([{'Setting': monitor.catalog[k].description, 'Name': monitor.catalog[k].name,
                'Current': display_value(monitor.catalog[k], state['cache'].get(k)) +
                    (' (command register)' if monitor.catalog[k].name == 'OnOffSet' and k in state['cache'] else ''),
                'Read status': state['read_status'].get(k, 'Waiting for read' if monitor.catalog[k].readable else 'Not readable'),
                'Last confirmed': f'{int(now - state["value_at"][k])}s ago' if k in state['value_at'] else 'Not yet confirmed',
                'Requirement': monitor.catalog[k].guard} for k in group],
                hide_index=True, width='stretch', key=table_key, selection_mode='single-row',
                on_select=partial(select_setting_row, st.session_state, table_key, tuple(group), monitor))
    if not keys:
        monitor.set_focus(domain=None if category == 'All' else category)
        return
    if st.session_state.get('edit_setting') not in keys:
        st.session_state['edit_setting'] = keys[0]
    key = st.selectbox('Edit setting', keys, format_func=lambda k: f'{monitor.catalog[k].description} · {monitor.catalog[k].name}', disabled=busy, key='edit_setting')
    monitor.set_focus(key, None if category == 'All' else category)
    item = monitor.catalog[key]
    current = state['cache'].get(key)
    st.caption(f'Current: {display_value(item, current)} · Requirement: {item.guard}')
    if item.name == 'OnOffSet':
        st.caption('This current value is the on/off command register. Actual inverter On/Off is confirmed only after an operating-state check.')
    st.write(item.meaning)
    if item.blocked:
        st.warning(item.blocked)
        return
    if item.name in TIME_FIELDS:
        st.caption('Use the System time editor above to set all six fields together.')
        return
    with st.expander('Requirements and API details'):
        # Display the document as plain text, never execute its CLI examples.
        st.text(item.notes)
    # Change the widget identity only when the reported current value changes.
    # Periodic reruns therefore preserve an unsubmitted choice.
    version = f'{key}:{current!r}'
    if item.choices:
        options = list(item.choices)
        label = display_value(item, current)
        chosen = st.selectbox('Value', options, index=options.index(label) if label in options else None,
                              placeholder='Current value unknown — select a value', key='value:' + version, disabled=disabled)
    else:
        chosen = st.text_input('Value (API engineering units)', value='' if current is None else str(current),
                               key='value:' + version, disabled=disabled)
    acknowledged = st.checkbox('I have checked the requirements and confirm sending this value to the inverter.', key='ack:' + version)
    if st.button('Set', type='primary', disabled=disabled or chosen is None, key='set:' + key):
        try:
            monitor.write(key, chosen, acknowledged)
            st.rerun()
        except ValueError as error:
            st.error(str(error))


interface()
