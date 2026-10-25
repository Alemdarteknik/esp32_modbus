"""Inverter calendar editor using the documented named MQTT time fields."""
import json
import time
from datetime import datetime
import tkinter as tk
from tkinter import ttk
from settings_selection import setting_member_matches
from settings_results import begin_write, finish_write, write_problem
from settings_polling import SettingsReadPolicy, CONTROL_REFRESH_SECONDS


class TimeSettings:
    FIELDS = ('Year', 'Month', 'Day', 'Hour', 'Minute', 'Second')
    RANGES = ((2022, 2050), (1, 12), (1, 31), (0, 23), (0, 59), (0, 59))

    def __init__(self, monitor, parent):
        self.monitor = monitor
        self.ready = False
        self.pending = None
        self.jobs = []
        self.values = {}
        self.read_values = {}
        self.writing = False
        self.dialog = None
        self.read_policy = SettingsReadPolicy(refresh_seconds=CONTROL_REFRESH_SECONDS)
        self.value = tk.StringVar(value='Unknown')
        style = ttk.Style(monitor.root)
        style.configure('Time.TSpinbox', fieldbackground='#111c30', foreground='#ffffff',
                        background='#162238', arrowcolor='#ffffff', insertcolor='#ffffff')
        style.map('Time.TSpinbox', fieldbackground=[('!disabled', '#111c30')],
                  foreground=[('!disabled', '#ffffff')])
        ttk.Label(parent, text='System time').grid(row=4, column=0, padx=8)
        self.field = ttk.Entry(parent, textvariable=self.value, state='readonly', width=24)
        self.field.grid(row=4, column=1, columnspan=2, padx=8, sticky='w')
        self.field.bind('<Button-1>', self.open_editor)
        self.field.bind('<Return>', self.open_editor)
        self.status = ttk.Label(parent, text='Connect to read inverter time. Click the field to edit.', wraplength=900)
        self.status.grid(row=5, column=0, columnspan=3, sticky='w', pady=12)
        monitor.root.after(10000, self.refresh)

    def reset(self):
        if self.writing:
            finish_write(self.monitor, self, uncertain=True)
        self.ready = False
        self.pending = None
        self.jobs = []
        self.values = {}
        self.read_values = {}
        self.writing = False
        self.read_policy.reset()
        self.value.set('Unknown')
        self.status.configure(text='Connect to read inverter time.')
        if self.dialog is not None:
            self.dialog.destroy()
            self.dialog = None

    def refresh(self):
        if not self.monitor.running:
            return
        if self.dialog is None and self.read_policy.due('calendar', time.monotonic()):
            self.request('get')
        self.monitor.root.after(10000, self.refresh)

    @classmethod
    def calendar(cls, values):
        parts = [int(values[name]) for name in cls.FIELDS]
        if not 2022 <= parts[0] <= 2050:
            raise ValueError('Year must be between 2022 and 2050')
        return datetime(*parts)

    def request(self, operation):
        if not self.ready or self.monitor.disconnecting or self.pending or self.jobs:
            return
        if operation != 'get':
            return
        self.read_values = {}
        self.read_policy.attempted('calendar', time.monotonic())
        self.jobs = [('get', name, None) for name in self.FIELDS]
        self.send_next()

    def send_next(self):
        if not self.jobs:
            return
        operation, name, value = self.jobs.pop(0)
        command_id = str(time.time_ns())
        payload = dict(command_id=command_id, operation=operation, name=name)
        if operation == 'get':
            payload['category'] = 'configuration'
        else:
            payload.update(value=value, confirmed=True, guarded_interlock=True)
        self.pending = (command_id, operation, name, value)
        try:
            info = self.monitor.mqtt_client.publish(
                self.monitor.mqtt_topic.rsplit('/', 1)[0] + '/command',
                json.dumps(payload), qos=0 if operation == 'set' else 1, retain=False)
            if info.rc != 0:
                raise RuntimeError(f'MQTT publish error {info.rc}')
        except Exception as error:
            self.fail(str(error))
            return
        self.monitor.root.after(30000, lambda: self.timeout(command_id))

    def timeout(self, command_id):
        if self.pending and self.pending[0] == command_id:
            self.fail('No matching inverter result. No write was retried.')

    def fail(self, message):
        partial = self.writing
        if not partial:
            self.read_policy.failed('calendar', time.monotonic())
        if partial:
            finish_write(self.monitor, self, uncertain=True)
        self.jobs = []
        self.pending = None
        self.writing = False
        self.values = {}
        self.value.set('Unknown')
        self.status.configure(text=('Time change not confirmed; some fields may have been written. ' if partial else '') + message)

    def receive(self, topic, data, retained=False):
        if retained or not setting_member_matches(self.monitor, data):
            return
        base = self.monitor.mqtt_topic.rsplit('/', 1)[0]
        pending = self.pending
        if topic != base + '/result' or not pending:
            return
        cid, operation, name, expected = pending
        if str(data.get('command_id')) != cid or data.get('name') != name or data.get('operation') != operation:
            return
        raw = data.get('value')
        if data.get('outcome') != 'confirmed' or isinstance(raw, bool) or not isinstance(raw, (int, float)) or not float(raw).is_integer():
            self.fail(f"{name}: {data.get('outcome', 'invalid result')}; invalid or unconfirmed value.")
            return
        value = int(raw)
        if name == 'Year' and 0 <= value <= 99:
            value += 2000
        if operation == 'set' and value != expected:
            self.fail(f'{name}: read-back mismatch ({value}, expected {expected}).')
            return
        self.pending = None
        if operation == 'get':
            self.read_values[name] = value
        if self.jobs:
            self.send_next()
        elif operation == 'set':
            self.writing = False
            finish_write(self.monitor, self)
            self.status.configure(text='All six time fields confirmed. Reading inverter time...')
            self.request('get')
        else:
            try:
                stamp = self.calendar(self.read_values)
            except (ValueError, KeyError, OverflowError) as error:
                self.fail(f'Invalid inverter time: {error}')
                return
            self.values = dict(self.read_values)
            self.read_policy.received('calendar', time.monotonic())
            self.value.set(stamp.strftime('%Y-%m-%d %H:%M:%S'))
            self.status.configure(text='Last read from inverter. Click the field to edit.')

    def open_editor(self, event=None):
        if self.dialog is not None:
            self.dialog.lift()
            return
        if not self.ready or self.monitor.disconnecting or not self.values or self.writing:
            return
        problem = write_problem(self.monitor, self)
        if problem:
            self.status.configure(text=problem)
            return
        self.dialog = tk.Toplevel(self.monitor.root)
        self.dialog.title('Set inverter system time')
        self.dialog.transient(self.monitor.root)
        self.dialog.configure(background='#0b1220')
        self.edit_values = {}
        for column, (name, (low, high)) in enumerate(zip(self.FIELDS, self.RANGES)):
            ttk.Label(self.dialog, text=name).grid(row=0, column=column, padx=8, pady=8)
            var = tk.StringVar(value=str(self.values[name]))
            self.edit_values[name] = var
            spin = ttk.Spinbox(self.dialog, from_=low, to=high, wrap=True, textvariable=var, width=6, style='Time.TSpinbox')
            spin.grid(row=1, column=column, padx=8, pady=8)
            spin.bind('<MouseWheel>', lambda e, v=var, a=low, b=high: self.roll(e, v, a, b))
        self.edit_status = ttk.Label(self.dialog, text='Scroll or use arrows. Seconds are applied last.', wraplength=500)
        self.edit_status.grid(row=2, column=0, columnspan=6, padx=8, pady=8)
        ttk.Button(self.dialog, text='Set', style='BuzzerSet.TButton', command=self.apply).grid(row=3, column=4, pady=12)
        ttk.Button(self.dialog, text='Cancel', command=self.close_editor).grid(row=3, column=5, pady=12)
        self.dialog.protocol('WM_DELETE_WINDOW', self.close_editor)

    @staticmethod
    def roll(event, variable, low, high):
        try:
            value = int(variable.get())
        except ValueError:
            value = low
        variable.set(str(low + (value - low + (1 if event.delta > 0 else -1)) % (high - low + 1)))
        return 'break'

    def close_editor(self):
        if self.dialog is not None:
            self.dialog.destroy()
            self.dialog = None

    def apply(self):
        if not self.ready or self.monitor.disconnecting:
            return
        if self.writing:
            return
        problem = write_problem(self.monitor, self)
        if problem:
            self.status.configure(text=problem)
            return
        try:
            values = {name: int(var.get()) for name, var in self.edit_values.items()}
            self.calendar(values)
        except (ValueError, OverflowError) as error:
            self.edit_status.configure(text=f'Invalid date/time: {error}')
            return
        # Supersede a background getter so one Set click is sufficient.
        self.pending = None
        begin_write(self.monitor, self)
        self.jobs = [('set', name, values[name]) for name in self.FIELDS]
        self.writing = True
        self.close_editor()
        self.status.configure(text='Writing inverter time; waiting for each read-back...')
        self.send_next()
