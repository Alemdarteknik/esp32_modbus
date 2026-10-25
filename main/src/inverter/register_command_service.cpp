#include "inverter_gateway/inverter/register_command_service.hpp"

#include <algorithm>

#include "inverter_gateway/inverter/command_service.hpp"
#include "inverter_gateway/inverter/register_value.hpp"

namespace inverter_gateway::inverter {
namespace {

RegisterCommandResult base_result(const RegisterCommandRequest &request)
{
    RegisterCommandResult result{};
    result.command_id = request.command_id;
    result.space = request.space;
    result.first_register = request.first_register;
    result.register_count = request.register_count;
    result.named_request = request.named_request;
    return result;
}

bool state_is_stopped(const InverterStateSnapshot &state)
{
    return state.fresh &&
           (state.state == InverterOperatingState::standby ||
            state.state == InverterOperatingState::stopped);
}

int guard_rank(RegisterWriteGuard guard)
{
    switch (guard) {
    case RegisterWriteGuard::hot_edit_confirmed: return 1;
    case RegisterWriteGuard::hot_edit_guarded: return 2;
    case RegisterWriteGuard::runtime_command: return 3;
    case RegisterWriteGuard::standby_required: return 4;
    case RegisterWriteGuard::commissioning_only: return 5;
    case RegisterWriteGuard::service_only: return 6;
    default: return 0;
    }
}

bool descriptor_is_writable(const RegisterDescriptor &descriptor)
{
    return descriptor.access == RegisterAccess::read_write ||
           descriptor.access == RegisterAccess::write_only;
}

bool pair_is_complete(RegisterSpace space, std::uint16_t first,
                      std::uint16_t count, std::uint16_t index,
                      const RegisterDescriptor &descriptor)
{
    if (descriptor_starts_multiword_value(descriptor)) {
        if (index + 1 >= count) return false;
        const auto *low = find_register(space, first + index + 1);
        return low != nullptr && descriptor_is_low_word(*low);
    }
    if (descriptor_is_low_word(descriptor)) {
        if (index == 0) return false;
        const auto *high = find_register(space, first + index - 1);
        return high != nullptr && descriptor_starts_multiword_value(*high);
    }
    return true;
}

} // namespace

RegisterCommandService::RegisterCommandService(protocol::ModbusRtuClient &client)
    : client_(client)
{
}

RegisterCommandResult RegisterCommandService::execute(
    const RegisterCommandRequest &request, const InverterStateSnapshot &state,
    bool write_operation)
{
    if (request.command_id == 0 || request.register_count == 0 ||
        request.register_count > register_command_word_count ||
        static_cast<std::uint32_t>(request.first_register) +
                request.register_count - 1U > 0xffffU) {
        return base_result(request);
    }
    return write_operation ? execute_write(request, state) : execute_read(request);
}

RegisterCommandResult RegisterCommandService::execute_read(
    const RegisterCommandRequest &request)
{
    auto result = base_result(request);
    for (std::uint16_t index = 0; index < request.register_count; ++index) {
        const auto *descriptor = find_register(
            request.space,
            static_cast<std::uint16_t>(request.first_register + index));
        if (descriptor == nullptr) {
            result.outcome = RegisterCommandOutcome::not_documented;
            return result;
        }
        if (descriptor->access == RegisterAccess::write_only) {
            result.outcome = RegisterCommandOutcome::not_readable;
            return result;
        }
    }
    const bool success = request.space == RegisterSpace::holding
                             ? client_.read_holding(request.first_register,
                                                    request.register_count,
                                                    result.result_values.data(),
                                                    result.result_values.size())
                             : client_.read_input(request.first_register,
                                                  request.register_count,
                                                  result.result_values.data(),
                                                  result.result_values.size());
    result.outcome = success ? RegisterCommandOutcome::confirmed
                             : RegisterCommandOutcome::communication_error;
    result.result_values_valid = success;
    return result;
}

RegisterCommandResult RegisterCommandService::execute_write(
    const RegisterCommandRequest &request, const InverterStateSnapshot &state)
{
    auto result = base_result(request);
    if (request.space != RegisterSpace::holding || !request.confirmed) {
        result.outcome = request.space != RegisterSpace::holding
                             ? RegisterCommandOutcome::read_only
                             : RegisterCommandOutcome::confirmation_required;
        return result;
    }

    bool has_write_only = false;
    bool requires_guarded_interlock = false;
    bool requires_commissioning = false;
    bool requires_service = false;
    int strongest_rank = 0;
    for (std::uint16_t index = 0; index < request.register_count; ++index) {
        const auto *descriptor = find_register(RegisterSpace::holding,
                                               request.first_register + index);
        if (descriptor == nullptr) {
            result.outcome = RegisterCommandOutcome::not_documented;
            return result;
        }
        if (descriptor->write_guard == RegisterWriteGuard::blocked_unvalidated) {
            result.outcome = RegisterCommandOutcome::blocked_unvalidated;
            return result;
        }
        if (!descriptor_is_writable(*descriptor) ||
            descriptor->write_guard == RegisterWriteGuard::read_only) {
            result.outcome = RegisterCommandOutcome::read_only;
            return result;
        }
        if (!pair_is_complete(RegisterSpace::holding, request.first_register,
                              request.register_count, index, *descriptor)) {
            result.outcome = RegisterCommandOutcome::invalid_request;
            return result;
        }
        if (descriptor->address == 147 && request.values[index] != 0 &&
            request.values[index] != 1 && request.values[index] != 2 &&
            request.values[index] != 255) {
            result.outcome = RegisterCommandOutcome::invalid_request;
            return result;
        }
        has_write_only = has_write_only || descriptor->access == RegisterAccess::write_only;
        requires_guarded_interlock = requires_guarded_interlock ||
            descriptor->write_guard == RegisterWriteGuard::hot_edit_guarded;
        requires_commissioning = requires_commissioning ||
                                 descriptor->write_guard == RegisterWriteGuard::commissioning_only;
        requires_service = requires_service ||
                           descriptor->write_guard == RegisterWriteGuard::service_only;
        const int rank = guard_rank(descriptor->write_guard);
        if (rank > strongest_rank) {
            strongest_rank = rank;
            result.strongest_guard = descriptor->write_guard;
        }
    }

    if (has_write_only && request.register_count != 1) {
        result.outcome = RegisterCommandOutcome::invalid_request;
        return result;
    }
    if (result.strongest_guard == RegisterWriteGuard::runtime_command) {
        const bool inverter_off = request.first_register == 0 &&
                                  request.register_count == 1 &&
                                  request.values[0] == 0;
        if (!inverter_off && !state.fresh) {
            result.outcome = RegisterCommandOutcome::state_unavailable;
            return result;
        }
        if (!inverter_off &&
            (state.state == InverterOperatingState::faulted ||
             state.state == InverterOperatingState::updating ||
             state.state == InverterOperatingState::unknown)) {
            result.outcome = RegisterCommandOutcome::state_not_allowed;
            return result;
        }
        if (request.first_register == 0 && request.register_count == 1 &&
            request.values[0] > 1) {
            result.outcome = RegisterCommandOutcome::blocked_unvalidated;
            return result;
        }
    }
    if ((result.strongest_guard == RegisterWriteGuard::standby_required ||
         result.strongest_guard == RegisterWriteGuard::commissioning_only ||
         result.strongest_guard == RegisterWriteGuard::service_only) &&
        !state.fresh) {
        result.outcome = RegisterCommandOutcome::state_unavailable;
        return result;
    }
    if ((result.strongest_guard == RegisterWriteGuard::standby_required ||
         result.strongest_guard == RegisterWriteGuard::commissioning_only ||
         result.strongest_guard == RegisterWriteGuard::service_only) &&
        !state_is_stopped(state)) {
        result.outcome = RegisterCommandOutcome::state_not_allowed;
        return result;
    }
    if (requires_guarded_interlock && !request.guarded_interlock) {
        result.outcome = RegisterCommandOutcome::guarded_interlock_required;
        return result;
    }
    if (requires_commissioning && !request.commissioning_interlock) {
        result.outcome = RegisterCommandOutcome::commissioning_interlock_required;
        return result;
    }
    if (requires_service && !request.service_interlock) {
        result.outcome = RegisterCommandOutcome::service_interlock_required;
        return result;
    }

    if (request.first_register == 0 && request.register_count == 1) {
        if (!request.named_request && !request.expected_values_present) {
            result.outcome = RegisterCommandOutcome::expected_values_required;
            return result;
        }
        if (request.expected_values_present) {
            std::uint16_t previous = 0;
            if (!client_.read_holding(0, 1, &previous, 1)) {
                result.outcome = RegisterCommandOutcome::communication_error;
                return result;
            }
            if (previous != request.expected_values[0]) {
                result.previous_values_valid = true;
                result.previous_values[0] = previous;
                result.outcome = RegisterCommandOutcome::expected_value_mismatch;
                return result;
            }
        }
        LocalCommandRequest control{};
        control.command_id = request.command_id;
        control.operation = LocalOperation::set_inverter_enabled;
        control.value = request.values[0];
        const auto actual = CommandService(client_).execute(control, state);
        result.modbus_write = actual.modbus_write;
        result.previous_values_valid = actual.previous_value_valid;
        result.previous_values[0] = actual.previous_value;
        result.result_values_valid = actual.readback_value_valid;
        result.result_values[0] = actual.readback_value;
        switch (actual.outcome) {
        case CommandOutcome::completed: result.outcome = RegisterCommandOutcome::completed; break;
        case CommandOutcome::transition_pending: result.outcome = RegisterCommandOutcome::transition_pending; break;
        case CommandOutcome::state_unavailable: result.outcome = RegisterCommandOutcome::state_unavailable; break;
        case CommandOutcome::state_not_allowed: result.outcome = RegisterCommandOutcome::state_not_allowed; break;
        case CommandOutcome::invalid_request: result.outcome = RegisterCommandOutcome::invalid_request; break;
        default: result.outcome = RegisterCommandOutcome::communication_error; break;
        }
        return result;
    }

    if (!has_write_only) {
        if (!request.named_request && !request.expected_values_present) {
            result.outcome = RegisterCommandOutcome::expected_values_required;
            return result;
        }
        if (!client_.read_holding(request.first_register, request.register_count,
                                  result.previous_values.data(),
                                  result.previous_values.size())) {
            result.outcome = RegisterCommandOutcome::communication_error;
            return result;
        }
        result.previous_values_valid = true;
        if (request.expected_values_present &&
            !std::equal(result.previous_values.begin(),
                        result.previous_values.begin() + request.register_count,
                        request.expected_values.begin())) {
            result.outcome = RegisterCommandOutcome::expected_value_mismatch;
            return result;
        }
        if (result.strongest_guard != RegisterWriteGuard::runtime_command &&
            std::equal(result.previous_values.begin(),
                       result.previous_values.begin() + request.register_count,
                       request.values.begin())) {
            std::copy_n(result.previous_values.begin(), request.register_count,
                        result.result_values.begin());
            result.result_values_valid = true;
            result.outcome = RegisterCommandOutcome::confirmed;
            return result;
        }
    }

    result.modbus_write = request.register_count == 1
                              ? client_.write_single(request.first_register,
                                                     request.values[0])
                              : client_.write_multiple(request.first_register,
                                                       request.register_count,
                                                       request.values.data());
    if (!result.modbus_write.ok()) {
        result.outcome = RegisterCommandOutcome::communication_error;
        return result;
    }
    if (has_write_only) {
        result.outcome = RegisterCommandOutcome::sent_unverifiable;
        return result;
    }
    if (!client_.read_holding(request.first_register, request.register_count,
                              result.result_values.data(),
                              result.result_values.size())) {
        result.outcome = RegisterCommandOutcome::communication_error;
        return result;
    }
    result.result_values_valid = true;
    result.outcome = std::equal(result.result_values.begin(),
                                result.result_values.begin() + request.register_count,
                                request.values.begin())
                         ? RegisterCommandOutcome::confirmed
                         : RegisterCommandOutcome::readback_mismatch;
    return result;
}

} // namespace inverter_gateway::inverter
