#include "inverter_gateway/transport/usb_hid_inverter.hpp"

#include <cstring>

#include "byte_transport_detail.hpp"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "inverter_gateway/transport/hid_device_selection.hpp"
#include "usb/usb_host.h"

namespace inverter_gateway::transport {
namespace {
constexpr char tag[] = "inverter_hid";
constexpr std::size_t report_size = 8;
constexpr std::uint16_t maximum_descriptor_size = 256;
constexpr std::int64_t input_retry_delay_us = 250000;
constexpr unsigned maximum_input_failures = 3;
enum class ControlMode { none, descriptor, output, input_recovery };
enum TxPhase : unsigned { idle, queued, started, done, canceled };

// This lifetime state is never freed while USB can invoke a callback. Only
// client_task touches handles and transfer ownership, including hotplug close.
struct HidState {
    UsbHidInverterConfig config;
    detail::ReceivePipe receive_pipe;
    SemaphoreHandle_t transaction_mutex = nullptr;
    usb_host_client_handle_t client = nullptr;
    usb_device_handle_t device = nullptr;
    usb_transfer_t *input = nullptr;
    usb_transfer_t *control = nullptr;
    std::atomic<bool> installed{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> quarantine{false};
    std::atomic<unsigned> tx_phase{idle};
    std::atomic<bool> tx_accepted{false};
    std::atomic<std::uint32_t> session{0};
    std::uint32_t tx_session = 0;
    std::uint8_t tx_report[report_size]{};
    std::int64_t tx_deadline = 0;
    esp_err_t tx_result = ESP_OK;
    std::uint8_t pending_addresses[4]{};
    std::uint16_t detected_vendor_id = 0;
    std::uint16_t detected_product_id = 0;
    unsigned candidate_index = 0;
    std::uint8_t interface_number = 0;
    std::uint8_t endpoint = 0;
    std::uint16_t descriptor_size = 0;
    std::int64_t probe_deadline = 0;
    bool claimed = false;
    bool gone = false;
    bool input_inflight = false;
    bool control_inflight = false;
    bool input_recovery_pending = false;
    bool input_stalled = false;
    bool input_reclaim_required = false;
    unsigned input_failures = 0;
    std::int64_t input_retry_at = 0;
    std::int64_t input_recovery_deadline = 0;
    bool halted = false;
    bool advance_interface = false;
    ControlMode control_mode = ControlMode::none;
};
HidState state;
TaskHandle_t library_handle = nullptr;
TaskHandle_t client_handle = nullptr;
bool host_installed = false;

void mark_unusable()
{
    state.ready.store(false, std::memory_order_release);
    state.receive_pipe.invalidate();
    state.quarantine.store(true, std::memory_order_release);
}

void input_complete(usb_transfer_t *transfer)
{
    state.input_inflight = false;
    if (state.gone || state.quarantine.load()) return;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes >= 0 && transfer->actual_num_bytes <= static_cast<int>(report_size)) {
        state.input_failures = 0;
        // USB reports may be short. A zero-length report is idle; copying the
        // rest of an eight-byte DMA buffer would inject bytes never received.
        if (transfer->actual_num_bytes != 0)
            state.receive_pipe.append(transfer->data_buffer, transfer->actual_num_bytes);
        return;
    }
    state.receive_pipe.invalidate();
    ESP_LOGW(tag, "HID input status=%u actual=%d requested=%d ep=%02x VID=%04x PID=%04x",
        static_cast<unsigned>(transfer->status), transfer->actual_num_bytes, transfer->num_bytes,
        state.endpoint, state.detected_vendor_id, state.detected_product_id);
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED ||
        transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        ++state.input_failures >= maximum_input_failures) {
        mark_unusable();
        return;
    }
    state.input_recovery_pending = true;
    state.input_stalled = transfer->status == USB_TRANSFER_STATUS_STALL;
    state.input_retry_at = esp_timer_get_time() + input_retry_delay_us;
}

void control_complete(usb_transfer_t *transfer)
{
    state.control_inflight = false;
    const auto mode = state.control_mode;
    state.control_mode = ControlMode::none;
    if (mode == ControlMode::input_recovery) {
        if (state.gone || state.quarantine.load()) return;
        if (transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
            transfer->actual_num_bytes != USB_SETUP_PACKET_SIZE ||
            esp_timer_get_time() >= state.input_recovery_deadline) {
            ESP_LOGW(tag, "HID endpoint clear failed status=%u actual=%d",
                static_cast<unsigned>(transfer->status), transfer->actual_num_bytes);
            mark_unusable();
        } else {
            state.input_stalled = false;
            state.input_reclaim_required = true;
        }
        return;
    }
    if (mode == ControlMode::output) {
        state.tx_result = !state.gone && !state.quarantine.load() &&
            transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            transfer->actual_num_bytes == static_cast<int>(USB_SETUP_PACKET_SIZE + report_size)
            ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        state.tx_phase.store(done, std::memory_order_release);
        if (state.tx_result != ESP_OK) mark_unusable();
        return;
    }
    if (mode != ControlMode::descriptor) return;
    if (state.gone || state.quarantine.load() || esp_timer_get_time() >= state.probe_deadline ||
        transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
        transfer->actual_num_bytes != USB_SETUP_PACKET_SIZE + state.descriptor_size) {
        ESP_LOGW(tag, "HID report descriptor transfer failed or expired");
        mark_unusable();
        return;
    }
    const bool automatic = state.config.vendor_id == 0 || state.config.product_id == 0;
    if (!is_usb_hid_inverter_report(transfer->data_buffer + USB_SETUP_PACKET_SIZE,
        state.descriptor_size, automatic)) {
        ESP_LOGW(tag, "Ignoring HID interface %u; need unnumbered 8-byte IN/OUT inverter reports",
            state.interface_number);
        state.advance_interface = true;
        return;
    }
    state.session.fetch_add(1, std::memory_order_release);
    state.input_failures = 0;
    state.input_recovery_pending = false;
    state.input_stalled = false;
    state.input_reclaim_required = false;
    state.input_retry_at = 0;
    state.ready.store(true, std::memory_order_release);
    ESP_LOGI(tag, "HID inverter connected VID=%04x PID=%04x interface=%u",
        state.detected_vendor_id, state.detected_product_id, state.interface_number);
}

void device_event(const usb_host_client_event_msg_t *event, void *)
{
    if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
        event->dev_gone.dev_hdl == state.device) {
        state.gone = true;
        state.session.fetch_add(1, std::memory_order_release);
        mark_unusable();
        ESP_LOGW(tag, "USB HID disconnected");
    } else if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        for (auto &address : state.pending_addresses) {
            if (address == 0) { address = event->new_dev.address; break; }
        }
    }
}

