#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include "freertos/task.h"
struct HostStream { std::size_t capacity; std::deque<std::uint8_t> bytes; };
using StreamBufferHandle_t = HostStream *;
extern std::function<void()> host_after_send, host_after_receive, host_on_empty;
inline StreamBufferHandle_t xStreamBufferCreate(std::size_t capacity, std::size_t) {
    static HostStream stream{}; stream.capacity = capacity; stream.bytes.clear(); return &stream;
}
inline std::size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t stream) { return stream->capacity - stream->bytes.size(); }
inline std::size_t xStreamBufferBytesAvailable(StreamBufferHandle_t stream) { return stream->bytes.size(); }
inline std::size_t xStreamBufferSend(StreamBufferHandle_t stream, const void *data, std::size_t count, TickType_t) {
    const auto stored = std::min(count, xStreamBufferSpacesAvailable(stream));
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0; i < stored; ++i) stream->bytes.push_back(bytes[i]);
    if (host_after_send) { auto callback = std::move(host_after_send); host_after_send = {}; callback(); }
    return stored;
}
inline std::size_t xStreamBufferReceive(StreamBufferHandle_t stream, void *data, std::size_t count, TickType_t wait) {
    if (stream->bytes.empty() && wait && host_on_empty) { auto callback = std::move(host_on_empty); host_on_empty = {}; callback(); }
    const auto received = std::min(count, stream->bytes.size());
    auto *bytes = static_cast<std::uint8_t *>(data);
    for (std::size_t i = 0; i < received; ++i) { bytes[i] = stream->bytes.front(); stream->bytes.pop_front(); }
    if (!received) host_ticks += wait;
    if (received && host_after_receive) host_after_receive();
    return received;
}
