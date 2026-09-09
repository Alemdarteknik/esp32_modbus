#pragma once

#include <cstdint>

#include "inverter_gateway/protocol/modbus_rtu.hpp"

namespace inverter_gateway::inverter {

enum class InverterOperatingState : std::uint8_t {
    unknown,
    operating,
    standby,
    stopped,
    faulted,
    updating,
};

enum class LocalOperation : std::uint8_t {
    set_buzzer_enabled,
    set_bluetooth_enabled,
    set_inverter_enabled,
    set_overload_restart_enabled,
    set_overload_to_bypass_enabled,
    set_battery_type,
    set_eco_mode_enabled,
    set_overtemperature_restart_enabled,
    set_input_change_reminder_enabled,
    set_output_split_phase_enabled,
    set_generator_auto_input_enabled,
    set_dual_channel_load_enabled,
    set_grid_feedback_enabled,
};

enum class WriteGuard : std::uint8_t {
    hot_edit_confirmed,
    runtime_command,
    standby_required,
};

enum class CommandOutcome : std::uint8_t {
    invalid_request,
    state_unavailable,
    state_not_allowed,
    communication_error,
    rejected,
    confirmed,
    adjusted,
    transition_pending,
    completed,
    unknown,
};

struct InverterStateSnapshot {
    InverterOperatingState state = InverterOperatingState::unknown;
    std::uint16_t raw_status = 0;
    bool fresh = false;
    bool battery_commissioning_interlock = false;
};

struct LocalCommandRequest {
    std::uint64_t command_id = 0;
    LocalOperation operation = LocalOperation::set_buzzer_enabled;
    std::int32_t value = 0;
};

struct LocalCommandResult {
    std::uint64_t command_id = 0;
    LocalOperation operation = LocalOperation::set_buzzer_enabled;
    CommandOutcome outcome = CommandOutcome::invalid_request;
    WriteGuard guard = WriteGuard::hot_edit_confirmed;
    std::uint16_t register_address = 0;
    std::uint16_t previous_value = 0;
    std::uint16_t requested_register_value = 0;
    std::uint16_t readback_value = 0;
    std::uint16_t related_status_raw = 0;
    bool previous_value_valid = false;
    bool readback_value_valid = false;
    bool related_status_valid = false;
    bool write_was_sent = false;
    protocol::ModbusWriteResult modbus_write{};
};

const char *operation_name(LocalOperation operation);
const char *command_outcome_name(CommandOutcome outcome);

} // namespace inverter_gateway::inverter
