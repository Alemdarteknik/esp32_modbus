#pragma once
#include <cstdint>

namespace inverter_gateway::app {
// PROPOSAL ONLY: tested offline, NOT included by or linked into firmware.
// Pure scheduling policy; no I/O, flash operations or implicit retries of writes.
enum class OtaRetryKind { normal_check, local_deferred, transient_failure };
constexpr std::uint32_t ota_retry_delay_ms(OtaRetryKind kind, unsigned failures,
    std::uint32_t random, std::uint32_t normal_interval, std::uint32_t normal_jitter)
{
    if (kind == OtaRetryKind::normal_check)
        return normal_interval + (normal_jitter ? random % normal_jitter : 0U);
    constexpr std::uint32_t initial = 5U * 60U * 1000U;
    constexpr std::uint32_t maximum = 60U * 60U * 1000U;
    std::uint32_t base = initial;
    if (kind == OtaRetryKind::transient_failure) {
        // At most four iterations, including for a saturated failure counter.
        for (unsigned attempt = 1; attempt < failures && base < maximum; ++attempt)
            base = base > maximum / 2U ? maximum : base * 2U;
    }
    return base + random % base;
}
} // namespace inverter_gateway::app
