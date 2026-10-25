// Exercise the production scheduler and catalog at the Modbus method boundary.
// No port or hardware is opened. The clock includes the configured 850 ms gap.
#include "inverter_gateway/inverter/production_poller.hpp"
#include "inverter_gateway/app/project_config.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

using namespace inverter_gateway;
using namespace inverter_gateway::inverter;
std::int64_t aohai_poll_host_time_ms = 1;
static unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "AOHAI poll check at %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace scripted {
struct Read { RegisterSpace space; std::uint16_t first, count; bool success; std::int64_t time_ms; };
static std::vector<Read> reads;
static bool reject_batches = false;
static bool reject_all = false;
static std::uint16_t status = 2;
static std::uint16_t features = feature_pv | feature_battery;
static unsigned pv_inputs = 2;
static unsigned unexpected = 0;

static unsigned effective_count(const PollRequest &request) {
    if (request.space != RegisterSpace::input) return request.register_count;
    if (request.first_register == 40) return 24 + 2 * std::min(pv_inputs, 2U);
    if (request.first_register == 68) return pv_inputs > 2 ? 2 * (pv_inputs - 2) : 0;
    if (request.first_register == 252) return 2 * pv_inputs;
#ifndef AOHAI_BASELINE_MEASURE
    if (request.first_register == 125 && (features & feature_bms) == 0) return 5;
#endif
    return request.register_count;
}

static bool read(RegisterSpace space, std::uint16_t first, std::uint16_t count,
                 std::uint16_t *destination, std::size_t capacity) {
    CHECK(destination != nullptr && count <= capacity && count > 0 && count <= 125);
    bool original = false;
    std::array<bool, 512> authorized{};
    const auto plan = default_poll_plan();
    for (std::size_t index = 0; index < plan.size; ++index) {
        const auto &request = plan.data[index];
        if (request.space != space || !poll_request_is_enabled(request, features) ||
            request.cadence == PollCadence::on_demand) continue;
        const auto words = effective_count(request);
        if (request.first_register == first && words == count) original = true;
        for (unsigned offset = 0; offset < words; ++offset)
            if (request.first_register + offset < authorized.size()) authorized[request.first_register + offset] = true;
    }
    for (unsigned offset = 0; offset < count; ++offset) {
        if (first + offset >= authorized.size() || !authorized[first + offset]) ++unexpected;
    }
    const bool success = !reject_all && !(reject_batches && !original);
    // One complete method call includes the configured minimum bus gap and
    // a deterministic successful response (or rejected-frame) turnaround.
    aohai_poll_host_time_ms += app::modbus_minimum_command_gap_ms + 20;
    reads.push_back({space, first, count, success, aohai_poll_host_time_ms});
    if (success) {
        for (unsigned offset = 0; offset < count; ++offset) destination[offset] = first + offset;
        if (space == RegisterSpace::input && first == 0) destination[0] = status;
    }
    return success;
}

static void reset(std::uint16_t enabled, unsigned pvs, bool reject = false) {
    aohai_poll_host_time_ms = 1; reads.clear(); features = enabled; pv_inputs = pvs;
    reject_batches = reject; reject_all = false; unexpected = 0; status = 2;
}
}

namespace inverter_gateway::protocol {
ModbusRtuClient::ModbusRtuClient(CdcAcmDevice &device, std::uint8_t slave,
    std::uint32_t timeout, std::uint32_t gap)
    : device_(device), slave_address_(slave), response_timeout_ms_(timeout), minimum_command_gap_ticks_(gap) {}
bool ModbusRtuClient::read_holding(std::uint16_t first, std::uint16_t count, std::uint16_t *values, std::size_t capacity)
{ return scripted::read(RegisterSpace::holding, first, count, values, capacity); }
bool ModbusRtuClient::read_input(std::uint16_t first, std::uint16_t count, std::uint16_t *values, std::size_t capacity)
{ return scripted::read(RegisterSpace::input, first, count, values, capacity); }
}

struct Sink final : network::RuntimeMessageSink {
    std::array<bool, 512> inputs{}, holding{};
    unsigned packets = 0;
    void on_telemetry(const network::TelemetryMessage &message) override {
        CHECK(message.register_count > 0 && message.register_count <= message.registers.size());
        CHECK(message.source_member_id == 3 && message.connected_pv_inputs == scripted::pv_inputs);
        CHECK(message.enabled_features == scripted::features);
        CHECK(message.timestamp_ms == aohai_poll_host_time_ms);
        ++packets;
        for (unsigned offset = 0; offset < message.register_count; ++offset) {
            const auto address = message.first_register + offset;
            CHECK(address < inputs.size());
            CHECK(message.registers[offset] == (message.register_space == 1 && address == 0
                ? scripted::status : address));
            (message.register_space == 1 ? inputs : holding)[address] = true;
        }
    }
    void on_command(const network::RoutedCommand &) override {}
    void on_command_result(const network::RoutedCommandResult &) override {}
};

static bool covered(const Sink &sink) {
    const auto plan = default_poll_plan();
    for (std::size_t index = 0; index < plan.size; ++index) {
        const auto &request = plan.data[index];
        if (!poll_request_is_enabled(request, scripted::features) || request.cadence == PollCadence::on_demand) continue;
        const auto &seen = request.space == RegisterSpace::input ? sink.inputs : sink.holding;
        for (unsigned offset = 0; offset < scripted::effective_count(request); ++offset)
            if (!seen[request.first_register + offset]) return false;
    }
    return true;
}

