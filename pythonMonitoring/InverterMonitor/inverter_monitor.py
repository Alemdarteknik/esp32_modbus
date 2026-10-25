import tkinter as tk
from tkinter import ttk
import json
import copy
import threading
import queue
import re
import time
from datetime import datetime

import paho.mqtt.client as mqtt
from time_settings import TimeSettings
from catalog_settings_ui import CatalogSettings
from mqtt_backend import MonitorSession
from native_protocols import PROFILES, PROTOCOL_IDS, classify_command
from settings_results import reset_write_session


# ============================================================
# MQTT CONFIGURATION
# ============================================================

MQTT_HOST = "mqtt-esp.alemdarteknik.com"
MQTT_PORT = 443
MQTT_PATH = "/mqtt"

# If authentication is required:
MQTT_USERNAME = None
MQTT_PASSWORD = None

LIVE_TIMEOUT_SECONDS = 15
HEALTH_TIMEOUT_SECONDS = 90
NATIVE_UI_REFRESH_SECONDS = 1
AOHAI_PROTOCOL = "AOHAI_MODBUS_RTU"


def freshness_text(last_received, now, timeout, connected_at=None):
    if last_received is None:
        if connected_at is not None and now - connected_at > timeout:
            return "MISSING"
        return "Waiting"
    age = max(0, now - last_received)
    return "Fresh" if age <= timeout else f"STALE ({int(age)}s)"


def native_power_value(summary, quality, key, unit="kW", signed=False):
    value = summary.get(key)
    flags = quality.get(key, {})
    if value is None or flags.get("available") is False:
        return "Unavailable"
    if flags.get("stale"):
        return "Stale"
    if not flags.get("fresh"):
        return "Unavailable"
    sign = "+" if signed and isinstance(value, (int, float)) and value >= 0 else ""
    if sign:
        value = abs(value)
    return f"{sign}{value} {unit}"


def native_result_text(result):
    """A protocol ACK accepts a request; it does not prove its setting value."""
    if not result:
        return "No command result yet."
    outcome = result.get("outcome", "unknown")
    messages = {
        "acknowledged": ("Accepted, but the fresh readback did not match the request. Check the device setting; no automatic retry."
                         if result.get("setting_verified") is False else
                         "Accepted by device; check the readback before treating the value as confirmed."),
        "completed": ("Setting verified by a fresh device readback." if result.get("setting_verified") is True
                      else "Read completed; measurements arrive on the protocol telemetry topic."),
        "uncertain": "Outcome uncertain. Do not repeat the setting; verify the device before reconnecting.",
        "rejected": "Device rejected the command.",
        "unsupported": "Command or protocol is unsupported by the selected device.",
        "unavailable": "Device is unavailable.",
        "not_sent": "Command was not sent to the device.",
        "invalid_response": "Response could not be validated.",
        "timeout": "No result before the deadline. A setting may have changed; verify the device first.",
    }
    label = messages.get(outcome, str(outcome))
    detail = result.get("detail", "")
    return f"{result.get('command', '')}: {label}" + (f" {detail}" if detail else "")


# ============================================================
# INVERTER MONITOR
# ============================================================

