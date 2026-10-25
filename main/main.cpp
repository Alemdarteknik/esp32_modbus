#include <cinttypes>
#include <atomic>
#include <memory>
#include <new>
#include <cstring>
#include <cstdio>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "inverter_gateway/app/json_memory.hpp"
#include "inverter_gateway/app/monitor_output.hpp"
#include "inverter_gateway/app/ota_update.hpp"
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/app/rgb_debug.hpp"
#include "inverter_gateway/app/system_config.hpp"
#include "inverter_gateway/inverter/command_service.hpp"
#include "inverter_gateway/inverter/production_poller.hpp"
#include "inverter_gateway/inverter/register_command_service.hpp"
#include "inverter_gateway/inverter/register_catalog.hpp"
#include "inverter_gateway/network/espnow_mesh.hpp"
#include "inverter_gateway/network/coordinator_link.hpp"
#include "inverter_gateway/network/mqtt_bridge.hpp"
#include "inverter_gateway/network/runtime_messages.hpp"
#include "inverter_gateway/network/protocol_command_history.hpp"
#include "inverter_gateway/network/wifi_provisioning.hpp"
#include "inverter_gateway/protocol/modbus_rtu.hpp"
#include "inverter_gateway/transport/usb_rs485.hpp"
#include "inverter_gateway/transport/energy_transport.hpp"
#include "inverter_gateway/transport/uart_inverter.hpp"
#include "inverter_gateway/transport/usb_hid_inverter.hpp"
#include "inverter_gateway/transport/usb_serial_protocol.hpp"
#include "inverter_gateway/transport/ble_inverter.hpp"
#include "inverter_gateway/protocol/bms_protocols.hpp"
#include "inverter_gateway/protocol/pi_protocols.hpp"
#include "inverter_gateway/protocol/energy_poll_schedule.hpp"
#include "nvs_flash.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "This application requires an ESP32-S3 with USB-OTG host support."
#endif

namespace {

constexpr char tag[] = "gateway";

class UniversalRuntime final : public inverter_gateway::network::RuntimeMessageSink {
public:
    explicit UniversalRuntime(inverter_gateway::app::SystemConfig &config) : config_(config)
    {
        command_queue_ = xQueueCreate(8, sizeof(inverter_gateway::network::RoutedCommand));
        protocol_queue_ = xQueueCreate(8, sizeof(QueuedProtocolCommand));
        if (inverter_gateway::app::role_has_local_inverter(config.profile.device_role) &&
            config.inverter_link.protocol != inverter_gateway::protocol::EnergyProtocol::aohai_modbus)
            protocol_history_.reset(new (std::nothrow) inverter_gateway::network::ProtocolCommandHistory);
        ota_admission_lock_ = xSemaphoreCreateMutexStatic(&ota_admission_storage_);
    }

    ~UniversalRuntime() override
    {
        if (command_queue_ != nullptr) vQueueDelete(command_queue_);
        if (protocol_queue_ != nullptr) vQueueDelete(protocol_queue_);
    }

    void attach_mesh(inverter_gateway::network::EspNowMesh *mesh) { mesh_ = mesh; }
    void attach_mqtt(inverter_gateway::network::MqttBridge *mqtt) { mqtt_ = mqtt; }
    void attach_coordinator_link(inverter_gateway::network::CoordinatorLink *link) { coordinator_link_ = link; }

    void on_telemetry(const inverter_gateway::network::TelemetryMessage &message) override
    {
        if (!inverter_gateway::app::role_has_local_inverter(config_.profile.device_role) ||
            message.source_member_id == config_.profile.logical_member_id) {
            last_data_ms_.store(static_cast<std::uint32_t>(esp_timer_get_time() / 1000));
        }
        if (message.source_member_id == config_.profile.logical_member_id) {
            local_reply_ms_.store(static_cast<std::uint32_t>(esp_timer_get_time() / 1000));
        }
        if (inverter_gateway::app::role_is_espnow_member(config_.profile.device_role)) {
            if (mesh_ != nullptr) mesh_->send_telemetry(message);
        } else if (mqtt_ != nullptr) {
            mqtt_->publish_telemetry(message);
        } else if (inverter_gateway::app::role_is_espnow_coordinator(config_.profile.device_role) &&
                   coordinator_link_ != nullptr) {
            coordinator_link_->send_telemetry(message);
        }
    }

    void on_command(const inverter_gateway::network::RoutedCommand &command) override
    {
        if (command.origin != inverter_gateway::network::RemoteCommandOrigin::mqtt) {
            ESP_LOGW(tag, "Rejected command without MQTT origin");
            return;
        }
        const std::uint8_t local_id = config_.profile.logical_member_id;
        if (inverter_gateway::app::role_has_local_inverter(config_.profile.device_role) &&
            command.target_member_id == local_id && command_queue_ != nullptr) {
            if (config_.inverter_link.protocol != inverter_gateway::protocol::EnergyProtocol::aohai_modbus) {
                reject_command(command, "Modbus command does not match selected protocol", false);
                return;
            }
            xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
            const bool unavailable = !local_inverter_available();
            const char *reason = ota_maintenance_.load() ? "OTA restarting" :
                unavailable ? "Inverter unavailable" : "Command queue full";
            const bool accepted = !ota_maintenance_.load() && !unavailable &&
                xQueueSend(command_queue_, &command, 0) == pdTRUE;
            // Track queued AND executing setters. Getters and rejected requests
            // must not postpone firmware checks or the write-quiet interval.
            if (accepted && command_is_write(command)) ++pending_write_count_;
            xSemaphoreGive(ota_admission_lock_);
            if (!accepted) reject_command(command, reason, unavailable);
        } else if (inverter_gateway::app::role_is_espnow_coordinator(config_.profile.device_role) &&
                   mesh_ != nullptr) {
            const esp_err_t result = mesh_->send_command(command);
            if (result != ESP_OK) {
                ESP_LOGW(tag, "Could not route command to member %u: %s",
                         command.target_member_id, esp_err_to_name(result));
                on_command_result(inverter_gateway::network::CoordinatorLink::command_route_failure(
                    command, false));
            }
        } else if (config_.profile.device_role == inverter_gateway::app::DeviceRole::internet_gateway &&
                   coordinator_link_ != nullptr) {
            const auto result = coordinator_link_->send_command(command);
            if (result != ESP_OK) {
                ESP_LOGW(tag, "Coordinator link did not accept command for member %u: %s",
                         command.target_member_id, esp_err_to_name(result));
                on_command_result(inverter_gateway::network::CoordinatorLink::command_route_failure(
                    command, false));
            }
        } else {
            ESP_LOGW(tag, "No route for command to member %u", command.target_member_id);
            on_command_result(inverter_gateway::network::CoordinatorLink::command_route_failure(
                command, false));
        }
    }

