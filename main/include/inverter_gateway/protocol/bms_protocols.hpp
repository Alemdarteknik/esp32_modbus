#pragma once

#include "inverter_gateway/protocol/energy_protocol.hpp"

namespace inverter_gateway::protocol {

std::size_t bms_poll_command_count(EnergyProtocol id);
const char *bms_poll_command(EnergyProtocol id, std::size_t index);
bool bms_command_info(EnergyProtocol id, const char *command, EnergyCommandInfo &info);
CodecStatus bms_build_request(EnergyProtocol id, const char *command,
    std::uint8_t address, std::uint8_t *out, std::size_t capacity, std::size_t &size);
FrameState bms_frame_state(EnergyProtocol id, const char *command,
    const std::uint8_t *data, std::size_t size, std::size_t &frame_size);
CodecStatus bms_decode_response(EnergyProtocol id, const char *command,
    std::uint8_t address, const std::uint8_t *data, std::size_t size,
    EnergyResponse &response);

// DALY cell/temperature responses have a variable number of 13-byte frames.
// The caller obtains the counts from the status (0x94) reply and uses this
// overload to wait for the whole group, rather than admitting its first frame.
FrameState daly_group_frame_state(const char *command, const std::uint8_t *data,
    std::size_t size, std::size_t expected_frames, std::size_t &frame_size);

} // namespace inverter_gateway::protocol
