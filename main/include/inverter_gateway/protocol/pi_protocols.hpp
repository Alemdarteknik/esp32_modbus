#pragma once

#include "inverter_gateway/protocol/energy_protocol.hpp"

namespace inverter_gateway::protocol {

std::size_t pi_poll_command_count(EnergyProtocol id);
const char *pi_poll_command(EnergyProtocol id, std::size_t index);
bool pi_command_info(EnergyProtocol id, const char *command,
                     EnergyCommandInfo &info);
CodecStatus pi_build_request(EnergyProtocol id, const char *command,
    std::uint8_t address, std::uint8_t *out, std::size_t capacity, std::size_t &size);
FrameState pi_frame_state(EnergyProtocol id, const char *command,
    const std::uint8_t *data, std::size_t size, std::size_t &frame_size);
CodecStatus pi_decode_response(EnergyProtocol id, const char *command,
    std::uint8_t address, const std::uint8_t *data, std::size_t size,
    EnergyResponse &response);

} // namespace inverter_gateway::protocol
