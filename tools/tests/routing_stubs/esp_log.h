#pragma once
template<class... Args> inline void routing_host_log(const char *,const char *,Args...) {}
#define ESP_LOGI(...) routing_host_log(__VA_ARGS__)
#define ESP_LOGW(...) routing_host_log(__VA_ARGS__)
#define ESP_LOGE(...) routing_host_log(__VA_ARGS__)
