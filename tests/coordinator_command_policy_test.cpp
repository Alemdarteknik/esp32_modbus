#include <cassert>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "inverter_gateway/network/coordinator_command_policy.hpp"

using namespace inverter_gateway::network::coordinator_policy;

namespace {

struct FakeFifo {
    std::uint32_t now = 0;
    std::uint32_t release_at = 0;
    std::size_t capacity = 0;
    std::size_t pushed = 0;
    unsigned push_calls = 0;
    unsigned completion_calls = 0;
    unsigned yields = 0;
    bool return_error = false;
    bool completion_error = false;
    bool complete = false;
    std::array<std::uint8_t, 32> accepted{};

    int push(const std::uint8_t *bytes, std::size_t size)
    {
        ++push_calls;
        if (return_error) return -1;
        if (now < release_at) return 0;
        const auto count = std::min(capacity, size);
        assert(pushed + count <= accepted.size());
        std::copy_n(bytes, count, accepted.data() + pushed);
        pushed += count;
        capacity -= count;
        return static_cast<int>(count);
    }
};

template <typename Admit, typename OnYield>
TransmitResult pump(FakeFifo &fifo, const std::array<std::uint8_t, 8> &bytes,
                     std::uint32_t start, Admit admit, OnYield on_yield)
{
    return transmit_bounded(bytes.data(), bytes.size(), start, 1000,
        [&] { return fifo.now; },
        [&](std::uint32_t now, const std::uint8_t *data, std::size_t size) -> TransmitChunk {
            const auto admission = admit(now);
            if (admission != TransmitAdmission::ready) return {admission};
            return {admission, size == 0 ? 0 : fifo.push(data, size)};
        },
        [&] {
            ++fifo.completion_calls;
            return fifo.completion_error ? TransmitCompletion::error :
                fifo.complete ? TransmitCompletion::complete : TransmitCompletion::pending;
        },
        [&] { ++fifo.yields; fifo.now += 10; on_yield(); });
}

void test_bounded_fifo_pump()
{
    using D = SendDisposition;
    constexpr std::array<std::uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
    const auto ready = [](std::uint32_t) { return TransmitAdmission::ready; };
    const auto no_action = [] {};

    // A permanently blocked FIFO never admits bytes; waits are bounded/yield.
    FakeFifo blocked;
    auto result = pump(blocked, bytes, 0, ready, no_action);
    assert(result.disposition == D::not_sent && result.stop == TransmitStop::deadline);
    assert(result.accepted_bytes == 0 && blocked.pushed == 0);
    assert(blocked.now == 1000 && blocked.yields == 100 && blocked.push_calls == 100);
    assert(blocked.completion_calls == 0);

    // A full FIFO after a prefix must not send/replay the suffix after timeout.
    FakeFifo partial;
    partial.capacity = 3;
    result = pump(partial, bytes, 0, ready, no_action);
    assert(result.disposition == D::uncertain && result.stop == TransmitStop::deadline);
    assert(result.accepted_bytes == 3 && partial.pushed == 3 && partial.now == 1000);
    assert(std::equal(bytes.begin(), bytes.begin() + 3, partial.accepted.begin()));

    // Entire frame admitted, but physical completion stalls: still uncertain.
    FakeFifo drain_stall;
    drain_stall.capacity = bytes.size();
    result = pump(drain_stall, bytes, 0, ready, no_action);
    assert(result.disposition == D::uncertain && result.accepted_bytes == bytes.size());
    assert(drain_stall.push_calls == 1 && drain_stall.completion_calls == 100);
    assert(drain_stall.now == 1000);

    // Chunks continue at their exact offsets, with no prefix replay.
    FakeFifo normal;
    normal.capacity = 3;
    result = pump(normal, bytes, 0, ready, [&] {
        normal.capacity = 3;
        normal.complete = normal.now >= 20;
    });
    assert(result.disposition == D::forwarded && result.stop == TransmitStop::none);
    assert(normal.pushed == bytes.size() && normal.push_calls == 3);
    assert(std::equal(bytes.begin(), bytes.end(), normal.accepted.begin()));

    // Nonblocking admission lock contention spends the same finite budget.
    FakeFifo locked;
    locked.capacity = bytes.size();
    result = pump(locked, bytes, 0,
        [](std::uint32_t) { return TransmitAdmission::wait; }, no_action);
    assert(result.disposition == D::not_sent && locked.push_calls == 0);
    assert(locked.now == 1000 && locked.yields == 100);

    // The send mutex's elapsed time counts: no fresh one-second window.
    FakeFifo mutex_delayed;
    mutex_delayed.now = 1000;
    mutex_delayed.capacity = bytes.size();
    result = pump(mutex_delayed, bytes, 0, ready, no_action);
    assert(result.disposition == D::not_sent && mutex_delayed.push_calls == 0);

    // An error after invoking UART is conservatively uncertain even at byte 0.
    FakeFifo failed;
    failed.return_error = true;
    result = pump(failed, bytes, 0, ready, no_action);
    assert(result.disposition == D::uncertain && result.stop == TransmitStop::io_error);
    assert(failed.push_calls == 1 && failed.yields == 0);

    // Known write admission followed by completion API failure is uncertain.
    FakeFifo completion_failed;
    completion_failed.capacity = bytes.size();
    completion_failed.completion_error = true;
    result = pump(completion_failed, bytes, 0, ready, no_action);
    assert(result.disposition == D::uncertain && result.stop == TransmitStop::io_error);

    // Queue expiry while the FIFO is blocked: never send once space arrives.
    LinkSession session;
    session.received(0, 10000);
    const auto ticket = session.ticket(0);
    const auto admissible = [&](std::uint32_t now) {
        return session.accepts(ticket, now, 10000) ? TransmitAdmission::ready
                                                  : TransmitAdmission::rejected;
    };
    FakeFifo expires;
    expires.now = 4990;
    expires.release_at = 5000;
    expires.capacity = bytes.size();
    result = pump(expires, bytes, 4990, admissible, no_action);
    assert(result.disposition == D::not_sent && result.stop == TransmitStop::rejected);
    assert(expires.push_calls == 1 && expires.pushed == 0 && expires.now == 5000);

    // Expiry after accepting a prefix is uncertain; do not finish the frame.
    FakeFifo prefix_expires;
    prefix_expires.now = 4990;
    prefix_expires.capacity = 3;
    result = pump(prefix_expires, bytes, 4990, admissible,
        [&] { prefix_expires.capacity = bytes.size(); });
    assert(result.disposition == D::uncertain && result.stop == TransmitStop::rejected);
    assert(prefix_expires.pushed == 3 && prefix_expires.push_calls == 1);

    // Reconnect between chunks cannot finish the old generation's command.
    FakeFifo reconnect;
    reconnect.capacity = 3;
    result = pump(reconnect, bytes, 0, admissible, [&] {
        session.invalidate();
        session.received(reconnect.now, 10000);
        reconnect.capacity = bytes.size();
    });
    assert(result.disposition == D::uncertain && result.stop == TransmitStop::rejected);
    assert(reconnect.pushed == 3 && reconnect.push_calls == 1);

    // TX deadline arithmetic also works across uint32 millisecond rollover.
    FakeFifo wrapped;
    wrapped.now = 0xfffffff0U;
    result = pump(wrapped, bytes, wrapped.now, ready, no_action);
    assert(result.disposition == D::not_sent && result.stop == TransmitStop::deadline);
    assert(wrapped.yields == 100);
}

} // namespace

