"""Session-owned MQTT transport. No UI imports; no automatic write retries."""
import copy
import json
import re
import threading
import time
from datetime import datetime

import paho.mqtt.client as mqtt
from settings_catalog import INTERLOCKS, load_catalog
from mqtt_connection import BrokerConfig, MQTT_USER_AGENT, websocket_failure
from native_protocols import PROTOCOL_IDS, NativeTelemetryCollector, classify_command
from native_live import normalize_native_live, native_measurement_received_at
from settings_results import named_write_result, normalize_setting_value
from settings_polling import (SettingsReadPolicy, settings_index, snapshot_setting,
                              valid_setting_read)

TIME_FIELDS = ('Year', 'Month', 'Day', 'Hour', 'Minute', 'Second')


def display_value(item, value):
    if value is None:
        return 'Unknown'
    for label, code in item.choices.items():
        if code == value:
            return label
    return str(value)


class MonitorSession:
    def __init__(self):
        self.catalog = load_catalog()
        self._settings_index = settings_index(self.catalog)
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.client = None
        self.active = False
        self.ready = False
        self.status = 'Disconnected'
        self.connection_error = False
        self.broker = BrokerConfig()
        self.notice = ''
        self.live = {}
        self.health = {}
        self.cache = {}
        self.last_read = {}
        self.read_policy = SettingsReadPolicy(self.last_read)
        self.value_at = {}
        self.read_status = {}
        self.focus_key = None
        self.focus_domain = None
        self._focus_refresh = False
        self.priority_turn = True
        self.unreadable = set()
        self.pending = None
        self.jobs = []
        self.time_writing = False
        self.last_sent = 0
        self.live_at = self.health_at = None
        self.connected_at = None
        self.heartbeat = time.monotonic()
        self.protocol = 'AOHAI_MODBUS_RTU'
        self.member_id = 0
        self.protocol_mismatch = ''
        self.native_telemetry = NativeTelemetryCollector()
        self.native_connected_pv_inputs = None
        self.native_raw_live = {}
        self.native_pending = None
        self.native_result = {}
        self.native_write_blocked = False
        self.aohai_write_blocked = False

    def select_protocol(self, protocol):
        with self.lock:
            if self.active:
                raise ValueError('Disconnect before changing the protocol.')
            if protocol not in PROTOCOL_IDS:
                raise ValueError('Select a supported inverter or BMS protocol.')
            self.protocol = protocol
            self.protocol_mismatch = ''
            self.native_telemetry.reset()
            self.native_raw_live = {}
            self.native_connected_pv_inputs = None
            self.native_pending = None
            self.native_result = {}
            self.native_write_blocked = False
            self.aohai_write_blocked = False
            self.live.clear()
            self.live_at = None
            self.cache.clear()
            self.value_at.clear()
            self.read_status.clear()
            self.read_policy.reset()
            self.unreadable.clear()
            self.time_writing = False
            self.pending = None
            self.jobs.clear()

    def connect(self, device_id, protocol=None, member_id=0):
        device_id = device_id.strip().upper().removeprefix('INV-')
        if not re.fullmatch('[0-9A-F]{12}', device_id):
            raise ValueError('Enter a 12-character hexadecimal inverter ID.')
        with self.lock:
            if self.active:
                return
            if type(member_id) is not int or not 0 <= member_id <= 9:
                raise ValueError('Logical member must be an integer from 0 to 9.')
            self.broker = BrokerConfig.from_env()
            if protocol is not None:
                self.select_protocol(protocol)
            self.member_id = member_id
            self.protocol_mismatch = ''
            self.native_telemetry.reset()
            self.native_raw_live = {}
            self.native_connected_pv_inputs = None
            self.native_pending = None
            self.native_result = {}
            self.read_policy.reset()
            self.base = f'inverter/INV-{device_id}/inverter/{member_id}'
            self.stop.clear()
            self.heartbeat = time.monotonic()
            self.client = mqtt.Client(client_id='', transport=self.broker.transport, protocol=mqtt.MQTTv311)
            if self.broker.transport == 'websockets':
                self.client.ws_set_options(path=self.broker.path, headers={'User-Agent': MQTT_USER_AGENT})
            self.client.tls_set()
            self.client.on_connect = self.on_connect
            self.client.on_subscribe = self.on_subscribe
            self.client.on_message = self.on_message
            self.client.on_disconnect = self.on_disconnect
            self.active = True
            self.connection_error = False
            self.status = 'Connecting…'
            threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        try:
            self.client.connect(self.broker.host, self.broker.port, keepalive=30)
            while not self.stop.is_set():
                if self.client.loop(timeout=0.2) != mqtt.MQTT_ERR_SUCCESS:
                    raise RuntimeError('MQTT connection lost. Reconnect manually; writes are not retried.')
                with self.lock:
                    if time.monotonic() - self.heartbeat > 90:
                        raise RuntimeError('Browser inactive for 90 seconds; connection closed.')
                    self.tick()
        except Exception as error:
            detail = (websocket_failure(self.broker, error)
                      if isinstance(error, mqtt.WebsocketConnectionError)
                      else f'MQTT connection failed at {self.broker.endpoint}: {error}')
            with self.lock:
                self.status = detail
                self.connection_error = True
        finally:
            try:
                self.client.disconnect()
            except Exception:
                pass  # Connection state and pending commands must still be cleared.
            with self.lock:
                self.active = self.ready = False
                self._abandon_named('Connection closed before a matching device result.')
                self._abandon_native('Connection closed before a matching device result.')
                self.pending = None
                self.jobs.clear()
                self.live.clear()
                self.health.clear()
                self.cache.clear()
                self.value_at.clear()
                self.read_status.clear()
                self.live_at = self.health_at = None
                self.native_telemetry.reset(preserve_counters=True)
                self.native_raw_live = {}
                self.native_connected_pv_inputs = None

    def close(self):
        with self.lock:
            self.stop.set()
            self.ready = False
            self.status = 'Disconnected'
            self.connection_error = False
            self._abandon_named('Disconnected before a matching device result.')
            self.jobs.clear()
            self._abandon_native('Disconnected before a matching device result.')
            self.native_telemetry.reset(preserve_counters=True)
            self.native_raw_live = {}
            self.native_connected_pv_inputs = None
            self.live_at = None

    def _abandon_named(self, detail):
        if self.pending and self.pending['operation'] == 'set':
            self.aohai_write_blocked = True
            self.notice = 'Setting outcome uncertain. ' + detail + ' Check the inverter before reconnecting; no retry.'
        elif self.time_writing:
            self.aohai_write_blocked = True
            self.notice = 'Time change interrupted; some fields may have changed. Check inverter time before reconnecting; no retry.'
        self.pending = None
        self.jobs.clear()
        self.time_writing = False

    def _abandon_native(self, detail):
        if self.native_pending and self.native_pending['write']:
            self.native_write_blocked = True
            self.native_result = dict(command_id=self.native_pending['command_id'],
                member_id=self.member_id, operation='protocol', protocol=self.protocol,
                command=self.native_pending['command'], outcome='uncertain',
                write_sent=None, detail=detail)
            self.notice = 'Setting outcome uncertain. Check the device before further settings; no retry.'
        self.native_pending = None

    def on_connect(self, client, userdata, flags, rc):
        with self.lock:
            if rc != 0:
                self.status = f'MQTT connection rejected ({rc})'
                self.connection_error = True
                self.stop.set()
                return
            self.status = 'Connected'
            self.connected_at = time.monotonic()
            self.read_policy.reset()
            # Start a new broker subscription epoch. Historical complete or
            # partial observations must not become fresh after reconnection.
            self.native_telemetry.reset(preserve_counters=True)
            self.native_raw_live = {}
            self.native_connected_pv_inputs = None
            self.live_at = None
            suffixes = ('/live', '/health', '/result',
                        '/snapshot/#' if self.protocol == 'AOHAI_MODBUS_RTU' else '/protocol/#')
            client.subscribe([(self.health_topic if suffix == '/health' else self.base + suffix,
                               0 if suffix == '/health' or (self.protocol == 'AOHAI_MODBUS_RTU'
                                    and suffix in ('/live', '/protocol/#')) else 1)
                              for suffix in suffixes])

    @property
    def health_topic(self):
        return self.base.rsplit('/', 1)[0] + '/0/health'

    def on_subscribe(self, client, userdata, mid, codes):
        with self.lock:
            self.ready = bool(codes) and all(code < 128 for code in codes)
            if not self.ready:
                self.status = 'Subscription rejected'

    def on_disconnect(self, client, userdata, rc):
        with self.lock:
            self.ready = False
            self._abandon_named('Broker disconnected before a matching device result.')
            self._abandon_native('Broker disconnected before a matching device result.')
            self.native_telemetry.reset(preserve_counters=True)
            self.native_raw_live = {}
            self.native_connected_pv_inputs = None
            self.live_at = None

    def on_message(self, client, userdata, message):
        try:
            data = json.loads(message.payload)
            if not isinstance(data, dict):
                return
            with self.lock:
                self.receive(message.topic, data, message.retain)
        except (ValueError, TypeError, UnicodeError):
            return

    def receive(self, topic, data, retained=False):
        now = time.monotonic()
        if topic == self.base + '/live':
            if self.protocol != 'AOHAI_MODBUS_RTU':
                if retained or type(data.get('member_id')) is not int or data['member_id'] != self.member_id:
                    return
                actual_protocol = data.get('protocol')
                if actual_protocol in PROTOCOL_IDS and actual_protocol != self.protocol:
                    self.protocol_mismatch = actual_protocol
                    self.notice = f'Device reports {actual_protocol}; disconnect and select its matching protocol.'
                    return
                if actual_protocol != self.protocol:
                    return
                records = data.get('native_records')
                if (not isinstance(records, list) or not 1 <= len(records) <= 64
                        or any(not isinstance(record, dict) or record.get('record_complete') is not True
                               or type(record.get('member_id')) is not int
                               or record['member_id'] != self.member_id
                               or record.get('protocol') != self.protocol for record in records)):
                    return
                # Every record carries its own sample age and complete fields;
                # cached publication never refreshes an existing observation.
                for record in records:
                    self.native_telemetry.receive_view(record, now)
                self.native_raw_live = data
                self._receive_native_pv_inputs(data)
                self.live_at = native_measurement_received_at(
                    self.native_telemetry.records_view(), self.protocol, self.member_id)
                return
            self.live = data
            if not retained:
                self.live_at = now
            return
        if topic == self.health_topic:
            self.health = data
            if not retained:
                self.health_at = now
            return
        if type(data.get('member_id')) is not int or data.get('member_id') != self.member_id:
            return
        if topic == self.base + '/protocol' or topic.startswith(self.base + '/protocol/'):
            if retained:
                return
            actual_protocol = data.get('protocol')
            if actual_protocol in PROTOCOL_IDS and actual_protocol != self.protocol:
                self.protocol_mismatch = actual_protocol
                self.notice = f'Device reports {actual_protocol}; disconnect and select its matching protocol.'
                return
            if self.protocol == 'AOHAI_MODBUS_RTU' or actual_protocol != self.protocol:
                return
            suffix = topic[len(self.base):]
            command = data.get('command')
            if suffix != '/protocol' and (not isinstance(command, str) or suffix != '/protocol/' + command):
                return
            completed = self.native_telemetry.receive_view(data, now)
            if completed is not None:
                self._receive_native_pv_inputs(data)
                # Mode, identity and settings queries have separate freshness;
                # they cannot make missing PV/grid/battery sensors look live.
                self.live_at = native_measurement_received_at(
                    self.native_telemetry.records_view(), self.protocol, self.member_id)
            return
        if self.protocol != 'AOHAI_MODBUS_RTU':
            if topic == self.base + '/result' and not retained:
                self._receive_native_result(data)
            return
        if topic.startswith(self.base + '/snapshot/'):
            item = snapshot_setting(self._settings_index, data)
            if item is not None and 'value' in data:
                key = item.key
                value = normalize_setting_value(item, data['value'])
                if not valid_setting_read(item, value):
                    return
                # A retained snapshot is a starting value, not a fresh device
                # read, and must not overwrite a subsequently confirmed read.
                if not retained or (key not in self.value_at and key not in self.read_policy.fresh_at):
                    self.cache[key] = value
                    self.read_status[key] = 'Cached snapshot' if retained else 'Snapshot received'
                    if not retained:
                        self.read_policy.received(key, now)
            return
        pending = self.pending
        if topic != self.base + '/result' or not pending or retained:
            return
        item = self.catalog[pending['key']]
        if (str(data.get('command_id')) != pending['command_id']
                or data.get('operation') != pending['operation'] or data.get('name') != item.name
                or ('domain' in data and str(data['domain']).casefold() != item.domain.casefold())
                or ('description' in data and data['description'] != item.description)):
            return
        self.pending = None
        outcome = data.get('outcome', 'invalid result')
        actual = normalize_setting_value(item, data.get('value'))
        valid = outcome == 'confirmed' and valid_setting_read(item, actual)
        if valid:
            self.cache[item.key] = actual
            self.value_at[item.key] = now
            self.read_status[item.key] = 'Read confirmed'
            self.read_policy.received(item.key, now)
        elif outcome in ('not_readable', 'not_documented'):
            self.unreadable.add(item.key)
            self.read_status[item.key] = 'Not readable'
        else:
            self.read_status[item.key] = f'Read failed: {outcome}'
            if pending['operation'] == 'get':
                self.read_policy.failed(item.key, now)
        if pending['operation'] == 'set':
            expected = pending['value']
            result = named_write_result(item, expected, data)
            if result['value_available']:
                self.cache[item.key] = result['value']
            if not result['confirmed']:
                self.jobs.clear()
                self.time_writing = False
                self.aohai_write_blocked |= result['uncertain']
                self.notice = result['message']
                self.read_status[item.key] = ('Operating state not verified' if item.name == 'OnOffSet' else f'Write not confirmed: {outcome}')
            else:
                self.notice = result['message'] or f'Confirmed: {item.description} = {display_value(item, actual)}'
                self.read_status[item.key] = 'Operating state verified' if item.name == 'OnOffSet' else 'Read confirmed'
                if self.time_writing and not self.jobs:
                    self.time_writing = False
                    self.notice = 'All six time fields confirmed. Reading inverter time…'
                    self.jobs = [('get', next(k for k, s in self.catalog.items() if s.name == name), None)
                                 for name in TIME_FIELDS]

    def send(self, operation, key, value=None):
        if self.protocol != 'AOHAI_MODBUS_RTU' or self.protocol_mismatch:
            raise ValueError('AOHAI catalog commands require a matching AOHAI device.')
        item = self.catalog[key]
        cid = str(time.time_ns())
        payload = dict(command_id=cid, operation=operation, name=item.name,
                       domain=item.domain, description=item.description)
        if operation == 'get':
            payload['category'] = 'configuration'
        else:
            if self.aohai_write_blocked:
                raise ValueError('Earlier setting outcome is uncertain. Check the inverter before reconnecting; reads remain available.')
            payload.update(value=value, confirmed=True)
            if item.guard in INTERLOCKS:
                payload[INTERLOCKS[item.guard]] = True
        encoded = json.dumps(payload)
        if len(encoded.encode()) > 2048:
            raise ValueError('Command exceeds gateway limit.')
        # QoS 0 avoids transport redelivery of a control command. A matching
        # inverter result, not MQTT delivery, is the only write confirmation.
        result = self.client.publish(self.base + '/command', encoded, qos=0, retain=False)
        self.last_sent = time.monotonic()
        if operation == 'get':
            self.read_policy.attempted(key, self.last_sent)
            if key == self.focus_key:
                self._focus_refresh = False
        if result.rc != 0:
            if operation == 'get':
                self.read_policy.failed(key, self.last_sent)
            self.jobs.clear()
            self.time_writing = False
            self.notice = 'Publish failed. No automatic write retry.'
            return
        self.pending = dict(command_id=cid, operation=operation, key=key,
                            value=value, deadline=self.last_sent + (30 if operation == 'set' else 10))

    def tick(self):
        now = time.monotonic()
        if self.protocol != 'AOHAI_MODBUS_RTU':
            self.native_telemetry.expire(now)
            if self.native_pending and now > self.native_pending['deadline']:
                pending = self.native_pending
                if pending['write']:
                    self._abandon_native('No matching device result before the timeout.')
                else:
                    self.native_result = dict(command_id=pending['command_id'], member_id=self.member_id,
                        operation='protocol', protocol=self.protocol, command=pending['command'],
                        outcome='uncertain', detail='Query timed out; no automatic retry.', write_sent=False)
                    self.native_pending = None
                    self.notice = 'Query timed out; no automatic retry.'
            return
        if self.pending and now > self.pending['deadline']:
            self.read_status[self.pending['key']] = 'Timed out; awaiting refresh'
            if self.pending['operation'] == 'set':
                self.aohai_write_blocked = True
                self.cache.pop(self.pending['key'], None)
                self.jobs.clear()
                self.time_writing = False
                self.notice = 'Write timed out; not confirmed. Time changes may be partial. No retry.'
            else:
                self.read_policy.failed(self.pending['key'], now)
            self.pending = None
        if not self.ready or self.protocol_mismatch or self.pending or now - self.last_sent < 1:
            return
        if self.jobs:
            self.send(*self.jobs.pop(0))
        else:
            keys = [k for k, s in self.catalog.items() if s.readable and k not in self.unreadable]
            if keys:
                key = self.next_read(keys, now)
                if key is not None:
                    self.send('get', key)

    def set_focus(self, key=None, domain=None):
        with self.lock:
            if key != self.focus_key:
                self._focus_refresh = key is not None
            self.focus_key = key
            self.focus_domain = domain

    def next_read(self, keys, now):
        """Read stale/missing values, alternating focus and global fallback slots."""
        keys = [key for key in keys if self.read_policy.due(key, now,
                focused=key == self.focus_key,
                force=key == self.focus_key and self._focus_refresh)]
        if not keys:
            return None
        candidates = keys
        if self.priority_turn:
            if self.focus_key in keys:
                candidates = [self.focus_key]
            elif self.focus_domain:
                candidates = [k for k in keys if self.catalog[k].domain == self.focus_domain] or keys
        self.priority_turn = not self.priority_turn
        return min(candidates, key=lambda k: self.last_read.get(k, 0))

    def write(self, key, text, acknowledged):
        with self.lock:
            if self.protocol != 'AOHAI_MODBUS_RTU' or self.protocol_mismatch:
                raise ValueError('AOHAI settings require a matching AOHAI device.')
            if not self.ready or self.jobs or (self.pending and self.pending['operation'] == 'set'):
                raise ValueError('Connect and wait for the previous write to finish.')
            if self.aohai_write_blocked:
                raise ValueError('Earlier setting outcome is uncertain. Check the inverter before reconnecting; reads remain available.')
            item = self.catalog[key]
            if item.name in TIME_FIELDS:
                raise ValueError('Use the system time editor.')
            if acknowledged is not True:
                raise ValueError('Acknowledge the requirements before sending.')
            value = item.parse(text)
            self.pending = None  # Supersede an automatic read, never a write.
            self.jobs = [('set', key, value)]
            self.notice = f'Queued: {item.description}. Waiting for inverter confirmation…'

    def write_time(self, stamp, acknowledged):
        with self.lock:
            if self.protocol != 'AOHAI_MODBUS_RTU' or self.protocol_mismatch:
                raise ValueError('The six-field time editor is for AOHAI devices.')
            if acknowledged is not True or not self.ready or self.jobs or (self.pending and self.pending['operation'] == 'set'):
                raise ValueError('Acknowledge the time change, connect, and wait for any pending write.')
            if self.aohai_write_blocked:
                raise ValueError('Earlier setting outcome is uncertain. Check the inverter before reconnecting; reads remain available.')
            if not isinstance(stamp, datetime) or not 2022 <= stamp.year <= 2050:
                raise ValueError('Year must be between 2022 and 2050.')
            values = (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second)
            keys = [next(k for k, s in self.catalog.items() if s.name == name) for name in TIME_FIELDS]
            self.pending = None
            self.jobs = [('set', k, v) for k, v in zip(keys, values)]
            self.time_writing = True
            self.notice = 'Writing six calendar fields sequentially; seconds last. Changes may be partial if interrupted.'

    def send_native(self, command, confirmed=False):
        with self.lock:
            if self.protocol == 'AOHAI_MODBUS_RTU':
                raise ValueError('Use the AOHAI catalog for this device.')
            if not self.ready or self.client is None:
                raise ValueError('Connect and wait for the MQTT subscription.')
            if self.protocol_mismatch:
                raise ValueError('Select the protocol reported by the device.')
            if self.native_pending:
                raise ValueError('Wait for the current command result before sending another command.')
            definition = classify_command(self.protocol, command)
            write = definition['setter']
            if write and self.native_write_blocked:
                raise ValueError('Earlier setting outcome uncertain. Check the device and reconnect before more settings.')
            if write and confirmed is not True:
                raise ValueError('Confirm sending this setting to the device.')
            cid = str(time.time_ns())
            payload = dict(command_id=cid, operation='protocol', protocol=self.protocol, command=command)
            if write:
                payload['confirmed'] = True
            encoded = json.dumps(payload)
            if len(encoded.encode('utf-8')) > 2048:
                raise ValueError('Command exceeds gateway limit.')
            result = self.client.publish(self.base + '/command', encoded, qos=0, retain=False)
            if result.rc != 0:
                self.notice = 'Publish failed. No automatic command retry.'
                raise ValueError('MQTT could not queue the command; it was not retried.')
            now = time.monotonic()
            self.last_sent = now
            self.native_pending = dict(command_id=cid, command=command, write=write,
                                       deadline=now + (30 if write else 10))
            self.native_result = {}
            self.notice = 'Setting sent; waiting for device acceptance.' if write else 'Query sent; waiting for device response.'
            return cid

    def _receive_native_result(self, data):
        pending = self.native_pending
        if not pending or data.get('operation') != 'protocol' or data.get('protocol') != self.protocol:
            return
        if (str(data.get('command_id')) != pending['command_id'] or
                data.get('command') != pending['command']):
            return
        outcome = data.get('outcome')
        if outcome not in ('completed', 'acknowledged', 'rejected', 'unsupported',
                           'unavailable', 'not_sent', 'uncertain', 'invalid_response'):
            return
        if 'write_sent' not in data or (data['write_sent'] is not None and type(data['write_sent']) is not bool):
            return
        if outcome == 'acknowledged' and (not pending['write'] or data['write_sent'] is not True):
            return
        if outcome == 'completed' and pending['write'] and (data['write_sent'] is not True or data.get('setting_verified') is not True):
            return
        if outcome == 'completed' and not pending['write'] and data['write_sent'] is not False:
            return
        if 'setting_verified' in data and data['setting_verified'] is not None and type(data['setting_verified']) is not bool:
            return
        if data.get('setting_verified') is True and not pending['write']:
            return
        self.native_result = copy.deepcopy(data)
        self.native_pending = None
        if pending['write']:
            if outcome in ('uncertain', 'invalid_response') or data['write_sent'] is None:
                self.native_write_blocked = True
                self.notice = 'Setting outcome uncertain. Check the device before further settings; no retry.'
            elif outcome == 'completed':
                self.notice = 'Setting verified by a fresh device readback.'
            elif outcome == 'acknowledged':
                self.notice = ('Setting accepted, but its fresh readback did not match. Check the device setting; no automatic retry.'
                               if data.get('setting_verified') is False else
                               'Setting accepted by the device. Readback measurements must be checked separately.')
            else:
                self.notice = f'Setting {outcome}: {data.get("detail", "")}. No retry.'
        else:
            self.notice = f'Query {outcome}: {data.get("detail", "")}'

    def _receive_native_pv_inputs(self, data):
        count = data.get('connected_pv_inputs')
        if type(count) is int and 0 <= count <= 16:
            self.native_connected_pv_inputs = count

    def snapshot(self, *, include_records=True):
        """Return isolated UI state; raw records are optional diagnostics data."""
        with self.lock:
            self.heartbeat = time.monotonic()
            self.native_telemetry.expire(self.heartbeat)
            state = copy.deepcopy({key: getattr(self, key) for key in (
                'active', 'ready', 'status', 'connection_error', 'notice', 'live', 'health', 'cache',
                'live_at', 'health_at', 'connected_at', 'pending', 'jobs', 'value_at', 'read_status',
                'protocol', 'member_id', 'protocol_mismatch', 'native_pending', 'native_result',
                'native_write_blocked', 'aohai_write_blocked', 'native_connected_pv_inputs')})
            state['native_records'] = self.native_telemetry.records() if include_records else []
            state['native_raw_live'] = copy.deepcopy(self.native_raw_live) if include_records else {}
            state['native_live'] = normalize_native_live(self.native_telemetry.records_view(),
                self.protocol, self.member_id, self.heartbeat,
                connected_pv_inputs=self.native_connected_pv_inputs)
            state['native_diagnostics'] = self.native_telemetry.diagnostics()
            state['native_incomplete'] = self.native_telemetry.incomplete_count
            state['native_drops'] = self.native_telemetry.dropped_count
            return state
