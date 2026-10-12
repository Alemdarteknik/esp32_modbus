// Exercises verbatim production MQTT parser/event guards/serializers against
// real cJSON and firmware codecs. Queues and the client are in-memory boundaries.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define private public
#include "inverter_gateway/network/mqtt_bridge.hpp"
#undef private
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/network/native_telemetry.hpp"
#include "inverter_gateway/protocol/bms_protocols.hpp"
#include "bms_protocol_vectors.generated.hpp"
#include "inverter_gateway/app/memory_budget.hpp"
#include "inverter_gateway/app/rgb_debug.hpp"
#include "inverter_gateway/inverter/register_value.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace host {
struct Queue {
    std::size_t capacity{}, item_size{};
    std::deque<std::vector<unsigned char>> messages;
};
struct Publication {
    std::string topic, payload;
    int qos{}, retain{};
};
struct Client {
    int outbox_bytes{};
    int enqueue_result{1};
    std::vector<Publication> published;
};
std::int64_t now_us = 100000000;
unsigned debug_errors{};
unsigned checks{};
bool memory_admission_allowed = true;
unsigned memory_guards_active{}, memory_guard_attempts{};
void check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        std::printf("MQTT check failed at line %d: %s\n", line, expression);
        std::fflush(stdout);
        std::exit(1);
    }
}
}
#define CHECK(expression) host::check((expression), #expression, __LINE__)

QueueHandle_t xQueueCreate(std::size_t capacity, std::size_t size)
{
    return new host::Queue{capacity, size, {}};
}
void vQueueDelete(QueueHandle_t queue) { delete static_cast<host::Queue *>(queue); }
BaseType_t xQueueSend(QueueHandle_t handle, const void *value, TickType_t)
{
    if (!handle) return 0;
    auto &queue = *static_cast<host::Queue *>(handle);
    if (queue.messages.size() >= queue.capacity) return 0;
    const auto *begin = static_cast<const unsigned char *>(value);
    queue.messages.emplace_back(begin, begin + queue.item_size);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *value, TickType_t)
{
    if (!handle) return 0;
    auto &queue = *static_cast<host::Queue *>(handle);
    if (queue.messages.empty()) return 0;
    std::memcpy(value, queue.messages.front().data(), queue.item_size);
    queue.messages.pop_front();
    return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t queue)
{
    return queue ? static_cast<unsigned>(static_cast<host::Queue *>(queue)->messages.size()) : 0;
}
std::int64_t esp_timer_get_time() { return host::now_us; }
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int) { return 1; }
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *, const char *, int, int, int) { return 1; }
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t client)
{
    return static_cast<host::Client *>(client)->outbox_bytes;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t handle, const char *topic, const char *payload,
                            int, int qos, int retain, bool)
{
    auto &client = *static_cast<host::Client *>(handle);
    if (client.enqueue_result < 0) return client.enqueue_result;
    client.published.push_back({topic, payload, qos, retain});
    client.outbox_bytes += static_cast<int>(std::strlen(topic) + std::strlen(payload) + 16);
    return client.enqueue_result;
}
namespace inverter_gateway::app {
void rgb_debug_publish_error() { ++host::debug_errors; }
MemoryWorkGuard::MemoryWorkGuard(TickType_t)
{
    ++host::memory_guard_attempts;
    acquired_ = host::memory_admission_allowed && host::memory_guards_active == 0;
    if (acquired_) ++host::memory_guards_active;
}
MemoryWorkGuard::~MemoryWorkGuard() { release(); }
void MemoryWorkGuard::release()
{
    if (!acquired_) return;
    --host::memory_guards_active;
    acquired_ = false;
}
}

#include "mqtt_native_production.generated.inc"

using namespace inverter_gateway;
namespace {
class Sink final : public network::RuntimeMessageSink {
public:
    std::vector<network::ProtocolCommand> native;
    std::vector<network::RoutedCommand> legacy;
    void on_telemetry(const network::TelemetryMessage &) override {}
    void on_command(const network::RoutedCommand &command) override { legacy.push_back(command); }
    void on_command_result(const network::RoutedCommandResult &) override {}
    void on_protocol_command(const network::ProtocolCommand &command) override { native.push_back(command); }
};
struct Fixture {
    app::SystemConfig config;
    Sink sink;
    host::Client client;
    network::MqttBridge bridge;
    std::array<char, 32768> json_buffer{};
    Fixture() : bridge(config, sink)
    {
        std::strcpy(config.site_id, "site1");
        std::strcpy(config.mqtt_topic_prefix, "test");
        config.profile.expected_inverter_count = 3;
        bridge.client_ = &client;
        bridge.result_queue_ = xQueueCreate(8, sizeof(network::RoutedCommandResult));
        bridge.protocol_result_queue_ = xQueueCreate(8, sizeof(network::ProtocolCommandResult));
        bridge.protocol_publish_queue_ = xQueueCreate(32, sizeof(network::ProtocolTelemetryMessage));
        bridge.live_json_buffer_ = json_buffer.data();
        bridge.live_json_capacity_ = json_buffer.size();
    }
    ~Fixture()
    {
        vQueueDelete(bridge.result_queue_);
        vQueueDelete(bridge.protocol_result_queue_);
        vQueueDelete(bridge.protocol_publish_queue_);
    }
    void send(const std::string &json, const std::string &topic = "test/site1/inverter/1/command")
    {
        bridge.handle_command(topic.c_str(), static_cast<int>(topic.size()),
                              json.c_str(), static_cast<int>(json.size()));
    }
    void event(const std::string &json, bool retained = false, int offset = 0, int total = -1)
    {
        std::string topic = "test/site1/inverter/1/command";
        esp_mqtt_event_t data{};
        data.event_id = MQTT_EVENT_DATA;
        data.retain = retained;
        data.topic = topic.data();
        data.topic_len = static_cast<int>(topic.size());
        data.data = const_cast<char *>(json.data());
        data.data_len = static_cast<int>(json.size());
        data.current_data_offset = offset;
        data.total_data_len = total < 0 ? data.data_len : total;
        bridge.handle_event(&data);
    }
    network::ProtocolCommandResult result()
    {
        network::ProtocolCommandResult value{};
        CHECK(xQueueReceive(bridge.protocol_result_queue_, &value, 0) == pdTRUE);
        return value;
    }
    void flush_telemetry()
    {
        network::ProtocolTelemetryMessage value{};
        CHECK(xQueueReceive(bridge.protocol_publish_queue_, &value, 0) == pdTRUE);
        bridge.publish_protocol_telemetry_now(value);
    }
};
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json parsed(const host::Publication &message)
{
    Json json(cJSON_Parse(message.payload.c_str()), cJSON_Delete);
    CHECK(json != nullptr);
    return json;
}
const cJSON *item(const cJSON *json, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    CHECK(value != nullptr);
    return value;
}
std::string command(std::uint64_t id, const char *profile, const char *text, const char *extra = "")
{
    return "{\"command_id\":\"" + std::to_string(id) + "\",\"operation\":\"protocol\",\"protocol\":\"" +
        profile + "\",\"command\":\"" + text + "\"" + extra + "}";
}

protocol::EnergyResponse pi30_reference_reading()
{
    // The real QPIGS reference response from pi_reference_vectors.generated.json,
    // sourced from upstream revision eafdd4328c77f516cf82430c5bcb6fa042c45d3a.
    constexpr const char *hex = "283030302e302030302e30203233302e302034392e392030313631203031313920303033203436302035372e3530203031322031303020303036392030303134203130332e382035372e343520303030303020303031313031313020303020303020303038353620303130248c0d";
    std::vector<std::uint8_t> frame;
    for (const char *p = hex; *p; p += 2) {
        char octet[]{p[0], p[1], 0};
        frame.push_back(static_cast<std::uint8_t>(std::strtoul(octet, nullptr, 16)));
    }
    protocol::EnergyResponse response;
    CHECK(protocol::energy_decode_response(protocol::EnergyProtocol::pi30, "QPIGS", 1,
        frame.data(), frame.size(), response) == protocol::CodecStatus::ok);
    CHECK(response.count == 30);
    return response;
}

std::vector<network::ProtocolTelemetryMessage> chunks(const protocol::EnergyResponse &response,
    const char *query = "QPIGS", std::uint32_t sequence = 100, std::int64_t timestamp = 900000)
{
    std::vector<network::ProtocolTelemetryMessage> result;
    for (std::size_t offset = 0; offset < response.count; offset += 2) {
        network::ProtocolTelemetryMessage message{};
        message.source_member_id = 2;
        message.protocol = protocol::EnergyProtocol::pi30;
        message.timestamp_ms = timestamp;
        message.sequence = sequence;
        message.chunk_index = static_cast<std::uint8_t>(offset / 2);
        message.chunk_count = static_cast<std::uint8_t>((response.count + 1) / 2);
        message.field_count = static_cast<std::uint8_t>(std::min(std::size_t{2}, response.count - offset));
        message.final_chunk = offset + message.field_count == response.count;
        std::snprintf(message.command, sizeof(message.command), "%s", query);
        for (std::size_t index = 0; index < message.field_count; ++index)
            message.fields[index] = response.fields[offset + index];
        result.push_back(message);
    }
    return result;
}

std::size_t cached_records(const network::ProtocolTelemetryCache &cache)
{
    return std::count_if(cache.records().begin(), cache.records().end(),
        [](const auto &record) { return record.response.count != 0; });
}

network::ProtocolTelemetryCache::Record *complete(network::ProtocolTelemetryCache &cache,
    const std::vector<network::ProtocolTelemetryMessage> &messages, std::uint32_t now = 1000)
{
    network::ProtocolTelemetryCache::Record *record = nullptr;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        record = cache.receive(messages[index], now + static_cast<std::uint32_t>(index));
        CHECK((record != nullptr) == (index + 1 == messages.size()));
    }
    return record;
}

