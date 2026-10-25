"""Generate portable PI command metadata from the MIT mpp-solar source.

Only ast.literal_eval reads upstream constants; no upstream code is imported or
executed. Inherited command dictionaries are copied per profile. The generated
catalog is checked in, so ESP-IDF builds do not need Python or a reference tree.

Usage: python tools/generate_pi_protocol_catalog.py --source SOURCE_CHECKOUT
"""

from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import re
from re import _parser as regex_parser
from re import _constants as rx
import subprocess
import warnings


ROOT = Path(__file__).resolve().parents[1]
PROFILES = (
    "pi16", "pi17", "pi17m058", "pi17infini", "pi18", "pi18lvx",
    "pi18sv", "pi30", "pi30max", "pi30mst", "pi30revo", "pi30m044",
    "pi30m045", "pi41",
)
PARENTS = {
    "pi17m058": "pi17", "pi17infini": "pi17", "pi18lvx": "pi18",
    "pi18sv": "pi18", "pi30max": "pi30", "pi30mst": "pi30max",
    "pi30m044": "pi30max", "pi30m045": "pi30max", "pi41": "pi30",
}
REPLACE_COMMANDS = {"pi18lvx", "pi18sv"}
POLL = {
    "pi16": ["QPIGS", "QMOD", "QPIRI", "QPI", "QID"],
    "pi17": ["GS", "PS", "MOD", "WS", "BATS", "FLAG", "PIRI", "PI", "ID"],
    "pi18": ["GS", "MOD", "PIRI", "FLAG", "PI", "ID"],
    "pi18lvx": ["GS", "MOD", "FWS", "PIRI", "FLAG", "PI", "ID"],
    "pi18sv": ["GS", "MOD", "FWS", "PIRI", "FLAG", "PI", "ID"],
    "pi30": ["QPIGS", "QPIGS2", "QMOD", "QPIWS", "QPIRI", "QFLAG", "QPI", "QID"],
    "pi30max": ["QPIGS", "QPIGS2", "QMOD", "QPIWS", "QPIRI", "QFLAG", "QPI", "QSID"],
    "pi30revo": ["QPIGS", "QALL", "QMOD", "QPIRI", "PQSE", "QPI"],
    "pi41": ["QPIGS", "QPIGS2", "QMOD", "QPIWS", "QPIRI", "QFLAG", "QPI", "QID"],
}


def literal_constants(file: Path) -> dict:
    result = {}
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", SyntaxWarning)
        tree = ast.parse(file.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign):
            try:
                value = ast.literal_eval(node.value)
            except (ValueError, TypeError):
                continue
            for target in node.targets:
                if isinstance(target, ast.Name):
                    result[target.id] = value
    return result


def quote(value) -> str:
    return json.dumps(str(value), ensure_ascii=True)


class Nfa:
    """Compile the catalog's regular-expression subset into a bounded NFA."""

    def __init__(self, pattern):
        self.nodes = []
        self.accept = self.node(None, -1, -1)
        self.start = self.sequence(regex_parser.parse(pattern, flags=re.ASCII), self.accept)
        if len(self.nodes) > 192:
            raise ValueError(f"Regex exceeds bounded matcher: {pattern}")

    def node(self, allowed, first, second=-1):
        index = len(self.nodes)
        self.nodes.append((allowed, first, second))
        return index

    def sequence(self, sequence, following):
        current = following
        for op, arg in reversed(sequence):
            if op is rx.LITERAL:
                current = self.node(chr(arg), current)
            elif op is rx.ANY:
                current = self.node("", current)  # any printable ASCII
            elif op is rx.CATEGORY:
                if arg is not rx.CATEGORY_DIGIT:
                    raise ValueError(f"Unsupported regex category: {arg}")
                current = self.node("0123456789", current)
            elif op is rx.IN:
                chars = set()
                for kind, val in arg:
                    if kind is rx.LITERAL:
                        chars.add(chr(val))
                    elif kind is rx.RANGE:
                        chars.update(chr(c) for c in range(val[0], val[1] + 1))
                    elif kind is rx.CATEGORY and val is rx.CATEGORY_DIGIT:
                        chars.update("0123456789")
                    else:
                        raise ValueError(f"Unsupported regex class: {kind}, {val}")
                current = self.node("".join(sorted(chars)), current)
            elif op is rx.SUBPATTERN:
                current = self.sequence(arg[3], current)
            elif op is rx.BRANCH:
                branches = [self.sequence(branch, current) for branch in arg[1]]
                start = branches[-1]
                for branch in reversed(branches[:-1]):
                    start = self.node(None, branch, start)
                current = start
            elif op is rx.MAX_REPEAT:
                minimum, maximum, repeated = arg
                if maximum == rx.MAXREPEAT:
                    split = self.node(None, -1, current)
                    repeated_start = self.sequence(repeated, split)
                    self.nodes[split] = (None, repeated_start, current)
                    current = split
                else:
                    for _ in range(maximum - minimum):
                        start = self.sequence(repeated, current)
                        current = self.node(None, start, current)
                for _ in range(minimum):
                    current = self.sequence(repeated, current)
            elif op is rx.AT and arg in (rx.AT_BEGINNING, rx.AT_END):
                pass  # runtime always requires a full-string match
            else:
                raise ValueError(f"Unsupported regex operation: {op}, {arg}")
        return current


