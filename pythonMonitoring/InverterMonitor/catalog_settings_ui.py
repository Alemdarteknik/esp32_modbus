"""Searchable catalog editor for documented MQTT settings and service actions."""
import json
import time
import re
import tkinter as tk
from tkinter import ttk, messagebox

from settings_catalog import load_catalog, INTERLOCKS
from settings_selection import setting_member_matches
from settings_results import (begin_write, finish_write, write_problem,
                              named_write_result, failure_details,
                              normalize_setting_value)
from settings_polling import (SettingsReadPolicy, settings_index, snapshot_setting,
                              valid_setting_read)


class CatalogSettings:
    TIME_FIELDS = {'Year', 'Month', 'Day', 'Hour', 'Minute', 'Second'}

    def __init__(self, monitor, parent):
        self.monitor = monitor
        self.catalog = load_catalog()
        self._settings_index = settings_index(self.catalog)
        self.ready = False
        self.pending = None
        self.cache = {}
        self.unreadable = set()
        self.selected = None
        self.dirty = False
        self.last_read = {}
        self.read_policy = SettingsReadPolicy(self.last_read)
        self.frame = parent
        self.search = tk.StringVar()
        self.domain = tk.StringVar(value='All domains')
        top = ttk.Frame(parent)
        top.pack(fill='x', padx=12, pady=10)
        ttk.Label(top, text='Search settings:').pack(side='left')
        ttk.Entry(top, textvariable=self.search, width=32).pack(side='left', padx=8)
        domains = ['All domains'] + sorted({s.domain for s in self.catalog.values()})
        ttk.Combobox(top, textvariable=self.domain, values=domains, state='readonly',
                     style='Buzzer.TCombobox', width=18).pack(side='left', padx=8)
        self.count = ttk.Label(top)
        self.count.pack(side='right')
        self.search.trace_add('write', lambda *args: self.filter())
        self.domain.trace_add('write', lambda *args: self.filter())
        table_frame = ttk.Frame(parent)
        table_frame.pack(fill='both', expand=True, padx=12)
        self.table = ttk.Treeview(table_frame, columns=('name', 'description', 'domain', 'current', 'guard'),
                                  show='headings', height=8, selectmode='browse')
        for key, title, width in [('name', 'Setting', 220), ('description', 'Description', 300),
                                   ('domain', 'Domain', 110), ('current', 'Current value', 160),
                                   ('guard', 'Requirement', 180)]:
            self.table.heading(key, text=title)
            self.table.column(key, width=width, minwidth=80)
        scroll = ttk.Scrollbar(table_frame, command=self.table.yview)
        scroll.pack(side='right', fill='y')
        self.table.configure(yscrollcommand=scroll.set)
        self.table.pack(fill='both', expand=True)
        self.table.bind('<<TreeviewSelect>>', self.select)
        editor = ttk.Frame(parent, padding=12)
        editor.pack(fill='x')
        self.title = ttk.Label(editor, text='Select a setting to view or edit.')
        self.title.pack(anchor='w')
        self.details = tk.Text(editor, height=5, wrap='word', background='#111c30', foreground='#dbe7f5',
                               font=('Segoe UI', 10), relief='flat', state='disabled')
        self.details.pack(fill='x', pady=8)
        row = ttk.Frame(editor)
        row.pack(fill='x')
        ttk.Label(row, text='New value:').pack(side='left', padx=(0, 8))
        self.value = tk.StringVar()
        self.input = ttk.Combobox(row, textvariable=self.value, width=52, style='Buzzer.TCombobox', state='disabled')
        self.input.pack(side='left', padx=(0, 10))
        self.value.trace_add('write', self.edited)
        self.button = ttk.Button(row, text='Set', style='BuzzerSet.TButton', command=self.write, state='disabled')
        self.button.pack(side='left')
        self.ack = tk.BooleanVar(value=False)
        self.acknowledge = ttk.Checkbutton(editor, variable=self.ack,
            text='I have validated the model, value/range, dependencies and operating-state requirements shown above.')
        self.acknowledge.pack(anchor='w', pady=6)
        self.status = ttk.Label(editor, text='Connect to read settings.', wraplength=1100)
        self.status.pack(anchor='w')
        self.filter()
        monitor.root.after(1000, self.refresh)

    def edited(self, *args):
        self.dirty = True

    def format(self, setting, value):
        for label, choice in setting.choices.items():
            if value == choice:
                return label
        if value is None:
            return 'Unavailable'
        if re.fullmatch(r'PeriodTime(?:Start|End)\d+', setting.name) and isinstance(value, int):
            if 0 <= value <= 0x173B and value % 256 <= 59:
                return f'{value // 256:02d}:{value % 256:02d}'
        return str(value)

    def filter(self):
        text = self.search.get().casefold()
        domain = self.domain.get()
        children = self.table.get_children()
        if children:
            self.table.delete(*children)
        for key, item in self.catalog.items():
            if domain != 'All domains' and item.domain != domain:
                continue
            if text not in f'{item.name} {item.description} {item.meaning}'.casefold():
                continue
            current = self.format(item, self.cache[key]) if key in self.cache else 'Unknown'
            if item.name == 'OnOffSet' and key in self.cache:
                current += ' (command register)'
            self.table.insert('', 'end', iid=key, values=(item.name, item.description, item.domain, current, item.guard))
        self.update_count()

    def update_count(self):
        self.count.configure(text=f'{len(self.table.get_children())} / {len(self.catalog)} settings | {len(self.cache)} values loaded')

    def next_read(self):
        # Firmware snapshots fill most rows. Only missing/stale values need a
        # fallback getter; the selected row has a shorter refresh interval.
        now = time.monotonic()
        candidates = [key for key, item in self.catalog.items()
                      if item.readable and key not in self.unreadable
                      and self.read_policy.due(key, now, focused=key == self.selected)]
        if not candidates:
            return None
        return min(candidates, key=lambda key: self.last_read.get(key, 0))

    def controls(self):
        item = self.catalog.get(self.selected)
        writing = self.pending and self.pending[1] == 'set'
        enabled = self.ready and not self.monitor.disconnecting and item is not None and not writing
        can_write = enabled and not item.blocked and item.name not in self.TIME_FIELDS and not write_problem(self.monitor, self)
        self.input.configure(state=('readonly' if item.choices else 'normal') if can_write else 'disabled')
        self.button.configure(state='normal' if can_write else 'disabled')

    def select(self, event=None):
        selection = self.table.selection()
        if not selection:
            return
        key = selection[0]
        if key == self.selected:
            return
        if self.pending and self.pending[1] == 'set':
            if self.table.exists(self.selected):
                self.table.selection_set(self.selected)
            self.status.configure(text='Waiting for the current write result before editing another setting.')
            return
        self.pending = None
        self.selected = key
        item = self.catalog[key]
        self.ack.set(False)
        self.title.configure(text=f'{item.description}  ({item.name})')
        notes = item.notes + '\nMeaning: ' + item.meaning
        if item.bounds:
            notes += f'\nValidated application range: {item.bounds[0]} to {item.bounds[1]}; step {item.step}.'
        elif not item.choices and not item.value_type.startswith('ascii'):
            notes += '\nNo single validated application range in this catalog. Validate the engineering value for your model before enabling the write.'
        if item.value_type.startswith('ascii'):
            notes += '\nEnter one two-character ASCII cell, not a full serial/name.'
            if item.value_type == 'ascii_or_uint16':
                notes += ' This item also accepts an integer; quote a numeric ASCII cell, e.g. "12".'
        if re.fullmatch(r'PeriodTime(?:Start|End)\d+', item.name):
            notes += '\nThis editor accepts HH:MM and sends the documented packed numeric value.'
        if item.name == 'OnOffSet':
            notes += '\nCurrent value is the command register, not the operating state. On/Off is confirmed only after the ESP checks the inverter operating state.'
        if item.blocked:
            notes += '\n' + item.blocked
        self.details.configure(state='normal')
        self.details.delete('1.0', 'end')
        self.details.insert('end', notes)
        self.details.configure(state='disabled')
        self.input.configure(values=tuple(item.choices))
        self.value.set(self.format(item, self.cache[key]) if key in self.cache else '')
        self.dirty = False
        self.status.configure(text='Current value updates automatically; enter a value then press Set.')
        problem = write_problem(self.monitor, self)
        if problem:
            self.status.configure(text=problem)
        if item.name in self.TIME_FIELDS:
            self.status.configure(text='Use the System time field above the list to edit the complete date/time with Seconds last.')
        self.controls()
        self.request('get')

    def refresh(self):
        if not self.monitor.running:
            return
        if self.ready and not self.monitor.disconnecting and self.pending is None:
            key = self.next_read()
            if key is not None:
                self.request('get', key=key)
        self.monitor.root.after(1000, self.refresh)

    def reset(self):
        if self.pending and self.pending[1] == 'set':
            finish_write(self.monitor, self, uncertain=True)
        self.ready = False
        self.pending = None
        self.cache.clear()
        self.unreadable.clear()
        self.read_policy.reset()
        self.value.set('')
        self.dirty = False
        self.ack.set(False)
        self.status.configure(text='Connect to read settings.')
        self.filter()
        self.controls()

    def request(self, operation, value=None, key=None):
        self.controls()
        if not self.ready or self.monitor.disconnecting or self.pending:
            return
        key = key or self.selected or (self.next_read() if operation == 'get' else None)
        if key is None:
            return
        item = self.catalog[key]
        if operation == 'set':
            problem = write_problem(self.monitor, self)
            if problem or item.blocked:
                self.status.configure(text=problem or item.blocked)
                return
        if operation == 'get' and (not item.readable or key in self.unreadable):
            self.status.configure(text='Current value is not readable for this command.')
            return
        cid = str(time.time_ns())
        payload = dict(command_id=cid, operation=operation, name=item.name,
                       domain=item.domain, description=item.description)
        if operation == 'get':
            payload['category'] = 'configuration'
        else:
            begin_write(self.monitor, self)
            payload.update(value=value, confirmed=True)
            if item.guard in INTERLOCKS:
                payload[INTERLOCKS[item.guard]] = True
        self.pending = (cid, operation, key, value)
        if operation == 'get':
            self.read_policy.attempted(key, time.monotonic())
        self.controls()
        try:
            result = self.monitor.mqtt_client.publish(self.monitor.mqtt_topic.rsplit('/', 1)[0] + '/command',
                                                     json.dumps(payload), qos=0 if operation == 'set' else 1, retain=False)
            if result.rc != 0:
                raise RuntimeError(f'MQTT publish failed ({result.rc})')
        except Exception as error:
            if operation == 'set':
                finish_write(self.monitor, self)
            self.pending = None
            if operation == 'get':
                self.read_policy.failed(key, time.monotonic())
            self.status.configure(text=str(error))
            self.controls()
            return
        if operation == 'set':
            self.status.configure(text=f'Waiting for inverter confirmation: {item.description}...')
        self.monitor.root.after(10000 if operation == 'get' else 30000, lambda: self.timeout(cid))

    def write(self):
        if not self.selected or not self.ready or self.monitor.disconnecting:
            return
        if self.pending and self.pending[1] == 'set':
            return
        item = self.catalog[self.selected]
        problem = write_problem(self.monitor, self)
        if problem:
            self.status.configure(text=problem)
            return
        if item.name in self.TIME_FIELDS:
            return
        try:
            value = item.parse(self.value.get())
        except ValueError as error:
            self.status.configure(text=str(error))
            return
        requires_ack = item.guard != 'hot_edit_confirmed' or (not item.choices and item.bounds is None)
        if requires_ack and not self.ack.get():
            self.status.configure(text='Validate the listed requirements and select the acknowledgement before Set.')
            return
        if item.guard in ('service_only', 'commissioning_only', 'runtime_command') or (item.name == 'NumberTimePeriods' and value == 165):
            if not messagebox.askyesno('Confirm inverter command',
                f'{item.description}\n{item.name} = {value}\nRequirement: {item.guard}\n\nSend this command to the inverter?', parent=self.frame):
                return
        # User Set takes priority over an outstanding background getter.
        self.pending = None
        self.ack.set(False)
        self.request('set', value)

    def timeout(self, cid):
        if self.pending and self.pending[0] == cid:
            _, operation, key, _ = self.pending
            self.pending = None
            if operation == 'set':
                finish_write(self.monitor, self, uncertain=True)
                self.cache.pop(key, None)
                self.status.configure(text='Write unconfirmed: no matching result. No automatic write retry.')
            else:
                self.read_policy.failed(key, time.monotonic())
                if key == self.selected:
                    self.status.configure(text='Read timed out; waiting for the next automatic refresh.')
            self.controls()

    def update_current(self, key, value, retained=False):
        value = normalize_setting_value(self.catalog[key], value)
        added = key not in self.cache
        changed = added or self.cache[key] != value
        self.cache[key] = value
        if not retained and valid_setting_read(self.catalog[key], value):
            self.read_policy.received(key, time.monotonic())
        item = self.catalog[key]
        display = self.format(item, value)
        if changed and self.table.exists(key):
            self.table.set(key, 'current', display + (' (command register)' if item.name == 'OnOffSet' else ''))
        if key == self.selected and (changed or not self.dirty):
            self.value.set(display)
            self.dirty = False
        if added:
            self.update_count()

    def receive(self, topic, data, retained=False):
        if not setting_member_matches(self.monitor, data):
            return
        base = self.monitor.mqtt_topic.rsplit('/', 1)[0]
        if topic.startswith(base + '/snapshot/'):
            item = snapshot_setting(self._settings_index, data)
            if (item is not None and 'value' in data
                    and valid_setting_read(item, normalize_setting_value(item, data['value']))
                    and (not retained or item.key not in self.read_policy.fresh_at)):
                self.update_current(item.key, data['value'], retained=retained)
            return
        if topic != base + '/result' or not self.pending or retained:
            return
        cid, operation, key, expected = self.pending
        item = self.catalog[key]
        if (str(data.get('command_id')) != cid or data.get('operation') != operation or data.get('name') != item.name
                or ('domain' in data and str(data['domain']).casefold() != item.domain.casefold())
                or ('description' in data and data['description'] != item.description)):
            return
        self.pending = None
        outcome = data.get('outcome', 'invalid result')
        if operation == 'set':
            result = named_write_result(item, expected, data)
            finish_write(self.monitor, self, uncertain=result['uncertain'])
            if result['value_available']:
                self.update_current(key, result['value'])
            self.status.configure(text=(result['message'] or f'Confirmed: {self.format(item, result["value"])}'))
        elif outcome == 'confirmed' and valid_setting_read(item, data.get('value')):
            self.update_current(key, data['value'])
        else:
            self.read_policy.failed(key, time.monotonic())
            if outcome in ('not_readable', 'not_documented'):
                self.unreadable.add(key)
                if self.table.exists(key):
                    self.table.set(key, 'current', 'Not readable')
            if operation == 'set' or key == self.selected:
                self.status.configure(text=f'Not confirmed: {outcome}. {failure_details(data)}')
        self.controls()
