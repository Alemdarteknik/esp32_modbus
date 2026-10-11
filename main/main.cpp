#include <cinttypes>
#include <atomic>
#include <memory>
#include <new>

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
#include "inverter_gateway/network/wifi_provisioning.hpp"
#include "inverter_gateway/protocol/modbus_rtu.hpp"
#include "inverter_gateway/transport/usb_rs485.hpp"
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
        ota_admission_lock_ = xSemaphoreCreateMutexStatic(&ota_admission_storage_);
    }

    ~UniversalRuntime() override
    {
        if (command_queue_ != nullptr) vQueueDelete(command_queue_);
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
        const auto last = local_reply_ms_.load();
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        return transport_ready_.load() &&
            !inverter_gateway::transport::UsbRs485::disconnected() && last != 0 &&
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
            bus_parked_.store(!queued);
            xSemaphoreGive(ota_admission_lock_);
            if (queued) {
                reject_command(command, "OTA restarting; queued inquiry cancelled", false);
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }

    bool ota_healthy() const
    {
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        const auto last = bus_heartbeat_ms_.load();
        return last != 0 && now - last < 15000 && command_queue_ != nullptr &&
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
                              inverter_gateway::protocol::ModbusWriteStatus::invalid_argument;
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

// app_main stays alive in idle_forever, so these references outlive this task.
struct InverterTaskContext {
    inverter_gateway::app::SystemConfig *config;
    UniversalRuntime *runtime;
};

void inverter_task_entry(void *argument)
{
    auto *context = static_cast<InverterTaskContext *>(argument);
    run_local_inverter(*context->config, *context->runtime);
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
