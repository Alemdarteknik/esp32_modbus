/*
 * Portable MPP/Voltronic PI codecs. Command layouts are derived from the MIT
 * jblance/mpp-solar project; see pi_protocol_catalog.generated.inc and the
 * upstream permission notice in third_party/mpp-solar/LICENSE.
 *
 * Each PI variant has its own resolved catalog and wire policy. Monitoring
 * requests and settings use the same codec over either HID or serial.
 */
#include "inverter_gateway/protocol/pi_protocols.hpp"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace inverter_gateway::protocol {
namespace {

constexpr std::size_t maximum_command_length = 128;
constexpr std::size_t maximum_frame_length = 2048;
constexpr std::size_t maximum_regex_nodes = 192;
constexpr std::size_t maximum_tokens = 96;

enum class WireFamily : std::uint8_t { pi16, pi17, pi18, pi30, revo };
enum class FieldKind : std::uint8_t {
    integer, number, text, option, keyed, digit, enabled, trim6, trim7,
    serial_length, command_value, year, month, day, date_time,
};
struct Lookup { const char *key; const char *value; };
struct RegexNode {
    // nullptr is epsilon/accept, "" is any printable character.
    const char *characters;
    std::int16_t first;
    std::int16_t second;
};
struct FieldDefinition {
    std::uint8_t token;
    std::int16_t character;
    FieldKind kind;
    const char *label;
    const char *unit;
    double scale;
    const Lookup *lookup;
    std::uint8_t lookup_count;
};
struct CommandDefinition {
    const char *name;
    const char *key;
    const RegexNode *pattern;
    std::uint16_t pattern_count;
    std::uint16_t pattern_start;
    const FieldDefinition *fields;
    std::uint8_t field_count;
    bool setter;
    const char *readback;
    const char *prefix;
    std::int8_t no_crc;
    bool energy_query;
    bool ascii_checksum;
    bool revo_checksum;
    const char *flag_codes;
};
struct ProfileDefinition {
    EnergyProtocol id;
    WireFamily wire;
    bool default_no_crc;
    const CommandDefinition *const *commands;
    std::uint16_t command_count;
    const char *const *poll;
    std::uint8_t poll_count;
};

#include "pi_protocol_catalog.generated.inc"

const ProfileDefinition *profile_for(EnergyProtocol id)
{
    for (const auto &profile : profiles) {
        if (profile.id == id) return &profile;
    }
    return nullptr;
}

bool valid_command_text(const char *command, std::size_t &size)
{
    size = 0;
    if (!command) return false;
    while (size <= maximum_command_length && command[size]) {
        const auto character = static_cast<unsigned char>(command[size]);
        if (character < 0x20 || character > 0x7e) return false;
        ++size;
    }
    return size > 0 && size <= maximum_command_length;
}

void add_closure(bool *active, const RegexNode *nodes, std::size_t count,
                 std::int16_t initial)
{
    std::int16_t pending[maximum_regex_nodes];
    std::size_t used = 0;
    auto add = [&](std::int16_t node) {
        if (node >= 0 && static_cast<std::size_t>(node) < count && !active[node]) {
            active[node] = true;
            pending[used++] = node;
        }
    };
    add(initial);
    while (used) {
        const auto &node = nodes[pending[--used]];
        if (!node.characters) {
            add(node.first);
            add(node.second);
        }
    }
}

bool matches(const CommandDefinition &definition, const char *command,
             std::size_t length)
{
    if (!definition.pattern) return std::strcmp(definition.key, command) == 0;
    if (definition.pattern_count > maximum_regex_nodes) return false;
    bool current[maximum_regex_nodes]{};
    bool next[maximum_regex_nodes]{};
    add_closure(current, definition.pattern, definition.pattern_count,
                static_cast<std::int16_t>(definition.pattern_start));
    for (std::size_t i = 0; i < length; ++i) {
        std::memset(next, 0, sizeof(next));
        for (std::size_t n = 0; n < definition.pattern_count; ++n) {
            const auto &node = definition.pattern[n];
            if (current[n] && node.characters &&
                (!node.characters[0] || std::strchr(node.characters, command[i]))) {
                add_closure(next, definition.pattern, definition.pattern_count, node.first);
            }
        }
        std::memcpy(current, next, sizeof(current));
    }
    // Generator reserves node 0 for the accepting state.
    return current[0];
}

int decimal_digits(const char *value, std::size_t count)
{
    int result = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (value[i] < '0' || value[i] > '9') return -1;
        result = result * 10 + value[i] - '0';
    }
    return result;
}

