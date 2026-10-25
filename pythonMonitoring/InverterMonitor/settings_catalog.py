"""Read the executable named-setting catalog; never execute its CLI examples."""
from dataclasses import dataclass, field
from decimal import Decimal, InvalidOperation
from pathlib import Path
import json
import re
import math


GUARDS = {'hot_edit_confirmed', 'hot_edit_guarded', 'runtime_command',
          'standby_required', 'commissioning_only', 'service_only'}
INTERLOCKS = {'hot_edit_guarded': 'guarded_interlock',
              'commissioning_only': 'commissioning_interlock', 'service_only': 'service_interlock'}


@dataclass
class Setting:
    name: str
    description: str
    domain: str
    value_type: str
    guard: str
    meaning: str
    choices: dict = field(default_factory=dict)
    notes: str = ''
    raw_range: str = ''
    raw_unit: str = ''
    bounds: tuple | None = None
    step: Decimal | None = None
    encoding_bounds: tuple | None = None
    blocked: str = ''
    readable: bool = False

    @property
    def key(self):
        return self.name + ' | ' + self.description

    def parse(self, text):
        if self.blocked:
            raise ValueError(self.blocked)
        if self.choices:
            if text not in self.choices:
                raise ValueError('Select one of the documented choices')
            return self.choices[text]
        if re.fullmatch(r'PeriodTime(?:Start|End)\d+', self.name):
            # The API explicitly requires packed hour/minute application
            # encoding. Offer HH:MM in the GUI and validate either notation.
            if re.fullmatch(r'\d{1,2}:\d{2}', text.strip()):
                hour, minute = map(int, text.strip().split(':'))
                if 0 <= hour <= 23 and 0 <= minute <= 59:
                    return hour * 256 + minute
            elif text.strip().isdigit():
                value = int(text.strip())
                if 0 <= value <= 0x173B and value % 256 <= 59:
                    return value
            raise ValueError('Enter a valid time as HH:MM (00:00 to 23:59)')
        if self.value_type.startswith('ascii') and (self.value_type != 'ascii_or_uint16' or not text.strip().isdigit()):
            if self.value_type == 'ascii_or_uint16' and text.startswith('"'):
                try:
                    text = json.loads(text)
                except json.JSONDecodeError:
                    raise ValueError('Enter a quoted two-character ASCII cell or an integer') from None
            # API strings are two-character ASCII cells, not entire serials.
            if len(text) != 2 or not text.isascii() or not text.isprintable():
                raise ValueError('Enter exactly two printable ASCII characters')
            return text
        try:
            number = Decimal(text.strip())
        except InvalidOperation:
            raise ValueError('Enter a numeric engineering value') from None
        if not number.is_finite():
            raise ValueError('Value must be finite')
        if self.bounds and not self.bounds[0] <= number <= self.bounds[1]:
            raise ValueError(f'Allowed application range: {self.bounds[0]} to {self.bounds[1]}')
        if self.encoding_bounds and not self.encoding_bounds[0] <= number <= self.encoding_bounds[1]:
            raise ValueError(f'Value cannot be encoded by this setting ({self.encoding_bounds[0]} to {self.encoding_bounds[1]})')
        if self.step and number % self.step:
            raise ValueError(f'Value must be a multiple of {self.step}')
        if self.value_type == 'ascii_or_uint16' and (number != number.to_integral_value() or not 0 <= number <= 65535):
            raise ValueError('Numeric value must be an integer from 0 to 65535')
        # Avoid non-finite floats and unbounded integer serialization for
        # entries whose model-specific range is not available in the catalog.
        if abs(number) > Decimal('1e15'):
            raise ValueError('Value exceeds the supported numeric magnitude')
        value = int(number) if number == number.to_integral_value() else float(number)
        if not math.isfinite(value):
            raise ValueError('Value must be finite')
        return value