void atomic_record_cache()
{
    const auto response = pi30_reference_reading();
    const auto messages = chunks(response);
    CHECK(messages.size() == 15);
    network::ProtocolTelemetryCache cache;
    std::uint32_t arrival = 1000;
    for (std::size_t index = messages.size() - 1; index > 0; --index) {
        CHECK(cache.receive(messages[index], arrival++) == nullptr);
        CHECK(cache.receive(messages[index], arrival++) == nullptr);
        CHECK(cached_records(cache) == 0); // Even a final chunk exposes no partial values.
    }
    auto *record = cache.receive(messages[0], 1200);
    CHECK(record != nullptr && record->response.count == 30);
    CHECK(cached_records(cache) == 1);
    for (std::size_t index = 0; index < response.count; ++index) {
        CHECK(std::strcmp(record->response.fields[index].key, response.fields[index].key) == 0);
        CHECK(record->response.fields[index].numeric == response.fields[index].numeric);
        CHECK(record->response.fields[index].value == response.fields[index].value);
    }
    const auto original_hash = network::ProtocolTelemetryCache::fingerprint(*record);
    record = complete(cache, chunks(response, "QPIGS", 101, 902000), 2000);
    CHECK(network::ProtocolTelemetryCache::fingerprint(*record) == original_hash);
    auto changed = response;
    changed.fields[0].value = 231.7;
    record = complete(cache, chunks(changed, "QPIGS", 102, 904000), 3000);
    CHECK(network::ProtocolTelemetryCache::fingerprint(*record) != original_hash);
    // A valid physical zero survives decoding, assembly and fingerprinting.
    CHECK(response.fields[0].numeric && response.fields[0].value == 0.0);

    const auto partial = chunks(response, "QPIGS", 103, 906000);
    CHECK(cache.receive(partial[0], 3100) == nullptr);
    CHECK(cache.records()[0].sequence == 102 && cache.records()[0].response.fields[0].value == 231.7);
    CHECK(cache.latest_sequence() == 102); // A partial update cannot refresh the old snapshot.

    for (const auto &message : chunks(response, "QPIGS", 101, 902000))
        CHECK(cache.receive(message, 4000) == nullptr);
    CHECK(cache.records()[0].sequence == 102 && cache.records()[0].response.fields[0].value == 231.7);
    for (const auto &message : chunks(response, "QPIGS", 102, 906000))
        CHECK(cache.receive(message, 4500) == nullptr);
    CHECK(cache.records()[0].timestamp_ms == 904000);

    network::ProtocolTelemetryCache conflict;
    CHECK(conflict.receive(messages[0], 1000) == nullptr);
    auto duplicate = messages[0]; duplicate.fields[0].value = 9;
    CHECK(conflict.receive(duplicate, 1001) == nullptr);
    for (std::size_t index = 1; index < messages.size(); ++index)
        CHECK(conflict.receive(messages[index], 1002) == nullptr);
    CHECK(cached_records(conflict) == 0 && conflict.rejected_records > 0);
    CHECK(complete(conflict, chunks(response, "QPIGS", 101, 902000)) != nullptr);

    network::ProtocolTelemetryCache names;
    CHECK(names.receive(messages[0], 1000) == nullptr);
    auto repeated_key = messages[1];
    std::strcpy(repeated_key.fields[0].key, messages[0].fields[0].key);
    CHECK(names.receive(repeated_key, 1001) == nullptr);
    for (std::size_t index = 2; index < messages.size(); ++index)
        CHECK(names.receive(messages[index], 1002) == nullptr);
    CHECK(cached_records(names) == 0);

    for (bool alter_timestamp : {true, false}) {
        network::ProtocolTelemetryCache metadata;
        CHECK(metadata.receive(messages[0], 1000) == nullptr);
        auto incompatible = messages[1];
        if (alter_timestamp) ++incompatible.timestamp_ms;
        else ++incompatible.chunk_count;
        CHECK(metadata.receive(incompatible, 1001) == nullptr);
        for (std::size_t index = 1; index < messages.size(); ++index)
            CHECK(metadata.receive(messages[index], 1002) == nullptr);
        CHECK(cached_records(metadata) == 0);
    }

    network::ProtocolTelemetryCache interleaved;
    auto fresh = chunks(response, "QPIGS", 101, 902000);
    CHECK(interleaved.receive(fresh[0], 1000) == nullptr);
    CHECK(interleaved.receive(messages[1], 1001) == nullptr); // Delayed older record cannot evict fresh assembly.
    for (std::size_t index = 1; index < fresh.size(); ++index) {
        auto *ready = interleaved.receive(fresh[index], 1002);
        CHECK((ready != nullptr) == (index + 1 == fresh.size()));
    }
    CHECK(interleaved.latest_sequence() == 101);

    network::ProtocolTelemetryCache mixed;
    CHECK(mixed.receive(messages[0], 1000) == nullptr);
    CHECK(mixed.receive(fresh[1], 1001) == nullptr);
    for (std::size_t index = 2; index < fresh.size(); ++index)
        CHECK(mixed.receive(fresh[index], 1002) == nullptr);
    CHECK(cached_records(mixed) == 0);
    CHECK(mixed.receive(fresh[0], 1003) != nullptr);

    network::ProtocolTelemetryCache wrapped;
    CHECK(complete(wrapped, chunks(response, "QPIGS", UINT32_MAX, 900000)) != nullptr);
    CHECK(complete(wrapped, chunks(response, "QPIGS", 0, 902000)) != nullptr);
    CHECK(wrapped.latest_sequence() == 0 && wrapped.latest_timestamp() == 902000);
    protocol::EnergyResponse mode; CHECK(mode.add_text("device_mode", "", "Line"));
    for (const auto &message : chunks(mode, "QMOD", UINT32_MAX, 900000))
        (void)wrapped.receive(message, 2000);
    CHECK(wrapped.latest_sequence() == 0 && wrapped.latest_timestamp() == 902000);
    CHECK(complete(wrapped, chunks(response, "QPIGS", 1, 904000)) != nullptr);

    protocol::EnergyResponse identity; CHECK(identity.add_text("serial_number", "", "92832207100756"));
    CHECK(complete(wrapped, chunks(identity, "QID", 2, 905000)) != nullptr);
    CHECK(cached_records(wrapped) >= 2);
    for (const auto &message : chunks(response, "QPIGS", 1, 30000))
        CHECK(wrapped.receive(message, 3000) == nullptr);
    CHECK(cached_records(wrapped) >= 2 && wrapped.latest_timestamp() == 905000);
    CHECK(complete(wrapped, chunks(response, "QPIGS", 2, 32000), 5000) != nullptr);
    CHECK(cached_records(wrapped) == 1 && wrapped.latest_timestamp() == 32000);

    network::ProtocolTelemetryCache timeout;
    CHECK(timeout.receive(messages[0], 1000) == nullptr);
    for (std::size_t index = 1; index < messages.size(); ++index)
        CHECK(timeout.receive(messages[index], 11001) == nullptr);
    CHECK(cached_records(timeout) == 0);
    CHECK(timeout.receive(messages[0], 11002) != nullptr);

    protocol::EnergyResponse largest;
    for (unsigned index = 0; index < 96; ++index) {
        const auto key = "sample_" + std::to_string(index);
        CHECK(largest.add_number(key.c_str(), "V", index));
    }
    network::ProtocolTelemetryCache capacity;
    CHECK(complete(capacity, chunks(largest)) != nullptr);
    CHECK(capacity.records()[0].response.count == 96);

    const std::vector<std::pair<std::uint8_t, std::uint8_t>> invalid_sizes = {{0, 0}, {49, 0}, {15, 15}};
    for (auto [count, index] : invalid_sizes) {
        auto broken = messages[0]; broken.chunk_count = count; broken.chunk_index = index;
        network::ProtocolTelemetryCache invalid;
        CHECK(invalid.receive(broken, 1000) == nullptr && cached_records(invalid) == 0);
    }
    auto short_chunk = messages[0]; short_chunk.field_count = 1;
    network::ProtocolTelemetryCache invalid;
    CHECK(invalid.receive(short_chunk, 1000) == nullptr && invalid.rejected_records == 1);
}

void cache_profile_capacity_and_clock()
{
    const auto messages = chunks(pi30_reference_reading());
    protocol::EnergyResponse mode; CHECK(mode.add_text("device_mode", "", "Line"));

    network::ProtocolTelemetryCache profile;
    CHECK(complete(profile, messages) != nullptr);
    auto different = chunks(mode, "QMOD", 1, 30000)[0];
    different.protocol = protocol::EnergyProtocol::pi41;
    CHECK(profile.receive(different, 2000) != nullptr);
    CHECK(cached_records(profile) == 1 && profile.profile() == protocol::EnergyProtocol::pi41);
    CHECK(std::strcmp(profile.records()[0].command, "QMOD") == 0);

    network::ProtocolTelemetryCache bounded;
    for (unsigned index = 0; index <= network::ProtocolTelemetryCache::maximum_records; ++index) {
        const auto query = "QX" + std::to_string(index);
        CHECK(complete(bounded, chunks(mode, query.c_str(), 100 + index, 900000 + index), 1000 + index) != nullptr);
    }
    CHECK(cached_records(bounded) == network::ProtocolTelemetryCache::maximum_records);
    CHECK(std::none_of(bounded.records().begin(), bounded.records().end(),
        [](const auto &entry) { return std::strcmp(entry.command, "QX0") == 0; }));

    network::ProtocolTelemetryCache arrival_wrap;
    auto *across_wrap = complete(arrival_wrap, messages, UINT32_MAX - 10);
    CHECK(across_wrap != nullptr && across_wrap->received_ms == UINT32_MAX - 10);
}

void conservative_reboot_probation()
{
    const auto response = pi30_reference_reading();
    network::ProtocolTelemetryCache reordered;
    CHECK(complete(reordered, chunks(response, "QPIGS", 100, 90000), 1000) != nullptr);
    auto newer_response = response; newer_response.fields[0].value = 231.7;
    CHECK(complete(reordered, chunks(newer_response, "QPIGS", 200, 120000), 31000) != nullptr);
    for (const auto &message : chunks(response, "QPIGS", 100, 90000))
        CHECK(reordered.receive(message, 32000) == nullptr);
    CHECK(reordered.latest_sequence() == 200 && reordered.latest_timestamp() == 120000);
    CHECK(reordered.records()[0].response.fields[0].value == 231.7);
    for (const auto &message : chunks(response, "QPIGS", 100, 90000))
        CHECK(reordered.receive(message, 33000) == nullptr);
    CHECK(reordered.latest_sequence() == 200 && reordered.latest_timestamp() == 120000);
    CHECK(reordered.records()[0].response.fields[0].value == 231.7);

    // Preserve the first once-per-session identity query during probation,
    // while withholding it until a second plausible new-session sample.
    network::ProtocolTelemetryCache rebooted;
    protocol::EnergyResponse original_id; CHECK(original_id.add_text("serial_number", "", "old-session"));
    protocol::EnergyResponse new_id; CHECK(new_id.add_text("serial_number", "", "new-session"));
    CHECK(complete(rebooted, chunks(original_id, "QID", 499, 898000), 1000) != nullptr);
    CHECK(complete(rebooted, chunks(response, "QPIGS", 500, 900000), 3000) != nullptr);
    CHECK(rebooted.receive(chunks(new_id, "QID", 1, 30000)[0], 10000) == nullptr);
    CHECK(cached_records(rebooted) == 2 && rebooted.latest_sequence() == 500);
    CHECK(std::strcmp(rebooted.records()[0].response.fields[0].text, "old-session") == 0);
    CHECK(complete(rebooted, chunks(response, "QPIGS", 2, 32000), 12000) != nullptr);
    CHECK(cached_records(rebooted) == 2 && rebooted.latest_sequence() == 2);
    CHECK(rebooted.latest_timestamp() == 32000);
    const auto identity = std::find_if(rebooted.records().begin(), rebooted.records().end(),
        [](const auto &entry) { return std::strcmp(entry.command, "QID") == 0; });
    CHECK(identity != rebooted.records().end());
    CHECK(std::strcmp(identity->response.fields[0].text, "new-session") == 0);
    CHECK(identity->received_ms == 10000); // Confirmation must not make the first sample appear newer.

    network::ProtocolTelemetryCache partial_reboot;
    CHECK(complete(partial_reboot, chunks(newer_response, "QPIGS", 500, 900000), 1000) != nullptr);
    CHECK(partial_reboot.receive(chunks(newer_response, "QPIGS", 501, 902000)[0], 3000) == nullptr);
    const auto rejected_before = partial_reboot.rejected_records;
    CHECK(partial_reboot.receive(chunks(new_id, "QID", 1, 30000)[0], 10000) == nullptr);
    CHECK(partial_reboot.rejected_records > rejected_before); // Abandoned old-session partial is counted.
    CHECK(cached_records(partial_reboot) == 1 && partial_reboot.latest_sequence() == 500);
    CHECK(partial_reboot.records()[0].response.fields[0].value == 231.7);
    CHECK(complete(partial_reboot, chunks(response, "QPIGS", 2, 32000), 12000) != nullptr);
    CHECK(cached_records(partial_reboot) == 2 && partial_reboot.latest_sequence() == 2);
    const auto restored = std::find_if(partial_reboot.records().begin(), partial_reboot.records().end(),
        [](const auto &entry) { return std::strcmp(entry.command, "QID") == 0; });
    CHECK(restored != partial_reboot.records().end());
    CHECK(std::strcmp(restored->response.fields[0].text, "new-session") == 0);
    CHECK(restored->received_ms == 10000);

    for (const bool implausible_sequence : {true, false}) {
        network::ProtocolTelemetryCache guarded;
        CHECK(complete(guarded, chunks(newer_response, "QPIGS", 500, 900000), 1000) != nullptr);
        CHECK(guarded.receive(chunks(new_id, "QID", 1, 30000)[0], 10000) == nullptr);
        const auto candidate = chunks(response, "QPIGS", implausible_sequence ? 20 : 2,
            implausible_sequence ? 32000 : 110000);
        for (const auto &message : candidate) CHECK(guarded.receive(message, 12000) == nullptr);
        CHECK(guarded.latest_sequence() == 500 && guarded.latest_timestamp() == 900000);
        CHECK(guarded.records()[0].response.fields[0].value == 231.7);
    }
}

