"""Native firmware command catalog and complete MQTT measurement collection.

No broker or UI dependencies.  The caller must discard retained MQTT telemetry
and filter its selected protocol/member before calling ``receive``.  Firmware
measurements are unretained chunks: a final chunk alone is never a reading.

Catalog provenance and the upstream MIT permission notice are in
protocol_catalog.json.  An ACK means accepted, not verified by readback.
"""
from __future__ import annotations

import copy
import json
import math
import re
from collections import OrderedDict
from datetime import datetime
from pathlib import Path


_CATALOG = json.loads(Path(__file__).with_name('protocol_catalog.json').read_text(encoding='utf-8'))
PROTOCOL_IDS = tuple(_CATALOG['protocol_ids'])
PROFILES = _CATALOG['profiles']
CATALOG_SOURCE = _CATALOG['source']
_MATCHERS = {
    (profile, group, index): re.compile(entry['pattern'], flags=re.ASCII)
    for profile, data in PROFILES.items()
    for group in ('queries', 'setters')
    for index, entry in enumerate(data[group]) if entry['pattern'] is not None
}
_DEFINITIONS = {
    protocol: sorted(((group, index, entry)
                      for group in ('queries', 'setters')
                      for index, entry in enumerate(profile[group])),
                     key=lambda item: item[2]['order'])
    for protocol, profile in PROFILES.items()
}


def classify_command(protocol, command):
    """Validate a complete, case-sensitive firmware command or raise ValueError.

    Returns a catalog entry copy including ``setter`` and ``readback``.  AOHAI
    retains its register/semantic API and does not accept native commands.
    """
    if not isinstance(protocol, str) or protocol not in PROFILES or protocol == 'AOHAI_MODBUS_RTU':
        raise ValueError('Select a supported native protocol.')
    if (not isinstance(command, str) or not 1 <= len(command) <= 79
            or any(not 32 <= ord(char) <= 126 for char in command)):
        raise ValueError('Command must contain 1–79 printable ASCII characters.')
    # Match the compiled firmware definition order: all fixed commands first,
    # then patterns.  Inheritance can add overlapping command prefixes.
    for patterned in (False, True):
        for group, index, entry in _DEFINITIONS[protocol]:
            if (entry['pattern'] is not None) != patterned:
                continue
            matched = (_MATCHERS[(protocol, group, index)].fullmatch(command)
                       if patterned else command == entry['key'])
            if not matched:
                continue
            if group == 'setters':
                suffix = command[len(entry['key']):]
                flags = entry.get('flags', '')
                if flags and (not suffix or len(set(suffix)) != len(suffix)
                              or any(char not in flags for char in suffix)):
                    continue
                if entry['key'] == 'DAT' and not _valid_date(suffix):
                    continue
                if entry.get('numeric'):
                    try:
                        number = float(suffix)
                    except (ValueError, OverflowError):
                        continue
                    if (not math.isfinite(number) or not entry['minimum'] <= number <= entry['maximum']
                            or abs(number - math.floor(number + .5)) > .000001):
                        continue
            return dict(entry, setter=group == 'setters')
    raise ValueError(f'Unsupported command or parameter for {protocol}.')


def _valid_date(value):
    if len(value) not in (12, 14) or not value.isascii() or not value.isdecimal():
        return False
    text = '20' + value if len(value) == 12 else value
    try:
        datetime(int(text[:4]), int(text[4:6]), int(text[6:8]),
                 int(text[8:10]), int(text[10:12]), int(text[12:14]))
    except ValueError:
        return False
    return True


def _integer(value, low, high):
    return type(value) is int and low <= value <= high


def _text(value, maximum, *, empty=True, key=False):
    if not isinstance(value, str) or (not empty and not value):
        return False
    try:
        length = len(value.encode('utf-8'))
    except UnicodeEncodeError:
        return False
    if length > maximum or any(ord(char) < 32 or ord(char) == 127 for char in value):
        return False
    return not key or re.fullmatch(r'[a-z0-9_]+', value, flags=re.ASCII) is not None