def load_catalog(path=None):
    source = Path(path or Path(__file__).with_name('MQTT_Register_Command_API.md')).read_text(encoding='utf-8')
    catalog = {}
    readable = set()
    for line in source.splitlines():
        if not line.startswith('| '):
            continue
        cols = line.strip().strip('|').split(' | ')
        cols = [c.strip() for c in cols]
        if len(cols) == 7 and cols[4] in GUARDS and '"operation":"set"' in cols[6]:
            name, description, domain, value_type, guard, meaning, _ = cols
            item = Setting(name, description, domain, value_type, guard, meaning)
            if item.key in catalog:
                raise ValueError(f'Duplicate catalog setting: {item.key}')
            catalog[item.key] = item
        if '"category":"configuration"' in line and '"operation":"get"' in line:
            readable.add(cols[0])
    # Choice-specific commands contain the authoritative application enum value.
    choice_part = source.split('### Enumerated setter choices', 1)[1].split('## Detailed setter', 1)[0]
    for line in choice_part.splitlines():
        match = re.search(r'\{[^\n]*"operation":"set"[^\n]*\}', line)
        if not match:
            continue
        command = json.loads(match.group())
        label = line.split('|')[1].strip()
        if re.search(r'TBD|not validated|reserved|unused|invalid', label, re.I):
            continue
        for item in catalog.values():
            if item.name == command['name'] and command.get('description', item.description) == item.description:
                item.choices[label] = command['value']
    recipes = re.split(r'^### Setter \d+ - ', source, flags=re.M)[1:]
    for recipe in recipes:
        heading, _, body = recipe.partition('\n')
        name, description = heading.split(': ', 1)
        item = catalog.get(name + ' | ' + description.strip())
        if item is None:
            continue
        item.readable = name in readable
        item.notes = body.split('Confirmation command', 1)[0].strip()
        match = re.search(r'map range: (.*?); map unit: (.*?)\. These source', item.notes)
        if match:
            item.raw_range, item.raw_unit = match.groups()
        if re.search(r'\bReserved\b|\bUnused\b', item.meaning, re.I):
            item.blocked = 'Catalog marks this item reserved/unused; no validated write is available.'
        # Only infer engineering bounds for a single explicit range and known
        # linear source unit. Complex/model-dependent ranges remain visible.
        match = re.fullmatch(r'\s*(-?\d+(?:\.\d+)?)\s*(?:~|～|-)\s*(-?\d+(?:\.\d+)?)\s*', item.raw_range)
        unit = item.raw_unit.strip()
        factor = Decimal(1) if unit in ('', '-', 'Year', 'Month', 'Day', 'Hour', 'Minutes', 'Seconds') else None
        scale = re.fullmatch(r'(0\.\d+|\d+)(V|A|Hz|W|ms|s|%|Pn%|%Pn|Pn/min|Pn%/min|min|DAY)?', unit)
        if scale:
            factor = Decimal(scale[1])
        if factor is not None and not item.choices and not item.value_type.startswith('ascii'):
            item.step = factor
            raw_limits = {'uint8': (0, 255), 'uint16': (0, 65535), 'sint16': (-32768, 32767)}
            limits = raw_limits.get(item.value_type.split(';', 1)[0])
            if limits:
                item.encoding_bounds = tuple(Decimal(limit) * factor for limit in limits)
        if match and factor is not None and not item.choices and not item.value_type.startswith('ascii'):
            # Prefer an explicit engineering-value boundary example when one
            # exists (some catalog source units do not describe API scaling).
            example = re.search(r'Range-boundary illustration[\s\S]*?"value":(-?\d+(?:\.\d+)?)', body)
            if example and Decimal(match[1]) != 0:
                factor = abs(Decimal(example[1]) / Decimal(match[1]))
            item.bounds = tuple(sorted((Decimal(match[1]) * factor, Decimal(match[2]) * factor)))
            item.step = factor
        if name == 'Year':
            item.bounds, item.step = (Decimal(2022), Decimal(2050)), Decimal(1)
        if name == 'Hour':
            item.bounds = (Decimal(0), Decimal(23))
        if name == 'OnOffSet':
            item.choices = {k: v for k, v in item.choices.items() if v in (0, 1)}
        if name == 'NumberTimePeriods':
            item.choices = {str(count): count for count in range(11)}
            item.choices['Reset all time-period settings'] = 165
        if name == 'ChargeSourcePriority':
            item.choices = {'CSO - Solar first': 0, 'SNU - Solar + utility charging': 1, 'OSO - Solar charging only': 2}
        if name == 'SourcePriority':
            item.choices = {'Sol - Solar first': 0, 'UTI - Utility/grid first': 1,
                            'SBU - Solar, battery, then utility': 2, 'Grid connected output mode': 10}
    if len(catalog) != 201 or len(recipes) != 201:
        raise ValueError(f'Catalog coverage changed: {len(catalog)} setters / {len(recipes)} recipes; review the API document')
    return catalog
