#include "inverter_gateway/network/coordinator_link.hpp"

#include <array>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/protocol/modbus_rtu.hpp"

namespace inverter_gateway::network {
namespace {

constexpr char tag[] = "coord_link";
constexpr std::uint16_t magic = 0x584d;
constexpr std::uint8_t version = 2;
constexpr std::uint8_t type_heartbeat = 1;
constexpr std::uint8_t type_telemetry = 2;
constexpr std::uint8_t type_command = 3;
constexpr std::uint8_t type_command_result = 4;
constexpr std::size_t maximum_payload = 256;

#pragma pack(push, 1)
struct Header {
    std::uint16_t magic_value;
    std::uint8_t protocol_version;
    std::uint8_t type;
    std::uint16_t payload_size;
    std::uint32_t site_hash;
    std::uint16_t crc;
};

struct IdentityPayload {
    std::uint8_t role;
    char site_id[app::max_site_id_length + 1];
};
#pragma pack(pop)

struct OutboundFrame {
    std::uint8_t type;
    std::uint16_t size;
    coordinator_policy::Ticket ticket;
    std::uint8_t payload[maximum_payload];
};

std::uint32_t monotonic_ms()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

std::uint32_t site_hash(const char *text)
{
    std::uint32_t hash = 2166136261U;
    while (text != nullptr && *text != '\0') {
        hash = (hash ^ static_cast<std::uint8_t>(*text++)) * 16777619U;
    }
    return hash;
}

std::uint16_t frame_crc(std::uint8_t type, std::uint32_t site,
                        const void *payload, std::uint16_t size)
{
    std::array<std::uint8_t, 8 + maximum_payload> bytes{};
    bytes[0] = version;
    bytes[1] = type;
    bytes[2] = static_cast<std::uint8_t>(size);
    bytes[3] = static_cast<std::uint8_t>(size >> 8U);
    bytes[4] = static_cast<std::uint8_t>(site);
    bytes[5] = static_cast<std::uint8_t>(site >> 8U);
    bytes[6] = static_cast<std::uint8_t>(site >> 16U);
    bytes[7] = static_cast<std::uint8_t>(site >> 24U);
    if (size != 0) std::memcpy(bytes.data() + 8, payload, size);
    return inverter_gateway::protocol::calculate_modbus_crc(bytes.data(), 8 + size);
}

} // namespace

CoordinatorLink::CoordinatorLink(app::SystemConfig &config,
                                 RuntimeMessageSink &sink)
    : config_(config), sink_(sink)
{
    state_mutex_ = xSemaphoreCreateMutexStatic(&state_mutex_storage_);
}

esp_err_t CoordinatorLink::start()
{
    if (started_) return ESP_OK;
    uart_config_t uart_config{};
    uart_config.baud_rate = inverter_gateway::app::coordinator_uart_baud;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    const auto port = static_cast<uart_port_t>(inverter_gateway::app::coordinator_uart_port);
    ESP_RETURN_ON_ERROR(uart_param_config(port, &uart_config), tag, "UART config");
    ESP_RETURN_ON_ERROR(uart_set_pin(port, inverter_gateway::app::coordinator_uart_tx_gpio,
                                     inverter_gateway::app::coordinator_uart_rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        tag, "UART pins");
    // No TX ring: uart_write_bytes would wait indefinitely for ring capacity.
    // The sole worker uses bounded, nonblocking FIFO admission instead.
    ESP_RETURN_ON_ERROR(uart_driver_install(port, 1024, 0, 0, nullptr, 0),
                        tag, "UART driver");
    send_mutex_ = xSemaphoreCreateMutex();
    if (send_mutex_ == nullptr) {
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }
    transmit_queue_ = xQueueCreate(32, sizeof(OutboundFrame));
    if (transmit_queue_ == nullptr) {
        vSemaphoreDelete(send_mutex_);
        send_mutex_ = nullptr;
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    session_.invalidate();
    started_ = true;
    xSemaphoreGive(state_mutex_);
    if (xTaskCreate(worker_task, "coord_link", 4096, this, 5, &task_) != pdPASS) {
        stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void CoordinatorLink::stop()
{
    // This is startup-error cleanup, not an in-flight cancellation API. A
    // task/power interruption during a UART write has an uncertain outcome.
    started_ = false;
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    uart_driver_delete(static_cast<uart_port_t>(inverter_gateway::app::coordinator_uart_port));
    if (send_mutex_ != nullptr) {
        vSemaphoreDelete(send_mutex_);
        send_mutex_ = nullptr;
    }
    if (transmit_queue_ != nullptr) {
        // Normal callers use stop during startup failure. Do not carry any
        // accepted-but-unsent command into a subsequent start of this object.
        OutboundFrame pending{};
        while (xQueueReceive(transmit_queue_, &pending, 0) == pdTRUE) {
            if (pending.type != type_command || pending.size != sizeof(RoutedCommand)) continue;
            RoutedCommand command{};
            std::memcpy(&command, pending.payload, sizeof(command));
            report_command_failure(command, false, "coordinator link stopped before send");
        }
        vQueueDelete(transmit_queue_);
        transmit_queue_ = nullptr;
    }
}

bool CoordinatorLink::connected() const
{
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    const bool ready = started_.load() &&
        session_.online(monotonic_ms(), app::coordinator_link_timeout_ms);
    xSemaphoreGive(state_mutex_);
    return ready;
}

void CoordinatorLink::note_receive()
{
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    session_.received(monotonic_ms(), app::coordinator_link_timeout_ms);
    xSemaphoreGive(state_mutex_);
}

bool CoordinatorLink::accept_received_command()
{
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    const bool accepted = started_.load() &&
        session_.received_command(monotonic_ms(), app::coordinator_link_timeout_ms);
    xSemaphoreGive(state_mutex_);
    return accepted;
}

esp_err_t CoordinatorLink::send_telemetry(const TelemetryMessage &message)
{
    return enqueue(type_telemetry, &message, sizeof(message));
}

esp_err_t CoordinatorLink::send_command(const RoutedCommand &command)
{
    if (config_.profile.device_role != app::DeviceRole::internet_gateway ||
        command.origin != RemoteCommandOrigin::mqtt ||
        (command.kind != RemoteCommandKind::semantic &&
         command.kind != RemoteCommandKind::read_registers &&
         command.kind != RemoteCommandKind::write_registers)) return ESP_ERR_INVALID_ARG;
    return enqueue(type_command, &command, sizeof(command));
}

RoutedCommandResult CoordinatorLink::command_route_failure(const RoutedCommand &command,
                                                          bool uncertain)
{
    RoutedCommandResult result{};
    result.source_member_id = command.target_member_id;
    result.kind = command.kind;
    const auto id = command.kind == RemoteCommandKind::semantic
        ? command.request.command_id : command.register_request.command_id;
    const auto marker = uncertain ? protocol::ModbusWriteStatus::routing_uncertain
                                  : protocol::ModbusWriteStatus::routing_not_sent;
    result.result.command_id = id;
    result.result.operation = command.request.operation;
    result.result.outcome = uncertain ? inverter::CommandOutcome::unknown
                                     : inverter::CommandOutcome::rejected;
    // No claim that an inverter write was observed. The routing marker tells
    // MQTT to report unknown write_sent for an uncertain forwarding outcome.
    result.result.write_was_sent = false;
    result.result.modbus_write.status = marker;
    result.register_result.command_id = id;
    result.register_result.space = command.register_request.space;
    result.register_result.first_register = command.register_request.first_register;
    result.register_result.register_count = command.register_request.register_count;
    result.register_result.named_request = command.register_request.named_request;
    result.register_result.outcome = uncertain ? inverter::RegisterCommandOutcome::communication_error
                                              : inverter::RegisterCommandOutcome::state_not_allowed;
    result.register_result.modbus_write.status = marker;
    return result;
}

void CoordinatorLink::report_command_failure(const RoutedCommand &command, bool uncertain,
                                             const char *reason)
{
    ESP_LOGW(tag, "Command route to member %u %s: %s (no automatic retry)",
             command.target_member_id, uncertain ? "uncertain" : "not sent", reason);
    sink_.on_command_result(command_route_failure(command, uncertain));
}

esp_err_t CoordinatorLink::send_command_result(const RoutedCommandResult &result)
{
    return enqueue(type_command_result, &result, sizeof(result));
}

esp_err_t CoordinatorLink::enqueue(std::uint8_t type, const void *payload,
                                   std::uint16_t size)
{
    if (!started_ || transmit_queue_ == nullptr || size > maximum_payload ||
        (size != 0 && payload == nullptr)) {
        return ESP_ERR_INVALID_STATE;
    }
    OutboundFrame frame{};
    frame.type = type;
    frame.size = size;
    if (size != 0) std::memcpy(frame.payload, payload, size);
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    const auto now = monotonic_ms();
    if (type == type_command && !session_.online(now, app::coordinator_link_timeout_ms)) {
        xSemaphoreGive(state_mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    frame.ticket = session_.ticket(now);
    const bool queued = xQueueSend(transmit_queue_, &frame, 0) == pdTRUE;
    xSemaphoreGive(state_mutex_);
    return queued ? ESP_OK : ESP_ERR_NO_MEM;
}

CoordinatorLink::SendResult CoordinatorLink::send(std::uint8_t type, const void *payload,
                                                 std::uint16_t size,
                                                 const coordinator_policy::Ticket *ticket)
{
    using coordinator_policy::SendDisposition;
    if (!started_ || size > maximum_payload) return {ESP_ERR_INVALID_STATE, SendDisposition::not_sent};
    std::array<std::uint8_t, sizeof(Header) + maximum_payload> frame{};
    const auto current_site_hash = site_hash(config_.site_id);
    Header header{magic, version, type, size, current_site_hash, 0};
    std::memcpy(frame.data(), &header, sizeof(header));
    if (size != 0) std::memcpy(frame.data() + sizeof(header), payload, size);
    header.crc = frame_crc(type, current_site_hash, payload, size);
    std::memcpy(frame.data(), &header, sizeof(header));
    const auto transmit_started_ms = monotonic_ms();
    if (xSemaphoreTake(send_mutex_, pdMS_TO_TICKS(coordinator_policy::transmit_deadline_ms)) != pdTRUE)
        return {ESP_ERR_TIMEOUT, SendDisposition::not_sent};
    const auto port = static_cast<uart_port_t>(inverter_gateway::app::coordinator_uart_port);
    const auto result = coordinator_policy::transmit_bounded(
        frame.data(), sizeof(header) + size, transmit_started_ms,
        coordinator_policy::transmit_deadline_ms,
        [] { return monotonic_ms(); },
        [this, ticket, port, transmit_started_ms](std::uint32_t, const std::uint8_t *bytes,
                                                std::size_t length) -> coordinator_policy::TransmitChunk {
            using coordinator_policy::TransmitAdmission;
            if (!started_.load()) return {TransmitAdmission::rejected};
            // A busy state lock consumes the same deadline, never waits here.
            if (ticket != nullptr && xSemaphoreTake(state_mutex_, 0) != pdTRUE)
                return {TransmitAdmission::wait};
            const auto now = monotonic_ms();
            if (static_cast<std::uint32_t>(now - transmit_started_ms) >=
                coordinator_policy::transmit_deadline_ms) {
                if (ticket != nullptr) xSemaphoreGive(state_mutex_);
                return {TransmitAdmission::deadline};
            }
            if (ticket != nullptr && !session_.accepts(*ticket, now, app::coordinator_link_timeout_ms)) {
                xSemaphoreGive(state_mutex_);
                return {TransmitAdmission::rejected};
            }
            // Hold the short state lock from this final ticket check through
            // FIFO insertion, so another core cannot invalidate between them.
            // UART TX buffer is disabled. This is the only UART transmitter,
            // so the driver's internal TX mutex is uncontended; it only fills
            // currently free FIFO slots and cannot wait for FIFO capacity.
            const int written = length == 0 ? 0 :
                uart_tx_chars(port, reinterpret_cast<const char *>(bytes),
                              static_cast<std::uint32_t>(length));
            if (ticket != nullptr) xSemaphoreGive(state_mutex_);
            return {TransmitAdmission::ready, written};
        },
        [port] {
            using coordinator_policy::TransmitCompletion;
            const auto status = uart_wait_tx_done(port, 0);
            return status == ESP_OK ? TransmitCompletion::complete :
                status == ESP_ERR_TIMEOUT ? TransmitCompletion::pending : TransmitCompletion::error;
        },
        [] { vTaskDelay(1); });
    xSemaphoreGive(send_mutex_);
    // An old/expired ticket must not tear down a newer healthy session.
    if (result.stop == coordinator_policy::TransmitStop::deadline ||
        result.stop == coordinator_policy::TransmitStop::io_error) {
        xSemaphoreTake(state_mutex_, portMAX_DELAY);
        session_.invalidate();
        xSemaphoreGive(state_mutex_);
    }
    const auto error = result.stop == coordinator_policy::TransmitStop::none ? ESP_OK :
        result.stop == coordinator_policy::TransmitStop::deadline ? ESP_ERR_TIMEOUT :
        result.stop == coordinator_policy::TransmitStop::rejected ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    return {error, result.disposition};
}

void CoordinatorLink::worker_task(void *argument)
{
    auto *self = static_cast<CoordinatorLink *>(argument);
    if (self != nullptr) self->run();
    vTaskDelete(nullptr);
}

void CoordinatorLink::run()
{
    const auto port = static_cast<uart_port_t>(inverter_gateway::app::coordinator_uart_port);
    std::array<std::uint8_t, maximum_payload> payload{};
    std::int64_t last_heartbeat_ms = 0;
    OutboundFrame outbound{};
    while (started_) {
        const std::int64_t now = esp_timer_get_time() / 1000;
        if (now - last_heartbeat_ms >= app::coordinator_heartbeat_interval_ms) {
            IdentityPayload identity{};
            identity.role = static_cast<std::uint8_t>(config_.profile.device_role);
            std::strncpy(identity.site_id, config_.site_id,
                         sizeof(identity.site_id) - 1);
            send(type_heartbeat, &identity, sizeof(identity));
            last_heartbeat_ms = now;
        }
        bool have_outbound = false;
        xSemaphoreTake(state_mutex_, portMAX_DELAY);
        const bool link_ready = session_.online(monotonic_ms(), app::coordinator_link_timeout_ms);
        if (xQueueReceive(transmit_queue_, &outbound, 0) == pdTRUE) {
            if (link_ready || outbound.type == type_command) have_outbound = true;
            else {
                // Keep non-command data for the existing reconnect workflow.
                // Rotate it so old commands behind it are rejected offline.
                xQueueSend(transmit_queue_, &outbound, 0);
            }
        }
        xSemaphoreGive(state_mutex_);
        if (have_outbound) {
            const bool command_frame = outbound.type == type_command;
            const auto result = send(outbound.type, outbound.payload, outbound.size,
                                     command_frame ? &outbound.ticket : nullptr);
            if (result.disposition != coordinator_policy::SendDisposition::forwarded) {
                if (command_frame && outbound.size == sizeof(RoutedCommand)) {
                    RoutedCommand command{};
                    std::memcpy(&command, outbound.payload, sizeof(command));
                    report_command_failure(command,
                        result.disposition == coordinator_policy::SendDisposition::uncertain,
                        esp_err_to_name(result.error));
                } else ESP_LOGW(tag, "UART forwarding failed: %s", esp_err_to_name(result.error));
            }
        }
        std::uint8_t first = 0;
        if (uart_read_bytes(port, &first, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (first != static_cast<std::uint8_t>(magic & 0xff)) continue;
        Header header{};
        reinterpret_cast<std::uint8_t *>(&header)[0] = first;
        const int header_tail = uart_read_bytes(port,
            reinterpret_cast<std::uint8_t *>(&header) + 1, sizeof(header) - 1,
            pdMS_TO_TICKS(100));
        if (header_tail != static_cast<int>(sizeof(header) - 1) ||
            header.magic_value != magic || header.protocol_version != version ||
            header.payload_size > payload.size()) continue;
        if (header.payload_size != 0 &&
            uart_read_bytes(port, payload.data(), header.payload_size,
                            pdMS_TO_TICKS(250)) != header.payload_size) continue;
        const auto crc = frame_crc(header.type, header.site_hash,
                                   payload.data(), header.payload_size);
        if (crc != header.crc) continue;
        if (header.site_hash != site_hash(config_.site_id)) {
            const bool can_adopt =
                !config_.provisioned &&
                config_.profile.device_role == app::DeviceRole::internet_gateway &&
                header.type == type_heartbeat &&
                header.payload_size == sizeof(IdentityPayload);
            if (!can_adopt) continue;
            IdentityPayload identity{};
            std::memcpy(&identity, payload.data(), sizeof(identity));
            if (identity.role != static_cast<std::uint8_t>(app::DeviceRole::parallel_coordinator) ||
                std::memchr(identity.site_id, '\0', sizeof(identity.site_id)) == nullptr ||
                !app::valid_site_id(identity.site_id)) continue;
            std::strncpy(config_.site_id, identity.site_id,
                         sizeof(config_.site_id) - 1);
            config_.site_id[sizeof(config_.site_id) - 1] = '\0';
            if (app::ConfigStore{}.save(config_) != ESP_OK) {
                ESP_LOGE(tag, "Could not save installation ID received from master ESP");
                continue;
            }
            ESP_LOGI(tag, "Adopted installation ID %s from master ESP; restarting",
                     config_.site_id);
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_restart();
        }
        // A command cannot establish/re-establish its own expired link. The
        // existing protocol has no remote expiry/session nonce, so require a
        // valid peer heartbeat before accepting commands after an observed gap.
        const bool coordinator = config_.profile.device_role == app::DeviceRole::parallel_coordinator;
        const bool gateway = config_.profile.device_role == app::DeviceRole::internet_gateway;
        if (header.type == type_heartbeat && header.payload_size == sizeof(IdentityPayload)) {
            IdentityPayload identity{};
            std::memcpy(&identity, payload.data(), sizeof(identity));
            const auto expected_role = coordinator ? app::DeviceRole::internet_gateway
                                                   : app::DeviceRole::parallel_coordinator;
            if (identity.role != static_cast<std::uint8_t>(expected_role) ||
                std::memchr(identity.site_id, '\0', sizeof(identity.site_id)) == nullptr ||
                std::strcmp(identity.site_id, config_.site_id) != 0) continue;
            note_receive();
        } else if (header.type == type_command && coordinator && header.payload_size == sizeof(RoutedCommand)) {
            RoutedCommand command{};
            std::memcpy(&command, payload.data(), sizeof(command));
            if (command.origin != RemoteCommandOrigin::mqtt ||
                (command.kind != RemoteCommandKind::semantic &&
                 command.kind != RemoteCommandKind::read_registers &&
                 command.kind != RemoteCommandKind::write_registers)) continue;
            if (!accept_received_command()) {
                report_command_failure(command, false, "no current coordinator-link session");
                continue;
            }
        } else if (gateway &&
                   ((header.type == type_telemetry && header.payload_size == sizeof(TelemetryMessage)) ||
                    (header.type == type_command_result && header.payload_size == sizeof(RoutedCommandResult)))) {
            note_receive();
        } else continue;
        dispatch(header.type, payload.data(), header.payload_size);
    }
}

void CoordinatorLink::dispatch(std::uint8_t type, const std::uint8_t *payload,
                               std::uint16_t size)
{
    const bool coordinator = config_.profile.device_role == app::DeviceRole::parallel_coordinator;
    const bool gateway = config_.profile.device_role == app::DeviceRole::internet_gateway;
    if (type == type_telemetry && gateway && size == sizeof(TelemetryMessage)) {
        TelemetryMessage message{};
        std::memcpy(&message, payload, size);
        sink_.on_telemetry(message);
    } else if (type == type_command && coordinator && size == sizeof(RoutedCommand)) {
        RoutedCommand message{};
        std::memcpy(&message, payload, size);
        sink_.on_command(message);
    } else if (type == type_command_result && gateway &&
               size == sizeof(RoutedCommandResult)) {
        RoutedCommandResult message{};
        std::memcpy(&message, payload, size);
        sink_.on_command_result(message);
    }
}

static_assert(sizeof(TelemetryMessage) <= maximum_payload);
static_assert(sizeof(IdentityPayload) <= maximum_payload);
static_assert(sizeof(RoutedCommand) <= maximum_payload);
static_assert(sizeof(RoutedCommandResult) <= maximum_payload);

} // namespace inverter_gateway::network
