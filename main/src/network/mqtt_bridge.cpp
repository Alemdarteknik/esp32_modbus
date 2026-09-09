#include "inverter_gateway/network/mqtt_bridge.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <new>
#include <initializer_list>

#include <sys/socket.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/app/rgb_debug.hpp"
#include "inverter_gateway/inverter/inverter_command.hpp"
#include "inverter_gateway/inverter/register_catalog.hpp"
#include "inverter_gateway/inverter/register_value.hpp"

namespace inverter_gateway::network {
namespace {

constexpr char tag[] = "mqtt_bridge";
constexpr std::size_t mqtt_outbox_limit_bytes = 64U * 1024U;
std::atomic<std::uint32_t> mqtt_enqueue_failures{0};
std::atomic<std::uint32_t> telemetry_queue_drops{0};

// Only for literal keys and strings owned by the static register catalog.
// Borrowing immutable metadata avoids duplicating it for every live reading.
bool add_catalog_string(cJSON *object, const char *key, const char *value)
{
    cJSON *item = cJSON_CreateStringReference(value != nullptr ? value : "");
    if (item == nullptr) return false;
    if (!cJSON_AddItemToObjectCS(object, key, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

// Bound the unformatted JSON size before allocating. cJSON's default printer
// grows by doubling, temporarily requiring both old and new output buffers.
std::size_t json_string_capacity(const char *value)
{
    std::size_t size = 2;
    if (value == nullptr) return size;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        size += *p < 32 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
    }
    return size;
}

std::size_t json_capacity(const cJSON *item)
{
    if (cJSON_IsString(item)) return json_string_capacity(item->valuestring);
    if (cJSON_IsRaw(item)) return item->valuestring ? std::strlen(item->valuestring) : 4;
    if (cJSON_IsNumber(item)) return 64;
    if (cJSON_IsArray(item) || cJSON_IsObject(item)) {
        std::size_t size = 2;
        for (const cJSON *child = item->child; child != nullptr; child = child->next) {
            size += json_capacity(child) + 1;
            if (cJSON_IsObject(item)) size += json_string_capacity(child->string) + 1;
        }
        return size;
    }
    return 5;
}

int enqueue_payload(esp_mqtt_client_handle_t client, const char *topic,
                    const char *payload, int qos, int retain)
{
    if (client == nullptr || payload == nullptr) return -1;
    const std::size_t bytes = std::strlen(payload) + std::strlen(topic) + 16U;
    const int queued = esp_mqtt_client_get_outbox_size(client);
    if (queued < 0 || bytes > mqtt_outbox_limit_bytes ||
        static_cast<std::size_t>(queued) > mqtt_outbox_limit_bytes - bytes) {
        app::rgb_debug_publish_error();
        ++mqtt_enqueue_failures;
        ESP_LOGW(tag, "MQTT outbox full; skipped %s (%u bytes queued)",
                 topic, static_cast<unsigned>(queued < 0 ? 0 : queued));
        return -2;
    }
    const int result = esp_mqtt_client_enqueue(client, topic, payload, 0, qos, retain, true);
    if (result < 0) {
        ++mqtt_enqueue_failures;
        app::rgb_debug_publish_error();
        ESP_LOGW(tag, "MQTT enqueue failed (%d): %s", result, topic);
    }
    return result;
}
bool uses_tls(const char *uri)
{
    return uri != nullptr &&
           (std::strncmp(uri, "mqtts://", 8) == 0 ||
            std::strncmp(uri, "wss://", 6) == 0);
}

enum class OutboundType : std::uint8_t { telemetry, command_result };

struct OutboundMessage {
    OutboundType type;
    TelemetryMessage telemetry{};
    RoutedCommandResult command_result{};
};

bool parse_operation(const char *name, inverter::LocalOperation &operation)
{
    if (name == nullptr) return false;
    constexpr inverter::LocalOperation operations[] = {
        inverter::LocalOperation::set_buzzer_enabled,
        inverter::LocalOperation::set_bluetooth_enabled,
        inverter::LocalOperation::set_inverter_enabled,
        inverter::LocalOperation::set_overload_restart_enabled,
        inverter::LocalOperation::set_overload_to_bypass_enabled,
        inverter::LocalOperation::set_battery_type,
        inverter::LocalOperation::set_eco_mode_enabled,
        inverter::LocalOperation::set_overtemperature_restart_enabled,
        inverter::LocalOperation::set_input_change_reminder_enabled,
        inverter::LocalOperation::set_output_split_phase_enabled,
        inverter::LocalOperation::set_generator_auto_input_enabled,
        inverter::LocalOperation::set_dual_channel_load_enabled,
        inverter::LocalOperation::set_grid_feedback_enabled,
    };
    for (const auto candidate : operations) {
        if (std::strcmp(name, inverter::operation_name(candidate)) == 0) {
            operation = candidate;
            return true;
        }
    }
    return false;
}

bool parse_command_id(const cJSON *item, std::uint64_t &command_id)
{
    if (cJSON_IsNumber(item) && item->valuedouble >= 1 &&
        item->valuedouble <= 9007199254740991.0 &&
        item->valuedouble == static_cast<double>(static_cast<std::uint64_t>(item->valuedouble))) {
        command_id = static_cast<std::uint64_t>(item->valuedouble);
        return true;
    }
    if (cJSON_IsString(item)) {
        if (item->valuestring == nullptr || item->valuestring[0] == '\0') return false;
        for (const char *character = item->valuestring; *character != '\0'; ++character) {
            if (*character < '0' || *character > '9') return false;
        }
        errno = 0;
        char *end = nullptr;
        command_id = std::strtoull(item->valuestring, &end, 10);
        return errno != ERANGE && command_id != 0 && end != item->valuestring && *end == '\0';
    }
    return false;
}

bool parse_named_category(const cJSON *item, inverter::RegisterSpace &space)
{
    if (!cJSON_IsString(item)) return false;
    if (std::strcmp(item->valuestring, "configuration") == 0) {
        space = inverter::RegisterSpace::holding;
        return true;
    }
    if (std::strcmp(item->valuestring, "input_data") == 0) {
        space = inverter::RegisterSpace::input;
        return true;
    }
    return false;
}

bool parse_register_domain(const cJSON *item, inverter::RegisterDomain &domain)
{
    if (!cJSON_IsString(item)) return false;
    constexpr inverter::RegisterDomain domains[] = {
        inverter::RegisterDomain::identity, inverter::RegisterDomain::communications,
        inverter::RegisterDomain::system, inverter::RegisterDomain::inverter,
        inverter::RegisterDomain::grid, inverter::RegisterDomain::eps,
        inverter::RegisterDomain::pv, inverter::RegisterDomain::generator,
        inverter::RegisterDomain::battery, inverter::RegisterDomain::bms,
        inverter::RegisterDomain::load, inverter::RegisterDomain::power,
        inverter::RegisterDomain::energy, inverter::RegisterDomain::parallel,
        inverter::RegisterDomain::diagnostics, inverter::RegisterDomain::reserved,
    };
    for (const auto candidate : domains) {
        std::string expected = inverter::register_domain_name(candidate);
        std::transform(expected.begin(), expected.end(), expected.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(character >= 'A' && character <= 'Z'
                                                        ? character - 'A' + 'a'
                                                        : character);
                       });
        if (expected == item->valuestring) {
            domain = candidate;
            return true;
        }
    }
    return false;
}

const inverter::RegisterDescriptor *find_named_register(
    inverter::RegisterSpace space, const char *name, const char *description,
    const inverter::RegisterDomain *domain)
{
    if (name == nullptr || name[0] == '\0') return nullptr;
    const auto catalog = space == inverter::RegisterSpace::holding
                             ? inverter::holding_register_catalog()
                             : inverter::input_register_catalog();
    const inverter::RegisterDescriptor *match = nullptr;
    for (std::size_t index = 0; index < catalog.size; ++index) {
        const auto &candidate = catalog.data[index];
        if (std::strcmp(candidate.name, name) != 0) continue;
        if (domain != nullptr && candidate.domain != *domain) continue;
        if (description != nullptr && description[0] != '\0' &&
            std::strcmp(candidate.description, description) != 0) continue;
        if (match != nullptr) return nullptr;
        match = &candidate;
    }
    return match;
}

bool encode_named_value(
    const inverter::RegisterDescriptor &descriptor, const cJSON *item,
    std::array<std::uint16_t, inverter::register_command_word_count> &destination,
    std::uint16_t &word_count)
{
    if (cJSON_IsBool(item)) {
        if (descriptor.data_type != inverter::RegisterDataType::uint8 &&
            descriptor.data_type != inverter::RegisterDataType::uint16 &&
            descriptor.data_type != inverter::RegisterDataType::unknown) return false;
        destination[0] = cJSON_IsTrue(item) ? 1 : 0;
        word_count = 1;
        return true;
    }
    if (cJSON_IsString(item) &&
        (descriptor.data_type == inverter::RegisterDataType::ascii ||
         descriptor.data_type == inverter::RegisterDataType::ascii_or_uint16)) {
        const std::size_t length = std::strlen(item->valuestring);
        if (length == 0 || length > 2) return false;
        destination[0] = static_cast<std::uint16_t>(
            static_cast<std::uint8_t>(item->valuestring[0]) << 8U);
        if (length == 2) {
            destination[0] |= static_cast<std::uint8_t>(item->valuestring[1]);
        }
        word_count = 1;
        return true;
    }
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        descriptor.scale <= 0.0) return false;

    double encoded = item->valuedouble / descriptor.scale;
    if (descriptor.space == inverter::RegisterSpace::holding &&
        descriptor.address == 21 && item->valuedouble >= 2000.0 &&
        item->valuedouble <= 2099.0) {
        encoded = item->valuedouble - 2000.0;
    }
    const double rounded = std::round(encoded);
    if (std::fabs(encoded - rounded) > 0.000001) return false;
    switch (descriptor.data_type) {
    case inverter::RegisterDataType::sint16:
        if (rounded < -32768.0 || rounded > 32767.0) return false;
        destination[0] = static_cast<std::uint16_t>(
            static_cast<std::int16_t>(rounded));
        word_count = 1;
        return true;
    case inverter::RegisterDataType::uint32:
        if (rounded < 0.0 || rounded > 4294967295.0) return false;
        {
            const auto raw = static_cast<std::uint32_t>(rounded);
            destination[0] = static_cast<std::uint16_t>(raw >> 16U);
            destination[1] = static_cast<std::uint16_t>(raw);
            word_count = 2;
        }
        return true;
    case inverter::RegisterDataType::sint32:
        if (rounded < -2147483648.0 || rounded > 2147483647.0) return false;
        {
            const auto raw = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(rounded));
            destination[0] = static_cast<std::uint16_t>(raw >> 16U);
            destination[1] = static_cast<std::uint16_t>(raw);
            word_count = 2;
        }
        return true;
    case inverter::RegisterDataType::uint8:
        if (rounded < 0.0 || rounded > 255.0) return false;
        destination[0] = static_cast<std::uint16_t>(rounded);
        word_count = 1;
        return true;
    case inverter::RegisterDataType::uint16:
    case inverter::RegisterDataType::ascii_or_uint16:
    case inverter::RegisterDataType::unknown:
        if (rounded < 0.0 || rounded > 65535.0) return false;
        destination[0] = static_cast<std::uint16_t>(rounded);
        word_count = 1;
        return true;
    default:
        return false;
    }
}

void add_register_details(cJSON *root, const char *key,
                          inverter::RegisterSpace space, std::uint16_t first,
                          std::uint16_t count, const std::uint16_t *values)
{
    cJSON *entries = cJSON_AddArrayToObject(root, key);
    for (std::uint16_t index = 0; index < count; ++index) {
        cJSON *entry = cJSON_CreateObject();
        const std::uint16_t address = first + index;
        const auto *descriptor = inverter::find_register(space, address);
        cJSON_AddNumberToObject(entry, "address", address);
        cJSON_AddNumberToObject(entry, "raw", values[index]);
        if (descriptor != nullptr) {
            cJSON_AddStringToObject(entry, "name", descriptor->name);
            cJSON_AddStringToObject(entry, "unit", descriptor->unit);
            const bool has_low = index + 1 < count;
            const auto decoded = inverter::decode_numeric_value(
                *descriptor, values[index], has_low ? values[index + 1] : 0,
                has_low);
            if (decoded.valid) {
                cJSON_AddNumberToObject(entry, "value", decoded.engineering_value);
            }
        }
        cJSON_AddItemToArray(entries, entry);
    }
}

void add_named_value(cJSON *root, const char *key,
                     const inverter::RegisterDescriptor &descriptor,
                     const std::uint16_t *values, std::uint16_t count)
{
    if (descriptor.data_type == inverter::RegisterDataType::ascii) {
        char text_value[3]{};
        text_value[0] = static_cast<char>(values[0] >> 8U);
        text_value[1] = static_cast<char>(values[0] & 0xffU);
        cJSON_AddStringToObject(root, key, text_value);
        return;
    }
    const auto decoded = inverter::decode_numeric_value(
        descriptor, values[0], count > 1 ? values[1] : 0, count > 1);
    if (decoded.valid) cJSON_AddNumberToObject(root, key, decoded.engineering_value);
    else cJSON_AddNullToObject(root, key);
}

std::uint8_t member_from_topic(const char *topic, int length)
{
    const std::string value(topic, static_cast<std::size_t>(length));
    const auto command_position = value.rfind("/command");
    if (command_position == std::string::npos) return 0xff;
    const auto slash = value.rfind('/', command_position - 1);
    if (slash == std::string::npos) return 0xff;
    const std::string id = value.substr(slash + 1, command_position - slash - 1);
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(id.c_str(), &end, 10);
    return end != id.c_str() && *end == '\0' && parsed <= 255
               ? static_cast<std::uint8_t>(parsed)
               : 0xff;
}

} // namespace

