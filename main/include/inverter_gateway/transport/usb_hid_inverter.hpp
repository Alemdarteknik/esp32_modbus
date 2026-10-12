#pragma once

#include "inverter_gateway/transport/energy_transport.hpp"

namespace inverter_gateway::transport {

struct UsbHidInverterConfig {
    // Zero discovers a compatible HID inverter without an ID filter. Either
    // nonzero field restricts discovery independently to that VID or PID.
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
};

// Voltronic HID byte transport: 8-byte input and output reports, report ID 0,
// output through class SET_REPORT. Discovery checks interface/report layout;
// automatic discovery also requires a vendor-defined application collection.
// It owns USB host for this boot; never install UsbRs485 in the same boot.
// The selected backend owns a lifetime instance; hotplug is handled internally.
class UsbHid final : public InverterByteTransport {
public:
    esp_err_t install(const UsbHidInverterConfig &config = {});
    bool connected() const override;
    std::uint32_t session_id() const override;
    esp_err_t exchange(const std::uint8_t *command, std::size_t command_size,
        std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
        ReceiveRule rule, bool &transmitted) override;
};

} // namespace inverter_gateway::transport
