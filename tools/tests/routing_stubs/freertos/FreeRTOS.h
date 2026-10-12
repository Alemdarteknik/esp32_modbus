#pragma once
#include <cstdint>
using TickType_t=std::uint32_t;
using BaseType_t=int;
inline constexpr int pdTRUE=1,pdPASS=1;
inline constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
