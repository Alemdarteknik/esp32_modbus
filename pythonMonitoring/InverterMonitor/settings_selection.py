"""Map a clicked table row to its exact catalog key, without sending commands."""
def select_setting_row(session_state, table_key, row_keys, monitor):
    state = monitor.snapshot()
    if state['jobs'] or (state['pending'] and state['pending']['operation'] == 'set'):
        return
    rows = session_state.get(table_key, {}).get('selection', {}).get('rows', [])
    if len(rows) == 1 and isinstance(rows[0], int) and 0 <= rows[0] < len(row_keys):
        session_state['edit_setting'] = row_keys[rows[0]]


def setting_member_matches(monitor, data):
    """Correlate a legacy setting reply with the member in its MQTT base topic."""
    if not isinstance(data, dict) or type(data.get('member_id')) is not int:
        return False
    topic = getattr(monitor, 'mqtt_topic', '')
    if not isinstance(topic, str):
        return False
    try:
        member = int(topic.rsplit('/', 2)[-2])
    except (ValueError, IndexError):
        return False
    return 0 <= member <= 9 and data['member_id'] == member