    void on_command_result(const inverter_gateway::network::RoutedCommandResult &result) override
    {
        if (inverter_gateway::app::role_is_espnow_member(config_.profile.device_role)) {
            if (mesh_ != nullptr) mesh_->send_command_result(result);
        } else if (mqtt_ != nullptr) {
            mqtt_->publish_command_result(result);
        } else if (inverter_gateway::app::role_is_espnow_coordinator(config_.profile.device_role) &&
                   coordinator_link_ != nullptr) {
            coordinator_link_->send_command_result(result);
        }
    }

    void reject_command(const inverter_gateway::network::RoutedCommand &command,
                        const char *reason, bool unavailable)
    {
        const auto id = command.kind == inverter_gateway::network::RemoteCommandKind::semantic
            ? command.request.command_id : command.register_request.command_id;
        ESP_LOGW(tag, "%s; rejected command %" PRIu64, reason, id);
        inverter_gateway::network::RoutedCommandResult rejected{};
        rejected.source_member_id = config_.profile.logical_member_id;
        rejected.kind = command.kind;
        rejected.result.command_id = id;
        rejected.result.operation = command.request.operation;
        rejected.result.outcome = unavailable
            ? inverter_gateway::inverter::CommandOutcome::state_unavailable
            : inverter_gateway::inverter::CommandOutcome::rejected;
        rejected.register_result.command_id = id;
        rejected.register_result.space = command.register_request.space;
        rejected.register_result.first_register = command.register_request.first_register;
        rejected.register_result.register_count = command.register_request.register_count;
        rejected.register_result.named_request = command.register_request.named_request;
        rejected.register_result.outcome = unavailable
            ? inverter_gateway::inverter::RegisterCommandOutcome::state_unavailable
            : inverter_gateway::inverter::RegisterCommandOutcome::state_not_allowed;
        on_command_result(rejected);
    }

    bool local_inverter_available() const
    {
        if (!transport_ready_.load()) return false;
        const auto last = local_reply_ms_.load();
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        const bool usb_present = config_.inverter_link.protocol !=
            inverter_gateway::protocol::EnergyProtocol::aohai_modbus ||
            !inverter_gateway::transport::UsbRs485::disconnected();
        return transport_ready_.load() && usb_present && last != 0 &&
            now - last < inverter_gateway::app::command_link_max_age_ms;
    }

    // Bus-owner only. Never carry queued commands into a new USB session.
    void set_transport_ready(bool ready)
    {
        xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
        transport_ready_.store(ready);
        if (!ready) local_reply_ms_.store(0);
        xSemaphoreGive(ota_admission_lock_);
        if (!ready && command_queue_ != nullptr) {
            inverter_gateway::network::RoutedCommand command{};
            while (xQueueReceive(command_queue_, &command, 0) == pdTRUE) {
                reject_command(command, "Inverter disconnected before execution", true);
                complete_command(command);
            }
        }
        if (!ready && protocol_queue_ != nullptr) {
            QueuedProtocolCommand queued{};
            while (xQueueReceive(protocol_queue_, &queued, 0) == pdTRUE) {
                reject_protocol_command(queued.command, "Device disconnected before execution", true);
                complete_protocol_command(queued.command);
            }
        }
    }

    bool receive_command(inverter_gateway::network::RoutedCommand &command)
    {
        while (command_queue_ != nullptr && xQueueReceive(command_queue_, &command, 0) == pdTRUE) {
            if (local_inverter_available()) return true;
            reject_command(command, "Inverter unavailable before execution", true);
            complete_command(command);
        }
        return false;
    }

    // Bus-owner only, exactly once for each accepted command after execution
    // or rejection. Unknown write outcomes still require a full quiet period.
    void complete_command(const inverter_gateway::network::RoutedCommand &command)
    {
        if (!command_is_write(command)) return;
        xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
        if (pending_write_count_ != 0) --pending_write_count_;
        last_write_completed_ms_ = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        write_completion_seen_ = true;
        xSemaphoreGive(ota_admission_lock_);
    }

    void on_protocol_telemetry(const inverter_gateway::network::ProtocolTelemetryMessage &message) override
    {
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (!inverter_gateway::app::role_has_local_inverter(config_.profile.device_role) ||
            message.source_member_id == config_.profile.logical_member_id) last_data_ms_.store(now);
        if (message.source_member_id == config_.profile.logical_member_id) local_reply_ms_.store(now);
        if (inverter_gateway::app::role_is_espnow_member(config_.profile.device_role)) {
            if (mesh_) mesh_->send_protocol_telemetry(message);
        } else if (mqtt_) mqtt_->publish_protocol_telemetry(message);
        else if (coordinator_link_) coordinator_link_->send_protocol_telemetry(message);
    }

    void on_protocol_command_result(const inverter_gateway::network::ProtocolCommandResult &result) override
    {
        if (protocol_history_ && result.source_member_id == config_.profile.logical_member_id) {
            xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
            protocol_history_->complete(result);
            xSemaphoreGive(ota_admission_lock_);
        }
        deliver_protocol_command_result(result);
    }

    void deliver_protocol_command_result(const inverter_gateway::network::ProtocolCommandResult &result)
    {
        if (inverter_gateway::app::role_is_espnow_member(config_.profile.device_role)) {
            if (mesh_) mesh_->send_protocol_command_result(result);
        } else if (mqtt_) mqtt_->publish_protocol_command_result(result);
        else if (coordinator_link_) coordinator_link_->send_protocol_command_result(result);
    }

    void reject_protocol_command(const inverter_gateway::network::ProtocolCommand &command,
                                 const char *reason, bool admitted = false)
    {
        inverter_gateway::network::ProtocolCommandResult result{};
        result.command_id = command.command_id;
        result.source_member_id = command.target_member_id;
        result.protocol = command.protocol;
        result.outcome = inverter_gateway::network::ProtocolCommandOutcome::not_sent;
        std::snprintf(result.command, sizeof(result.command), "%.*s", 79, command.command);
        std::snprintf(result.detail, sizeof(result.detail), "%s", reason);
        if (admitted) on_protocol_command_result(result);
        else deliver_protocol_command_result(result);
    }

