#include "inverter_gateway/transport/usb_rs485.hpp"

#include <algorithm>
#include <atomic>
#include "inverter_gateway/diagnostics/bounded_counter.hpp"

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp_ch34x.hpp"

using esp_usb::CH34x;
using esp_usb::VCP;

namespace inverter_gateway::transport {
namespace {

constexpr EventBits_t device_disconnected_bit = BIT0;
constexpr char tag[] = "usb_rs485";

StreamBufferHandle_t rx_stream;
EventGroupHandle_t usb_events;
std::atomic<std::uint32_t> rx_overflow_generation{0};
std::atomic<std::uint32_t> rx_callback_sequence{0};
std::atomic<std::uint32_t> rx_dropped_bytes{0};
std::atomic<std::uint32_t> rx_overflow_events{0};
std::atomic<std::uint32_t> disconnect_events{0};
std::atomic<std::uint32_t> reconnect_events{0};
bool opened_before = false; // Only the bus task opens a device.

bool handle_rx(const std::uint8_t *data, std::size_t data_len, void *)
{
    rx_callback_sequence.fetch_add(1, std::memory_order_acq_rel);
    // Single producer/single consumer: the reader can only increase space.
    // Announce an overflow BEFORE publishing any bytes from its callback;
    // announcing it after Send would race acceptance of a valid frame prefix.
    const std::size_t accepted = std::min(data_len, xStreamBufferSpacesAvailable(rx_stream));
    if (accepted != data_len) {
        rx_overflow_generation.fetch_add(1, std::memory_order_release);
        diagnostics::add_saturating(rx_overflow_events);
    }
    const std::size_t stored = accepted == 0 ? 0 :
        xStreamBufferSend(rx_stream, data, accepted, 0);
    if (stored != data_len) {
        diagnostics::add_saturating(rx_dropped_bytes,
            static_cast<std::uint32_t>(data_len - stored));
        ESP_LOGW(tag, "USB receive buffer full; dropped %u byte(s)",
                 static_cast<unsigned>(data_len - stored));
    }
    rx_callback_sequence.fetch_add(1, std::memory_order_release);
    return true;
}

void handle_device_event(const cdc_acm_host_dev_event_data_t *event, void *)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        diagnostics::add_saturating(disconnect_events);
        xEventGroupSetBits(usb_events, device_disconnected_bit);
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(tag, "USB serial error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(tag, "USB serial state: 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

void usb_library_task(void *)
{
    while (true) {
        std::uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(tag, "USB host event handling failed: %s", esp_err_to_name(err));
            continue;
        }
        if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
            err = usb_host_device_free_all();
            if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(tag, "Could not free USB devices: %s", esp_err_to_name(err));
            }
        }
    }
}

} // namespace

esp_err_t UsbRs485::install(std::size_t buffer_size)
{
    rx_stream = xStreamBufferCreate(buffer_size, 1);
    usb_events = xEventGroupCreate();
    if (rx_stream == nullptr || usb_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(usb_library_task, "usb_events", 4096, nullptr, 10, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = cdc_acm_host_install(nullptr);
    if (err != ESP_OK) {
        return err;
    }
    VCP::register_driver<CH34x>();
    return ESP_OK;
}

std::unique_ptr<CdcAcmDevice> UsbRs485::open(std::uint32_t connection_timeout_ms,
                                             std::size_t buffer_size)
{
    const cdc_acm_host_device_config_t config = {
        .connection_timeout_ms = connection_timeout_ms,
        .out_buffer_size = buffer_size,
        .in_buffer_size = buffer_size,
        .event_cb = handle_device_event,
        .data_cb = handle_rx,
        .user_arg = nullptr,
    };
    std::unique_ptr<CdcAcmDevice> device(VCP::open(&config));
    if (device) {
        if (opened_before) diagnostics::add_saturating(reconnect_events);
        opened_before = true;
    }
    return device;
}

esp_err_t UsbRs485::configure(CdcAcmDevice &device, std::uint32_t baud)
{
    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = baud,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };
    esp_err_t err = device.line_coding_set(&line_coding);
    if (err != ESP_OK) {
        return err;
    }
    return device.set_control_line_state(true, true);
}

void UsbRs485::begin_connection_session()
{
    xEventGroupClearBits(usb_events, device_disconnected_bit);
    clear_receive_buffer();
}

bool UsbRs485::disconnected()
{
    return (xEventGroupGetBits(usb_events) & device_disconnected_bit) != 0;
}

std::size_t UsbRs485::clear_receive_buffer()
{
    std::uint8_t scratch[64];
    std::size_t discarded = 0;
    if (rx_stream == nullptr) return 0;
    // Bound the drain to the current occupancy; a noisy producer must not
    // trap the bus task here. Never reset a stream while its producer runs.
    const std::size_t pending = xStreamBufferBytesAvailable(rx_stream);
    while (discarded < pending) {
        const auto received = xStreamBufferReceive(rx_stream, scratch,
            std::min(sizeof(scratch), pending - discarded), 0);
        if (received == 0) break;
        discarded += received;
    }
    return discarded;
}

bool UsbRs485::prepare_receive_transaction(std::uint32_t &generation)
{
    const auto before = rx_callback_sequence.load(std::memory_order_acquire);
    generation = rx_overflow_generation.load(std::memory_order_acquire);
    clear_receive_buffer();
    const auto after = rx_callback_sequence.load(std::memory_order_acquire);
    return (before & 1U) == 0 && before == after && !receive_overflow_since(generation);
}

bool UsbRs485::receive_overflow_since(std::uint32_t generation)
{
    return rx_overflow_generation.load(std::memory_order_acquire) != generation;
}

UsbRs485Stats UsbRs485::stats()
{
    return {rx_dropped_bytes.load(std::memory_order_relaxed),
            rx_overflow_events.load(std::memory_order_relaxed),
            disconnect_events.load(std::memory_order_relaxed),
            reconnect_events.load(std::memory_order_relaxed)};
}

StreamBufferHandle_t UsbRs485::receive_stream()
{
    return rx_stream;
}

} // namespace inverter_gateway::transport
