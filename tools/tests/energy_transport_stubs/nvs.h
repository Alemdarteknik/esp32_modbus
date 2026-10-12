#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "esp_err.h"
using nvs_handle_t = unsigned;
constexpr int NVS_READONLY=0, NVS_READWRITE=1;
constexpr int ESP_ERR_NVS_NOT_FOUND=0x1102;
constexpr int ESP_ERR_NVS_INVALID_LENGTH=0x110c;
inline std::vector<std::uint8_t> host_nvs_blob;
inline std::uint8_t host_verification_pending=0;
inline esp_err_t nvs_open(const char *, int, nvs_handle_t *handle) { *handle=1; return ESP_OK; }
inline void nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *data, std::size_t *size)
{
    if (host_nvs_blob.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < host_nvs_blob.size()) { *size=host_nvs_blob.size(); return ESP_ERR_NVS_INVALID_LENGTH; }
    *size=host_nvs_blob.size(); std::memcpy(data,host_nvs_blob.data(),*size); return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *data, std::size_t size)
{
    const auto *bytes=static_cast<const std::uint8_t *>(data);
    host_nvs_blob.assign(bytes,bytes+size); return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
inline esp_err_t nvs_erase_all(nvs_handle_t) { host_nvs_blob.clear(); host_verification_pending=0; return ESP_OK; }
inline esp_err_t nvs_get_u8(nvs_handle_t, const char *, std::uint8_t *value) { *value=host_verification_pending; return ESP_OK; }
inline esp_err_t nvs_set_u8(nvs_handle_t, const char *, std::uint8_t value) { host_verification_pending=value; return ESP_OK; }
