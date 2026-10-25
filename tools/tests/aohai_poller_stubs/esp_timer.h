#pragma once
#include <cstdint>
extern std::int64_t aohai_poll_host_time_ms;
inline std::int64_t esp_timer_get_time() { return aohai_poll_host_time_ms * 1000; }