bool valid_date(const char *value, std::size_t size)
{
    if (size != 12 && size != 14) return false;
    const std::size_t year_digits = size == 14 ? 4 : 2;
    int year = decimal_digits(value, year_digits);
    if (year < 0) return false;
    if (size == 12) year += 2000;
    const int month = decimal_digits(value + year_digits, 2);
    const int day = decimal_digits(value + year_digits + 2, 2);
    const int hour = decimal_digits(value + year_digits + 4, 2);
    const int minute = decimal_digits(value + year_digits + 6, 2);
    const int second = decimal_digits(value + year_digits + 8, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    static constexpr int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

bool valid_setter_value(const CommandDefinition &definition, const char *command)
{
    if (!definition.setter) return true;
    const std::size_t prefix_length = std::strlen(definition.key);
    const char *value = command + prefix_length;
    if (definition.flag_codes[0]) {
        bool seen[128]{};
        if (!*value) return false;
        for (const char *p = value; *p; ++p) {
            const auto character = static_cast<unsigned char>(*p);
            if (!std::strchr(definition.flag_codes, *p) || seen[character]) return false;
            seen[character] = true;
        }
    }
    if (std::strcmp(definition.key, "DAT") == 0) {
        return valid_date(value, std::strlen(value));
    }
    if (std::strcmp(definition.key, "PSET") == 0 || std::strcmp(definition.key, "_PSET") == 0) {
        // REVO's last two-digit item is a settings flag, not seconds. Validate
        // the separate date/hour/minute fields before any bulk write.
        const char *p = value;
        for (int token = 0; token < 7; ++token) {
            p = std::strchr(p, ' ');
            if (!p) return false;
            ++p;
        }
        char date[15]{};
        const std::size_t widths[] = {4, 2, 2, 2, 2};
        std::size_t used = 0;
        for (const auto width : widths) {
            std::memcpy(date + used, p, width);
            used += width;
            p += width;
            if (*p++ != ' ') return false;
        }
        date[12] = '0'; date[13] = '0';
        return valid_date(date, 14);
    }
    return true;
}

const CommandDefinition *find_command(const ProfileDefinition &profile,
                                      const char *command)
{
    std::size_t length = 0;
    if (!valid_command_text(command, length)) return nullptr;
    // Exact queries take priority over a setter with a shared command prefix.
    for (std::size_t i = 0; i < profile.command_count; ++i) {
        const auto &definition = *profile.commands[i];
        if (!definition.pattern && matches(definition, command, length) &&
            valid_setter_value(definition, command)) return &definition;
    }
    for (std::size_t i = 0; i < profile.command_count; ++i) {
        const auto &definition = *profile.commands[i];
        if (definition.pattern && matches(definition, command, length) &&
            valid_setter_value(definition, command)) return &definition;
    }
    return nullptr;
}

std::uint16_t xmodem_crc(const std::uint8_t *data, std::size_t size)
{
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    return crc;
}

std::uint8_t escaped_crc_byte(std::uint8_t value)
{
    return value == 0x00 || value == 0x0a || value == 0x0d || value == 0x28
        ? static_cast<std::uint8_t>(value + 1) : value;
}

bool valid_crc(const std::uint8_t *data, std::size_t size, bool escaped)
{
    if (size < 4) return false;
    const auto crc = xmodem_crc(data, size - 3);
    auto high = static_cast<std::uint8_t>(crc >> 8);
    auto low = static_cast<std::uint8_t>(crc);
    if (escaped) {
        high = escaped_crc_byte(high);
        low = escaped_crc_byte(low);
    }
    return high == data[size - 3] && low == data[size - 2];
}

std::uint8_t byte_sum(const std::uint8_t *data, std::size_t size)
{
    std::uint8_t result = 0;
    for (std::size_t i = 0; i < size; ++i) result = static_cast<std::uint8_t>(result + data[i]);
    return result;
}

bool valid_revo_checksum(const std::uint8_t *data, std::size_t size)
{
    return size >= 3 && data[size - 2] == static_cast<std::uint8_t>(byte_sum(data, size - 2) + 1);
}

bool printable(const std::uint8_t *data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] < 0x20 || data[i] > 0x7e) return false;
    }
    return true;
}