bool find_interface(const usb_config_desc_t *config)
{
    if (config == nullptr || config->wTotalLength < sizeof(usb_config_desc_t) ||
        config->wTotalLength > 2048) return false;
    UsbHidInterface selected;
    if (!select_usb_hid_interface(reinterpret_cast<const std::uint8_t *>(config),
        config->wTotalLength, selected, state.candidate_index)) return false;
    state.interface_number = selected.interface_number;
    state.endpoint = selected.endpoint;
    state.descriptor_size = selected.report_descriptor_size;
    return true;
}

void close_when_safe()
{
    if (state.device == nullptr || (!state.gone && !state.quarantine.load())) return;
    state.ready.store(false, std::memory_order_release);
    if (!state.halted && state.claimed) {
        // Callback draining happens in this same task before releasing handles.
        usb_host_endpoint_halt(state.device, state.endpoint);
        usb_host_endpoint_flush(state.device, state.endpoint);
        state.halted = true;
    }
    // IDF5.5 does not implement control-transfer timeout_ms/cancel. An expired
    // SET_REPORT retains its DMA buffer until callback or unplug, and closes
    // admission meanwhile. This never replays that uncertain write.
    if (state.input_inflight || state.control_inflight) return;
    if (state.claimed) {
        if (usb_host_interface_release(state.client, state.device, state.interface_number) != ESP_OK) return;
        state.claimed = false;
    }
    if (usb_host_device_close(state.client, state.device) != ESP_OK) return;
    state.device = nullptr;
    state.gone = false;
    state.halted = false;
    state.advance_interface = false;
    state.input_recovery_pending = false;
    state.input_stalled = false;
    state.input_reclaim_required = false;
    state.input_retry_at = 0;
    state.detected_vendor_id = 0;
    state.detected_product_id = 0;
    state.quarantine.store(false, std::memory_order_release);
    // A connected unsupported or stalled device is not reopened automatically.
    // A new enumeration event (unplug/replug) is required to probe it again.
}

