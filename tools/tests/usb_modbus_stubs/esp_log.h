#pragma once
template <class... Args> inline void host_log(const char *, const char *, Args...) {}
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGD(...) host_log(__VA_ARGS__)
