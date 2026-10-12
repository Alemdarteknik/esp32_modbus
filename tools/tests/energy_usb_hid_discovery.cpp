// Compiles the actual HID transport against deterministic USB-host stubs.
// Tests enumeration, asynchronous report admission and actual byte exchanges;
// no USB devices are opened and the FreeRTOS tasks are deliberately not started.
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "freertos/stream_buffer.h"
#include "usb/usb_host.h"
TickType_t host_ticks = 0;
std::function<void()> host_after_send, host_after_receive, host_on_empty;
std::function<void()> host_on_delay;
#include "../../main/src/transport/usb_hid_inverter.cpp"

using namespace inverter_gateway::transport;
using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0;
void check(bool condition, const char *expression, unsigned line)
{
    ++checks;
    if (!condition) throw std::runtime_error(
        "line " + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check((expression), #expression, __LINE__)

const Bytes byte_pipe{0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
    0x75, 0x08, 0x95, 0x08, 0x09, 0x01, 0x81, 0x02,
    0x09, 0x02, 0x91, 0x02, 0xc0};
struct Interface {
    unsigned number = 0;
    unsigned cls = 3, subclass = 0, protocol = 0;
    unsigned endpoint = 0x81, packet_size = 8, attributes = 3;
    Bytes report = byte_pipe;
};

HostHidDevice device(unsigned vendor, unsigned product,
    std::initializer_list<Interface> interfaces = {Interface{}})
{
    HostHidDevice result;
    result.descriptor.idVendor = vendor;
    result.descriptor.idProduct = product;
    result.configuration = {9, 2, 0, 0, static_cast<std::uint8_t>(interfaces.size()),
        1, 0, 0x80, 50};
    for (const auto &iface : interfaces) {
        const Bytes descriptor{9, 4, static_cast<std::uint8_t>(iface.number), 0, 1,
            static_cast<std::uint8_t>(iface.cls), static_cast<std::uint8_t>(iface.subclass),
            static_cast<std::uint8_t>(iface.protocol), 0,
            9, 0x21, 0x11, 0x01, 0, 1, 0x22,
            static_cast<std::uint8_t>(iface.report.size()),
            static_cast<std::uint8_t>(iface.report.size() >> 8),
            7, 5, static_cast<std::uint8_t>(iface.endpoint),
            static_cast<std::uint8_t>(iface.attributes),
            static_cast<std::uint8_t>(iface.packet_size),
            static_cast<std::uint8_t>(iface.packet_size >> 8), 1};
        result.configuration.insert(result.configuration.end(), descriptor.begin(), descriptor.end());
        result.reports[iface.number] = iface.report;
    }
    result.configuration[2] = static_cast<std::uint8_t>(result.configuration.size());
    result.configuration[3] = static_cast<std::uint8_t>(result.configuration.size() >> 8);
    return result;
}

void reset(UsbHid &transport, UsbHidInverterConfig filter = {})
{
    // Each case has completed or canceled its synthetic control transfer.
    if (state.installed.load()) startup_cleanup();
    state.~HidState();
    new (&state) HidState;
    host_hid_usb = {};
    host_ticks = 0;
    host_after_send = {}; host_after_receive = {}; host_on_empty = {}; host_on_delay = {};
    CHECK(transport.install(filter) == ESP_OK);
    CHECK(!transport.connected());
}

void complete_descriptor(bool fail = false, bool truncate = false)
{
    auto *transfer = host_hid_usb.pending_control;
    CHECK(transfer != nullptr);
    host_hid_usb.pending_control = nullptr;
    const auto setup = *reinterpret_cast<const usb_setup_packet_t *>(transfer->data_buffer);
    const auto &report = host_hid_usb.opened->reports.at(setup.wIndex);
    const auto length = std::min<std::size_t>(setup.wLength, report.size());
    std::memcpy(transfer->data_buffer + USB_SETUP_PACKET_SIZE, report.data(), length);
    transfer->status = fail ? USB_TRANSFER_STATUS_ERROR : USB_TRANSFER_STATUS_COMPLETED;
    transfer->actual_num_bytes = static_cast<int>(USB_SETUP_PACKET_SIZE + length) - (truncate ? 1 : 0);
    transfer->callback(transfer);
    probe_next_interface();
    close_when_safe();
}

void probe(HostHidDevice candidate)
{
    host_hid_usb.devices[1] = std::move(candidate);
    open_candidate(1);
}

void complete_input(const Bytes &bytes, int status = USB_TRANSFER_STATUS_COMPLETED,
    int actual_size = std::numeric_limits<int>::min())
{
    CHECK(bytes.size() <= report_size);
    std::memset(state.input->data_buffer, 0xa5, report_size);
    if (!bytes.empty()) std::memcpy(state.input->data_buffer, bytes.data(), bytes.size());
    state.input->status = status;
    state.input->actual_num_bytes = actual_size == std::numeric_limits<int>::min()
        ? static_cast<int>(bytes.size()) : actual_size;
    host_hid_usb.pending_input = nullptr;
    state.input_inflight = true;
    input_complete(state.input);
    CHECK(!state.input_inflight);
}

FrameDecision terminated(const std::uint8_t *bytes, std::size_t length, void *)
{
    return bytes[length - 1] == '\r' ? FrameDecision::complete : FrameDecision::incomplete;
}

void complete_output(bool success = true)
{
    auto *transfer = host_hid_usb.pending_control;
    CHECK(transfer != nullptr && state.control_mode == ControlMode::output);
    host_hid_usb.pending_control = nullptr;
    transfer->status = success ? USB_TRANSFER_STATUS_COMPLETED : USB_TRANSFER_STATUS_ERROR;
    transfer->actual_num_bytes = USB_SETUP_PACKET_SIZE + report_size;
    transfer->callback(transfer);
}

void connect(UsbHid &transport)
{
    reset(transport);
    probe(device(0x0665, 0x5161));
    complete_descriptor();
    CHECK(transport.connected());
}

void input_report_cases(UsbHid &transport)
{
    // These are the real PI30 QPI reply bytes, including the CRC 0x9a0b.
    // Test every interrupt fragment length independently of the 8-byte maximum.
    const Bytes frame{'(', 'P', 'I', '3', '0', 0x9a, 0x0b, '\r'};
    for (std::size_t fragment = 1; fragment <= report_size; ++fragment) {
        connect(transport);
        const auto session = transport.session_id();
        std::uint32_t snapshot = 0;
        CHECK(state.receive_pipe.prepare(snapshot));
        complete_input({}); // Completed empty interrupt report is idle.
        CHECK(transport.connected() && transport.session_id() == session);
        CHECK(state.receive_pipe.valid(snapshot));
        CHECK(xStreamBufferBytesAvailable(state.receive_pipe.stream) == 0);
        for (std::size_t offset = 0; offset < frame.size(); offset += fragment) {
            const auto length = std::min(fragment, frame.size() - offset);
            complete_input(Bytes(frame.begin() + offset, frame.begin() + offset + length));
            CHECK(transport.connected() && transport.session_id() == session);
        }
        std::uint8_t response[32]{};
        std::size_t size = 0;
        CHECK(detail::receive(state.receive_pipe, snapshot, response, sizeof(response), size,
            detail::Deadline(100), {terminated, nullptr}, [&] { return transport.connected(); }) == ESP_OK);
        CHECK(size == frame.size() && std::memcmp(response, frame.data(), size) == 0);
        CHECK(host_hid_usb.output_count == 0);
    }

    // Binary protocol payloads contain genuine zero bytes. Short reports must
    // append only their actual bytes and must not strip zero as presumed padding.
    connect(transport);
    std::uint32_t snapshot = 0;
    CHECK(state.receive_pipe.prepare(snapshot));
    complete_input({0x55, 0x00});
    complete_input({0xaa, '\r'});
    std::uint8_t response[32]{};
    std::size_t size = 0;
    CHECK(detail::receive(state.receive_pipe, snapshot, response, sizeof(response), size,
        detail::Deadline(100), {terminated, nullptr}, [&] { return transport.connected(); }) == ESP_OK);
    CHECK(size == 4 && response[0] == 0x55 && response[1] == 0 && response[2] == 0xaa);

    // The frame parser stops at CR, leaving only post-frame report padding for
    // the next transaction's bounded drain. Padding never enters the reply.
    CHECK(state.receive_pipe.prepare(snapshot));
    complete_input({'(', 'A', 'C', 'K', 0x39, 0x20, '\r', 0});
    size = 0;
    CHECK(detail::receive(state.receive_pipe, snapshot, response, sizeof(response), size,
        detail::Deadline(100), {terminated, nullptr}, [&] { return transport.connected(); }) == ESP_OK);
    CHECK(size == 7 && response[size - 1] == '\r');
    CHECK(xStreamBufferBytesAvailable(state.receive_pipe.stream) == 1);
    CHECK(state.receive_pipe.prepare(snapshot));
    CHECK(xStreamBufferBytesAvailable(state.receive_pipe.stream) == 0);
}

void exchange_cases(UsbHid &transport)
{
    const Bytes command{'Q', 'P', 'I', 0xbe, 0xac, '\r'};
    const Bytes reply{'(', 'P', 'I', '3', '0', 0x9a, 0x0b, '\r'};
    std::uint8_t response[32]{};
    std::size_t size = sizeof(response);
    bool transmitted = false;

    connect(transport);
    host_on_delay = [&] {
        service_output();
        if (host_hid_usb.pending_control != nullptr) complete_output();
    };
    host_on_empty = [&] {
        complete_input({});
        complete_input(Bytes(reply.begin(), reply.begin() + 3));
        complete_input(Bytes(reply.begin() + 3, reply.end()));
    };
    CHECK(transport.exchange(command.data(), command.size(), response, size, 200,
        {terminated, nullptr, transport.session_id()}, transmitted) == ESP_OK);
    CHECK(transmitted && size == reply.size() && std::memcmp(response, reply.data(), size) == 0);
    CHECK(host_hid_usb.output_count == 1 && transport.connected());
    CHECK(host_ticks >= 50 && host_ticks < 200);

    // With no worker admission, expiry cancels the queued SET_REPORT; it must
    // never be submitted later when USB task execution resumes.
    connect(transport);
    size = sizeof(response); transmitted = true;
    CHECK(transport.exchange(command.data(), command.size(), response, size, 100,
        {terminated, nullptr}, transmitted) == ESP_ERR_TIMEOUT);
    CHECK(!transmitted && size == 0 && host_hid_usb.output_count == 0);
    CHECK(state.tx_phase.load() == canceled && transport.connected());
    service_output();
    CHECK(state.tx_phase.load() == idle && host_hid_usb.output_count == 0);

    // A command sent but unanswered consumes one deadline. The transport does
    // not submit it again, including when the next worker step runs.
    connect(transport);
    host_on_delay = [&] {
        service_output();
        if (host_hid_usb.pending_control != nullptr) complete_output();
    };
    size = sizeof(response); transmitted = false;
    CHECK(transport.exchange(command.data(), command.size(), response, size, 100,
        {terminated, nullptr}, transmitted) == ESP_ERR_TIMEOUT);
    CHECK(transmitted && size == 0 && host_hid_usb.output_count == 1 && host_ticks == 100);
    service_output();
    CHECK(host_hid_usb.output_count == 1 && transport.connected());

    // An admitted control transfer may still be writing after the caller's
    // deadline. Its DMA buffer and ownership remain until the completion; an
    // uncertain settings command is never retried by recovery.
    connect(transport);
    const Bytes setting{'P', 'O', 'P', '0', '1', 0xd2, 0x69, '\r'};
    host_on_delay = [&] { service_output(); };
    size = sizeof(response); transmitted = false;
    CHECK(transport.exchange(setting.data(), setting.size(), response, size, 100,
        {terminated, nullptr}, transmitted) == ESP_ERR_TIMEOUT);
    CHECK(transmitted && size == 0 && !transport.connected());
    CHECK(host_hid_usb.output_count == 1 && state.control_inflight);
    const auto *retained_buffer = state.control->data_buffer;
    close_when_safe();
    CHECK(state.device != nullptr && state.control->data_buffer == retained_buffer);
    CHECK(std::memcmp(retained_buffer + USB_SETUP_PACKET_SIZE, setting.data(), setting.size()) == 0);
    service_output();
    CHECK(host_hid_usb.output_count == 1);
    complete_output();
    close_when_safe();
    CHECK(!state.control_inflight && state.device == nullptr);
    CHECK(host_hid_usb.output_count == 1);

    // RX failure during an admitted setting must not reuse its control buffer
    // for recovery, or replay the setter once the endpoint has recovered.
    connect(transport);
    const auto setting_session = transport.session_id();
    bool failed_input = false;
    host_on_delay = [&] {
        service_output();
        if (host_hid_usb.pending_control != nullptr && !failed_input) {
            failed_input = true;
            const Bytes retained(state.control->data_buffer,
                state.control->data_buffer + USB_SETUP_PACKET_SIZE + report_size);
            complete_input({}, USB_TRANSFER_STATUS_ERROR);
            host_ticks += 250;
            service_input_recovery();
            CHECK(state.control_inflight && host_hid_usb.clear_count == 0);
            CHECK(std::memcmp(state.control->data_buffer, retained.data(), retained.size()) == 0);
            complete_output();
            service_input_recovery();
        }
    };
    size = sizeof(response); transmitted = false;
    CHECK(transport.exchange(setting.data(), setting.size(), response, size, 500,
        {terminated, nullptr}, transmitted) == ESP_ERR_INVALID_RESPONSE);
    CHECK(transmitted && failed_input && size == 0 && host_hid_usb.output_count == 1);
    CHECK(transport.connected() && transport.session_id() == setting_session);
    CHECK(host_hid_usb.clear_count == 1);
    service_output(); service_input_recovery();
    CHECK(host_hid_usb.output_count == 1);

    // A queued setting is blocked while RX recovery is pending. Its deadline
    // can expire without transmission; recovery must not later admit it.
    connect(transport);
    complete_input({}, USB_TRANSFER_STATUS_ERROR);
    host_on_delay = [&] { service_input_recovery(); service_output(); };
    size = sizeof(response); transmitted = true;
    CHECK(transport.exchange(setting.data(), setting.size(), response, size, 100,
        {terminated, nullptr}, transmitted) == ESP_ERR_TIMEOUT);
    CHECK(!transmitted && host_hid_usb.output_count == 0);
    host_ticks += 150; service_input_recovery(); service_output();
    CHECK(!state.input_recovery_pending && host_hid_usb.output_count == 0);

    // Binding admission to the detected USB session rejects a stale setting
    // before any bytes leave the transport.
    connect(transport);
    size = sizeof(response); transmitted = true;
    CHECK(transport.exchange(setting.data(), setting.size(), response, size, 100,
        {terminated, nullptr, transport.session_id() + 1}, transmitted) == ESP_ERR_INVALID_STATE);
    CHECK(!transmitted && size == 0 && host_hid_usb.output_count == 0);
}

void complete_recovery(int status = USB_TRANSFER_STATUS_COMPLETED,
    int actual_size = USB_SETUP_PACKET_SIZE)
{
    auto *transfer = host_hid_usb.pending_control;
    CHECK(transfer != nullptr && state.control_mode == ControlMode::input_recovery);
    host_hid_usb.pending_control = nullptr;
    transfer->status = status;
    transfer->actual_num_bytes = actual_size;
    if (status == USB_TRANSFER_STATUS_COMPLETED && actual_size == USB_SETUP_PACKET_SIZE)
        host_hid_usb.device_data_pid = 0; // CLEAR_FEATURE resets the device to DATA0.
    transfer->callback(transfer);
}

void input_recovery_cases(UsbHid &transport)
{
    for (const auto status : {USB_TRANSFER_STATUS_ERROR, USB_TRANSFER_STATUS_TIMED_OUT,
        USB_TRANSFER_STATUS_OVERFLOW, USB_TRANSFER_STATUS_CANCELED}) {
        connect(transport);
        host_hid_usb.host_data_pid = 1;
        host_hid_usb.device_data_pid = 1;
        const auto session = transport.session_id();
        std::uint32_t snapshot = 0;
        CHECK(state.receive_pipe.prepare(snapshot));
        service_input();
        CHECK(state.input_inflight && host_hid_usb.input_submit_count == 1);
        complete_input({'(', 'P'});
        complete_input({}, status);
        CHECK(!state.receive_pipe.valid(snapshot));
        CHECK(transport.connected() && transport.session_id() == session);
        CHECK(state.input_recovery_pending && !state.quarantine.load());
        std::uint8_t response[32]{};
        std::size_t size = 0;
        CHECK(detail::receive(state.receive_pipe, snapshot, response, sizeof(response), size,
            detail::Deadline(100), {terminated, nullptr}, [&] { return transport.connected(); }) == ESP_ERR_INVALID_RESPONSE);
        CHECK(size == 0); // A valid-looking prefix cannot survive the error.
        service_input_recovery(); service_input();
        CHECK(host_hid_usb.halt_count == 0 && host_hid_usb.input_submit_count == 1);
        host_ticks += 249;
        service_input_recovery(); service_input();
        CHECK(host_hid_usb.halt_count == 0 && host_hid_usb.input_submit_count == 1);
        ++host_ticks;
        service_input_recovery();
        CHECK(host_hid_usb.recovery_calls == std::vector<char>({'h', 'f', 'c'}));
        CHECK(host_hid_usb.host_data_pid == 1 && host_hid_usb.device_data_pid == 1);
        CHECK(!state.input_recovery_pending && transport.session_id() == session);
        CHECK(state.receive_pipe.prepare(snapshot));
        service_input();
        CHECK(state.input_inflight && host_hid_usb.input_submit_count == 2);
        complete_input({'(', 'P', 'I', '3', '0', 0x9a, 0x0b, '\r'});
        size = 0;
        CHECK(detail::receive(state.receive_pipe, snapshot, response, sizeof(response), size,
            detail::Deadline(100), {terminated, nullptr}, [&] { return transport.connected(); }) == ESP_OK);
        CHECK(size == 8 && state.input_failures == 0 && host_hid_usb.output_count == 0);
        CHECK(host_hid_usb.pid_mismatch_count == 0);
    }

    // Invalid reported sizes are rejected before copying any DMA bytes, and
    // NO_DEVICE never requests host-pipe recovery against a removed device.
    for (const auto length : {-1, 9}) {
        connect(transport);
        std::uint32_t snapshot = 0;
        CHECK(state.receive_pipe.prepare(snapshot));
        complete_input({}, USB_TRANSFER_STATUS_COMPLETED, length);
        CHECK(!transport.connected() && state.quarantine.load());
        CHECK(!state.receive_pipe.valid(snapshot));
        CHECK(xStreamBufferBytesAvailable(state.receive_pipe.stream) == 0);
        host_ticks += 300;
        service_input_recovery(); service_input();
        CHECK(host_hid_usb.clear_count == 0 && host_hid_usb.input_submit_count == 0);
    }
    connect(transport);
    complete_input({}, USB_TRANSFER_STATUS_NO_DEVICE);
    CHECK(!transport.connected() && !state.input_recovery_pending);

    // Three consecutive failures bound recovery. A successful empty idle
    // report, as well as a nonempty report, resets that failure budget.
    connect(transport);
    for (unsigned failure = 1; failure <= 3; ++failure) {
        complete_input({}, USB_TRANSFER_STATUS_ERROR);
        CHECK(state.input_failures == failure);
        if (failure < 3) {
            CHECK(transport.connected());
            host_ticks += 250; service_input_recovery();
        } else CHECK(!transport.connected() && state.quarantine.load());
    }
    CHECK(host_hid_usb.clear_count == 2 && host_hid_usb.output_count == 0);
    connect(transport);
    for (unsigned failure = 0; failure < 4; ++failure) {
        complete_input({}, USB_TRANSFER_STATUS_ERROR);
        CHECK(state.input_failures == 1 && transport.connected());
        host_ticks += 250; service_input_recovery();
        complete_input({});
        CHECK(state.input_failures == 0);
    }
    CHECK(host_hid_usb.clear_count == 4 && host_hid_usb.output_count == 0);

    // Submit failures use the same delayed, bounded host-pipe recovery.
    connect(transport);
    host_hid_usb.input_submit_result = ESP_ERR_INVALID_STATE;
    service_input();
    CHECK(!state.input_inflight && state.input_recovery_pending && state.input_failures == 1);
    host_ticks += 250; service_input_recovery();
    host_hid_usb.input_submit_result = ESP_OK;
    service_input();
    CHECK(state.input_inflight && host_hid_usb.input_submit_count == 2);
    complete_input({});
    CHECK(transport.connected() && state.input_failures == 0);

    // Failures in halt, flush or host clear stop admission. Recovery is not an
    // unchecked retry loop, and never sends an inverter SET_REPORT.
    for (unsigned failure = 0; failure < 3; ++failure) {
        connect(transport);
        complete_input({}, USB_TRANSFER_STATUS_ERROR);
        if (failure == 0) host_hid_usb.halt_result = ESP_ERR_INVALID_STATE;
        if (failure == 1) host_hid_usb.flush_result = ESP_ERR_INVALID_STATE;
        if (failure == 2) host_hid_usb.clear_result = ESP_ERR_INVALID_STATE;
        host_ticks += 250; service_input_recovery();
        CHECK(!transport.connected() && host_hid_usb.output_count == 0);
    }

    // A stalled endpoint needs device CLEAR_FEATURE and a fresh host pipe with
    // its DATA0 PID. This control request never replays an inverter setting.
    connect(transport);
    const auto stalled_session = transport.session_id();
    host_hid_usb.host_data_pid = 1;
    host_hid_usb.device_data_pid = 1;
    complete_input({}, USB_TRANSFER_STATUS_STALL);
    host_ticks += 250; service_input_recovery();
    CHECK(state.control_inflight && host_hid_usb.clear_feature_count == 1);
    CHECK(host_hid_usb.clear_count == 0 && host_hid_usb.output_count == 0);
    const auto setup = *reinterpret_cast<const usb_setup_packet_t *>(state.control->data_buffer);
    CHECK(setup.bmRequestType == 0x02 && setup.bRequest == USB_B_REQUEST_CLEAR_FEATURE);
    CHECK(setup.wValue == 0 && setup.wIndex == state.endpoint && setup.wLength == 0);
    CHECK(state.control->num_bytes == USB_SETUP_PACKET_SIZE);
    service_input_recovery(); service_input();
    CHECK(host_hid_usb.clear_feature_count == 1 && host_hid_usb.input_submit_count == 0);
    complete_recovery();
    CHECK(host_hid_usb.host_data_pid == 1 && host_hid_usb.device_data_pid == 0);
    CHECK(host_hid_usb.release_count == 0); // No handle release inside callback.
    service_input_recovery();
    CHECK(host_hid_usb.recovery_calls == std::vector<char>({'s', 'r', 'a'}));
    CHECK(host_hid_usb.host_data_pid == 0 && host_hid_usb.device_data_pid == 0);
    CHECK(host_hid_usb.release_count == 1 && host_hid_usb.claim_count == 2);
    CHECK(host_hid_usb.release_while_inflight_count == 0);
    CHECK(transport.connected() && transport.session_id() == stalled_session);
    service_input();
    CHECK(state.input_inflight && host_hid_usb.input_submit_count == 1);
    CHECK(host_hid_usb.pid_mismatch_count == 0);
    complete_input({});

    // Recreating the host endpoint can fail too; neither failure permits RX or
    // sends an inverter command against a partially reclaimed interface.
    for (unsigned failure = 0; failure < 2; ++failure) {
        connect(transport);
        complete_input({}, USB_TRANSFER_STATUS_STALL);
        host_ticks += 250; service_input_recovery();
        complete_recovery();
        if (failure == 0) host_hid_usb.release_result = ESP_ERR_INVALID_STATE;
        else host_hid_usb.claim_result = ESP_ERR_INVALID_STATE;
        service_input_recovery(); service_input();
        CHECK(!transport.connected() && host_hid_usb.input_submit_count == 0);
        CHECK(host_hid_usb.output_count == 0 && host_hid_usb.release_while_inflight_count == 0);
    }

    // Failed, truncated or expired CLEAR_FEATURE completions never admit RX.
    for (unsigned failure = 0; failure < 4; ++failure) {
        connect(transport);
        complete_input({}, USB_TRANSFER_STATUS_STALL);
        if (failure == 3) host_hid_usb.clear_feature_submit_result = ESP_ERR_INVALID_STATE;
        host_ticks += 250; service_input_recovery();
        if (failure != 3) {
            if (failure == 2) host_ticks += 2000;
            complete_recovery(failure == 0 ? USB_TRANSFER_STATUS_ERROR : USB_TRANSFER_STATUS_COMPLETED,
                failure == 1 ? USB_SETUP_PACKET_SIZE - 1 : USB_SETUP_PACKET_SIZE);
        }
        CHECK(!transport.connected() && host_hid_usb.clear_count == 0);
        CHECK(host_hid_usb.output_count == 0 && !state.control_inflight);
    }

    // Unplug during recovery retains the control DMA buffer until callback and
    // cannot let that late callback make a removed inverter ready again.
    connect(transport);
    complete_input({}, USB_TRANSFER_STATUS_STALL);
    host_ticks += 250; service_input_recovery();
    const auto *retained_buffer = state.control->data_buffer;
    usb_host_client_event_msg_t gone{};
    gone.event = USB_HOST_CLIENT_EVENT_DEV_GONE; gone.dev_gone.dev_hdl = state.device;
    device_event(&gone, nullptr); close_when_safe();
    CHECK(!transport.connected() && state.device != nullptr && state.control_inflight);
    CHECK(state.control->data_buffer == retained_buffer);
    complete_recovery(); close_when_safe();
    CHECK(!transport.connected() && state.device == nullptr && !state.control_inflight);
    CHECK(host_hid_usb.output_count == 0);
}

int main()
try {
    UsbHid transport;
    // Default discovery accepts compatible bridges independently of IDs.
    CHECK(UsbHidInverterConfig{}.vendor_id == 0);
    CHECK(UsbHidInverterConfig{}.product_id == 0);
    for (const auto &[vendor, product] : std::vector<std::pair<unsigned, unsigned>>{
        {0x0665, 0x5161}, {0x1234, 0xabcd}, {0x10c4, 0xea80}}) {
        reset(transport);
        probe(device(vendor, product));
        CHECK(!transport.connected()); // Descriptor admission is asynchronous.
        CHECK(host_hid_usb.get_report_count == 1 && host_hid_usb.output_count == 0);
        complete_descriptor();
        CHECK(transport.connected());
        CHECK(state.interface_number == 0 && state.endpoint == 0x81);
        CHECK(transport.session_id() != 0);
    }

    // Optional fields independently restrict discovery; zero is a wildcard.
    for (const auto filter : std::vector<UsbHidInverterConfig>{
        {0x1234, 0}, {0, 0xabcd}, {0x1234, 0xabcd}}) {
        reset(transport, filter);
        probe(device(0x1234, 0xabcd));
        complete_descriptor();
        CHECK(transport.connected());
    }
    for (const auto filter : std::vector<UsbHidInverterConfig>{
        {0x1235, 0}, {0, 0xabce}, {0x1234, 0xabce}, {0x1235, 0xabcd}}) {
        reset(transport, filter);
        probe(device(0x1234, 0xabcd));
        CHECK(!transport.connected());
        CHECK(host_hid_usb.claim_count == 0 && host_hid_usb.get_report_count == 0);
        CHECK(host_hid_usb.close_count == 1);
    }

    // USB interface identity and endpoint shape must match before claiming.
    std::vector<Interface> rejected_interfaces;
    auto iface = Interface{}; iface.cls = 2; rejected_interfaces.push_back(iface); // CDC.
    iface = {}; iface.subclass = 1; iface.protocol = 1; rejected_interfaces.push_back(iface); // Keyboard.
    iface = {}; iface.subclass = 1; iface.protocol = 2; rejected_interfaces.push_back(iface); // Mouse.
    iface = {}; iface.packet_size = 16; rejected_interfaces.push_back(iface);
    iface = {}; iface.attributes = 2; rejected_interfaces.push_back(iface); // Bulk endpoint.
    iface = {}; iface.endpoint = 1; rejected_interfaces.push_back(iface); // No interrupt IN.
    for (const auto &bad : rejected_interfaces) {
        reset(transport);
        probe(device(0x1234, 0xabcd, {bad}));
        CHECK(!transport.connected());
        CHECK(host_hid_usb.claim_count == 0 && host_hid_usb.get_report_count == 0);
    }

    // An HID interface alone is insufficient: the report must be a byte pipe.
    std::vector<Bytes> rejected_reports;
    auto report = byte_pipe; report.insert(report.begin() + 7, {0x85, 1}); rejected_reports.push_back(report);
    report = byte_pipe; report[10] = 9; rejected_reports.push_back(report); // Nine bytes.
    report = byte_pipe; report[8] = 7; rejected_reports.push_back(report); // Bit packed.
    report = byte_pipe; report[14] = 1; rejected_reports.push_back(report); // Constant input.
    report = byte_pipe; report[17] = 0x81; rejected_reports.push_back(report); // No output.
    report = byte_pipe; report.push_back(0x75); rejected_reports.push_back(report); // Truncated item.
    for (const auto &bad : rejected_reports) {
        reset(transport);
        iface = {}; iface.report = bad;
        probe(device(0x1234, 0xabcd, {iface}));
        complete_descriptor();
        CHECK(!transport.connected());
        CHECK(host_hid_usb.output_count == 0);
    }

    // A nonboot keyboard/gamepad can expose 8-byte IN/OUT reports. Automatic
    // discovery additionally requires a vendor-defined application collection.
    report = byte_pipe;
    report.erase(report.begin()); // Short Usage Page 0x01 instead of 0xff00.
    report[0] = 0x05; report[1] = 0x01;
    report[3] = 0x06; // Generic Desktop keyboard application, not vendor data.
    for (const auto filter : std::vector<UsbHidInverterConfig>{
        {0, 0}, {0x1234, 0}, {0, 0xabcd}}) {
        reset(transport, filter);
        iface = {}; iface.report = report;
        probe(device(0x1234, 0xabcd, {iface}));
        complete_descriptor();
        CHECK(!transport.connected());
    }
    reset(transport, {0x1234, 0xabcd});
    iface = {}; iface.report = report;
    probe(device(0x1234, 0xabcd, {iface}));
    complete_descriptor();
    CHECK(transport.connected()); // Explicitly selected nonvendor byte pipe.

    // Composite devices must skip keyboard interfaces and continue past an
    // incompatible nonboot report before choosing a compatible bridge pipe.
    reset(transport);
    auto keyboard = Interface{};
    keyboard.number = 0; keyboard.subclass = 1; keyboard.protocol = 1;
    const auto bridge = Interface{.number = 2};
    probe(device(0x1234, 0xabcd, {keyboard, bridge}));
    CHECK(state.interface_number == 2 && host_hid_usb.claim_count == 1);
    complete_descriptor();
    CHECK(transport.connected() && host_hid_usb.get_report_count == 1);
    reset(transport);
    auto unrelated = Interface{};
    unrelated.number = 0; unrelated.report = report;
    probe(device(0x1234, 0xabcd, {unrelated, bridge}));
    complete_descriptor();
    CHECK(!transport.connected() && state.interface_number == 2);
    CHECK(host_hid_usb.get_report_count == 2 && host_hid_usb.release_count == 1);
    complete_descriptor();
    CHECK(transport.connected() && host_hid_usb.output_count == 0);

    // Vendor usage does not excuse an unbalanced application collection.
    reset(transport);
    iface = {}; iface.report = byte_pipe; iface.report.pop_back();
    probe(device(0x1234, 0xabcd, {iface})); complete_descriptor();
    CHECK(!transport.connected());

    // A malformed trailing descriptor cannot be hidden behind a good interface.
    reset(transport);
    auto malformed = device(0x1234, 0xabcd);
    malformed.configuration.insert(malformed.configuration.end(), {0, 4});
    malformed.configuration[2] = static_cast<std::uint8_t>(malformed.configuration.size());
    probe(std::move(malformed));
    CHECK(!transport.connected() && host_hid_usb.claim_count == 0);
    reset(transport);
    malformed = device(0x1234, 0xabcd);
    malformed.configuration.insert(malformed.configuration.end(), {7, 5, 0x82});
    malformed.configuration[2] = static_cast<std::uint8_t>(malformed.configuration.size());
    probe(std::move(malformed));
    CHECK(!transport.connected() && host_hid_usb.claim_count == 0);

    // USB errors, truncated descriptors, and expired probes never mark ready.
    for (unsigned failure = 0; failure < 3; ++failure) {
        reset(transport);
        probe(device(0x1234, 0xabcd));
        if (failure == 2) host_ticks = 2000;
        complete_descriptor(failure == 0, failure == 1);
        CHECK(!transport.connected());
    }

    // Disconnect during descriptor DMA completion cannot admit a gone device.
    reset(transport);
    probe(device(0x1234, 0xabcd));
    usb_host_client_event_msg_t disconnect_during_probe{};
    disconnect_during_probe.event = USB_HOST_CLIENT_EVENT_DEV_GONE;
    disconnect_during_probe.dev_gone.dev_hdl = state.device;
    device_event(&disconnect_during_probe, nullptr); close_when_safe();
    CHECK(state.device != nullptr); // Buffer is retained until its callback.
    complete_descriptor();
    CHECK(!transport.connected() && state.device == nullptr);

    // Discovery sends no inverter commands, and a removed device loses readiness.
    reset(transport);
    probe(device(0x1234, 0xabcd)); complete_descriptor();
    const auto previous_session = transport.session_id();
    usb_host_client_event_msg_t gone{};
    gone.event = USB_HOST_CLIENT_EVENT_DEV_GONE; gone.dev_gone.dev_hdl = state.device;
    device_event(&gone, nullptr); close_when_safe();
    CHECK(!transport.connected() && transport.session_id() != previous_session);
    CHECK(host_hid_usb.output_count == 0);
    host_hid_usb.devices[2] = device(0xbeef, 0xcafe, {Interface{.number = 3}});
    open_candidate(2); complete_descriptor();
    CHECK(transport.connected() && state.interface_number == 3);
    CHECK(host_hid_usb.last_claimed_interface == 3 && host_hid_usb.output_count == 0);
    input_report_cases(transport);
    exchange_cases(transport);
    input_recovery_cases(transport);
    startup_cleanup(); state.installed.store(false);
    std::printf("Actual USB HID discovery, short/empty reports, recovery/PID, DMA ownership and no-write-replay checks passed (%u checks).\n", checks);
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "USB HID discovery test failed: %s\n", error.what());
    return 1;
}
