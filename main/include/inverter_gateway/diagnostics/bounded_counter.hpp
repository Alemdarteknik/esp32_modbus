#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace inverter_gateway::diagnostics {
// Cumulative per-boot diagnostics saturate instead of wrapping back to zero.
inline void add_saturating(std::atomic<std::uint32_t> &counter,
                           std::uint32_t amount = 1)
{
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    auto current = counter.load(std::memory_order_relaxed);
    while (current != maximum &&
           !counter.compare_exchange_weak(current,
               amount > maximum - current ? maximum : current + amount,
               std::memory_order_relaxed)) {}
}
// ESP-IDF's PSRAM workaround may dispatch these operations through SDK atomic
// helpers. These task-context diagnostics require atomicity, not lock freedom.
} // namespace inverter_gateway::diagnostics
