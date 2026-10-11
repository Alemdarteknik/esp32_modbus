#include "inverter_gateway/app/memory_budget.hpp"

#include "freertos/semphr.h"

namespace inverter_gateway::app {
namespace {
StaticSemaphore_t memory_work_storage{};
SemaphoreHandle_t const memory_work_lock =
    xSemaphoreCreateMutexStatic(&memory_work_storage);
}

bool try_begin_memory_work()
{
    return xSemaphoreTake(memory_work_lock, 0) == pdTRUE;
}

void end_memory_work()
{
    xSemaphoreGive(memory_work_lock);
}

MemoryWorkGuard::MemoryWorkGuard(TickType_t wait)
    : acquired_(xSemaphoreTake(memory_work_lock, wait) == pdTRUE)
{
}

MemoryWorkGuard::~MemoryWorkGuard()
{
    release();
}

void MemoryWorkGuard::release()
{
    if (acquired_) {
        acquired_ = false;
        end_memory_work();
    }
}

} // namespace inverter_gateway::app
