#include "inverter_gateway/protocol/modbus_rtu.hpp"

#include <cstdio>
#include <atomic>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "inverter_gateway/app/monitor_output.hpp"
#include "inverter_gateway/diagnostics/bounded_counter.hpp"
#include "inverter_gateway/transport/usb_rs485.hpp"

namespace inverter_gateway::protocol {
namespace {

constexpr char tag[] = "modbus_rtu";
constexpr std::size_t maximum_response_size = 256;
std::atomic<std::uint32_t> crc_errors{0};
std::atomic<std::uint32_t> response_timeouts{0};
std::atomic<std::uint32_t> incomplete_responses{0};
std::atomic<std::uint32_t> receive_overflow_failures{0};

void print_hex_frame(const char *label, const std::uint8_t *frame, std::size_t length)
{
    if (!app::monitor_output_enabled()) return;
    std::printf("%s", label);
    for (std::size_t index = 0; index < length; ++index) {
        std::printf("%02X%s", frame[index], index + 1 < length ? " " : "\n");
    }
}

} // namespace

ModbusRtuStats modbus_rtu_stats()
{
    return {crc_errors.load(std::memory_order_relaxed),
            response_timeouts.load(std::memory_order_relaxed),
            incomplete_responses.load(std::memory_order_relaxed),
            receive_overflow_failures.load(std::memory_order_relaxed)};
}

bool ModbusRtuClient::prepare_receive_transaction()
{
    receive_overflow_reported_ = false;
    if (!transport::UsbRs485::prepare_receive_transaction(receive_generation_)) {
        receive_overflowed();
        ESP_LOGW(tag, "USB RX changed while draining; request not transmitted");
        return false;
    }
    return true;
}

bool ModbusRtuClient::receive_overflowed()
{
    if (!transport::UsbRs485::receive_overflow_since(receive_generation_)) return false;
    if (!receive_overflow_reported_) {
        receive_overflow_reported_ = true;
        diagnostics::add_saturating(receive_overflow_failures);
        transport::UsbRs485::clear_receive_buffer();
        ESP_LOGW(tag, "USB RX overflow invalidated the Modbus transaction");
    }
    return true;
}

std::uint16_t calculate_modbus_crc(const std::uint8_t *data, std::size_t length)
{
    std::uint16_t crc = 0xFFFF;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0
                      ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U)
                      : static_cast<std::uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

ModbusRtuClient::ModbusRtuClient(CdcAcmDevice &device, std::uint8_t slave_address,
                                 std::uint32_t response_timeout_ms,
                                 std::uint32_t minimum_command_gap_ms)
    : device_(device), slave_address_(slave_address),
      response_timeout_ms_(response_timeout_ms),
      minimum_command_gap_ticks_(pdMS_TO_TICKS(minimum_command_gap_ms))
{
}

bool ModbusRtuClient::read_holding(std::uint16_t first_register,
                                   std::uint16_t register_count,
                                   std::uint16_t *destination,
                                   std::size_t destination_capacity)
{
    return read(ModbusFunction::read_holding_registers, first_register, register_count,
                destination, destination_capacity);
}

bool ModbusRtuClient::read_input(std::uint16_t first_register,
                                 std::uint16_t register_count,
                                 std::uint16_t *destination,
                                 std::size_t destination_capacity)
{
    return read(ModbusFunction::read_input_registers, first_register, register_count,
                destination, destination_capacity);
}

bool ModbusRtuClient::read(ModbusFunction function, std::uint16_t first_register,
                           std::uint16_t register_count, std::uint16_t *destination,
                           std::size_t destination_capacity)
{
    if (register_count == 0 || register_count > 125 || destination == nullptr ||
        destination_capacity < register_count) {
        ESP_LOGE(tag, "Invalid Modbus read request");
        return false;
    }
    wait_for_command_slot();
    return send_read_request(function, first_register, register_count) &&
           receive_read_response(function, register_count, destination,
                                 destination_capacity);
}

void ModbusRtuClient::wait_for_command_slot()
{
    if (command_has_been_sent_) {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t elapsed = now - last_command_tick_;
        if (elapsed < minimum_command_gap_ticks_) {
            vTaskDelay(minimum_command_gap_ticks_ - elapsed);
        }
    }
    last_command_tick_ = xTaskGetTickCount();
    command_has_been_sent_ = true;
}

ModbusWriteResult ModbusRtuClient::write_single(std::uint16_t register_address,
                                                std::uint16_t value)
{
    wait_for_command_slot();

    std::uint8_t request[8] = {
        slave_address_,
        static_cast<std::uint8_t>(ModbusFunction::write_single_register),
        static_cast<std::uint8_t>(register_address >> 8U),
        static_cast<std::uint8_t>(register_address),
        static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value),
        0,
        0,
    };
    const std::uint16_t crc = calculate_modbus_crc(request, 6);
    request[6] = static_cast<std::uint8_t>(crc);
    request[7] = static_cast<std::uint8_t>(crc >> 8U);

