#pragma once
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
struct usb_host_config_t { bool skip_phy_setup; int intr_flags; };
constexpr unsigned USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS = 1;
inline int usb_host_install(const usb_host_config_t *) { return ESP_OK; }
inline int usb_host_device_free_all() { return ESP_OK; }
inline int usb_host_lib_handle_events(TickType_t, std::uint32_t *) { return ESP_OK; }