MqttBridge::MqttBridge(const app::SystemConfig &config, RuntimeMessageSink &sink)
    : config_(config), sink_(sink)
{
}

esp_err_t MqttBridge::start()
{
    if (client_ != nullptr) return ESP_OK;
    ESP_LOGW(tag, "Starting MQTT connection to %s (authentication %s)",
             config_.mqtt_uri,
             config_.mqtt_username[0] != '\0' ? "enabled" : "disabled");
    publish_queue_ = xQueueCreate(32, sizeof(OutboundMessage));
    if (publish_queue_ == nullptr) return ESP_ERR_NO_MEM;
    esp_mqtt_client_config_t mqtt_config{};
    mqtt_config.broker.address.uri = config_.mqtt_uri;
    if (uses_tls(config_.mqtt_uri)) {
        mqtt_config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(tag, "TLS server verification enabled with the ESP certificate bundle");
    }
    struct ifreq station_interface{};
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station != nullptr &&
        esp_netif_get_netif_impl_name(station, station_interface.ifr_name) ==
            ESP_OK) {
        mqtt_config.network.if_name = &station_interface;
        ESP_LOGW(tag, "MQTT bound to Wi-Fi station interface %s",
                 station_interface.ifr_name);
    } else {
        ESP_LOGW(tag, "Could not bind MQTT explicitly to Wi-Fi station interface");
    }
    mqtt_config.credentials.username = config_.mqtt_username[0] ? config_.mqtt_username : nullptr;
    mqtt_config.credentials.authentication.password =
        config_.mqtt_password[0] ? config_.mqtt_password : nullptr;
    make_topic(availability_topic_, sizeof(availability_topic_),
               config_.profile.logical_member_id, "availability");
    mqtt_config.session.last_will.topic = availability_topic_;
    mqtt_config.session.last_will.msg = "offline";
    mqtt_config.session.last_will.qos = 1;
    mqtt_config.session.last_will.retain = 1;
    mqtt_config.buffer.size = 8192;
    mqtt_config.buffer.out_size = 8192;
    mqtt_config.outbox.limit = mqtt_outbox_limit_bytes;
    mqtt_config.network.reconnect_timeout_ms = app::mqtt_reconnect_interval_ms;
    mqtt_config.network.timeout_ms = app::mqtt_network_timeout_ms;
    mqtt_config.session.keepalive = 60;
    client_ = esp_mqtt_client_init(&mqtt_config);
    if (client_ == nullptr) {
        vQueueDelete(publish_queue_);
        publish_queue_ = nullptr;
        return ESP_FAIL;
    }
    esp_err_t result = esp_mqtt_client_register_event(
        client_, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), event_callback, this);
    if (result == ESP_OK) result = esp_mqtt_client_start(client_);
    if (result == ESP_OK &&
        xTaskCreate(publisher_task, "mqtt_publish", 6144, this, 4,
                    &publisher_task_) != pdPASS) result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) stop();
    return result;
}

void MqttBridge::stop()
{
    if (connected_ && client_ != nullptr) {
        esp_mqtt_client_publish(client_, availability_topic_, "offline", 0, 1, 1);
    }
    connected_ = false;
    if (publisher_task_ != nullptr) {
        vTaskDelete(publisher_task_);
        publisher_task_ = nullptr;
    }
    if (client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    if (publish_queue_ != nullptr) {
        vQueueDelete(publish_queue_);
        publish_queue_ = nullptr;
    }
}

void MqttBridge::event_callback(void *argument, esp_event_base_t,
                                std::int32_t, void *event_data)
{
    auto *self = static_cast<MqttBridge *>(argument);
    if (self != nullptr) self->handle_event(static_cast<esp_mqtt_event_handle_t>(event_data));
}

void MqttBridge::handle_event(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED: {
        connected_ = true;
        char topic[192]{};
        std::snprintf(topic, sizeof(topic), "%s/%s/inverter/+/command",
                      config_.mqtt_topic_prefix, config_.site_id);
        esp_mqtt_client_subscribe(client_, topic, 1);
        esp_mqtt_client_publish(client_, availability_topic_, "online", 0, 1, 1);
        ESP_LOGW(tag, "MQTT connected; subscribed to %s", topic);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ++reconnect_count_;
        connected_ = false;
        ESP_LOGW(tag, "MQTT disconnected from %s", config_.mqtt_uri);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle != nullptr) {
            ESP_LOGE(tag,
                     "MQTT error: type=%d esp_tls=0x%x tls_stack=0x%x socket_errno=%d (%s) broker=%s",
                     static_cast<int>(event->error_handle->error_type),
                     static_cast<unsigned>(event->error_handle->esp_tls_last_esp_err),
                     static_cast<unsigned>(event->error_handle->esp_tls_stack_err),
                     event->error_handle->esp_transport_sock_errno,
                     std::strerror(event->error_handle->esp_transport_sock_errno),
                     config_.mqtt_uri);
        }
        break;
    case MQTT_EVENT_DATA:
        if (event->retain || event->total_data_len > static_cast<int>(app::mqtt_command_max_bytes)) {
            ESP_LOGW(tag, "Rejected retained or oversized MQTT command");
            break;
        }
        if (event->current_data_offset == 0 && event->data_len == event->total_data_len) {
            handle_command(event->topic, event->topic_len, event->data, event->data_len);
        } else {
            ESP_LOGW(tag, "Rejected fragmented MQTT command");
        }
        break;
    default:
        break;
    }
}

