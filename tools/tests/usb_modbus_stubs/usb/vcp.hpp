#pragma once
#include <functional>
#include "usb/cdc_acm_host.h"
extern cdc_acm_host_device_config_t host_usb_config;
extern std::function<esp_err_t(const std::uint8_t *, std::size_t)> host_tx;
extern unsigned host_tx_count;
class CdcAcmDevice {
public:
    esp_err_t tx_blocking(const std::uint8_t *bytes, std::size_t count, unsigned) {
        ++host_tx_count; return host_tx ? host_tx(bytes, count) : ESP_OK;
    }
    esp_err_t line_coding_set(const cdc_acm_line_coding_t *) { return ESP_OK; }
    esp_err_t set_control_line_state(bool, bool) { return ESP_OK; }
};
namespace esp_usb {
class VCP {
public:
    template <class T> static void register_driver() {}
    static CdcAcmDevice *open(const cdc_acm_host_device_config_t *config) { host_usb_config = *config; return new CdcAcmDevice; }
};
}
