#pragma once

#include "inverter_gateway/protocol/energy_protocol.hpp"

#include <cstring>

namespace inverter_gateway::protocol {

// One scheduler belongs to one transport owner. Fixed storage and an explicit
// completion prevent duplicate polls; it never retries settings commands.
class EnergyPollScheduler {
public:
    static constexpr std::size_t maximum_commands = 32;

    void reset(EnergyProtocol protocol, std::uint64_t session,
               std::uint64_t now_ms = 0)
    {
        protocol_ = protocol;
        session_ = session;
        count_ = 0;
        active_ = maximum_commands;
        live_burst_ = 0;
        for (std::size_t i = 0; i < maximum_commands; ++i) entries_[i] = {};
        for (std::size_t i = 0; i < energy_background_poll_count(protocol); ++i) {
            EnergyPollInfo poll{};
            if (!energy_background_poll_command(protocol, i, poll)) continue;
            if (count_ == maximum_commands) break;
            entries_[count_].poll = poll;
            entries_[count_].read_context_ready =
                (protocol != EnergyProtocol::daly && protocol != EnergyProtocol::daly40) ||
                (std::strcmp(poll.command, "cellVoltages") && std::strcmp(poll.command, "cellTemperatures"));
            entries_[count_++].due_ms = now_ms;
        }
    }

    std::size_t count() const { return count_; }
    std::uint64_t session() const { return session_; }

    // Returns nullptr when no poll is due, or while the caller owns an active
    // poll. Calling record() completes that poll and sets its next due time.
    const EnergyPollInfo *next(std::uint64_t now_ms)
    {
        if (active_ != maximum_commands) return nullptr;
        // JK notifications are not valid until the per-session information
        // request succeeds. A transient prerequisite failure backs off.
        for (std::size_t i = 0; i < count_; ++i) {
            const auto &entry = entries_[i];
            if (entry.poll.prerequisite && !entry.completed) {
                if (entry.due_ms > now_ms) return nullptr;
                active_ = i;
                return &entries_[i].poll;
            }
        }

        std::size_t live = maximum_commands, background = maximum_commands;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto &entry = entries_[i];
            if (!entry.read_context_ready || entry.disabled || entry.completed || entry.due_ms > now_ms) continue;
            auto &candidate = entry.poll.primary ? live : background;
            const bool urgent = entry.poll.primary;
            const bool candidate_urgent = candidate != maximum_commands &&
                entries_[candidate].poll.primary;
            if (candidate == maximum_commands || (urgent && !candidate_urgent) ||
                (urgent == candidate_urgent &&
                 (entry.due_ms < entries_[candidate].due_ms ||
                  (entry.due_ms == entries_[candidate].due_ms &&
                   entry.poll.primary && !entries_[candidate].poll.primary)))) candidate = i;
        }
        // Slow or failed core transactions leave a bounded slot for secondary
        // measurements, status, identity and configuration. Two due core
        // requests may run before one oldest due supplemental request.
        active_ = background != maximum_commands && (live == maximum_commands || live_burst_ >= 2)
            ? background : live;
        if (active_ == maximum_commands) return nullptr;
        return &entries_[active_].poll;
    }

    void record(bool success, std::uint64_t now_ms,
                CodecStatus failure = CodecStatus::invalid_frame)
    {
        if (active_ == maximum_commands) return;
        const auto index = active_;
        active_ = maximum_commands;
        finish(entries_[index], success, now_ms, failure);
        if (entries_[index].poll.primary) {
            if (live_burst_ < 2) ++live_burst_;
        } else live_burst_ = 0;
    }

    // A user query/readback can satisfy the scheduled query without issuing
    // an immediate duplicate. Only background allowlisted reads are tracked.
    void observe(const char *command, bool success, std::uint64_t now_ms,
                 CodecStatus failure = CodecStatus::invalid_frame)
    {
        if (!command) return;
        for (std::size_t i = 0; i < count_; ++i) {
            if (std::strcmp(command, entries_[i].poll.command) == 0) {
                finish(entries_[i], success, now_ms, failure);
                return;
            }
        }
    }

    // Daly's frame counts come from status, not installer input. A context
    // transition makes a newly valid read due without inventing an observation
    // or repeatedly reenabling an unsupported query on every status response.
    void set_read_context_ready(const char *command, bool ready, std::uint64_t now_ms)
    {
        if (!command) return;
        for (std::size_t i = 0; i < count_; ++i) {
            auto &entry = entries_[i];
            if (std::strcmp(command, entry.poll.command) || entry.read_context_ready == ready) continue;
            entry.read_context_ready = ready;
            if (ready) {
                entry.disabled = false;
                entry.completed = false;
                entry.failures = 0;
                entry.due_ms = now_ms;
            }
            return;
        }
    }

    void refresh_configuration(std::uint64_t now_ms)
    {
        for (std::size_t i = 0; i < count_; ++i) {
            auto &entry = entries_[i];
            if (entry.poll.poll_class == EnergyPollClass::configuration && !entry.disabled)
                entry.due_ms = now_ms;
        }
    }

    void refresh_live(std::uint64_t now_ms)
    {
        for (std::size_t i = 0; i < count_; ++i)
            if (is_live(entries_[i].poll) && !entries_[i].disabled)
                entries_[i].due_ms = now_ms;
        live_burst_ = 0;
    }

private:
    struct Entry {
        EnergyPollInfo poll{};
        std::uint64_t due_ms = 0;
        std::uint8_t failures = 0;
        bool completed = false;
        bool disabled = false;
        bool read_context_ready = true;
    };
    static bool is_live(const EnergyPollInfo &poll)
    {
        return poll.poll_class == EnergyPollClass::live || poll.poll_class == EnergyPollClass::status;
    }
    static void finish(Entry &entry, bool success, std::uint64_t now_ms,
                       CodecStatus failure)
    {
        if (success) {
            entry.failures = 0;
            entry.disabled = false;
            entry.completed = entry.poll.poll_class == EnergyPollClass::identity;
            entry.due_ms = now_ms + entry.poll.interval_ms;
            return;
        }
        if (entry.failures < 255) ++entry.failures;
        entry.completed = false;
        // Core telemetry and required JK setup keep retrying, with a cap.
        // Optional rejected queries stop immediately; other optional queries
        // get three attempts before sleeping until the next transport session.
        const bool required = entry.poll.primary || entry.poll.prerequisite;
        if (!required && (failure == CodecStatus::unsupported ||
            failure == CodecStatus::invalid_command || failure == CodecStatus::rejected ||
            entry.failures >= 3)) {
            entry.disabled = true;
            return;
        }
        const std::uint32_t delay_ms = required
            ? (entry.failures == 1 ? 2000 : entry.failures == 2 ? 5000 : 10000)
            : (entry.failures == 1 ? 10000 : 60000);
        entry.due_ms = now_ms + delay_ms;
    }

    Entry entries_[maximum_commands]{};
    EnergyProtocol protocol_ = EnergyProtocol::aohai_modbus;
    std::uint64_t session_ = 0;
    std::size_t count_ = 0;
    std::size_t active_ = maximum_commands;
    std::uint8_t live_burst_ = 0;
};

} // namespace inverter_gateway::protocol
