#pragma once

#include "freertos/FreeRTOS.h"

namespace inverter_gateway::app {

// Serialize large, transient allocations (live JSON and a second TLS handshake).
// This is admission control, not a heap reservation or a Modbus lock.
bool try_begin_memory_work();
void end_memory_work();

class MemoryWorkGuard {
public:
    explicit MemoryWorkGuard(TickType_t wait = 0);
    ~MemoryWorkGuard();
    MemoryWorkGuard(const MemoryWorkGuard &) = delete;
    MemoryWorkGuard &operator=(const MemoryWorkGuard &) = delete;

    explicit operator bool() const { return acquired_; }
    void release();

private:
    bool acquired_ = false;
};

} // namespace inverter_gateway::app