void native_parser()
{
    Fixture f;
    f.event(command(1, "PI30", "QPIGS"));
    CHECK(f.sink.native.size() == 1);
    const auto &read = f.sink.native.back();
    CHECK(read.command_id == 1 && read.target_member_id == 1);
    CHECK(read.origin == network::RemoteCommandOrigin::mqtt);
    CHECK(read.protocol == protocol::EnergyProtocol::pi30 && !read.write && !read.confirmed);
    CHECK(std::strcmp(read.command, "QPIGS") == 0);
    f.send(command(1, "PI30", "POP02", ",\"confirmed\":true"));
    CHECK(f.sink.native.size() == 1); // Duplicate ID cannot change a read into a write.
    f.send(command(2, "PI30", "POP02"));
    CHECK(f.sink.native.size() == 1);
    auto rejected = f.result();
    CHECK(rejected.outcome == network::ProtocolCommandOutcome::not_sent);
    CHECK(rejected.command_id == 2 && !rejected.write_sent && rejected.write_sent_known);
    CHECK(std::strstr(rejected.detail, "confirmed:true") != nullptr);
    f.send(command(2, "PI30", "POP02", ",\"confirmed\":true"));
    CHECK(f.sink.native.size() == 1); // Explicitly rejected ID is consumed too.
    f.send(command(UINT64_MAX, "PI30", "POP02", ",\"confirmed\":true,\"write\":true"));
    CHECK(f.sink.native.size() == 2);
    CHECK(f.sink.native.back().command_id == UINT64_MAX);
    CHECK(f.sink.native.back().write && f.sink.native.back().confirmed);
    f.send(command(3, "PI30", "QPIGS", ",\"write\":false"));
    CHECK(f.sink.native.size() == 3);

    const std::vector<std::string> malformed = {
        command(4, "AOHAI_MODBUS_RTU", "QPIGS"), command(4, "UNKNOWN", "QPIGS"),
        command(4, "PI30", "QPIGS", ",\"write\":true"),
        command(4, "PI30", "POP02", ",\"write\":false,\"confirmed\":true"),
        command(4, "PI30", "QPIGS", ",\"write\":0"),
        command(4, "PI30", "QPIGS\\rPOP02"), command(4, "PI30", "POP99", ",\"confirmed\":true"),
        command(4, "PI16", "POP02", ",\"confirmed\":true"), command(4, "PI30", ""),
        command(4, "PI30", std::string(80, 'Q').c_str()),
        "{\"command_id\":0,\"operation\":\"protocol\",\"protocol\":\"PI30\",\"command\":\"QPIGS\"}",
        "{\"command_id\":1.5,\"operation\":\"protocol\",\"protocol\":\"PI30\",\"command\":\"QPIGS\"}",
        "{\"command_id\":\"18446744073709551616\",\"operation\":\"protocol\",\"protocol\":\"PI30\",\"command\":\"QPIGS\"}",
        "{\"command_id\":\"-1\",\"operation\":\"protocol\",\"protocol\":\"PI30\",\"command\":\"QPIGS\"}",
        "{\"command_id\":\" 4\",\"operation\":\"protocol\",\"protocol\":\"PI30\",\"command\":\"QPIGS\"}",
        "{\"command_id\":4,\"operation\":\"protocol\",\"protocol\":30,\"command\":\"QPIGS\"}",
    };
    for (const auto &value : malformed) {
        f.send(value);
        CHECK(f.sink.native.size() == 3);
        CHECK(uxQueueMessagesWaiting(f.bridge.protocol_result_queue_) == 0);
    }
    f.send(command(4, "PI30", "QPIGS"));
    CHECK(f.sink.native.size() == 4); // Invalid requests do not consume an ID.
    f.send(command(5, "PI30", "QPIGS"), "test/site1/inverter/3/command");
    f.send(command(5, "PI30", "QPIGS"), "test/site1/inverter/255/command");
    f.send(command(5, "PI30", "QPIGS"), "test/site1/inverter/x/command");
    CHECK(f.sink.native.size() == 4);
    f.send("{\"command_id\":5,\"operation\":\"set_buzzer_enabled\",\"value\":1}");
    CHECK(f.sink.legacy.size() == 1);
    f.send(command(5, "PI30", "QPIGS"));
    CHECK(f.sink.native.size() == 4); // Both APIs share replay protection.
    f.send("{\"command_id\":4,\"operation\":\"set_buzzer_enabled\",\"value\":1}");
    CHECK(f.sink.legacy.size() == 1);

    for (unsigned n = 1; n < static_cast<unsigned>(protocol::EnergyProtocol::count); ++n) {
        const auto profile = static_cast<protocol::EnergyProtocol>(n);
        const auto &info = protocol::energy_protocol_info(profile);
        const auto before = f.sink.native.size();
        f.send(command(100 + n, info.name, protocol::energy_poll_command(profile, 0)));
        CHECK(f.sink.native.size() == before + 1);
        CHECK(f.sink.native.back().protocol == profile && !f.sink.native.back().write);
    }
}

void event_guards()
{
    Fixture f;
    const auto read = command(10, "PI30", "QPIGS");
    f.event(read, true);
    f.event(read, false, 1);
    f.event(read, false, 0, static_cast<int>(read.size()) + 1);
    f.event(read, false, 0, static_cast<int>(app::mqtt_command_max_bytes) + 1);
    CHECK(f.sink.native.empty());
    CHECK(uxQueueMessagesWaiting(f.bridge.protocol_result_queue_) == 0);
    f.event(read);
    CHECK(f.sink.native.size() == 1); // Guard rejections never reach deduplication.
}

network::ProtocolTelemetryMessage reading()
{
    network::ProtocolTelemetryMessage value{};
    value.source_member_id = 2;
    value.protocol = protocol::EnergyProtocol::pi30;
    value.timestamp_ms = 123456789;
    value.sequence = 4000000000U;
    value.chunk_count = 2;
    value.field_count = 2;
    std::strcpy(value.command, "QPIGS");
    std::strcpy(value.fields[0].key, "battery_voltage");
    std::strcpy(value.fields[0].unit, "V");
    value.fields[0].numeric = true;
    value.fields[0].value = 52.4;
    std::strcpy(value.fields[1].key, "mode");
    std::strcpy(value.fields[1].text, "Line \"mode\"");
    value.fields[1].numeric = false;
    return value;
}

network::ProtocolTelemetryMessage last_reading_chunk()
{
    auto value = reading();
    value.chunk_index = 1;
    value.final_chunk = true;
    std::strcpy(value.fields[0].key, "pv_input_power");
    std::strcpy(value.fields[0].unit, "W");
    value.fields[0].value = 856;
    std::strcpy(value.fields[1].key, "grid_voltage");
    std::strcpy(value.fields[1].unit, "V");
    value.fields[1].numeric = true;
    value.fields[1].value = 0;
    return value;
}

const host::Publication &publication(const Fixture &fixture, const char *topic)
{
    const auto found = std::find_if(fixture.client.published.rbegin(), fixture.client.published.rend(),
        [&topic](const auto &message) { return message.topic == topic; });
    CHECK(found != fixture.client.published.rend());
    return *found;
}

void deliver(Fixture &fixture, const network::ProtocolTelemetryMessage &message)
{
    fixture.bridge.publish_protocol_telemetry(message);
    CHECK(uxQueueMessagesWaiting(fixture.bridge.protocol_publish_queue_) == 1);
    fixture.flush_telemetry();
}

void telemetry_serialization()
{
    Fixture f;
    const auto value = reading();
    deliver(f, value);
    CHECK(f.client.published.empty()); // The former per-chunk MQTT loss regression.
    deliver(f, value); // Duplicate transport reports do not create MQTT fragments.
    CHECK(f.client.published.empty());
    deliver(f, last_reading_chunk());
    CHECK(f.client.published.size() == 2);
    const auto &message = publication(f, "test/site1/inverter/2/protocol/QPIGS");
    CHECK(message.topic == "test/site1/inverter/2/protocol/QPIGS");
    CHECK(message.qos == 1 && message.retain == 0);
    auto json = parsed(message);
    CHECK(std::strcmp(item(json.get(), "protocol")->valuestring, "PI30") == 0);
    CHECK(std::strcmp(item(json.get(), "command")->valuestring, "QPIGS") == 0);
    CHECK(item(json.get(), "member_id")->valueint == 2);
    CHECK(item(json.get(), "sequence")->valuedouble == 4000000000.0);
    CHECK(item(json.get(), "timestamp_ms")->valuedouble == 123456789.0);
    CHECK(cJSON_IsTrue(item(json.get(), "record_complete")));
    CHECK(cJSON_IsNumber(item(json.get(), "sample_age_ms")));
    CHECK(cJSON_GetObjectItemCaseSensitive(json.get(), "chunk_index") == nullptr);
    CHECK(cJSON_GetObjectItemCaseSensitive(json.get(), "chunk_count") == nullptr);
    CHECK(cJSON_GetObjectItemCaseSensitive(json.get(), "final_chunk") == nullptr);
    const auto *fields = item(json.get(), "fields");
    CHECK(cJSON_GetArraySize(fields) == 4);
    const auto *voltage = item(fields, "battery_voltage");
    CHECK(std::strcmp(item(voltage, "unit")->valuestring, "V") == 0);
    CHECK(std::fabs(item(voltage, "value")->valuedouble - 52.4) < 0.00001);
    const auto *mode = item(item(fields, "mode"), "value");
    CHECK(cJSON_IsString(mode));
    CHECK(std::strcmp(mode->valuestring, "Line \"mode\"") == 0);
    CHECK(item(item(fields, "pv_input_power"), "value")->valuedouble == 856);
    CHECK(item(item(fields, "grid_voltage"), "value")->valuedouble == 0);
    auto live = parsed(publication(f, "test/site1/inverter/2/live"));
    CHECK(std::strcmp(item(live.get(), "protocol")->valuestring, "PI30") == 0);
    CHECK(item(live.get(), "member_id")->valueint == 2);
    const auto *records = item(live.get(), "native_records");
    CHECK(cJSON_IsArray(records) && cJSON_GetArraySize(records) == 1);
    CHECK(cJSON_IsTrue(item(cJSON_GetArrayItem(records, 0), "record_complete")));
    CHECK(cJSON_GetArraySize(item(cJSON_GetArrayItem(records, 0), "fields")) == 4);
    auto unsafe_topic = value;
    std::strcpy(unsafe_topic.command, "bad/+topic");
    auto unsafe_last = last_reading_chunk();
    std::strcpy(unsafe_last.command, unsafe_topic.command);
    host::now_us += 2000000;
    deliver(f, unsafe_topic);
    deliver(f, unsafe_last);
    CHECK(publication(f, "test/site1/inverter/2/protocol").retain == 0);

    std::vector<network::ProtocolTelemetryMessage> invalid;
    auto broken = value; broken.chunk_count = 0; invalid.push_back(broken);
    broken = value; broken.chunk_index = 2; invalid.push_back(broken);
    broken = value; broken.final_chunk = true; invalid.push_back(broken);
    broken = value; broken.field_count = 0; invalid.push_back(broken);
    broken = value; broken.field_count = 3; invalid.push_back(broken);
    broken = value; broken.source_member_id = 10; invalid.push_back(broken);
    broken = value; broken.protocol = protocol::EnergyProtocol::count; invalid.push_back(broken);
    broken = value; broken.protocol = protocol::EnergyProtocol::aohai_modbus; invalid.push_back(broken);
    broken = value; broken.fields[0].value = std::numeric_limits<double>::infinity(); invalid.push_back(broken);
    broken = value; broken.fields[0].value = std::numeric_limits<double>::quiet_NaN(); invalid.push_back(broken);
    broken = value; std::memset(broken.command, 'Q', sizeof(broken.command)); invalid.push_back(broken);
    broken = value; broken.fields[0].key[0] = 0; invalid.push_back(broken);
    broken = value; std::memset(broken.fields[0].key, 'x', sizeof(broken.fields[0].key)); invalid.push_back(broken);
    broken = value; std::memset(broken.fields[0].unit, 'x', sizeof(broken.fields[0].unit)); invalid.push_back(broken);
    broken = value; std::memset(broken.fields[1].text, 'x', sizeof(broken.fields[1].text)); invalid.push_back(broken);
    for (const auto &bad : invalid) {
        f.bridge.publish_protocol_telemetry(bad);
        CHECK(uxQueueMessagesWaiting(f.bridge.protocol_publish_queue_) == 0);
    }
    // The default standalone AOHAI has no native telemetry queue.
    vQueueDelete(f.bridge.protocol_publish_queue_);
    f.bridge.protocol_publish_queue_ = nullptr;
    f.bridge.publish_protocol_telemetry(value);
    CHECK(uxQueueMessagesWaiting(f.bridge.protocol_publish_queue_) == 0);
}