    if (!prepare_receive_transaction()) return {ModbusWriteStatus::receive_busy, 0};
    print_hex_frame("TX: ", request, sizeof(request));
    const esp_err_t err = device_.tx_blocking(request, sizeof(request), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "USB transmit failed: %s", esp_err_to_name(err));
        return {ModbusWriteStatus::transmit_failed, 0};
    }
    return receive_write_single_response(register_address, value);
}

ModbusWriteResult ModbusRtuClient::write_multiple(
    std::uint16_t first_register, std::uint16_t register_count,
    const std::uint16_t *values)
{
    if (register_count == 0 || register_count > 123 || values == nullptr) {
        return {ModbusWriteStatus::invalid_argument, 0};
    }
    wait_for_command_slot();
    std::uint8_t request[9 + 123 * 2]{};
    request[0] = slave_address_;
    request[1] = static_cast<std::uint8_t>(ModbusFunction::write_multiple_registers);
    request[2] = static_cast<std::uint8_t>(first_register >> 8U);
    request[3] = static_cast<std::uint8_t>(first_register);
    request[4] = static_cast<std::uint8_t>(register_count >> 8U);
    request[5] = static_cast<std::uint8_t>(register_count);
    request[6] = static_cast<std::uint8_t>(register_count * 2U);
    for (std::uint16_t index = 0; index < register_count; ++index) {
        request[7 + index * 2] = static_cast<std::uint8_t>(values[index] >> 8U);
        request[8 + index * 2] = static_cast<std::uint8_t>(values[index]);
    }
    const std::size_t payload_size = 7 + register_count * 2U;
    const std::uint16_t crc = calculate_modbus_crc(request, payload_size);
    request[payload_size] = static_cast<std::uint8_t>(crc);
    request[payload_size + 1] = static_cast<std::uint8_t>(crc >> 8U);
    const std::size_t frame_size = payload_size + 2;

    if (!prepare_receive_transaction()) return {ModbusWriteStatus::receive_busy, 0};
    print_hex_frame("TX: ", request, frame_size);
    const esp_err_t err = device_.tx_blocking(request, frame_size, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "USB transmit failed: %s", esp_err_to_name(err));
        return {ModbusWriteStatus::transmit_failed, 0};
    }
    return receive_write_multiple_response(first_register, register_count);
}

ModbusWriteResult ModbusRtuClient::receive_write_single_response(
    std::uint16_t register_address, std::uint16_t value)
{
    std::uint8_t response[8] = {};
    std::size_t received_length = 0;
    std::size_t expected_length = 0;
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(response_timeout_ms_);
    constexpr std::uint8_t function =
        static_cast<std::uint8_t>(ModbusFunction::write_single_register);

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
        if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
        if (transport::UsbRs485::disconnected()) {
            return {ModbusWriteStatus::disconnected, 0};
        }
        const TickType_t elapsed = xTaskGetTickCount() - start_tick;
        const TickType_t remaining = timeout_ticks > elapsed ? timeout_ticks - elapsed : 0;
        std::uint8_t byte = 0;
        const auto received = xStreamBufferReceive(transport::UsbRs485::receive_stream(), &byte, 1,
                                                   remaining);
        if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
        if (received != 1) {
            break;
        }

        if (received_length == 0) {
            if (byte == slave_address_) response[received_length++] = byte;
            continue;
        }
        if (received_length == 1) {
            if (byte == function || byte == (function | 0x80U)) {
                response[received_length++] = byte;
                expected_length = byte == (function | 0x80U) ? 5U : 8U;
            } else {
                received_length = byte == slave_address_ ? 1 : 0;
            }
            continue;
        }
        if (received_length >= sizeof(response)) {
            return {ModbusWriteStatus::malformed_response, 0};
        }
        response[received_length++] = byte;
        if (expected_length != 0 && received_length == expected_length) break;
    }

    if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
    if (received_length == 0) {
        diagnostics::add_saturating(response_timeouts);
        return {ModbusWriteStatus::timeout, 0};
    }
    print_hex_frame("RX: ", response, received_length);
    if (expected_length == 0 || received_length != expected_length) {
        diagnostics::add_saturating(incomplete_responses);
        return {ModbusWriteStatus::malformed_response, 0};
    }

    const std::uint16_t received_crc = response[received_length - 2] |
        (static_cast<std::uint16_t>(response[received_length - 1]) << 8U);
    if (received_crc != calculate_modbus_crc(response, received_length - 2)) {
        diagnostics::add_saturating(crc_errors);
        return {ModbusWriteStatus::crc_error, 0};
    }
    if ((response[1] & 0x80U) != 0) {
        return {ModbusWriteStatus::exception_response, response[2]};
    }

    const std::uint16_t echoed_address =
        (static_cast<std::uint16_t>(response[2]) << 8U) | response[3];
    const std::uint16_t echoed_value =
        (static_cast<std::uint16_t>(response[4]) << 8U) | response[5];
    if (echoed_address != register_address || echoed_value != value) {
        return {ModbusWriteStatus::echo_mismatch, 0};
    }
    if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
    return {ModbusWriteStatus::success, 0};
}

