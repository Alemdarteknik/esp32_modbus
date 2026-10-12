#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_NO_MEM = 0x101;
constexpr int ESP_ERR_INVALID_ARG = 0x102;
constexpr int ESP_ERR_INVALID_STATE = 0x103;
constexpr int ESP_ERR_INVALID_SIZE = 0x104;
constexpr int ESP_ERR_NOT_FOUND = 0x105;
constexpr int ESP_ERR_NOT_SUPPORTED = 0x106;
constexpr int ESP_ERR_TIMEOUT = 0x107;
constexpr int ESP_ERR_INVALID_RESPONSE = 0x108;
constexpr int ESP_ERR_INVALID_VERSION = 0x10a;
constexpr int ESP_ERR_NOT_FINISHED = 0x10c;
inline const char *esp_err_to_name(int) { return "host stub"; }