void MqttBridge::handle_command(const char *topic, int topic_length,
                                const char *payload, int payload_length)
{
    if (topic == nullptr || payload == nullptr || payload_length <= 0) return;
    const std::uint8_t target = member_from_topic(topic, topic_length);
    if (target == 0xff || target >= config_.profile.expected_inverter_count) return;
    cJSON *root = cJSON_ParseWithLength(payload, static_cast<std::size_t>(payload_length));
    if (root == nullptr) return;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "command_id");
    const cJSON *operation = cJSON_GetObjectItemCaseSensitive(root, "operation");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    RoutedCommand command{};
    command.target_member_id = target;
    command.origin = RemoteCommandOrigin::mqtt;
    std::uint64_t command_id = 0;
    bool valid = cJSON_IsString(operation) && parse_command_id(id, command_id);
    if (valid && std::strcmp(operation->valuestring, "get") == 0) {
        command.kind = RemoteCommandKind::read_registers;
    } else if (valid && std::strcmp(operation->valuestring, "set") == 0) {
        command.kind = RemoteCommandKind::write_registers;
    } else if (valid) {
        command.kind = RemoteCommandKind::semantic;
        command.commissioning_interlock = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(root, "commissioning_interlock"));
        command.request.command_id = command_id;
        valid = cJSON_IsNumber(value) &&
                value->valuedouble == static_cast<double>(value->valueint) &&
                parse_operation(operation->valuestring, command.request.operation);
        if (valid) command.request.value = value->valueint;
    }
    if (valid && command.kind != RemoteCommandKind::semantic) {
        constexpr const char *forbidden_public_fields[] = {
            "space", "address", "count", "values", "expected_value",
            "expected_values", "first_register", "register_count", "scale",
        };
        for (const char *field : forbidden_public_fields) {
            if (cJSON_GetObjectItemCaseSensitive(root, field) != nullptr) {
                valid = false;
                break;
            }
        }
        auto &request = command.register_request;
        request.command_id = command_id;
        request.named_request = true;
        const cJSON *category = cJSON_GetObjectItemCaseSensitive(root, "category");
        const inverter::RegisterDescriptor *named_descriptor = nullptr;
        if (command.kind == RemoteCommandKind::write_registers) {
            request.space = inverter::RegisterSpace::holding;
        } else {
            valid = parse_named_category(category, request.space);
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *description = cJSON_GetObjectItemCaseSensitive(root, "description");
        const cJSON *domain_item = cJSON_GetObjectItemCaseSensitive(root, "domain");
        inverter::RegisterDomain domain{};
        const inverter::RegisterDomain *domain_filter = nullptr;
        if (domain_item != nullptr) {
            valid = valid && parse_register_domain(domain_item, domain);
            if (valid) domain_filter = &domain;
        }
        named_descriptor = valid && cJSON_IsString(name)
                               ? find_named_register(
                                     request.space, name->valuestring,
                                     cJSON_IsString(description)
                                         ? description->valuestring : nullptr,
                                     domain_filter)
                               : nullptr;
        valid = named_descriptor != nullptr &&
                !inverter::descriptor_is_low_word(*named_descriptor);
        if (valid) {
            request.first_register = named_descriptor->address;
            request.register_count = inverter::descriptor_starts_multiword_value(*named_descriptor)
                                         ? 2 : 1;
        }
        if (valid && command.kind == RemoteCommandKind::write_registers) {
            request.confirmed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "confirmed"));
            request.guarded_interlock = cJSON_IsTrue(
                cJSON_GetObjectItemCaseSensitive(root, "guarded_interlock"));
            request.commissioning_interlock = cJSON_IsTrue(
                cJSON_GetObjectItemCaseSensitive(root, "commissioning_interlock"));
            request.service_interlock = cJSON_IsTrue(
                cJSON_GetObjectItemCaseSensitive(root, "service_interlock"));
            valid = named_descriptor != nullptr &&
                    encode_named_value(*named_descriptor, value, request.values,
                                       request.register_count);
            request.expected_values_present = false;
        }
    }
    if (valid && remember_command_id(command_id)) sink_.on_command(command);
    else if (valid) ESP_LOGW(tag, "Rejected duplicate MQTT command ID %llu",
                            static_cast<unsigned long long>(command_id));
    else ESP_LOGW(tag, "Rejected malformed MQTT command");
    cJSON_Delete(root);
}

bool MqttBridge::remember_command_id(std::uint64_t command_id)
{
    if (std::find(recent_command_ids_.begin(), recent_command_ids_.end(),
                  command_id) != recent_command_ids_.end()) return false;
    recent_command_ids_[next_command_id_slot_] = command_id;
    next_command_id_slot_ = (next_command_id_slot_ + 1U) % recent_command_ids_.size();
    return true;
}

bool is_public_descriptor(const inverter::RegisterDescriptor *descriptor)
{
    return descriptor != nullptr && descriptor->name != nullptr &&
           descriptor->name[0] != '\0' &&
           std::strcmp(descriptor->name, "Reserved") != 0 &&
           !inverter::descriptor_is_low_word(*descriptor);
}

std::uint8_t pv_input_index(const inverter::RegisterDescriptor &descriptor)
{
    if (descriptor.space != inverter::RegisterSpace::input) return 0;
    if (descriptor.address >= 64 && descriptor.address <= 95) {
        return static_cast<std::uint8_t>((descriptor.address - 64) / 2 + 1);
    }
    if (descriptor.address >= 252 && descriptor.address <= 283) {
        return static_cast<std::uint8_t>((descriptor.address - 252) / 2 + 1);
    }
    return 0;
}

bool descriptor_is_enabled(const inverter::RegisterDescriptor &descriptor,
                           std::uint16_t enabled_features,
                           std::uint8_t connected_pv_inputs)
{
    if (descriptor.feature_flags != inverter::feature_none &&
        (enabled_features & descriptor.feature_flags) != descriptor.feature_flags) {
        return false;
    }
    if ((enabled_features & inverter::feature_three_phase) == 0 &&
        descriptor.applicable_models != nullptr &&
        std::strstr(descriptor.applicable_models, "Single-phase not supported") != nullptr) {
        return false;
    }
    if (descriptor.domain == inverter::RegisterDomain::pv && connected_pv_inputs == 0) {
        return false;
    }
    const std::uint8_t input = pv_input_index(descriptor);
    return input == 0 || input <= connected_pv_inputs;
}

const char *application_name(const inverter::RegisterDescriptor &descriptor)
{
    return descriptor.description != nullptr && descriptor.description[0] != '\0'
               ? descriptor.description
               : descriptor.name;
}

const char *engineering_unit(const inverter::RegisterDescriptor &descriptor)
{
    const char *unit = descriptor.unit == nullptr ? "" : descriptor.unit;
    if (std::strcmp(descriptor.name, "SOC") == 0 ||
        std::strcmp(descriptor.name, "MaxSOC") == 0 ||
        std::strcmp(descriptor.name, "MinSOC") == 0 ||
        std::strcmp(descriptor.name, "BMS_SOH") == 0) return "%";
    if (std::strcmp(descriptor.name, "Loadpercent") == 0 ||
        std::strcmp(descriptor.name, "RealOPPercent") == 0) return "%";
    if (std::strcmp(unit, "0.1V") == 0 || std::strcmp(unit, "0.01V") == 0 ||
        std::strcmp(unit, "0.001V") == 0 || std::strcmp(unit, "1V") == 0) return "V";
    if (std::strcmp(unit, "0.1A") == 0 || std::strcmp(unit, "0.01A") == 0 ||
        std::strcmp(unit, "1A") == 0) return "A";
    if (std::strcmp(unit, "0.1W") == 0 || std::strcmp(unit, "10W") == 0) return "kW";
    if (std::strcmp(unit, "0.1VA") == 0) return "kVA";
    if (std::strcmp(unit, "0.1var") == 0) return "kvar";
    if (std::strcmp(unit, "0.01Hz") == 0) return "Hz";
    if (std::strcmp(unit, "0.1C deg") == 0) return "C";
    if (std::strcmp(unit, "0.1Ah") == 0) return "Ah";
    if (std::strcmp(unit, "0.1kWh") == 0 || std::strcmp(unit, "0.1kwh") == 0) return "kWh";
    if (std::strcmp(unit, "1s") == 0) return "s";
    if (std::strcmp(unit, "0.5min") == 0 || std::strcmp(unit, "1min") == 0) return "min";
    if (std::strcmp(unit, "1kohm") == 0) return "kOhm";
    if (std::strcmp(unit, "1mV") == 0) return "mV";
    if (std::strcmp(unit, "1mA") == 0) return "mA";
    if (std::strcmp(unit, "20ms") == 0) return "ms";
    if (std::strcmp(unit, "1DAY") == 0) return "day";
    if (std::strcmp(unit, "1%Pn") == 0 || std::strcmp(unit, "1Pn%") == 0) return "%Pn";
    if (std::strcmp(unit, "0.1Pn%/min") == 0 ||
        std::strcmp(unit, "0.1Pn/min") == 0) return "%Pn/min";
    if (std::strcmp(unit, "1E-4") == 0 || std::strcmp(unit, "0.01") == 0) return "";
    return unit;
}

double published_value(const inverter::RegisterDescriptor &descriptor,
                       double engineering_value)
{
    const char *unit = descriptor.unit == nullptr ? "" : descriptor.unit;
    if (std::strcmp(unit, "0.1W") == 0 || std::strcmp(unit, "10W") == 0 ||
        std::strcmp(unit, "0.1VA") == 0 || std::strcmp(unit, "0.1var") == 0) {
        return engineering_value / 1000.0;
    }
    return engineering_value;
}

const char *inverter_status_text(std::uint16_t value)
{
    switch (value) {
    case 0: return "Waiting";
    case 1: return "On-grid";
    case 2: return "Off-grid";
    case 3: return "Fault";
    case 4: return "Firmware update";
    case 5: return "Bypass";
    case 6: return "Self-charging";
    default: return "Unknown";
    }
}

const char *priority_text(std::uint16_t value)
{
    switch (value) {
    case 0: return "Load first";
    case 1: return "Battery first";
    case 2: return "Grid first";
    default: return "Unknown";
    }
}

const char *source_priority_text(std::uint16_t value)
{
    switch (value) {
    case 0: return "SOL";
    case 1: return "UTI";
    case 2: return "SBU";
    case 10: return "Grid-connected output";
    default: return "Unknown";
    }
}

const char *live_section(const inverter::RegisterDescriptor &descriptor)
{
    if (descriptor.read_class == inverter::RegisterReadClass::critical_status) {
        return "status";
    }
    switch (descriptor.domain) {
    case inverter::RegisterDomain::pv: return "pv";
    case inverter::RegisterDomain::grid: return "grid";
    case inverter::RegisterDomain::eps: return "load";
    case inverter::RegisterDomain::load:
        return std::strstr(descriptor.name, "grid") != nullptr ||
               std::strstr(descriptor.name, "Grid") != nullptr
                   ? "grid" : "load";
    case inverter::RegisterDomain::battery: return "battery";
    case inverter::RegisterDomain::bms: return "bms";
    case inverter::RegisterDomain::generator: return "generator";
    case inverter::RegisterDomain::energy: return "energy";
    case inverter::RegisterDomain::parallel: return "parallel";
    case inverter::RegisterDomain::system:
    case inverter::RegisterDomain::inverter: return "inverter";
    case inverter::RegisterDomain::power:
        if (std::strstr(descriptor.name, "Pcharge") != nullptr ||
            std::strstr(descriptor.name, "Pdischarge") != nullptr ||
            std::strstr(descriptor.name, "charge Power") != nullptr) {
            return "battery";
        }
        if (std::strstr(descriptor.name, "grid") != nullptr ||
            std::strstr(descriptor.name, "Grid") != nullptr) return "grid";
        return "load";
    default: return "other";
    }
}

