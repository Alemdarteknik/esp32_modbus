#pragma once

#include "inverter_gateway/transport/energy_transport.hpp"

namespace inverter_gateway::transport {

struct BleInverterConfig {
    char mac_address[18]{}; // Exactly AA:BB:CC:DD:EE:FF; no automatic pairing.
    std::uint8_t address_type = 0; // 0 public, 1 random/static.
    std::uint16_t service_uuid = 0xffe0;
    std::uint16_t write_uuid = 0xffe1;
    std::uint16_t notify_uuid = 0xffe1;
};

// JK BMS FFE0/FFE1 byte transport. The configured peer is the sole target;
// service, properties and CCCD are discovered before command admission.
class BleInverter final : public InverterByteTransport {
public:
    esp_err_t install(const BleInverterConfig &config);
    bool connected() const override;
    std::uint32_t session_id() const override;
    esp_err_t exchange(const std::uint8_t *command, std::size_t command_size,
        std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
        ReceiveRule rule, bool &transmitted) override;
};

} // namespace inverter_gateway::transport
