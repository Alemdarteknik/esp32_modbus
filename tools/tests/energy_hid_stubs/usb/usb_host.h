#pragma once
#include <cstdlib>
#include <map>
#include <vector>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#pragma pack(push, 1)
struct usb_device_desc_t {
    std::uint8_t bLength = 18, bDescriptorType = 1;
    std::uint16_t bcdUSB = 0x0200;
    std::uint8_t bDeviceClass = 0, bDeviceSubClass = 0, bDeviceProtocol = 0,
        bMaxPacketSize0 = 8;
    std::uint16_t idVendor = 0, idProduct = 0, bcdDevice = 0;
    std::uint8_t iManufacturer = 0, iProduct = 0, iSerialNumber = 0,
        bNumConfigurations = 1;
};
struct usb_config_desc_t {
    std::uint8_t bLength, bDescriptorType;
    std::uint16_t wTotalLength;
    std::uint8_t bNumInterfaces, bConfigurationValue, iConfiguration,
        bmAttributes, bMaxPower;
};
struct usb_setup_packet_t {
    std::uint8_t bmRequestType, bRequest;
    std::uint16_t wValue, wIndex, wLength;
};
#pragma pack(pop)
using usb_host_client_handle_t = void *;
using usb_device_handle_t = void *;
struct usb_transfer_t {
    std::uint8_t *data_buffer = nullptr;
    int num_bytes = 0, actual_num_bytes = 0;
    int status = 0;
    usb_device_handle_t device_handle = nullptr;
    std::uint8_t bEndpointAddress = 0;
    void (*callback)(usb_transfer_t *) = nullptr;
    void *context = nullptr;
};
struct usb_host_client_event_msg_t {
    int event = 0;
    struct { std::uint8_t address = 0; } new_dev;
    struct { usb_device_handle_t dev_hdl = nullptr; } dev_gone;
};
struct usb_host_client_config_t {
    int max_num_event_msg = 0;
    struct {
        void (*client_event_callback)(const usb_host_client_event_msg_t *, void *) = nullptr;
        void *callback_arg = nullptr;
    } async;
};
struct usb_host_config_t { bool skip_phy_setup = false; int intr_flags = 0; };
constexpr unsigned USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS = 1;
constexpr int USB_HOST_CLIENT_EVENT_NEW_DEV = 1, USB_HOST_CLIENT_EVENT_DEV_GONE = 2;
constexpr int USB_TRANSFER_STATUS_COMPLETED = 0, USB_TRANSFER_STATUS_ERROR = 1,
    USB_TRANSFER_STATUS_TIMED_OUT = 2, USB_TRANSFER_STATUS_OVERFLOW = 3,
    USB_TRANSFER_STATUS_CANCELED = 4, USB_TRANSFER_STATUS_NO_DEVICE = 5,
    USB_TRANSFER_STATUS_STALL = 6;
constexpr int USB_SETUP_PACKET_SIZE = 8;
constexpr int USB_B_DESCRIPTOR_TYPE_INTERFACE = 4, USB_B_DESCRIPTOR_TYPE_ENDPOINT = 5;
constexpr int USB_B_REQUEST_GET_DESCRIPTOR = 6;
constexpr int USB_B_REQUEST_CLEAR_FEATURE = 1;
constexpr int USB_B_FEATURE_ENDPOINT_HALT = 0;