void probe_interface()
{
    const usb_config_desc_t *config_desc = nullptr;
    if (usb_host_get_active_config_descriptor(state.device, &config_desc) != ESP_OK ||
        !find_interface(config_desc)) {
        ESP_LOGW(tag, "No compatible HID interface for VID=%04x PID=%04x",
            state.detected_vendor_id, state.detected_product_id);
        state.quarantine.store(true);
        close_when_safe();
        return;
    }
    if (usb_host_interface_claim(state.client, state.device, state.interface_number, 0) != ESP_OK) {
        state.quarantine.store(true);
        close_when_safe();
        return;
    }
    state.claimed = true;
    auto *setup = reinterpret_cast<usb_setup_packet_t *>(state.control->data_buffer);
    setup->bmRequestType = 0x81; // IN, standard, interface.
    setup->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    setup->wValue = 0x2200; // HID report descriptor.
    setup->wIndex = state.interface_number;
    setup->wLength = state.descriptor_size;
    state.control->num_bytes = USB_SETUP_PACKET_SIZE + state.descriptor_size;
    state.control->device_handle = state.device;
    state.control->bEndpointAddress = 0;
    state.control_mode = ControlMode::descriptor;
    state.probe_deadline = esp_timer_get_time() + 2000000;
    if (usb_host_transfer_submit_control(state.client, state.control) == ESP_OK)
        state.control_inflight = true;
    else mark_unusable();
}

void open_candidate(std::uint8_t address)
{
    if (usb_host_device_open(state.client, address, &state.device) != ESP_OK) {
        state.device = nullptr;
        return;
    }
    const usb_device_desc_t *device_desc = nullptr;
    if (usb_host_get_device_descriptor(state.device, &device_desc) != ESP_OK ||
        device_desc == nullptr || !usb_hid_ids_match(device_desc->idVendor,
            device_desc->idProduct, state.config.vendor_id, state.config.product_id)) {
        state.quarantine.store(true);
        close_when_safe();
        return;
    }
    state.detected_vendor_id = device_desc->idVendor;
    state.detected_product_id = device_desc->idProduct;
    state.candidate_index = 0;
    state.advance_interface = false;
    probe_interface();
}

void probe_next_interface()
{
    if (!state.advance_interface || state.device == nullptr || state.gone ||
        state.quarantine.load() || state.control_inflight || state.input_inflight) return;
    state.advance_interface = false;
    if (state.claimed) {
        if (usb_host_interface_release(state.client, state.device, state.interface_number) != ESP_OK) {
            mark_unusable();
            return;
        }
        state.claimed = false;
    }
    ++state.candidate_index;
    probe_interface();
}

void service_output()
{
    auto phase = state.tx_phase.load(std::memory_order_acquire);
    if (phase == canceled) {
        state.tx_phase.store(idle, std::memory_order_release);
        return;
    }
    if (phase != queued || state.control_inflight || state.input_recovery_pending) return;
    const bool same_session = state.ready.load() && state.tx_session == state.session.load();
    if (!same_session || esp_timer_get_time() >= state.tx_deadline) {
        state.tx_result = same_session ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
        unsigned expected = queued;
        state.tx_phase.compare_exchange_strong(expected, done, std::memory_order_release);
        return;
    }
    unsigned expected = queued;
    if (!state.tx_phase.compare_exchange_strong(expected, started, std::memory_order_acq_rel)) return;
    // started conservatively counts as admitted even if submit fails: the caller
    // can return at its deadline while this nonblocking API is executing.
    state.tx_accepted.store(true, std::memory_order_release);
    auto *setup = reinterpret_cast<usb_setup_packet_t *>(state.control->data_buffer);
    setup->bmRequestType = 0x21; // OUT, class, interface.
    setup->bRequest = 0x09; // SET_REPORT.
    setup->wValue = 0x0200; // Output report, report ID0.
    setup->wIndex = state.interface_number;
    setup->wLength = report_size;
    std::memcpy(state.control->data_buffer + USB_SETUP_PACKET_SIZE, state.tx_report, report_size);
    state.control->num_bytes = USB_SETUP_PACKET_SIZE + report_size;
    state.control->device_handle = state.device;
    state.control->bEndpointAddress = 0;
    state.control_mode = ControlMode::output;
    state.tx_result = usb_host_transfer_submit_control(state.client, state.control);
    if (state.tx_result == ESP_OK) state.control_inflight = true;
    else {
        state.control_mode = ControlMode::none;
        state.tx_phase.store(done, std::memory_order_release);
        mark_unusable();
    }
}

