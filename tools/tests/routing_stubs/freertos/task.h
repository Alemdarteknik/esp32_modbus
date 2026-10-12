#pragma once
#include "FreeRTOS.h"
using TaskHandle_t=void *;
BaseType_t xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t);
