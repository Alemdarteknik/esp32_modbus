#pragma once

#include <atomic>
#include <cstdint>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "inverter_gateway/app/system_config.hpp"
#include "inverter_gateway/network/coordinator_command_policy.hpp"
#include "inverter_gateway/network/runtime_messages.hpp"

namespace inverter_gateway::network {

class CoordinatorLink {
public:
    CoordinatorLink(app::SystemConfig &config, RuntimeMessageSink &sink);
    esp_err_t start();
    void stop();
    bool connected() const;
    esp_err_t send_telemetry(const TelemetryMessage &message);
    esp_err_t send_command(const RoutedCommand &command);
    esp_err_t send_command_result(const RoutedCommandResult &result);
    esp_err_t send_protocol_telemetry(const ProtocolTelemetryMessage &message);
    esp_err_t send_protocol_command(const ProtocolCommand &command);
    esp_err_t send_protocol_command_result(const ProtocolCommandResult &result);
    // Synthesizes a routing result for the requested member, not this gateway.
    static RoutedCommandResult command_route_failure(const RoutedCommand &command,
                                                     bool uncertain);
    static ProtocolCommandResult protocol_command_route_failure(const ProtocolCommand &command,
                                                                bool uncertain);

private:
    struct SendResult {
        esp_err_t error;
        coordinator_policy::SendDisposition disposition;
    };
    static void worker_task(void *argument);
    void run();
    SendResult send(std::uint8_t type, const void *payload, std::uint16_t size,
                    const coordinator_policy::Ticket *ticket = nullptr);
    esp_err_t enqueue(std::uint8_t type, const void *payload, std::uint16_t size);
    void dispatch(std::uint8_t type, const std::uint8_t *payload, std::uint16_t size);
    void report_command_failure(const RoutedCommand &command, bool uncertain,
                                const char *reason);
    void report_protocol_command_failure(const ProtocolCommand &command, bool uncertain,
                                         const char *reason);
    void note_receive();
    bool accept_received_command();

    app::SystemConfig &config_;
    RuntimeMessageSink &sink_;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t send_mutex_ = nullptr;
    QueueHandle_t transmit_queue_ = nullptr;
    StaticSemaphore_t state_mutex_storage_{};
    SemaphoreHandle_t state_mutex_ = nullptr;
    mutable coordinator_policy::LinkSession session_{};
    std::atomic_bool started_{false};
};

} // namespace inverter_gateway::network
