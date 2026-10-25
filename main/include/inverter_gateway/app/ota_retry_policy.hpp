#pragma once
#include <cstdint>

namespace inverter_gateway::app {
enum class OtaRetryKind { normal_check, local_deferred, transient_failure };

// Failed checks retry with bounded backoff and jitter. Successful checks and
// permanent image/HTTP rejections retain the fleet-wide polling interval.
constexpr std::uint32_t ota_retry_delay_ms(OtaRetryKind kind, unsigned failures,
    std::uint32_t random, std::uint32_t normal_interval, std::uint32_t normal_jitter)
{
    if (kind == OtaRetryKind::normal_check)
        return normal_interval + (normal_jitter ? random % normal_jitter : 0U);
    constexpr std::uint32_t initial = 5U * 60U * 1000U;
    // The added jitter is below base, keeping the total below one hour.
    constexpr std::uint32_t maximum_base = 30U * 60U * 1000U;
    std::uint32_t base = initial;
    if (kind == OtaRetryKind::transient_failure) {
        for (unsigned attempt = 1; attempt < failures && base < maximum_base; ++attempt)
            base = base > maximum_base / 2U ? maximum_base : base * 2U;
    }
    return base + random % base;
}
} // namespace inverter_gateway::app