CodecStatus payload(const ProfileDefinition &profile, const std::uint8_t *data,
                    std::size_t size, const std::uint8_t *&begin, std::size_t &length)
{
    begin = nullptr;
    length = 0;
    if (!data || size < 2 || size > maximum_frame_length) return CodecStatus::invalid_frame;
    const bool caret = profile.wire == WireFamily::pi17 || profile.wire == WireFamily::pi18;
    if (data[size - 1] != '\r' && !(profile.wire == WireFamily::revo && data[size - 1] == '\n')) {
        return CodecStatus::invalid_frame;
    }
    std::size_t trailer = 3;
    if (profile.wire == WireFamily::revo) {
        if (valid_crc(data, size, true)) trailer = 3;
        else if (valid_revo_checksum(data, size)) trailer = 2;
        else return CodecStatus::checksum_error;
    } else if (profile.wire == WireFamily::pi16) {
        // PI16 specifies optional reply CRC. A present binary trailer is always
        // checked, and a fully printable reply can carry its documented plain
        // form. Missing numeric values stay absent instead of becoming zero.
        if (valid_crc(data, size, true)) trailer = 3;
        else if (size >= 2 && data[0] == '(' && printable(data + 1, size - 2)) trailer = 1;
        else return CodecStatus::checksum_error;
    } else if (!valid_crc(data, size, profile.wire != WireFamily::pi17)) {
        return CodecStatus::checksum_error;
    }
    if (size <= trailer) return CodecStatus::invalid_frame;
    if (caret && data[0] == '^') {
        if (size == 5 && (data[1] == '0' || data[1] == '1')) {
            begin = data + 1;
            length = 1;
        } else {
            if (size < 8 || data[1] != 'D') return CodecStatus::invalid_frame;
            const int declared = decimal_digits(reinterpret_cast<const char *>(data + 2), 3);
            if (declared < 3 || size != static_cast<std::size_t>(declared + 5)) return CodecStatus::invalid_frame;
            begin = data + 5;
            length = size - 5 - trailer;
        }
    } else if (data[0] == '(') {
        begin = data + 1;
        length = size - 1 - trailer;
        // Some devices use the ordinary CRC-wrapped ACK form for PI18 writes.
        if (caret && !(length == 3 &&
            (std::memcmp(begin, "ACK", 3) == 0 || std::memcmp(begin, "NAK", 3) == 0))) {
            return CodecStatus::invalid_frame;
        }
    } else if (profile.wire == WireFamily::revo) {
        begin = data;
        length = size - trailer;
    } else {
        return CodecStatus::invalid_frame;
    }
    return printable(begin, length) ? CodecStatus::ok : CodecStatus::invalid_frame;
}

struct Token { const std::uint8_t *begin; std::size_t size; };

bool copy_token(const Token &token, char *out, std::size_t capacity)
{
    if (token.size >= capacity) return false;
    std::memcpy(out, token.begin, token.size);
    out[token.size] = '\0';
    return true;
}

bool is_missing(const char *value)
{
    if (!*value) return true;
    bool dash = false;
    for (const char *p = value; *p; ++p) {
        if (*p == '-') dash = true;
        else if (*p != '.') return false;
    }
    return dash;
}

bool number_value(const char *text, bool integer, double &value)
{
    const char *p = text;
    if (*p == '+' || *p == '-') ++p;
    bool digit = false;
    bool point = false;
    while (*p) {
        if (*p >= '0' && *p <= '9') digit = true;
        else if (*p == '.' && !integer && !point) point = true;
        else return false;
        ++p;
    }
    if (!digit) return false;
    errno = 0;
    char *end = nullptr;
    value = std::strtod(text, &end);
    return errno != ERANGE && end && !*end && std::isfinite(value);
}

const char *lookup_value(const FieldDefinition &definition, const char *key)
{
    for (std::size_t i = 0; i < definition.lookup_count; ++i) {
        if (std::strcmp(definition.lookup[i].key, key) == 0) return definition.lookup[i].value;
    }
    return key;
}