void service_input_recovery()
{
    if (!state.input_recovery_pending || state.device == nullptr ||
        state.gone || state.quarantine.load() || state.input_inflight ||
        state.control_inflight || esp_timer_get_time() < state.input_retry_at) return;
    if (state.input_stalled) {
        // Clearing the host pipe alone cannot clear a device's STALL. This
        // control buffer remains owned until its callback, including timeout.
        auto *setup = reinterpret_cast<usb_setup_packet_t *>(state.control->data_buffer);
        setup->bmRequestType = 0x02; // OUT, standard, endpoint.
        setup->bRequest = USB_B_REQUEST_CLEAR_FEATURE;
        setup->wValue = 0; // ENDPOINT_HALT.
        setup->wIndex = state.endpoint;
        setup->wLength = 0;
        state.control->num_bytes = USB_SETUP_PACKET_SIZE;
        state.control->device_handle = state.device;
        state.control->bEndpointAddress = 0;
        state.control_mode = ControlMode::input_recovery;
        state.input_recovery_deadline = esp_timer_get_time() + 2000000;
        if (usb_host_transfer_submit_control(state.client, state.control) == ESP_OK)
            state.control_inflight = true;
        else { state.control_mode = ControlMode::none; mark_unusable(); }
        return;
    }
    if (state.input_reclaim_required) {
        // CLEAR_FEATURE resets the device to DATA0. IDF5.5 endpoint_clear
        // preserves the host toggle, so allocate a fresh pipe after both DMA
        // callbacks have drained. Claiming this interface sends no commands.
        auto error = usb_host_interface_release(state.client, state.device, state.interface_number);
        if (error == ESP_OK) {
            state.claimed = false;
            error = usb_host_interface_claim(state.client, state.device, state.interface_number, 0);
            if (error == ESP_OK) state.claimed = true;
        }
        if (error != ESP_OK) {
            ESP_LOGW(tag, "HID input interface recovery failed: %s", esp_err_to_name(error));
            mark_unusable();
            return;
        }
        state.input_reclaim_required = false;
    } else {
        const auto halted = usb_host_endpoint_halt(state.device, state.endpoint);
        const auto flushed = halted == ESP_OK
            ? usb_host_endpoint_flush(state.device, state.endpoint) : halted;
        const auto cleared = flushed == ESP_OK
            ? usb_host_endpoint_clear(state.device, state.endpoint) : flushed;
        if (cleared != ESP_OK) {
            ESP_LOGW(tag, "HID input endpoint recovery failed: %s", esp_err_to_name(cleared));
            mark_unusable();
            return;
        }
    }
    state.input_recovery_pending = false;
    // This is the same physical connection: do not reset the session and
    // thereby clear the protocol owner's uncertain-setting guard.
    ESP_LOGW(tag, "HID input endpoint recovered; resuming monitoring (attempt %u)",
        state.input_failures);
}

void service_input()
{
    if (!state.ready.load() || state.input_inflight || state.quarantine.load() ||
        state.input_recovery_pending || state.gone) return;
    state.input->device_handle = state.device;
    state.input->bEndpointAddress = state.endpoint;
    state.input->num_bytes = report_size;
    const auto error = usb_host_transfer_submit(state.input);
    if (error == ESP_OK) state.input_inflight = true;
    else {
        state.receive_pipe.invalidate();
        ESP_LOGW(tag, "HID input submit failed: %s", esp_err_to_name(error));
        if (++state.input_failures >= maximum_input_failures) mark_unusable();
        else {
            state.input_recovery_pending = true;
            state.input_retry_at = esp_timer_get_time() + input_retry_delay_us;
        }
    }
}

