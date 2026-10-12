#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "inverter_gateway/network/runtime_messages.hpp"

namespace inverter_gateway::network {

// Bus-owner replay defense. The caller holds the same external admission lock
// for lookup/reserve/complete; this helper has no lock, clock or dynamic memory.
// Retention is bounded: the oldest completed entry can be evicted, pending
// commands cannot. Call reserve only after validating a local native command.
class ProtocolCommandHistory {
public:
    static constexpr std::size_t capacity = 16;

    bool writes_blocked() const { return writes_blocked_; }

    // A timed-out write can leave a late ACK in the same physical session.
    // Only a new transport session permits another write; successful reads do
    // not establish whether that earlier command was applied. Keep cached IDs.
    void new_connection_session() { writes_blocked_ = false; }

    bool lookup(const ProtocolCommand &request, ProtocolCommandResult &result) const
    {
        const auto *entry = find(request.command_id);
        if (entry == nullptr) return false;
        if (!same_request(entry->request, request)) {
            result = {};
            result.command_id = request.command_id;
            result.source_member_id = request.target_member_id;
            result.protocol = request.protocol;
            result.outcome = ProtocolCommandOutcome::rejected;
            copy_text(result.command, request.command);
            copy_text(result.detail, "Command ID reused for a different request; no command sent");
        } else if (entry->state == State::completed) {
            result = entry->result;
        } else {
            result = {};
            result.command_id = request.command_id;
            result.source_member_id = request.target_member_id;
            result.protocol = request.protocol;
            result.outcome = request.write ? ProtocolCommandOutcome::uncertain
                                           : ProtocolCommandOutcome::unavailable;
            result.write_sent_known = !request.write;
            copy_text(result.command, request.command);
            copy_text(result.detail, "Command already admitted; await the original result");
        }
        return true;
    }

    bool reserve(const ProtocolCommand &request)
    {
        if (!valid_request(request) || find(request.command_id) != nullptr ||
            (request.write && writes_blocked_)) return false;
        Entry *slot = nullptr;
        for (auto &entry : entries_) {
            if (entry.state == State::unused) { slot = &entry; break; }
            if (entry.state == State::completed &&
                (slot == nullptr || entry.completed_age > slot->completed_age)) slot = &entry;
        }
        if (slot == nullptr) return false; // Every entry is pending.
        *slot = {};
        slot->request = request;
        slot->state = State::pending;
        return true;
    }

    void complete(const ProtocolCommandResult &result)
    {
        if (result.command_id == 0 || !terminated(result.command) || !terminated(result.detail) ||
            result.outcome > ProtocolCommandOutcome::invalid_response) return;
        auto *entry = find(result.command_id);
        if (entry == nullptr || entry->state != State::pending ||
            result.protocol != entry->request.protocol ||
            result.source_member_id != entry->request.target_member_id ||
            std::strcmp(result.command, entry->request.command) != 0) return;
        // Ages are ranks within at most 16 completed entries, avoiding clock
        // wrap and indefinitely increasing counters. Higher means older.
        for (auto &saved : entries_) {
            if (saved.state == State::completed && saved.completed_age < capacity - 1)
                ++saved.completed_age;
        }
        entry->result = result;
        entry->completed_age = 0;
        entry->state = State::completed;
        if (entry->request.write && result.outcome == ProtocolCommandOutcome::uncertain &&
            result.write_sent_known && result.write_sent) writes_blocked_ = true;
    }

private:
    enum class State : std::uint8_t { unused, pending, completed };
    struct Entry {
        ProtocolCommand request{};
        ProtocolCommandResult result{};
        State state = State::unused;
        std::uint8_t completed_age = 0;
    };
    std::array<Entry, capacity> entries_{};
    bool writes_blocked_ = false;

    template<std::size_t Size>
    static bool terminated(const char (&text)[Size])
    {
        return std::memchr(text, 0, Size) != nullptr;
    }

    template<std::size_t Destination, std::size_t Source>
    static void copy_text(char (&destination)[Destination], const char (&source)[Source])
    {
        std::size_t length = 0;
        while (length + 1 < Destination && length < Source && source[length] != '\0') ++length;
        std::memcpy(destination, source, length);
        destination[length] = '\0';
    }

    static bool valid_request(const ProtocolCommand &request)
    {
        return request.command_id != 0 && request.origin == RemoteCommandOrigin::mqtt &&
            request.protocol < protocol::EnergyProtocol::count && terminated(request.command) &&
            request.command[0] != '\0' && (!request.write || request.confirmed);
    }

    static bool same_request(const ProtocolCommand &left, const ProtocolCommand &right)
    {
        return valid_request(right) && left.command_id == right.command_id &&
            left.target_member_id == right.target_member_id && left.origin == right.origin &&
            left.protocol == right.protocol && left.write == right.write &&
            left.confirmed == right.confirmed && std::strcmp(left.command, right.command) == 0;
    }

    Entry *find(std::uint64_t id)
    {
        if (id == 0) return nullptr;
        for (auto &entry : entries_)
            if (entry.state != State::unused && entry.request.command_id == id) return &entry;
        return nullptr;
    }
    const Entry *find(std::uint64_t id) const
    {
        if (id == 0) return nullptr;
        for (const auto &entry : entries_)
            if (entry.state != State::unused && entry.request.command_id == id) return &entry;
        return nullptr;
    }
};

} // namespace inverter_gateway::network