    void on_protocol_command(const inverter_gateway::network::ProtocolCommand &command) override
    {
        using namespace inverter_gateway;
        protocol::EnergyCommandInfo info{};
        if (command.origin != network::RemoteCommandOrigin::mqtt || command.command_id == 0 ||
            std::memchr(command.command, 0, sizeof(command.command)) == nullptr ||
            !protocol::energy_command_info(command.protocol, command.command, info) ||
            info.setter != command.write || (command.write && !command.confirmed)) {
            reject_protocol_command(command, "Invalid protocol command or missing write confirmation");
            return;
        }
        if (app::role_has_local_inverter(config_.profile.device_role) &&
            command.target_member_id == config_.profile.logical_member_id) {
            if (command.protocol != config_.inverter_link.protocol) {
                reject_protocol_command(command, "Command protocol does not match device");
                return;
            }
            xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
            network::ProtocolCommandResult previous{};
            if (protocol_history_ && protocol_history_->lookup(command, previous)) {
                xSemaphoreGive(ota_admission_lock_);
                deliver_protocol_command_result(previous);
                return;
            }
            QueuedProtocolCommand queued{command, static_cast<std::uint32_t>(esp_timer_get_time() / 1000)};
            const bool reserved = !ota_maintenance_.load() && local_inverter_available() &&
                protocol_queue_ && uxQueueSpacesAvailable(protocol_queue_) != 0 &&
                protocol_history_ && protocol_history_->reserve(command);
            const bool accepted = reserved && xQueueSend(protocol_queue_, &queued, 0) == pdTRUE;
            const bool settings_blocked = command.write && protocol_history_ && protocol_history_->writes_blocked();
            if (accepted && command.write) ++pending_write_count_;
            xSemaphoreGive(ota_admission_lock_);
            if (!accepted) reject_protocol_command(command, settings_blocked
                ? "Earlier setting outcome uncertain; reconnect device or restart UART node"
                : "Device unavailable, restarting or queue full", reserved);
        } else if (app::role_is_espnow_coordinator(config_.profile.device_role) && mesh_) {
            if (mesh_->send_protocol_command(command) != ESP_OK)
                reject_protocol_command(command, "Member route unavailable");
        } else if (config_.profile.device_role == app::DeviceRole::internet_gateway && coordinator_link_) {
            if (coordinator_link_->send_protocol_command(command) != ESP_OK)
                reject_protocol_command(command, "Coordinator route unavailable");
        } else reject_protocol_command(command, "No route to member");
    }

    bool receive_protocol_command(inverter_gateway::network::ProtocolCommand &command)
    {
        QueuedProtocolCommand queued{};
        while (protocol_queue_ && xQueueReceive(protocol_queue_, &queued, 0) == pdTRUE) {
            command = queued.command;
            const auto age = static_cast<std::uint32_t>(esp_timer_get_time() / 1000) - queued.admitted_ms;
            if (age < 5000 && local_inverter_available()) return true;
            reject_protocol_command(command, "Device unavailable or queued command expired", true);
            complete_protocol_command(command);
        }
        return false;
    }

    void complete_protocol_command(const inverter_gateway::network::ProtocolCommand &command)
    {
        if (!command.write) return;
        xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
        if (pending_write_count_ != 0) --pending_write_count_;
        last_write_completed_ms_ = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        write_completion_seen_ = true;
        xSemaphoreGive(ota_admission_lock_);
    }

    void new_protocol_connection_session()
    {
        xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
        if (protocol_history_) protocol_history_->new_connection_session();
        xSemaphoreGive(ota_admission_lock_);
    }