void client_task(void *)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (true) {
        usb_host_client_handle_events(state.client, std::max<TickType_t>(1, pdMS_TO_TICKS(5)));
        if (state.control_inflight && state.control_mode == ControlMode::descriptor &&
            esp_timer_get_time() >= state.probe_deadline && !state.quarantine.load()) mark_unusable();
        if (state.control_inflight && state.control_mode == ControlMode::output &&
            esp_timer_get_time() >= state.tx_deadline && !state.quarantine.load()) mark_unusable();
        if (state.control_inflight && state.control_mode == ControlMode::input_recovery &&
            esp_timer_get_time() >= state.input_recovery_deadline && !state.quarantine.load()) mark_unusable();
        close_when_safe();
        probe_next_interface();
        if (state.device == nullptr) {
            for (auto &address : state.pending_addresses) {
                if (address == 0) continue;
                const auto candidate = address;
                address = 0;
                open_candidate(candidate);
                if (state.device != nullptr) break;
            }
        }
        service_input_recovery();
        service_input();
        service_output();
    }
}

void library_task(void *)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (true) {
        std::uint32_t flags = 0;
        const auto err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) ESP_LOGW(tag, "USB library error: %s", esp_err_to_name(err));
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) usb_host_device_free_all();
    }
}

void startup_cleanup()
{
    // Startup workers are still waiting on their activation notifications.
    if (client_handle != nullptr) { vTaskDelete(client_handle); client_handle = nullptr; }
    if (library_handle != nullptr) { vTaskDelete(library_handle); library_handle = nullptr; }
    if (state.client != nullptr) { usb_host_client_deregister(state.client); state.client = nullptr; }
    if (host_installed) {
        usb_host_device_free_all();
        usb_host_uninstall();
        host_installed = false;
    }
    usb_host_transfer_free(state.input);
    usb_host_transfer_free(state.control);
    state.input = state.control = nullptr;
    if (state.receive_pipe.stream != nullptr) vStreamBufferDelete(state.receive_pipe.stream);
    if (state.transaction_mutex != nullptr) vSemaphoreDelete(state.transaction_mutex);
    state.receive_pipe.stream = nullptr;
    state.transaction_mutex = nullptr;
}
} // namespace

esp_err_t UsbHid::install(const UsbHidInverterConfig &config)
{
    if (state.installed.load() || state.client != nullptr) return ESP_ERR_INVALID_STATE;
    state.config = config;
    state.receive_pipe.stream = xStreamBufferCreate(2048, 1);
    state.transaction_mutex = xSemaphoreCreateMutex();
    esp_err_t err = state.receive_pipe.stream == nullptr || state.transaction_mutex == nullptr
        ? ESP_ERR_NO_MEM : usb_host_transfer_alloc(report_size, 0, &state.input);
    if (err == ESP_OK) err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + maximum_descriptor_size,
        0, &state.control);
    if (err != ESP_OK) { startup_cleanup(); return err; }
    state.input->callback = input_complete;
    state.control->callback = control_complete;
    usb_host_config_t host_config{};
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    // A failed install is never treated as an existing shared host. Only one
    // selected transport may own USB in a boot; Modbus startup is untouched.
    err = usb_host_install(&host_config);
    if (err != ESP_OK) { startup_cleanup(); return err; }
    host_installed = true;
    usb_host_client_config_t client_config{};
    client_config.max_num_event_msg = 8;
    client_config.async.client_event_callback = device_event;
    err = usb_host_client_register(&client_config, &state.client);
    if (err != ESP_OK) { startup_cleanup(); return err; }
    if (xTaskCreate(library_task, "hid_usb_events", 3072, nullptr, 10, &library_handle) != pdPASS ||
        xTaskCreate(client_task, "inverter_hid", 4096, nullptr, 8, &client_handle) != pdPASS) {
        startup_cleanup(); return ESP_ERR_NO_MEM;
    }
    state.installed.store(true, std::memory_order_release);
    xTaskNotifyGive(library_handle);
    xTaskNotifyGive(client_handle);
    return ESP_OK;
}