void complete_pi30_live_telemetry(const char *export_path)
{
    Fixture fixture;
    const auto response = pi30_reference_reading();
    const auto messages = chunks(response);
    for (std::size_t index = messages.size() - 1; index > 0; --index) {
        deliver(fixture, messages[index]);
        CHECK(fixture.client.published.empty());
    }
    deliver(fixture, messages[0]);
    CHECK(fixture.client.published.size() == 2);
    auto native = parsed(publication(fixture, "test/site1/inverter/2/protocol/QPIGS"));
    const auto *fields = item(native.get(), "fields");
    CHECK(cJSON_GetArraySize(fields) == 30);
    for (std::size_t index = 0; index < response.count; ++index) {
        const auto &expected = response.fields[index];
        const auto *field = item(fields, expected.key);
        CHECK(std::strcmp(item(field, "unit")->valuestring, expected.unit) == 0);
        if (expected.numeric) CHECK(item(field, "value")->valuedouble == expected.value);
        else CHECK(std::strcmp(item(field, "value")->valuestring, expected.text) == 0);
    }
    const auto &live_message = publication(fixture, "test/site1/inverter/2/live");
    CHECK(live_message.qos == 1 && live_message.retain == 0);
    auto live = parsed(live_message);
    const auto *records = item(live.get(), "native_records");
    CHECK(cJSON_IsArray(records) && cJSON_GetArraySize(records) == 1);
    const auto *embedded_fields = item(cJSON_GetArrayItem(records, 0), "fields");
    CHECK(cJSON_GetArraySize(embedded_fields) == 30);
    for (std::size_t index = 0; index < response.count; ++index) {
        const auto &expected = response.fields[index];
        const auto *field = item(embedded_fields, expected.key);
        if (expected.numeric) CHECK(item(field, "value")->valuedouble == expected.value);
        else CHECK(std::strcmp(item(field, "value")->valuestring, expected.text) == 0);
    }
    if (export_path) {
        std::ofstream output(export_path, std::ios::binary);
        CHECK(output.good());
        for (const auto &message : fixture.client.published) {
            Json envelope(cJSON_CreateObject(), cJSON_Delete);
            CHECK(envelope != nullptr);
            CHECK(cJSON_AddStringToObject(envelope.get(), "topic", message.topic.c_str()) != nullptr);
            CHECK(cJSON_AddStringToObject(envelope.get(), "payload", message.payload.c_str()) != nullptr);
            char *encoded = cJSON_PrintUnformatted(envelope.get()); CHECK(encoded != nullptr);
            output << encoded << '\n'; cJSON_free(encoded);
        }
        CHECK(output.good());
    }
}

std::size_t publication_count(const Fixture &fixture, const char *topic)
{
    return std::count_if(fixture.client.published.begin(), fixture.client.published.end(),
        [topic](const auto &message) { return message.topic == topic; });
}

void status_changes_and_quality()
{
    host::now_us = 1000000000;
    Fixture status;
    protocol::EnergyResponse mode; CHECK(mode.add_text("device_mode", "", "Standby"));
    deliver(status, chunks(mode, "QMOD", 100, 900000)[0]);
    CHECK(publication_count(status, "test/site1/inverter/2/protocol/QMOD") == 1);
    auto initial = parsed(publication(status, "test/site1/inverter/2/live"));
    CHECK(cJSON_IsFalse(item(initial.get(), "core_status_fresh")));
    CHECK(std::strcmp(item(item(initial.get(), "operating_status"), "device_mode")->valuestring, "Standby") == 0);
    CHECK(cJSON_GetObjectItemCaseSensitive(item(initial.get(), "power_summary"), "pv_kw") == nullptr);
    host::now_us += 2000000;
    deliver(status, chunks(mode, "QMOD", 101, 902000)[0]);
    CHECK(publication_count(status, "test/site1/inverter/2/protocol/QMOD") == 1);
    CHECK(publication_count(status, "test/site1/inverter/2/live") == 1);
    std::strcpy(mode.fields[0].text, "Line");
    host::now_us += 1000000;
    deliver(status, chunks(mode, "QMOD", 102, 903000)[0]);
    CHECK(publication_count(status, "test/site1/inverter/2/protocol/QMOD") == 2);
    auto changed = parsed(publication(status, "test/site1/inverter/2/protocol/QMOD"));
    CHECK(std::strcmp(item(item(item(changed.get(), "fields"), "device_mode"), "value")->valuestring, "Line") == 0);
    host::now_us += 3000000;
    deliver(status, chunks(mode, "QMOD", 103, 906000)[0]);
    CHECK(publication_count(status, "test/site1/inverter/2/protocol/QMOD") == 2);
    CHECK(publication_count(status, "test/site1/inverter/2/live") == 2);
    host::now_us += 60000000;
    deliver(status, chunks(mode, "QMOD", 104, 966000)[0]);
    CHECK(publication_count(status, "test/site1/inverter/2/protocol/QMOD") == 3); // Unchanged status heartbeat.
    const auto live_count = publication_count(status, "test/site1/inverter/2/live");
    status.bridge.reconnect_count_ = 1;
    status.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
    CHECK(publication_count(status, "test/site1/inverter/2/live") == live_count + 1);

    Fixture power;
    for (const auto &message : chunks(pi30_reference_reading())) deliver(power, message);
    auto fresh = parsed(publication(power, "test/site1/inverter/2/live"));
    CHECK(cJSON_IsTrue(item(fresh.get(), "core_status_fresh")));
    CHECK(item(item(fresh.get(), "power_summary"), "pv_kw")->valuedouble == 0.856);
    CHECK(item(item(fresh.get(), "power_summary"), "load_kw")->valuedouble == 0.119);
    CHECK(item(item(fresh.get(), "power_summary"), "load_apparent_kva")->valuedouble == 0.161);
    CHECK(item(item(fresh.get(), "power_summary"), "load_percentage")->valuedouble == 3);
    CHECK(cJSON_GetObjectItemCaseSensitive(item(fresh.get(), "power_summary"), "grid_kw") == nullptr);
    CHECK(std::fabs(item(item(fresh.get(), "power_summary"), "battery_kw")->valuedouble + 0.690) < 1e-12);
    const auto *grid = item(item(fresh.get(), "sections"), "grid");
    CHECK(item(cJSON_GetArrayItem(grid, 0), "value")->valuedouble == 0);
    CHECK(cJSON_IsFalse(item(cJSON_GetArrayItem(grid, 0), "stale")));
    host::now_us += 16000000;
    power.client.outbox_bytes = 0; // Previously enqueued QoS 1 messages were acknowledged.
    power.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
    auto stale = parsed(publication(power, "test/site1/inverter/2/live"));
    CHECK(cJSON_IsFalse(item(stale.get(), "core_status_fresh")));
    CHECK(cJSON_IsNull(item(item(stale.get(), "power_summary"), "pv_kw")));
    CHECK(cJSON_IsNull(item(item(stale.get(), "power_summary"), "load_kw")));
    CHECK(cJSON_IsNull(item(item(stale.get(), "power_summary"), "load_apparent_kva")));
    CHECK(cJSON_IsNull(item(item(stale.get(), "power_summary"), "battery_kw")));
    const auto *quality = item(item(stale.get(), "power_quality"), "pv_kw");
    CHECK(cJSON_IsFalse(item(quality, "valid")) && cJSON_IsTrue(item(quality, "stale")));
    CHECK(item(quality, "age_ms")->valuedouble == 16000);
    CHECK(cJSON_IsTrue(item(cJSON_GetArrayItem(item(item(stale.get(), "sections"), "grid"), 0), "stale")));

    Fixture identity;
    protocol::EnergyResponse serial; CHECK(serial.add_text("serial_number", "", "92832207100756"));
    deliver(identity, chunks(serial, "QID", 100, 900000)[0]);
    auto identity_live = parsed(publication(identity, "test/site1/inverter/2/live"));
    CHECK(cJSON_IsFalse(item(identity_live.get(), "core_status_fresh")));
    CHECK(cJSON_IsArray(item(item(identity_live.get(), "sections"), "identity")));

    Fixture failure;
    failure.client.enqueue_result = -1;
    deliver(failure, chunks(serial, "QID", 100, 900000)[0]);
    CHECK(failure.client.published.empty());
    auto &cache = *failure.bridge.native_cache_[2];
    CHECK(!cache.live_published && !cache.records()[0].published);
    failure.client.enqueue_result = 1;
    failure.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
    CHECK(failure.client.published.empty()); // Failed attempts cannot flood every publisher loop.
    host::now_us += 1000000;
    failure.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
    CHECK(failure.client.published.size() == 2 && cache.records()[0].published && cache.live_published);
}

void compact_bms_command_labels()
{
    for (unsigned profile_index = 1; profile_index < static_cast<unsigned>(protocol::EnergyProtocol::count); ++profile_index) {
        const auto profile = static_cast<protocol::EnergyProtocol>(profile_index);
        for (std::size_t index = 0; index < protocol::energy_background_poll_count(profile); ++index) {
            protocol::EnergyPollInfo expected;
            CHECK(protocol::energy_background_poll_command(profile, index, expected));
            if (std::strlen(expected.command) <= 15) continue;
            char label[16]{}; std::memcpy(label, expected.command, 15);
            protocol::EnergyPollInfo actual;
            CHECK(network::protocol_poll_info(profile, label, actual));
            CHECK(std::strcmp(actual.command, expected.command) == 0);
            CHECK(actual.poll_class == expected.poll_class && actual.primary == expected.primary);
        }
    }
    protocol::EnergyPollInfo unknown;
    CHECK(!network::protocol_poll_info(protocol::EnergyProtocol::jkpb, "getAdditional", unknown));
    CHECK(!network::protocol_poll_info(protocol::EnergyProtocol::jkpb, "unrecognizedXYZ", unknown));
    CHECK(!network::protocol_poll_info(protocol::EnergyProtocol::pi30, "getAdditionalIn", unknown));

    host::now_us += 10000000;
    Fixture settings;
    protocol::EnergyResponse limits; CHECK(limits.add_number("cell_voltage_high_limit", "V", 3.6));
    auto setting = chunks(limits, "getAdditionalIn", 100, 900000)[0];
    setting.protocol = protocol::EnergyProtocol::jkpb;
    deliver(settings, setting);
    auto live = parsed(publication(settings, "test/site1/inverter/2/live"));
    CHECK(std::strcmp(item(live.get(), "protocol")->valuestring, "JKPB") == 0);
    CHECK(cJSON_IsFalse(item(live.get(), "core_status_fresh")));
    const auto *fields = item(item(live.get(), "sections"), "settings");
    CHECK(cJSON_IsArray(fields) && cJSON_GetArraySize(fields) == 1);
    CHECK(std::strcmp(item(cJSON_GetArrayItem(fields, 0), "name")->valuestring, "cell_voltage_high_limit") == 0);
    CHECK(item(cJSON_GetArrayItem(fields, 0), "value")->valuedouble == 3.6);
    CHECK(publication_count(settings, "test/site1/inverter/2/protocol/getAdditionalIn") == 1);
    ++setting.sequence; setting.timestamp_ms += 2000; host::now_us += 2000000;
    deliver(settings, setting);
    CHECK(publication_count(settings, "test/site1/inverter/2/protocol/getAdditionalIn") == 1);
    ++setting.sequence; setting.timestamp_ms += 2000; setting.fields[0].value = 3.65; host::now_us += 2000000;
    deliver(settings, setting);
    CHECK(publication_count(settings, "test/site1/inverter/2/protocol/getAdditionalIn") == 2);

    Fixture daly;
    protocol::EnergyResponse battery; CHECK(battery.add_number("battery_voltage", "V", 52.4));
    auto soc = chunks(battery, "SOC", 100, 900000)[0]; soc.protocol = protocol::EnergyProtocol::daly;
    deliver(daly, soc);
    auto primary = parsed(publication(daly, "test/site1/inverter/2/live"));
    CHECK(std::strcmp(item(primary.get(), "protocol")->valuestring, "DALY") == 0);
    CHECK(cJSON_IsTrue(item(primary.get(), "core_status_fresh")));
    CHECK(item(cJSON_GetArrayItem(item(item(primary.get(), "sections"), "battery"), 0), "value")->valuedouble == 52.4);
    protocol::EnergyResponse fault; CHECK(fault.add_number("battery_failure_bits", "bool", 0));
    auto warning = chunks(fault, "batteryFailureS", 101, 905000)[0]; warning.protocol = protocol::EnergyProtocol::daly;
    host::now_us += 5000000; deliver(daly, warning);
    auto status = parsed(publication(daly, "test/site1/inverter/2/live"));
    const auto *warnings = item(item(status.get(), "sections"), "status");
    CHECK(cJSON_IsArray(warnings) && cJSON_GetArraySize(warnings) == 1);
    CHECK(item(cJSON_GetArrayItem(warnings, 0), "value")->valuedouble == 0);
    CHECK(cJSON_GetArraySize(item(status.get(), "native_records")) == 2);
    CHECK(publication_count(daly, "test/site1/inverter/2/protocol/batteryFailureS") == 1);
    ++warning.sequence; warning.timestamp_ms += 2000; host::now_us += 2000000; deliver(daly, warning);
    CHECK(publication_count(daly, "test/site1/inverter/2/protocol/batteryFailureS") == 1);
}

