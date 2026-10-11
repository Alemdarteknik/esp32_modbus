#pragma once

#include <cstddef>
#include <cstdint>

namespace inverter_gateway::network::coordinator_policy {

// Local UART forwarding admission only; these fields never go on the wire.
// No remote command deadline, cryptographic session, or exactly-once guarantee.
inline constexpr std::uint32_t command_queue_max_age_ms = 5000;
inline constexpr std::uint32_t transmit_deadline_ms = 1000;

struct Ticket {
    std::uint32_t generation = 0;
    std::uint32_t queued_at_ms = 0;
};

class LinkSession {
public:
    bool online(std::uint32_t now, std::uint32_t timeout)
    {
        if (online_ && static_cast<std::uint32_t>(now - last_receive_ms_) >= timeout) {
            online_ = false;
            ++generation_;
        }
        return online_;
    }

    void received(std::uint32_t now, std::uint32_t timeout)
    {
        // Detect a gap even if no observer ran while the peer was absent.
        if (!online(now, timeout)) ++generation_;
        online_ = true;
        last_receive_ms_ = now;
    }

    void invalidate()
    {
        online_ = false;
        ++generation_;
    }

    bool received_command(std::uint32_t now, std::uint32_t timeout)
    {
        // Unlike a heartbeat, a command cannot establish a new session.
        if (!online(now, timeout)) return false;
        last_receive_ms_ = now;
        return true;
    }

    Ticket ticket(std::uint32_t now) const { return {generation_, now}; }

    bool accepts(const Ticket &ticket, std::uint32_t now, std::uint32_t timeout)
    {
        return online(now, timeout) && ticket.generation == generation_ &&
            static_cast<std::uint32_t>(now - ticket.queued_at_ms) < command_queue_max_age_ms;
    }

private:
    std::uint32_t generation_ = 1;
    std::uint32_t last_receive_ms_ = 0;
    bool online_ = false;
};

enum class SendDisposition { not_sent, uncertain, forwarded };

constexpr SendDisposition classify_send(bool uart_called, int written,
                                        std::size_t expected, bool tx_complete)
{
    if (!uart_called || written == 0) return SendDisposition::not_sent;
    // A negative/partial write or a completion timeout is not safe to retry.
    if (written < 0 || static_cast<std::size_t>(written) != expected || !tx_complete)
        return SendDisposition::uncertain;
    return SendDisposition::forwarded; // Not acknowledgement from the inverter.
}

enum class TransmitAdmission { ready, wait, rejected, deadline };
enum class TransmitCompletion { pending, complete, error };
enum class TransmitStop { none, deadline, rejected, io_error };

struct TransmitChunk {
    TransmitAdmission admission;
    int accepted_bytes = 0;
};

struct TransmitResult {
    SendDisposition disposition;
    TransmitStop stop;
    std::size_t accepted_bytes;
};

// The injected push/completion operations must not wait for space or locks.
// Push combines the session check with immediate FIFO admission atomically;
// it must also validate admission when the remaining byte count is zero.
// One deadline covers admission, every FIFO chunk, and physical drain.
// This exact pump is shared by the UART worker and deterministic host tests.
template <typename Clock, typename Push, typename Complete, typename Yield>
TransmitResult transmit_bounded(const std::uint8_t *data, std::size_t size,
                                 std::uint32_t started_at_ms, std::uint32_t budget_ms,
                                 Clock clock, Push push,
                                 Complete complete, Yield yield)
{
    std::size_t accepted = 0;
    const auto abort = [&](TransmitStop stop, bool ambiguous = false) {
        return TransmitResult{accepted != 0 || ambiguous ? SendDisposition::uncertain
                                                        : SendDisposition::not_sent,
                              stop, accepted};
    };
    while (true) {
        const auto now = clock();
        if (static_cast<std::uint32_t>(now - started_at_ms) >= budget_ms)
            return abort(TransmitStop::deadline);
        const auto chunk = push(now, data + accepted, size - accepted);
        if (chunk.admission == TransmitAdmission::rejected) return abort(TransmitStop::rejected);
        if (chunk.admission == TransmitAdmission::deadline) return abort(TransmitStop::deadline);
        if (chunk.admission == TransmitAdmission::wait) {
            yield();
            continue;
        }
        if (chunk.accepted_bytes < 0 || static_cast<std::size_t>(chunk.accepted_bytes) > size - accepted)
            return abort(TransmitStop::io_error, true);
        accepted += static_cast<std::size_t>(chunk.accepted_bytes);
        // Even a platform operation unexpectedly running past its budget must
        // not result in success. Already accepted bytes remain uncertain.
        if (static_cast<std::uint32_t>(clock() - started_at_ms) >= budget_ms)
            return abort(TransmitStop::deadline);
        if (accepted == size) {
            const auto completion = complete();
            if (completion == TransmitCompletion::error) return abort(TransmitStop::io_error);
            if (static_cast<std::uint32_t>(clock() - started_at_ms) >= budget_ms)
                return abort(TransmitStop::deadline);
            if (completion == TransmitCompletion::complete)
                return {SendDisposition::forwarded, TransmitStop::none, accepted};
        }
        // A full/stalled FIFO or pending TX completion must never busy-spin.
        yield();
    }
}

} // namespace inverter_gateway::network::coordinator_policy