CodecStatus decode_field(const FieldDefinition &field, const Token *tokens,
                         std::size_t count, const char *command_value,
                         EnergyResponse &response)
{
    char text[160]{};
    const bool info = field.kind == FieldKind::command_value || field.kind == FieldKind::year ||
                      field.kind == FieldKind::month || field.kind == FieldKind::day;
    if (!info) {
        if (field.token >= count) return CodecStatus::ok;
        if (!copy_token(tokens[field.token], text, sizeof(text))) return CodecStatus::invalid_frame;
        if (field.kind == FieldKind::enabled) {
            bool enabled = false;
            bool seen = false;
            for (const char *p = text; *p; ++p) {
                if (*p == 'E') enabled = true;
                else if (*p == 'D') enabled = false;
                else if (*p == field.character) {
                    seen = true;
                    break;
                }
            }
            if (!seen) return CodecStatus::ok;
            return response.add_number(field.label, field.unit, enabled ? 1 : 0)
                ? CodecStatus::ok : CodecStatus::buffer_too_small;
        }
        if (field.character >= 0) {
            if (static_cast<std::size_t>(field.character) >= tokens[field.token].size) return CodecStatus::ok;
            text[0] = text[field.character];
            text[1] = '\0';
        }
        if (is_missing(text)) return CodecStatus::ok;
    }
    switch (field.kind) {
    case FieldKind::integer:
    case FieldKind::number:
    case FieldKind::digit: {
        double value = 0;
        if (!number_value(text, field.kind != FieldKind::number, value)) return CodecStatus::invalid_frame;
        if (field.kind == FieldKind::digit && value != 0 && value != 1) return CodecStatus::invalid_frame;
        return response.add_number(field.label, field.unit, value * field.scale)
            ? CodecStatus::ok : CodecStatus::buffer_too_small;
    }
    case FieldKind::option: {
        double value = 0;
        if (!number_value(text, true, value) || value < 0 || value > 9999) return CodecStatus::invalid_frame;
        char key[8];
        std::snprintf(key, sizeof(key), "%d", static_cast<int>(value));
        return response.add_text(field.label, field.unit, lookup_value(field, key))
            ? CodecStatus::ok : CodecStatus::buffer_too_small;
    }
    case FieldKind::keyed:
        return response.add_text(field.label, field.unit, lookup_value(field, text))
            ? CodecStatus::ok : CodecStatus::buffer_too_small;
    case FieldKind::trim6:
    case FieldKind::trim7: {
        const auto prefix = field.kind == FieldKind::trim6 ? 6U : 7U;
        if (std::strlen(text) < prefix) return CodecStatus::invalid_frame;
        std::memmove(text, text + prefix, std::strlen(text + prefix) + 1);
        break;
    }
    case FieldKind::serial_length: {
        const int size = decimal_digits(text, 2);
        if (size < 0 || std::strlen(text) < static_cast<std::size_t>(size + 2)) return CodecStatus::invalid_frame;
        std::memmove(text, text + 2, static_cast<std::size_t>(size));
        text[size] = '\0';
        break;
    }
    case FieldKind::command_value:
        std::snprintf(text, sizeof(text), "%s", command_value);
        break;
    case FieldKind::year:
        if (std::strlen(command_value) < 4) return CodecStatus::invalid_command;
        std::memcpy(text, command_value, 4);
        text[4] = '\0';
        break;
    case FieldKind::month: {
        static constexpr const char *months[] = {"", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
        if (std::strlen(command_value) < 6) return CodecStatus::invalid_command;
        const int month = decimal_digits(command_value + 4, 2);
        if (month < 1 || month > 12) return CodecStatus::invalid_command;
        std::snprintf(text, sizeof(text), "%s", months[month]);
        break;
    }
    case FieldKind::day:
        if (std::strlen(command_value) < 8) return CodecStatus::invalid_command;
        std::snprintf(text, sizeof(text), "%s", command_value + 6);
        break;
    case FieldKind::date_time: {
        if (!valid_date(text, std::strlen(text)) || std::strlen(text) != 14) return CodecStatus::invalid_frame;
        char formatted[24];
        std::snprintf(formatted, sizeof(formatted), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                      text, text + 4, text + 6, text + 8, text + 10, text + 12);
        std::memcpy(text, formatted, std::strlen(formatted) + 1);
        break;
    }
    case FieldKind::text:
        // PI17 BMS labels its numeric percentage as a string upstream. Admit
        // only a finite in-range observation; unknown text stays diagnostic.
        if (!std::strcmp(field.label, "soc") && !std::strcmp(field.unit, "%")) {
            double value = 0;
            if (number_value(text, false, value) && value >= 0 && value <= 100)
                return response.add_number(field.label, field.unit, value)
                    ? CodecStatus::ok : CodecStatus::buffer_too_small;
        }
        break;
    case FieldKind::enabled:
        return CodecStatus::invalid_frame;
    }
    return response.add_text(field.label, field.unit, text)
        ? CodecStatus::ok : CodecStatus::buffer_too_small;
}

int parallel_unit(const ProfileDefinition &profile, const CommandDefinition &definition,
                  const char *command)
{
    const auto *key = definition.key;
    const bool pi30 = profile.wire == WireFamily::pi30;
    const bool pi18 = profile.wire == WireFamily::pi18;
    if ((pi30 && (!std::strcmp(key, "MCHGC") || !std::strcmp(key, "MNCHGC") ||
                  !std::strcmp(key, "MUCHGC") || !std::strcmp(key, "POPM") ||
                  !std::strcmp(key, "PPCP"))) ||
        (pi18 && (!std::strcmp(key, "MCHGC") || !std::strcmp(key, "MUCHGC") ||
                  !std::strcmp(key, "PCP")))) {
        return command[std::strlen(key)] - '0';
    }
    return -1;
}

void comparison_key(char (&out)[40], const char *label)
{
    // Match EnergyResponse's public normalized field identity, including its
    // hash suffix for long labels, without allocating another 9 KB response.
    std::uint32_t hash = 2166136261U;
    char full[192]{};
    std::size_t n = 0;
    bool separator = false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(label); *p; ++p) {
        hash = (hash ^ *p) * 16777619U;
        if (std::isalnum(*p)) {
            if (separator && n && n < sizeof(full) - 1) full[n++] = '_';
            separator = false;
            if (n < sizeof(full) - 1) full[n++] = static_cast<char>(std::tolower(*p));
        } else separator = true;
    }
    if (!n) std::strcpy(out, "value");
    else if (n < sizeof(out)) std::memcpy(out, full, n + 1);
    else std::snprintf(out, sizeof(out), "%.30s_%08x", full, static_cast<unsigned>(hash));
}

void observed_text(char (&out)[32], const char *text)
{
    std::snprintf(out, sizeof(out), "%s", text);
    if (std::strlen(text) >= sizeof(out)) std::memcpy(out + sizeof(out) - 4, "...", 4);
}

struct SettingComparison {
    const CommandDefinition &query;
    const EnergyResponse &response;
    bool available = true;
    bool equal = true;
    unsigned checked = 0;

