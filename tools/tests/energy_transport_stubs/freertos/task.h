#pragma once
#include "freertos/FreeRTOS.h"
extern TickType_t host_ticks;
inline TickType_t xTaskGetTickCount() { return host_ticks; }
inline void vTaskDelay(TickType_t ticks) { host_ticks += ticks; }
inline int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *) { return pdPASS; }
