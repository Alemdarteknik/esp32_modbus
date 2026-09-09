#pragma once

#include <cstddef>
#include <cstdint>

#include "inverter_gateway/inverter/register_catalog.hpp"

namespace inverter_gateway::inverter {

inline constexpr std::size_t poll_plan_capacity = 128;

enum class PollCadence : std::uint8_t {
    boot_once,
    setup_once,
    critical,
    fast,
    normal,
    slow,
    on_demand,
};

struct PollRequest {
    RegisterSpace space;
    std::uint16_t first_register;
    std::uint16_t register_count;
    PollCadence cadence;
    std::uint16_t required_features;
    const char *name;
};

struct PollPlanView {
    const PollRequest *data;
    std::size_t size;
};

PollPlanView default_poll_plan();
bool poll_request_is_enabled(const PollRequest &request,
                             std::uint16_t enabled_features);
const char *poll_cadence_name(PollCadence cadence);

} // namespace inverter_gateway::inverter
