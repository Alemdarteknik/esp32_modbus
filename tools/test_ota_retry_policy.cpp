#include "ota_retry_policy.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

int main()
{
    using namespace inverter_gateway::app;
    constexpr std::uint32_t hour = 60U * 60U * 1000U;
    const auto delay = [](OtaRetryKind kind, unsigned failures, std::uint32_t random) {
        return ota_retry_delay_ms(kind, failures, random, 6U * hour, hour);
    };
    for (const auto random : {0U, 1U, 299999U, 3599999U, 0xffffffffU}) {
        const auto normal = delay(OtaRetryKind::normal_check, 0, random);
        assert(normal >= 6U * hour && normal < 7U * hour);
        const auto local = delay(OtaRetryKind::local_deferred, 100, random);
        assert(local >= 300000U && local < 600000U);
    }
    const std::array<std::uint32_t, 6> bases{300000U, 600000U, 1200000U, hour / 2U, hour / 2U, hour / 2U};
    for (unsigned failure = 1; failure <= bases.size(); ++failure) {
        assert(delay(OtaRetryKind::transient_failure, failure, 0) == bases[failure - 1]);
        const auto high = delay(OtaRetryKind::transient_failure, failure, bases[failure - 1] - 1);
        assert(high == 2U * bases[failure - 1] - 1);
    }
    assert(delay(OtaRetryKind::transient_failure, 0, 0) == 300000U);
    assert(delay(OtaRetryKind::transient_failure, std::numeric_limits<unsigned>::max(), 0) == hour / 2U);
    assert(ota_retry_delay_ms(OtaRetryKind::normal_check, 0, 123, 1000, 0) == 1000);
    // Synthetic reproducible fleet sample tests range/distribution, not capacity.
    std::array<unsigned, 10> buckets{};
    std::uint32_t random = 0x12876731U;
    for (unsigned device = 0; device < 1000; ++device) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        const auto value = delay(OtaRetryKind::transient_failure, 1, random);
        assert(value >= 300000U && value < 600000U);
        ++buckets[(value - 300000U) / 30000U];
    }
    for (auto count : buckets) assert(count > 0 && count < 250);
    std::cout << "PASS: OTA retry boundaries, saturation and 1000-device synthetic jitter sample\n";
}