bool UsbHid::connected() const
{
    return state.installed.load(std::memory_order_acquire) && state.ready.load(std::memory_order_acquire) &&
        !state.quarantine.load(std::memory_order_acquire);
}

std::uint32_t UsbHid::session_id() const { return state.session.load(std::memory_order_acquire); }

esp_err_t UsbHid::exchange(const std::uint8_t *command, std::size_t command_size,
    std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
    ReceiveRule rule, bool &transmitted)
{
    const auto capacity = response_size;
    response_size = 0;
    transmitted = false;
    if (!detail::valid_exchange(command, command_size, response, capacity, timeout_ms, rule) ||
        command_size == 0) return ESP_ERR_INVALID_ARG;
    if (!state.installed.load()) return ESP_ERR_INVALID_STATE;
    const detail::Deadline deadline(timeout_ms);
    if (!detail::take_until(state.transaction_mutex, deadline)) return ESP_ERR_TIMEOUT;
    while (!connected() && !deadline.expired()) vTaskDelay(1);
    esp_err_t err = connected() ? ESP_OK : ESP_ERR_TIMEOUT;
    const auto session = state.session.load(std::memory_order_acquire);
    if (err == ESP_OK && rule.expected_session != 0 && rule.expected_session != session)
        err = ESP_ERR_INVALID_STATE;
    std::uint32_t snapshot = 0;
    if (err == ESP_OK && !state.receive_pipe.prepare(snapshot)) err = ESP_ERR_INVALID_RESPONSE;
    // Match the reference HID driver's 50 ms report pacing. USB control
    // completion does not establish that the inverter's serial bridge has
    // consumed the preceding eight bytes.
    auto next_report_us = esp_timer_get_time() + 50000;
    for (std::size_t offset = 0; err == ESP_OK && offset < command_size; offset += report_size) {
        while (!deadline.expired() && esp_timer_get_time() < next_report_us) vTaskDelay(1);
        if (deadline.expired()) { err = ESP_ERR_TIMEOUT; break; }
        if (!connected() || session != state.session.load()) { err = ESP_ERR_INVALID_STATE; break; }
        auto phase = state.tx_phase.load(std::memory_order_acquire);
        if (phase == done) { state.tx_phase.store(idle); phase = idle; }
        if (phase != idle) { err = ESP_ERR_INVALID_STATE; break; }
        if (deadline.expired()) { err = ESP_ERR_TIMEOUT; break; }
        std::memset(state.tx_report, 0, report_size);
        std::memcpy(state.tx_report, command + offset, std::min(report_size, command_size - offset));
        state.tx_deadline = deadline.end();
        state.tx_session = session;
        state.tx_accepted.store(false);
        state.tx_phase.store(queued, std::memory_order_release);
        usb_host_client_unblock(state.client);
        while (!deadline.expired()) {
            phase = state.tx_phase.load(std::memory_order_acquire);
            if (phase == done) break;
            vTaskDelay(1);
        }
        phase = state.tx_phase.load(std::memory_order_acquire);
        transmitted = transmitted || state.tx_accepted.load(std::memory_order_acquire) || phase == started;
        if (phase == done) {
            err = state.tx_result;
            state.tx_phase.store(idle, std::memory_order_release);
            if (deadline.expired()) err = ESP_ERR_TIMEOUT;
        } else {
            unsigned expected = queued;
            if (!state.tx_phase.compare_exchange_strong(expected, canceled, std::memory_order_acq_rel) &&
                expected == started) {
                transmitted = true;
                mark_unusable();
            }
            err = ESP_ERR_TIMEOUT;
        }
        if (err == ESP_OK && (!connected() || session != state.session.load())) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK && !state.receive_pipe.valid(snapshot)) err = ESP_ERR_INVALID_RESPONSE;
        next_report_us = esp_timer_get_time() + 50000;
    }
    if (err == ESP_OK) err = detail::receive(state.receive_pipe, snapshot, response, capacity,
        response_size, deadline, rule, [this, session] { return connected() && session == state.session.load(); });
    xSemaphoreGive(state.transaction_mutex);
    return err;
}

} // namespace inverter_gateway::transport
