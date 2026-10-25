#pragma once

#include <array>
#include <cstdint>

#include "inverter_gateway/inverter/register_catalog.hpp"
#include "inverter_gateway/protocol/modbus_rtu.hpp"

namespace inverter_gateway::inverter {

inline constexpr std::size_t register_command_word_count = 24;

enum class RegisterCommandOutcome : std::uint8_t {
    invalid_request,
    not_documented,
    not_readable,
    read_only,
    blocked_unvalidated,
    confirmation_required,
    expected_values_required,
    expected_value_mismatch,
    state_unavailable,
    state_not_allowed,
    guarded_interlock_required,
    commissioning_interlock_required,
    service_interlock_required,
    communication_error,
    confirmed,
    sent_unverifiable,
    completed,
    transition_pending,
    readback_mismatch,
};

struct RegisterCommandRequest {
    std::uint64_t command_id = 0;
    RegisterSpace space = RegisterSpace::holding;
    std::uint16_t first_register = 0;
    std::uint16_t register_count = 0;
    bool named_request = false;
    bool confirmed = false;
    bool expected_values_present = false;
    bool guarded_interlock = false;
    bool commissioning_interlock = false;
    bool service_interlock = false;
    std::array<std::uint16_t, register_command_word_count> values{};
    std::array<std::uint16_t, register_command_word_count> expected_values{};
};

struct RegisterCommandResult {
    std::uint64_t command_id = 0;
    RegisterCommandOutcome outcome = RegisterCommandOutcome::invalid_request;
    RegisterSpace space = RegisterSpace::holding;
    RegisterWriteGuard strongest_guard = RegisterWriteGuard::read_only;
    std::uint16_t first_register = 0;
    std::uint16_t register_count = 0;
    bool named_request = false;
    bool previous_values_valid = false;
    bool result_values_valid = false;
    protocol::ModbusWriteResult modbus_write{};
    std::array<std::uint16_t, register_command_word_count> previous_values{};
    std::array<std::uint16_t, register_command_word_count> result_values{};
};

const char *register_command_outcome_name(RegisterCommandOutcome outcome);

} // namespace inverter_gateway::inverter