void native_memory_admission()
{
    host::now_us += 10000000;
    Fixture fixture;
    protocol::EnergyResponse serial; CHECK(serial.add_text("serial_number", "", "92832207100756"));
    const auto identity = chunks(serial, "QID", 100, 900000)[0];
    const auto attempts_before = host::memory_guard_attempts;
    host::memory_admission_allowed = false;
    deliver(fixture, identity);
    CHECK(fixture.client.published.empty());
    CHECK(host::memory_guard_attempts > attempts_before && host::memory_guards_active == 0);
    auto &cache = *fixture.bridge.native_cache_[2];
    CHECK(cached_records(cache) == 1 && cache.latest_sequence() == 100);
    CHECK(!cache.records()[0].published && !cache.live_published);
    CHECK(std::strcmp(cache.records()[0].response.fields[0].text, "92832207100756") == 0);
    const auto received_ms = cache.records()[0].received_ms;
    host::memory_admission_allowed = true;
    host::now_us += 1000000;
    fixture.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
    CHECK(fixture.client.published.size() == 2);
    CHECK(cache.records()[0].published && cache.live_published && host::memory_guards_active == 0);
    CHECK(cache.records()[0].received_ms == received_ms);
    auto recovered = parsed(publication(fixture, "test/site1/inverter/2/protocol/QID"));
    CHECK(item(recovered.get(), "sample_age_ms")->valuedouble == 1000);

    Fixture pressure;
    pressure.client.outbox_bytes = 8193;
    deliver(pressure, identity);
    CHECK(pressure.client.published.empty());
    auto &pressured_cache = *pressure.bridge.native_cache_[2];
    CHECK(!pressured_cache.records()[0].published && !pressured_cache.live_published);
    CHECK(cached_records(pressured_cache) == 1);
    pressure.client.outbox_bytes = 0;
    host::now_us += 1000000;
    pressure.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
    CHECK(pressure.client.published.size() == 2);
    CHECK(pressured_cache.records()[0].published && pressured_cache.live_published);
    CHECK(host::memory_guards_active == 0);
}

struct NativeObservation {
    std::string command, source;
    protocol::EnergyResponse response;
};

std::vector<std::uint8_t> reference_hex(const char *hex)
{
    std::vector<std::uint8_t> frame;
    for (; *hex; hex += 2) {
        char octet[]{hex[0], hex[1], 0};
        frame.push_back(static_cast<std::uint8_t>(std::strtoul(octet, nullptr, 16)));
    }
    return frame;
}

bool add_observation(std::vector<NativeObservation> &observations, protocol::EnergyProtocol profile,
                     const char *command, const std::vector<std::uint8_t> &frame, const char *source)
{
    if (std::any_of(observations.begin(), observations.end(),
                   [command](const auto &value) { return value.command == command; })) return false;
    protocol::EnergyPollInfo poll;
    if (!protocol::energy_poll_info(profile, command, poll)) return false;
    bool background = false;
    for (std::size_t i = 0; i < protocol::energy_background_poll_count(profile); ++i) {
        protocol::EnergyPollInfo candidate;
        CHECK(protocol::energy_background_poll_command(profile, i, candidate));
        if (!std::strcmp(candidate.command, command)) background = true;
    }
    if (!background) return false;
    std::size_t frame_size = 0;
    const bool daly_group = (profile == protocol::EnergyProtocol::daly || profile == protocol::EnergyProtocol::daly40) &&
                            (!std::strcmp(command, "cellVoltages") || !std::strcmp(command, "cellTemperatures"));
    const auto framed = daly_group ? protocol::daly_group_frame_state(command, frame.data(), frame.size(),
                                      static_cast<unsigned>(frame.size() / 13), frame_size) :
                                    protocol::energy_frame_state(profile, command, frame.data(), frame.size(), frame_size);
    if (framed != protocol::FrameState::complete) return false;
    protocol::EnergyResponse response;
    const auto decoded = protocol::energy_decode_response(profile, command, 1, frame.data(), frame_size, response);
    if (decoded != protocol::CodecStatus::ok || !response.count) {
        if (!std::strcmp(source, "constructed_documented_packet"))
            std::printf("Constructed %s %s decode %u frame %zu/%zu\n", protocol::energy_protocol_info(profile).name, command,
                        static_cast<unsigned>(decoded), frame_size, frame.size());
        return false;
    }
    observations.push_back({command, source, response});
    return true;
}

std::vector<std::uint8_t> constructed_pi_frame(protocol::EnergyProtocol profile, const char *text)
{
    using Id = protocol::EnergyProtocol;
    std::string body;
    if (profile >= Id::pi17 && profile <= Id::pi18sv) {
        char header[8]{}; std::snprintf(header, sizeof(header), "^D%03u", static_cast<unsigned>(std::strlen(text) + 3));
        body = header + std::string(text);
    } else body = "(" + std::string(text);
    std::vector<std::uint8_t> frame(body.begin(), body.end());
    unsigned sum = 0;
    for (const auto octet : frame) {
        sum ^= static_cast<unsigned>(octet) << 8;
        for (unsigned bit = 0; bit < 8; ++bit) sum = ((sum << 1) ^ ((sum & 0x8000) ? 0x1021 : 0)) & 0xffff;
    }
    const bool raw = profile >= Id::pi17 && profile <= Id::pi17infini;
    const auto escape = [raw](unsigned octet) {
        return static_cast<std::uint8_t>(!raw && (octet == 0 || octet == 10 || octet == 13 || octet == 40) ? octet + 1 : octet);
    };
    frame.push_back(escape(sum >> 8)); frame.push_back(escape(sum & 255)); frame.push_back(13);
    return frame;
}

std::vector<std::uint8_t> constructed_jk_info()
{
    std::vector<std::uint8_t> frame(300);
    frame[0] = 0x55; frame[1] = 0xaa; frame[2] = 0xeb; frame[3] = 0x90; frame[4] = 3;
    std::memcpy(frame.data() + 6, "Documented JK", 13);
    std::memcpy(frame.data() + 22, "HW1", 3); std::memcpy(frame.data() + 30, "SW1", 3);
    std::memcpy(frame.data() + 86, "SERIAL00001", 11);
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) frame.back() += frame[i];
    return frame;
}

std::vector<std::uint8_t> constructed_jk232_cells()
{
    std::vector<std::uint8_t> frame{0xdd, 4, 0, 32};
    for (unsigned i = 0; i < 16; ++i) { frame.push_back(0x0c); frame.push_back(0xe4); }
    unsigned sum = 0; for (std::size_t i = 2; i < frame.size(); ++i) sum += frame[i];
    const auto checksum = static_cast<std::uint16_t>(0 - sum);
    frame.push_back(checksum >> 8); frame.push_back(checksum & 255); frame.push_back(0x77);
    return frame;
}

std::vector<std::uint8_t> constructed_daly_status(std::uint8_t command)
{
    std::vector<std::uint8_t> frame(13); frame[0] = 0xa5; frame[1] = 1; frame[2] = command; frame[3] = 8;
    for (unsigned i = 0; i < 12; ++i) frame[12] += frame[i];
    return frame;
}

std::vector<std::uint8_t> constructed_ved_reading()
{
    const std::string text = "\r\nPID\t0xA389\r\nFW\t0300\r\nSER#\tREFERENCE001\r\nV\t53509\r\nI\t-23866\r\nSOC\t548\r\nP\t-1277\r\nPPV\t0\r\nVPV\t120000\r\nAC_OUT_V\t23000\r\nAC_OUT_I\t123\r\nAC_OUT_S\t700\r\nIL\t1230\r\nCS\t3\r\nMPPT\t2\r\nH19\t1234\r\nChecksum\t";
    std::vector<std::uint8_t> frame(text.begin(), text.end());
    std::uint8_t sum = 0; for (const auto value : frame) sum += value;
    frame.push_back(0 - sum); return frame;
}

