// Compile the actual transport and parsers against deterministic host stubs.
// These tests do not access USB hardware, serial ports, MQTT or the network.
#include <cassert>
#include <cstdio>
#include <vector>
#include <limits>
#include "freertos/stream_buffer.h"
#include "usb/vcp.hpp"
#include "inverter_gateway/app/monitor_output.hpp"
TickType_t host_ticks = 0;
std::function<void()> host_after_send, host_after_receive, host_on_empty;
cdc_acm_host_device_config_t host_usb_config{};
std::function<esp_err_t(const std::uint8_t *, std::size_t)> host_tx;
unsigned host_tx_count = 0;
namespace inverter_gateway::app { bool monitor_output_enabled() { return false; } }
#include "../../main/src/transport/usb_rs485.cpp"
#include "../../main/src/protocol/modbus_rtu.cpp"

using inverter_gateway::transport::UsbRs485;
using inverter_gateway::protocol::ModbusRtuClient;
using inverter_gateway::protocol::ModbusWriteStatus;
using inverter_gateway::protocol::modbus_rtu_stats;
using Bytes = std::vector<std::uint8_t>;
Bytes frame(Bytes bytes) {
    const auto crc = inverter_gateway::protocol::calculate_modbus_crc(bytes.data(), bytes.size());
    bytes.push_back(static_cast<std::uint8_t>(crc)); bytes.push_back(static_cast<std::uint8_t>(crc >> 8)); return bytes;
}
void inject(const Bytes &bytes) { host_usb_config.data_cb(bytes.data(), bytes.size(), nullptr); }
void answer(const Bytes &bytes) { host_tx = [bytes](const std::uint8_t *, std::size_t) { inject(bytes); return ESP_OK; }; }

