#include "inverter_gateway/network/espnow_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "esp_wifi.h"
#include "inverter_gateway/app/project_config.hpp"

namespace inverter_gateway::network {
namespace {

constexpr char tag[] = "espnow_mesh";
constexpr std::uint16_t wire_magic = 0x4D58;
constexpr std::uint8_t wire_version = 2;
constexpr std::uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

enum WireType : std::uint8_t {
    hello = 1,
    telemetry = 2,
    command = 3,
    command_result = 4,
    protocol_telemetry = 5,
    protocol_command = 6,
    protocol_command_result = 7,
};

#pragma pack(push, 1)
struct WireHeader {
    std::uint16_t magic;
    std::uint8_t version;
    std::uint8_t type;
    std::uint16_t payload_size;
    std::uint32_t sequence;
    std::uint32_t site_hash;
};

struct HelloPayload {
    std::uint8_t role;
    std::uint8_t member_id;
    std::uint8_t topology;
    std::uint8_t inverter_count;
    char site_id[app::max_site_id_length + 1];
};
#pragma pack(pop)

constexpr std::size_t max_packet_size = 250;

struct ReceivedPacket {
    std::uint8_t source[6];
    std::uint16_t length;
    std::uint8_t data[max_packet_size];
};

bool same_mac(const std::uint8_t *a, const std::uint8_t *b)
{
    return std::memcmp(a, b, 6) == 0;
}

bool valid_protocol(protocol::EnergyProtocol value)
{
    return static_cast<std::uint8_t>(value) < static_cast<std::uint8_t>(protocol::EnergyProtocol::count);
}

bool valid_bool(const bool &value)
{
    return *reinterpret_cast<const std::uint8_t *>(&value) <= 1;
}

bool valid_terminated(const char *text, std::size_t capacity, bool allow_empty = true)
{
    return (allow_empty || text[0] != '\0') && std::memchr(text, '\0', capacity) != nullptr;
}

bool valid_protocol_telemetry(const ProtocolTelemetryMessage &message)
{
    if (!valid_protocol(message.protocol) || !valid_bool(message.final_chunk) || message.field_count > 2 ||
        message.chunk_count == 0 || message.chunk_count > 48 || message.chunk_index >= message.chunk_count ||
        message.final_chunk != (message.chunk_index + 1 == message.chunk_count) ||
        !valid_terminated(message.command, sizeof(message.command), false)) return false;
    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto &field = message.fields[i];
        if (!valid_bool(field.numeric) || !valid_terminated(field.key, sizeof(field.key), false) ||
            !valid_terminated(field.unit, sizeof(field.unit)) ||
            (field.numeric ? !std::isfinite(field.value) :
                !valid_terminated(field.text, sizeof(field.text)))) return false;
    }
    return true;
}

bool valid_protocol_command(const ProtocolCommand &message)
{
    return valid_protocol(message.protocol) && message.origin == RemoteCommandOrigin::mqtt &&
        valid_bool(message.write) && valid_bool(message.confirmed) &&
        valid_terminated(message.command, sizeof(message.command), false);
}

bool valid_protocol_result(const ProtocolCommandResult &message)
{
    return valid_protocol(message.protocol) && valid_bool(message.write_sent) &&
        valid_bool(message.write_sent_known) &&
        static_cast<std::uint8_t>(message.outcome) <=
            static_cast<std::uint8_t>(ProtocolCommandOutcome::invalid_response) &&
        valid_terminated(message.command, sizeof(message.command), false) &&
        valid_terminated(message.detail, sizeof(message.detail));
}

std::uint32_t site_hash(const char *text)
{
    std::uint32_t hash = 2166136261U;
    while (text != nullptr && *text != '\0') {
        hash = (hash ^ static_cast<std::uint8_t>(*text++)) * 16777619U;
    }
    return hash;
}

esp_err_t ensure_peer(const std::uint8_t *mac, std::uint8_t channel)
{
    if (esp_now_is_peer_exist(mac)) return ESP_OK;
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

} // namespace

EspNowMesh *EspNowMesh::instance_ = nullptr;

EspNowMesh::EspNowMesh(app::SystemConfig &config, RuntimeMessageSink &sink)
    : config_(config), sink_(sink)
{
}

esp_err_t EspNowMesh::start()
{
    if (started_) return ESP_OK;
    if (instance_ != nullptr) return ESP_ERR_INVALID_STATE;
    receive_queue_ = xQueueCreate(8, sizeof(ReceivedPacket));
    if (receive_queue_ == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t result = esp_now_init();
    if (result != ESP_OK) {
        vQueueDelete(receive_queue_);
        receive_queue_ = nullptr;
        return result;
    }
    instance_ = this;
    result = esp_now_register_recv_cb(receive_callback);
    if (result != ESP_OK) {
        instance_ = nullptr;
        esp_now_deinit();
        vQueueDelete(receive_queue_);
        receive_queue_ = nullptr;
        return result;
    }
    result = ensure_peer(broadcast_mac, config_.profile.espnow_channel);
    if (result != ESP_OK) {
        stop();
        return result;
    }
    for (const auto &peer : config_.peers) {
        if (peer.valid) ensure_peer(peer.mac, config_.profile.espnow_channel);
    }
    started_ = true;
    if (xTaskCreate(worker_task, "espnow_mesh", 4096, this, 5,
                    &worker_task_) != pdPASS) {
        stop();
        return ESP_ERR_NO_MEM;
    }
    return announce();
}

void EspNowMesh::stop()
{
    if (!started_ && instance_ != this) return;
    if (worker_task_ != nullptr) {
        vTaskDelete(worker_task_);
        worker_task_ = nullptr;
    }
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    if (receive_queue_ != nullptr) {
        vQueueDelete(receive_queue_);
        receive_queue_ = nullptr;
    }
    started_ = false;
    if (instance_ == this) instance_ = nullptr;
}

esp_err_t EspNowMesh::announce()
{
    HelloPayload hello_message{};
    hello_message.role = static_cast<std::uint8_t>(config_.profile.device_role);
    hello_message.member_id = config_.profile.logical_member_id;
    hello_message.topology = static_cast<std::uint8_t>(config_.profile.topology);
    hello_message.inverter_count = config_.profile.expected_inverter_count;
    std::strncpy(hello_message.site_id, config_.site_id,
                 sizeof(hello_message.site_id) - 1);
    return send_packet(broadcast_mac, WireType::hello, &hello_message,
                       sizeof(hello_message));
}

esp_err_t EspNowMesh::send_telemetry(const TelemetryMessage &message)
{
    const std::uint8_t *destination = broadcast_mac;
    if (app::role_is_espnow_member(config_.profile.device_role)) {
        if (const auto *coordinator = peer_for_member(0)) destination = coordinator;
    }
    return send_packet(destination, WireType::telemetry, &message, sizeof(message));
}

esp_err_t EspNowMesh::send_command(const RoutedCommand &command)
{
    const auto *destination = peer_for_member(command.target_member_id);
    if (destination == nullptr) return ESP_ERR_NOT_FOUND;
    return send_packet(destination, WireType::command, &command, sizeof(command));
}

esp_err_t EspNowMesh::send_command_result(const RoutedCommandResult &result)
{
    const auto *destination = peer_for_member(0);
    if (destination == nullptr) destination = broadcast_mac;
    return send_packet(destination, WireType::command_result, &result, sizeof(result));
}

esp_err_t EspNowMesh::send_protocol_telemetry(const ProtocolTelemetryMessage &message)
{
    if (!valid_protocol_telemetry(message) ||
        message.source_member_id != config_.profile.logical_member_id) return ESP_ERR_INVALID_ARG;
    const std::uint8_t *destination = broadcast_mac;
    if (app::role_is_espnow_member(config_.profile.device_role)) {
        if (const auto *coordinator = peer_for_member(0)) destination = coordinator;
    }
    return send_packet(destination, WireType::protocol_telemetry, &message, sizeof(message));
}

esp_err_t EspNowMesh::send_protocol_command(const ProtocolCommand &message)
{
    if (!app::role_is_espnow_coordinator(config_.profile.device_role) ||
        !valid_protocol_command(message)) return ESP_ERR_INVALID_ARG;
    const auto *destination = peer_for_member(message.target_member_id);
    if (destination == nullptr) return ESP_ERR_NOT_FOUND;
    return send_packet(destination, WireType::protocol_command, &message, sizeof(message));
}

esp_err_t EspNowMesh::send_protocol_command_result(const ProtocolCommandResult &result)
{
    if (!app::role_is_espnow_member(config_.profile.device_role) ||
        !valid_protocol_result(result) ||
        result.source_member_id != config_.profile.logical_member_id) return ESP_ERR_INVALID_ARG;
    const auto *destination = peer_for_member(0);
    if (destination == nullptr) destination = broadcast_mac;
    return send_packet(destination, WireType::protocol_command_result, &result, sizeof(result));
}

std::size_t EspNowMesh::discovered_member_count() const
{
    return std::count_if(config_.peers.begin(), config_.peers.end(),
                         [](const app::SavedPeer &peer) {
                             return peer.valid && peer.logical_member_id != 0 &&
                                    peer.logical_member_id != 0xff;
                         });
}

bool EspNowMesh::has_coordinator() const
{
    return peer_for_member(0) != nullptr;
}

void EspNowMesh::receive_callback(const esp_now_recv_info_t *info,
                                  const std::uint8_t *data, int length)
{
    if (instance_ != nullptr && instance_->receive_queue_ != nullptr && info != nullptr &&
        data != nullptr && length > 0 && static_cast<std::size_t>(length) <= max_packet_size) {
        ReceivedPacket packet{};
        std::memcpy(packet.source, info->src_addr, sizeof(packet.source));
        packet.length = static_cast<std::uint16_t>(length);
        std::memcpy(packet.data, data, packet.length);
        xQueueSend(instance_->receive_queue_, &packet, 0);
    }
}

void EspNowMesh::worker_task(void *argument)
{
    auto *self = static_cast<EspNowMesh *>(argument);
    ReceivedPacket packet{};
    const TickType_t announce_interval = std::max<TickType_t>(
        1, pdMS_TO_TICKS(app::espnow_announce_interval_ms));
    TickType_t last_announce = xTaskGetTickCount();
    while (self != nullptr && self->started_) {
        const TickType_t elapsed = xTaskGetTickCount() - last_announce;
        const TickType_t wait = elapsed < announce_interval ? announce_interval - elapsed : 0;
        if (xQueueReceive(self->receive_queue_, &packet,
                          wait) == pdTRUE) {
            self->receive(packet.source, packet.data, packet.length);
        }
        // A busy receive queue must not suppress discovery announcements.
        const TickType_t now = xTaskGetTickCount();
        if (now - last_announce >= announce_interval) {
            self->announce();
            last_announce = now;
        }
    }
    vTaskDelete(nullptr);
}

void EspNowMesh::receive(const std::uint8_t *source, const std::uint8_t *data,
                         std::size_t length)
{
    if (length < sizeof(WireHeader)) return;
    WireHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != wire_magic || header.version != wire_version ||
        header.payload_size != length - sizeof(header)) return;
    const std::uint8_t *payload = data + sizeof(header);
    const bool site_matches = header.site_hash == site_hash(config_.site_id);

    if (header.type == WireType::hello && header.payload_size == sizeof(HelloPayload)) {
        HelloPayload message{};
        std::memcpy(&message, payload, sizeof(message));
        if (std::memchr(message.site_id, '\0', sizeof(message.site_id)) == nullptr ||
            !app::valid_site_id(message.site_id) ||
            message.topology != static_cast<std::uint8_t>(config_.profile.topology) ||
            message.inverter_count != config_.profile.expected_inverter_count) return;
        const auto remote_role = static_cast<app::DeviceRole>(message.role);
        if (app::role_is_espnow_coordinator(config_.profile.device_role) &&
            app::role_is_espnow_member(remote_role) && message.member_id != 0 &&
            (site_matches || !config_.provisioned)) {
            learn_peer(source, message.member_id);
            accept_sequence(source, header.sequence, true);
            announce();
        } else if (app::role_is_espnow_member(config_.profile.device_role) &&
                   app::role_is_espnow_coordinator(remote_role) &&
                   (site_matches || !config_.provisioned)) {
            if (!site_matches) {
                config_.peers.fill({});
                std::strncpy(config_.site_id, message.site_id,
                             sizeof(config_.site_id) - 1);
                config_.site_id[sizeof(config_.site_id) - 1] = '\0';
                ESP_LOGI(tag, "Adopted installation ID %s from master ESP",
                         config_.site_id);
            }
            learn_peer(source, 0);
            accept_sequence(source, header.sequence, true);
        }
        return;
    }

    if (!site_matches) return;

    if (header.type == WireType::telemetry && header.payload_size == sizeof(TelemetryMessage) &&
        app::role_is_espnow_coordinator(config_.profile.device_role)) {
        TelemetryMessage message{};
        std::memcpy(&message, payload, sizeof(message));
        const auto *known = peer_for_member(message.source_member_id);
        if (known != nullptr && same_mac(known, source) &&
            accept_sequence(source, header.sequence, false)) sink_.on_telemetry(message);
    } else if (header.type == WireType::command && header.payload_size == sizeof(RoutedCommand)) {
        const bool member_accepts = app::role_is_espnow_member(config_.profile.device_role) &&
                                    peer_for_member(0) != nullptr && same_mac(peer_for_member(0), source);
        if (member_accepts && accept_sequence(source, header.sequence, false)) {
            RoutedCommand message{};
            std::memcpy(&message, payload, sizeof(message));
            if (message.target_member_id == config_.profile.logical_member_id) {
                sink_.on_command(message);
            }
        }
    } else if (header.type == WireType::command_result &&
               header.payload_size == sizeof(RoutedCommandResult) &&
               app::role_is_espnow_coordinator(config_.profile.device_role)) {
        RoutedCommandResult message{};
        std::memcpy(&message, payload, sizeof(message));
        const auto *known = peer_for_member(message.source_member_id);
        if (known != nullptr && same_mac(known, source) &&
            accept_sequence(source, header.sequence, false)) sink_.on_command_result(message);
    } else if (header.type == WireType::protocol_telemetry &&
               header.payload_size == sizeof(ProtocolTelemetryMessage) &&
               app::role_is_espnow_coordinator(config_.profile.device_role)) {
        ProtocolTelemetryMessage message{};
        std::memcpy(&message, payload, sizeof(message));
        const auto *known = peer_for_member(message.source_member_id);
        if (valid_protocol_telemetry(message) && known != nullptr && same_mac(known, source) &&
            accept_sequence(source, header.sequence, false)) sink_.on_protocol_telemetry(message);
    } else if (header.type == WireType::protocol_command &&
               header.payload_size == sizeof(ProtocolCommand) &&
               app::role_is_espnow_member(config_.profile.device_role)) {
        const auto *coordinator = peer_for_member(0);
        if (coordinator == nullptr || !same_mac(coordinator, source)) return;
        ProtocolCommand message{};
        std::memcpy(&message, payload, sizeof(message));
        if (valid_protocol_command(message) &&
            message.target_member_id == config_.profile.logical_member_id &&
            accept_sequence(source, header.sequence, false)) sink_.on_protocol_command(message);
    } else if (header.type == WireType::protocol_command_result &&
               header.payload_size == sizeof(ProtocolCommandResult) &&
               app::role_is_espnow_coordinator(config_.profile.device_role)) {
        ProtocolCommandResult message{};
        std::memcpy(&message, payload, sizeof(message));
        const auto *known = peer_for_member(message.source_member_id);
        if (valid_protocol_result(message) && known != nullptr && same_mac(known, source) &&
            accept_sequence(source, header.sequence, false)) sink_.on_protocol_command_result(message);
    }
}

esp_err_t EspNowMesh::send_packet(const std::uint8_t *destination, std::uint8_t type,
                                  const void *payload, std::size_t payload_size)
{
    if (!started_ && type != WireType::hello) return ESP_ERR_INVALID_STATE;
    if (sizeof(WireHeader) + payload_size > max_packet_size) return ESP_ERR_INVALID_SIZE;
    std::array<std::uint8_t, max_packet_size> packet{};
    const WireHeader header{wire_magic, wire_version, type,
                            static_cast<std::uint16_t>(payload_size),
                            sequence_.fetch_add(1, std::memory_order_relaxed) + 1U,
                            site_hash(config_.site_id)};
    std::memcpy(packet.data(), &header, sizeof(header));
    std::memcpy(packet.data() + sizeof(header), payload, payload_size);
    return esp_now_send(destination, packet.data(), sizeof(header) + payload_size);
}

const std::uint8_t *EspNowMesh::peer_for_member(std::uint8_t member_id) const
{
    for (const auto &peer : config_.peers) {
        if (peer.valid && peer.logical_member_id == member_id) return peer.mac;
    }
    return nullptr;
}

void EspNowMesh::learn_peer(const std::uint8_t *mac, std::uint8_t member_id)
{
    for (auto &peer : config_.peers) {
        if (peer.valid && (peer.logical_member_id == member_id || same_mac(peer.mac, mac))) {
            if (config_.provisioned && !same_mac(peer.mac, mac)) {
                ESP_LOGW(tag, "Rejected MAC change for provisioned member %u", member_id);
                return;
            }
            if (peer.logical_member_id == member_id && same_mac(peer.mac, mac)) {
                // Repeated HELLOs refresh discovery, not persistent configuration.
                ensure_peer(mac, config_.profile.espnow_channel);
                return;
            }
            std::memcpy(peer.mac, mac, sizeof(peer.mac));
            peer.logical_member_id = member_id;
            ensure_peer(mac, config_.profile.espnow_channel);
            app::ConfigStore{}.save(config_);
            return;
        }
    }
    if (config_.provisioned) {
        ESP_LOGW(tag, "Rejected unknown peer while provisioning is closed");
        return;
    }
    for (auto &peer : config_.peers) {
        if (!peer.valid) {
            std::memcpy(peer.mac, mac, sizeof(peer.mac));
            peer.logical_member_id = member_id;
            peer.valid = true;
            ensure_peer(mac, config_.profile.espnow_channel);
            app::ConfigStore{}.save(config_);
            ESP_LOGI(tag, "Saved ESP-NOW peer member %u", member_id);
            return;
        }
    }
    ESP_LOGW(tag, "Peer table full; ignored member %u", member_id);
}

bool EspNowMesh::accept_sequence(const std::uint8_t *mac, std::uint32_t sequence,
                                 bool reset)
{
    for (std::size_t index = 0; index < config_.peers.size(); ++index) {
        if (!config_.peers[index].valid || !same_mac(config_.peers[index].mac, mac)) continue;
        if (!reset && sequence <= last_peer_sequence_[index]) {
            ESP_LOGW(tag, "Rejected duplicate/out-of-order packet from member %u",
                     config_.peers[index].logical_member_id);
            return false;
        }
        last_peer_sequence_[index] = sequence;
        return true;
    }
    return false;
}

static_assert(sizeof(WireHeader) + sizeof(TelemetryMessage) <= max_packet_size);
static_assert(sizeof(WireHeader) + sizeof(HelloPayload) <= max_packet_size);
static_assert(sizeof(WireHeader) + sizeof(RoutedCommand) <= max_packet_size);
static_assert(sizeof(WireHeader) + sizeof(RoutedCommandResult) <= max_packet_size);
static_assert(sizeof(WireHeader) + sizeof(ProtocolTelemetryMessage) <= max_packet_size);
static_assert(sizeof(WireHeader) + sizeof(ProtocolCommand) <= max_packet_size);
static_assert(sizeof(WireHeader) + sizeof(ProtocolCommandResult) <= max_packet_size);

} // namespace inverter_gateway::network