struct HostHidDevice {
    usb_device_desc_t descriptor;
    std::vector<std::uint8_t> configuration;
    std::map<unsigned, std::vector<std::uint8_t>> reports;
};
struct HostHidUsb {
    std::map<unsigned, HostHidDevice> devices;
    HostHidDevice *opened = nullptr;
    usb_transfer_t *pending_control = nullptr;
    usb_transfer_t *pending_input = nullptr;
    unsigned open_count = 0, close_count = 0, claim_count = 0,
        release_count = 0, get_report_count = 0, output_count = 0,
        input_submit_count = 0, halt_count = 0, flush_count = 0,
        clear_count = 0, clear_feature_count = 0, pid_mismatch_count = 0,
        release_while_inflight_count = 0;
    unsigned last_claimed_interface = 0;
    int halt_result = ESP_OK, flush_result = ESP_OK, clear_result = ESP_OK,
        input_submit_result = ESP_OK, clear_feature_submit_result = ESP_OK,
        release_result = ESP_OK, claim_result = ESP_OK;
    unsigned host_data_pid = 0, device_data_pid = 0;
    std::vector<char> recovery_calls;
};
inline HostHidUsb host_hid_usb;
inline int usb_host_install(const usb_host_config_t *) { return ESP_OK; }
inline int usb_host_uninstall() { return ESP_OK; }
inline int usb_host_device_free_all() { return ESP_OK; }
inline int usb_host_lib_handle_events(TickType_t, std::uint32_t *) { return ESP_OK; }
inline int usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *handle)
{ *handle = reinterpret_cast<void *>(1); return ESP_OK; }
inline int usb_host_client_deregister(usb_host_client_handle_t) { return ESP_OK; }
inline int usb_host_client_handle_events(usb_host_client_handle_t, TickType_t) { return ESP_OK; }
inline int usb_host_client_unblock(usb_host_client_handle_t) { return ESP_OK; }
inline int usb_host_device_open(usb_host_client_handle_t, std::uint8_t address,
    usb_device_handle_t *handle)
{
    ++host_hid_usb.open_count;
    const auto found = host_hid_usb.devices.find(address);
    if (found == host_hid_usb.devices.end()) return ESP_ERR_NOT_FOUND;
    host_hid_usb.opened = &found->second;
    *handle = host_hid_usb.opened;
    return ESP_OK;
}
inline int usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t)
{ ++host_hid_usb.close_count; host_hid_usb.opened = nullptr; return ESP_OK; }
inline int usb_host_get_device_descriptor(usb_device_handle_t handle,
    const usb_device_desc_t **descriptor)
{ *descriptor = &static_cast<HostHidDevice *>(handle)->descriptor; return ESP_OK; }
inline int usb_host_get_active_config_descriptor(usb_device_handle_t handle,
    const usb_config_desc_t **descriptor)
{
    *descriptor = reinterpret_cast<const usb_config_desc_t *>(
        static_cast<HostHidDevice *>(handle)->configuration.data());
    return ESP_OK;
}
inline int usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t,
    unsigned interface_number, unsigned)
{
    ++host_hid_usb.claim_count;
    host_hid_usb.last_claimed_interface = interface_number;
    if (host_hid_usb.get_report_count != 0) host_hid_usb.recovery_calls.push_back('a');
    if (host_hid_usb.claim_result == ESP_OK) host_hid_usb.host_data_pid = 0;
    return host_hid_usb.claim_result;
}
inline int usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, unsigned)
{
    ++host_hid_usb.release_count; host_hid_usb.recovery_calls.push_back('r');
    if (host_hid_usb.pending_input != nullptr || host_hid_usb.pending_control != nullptr)
        ++host_hid_usb.release_while_inflight_count;
    return host_hid_usb.release_result;
}
inline int usb_host_endpoint_halt(usb_device_handle_t, unsigned) {
    ++host_hid_usb.halt_count; host_hid_usb.recovery_calls.push_back('h');
    return host_hid_usb.halt_result;
}
inline int usb_host_endpoint_flush(usb_device_handle_t, unsigned) {
    ++host_hid_usb.flush_count; host_hid_usb.recovery_calls.push_back('f');
    return host_hid_usb.flush_result;
}
inline int usb_host_endpoint_clear(usb_device_handle_t, unsigned) {
    ++host_hid_usb.clear_count; host_hid_usb.recovery_calls.push_back('c');
    return host_hid_usb.clear_result;
}
inline int usb_host_transfer_alloc(std::size_t size, unsigned, usb_transfer_t **transfer)
{
    *transfer = new usb_transfer_t;
    (*transfer)->data_buffer = static_cast<std::uint8_t *>(std::calloc(size, 1));
    return ESP_OK;
}
inline int usb_host_transfer_free(usb_transfer_t *transfer)
{
    if (transfer != nullptr) { std::free(transfer->data_buffer); delete transfer; }
    return ESP_OK;
}
inline int usb_host_transfer_submit(usb_transfer_t *transfer) {
    ++host_hid_usb.input_submit_count;
    if (host_hid_usb.host_data_pid != host_hid_usb.device_data_pid) {
        ++host_hid_usb.pid_mismatch_count;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (host_hid_usb.input_submit_result == ESP_OK) host_hid_usb.pending_input = transfer;
    return host_hid_usb.input_submit_result;
}
inline int usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *transfer)
{
    host_hid_usb.pending_control = transfer;
    const auto *setup = reinterpret_cast<const usb_setup_packet_t *>(transfer->data_buffer);
    if (setup->bRequest == USB_B_REQUEST_GET_DESCRIPTOR) ++host_hid_usb.get_report_count;
    else if (setup->bRequest == USB_B_REQUEST_CLEAR_FEATURE) {
        ++host_hid_usb.clear_feature_count;
        host_hid_usb.recovery_calls.push_back('s');
        if (host_hid_usb.clear_feature_submit_result != ESP_OK) {
            host_hid_usb.pending_control = nullptr;
            return host_hid_usb.clear_feature_submit_result;
        }
    } else ++host_hid_usb.output_count;
    return ESP_OK;
}
