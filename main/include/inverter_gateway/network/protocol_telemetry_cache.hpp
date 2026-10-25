#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "inverter_gateway/network/runtime_messages.hpp"

namespace inverter_gateway::network {

// USB/radio/cable reports remain small. MQTT only sees a complete observation,
// so losing one MQTT message cannot leave fifteen independently sent fragments.
class ProtocolTelemetryCache {
public:
    // Keep every fixed background query, including once-only identity. The
    // largest supported PI variant schedules 28 queries in a device session.
    static constexpr std::size_t maximum_records = 32;
    struct Record {
        protocol::EnergyResponse response{};
        char command[16]{};
        std::uint32_t sequence = 0;
        std::int64_t timestamp_ms = 0;
        std::uint32_t received_ms = 0;
        std::uint32_t published_ms = 0;
        std::uint32_t published_generation = 0;
        std::uint64_t published_hash = 0;
        bool published = false;
        void clear()
        {
            response.count = 0; command[0] = '\0'; sequence = received_ms = published_ms = 0;
            timestamp_ms = 0; published_hash = 0; published_generation = 0; published = false;
        }
    };

    const std::array<Record, maximum_records> &records() const { return records_; }
    std::array<Record, maximum_records> &records() { return records_; }
    protocol::EnergyProtocol profile() const { return profile_; }
    std::uint32_t latest_sequence() const { return latest_sequence_; }
    std::int64_t latest_timestamp() const { return latest_timestamp_; }
    std::uint32_t last_live_publish_ms = 0;
    bool live_published = false;
    bool live_attempted = false;
    std::uint32_t last_live_attempt_ms = 0;
    std::uint32_t reconnect_generation = 0;
    unsigned rejected_records = 0;

    Record *receive(const ProtocolTelemetryMessage &message, std::uint32_t now)
    {
        if (message.chunk_count == 0 || message.chunk_count > 48 ||
            message.chunk_index >= message.chunk_count || message.field_count == 0 ||
            message.field_count > 2 || (!message.final_chunk && message.field_count != 2) ||
            message.final_chunk != (message.chunk_index + 1 == message.chunk_count)) {
            ++rejected_records; return nullptr;
        }
        if (profile_ != message.protocol) reset(message.protocol);
        auto &assembly = assembly_;
        const bool same = assembly.active && assembly.sequence == message.sequence &&
            std::strcmp(assembly.command, message.command) == 0;
        const bool possible_reboot = have_latest_ && latest_timestamp_ > message.timestamp_ms &&
            message.timestamp_ms <= 120000 && message.sequence < 256 &&
            latest_timestamp_ - message.timestamp_ms >= 5000;
        if (assembly.active && !same && now - assembly.started_ms < 10000 &&
            !newer(message.sequence, assembly.sequence) && !possible_reboot) {
            ++rejected_records; return nullptr;
        }
        if (!same || now - assembly.started_ms >= 10000) {
            if (assembly.active && !assembly.invalid) ++rejected_records;
            assembly.clear();
            assembly.active = true;
            assembly.sequence = message.sequence;
            assembly.timestamp_ms = message.timestamp_ms;
            assembly.started_ms = now;
            assembly.chunks = message.chunk_count;
            std::memcpy(assembly.command, message.command, sizeof(assembly.command));
        }
        if (assembly.invalid || assembly.timestamp_ms != message.timestamp_ms ||
            assembly.chunks != message.chunk_count) {
            if (!assembly.invalid) ++rejected_records;
            assembly.invalid = true; return nullptr;
        }
        const auto bit = std::uint64_t{1} << message.chunk_index;
        const auto offset = static_cast<std::size_t>(message.chunk_index) * 2;
        if (assembly.seen & bit) {
            if (assembly.counts[message.chunk_index] != message.field_count) assembly.invalid = true;
            for (unsigned i = 0; i < message.field_count && !assembly.invalid; ++i)
                if (!equal_field(assembly.fields[offset + i], message.fields[i])) assembly.invalid = true;
            if (assembly.invalid) ++rejected_records;
            return nullptr;
        }
        for (unsigned i = 0; i < message.field_count; ++i) {
            for (unsigned chunk = 0; chunk < assembly.chunks; ++chunk) {
                if (!(assembly.seen & (std::uint64_t{1} << chunk))) continue;
                for (unsigned j = 0; j < assembly.counts[chunk]; ++j)
                    if (std::strcmp(message.fields[i].key, assembly.fields[chunk * 2 + j].key) == 0) {
                        assembly.invalid = true; ++rejected_records; return nullptr;
                    }
            }
            if (i && std::strcmp(message.fields[0].key, message.fields[i].key) == 0) {
                assembly.invalid = true; ++rejected_records; return nullptr;
            }
            assembly.fields[offset + i] = message.fields[i];
        }
        assembly.counts[message.chunk_index] = message.field_count;
        assembly.seen |= bit;
        if (assembly.seen != ((std::uint64_t{1} << assembly.chunks) - 1)) return nullptr;
        // One delayed old observation is insufficient evidence of a reboot.
        // Require two increasing low-uptime observations whose timing agrees
        // with arrival timing, retaining the first once-only identity if any.
        if (possible_reboot) {
            const auto elapsed = now - reboot_record_.received_ms;
            const auto sequence_delta = message.sequence - reboot_record_.sequence;
            const auto uptime_delta = message.timestamp_ms - reboot_record_.timestamp_ms;
            const bool confirmed = reboot_record_.response.count && elapsed >= 50 && elapsed <= 10000 &&
                sequence_delta > 0 && sequence_delta <= 4 && uptime_delta >= 50 &&
                uptime_delta <= static_cast<std::int64_t>(elapsed) + 2000;
            if (!confirmed) {
                if (!reboot_record_.response.count || elapsed > 10000)
                    copy_assembly(reboot_record_);
                assembly.active = false;
                ++rejected_records;
                return nullptr;
            }
            clear_records(message.protocol);
            records_[0] = reboot_record_;
            reboot_record_.clear();
        } else if (have_latest_ && message.timestamp_ms >= latest_timestamp_)
            reboot_record_.clear();
        Record *record = nullptr;
        for (auto &candidate : records_)
            if (std::strcmp(candidate.command, message.command) == 0) { record = &candidate; break; }
        if (record && record->response.count &&
            (message.timestamp_ms < record->timestamp_ms || !newer(message.sequence, record->sequence))) {
            assembly.active = false; ++rejected_records; return nullptr;
        }
        if (!record) {
            for (auto &candidate : records_) if (!candidate.response.count) { record = &candidate; break; }
        }
        if (!record) {
            record = &records_[0];
            for (auto &candidate : records_)
                if (now - candidate.received_ms > now - record->received_ms) record = &candidate;
            record->clear();
        }
        copy_assembly(*record);
        if (!have_latest_ || newer(message.sequence, latest_sequence_)) {
            latest_sequence_ = message.sequence;
            latest_timestamp_ = message.timestamp_ms;
            have_latest_ = true;
        }
        assembly.active = false;
        return record;
    }

