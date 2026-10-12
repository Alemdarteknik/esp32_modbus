"""Boolean MQTT settings with correlated read-back; widgets use the Tk thread."""
import json
import time
import tkinter as tk
from tkinter import ttk
from settings_selection import setting_member_matches


class BuzzerSettings:
    NAME = "DspBeepOnOff"
    TITLE = "Buzzer"
    CHOICES = {False: "Disable", True: "Enable"}
    GUARDED = False

    def __init__(self, monitor, name="DspBeepOnOff", title="Buzzer", parent=None, row=0, choices=None, guarded=False):
        self.monitor = monitor
        self.NAME = name
        self.TITLE = title
        self.CHOICES = self.CHOICES if choices is None else choices
        self.GUARDED = guarded
        self.ready = False
        self.current = None
        self.target = None
        self.pending = None
        self.frame = parent if parent is not None else ttk.Frame(monitor.notebook, padding=15)
        if parent is None:
            monitor.notebook.add(self.frame, text="Settings")
        style = ttk.Style(monitor.root)
        style.configure("BuzzerSet.TButton", background="#2563eb", foreground="#ffffff", padding=(16, 7))
        style.map("BuzzerSet.TButton",
                  background=[("disabled", "#2563eb"), ("active", "#3b82f6"), ("!disabled", "#2563eb")],
                  foreground=[("disabled", "#ffffff"), ("!disabled", "#ffffff")])
        style.configure("Buzzer.TCombobox", fieldbackground="#111c30", background="#162238",
                        foreground="#ffffff", arrowcolor="#ffffff",
                        selectbackground="#244565", selectforeground="#ffffff")
        style.map("Buzzer.TCombobox",
                  fieldbackground=[("disabled", "#111c30"), ("readonly", "#111c30"), ("!disabled", "#111c30")],
                  foreground=[("disabled", "#ffffff"), ("readonly", "#ffffff"), ("!disabled", "#ffffff")],
                  selectbackground=[("disabled", "#244565"), ("!disabled", "#244565")],
                  selectforeground=[("disabled", "#ffffff"), ("!disabled", "#ffffff")],
                  arrowcolor=[("disabled", "#ffffff"), ("!disabled", "#ffffff")])
        monitor.root.option_add("*TCombobox*Listbox.background", "#111c30")
        monitor.root.option_add("*TCombobox*Listbox.foreground", "#dbe7f5")
        ttk.Label(self.frame, text=title).grid(row=row, column=0, padx=8)
        self.value = tk.StringVar(value="Unknown")
        self.combo = ttk.Combobox(self.frame, textvariable=self.value,
                                  values=tuple(self.CHOICES.values()), width=max(16, max(map(len, self.CHOICES.values()))),
                                  style="Buzzer.TCombobox", state="disabled")
        self.combo.grid(row=row, column=1, padx=8)
        self.combo.bind("<<ComboboxSelected>>", self.select)
        self.button = ttk.Button(self.frame, text="Set", command=self.write, state="disabled", style="BuzzerSet.TButton")
        self.button.grid(row=row, column=2, padx=8)
        self.status = ttk.Label(self.frame, text=f"Connect to read the {title.lower()} setting.", wraplength=900)
        self.status.grid(row=row + 1, column=0, columnspan=3, sticky="w", pady=12)
        monitor.root.after(5000, self.refresh_tick)

    def refresh_tick(self):
        if not self.monitor.running:
            return
        # Some firmware only broadcasts snapshots at startup or after an MQTT
        # setter. Read automatically to also detect local/front-panel changes.
        if self.ready and not self.monitor.disconnecting and self.pending is None:
            self.request("get", background=True)
        self.monitor.root.after(5000, self.refresh_tick)

    def label(self, value):
        if isinstance(value, (bool, int, float)):
            return self.CHOICES.get(value)
        return None

    def controls(self):
        writing = self.pending is not None and self.pending[1] == 'set'
        ready = self.ready and not self.monitor.disconnecting and not writing
        self.combo.configure(state="readonly" if ready and self.current is not None else "disabled")
        self.button.configure(state="normal" if ready and self.target is not None else "disabled")

    def reset(self):
        self.ready = False
        self.pending = None
        self.target = None
        self.current = None
        self.value.set("Unknown")
        self.status.configure(text=f"Connect to read the {self.TITLE.lower()} setting.")
        self.controls()

    def select(self, event=None):
        choice = self.value.get()
        if choice not in self.CHOICES.values():
            return
        self.target = next(value for value, label in self.CHOICES.items() if label == choice)
        self.status.configure(text=f"Current: {self.label(self.current) or 'Unknown'}. Selected: {choice}. Press Set to apply.")
        self.controls()

    def write(self):
        if self.target is not None:
            # A user write takes priority over a background read. The old
            # read's result/timer is ignored by command-ID correlation.
            if self.pending and self.pending[1] == 'get':
                self.pending = None
            self.request("set", self.target)

    def request(self, operation, value=None, background=False):
        if not self.ready or self.monitor.disconnecting or self.pending is not None:
            return
        command_id = str(time.time_ns())
        payload = {"command_id": command_id, "operation": operation, "name": self.NAME}
        if operation == "get":
            payload["category"] = "configuration"
        else:
            if self.label(value) is None:
                self.status.configure(text="Invalid setting choice.")
                return
            payload.update(value=value, confirmed=True)
            if self.GUARDED:
                payload['guarded_interlock'] = True
        self.pending = (command_id, operation, value)
        self.controls()
        try:
            result = self.monitor.mqtt_client.publish(
                self.monitor.mqtt_topic.rsplit("/", 1)[0] + "/command",
                json.dumps(payload), qos=1, retain=False)
            if result.rc != 0:
                raise RuntimeError(f"Publish failed ({result.rc})")
        except Exception as error:
            self.pending = None
            self.status.configure(text=str(error))
            self.controls()
            return
        if not background:
            self.status.configure(text="Waiting for inverter read-back..." if operation == "set" else f"Reading current {self.TITLE.lower()} setting...")
        self.monitor.root.after(30000, lambda: self.timeout(command_id))

    def timeout(self, command_id):
        if self.pending and self.pending[0] == command_id:
            self.pending = None
            self.target = None
            self.current = None
            self.value.set("Unknown")
            self.status.configure(text="No matching result received. Not confirmed. Waiting for a setting update; no write was retried.")
            self.controls()

    def receive(self, topic, data):
        if not setting_member_matches(self.monitor, data) or data.get("name") != self.NAME:
            return
        base = self.monitor.mqtt_topic.rsplit("/", 1)[0]
        label = self.label(data.get("value"))
        if topic.startswith(base + "/snapshot/"):
            if label:
                changed = self.current != data["value"]
                self.current = data["value"]
                # Actual changes take precedence over an unsubmitted choice.
                # A snapshot is still not a correlated write confirmation.
                if changed or (self.target is None and self.pending is None):
                    if self.pending is None or self.pending[1] != "set":
                        self.target = None
                    self.value.set(label)
                    if self.pending is None:
                        self.status.configure(text=f"Current: {label}")
                elif self.target is not None and self.pending is None:
                    self.status.configure(text=f"Current: {label}. Selected: {self.label(self.target)}. Press Set to apply.")
                self.controls()
            return
        if topic != base + "/result":
            return
        pending = self.pending
        if not pending or str(data.get("command_id")) != pending[0] or data.get("operation") != pending[1]:
            return
        self.pending = None
        if data.get("outcome") == "confirmed" and label:
            changed = self.current != data["value"]
            self.current = data["value"]
            if pending[1] == "get" and not changed and self.target is not None:
                # Unchanged background reads must not cancel a fresh selection.
                self.controls()
                return
            self.target = None
            self.value.set(label)
            matched = pending[1] == "get" or data["value"] == pending[2]
            self.status.configure(text=f"Confirmed: {label}" if matched else f"Read-back mismatch: inverter reports {label}")
        else:
            self.target = None
            self.value.set(self.label(self.current) or "Unknown")
            self.status.configure(text=f"Not confirmed: {data.get('outcome', 'invalid result')}. {data.get('message', '')}")
        self.controls()