int main() {
    static_assert(static_cast<unsigned>(ModbusWriteStatus::receive_busy) == 12);
    assert(UsbRs485::install(32) == ESP_OK);
    UsbRs485::begin_connection_session();
    auto device = UsbRs485::open(10, 32);
    assert(device && UsbRs485::stats().reconnect_events == 0);
    ModbusRtuClient client(*device, 1, 20, 850);
    std::uint16_t value = 0;
    const auto valid = frame({1,4,2,0x12,0x34});
    answer(valid); assert(client.read_input(40,1,&value,1) && value == 0x1234);
    const auto first_tx_tick = host_ticks;
    answer(frame({1,3,2,0x56,0x78})); assert(client.read_holding(40,1,&value,1) && value == 0x5678);
    assert(host_ticks - first_tx_tick >= 850); // Existing bus gap is unchanged.

    host_tx = [valid](const std::uint8_t *, std::size_t) {
        inject(Bytes(valid.begin(), valid.begin()+3));
        host_on_empty = [valid] { inject(Bytes(valid.begin()+3, valid.end())); }; return ESP_OK;
    };
    assert(client.read_input(40,1,&value,1)); // Segmented, complete response.
    auto corrupt = valid; corrupt.back() ^= 1; answer(corrupt);
    const auto before_crc = modbus_rtu_stats().crc_errors;
    assert(!client.read_input(40,1,&value,1));
    assert(modbus_rtu_stats().crc_errors == before_crc+1);
    answer({1,4,2}); const auto before_partial = modbus_rtu_stats().incomplete_responses;
    assert(!client.read_input(40,1,&value,1));
    assert(modbus_rtu_stats().incomplete_responses == before_partial+1);
    answer({}); const auto before_timeout = modbus_rtu_stats().response_timeouts;
    assert(!client.read_input(40,1,&value,1));
    assert(modbus_rtu_stats().response_timeouts == before_timeout+1);

    // A valid prefix of an overflowing callback must never be accepted.
    auto overflowing = valid; overflowing.resize(80,0xa5);
    std::uint32_t generation = 0; assert(UsbRs485::prepare_receive_transaction(generation));
    host_after_send = [generation] { assert(UsbRs485::receive_overflow_since(generation)); };
    const auto before_drop = UsbRs485::stats();
    const auto before_overflow = modbus_rtu_stats().receive_overflow_failures;
    answer(overflowing); assert(!client.read_input(40,1,&value,1));
    assert(UsbRs485::stats().rx_dropped_bytes == before_drop.rx_dropped_bytes+48);
    assert(UsbRs485::stats().rx_overflow_events == before_drop.rx_overflow_events+1);
    assert(modbus_rtu_stats().receive_overflow_failures == before_overflow+1);
    answer(valid); assert(client.read_input(40,1,&value,1)); // Resynchronizes next transaction.

    // Overflow arriving immediately after the last response byte is read.
    answer(valid);
    unsigned received = 0;
    host_after_receive = [&] { if (++received == valid.size()) inject(Bytes(80,0xa5)); };
    assert(!client.read_input(40,1,&value,1)); host_after_receive = {};
    answer(valid); assert(client.read_input(40,1,&value,1));

    // An old overflow outside any transaction is drained, not sticky forever.
    inject(Bytes(80,0xa5)); answer(valid); assert(client.read_input(40,1,&value,1));
    const auto before_busy = host_tx_count;
    host_after_send = [&] {
        assert(client.write_single(207,1).status == ModbusWriteStatus::receive_busy);
        const std::uint16_t busy_values[]{1,2};
        assert(client.write_multiple(200,2,busy_values).status == ModbusWriteStatus::receive_busy);
    };
    inject({0xa5}); assert(host_tx_count == before_busy); // Producer-active admission sends nothing.

    const auto single = frame({1,6,0,207,0,1}); answer(single);
    assert(client.write_single(207,1).ok());
    received = 0;
    host_after_receive = [&] { if (++received == single.size()) inject(Bytes(80,0xa5)); };
    assert(client.write_single(207,1).status == ModbusWriteStatus::receive_overflow);
    host_after_receive = {};
    auto corrupt_single = single; corrupt_single.back() ^= 1; answer(corrupt_single);
    assert(client.write_single(207,1).status == ModbusWriteStatus::crc_error);
    auto write_overflow = single; write_overflow.resize(80,0);
    answer(write_overflow); const auto tx_before_write = host_tx_count;
    assert(client.write_single(207,1).status == ModbusWriteStatus::receive_overflow);
    assert(host_tx_count == tx_before_write+1); // Never retry a write.
    std::uint16_t values[]{1,2}; answer(frame({1,16,0,200,0,2}));
    assert(client.write_multiple(200,2,values).ok());
    received = 0;
    host_after_receive = [&] { if (++received == 8) inject(Bytes(80,0xa5)); };
    assert(client.write_multiple(200,2,values).status == ModbusWriteStatus::receive_overflow);
    host_after_receive = {};
    auto multi_overflow = frame({1,16,0,200,0,2}); multi_overflow.resize(80,0);
    answer(multi_overflow); assert(client.write_multiple(200,2,values).status == ModbusWriteStatus::receive_overflow);
    answer({1,6,0}); assert(client.write_single(207,1).status == ModbusWriteStatus::malformed_response);
    answer({}); assert(client.write_multiple(200,2,values).status == ModbusWriteStatus::timeout);

    cdc_acm_host_dev_event_data_t event{}; event.type = CDC_ACM_HOST_DEVICE_DISCONNECTED;
    host_usb_config.event_cb(&event,nullptr);
    assert(UsbRs485::disconnected() && UsbRs485::stats().disconnect_events == 1);
    UsbRs485::begin_connection_session();
    auto second = UsbRs485::open(10,32);
    assert(second && !UsbRs485::disconnected() && UsbRs485::stats().reconnect_events == 1);
    std::atomic<std::uint32_t> saturated{UINT32_MAX-1};
    inverter_gateway::diagnostics::add_saturating(saturated,10);
    inverter_gateway::diagnostics::add_saturating(saturated);
    assert(saturated.load() == UINT32_MAX);
    std::puts("PASS: actual USB/Modbus parser valid/partial/CRC/timeout/overflow/race/resync/write-no-retry/counter cases.");
}
