#include "inverter_gateway/inverter/register_command.hpp"

namespace inverter_gateway::inverter {

const char *register_command_outcome_name(RegisterCommandOutcome outcome)
{
    switch (outcome) {
    case RegisterCommandOutcome::invalid_request: return "invalid_request";
    case RegisterCommandOutcome::not_documented: return "not_documented";
    case RegisterCommandOutcome::not_readable: return "not_readable";
    case RegisterCommandOutcome::read_only: return "read_only";
    case RegisterCommandOutcome::blocked_unvalidated: return "blocked_unvalidated";
    case RegisterCommandOutcome::confirmation_required: return "confirmation_required";
    case RegisterCommandOutcome::expected_values_required: return "expected_values_required";
    case RegisterCommandOutcome::expected_value_mismatch: return "expected_value_mismatch";
    case RegisterCommandOutcome::state_unavailable: return "state_unavailable";
    case RegisterCommandOutcome::state_not_allowed: return "state_not_allowed";
    case RegisterCommandOutcome::guarded_interlock_required: return "guarded_interlock_required";
    case RegisterCommandOutcome::commissioning_interlock_required: return "commissioning_interlock_required";
    case RegisterCommandOutcome::service_interlock_required: return "service_interlock_required";
    case RegisterCommandOutcome::communication_error: return "communication_error";
    case RegisterCommandOutcome::confirmed: return "confirmed";
    case RegisterCommandOutcome::sent_unverifiable: return "sent_unverifiable";
    case RegisterCommandOutcome::completed: return "completed";
    case RegisterCommandOutcome::transition_pending: return "transition_pending";
    case RegisterCommandOutcome::readback_mismatch: return "readback_mismatch";
    }
    return "unknown";
}

} // namespace inverter_gateway::inverter
