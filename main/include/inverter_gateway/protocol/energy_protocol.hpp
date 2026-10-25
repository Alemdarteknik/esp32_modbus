#pragma once

#include <cstddef>
#include <cstdint>

namespace inverter_gateway::protocol {

enum class EnergyProtocol : std::uint8_t {
    aohai_modbus, pi16, pi17, pi17m058, pi17infini, pi18, pi18lvx,
    pi18sv, pi30, pi30max, pi30mst, pi30revo, pi30m044, pi30m045,
    pi41, jk02, jk02_32, jk04, jk232, jk485, jkpb, jkv11, jkserial,
    daly, daly40, ved, count,
};

enum class EnergyConnection : std::uint8_t {
    usb_rs485, usb_hid, uart_rs232, uart_rs485, uart_ttl, bluetooth,
};

enum class CodecStatus : std::uint8_t {
    ok, unsupported, invalid_command, invalid_frame, checksum_error,
    buffer_too_small, rejected,
};

enum class FrameState : std::uint8_t { incomplete, complete, invalid };

struct EnergyField {
    char key[40]{};
    char unit[8]{};
    char text[32]{};
    double value = 0;
    bool numeric = true;
};

struct EnergyResponse {
    static constexpr std::size_t maximum_fields = 96;
    EnergyField fields[maximum_fields]{};
    std::size_t count = 0;
    bool acknowledged = false;
    bool rejected = false;
    bool add_number(const char *label, const char *unit, double value);
    bool add_text(const char *label, const char *unit, const char *value);
};

struct EnergyCommandInfo {
    const char *name = nullptr;
    bool setter = false;
    // Optional query to observe settings after a write; never implies equality.
    const char *readback_command = nullptr;
};

struct EnergyProtocolInfo {
    EnergyProtocol id;
    const char *name;
    const char *description;
    const char *default_command;
    std::uint32_t default_baud;
    EnergyConnection default_connection;
    bool implemented;
};

// Polling frequency reflects the kind of observation, rather than the order
// of the query catalog. Identity is read once after each transport connection.
enum class EnergyPollClass : std::uint8_t {
    live, status, configuration, identity, energy,
};

struct EnergyPollInfo {
    const char *command = nullptr;
    EnergyPollClass poll_class = EnergyPollClass::live;
    // Zero means once per transport session (identity or static capabilities).
    std::uint32_t interval_ms = 2000;
    bool publish_on_change = false;
    bool primary = false;
    // JK Bluetooth requires getInfo to enable notifications before live reads.
    bool prerequisite = false;
    std::uint8_t minimum_pv_inputs = 0;
};

const EnergyProtocolInfo &energy_protocol_info(EnergyProtocol id);
bool parse_energy_protocol(const char *name, EnergyProtocol &id);
const char *energy_connection_name(EnergyConnection connection);
bool parse_energy_connection(const char *name, EnergyConnection &connection);
bool energy_connection_supported(EnergyProtocol id, EnergyConnection connection);
const char *codec_status_name(CodecStatus status);

std::size_t energy_poll_command_count(EnergyProtocol id);
const char *energy_poll_command(EnergyProtocol id, std::size_t index);
std::size_t energy_background_poll_count(EnergyProtocol id);
bool energy_background_poll_command(EnergyProtocol id, std::size_t index,
                                    EnergyPollInfo &poll);
bool energy_poll_info(EnergyProtocol id, const char *command,
                      EnergyPollInfo &poll);
bool energy_command_info(EnergyProtocol id, const char *command,
                         EnergyCommandInfo &info);
CodecStatus energy_build_request(EnergyProtocol id, const char *command,
    std::uint8_t address, std::uint8_t *out, std::size_t capacity, std::size_t &size);
FrameState energy_frame_state(EnergyProtocol id, const char *command,
    const std::uint8_t *data, std::size_t size, std::size_t &frame_size);
CodecStatus energy_decode_response(EnergyProtocol id, const char *command,
    std::uint8_t address, const std::uint8_t *data, std::size_t size,
    EnergyResponse &response);

} // namespace inverter_gateway::protocol