    const FieldDefinition *definition(unsigned token, int character = -1) const
    {
        for (unsigned i = 0; i < query.field_count; ++i) {
            if (query.fields[i].token == token && query.fields[i].character == character)
                return &query.fields[i];
        }
        return nullptr;
    }
    const EnergyField *observation(const FieldDefinition *field)
    {
        if (!field) { available = false; return nullptr; }
        char key[40]{}; comparison_key(key, field->label);
        const EnergyField *found = nullptr;
        for (std::size_t i = 0; i < response.count; ++i) {
            if (std::strcmp(response.fields[i].key, key)) continue;
            if (found) { available = false; return nullptr; }
            found = &response.fields[i];
        }
        char unit[8]{}; std::snprintf(unit, sizeof(unit), "%s", field->unit);
        if (!found || std::strcmp(found->unit, unit)) { available = false; return nullptr; }
        ++checked;
        return found;
    }
    void number(unsigned token, double expected, int character = -1)
    {
        const auto *field = definition(token, character);
        const auto *observed = observation(field);
        if (!observed) return;
        if (!observed->numeric || !std::isfinite(observed->value)) { available = false; return; }
        equal = equal && std::fabs(observed->value - expected) < 0.000001;
    }
    void text(unsigned token, const char *expected)
    {
        const auto *observed = observation(definition(token));
        if (!observed) return;
        if (observed->numeric) { available = false; return; }
        char encoded[32]{}; observed_text(encoded, expected);
        equal = equal && !std::strcmp(observed->text, encoded);
    }
    void code(unsigned token, int expected, const char *raw = nullptr)
    {
        const auto *field = definition(token);
        if (!field || (field->kind != FieldKind::option && field->kind != FieldKind::keyed)) {
            available = false; return;
        }
        char digits[16]{}; std::snprintf(digits, sizeof(digits), "%d", expected);
        const char *encoded = raw ? raw : digits;
        const char *label = lookup_value(*field, encoded);
        if (label == encoded) { available = false; return; }
        char truncated[32]{}; observed_text(truncated, label);
        // Long option labels are truncated on the wire. They verify a value
        // only if their displayed representation still identifies it uniquely.
        for (unsigned i = 0; i < field->lookup_count; ++i) {
            if (!std::strcmp(field->lookup[i].key, encoded)) continue;
            char other[32]{}; observed_text(other, field->lookup[i].value);
            if (!std::strcmp(truncated, other)) { available = false; return; }
        }
        text(token, label);
    }
    void flag(int character, double expected)
    {
        number(0, expected, character);
    }
};

bool numeric_parts(const char *text, double *values, std::size_t count, char separator = ',')
{
    for (std::size_t i = 0; i < count; ++i) {
        char item[24]{};
        const char *end = std::strchr(text, separator);
        const auto size = end ? static_cast<std::size_t>(end - text) : std::strlen(text);
        if (!size || size >= sizeof(item) || (end == nullptr) != (i + 1 == count)) return false;
        std::memcpy(item, text, size);
        if (!number_value(item, false, values[i])) return false;
        if (end) text = end + 1;
    }
    return true;
}

} // namespace

std::size_t pi_poll_command_count(EnergyProtocol id)
{
    const auto *profile = profile_for(id);
    return profile ? profile->poll_count : 0;
}

const char *pi_poll_command(EnergyProtocol id, std::size_t index)
{
    const auto *profile = profile_for(id);
    return profile && index < profile->poll_count ? profile->poll[index] : nullptr;
}

bool pi_command_info(EnergyProtocol id, const char *command, EnergyCommandInfo &info)
{
    info = {};
    const auto *profile = profile_for(id);
    const auto *definition = profile ? find_command(*profile, command) : nullptr;
    if (!definition) return false;
    info.name = definition->name;
    info.setter = definition->setter;
    info.readback_command = definition->readback;
    const int unit = parallel_unit(*profile, *definition, command);
    if (definition->setter && unit >= 0) {
        static constexpr const char *pi30_queries[] = {
            "QPGS0", "QPGS1", "QPGS2", "QPGS3", "QPGS4", "QPGS5", "QPGS6", "QPGS7", "QPGS8", "QPGS9"};
        static constexpr const char *pi18_queries[] = {
            "PRI0", "PRI1", "PRI2", "PRI3", "PRI4", "PRI5", "PRI6", "PRI7", "PRI8", "PRI9"};
        const auto *query = profile->wire == WireFamily::pi18 ? pi18_queries[unit] : pi30_queries[unit];
        info.readback_command = find_command(*profile, query) ? query : nullptr;
    }
    return true;
}