class InverterMonitor:

    def __init__(self, root):

        self.root = root

        self.root.title("Inverter MQTT Live Monitor")
        self.root.geometry("1600x900")
        self.root.minsize(1000, 700)
        self.setup_theme()

        self.running = True
        self.connection_active = False
        self.id_locked = False
        self.disconnecting = False
        self.mqtt_client = None

        self.data_queue = queue.Queue()

        self.trees = {}
        self.tabs = {}

        self.latest_data = {}
        self.raw_aohai_data = None
        self.last_live_received = None
        self.last_health_received = None
        self.connected_at = None
        self.native_session = None
        self.native_last_render = None
        self.native_last_raw_render = None
        self.native_last_received = None
        self.native_snapshot = {}
        self.next_native_refresh = 0
        self.native_health_render = None
        self.native_control_signature = None
        self.tree_rows = {}
        self.native_widget_options = {}


        # ====================================================
        # TOP BAR
        # ====================================================

        top = ttk.Frame(root)
        top.pack(fill="x", padx=10, pady=10)

        ttk.Label(
            top,
            text="SOLAR / Live energy monitor",
            font=("Segoe UI", 12)
        ).pack(side="left")

        self.connection_label = ttk.Label(
            top,
            text="DISCONNECTED", foreground="red"
        )

        self.connection_label.pack(side="right")


        # ====================================================
        # INFORMATION BAR
        # ====================================================

        info = ttk.Frame(root)
        info.pack(fill="x", padx=10, pady=5)

        ttk.Label(info, text="Inverter ID:").pack(side="left", padx=(0, 6))
        self.inverter_id = tk.StringVar()
        self.id_entry = ttk.Entry(info, textvariable=self.inverter_id, width=22)
        self.id_entry.pack(side="left", padx=(0, 8))
        ttk.Label(info, text="Protocol:").pack(side="left", padx=(0, 5))
        self.protocol_id = tk.StringVar(value=AOHAI_PROTOCOL)
        self.protocol_selector = ttk.Combobox(
            info, textvariable=self.protocol_id, values=PROTOCOL_IDS,
            state="readonly", width=19
        )
        self.protocol_selector.pack(side="left", padx=(0, 8))
        self.protocol_selector.bind("<<ComboboxSelected>>", self.select_protocol)
        ttk.Label(info, text="Member:").pack(side="left", padx=(0, 5))
        self.member_id = tk.StringVar(value="0")
        self.member_selector = ttk.Combobox(
            info, textvariable=self.member_id, values=tuple(str(i) for i in range(10)),
            state="readonly", width=3
        )
        self.member_selector.pack(side="left", padx=(0, 8))
        self.connect_button = ttk.Button(info, text="Connect", command=self.connect, style="Connect.TButton")
        self.connect_button.pack(side="left")
        self.id_entry.bind("<Return>", lambda event: self.connect())


        self.time_label = ttk.Label(
            info,
            text="Last update: --"
        )
        self.time_label.pack(side="right", padx=10)
        self.freshness_label = ttk.Label(root, text="Live: -- | Gateway health: --")
        self.freshness_label.pack(fill="x", padx=10)


        # ====================================================
        # NOTEBOOK
        # ====================================================

        self.notebook = ttk.Notebook(root)

        self.notebook.pack(
            fill="both",
            expand=True,
            padx=10,
            pady=10
        )


        power_frame = ttk.Frame(self.notebook)
        self.notebook.add(power_frame, text="Power Summary")
        self.create_tree(power_frame, "Power Summary")
        health_frame = ttk.Frame(self.notebook)
        self.notebook.add(health_frame, text="Gateway Health")
        ttk.Label(health_frame, text="Internet-facing gateway only; not per-slave hardware health.").pack(anchor="w", padx=12, pady=8)
        self.create_tree(health_frame, "Gateway Health")


        # ====================================================
        # RAW JSON TAB
        # ====================================================

        raw_frame = ttk.Frame(self.notebook)
        self.raw_frame = raw_frame

        self.notebook.add(
            raw_frame,
            text="Raw JSON"
        )

        self.raw_text = tk.Text(
            raw_frame,
            wrap="none",
            font=("Consolas", 10)
        )

        self.raw_text.pack(
            side="left",
            fill="both",
            expand=True
        )

        raw_y = ttk.Scrollbar(
            raw_frame,
            orient="vertical",
            command=self.raw_text.yview
        )

        raw_y.pack(
            side="right",
            fill="y"
        )

        raw_x = ttk.Scrollbar(
            raw_frame,
            orient="horizontal",
            command=self.raw_text.xview
        )

        raw_x.pack(
            side="bottom",
            fill="x"
        )

        self.raw_text.configure(
            yscrollcommand=raw_y.set,
            xscrollcommand=raw_x.set,
            background="#111c30", foreground="#dbe7f5",
            insertbackground="#dbe7f5", borderwidth=0,
            padx=18, pady=18
        )


        # ====================================================
        # MAIN DATA TAB
        # ====================================================

        main_frame = ttk.Frame(self.notebook)

        self.notebook.add(
            main_frame,
            text="Main Data"
        )

        self.create_tree(
            main_frame,
            "Main"
        )


        # ====================================================
        # OPERATING STATUS TAB
        # ====================================================

        self.settings_outer = ttk.Frame(self.notebook)
        self.notebook.add(self.settings_outer, text="Settings")
        time_frame = ttk.Frame(self.settings_outer, padding=(12, 8))
        time_frame.pack(fill="x")
        self.time_settings = TimeSettings(self, time_frame)
        catalog_frame = ttk.Frame(self.settings_outer)
        catalog_frame.pack(fill="both", expand=True)
        self.catalog_settings = CatalogSettings(self, catalog_frame)
        self.setting_controls = (self.time_settings, self.catalog_settings)
        status_frame = ttk.Frame(self.notebook)

        self.notebook.add(
            status_frame,
            text="Operating Status"
        )

        self.create_tree(
            status_frame,
            "Operating Status"
        )
        self.aohai_frames = (power_frame, main_frame, status_frame, self.settings_outer)
        self.native_frames = self.create_native_tabs()
        self.select_protocol()
        self.notebook.bind("<<NotebookTabChanged>>", self.native_tab_changed)


        # ====================================================
        # START MQTT
        # ====================================================



        # ====================================================
        # PROCESS DATA
        # ====================================================

        self.root.after(
            100,
            self.process_queue
        )


        # ====================================================
        # CLOSE
        # ====================================================

        self.notebook.insert("end", self.settings_outer)
        self.root.protocol(
            "WM_DELETE_WINDOW",
            self.close
        )


    # ========================================================
    # CREATE TREE
    # ========================================================

    def create_native_tabs(self):
        measurements = ttk.Frame(self.notebook)
        self.notebook.add(measurements, text="Live data")
        ttk.Label(measurements, text="The ESP reads live measurements automatically. Device information and settings may update less often.").pack(
            anchor="w", padx=12, pady=8
        )
        self.native_live_notice = ttk.Label(measurements, text="Waiting for fresh electrical measurements.")
        self.native_live_notice.pack(anchor="w", padx=12, pady=4)
        cards = ttk.Frame(measurements, padding=12)
        cards.pack(fill="x")
        self.native_metric_labels = {}
        self.native_metric_frames = {}
        for index, (key, label) in enumerate((("pv", "PV"), ("grid", "Grid"),
                ("battery", "Battery"), ("output", "Output"), ("load", "Load"))):
            cards.columnconfigure(index, weight=1)
            frame = ttk.LabelFrame(cards, text=label, padding=12)
            frame.grid(row=0, column=index, sticky="nsew", padx=4)
            value = ttk.Label(frame, text="Unavailable", font=("Segoe UI", 17))
            value.pack(anchor="w")
            self.native_metric_labels[key] = value
            self.native_metric_frames[key] = frame
        self.native_mode_label = ttk.Label(measurements, text="Operating mode: unavailable", wraplength=1400)
        self.native_mode_label.pack(anchor="w", padx=12, pady=5)
        ttk.Label(measurements, text="Unavailable is not zero. Stale readings remain visible with their update age.").pack(anchor="w", padx=12, pady=4)
        self.native_power_caption = ttk.Label(measurements,
            text="Battery power · kW: + discharge / − charge. Active power · kW; apparent power · kVA.")
        self.native_power_caption.pack(anchor="w", padx=12, pady=4)
        canvas = tk.Canvas(measurements, highlightthickness=0)
        scroll = ttk.Scrollbar(measurements, orient="vertical", command=canvas.yview)
        scroll.pack(side="right", fill="y")
        canvas.pack(fill="both", expand=True, padx=12, pady=8)
        canvas.configure(yscrollcommand=scroll.set)
        body = ttk.Frame(canvas)
        window = canvas.create_window((0, 0), window=body, anchor="nw")
        body.bind("<Configure>", lambda _: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda event: canvas.itemconfigure(window, width=event.width))
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=1)
        self.native_group_trees = {}
        for index, (key, label) in enumerate((("pv", "PV"), ("grid", "Grid"),
                ("battery", "Battery"), ("output", "Output"), ("load", "Load"),
                ("status", "Operating status"), ("energy", "Energy totals"),
                ("other", "Other measurements"))):
            frame = ttk.LabelFrame(body, text=label, padding=6)
            frame.grid(row=index // 2, column=index % 2, sticky="nsew", padx=4, pady=5)
            tree = ttk.Treeview(frame, columns=("measurement", "value", "unit", "updated"), show="headings", height=4)
            for column, width in (("measurement", 260), ("value", 150), ("unit", 65), ("updated", 150)):
                tree.heading(column, text=column.title())
                tree.column(column, width=width)
            scrollbar = ttk.Scrollbar(frame, orient="vertical", command=tree.yview)
            scrollbar.pack(side="right", fill="y")
            tree.configure(yscrollcommand=scrollbar.set)
            tree.pack(fill="both", expand=True)
            self.native_group_trees[key] = tree

        diagnostics = ttk.Frame(self.notebook)
        self.notebook.add(diagnostics, text="Advanced diagnostics")
        ttk.Label(diagnostics, text="Protocol records are diagnostics; normal monitoring does not require choosing a command.").pack(anchor="w", padx=12, pady=8)
        self.native_delivery_label = ttk.Label(diagnostics, text="")
        self.native_delivery_label.pack(anchor="w", padx=12, pady=4)
        columns = ("field", "value", "unit", "command", "protocol", "sequence", "freshness")
        self.native_tree = ttk.Treeview(diagnostics, columns=columns, show="headings")
        for column in columns:
            self.native_tree.heading(column, text=column.title())
            self.native_tree.column(column, width=230 if column == "field" else 130)
        scrollbar = ttk.Scrollbar(diagnostics, orient="vertical", command=self.native_tree.yview)
        scrollbar.pack(side="right", fill="y")
        horizontal = ttk.Scrollbar(diagnostics, orient="horizontal", command=self.native_tree.xview)
        horizontal.pack(side="bottom", fill="x")
        self.native_tree.configure(yscrollcommand=scrollbar.set, xscrollcommand=horizontal.set)
        self.native_tree.pack(fill="both", expand=True, padx=12, pady=(0, 12))

        commands = ttk.Frame(self.notebook, padding=12)
        self.notebook.add(commands, text="Manual settings")
        ttk.Label(commands, text="Live readings arrive automatically. Use these advanced controls only for a deliberate manual read or setting change.").pack(anchor="w", pady=(0, 8))
        self.native_profile_label = ttk.Label(commands, text="")
        self.native_profile_label.pack(anchor="w", pady=(0, 10))
        ttk.Label(commands, text="Documented command templates:").pack(anchor="w")
        self.native_template = tk.StringVar()
        self.native_template_selector = ttk.Combobox(
            commands, textvariable=self.native_template, state="readonly", width=95
        )
        self.native_template_selector.pack(anchor="w", fill="x", pady=6)
        self.native_template_selector.bind("<<ComboboxSelected>>", self.select_native_template)
        self.native_template_detail = ttk.Label(commands, text="", wraplength=1100)
        self.native_template_detail.pack(anchor="w", pady=6)
        ttk.Label(commands, text="Complete protocol command (edit the template before sending):").pack(anchor="w")
        self.native_command = tk.StringVar()
        self.native_command_entry = ttk.Entry(commands, textvariable=self.native_command, width=75)
        self.native_command_entry.pack(anchor="w", fill="x", pady=6)
        self.native_command.trace_add("write", lambda *_: self.native_command_changed())
        self.native_confirmed = tk.BooleanVar(value=False)
        self.native_confirmation = ttk.Checkbutton(
            commands, text="I checked this setting and want to send it once.",
            variable=self.native_confirmed, command=self.update_native_controls
        )
        self.native_confirmation.pack(anchor="w", pady=6)
        self.native_send_button = ttk.Button(commands, text="Send command", command=self.send_native_command)
        self.native_send_button.pack(anchor="w", pady=6)
        self.native_command_hint = ttk.Label(commands, text="", wraplength=1100)
        self.native_command_hint.pack(anchor="w", pady=6)
        self.native_result_label = ttk.Label(commands, text="No command result yet.", wraplength=1100)
        self.native_result_label.pack(anchor="w", pady=12)
        self.native_notice_label = ttk.Label(commands, text="", wraplength=1100)
        self.native_notice_label.pack(anchor="w", pady=6)
        return measurements, commands, diagnostics

    def select_protocol(self, event=None):
        if self.connection_active:
            return
        protocol = self.protocol_id.get()
        native = protocol != AOHAI_PROTOCOL
        self.native_session = None
        for frame in self.aohai_frames:
            if native:
                self.notebook.hide(frame)
            else:
                self.notebook.add(frame)
        for frame in self.native_frames:
            if native:
                self.notebook.add(frame)
            else:
                self.notebook.hide(frame)
        if native:
            self.notebook.insert(0, self.native_frames[0])
        self.clear_display()
        for setting in self.setting_controls:
            setting.reset()
        self.native_snapshot = {}
        self.native_last_render = None
        self.clear_tree(self.native_tree)
        self.native_command.set("")
        self.native_confirmed.set(False)
        self.native_template_entries = []
        profile = PROFILES.get(protocol, {})
        self.native_profile_label.configure(text=profile.get("label", protocol))
        if native:
            labels = []
            for kind, entries in (("Read", profile.get("queries", [])), ("Setting", profile.get("setters", []))):
                for entry in entries:
                    labels.append(f"{kind}: {entry.get('label', entry['key'])} [{entry['key']}]")
                    self.native_template_entries.append(entry)
            self.native_template_selector.configure(values=labels)
            self.native_template.set(labels[0] if labels else "No documented commands")
            if labels:
                self.native_template_selector.current(0)
                self.select_native_template()
            self.notebook.select(self.native_frames[0])
        else:
            self.notebook.select(self.aohai_frames[0])
        self.update_native_controls()

    def select_native_template(self, event=None):
        index = self.native_template_selector.current()
        if index < 0 or index >= len(self.native_template_entries):
            return
        entry = self.native_template_entries[index]
        example = entry.get("example") or (entry["key"] if not entry.get("pattern") else "")
        self.native_command.set(example)
        self.native_confirmed.set(False)
        details = entry.get("description", "")
        if entry.get("pattern"):
            details += f" Required format: {entry['pattern']}."
        if entry.get("readback"):
            details += f" Readback: {entry['readback']}."
        self.native_template_detail.configure(text=details.strip())
        self.update_native_controls()

    def native_command_changed(self):
        if not hasattr(self, "native_confirmed"):
            return
        self.native_confirmed.set(False)
        self.update_native_controls()

    def update_native_controls(self):
        if not hasattr(self, "native_send_button"):
            return
        snapshot = self.native_snapshot
        signature = (self.protocol_id.get(), self.native_command.get(),
                     self.native_confirmed.get(), self.disconnecting,
                     bool(snapshot.get("ready")), bool(snapshot.get("native_pending")),
                     bool(snapshot.get("native_write_blocked")), snapshot.get("protocol_mismatch"))
        if signature == self.native_control_signature:
            return
        self.native_control_signature = signature
        ready = bool(snapshot.get("ready")) and not self.disconnecting
        pending = bool(snapshot.get("native_pending"))
        write = False
        valid = False
        hint = "Select a documented command."
        try:
            definition = classify_command(self.protocol_id.get(), self.native_command.get())
            write = definition["setter"]
            valid = True
            hint = "Setting: explicit confirmation is required." if write else "Read-only command."
            if write:
                hint += f' Readback for this command: {definition["readback"]}.' if definition.get('readback') else ' No documented readback query for this command.'
        except (ValueError, KeyError, TypeError) as error:
            if self.native_command.get():
                hint = str(error)
        blocked = bool(snapshot.get("native_write_blocked"))
        if blocked:
            hint += " Earlier setting outcome is uncertain. Reads remain available; resolve it before reconnecting."
        mismatch = snapshot.get("protocol_mismatch")
        if mismatch:
            hint = f"Device reports {mismatch}. Disconnect and select its matching protocol."
        self.native_command_hint.configure(text=hint)
        editable = not pending and not self.disconnecting
        self.native_command_entry.configure(state="normal" if editable else "disabled")
        self.native_template_selector.configure(state="readonly" if editable else "disabled")
        self.native_confirmation.configure(state="normal" if ready and editable and write and not blocked else "disabled")
        can_send = ready and editable and valid and not mismatch and (not write or (self.native_confirmed.get() and not blocked))
        self.native_send_button.configure(state="normal" if can_send else "disabled")

    def send_native_command(self):
        if self.native_session is None or self.disconnecting:
            return
        try:
            command_id = self.native_session.send_native(
                self.native_command.get(), confirmed=self.native_confirmed.get()
            )
            self.native_confirmed.set(False)
            self.native_result_label.configure(text=f"Command {command_id} sent once; waiting for its result.")
            self.native_snapshot = self.native_session.snapshot(include_records=False)
        except (ValueError, RuntimeError) as error:
            self.native_result_label.configure(text=str(error))
        self.update_native_controls()

    def refresh_native(self):
        session = self.native_session
        if session is None:
            return
        selected_tab = self.notebook.select()
        diagnostics_visible = selected_tab == str(self.native_frames[2])
        raw_visible = selected_tab == str(self.raw_frame)
        snapshot = session.snapshot(include_records=diagnostics_visible or raw_visible)
        self.native_snapshot = snapshot
        ready = bool(snapshot.get("ready"))
        active = bool(snapshot.get("active"))
        self.connection_label.configure(
            text=str(snapshot.get("status", "Disconnected")),
            foreground="green" if ready else ("#9aaec8" if active else "red")
        )
        if ready and not self.disconnecting:
            self.connect_button.configure(
                text="Disconnect", command=self.disconnect, style="Disconnect.TButton", state="normal"
            )
        if not active and self.connection_active:
            self.connection_active = self.id_locked = False
            self.disconnecting = False
            self.id_entry.configure(state="normal")
            self.protocol_selector.configure(state="readonly")
            self.member_selector.configure(state="readonly")
            self.connect_button.configure(
                text="Connect", command=self.connect, style="Connect.TButton", state="normal"
            )
        records = snapshot.get("native_records", [])
        signature = tuple((record.get("command"), record.get("sequence"), record.get("timestamp_ms"))
                          for record in records)
        if diagnostics_visible and signature != self.native_last_render:
            self.native_last_render = signature
            self.clear_tree(self.native_tree)
            self.native_row_received = {}
            self.native_row_freshness = {}
            for record in records:
                for key, field in record.get("fields", {}).items():
                    row = self.native_tree.insert("", "end", values=(
                        key, str(field.get("value", "")), field.get("unit", ""),
                        record.get("command", ""), record.get("protocol", ""), record.get("sequence", ""), ""
                    ))
                    self.native_row_received[row] = record.get("received_at")
        received_at = snapshot.get("native_live", {}).get("received_at")
        if received_at != self.native_last_received:
            self.native_last_received = received_at
            self.time_label.configure(text="Last update: " + datetime.now().strftime("%H:%M:%S") if received_at is not None else "Last update: --")
        if raw_visible and signature != self.native_last_raw_render:
            self.native_last_raw_render = signature
            self.raw_text.delete("1.0", tk.END)
            self.raw_text.insert(tk.END, json.dumps(records, indent=2, ensure_ascii=False))
        health = snapshot.get("health", {})
        if health != self.native_health_render:
            self.native_health_render = copy.deepcopy(health)
            self.clear_tree(self.trees["Gateway Health"])
            for key, value in health.items():
                self.insert_value(self.trees["Gateway Health"], key, value)
        now = time.monotonic()
        self.render_native_live(snapshot)
        for row, received_at in (getattr(self, "native_row_received", {}).items() if diagnostics_visible else ()):
            value = freshness_text(received_at, now, LIVE_TIMEOUT_SECONDS)
            if value != self.native_row_freshness.get(row) and self.native_tree.exists(row):
                self.native_tree.set(row, "freshness", value)
                self.native_row_freshness[row] = value
        received = snapshot.get("native_live", {}).get("received_at")
        live = freshness_text(received, now, LIVE_TIMEOUT_SECONDS, snapshot.get("connected_at"))
        health_freshness = freshness_text(snapshot.get("health_at"), now, HEALTH_TIMEOUT_SECONDS, snapshot.get("connected_at"))
        self.freshness_label.configure(
            text=f"Live electrical data: {live} | Gateway health: {health_freshness}"
        )
        self.native_delivery_label.configure(text=f"Incomplete records: {snapshot.get('native_incomplete', 0)} | Dropped: {snapshot.get('native_drops', 0)}")
        if snapshot.get("native_pending"):
            pending = snapshot["native_pending"]
            self.native_result_label.configure(text=f"Waiting for command {pending.get('command_id')}: {pending.get('command')}")
        elif snapshot.get("native_result"):
            self.native_result_label.configure(text=native_result_text(snapshot["native_result"]))
        self.native_notice_label.configure(text=snapshot.get("notice", ""))
        self.update_native_controls()

    def native_tab_changed(self, event=None):
        # Load diagnostics on demand, without slowing the normal measurements.
        if self.native_session is not None:
            self.next_native_refresh = time.monotonic() + NATIVE_UI_REFRESH_SECONDS
            self.refresh_native()
        elif self.notebook.select() == str(self.raw_frame):
            self.render_aohai_raw()

    def render_aohai_raw(self):
        if self.raw_aohai_data is self.latest_data:
            return
        self.raw_aohai_data = self.latest_data
        self.raw_text.delete("1.0", tk.END)
        self.raw_text.insert(tk.END, json.dumps(self.latest_data, indent=4, ensure_ascii=False))

    def render_native_live(self, snapshot):
        live = snapshot.get("native_live", {})
        groups = live.get("groups", {})
        self.configure_native_widget(self.native_live_notice, text="Live electrical measurements received." if live.get("telemetry_available")
            else "Waiting for fresh electrical measurements. Operating mode and device information do not confirm live readings.")
        summary = live.get("power_summary", {})
        quality = live.get("power_quality", {})
        bms = live.get("device_type") == "bms"
        card_titles = (("battery", "Battery"), ("pv", "Current"), ("grid", "State of charge"),
                       ("output", "Temperature"), ("load", "Cell difference")) if bms else (
                       ("pv", "PV"), ("grid", "Grid power"), ("battery", "Battery power"),
                       ("output", "Load power"), ("load", "Load · %"))
        for index, (key, title) in enumerate(card_titles):
            if bms and key == "pv" and live.get("battery_metrics", {}).get("current", {}).get("name") == "balance_current":
                title = "Balance current"
            self.configure_native_widget(self.native_metric_frames[key], text=title)
            if getattr(self, "native_card_order", None) != bms:
                self.native_metric_frames[key].grid_configure(column=index)
        self.native_card_order = bms
        battery_card_keys = {"pv": "current", "grid": "state_of_charge",
                             "output": "temperature", "load": "cell_difference"}
        for key, summary_key in (("pv", "pv_kw"), ("grid", "grid_kw"),
                ("battery", "battery_kw"), ("output", "load_kw"), ("load", "load_percentage")):
            group = groups.get(key, {})
            value = "Unavailable"
            if bms and key in battery_card_keys:
                field = live.get("battery_metrics", {}).get(battery_card_keys[key], {})
                if field.get("stale"):
                    value = "Stale"
                elif field.get("fresh"):
                    value = f"{field.get('value')} {field.get('unit', '')}".rstrip()
            elif not bms and key in ("grid", "output"):
                prefix = "grid" if key == "grid" else "load"
                active = native_power_value(summary, quality, prefix + "_kw")
                apparent = native_power_value(summary, quality, prefix + "_apparent_kva", unit="kVA")
                value = f"Active: {active}\nApparent: {apparent}"
            elif summary_key in summary:
                value = native_power_value(summary, quality, summary_key,
                    unit="%" if summary_key == "load_percentage" else "kW", signed=summary_key == "battery_kw")
            elif (bms or key in ("pv", "load")) and not (
                    key == "pv" and live.get("pv_total_requires_channels")):
                field = group.get("primary") or {}
                if bms and key == "battery" and field.get("unit") in ("W", "kW"):
                    field = next((group["fields"][name] for name in ("battery_voltage", "total_battery_voltage",
                                 "total_voltage", "voltage", "v") if name in group.get("fields", {})), {})
                if field.get("stale"):
                    value = "Stale"
                elif field.get("fresh"):
                    value = f"{field.get('value')} {field.get('unit', '')}".rstrip()
            self.configure_native_widget(self.native_metric_labels[key], text=value)
        visible_row = 0
        for index, (key, tree) in enumerate(self.native_group_trees.items()):
            group = groups.get(key, {})
            hidden = bms and group.get("supported") is False
            layout = (hidden, visible_row if bms else index // 2,
                      0 if bms else index % 2, 2 if bms else 1,
                      8 if bms and key == "battery" else 4)
            if getattr(tree, "native_layout", None) != layout:
                if hidden:
                    tree.master.grid_remove()
                else:
                    tree.master.grid(row=layout[1], column=layout[2], columnspan=layout[3])
                    tree.configure(height=layout[4])
                tree.native_layout = layout
            if not hidden:
                visible_row += 1
            fields = group.get("fields", {})
            rows = []
            for name, field in fields.items():
                age = field.get("age_seconds")
                state = "Stale" if field.get("stale") else "Fresh" if field.get("fresh") else "Unavailable"
                updated = state + (f" · {int(age)}s ago" if age is not None else "")
                value = field.get("value")
                rows.append((name.replace("_", " ").capitalize(),
                    "Unavailable" if value is None else str(value), field.get("unit", ""), updated))
            if not fields:
                waiting = "Not reported by this profile" if group.get("supported") is False else "Waiting for measurements"
                rows.append((waiting, "Unavailable", "", ""))
            self.sync_tree_rows(tree, rows)
        status = groups.get("status", {}).get("fields", {})
        mode = (status.get("device_mode") or status.get("mode") or status.get("operating_mode")
                or status.get("working_mode"))
        if mode and mode.get("value") is not None:
            age = mode.get("age_seconds")
            state = "Stale" if mode.get("stale") else "Fresh" if mode.get("fresh") else "Unavailable"
            age_text = f" · {int(age)}s ago" if age is not None else ""
            self.configure_native_widget(self.native_mode_label, text=f"Operating mode reported by inverter: {mode['value']} · {state}{age_text}")
        else:
            self.configure_native_widget(self.native_mode_label, text="BMS operating flags and protections are shown below." if bms
                else "Operating mode: unavailable; waiting for the inverter to report it.")

    def configure_native_widget(self, widget, **options):
        """Avoid scheduling a Tk redraw when a displayed value is unchanged."""
        previous = self.native_widget_options.setdefault(widget, {})
        changed = {key: value for key, value in options.items() if previous.get(key) != value}
        if changed:
            widget.configure(**changed)
            previous.update(changed)

    def sync_tree_rows(self, tree, rows):
        """Keep row IDs, selection and scrolling while updating changed cells."""
        rows = tuple(tuple(str(value) for value in row) for row in rows)
        previous = self.tree_rows.get(tree, ())
        if rows == previous:
            return
        children = tree.get_children()
        for index, values in enumerate(rows):
            if index >= len(children):
                tree.insert("", "end", values=values)
            elif index >= len(previous) or values != previous[index]:
                tree.item(children[index], values=values)
        if len(children) > len(rows):
            tree.delete(*children[len(rows):])
        self.tree_rows[tree] = rows

    def create_tree(self, parent, name):

        frame = ttk.Frame(parent)

        frame.pack(
            fill="both",
            expand=True
        )

        columns = (
            "field",
            "register",
            "description",
            "domain",
            "value",
            "unit",
            "source",
            "extra"
        )

        tree = ttk.Treeview(
            frame,
            columns=columns,
            displaycolumns=("field", "value", "unit", "source", "register", "domain", "description", "extra"),
            show="headings"
        )

        headers = {
            "field": "Field / Name",
            "register": "Register",
            "description": "Description",
            "domain": "Domain",
            "value": "Value",
            "unit": "Unit",
            "source": "Source",
            "extra": "Extra"
        }

        widths = {
            "field": 260,
            "register": 180,
            "description": 350,
            "domain": 120,
            "value": 160,
            "unit": 80,
            "source": 100,
            "extra": 300
        }

        for column in columns:

            tree.heading(
                column,
                text=headers[column]
            )

            tree.column(
                column,
                width=widths[column],
                minwidth=60
            )


        # Vertical scrollbar

        scroll_y = ttk.Scrollbar(
            frame,
            orient="vertical",
            command=tree.yview
        )

        scroll_y.pack(
            side="right",
            fill="y"
        )


        # Horizontal scrollbar

        scroll_x = ttk.Scrollbar(
            frame,
            orient="horizontal",
            command=tree.xview
        )

        scroll_x.pack(
            side="bottom",
            fill="x"
        )


        tree.configure(
            yscrollcommand=scroll_y.set,
            xscrollcommand=scroll_x.set
        )

        tree.pack(
            side="left",
            fill="both",
            expand=True
        )

        self.trees[name] = tree

    def setup_theme(self):
        self.root.configure(background="#0b1220")
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure(".", font=("Segoe UI", 10), background="#0b1220", foreground="#dbe7f5")
        style.configure("BuzzerSet.TButton", background="#2563eb", foreground="#ffffff", padding=(16, 7))
        style.map("BuzzerSet.TButton", background=[("disabled", "#2563eb"), ("active", "#3b82f6"), ("!disabled", "#2563eb")],
                  foreground=[("disabled", "#ffffff"), ("!disabled", "#ffffff")])
        style.configure("Buzzer.TCombobox", fieldbackground="#111c30", foreground="#ffffff",
                        background="#162238", arrowcolor="#ffffff", insertcolor="#ffffff")
        style.map("Buzzer.TCombobox", fieldbackground=[("disabled", "#111c30"), ("readonly", "#111c30"), ("!disabled", "#111c30")],
                  foreground=[("disabled", "#ffffff"), ("readonly", "#ffffff"), ("!disabled", "#ffffff")],
                  selectbackground=[("!disabled", "#244565")], selectforeground=[("!disabled", "#ffffff")])
        self.root.option_add("*TCombobox*Listbox.background", "#111c30")
        self.root.option_add("*TCombobox*Listbox.foreground", "#ffffff")
        for name, color, hover in (
            ("Connect.TButton", "#16734a", "#208c5d"),
            ("Disconnect.TButton", "#aa3636", "#c34646"),
        ):
            style.configure(name, background=color, foreground="#ffffff", padding=(16, 7))
            style.map(name,
                      background=[("disabled", "#344154"), ("active", hover), ("!disabled", color)],
                      foreground=[("disabled", "#a5b1c0"), ("!disabled", "#ffffff")])
        style.configure(
            "TEntry", fieldbackground="#111c30", foreground="#dbe7f5",
            insertcolor="#ffffff", selectbackground="#244565",
            selectforeground="#ffffff", padding=6
        )
        style.map(
            "TEntry",
            fieldbackground=[("readonly", "#162238"), ("disabled", "#162238"), ("!disabled", "#111c30")],
            foreground=[("readonly", "#dbe7f5"), ("disabled", "#9aaec8"), ("!disabled", "#dbe7f5")],
            selectbackground=[("!disabled", "#244565")],
            selectforeground=[("!disabled", "#ffffff")]
        )
        style.configure("TNotebook", borderwidth=0, tabmargins=(0, 6, 0, 0))
        style.configure("TNotebook.Tab", padding=(12, 10), background="#162238", foreground="#9aaec8")
        style.map("TNotebook.Tab", background=[("selected", "#244565")], foreground=[("selected", "#ffffff")])
        style.configure("Treeview", background="#111c30", fieldbackground="#111c30", foreground="#dbe7f5", rowheight=32, borderwidth=0)
        style.configure("Treeview.Heading", background="#1c2c44", foreground="#9eb5d1", padding=(10, 10), font=("Segoe UI", 10, "bold"))
        style.map("Treeview", background=[("selected", "#244565")])


    # ========================================================
    # MQTT THREAD
    # ========================================================

    def connect(self):
        if self.connection_active or not self.running:
            return
        device_id = self.inverter_id.get().strip().upper()
        if device_id.startswith("INV-"):
            device_id = device_id[4:]
        if not re.fullmatch(r"[0-9A-F]{12}", device_id):
            self.connection_label.configure(
                text="Enter a 12-character hexadecimal inverter ID", foreground="red"
            )
            return
        protocol = self.protocol_id.get()
        if protocol not in PROTOCOL_IDS:
            self.connection_label.configure(text="Select a supported protocol", foreground="red")
            return
        try:
            member = int(self.member_id.get())
            if not 0 <= member <= 9:
                raise ValueError
        except ValueError:
            self.connection_label.configure(text="Select a member ID from 0 to 9", foreground="red")
            return
        self.inverter_id.set(device_id)
        self.mqtt_topic = f"inverter/INV-{device_id}/inverter/{member}/live"
        self.clear_display()
        reset_write_session(self)
        self.connection_active = True
        self.disconnecting = False
        self.id_entry.configure(state="readonly")
        self.protocol_selector.configure(state="disabled")
        self.member_selector.configure(state="disabled")
        self.connect_button.configure(state="disabled")
        self.connection_label.configure(text="CONNECTING...", foreground="#9aaec8")
        if protocol != AOHAI_PROTOCOL:
            self.native_session = MonitorSession()
            try:
                self.native_session.connect(device_id, protocol=protocol, member_id=member)
            except (ValueError, RuntimeError) as error:
                self.connection_active = False
                self.native_session = None
                self.id_entry.configure(state="normal")
                self.protocol_selector.configure(state="readonly")
                self.member_selector.configure(state="readonly")
                self.connect_button.configure(state="normal")
                self.connection_label.configure(text=str(error), foreground="red")
            return
        self.native_session = None
        threading.Thread(target=self.mqtt_thread, daemon=True).start()

    def disconnect(self):
        if not self.connection_active or self.disconnecting:
            return
        self.disconnecting = True
        self.clear_display()
        self.connect_button.configure(text="Disconnecting...", state="disabled")
        self.connection_label.configure(text="DISCONNECTING...", foreground="#9aaec8")
        if self.native_session is not None:
            self.native_session.close()
            return
        if self.mqtt_client is not None:
            self.mqtt_client.disconnect()

    def clear_display(self):
        self.connected_at = None
        self.last_live_received = None
        self.last_health_received = None
        self.freshness_label.configure(text="Live: -- | Gateway health: --")
        self.next_native_refresh = 0
        self.native_health_render = None
        for setting in self.setting_controls:
            setting.reset()
        """Remove the previous connection's telemetry from the UI."""
        self.latest_data = {}
        self.raw_aohai_data = None
        self.update_sections({})
        for tree in self.trees.values():
            self.clear_tree(tree)
        self.raw_text.delete("1.0", tk.END)
        self.time_label.configure(text="Last update: --")
        if hasattr(self, "native_tree"):
            self.clear_tree(self.native_tree)
            self.render_native_live({})
            self.native_delivery_label.configure(text="")
            self.native_last_render = None
            self.native_last_raw_render = None
            self.native_last_received = None
            self.native_snapshot = {}
            self.native_confirmed.set(False)
            self.native_result_label.configure(text="No command result yet.")
            self.native_notice_label.configure(text="")
            self.update_native_controls()
        self.notebook.select(self.native_frames[0] if self.protocol_id.get() != AOHAI_PROTOCOL else self.aohai_frames[0])

    def mqtt_thread(self):
        client = None
        try:

            # CallbackAPIVersion was added in paho-mqtt 2.0.  Keep the
            # monitor compatible with the 1.x release commonly installed in
            # existing virtual environments as well.
            if hasattr(mqtt, "CallbackAPIVersion"):
                client = mqtt.Client(
                    mqtt.CallbackAPIVersion.VERSION2,
                    transport="websockets",
                )
            else:
                client = mqtt.Client(transport="websockets")

            client.ws_set_options(
                path=MQTT_PATH
            )


            # ------------------------------------------------
            # Authentication
            # ------------------------------------------------

            if MQTT_USERNAME:

                client.username_pw_set(
                    MQTT_USERNAME,
                    MQTT_PASSWORD
                )


            # ------------------------------------------------
            # TLS for WSS
            # ------------------------------------------------

            client.tls_set()


            # ------------------------------------------------
            # Callbacks
            # ------------------------------------------------

            client.on_connect = self.on_connect
            client.on_disconnect = self.on_disconnect
            client.on_message = self.on_message
            client.on_subscribe = self.on_subscribe


            print(
                f"Connecting to "
                f"wss://{MQTT_HOST}:{MQTT_PORT}{MQTT_PATH}"
            )


            self.mqtt_client = client
            if not self.running:
                return
            client.connect(
                MQTT_HOST,
                MQTT_PORT,
                keepalive=60
            )


            if self.running:
                client.loop_forever()


        except Exception as error:

            print(
                "MQTT ERROR:",
                error
            )

            self.data_queue.put(
                (
                    "error",
                    str(error)
                )
            )

        finally:
            try:
                if client is not None:
                    client.disconnect()
            finally:
                self.data_queue.put(("finished", None))


    # ========================================================
    # MQTT CONNECTED
    # ========================================================

    def on_connect(
        self,
        client,
        userdata,
        flags,
        reason_code,
        properties=None
    ):

        print(
            "MQTT connected:",
            reason_code
        )


        if self.disconnecting:
            client.disconnect()
            return

        if reason_code == 0:

            client.subscribe(
                [(self.mqtt_topic, 0),
                 (self.mqtt_topic.rsplit("/", 2)[0] + "/0/health", 0),
                 (self.mqtt_topic.rsplit("/", 1)[0] + "/result", 1),
                 (self.mqtt_topic.rsplit("/", 1)[0] + "/snapshot/#", 1)]
            )

            print(
                "Subscribed to:",
                self.mqtt_topic
            )

            self.data_queue.put(
                (
                    "connection",
                    True
                )
            )

        else:

            client.disconnect()

            self.data_queue.put(
                (
                    "connection",
                    False
                )
            )


    # ========================================================
    # MQTT DISCONNECTED
    # ========================================================

    def on_subscribe(self, client, userdata, mid, reason_codes, properties=None):
        accepted = all(getattr(code, "value", code) < 128 for code in reason_codes)
        self.data_queue.put(("settings_ready", accepted))

    def on_disconnect(
        self,
        client,
        userdata,
        disconnect_flags,
        reason_code=None,
        properties=None
    ):

        # Paho 1.x passes the reason as the third callback argument.
        if reason_code is None:
            reason_code = disconnect_flags
        print(
            "MQTT disconnected:",
            reason_code
        )

        self.data_queue.put(
            (
                "connection",
                False
            )
        )


    # ========================================================
    # MQTT MESSAGE
    # ========================================================

    def on_message(
        self,
        client,
        userdata,
        message
    ):

        try:

            payload = message.payload.decode(
                "utf-8"
            )

            data = json.loads(
                payload
            )

            if not isinstance(data, dict):
                return
            if message.retain and message.topic.endswith('/result'):
                return
            if message.topic in (self.mqtt_topic, self.mqtt_topic.rsplit("/", 2)[0] + "/0/health"):
                # Retained packets do not prove the publisher is currently alive.
                kind = "data" if message.topic == self.mqtt_topic else "health"
                self.data_queue.put((kind, (data, time.monotonic(), message.retain)))
                return

            self.data_queue.put(
                (
                    "data" if message.topic == self.mqtt_topic else "setting_message",
                    data if message.topic == self.mqtt_topic else (message.topic, data, message.retain)
                )
            )

        except Exception as error:

            print(
                "MESSAGE ERROR:",
                error
            )


    # ========================================================
    # PROCESS QUEUE
    # ========================================================

    def drain_ui_queue(self):
        """Coalesce identical snapshots, preserving status and control events."""
        pending = None
        for _ in range(50):
            try:
                current = self.data_queue.get_nowait()
            except queue.Empty:
                break
            kind, data = current
            replace = (pending is not None and kind in ("data", "health")
                       and kind == pending[0] and data[2] == pending[1][2]
                       and data[0] == pending[1][0])
            if pending is not None and not replace:
                yield pending
            pending = current
        if pending is not None:
            yield pending

    def process_queue(self):

        try:

            for message_type, data in self.drain_ui_queue():


                if message_type == "connection":

                    if self.disconnecting:
                        continue

                    if data:

                        self.connected_at = time.monotonic()

                        self.id_locked = True
                        self.id_entry.configure(state="readonly")
                        self.connect_button.configure(
                            text="Disconnect", command=self.disconnect,
                            style="Disconnect.TButton", state="normal"
                        )

                        self.connection_label.config(
                            text="CONNECTED", foreground="green"
                        )

                    else:

                        self.connection_label.config(
                            text="DISCONNECTED", foreground="red"
                        )
                        self.clear_display()


                elif message_type == "data":

                    if self.disconnecting:
                        continue

                    self.update_data(
                        data[0]
                    )
                    if not data[2]:
                        self.last_live_received = data[1]

                elif message_type == "health":
                    if not self.disconnecting:
                        if not data[2]:
                            self.last_health_received = data[1]
                        tree = self.trees["Gateway Health"]
                        self.clear_tree(tree)
                        for key, value in data[0].items():
                            self.insert_value(tree, key, value)

                elif message_type == "settings_ready":
                    if not self.disconnecting and self.protocol_id.get() == AOHAI_PROTOCOL:
                        for setting in self.setting_controls:
                            setting.ready = data
                            if data:
                                setting.request("get")
                            else:
                                setting.status.configure(text="Settings subscription rejected.")

                elif message_type == "setting_message":
                    if not self.disconnecting and self.protocol_id.get() == AOHAI_PROTOCOL:
                        for setting in self.setting_controls:
                            setting.receive(*data)

                elif message_type == "finished":
                    for setting in self.setting_controls:
                        setting.reset()
                    self.connection_active = False
                    self.mqtt_client = None
                    self.id_locked = False
                    self.id_entry.configure(state="normal")
                    self.protocol_selector.configure(state="readonly")
                    self.member_selector.configure(state="readonly")
                    self.connect_button.configure(
                        text="Connect", command=self.connect,
                        style="Connect.TButton", state="normal"
                    )
                    if self.disconnecting:
                        self.connection_label.configure(text="DISCONNECTED", foreground="red")
                    self.disconnecting = False


                elif message_type == "error":

                    self.connection_label.config(
                        text="ERROR", foreground="red"
                    )

                    print(
                        "MQTT:",
                        data
                    )


        except queue.Empty:

            pass


        if self.running:

            if self.native_session is not None:
                now = time.monotonic()
                if now >= self.next_native_refresh:
                    self.next_native_refresh = now + NATIVE_UI_REFRESH_SECONDS
                    self.refresh_native()

            if self.native_session is None and self.connection_active and not self.disconnecting:
                now = time.monotonic()
                live = freshness_text(self.last_live_received, now, LIVE_TIMEOUT_SECONDS, self.connected_at)
                health = freshness_text(self.last_health_received, now, HEALTH_TIMEOUT_SECONDS, self.connected_at)
                if self.latest_data.get("core_status_fresh") is False:
                    live += " / core status STALE"
                self.freshness_label.configure(text=f"Live: {live} | Gateway health: {health}")

            self.root.after(
                100,
                self.process_queue
            )


    # ========================================================
    # UPDATE COMPLETE DATA
    # ========================================================

    def update_data(self, data):

        # The live topic publishes one complete snapshot.  It always has a
        # small header plus a ``sections`` object whose keys may change as the
        # inverter discovers PV, BMS, generator, or parallel devices.
        if not isinstance(data, dict):
            raise ValueError("MQTT live payload must be a JSON object")

        self.latest_data = data


        self.time_label.config(
            text=(
                "Last update: "
                + datetime.now().strftime(
                    "%H:%M:%S"
                )
            )
        )


        # ====================================================
        # MAIN DATA
        # ====================================================

        self.update_main_data(
            data
        )

        summary = data.get("power_summary", {})
        self.update_power_summary(
            summary if isinstance(summary, dict) else {}, data.get("sections")
        )


        # ====================================================
        # OPERATING STATUS
        # ====================================================

        operating_status = data.get("operating_status", {})
        self.update_operating_status(
            operating_status if isinstance(operating_status, dict) else {}
        )


        # ====================================================
        # SECTIONS
        # ====================================================

        sections = data.get("sections", {})
        self.update_sections(sections if isinstance(sections, dict) else {})


        # ====================================================
        # RAW JSON
        # ====================================================

        if self.notebook.select() == str(self.raw_frame):
            self.render_aohai_raw()


    # ========================================================
    # MAIN DATA
    # ========================================================

    def update_main_data(self, data):

        tree = self.trees["Main"]

        self.clear_tree(
            tree
        )


        for key, value in data.items():

            if key in (
                "operating_status",
                "sections",
                "power_summary"
            ):
                continue


            self.insert_value(
                tree,
                key,
                value
            )


    # ========================================================
    # OPERATING STATUS
    # ========================================================

    def update_power_summary(self, summary, sections=None):
        """Display reported power with its units and provenance, without rescaling."""
        tree = self.trees["Power Summary"]
        self.clear_tree(tree)
        if not summary:
            self.insert_value(tree, "Status", "No power summary in latest payload")
            return

        metrics = {
            "system_generation_kw": ("System generation", "kW", "system_generation"),
            "pv_kw": ("PV active power", "kW", "pv"),
            "grid_kw": ("Grid active power", "kW", "grid"),
            "grid_apparent_kva": ("Grid apparent power", "kVA", "grid"),
            "inverter_kw": ("Inverter active power", "kW", "inverter"),
            "inverter_apparent_kva": ("Inverter apparent power", "kVA", "inverter"),
            "load_kw": ("Load active power", "kW", "load"),
            "load_apparent_kva": ("Load apparent power", "kVA", "load"),
            "output_kw": ("Output active power", "kW", "output"),
            "output_apparent_kva": ("Output apparent power", "kVA", "output"),
            "load_percentage": ("Load percentage", "%", "load_percentage"),
            "battery_kw": ("Battery active power", "kW", "battery"),
        }
        summary = dict(summary)
        # Older firmware uses load_kva for the same apparent-power reading.
        if "load_apparent_kva" not in summary and "load_kva" in summary:
            summary["load_apparent_kva"] = summary["load_kva"]
        summary.pop("load_kva", None)
        # Also retain every metadata/unknown field as its own row so future
        # firmware additions remain visible without a monitor update.
        ordered_keys = [key for key in metrics if key in summary]
        ordered_keys.extend(key for key in summary if key not in metrics)
        for key in ordered_keys:
            value = summary[key]
            if key not in metrics:
                self.insert_value(tree, key, value)
                continue
            label, unit, prefix = metrics[key]
            source = summary.get(key + "_source", "")
            if not source:
                # Generic source fields describe active power, not apparent power.
                generic_source_keys = {
                    "pv_kw", "grid_kw", "inverter_kw", "output_kw",
                    "load_kw", "load_percentage", "battery_kw",
                }
                if key in generic_source_keys:
                    source = summary.get(prefix + "_source", "")
            if not source and isinstance(sections, dict):
                record_names = {label.casefold()}
                if key == "inverter_apparent_kva":
                    record_names.add("inverter ac apparent power")
                for records in sections.values():
                    if not isinstance(records, list):
                        continue
                    for item in records:
                        if (isinstance(item, dict)
                                and str(item.get("name", "")).casefold() in record_names
                                and item.get("unit") == unit):
                            source = item.get("source", "")
                            if source:
                                break
                    if source:
                        break
            record = {
                "value": "Unavailable" if value is None else value,
                "unit": unit,
                "description": key,
                "source": source,
            }
            if value is None:
                record["source"] = ""
                record["availability"] = "Payload reports null"
            for suffix in ("direction", "phase_count"):
                metadata_key = prefix + "_" + suffix
                if metadata_key in summary:
                    record[suffix] = summary[metadata_key]
            self.insert_record(tree, label, record)

    def update_operating_status(
        self,
        data
    ):

        tree = self.trees[
            "Operating Status"
        ]

        self.clear_tree(
            tree
        )


        for key, value in data.items():

            if isinstance(
                value,
                dict
            ):

                if self.is_record(
                    value
                ):

                    self.insert_record(
                        tree,
                        key,
                        value
                    )

                else:

                    self.insert_dictionary(
                        tree,
                        key,
                        value
                    )

            elif isinstance(
                value,
                list
            ):

                self.insert_list(
                    tree,
                    key,
                    value
                )

            else:

                self.insert_value(
                    tree,
                    key,
                    value
                )


    # ========================================================
    # UPDATE SECTIONS
    # ========================================================

    def update_sections(
        self,
        sections
    ):

        # Firmware may omit sections entirely. Remove their tabs and recreate
        # them normally if a later snapshot includes them again.
        section_names = set(sections)
        for section_name in set(self.tabs) - section_names:
            frame = self.tabs.pop(section_name)
            self.notebook.forget(frame)
            frame.destroy()
            del self.trees[section_name]

        for section_name, section_data in sections.items():

            self.create_section(
                section_name
            )

            tree = self.trees[
                section_name
            ]

            self.clear_tree(
                tree
            )


            if isinstance(
                section_data,
                list
            ):

                for item in section_data:

                    self.process_item(
                        tree,
                        item
                    )


            elif isinstance(
                section_data,
                dict
            ):

                self.insert_dictionary(
                    tree,
                    section_name,
                    section_data
                )

            else:

                self.insert_value(
                    tree,
                    section_name,
                    section_data
                )


    # ========================================================
    # CREATE SECTION TAB
    # ========================================================

    def create_section(
        self,
        name
    ):

        if name in self.tabs:

            return


        frame = ttk.Frame(
            self.notebook
        )


        title = (
            str(name)
            .replace("_", " ")
            .title()
        )


        self.notebook.add(
            frame,
            text=title
        )


        self.create_tree(
            frame,
            name
        )


        self.tabs[name] = frame
        self.notebook.insert("end", self.settings_outer)


    # ========================================================
    # PROCESS ITEM
    # ========================================================

    def process_item(
        self,
        tree,
        item
    ):

        if isinstance(
            item,
            dict
        ):

            if self.is_record(
                item
            ):

                name = item.get(
                    "name",
                    ""
                )

                self.insert_record(
                    tree,
                    name,
                    item
                )

            else:

                self.insert_dictionary(
                    tree,
                    "",
                    item
                )

        else:

            self.insert_value(
                tree,
                "",
                item
            )


    # ========================================================
    # CHECK IF RECORD
    # ========================================================

    def is_record(
        self,
        value
    ):

        if not isinstance(
            value,
            dict
        ):

            return False


        fields = {
            "name",
            "register_name",
            "description",
            "domain",
            "value",
            "unit",
            "source"
        }


        return any(
            field in value
            for field in fields
        )


    # ========================================================
    # INSERT RECORD
    # ========================================================

    def insert_record(
        self,
        tree,
        name,
        record
    ):

        register = record.get(
            "register_name",
            ""
        )

        description = record.get(
            "description",
            ""
        )

        domain = record.get(
            "domain",
            ""
        )

        value = record.get(
            "value",
            ""
        )

        unit = record.get(
            "unit",
            ""
        )

        source = record.get(
            "source",
            ""
        )


        standard = {
            "name",
            "register_name",
            "description",
            "domain",
            "value",
            "unit",
            "source"
        }


        extra = {
            key: val
            for key, val in record.items()
            if key not in standard
        }


        extra_text = ""

        if extra:

            extra_text = json.dumps(
                extra,
                ensure_ascii=False
            )


        tree.insert(
            "",
            "end",
            values=(
                name,
                register,
                description,
                domain,
                self.format_value(value),
                self.format_value(unit),
                self.format_value(source),
                extra_text
            )
        )


    # ========================================================
    # INSERT DICTIONARY
    # ========================================================

    def insert_dictionary(
        self,
        tree,
        parent,
        data
    ):

        for key, value in data.items():

            if parent:

                name = f"{parent}.{key}"

            else:

                name = key


            if isinstance(
                value,
                dict
            ):

                if self.is_record(
                    value
                ):

                    self.insert_record(
                        tree,
                        name,
                        value
                    )

                else:

                    self.insert_dictionary(
                        tree,
                        name,
                        value
                    )


            elif isinstance(
                value,
                list
            ):

                self.insert_list(
                    tree,
                    name,
                    value
                )


            else:

                self.insert_value(
                    tree,
                    name,
                    value
                )


    # ========================================================
    # INSERT LIST
    # ========================================================

    def insert_list(
        self,
        tree,
        parent,
        data
    ):

        for index, item in enumerate(data):

            name = (
                f"{parent}[{index}]"
            )


            if isinstance(
                item,
                dict
            ):

                if self.is_record(
                    item
                ):

                    self.insert_record(
                        tree,
                        name,
                        item
                    )

                else:

                    self.insert_dictionary(
                        tree,
                        name,
                        item
                    )


            elif isinstance(
                item,
                list
            ):

                self.insert_list(
                    tree,
                    name,
                    item
                )

            else:

                self.insert_value(
                    tree,
                    name,
                    item
                )


    # ========================================================
    # INSERT SIMPLE VALUE
    # ========================================================

    def insert_value(
        self,
        tree,
        name,
        value
    ):

        tree.insert(
            "",
            "end",
            values=(
                name,
                "",
                "",
                "",
                self.format_value(value),
                "",
                "",
                ""
            )
        )


    # ========================================================
    # FORMAT VALUE
    # ========================================================

    def format_value(
        self,
        value
    ):

        if value is None:

            return ""


        if isinstance(
            value,
            bool
        ):

            return str(value)


        if isinstance(
            value,
            float
        ):

            # Match the live feed's two-decimal measurement presentation.
            # Integer status codes/counts retain their integer formatting.
            return f"{value:.2f}"


        if isinstance(
            value,
            (dict, list)
        ):

            return json.dumps(
                value,
                ensure_ascii=False
            )


        return str(value)


    # ========================================================
    # CLEAR TREE
    # ========================================================

    def clear_tree(
        self,
        tree
    ):

        if hasattr(self, "tree_rows"):
            self.tree_rows.pop(tree, None)
        children = tree.get_children()
        if children:
            tree.delete(*children)


    # ========================================================
    # CLOSE
    # ========================================================

    def close(self):

        self.running = False
        if self.native_session is not None:
            self.native_session.close()

        try:

            if hasattr(
                self,
                "mqtt_client"
            ):

                self.mqtt_client.disconnect()

        except Exception:
            pass


        self.root.destroy()


# ============================================================
# START PROGRAM
# ============================================================

if __name__ == "__main__":

    root = tk.Tk()

    app = InverterMonitor(
        root
    )

    root.mainloop()