def normalized_pattern(profile: str, key: str, command: dict) -> str | None:
    pattern = command.get("regex")
    if not pattern:
        return None
    # Some definitions double escape \d and \. inside an already escaped Python
    # literal. These literal backslashes contradict the documented examples.
    pattern = pattern.replace("\\\\d", "\\d").replace("\\\\.", "\\.")
    # PI16's QEH/QEM/QEY patterns erroneously use QED as their command prefix.
    if profile == "pi16" and key in {"QEH", "QEM", "QEY"}:
        pattern = key + pattern[3:]
    # Resolve contradictions between permissive upstream regexes and the
    # command's documented wire fields. Keep model-dependent voltage/current
    # limits on the inverter; only enforce limits explicitly given here.
    if profile.startswith("pi17"):
        if key == "LST":
            pattern = r"LST(00|01|02|10|20)$"
        elif key == "ACCT":
            time = r"(2[0-3]|[01][0-9])[0-5][0-9]"
            count = 4 if profile == "pi17infini" else 2
            separator = "," if count == 4 else "-"
            pattern = "ACCT(" + separator.join([time] * count) + ")$"
        elif key == "ACCB":
            pattern = r"ACCB([01],(0[45][0-9][0-9]|0600))$"
    if profile == "pi18" and key == "MCHGV":
        voltage = r"(4[8-9][0-9]|5[0-7][0-9]|58[0-4])"
        pattern = "MCHGV(" + voltage + "," + voltage + ")$"
    if profile in {"pi18lvx", "pi18sv"}:
        if key == "POP":
            pattern = r"POP([01])$"
        elif key == "BUCD":
            pattern = r"BUCD((4[4-9][0-9]|50[0-9]|510),(000|4[8-9][0-9]|5[0-7][0-9]|580))$"
    if profile.startswith("pi30") or profile == "pi41":
        if key == "PBATMAXDISC":
            pattern = r"PBATMAXDISC(000|0[3-9][0-9]|1[0-4][0-9]|150)$"
        elif key == "PLEDC":
            channel = r"(0[0-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])"
            pattern = "PLEDC([1-3]" + channel * 3 + ")$"
    if profile == "pi30revo" and key in {"PSET", "_PSET"}:
        # Flags, four voltages, two currents, calendar/minute fields, and
        # configuration flags. The final recovery voltage is optional in
        # reference firmware. Never accept trailing arbitrary commands/text.
        voltage = r"[0-9]{1,3}[.][0-9]"
        pattern = (key + r"([01][0-4][01][0-2][01][0-3] "
                   + (voltage + " ") * 4
                   + r"[0-9]{3} [0-9]{3} [0-9]{4} [0-9]{2} [0-9]{2} "
                     r"[0-9]{2} [0-9]{2} [0-9]{2,5}( [0-9]{1,3}([.][0-9])?)?)$")
    return pattern


