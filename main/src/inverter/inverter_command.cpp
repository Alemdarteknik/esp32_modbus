#include "inverter_gateway/inverter/inverter_command.hpp"

namespace inverter_gateway::inverter {

const char *operation_name(LocalOperation operation)
{
    switch (operation) {
    case LocalOperation::set_buzzer_enabled: return "set_buzzer_enabled";
    case LocalOperation::set_bluetooth_enabled: return "set_bluetooth_enabled";
    case LocalOperation::set_inverter_enabled: return "set_inverter_enabled";
    case LocalOperation::set_overload_restart_enabled:
        return "set_overload_restart_enabled";
    case LocalOperation::set_overload_to_bypass_enabled:
        return "set_overload_to_bypass_enabled";
    case LocalOperation::set_battery_type: return "set_battery_type";
    case LocalOperation::set_eco_mode_enabled: return "set_eco_mode_enabled";
    case LocalOperation::set_overtemperature_restart_enabled: return "set_overtemperature_restart_enabled";
    case LocalOperation::set_input_change_reminder_enabled: return "set_input_change_reminder_enabled";
    case LocalOperation::set_output_split_phase_enabled: return "set_output_split_phase_enabled";
    case LocalOperation::set_generator_auto_input_enabled: return "set_generator_auto_input_enabled";
    case LocalOperation::set_dual_channel_load_enabled: return "set_dual_channel_load_enabled";
    case LocalOperation::set_grid_feedback_enabled: return "set_grid_feedback_enabled";
    default: return "unknown";
    }
}

const char *command_outcome_name(CommandOutcome outcome)
{
    switch (outcome) {
    case CommandOutcome::invalid_request: return "INVALID_REQUEST";
    case CommandOutcome::state_unavailable: return "STATE_UNAVAILABLE";
    case CommandOutcome::state_not_allowed: return "STATE_NOT_ALLOWED";
    case CommandOutcome::communication_error: return "COMMUNICATION_ERROR";
    case CommandOutcome::rejected: return "REJECTED";
    case CommandOutcome::confirmed: return "CONFIRMED";
    case CommandOutcome::adjusted: return "ADJUSTED";
    case CommandOutcome::transition_pending: return "TRANSITION_PENDING";
    case CommandOutcome::completed: return "COMPLETED";
    case CommandOutcome::unknown: return "UNKNOWN";
    default: return "UNKNOWN";
    }
}

} // namespace inverter_gateway::inverter
