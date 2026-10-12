#pragma once
#include "freertos/task.h"
struct HostSemaphore { bool available = true; };
using SemaphoreHandle_t = HostSemaphore *;
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks)
{
    if (mutex->available) { mutex->available = false; return pdTRUE; }
    host_ticks += ticks;
    return pdFALSE;
}
inline int xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->available = true; return pdTRUE; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new HostSemaphore; }
inline void vSemaphoreDelete(SemaphoreHandle_t mutex) { delete mutex; }
