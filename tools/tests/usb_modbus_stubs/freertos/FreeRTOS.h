#pragma once
#include <cstdint>
using TickType_t = std::uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int pdPASS = 1;
constexpr int BIT0 = 1;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
