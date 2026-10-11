#pragma once
#include "freertos/FreeRTOS.h"
using EventBits_t = std::uint32_t;
using EventGroupHandle_t = EventBits_t *;
inline EventGroupHandle_t xEventGroupCreate() { static EventBits_t bits = 0; return &bits; }
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { return *group |= bits; }
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { return *group &= ~bits; }
inline EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { return *group; }
