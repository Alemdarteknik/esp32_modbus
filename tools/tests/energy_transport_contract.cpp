// Exercises the actual transport receive/deadline helper with deterministic
// bytes and the actual HID descriptor validator. No firmware/network/hardware.
#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>

#include "freertos/stream_buffer.h"
TickType_t host_ticks = 0;
std::function<void()> host_after_send, host_after_receive, host_on_empty;
#include "../../main/src/transport/byte_transport_detail.hpp"
#include "inverter_gateway/transport/hid_report_layout.hpp"

using namespace inverter_gateway::transport;
using Bytes = std::vector<std::uint8_t>;

FrameDecision terminated(const std::uint8_t *data, std::size_t size, void *)
{
    return data[size - 1] == '\r' ? FrameDecision::complete : FrameDecision::incomplete;
}

int main()
{
    detail::ReceivePipe pipe;
    pipe.stream = xStreamBufferCreate(32, 1);
    const ReceiveRule rule{terminated, nullptr};
    std::uint8_t response[32]{};
    std::uint32_t generation = 0;
    std::size_t received = 0;
    bool online = true;
    const auto ready = [&] { return online; };
    const auto inject = [&](Bytes bytes) { pipe.append(bytes.data(), bytes.size()); };
    const auto prepare = [&] {
        host_after_send = {}; host_after_receive = {}; host_on_empty = {};
        host_ticks = 0; received = 0; online = true;
        assert(pipe.prepare(generation));
    };

    // Binary zero bytes are preserved rather than mistaken for HID padding.
    prepare(); inject({0x55, 0, 0xaa, '\r'});
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(50), rule, ready) == ESP_OK);
    assert(received == 4 && response[1] == 0 && response[2] == 0xaa);

    prepare(); inject({'(', 'A'});
    host_on_empty = [&] { inject({'C', 'K', '\r'}); };
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(50), rule, ready) == ESP_OK && received == 5);

    // A partial or absent reply consumes one shared deadline, never succeeds.
    prepare(); inject({'(', 'A'});
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(37), rule, ready) == ESP_ERR_TIMEOUT);
    assert(received == 2 && host_ticks == 37);
    prepare();
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(37), rule, ready) == ESP_ERR_TIMEOUT && received == 0);

    prepare(); inject({'a', 'b', 'c'});
    assert(detail::receive(pipe, generation, response, 2, received,
        detail::Deadline(50), rule, ready) == ESP_ERR_INVALID_SIZE && received == 2);

    // A valid prefix inside an overflowing callback is never accepted.
    prepare(); Bytes overflowing{'(', 'A', 'C', 'K', '\r'}; overflowing.resize(80, 0xa5);
    inject(overflowing);
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(50), rule, ready) == ESP_ERR_INVALID_RESPONSE);

    prepare(); inject({'a', '\r'}); unsigned count = 0;
    host_after_receive = [&] { if (++count == 2) inject(Bytes(80, 0xa5)); };
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(50), rule, ready) == ESP_ERR_INVALID_RESPONSE);

    prepare(); inject({'a', '\r'});
    host_after_receive = [&] { online = false; };
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(50), rule, ready) == ESP_ERR_INVALID_STATE);

    prepare(); inject({'a', '\r'});
    ReceiveRule overflowing_rule{[](const std::uint8_t *, std::size_t, void *context) {
        static_cast<detail::ReceivePipe *>(context)->invalidate();
        return FrameDecision::complete;
    }, &pipe};
    assert(detail::receive(pipe, generation, response, sizeof(response), received,
        detail::Deadline(50), overflowing_rule, ready) == ESP_ERR_INVALID_RESPONSE);

    // Stream draining is bounded even when a producer injects at every read.
    prepare(); inject({'a', 'b'}); count = 0;
    host_after_receive = [&] { ++count; inject({'x'}); };
    assert(!pipe.prepare(generation) && count == 1);
    host_after_receive = {};
    pipe.callback_sequence.store(1);
    assert(!pipe.prepare(generation));
    pipe.callback_sequence.store(2);
    assert(pipe.prepare(generation));

    HostSemaphore mutex{false}; host_ticks = 0;
    assert(!detail::take_until(&mutex, detail::Deadline(25)) && host_ticks == 25);
    mutex.available = true; host_ticks = 0;
    assert(detail::take_until(&mutex, detail::Deadline(25)) && host_ticks == 0);
    assert(detail::valid_exchange(nullptr, 0, response, 32, 50, rule));
    assert(!detail::valid_exchange(nullptr, 1, response, 32, 50, rule));
    assert(!detail::valid_exchange(response, 1025, response, 32, 50, rule));
    assert(!detail::valid_exchange(response, 1, response, 1025, 50, rule));
    assert(!detail::valid_exchange(response, 1, response, 32, 60001, rule));
    assert(!detail::valid_exchange(response, 1, response, 32, 0, rule));
    assert(!detail::valid_exchange(response, 1, response, 32, 50, {}));

    const Bytes descriptor{0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
        0x75, 0x08, 0x95, 0x08, 0x09, 0x01, 0x81, 0x02,
        0x09, 0x02, 0x91, 0x02, 0xc0};
    assert(is_eight_byte_hid_pipe(descriptor.data(), descriptor.size()));
    auto wrong = descriptor; wrong.insert(wrong.begin() + 7, {0x85, 0x01});
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Report ID.
    wrong = descriptor; wrong[10] = 9;
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Larger report.
    wrong = descriptor; wrong[8] = 7;
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Bit packed.
    wrong = descriptor; wrong[14] = 1;
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Constant input.
    wrong = descriptor; wrong[17] = 0x81;
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Missing output.
    wrong = descriptor; wrong.push_back(0x75);
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Truncated item.
    wrong = descriptor; wrong.insert(wrong.begin(), 0xb4);
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Stack underflow.
    wrong = descriptor; wrong.insert(wrong.begin(), 5, 0xa4);
    assert(!is_eight_byte_hid_pipe(wrong.data(), wrong.size())); // Stack overflow.
    assert(!is_eight_byte_hid_pipe(nullptr, 0));
    std::puts("Energy transport deadline, overflow, framing and HID descriptor checks passed.");
}
