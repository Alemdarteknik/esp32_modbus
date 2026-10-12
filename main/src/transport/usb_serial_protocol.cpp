#include "inverter_gateway/transport/usb_serial_protocol.hpp"

#include "byte_transport_detail.hpp"
#include "freertos/task.h"
#include "inverter_gateway/transport/usb_rs485.hpp"

namespace inverter_gateway::transport {

esp_err_t UsbSerialProtocol::install(std::uint32_t baud)
{
    if (baud < 1200 || baud > 115200) return ESP_ERR_INVALID_ARG;
    baud_ = baud;
    return UsbRs485::install(2048);
}

bool UsbSerialProtocol::reconnect()
{
    device_.reset();
    ++session_; // Invalidates any availability from the previous USB handle.
    UsbRs485::begin_connection_session();
    device_ = UsbRs485::open(1000, 2048);
    if (!device_) return false;
    if (UsbRs485::configure(*device_, baud_) != ESP_OK) { device_.reset(); return false; }
    vTaskDelay(pdMS_TO_TICKS(200));
    UsbRs485::clear_receive_buffer();
    return connected();
}

bool UsbSerialProtocol::connected() const { return device_ && !UsbRs485::disconnected(); }

esp_err_t UsbSerialProtocol::exchange(const std::uint8_t *command, std::size_t command_size,
    std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
    ReceiveRule rule, bool &transmitted)
{
    const auto capacity = response_size;
    response_size = 0;
    transmitted = false;
    if (!detail::valid_exchange(command, command_size, response, capacity, timeout_ms, rule))
        return ESP_ERR_INVALID_ARG;
    if (!connected()) return ESP_ERR_INVALID_STATE;
    const auto session = session_id();
    if (rule.expected_session != 0 && rule.expected_session != session) return ESP_ERR_INVALID_STATE;
    const detail::Deadline deadline(timeout_ms);
    std::uint32_t generation = 0;
    if (!UsbRs485::prepare_receive_transaction(generation)) return ESP_ERR_INVALID_RESPONSE;
    if (deadline.expired()) return ESP_ERR_TIMEOUT;
    if (!connected() || session != session_id()) return ESP_ERR_INVALID_STATE;
    if (command_size != 0) {
        transmitted = true; // Submission failure leaves the outcome uncertain.
        const auto remaining_ms = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, (deadline.end() - esp_timer_get_time()) / 1000));
        const auto error = device_->tx_blocking(const_cast<std::uint8_t *>(command), command_size, remaining_ms);
        if (error != ESP_OK) return error;
    }
    while (!deadline.expired()) {
        if (!connected() || session != session_id()) return ESP_ERR_INVALID_STATE;
        if (UsbRs485::receive_overflow_since(generation)) return ESP_ERR_INVALID_RESPONSE;
        if (response_size == capacity) return ESP_ERR_INVALID_SIZE;
        if (xStreamBufferReceive(UsbRs485::receive_stream(), response + response_size, 1, deadline.ticks()) == 0)
            continue;
        ++response_size;
        if (!connected() || session != session_id()) return ESP_ERR_INVALID_STATE;
        if (UsbRs485::receive_overflow_since(generation)) return ESP_ERR_INVALID_RESPONSE;
        if (deadline.expired()) return ESP_ERR_TIMEOUT;
        const auto decision = rule.inspect(response, response_size, rule.context);
        if (decision == FrameDecision::invalid) return ESP_ERR_INVALID_RESPONSE;
        if (decision == FrameDecision::complete) {
            if (!connected() || session != session_id()) return ESP_ERR_INVALID_STATE;
            if (UsbRs485::receive_overflow_since(generation)) return ESP_ERR_INVALID_RESPONSE;
            return deadline.expired() ? ESP_ERR_TIMEOUT : ESP_OK;
        }
    }
    return connected() && session == session_id() ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

} // namespace inverter_gateway::transport