const char *telemetry_topology(std::uint16_t features, std::uint8_t phase)
{
    const bool parallel = (features & inverter::feature_parallel) != 0;
    const bool native_three_phase = (features & inverter::feature_three_phase) != 0;
    const auto assignment = static_cast<app::PhaseAssignment>(phase);
    if (!parallel) {
        return native_three_phase ? "standalone_native_three_phase"
                                  : "standalone_single_phase";
    }
    if (native_three_phase) return "parallel_native_three_phase";
    if (assignment == app::PhaseAssignment::phase_1 ||
        assignment == app::PhaseAssignment::phase_2 ||
        assignment == app::PhaseAssignment::phase_3) {
        return "parallel_three_phase_groups";
    }
    return "parallel_single_phase";
}

// Raw numeric JSON preserves the two decimal places without making a string.
cJSON *add_decimal_value(cJSON *object, const char *key, double value)
{
    if (!std::isfinite(value)) return cJSON_AddNullToObject(object, key);
    char number[384]{};
    if (std::fabs(value) < 0.005) value = 0.0;
    const int length = std::snprintf(number, sizeof(number), "%.2f", value);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(number)) {
        return cJSON_AddNullToObject(object, key);
    }
    return cJSON_AddRawToObject(object, key, number);
}

void add_application_value(cJSON *object, const char *key,
                           const inverter::RegisterDescriptor &descriptor,
                           const std::uint16_t *values, std::uint16_t count)
{
    if (descriptor.data_type == inverter::RegisterDataType::ascii ||
        descriptor.data_type == inverter::RegisterDataType::ascii_or_uint16) {
        char text[3]{};
        if (inverter::decode_ascii_word(values[0], text)) {
            cJSON_AddStringToObject(object, key, text);
            return;
        }
    }
    const auto decoded = inverter::decode_numeric_value(
        descriptor, values[0], count > 1 ? values[1] : 0, count > 1);
    if (decoded.valid) {
        const double value = published_value(descriptor, decoded.engineering_value);
        if (descriptor.scale != 1.0 || value != decoded.engineering_value) {
            add_decimal_value(object, key, value);
        } else {
            cJSON_AddNumberToObject(object, key, value);
        }
    }
    else if (descriptor.data_type == inverter::RegisterDataType::ascii_or_uint16) {
        cJSON_AddNumberToObject(object, key, values[0] * descriptor.scale);
    }
    else cJSON_AddNullToObject(object, key);
}

cJSON *add_calculated_section_entry(cJSON *sections, const char *section_name,
                                    const char *name, const char *description,
                                    const char *domain, const char *unit,
                                    double value, const char *source)
{
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(sections, section_name);
    if (entries == nullptr) return nullptr;
    cJSON *entry = cJSON_CreateObject();
    if (entry == nullptr) return nullptr;
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddStringToObject(entry, "register_name", "");
    cJSON_AddStringToObject(entry, "description", description);
    cJSON_AddStringToObject(entry, "domain", domain);
    cJSON_AddStringToObject(entry, "unit", unit);
    add_decimal_value(entry, "value", value);
    cJSON_AddStringToObject(entry, "source", source);
    cJSON_AddItemToArray(entries, entry);
    return entry;
}

void make_topic_component(const char *input, char *output, std::size_t capacity)
{
    if (output == nullptr || capacity == 0) return;
    std::size_t used = 0;
    for (const unsigned char *source =
             reinterpret_cast<const unsigned char *>(input == nullptr ? "" : input);
         *source != 0 && used + 1 < capacity; ++source) {
        const bool alpha_numeric = (*source >= 'a' && *source <= 'z') ||
                                   (*source >= 'A' && *source <= 'Z') ||
                                   (*source >= '0' && *source <= '9');
        if (alpha_numeric) output[used++] = static_cast<char>(*source);
        else if (used != 0 && output[used - 1] != '_') output[used++] = '_';
    }
    while (used != 0 && output[used - 1] == '_') --used;
    if (used == 0 && capacity > 1) {
        output[used++] = 'u';
        if (used + 1 < capacity) output[used++] = 'n';
        if (used + 1 < capacity) output[used++] = 'k';
    }
    output[used] = '\0';
}

