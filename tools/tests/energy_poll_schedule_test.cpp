#include "inverter_gateway/protocol/energy_poll_schedule.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace inverter_gateway::protocol;
namespace {
unsigned checks = 0;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s (check %u)\n", message, checks); std::exit(1); }
}
bool command_is(const EnergyPollInfo *poll, const char *command)
{
    return poll && std::strcmp(poll->command, command) == 0;
}
using History = std::map<std::string, std::vector<std::uint64_t>>;
History run(EnergyPollScheduler &schedule, std::uint64_t begin, std::uint64_t end,
            std::uint64_t slot_ms = 1000, std::uint64_t transaction_ms = 0)
{
    History result;
    for (auto now = begin; now <= end; now += slot_ms) {
        const auto *poll = schedule.next(now);
        if (!poll) continue;
        result[poll->command].push_back(now);
        check(schedule.next(now) == nullptr, "active poll cannot be issued twice");
        schedule.record(true, now + transaction_ms);
    }
    return result;
}
void profile_coverage()
{
    EnergyPollScheduler schedule;
    schedule.reset(EnergyProtocol::aohai_modbus, 1);
    check(schedule.count() == 0 && !schedule.next(0), "AOHAI keeps its existing poller");
    for (unsigned raw = 1; raw < static_cast<unsigned>(EnergyProtocol::count); ++raw) {
        const auto protocol = static_cast<EnergyProtocol>(raw);
        const auto count = energy_background_poll_count(protocol);
        check(count && count <= schedule.maximum_commands, "all native profiles fit fixed storage");
        schedule.reset(protocol, raw, 1000);
        check(schedule.count() == count && schedule.session() == raw, "all profile entries loaded");
        check(!schedule.next(999), "reset does not poll before supplied time");
        EnergyPollInfo poll{};
        unsigned primary = 0;
        for (std::size_t index = 0; index < count; ++index) {
            check(energy_background_poll_command(protocol, index, poll), "entry exists");
            EnergyCommandInfo info{};
            check(energy_command_info(protocol, poll.command, info) && !info.setter,
                  "automatic reads always belong to readonly catalog");
            check(!std::strchr(poll.command, '(') && !std::strchr(poll.command, '\\'),
                  "automatic polls need no unspecified date or parallel index");
            EnergyPollInfo recognized{};
            check(energy_poll_info(protocol, poll.command, recognized) &&
                  recognized.poll_class == poll.poll_class && recognized.primary == poll.primary,
                  "manual classification agrees with scheduled classification");
            std::uint8_t request[256]{};
            std::size_t size = 0;
            check(energy_build_request(protocol, poll.command, 1, request, sizeof(request), size) == CodecStatus::ok,
                  "all scheduled requests have valid concrete wire encodings");
            if (poll.primary) { ++primary; check(poll.interval_ms == 2000, "core status is fast"); }
            if (poll.poll_class == EnergyPollClass::identity)
                check(poll.interval_ms == 0 && poll.publish_on_change, "identity once per session");
            if (poll.poll_class == EnergyPollClass::configuration)
                check((poll.interval_ms == 120000 || poll.interval_ms == 0) && poll.publish_on_change,
                      "settings are slow and capabilities once, both changed-only");
        }
        check(primary != 0, "all native profiles have a core telemetry request");
        constexpr unsigned expected[] = {
            0, 9, 20, 20, 20, 10, 14, 14, 18, 26, 26, 7, 27, 26, 18,
            2, 2, 2, 3, 1, 3, 2, 1, 9, 9, 2,
        };
        check(count == expected[raw], "resolved native profile has all expected fixed queries");
        check(!energy_background_poll_command(protocol, count, poll), "past end rejected");
        if (protocol == EnergyProtocol::daly || protocol == EnergyProtocol::daly40) {
            schedule.set_read_context_ready("cellVoltages", true, 1000);
            schedule.set_read_context_ready("cellTemperatures", true, 1000);
        }
        const auto history = run(schedule, 1000, 181000);
        for (std::size_t index = 0; index < count; ++index) {
            energy_background_poll_command(protocol, index, poll);
            const auto found = history.find(poll.command);
            check(found != history.end(), "every profile command gets a bounded slot");
            if (poll.poll_class == EnergyPollClass::identity || poll.interval_ms == 0)
                check(found->second.size() == 1, "static metadata not repeated while connected");
        }
    }
}
void pi30_frequency()
{
    EnergyPollScheduler schedule;
    schedule.reset(EnergyProtocol::pi30, 7);
    check(command_is(schedule.next(0), "QPIGS"), "PI30 starts with live PV/grid/battery measurements");
    schedule.record(true, 0);
    const auto history = run(schedule, 1000, 180000, 250);
    const auto &live = history.at("QPIGS");
    check(live.size() >= 70, "live measurements repeat frequently");
    for (std::size_t i = 1; i < live.size(); ++i)
        check(live[i] - live[i - 1] >= 2000 && live[i] - live[i - 1] <= 4000,
              "metadata cannot starve fast telemetry");
    check(history.at("QPI").size() == 1 && history.at("QID").size() == 1 &&
          history.at("QVFW").size() == 1 && history.at("QVFW2").size() == 1,
          "protocol, serial, and firmware identity once each");
    check(history.at("QPIRI").size() == 2 && history.at("QFLAG").size() == 2,
          "configuration at two-minute cadence");
    check(history.at("QMOD").size() >= 25, "mode follows actual device regularly");
    schedule.reset(EnergyProtocol::pi30, 8, 200000);
    const auto reconnect = run(schedule, 200000, 260000);
    check(reconnect.at("QPI").size() == 1 && reconnect.at("QID").size() == 1,
          "reconnect refreshes identity");

    schedule.reset(EnergyProtocol::pi30, 9);
    const auto slow = run(schedule, 0, 240000, 2000, 2000);
    const auto &slow_live = slow.at("QPIGS");
    for (std::size_t i = 1; i < slow_live.size(); ++i) {
        check(slow_live[i] - slow_live[i - 1] <= 6000, "slow transactions still prioritize core measurements");
    }
    check(slow.at("QPI").size() == 1 && slow.at("QVFW2").size() == 1,
          "continuous slow live work cannot starve identity");
}
void identity_failures_and_write_refresh()
{
    EnergyPollScheduler schedule;
    schedule.reset(EnergyProtocol::pi30, 1);
    unsigned attempts = 0;
    std::vector<std::uint64_t> attempted_at;
    for (std::uint64_t now = 0; now <= 180000; now += 1000) {
        const auto *poll = schedule.next(now);
        if (!poll) continue;
        const bool optional = command_is(poll, "QVFW2");
        if (optional) { ++attempts; attempted_at.push_back(now); }
        schedule.record(!optional, now);
    }
    check(attempts == 3, "unresponsive optional identity has bounded retries");
    check(attempted_at[1] - attempted_at[0] >= 10000 &&
          attempted_at[2] - attempted_at[1] >= 60000, "optional identity backs off");
    schedule.reset(EnergyProtocol::pi30, 2);
    unsigned rejected = 0;
    for (std::uint64_t now = 0; now < 120000; now += 1000) {
        const auto *poll = schedule.next(now);
        if (!poll) continue;
        const bool optional = command_is(poll, "QVFW2");
        if (optional) ++rejected;
        schedule.record(!optional, now, CodecStatus::rejected);
    }
    check(rejected == 1, "unsupported optional query does not thrash inverter");
    schedule.reset(EnergyProtocol::pi30, 3);
    check(run(schedule, 0, 60000).at("QVFW2").size() == 1, "new session restores optional probes");

    schedule.reset(EnergyProtocol::pi30, 4);
    schedule.observe("QPI", true, 0);
    const auto observed = run(schedule, 0, 120000);
    check(observed.count("QPI") == 0, "manual identity query satisfies background once read");
    schedule.refresh_configuration(121000);
    schedule.refresh_live(121000);
    const auto refreshed = run(schedule, 121000, 141000);
    check(refreshed.count("QPIRI") && refreshed.count("QFLAG") && refreshed.count("QPIGS"),
          "write refreshes settings and live state without catalog round robin");

    schedule.reset(EnergyProtocol::pi30, 5);
    schedule.observe("QPIRI", true, 0);
    check(run(schedule, 0, 20000).count("QPIRI") == 0, "write readback avoids duplicate background settings read");
}
void economical_refresh_policy()
{
    for (unsigned raw = 1; raw < static_cast<unsigned>(EnergyProtocol::count); ++raw) {
        const auto protocol = static_cast<EnergyProtocol>(raw);
        EnergyPollScheduler schedule;
        schedule.reset(protocol, 1);
        const auto history = run(schedule, 0, 360000, 100);
        for (const auto *command : {"QDI", "DI"}) {
            check(history.count(command) == 0,
                  "manufacturer defaults are not read as current telemetry");
            EnergyCommandInfo info{};
            if (!energy_command_info(protocol, command, info)) continue;
            check(!info.setter, "manufacturer defaults remain available as explicit readonly queries");
            std::uint8_t request[256]{};
            std::size_t size = 0;
            check(energy_build_request(protocol, command, 1, request, sizeof(request), size) == CodecStatus::ok,
                  "manual manufacturer default query retains its wire encoding");
        }
        for (const auto *command : {"QMCHGCR", "QMUCHGCR", "MCHGCR", "MUCHGCR"}) {
            const auto found = history.find(command);
            if (found == history.end()) continue;
            EnergyPollInfo info{};
            check(energy_poll_info(protocol, command, info) &&
                  info.poll_class == EnergyPollClass::configuration && info.interval_ms == 0 &&
                  info.publish_on_change && !info.primary,
                  "charging choices are immutable capability metadata");
            check(found->second.size() == 1, "charging choices are not repeatedly read");
        }
        schedule.refresh_configuration(360100);
        const auto after_write = run(schedule, 360100, 390000, 100);
        for (const auto *command : {"QMCHGCR", "QMUCHGCR", "MCHGCR", "MUCHGCR"})
            check(after_write.count(command) == 0,
                  "a settings write does not reread unchanged capability choices");
        for (const auto *command : {"Q1", "QT", "T"}) {
            const auto found = history.find(command);
            if (found == history.end()) continue;
            EnergyPollInfo info{};
            check(energy_poll_info(protocol, command, info) && info.interval_ms == 10000 &&
                  !info.primary, "diagnostic and clock reads fit existing live freshness");
            check(found->second.size() >= 25 && found->second.size() <= 37,
                  "diagnostic and clock reads continue at their slower cadence");
        }
    }

    EnergyPollScheduler schedule;
    schedule.reset(EnergyProtocol::pi30, 1);
    const auto first = run(schedule, 0, 119000, 100);
    check(first.at("QPIRI").size() == 1 && first.at("QFLAG").size() == 1,
          "unchanged current settings are not reread every minute");
    schedule.refresh_configuration(119100);
    schedule.observe("QPIRI", true, 119100);
    const auto refreshed = run(schedule, 119100, 140000, 100);
    check(!refreshed.count("QPIRI") && refreshed.count("QFLAG"),
          "direct settings readback coalesces only its own periodic refresh");
}
void bluetooth_prerequisite()
{
    for (auto protocol : {EnergyProtocol::jk02, EnergyProtocol::jk02_32, EnergyProtocol::jk04, EnergyProtocol::jkv11}) {
        EnergyPollScheduler schedule;
        schedule.reset(protocol, 1);
        check(command_is(schedule.next(0), "getInfo"), "JK notification setup goes first");
        schedule.record(false, 0);
        check(!schedule.next(1999), "failed prerequisite gates measurements during backoff");
        check(command_is(schedule.next(2000), "getInfo"), "prerequisite retries after delay");
        schedule.record(true, 2000);
        check(command_is(schedule.next(2000), "getCellData"), "valid prerequisite unlocks measurements");
        schedule.record(true, 2000);
        const auto history = run(schedule, 3000, 120000);
        check(history.count("getInfo") == 0 && history.at("getCellData").size() >= 50,
              "JK info once and measurements continuously");
        schedule.reset(protocol, 2, 121000);
        check(command_is(schedule.next(121000), "getInfo"), "JK setup repeated after reconnect");
    }
}
void manual_query_classification()
{
    EnergyPollInfo info{};
    check(energy_poll_info(EnergyProtocol::pi30, "QPGS0", info) &&
          info.poll_class == EnergyPollClass::live && !info.primary,
          "explicit parallel query classified without being automatically assumed");
    check(energy_poll_info(EnergyProtocol::pi30max, "QED20261011", info) &&
          info.poll_class == EnergyPollClass::energy, "explicit historical query is energy");
    check(energy_poll_info(EnergyProtocol::pi30, "QMCHGCR", info) &&
          info.poll_class == EnergyPollClass::configuration && info.interval_ms == 0,
          "charging range is a static device capability, not live current");
    check(!energy_poll_info(EnergyProtocol::pi30, "POP02", info), "setter excluded from telemetry schedule");
    check(!energy_poll_info(EnergyProtocol::pi30, "invented", info), "unknown query not classified as valid telemetry");
    check(energy_poll_info(EnergyProtocol::jkpb, "getBalancerData", info) &&
          info.poll_class == EnergyPollClass::identity, "JKPB legacy-named balancer read is actual identity");
}
void supplemental_telemetry()
{
    auto require = [](EnergyProtocol protocol, const char *command, EnergyPollClass poll_class,
                      bool primary = false) {
        EnergyPollInfo info{};
        bool scheduled = false;
        for (std::size_t i = 0; i < energy_background_poll_count(protocol); ++i)
            if (energy_background_poll_command(protocol, i, info) && !std::strcmp(info.command, command)) {
                scheduled = true;
                check(info.poll_class == poll_class && info.primary == primary,
                      "supplemental measurement has correct scheduler class");
                break;
            }
        check(scheduled, "supplemental measurement runs automatically without a user command");
    };
    require(EnergyProtocol::pi16, "QPIBI", EnergyPollClass::live);
    for (auto id : {EnergyProtocol::pi17, EnergyProtocol::pi17m058, EnergyProtocol::pi17infini}) {
        require(id, "PS", EnergyPollClass::live, true);
        for (const char *query : {"INGS", "EMINFO", "BMS"}) require(id, query, EnergyPollClass::live);
        require(id, "DM", EnergyPollClass::identity);
        require(id, "HECS", EnergyPollClass::configuration);
    }
    for (auto id : {EnergyProtocol::pi30, EnergyProtocol::pi30max, EnergyProtocol::pi30mst,
                    EnergyProtocol::pi30m044, EnergyProtocol::pi30m045, EnergyProtocol::pi41}) {
        require(id, "QBMS", EnergyPollClass::live);
        require(id, "Q1", EnergyPollClass::live);
    }
    require(EnergyProtocol::pi30revo, "QLITH0", EnergyPollClass::live);
    for (auto id : {EnergyProtocol::pi30, EnergyProtocol::pi30max, EnergyProtocol::pi30mst, EnergyProtocol::pi30m044,
                    EnergyProtocol::pi30m045, EnergyProtocol::pi41})
        require(id, "QPIGS2", EnergyPollClass::live);
    for (auto id : {EnergyProtocol::pi18lvx, EnergyProtocol::pi18sv}) {
        require(id, "ACCT", EnergyPollClass::configuration);
        require(id, "ACLT", EnergyPollClass::configuration);
    }
    require(EnergyProtocol::ved, "batteryCapacity", EnergyPollClass::configuration);

    EnergyPollScheduler schedule;
    schedule.reset(EnergyProtocol::pi30max, 1);
    unsigned rejected_bms = 0, core = 0, pv2 = 0;
    for (std::uint64_t now = 0; now <= 120000; now += 250) {
        const auto *poll = schedule.next(now);
        if (!poll) continue;
        if (command_is(poll, "QBMS")) ++rejected_bms;
        if (command_is(poll, "QPIGS")) ++core;
        if (command_is(poll, "QPIGS2")) ++pv2;
        schedule.record(!command_is(poll, "QBMS"), now, CodecStatus::rejected);
    }
    check(rejected_bms == 1 && core >= 50 && pv2 >= 15,
          "unsupported optional battery queries stop while inverter and second tracker stay live");
}
void pi30_optional_second_tracker()
{
    EnergyPollInfo classified{};
    check(energy_poll_info(EnergyProtocol::pi30, "QPIGS2", classified) &&
          classified.poll_class == EnergyPollClass::live && !classified.primary &&
          !classified.publish_on_change && classified.interval_ms == 5000 &&
          classified.minimum_pv_inputs == 2,
          "base PI30 second tracker is a supplemental repeated live observation");
    for (const auto protocol : {EnergyProtocol::pi30max, EnergyProtocol::pi30mst,
                               EnergyProtocol::pi30m044, EnergyProtocol::pi30m045}) {
        check(energy_poll_info(protocol, "QPIGS2", classified) && classified.minimum_pv_inputs == 0,
              "existing specialized PI30 profiles retain their complete measurement query");
        EnergyPollScheduler schedule;
        schedule.reset(protocol, 1, 0, 1);
        check(run(schedule, 0, 30000, 250).count("QPIGS2") != 0,
              "the base PI30 PV-count condition does not change existing specialized profiles");
    }
    check(energy_poll_info(EnergyProtocol::pi41, "QPIGS2", classified) &&
          classified.minimum_pv_inputs == 0,
          "PI41 QPIGS2 is a second output phase and is independent of PV count");
    EnergyPollScheduler pi41;
    pi41.reset(EnergyProtocol::pi41, 1, 0, 0);
    check(run(pi41, 0, 30000, 250).count("QPIGS2") != 0,
          "PI41 continues reading its second phase even when no PV is connected");
    for (const std::uint8_t pv_count : {0, 1, 2, 3, 16}) {
        EnergyPollScheduler schedule;
        schedule.reset(EnergyProtocol::pi30, pv_count + 1, 1000, pv_count);
        const bool enabled = pv_count >= 2;
        check(schedule.count() == energy_background_poll_count(EnergyProtocol::pi30) - (enabled ? 0 : 1),
              "only configured second tracker changes the base PI30 schedule size");
        const auto history = run(schedule, 1000, 121000, 250);
        check((history.count("QPIGS2") != 0) == enabled,
              "second tracker is polled only when at least two PV inputs are configured");
        check(history.at("QPIGS").size() >= 50 && history.at("QPI").size() == 1 &&
              history.at("QVFW2").size() == 1,
              "PV count never suppresses normal readings or identity");
        if (enabled) {
            const auto &tracker = history.at("QPIGS2");
            check(tracker.size() >= 15, "second tracker is refreshed regularly");
            for (std::size_t i = 1; i < tracker.size(); ++i)
                check(tracker[i] - tracker[i - 1] >= 5000 && tracker[i] - tracker[i - 1] <= 15000,
                      "supplemental tracker honors its cadence without metadata starvation");
        }
    }

    for (const auto failure : {CodecStatus::rejected, CodecStatus::unsupported,
                              CodecStatus::invalid_command, CodecStatus::invalid_frame}) {
        EnergyPollScheduler schedule;
        schedule.reset(EnergyProtocol::pi30, 1, 0, 2);
        unsigned attempts = 0, core = 0;
        std::vector<std::uint64_t> attempt_times;
        const bool transient = failure == CodecStatus::invalid_frame;
        for (std::uint64_t now = 0; now <= 180000; now += 250) {
            const auto *poll = schedule.next(now);
            if (!poll) continue;
            const bool tracker = command_is(poll, "QPIGS2");
            if (tracker) { ++attempts; attempt_times.push_back(now); }
            if (command_is(poll, "QPIGS")) ++core;
            schedule.record(!tracker, now, failure);
            if (tracker && !transient) schedule.refresh_live(now);
        }
        check(attempts == (transient ? 3U : 1U),
              "unsupported PV2 stops immediately; transient PV2 failures have bounded attempts");
        check(core >= 70, "failed PV2 never stops primary PI30 telemetry");
        if (transient)
            check(attempt_times[1] - attempt_times[0] >= 10000 &&
                  attempt_times[2] - attempt_times[1] >= 60000,
                  "PV2 transient retry backs off instead of thrashing the inverter");
        schedule.reset(EnergyProtocol::pi30, 2, 200000, 2);
        check(run(schedule, 200000, 230000, 250).count("QPIGS2") != 0,
              "a new USB session probes the configured second tracker again");
        schedule.reset(EnergyProtocol::pi30, 3, 240000, 1);
        check(run(schedule, 240000, 270000, 250).count("QPIGS2") == 0,
              "a setup change back to one PV removes second tracker reads");
    }
}
void daly_read_context()
{
    // Manufacturer 0x94 status: 16 cells and 2 temperature sensors, checksum54.
    const std::uint8_t status_frame[] = {0xa5, 1, 0x94, 8, 16, 2, 0, 0, 0, 0, 0, 0, 0x54};
    for (auto id : {EnergyProtocol::daly, EnergyProtocol::daly40}) {
        EnergyPollScheduler schedule;
        schedule.reset(id, 1);
        auto history = run(schedule, 0, 20000, 250);
        check(!history.count("cellVoltages") && !history.count("cellTemperatures") && history.count("status"),
              "Daly learns counts before any automatic group query");
        EnergyResponse response{};
        check(energy_decode_response(id, "status", 1, status_frame, sizeof(status_frame), response) == CodecStatus::ok,
              "concrete Daly status fixture supplies group context");
        for (std::size_t i = 0; i < response.count; ++i) {
            const auto &field = response.fields[i];
            if (!std::strcmp(field.key, "number_of_cells")) {
                check(field.numeric && field.value == 16, "Daly decoded exact cell count");
                schedule.set_read_context_ready("cellVoltages", field.value > 0, 21000);
            }
        }
        history = run(schedule, 21000, 45000, 250);
        check(history.count("cellVoltages") && !history.count("cellTemperatures"),
              "each group activates independently when its count becomes known");
        schedule.set_read_context_ready("cellTemperatures", true, 46000);
        history = run(schedule, 46000, 70000, 250);
        check(history.count("cellTemperatures"), "known temperature count enables normal temperature telemetry");
        unsigned rejected = 0;
        for (std::uint64_t now = 71000; now <= 110000; now += 250) {
            const auto *poll = schedule.next(now);
            if (!poll) continue;
            const bool cells = command_is(poll, "cellVoltages");
            if (cells) ++rejected;
            schedule.record(!cells, now, CodecStatus::rejected);
            schedule.set_read_context_ready("cellVoltages", true, now);
        }
        check(rejected == 1, "unchanged status count does not reactivate an unsupported cell query");
        schedule.set_read_context_ready("cellVoltages", false, 111000);
        history = run(schedule, 111000, 120000, 250);
        check(!history.count("cellVoltages"), "zero or lost cell context gates the cell query");
        schedule.set_read_context_ready("cellVoltages", true, 121000);
        check(run(schedule, 121000, 130000, 250).count("cellVoltages"),
              "a newly acquired valid context allows a fresh bounded probe");
        schedule.reset(id, 2, 131000);
        history = run(schedule, 131000, 151000, 250);
        check(!history.count("cellVoltages") && !history.count("cellTemperatures"),
              "reconnect forgets previous device counts");
    }
}
} // namespace

int main()
{
    profile_coverage();
    pi30_frequency();
    identity_failures_and_write_refresh();
    economical_refresh_policy();
    bluetooth_prerequisite();
    manual_query_classification();
    supplemental_telemetry();
    pi30_optional_second_tracker();
    daly_read_context();
    std::printf("Energy poll schedule: %u checks passed across all 25 native profiles\n", checks);
}
