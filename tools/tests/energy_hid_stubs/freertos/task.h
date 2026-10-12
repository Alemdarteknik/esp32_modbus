#pragma once
#include <functional>
#include "freertos/FreeRTOS.h"
extern TickType_t host_ticks;
extern std::function<void()> host_on_delay;
using TaskHandle_t = void *;
inline TickType_t xTaskGetTickCount() { return host_ticks; }
inline void vTaskDelay(TickType_t ticks) {
    host_ticks += ticks;
    if (host_on_delay) host_on_delay();
}
inline int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned,
    TaskHandle_t *handle)
{
    *handle = reinterpret_cast<void *>(1);
    return pdPASS;
}
inline void vTaskDelete(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(int, TickType_t) { return 0; }
inline void xTaskNotifyGive(TaskHandle_t) {}
