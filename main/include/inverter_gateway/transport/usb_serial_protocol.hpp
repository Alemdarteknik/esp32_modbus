#pragma once

#include <memory>
#include "inverter_gateway/transport/energy_transport.hpp"
#include "usb/vcp.hpp"

namespace inverter_gateway::transport {

// Reuses the existing CH34x USB host unchanged for native byte protocols.
// One bus-task owner calls reconnect/exchange; 8N1 only. Never install HID
// concurrently. A physical RS232/RS485 adapter chooses electrical signaling.
class UsbSerialProtocol final : public InverterByteTransport {
public:
    esp_err_t install(std::uint32_t baud);
    bool reconnect();
    bool connected() const override;
    std::uint32_t session_id() const override { return session_; }
    esp_err_t exchange(const std::uint8_t *command, std::size_t command_size,
        std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
        ReceiveRule rule, bool &transmitted) override;
private:
    std::uint32_t baud_ = 9600;
    std::uint32_t session_ = 0;
    std::unique_ptr<CdcAcmDevice> device_;
};

} // namespace inverter_gateway::transport
