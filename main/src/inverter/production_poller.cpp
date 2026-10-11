#include "inverter_gateway/inverter/production_poller.hpp"

#include <algorithm>

#include "esp_log.h"
#include "esp_timer.h"
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/inverter/register_value.hpp"

namespace inverter_gateway::inverter {
namespace {

constexpr char tag[] = "production_poll";

std::int64_t cadence_interval_ms(PollCadence cadence)
{
    switch (cadence) {
    case PollCadence::critical: return app::critical_poll_interval_ms;
    case PollCadence::fast: return app::fast_poll_interval_ms;
    case PollCadence::normal: return app::normal_poll_interval_ms;
    case PollCadence::slow: return app::slow_poll_interval_ms;
    default: return 0;
    }
}

InverterOperatingState decode_state(std::uint16_t raw)
{
    switch (raw) {
    case 0: return InverterOperatingState::stopped;
    case 1:
    case 2:
    case 5:
    case 6:
    case 7: return InverterOperatingState::operating;
    case 3: return InverterOperatingState::faulted;
    case 4: return InverterOperatingState::updating;
    default: return InverterOperatingState::unknown;
    }
}

} // namespace

ProductionPoller::ProductionPoller(protocol::ModbusRtuClient &client,
                                   std::uint8_t member_id,
                                   std::uint8_t connected_pv_inputs,
                                   std::uint8_t phase_assignment,
                                   network::RuntimeMessageSink &sink)
    : client_(client), member_id_(member_id),
      connected_pv_inputs_(std::min<std::uint8_t>(connected_pv_inputs, 16)),
      phase_assignment_(phase_assignment), sink_(sink)
{
    reset();
}

void ProductionPoller::reset()
{
    last_poll_ms_.fill(0);
    completed_once_.fill(false);
    batch_disabled_.fill(false);
    next_poll_index_ = 0;
    last_poll_was_critical_ = false;
    power_cursor_ = 0;
    background_turn_ = false;
    state_ = {};
    state_updated_ms_ = 0;
    sequence_ = 0;
}

void ProductionPoller::invalidate_state_after_write()
{
    // Even a timeout can mean that the inverter accepted the write. Never
    // authorize the next guarded command using the pre-write operating state.
    state_ = {};
    state_updated_ms_ = 0;
    last_poll_was_critical_ = false;
    const auto plan = default_poll_plan();
    for (std::size_t index = 0; index < plan.size && index < maximum_requests; ++index) {
        const auto &request = plan.data[index];
        if (request.space == RegisterSpace::input && request.first_register == 0 &&
            request.cadence == PollCadence::critical) {
            last_poll_ms_[index] = 0;
        }
    }
}

InverterStateSnapshot ProductionPoller::state() const
{
    auto result = state_;
    result.fresh = state_updated_ms_ != 0 &&
                   esp_timer_get_time() / 1000 - state_updated_ms_ <=
                       app::inverter_state_freshness_ms;
    return result;
}

void ProductionPoller::run_due(std::uint16_t enabled_features)
{
    const auto plan = default_poll_plan();
    const std::int64_t now_ms = esp_timer_get_time() / 1000;
    if (plan.size == 0 || plan.size > maximum_requests) {
        ESP_LOGE(tag, "Invalid polling plan size: %u", static_cast<unsigned>(plan.size));
        return;
    }
    // Keep state fresh, but reserve alternate transactions for other requests.
    if (!last_poll_was_critical_) {
        for (std::size_t index = 0; index < plan.size; ++index) {
            const auto &request = plan.data[index];
            if (request.cadence == PollCadence::critical &&
                poll_request_is_enabled(request, enabled_features) && due(index, request, now_ms)) {
                last_poll_was_critical_ = true;
                read_request(index, request, now_ms, enabled_features);
                return;
            }
        }
    }
    // Repeat the essential power group, granting one rotating background read
    // per completed pass. Merely reordering the full plan did not refresh it.
    constexpr std::uint16_t power_addresses[] = {
        40, 320, 250, 286, 332, 334, 336, 338, 340, 342, 349, 351
    };
    constexpr auto power_count = sizeof(power_addresses) / sizeof(power_addresses[0]);
    if (!background_turn_) {
        while (power_cursor_ < power_count) {
            const auto address = power_addresses[power_cursor_++];
            for (std::size_t index = 0; index < plan.size; ++index) {
                const auto &request = plan.data[index];
                if (request.space == RegisterSpace::input && request.first_register == address &&
                    poll_request_is_enabled(request, enabled_features) && due(index, request, now_ms)) {
                    last_poll_was_critical_ = false;
                    // Try only adjacent documented power pairs, never reserved gaps.
                    if (!batch_disabled_[index] &&
                        (address == 332 || address == 336 || address == 340 || address == 349)) {
                        auto combined = request;
                        combined.register_count = 4;
                        if (read_request(index, combined, now_ms, enabled_features)) {
                            for (std::size_t other = 0; other < plan.size; ++other) {
                                if (plan.data[other].space == RegisterSpace::input &&
                                    plan.data[other].first_register == address + 2) {
                                    last_poll_ms_[other] = last_poll_ms_[index];
                                    completed_once_[other] = true;
                                }
                            }
                            if (power_cursor_ < power_count && power_addresses[power_cursor_] == address + 2)
                                ++power_cursor_;
                            return;
                        }
                        batch_disabled_[index] = true;
                        // Retry this pair individually on the next scheduler call,
                        // allowing critical status handling between transactions.
                        last_poll_ms_[index] = 0;
                        --power_cursor_;
                        return;
                    }
                    read_request(index, request, now_ms, enabled_features);
                    return;
                }
            }
        }
        power_cursor_ = 0;
        background_turn_ = true;
    }
    background_turn_ = false;
    for (std::size_t offset = 0; offset < plan.size; ++offset) {
        const std::size_t index = (next_poll_index_ + offset) % plan.size;
        const auto &request = plan.data[index];
        const bool power_request = request.space == RegisterSpace::input &&
            std::find(std::begin(power_addresses), std::end(power_addresses), request.first_register) !=
                std::end(power_addresses);
        if (power_request || request.cadence == PollCadence::critical) continue;
        if (poll_request_is_enabled(request, enabled_features) && due(index, request, now_ms)) {
            next_poll_index_ = (index + 1) % plan.size;
            last_poll_was_critical_ = request.cadence == PollCadence::critical;
            read_request(index, request, now_ms, enabled_features);
            return;
        }
    }
}

bool ProductionPoller::due(std::size_t index, const PollRequest &request,
                           std::int64_t now_ms) const
{
    if (request.cadence == PollCadence::on_demand) return false;
    if (request.cadence == PollCadence::boot_once || request.cadence == PollCadence::setup_once) {
        return !completed_once_[index] &&
               (last_poll_ms_[index] == 0 ||
                now_ms - last_poll_ms_[index] >= app::normal_poll_interval_ms);
    }
    return last_poll_ms_[index] == 0 ||
           now_ms - last_poll_ms_[index] >= cadence_interval_ms(request.cadence);
}

bool ProductionPoller::read_request(std::size_t index, const PollRequest &request,
                                    std::int64_t, std::uint16_t enabled_features)
{
    std::array<std::uint16_t, 125> values{};
    std::uint16_t register_count = request.register_count;
    if (request.space == RegisterSpace::input && request.first_register == 40) {
        register_count = static_cast<std::uint16_t>(
            24U + 2U * std::min<std::uint8_t>(connected_pv_inputs_, 2));
    } else if (request.space == RegisterSpace::input && request.first_register == 68) {
        register_count = connected_pv_inputs_ > 2
                             ? static_cast<std::uint16_t>(2U * (connected_pv_inputs_ - 2U))
                             : 0;
    } else if (request.space == RegisterSpace::input && request.first_register == 252) {
        register_count = static_cast<std::uint16_t>(2U * connected_pv_inputs_);
    }
    if (register_count == 0 || register_count > values.size()) return false;
    const bool success = request.space == RegisterSpace::holding
                             ? client_.read_holding(request.first_register, register_count,
                                                    values.data(), values.size())
                             : client_.read_input(request.first_register, register_count,
                                                  values.data(), values.size());
    const std::int64_t completed_at_ms = esp_timer_get_time() / 1000;
    last_poll_ms_[index] = completed_at_ms;
    if (!success) {
        ESP_LOGW(tag, "%s poll failed (%u:%u+%u)", request.name,
                 static_cast<unsigned>(request.space), request.first_register,
                 register_count);
        return false;
    }
    completed_once_[index] = true;
    update_state(request, values.data(), register_count);
    emit(request, values.data(), register_count, completed_at_ms, enabled_features);
    return true;
}

void ProductionPoller::emit(const PollRequest &request, const std::uint16_t *values,
                            std::uint16_t count, std::int64_t timestamp_ms,
                            std::uint16_t enabled_features)
{
    std::uint16_t offset = 0;
    while (offset < count) {
        network::TelemetryMessage message{};
        message.source_member_id = member_id_;
        message.register_space = request.space == RegisterSpace::holding ? 0 : 1;
        message.connected_pv_inputs = connected_pv_inputs_;
        message.phase_assignment = phase_assignment_;
        message.enabled_features = enabled_features;
        message.first_register = request.first_register + offset;
        message.register_count = std::min<std::uint16_t>(
            count - offset, static_cast<std::uint16_t>(message.registers.size()));
        if (offset + message.register_count < count && message.register_count > 1) {
            const auto *last = find_register(request.space,
                                             message.first_register + message.register_count - 1);
            if (last != nullptr && descriptor_starts_multiword_value(*last)) {
                --message.register_count;
            }
        }
        message.sequence = ++sequence_;
        message.timestamp_ms = timestamp_ms;
        std::copy_n(values + offset, message.register_count, message.registers.begin());
        sink_.on_telemetry(message);
        offset += message.register_count;
    }
}

void ProductionPoller::update_state(const PollRequest &request,
                                    const std::uint16_t *values,
                                    std::uint16_t count)
{
    if (request.space != RegisterSpace::input || request.first_register != 0 || count == 0) return;
    state_.raw_status = values[0];
    state_.state = decode_state(state_.raw_status);
    state_.fresh = true;
    state_updated_ms_ = esp_timer_get_time() / 1000;
}

} // namespace inverter_gateway::inverter
