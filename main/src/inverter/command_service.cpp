#include "inverter_gateway/inverter/command_service.hpp"

#include "inverter_gateway/inverter/aohai_registers.hpp"

namespace inverter_gateway::inverter {
namespace {

bool is_boolean_value(std::int32_t value)
{
    return value == 0 || value == 1;
}

bool state_allows_standby_write(const InverterStateSnapshot &state)
{
    return state.fresh &&
           (state.state == InverterOperatingState::standby ||
            state.state == InverterOperatingState::stopped);
}

LocalCommandResult make_base_result(const LocalCommandRequest &request,
                                    WriteGuard guard,
                                    std::uint16_t register_address)
{
    LocalCommandResult result{};
    result.command_id = request.command_id;
    result.operation = request.operation;
    result.guard = guard;
    result.register_address = register_address;
    return result;
}

} // namespace

CommandService::CommandService(protocol::ModbusRtuClient &client)
    : client_(client)
{
}

LocalCommandResult CommandService::execute(const LocalCommandRequest &request,
                                           const InverterStateSnapshot &state)
{
    switch (request.operation) {
    case LocalOperation::set_buzzer_enabled:
        return execute_boolean_register(request, WriteGuard::hot_edit_confirmed,
                                        aohai::holding_buzzer);
    case LocalOperation::set_bluetooth_enabled:
        return execute_boolean_register(request, WriteGuard::hot_edit_confirmed,
                                        aohai::holding_bluetooth);
    case LocalOperation::set_inverter_enabled:
        return execute_inverter_enabled(request, state);
    case LocalOperation::set_overload_restart_enabled:
        return execute_feature_bit(request, state, 1);
    case LocalOperation::set_eco_mode_enabled:
        return execute_feature_bit(request, state, 0);
    case LocalOperation::set_overtemperature_restart_enabled:
        return execute_feature_bit(request, state, 2);
    case LocalOperation::set_input_change_reminder_enabled:
        return execute_feature_bit(request, state, 3);
    case LocalOperation::set_output_split_phase_enabled:
        return execute_feature_bit(request, state, 4);
    case LocalOperation::set_generator_auto_input_enabled:
        return execute_feature_bit(request, state, 5);
    case LocalOperation::set_dual_channel_load_enabled:
        return execute_feature_bit(request, state, 6);
    case LocalOperation::set_grid_feedback_enabled:
        return execute_feature_bit(request, state, 7);
    case LocalOperation::set_overload_to_bypass_enabled:
        if (!state.fresh) {
            auto result = make_base_result(request, WriteGuard::standby_required,
                                           aohai::holding_overload_to_bypass);
            result.outcome = CommandOutcome::state_unavailable;
            return result;
        }
        if (!state_allows_standby_write(state)) {
            auto result = make_base_result(request, WriteGuard::standby_required,
                                           aohai::holding_overload_to_bypass);
            result.outcome = CommandOutcome::state_not_allowed;
            return result;
        }
        return execute_boolean_register(request, WriteGuard::standby_required,
                                        aohai::holding_overload_to_bypass);
    case LocalOperation::set_battery_type:
        return execute_battery_type(request, state);
    default:
        return make_base_result(request, WriteGuard::standby_required, 0);
    }
}

LocalCommandResult CommandService::execute_boolean_register(
    const LocalCommandRequest &request, WriteGuard guard,
    std::uint16_t register_address)
{
    auto result = make_base_result(request, guard, register_address);
    if (!is_boolean_value(request.value)) {
        result.outcome = CommandOutcome::invalid_request;
        return result;
    }

    std::uint16_t previous = 0;
    if (!read_holding(register_address, previous)) {
        result.outcome = CommandOutcome::communication_error;
        return result;
    }
    result.previous_value = previous;
    result.previous_value_valid = true;
    result.requested_register_value = static_cast<std::uint16_t>(request.value);
    if (previous == result.requested_register_value) {
        result.readback_value = previous;
        result.readback_value_valid = true;
        result.outcome = CommandOutcome::confirmed;
        return result;
    }

    return write_and_verify(request, guard, register_address,
                            result.requested_register_value, true, previous);
}

LocalCommandResult CommandService::execute_feature_bit(
    const LocalCommandRequest &request, const InverterStateSnapshot &state,
    std::uint8_t bit)
{
    auto result = make_base_result(request, WriteGuard::standby_required,
                                   aohai::holding_feature_flags);
    if (!is_boolean_value(request.value) || bit >= 16) {
        result.outcome = CommandOutcome::invalid_request;
        return result;
    }
    if (!state.fresh) {
        result.outcome = CommandOutcome::state_unavailable;
        return result;
    }
    if (!state_allows_standby_write(state)) {
        result.outcome = CommandOutcome::state_not_allowed;
        return result;
    }
    std::uint16_t previous = 0;
    if (!read_holding(aohai::holding_feature_flags, previous)) {
        result.outcome = CommandOutcome::communication_error;
        return result;
    }
    const std::uint16_t mask = static_cast<std::uint16_t>(1U << bit);
    const std::uint16_t requested = request.value == 1
                                        ? static_cast<std::uint16_t>(previous | mask)
                                        : static_cast<std::uint16_t>(previous & ~mask);
    if (requested == previous) {
        result.previous_value = previous;
        result.previous_value_valid = true;
        result.requested_register_value = requested;
        result.readback_value = previous;
        result.readback_value_valid = true;
        result.outcome = CommandOutcome::confirmed;
        return result;
    }
    return write_and_verify(request, WriteGuard::standby_required,
                            aohai::holding_feature_flags, requested, true, previous);
}

LocalCommandResult CommandService::execute_battery_type(
    const LocalCommandRequest &request, const InverterStateSnapshot &state)
{
    auto result = make_base_result(request, WriteGuard::standby_required,
                                   aohai::holding_battery_type);
    if (request.value < 0 || request.value > 5) {
        result.outcome = CommandOutcome::invalid_request;
        return result;
    }
    if (!state.fresh) {
        result.outcome = CommandOutcome::state_unavailable;
        return result;
    }
    if (!state_allows_standby_write(state)) {
        result.outcome = CommandOutcome::state_not_allowed;
        return result;
    }
    if (!state.battery_commissioning_interlock) {
        result.outcome = CommandOutcome::state_not_allowed;
        return result;
    }

    std::uint16_t previous = 0;
    if (!read_holding(aohai::holding_battery_type, previous)) {
        result.outcome = CommandOutcome::communication_error;
        return result;
    }
    const std::uint16_t requested = static_cast<std::uint16_t>(request.value);
    if (requested == previous) {
        result.previous_value = previous;
        result.previous_value_valid = true;
        result.requested_register_value = requested;
        result.readback_value = previous;
        result.readback_value_valid = true;
        result.outcome = CommandOutcome::confirmed;
        return result;
    }
    return write_and_verify(request, WriteGuard::standby_required,
                            aohai::holding_battery_type, requested, true, previous);
}

LocalCommandResult CommandService::execute_inverter_enabled(
    const LocalCommandRequest &request, const InverterStateSnapshot &state)
{
    auto result = make_base_result(request, WriteGuard::runtime_command,
                                   aohai::holding_inverter_on_off);
    if (!is_boolean_value(request.value)) {
        result.outcome = CommandOutcome::invalid_request;
        return result;
    }
    if (request.value == 1 && !state.fresh) {
        result.outcome = CommandOutcome::state_unavailable;
        return result;
    }
    if (request.value == 1 &&
        (state.state == InverterOperatingState::faulted ||
         state.state == InverterOperatingState::updating ||
         state.state == InverterOperatingState::unknown)) {
        result.outcome = CommandOutcome::state_not_allowed;
        return result;
    }

    std::uint16_t previous = 0;
    const bool previous_valid = read_holding(aohai::holding_inverter_on_off, previous);
    const std::uint16_t requested = static_cast<std::uint16_t>(request.value);
    // H0 is a nonstored runtime command, not the actual operating state.
    // Send each newly admitted request once even when H0 already reads 0/1.
    result = write_and_verify(request, WriteGuard::runtime_command,
                              aohai::holding_inverter_on_off, requested,
                              previous_valid, previous);
    if (!result.modbus_write.ok()) {
        return result;
    }

    std::uint16_t raw_status = 0;
    if (!read_status(raw_status)) {
        result.outcome = CommandOutcome::transition_pending;
        return result;
    }
    result.related_status_raw = raw_status;
    result.related_status_valid = true;
    const bool stopped = requested == 0 && raw_status == 0;
    const bool active = requested == 1 &&
                        (raw_status == 1 || raw_status == 2 || raw_status == 5 ||
                         raw_status == 6 || raw_status == 7);
    result.outcome = stopped || active ? CommandOutcome::completed
                                       : CommandOutcome::transition_pending;
    return result;
}

LocalCommandResult CommandService::write_and_verify(
    const LocalCommandRequest &request, WriteGuard guard,
    std::uint16_t register_address, std::uint16_t register_value,
    bool previous_value_valid, std::uint16_t previous_value)
{
    auto result = make_base_result(request, guard, register_address);
    result.previous_value = previous_value;
    result.previous_value_valid = previous_value_valid;
    result.requested_register_value = register_value;
    result.modbus_write = client_.write_single(register_address, register_value);
    // Other transport failures can occur after a partial send; preserve their
    // existing attempted-write semantics rather than claiming nothing was sent.
    result.write_was_sent = result.modbus_write.status != protocol::ModbusWriteStatus::receive_busy &&
                            result.modbus_write.status != protocol::ModbusWriteStatus::invalid_argument;

    if (!result.modbus_write.ok()) {
        switch (result.modbus_write.status) {
        case protocol::ModbusWriteStatus::exception_response:
        case protocol::ModbusWriteStatus::echo_mismatch:
            result.outcome = CommandOutcome::rejected;
            break;
        case protocol::ModbusWriteStatus::transmit_failed:
        case protocol::ModbusWriteStatus::receive_busy:
        case protocol::ModbusWriteStatus::invalid_argument:
            result.outcome = CommandOutcome::communication_error;
            break;
        default:
            result.outcome = CommandOutcome::unknown;
            break;
        }
        return result;
    }

    std::uint16_t readback = 0;
    if (!read_holding(register_address, readback)) {
        result.outcome = CommandOutcome::unknown;
        return result;
    }
    result.readback_value = readback;
    result.readback_value_valid = true;
    result.outcome = readback == register_value ? CommandOutcome::confirmed
                                                : CommandOutcome::adjusted;
    return result;
}

bool CommandService::read_holding(std::uint16_t address, std::uint16_t &value)
{
    return client_.read_holding(address, 1, &value, 1);
}

bool CommandService::read_status(std::uint16_t &value)
{
    return client_.read_input(aohai::inverter_status, 1, &value, 1);
}

} // namespace inverter_gateway::inverter
