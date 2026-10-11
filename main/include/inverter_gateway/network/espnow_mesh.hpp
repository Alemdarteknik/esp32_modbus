#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "inverter_gateway/app/system_config.hpp"
#include "inverter_gateway/network/runtime_messages.hpp"

namespace inverter_gateway::network {

class EspNowMesh {
public:
    EspNowMesh(app::SystemConfig &config, RuntimeMessageSink &sink);
    esp_err_t start();
    void stop();
    esp_err_t announce();
    esp_err_t send_telemetry(const TelemetryMessage &message);
    esp_err_t send_command(const RoutedCommand &command);
    esp_err_t send_command_result(const RoutedCommandResult &result);
    std::size_t discovered_member_count() const;
    bool has_coordinator() const;

private:
    static void receive_callback(const esp_now_recv_info_t *info,
                                 const std::uint8_t *data, int length);
    void receive(const std::uint8_t *source, const std::uint8_t *data,
                 std::size_t length);
    static void worker_task(void *argument);
    esp_err_t send_packet(const std::uint8_t *destination, std::uint8_t type,
                          const void *payload, std::size_t payload_size);
    const std::uint8_t *peer_for_member(std::uint8_t member_id) const;
    void learn_peer(const std::uint8_t *mac, std::uint8_t member_id);
    bool accept_sequence(const std::uint8_t *mac, std::uint32_t sequence,
                         bool reset);

    app::SystemConfig &config_;
    RuntimeMessageSink &sink_;
    std::atomic<std::uint32_t> sequence_{0};
    bool started_ = false;
    QueueHandle_t receive_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    std::array<std::uint32_t, app::max_saved_peers> last_peer_sequence_{};
    static EspNowMesh *instance_;
};

} // namespace inverter_gateway::network