bool pi_setting_readback(EnergyProtocol id, const char *set_command,
    const char *read_command, const EnergyResponse &response, bool &matches)
{
    matches = false;
    EnergyCommandInfo info{};
    if (!pi_command_info(id, set_command, info) || !info.setter ||
        !info.readback_command || !read_command || std::strcmp(info.readback_command, read_command) ||
        response.rejected || response.acknowledged || response.count > EnergyResponse::maximum_fields) return false;
    const auto *profile = profile_for(id);
    const auto *setting = find_command(*profile, set_command);
    const auto *query = find_command(*profile, read_command);
    if (!query || query->setter) return false;
    const auto *key = setting->key;
    const auto *value = set_command + std::strlen(key);
    SettingComparison compare{*query, response};
    auto is = [&](const char *name) { return !std::strcmp(key, name); };
    double parts[4]{};
    auto scalar = [&]() { return numeric_parts(value, parts, 1); };
    const int unit = parallel_unit(*profile, *setting, set_command);
    if (profile->wire == WireFamily::pi17) {
        if (is("PA") || is("PB") || is("PC") || is("PF")) {
            compare.code(is("PF") ? 5 : static_cast<unsigned>(key[1] - 'A'), value[0] - '0');
        } else if (is("PEA") || is("PDA")) compare.code(0, key[1] == 'E' ? 1 : 0);
        else if (is("PE") || is("PD")) {
            const int code = value[0] - 'A';
            if (code >= 0 && code <= 10) compare.code(code, key[1] == 'E' ? 1 : 0);
            else if (value[0] == 'M') compare.code(11, key[1] == 'E' ? 1 : 0);
            else if (value[0] == 'N') compare.code(12, key[1] == 'E' ? 1 : 0);
            else return false;
        } else if (is("LON")) compare.code(16, value[0] - '0');
        else if (is("GPMP0") && scalar()) compare.number(0, parts[0]);
        else if (is("LST") && scalar()) compare.number(9, parts[0] * 30);
        else if (is("SEP") && scalar()) compare.code(0, static_cast<int>(parts[0]), value);
        else if (!std::strncmp(key, "ED", 2) && std::strlen(key) == 3) compare.code(key[2] - 'A' + 1, value[0] - '0');
        else if (is("BT")) compare.code(10, value[0] - '0');
        else if (is("MCHGV") && numeric_parts(value, parts, 2)) {
            compare.number(1, parts[0] * 0.1); compare.number(2, parts[1] * 0.1);
        } else if (is("MCHGC") && scalar()) compare.number(0, parts[0] * 0.1);
        else if (is("MUCHGC") && scalar()) compare.number(16, parts[0] * 0.1);
        else if (is("BDCM") && scalar()) compare.number(17, parts[0]);
        else if (is("BATDV") && numeric_parts(value, parts, 4)) {
            for (unsigned i = 0; i < 4; ++i) compare.number(6 + i, parts[i] * 0.1);
        } else if (is("ACCB") && numeric_parts(value, parts, 2)) {
            compare.code(13, static_cast<int>(parts[0])); compare.number(14, parts[1] * 0.1);
        } else if (is("BCA") && numeric_parts(value, parts, 3)) {
            compare.number(3, parts[0] * 0.1); compare.number(4, parts[1]); compare.number(5, parts[2] * 0.1);
        } else if (is("ACCT") && id != EnergyProtocol::pi17infini) {
            char start[5]{}; std::memcpy(start, value, 4);
            compare.text(16, start); compare.text(17, value + 5);
        } else return false;
    } else if (profile->wire == WireFamily::pi18) {
        if (std::strlen(key) == 3 && key[0] == 'P' && (key[1] == 'E' || key[1] == 'D') && key[2] >= 'A' && key[2] <= 'H') {
            compare.code(key[2] - 'A', key[1] == 'E' ? 1 : 0);
        } else if (is("LON")) compare.code(23, value[0] - '0');
        else if (is("PBT") && scalar()) compare.code(13, static_cast<int>(parts[0]));
        else if (is("POP") && scalar()) compare.code(17, static_cast<int>(parts[0]));
        else if (is("PSP") && scalar()) compare.code(23, static_cast<int>(parts[0]));
        else if (is("PCP")) compare.code(3, value[2] - '0');
        else if ((is("MCHGC") || is("MUCHGC")) && numeric_parts(value + 2, parts, 1)) {
            compare.number(is("MCHGC") ? 4 : 5, parts[0]);
        } else if (is("MCHGV") && numeric_parts(value, parts, 2)) {
            compare.number(11, parts[0] * 0.1); compare.number(12, parts[1] * 0.1);
        } else if (is("PSDV") && scalar()) compare.number(10, parts[0] * 0.1);
        else if (is("BUCD") && numeric_parts(value, parts, 2)) {
            compare.number(8, parts[0] * 0.1); compare.number(9, parts[1] * 0.1);
        } else return false;
        if (unit >= 0) compare.code(0, 1); // Nonexistent parallel peers cannot verify settings.
    } else if (profile->wire == WireFamily::pi30) {
        if (is("PE") || is("PD")) {
            for (const char *p = value; *p; ++p) compare.flag(*p, key[1] == 'E' ? 1 : 0);
        } else if (is("MCHGC") || is("MNCHGC") || is("MUCHGC")) {
            if (!numeric_parts(value + 1, parts, 1)) return false;
            const bool utility = !is("MCHGC");
            compare.number(utility ? 24 : 22, parts[0]);
        } else if (is("POPM")) compare.code(20, value[1] - '0');
        else if (is("PPCP")) compare.code(21, decimal_digits(value + 1, 2));
        else if ((is("F") || is("PBCV") || is("PBDV") || is("PBFT") || is("PCVV") || is("PSDV")) && scalar()) {
            compare.number(is("F") ? 3 : is("PBCV") ? 8 : is("PBDV") ? 22 : is("PBFT") ? 11 : is("PCVV") ? 10 : 9, parts[0]);
        } else if ((is("PBT") || is("PCP") || is("PGR") || is("POP") || is("POPLG") || is("PPVOKC") || is("PSPB")) && scalar()) {
            compare.code(is("PBT") ? 12 : is("PCP") ? 17 : is("PGR") ? 15 : is("POP") ? 16 : is("POPLG") ? 26 : is("PPVOKC") ? 23 : 24, static_cast<int>(parts[0]));
        } else if (is("PBATMAXDISC") && scalar() && parts[0] > 0) compare.number(9, parts[0]);
        else if ((is("PLEDE") || is("PLEDS") || is("PLEDM")) && scalar()) {
            compare.code(is("PLEDE") ? 0 : is("PLEDS") ? 1 : 2, static_cast<int>(parts[0]));
        } else if ((is("PLEDB") || is("PLEDT")) && scalar()) compare.number(is("PLEDB") ? 3 : 4, parts[0]);
        else return false;
        if (unit >= 0) compare.code(0, 1);
    } else return false;
    matches = compare.equal && compare.available && compare.checked;
    return compare.available && compare.checked;
}

