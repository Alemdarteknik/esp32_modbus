#pragma once

#include "driver/uart.h"
#include "inverter_gateway/transport/energy_transport.hpp"

namespace inverter_gateway::transport {

struct UartInverterConfig {
    int tx_pin = 4;
    int rx_pin = 5;
    int direction_pin = -1; // RS232: no direction GPIO; RS485: DE+/RE together.
    std::uint32_t baud = 2400;
    uart_parity_t parity = UART_PARITY_DISABLE;
};

// One lifetime instance. UART2 is separate from coordinator UART1 (GPIO17/18).
// ESP pins are TTL signals; an external RS232/RS485 transceiver is required.
class UartInverter final : public InverterByteTransport {
public:
    esp_err_t install(const UartInverterConfig &config);
    bool connected() const override;
    std::uint32_t session_id() const override;
    esp_err_t exchange(const std::uint8_t *command, std::size_t command_size,
        std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
        ReceiveRule rule, bool &transmitted) override;
};

} // namespace inverter_gateway::transport