ModbusWriteResult ModbusRtuClient::receive_write_multiple_response(
    std::uint16_t first_register, std::uint16_t register_count)
{
    std::uint8_t response[8]{};
    std::size_t received_length = 0;
    std::size_t expected_length = 0;
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(response_timeout_ms_);
    constexpr std::uint8_t function =
        static_cast<std::uint8_t>(ModbusFunction::write_multiple_registers);
    while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
        if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
        if (transport::UsbRs485::disconnected()) {
            return {ModbusWriteStatus::disconnected, 0};
        }
        const TickType_t elapsed = xTaskGetTickCount() - start_tick;
        const TickType_t remaining = timeout_ticks > elapsed ? timeout_ticks - elapsed : 0;
        std::uint8_t byte = 0;
        const auto received = xStreamBufferReceive(transport::UsbRs485::receive_stream(), &byte, 1,
                                                   remaining);
        if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
        if (received != 1) break;
        if (received_length == 0) {
            if (byte == slave_address_) response[received_length++] = byte;
            continue;
        }
        if (received_length == 1) {
            if (byte == function || byte == (function | 0x80U)) {
                response[received_length++] = byte;
                expected_length = byte == (function | 0x80U) ? 5U : 8U;
            } else {
                received_length = byte == slave_address_ ? 1 : 0;
            }
            continue;
        }
        if (received_length >= sizeof(response)) {
            return {ModbusWriteStatus::malformed_response, 0};
        }
        response[received_length++] = byte;
        if (expected_length != 0 && received_length == expected_length) break;
    }
    if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
    if (received_length == 0) {
        diagnostics::add_saturating(response_timeouts);
        return {ModbusWriteStatus::timeout, 0};
    }
    print_hex_frame("RX: ", response, received_length);
    if (expected_length == 0 || received_length != expected_length) {
        diagnostics::add_saturating(incomplete_responses);
        return {ModbusWriteStatus::malformed_response, 0};
    }
    const std::uint16_t received_crc = response[received_length - 2] |
        (static_cast<std::uint16_t>(response[received_length - 1]) << 8U);
    if (received_crc != calculate_modbus_crc(response, received_length - 2)) {
        diagnostics::add_saturating(crc_errors);
        return {ModbusWriteStatus::crc_error, 0};
    }
    if ((response[1] & 0x80U) != 0) {
        return {ModbusWriteStatus::exception_response, response[2]};
    }
    const std::uint16_t echoed_first =
        (static_cast<std::uint16_t>(response[2]) << 8U) | response[3];
    const std::uint16_t echoed_count =
        (static_cast<std::uint16_t>(response[4]) << 8U) | response[5];
    if (echoed_first != first_register || echoed_count != register_count) {
        return {ModbusWriteStatus::echo_mismatch, 0};
    }
    if (receive_overflowed()) return {ModbusWriteStatus::receive_overflow, 0};
    return {ModbusWriteStatus::success, 0};
}