void all_profile_telemetry(const char *export_path, const char *reference_path)
{
    std::ifstream reference_file(reference_path, std::ios::binary); CHECK(reference_file.good());
    const std::string reference_data((std::istreambuf_iterator<char>(reference_file)), {});
    Json references(cJSON_Parse(reference_data.c_str()), cJSON_Delete); CHECK(references != nullptr);
    const auto *vectors = item(references.get(), "vectors");
    std::ofstream output;
    if (export_path) { output.open(export_path, std::ios::binary); CHECK(output.good()); }
    using Id = protocol::EnergyProtocol;
    unsigned tested_profiles = 0;
    for (unsigned profile_index = 1; profile_index < static_cast<unsigned>(Id::count); ++profile_index) {
        const auto profile = static_cast<Id>(profile_index);
        const char *profile_name = protocol::energy_protocol_info(profile).name;
        std::vector<NativeObservation> observations;
        for (int i = 0; i < cJSON_GetArraySize(vectors); ++i) {
            const auto *entry = cJSON_GetArrayItem(vectors, i);
            if (!cJSON_IsNull(item(entry, "pattern"))) continue;
            std::string name = item(entry, "protocol")->valuestring;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
            if (name != profile_name) continue;
            add_observation(observations, profile, item(entry, "command")->valuestring,
                reference_hex(item(entry, "frame_hex")->valuestring), "recorded_upstream");
        }
        for (const auto &entry : bms_vectors) {
            const bool daly_alias = profile == Id::daly40 && std::strcmp(entry.profile, "DALY") == 0;
            if (std::strcmp(entry.profile, profile_name) != 0 && !daly_alias) continue;
            add_observation(observations, profile, entry.command, reference_hex(entry.hex),
                            daly_alias ? "recorded_response_shared_daly_layout" : "recorded_upstream");
        }
        if (profile == Id::pi18 || profile == Id::pi18lvx || profile == Id::pi18sv)
            add_observation(observations, profile, "GS", constructed_pi_frame(profile,
                "2300,500,2300,500,1000,900,18,540,540,540,2,3,85,35,30,30,600,400,3500,3400,0,2,2,1,1,2,1,0"), "constructed_documented_packet");
        if (profile == Id::pi17 || profile == Id::pi17m058 || profile == Id::pi17infini)
            add_observation(observations, profile, "PS", constructed_pi_frame(profile,
                "00263,00381,-00100,-00855,-03141,-03156,-07152,00100,00200,00300,00600,00110,00210,00310,00630,003,1,1,1,2,2,1"), "constructed_documented_packet");
        if (profile == Id::pi30mst) {
            // Upstream captured only a truncated three-token variant; exercise
            // the documented twelve-token reply with the reported total too.
            observations.erase(std::remove_if(observations.begin(), observations.end(),
                [](const auto &entry) { return entry.command == "QPIGS2"; }), observations.end());
            CHECK(add_observation(observations, profile, "QPIGS2", constructed_pi_frame(profile,
                "03.1 327.3 54.4 01026 00010110 2.0 108 1.1 325.0 54.3 358 02263"), "constructed_documented_packet"));
        }
        if (profile == Id::pi41)
            add_observation(observations, profile, "QPIGS2", constructed_pi_frame(profile,
                "000.0 00.0 230.0 49.9 0322 0238 006 004 103.8 57.50 0011011"), "constructed_documented_packet");
        if (profile == Id::jk02 || profile == Id::jk02_32 || profile == Id::jk04 || profile == Id::jkv11)
            CHECK(add_observation(observations, profile, "getInfo", constructed_jk_info(), "constructed_documented_packet"));
        if (profile == Id::jk232)
            CHECK(add_observation(observations, profile, "getCellData", constructed_jk232_cells(), "constructed_documented_packet"));
        if (profile == Id::daly || profile == Id::daly40) {
            CHECK(add_observation(observations, profile, "cellBalanceState", constructed_daly_status(0x97), "constructed_documented_packet"));
            CHECK(add_observation(observations, profile, "batteryFailureStatus", constructed_daly_status(0x98), "constructed_documented_packet"));
        }
        if (profile == Id::ved) {
            CHECK(add_observation(observations, profile, "vedtext", constructed_ved_reading(), "constructed_documented_packet"));
            CHECK(add_observation(observations, profile, "batteryCapacity", reference_hex("3a373030313030303738303043360a"), "manufacturer_hex_reference"));
        }
        CHECK(!observations.empty());
        bool primary = false;
        for (const auto &observation : observations) {
            protocol::EnergyPollInfo info; CHECK(protocol::energy_poll_info(profile, observation.command.c_str(), info));
            primary = primary || info.primary;
        }
        if (!primary) {
            std::printf("Missing primary decoded observation for %s\n", profile_name);
            CHECK(primary);
        }
        Fixture fixture;
        host::now_us += 20000000;
        const auto start_us = host::now_us;
        std::uint32_t sequence = 100;
        std::size_t total_fields = 0;
        for (const auto &observation : observations) {
            total_fields += observation.response.count;
            fixture.client.outbox_bytes = 0;
            const auto observation_sequence = sequence++;
            for (auto message : chunks(observation.response, observation.command.c_str(), observation_sequence, 900000 + observation_sequence * 100)) {
                message.protocol = profile;
                deliver(fixture, message);
            }
        }
        host::now_us = start_us + 6000000;
        fixture.client.outbox_bytes = 0;
        fixture.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
        const auto &live_publication = publication(fixture, "test/site1/inverter/2/live");
        auto live = parsed(live_publication);
        std::printf("%s: %zu decoded queries, %zu fields, aggregate /live %zu bytes, embedded %d queries\n", profile_name,
                    observations.size(), total_fields, live_publication.payload.size(), cJSON_GetArraySize(item(live.get(), "native_records")));
        CHECK(std::strcmp(item(live.get(), "protocol")->valuestring, profile_name) == 0);
        CHECK(cJSON_IsTrue(item(live.get(), "core_status_fresh")));
        const auto *records = item(live.get(), "native_records");
        const auto *sections = item(live.get(), "sections");
        CHECK(static_cast<std::size_t>(cJSON_GetArraySize(records)) == observations.size());
        network::NativePowerSummary expected_power(profile);
        Json expected_fields(cJSON_CreateObject(), cJSON_Delete); CHECK(expected_fields != nullptr);
        std::uint32_t expected_sequence = 100;
        for (const auto &observation : observations) {
            protocol::EnergyPollInfo info; CHECK(protocol::energy_poll_info(profile, observation.command.c_str(), info));
            char command_label[16]{}; std::snprintf(command_label, sizeof(command_label), "%s", observation.command.c_str());
            cJSON *expected_command = cJSON_AddObjectToObject(expected_fields.get(), command_label); CHECK(expected_command != nullptr);
            const cJSON *record = nullptr;
            for (int index = 0; index < cJSON_GetArraySize(records); ++index) {
                const auto *candidate = cJSON_GetArrayItem(records, index);
                if (std::strcmp(item(candidate, "command")->valuestring, command_label) == 0) record = candidate;
            }
            CHECK(record != nullptr);
            const auto *actual_fields = item(record, "fields");
            CHECK(static_cast<std::size_t>(cJSON_GetArraySize(actual_fields)) == observation.response.count);
            for (std::size_t field_index = 0; field_index < observation.response.count; ++field_index) {
                const auto &field = observation.response.fields[field_index];
                const auto *actual = item(actual_fields, field.key);
                CHECK(std::strcmp(item(actual, "unit")->valuestring, field.unit) == 0);
                if (field.numeric) CHECK(std::fabs(item(actual, "value")->valuedouble - field.value) <=
                                         std::max(1.0, std::fabs(field.value)) * 1e-12);
                else CHECK(std::strcmp(item(actual, "value")->valuestring, field.text) == 0);
                const auto *section_name = network::protocol_live_section(profile, command_label, field.key, field.unit);
                const auto *section = cJSON_GetObjectItemCaseSensitive(sections, section_name);
                if (section != nullptr) {
                    bool found = false;
                    for (int i = 0; i < cJSON_GetArraySize(section); ++i)
                        if (std::strcmp(item(cJSON_GetArrayItem(section, i), "name")->valuestring, field.key) == 0) found = true;
                    CHECK(found);
                } else {
                    const auto *metadata_only = cJSON_GetObjectItemCaseSensitive(live.get(), "metadata_in_native_records");
                    const auto *all_compacted = cJSON_GetObjectItemCaseSensitive(live.get(), "sections_in_native_records");
                    CHECK(cJSON_IsTrue(all_compacted) || (cJSON_IsTrue(metadata_only) &&
                          (!std::strcmp(section_name, "identity") || !std::strcmp(section_name, "settings") || !std::strcmp(section_name, "energy"))));
                }
                cJSON *expected = cJSON_AddObjectToObject(expected_command, field.key); CHECK(expected != nullptr);
                CHECK(cJSON_AddStringToObject(expected, "unit", field.unit) != nullptr);
                CHECK(cJSON_AddStringToObject(expected, "group", section_name) != nullptr);
                CHECK((field.numeric ? cJSON_AddNumberToObject(expected, "value", field.value) :
                                       cJSON_AddStringToObject(expected, "value", field.text)) != nullptr);
                if (info.poll_class == protocol::EnergyPollClass::live) expected_power.observe(field, 6000, expected_sequence);
            }
            ++expected_sequence;
        }
        Json expected_summary(cJSON_CreateObject(), cJSON_Delete); CHECK(expected_summary != nullptr);
        const auto *power = item(live.get(), "power_summary");
        for (std::size_t metric = 0; metric < network::NativePowerSummary::metric_count; ++metric) {
            const auto reading = expected_power.reading(static_cast<network::NativePowerSummary::Metric>(metric));
            const char *key = network::NativePowerSummary::names[metric];
            const auto *value = cJSON_GetObjectItemCaseSensitive(power, key);
            if (!reading.present) { CHECK(value == nullptr); continue; }
            CHECK(cJSON_IsNumber(value) && std::fabs(value->valuedouble - reading.value) <=
                                         std::max(1.0, std::fabs(reading.value)) * 1e-12);
            CHECK(cJSON_AddNumberToObject(expected_summary.get(), key, reading.value) != nullptr);
        }
        // Independent expected values from these packet bytes supplement the
        // complete round-trip comparison; the summary cannot just mirror a
        // mistaken alias, electrical-unit conversion or channel sum.
        struct Expected { const char *key; double value; };
        std::vector<Expected> fixed_expected;
        if (profile == Id::pi16) fixed_expected = {{"load_kw", 0}, {"load_percentage", 29}};
        else if (profile == Id::pi17 || profile == Id::pi17m058 || profile == Id::pi17infini)
            fixed_expected = {{"pv_kw", 0.644}, {"grid_kw", -7.152}, {"grid_apparent_kva", 0}, {"load_kw", 0}, {"load_apparent_kva", 0.535}, {"load_percentage", 3}};
        else if (profile == Id::pi18 || profile == Id::pi18lvx || profile == Id::pi18sv)
            fixed_expected = {{"pv_kw", 1}, {"load_kw", 0.9}, {"load_apparent_kva", 1}, {"load_percentage", 18}, {"battery_kw", -0.054}};
        else if (profile == Id::pi30)
            fixed_expected = {{"pv_kw", 0.856}, {"load_kw", 0.119}, {"load_apparent_kva", 0.161}, {"load_percentage", 3}, {"battery_kw", -0.690}};
        else if (profile == Id::pi30max || profile == Id::pi30m044 || profile == Id::pi30m045)
            fixed_expected = {{"pv_kw", 1.905}, {"load_kw", 0.751}, {"load_apparent_kva", 0.829}, {"load_percentage", 10}, {"battery_kw", -1.090}};
        else if (profile == Id::pi30mst)
            fixed_expected = {{"pv_kw", 2.263}, {"load_kw", 0.751}, {"load_apparent_kva", 0.829}, {"load_percentage", 10}, {"battery_kw", -1.090}};
        else if (profile == Id::pi30revo)
            fixed_expected = {{"pv_kw", 0}, {"load_kw", 0.707}, {"load_apparent_kva", 0}, {"load_percentage", 12}, {"battery_kw", 0.04392}};
        else if (profile == Id::pi41) fixed_expected = {{"pv_kw", 0.856}, {"load_kw", 0.357}, {"load_apparent_kva", 0.483}, {"battery_kw", -0.690}};
        else if (profile == Id::jk02 || profile == Id::jkv11 || profile == Id::jk232) fixed_expected = {{"battery_kw", 0}};
        else if (profile == Id::jk02_32) fixed_expected = {{"battery_kw", 0.214448472}}; // -25.029 V * -8.568 A.
        else if (profile == Id::jkpb) fixed_expected = {{"battery_kw", -0.479066532}}; // -26.852 V * 17.841 A.
        else if (profile == Id::jkserial) fixed_expected = {{"battery_kw", -0.2526834}}; // -55.78 V * 4.53 A.
        else if (profile == Id::daly || profile == Id::daly40) fixed_expected = {{"battery_kw", 0.33264}}; // 52.8 V * 6.3 A.
        else if (profile == Id::ved) fixed_expected = {{"pv_kw", 0}, {"load_apparent_kva", 0.7}, {"battery_kw", 1.277}};
        CHECK(static_cast<std::size_t>(cJSON_GetArraySize(expected_summary.get())) == fixed_expected.size());
        for (const auto &expected : fixed_expected)
            CHECK(std::fabs(item(power, expected.key)->valuedouble - expected.value) <= 1e-12);
        if (export_path) {
            // Keep only the final aggregate /live, plus complete /protocol messages.
            for (const auto &message : fixture.client.published) {
                if (message.topic.ends_with("/live") && &message != &live_publication) continue;
                Json envelope(cJSON_CreateObject(), cJSON_Delete); CHECK(envelope != nullptr);
                CHECK(cJSON_AddStringToObject(envelope.get(), "topic", message.topic.c_str()) != nullptr);
                CHECK(cJSON_AddStringToObject(envelope.get(), "payload", message.payload.c_str()) != nullptr);
                CHECK(cJSON_AddStringToObject(envelope.get(), "profile", profile_name) != nullptr);
                CHECK(cJSON_AddStringToObject(envelope.get(), "fixture_source", "recorded_and_documented_constructed_packets") != nullptr);
                CHECK(cJSON_AddItemToObject(envelope.get(), "expected_fields", cJSON_Duplicate(expected_fields.get(), true)));
                CHECK(cJSON_AddItemToObject(envelope.get(), "expected_summary", cJSON_Duplicate(expected_summary.get(), true)));
                cJSON *sources = cJSON_AddObjectToObject(envelope.get(), "observation_sources"); CHECK(sources != nullptr);
                for (const auto &observation : observations)
                    CHECK(cJSON_AddStringToObject(sources, observation.command.c_str(), observation.source.c_str()) != nullptr);
                char *encoded = cJSON_PrintUnformatted(envelope.get()); CHECK(encoded != nullptr);
                output << encoded << '\n'; cJSON_free(encoded);
            }
        }
        ++tested_profiles;
    }
    CHECK(tested_profiles == 25);
    if (export_path) CHECK(output.good());
    std::printf("All %u native profiles decoded through production MQTT records and aggregate telemetry.\n", tested_profiles);
}

void compact_native_snapshot_and_recovery()
{
    host::now_us += 10000000;
    Fixture compact;
    compact.bridge.live_json_capacity_ = 3500;
    const auto response = pi30_reference_reading();
    for (const auto &message : chunks(response)) deliver(compact, message);
    const auto &publication_value = publication(compact, "test/site1/inverter/2/live");
    CHECK(publication_value.payload.size() < 3500);
    auto live = parsed(publication_value);
    CHECK(cJSON_IsTrue(item(live.get(), "sections_in_native_records")));
    CHECK(cJSON_GetObjectItemCaseSensitive(live.get(), "sections") == nullptr);
    CHECK(cJSON_GetArraySize(item(live.get(), "native_records")) == 1);
    CHECK(cJSON_GetArraySize(item(cJSON_GetArrayItem(item(live.get(), "native_records"), 0), "fields")) == 30);
    CHECK(item(item(live.get(), "power_summary"), "pv_kw")->valuedouble == 0.856);
    CHECK(item(item(live.get(), "power_summary"), "load_kw")->valuedouble == 0.119);
    CHECK(cJSON_IsTrue(item(live.get(), "core_status_fresh")));

    Fixture too_small;
    too_small.bridge.live_json_capacity_ = 256;
    for (const auto &message : chunks(response)) deliver(too_small, message);
    CHECK(too_small.client.published.empty());
    CHECK(too_small.bridge.native_cache_[2] != nullptr);
    const auto &record = too_small.bridge.native_cache_[2]->records()[0];
    CHECK(record.response.count == 30 && !record.published);
    CHECK(!too_small.bridge.native_cache_[2]->live_published);
    too_small.bridge.live_json_capacity_ = too_small.json_buffer.size();
    host::now_us += 1000000;
    too_small.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
    CHECK(too_small.client.published.size() == 2);
    auto recovered = parsed(publication(too_small, "test/site1/inverter/2/live"));
    CHECK(cJSON_GetArraySize(item(cJSON_GetArrayItem(item(recovered.get(), "native_records"), 0), "fields")) == 30);
    CHECK(item(cJSON_GetArrayItem(item(recovered.get(), "native_records"), 0), "sample_age_ms")->valuedouble == 1000);
    CHECK(cJSON_IsTrue(item(recovered.get(), "core_status_fresh")));
}