static void simulate(protocol::ModbusRtuClient &client, const char *name,
                     std::uint16_t features, unsigned pvs, bool reject = false) {
    scripted::reset(features, pvs, reject);
    Sink sink;
    ProductionPoller poller(client, 3, pvs, 0, sink);
    unsigned turns = 0;
    while (!covered(sink) && ++turns < 50000) {
        poller.run_due(features);
        aohai_poll_host_time_ms += app::inverter_loop_delay_ms;
    }
    CHECK(covered(sink)); CHECK(scripted::unexpected == 0);
    const auto coverage_ms = aohai_poll_host_time_ms - 1;
    unsigned words = 0, core_reads = 0, optional_batches = 0;
    std::int64_t last_core_ms = 0;
    for (const auto &read : scripted::reads) {
        words += read.count;
        if (read.space == RegisterSpace::input && read.first == 0) {
            CHECK(read.count == 40); ++core_reads;
            if (last_core_ms != 0)
                CHECK(read.time_ms - last_core_ms <= app::inverter_state_freshness_ms);
            last_core_ms = read.time_ms;
        }
        if (read.space == RegisterSpace::input && read.first >= 250 && read.first < 369 && read.count > 2)
            ++optional_batches;
    }
    // Identity/settings are snapshots, never swept again after success.
    const auto before = scripted::reads.size();
    for (unsigned turn = 0; turn < 1000; ++turn) {
        poller.run_due(features); aohai_poll_host_time_ms += app::inverter_loop_delay_ms;
    }
    for (std::size_t index = before; index < scripted::reads.size(); ++index) {
        const auto &read = scripted::reads[index];
        CHECK(read.space != RegisterSpace::holding || read.first == 182);
    }
    CHECK(poller.state().fresh && poller.state().state == InverterOperatingState::operating);
    poller.invalidate_state_after_write(); CHECK(!poller.state().fresh);
    poller.run_due(features);
    CHECK(poller.state().fresh);
    CHECK(scripted::reads.back().space == RegisterSpace::input && scripted::reads.back().first == 0);
    aohai_poll_host_time_ms += app::inverter_state_freshness_ms + 1;
    CHECK(!poller.state().fresh);
    std::printf("%s: first complete coverage %zu transactions / %.2f seconds, %u words, %u core reads, %u optional power groups\n",
                name, before, double(coverage_ms) / 1000.0, words, core_reads, optional_batches);
}

static void failure_and_reconnect(protocol::ModbusRtuClient &client) {
    const auto features = std::uint16_t(feature_pv | feature_battery);
    scripted::reset(features, 2, true);
    Sink sink;
    ProductionPoller poller(client, 3, 2, 0, sink);
    for (unsigned turn = 0; turn < 1200; ++turn) {
        poller.run_due(features); aohai_poll_host_time_ms += app::inverter_loop_delay_ms;
    }
    CHECK(covered(sink)); CHECK(scripted::unexpected == 0);
    const auto stable_start = scripted::reads.size();
    for (unsigned turn = 0; turn < 600; ++turn) {
        poller.run_due(features); aohai_poll_host_time_ms += app::inverter_loop_delay_ms;
    }
    // A model that rejects batches continues producing every measurement using
    // the original reads, without repeatedly probing its unsupported ranges.
    for (auto index = stable_start; index < scripted::reads.size(); ++index)
        CHECK(scripted::reads[index].success);
    poller.reset();
    const auto reconnect_start = scripted::reads.size();
    for (unsigned turn = 0; turn < 100; ++turn) {
        poller.run_due(features); aohai_poll_host_time_ms += app::inverter_loop_delay_ms;
    }
    CHECK(std::any_of(scripted::reads.begin() + reconnect_start, scripted::reads.end(),
        [](const auto &read) { return !read.success; }));
    poller.invalidate_state_after_write(); scripted::status = 3;
    poller.run_due(features);
    CHECK(poller.state().fresh && poller.state().state == InverterOperatingState::faulted);
    scripted::reject_all = true;
    poller.invalidate_state_after_write();
    const auto emitted_before_failure = sink.packets;
    for (unsigned turn = 0; turn < 30; ++turn) {
        poller.run_due(features); aohai_poll_host_time_ms += app::inverter_loop_delay_ms;
    }
    CHECK(!poller.state().fresh && sink.packets == emitted_before_failure);
}

int main() {
    CdcAcmDevice device;
    protocol::ModbusRtuClient client(device, 1, app::modbus_response_timeout_ms, app::modbus_minimum_command_gap_ms);
    simulate(client, "single phase 2 PV battery", feature_pv | feature_battery, 2);
    simulate(client, "single phase no PV/battery", feature_none, 0);
    simulate(client, "three phase 16 PV BMS generator parallel",
        feature_pv | feature_extended_pv | feature_battery | feature_bms | feature_three_phase | feature_generator | feature_parallel, 16);
    simulate(client, "short-read-only inverter", feature_pv | feature_battery, 2, true);
    failure_and_reconnect(client);
    std::printf("AOHAI production poll tests passed: %u checks\n", checks);
}