def clean_unit(unit: str) -> tuple[str, float]:
    unit = unit.replace("Â", "").replace("°C", "C").replace("℃", "C")
    # Some reference units separate a scale with whitespace or have a damaged
    # degree sign. Preserve the documented factor while exposing a normal unit.
    unit = unit.replace("\ufffdC", "C").replace("°C", "C").replace("℃", "C").strip()
    scale = 1.0
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)\s*([A-Za-z]+|%)", unit)
    if match:
        scale, unit = float(match[1]), match[2]
    if unit.upper() == "HZ":
        unit = "Hz"
    if unit.upper() == "WH":
        unit = "Wh"
    if unit.upper() == "KWH":
        unit = "kWh"
    return unit, scale


def readback(profile: str, key: str, commands: dict) -> str | None:
    if profile.startswith("pi17"):
        if key in {"PA", "PB", "PC", "PD", "PE", "PF", "PEA", "PDA"}:
            query = "FLAG"
        elif key == "DAT":
            query = "T"
        elif key == "GPMP0":
            query = "GPMP"
        elif key == "SEP" or key in {"EDA", "EDB", "EDC", "EDD", "EDE", "EDF", "EDG"}:
            query = "HECS"
        elif key in {"LST", "ACCT"}:
            query = "DI"
        elif key in {"BT", "MCHGV", "MCHGC", "MUCHGC", "BDCM", "BATDV", "BCA", "ACCB"}:
            query = "BATS"
        elif key == "LON":
            query = "PS"
        else:
            return None
    elif profile.startswith("pi18"):
        if key in {"PCP", "MCHGC", "MUCHGC"}:
            return "PRI0" if "PRI" in commands else None
        if key.startswith(("PE", "PD")) and key not in {"PEI", "PDI"}:
            query = "FLAG"
        elif key == "DAT":
            query = "T"
        elif key == "LON":
            query = "GS"
        else:
            query = "PIRI"
    elif profile == "pi30revo":
        query = "PQSE"
    else:
        if key in {"MCHGC", "MNCHGC", "MUCHGC", "POPM", "PPCP"}:
            return "QPGS0" if "QPGS" in commands else None
        if key in {"PE", "PD"}:
            query = "QFLAG"
        elif key.startswith("PLED"):
            query = "QLED"
        elif key == "DAT":
            query = "QT"
        elif key in {"PBATCD", "PBATMAXDISC"}:
            query = "QBMS"
        elif key == "BTA":
            query = "QPIGS"
        elif key == "PSAVE":
            return None
        else:
            query = "QPIRI"
    return query if query in commands and commands[query]["type"] != "SETTER" else None


def setter_example(profile: str, key: str, command: dict) -> str:
    """A concrete syntax example, never a suggested installation setting."""
    if not command.get("regex"):
        return key
    common = {
        "GPMP0": "00500", "LON": "1", "PA": "1", "PB": "1", "PC": "1",
        "PF": "1", "LST": "00", "EDA": "1", "EDB": "1", "EDC": "1",
        "EDD": "1", "EDE": "1", "EDF": "1", "EDG": "1", "BT": "1",
        "ACCB": "1,0450", "SEP": "01", "BDCM": "0100",
        "BATDV": "0420,0440,0430,0450", "BCA": "0000,060,0530",
        "F": "50", "MNCHGC": "0120", "PBCV": "44.0", "PBDV": "48.0",
        "PBFT": "54.0", "PCVV": "56.0", "POPLG": "00", "POPM": "01",
        "PPCP": "001", "PPVOKC": "0", "PSPB": "0", "PBATCD": "000",
        "PBATMAXDISC": "100", "BTA": "+01", "PGR": "00", "PSP": "0",
        "BUCD": "440,480", "PLEDE": "1", "PLEDS": "1", "PLEDM": "2",
        "PLEDB": "5", "PLEDT": "2", "PLEDC": "1000255255",
    }
    if key in {"PSET", "_PSET"}:
        value = "120103 56.3 54.6 43.8 42.6 040 020 2026 10 14 12 34 00"
    elif key == "DAT":
        value = "261014123400" if profile.startswith(("pi17", "pi18")) or profile == "pi30m045" else "20261014123400"
    elif key == "ACCT":
        value = "2200,2259,2300,2359" if profile == "pi17infini" else "2200-0259"
    elif key in {"PE", "PD"}:
        # Infini has exact PEA/PDA definitions as well as the PE/PD patterns.
        # Exercise the pattern itself rather than its shared-prefix exact form.
        value = "B" if profile.startswith("pi17") else "a"
    elif key in {"PBT", "PCP", "POP"}:
        value = "1" if profile.startswith("pi18") and key != "PCP" else "01"
        if profile.startswith("pi18") and key == "PCP":
            value = "0,1"
    elif key == "MCHGV":
        value = "0576,0540" if profile.startswith("pi17") else "552,540"
    elif key == "PSDV":
        value = "450" if profile.startswith("pi18") else "40.0"
    elif key in {"MCHGC", "MUCHGC"}:
        value = ("1200" if profile.startswith("pi17") else "0,030" if profile.startswith("pi18")
                 else "0030" if key == "MUCHGC" and profile == "pi30m045" else "030")
    else:
        value = common[key]
    example = key + value
    if not re.fullmatch(normalized_pattern(profile, key, command), example, flags=re.ASCII):
        raise ValueError(f"Invalid setter example: {profile} {example}")
    return example