def _validated_chunk(data):
    if not isinstance(data, dict):
        raise ValueError('Telemetry must be an object.')
    protocol, command = data.get('protocol'), data.get('command')
    if protocol not in PROFILES or protocol == 'AOHAI_MODBUS_RTU':
        raise ValueError('Unknown telemetry protocol.')
    if not _text(command, 15, empty=False) or not command.isascii():
        raise ValueError('Invalid telemetry command.')
    # A valid setter ACK does not contain measurements.  Query results are the
    # only native messages eligible to update the monitoring snapshot.
    wire_query = any(entry['pattern'] is None and command == entry['key'][:15]
                     for entry in PROFILES[protocol]['queries'])
    if not wire_query and classify_command(protocol, command)['setter']:
        raise ValueError('Setting ACK is not telemetry.')
    member = data.get('member_id')
    sequence, stamp = data.get('sequence'), data.get('timestamp_ms')
    atomic = data.get('record_complete') is True
    if 'record_complete' in data and not atomic:
        raise ValueError('Invalid complete record marker.')
    index, count = data.get('chunk_index'), data.get('chunk_count')
    if (not _integer(member, 0, 9) or not _integer(sequence, 0, 0xffffffff)
            or not _integer(stamp, 0, 0x7fffffffffffffff)):
        raise ValueError('Invalid telemetry metadata.')
    if atomic:
        if any(key in data for key in ('chunk_index', 'chunk_count', 'final_chunk')):
            raise ValueError('Complete records must not include chunk metadata.')
        if not _integer(data.get('sample_age_ms', 0), 0, 0x7fffffffffffffff):
            raise ValueError('Invalid record sample age.')
        index, count = 0, 1
    elif (not _integer(count, 1, 48) or not _integer(index, 0, count - 1)
          or type(data.get('final_chunk')) is not bool
          or data['final_chunk'] != (index == count - 1)):
        raise ValueError('Invalid telemetry metadata.')
    fields = data.get('fields')
    if not isinstance(fields, dict) or not 1 <= len(fields) <= (96 if atomic else 2):
        raise ValueError('Invalid telemetry field count.')
    normalized = {}
    for key, item in fields.items():
        if (not _text(key, 39, empty=False, key=True) or not isinstance(item, dict)
                or set(item) != {'value', 'unit'} or not _text(item['unit'], 7)):
            raise ValueError('Invalid telemetry field.')
        value = item['value']
        if isinstance(value, str):
            if not _text(value, 31):
                raise ValueError('Invalid telemetry text value.')
        elif type(value) not in (int, float):
            raise ValueError('Invalid telemetry numeric type.')
        else:
            try:
                finite = math.isfinite(value)
            except OverflowError:
                finite = False
            if not finite:
                raise ValueError('Non-finite telemetry value.')
        normalized[key] = {'unit': item['unit'], 'value': value}
    return {'member_id': member, 'protocol': protocol, 'command': command,
            'sequence': sequence, 'timestamp_ms': stamp, 'chunk_index': index,
            'chunk_count': count, 'fields': normalized, 'atomic': atomic,
            'sample_age_ms': data.get('sample_age_ms', 0) if atomic else 0}


def _newer_sequence(candidate, current):
    # RFC-style serial arithmetic handles a normal 32-bit sequence rollover.
    delta = (candidate - current) & 0xffffffff
    return 0 < delta < 0x80000000


