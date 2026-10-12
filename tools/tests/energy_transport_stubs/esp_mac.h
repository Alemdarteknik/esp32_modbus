#pragma once
#include <cstdint>
#include <cstring>
#include "esp_err.h"
constexpr int ESP_MAC_WIFI_STA = 0;
inline esp_err_t esp_read_mac(std::uint8_t *mac, int)
{
    const std::uint8_t value[6]{0x12,0x34,0x56,0x78,0x9a,0xbc};
    std::memcpy(mac,value,sizeof(value)); return ESP_OK;
}
