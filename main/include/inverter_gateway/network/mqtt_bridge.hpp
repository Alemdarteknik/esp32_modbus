#pragma once

#include <array>
#include <cstdint>
#include <atomic>
#include <memory>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "inverter_gateway/app/system_config.hpp"
#include "inverter_gateway/network/runtime_messages.hpp"
#include "inverter_gateway/network/protocol_telemetry_cache.hpp"

namespace inverter_gateway::network {

class MqttBridge {
public:
    MqttBridge(const app::SystemConfig &config, RuntimeMessageSink &sink);
    esp_err_t start();
    void stop();
    bool connected() const { return connected_.load(); }
    bool ota_publisher_healthy() const;
    bool ota_results_drained() const;
    void publish_telemetry(const TelemetryMessage &message);
    void publish_command_result(const RoutedCommandResult &result);
    void publish_protocol_telemetry(const ProtocolTelemetryMessage &message);
    void publish_protocol_command_result(const ProtocolCommandResult &result);

private:
    static constexpr std::size_t alert_point_count = 512;
    static constexpr std::size_t input_point_count = 499;
    static constexpr std::size_t maximum_members = app::max_saved_peers + 1;

    struct LiveCache {
        struct LastPower {
            double value = 0;
            std::uint32_t validated_ms = 0;
            bool valid = false;
        };
        std::array<LastPower, 9> last_power{};
        // Allocate timestamps only for members that actually send data.
        std::unique_ptr<std::uint32_t[]> received_ms;
        std::unique_ptr<std::uint32_t[]> sampled_ms;
        std::array<std::uint16_t, input_point_count> values{};
        std::array<bool, input_point_count> seen{};
        std::uint16_t enabled_features = 0;
        std::uint16_t source_priority = 0;
        bool source_priority_seen = false;
        double rated_power_w = 0.0;
        bool rated_power_seen = false;
        std::uint8_t connected_pv_inputs = 0;
        std::uint8_t phase_assignment = 0;
        std::uint32_t sequence = 0;
        std::int64_t timestamp_ms = 0;
        std::int64_t last_publish_ms = 0;
        std::int64_t last_core_status_ms = 0;
    };

    static void event_callback(void *argument, esp_event_base_t base,
                               std::int32_t event_id, void *event_data);
    void handle_event(esp_mqtt_event_handle_t event);
    void handle_command(const char *topic, int topic_length,
                        const char *payload, int payload_length);
    void make_topic(char *destination, std::size_t capacity,
                    std::uint8_t member_id, const char *suffix) const;
    static void publisher_task(void *argument);
    void publish_telemetry_now(const TelemetryMessage &message);
    void publish_command_result_now(const RoutedCommandResult &result);
    void publish_protocol_telemetry_now(const ProtocolTelemetryMessage &message);
    void publish_protocol_record_now(std::uint8_t member, ProtocolTelemetryCache::Record &record,
                                     std::uint32_t now);
    void publish_protocol_live_now(std::uint8_t member, std::uint32_t now);
    void publish_protocol_command_result_now(const ProtocolCommandResult &result);
    void publish_live(const TelemetryMessage &message);
    void publish_snapshot(const TelemetryMessage &message);
    void publish_alert_changes(const TelemetryMessage &message);
    void publish_setting_snapshot(const RoutedCommandResult &message);
    bool remember_command_id(std::uint64_t command_id);
    void publish_health();

    const app::SystemConfig &config_;
    RuntimeMessageSink &sink_;
    esp_mqtt_client_handle_t client_ = nullptr;
    std::atomic_bool connected_{false};
    std::atomic<std::uint32_t> publisher_heartbeat_ms_{0};
    QueueHandle_t publish_queue_ = nullptr;
    QueueHandle_t result_queue_ = nullptr;
    QueueHandle_t protocol_publish_queue_ = nullptr;
    QueueHandle_t protocol_result_queue_ = nullptr;
    char hardware_client_id_[32]{};
    TaskHandle_t publisher_task_ = nullptr;
    // Owned by this bridge, used only by mqtt_publish; enqueue copies its data.
    char *live_json_buffer_ = nullptr;
    std::size_t live_json_capacity_ = 0;
    char availability_topic_[192]{};
    std::array<std::uint64_t, 32> recent_command_ids_{};
    std::size_t next_command_id_slot_ = 0;
    std::atomic<std::uint32_t> last_telemetry_ms_{0};
    std::atomic<std::uint32_t> last_live_enqueued_ms_{0};
    std::atomic<std::uint32_t> reconnect_count_{0};
    std::array<LiveCache, maximum_members> live_cache_{};
    // Allocate only for observed native members. The large bounded cache uses
    // the configured PSRAM-capable allocator, never the publisher task stack.
    std::array<std::unique_ptr<ProtocolTelemetryCache>, maximum_members> native_cache_{};
    std::array<std::array<std::uint16_t, alert_point_count>, maximum_members>
        alert_values_{};
    std::array<std::array<bool, alert_point_count>, maximum_members> alert_seen_{};
};

} // namespace inverter_gateway::network