CodecStatus pi_build_request(EnergyProtocol id, const char *command,
    std::uint8_t /* address */, std::uint8_t *out, std::size_t capacity, std::size_t &size)
{
    size = 0;
    const auto *profile = profile_for(id);
    if (!profile) return CodecStatus::unsupported;
    const auto *definition = find_command(*profile, command);
    if (!definition) return CodecStatus::invalid_command;
    const auto command_length = std::strlen(command);
    // A command is bounded to 128 bytes above; leave room for the full unsigned
    // formatting range as well so every compiler can prove snprintf is safe.
    char prefix[16]{};
    std::size_t prefix_length = 0;
    bool crc = profile->wire == WireFamily::pi30 || profile->wire == WireFamily::revo;
    bool ascii_sum = false;
    bool revo_sum = false;
    bool terminate = true;
    if (profile->wire == WireFamily::pi16) {
        ascii_sum = definition->ascii_checksum;
    } else if (profile->wire == WireFamily::pi17) {
        ascii_sum = definition->energy_query;
        std::snprintf(prefix, sizeof(prefix), "^%c%03u", definition->setter ? 'S' : 'P',
            static_cast<unsigned>(command_length + (ascii_sum ? 4 : 1)));
        prefix_length = 5;
    } else if (profile->wire == WireFamily::pi18) {
        if (!definition->prefix || std::strlen(definition->prefix) != 5) return CodecStatus::invalid_command;
        std::memcpy(prefix, definition->prefix, 5);
        prefix_length = 5;
        crc = definition->no_crc == 0 || (definition->no_crc < 0 && !profile->default_no_crc);
    } else if (profile->wire == WireFamily::revo) {
        revo_sum = definition->revo_checksum;
        crc = !revo_sum;
        if (std::strncmp(command, "PSET", 4) == 0) terminate = false;
    }
    const auto checksum_size = crc ? 2U : ascii_sum ? 3U : revo_sum ? 1U : 0U;
    const auto required = prefix_length + command_length + checksum_size + (terminate ? 1U : 0U);
    if (!out || capacity < required) return CodecStatus::buffer_too_small;
    std::memcpy(out, prefix, prefix_length);
    std::memcpy(out + prefix_length, command, command_length);
    size = prefix_length + command_length;
    if (crc) {
        const auto value = xmodem_crc(out, size);
        out[size++] = escaped_crc_byte(static_cast<std::uint8_t>(value >> 8));
        out[size++] = escaped_crc_byte(static_cast<std::uint8_t>(value));
    } else if (ascii_sum) {
        const auto value = byte_sum(out, size);
        out[size++] = static_cast<std::uint8_t>('0' + value / 100);
        out[size++] = static_cast<std::uint8_t>('0' + (value / 10) % 10);
        out[size++] = static_cast<std::uint8_t>('0' + value % 10);
    } else if (revo_sum) {
        const auto value = static_cast<std::uint8_t>(byte_sum(out, size) + 1);
        out[size++] = value;
    }
    if (terminate) out[size++] = '\r';
    return CodecStatus::ok;
}