    static std::uint64_t fingerprint(const Record &record)
    {
        std::uint64_t value = 14695981039346656037ULL;
        auto bytes = [&](const void *data, std::size_t size) {
            const auto *p = static_cast<const unsigned char *>(data);
            while (size--) { value ^= *p++; value *= 1099511628211ULL; }
        };
        bytes(&record.response.count, sizeof(record.response.count));
        for (std::size_t i = 0; i < record.response.count; ++i) {
            const auto &field = record.response.fields[i];
            bytes(field.key, std::strlen(field.key) + 1);
            bytes(field.unit, std::strlen(field.unit) + 1);
            bytes(&field.numeric, sizeof(field.numeric));
            if (field.numeric) bytes(&field.value, sizeof(field.value));
            else bytes(field.text, std::strlen(field.text) + 1);
        }
        return value;
    }

private:
    static bool newer(std::uint32_t candidate, std::uint32_t previous)
    { const auto delta = candidate - previous; return delta != 0 && delta < 0x80000000U; }
    static bool equal_field(const protocol::EnergyField &a, const protocol::EnergyField &b)
    {
        return a.numeric == b.numeric && std::strcmp(a.key, b.key) == 0 &&
            std::strcmp(a.unit, b.unit) == 0 &&
            (a.numeric ? a.value == b.value : std::strcmp(a.text, b.text) == 0);
    }
    void reset(protocol::EnergyProtocol profile)
    {
        clear_records(profile);
        assembly_.clear();
        reboot_record_.clear();
    }
    void clear_records(protocol::EnergyProtocol profile)
    {
        for (auto &record : records_) record.clear();
        profile_ = profile;
        latest_sequence_ = 0;
        latest_timestamp_ = 0;
        have_latest_ = false;
        live_published = false;
        live_attempted = false;
    }
    void copy_assembly(Record &record)
    {
        std::memcpy(record.command, assembly_.command, sizeof(record.command));
        record.response.count = (assembly_.chunks - 1U) * 2U + assembly_.counts[assembly_.chunks - 1U];
        for (std::size_t i = 0; i < record.response.count; ++i) record.response.fields[i] = assembly_.fields[i];
        record.sequence = assembly_.sequence;
        record.timestamp_ms = assembly_.timestamp_ms;
        record.received_ms = assembly_.started_ms;
    }
    struct Assembly {
        protocol::EnergyField fields[96]{};
        char command[16]{};
        std::int64_t timestamp_ms = 0;
        std::uint64_t seen = 0;
        std::uint32_t sequence = 0, started_ms = 0;
        std::uint8_t counts[48]{}, chunks = 0;
        bool active = false, invalid = false;
        void clear()
        {
            command[0] = '\0'; timestamp_ms = 0; seen = 0; sequence = started_ms = 0;
            std::memset(counts, 0, sizeof(counts)); chunks = 0; active = invalid = false;
        }
    } assembly_{};
    std::array<Record, maximum_records> records_{};
    Record reboot_record_{};
    protocol::EnergyProtocol profile_ = protocol::EnergyProtocol::count;
    std::uint32_t latest_sequence_ = 0;
    std::int64_t latest_timestamp_ = 0;
    bool have_latest_ = false;
};

} // namespace inverter_gateway::network