bool contains_case_insensitive(const char *text, const char *needle)
{
    if (text == nullptr || needle == nullptr) return false;
    std::string value(text);
    std::string match(needle);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    std::transform(match.begin(), match.end(), match.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value.find(match) != std::string::npos;
}

bool is_alert_descriptor(const inverter::RegisterDescriptor &descriptor)
{
    if (descriptor.read_class != inverter::RegisterReadClass::critical_status) return false;
    return descriptor.address != 1 && descriptor.address != 39;
}

const char *alert_severity(const inverter::RegisterDescriptor &descriptor)
{
    if (contains_case_insensitive(descriptor.name, "warn") ||
        contains_case_insensitive(descriptor.name, "alarm") ||
        contains_case_insensitive(descriptor.description, "warning") ||
        contains_case_insensitive(descriptor.description, "alarm")) return "warning";
    if (contains_case_insensitive(descriptor.name, "error") ||
        contains_case_insensitive(descriptor.name, "fault") ||
        contains_case_insensitive(descriptor.description, "error") ||
        contains_case_insensitive(descriptor.description, "fault")) return "fault";
    if (contains_case_insensitive(descriptor.name, "status")) return "status";
    if (descriptor.address == 0) return "status";
    if (descriptor.address == 23) return "event";
    return "fault";
}

void MqttBridge::publish_telemetry(const TelemetryMessage &message)
{
    last_telemetry_ms_ = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    if (publish_queue_ == nullptr) return;
    OutboundMessage outbound{};
    outbound.type = OutboundType::telemetry;
    outbound.telemetry = message;
    if (xQueueSend(publish_queue_, &outbound, 0) != pdTRUE) {
        app::rgb_debug_publish_error();
        ESP_LOGW(tag, "MQTT publish queue full; telemetry dropped");
        ++telemetry_queue_drops;
    }
}

void MqttBridge::publish_telemetry_now(const TelemetryMessage &message)
{
    if (message.register_space == 0) publish_snapshot(message);
    else {
        publish_live(message);
        publish_alert_changes(message);
    }
}

void MqttBridge::publish_live(const TelemetryMessage &message)
{
    if (message.source_member_id >= maximum_members) return;
    LiveCache &cache = live_cache_[message.source_member_id];
    const auto received_now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    if (message.timestamp_ms < cache.timestamp_ms) {
        // The remote ESP restarted: old high/low words and settings cannot
        // be combined with the new session's readings.
        cache.seen.fill(false);
        cache.last_power.fill({});
        cache.source_priority_seen = false;
        cache.rated_power_seen = false;
        cache.last_publish_ms = 0;
        cache.last_core_status_ms = 0;
        alert_seen_[message.source_member_id].fill(false);
    }
    if (!cache.received_ms || !cache.sampled_ms) {
        cache.received_ms.reset(new (std::nothrow) std::uint32_t[input_point_count]{});
        cache.sampled_ms.reset(new (std::nothrow) std::uint32_t[input_point_count]{});
        if (!cache.received_ms || !cache.sampled_ms) {
            app::rgb_debug_publish_error();
            ESP_LOGW(tag, "No memory for live freshness tracking");
            return;
        }
    }
    const std::uint16_t count = std::min<std::uint16_t>(
        message.register_count,
        static_cast<std::uint16_t>(message.registers.size()));
    for (std::uint16_t index = 0; index < count; ++index) {
        const std::uint32_t address =
            static_cast<std::uint32_t>(message.first_register) + index;
        if (address >= input_point_count) continue;
        cache.values[address] = message.registers[index];
        cache.seen[address] = true;
        cache.received_ms[address] = received_now;
        cache.sampled_ms[address] = static_cast<std::uint32_t>(message.timestamp_ms);
    }
    // Keep last-known readings for display; only fresh readings feed calculations.
    auto fresh = cache.seen;
    for (std::uint16_t address = 0; address < input_point_count; ++address) {
        if (!fresh[address]) continue;
        const auto *descriptor = inverter::find_register(inverter::RegisterSpace::input, address);
        const auto limit = descriptor && descriptor->read_class == inverter::RegisterReadClass::slow_counter
            ? app::slow_value_max_age_ms : app::live_value_max_age_ms;
        fresh[address] = received_now - cache.received_ms[address] <= limit;
    }
    cache.enabled_features = message.enabled_features;
    cache.connected_pv_inputs = message.connected_pv_inputs;
    cache.phase_assignment = message.phase_assignment;
    cache.sequence = message.sequence;
    cache.timestamp_ms = message.timestamp_ms;

    if (message.first_register == 0 && count != 0) {
        cache.last_core_status_ms = message.timestamp_ms;
    }
    // Any successful input read may refresh live. A failed status read must
    // not suppress otherwise valid telemetry; report its age explicitly.
    const bool live_publish_tick =
        count != 0 && (cache.last_publish_ms == 0 ||
        cache.timestamp_ms - cache.last_publish_ms >= app::fast_poll_interval_ms);
    if (!live_publish_tick) return;
    // Limit failed allocation attempts too; avoid rebuilding a large tree on
    // every incoming Modbus block while memory is constrained.
    cache.last_publish_ms = cache.timestamp_ms;

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        app::rgb_debug_publish_error();
        ESP_LOGW(tag, "Could not allocate live JSON object");
        return;
    }
    cJSON_AddBoolToObject(root, "core_status_fresh",
        cache.last_core_status_ms != 0 &&
        cache.timestamp_ms - cache.last_core_status_ms <= app::inverter_state_freshness_ms);
    if (cache.last_core_status_ms != 0) {
        cJSON_AddNumberToObject(root, "core_status_age_ms",
                               cache.timestamp_ms - cache.last_core_status_ms);
    } else cJSON_AddNullToObject(root, "core_status_age_ms");
    cJSON_AddNumberToObject(root, "member_id", message.source_member_id);
    cJSON_AddNumberToObject(root, "sequence", cache.sequence);
    cJSON_AddNumberToObject(root, "uptime_ms", static_cast<double>(cache.timestamp_ms));
    cJSON_AddNumberToObject(root, "connected_pv_inputs",
                            cache.connected_pv_inputs);
    cJSON_AddStringToObject(root, "topology",
                            telemetry_topology(cache.enabled_features,
                                               cache.phase_assignment));
    cJSON_AddBoolToObject(root, "battery_installed",
                          (cache.enabled_features & inverter::feature_battery) != 0);
    cJSON_AddBoolToObject(root, "bms_configured",
                          (cache.enabled_features & inverter::feature_bms) != 0);
    cJSON_AddNullToObject(root, "bms_connected"); // No verified communication-state mapping yet.
    cJSON_AddStringToObject(root, "installation_flags_source", "configuration");
    cJSON_AddBoolToObject(root, "generator_installed",
                          (cache.enabled_features & inverter::feature_generator) != 0);
    cJSON_AddStringToObject(
        root, "phase",
        app::phase_assignment_name(
            static_cast<app::PhaseAssignment>(cache.phase_assignment)));
    cJSON *operating = cJSON_AddObjectToObject(root, "operating_status");
    if (fresh[0]) {
        const std::uint16_t raw_status = cache.values[0];
        const bool running = raw_status == 1 || raw_status == 2 ||
                             raw_status == 5 || raw_status == 6 ||
                             raw_status == 7;
        cJSON_AddNumberToObject(operating, "value", raw_status);
        cJSON_AddBoolToObject(operating, "running", running);
        cJSON_AddStringToObject(
            operating, "running_status",
            inverter_status_text(raw_status));
    }
    if (cache.source_priority_seen) {
        cJSON_AddNumberToObject(operating, "operating_mode_value",
                                cache.source_priority);
        cJSON_AddStringToObject(operating, "operating_mode",
                                source_priority_text(cache.source_priority));
    }
    if ((cache.enabled_features & inverter::feature_battery) != 0 &&
        fresh[125]) {
        cJSON_AddStringToObject(operating, "energy_priority",
                                priority_text(cache.values[125]));
    }

    const auto sample_now = static_cast<std::uint32_t>(message.timestamp_ms);
    auto coherent = [&](std::initializer_list<std::uint16_t> addresses) {
        std::uint32_t youngest = UINT32_MAX, oldest = 0;
        for (auto address : addresses) {
            if (address >= input_point_count || !fresh[address]) return false;
            const auto age = sample_now - cache.sampled_ms[address];
            if (age > app::power_calculation_max_age_ms ||
                received_now - cache.received_ms[address] > app::power_calculation_max_age_ms) return false;
            youngest = std::min(youngest, age);
            oldest = std::max(oldest, age);
        }
        return addresses.size() != 0 && oldest - youngest <= app::power_calculation_max_skew_ms;
    };
    auto decoded_published_value = [&](std::uint16_t address,
                                            double &value) {
        if (!coherent({address})) return false;
        const auto *descriptor = inverter::find_register(
            inverter::RegisterSpace::input, address);
        if (!is_public_descriptor(descriptor)) return false;
        const bool next_seen = address + 1 < input_point_count && coherent({address, static_cast<std::uint16_t>(address + 1)}) &&
                               fresh[address + 1] &&
                               cache.received_ms[address] == cache.received_ms[address + 1];
        const auto decoded = inverter::decode_numeric_value(
            *descriptor, cache.values[address],
            next_seen ? cache.values[address + 1] : 0, next_seen);
        if (!decoded.valid) return false;
        value = published_value(*descriptor, decoded.engineering_value);
        return true;
    };

    auto signed_input_value = [&cache, &fresh](std::uint16_t address, double scale,
                                       double &value) {
        if (address >= input_point_count || !fresh[address]) return false;
        value = static_cast<std::int16_t>(cache.values[address]) * scale;
        return true;
    };

    const bool three_phase =
        (cache.enabled_features & inverter::feature_three_phase) != 0;
    auto sum_phase_power = [&](std::uint16_t first, std::uint16_t stride,
                               double &total) {
        total = 0.0;
        for (unsigned phase = 0; phase < (three_phase ? 3U : 1U); ++phase) {
            double value = 0.0;
            if (!decoded_published_value(first + phase * stride, value)) return false;
            total += value;
        }
        return !three_phase || coherent({first, static_cast<std::uint16_t>(first + stride),
                                         static_cast<std::uint16_t>(first + 2 * stride)});
    };
    const bool phase_r_available = coherent({55, 56});
    const bool phase_s_available = coherent({55, 56, 57, 58});
    const bool phase_t_available = coherent({55, 56, 59, 60});
    bool output_apparent_power_available = false;
    std::uint8_t output_phase_count = 0;
    double output_apparent_power_va = 0.0;
    if (phase_r_available &&
        (!three_phase || (phase_s_available && phase_t_available))) {
        output_apparent_power_va =
            (cache.values[55] * 0.1) * (cache.values[56] * 0.1);
        output_phase_count = 1;
        if (three_phase) {
            output_apparent_power_va +=
                (cache.values[57] * 0.1) * (cache.values[58] * 0.1);
            output_apparent_power_va +=
                (cache.values[59] * 0.1) * (cache.values[60] * 0.1);
            output_phase_count = 3;
        }
        output_apparent_power_available = true;
    }
    double output_register_kva = 0.0;
    const bool output_apparent_from_register = sum_phase_power(332, 4, output_register_kva);
    if (output_apparent_from_register) {
        output_apparent_power_va = output_register_kva * 1000.0;
        output_apparent_power_available = true;
        output_phase_count = three_phase ? 3 : 1;
    }
    double output_active_kw = 0.0;
    const bool output_active_available = sum_phase_power(334, 4, output_active_kw);

    bool pv_power_available = false;
    bool pv_power_calculated = false;
    double pv_power_kw = 0.0;
    if (!decoded_published_value(250, pv_power_kw)) {
        const std::uint8_t pv_count = cache.connected_pv_inputs;
        bool all_pv_available = pv_count > 0 && pv_count <= 16;
        std::uint32_t pv_youngest = UINT32_MAX, pv_oldest = 0;
        for (std::uint8_t pv = 0; pv < pv_count && pv < 16; ++pv) {
            const std::uint16_t voltage_address = 64 + pv * 2;
            const std::uint16_t current_address = voltage_address + 1;
            if (!coherent({64, voltage_address, current_address})) {
                all_pv_available = false;
                continue;
            }
            pv_power_kw += (cache.values[voltage_address] * 0.1) *
                           (cache.values[current_address] * 0.1) / 1000.0;
            for (auto address : {voltage_address, current_address}) {
                const auto age = sample_now - cache.sampled_ms[address];
                pv_youngest = std::min(pv_youngest, age);
                pv_oldest = std::max(pv_oldest, age);
            }
            pv_power_available = true;
            pv_power_calculated = true;
        }
        pv_power_available = pv_power_available && all_pv_available &&
            pv_oldest - pv_youngest <= app::power_calculation_max_skew_ms;
    } else {
        pv_power_available = true;
    }

    bool grid_power_available = false;
    double grid_power_kw = 0.0;
    double grid_apparent_kva = 0.0;
    std::uint8_t grid_phase_count = 0;
    const std::uint16_t grid_voltage_addresses[] = {42, 44, 46};
    const std::uint16_t grid_current_addresses[] = {43, 45, 47};
    const std::uint8_t wanted_grid_phases = three_phase ? 3 : 1;
    for (std::uint8_t phase = 0; phase < wanted_grid_phases; ++phase) {
        const std::uint16_t voltage_address = grid_voltage_addresses[phase];
        const std::uint16_t current_address = grid_current_addresses[phase];
        if (!coherent({42, voltage_address, current_address})) {
            continue;
        }
        double current_a = 0.0;
        signed_input_value(current_address, 0.1, current_a);
        grid_apparent_kva += (cache.values[voltage_address] * 0.1) *
                             std::fabs(current_a) / 1000.0;
        ++grid_phase_count;
        grid_power_available = true;
    }
    const bool grid_apparent_available = grid_phase_count == wanted_grid_phases &&
        (!three_phase || coherent({42,43,44,45,46,47}));
    // User-approved provisional interpretation: R320 is "AC power to user".
    // Decode the documented unsigned high/low pair; do not infer import sign.
    grid_power_available = decoded_published_value(320, grid_power_kw);

    bool inverter_power_available = false;
    double inverter_power_kw = 0.0;
    const bool inverter_register_power_available =
        decoded_published_value(286, inverter_power_kw);
    if (inverter_register_power_available) {
        inverter_power_available = true;
    } else {
        inverter_power_available = sum_phase_power(292, 6, inverter_power_kw);
    }
    double inverter_apparent_kva = 0.0;
    bool inverter_apparent_available = true;
    for (unsigned phase = 0; phase < (three_phase ? 3U : 1U); ++phase) {
        if (!coherent({2, static_cast<std::uint16_t>(2 + phase),
                       static_cast<std::uint16_t>(5 + phase)})) {
            inverter_apparent_available = false;
            break;
        }
        inverter_apparent_kva += (cache.values[2 + phase] * 0.1) *
            std::fabs(static_cast<std::int16_t>(cache.values[5 + phase]) * 0.1) / 1000.0;
    }

    bool battery_power_available = false;
    if (three_phase && !coherent({2,3,4,5,6,7})) inverter_apparent_available = false;
    bool battery_power_from_registers = false;
    double battery_power_kw = 0.0;
    double discharge_power_kw = 0.0;
    double charge_power_kw = 0.0;
    const bool discharge_power_available =
        decoded_published_value(349, discharge_power_kw);
    const bool charge_power_available =
        decoded_published_value(351, charge_power_kw);
    if (discharge_power_available && charge_power_available && coherent({349, 351})) {
        battery_power_kw = discharge_power_kw - charge_power_kw;
        battery_power_available = true;
        battery_power_from_registers = true;
    } else if ((cache.enabled_features & inverter::feature_battery) != 0 &&
               (fresh[127] || fresh[129]) && fresh[141] &&
               coherent({static_cast<std::uint16_t>(fresh[127] ? 127 : 129), 141}) &&
               !cache.seen[349] && !cache.seen[351]) {
        const std::uint16_t voltage_raw =
            fresh[127] ? cache.values[127] : cache.values[129];
        double battery_current_a = 0.0;
        signed_input_value(141, 0.01, battery_current_a);
        battery_power_kw = (voltage_raw * 0.1) * battery_current_a / 1000.0;
        battery_power_available = true;
    }

    bool load_percentage_available = false;
    bool load_percentage_direct = false;
    double load_percentage = 0.0;
    // This gateway's nameplate override must not be applied to other members.
    const bool local_member = message.source_member_id == config_.profile.logical_member_id;
    const double rated_power_w = local_member ? (config_.output_rated_power_kw > 0.0F ? config_.output_rated_power_kw : 12.0F) * 1000.0 : 0.0;
    const double rated_apparent_kva = local_member && rated_power_w == 0.0
        ? app::output_rated_apparent_kva : 0.0;
    const char *load_percentage_basis = "register";
    if (three_phase && coherent({344})) {
        load_percentage_available = true;
        load_percentage_direct = true;
        load_percentage = cache.values[344] * 0.01;
    } else if (rated_apparent_kva > 0.0 && output_apparent_power_available) {
        load_percentage = output_apparent_power_va * 0.1 / rated_apparent_kva;
        load_percentage_basis = "apparent_power";
        load_percentage_available = true;
    } else if (rated_power_w > 0.0 && output_active_available) {
        load_percentage = output_active_kw * 100000.0 / rated_power_w;
        load_percentage_basis = "active_power";
        load_percentage_available = true;
    }

    // Do not silently switch a previously reported measurement point after expiry.
    if (cache.seen[250] && !fresh[250]) pv_power_available = false;
    if (cache.seen[286] && !fresh[286]) inverter_power_available = false;
    if (cache.seen[332] && !fresh[332]) output_apparent_power_available = false;
    if ((cache.seen[349] && !fresh[349]) || (cache.seen[351] && !fresh[351]))
        battery_power_available = false;
    if (!load_percentage_direct &&
        std::strcmp(load_percentage_basis, "apparent_power") == 0 && !output_apparent_power_available)
        load_percentage_available = false;

    cJSON *power_summary = cJSON_AddObjectToObject(root, "power_summary");
    if (power_summary != nullptr) {
        add_decimal_value(power_summary, "rated_power_kw", rated_power_w / 1000.0);
        cJSON_AddStringToObject(power_summary, "rating_source", !local_member ? "unavailable_remote" : config_.output_rated_power_kw > 0.0F ? "saved_setup" : "default_12_kw");
        cJSON_AddNumberToObject(power_summary, "freshness_limit_ms", app::live_value_max_age_ms);
        cJSON_AddStringToObject(power_summary, "sampling", "sequential_cached_readings");
        if (!battery_power_available) cJSON_AddNullToObject(power_summary, "battery_kw");
        if (!pv_power_available) cJSON_AddNullToObject(power_summary, "pv_kw");
        if (!load_percentage_available) cJSON_AddNullToObject(power_summary, "load_percentage");
        cJSON_AddStringToObject(power_summary, "load_percentage_basis",
            load_percentage_available ? load_percentage_basis : "unavailable");
        // Stable keys distinguish an unavailable active reading from real zero.
        if (!grid_power_available) cJSON_AddNullToObject(power_summary, "grid_kw");
        if (!inverter_power_available) cJSON_AddNullToObject(power_summary, "inverter_kw");
        if (grid_apparent_available) add_decimal_value(power_summary, "grid_apparent_kva", grid_apparent_kva);
        else cJSON_AddNullToObject(power_summary, "grid_apparent_kva");
        if (inverter_apparent_available) add_decimal_value(power_summary, "inverter_apparent_kva", inverter_apparent_kva);
        else cJSON_AddNullToObject(power_summary, "inverter_apparent_kva");
        if (output_apparent_power_available) add_decimal_value(power_summary, "load_apparent_kva", output_apparent_power_va / 1000.0);
        else cJSON_AddNullToObject(power_summary, "load_apparent_kva");
        if (output_active_available) add_decimal_value(power_summary, "load_kw", output_active_kw);
        else cJSON_AddNullToObject(power_summary, "load_kw");
        if (pv_power_available) {
            add_decimal_value(power_summary, "pv_kw", pv_power_kw);
            cJSON_AddStringToObject(
                power_summary, "pv_source",
                pv_power_calculated ? "calculated_from_voltage_current"
                                    : "register");
        }
        if (grid_power_available) {
            add_decimal_value(power_summary, "grid_kw", grid_power_kw);
            cJSON_AddNumberToObject(power_summary, "grid_phase_count",
                                    grid_phase_count);
        }
        if (inverter_power_available) {
            add_decimal_value(power_summary, "inverter_kw", inverter_power_kw);
            cJSON_AddStringToObject(power_summary, "inverter_source",
                                    inverter_register_power_available
                                        ? "ac_active_total_register"
                                        : "ac_active_phase_registers");
        }


        if (load_percentage_available) {
            add_decimal_value(power_summary, "load_percentage",
                                    load_percentage);
            cJSON_AddStringToObject(power_summary, "load_percentage_source",
                                    load_percentage_direct ? "register"
                                                           : "calculated");
        }
        if (battery_power_available) {
            add_decimal_value(power_summary, "battery_kw",
                                    battery_power_kw);
            cJSON_AddStringToObject(power_summary, "battery_direction",
                                    "positive=discharging, negative=charging");
            cJSON_AddStringToObject(power_summary, "battery_source",
                                    battery_power_from_registers
                                        ? "charge_discharge_registers"
                                        : "calculated_from_voltage_current");
        }
    }

    // Retain display values separately from the raw inputs used by calculations.
    // A held value must never feed a new calculation or appear fresh.
    constexpr const char *power_keys[] = {"pv_kw", "grid_kw", "inverter_kw", "load_kw",
        "grid_apparent_kva", "inverter_apparent_kva", "load_apparent_kva",
        "battery_kw", "load_percentage"};
    constexpr const char *power_groups[] = {"pv", "grid", "inverter", "load",
        "grid", "inverter", "load", "battery", "load"};
    std::array<bool, 9> held_power{};
    cJSON *quality = cJSON_AddObjectToObject(root, "power_quality");
    if (power_summary != nullptr && quality != nullptr) {
        for (std::size_t index = 0; index < cache.last_power.size(); ++index) {
            auto &saved = cache.last_power[index];
            const cJSON *current = cJSON_GetObjectItemCaseSensitive(power_summary, power_keys[index]);
            // Decimal measurements use cJSON_Raw to preserve numeric .2f output.
            const bool valid = cJSON_IsNumber(current) || cJSON_IsRaw(current);
            if (valid) {
                saved.value = cJSON_IsNumber(current) ? current->valuedouble : std::strtod(current->valuestring, nullptr);
                saved.valid = true;
                saved.validated_ms = received_now;
            } else if (saved.valid) {
                cJSON_DeleteItemFromObjectCaseSensitive(power_summary, power_keys[index]);
                add_decimal_value(power_summary, power_keys[index], saved.value);
                held_power[index] = true;
            }
            cJSON *item = cJSON_AddObjectToObject(quality, power_keys[index]);
            cJSON_AddBoolToObject(item, "available", saved.valid);
            cJSON_AddBoolToObject(item, "stale", !valid);
            cJSON_AddStringToObject(item, "state", valid ? "valid" : saved.valid ? "last_known" : "unavailable");
            if (saved.valid) cJSON_AddNumberToObject(item, "since_valid_ms", received_now - saved.validated_ms);
            else cJSON_AddNullToObject(item, "since_valid_ms");
        }
    }
    cJSON *sections = cJSON_AddObjectToObject(root, "sections");
    constexpr const char *section_names[] = {
        "status", "inverter", "pv", "grid", "load",
        "battery", "bms", "generator", "parallel", "other",
    };
    for (const char *section_name : section_names) {
        cJSON_AddArrayToObject(sections, section_name);
    }
    if (pv_power_available && pv_power_calculated) {
        add_calculated_section_entry(
            sections, "pv", "PV total input power",
            "Calculated from PV voltage/current", "PV", "kW", pv_power_kw,
            "calculated");
    }
    if (grid_power_available) {
        cJSON *entry = add_calculated_section_entry(
            sections, "grid", "Grid active power",
            "R320 total AC power to user; grid-intake interpretation UNVERIFIED",
            "GRID", "kW", grid_power_kw, "unverified_R320_register");
        if (entry != nullptr) {
            cJSON_AddNumberToObject(entry, "phase_count", grid_phase_count);
            cJSON_AddNullToObject(entry, "power_factor"); // Not used to decode R320.
        }
    }
    if (grid_apparent_available) {
        add_calculated_section_entry(sections, "grid", "Grid apparent power",
            "Sum of grid phase voltage times current magnitude", "GRID", "kVA",
            grid_apparent_kva, "calculated");
    }
    if (inverter_power_available) {
        add_calculated_section_entry(sections, "inverter", "Inverter active power",
            "AC active power total or sum of phase readings", "INVERTER", "kW",
            inverter_power_kw, "register");
    }
    if (inverter_apparent_available) {
        add_calculated_section_entry(
            sections, "inverter", "Inverter AC apparent power",
            "Calculated from inverter voltage/current", "INVERTER", "kVA",
            inverter_apparent_kva, "calculated");
    }
    if (output_active_available) {
        add_calculated_section_entry(sections, "load", "Load active power",
            "Sum of EPS phase active power", "EPS", "kW", output_active_kw, "register");
    }

    if (output_apparent_power_available) {
        cJSON *entry = add_calculated_section_entry(
            sections, "load", "Load apparent power",
            "EPS apparent power: phase registers or voltage/current", "EPS", "kVA",
            output_apparent_power_va / 1000.0, output_apparent_from_register ? "register" : "calculated");
        if (entry != nullptr) {
            cJSON_AddNumberToObject(entry, "phase_count", output_phase_count);
        }
    }

    if (load_percentage_available) {
        cJSON *entry = add_calculated_section_entry(
            sections, "load", "Load percentage",
            load_percentage_direct ? "Reported EPS load percentage" : "Calculated from EPS active output and selected rating",
            "EPS", "%", load_percentage, load_percentage_direct ? "register" : "calculated");
        if (entry != nullptr) {
            cJSON_AddStringToObject(entry, "basis", load_percentage_basis);
            add_decimal_value(entry, "rated_apparent_kva", rated_apparent_kva);
            add_decimal_value(entry, "apparent_power_kva",
                                    output_apparent_power_va / 1000.0);
            add_decimal_value(entry, "rated_power_kw",
                                    rated_power_w / 1000.0);
        }
    }
    if (battery_power_available) {
        cJSON *entry = add_calculated_section_entry(
            sections, "battery", "Battery power",
            "Signed battery power: positive discharging, negative charging",
            "BATTERY", "kW", battery_power_kw,
            battery_power_from_registers ? "charge_discharge_registers"
                                         : "calculated");
        if (entry != nullptr) {
            cJSON_AddStringToObject(entry, "direction",
                                    "positive=discharging, negative=charging");
        }
    }
    for (std::uint16_t address = 0; address < input_point_count; ++address) {
        if (!cache.seen[address]) continue;
        if (address == 344 && !three_phase) continue;
        // The live load group is EPS-terminal load, not every diagnostic power point.
        // Named register inquiries remain available for these raw measurements.
        if (address >= 284 && address <= 355) continue;
        const auto *descriptor = inverter::find_register(
            inverter::RegisterSpace::input, address);
        if (!is_public_descriptor(descriptor) ||
            !descriptor_is_enabled(*descriptor, cache.enabled_features,
                                   cache.connected_pv_inputs)) continue;
        const char *section_name = live_section(*descriptor);
        cJSON *entries = cJSON_GetObjectItemCaseSensitive(sections, section_name);
        if (entries == nullptr) continue;
        cJSON *entry = cJSON_CreateObject();
        if (entry == nullptr ||
            !add_catalog_string(entry, "name", application_name(*descriptor)) ||
            !add_catalog_string(entry, "register_name", descriptor->name) ||
            !add_catalog_string(entry, "description", descriptor->description) ||
            !add_catalog_string(entry, "domain", inverter::register_domain_name(descriptor->domain)) ||
            !add_catalog_string(entry, "unit", engineering_unit(*descriptor))) {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            app::rgb_debug_publish_error();
            ESP_LOGW(tag, "Live JSON metadata allocation failed");
            return;
        }
        const bool next_seen = address + 1 < input_point_count &&
                               cache.seen[address + 1] &&
                               cache.received_ms[address] == cache.received_ms[address + 1];
        cJSON_AddNumberToObject(entry, "age_ms", received_now - cache.received_ms[address]);
        cJSON_AddBoolToObject(entry, "stale", !fresh[address] || (next_seen && !fresh[address + 1]));
        add_application_value(entry, "value", *descriptor,
                              cache.values.data() + address,
                              next_seen ? 2 : 1);
        cJSON_AddItemToArray(entries, entry);
    }
    for (std::size_t index = 0; index < held_power.size(); ++index) {
        if (!held_power[index]) continue;
        cJSON *entry = add_calculated_section_entry(sections, power_groups[index], power_keys[index],
            "Last valid result; not a new measurement", power_groups[index],
            index == 8 ? "%" : index >= 4 && index <= 6 ? "kVA" : "kW",
            cache.last_power[index].value, "last_known");
        if (entry != nullptr) {
            cJSON_AddBoolToObject(entry, "stale", true);
            cJSON_AddNumberToObject(entry, "since_valid_ms", received_now - cache.last_power[index].validated_ms);
        }
    }
    const std::size_t capacity = json_capacity(root) + 5;
    const auto free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const auto largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    char *json = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (json == nullptr) json = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_8BIT));
    const bool serialized = json != nullptr &&
        cJSON_PrintPreallocated(root, json, static_cast<int>(capacity), false);
    // Release all JSON nodes before enqueue copies the serialized payload.
    cJSON_Delete(root);
    char topic[192]{};
    make_topic(topic, sizeof(topic), message.source_member_id, "live");
    if (serialized) {
        const int message_id =
            enqueue_payload(client_, topic, json, 0, 0);
        if (message_id < 0) {
            ESP_LOGW(tag, "Could not publish live data to %s", topic);
        } else {
            cache.last_publish_ms = cache.timestamp_ms;
            last_live_enqueued_ms_ = received_now;
        }
    } else {
        app::rgb_debug_publish_error();
        ESP_LOGW(tag, "Live JSON %s failed: requested=%u free_before=%u largest_before=%u free_after=%u largest_after=%u outbox=%d",
                 json == nullptr ? "allocation" : "serialization",
                 static_cast<unsigned>(capacity),
                 static_cast<unsigned>(free_before),
                 static_cast<unsigned>(largest_before),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                 esp_mqtt_client_get_outbox_size(client_));
    }
    heap_caps_free(json);
}

