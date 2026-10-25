"""Named setting result semantics, shared by the browser and desktop editors."""
import math


def value_available(value):
    return (type(value) in (str, bool) or (type(value) is int and abs(value) <= 2 ** 53) or
            (type(value) is float and math.isfinite(value)))


def values_equal(actual, expected):
    if type(actual) in (bool, int, float) and type(expected) in (bool, int, float):
        return math.isfinite(actual) and math.isfinite(expected) and math.isclose(
            actual, expected, rel_tol=1e-9, abs_tol=1e-9)
    return type(actual) is type(expected) and actual == expected


def failure_details(data):
    details = [str(data[key]) for key in ('message', 'detail') if data.get(key)]
    for key in ('modbus_status', 'modbus_exception'):
        if key in data and data[key] is not None:
            details.append(f'{key}={data[key]}')
    return ' '.join(details)


def normalize_setting_value(item, value):
    if item.name == 'Year' and type(value) in (int, float) and 0 <= value <= 99 and float(value).is_integer():
        return int(value) + 2000
    return value


def named_write_result(item, expected, data):
    """Do not treat an on/off command latch as proof of operating state.

    Firmware may report H0 reset after a command; fresh R0 verification and the
    correlated command ID establish success independently of that latch value.
    """
    outcome = data.get('outcome', 'invalid result')
    actual = normalize_setting_value(item, data.get('value'))
    if item.value_type == 'ascii_or_uint16' and type(expected) is str and len(expected) == 2:
        expected = (ord(expected[0]) << 8) | ord(expected[1])
    available = value_available(actual)
    on_off = item.name == 'OnOffSet'
    confirmed = ((outcome == 'completed' and data.get('operating_state_confirmed') is True)
                 if on_off else (outcome == 'confirmed' and available and values_equal(actual, expected)))
    uncertain = not confirmed and (
        (outcome == 'communication_error' and data.get('write_sent') is not False)
        or outcome in ('invalid_response', 'uncertain', 'transition_pending', 'sent_unverifiable')
        or ('write_sent' in data and data['write_sent'] is None)
        or (on_off and outcome == 'confirmed'))
    if confirmed:
        message = f'Confirmed: inverter {"On" if expected == 1 else "Off"} (operating state verified).' if on_off else ''
    elif on_off and outcome == 'confirmed':
        message = 'Command register read back; inverter operating state was not verified. Update the ESP firmware and check the inverter. No automatic retry.'
    elif outcome == 'transition_pending':
        message = 'On/off request sent; requested operating state is not yet verified. Check the inverter. No automatic retry.'
    elif outcome == 'sent_unverifiable':
        message = 'Sent but not verifiable: inverter action has no read-back. No automatic retry.'
    elif outcome == 'confirmed' and available:
        message = f'Not confirmed: Read-back mismatch: {actual}; requested {expected}. No automatic retry.'
    else:
        message = f'Not confirmed: {outcome}. No automatic retry.'
    detail = failure_details(data)
    if detail:
        message += ' ' + detail
    return dict(confirmed=confirmed, uncertain=uncertain, message=message,
                value=actual, value_available=available)


def _write_state(monitor):
    return monitor.__dict__.setdefault('_named_write_state', dict(owner=None, blocked=False))


def write_problem(monitor, owner=None):
    state = _write_state(monitor)
    if state['blocked']:
        return 'Earlier setting outcome is uncertain. Check the inverter before reconnecting; reads remain available.'
    if state['owner'] is not None and state['owner'] is not owner:
        return 'Wait for the current setting write to finish.'
    return ''


def begin_write(monitor, owner):
    problem = write_problem(monitor, owner)
    if problem:
        raise ValueError(problem)
    _write_state(monitor)['owner'] = owner


def finish_write(monitor, owner, uncertain=False):
    state = _write_state(monitor)
    if state['owner'] is owner:
        state['owner'] = None
    if uncertain:
        state['blocked'] = True


def reset_write_session(monitor):
    monitor.__dict__['_named_write_state'] = dict(owner=None, blocked=False)
