#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "usb/vcp.hpp"

namespace inverter_gateway::transport {

struct UsbRs485Stats {
    std::uint32_t rx_dropped_bytes; // Bytes rejected at bounded callback admission.
    std::uint32_t rx_overflow_events; // Callbacks with at least one dropped byte.
    std::uint32_t disconnect_events; // USB DEVICE_DISCONNECTED notifications.
    std::uint32_t reconnect_events; // Successful opens after the first successful open.
};

class UsbRs485 {
public:
    static esp_err_t install(std::size_t buffer_size);
    static std::unique_ptr<CdcAcmDevice> open(std::uint32_t connection_timeout_ms,
                                              std::size_t buffer_size);
    static esp_err_t configure(CdcAcmDevice &device, std::uint32_t baud);

    static void begin_connection_session();
    static bool disconnected();
    static std::size_t clear_receive_buffer();
    static StreamBufferHandle_t receive_stream();
    // One bus-task consumer only. Drain stale bytes and snapshot before TX;
    // false means an RX callback overlapped admission: do not transmit.
    static bool prepare_receive_transaction(std::uint32_t &overflow_generation);
    static bool receive_overflow_since(std::uint32_t overflow_generation);
    // Per-boot saturating counts; individual atomic loads, not one coherent snapshot.
    static UsbRs485Stats stats();
};

} // namespace inverter_gateway::transport