int main()
{
    constexpr std::uint32_t timeout = 10000;
    LinkSession session;
    assert(!session.online(0, timeout));
    assert(!session.received_command(0, timeout));
    assert(!session.accepts(session.ticket(0), 0, timeout));

    // A heartbeat at uptime zero is valid; no zero-sentinel false offline.
    session.received(0, timeout);
    auto ticket = session.ticket(0);
    assert(session.accepts(ticket, 0, timeout));
    assert(session.accepts(ticket, 4999, timeout));
    assert(!session.accepts(ticket, 5000, timeout));
    // Further heartbeats do not extend the original queue deadline.
    session.received(5000, timeout);
    assert(!session.accepts(ticket, 5001, timeout));

    // Local TX failure/disconnect invalidation defeats a still-young ticket.
    ticket = session.ticket(5001);
    session.invalidate();
    assert(!session.accepts(ticket, 5002, timeout));
    session.received(5003, timeout);
    assert(!session.accepts(ticket, 5003, timeout));
    assert(session.accepts(session.ticket(5003), 5003, timeout));

    // Observed timeout, then reconnect, cannot replay the old generation.
    LinkSession short_session;
    short_session.received(100, 1000);
    ticket = short_session.ticket(200);
    assert(short_session.online(1099, 1000));
    assert(!short_session.online(1100, 1000));
    short_session.received(1101, 1000);
    assert(!short_session.accepts(ticket, 1101, 1000));

    // Even without a timeout observer, a delayed heartbeat observes the gap.
    ticket = short_session.ticket(1102);
    short_session.received(2101, 1000);
    assert(!short_session.accepts(ticket, 2101, 1000));

    // Stale incoming commands neither restore an expired session nor move its
    // last-receive time. A valid heartbeat is required before a new command.
    assert(!short_session.received_command(3101, 1000));
    assert(!short_session.received_command(3102, 1000));
    assert(!short_session.online(3102, 1000));
    short_session.received(3103, 1000);
    assert(short_session.received_command(3104, 1000));
    assert(short_session.online(4103, 1000));
    assert(!short_session.online(4104, 1000));

    // uint32 millisecond wrap: both queue age and link timeout are modular.
    constexpr std::uint32_t before_wrap = 0xfffffff0U;
    LinkSession wrapping;
    wrapping.received(before_wrap, timeout);
    ticket = wrapping.ticket(before_wrap);
    assert(wrapping.accepts(ticket, 0x20, timeout));
    assert(wrapping.accepts(ticket, before_wrap + std::uint32_t{4999}, timeout));
    assert(!wrapping.accepts(ticket, before_wrap + std::uint32_t{5000}, timeout));
    assert(wrapping.online(before_wrap + std::uint32_t{9999}, timeout));
    assert(!wrapping.online(before_wrap + std::uint32_t{10000}, timeout));
    wrapping.received(before_wrap + std::uint32_t{10001}, timeout);
    assert(!wrapping.accepts(ticket, before_wrap + std::uint32_t{10001}, timeout));

    // A deadline crossing while waiting for the send mutex must be rechecked.
    LinkSession delayed;
    delayed.received(0, timeout);
    ticket = delayed.ticket(0);
    assert(delayed.accepts(ticket, 4999, timeout));
    assert(!delayed.accepts(ticket, 5001, timeout));

    // UART acceptance is not remote/inverter acknowledgement. Any possibly
    // partial send or TX completion timeout must be uncertain, never rejected.
    using D = SendDisposition;
    static_assert(classify_send(false, 0, 32, false) == D::not_sent);
    static_assert(classify_send(false, -1, 32, false) == D::not_sent);
    static_assert(classify_send(true, 0, 32, false) == D::not_sent);
    static_assert(classify_send(true, -1, 32, false) == D::uncertain);
    static_assert(classify_send(true, 1, 32, false) == D::uncertain);
    static_assert(classify_send(true, 31, 32, true) == D::uncertain);
    static_assert(classify_send(true, 32, 32, false) == D::uncertain);
    static_assert(classify_send(true, 33, 32, true) == D::uncertain);
    static_assert(classify_send(true, 32, 32, true) == D::forwarded);

    test_bounded_fifo_pump();
    std::puts("PASS: coordinator command age, wrap, generation, incoming session and bounded FIFO transmission");
}