class Catalog:
    def __init__(self):
        self.lines = []
        self.lists = {}
        self.regexes = {}
        self.fields = {}
        self.commands = {}

    def emit(self, line=""):
        self.lines.append(line)

    def lookup(self, pairs):
        pairs = tuple(pairs)
        if not pairs:
            return "nullptr", 0
        if pairs not in self.lists:
            name = f"lookup_{len(self.lists)}"
            self.lists[pairs] = name
            self.emit(f"static const Lookup {name}[] = {{")
            for key, value in pairs:
                self.emit(f"    {{{quote(key)}, {quote(value)}}},")
            self.emit("};")
        return self.lists[pairs], len(pairs)

    def regex(self, pattern):
        if not pattern:
            return "nullptr", 0, 0
        if pattern not in self.regexes:
            name = f"regex_{len(self.regexes)}"
            nfa = Nfa(pattern)
            self.regexes[pattern] = (name, len(nfa.nodes), nfa.start)
            self.emit(f"// Pattern: {pattern}")
            self.emit(f"static const RegexNode {name}[] = {{")
            for chars, first, second in nfa.nodes:
                self.emit(f"    {{{'nullptr' if chars is None else quote(chars)}, {first}, {second}}},")
            self.emit("};")
        return self.regexes[pattern]

    def field(self, raw, token, char_index=-1):
        if isinstance(raw[0], int):
            _, name, kind, *tail = raw
        else:
            kind, name, *tail = raw
        units = tail[0] if tail else ""
        if kind in {"discard", "exclude", "ack"}:
            return []
        if kind in {"flags", "stat_flags"}:
            return [(token, i, "digit", label, "bool", 1.0, "nullptr", 0)
                    for i, label in enumerate(units) if label and label != "Reserved"]
        if kind == "enflags":
            return [(token, ord(code), "enabled", value["name"], "bool", 1.0, "nullptr", 0)
                    for code, value in units.items()]
        if kind == "multi":
            fields = []
            for i, child in enumerate(name):
                fields.extend(self.field(child, token, i))
            return fields
        lookup_name, lookup_size = "nullptr", 0
        scale = 1.0
        if kind == "option":
            lookup_name, lookup_size = self.lookup((str(i), text) for i, text in enumerate(units))
            unit, fmt = "", "option"
        elif kind in {"keyed", "str_keyed"}:
            lookup_name, lookup_size = self.lookup(units.items())
            unit, fmt = "", "keyed"
        else:
            unit, scale = clean_unit(units)
            fmt = {
                "int": "integer", "int:r": "integer", "float": "number",
                "10int": "number", "int:r/10": "number", "int:r/100": "number",
                "float:r/10": "number", "int:r*30": "number",
                "bytes.decode:r[6:]": "trim6", "bytes.decode:r[7:]": "trim7",
                "bytes.decode:r[2:int(r[0:2])+2]": "serial_length",
                "info:cv": "command_value", "info:cv[:4]": "year",
                "info:cv[6:]": "day",
                "info:calendar.month_name[int(cv[4:6])]": "month",
                "info:calendar.month_name[int(cv[4:])]": "month",
                "bytes.decode:datetime.strptime(r, '%Y%m%d%H%M%S').strftime('%Y-%m-%d %H:%M:%S')": "date_time",
                "string": "text", "bytes.decode": "text", "bytes:r.decode()": "text",
            }.get(kind)
            if fmt is None:
                raise ValueError(f"Unhandled PI field type {kind}")
            if kind in {"10int", "int:r/10", "float:r/10"}:
                scale *= 0.1
            elif kind == "int:r/100":
                scale *= 0.01
            elif kind == "int:r*30":
                scale *= 30
        return [(token, char_index, fmt, name, unit, scale, lookup_name, lookup_size)]

    def field_list(self, definition):
        fields = []
        for token, field in enumerate(definition):
            fields.extend(self.field(field, token))
        # Repeated names (e.g. reserved/unknown BMS fields) must retain identity.
        names = {}
        unique = []
        for token, char_index, fmt, name, unit, scale, lookup, count in fields:
            names[name] = names.get(name, 0) + 1
            if names[name] > 1:
                name = f"{name} {names[name]}"
            unique.append((token, char_index, fmt, name, unit, scale, lookup, count))
        fields = tuple(unique)
        if not fields:
            return "nullptr", 0
        if fields not in self.fields:
            name = f"fields_{len(self.fields)}"
            self.fields[fields] = name
            self.emit(f"static const FieldDefinition {name}[] = {{")
            for token, char_index, fmt, label, unit, scale, lookup, count in fields:
                self.emit(f"    {{{token}, {char_index}, FieldKind::{fmt}, {quote(label)}, {quote(unit)}, {scale:g}, {lookup}, {count}}},")
            self.emit("};")
        return self.fields[fields], len(fields)

    def command(self, profile, key, cmd, commands):
        pattern = normalized_pattern(profile, key, cmd)
        nfa, node_count, start = self.regex(pattern)
        fields, field_count = self.field_list(cmd.get("response", []))
        setter = cmd["type"] == "SETTER"
        read = readback(profile, key, commands) if setter else None
        prefix = cmd.get("prefix")
        nocrc = 1 if cmd.get("nocrc") is True else (0 if cmd.get("nocrc") is False else -1)
        flag_codes = ""
        if setter and key in {"PE", "PD"} and profile.startswith(("pi30", "pi41")):
            for field in commands.get("QFLAG", {}).get("response", []):
                if field[0] == "enflags":
                    flag_codes = "".join(field[2])
        identity = (cmd.get("name", key), key, nfa, node_count, start, fields,
                    field_count, setter, read, prefix, nocrc,
                    cmd.get("type") == "QUERYEN", cmd.get("checksum_required") == "True",
                    cmd.get("crctype") == "chk", flag_codes)
        if identity not in self.commands:
            name = f"command_{len(self.commands)}"
            self.commands[identity] = name
            (label, lookup_key, regex, nodes, begin, defs, count, write, back,
             wire_prefix, no_crc, energy, checksum, revo_sum, flags) = identity
            self.emit(f"static const CommandDefinition {name} = {{")
            self.emit(f"    {quote(label)}, {quote(lookup_key)}, {regex}, {nodes}, {begin}, {defs}, {count},")
            self.emit(f"    {'true' if write else 'false'}, {'nullptr' if back is None else quote(back)}, {'nullptr' if wire_prefix is None else quote(wire_prefix)}, {no_crc},")
            self.emit(f"    {'true' if energy else 'false'}, {'true' if checksum else 'false'}, {'true' if revo_sum else 'false'}, {quote(flags)},")
            self.emit("};")
        return self.commands[identity]


