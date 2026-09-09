#include "inverter_gateway/inverter/polling_plan.hpp"

namespace inverter_gateway::inverter {
namespace {

constexpr PollRequest requests[] = {
    {RegisterSpace::holding, 2, 1, PollCadence::boot_once, feature_none, "device-type"},
    {RegisterSpace::holding, 5, 15, PollCadence::boot_once, feature_none, "serial-number"},
    {RegisterSpace::holding, 28, 37, PollCadence::boot_once, feature_none, "versions-model-rated-power"},
    {RegisterSpace::holding, 3, 2, PollCadence::setup_once, feature_none, "modbus-configuration"},
    {RegisterSpace::holding, 20, 8, PollCadence::setup_once, feature_none, "communication-and-clock"},
    {RegisterSpace::holding, 68, 28, PollCadence::setup_once, feature_none, "grid-pv-ct-policy"},
    {RegisterSpace::holding, 100, 17, PollCadence::setup_once, feature_none, "startup-eps-protection"},
    {RegisterSpace::holding, 121, 103, PollCadence::setup_once, feature_none, "battery-schedules-parallel-generator"},
    {RegisterSpace::holding, 231, 5, PollCadence::setup_once, feature_none, "bluetooth-and-collector"},
    {RegisterSpace::holding, 250, 31, PollCadence::setup_once, feature_none, "grid-code-protection"},
    // This setting can be changed from the inverter while the ESP is running.
    // Keep the SOL/UTI/SBU value current instead of treating it as setup-only.
    {RegisterSpace::holding, 182, 1, PollCadence::normal, feature_none, "source-priority"},

    {RegisterSpace::input, 0, 40, PollCadence::critical, feature_none, "core-status-faults"},
    // Consecutive essential measurements; retain short independent reads for model compatibility.
    {RegisterSpace::input, 40, 28, PollCadence::fast, feature_none, "grid-eps-pv1-pv2"},
    {RegisterSpace::input, 250, 2, PollCadence::fast, feature_pv, "pv-total-power"},
    {RegisterSpace::input, 286, 2, PollCadence::fast, feature_none, "ac-active-total"},
    {RegisterSpace::input, 332, 2, PollCadence::fast, feature_none, "eps-apparent-r"},
    {RegisterSpace::input, 334, 2, PollCadence::fast, feature_none, "eps-active-r"},
    {RegisterSpace::input, 336, 2, PollCadence::fast, feature_three_phase, "eps-apparent-s"},
    {RegisterSpace::input, 338, 2, PollCadence::fast, feature_three_phase, "eps-active-s"},
    {RegisterSpace::input, 340, 2, PollCadence::fast, feature_three_phase, "eps-apparent-t"},
    {RegisterSpace::input, 342, 2, PollCadence::fast, feature_three_phase, "eps-active-t"},
    {RegisterSpace::input, 349, 2, PollCadence::fast, feature_battery, "battery-discharge-power"},
    {RegisterSpace::input, 351, 2, PollCadence::fast, feature_battery, "battery-charge-power"},
    {RegisterSpace::input, 125, 8, PollCadence::fast, feature_battery, "battery"},
    {RegisterSpace::input, 133, 38, PollCadence::fast, feature_battery | feature_bms, "bms"},
    {RegisterSpace::input, 68, 28, PollCadence::normal, feature_extended_pv, "pv3-pv16"},
    {RegisterSpace::input, 96, 2, PollCadence::normal, feature_generator, "generator-r-electrical"},
    {RegisterSpace::input, 98, 4, PollCadence::normal, feature_generator | feature_three_phase, "generator-st-electrical"},
    {RegisterSpace::input, 102, 1, PollCadence::normal, feature_generator, "generator-frequency"},
    {RegisterSpace::input, 106, 3, PollCadence::normal, feature_none, "fan-and-runtime"},
    {RegisterSpace::input, 109, 16, PollCadence::on_demand, feature_service, "dsp-debug"},
    {RegisterSpace::input, 171, 5, PollCadence::slow, feature_battery, "battery-counters"},
    {RegisterSpace::input, 186, 10, PollCadence::on_demand, feature_service, "bms-debug"},
    {RegisterSpace::input, 252, 32, PollCadence::fast, feature_pv, "pv-channel-power"},
    // FSC variants can reject a wide request when it contains an unsupported
    // phase-specific register. Keep live power-flow measurements independent.
    {RegisterSpace::input, 284, 2, PollCadence::fast, feature_none, "ac-apparent-total"},
    {RegisterSpace::input, 288, 2, PollCadence::fast, feature_none, "ac-reactive-total"},
    {RegisterSpace::input, 290, 2, PollCadence::fast, feature_none, "ac-apparent-r"},
    {RegisterSpace::input, 292, 2, PollCadence::fast, feature_none, "ac-active-r"},
    {RegisterSpace::input, 294, 2, PollCadence::fast, feature_none, "ac-reactive-r"},
    {RegisterSpace::input, 298, 2, PollCadence::fast, feature_three_phase, "ac-active-s"},
    {RegisterSpace::input, 304, 2, PollCadence::fast, feature_three_phase, "ac-active-t"},
    {RegisterSpace::input, 314, 2, PollCadence::fast, feature_none, "power-to-user-r"},
    {RegisterSpace::input, 320, 2, PollCadence::fast, feature_none, "total-power-to-user"},
    {RegisterSpace::input, 322, 2, PollCadence::fast, feature_none, "power-to-grid-r"},
    {RegisterSpace::input, 328, 2, PollCadence::fast, feature_none, "total-power-to-grid"},
    {RegisterSpace::input, 330, 2, PollCadence::fast, feature_none, "local-load-power"},
    {RegisterSpace::input, 344, 1, PollCadence::fast, feature_three_phase, "load-percent"},
    {RegisterSpace::input, 345, 2, PollCadence::on_demand, feature_none, "system-generation-power"},
    {RegisterSpace::input, 347, 2, PollCadence::fast, feature_none, "self-consumption-power"},
    {RegisterSpace::input, 353, 2, PollCadence::fast, feature_battery, "ac-battery-charge-power"},
    {RegisterSpace::input, 355, 2, PollCadence::fast, feature_none, "extra-power-to-grid"},
    {RegisterSpace::input, 357, 2, PollCadence::normal, feature_generator, "generator-r-apparent-power"},
    {RegisterSpace::input, 359, 4, PollCadence::normal, feature_generator | feature_three_phase, "generator-st-apparent-power"},
    {RegisterSpace::input, 363, 2, PollCadence::normal, feature_generator, "generator-r-active-power"},
    {RegisterSpace::input, 365, 4, PollCadence::normal, feature_generator | feature_three_phase, "generator-st-active-power"},
    // Read energy in short groups so a model-specific counter does not hide
    // the other valid daily and lifetime counters.
    {RegisterSpace::input, 375, 8, PollCadence::slow, feature_none, "energy-generation-pv"},
    {RegisterSpace::input, 383, 8, PollCadence::slow, feature_none, "energy-battery"},
    {RegisterSpace::input, 391, 8, PollCadence::slow, feature_none, "energy-system"},
    {RegisterSpace::input, 399, 8, PollCadence::slow, feature_none, "energy-self-use"},
    {RegisterSpace::input, 407, 8, PollCadence::slow, feature_none, "energy-load-export"},
    {RegisterSpace::input, 415, 8, PollCadence::slow, feature_none, "energy-grid-import"},
    {RegisterSpace::input, 423, 8, PollCadence::slow, feature_none, "energy-flow-counters"},
    {RegisterSpace::input, 450, 49, PollCadence::fast, feature_parallel, "parallel-system"},
};

static_assert(sizeof(requests) / sizeof(requests[0]) <= poll_plan_capacity,
              "Polling plan exceeds scheduler storage");

} // namespace

PollPlanView default_poll_plan()
{
    return {requests, sizeof(requests) / sizeof(requests[0])};
}

bool poll_request_is_enabled(const PollRequest &request,
                             std::uint16_t enabled_features)
{
    return request.required_features == feature_none ||
           (enabled_features & request.required_features) == request.required_features;
}

const char *poll_cadence_name(PollCadence cadence)
{
    switch (cadence) {
    case PollCadence::boot_once: return "BOOT_ONCE";
    case PollCadence::setup_once: return "SETUP_ONCE";
    case PollCadence::critical: return "CRITICAL";
    case PollCadence::fast: return "FAST";
    case PollCadence::normal: return "NORMAL";
    case PollCadence::slow: return "SLOW";
    case PollCadence::on_demand: return "ON_DEMAND";
    default: return "UNKNOWN";
    }
}

} // namespace inverter_gateway::inverter