bool ModbusRtuClient::send_read_request(ModbusFunction function,
                                        std::uint16_t first_register,
                                        std::uint16_t register_count)
{
    std::uint8_t request[8] = {
        slave_address_,
        static_cast<std::uint8_t>(function),
        static_cast<std::uint8_t>(first_register >> 8U),
        static_cast<std::uint8_t>(first_register),
        static_cast<std::uint8_t>(register_count >> 8U),
        static_cast<std::uint8_t>(register_count),
        0,
        0,
    };
    const std::uint16_t crc = calculate_modbus_crc(request, 6);
    request[6] = static_cast<std::uint8_t>(crc);
    request[7] = static_cast<std::uint8_t>(crc >> 8U);

    if (!prepare_receive_transaction()) return false;
    print_hex_frame("TX: ", request, sizeof(request));

    const esp_err_t err = device_.tx_blocking(request, sizeof(request), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "USB transmit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ModbusRtuClient::receive_read_response(ModbusFunction expected_function,
                                             std::uint16_t requested_register_count,
                                             std::uint16_t *destination,
                                             std::size_t destination_capacity)
{
    std::uint8_t response[maximum_response_size] = {};
    std::size_t received_length = 0;
    std::size_t expected_length = 0;
    std::size_t ignored_noise_bytes = 0;
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(response_timeout_ms_);
    const std::uint8_t expected_function_code = static_cast<std::uint8_t>(expected_function);

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
        if (receive_overflowed()) return false;
        if (transport::UsbRs485::disconnected()) {
            ESP_LOGE(tag, "USB-RS485 adapter disconnected during request");
            return false;
        }

        const TickType_t elapsed = xTaskGetTickCount() - start_tick;
        const TickType_t remaining = timeout_ticks > elapsed ? timeout_ticks - elapsed : 0;
        std::uint8_t value = 0;
        const auto received = xStreamBufferReceive(transport::UsbRs485::receive_stream(), &value, 1,
                                                   remaining);
        if (receive_overflowed()) return false;
        if (received != 1) {
            break;
        }

        if (received_length == 0) {
            if (value == slave_address_) {
                response[received_length++] = value;
            } else {
                ++ignored_noise_bytes;
            }
            continue;
        }

        if (received_length == 1) {
            const bool valid_function = value == expected_function_code ||
                                        value == (expected_function_code | 0x80U);
            if (valid_function) {
                response[received_length++] = value;
            } else {
                ++ignored_noise_bytes;
                received_length = value == slave_address_ ? 1 : 0;
            }
            continue;
        }

        if (received_length >= sizeof(response)) {
            ESP_LOGE(tag, "Modbus response exceeded receive buffer");
            return false;
        }
        response[received_length++] = value;

        if (received_length == 3) {
            expected_length = (response[1] & 0x80U) != 0
                                  ? 5U
                                  : static_cast<std::size_t>(response[2]) + 5U;
            if (expected_length < 5 || expected_length > sizeof(response)) {
                ESP_LOGE(tag, "Invalid Modbus frame length %u",
                         static_cast<unsigned>(expected_length));
                return false;
            }
        }
        if (expected_length != 0 && received_length == expected_length) {
            break;
        }
    }

    if (receive_overflowed()) return false;
    if (ignored_noise_bytes != 0) {
        ESP_LOGW(tag, "Ignored %u byte(s) before valid Modbus header",
                 static_cast<unsigned>(ignored_noise_bytes));
    }
    if (received_length == 0) {
        diagnostics::add_saturating(response_timeouts);
        ESP_LOGE(tag, "No valid %02X %02X response before timeout", slave_address_,
                 expected_function_code);
        return false;
    }

    print_hex_frame("RX: ", response, received_length);
    if (expected_length == 0 || received_length != expected_length) {
        diagnostics::add_saturating(incomplete_responses);
        ESP_LOGE(tag, "Incomplete response: received %u byte(s), expected %u",
                 static_cast<unsigned>(received_length),
                 static_cast<unsigned>(expected_length));
        return false;
    }

    const std::uint16_t received_crc = response[received_length - 2] |
        (static_cast<std::uint16_t>(response[received_length - 1]) << 8U);
    const std::uint16_t calculated_crc = calculate_modbus_crc(response,
                                                               received_length - 2);
    if (received_crc != calculated_crc) {
        diagnostics::add_saturating(crc_errors);
        ESP_LOGE(tag, "CRC mismatch: received 0x%04X, calculated 0x%04X",
                 received_crc, calculated_crc);
        return false;
    }
    if (app::monitor_output_enabled()) ESP_LOGI(tag, "CRC OK");

    if ((response[1] & 0x80U) != 0) {
        ESP_LOGE(tag, "Modbus exception code %u", response[2]);
        return false;
    }

    const std::uint8_t byte_count = response[2];
    const std::uint16_t response_register_count = byte_count / 2;
    if ((byte_count & 1U) != 0 || response_register_count != requested_register_count ||
        response_register_count > destination_capacity) {
        ESP_LOGE(tag, "Response contains %u registers; expected %u",
                 response_register_count, requested_register_count);
        return false;
    }

    if (receive_overflowed()) return false;
    for (std::uint16_t index = 0; index < response_register_count; ++index) {
        const std::size_t data_index = 3 + index * 2;
        destination[index] = (static_cast<std::uint16_t>(response[data_index]) << 8U) |
                             response[data_index + 1];
    }
    return !receive_overflowed();
}

} // namespace inverter_gateway::protocol
