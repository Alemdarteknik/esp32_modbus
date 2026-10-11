#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
enum { CDC_ACM_HOST_DEVICE_DISCONNECTED, CDC_ACM_HOST_ERROR, CDC_ACM_HOST_SERIAL_STATE };
struct cdc_acm_host_dev_event_data_t { int type; struct { int error; struct { unsigned val; } serial_state; } data{}; };
struct cdc_acm_host_device_config_t {
    std::uint32_t connection_timeout_ms;
    std::size_t out_buffer_size, in_buffer_size;
    void (*event_cb)(const cdc_acm_host_dev_event_data_t *, void *);
    bool (*data_cb)(const std::uint8_t *, std::size_t, void *);
    void *user_arg;
};
struct cdc_acm_line_coding_t { std::uint32_t dwDTERate; unsigned bCharFormat, bParityType, bDataBits; };
inline int cdc_acm_host_install(void *) { return ESP_OK; }