void MqttBridge::publish_snapshot(const TelemetryMessage &message)
{
    const std::uint16_t count = std::min<std::uint16_t>(
        message.register_count,
        static_cast<std::uint16_t>(message.registers.size()));
    const std::uint32_t end = static_cast<std::uint32_t>(message.first_register) + count;
    if (message.source_member_id < maximum_members &&
        message.first_register <= 182 && end > 182) {
        LiveCache &cache = live_cache_[message.source_member_id];
        cache.source_priority =
            message.registers[182U - message.first_register];
        cache.source_priority_seen = true;
    }
    if (message.source_member_id < maximum_members &&
        message.first_register <= 63 && end > 64) {
        const auto *descriptor = inverter::find_register(
            inverter::RegisterSpace::holding, 63);
        if (descriptor != nullptr) {
            const std::size_t offset = 63U - message.first_register;
            const auto decoded = inverter::decode_numeric_value(
                *descriptor, message.registers[offset],
                message.registers[offset + 1], true);
            // Some FSC models return a placeholder (0.1 W) here.  It is not
            // a usable inverter rating and must never generate a percentage.
            if (decoded.valid && decoded.engineering_value >= 500.0 &&
                decoded.engineering_value <= 100000.0) {
                LiveCache &cache = live_cache_[message.source_member_id];
                cache.rated_power_w = decoded.engineering_value;
                cache.rated_power_seen = true;
            }
        }
    }
    if (message.first_register <= 5 && end >= 13) {
        char serial_number[17]{};
        std::size_t used = 0;
        for (std::uint16_t address = 5; address <= 12 && used + 1 < sizeof(serial_number);
             ++address) {
            const std::uint16_t word = message.registers[address - message.first_register];
            const unsigned char high = static_cast<unsigned char>(word >> 8U);
            const unsigned char low = static_cast<unsigned char>(word);
            if (std::isprint(high) != 0 && used + 1 < sizeof(serial_number)) {
                serial_number[used++] = static_cast<char>(high);
            }
            if (std::isprint(low) != 0 && used + 1 < sizeof(serial_number)) {
                serial_number[used++] = static_cast<char>(low);
            }
        }
        serial_number[used] = '\0';
        cJSON *identity = cJSON_CreateObject();
        cJSON_AddNumberToObject(identity, "member_id", message.source_member_id);
        cJSON_AddNumberToObject(identity, "sequence", message.sequence);
        cJSON_AddNumberToObject(identity, "uptime_ms",
                                static_cast<double>(message.timestamp_ms));
        cJSON_AddStringToObject(identity, "name", "SerialNumber");
        cJSON_AddStringToObject(identity, "description", "Inverter serial number");
        cJSON_AddStringToObject(identity, "domain", "IDENTITY");
        cJSON_AddStringToObject(identity, "value", serial_number);
        char topic[256]{};
        make_topic(topic, sizeof(topic), message.source_member_id,
                   "snapshot/identity/SerialNumber/Inverter_serial_number");
        char *json = cJSON_PrintUnformatted(identity);
        if (json != nullptr) {
            enqueue_payload(client_, topic, json, 1, 1);
            cJSON_free(json);
        }
        cJSON_Delete(identity);
    }
    for (std::uint16_t index = 0; index < count; ++index) {
        const std::uint16_t address = message.first_register + index;
        if (address >= 5 && address <= 12) continue;
        const auto *descriptor = inverter::find_register(
            inverter::RegisterSpace::holding, address);
        if (!is_public_descriptor(descriptor) ||
            descriptor->access == inverter::RegisterAccess::write_only ||
            !descriptor_is_enabled(*descriptor, message.enabled_features,
                                   message.connected_pv_inputs)) continue;

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "member_id", message.source_member_id);
        cJSON_AddNumberToObject(root, "sequence", message.sequence);
        cJSON_AddNumberToObject(root, "uptime_ms",
                                static_cast<double>(message.timestamp_ms));
        cJSON_AddStringToObject(root, "name", application_name(*descriptor));
        cJSON_AddStringToObject(root, "register_name", descriptor->name);
        cJSON_AddStringToObject(root, "description", descriptor->description);
        cJSON_AddStringToObject(root, "domain",
                                inverter::register_domain_name(descriptor->domain));
        cJSON_AddStringToObject(root, "unit", engineering_unit(*descriptor));
        add_application_value(root, "value", *descriptor,
                              message.registers.data() + index,
                              static_cast<std::uint16_t>(count - index));

        char domain[32]{};
        char name[64]{};
        char description[64]{};
        make_topic_component(inverter::register_domain_name(descriptor->domain),
                             domain, sizeof(domain));
        make_topic_component(descriptor->name, name, sizeof(name));
        make_topic_component(descriptor->description, description,
                             sizeof(description));
        char suffix[192]{};
        std::snprintf(suffix, sizeof(suffix), "snapshot/%s/%s/%s",
                      domain, name, description);
        char topic[320]{};
        make_topic(topic, sizeof(topic), message.source_member_id, suffix);
        char *json = cJSON_PrintUnformatted(root);
        if (json != nullptr) {
            enqueue_payload(client_, topic, json, 1, 1);
            cJSON_free(json);
        }
        cJSON_Delete(root);
    }
}