class NativeTelemetryCollector:
    """Bounded native measurement assembler, with no partial snapshot updates.

    ``now`` is the caller's monotonic time in seconds.  Completed records are
    copied on return.  A repeated identical chunk is harmless; conflicting
    metadata/duplicate fields invalidate the entire assembly.  Older complete
    records never replace a newer reading for the same command.

    Firmware has no boot ID.  After an idle gap, two increasing, low-uptime
    records can establish a plausible reboot; old readings are then cleared.
    This conservative rule avoids treating a single reordered message as a
    reboot.  ``reset`` explicitly clears history on target/profile changes.
    """

    def __init__(self, max_assemblies=16, expiry_seconds=5.0, max_records=64,
                 reboot_idle_seconds=5.0):
        if (not _integer(max_assemblies, 1, 256) or not _integer(max_records, 1, 256)
                or type(expiry_seconds) not in (int, float) or not math.isfinite(expiry_seconds)
                or not 0 < expiry_seconds <= 120
                or type(reboot_idle_seconds) not in (int, float)
                or not math.isfinite(reboot_idle_seconds) or reboot_idle_seconds < 1):
            raise ValueError('Invalid collector limits.')
        self.max_assemblies = max_assemblies
        self.expiry_seconds = float(expiry_seconds)
        self.max_records = max_records
        self.reboot_idle_seconds = float(reboot_idle_seconds)
        self.reset()

    def reset(self, preserve_counters=False):
        self._assemblies = OrderedDict()
        self._completed = OrderedDict()
        self._invalid = OrderedDict()
        self._peer = None
        self._identity = None
        self._reboot_candidate = None
        self._reboot_candidate_at = None
        self._last_now = None
        if not preserve_counters:
            self._dropped_count = 0
            self._drop_reasons = {}
            self._duplicate_count = 0
            self._received_count = 0
            self._completed_count = 0

    @property
    def incomplete_count(self):
        return len(self._assemblies)

    @property
    def dropped_count(self):
        return self._dropped_count

    def diagnostics(self):
        return {'received_messages': self._received_count,
                'completed_records': self._completed_count,
                'duplicate_messages': self._duplicate_count,
                'dropped_records': self._dropped_count,
                'incomplete_records': self.incomplete_count,
                'drop_reasons': dict(self._drop_reasons)}

    def _drop(self, reason):
        self._dropped_count += 1
        self._drop_reasons[reason] = self._drop_reasons.get(reason, 0) + 1

    def records(self):
        return copy.deepcopy(list(self._completed.values()))

    def expire(self, now):
        self._validate_now(now)
        for key, assembly in list(self._assemblies.items()):
            if now - assembly['started'] >= self.expiry_seconds:
                del self._assemblies[key]
                self._drop('incomplete_record_expired')
        for key, deadline in list(self._invalid.items()):
            if now >= deadline:
                del self._invalid[key]
        if self._reboot_candidate and now - self._reboot_candidate_at > self.expiry_seconds:
            self._reboot_candidate = None
            self._reboot_candidate_at = None

    def _validate_now(self, now):
        if type(now) not in (int, float) or not math.isfinite(now):
            raise ValueError('now must be a finite monotonic time.')
        if self._last_now is not None and now < self._last_now:
            raise ValueError('Monotonic time moved backwards.')
        self._last_now = now

    def _invalidate(self, key, now, reason='conflicting_chunks'):
        self._assemblies.pop(key, None)
        self._invalid[key] = now + self.expiry_seconds
        while len(self._invalid) > self.max_assemblies:
            self._invalid.popitem(last=False)
        self._drop(reason)

    def receive(self, data, now):
        self.expire(now)
        self._received_count += 1
        try:
            chunk = _validated_chunk(data)
        except (ValueError, TypeError, KeyError) as error:
            # Error text is an internal bounded schema label, never payload data.
            self._drop(str(error) if isinstance(error, ValueError) else 'Invalid telemetry schema.')
            return None
        identity = (chunk['member_id'], chunk['protocol'])
        # A collector follows one selected member/profile.  Mixing peers must
        # be handled by separate collectors or an explicit reset by the caller.
        if self._identity is not None and identity != self._identity:
            self._drop('peer_mismatch')
            return None
        self._identity = identity
        key = (*identity, chunk['command'], chunk['sequence'])
        if key in self._invalid:
            return None
        previous = self._completed.get(chunk['command'])
        if previous is not None and chunk['sequence'] == previous['sequence'] and (
                chunk['timestamp_ms'] == previous['timestamp_ms']):
            if ((chunk['atomic'] and chunk['fields'] == previous['fields']) or
                    (not chunk['atomic'] and chunk['chunk_count'] == (len(previous['fields']) + 1) // 2
                     and all(previous['fields'].get(name) == field
                             for name, field in chunk['fields'].items()))):
                self._duplicate_count += 1
                return None
            self._invalidate(key, now, 'conflicting_complete_duplicate')
            return None
        if chunk['atomic']:
            self._assemblies.pop(key, None)
            record = {name: chunk[name] for name in ('member_id', 'protocol', 'command', 'sequence', 'timestamp_ms')}
            record.update(fields=chunk['fields'], received_at=now - chunk['sample_age_ms'] / 1000.0,
                          sample_age_ms=chunk['sample_age_ms'])
            return self._accept(record, identity, now)
        assembly = self._assemblies.get(key)
        if assembly is None:
            while len(self._assemblies) >= self.max_assemblies:
                self._assemblies.popitem(last=False)
                self._drop('assembly_capacity')
            assembly = {'started': now, 'timestamp_ms': chunk['timestamp_ms'],
                        'count': chunk['chunk_count'], 'chunks': {}}
            self._assemblies[key] = assembly
        elif (assembly['timestamp_ms'] != chunk['timestamp_ms']
              or assembly['count'] != chunk['chunk_count']):
            self._invalidate(key, now)
            return None
        chunks = assembly['chunks']
        old = chunks.get(chunk['chunk_index'])
        if old is not None:
            if old != chunk['fields']:
                self._invalidate(key, now)
            else:
                self._duplicate_count += 1
            return None
        keys = {name for fields in chunks.values() for name in fields}
        if keys.intersection(chunk['fields']) or len(keys) + len(chunk['fields']) > 96:
            self._invalidate(key, now)
            return None
        chunks[chunk['chunk_index']] = chunk['fields']
        if len(chunks) != assembly['count']:
            return None
        del self._assemblies[key]
        fields = {}
        for index in range(assembly['count']):
            fields.update(chunks[index])
        record = {key: chunk[key] for key in ('member_id', 'protocol', 'command', 'sequence', 'timestamp_ms')}
        record.update(fields=fields, received_at=now)
        return self._accept(record, identity, now)

    def _accept(self, record, identity, now):
        source_uptime = record['timestamp_ms'] + record.get('sample_age_ms', 0)
        if self._peer is None:
            self._peer = {'identity': identity, 'sequence': record['sequence'],
                          'timestamp_ms': record['timestamp_ms'],
                          'source_uptime_ms': source_uptime, 'received_at': now}
        else:
            peer = self._peer
            lower_uptime = record['timestamp_ms'] < peer['timestamp_ms']
            # Cached once-only identity can have an early timestamp and low
            # sequence in the current boot. Its sample age distinguishes it
            # from observations actually sent shortly after a restart.
            reboot = (lower_uptime and source_uptime <= 120000
                      and record['sequence'] <= 256
                      and peer['source_uptime_ms'] - source_uptime >= 5000)
            if reboot:
                candidate = self._reboot_candidate
                if candidate is not None:
                    elapsed = now - self._reboot_candidate_at
                    candidate_uptime = candidate['timestamp_ms'] + candidate.get('sample_age_ms', 0)
                    uptime_elapsed = (source_uptime - candidate_uptime) / 1000
                    if (_newer_sequence(record['sequence'], candidate['sequence'])
                            and .05 <= elapsed <= self.expiry_seconds
                            and .05 <= uptime_elapsed <= elapsed + 2):
                        self._assemblies.clear()
                        self._invalid.clear()
                        self._completed.clear()
                        self._store(candidate)
                        self._peer = {'identity': identity, 'sequence': record['sequence'],
                                      'timestamp_ms': record['timestamp_ms'],
                                      'source_uptime_ms': source_uptime, 'received_at': now}
                        self._reboot_candidate = None
                        self._reboot_candidate_at = None
                    else:
                        self._drop('implausible_reboot_record')
                        return None
                elif now - peer['received_at'] >= self.reboot_idle_seconds:
                    self._reboot_candidate = record
                    self._reboot_candidate_at = now
                    return None
                else:
                    self._drop('old_record_before_reboot_idle')
                    return None
            elif _newer_sequence(record['sequence'], peer['sequence']) and not lower_uptime:
                self._peer.update(sequence=record['sequence'], timestamp_ms=record['timestamp_ms'],
                                  source_uptime_ms=source_uptime, received_at=now)
                self._reboot_candidate = None
                self._reboot_candidate_at = None
        previous = self._completed.get(record['command'])
        if previous is not None and (record['timestamp_ms'] < previous['timestamp_ms']
                                      or not _newer_sequence(record['sequence'], previous['sequence'])):
            self._drop('out_of_order_record')
            return None
        self._store(record)
        return copy.deepcopy(record)

    def _store(self, record):
        self._completed_count += 1
        self._completed[record['command']] = record
        self._completed.move_to_end(record['command'])
        while len(self._completed) > self.max_records:
            self._completed.popitem(last=False)