void native_normalization_guards()
{
    using Id = protocol::EnergyProtocol;
    const auto grouped = [](Id id, const char *key, const char *unit, const char *expected) {
        CHECK(std::strcmp(network::native_field_section(id, key, unit), expected) == 0);
    };
    grouped(Id::jk02, "Battery_Voltage", "V", "battery");
    grouped(Id::jk232, "Total Current", "A", "battery");
    grouped(Id::jk232, "Protection State", "", "status");
    grouped(Id::jkserial, "Software Version", "", "identity");
    grouped(Id::pi30, "PV Input Current for Battery", "A", "pv");
    grouped(Id::pi30, "Battery Voltage from SCC", "V", "battery");
    grouped(Id::ved, "P", "W", "battery");
    grouped(Id::ved, "PPV", "W", "pv");
    grouped(Id::ved, "AC_OUT_S", "VA", "output");
    grouped(Id::ved, "H19", "kWh", "energy");
    grouped(Id::ved, "MPPT", "", "status");
    network::NativePowerSummary incompatible(Id::pi30);
    protocol::EnergyResponse fields;
    CHECK(fields.add_number("PV Input Power", "VA", 1000));
    CHECK(fields.add_number("AC Output Active Power", "VA", 2000));
    CHECK(fields.add_number("AC Output Apparent Power", "W", 3000));
    CHECK(fields.add_number("Battery Voltage", "V", 54));
    CHECK(fields.add_number("Battery Current", "A", -20));
    for (std::size_t i = 0; i < fields.count; ++i) incompatible.observe(fields.fields[i], 0);
    for (std::size_t i = 0; i < network::NativePowerSummary::metric_count; ++i)
        CHECK(!incompatible.reading(static_cast<network::NativePowerSummary::Metric>(i)).present);
    network::NativePowerSummary split(Id::pi41);
    fields = {}; CHECK(fields.add_number("L1 AC Output Active Power", "W", 0));
    split.observe(fields.fields[0], 0);
    CHECK(!split.reading(network::NativePowerSummary::output).present);
    CHECK(fields.add_number("L2 AC Output Active Power", "kW", 0.238));
    split.observe(fields.fields[1], 0);
    CHECK(split.reading(network::NativePowerSummary::output).value == 0.238);
    split.observe(fields.fields[1], 16000); // A delayed stale sample cannot replace fresh physical zero.
    CHECK(split.reading(network::NativePowerSummary::output).fresh());
    network::NativePowerSummary both_stale(Id::pi41);
    for (std::size_t i = 0; i < fields.count; ++i) both_stale.observe(fields.fields[i], 16000);
    CHECK(both_stale.reading(network::NativePowerSummary::output).present && !both_stale.reading(network::NativePowerSummary::output).fresh());
}

void native_signed_battery_power()
{
    using Id = protocol::EnergyProtocol;
    using Power = network::NativePowerSummary;
    const auto battery_fields = [](double volts, double charge, double discharge) {
        protocol::EnergyResponse response;
        CHECK(response.add_number("battery_voltage", "V", volts));
        CHECK(response.add_number("battery_charging_current", "A", charge));
        CHECK(response.add_number("battery_discharge_current", "A", discharge));
        return response;
    };
    const auto observe = [](Power &summary, const protocol::EnergyResponse &response,
                            std::uint32_t age = 0, std::uint32_t sequence = 100) {
        for (std::size_t index = 0; index < response.count; ++index)
            summary.observe(response.fields[index], age, sequence);
    };
    const auto signed_case = [&](double charge, double discharge, double expected) {
        host::now_us += 20000000;
        Fixture fixture;
        const auto response = battery_fields(54, charge, discharge);
        const auto messages = chunks(response);
        CHECK(messages.size() == 2);
        deliver(fixture, messages[0]);
        CHECK(fixture.client.published.empty()); // No estimate from an incomplete observation.
        deliver(fixture, messages[1]);
        auto live = parsed(publication(fixture, "test/site1/inverter/2/live"));
        const auto *power = item(live.get(), "power_summary");
        CHECK(std::fabs(item(power, "battery_kw")->valuedouble - expected) < 1e-12);
        CHECK(cJSON_GetObjectItemCaseSensitive(power, "grid_kw") == nullptr);
        CHECK(cJSON_GetObjectItemCaseSensitive(power, "grid_apparent_kva") == nullptr);
        CHECK(cJSON_IsTrue(item(item(item(live.get(), "power_quality"), "battery_kw"), "valid")));
        host::now_us += 16000000;
        fixture.client.outbox_bytes = 0;
        fixture.bridge.publish_protocol_live_now(2, static_cast<std::uint32_t>(host::now_us / 1000));
        auto stale = parsed(publication(fixture, "test/site1/inverter/2/live"));
        CHECK(cJSON_IsNull(item(item(stale.get(), "power_summary"), "battery_kw")));
        const auto *quality = item(item(stale.get(), "power_quality"), "battery_kw");
        CHECK(cJSON_IsFalse(item(quality, "valid")) && cJSON_IsTrue(item(quality, "stale")));
        CHECK(item(quality, "age_ms")->valuedouble == 16000);
    };
    signed_case(3, 20, 0.918); // 54 V * (20 A discharge - 3 A charge) / 1000.
    signed_case(3, 0, -0.162);
    signed_case(0, 0, 0);

    auto response = battery_fields(54, 3, 20);
    Power oldest(Id::pi30);
    oldest.observe(response.fields[0], 2000, 100);
    oldest.observe(response.fields[1], 7000, 100);
    oldest.observe(response.fields[2], 9000, 100);
    auto reading = oldest.reading(Power::battery);
    CHECK(reading.present && reading.fresh() && reading.age_ms == 9000);
    CHECK(std::fabs(reading.value - 0.918) < 1e-12);
    Power stale(Id::pi30);
    stale.observe(response.fields[0], 1000, 100);
    stale.observe(response.fields[1], 16000, 100);
    stale.observe(response.fields[2], 1000, 100);
    reading = stale.reading(Power::battery);
    CHECK(reading.present && !reading.fresh() && reading.age_ms == 16000);

    Power mixed(Id::pi30);
    mixed.observe(response.fields[0], 0, 100);
    mixed.observe(response.fields[1], 0, 100);
    mixed.observe(response.fields[2], 0, 101);
    CHECK(!mixed.reading(Power::battery).present);
    Power missing_charge(Id::pi30);
    missing_charge.observe(response.fields[0], 0, 100);
    missing_charge.observe(response.fields[2], 0, 100);
    CHECK(!missing_charge.reading(Power::battery).present);
    Power missing_discharge(Id::pi30);
    missing_discharge.observe(response.fields[0], 0, 100);
    missing_discharge.observe(response.fields[1], 0, 100);
    CHECK(!missing_discharge.reading(Power::battery).present);
    Power missing_voltage(Id::pi30);
    missing_voltage.observe(response.fields[1], 0, 100);
    missing_voltage.observe(response.fields[2], 0, 100);
    CHECK(!missing_voltage.reading(Power::battery).present);
    for (unsigned invalid_index = 0; invalid_index < 3; ++invalid_index) {
        auto invalid = response;
        std::strcpy(invalid.fields[invalid_index].unit, invalid_index ? "W" : "A");
        Power incompatible(Id::pi30); observe(incompatible, invalid);
        CHECK(!incompatible.reading(Power::battery).present);
    }
    for (unsigned invalid_index = 0; invalid_index < 3; ++invalid_index) {
        auto invalid = response;
        invalid.fields[invalid_index].value = std::numeric_limits<double>::quiet_NaN();
        Power nonfinite(Id::pi30); observe(nonfinite, invalid);
        CHECK(!nonfinite.reading(Power::battery).present);
    }
    for (unsigned invalid_index = 1; invalid_index < 3; ++invalid_index) {
        auto invalid = response;
        invalid.fields[invalid_index].value = -1;
        Power negative_magnitude(Id::pi30); observe(negative_magnitude, invalid);
        CHECK(!negative_magnitude.reading(Power::battery).present);
    }

    // QPIRI's similarly named ratings are settings; they cannot create live power.
    host::now_us += 20000000;
    Fixture rated;
    CHECK(response.add_number("ac_input_voltage", "V", 230));
    CHECK(response.add_number("ac_input_current", "A", 21.7));
    for (const auto &message : chunks(response, "QPIRI")) deliver(rated, message);
    auto live = parsed(publication(rated, "test/site1/inverter/2/live"));
    const auto *power = item(live.get(), "power_summary");
    CHECK(cJSON_GetObjectItemCaseSensitive(power, "battery_kw") == nullptr);
    CHECK(cJSON_GetObjectItemCaseSensitive(power, "grid_kw") == nullptr);
    CHECK(cJSON_GetObjectItemCaseSensitive(power, "grid_apparent_kva") == nullptr);
    CHECK(cJSON_IsFalse(item(live.get(), "core_status_fresh")));
}

void native_grid_active_and_apparent_power()
{
    using Id = protocol::EnergyProtocol;
    using Power = network::NativePowerSummary;
    protocol::EnergyResponse single;
    CHECK(single.add_number("ac_input_voltage", "V", 230));
    CHECK(single.add_number("ac_input_current", "A", 3));
    Power apparent(Id::pi30);
    apparent.observe(single.fields[0], 4000, 101);
    apparent.observe(single.fields[1], 7000, 101);
    auto reading = apparent.reading(Power::grid_apparent);
    CHECK(reading.present && reading.fresh() && reading.age_ms == 7000);
    CHECK(std::fabs(reading.value - 0.690) < 1e-12);
    CHECK(!apparent.reading(Power::grid).present); // V * I supplies VA, not W.
    Power mixed(Id::pi30);
    mixed.observe(single.fields[0], 0, 101);
    mixed.observe(single.fields[1], 0, 102);
    CHECK(!mixed.reading(Power::grid_apparent).present);
    Power unknown_observation(Id::pi30);
    unknown_observation.observe(single.fields[0], 0);
    unknown_observation.observe(single.fields[1], 0);
    CHECK(!unknown_observation.reading(Power::grid_apparent).present);
    Power missing_current(Id::pi30);
    missing_current.observe(single.fields[0], 0, 101);
    CHECK(!missing_current.reading(Power::grid_apparent).present);
    for (unsigned invalid_index = 0; invalid_index < 2; ++invalid_index) {
        auto invalid = single;
        std::strcpy(invalid.fields[invalid_index].unit, invalid_index ? "VA" : "A");
        Power incompatible(Id::pi30);
        for (std::size_t index = 0; index < invalid.count; ++index)
            incompatible.observe(invalid.fields[index], 0, 101);
        CHECK(!incompatible.reading(Power::grid_apparent).present);
    }
    protocol::EnergyResponse phases;
    CHECK(phases.add_number("ac_input_voltage_r", "V", 230));
    CHECK(phases.add_number("ac_input_current_r", "A", 3));
    CHECK(phases.add_number("ac_input_voltage_s", "V", 231));
    CHECK(phases.add_number("ac_input_current_s", "A", 2));
    CHECK(phases.add_number("ac_input_voltage_t", "V", 232));
    CHECK(phases.add_number("ac_input_current_t", "A", 1));
    Power incomplete(Id::pi17);
    for (std::size_t index = 0; index + 1 < phases.count; ++index)
        incomplete.observe(phases.fields[index], 0, 100);
    CHECK(!incomplete.reading(Power::grid_apparent).present);
    incomplete.observe(phases.fields[phases.count - 1], 0, 100);
    CHECK(std::fabs(incomplete.reading(Power::grid_apparent).value - 1.384) < 1e-12);
    CHECK(!incomplete.reading(Power::grid).present);
    Power mixed_phases(Id::pi17);
    for (std::size_t index = 0; index < phases.count; ++index)
        mixed_phases.observe(phases.fields[index], 0, index < 4 ? 100 : 101);
    CHECK(!mixed_phases.reading(Power::grid_apparent).present);
    Power oldest(Id::pi17);
    for (std::size_t index = 0; index < phases.count; ++index)
        oldest.observe(phases.fields[index], index == 2 ? 16000 : 1000, 100);
    reading = oldest.reading(Power::grid_apparent);
    CHECK(reading.present && !reading.fresh() && reading.age_ms == 16000);

    host::now_us += 20000000;
    Fixture fixture;
    for (auto message : chunks(phases, "GS", 100)) {
        message.protocol = Id::pi17;
        deliver(fixture, message);
    }
    auto live = parsed(publication(fixture, "test/site1/inverter/2/live"));
    CHECK(std::fabs(item(item(live.get(), "power_summary"), "grid_apparent_kva")->valuedouble - 1.384) < 1e-12);
    CHECK(cJSON_GetObjectItemCaseSensitive(item(live.get(), "power_summary"), "grid_kw") == nullptr);
    protocol::EnergyResponse measured;
    CHECK(measured.add_number("ac_input_total_active_power", "W", -500));
    CHECK(measured.add_number("ac_output_total_active_power", "W", 400));
    CHECK(measured.add_number("ac_output_total_apparent_power", "VA", 650));
    host::now_us += 6000000;
    for (auto message : chunks(measured, "PS", 101)) {
        message.protocol = Id::pi17;
        deliver(fixture, message);
    }
    live = parsed(publication(fixture, "test/site1/inverter/2/live"));
    const auto *power = item(live.get(), "power_summary");
    CHECK(std::fabs(item(power, "grid_kw")->valuedouble + 0.5) < 1e-12);
    CHECK(std::fabs(item(power, "grid_apparent_kva")->valuedouble - 1.384) < 1e-12);
    CHECK(std::fabs(item(power, "load_kw")->valuedouble - 0.4) < 1e-12);
    CHECK(std::fabs(item(power, "load_apparent_kva")->valuedouble - 0.650) < 1e-12);
    const auto *quality = item(item(live.get(), "power_quality"), "grid_apparent_kva");
    CHECK(item(quality, "age_ms")->valuedouble == 6000);
}