void MqttBridge::publish_alert_changes(const TelemetryMessage &message)
{
    if (message.source_member_id >= maximum_members) return;
    const std::uint16_t count = std::min<std::uint16_t>(
        message.register_count,
        static_cast<std::uint16_t>(message.registers.size()));
    for (std::uint16_t index = 0; index < count; ++index) {
        const std::uint16_t address = message.first_register + index;
        if (address >= alert_point_count) continue;
        const auto *descriptor = inverter::find_register(
            inverter::RegisterSpace::input, address);
        if (!is_public_descriptor(descriptor) || !is_alert_descriptor(*descriptor) ||
            !descriptor_is_enabled(*descriptor, message.enabled_features,
                                   message.connected_pv_inputs)) continue;
        const std::uint16_t value = message.registers[index];
        const bool seen = alert_seen_[message.source_member_id][address];
        const std::uint16_t previous = alert_values_[message.source_member_id][address];
        if (seen && previous == value) continue;

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "member_id", message.source_member_id);
        cJSON_AddNumberToObject(root, "sequence", message.sequence);
        cJSON_AddNumberToObject(root, "uptime_ms",
                                static_cast<double>(message.timestamp_ms));
        const char *severity = alert_severity(*descriptor);
        cJSON_AddStringToObject(root, "severity", severity);
        cJSON_AddStringToObject(
            root, "state",
            std::strcmp(severity, "status") == 0 || std::strcmp(severity, "event") == 0
                ? "changed" : (value == 0 ? "clear" : "active"));
        cJSON_AddStringToObject(root, "name", descriptor->name);
        cJSON_AddStringToObject(root, "description", descriptor->description);
        if (seen) cJSON_AddNumberToObject(root, "previous_value", previous);
        else cJSON_AddNullToObject(root, "previous_value");
        cJSON_AddNumberToObject(root, "value", value);

        char name[64]{};
        make_topic_component(descriptor->name, name, sizeof(name));
        char suffix[96]{};
        std::snprintf(suffix, sizeof(suffix), "alert/%s", name);
        char topic[256]{};
        make_topic(topic, sizeof(topic), message.source_member_id, suffix);
        char *json = cJSON_PrintUnformatted(root);
        if (json != nullptr) {
            if (enqueue_payload(client_, topic, json, 1, 1) >= 0) {
                alert_seen_[message.source_member_id][address] = true;
                alert_values_[message.source_member_id][address] = value;
            }
            cJSON_free(json);
        }
        cJSON_Delete(root);
    }
}