def build(source: Path, output: Path, fixtures: Path, setter_fixtures: Path | None = None):
    upstream = source / "mppsolar" / "protocols"
    constants = {name: literal_constants(upstream / f"{name}.py") for name in PROFILES}
    resolved = {}

    def resolve(name):
        if name in resolved:
            return resolved[name]
        parent = PARENTS.get(name)
        commands = dict(resolve(parent)) if parent and name not in REPLACE_COMMANDS else {}
        module = constants[name]
        for group in ("COMMANDS", "QUERY_COMMANDS", "SETTER_COMMANDS", "NEW_COMMANDS"):
            commands.update(module.get(group, {}))
        for key in module.get("COMMANDS_TO_REMOVE", []):
            commands.pop(key, None)
        if name == "pi30":
            # The PI30 manufacturer protocol (2015-09-24, section 2.10)
            # documents this optional read for SCC2/SCC3. Upstream currently
            # exposes its twelve-token layout only under PI30MST. The portable
            # decoder also accepts the documented three-token MAX response;
            # selecting two PV inputs enables this query, and NAK stays absent.
            commands["QPIGS2"] = constants["pi30mst"]["QUERY_COMMANDS"]["QPIGS2"]
        resolved[name] = commands
        return commands

    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    catalog = Catalog()
    catalog.emit("// Generated by tools/generate_pi_protocol_catalog.py. Do not edit.")
    catalog.emit("// Source: https://github.com/jblance/mpp-solar (MIT, Copyright 2017 jblance)")
    catalog.emit(f"// Revision: {revision}")
    catalog.emit("// See third_party/mpp-solar/LICENSE for the upstream permission notice.")
    profile_lines = []
    test_vectors = []
    setting_vectors = []
    for name in PROFILES:
        commands = resolve(name)
        refs = [catalog.command(name, key, cmd, commands) for key, cmd in commands.items()]
        catalog.emit(f"static const CommandDefinition *const {name}_commands[] = {{")
        catalog.emit("    " + ", ".join(f"&{ref}" for ref in refs) + ",")
        catalog.emit("};")
        poll_name = name
        while poll_name not in POLL:
            poll_name = PARENTS[poll_name]
        poll = [cmd for cmd in POLL[poll_name] if cmd in commands]
        assert all(commands[cmd]["type"] != "SETTER" and not commands[cmd].get("regex") for cmd in poll)
        catalog.emit(f"static const char *const {name}_poll[] = {{")
        catalog.emit("    " + ", ".join(quote(cmd) for cmd in poll) + ",")
        catalog.emit("};")
        family = ("pi16" if name == "pi16" else "pi17" if name.startswith("pi17")
                  else "pi18" if name.startswith("pi18") else "revo" if name == "pi30revo" else "pi30")
        profile_lines.append(f"    {{EnergyProtocol::{name}, WireFamily::{family}, {'true' if name == 'pi18lvx' else 'false'}, {name}_commands, {len(refs)}, {name}_poll, {len(poll)}}},")
        for key, cmd in commands.items():
            if cmd["type"] == "SETTER":
                example = setter_example(name, key, cmd)
                setting_vectors.append((name, key, example, readback(name, key, commands),
                                        cmd.get("prefix"), cmd.get("nocrc")))
            for response in cmd.get("test_responses", []):
                if isinstance(response, bytes) and response:
                    test_vectors.append({"protocol": name, "command": key,
                                         "pattern": normalized_pattern(name, key, cmd),
                                         "frame_hex": response.hex()})
    catalog.emit("static const ProfileDefinition profiles[] = {")
    for line in profile_lines:
        catalog.emit(line)
    catalog.emit("};")
    output.write_text("\n".join(catalog.lines) + "\n", encoding="utf-8")
    fixtures.write_text(json.dumps({"source_revision": revision, "vectors": test_vectors}, indent=2) + "\n", encoding="utf-8")
    if setter_fixtures:
        lines = ["// Generated concrete syntax cases; never send these to hardware.",
                 "static const SetterVector setter_vectors[] = {"]
        for name, key, example, back, prefix, nocrc in setting_vectors:
            lines.append("    {EnergyProtocol::" + name + ", " + ", ".join(
                "nullptr" if x is None else quote(x) for x in (key, example, back, prefix))
                + ", " + ("true" if nocrc is True or name == "pi18lvx" else "false") + "},")
        lines.append("};")
        setter_fixtures.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Generated {len(PROFILES)} PI profiles, {len(catalog.commands)} distinct command definitions, {len(catalog.regexes)} patterns, {len(test_vectors)} reference vectors.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "build/protocol_reference/mpp-solar")
    parser.add_argument("--output", type=Path, default=ROOT / "main/src/protocol/pi_protocol_catalog.generated.inc")
    parser.add_argument("--fixtures", type=Path, default=ROOT / "tools/tests/pi_reference_vectors.generated.json")
    parser.add_argument("--setter-fixtures", type=Path, default=ROOT / "tools/tests/pi_setter_vectors.generated.inc")
    args = parser.parse_args()
    build(args.source, args.output, args.fixtures, args.setter_fixtures)


if __name__ == "__main__":
    main()
