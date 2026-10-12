#pragma once
#include <cstddef>
#include "freertos/FreeRTOS.h"
using QueueHandle_t = void *;
QueueHandle_t xQueueCreate(std::size_t, std::size_t);
void vQueueDelete(QueueHandle_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