void MqttBridge::publish_command_result(const RoutedCommandResult &message)
{
    if (publish_queue_ == nullptr) return;
    OutboundMessage outbound{};
    outbound.type = OutboundType::command_result;
    outbound.command_result = message;
    if (xQueueSend(publish_queue_, &outbound, 0) != pdTRUE) {
        ESP_LOGW(tag, "MQTT publish queue full; command result dropped");
    }
}

void MqttBridge::publish_command_result_now(const RoutedCommandResult &message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "member_id", message.source_member_id);
    char command_id[24]{};
    const std::uint64_t id = message.kind == RemoteCommandKind::semantic
                                 ? message.result.command_id
                                 : message.register_result.command_id;
    std::snprintf(command_id, sizeof(command_id), "%llu",
                  static_cast<unsigned long long>(id));
    cJSON_AddStringToObject(root, "command_id", command_id);
    if (message.kind == RemoteCommandKind::semantic) {
        cJSON_AddStringToObject(root, "operation", inverter::operation_name(message.result.operation));
        cJSON_AddStringToObject(root, "outcome", inverter::command_outcome_name(message.result.outcome));
        cJSON_AddBoolToObject(root, "write_sent", message.result.write_was_sent);
    } else {
        const auto &result = message.register_result;
        cJSON_AddStringToObject(root, "operation",
                                result.named_request
                                    ? (message.kind == RemoteCommandKind::read_registers
                                           ? "get" : "set")
                                    : (message.kind == RemoteCommandKind::read_registers
                                           ? "read_registers" : "write_registers"));
        cJSON_AddStringToObject(root, "outcome",
                                inverter::register_command_outcome_name(result.outcome));
        cJSON_AddStringToObject(root, "write_guard",
                                inverter::write_guard_name(result.strongest_guard));
        if (result.named_request) {
            const auto *descriptor = inverter::find_register(
                result.space, result.first_register);
            if (descriptor != nullptr) {
                cJSON_AddStringToObject(root, "name", descriptor->name);
                cJSON_AddStringToObject(root, "description", descriptor->description);
                cJSON_AddStringToObject(root, "domain",
                                        inverter::register_domain_name(descriptor->domain));
                cJSON_AddStringToObject(root, "unit", descriptor->unit);
                if (result.previous_values_valid) {
                    add_named_value(root, "previous_value", *descriptor,
                                    result.previous_values.data(), result.register_count);
                }
                if (result.result_values_valid) {
                    add_named_value(root, "value", *descriptor,
                                    result.result_values.data(), result.register_count);
                }
            }
        } else {
            cJSON_AddStringToObject(root, "space",
                                    result.space == inverter::RegisterSpace::holding
                                        ? "holding" : "input");
            cJSON_AddNumberToObject(root, "first_register", result.first_register);
            cJSON_AddNumberToObject(root, "register_count", result.register_count);
            if (result.previous_values_valid) {
                cJSON *previous = cJSON_AddArrayToObject(root, "previous_values");
                for (std::uint16_t index = 0; index < result.register_count; ++index) {
                    cJSON_AddItemToArray(previous,
                                        cJSON_CreateNumber(result.previous_values[index]));
                }
            }
            if (result.result_values_valid) {
                cJSON *values = cJSON_AddArrayToObject(root, "values");
                for (std::uint16_t index = 0; index < result.register_count; ++index) {
                    cJSON_AddItemToArray(values,
                                        cJSON_CreateNumber(result.result_values[index]));
                }
                add_register_details(root, "registers", result.space,
                                     result.first_register, result.register_count,
                                     result.result_values.data());
            }
        }
        if (!result.modbus_write.ok() &&
            result.modbus_write.status != protocol::ModbusWriteStatus::invalid_argument) {
            cJSON_AddNumberToObject(root, "modbus_status",
                                    static_cast<int>(result.modbus_write.status));
            cJSON_AddNumberToObject(root, "modbus_exception",
                                    result.modbus_write.exception_code);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    char topic[192]{};
    make_topic(topic, sizeof(topic), message.source_member_id, "result");
    if (json != nullptr) {
        enqueue_payload(client_, topic, json, 1, 0);
        cJSON_free(json);
    }
    cJSON_Delete(root);
    publish_setting_snapshot(message);
}

void MqttBridge::publish_setting_snapshot(const RoutedCommandResult &message)
{
    if (message.kind != RemoteCommandKind::write_registers) return;
    const auto &result = message.register_result;
    if (!result.named_request ||
        result.outcome != inverter::RegisterCommandOutcome::confirmed ||
        !result.result_values_valid) return;
    const auto *descriptor = inverter::find_register(result.space, result.first_register);
    if (!is_public_descriptor(descriptor)) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "member_id", message.source_member_id);
    char command_id[24]{};
    std::snprintf(command_id, sizeof(command_id), "%llu",
                  static_cast<unsigned long long>(result.command_id));
    cJSON_AddStringToObject(root, "updated_by_command_id", command_id);
    cJSON_AddStringToObject(root, "name", descriptor->name);
    cJSON_AddStringToObject(root, "description", descriptor->description);
    cJSON_AddStringToObject(root, "domain",
                            inverter::register_domain_name(descriptor->domain));
    cJSON_AddStringToObject(root, "unit", descriptor->unit);
    add_application_value(root, "value", *descriptor,
                          result.result_values.data(), result.register_count);

    char domain[32]{};
    char name[64]{};
    char description[64]{};
    make_topic_component(inverter::register_domain_name(descriptor->domain),
                         domain, sizeof(domain));
    make_topic_component(descriptor->name, name, sizeof(name));
    make_topic_component(descriptor->description, description,
                         sizeof(description));
    char suffix[192]{};
    std::snprintf(suffix, sizeof(suffix), "snapshot/%s/%s/%s",
                  domain, name, description);
    char topic[320]{};
    make_topic(topic, sizeof(topic), message.source_member_id, suffix);
    char *json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        enqueue_payload(client_, topic, json, 1, 1);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

void MqttBridge::publish_health()
{
    const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    const auto telemetry = last_telemetry_ms_.load();
    const auto live = last_live_enqueued_ms_.load();
    char payload[768]{};
    const int size = std::snprintf(payload, sizeof(payload),
        "{\"uptime_ms\":%llu,\"reset_reason\":%d,\"heap_free_bytes\":%u,"
        "\"heap_minimum_bytes\":%u,\"heap_largest_block_bytes\":%u,"
        "\"publisher_stack_free_bytes\":%u,\"outbox_bytes\":%d,"
        "\"publish_queue_depth\":%u,\"telemetry_seen\":%s,\"telemetry_age_ms\":%u,"
        "\"live_enqueued\":%s,\"live_enqueue_age_ms\":%u,"
        "\"mqtt_disconnect_count\":%u,\"enqueue_failures\":%u,\"telemetry_queue_drops\":%u}",
        static_cast<unsigned long long>(esp_timer_get_time() / 1000),
        static_cast<int>(esp_reset_reason()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
        esp_mqtt_client_get_outbox_size(client_),
        static_cast<unsigned>(uxQueueMessagesWaiting(publish_queue_)),
        telemetry ? "true" : "false", static_cast<unsigned>(telemetry ? now - telemetry : 0U),
        live ? "true" : "false", static_cast<unsigned>(live ? now - live : 0U),
        static_cast<unsigned>(reconnect_count_.load()),
        static_cast<unsigned>(mqtt_enqueue_failures.load()),
        static_cast<unsigned>(telemetry_queue_drops.load()));
    char topic[192]{};
    make_topic(topic, sizeof(topic), config_.profile.logical_member_id, "health");
    if (size > 0 && static_cast<std::size_t>(size) < sizeof(payload)) {
        enqueue_payload(client_, topic, payload, 0, 0);
    }
}

void MqttBridge::publisher_task(void *argument)
{
    auto *self = static_cast<MqttBridge *>(argument);
    OutboundMessage message{};
    std::uint32_t last_health = 0;
    while (self != nullptr) {
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (self->connected_ && (last_health == 0 || now - last_health >= app::health_publish_interval_ms)) {
            self->publish_health();
            last_health = now;
        }
        // Do not wait indefinitely on one old sample while the queue fills.
        // Drain telemetry during outages; a new input sample refreshes live.
        if (xQueueReceive(self->publish_queue_, &message, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (!self->connected_ && message.type == OutboundType::telemetry) {
            ++telemetry_queue_drops;
            continue;
        }
        if (message.type == OutboundType::telemetry) {
            self->publish_telemetry_now(message.telemetry);
        } else {
            self->publish_command_result_now(message.command_result);
        }
    }
}

void MqttBridge::make_topic(char *destination, std::size_t capacity,
                            std::uint8_t member_id, const char *suffix) const
{
    std::snprintf(destination, capacity, "%s/%s/inverter/%u/%s",
                  config_.mqtt_topic_prefix, config_.site_id, member_id, suffix);
}

} // namespace inverter_gateway::network
