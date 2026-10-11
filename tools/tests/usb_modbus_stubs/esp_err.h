#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_NO_MEM = 0x101;
constexpr int ESP_ERR_NOT_FINISHED = 0x10c;
inline const char *esp_err_to_name(int) { return "stub"; }
