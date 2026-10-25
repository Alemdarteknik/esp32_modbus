"""Freshness-aware fallback reads; firmware snapshots remain the main source.

Only successful, nonretained device observations refresh these timers.  A
retained value is useful for display but cannot suppress a validating getter.
Explicit editor reads and setting writes bypass this background policy.
"""

from settings_results import value_available

SETTINGS_REFRESH_SECONDS = 600.0
CONTROL_REFRESH_SECONDS = 120.0
FOCUSED_REFRESH_SECONDS = 30.0
READ_RETRY_SECONDS = 30.0
MAX_READ_RETRY_SECONDS = 300.0


def valid_setting_read(item, value):
    """Validate a read's shape, without applying writable ranges or enum lists.

    Unknown numeric enum codes and model-specific values remain observable.
    ASCII registers can arrive as two printable characters or packed uint16.
    """
    if item.value_type.startswith('ascii'):
        if type(value) is str:
            return len(value) == 2 and value.isascii() and value.isprintable()
        return (type(value) in (int, float) and value_available(value)
                and 0 <= value <= 65535 and int(value) == value)
    if type(value) is bool:
        return any(type(choice) is bool for choice in item.choices.values())
    return type(value) in (int, float) and value_available(value)


class SettingsReadPolicy:
    def __init__(self, last_attempt=None, *, refresh_seconds=SETTINGS_REFRESH_SECONDS):
        self.last_attempt = {} if last_attempt is None else last_attempt
        self.refresh_seconds = refresh_seconds
        self.fresh_at = {}
        self.retry_at = {}
        self.failures = {}

    def reset(self):
        self.last_attempt.clear()
        self.fresh_at.clear()
        self.retry_at.clear()
        self.failures.clear()

    def attempted(self, key, now):
        self.last_attempt[key] = now

    def received(self, key, now):
        self.fresh_at[key] = now
        self.retry_at.pop(key, None)
        self.failures.pop(key, None)

    def failed(self, key, now):
        failures = min(self.failures.get(key, 0) + 1, 5)
        self.failures[key] = failures
        self.retry_at[key] = now + min(
            READ_RETRY_SECONDS * 2 ** (failures - 1), MAX_READ_RETRY_SECONDS)

    def due(self, key, now, *, focused=False, force=False):
        if force:
            return True
        if key in self.retry_at:
            return now >= self.retry_at[key]
        latest = max(self.fresh_at.get(key, float('-inf')),
                     self.last_attempt.get(key, float('-inf')))
        interval = FOCUSED_REFRESH_SECONDS if focused else self.refresh_seconds
        return now - latest >= interval


def settings_index(catalog):
    """Index exact semantic identities, including duplicate AOHAI names."""
    result = {}
    for item in catalog.values():
        result.setdefault((item.name, item.domain.casefold()), []).append(item)
    return result


def snapshot_setting(index, data):
    name = data.get('name')
    if not isinstance(name, str):
        return None
    matches = index.get((name, str(data.get('domain', '')).casefold()), ())
    if len(matches) > 1:
        matches = [item for item in matches if item.description == data.get('description')]
    return matches[0] if len(matches) == 1 else None
