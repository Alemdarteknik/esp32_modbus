#include <cassert>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <cstdlib>
#include <windows.h>
#endif

#include "inverter_gateway/network/protocol_command_history.hpp"

using namespace inverter_gateway;
using namespace inverter_gateway::network;

ProtocolCommand request(std::uint64_t id, bool setter = true)
{
    ProtocolCommand command{};
    command.command_id = id;
    command.target_member_id = 3;
    command.origin = RemoteCommandOrigin::mqtt;
    command.protocol = protocol::EnergyProtocol::pi30;
    command.write = command.confirmed = setter;
    std::strcpy(command.command, setter ? "POP02" : "QPIGS");
    return command;
}
ProtocolCommandResult result(const ProtocolCommand &command)
{
    ProtocolCommandResult response{};
    response.command_id = command.command_id;
    response.source_member_id = command.target_member_id;
    response.protocol = command.protocol;
    response.outcome = command.write ? ProtocolCommandOutcome::acknowledged : ProtocolCommandOutcome::completed;
    response.write_sent = command.write;
    std::strcpy(response.command, command.command);
    std::strcpy(response.detail, "Original inverter result");
    return response;
}

int main()
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
#endif
    ProtocolCommandHistory history;
    ProtocolCommandResult previous{};
    const auto write = request(1);
    assert(!history.lookup(write, previous));
    assert(history.reserve(write));
    assert(!history.reserve(write));
    assert(history.lookup(write, previous));
    assert(previous.command_id == 1 && previous.outcome == ProtocolCommandOutcome::uncertain);
    assert(!previous.write_sent_known && !previous.write_sent);
    assert(std::strstr(previous.detail, "await the original") != nullptr);
    // Pending includes both queued and executing; there is no second admission.
    assert(history.lookup(write, previous) && !history.reserve(write));
    auto changed = write;
    std::strcpy(changed.command, "POP00");
    assert(history.lookup(changed, previous) && previous.outcome == ProtocolCommandOutcome::rejected);
    assert(previous.write_sent_known && !previous.write_sent && !history.reserve(changed));
    changed = write; changed.protocol = protocol::EnergyProtocol::pi30max;
    assert(history.lookup(changed, previous) && previous.outcome == ProtocolCommandOutcome::rejected);
    changed = write; ++changed.target_member_id;
    assert(history.lookup(changed, previous) && previous.outcome == ProtocolCommandOutcome::rejected);
    changed = write; changed.write = false;
    assert(history.lookup(changed, previous) && previous.outcome == ProtocolCommandOutcome::rejected);

    const auto original = result(write);
    history.complete(original);
    assert(history.lookup(write, previous));
    assert(previous.command_id == original.command_id && previous.source_member_id == original.source_member_id &&
        previous.protocol == original.protocol && previous.outcome == original.outcome &&
        previous.write_sent == original.write_sent && previous.write_sent_known == original.write_sent_known &&
        std::strcmp(previous.command, original.command) == 0 && std::strcmp(previous.detail, original.detail) == 0);
    auto duplicate_completion = original;
    duplicate_completion.outcome = ProtocolCommandOutcome::uncertain;
    history.complete(duplicate_completion);
    assert(history.lookup(write, previous) && previous.outcome == ProtocolCommandOutcome::acknowledged);
    assert(history.lookup(changed, previous) && previous.outcome == ProtocolCommandOutcome::rejected);

    const auto getter = request(2, false);
    assert(history.reserve(getter) && history.lookup(getter, previous));
    assert(previous.outcome == ProtocolCommandOutcome::unavailable && previous.write_sent_known);
    history.complete(result(getter));
    assert(history.lookup(getter, previous) && previous.outcome == ProtocolCommandOutcome::completed);

    // Mismatched/corrupt completion cannot release a pending reservation.
    const auto pending = request(3);
    assert(history.reserve(pending));
    auto bad = result(pending);
    bad.source_member_id = 4; history.complete(bad);
    bad = result(pending); bad.protocol = protocol::EnergyProtocol::pi18; history.complete(bad);
    bad = result(pending); std::strcpy(bad.command, "POP00"); history.complete(bad);
    bad = result(pending); std::memset(bad.detail, 'x', sizeof(bad.detail)); history.complete(bad);
    bad = result(pending); std::memset(bad.command, 'x', sizeof(bad.command)); history.complete(bad);
    bad = result(pending); bad.outcome = static_cast<ProtocolCommandOutcome>(255); history.complete(bad);
    bad = result(pending); bad.command_id = 999; history.complete(bad);
    assert(history.lookup(pending, previous) && previous.outcome == ProtocolCommandOutcome::uncertain);

    ProtocolCommandHistory full;
    for (unsigned i = 1; i <= ProtocolCommandHistory::capacity; ++i) assert(full.reserve(request(i)));
    assert(!full.reserve(request(17))); // All pending: none can be evicted.
    for (unsigned i = 1; i <= ProtocolCommandHistory::capacity; ++i)
        assert(full.lookup(request(i), previous) && previous.outcome == ProtocolCommandOutcome::uncertain);
    full.complete(result(request(7))); // The only completed entry can be evicted.
    assert(full.reserve(request(17)));
    assert(!full.lookup(request(7), previous));
    assert(full.lookup(request(1), previous) && !full.reserve(request(18)));

    ProtocolCommandHistory completed;
    for (unsigned i = 1; i <= ProtocolCommandHistory::capacity; ++i) assert(completed.reserve(request(i)));
    // Completion order, rather than reservation order, controls eviction.
    for (unsigned i = 16; i >= 1; --i) completed.complete(result(request(i)));
    assert(completed.reserve(request(17)));
    assert(!completed.lookup(request(16), previous));
    assert(completed.lookup(request(1), previous));
    assert(completed.reserve(request(18)));
    assert(!completed.lookup(request(15), previous));
    completed.complete(result(request(17)));
    completed.complete(result(request(18)));
    assert(completed.reserve(request(19)) && !completed.lookup(request(14), previous));

    auto invalid = request(0);
    assert(!history.reserve(invalid));
    invalid = request(4); invalid.origin = RemoteCommandOrigin::none;
    assert(!history.reserve(invalid));
    invalid = request(4); invalid.protocol = protocol::EnergyProtocol::count;
    assert(!history.reserve(invalid));
    invalid = request(4); invalid.confirmed = false;
    assert(!history.reserve(invalid));
    invalid = request(4); std::memset(invalid.command, 'x', sizeof(invalid.command));
    assert(!history.reserve(invalid));
    invalid = write; std::memset(invalid.command, 'x', sizeof(invalid.command));
    assert(history.lookup(invalid, previous) && previous.outcome == ProtocolCommandOutcome::rejected);
    assert(previous.command[79] == '\0');
    invalid = request(4); invalid.command[0] = '\0';
    assert(!history.reserve(invalid));
    auto padded = write; padded.command[20] = 'x';
    assert(history.lookup(padded, previous) && previous.outcome == ProtocolCommandOutcome::acknowledged);

    // A late ACK from an uncertain write must not acknowledge a later setter.
    // A valid read in the same session does not remove that uncertainty.
    ProtocolCommandHistory guarded;
    const auto first_write = request(100);
    const auto later_write = request(101);
    const auto intervening_read = request(102, false);
    assert(!guarded.writes_blocked() && guarded.reserve(first_write));
    auto uncertain_write = result(first_write);
    uncertain_write.outcome = ProtocolCommandOutcome::uncertain;
    guarded.complete(uncertain_write);
    assert(guarded.writes_blocked());
    assert(guarded.lookup(first_write, previous) && previous.outcome == ProtocolCommandOutcome::uncertain);
    assert(previous.write_sent_known && previous.write_sent && !guarded.reserve(first_write));
    assert(guarded.reserve(intervening_read));
    guarded.complete(result(intervening_read));
    assert(guarded.writes_blocked() && !guarded.reserve(later_write));
    guarded.new_connection_session();
    assert(!guarded.writes_blocked() && guarded.reserve(later_write));
    assert(guarded.lookup(first_write, previous) && previous.outcome == ProtocolCommandOutcome::uncertain);
    assert(previous.write_sent_known && previous.write_sent && !guarded.reserve(first_write));
    guarded.complete(result(later_write));
    assert(!guarded.writes_blocked());

    // Only an actual completed setter that may have been sent latches the block.
    ProtocolCommandHistory no_send;
    const auto never_sent = request(103);
    assert(no_send.reserve(never_sent));
    auto not_transmitted = result(never_sent);
    not_transmitted.outcome = ProtocolCommandOutcome::uncertain;
    not_transmitted.write_sent = false;
    no_send.complete(not_transmitted);
    assert(!no_send.writes_blocked());
    const auto no_write = request(104, false);
    assert(no_send.reserve(no_write));
    auto uncertain_read = result(no_write);
    uncertain_read.outcome = ProtocolCommandOutcome::uncertain;
    uncertain_read.write_sent = true;
    no_send.complete(uncertain_read);
    assert(!no_send.writes_blocked());
    const auto unknown_submission = request(105);
    assert(no_send.reserve(unknown_submission));
    auto unknown_result = result(unknown_submission);
    unknown_result.outcome = ProtocolCommandOutcome::uncertain;
    unknown_result.write_sent_known = false;
    no_send.complete(unknown_result);
    assert(!no_send.writes_blocked());
    std::printf("Native command history checks passed (%zu bytes, 16 bounded entries).\n", sizeof(ProtocolCommandHistory));
}