FrameState pi_frame_state(EnergyProtocol id, const char *command,
    const std::uint8_t *data, std::size_t size, std::size_t &frame_size)
{
    frame_size = 0;
    const auto *profile = profile_for(id);
    if (!profile || !find_command(*profile, command)) return FrameState::invalid;
    if (!size) return FrameState::incomplete;
    if (!data || size > maximum_frame_length) return FrameState::invalid;
    const bool caret = profile->wire == WireFamily::pi17 || profile->wire == WireFamily::pi18;
    if (caret && data[0] == '^') {
        if (size < 2) return FrameState::incomplete;
        std::size_t required = 0;
        if (data[1] == '0' || data[1] == '1') required = 5;
        else if (data[1] == 'D') {
            if (size < 5) return FrameState::incomplete;
            const int declared = decimal_digits(reinterpret_cast<const char *>(data + 2), 3);
            if (declared < 3) return FrameState::invalid;
            required = static_cast<std::size_t>(declared + 5);
        } else return FrameState::invalid;
        if (size < required) return FrameState::incomplete;
        if (data[required - 1] != '\r') return FrameState::invalid;
        frame_size = required;
        return FrameState::complete;
    }
    if (data[0] != '(' && profile->wire != WireFamily::revo) return FrameState::invalid;
    for (std::size_t i = 1; i < size; ++i) {
        if (data[i] != '\r' && !(profile->wire == WireFamily::revo && data[i] == '\n')) continue;
        // A REVO checksum can itself be CR/LF. Only a checksum-valid candidate
        // ends its response, so fragmented reports cannot truncate that byte.
        if (profile->wire == WireFamily::revo &&
            !valid_crc(data, i + 1, true) && !valid_revo_checksum(data, i + 1)) continue;
        frame_size = i + 1;
        return FrameState::complete;
    }
    return FrameState::incomplete;
}

CodecStatus pi_decode_response(EnergyProtocol id, const char *command,
    std::uint8_t /* address */, const std::uint8_t *data, std::size_t size,
    EnergyResponse &response)
{
    response.count = 0;
    response.acknowledged = false;
    response.rejected = false;
    const auto *profile = profile_for(id);
    if (!profile) return CodecStatus::unsupported;
    const auto *definition = find_command(*profile, command);
    if (!definition) return CodecStatus::invalid_command;
    const std::uint8_t *begin = nullptr;
    std::size_t length = 0;
    const auto validated = payload(*profile, data, size, begin, length);
    if (validated != CodecStatus::ok) return validated;
    const bool caret_ack = length == 1 && data[0] == '^' && (begin[0] == '0' || begin[0] == '1');
    const bool ack = caret_ack ? begin[0] == '1' : length == 3 && std::memcmp(begin, "ACK", 3) == 0;
    const bool nak = caret_ack ? begin[0] == '0' : length == 3 && std::memcmp(begin, "NAK", 3) == 0;
    if (nak) {
        response.rejected = true;
        return CodecStatus::rejected;
    }
    if (definition->setter) {
        if (!ack) return CodecStatus::invalid_frame;
        response.acknowledged = true;
        return CodecStatus::ok;
    }
    if (ack || !length) return CodecStatus::invalid_frame;
    Token tokens[maximum_tokens]{};
    std::size_t token_count = 0;
    const bool comma = profile->wire == WireFamily::pi17 || profile->wire == WireFamily::pi18;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= length; ++i) {
        if (i != length && begin[i] != (comma ? ',' : ' ') &&
            !(profile->wire == WireFamily::pi17 && begin[i] == ' ')) continue;
        if (token_count >= maximum_tokens) return CodecStatus::buffer_too_small;
        tokens[token_count++] = {begin + start, i - start};
        start = i + 1;
    }
    if ((id == EnergyProtocol::pi30 || id == EnergyProtocol::pi30mst) &&
        !std::strcmp(command, "QPIGS2")) {
        // PI30 SCC2/SCC3 replies have twelve tokens, while dual-MPPT MAX
        // firmware uses three. Their third tokens are different quantities:
        // SCC battery voltage versus PV2 power. Select a complete known layout
        // before decoding so a truncated reply cannot publish voltage as watts.
        // Some devices append a space immediately before their CRC.
        while (token_count && !tokens[token_count - 1].size) --token_count;
        const auto layout = token_count == 3 ? EnergyProtocol::pi30max : EnergyProtocol::pi30mst;
        if (token_count != 3 && token_count != 12) return CodecStatus::invalid_frame;
        definition = find_command(*profile_for(layout), "QPIGS2");
    }
    for (std::size_t i = 0; i < definition->field_count; ++i) {
        const auto result = decode_field(definition->fields[i], tokens, token_count,
                                         command + std::strlen(definition->key), response);
        if (result != CodecStatus::ok) {
            response.count = 0;
            return result;
        }
    }
    // Unknown firmware extensions remain visible without fabricated units.
    std::size_t known_tokens = 0;
    for (std::size_t i = 0; i < definition->field_count; ++i) {
        const auto token = static_cast<std::size_t>(definition->fields[i].token);
        if (token >= known_tokens) known_tokens = token + 1;
    }
    for (std::size_t i = known_tokens; i < token_count; ++i) {
        if (!tokens[i].size) continue;
        char name[32];
        char value[160];
        std::snprintf(name, sizeof(name), "Unknown value %u", static_cast<unsigned>(i + 1));
        if (!copy_token(tokens[i], value, sizeof(value))) return CodecStatus::invalid_frame;
        if (!response.add_text(name, "", value)) return CodecStatus::buffer_too_small;
    }
    return response.count ? CodecStatus::ok : CodecStatus::invalid_frame;
}

} // namespace inverter_gateway::protocol
