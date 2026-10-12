#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace inverter_gateway::transport {

enum class FrameDecision : std::uint8_t { incomplete, complete, invalid };

struct ReceiveRule {
    // Called after every admitted byte. This keeps binary framing out of the
    // transport and avoids reading HID padding beyond a completed response.
    FrameDecision (*inspect)(const std::uint8_t *, std::size_t, void *) = nullptr;
    void *context = nullptr;
    // Nonzero binds admission to the connection validated by the bus owner.
    // Reject an old session before TX even if a replacement device is ready.
    std::uint32_t expected_session = 0;
};

inline constexpr std::size_t maximum_inverter_frame = 1024;

class InverterByteTransport {
public:
    virtual ~InverterByteTransport() = default;
    virtual bool connected() const = 0;
    // Changes on connect/disconnect, including a hotplug cycle between polls.
    virtual std::uint32_t session_id() const = 0;
    // Capacity enters in response_size; admitted length leaves it. The deadline
    // includes connection, transaction lock, TX and RX. Never retries a write.
    // transmitted is conservative: true means the outcome can be uncertain.
    virtual esp_err_t exchange(const std::uint8_t *command, std::size_t command_size,
        std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
        ReceiveRule rule, bool &transmitted) = 0;
};

} // namespace inverter_gateway::transport