void native_battery_direction_and_protocol_signs()
{
    using Id = protocol::EnergyProtocol;
    using Power = network::NativePowerSummary;
    for (const auto profile : {Id::pi17, Id::pi17m058, Id::pi17infini}) {
        for (const unsigned direction : {1u, 2u}) {
            // A valid checksummed PS includes the signed magnitude token that
            // the recorded reference packet leaves empty. Its direction token
            // determines our normalized discharge-positive display convention.
            const auto frame = constructed_pi_frame(profile, direction == 1 ?
                "00263,00381,-00100,-00855,-03141,-03156,-07152,00100,00200,00300,00600,00110,00210,00310,00630,003,1,1,1,1,2,1" :
                "00263,00381,-00100,-00855,-03141,-03156,-07152,00100,00200,00300,00600,00110,00210,00310,00630,003,1,1,1,2,2,1");
            protocol::EnergyResponse response;
            CHECK(protocol::energy_decode_response(profile, "PS", 1, frame.data(), frame.size(), response) == protocol::CodecStatus::ok);
            bool battery_found = false;
            for (std::size_t index = 0; index < response.count; ++index) {
                if (std::strcmp(response.fields[index].key, "battery_power") == 0) {
                    battery_found = true;
                    CHECK(response.fields[index].numeric && response.fields[index].value == -100);
                }
            }
            CHECK(battery_found);
            host::now_us += 20000000;
            Fixture fixture;
            for (auto message : chunks(response, "PS", 100)) {
                message.protocol = profile;
                deliver(fixture, message);
            }
            auto live = parsed(publication(fixture, "test/site1/inverter/2/live"));
            CHECK(std::fabs(item(item(live.get(), "power_summary"), "battery_kw")->valuedouble -
                            (direction == 1 ? -0.100 : 0.100)) < 1e-12);
        }
    }
    protocol::EnergyResponse fields;
    CHECK(fields.add_number("battery_power", "W", 100));
    CHECK(fields.add_text("battery_power_direction", "", "Discharging"));
    Power mixed_direction(Id::pi17);
    mixed_direction.observe(fields.fields[0], 0, 100);
    mixed_direction.observe(fields.fields[1], 0, 101);
    CHECK(!mixed_direction.reading(Power::battery).present);
    Power missing_direction(Id::pi17);
    missing_direction.observe(fields.fields[0], 0, 100);
    CHECK(!missing_direction.reading(Power::battery).present);
    Power stale_direction(Id::pi17);
    stale_direction.observe(fields.fields[0], 0, 100);
    stale_direction.observe(fields.fields[1], 16000, 100);
    CHECK(stale_direction.reading(Power::battery).present &&
          !stale_direction.reading(Power::battery).fresh() && stale_direction.reading(Power::battery).age_ms == 16000);
    fields.fields[0].value = 0;
    Power zero(Id::pi17);
    zero.observe(fields.fields[0], 0, 100);
    CHECK(zero.reading(Power::battery).present && zero.reading(Power::battery).value == 0);

    for (const auto profile : {Id::jk02, Id::jk02_32, Id::jkv11, Id::jkpb}) {
        for (const double current : {-2.0, 2.0, 0.0}) {
            fields = {};
            CHECK(fields.add_number("battery_power", "W", 999)); // An unsigned magnitude must not set direction.
            CHECK(fields.add_number("battery_voltage", "V", 50));
            CHECK(fields.add_number("battery_current", "A", current));
            Power signed_current(profile);
            for (std::size_t index = 0; index < fields.count; ++index)
                signed_current.observe(fields.fields[index], 0, 100);
            const auto reading = signed_current.reading(Power::battery);
            CHECK(reading.present && std::fabs(reading.value - (current < 0 ? 0.100 : current > 0 ? -0.100 : 0)) < 1e-12);
            Power magnitude_only(profile);
            magnitude_only.observe(fields.fields[0], 0, 100);
            CHECK(!magnitude_only.reading(Power::battery).present);
            Power mixed_current(profile);
            mixed_current.observe(fields.fields[1], 0, 100);
            mixed_current.observe(fields.fields[2], 0, 101);
            CHECK(!mixed_current.reading(Power::battery).present);
        }
    }
    for (const auto profile : {Id::jk232, Id::jkserial, Id::daly, Id::daly40, Id::ved}) {
        const bool daly = profile == Id::daly || profile == Id::daly40;
        const char *voltage_key = profile == Id::jk232 ? "total_battery_voltage" : daly ? "battery_bank_voltage" : profile == Id::ved ? "v" : "battery_voltage";
        const char *current_key = profile == Id::jk232 ? "total_current" : daly ? "current" : profile == Id::ved ? "i" : "battery_current";
        for (const double current : {-2.0, 2.0, 0.0}) {
            fields = {};
            CHECK(fields.add_number(voltage_key, "V", 50));
            CHECK(fields.add_number(current_key, "A", current));
            Power signed_current(profile);
            signed_current.observe(fields.fields[0], 0, 100);
            signed_current.observe(fields.fields[1], 0, 100);
            const auto reading = signed_current.reading(Power::battery);
            CHECK(reading.present && std::fabs(reading.value - (daly ? current : -current) * 0.05) < 1e-12);
        }
    }
    fields = {};
    CHECK(fields.add_number("p", "W", -120));
    CHECK(fields.add_number("v", "V", 50));
    CHECK(fields.add_number("i", "A", -2));
    Power measured_ved(Id::ved);
    for (std::size_t index = 0; index < fields.count; ++index) measured_ved.observe(fields.fields[index], 0, 100);
    CHECK(measured_ved.reading(Power::battery).present && measured_ved.reading(Power::battery).value == 0.120);
}

void result_serialization_and_ota()
{
    Fixture f;
    f.bridge.connected_ = true;
    f.bridge.publisher_heartbeat_ms_ = static_cast<std::uint32_t>(host::now_us / 1000);
    CHECK(f.bridge.ota_results_drained());
    network::ProtocolCommandResult result{};
    result.command_id = UINT64_MAX;
    result.source_member_id = 1;
    result.protocol = protocol::EnergyProtocol::pi30;
    result.outcome = network::ProtocolCommandOutcome::acknowledged;
    result.write_sent = true;
    std::strcpy(result.command, "POP02");
    std::strcpy(result.detail, "Accepted; readback pending");
    f.bridge.publish_protocol_command_result(result);
    CHECK(!f.bridge.ota_results_drained());
    f.bridge.publish_protocol_command_result_now(f.result());
    CHECK(!f.bridge.ota_results_drained()); // Still in the shared MQTT outbox.
    CHECK(f.client.published.size() == 1);
    CHECK(f.client.published.back().topic == "test/site1/inverter/1/result");
    CHECK(f.client.published.back().qos == 1 && f.client.published.back().retain == 0);
    auto json = parsed(f.client.published.back());
    CHECK(std::strcmp(item(json.get(), "command_id")->valuestring, "18446744073709551615") == 0);
    CHECK(std::strcmp(item(json.get(), "operation")->valuestring, "protocol") == 0);
    CHECK(std::strcmp(item(json.get(), "outcome")->valuestring, "acknowledged") == 0);
    CHECK(cJSON_IsTrue(item(json.get(), "write_sent")));
    f.client.outbox_bytes = 0;
    CHECK(f.bridge.ota_results_drained());
    result.outcome = network::ProtocolCommandOutcome::uncertain;
    result.write_sent_known = false;
    f.bridge.publish_protocol_command_result(result);
    f.bridge.publish_protocol_command_result_now(f.result());
    auto uncertain = parsed(f.client.published.back());
    CHECK(cJSON_IsNull(item(uncertain.get(), "write_sent")));
    CHECK(std::strcmp(item(uncertain.get(), "outcome")->valuestring, "uncertain") == 0);
    constexpr const char *outcomes[] = {
        "completed", "acknowledged", "rejected", "unsupported", "unavailable",
        "not_sent", "uncertain", "invalid_response",
    };
    result.write_sent = false;
    result.write_sent_known = true;
    for (unsigned index = 0; index < std::size(outcomes); ++index) {
        result.outcome = static_cast<network::ProtocolCommandOutcome>(index);
        f.bridge.publish_protocol_command_result(result);
        f.bridge.publish_protocol_command_result_now(f.result());
        auto outcome = parsed(f.client.published.back());
        CHECK(std::strcmp(item(outcome.get(), "outcome")->valuestring, outcomes[index]) == 0);
        CHECK(cJSON_IsFalse(item(outcome.get(), "write_sent")));
    }
    f.client.outbox_bytes = 0;
    f.bridge.publisher_heartbeat_ms_ = 1;
    CHECK(!f.bridge.ota_results_drained());
    f.bridge.publisher_heartbeat_ms_ = static_cast<std::uint32_t>(host::now_us / 1000);
    f.bridge.connected_ = false;
    CHECK(!f.bridge.ota_results_drained());

    // Existing outbox reserves room for results while native telemetry backs up.
    f.client.outbox_bytes = 60 * 1024;
    const auto published = f.client.published.size();
    deliver(f, reading());
    deliver(f, last_reading_chunk());
    CHECK(f.client.published.size() == published);
    f.bridge.publish_protocol_command_result(result);
    f.bridge.publish_protocol_command_result_now(f.result());
    CHECK(f.client.published.size() == published + 1);
    f.client.outbox_bytes = 64 * 1024;
    f.bridge.publish_protocol_command_result(result);
    f.bridge.publish_protocol_command_result_now(f.result());
    CHECK(f.client.published.size() == published + 1);
    CHECK(host::debug_errors >= 2);
}
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif
    native_parser();
    event_guards();
    atomic_record_cache();
    cache_profile_capacity_and_clock();
    conservative_reboot_probation();
    telemetry_serialization();
    complete_pi30_live_telemetry(argc > 1 ? argv[1] : nullptr);
    status_changes_and_quality();
    compact_bms_command_labels();
    native_memory_admission();
    native_normalization_guards();
    native_signed_battery_power();
    native_grid_active_and_apparent_power();
    native_battery_direction_and_protocol_signs();
    compact_native_snapshot_and_recovery();
    all_profile_telemetry(argc > 1 && argv[1][0] ? argv[1] : nullptr,
                          argc > 2 ? argv[2] : "tools/tests/pi_reference_vectors.generated.json");
    result_serialization_and_ota();
    std::printf("%u native MQTT checks passed using actual production methods and real cJSON/codecs.\n", host::checks);
}