    // Called only by the Modbus owner, outside any transaction or command.
    void ota_bus_boundary()
    {
        while (true) {
            bus_heartbeat_ms_.store(static_cast<std::uint32_t>(esp_timer_get_time() / 1000));
            xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
            if (!ota_maintenance_.load() || pending_write_count_ != 0) {
                bus_parked_.store(false);
                xSemaphoreGive(ota_admission_lock_);
                return;
            }
            // Admission closes only when there are no accepted writes. Finish
            // the in-flight read first, then reject queued getters here rather
            // than allowing a getter flood to delay activation indefinitely.
            inverter_gateway::network::RoutedCommand command{};
            const bool queued = command_queue_ != nullptr &&
                xQueueReceive(command_queue_, &command, 0) == pdTRUE;
            QueuedProtocolCommand protocol_command{};
            const bool protocol_queued = protocol_queue_ != nullptr &&
                xQueueReceive(protocol_queue_, &protocol_command, 0) == pdTRUE;
            bus_parked_.store(!queued && !protocol_queued);
            xSemaphoreGive(ota_admission_lock_);
            if (queued || protocol_queued) {
                if (queued) reject_command(command, "OTA restarting; queued inquiry cancelled", false);
                if (protocol_queued) reject_protocol_command(protocol_command.command, "OTA restarting; queued inquiry cancelled", true);
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }

    bool ota_healthy() const
    {
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        const auto last = bus_heartbeat_ms_.load();
        return last != 0 && now - last < 15000 && command_queue_ != nullptr && protocol_queue_ != nullptr &&
            mqtt_ != nullptr && mqtt_->ota_publisher_healthy();
    }

    const char *ota_download_blocker() const
    {
        if (!ota_healthy()) return "runtime_unhealthy";
        if (!mqtt_->connected()) return "mqtt_disconnected";
        xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        const char *blocker = write_blocker_locked(now);
        xSemaphoreGive(ota_admission_lock_);
        return blocker;
    }

    bool ota_quiesce()
    {
        if (ota_download_blocker() != nullptr) return false;
        xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        const bool quiet = !ota_maintenance_.load() && write_blocker_locked(now) == nullptr;
        if (quiet) {
            bus_parked_.store(false);
            ota_maintenance_.store(true);
        }
        xSemaphoreGive(ota_admission_lock_);
        if (!quiet) return false;
        for (unsigned count = 0; count < 400; ++count) {
            if (bus_parked_.load()) {
                // Best effort only: incoming rejected getters can keep adding
                // results. Do not require a perpetually empty MQTT outbox.
                // The outbox query takes the MQTT API mutex, so it is not a
                // bounded readiness probe. Give the publisher a fixed grace.
                vTaskDelay(pdMS_TO_TICKS(2000));
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        ota_resume();
        return false;
    }

    void ota_resume()
    {
        xSemaphoreTake(ota_admission_lock_, portMAX_DELAY);
        ota_maintenance_.store(false);
        bus_parked_.store(false);
        xSemaphoreGive(ota_admission_lock_);
    }
    bool local_inverter_seen() const { return local_inverter_available(); }
    inverter_gateway::app::RgbDebugState rgb_state(bool station_ready) const
    {
        using namespace inverter_gateway::app;
        const auto role = config_.profile.device_role;
        if (role_uses_mqtt(role)) {
            if (!station_ready) return RgbDebugState::wifi_wait;
            if (mqtt_ == nullptr || !mqtt_->connected()) return RgbDebugState::mqtt_wait;
        }
        if (role_is_espnow_member(role) &&
            (mesh_ == nullptr || !mesh_->has_coordinator())) return RgbDebugState::link_wait;
        if (role_uses_coordinator_link(role) &&
            (coordinator_link_ == nullptr || !coordinator_link_->connected())) return RgbDebugState::link_wait;
        const auto last = last_data_ms_.load();
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (last == 0 || now - last > rgb_debug_data_timeout_ms) return RgbDebugState::data_wait;
        return RgbDebugState::ready;
    }
    bool telemetry_transport_ready() const
    {
        return !inverter_gateway::app::role_is_espnow_member(config_.profile.device_role) ||
               (mesh_ != nullptr && mesh_->has_coordinator());
    }

private:
    struct QueuedProtocolCommand {
        inverter_gateway::network::ProtocolCommand command{};
        std::uint32_t admitted_ms = 0;
    };
    static bool command_is_write(const inverter_gateway::network::RoutedCommand &command)
    {
        using inverter_gateway::network::RemoteCommandKind;
        return command.kind == RemoteCommandKind::semantic ||
               command.kind == RemoteCommandKind::write_registers;
    }

    // Caller holds ota_admission_lock_. Timestamp subtraction tolerates wrap.
    const char *write_blocker_locked(std::uint32_t now) const
    {
        if (pending_write_count_ != 0) return "write_pending";
        if (write_completion_seen_ && now - last_write_completed_ms_ < 30000U)
            return "write_quiet_period";
        return nullptr;
    }

    StaticSemaphore_t ota_admission_storage_{};
    SemaphoreHandle_t ota_admission_lock_ = nullptr;
    std::atomic_bool ota_maintenance_{false};
    std::atomic_bool bus_parked_{false};
    unsigned pending_write_count_ = 0;
    std::uint32_t last_write_completed_ms_ = 0;
    bool write_completion_seen_ = false;
    std::atomic<std::uint32_t> bus_heartbeat_ms_{0};
    inverter_gateway::app::SystemConfig &config_;
    QueueHandle_t command_queue_ = nullptr;
    QueueHandle_t protocol_queue_ = nullptr;
    std::unique_ptr<inverter_gateway::network::ProtocolCommandHistory> protocol_history_;
    inverter_gateway::network::EspNowMesh *mesh_ = nullptr;
    inverter_gateway::network::MqttBridge *mqtt_ = nullptr;
    inverter_gateway::network::CoordinatorLink *coordinator_link_ = nullptr;
    std::atomic_bool transport_ready_{false};
    std::atomic<std::uint32_t> local_reply_ms_{0};
    std::atomic<std::uint32_t> last_data_ms_{0};
};

struct RgbContext {
    UniversalRuntime &runtime;
    inverter_gateway::network::WifiProvisioning &wifi;
};

inverter_gateway::app::RgbDebugState rgb_status(void *context)
{
    using inverter_gateway::app::RgbDebugState;
    auto &state = *static_cast<RgbContext *>(context);
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) return RgbDebugState::setup;
    return state.runtime.rgb_state(state.wifi.station_connected());
}

std::uint16_t enabled_features(const inverter_gateway::app::SystemConfig &config)
{
    using namespace inverter_gateway::inverter;
    std::uint16_t features = feature_none;
    if (config.profile.connected_pv_inputs > 0) features |= feature_pv;
    if (config.profile.connected_pv_inputs > 2) features |= feature_extended_pv;
    if (config.profile.battery_installed) features |= feature_battery;
    if (config.profile.battery_installed && config.profile.bms_connected) {
        features |= feature_bms;
    }
    if (config.profile.generator_installed) features |= feature_generator;
    if (config.profile.topology == inverter_gateway::app::SystemTopology::standalone_native_three_phase ||
        config.profile.topology == inverter_gateway::app::SystemTopology::parallel_native_three_phase) {
        features |= feature_three_phase;
    }
    if (config.profile.topology == inverter_gateway::app::SystemTopology::parallel_single_phase ||
        config.profile.topology == inverter_gateway::app::SystemTopology::parallel_three_phase_groups ||
        config.profile.topology == inverter_gateway::app::SystemTopology::parallel_native_three_phase) {
        features |= feature_parallel;
    }
    return features;
}

[[noreturn]] void run_local_inverter(inverter_gateway::app::SystemConfig &config,
                                     UniversalRuntime &runtime)
{
    using inverter_gateway::transport::UsbRs485;
    ESP_ERROR_CHECK(UsbRs485::install(inverter_gateway::app::usb_buffer_size));
    while (true) {
        runtime.set_transport_ready(false);
        runtime.ota_bus_boundary();
        UsbRs485::begin_connection_session();
        std::unique_ptr<CdcAcmDevice> device =
            UsbRs485::open(inverter_gateway::app::usb_open_timeout_ms,
                           inverter_gateway::app::usb_buffer_size);
        if (!device) {
            vTaskDelay(pdMS_TO_TICKS(inverter_gateway::app::usb_reconnect_delay_ms));
            continue;
        }
        if (UsbRs485::configure(*device, inverter_gateway::app::inverter_baud) != ESP_OK) {
            device.reset();
            vTaskDelay(pdMS_TO_TICKS(inverter_gateway::app::usb_configure_retry_ms));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(inverter_gateway::app::usb_settle_time_ms));
        UsbRs485::clear_receive_buffer();
        inverter_gateway::protocol::ModbusRtuClient client(
            *device, config.profile.modbus_slave_address,
            inverter_gateway::app::modbus_response_timeout_ms,
            inverter_gateway::app::modbus_minimum_command_gap_ms);
        // The expanded polling state must not consume the small main-task stack.
        std::unique_ptr<inverter_gateway::inverter::ProductionPoller> poller(
            new (std::nothrow) inverter_gateway::inverter::ProductionPoller(
            client, config.profile.logical_member_id,
            config.profile.connected_pv_inputs,
            static_cast<std::uint8_t>(config.profile.phase), runtime));
        if (!poller) {
            ESP_LOGE(tag, "Not enough heap for inverter polling state");
            device.reset();
            vTaskDelay(pdMS_TO_TICKS(inverter_gateway::app::usb_reconnect_delay_ms));
            continue;
        }
        inverter_gateway::inverter::CommandService commands(client);
        inverter_gateway::inverter::RegisterCommandService register_commands(client);
        std::int64_t last_task_diagnostics_ms = 0;

        runtime.set_transport_ready(true);
        while (!UsbRs485::disconnected()) {
            runtime.ota_bus_boundary();
            inverter_gateway::network::RoutedCommand command{};
            if (runtime.receive_command(command)) {
                auto state = poller->state();
                inverter_gateway::network::RoutedCommandResult routed_result{};
                routed_result.source_member_id = config.profile.logical_member_id;
                routed_result.kind = command.kind;
                if (command.kind == inverter_gateway::network::RemoteCommandKind::semantic) {
                    state.battery_commissioning_interlock = command.commissioning_interlock;
                    routed_result.result = commands.execute(command.request, state);
                } else {
                    routed_result.register_result = register_commands.execute(
                        command.register_request, state,
                        command.kind == inverter_gateway::network::RemoteCommandKind::write_registers);
                }
                const bool write_attempted =
                    command.kind == inverter_gateway::network::RemoteCommandKind::semantic
                        ? routed_result.result.write_was_sent
                        : command.kind == inverter_gateway::network::RemoteCommandKind::write_registers &&
                          routed_result.register_result.modbus_write.status !=
                              inverter_gateway::protocol::ModbusWriteStatus::invalid_argument &&
                          routed_result.register_result.modbus_write.status !=
                              inverter_gateway::protocol::ModbusWriteStatus::receive_busy;
                if (write_attempted) poller->invalidate_state_after_write();
                runtime.on_command_result(routed_result);
                runtime.complete_command(command);
            }
            if (runtime.telemetry_transport_ready()) {
                poller->run_due(enabled_features(config));
            }
            const auto diagnostics_now_ms = esp_timer_get_time() / 1000;
            if (diagnostics_now_ms - last_task_diagnostics_ms >=
                inverter_gateway::app::inverter_task_diagnostics_ms) {
                ESP_LOGI(tag, "Modbus task stack minimum free: %u bytes",
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
                last_task_diagnostics_ms = diagnostics_now_ms;
            }
            vTaskDelay(pdMS_TO_TICKS(inverter_gateway::app::inverter_loop_delay_ms));
        }
        runtime.set_transport_ready(false);
        ESP_LOGW(tag, "USB-RS485 adapter disconnected");
        device.reset();
        vTaskDelay(pdMS_TO_TICKS(inverter_gateway::app::usb_reconnect_delay_ms));
    }
}

using UsbProtocolStream = inverter_gateway::transport::UsbSerialProtocol;

struct ProtocolReceiveContext {
    inverter_gateway::protocol::EnergyProtocol id;
    const char *command;
    std::size_t frame_size = 0;
    std::size_t daly_expected_frames = 0;
};

inverter_gateway::transport::FrameDecision inspect_protocol_frame(
    const std::uint8_t *data, std::size_t size, void *argument)
{
    using namespace inverter_gateway;
    auto &context = *static_cast<ProtocolReceiveContext *>(argument);
    const auto state = context.daly_expected_frames
        ? protocol::daly_group_frame_state(context.command, data, size,
            context.daly_expected_frames, context.frame_size)
        : protocol::energy_frame_state(context.id, context.command, data, size, context.frame_size);
    return state == protocol::FrameState::complete ? transport::FrameDecision::complete :
        state == protocol::FrameState::invalid ? transport::FrameDecision::invalid :
        transport::FrameDecision::incomplete;
}

[[noreturn]] void run_energy_inverter(inverter_gateway::app::SystemConfig &config,
                                    UniversalRuntime &runtime)
{
    using namespace inverter_gateway;
    const auto &link = config.inverter_link;
    std::unique_ptr<transport::InverterByteTransport> link_transport;
    UsbProtocolStream *serial_usb = nullptr;
    esp_err_t installed = ESP_ERR_NO_MEM;
    if (link.connection == protocol::EnergyConnection::usb_rs485) {
        auto driver = std::unique_ptr<UsbProtocolStream>(new (std::nothrow) UsbProtocolStream);
        if (driver) { installed = driver->install(link.baud); serial_usb = driver.get(); }
        link_transport = std::move(driver);
    } else if (link.connection == protocol::EnergyConnection::usb_hid) {
        auto driver = std::unique_ptr<transport::UsbHid>(new (std::nothrow) transport::UsbHid);
        transport::UsbHidInverterConfig options{};
        options.vendor_id = link.usb_vid;
        options.product_id = link.usb_pid;
        if (driver) installed = driver->install(options);
        link_transport = std::move(driver);
    } else if (link.connection == protocol::EnergyConnection::bluetooth) {
        auto driver = std::unique_ptr<transport::BleInverter>(new (std::nothrow) transport::BleInverter);
        transport::BleInverterConfig options{};
        std::memcpy(options.mac_address, link.bluetooth_address, sizeof(options.mac_address));
        options.address_type = link.bluetooth_address_type;
        if (driver) installed = driver->install(options);
        link_transport = std::move(driver);
    } else {
        auto driver = std::unique_ptr<transport::UartInverter>(new (std::nothrow) transport::UartInverter);
        transport::UartInverterConfig options{};
        options.tx_pin = link.tx_pin;
        options.rx_pin = link.rx_pin;
        options.direction_pin = link.connection == protocol::EnergyConnection::uart_rs485 ? link.direction_pin : -1;
        options.baud = link.baud;
        options.parity = link.parity == 1 ? UART_PARITY_EVEN : link.parity == 2 ? UART_PARITY_ODD : UART_PARITY_DISABLE;
        if (driver) installed = driver->install(options);
        link_transport = std::move(driver);
    }
    auto response = std::unique_ptr<protocol::EnergyResponse>(new (std::nothrow) protocol::EnergyResponse);
    auto receive = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[transport::maximum_inverter_frame]);
    if (installed != ESP_OK || !link_transport || !response || !receive) {
        ESP_LOGE(tag, "Protocol transport could not start: %s", esp_err_to_name(installed));
        while (true) { runtime.set_transport_ready(false); runtime.ota_bus_boundary(); vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(tag, "Selected %s over %s", protocol::energy_protocol_info(link.protocol).name,
             protocol::energy_connection_name(link.connection));
    protocol::EnergyPollScheduler scheduler;
    std::uint32_t sequence = 0;
    std::uint32_t connection_session = link_transport->session_id();
    scheduler.reset(link.protocol, connection_session, esp_timer_get_time() / 1000,
                    config.profile.connected_pv_inputs);
    unsigned cell_count = 0, temperature_count = 0;
    std::int64_t next_request_us = 0;
    auto transact = [&](const char *command, bool &sent) -> protocol::CodecStatus {
        protocol::EnergyCommandInfo command_info{};
        if (!protocol::energy_command_info(link.protocol, command, command_info)) {
            sent = false; return protocol::CodecStatus::invalid_command;
        }
        std::uint8_t request[128]{};
        std::size_t request_size = 0;
        auto status = protocol::energy_build_request(link.protocol, command, link.address,
            request, sizeof(request), request_size);
        if (status != protocol::CodecStatus::ok) { sent = false; return status; }
        ProtocolReceiveContext context{link.protocol, command};
        if (link.protocol == protocol::EnergyProtocol::daly || link.protocol == protocol::EnergyProtocol::daly40) {
            if (std::strcmp(command, "cellVoltages") == 0) context.daly_expected_frames = (cell_count + 2) / 3;
            if (std::strcmp(command, "cellTemperatures") == 0) context.daly_expected_frames = (temperature_count + 6) / 7;
            if ((!std::strcmp(command, "cellVoltages") && !cell_count) ||
                (!std::strcmp(command, "cellTemperatures") && !temperature_count)) {
                sent = false; return protocol::CodecStatus::invalid_command;
            }
        }
        std::size_t size = transport::maximum_inverter_frame;
        while (esp_timer_get_time() < next_request_us) vTaskDelay(pdMS_TO_TICKS(10));
        const auto error = link_transport->exchange(request, request_size, receive.get(), size, 2000,
            {inspect_protocol_frame, &context, connection_session}, sent);
        next_request_us = esp_timer_get_time() + 100000;
        if (error != ESP_OK) return protocol::CodecStatus::invalid_frame;
        status = protocol::energy_decode_response(link.protocol, command, link.address,
            receive.get(), context.frame_size, *response);
        if (status != protocol::CodecStatus::ok) return status;
        // A setting ACK belongs to its command result. Only actual read
        // observations establish device readiness and refresh telemetry.
        if (command_info.setter) return status;
        if (response->count == 0) return protocol::CodecStatus::invalid_frame;
        if (context.daly_expected_frames) {
            const bool cells = std::strcmp(command, "cellVoltages") == 0;
            const char *prefix = cells ? "voltage_cell" : "battery_t";
            const auto prefix_size = std::strlen(prefix);
            const auto exact_count = cells ? cell_count : temperature_count;
            std::size_t kept = 0;
            for (std::size_t i = 0; i < response->count; ++i) {
                const auto &field = response->fields[i];
                unsigned index = 0;
                const bool padding = std::strncmp(field.key, prefix, prefix_size) == 0 &&
                    std::sscanf(field.key + prefix_size, "%u", &index) == 1 && index > exact_count;
                if (!padding) response->fields[kept++] = field;
            }
            response->count = kept;
        }
        const auto current_sequence = ++sequence;
        const auto sampled_ms = esp_timer_get_time() / 1000;
        for (std::size_t offset = 0; offset < response->count || offset == 0; offset += 2) {
            network::ProtocolTelemetryMessage message{};
            message.timestamp_ms = sampled_ms;
            message.sequence = current_sequence;
            message.source_member_id = config.profile.logical_member_id;
            message.protocol = link.protocol;
            message.chunk_index = static_cast<std::uint8_t>(offset / 2);
            message.chunk_count = static_cast<std::uint8_t>((response->count + 1) / 2);
            if (!message.chunk_count) message.chunk_count = 1;
            std::snprintf(message.command, sizeof(message.command), "%.15s", command);
            for (unsigned i = 0; i < 2 && offset + i < response->count; ++i)
                message.fields[message.field_count++] = response->fields[offset + i];
            message.final_chunk = offset + message.field_count >= response->count;
            runtime.on_protocol_telemetry(message);
            if (message.final_chunk) break;
            // Pace large BMS records for the 115200-baud coordinator cable
            // and bounded radio queues shared by every protocol.
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        for (std::size_t i = 0; i < response->count; ++i) {
            const auto &field = response->fields[i];
            if (!field.numeric) continue;
            if (std::strcmp(field.key, "number_of_cells") == 0 && field.value >= 0 && field.value <= 48) {
                cell_count = static_cast<unsigned>(field.value);
                scheduler.set_read_context_ready("cellVoltages", cell_count != 0, sampled_ms);
            }
            if (std::strcmp(field.key, "number_of_temperature_sensors") == 0 && field.value >= 0 && field.value <= 21) {
                temperature_count = static_cast<unsigned>(field.value);
                scheduler.set_read_context_ready("cellTemperatures", temperature_count != 0, sampled_ms);
            }
        }
        return status;
    };
    while (true) {
        runtime.ota_bus_boundary();
        if (!link_transport->connected()) {
            runtime.set_transport_ready(false);
            cell_count = temperature_count = 0;
            scheduler.set_read_context_ready("cellVoltages", false, esp_timer_get_time() / 1000);
            scheduler.set_read_context_ready("cellTemperatures", false, esp_timer_get_time() / 1000);
            if (serial_usb) serial_usb->reconnect();
            if (!link_transport->connected()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        }
        const auto current_session = link_transport->session_id();
        if (current_session != connection_session) {
            runtime.set_transport_ready(false);
            runtime.new_protocol_connection_session();
            connection_session = current_session;
            cell_count = temperature_count = 0;
            scheduler.reset(link.protocol, connection_session, esp_timer_get_time() / 1000,
                            config.profile.connected_pv_inputs);
        }
        runtime.set_transport_ready(true);
        // Run due background work before admitting the next queued command.
        // Live readings retain priority over metadata and command bursts.
        if (runtime.telemetry_transport_ready()) {
            const auto now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
            if (const auto *poll = scheduler.next(now)) {
                bool sent = false;
                const auto status = transact(poll->command, sent);
                scheduler.record(status == protocol::CodecStatus::ok,
                    esp_timer_get_time() / 1000, status);
                if (status != protocol::CodecStatus::ok && poll->minimum_pv_inputs)
                    ESP_LOGW(tag, "%s additional PV query %s: %s; PV2/PV3 readings remain unavailable",
                        protocol::energy_protocol_info(link.protocol).name, poll->command,
                        protocol::codec_status_name(status));
                else if (status != protocol::CodecStatus::ok)
                    ESP_LOGD(tag, "%s %s: %s", protocol::energy_protocol_info(link.protocol).name,
                        poll->command, protocol::codec_status_name(status));
            }
        }
        network::ProtocolCommand command{};
        if (runtime.receive_protocol_command(command)) {
            bool sent = false;
            auto status = transact(command.command, sent);
            scheduler.observe(command.command, status == protocol::CodecStatus::ok,
                              esp_timer_get_time() / 1000, status);
            if (command.write && status == protocol::CodecStatus::ok && !response->acknowledged)
                status = protocol::CodecStatus::invalid_frame;
            network::ProtocolCommandResult result{};
            result.command_id = command.command_id;
            result.source_member_id = config.profile.logical_member_id;
            result.protocol = link.protocol;
            result.write_sent = command.write && sent;
            result.outcome = status == protocol::CodecStatus::ok
                ? (command.write ? network::ProtocolCommandOutcome::acknowledged : network::ProtocolCommandOutcome::completed)
                : status == protocol::CodecStatus::rejected ? network::ProtocolCommandOutcome::rejected
                : command.write && sent ? network::ProtocolCommandOutcome::uncertain
                : sent ? network::ProtocolCommandOutcome::invalid_response : network::ProtocolCommandOutcome::not_sent;
            std::snprintf(result.command, sizeof(result.command), "%s", command.command);
            std::snprintf(result.detail, sizeof(result.detail), "%s",
                command.write && status == protocol::CodecStatus::ok ?
                    "Acknowledged; no setting verification available" : protocol::codec_status_name(status));
            if (command.write && sent) {
                // Invalidate earlier measurements and queued commands after a
                // write, including an uncertain write. Reads may refresh state;
                // a general ACK never asserts equality with the requested value.
                runtime.set_transport_ready(false);
                scheduler.refresh_live(esp_timer_get_time() / 1000);
                scheduler.refresh_configuration(esp_timer_get_time() / 1000);
                protocol::EnergyCommandInfo info{}, readback{};
                if (status == protocol::CodecStatus::ok &&
                    protocol::energy_command_info(link.protocol, command.command, info) &&
                    info.readback_command &&
                    protocol::energy_command_info(link.protocol, info.readback_command, readback) && !readback.setter) {
                    bool read_sent = false;
                    const auto observed = transact(info.readback_command, read_sent);
                    scheduler.observe(info.readback_command, observed == protocol::CodecStatus::ok,
                                      esp_timer_get_time() / 1000, observed);
                    if (observed == protocol::CodecStatus::ok) {
                        bool matches = false;
                        const bool verified = protocol::pi_setting_readback(link.protocol, command.command,
                            info.readback_command, *response, matches) ||
                            protocol::bms_setting_readback(link.protocol, command.command,
                                info.readback_command, *response, matches);
                        if (verified && matches) {
                            result.outcome = network::ProtocolCommandOutcome::completed;
                            std::snprintf(result.detail, sizeof(result.detail), "Setting verified by %s",
                                info.readback_command);
                        } else std::snprintf(result.detail, sizeof(result.detail), "%s",
                            verified ? "Readback mismatch; setting not verified" :
                                       "Acknowledged; readback does not verify this setting");
                    } else std::snprintf(result.detail, sizeof(result.detail), "Acknowledged; readback failed: %s",
                        protocol::codec_status_name(observed));
                }
            }
            runtime.on_protocol_command_result(result);
            runtime.complete_protocol_command(command);
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

// app_main stays alive in idle_forever, so these references outlive this task.
struct InverterTaskContext {
    inverter_gateway::app::SystemConfig *config;
    UniversalRuntime *runtime;
};

void inverter_task_entry(void *argument)
{
    auto *context = static_cast<InverterTaskContext *>(argument);
    if (context->config->inverter_link.protocol == inverter_gateway::protocol::EnergyProtocol::aohai_modbus)
        run_local_inverter(*context->config, *context->runtime);
    run_energy_inverter(*context->config, *context->runtime);
}

void start_local_inverter_task(inverter_gateway::app::SystemConfig &config,
                               UniversalRuntime &runtime)
{
    static InverterTaskContext context{};
    static TaskHandle_t task = nullptr;
    if (task != nullptr) return;
    context = {&config, &runtime};
    if (xTaskCreate(inverter_task_entry, "inverter_bus",
                    inverter_gateway::app::inverter_task_stack_bytes, &context,
                    inverter_gateway::app::inverter_task_priority, &task) != pdPASS) {
        ESP_LOGE(tag, "Could not start inverter task; no inverter communication is running");
    }
}

[[noreturn]] void idle_forever()
{
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

bool mqtt_ready(void *context)
{
    auto *mqtt = static_cast<inverter_gateway::network::MqttBridge *>(context);
    return mqtt != nullptr && mqtt->connected();
}

bool wait_for_station_ip(inverter_gateway::network::WifiProvisioning &wifi)
{
    const TickType_t started = xTaskGetTickCount();
    while (!wifi.station_connected() &&
           xTaskGetTickCount() - started <
               pdMS_TO_TICKS(inverter_gateway::app::mqtt_wifi_ready_wait_ms)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return wifi.station_connected();
}

bool inverter_ready(void *context)
{
    auto *runtime = static_cast<UniversalRuntime *>(context);
    return runtime != nullptr && runtime->local_inverter_seen();
}

esp_err_t clear_factory_setup()
{
    const esp_err_t config_result = inverter_gateway::app::ConfigStore{}.clear();
    if (config_result != ESP_OK) return config_result;
    // Clear any station credentials retained by older firmware versions that
    // used the Wi-Fi driver's default flash-backed storage.
    return esp_wifi_restore();
}

bool provisioning_button_held()
{
    gpio_config_t configuration{};
    configuration.pin_bit_mask = 1ULL << inverter_gateway::app::provisioning_button_gpio;
    configuration.mode = GPIO_MODE_INPUT;
    configuration.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&configuration);
    if (gpio_get_level(inverter_gateway::app::provisioning_button_gpio) != 0) return false;
    const TickType_t started = xTaskGetTickCount();
    while (gpio_get_level(inverter_gateway::app::provisioning_button_gpio) == 0) {
        if (xTaskGetTickCount() - started >=
            pdMS_TO_TICKS(inverter_gateway::app::provisioning_button_hold_ms)) return true;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return false;
}

void provisioning_button_task(void *argument)
{
    auto *config = static_cast<inverter_gateway::app::SystemConfig *>(argument);
    bool timing_press = false;
    TickType_t pressed_at = 0;
    while (config != nullptr) {
        if (gpio_get_level(inverter_gateway::app::provisioning_button_gpio) == 0) {
            if (!timing_press) {
                timing_press = true;
                pressed_at = xTaskGetTickCount();
            } else if (xTaskGetTickCount() - pressed_at >=
                       pdMS_TO_TICKS(inverter_gateway::app::provisioning_button_hold_ms)) {
                const esp_err_t result = clear_factory_setup();
                if (result == ESP_OK) {
                    ESP_LOGW(tag,
                             "Factory reset complete; release BOOT to restart in first-installation mode");
                    while (gpio_get_level(
                               inverter_gateway::app::provisioning_button_gpio) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(25));
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                }
                ESP_LOGE(tag, "Factory reset failed: %s",
                         esp_err_to_name(result));
                while (gpio_get_level(inverter_gateway::app::provisioning_button_gpio) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                timing_press = false;
            }
        } else {
            timing_press = false;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void app_main(void)
{
    inverter_gateway::app::initialize_json_memory();
    inverter_gateway::app::arm_ota_boot_trial();
    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        ESP_LOGE(tag, "NVS initialization failed: %s", esp_err_to_name(nvs_result));
        return;
    }
    const esp_err_t monitor_result = inverter_gateway::app::load_monitor_output();
    if (monitor_result != ESP_OK) {
        ESP_LOGW(tag, "Could not load serial-monitor setting: %s",
                 esp_err_to_name(monitor_result));
    }

    inverter_gateway::app::SystemConfig config{};
    config.profile.expected_inverter_count = 1;
    const esp_err_t load_result = inverter_gateway::app::ConfigStore{}.load(config);
    if (config.site_id[0] == '\0') {
        ESP_ERROR_CHECK(inverter_gateway::app::ensure_hardware_site_id(config));
    }
    const auto validation = inverter_gateway::app::validate_config(config);
    const bool configuration_ready = load_result == ESP_OK &&
        validation == inverter_gateway::app::ConfigValidation::valid;
    inverter_gateway::network::WifiProvisioning wifi(config);
    ESP_ERROR_CHECK(wifi.initialize());
    if (configuration_ready && provisioning_button_held()) {
        ESP_ERROR_CHECK(clear_factory_setup());
        ESP_LOGW(tag,
                 "Factory reset complete; release BOOT to restart in first-installation mode");
        while (gpio_get_level(inverter_gateway::app::provisioning_button_gpio) == 0) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    UniversalRuntime runtime(config);
    RgbContext rgb_context{runtime, wifi};
    wifi.set_inverter_ready_probe(inverter_ready, &runtime);

    // A valid saved configuration may be in either normal operation or the
    // post-save verification portal. Monitor BOOT in both states so a long
    // press always performs the same complete factory reset.
    if (configuration_ready &&
        xTaskCreate(provisioning_button_task, "setup_button", 3072, &config, 5,
                    nullptr) != pdPASS) {
        ESP_LOGE(tag, "Could not start setup-button monitor");
    }

    if (!configuration_ready || !config.provisioned) {
        ESP_LOGW(tag, "Provisioning mode (%s)",
                 configuration_ready ? "awaiting_verification" :
                 inverter_gateway::app::config_validation_name(validation));
        ESP_ERROR_CHECK(wifi.start_portal());
        std::unique_ptr<inverter_gateway::network::EspNowMesh> mesh;
        if (configuration_ready && inverter_gateway::app::role_uses_espnow(config.profile.device_role)) {
            mesh = std::make_unique<inverter_gateway::network::EspNowMesh>(config, runtime);
            runtime.attach_mesh(mesh.get());
            ESP_ERROR_CHECK(mesh->start());
            wifi.set_mesh(mesh.get());
        }
        std::unique_ptr<inverter_gateway::network::CoordinatorLink> coordinator_link;
        if (configuration_ready &&
            inverter_gateway::app::role_uses_coordinator_link(config.profile.device_role)) {
            coordinator_link = std::make_unique<inverter_gateway::network::CoordinatorLink>(config, runtime);
            runtime.attach_coordinator_link(coordinator_link.get());
            wifi.set_coordinator_link(coordinator_link.get());
            ESP_ERROR_CHECK(coordinator_link->start());
        }
        std::unique_ptr<inverter_gateway::network::MqttBridge> mqtt;
        const bool waiting_for_master_identity =
            config.profile.device_role == inverter_gateway::app::DeviceRole::internet_gateway &&
            inverter_gateway::app::site_id_is_hardware_default(config);
        if (configuration_ready && inverter_gateway::app::role_uses_mqtt(config.profile.device_role) &&
            !waiting_for_master_identity) {
            if (!wait_for_station_ip(wifi)) {
                ESP_LOGW(tag,
                         "Wi-Fi has no IP yet; MQTT will start and retry in the background");
            }
            mqtt = std::make_unique<inverter_gateway::network::MqttBridge>(config, runtime);
            const esp_err_t mqtt_start_result = mqtt->start();
            if (mqtt_start_result == ESP_OK) {
                runtime.attach_mqtt(mqtt.get());
                wifi.set_mqtt_ready_probe(mqtt_ready, mqtt.get());
            } else {
                ESP_LOGE(tag, "MQTT could not start: %s; setup AP remains active",
                         esp_err_to_name(mqtt_start_result));
                mqtt.reset();
            }
        }
        inverter_gateway::app::start_rgb_debug(rgb_status, &rgb_context);
        if (configuration_ready &&
            inverter_gateway::app::role_has_local_inverter(config.profile.device_role)) {
            start_local_inverter_task(config, runtime);
        }
        idle_forever();
    }

    ESP_LOGI(tag, "Starting role=%s topology=%s",
             inverter_gateway::app::device_role_name(config.profile.device_role),
             inverter_gateway::app::topology_name(config.profile.topology));
    if (inverter_gateway::app::role_uses_mqtt(config.profile.device_role)) {
        ESP_ERROR_CHECK(wifi.start_station());
    } else {
        ESP_ERROR_CHECK(wifi.start_radio(config.profile.espnow_channel));
    }
    std::unique_ptr<inverter_gateway::network::EspNowMesh> mesh;
    if (inverter_gateway::app::role_uses_espnow(config.profile.device_role)) {
        mesh = std::make_unique<inverter_gateway::network::EspNowMesh>(config, runtime);
        runtime.attach_mesh(mesh.get());
        ESP_ERROR_CHECK(mesh->start());
        wifi.set_mesh(mesh.get());
    }

    std::unique_ptr<inverter_gateway::network::CoordinatorLink> coordinator_link;
    if (inverter_gateway::app::role_uses_coordinator_link(config.profile.device_role)) {
        coordinator_link = std::make_unique<inverter_gateway::network::CoordinatorLink>(config, runtime);
        runtime.attach_coordinator_link(coordinator_link.get());
        wifi.set_coordinator_link(coordinator_link.get());
        ESP_ERROR_CHECK(coordinator_link->start());
    }

    std::unique_ptr<inverter_gateway::network::MqttBridge> mqtt;
    if (inverter_gateway::app::role_uses_mqtt(config.profile.device_role)) {
        if (!wait_for_station_ip(wifi)) {
            ESP_LOGW(tag,
                     "Wi-Fi has no IP yet; MQTT will start and retry in the background");
        }
        mqtt = std::make_unique<inverter_gateway::network::MqttBridge>(config, runtime);
        const esp_err_t mqtt_start_result = mqtt->start();
        if (mqtt_start_result == ESP_OK) {
            runtime.attach_mqtt(mqtt.get());
            wifi.set_mqtt_ready_probe(mqtt_ready, mqtt.get());
        } else {
            ESP_LOGE(tag, "MQTT could not start: %s",
                     esp_err_to_name(mqtt_start_result));
            mqtt.reset();
        }
    }

    if (inverter_gateway::app::role_uses_mqtt(config.profile.device_role)) {
        const esp_err_t webpage_result = wifi.start_lan_page();
        if (webpage_result != ESP_OK) {
            ESP_LOGE(tag, "Configuration webpage could not start: %s",
                     esp_err_to_name(webpage_result));
        }
    }

    inverter_gateway::app::start_rgb_debug(rgb_status, &rgb_context);
    if (inverter_gateway::app::role_has_local_inverter(config.profile.device_role)) {
        start_local_inverter_task(config, runtime);
    }
    if (config.profile.device_role == inverter_gateway::app::DeviceRole::standalone_combined) {
        inverter_gateway::app::start_ota_update({
            &runtime,
            [](void *p) { return static_cast<UniversalRuntime *>(p)->ota_healthy(); },
            [](void *p) { return static_cast<UniversalRuntime *>(p)->ota_download_blocker(); },
            [](void *p) { return static_cast<UniversalRuntime *>(p)->ota_quiesce(); },
            [](void *p) { static_cast<UniversalRuntime *>(p)->ota_resume(); }
        });
    }
    idle_forever();
}
