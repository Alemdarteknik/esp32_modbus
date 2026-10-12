#pragma once

#include <algorithm>
#include <atomic>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "inverter_gateway/transport/energy_transport.hpp"

namespace inverter_gateway::transport::detail {

class Deadline {
public:
    explicit Deadline(std::uint32_t timeout_ms)
        : end_(esp_timer_get_time() + static_cast<std::int64_t>(timeout_ms) * 1000) {}
    bool expired() const { return esp_timer_get_time() >= end_; }
    std::int64_t end() const { return end_; }
    TickType_t ticks(std::uint32_t maximum_ms = 20) const
    {
        const auto us = end_ - esp_timer_get_time();
        if (us <= 0) return 0;
        const auto ms = std::min<std::int64_t>((us + 999) / 1000, maximum_ms);
        return std::max<TickType_t>(1, pdMS_TO_TICKS(ms));
    }
private:
    std::int64_t end_;
};

inline bool take_until(SemaphoreHandle_t mutex, const Deadline &deadline)
{
    while (!deadline.expired()) {
        if (xSemaphoreTake(mutex, deadline.ticks()) == pdTRUE) {
            if (!deadline.expired()) return true;
            xSemaphoreGive(mutex);
            return false;
        }
    }
    return false;
}

struct ReceivePipe {
    StreamBufferHandle_t stream = nullptr;
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::uint32_t> callback_sequence{0};

    void invalidate() { generation.fetch_add(1, std::memory_order_release); }
    void append(const std::uint8_t *data, std::size_t size)
    {
        callback_sequence.fetch_add(1, std::memory_order_acq_rel);
        const auto accepted = std::min(size, xStreamBufferSpacesAvailable(stream));
        // Invalidate before publishing any valid-looking frame prefix.
        if (accepted != size) invalidate();
        const auto sent = accepted == 0 ? 0 : xStreamBufferSend(stream, data, accepted, 0);
        if (sent != accepted) invalidate();
        callback_sequence.fetch_add(1, std::memory_order_release);
    }
    bool prepare(std::uint32_t &snapshot)
    {
        const auto before = callback_sequence.load(std::memory_order_acquire);
        snapshot = generation.load(std::memory_order_acquire);
        // Bound drain to its starting occupancy. Never reset a running stream.
        auto pending = xStreamBufferBytesAvailable(stream);
        std::uint8_t discard[64];
        while (pending != 0) {
            const auto n = xStreamBufferReceive(stream, discard,
                std::min(pending, sizeof(discard)), 0);
            if (n == 0) break;
            pending -= n;
        }
        return (before & 1U) == 0 &&
            before == callback_sequence.load(std::memory_order_acquire) && valid(snapshot);
    }
    bool valid(std::uint32_t snapshot) const
    {
        return snapshot == generation.load(std::memory_order_acquire);
    }
};

inline bool valid_exchange(const std::uint8_t *command, std::size_t command_size,
    const std::uint8_t *response, std::size_t capacity, std::uint32_t timeout_ms,
    ReceiveRule rule)
{
    return (command != nullptr || command_size == 0) &&
        command_size <= maximum_inverter_frame && response != nullptr &&
        capacity > 0 && capacity <= maximum_inverter_frame &&
        timeout_ms > 0 && timeout_ms <= 60000 && rule.inspect != nullptr;
}

template<typename IsConnected>
esp_err_t receive(ReceivePipe &pipe, std::uint32_t snapshot,
    std::uint8_t *response, std::size_t capacity, std::size_t &size,
    const Deadline &deadline, ReceiveRule rule, IsConnected is_connected)
{
    while (!deadline.expired()) {
        if (!is_connected()) return ESP_ERR_INVALID_STATE;
        if (!pipe.valid(snapshot)) return ESP_ERR_INVALID_RESPONSE;
        if (size == capacity) return ESP_ERR_INVALID_SIZE;
        if (xStreamBufferReceive(pipe.stream, response + size, 1, deadline.ticks()) == 0) continue;
        ++size;
        // Both disconnect and overflow invalidate even a correctly terminated prefix.
        if (!is_connected()) return ESP_ERR_INVALID_STATE;
        if (!pipe.valid(snapshot)) return ESP_ERR_INVALID_RESPONSE;
        if (deadline.expired()) return ESP_ERR_TIMEOUT;
        const auto decision = rule.inspect(response, size, rule.context);
        if (decision == FrameDecision::invalid) return ESP_ERR_INVALID_RESPONSE;
        if (decision == FrameDecision::complete) {
            if (!is_connected()) return ESP_ERR_INVALID_STATE;
            if (!pipe.valid(snapshot)) return ESP_ERR_INVALID_RESPONSE;
            return deadline.expired() ? ESP_ERR_TIMEOUT : ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

} // namespace inverter_gateway::transport::detail
