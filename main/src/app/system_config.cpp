#include "inverter_gateway/app/system_config.hpp"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cctype>

#include "esp_mac.h"
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/transport/uart_pin_policy.hpp"
#include "nvs.h"

namespace inverter_gateway::app {
namespace {

constexpr char nvs_namespace[] = "inv_gateway";
constexpr char nvs_key[] = "system_config";
constexpr char verification_pending_key[] = "verify_pending";

bool terminated(const char *text, std::size_t size)
{
    return std::memchr(text, '\0', size) != nullptr;
}

bool has_text(const char *text, std::size_t size)
{
    return terminated(text, size) && text[0] != '\0';
}

bool valid_topic_segment(const char *text)
{
    if (text == nullptr || text[0] == '\0') return false;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(text);
         *cursor != '\0'; ++cursor) {
        const bool allowed = (*cursor >= 'a' && *cursor <= 'z') ||
                             (*cursor >= 'A' && *cursor <= 'Z') ||
                             (*cursor >= '0' && *cursor <= '9') ||
                             *cursor == '-' || *cursor == '_';
        if (!allowed) return false;
    }
    return true;
}

bool valid_mqtt_uri(const char *text)
{
    if (text == nullptr) return false;
    constexpr const char *schemes[] = {"mqtt://", "mqtts://", "ws://", "wss://"};
    for (const char *scheme : schemes) {
        const std::size_t length = std::strlen(scheme);
        if (std::strncmp(text, scheme, length) == 0 && text[length] != '\0') {
            return true;
        }
    }
    return false;
}

} // namespace

bool valid_site_id(const char *text)
{
    return valid_topic_segment(text);
}

esp_err_t ensure_hardware_site_id(SystemConfig &config)
{
    if (config.site_id[0] != '\0') {
        return valid_site_id(config.site_id) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    std::uint8_t mac[6]{};
    const esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (result != ESP_OK) return result;
    std::snprintf(config.site_id, sizeof(config.site_id),
                  "INV-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned>(mac[0]), static_cast<unsigned>(mac[1]),
                  static_cast<unsigned>(mac[2]), static_cast<unsigned>(mac[3]),
                  static_cast<unsigned>(mac[4]), static_cast<unsigned>(mac[5]));
    return ESP_OK;
}

bool site_id_is_hardware_default(const SystemConfig &config)
{
    SystemConfig generated{};
    if (ensure_hardware_site_id(generated) != ESP_OK) return false;
    return std::strcmp(config.site_id, generated.site_id) == 0;
}

ConfigValidation validate_config(const SystemConfig &config)
{
    if (!std::isfinite(config.output_rated_power_kw) || config.output_rated_power_kw < 0.0F ||
        config.output_rated_power_kw > 1000.0F) return ConfigValidation::invalid_counts;
    if (config.magic != SystemConfig::magic_value ||
        config.schema_version != SystemConfig::current_schema_version ||
        config.profile.schema_version != DeviceProfile::current_schema_version) {
        return ConfigValidation::bad_header;
    }
    if (config.profile.device_role == DeviceRole::unprovisioned) {
        return ConfigValidation::role_missing;
    }
    if (static_cast<std::uint8_t>(config.profile.device_role) >
            static_cast<std::uint8_t>(DeviceRole::coordinator_gateway_combined) ||
        static_cast<std::uint8_t>(config.profile.inverter_role) >
            static_cast<std::uint8_t>(InverterRole::member) ||
        static_cast<std::uint8_t>(config.profile.topology) >
            static_cast<std::uint8_t>(SystemTopology::parallel_native_three_phase) ||
        static_cast<std::uint8_t>(config.profile.phase) >
            static_cast<std::uint8_t>(PhaseAssignment::phase_3)) {
        return ConfigValidation::bad_header;
    }
    if (config.profile.topology == SystemTopology::unknown) {
        return ConfigValidation::topology_missing;
    }
    if (role_has_local_inverter(config.profile.device_role)) {
        const auto &link = config.inverter_link;
        if (link.protocol >= protocol::EnergyProtocol::count ||
            !protocol::energy_protocol_info(link.protocol).implemented)
            return ConfigValidation::inverter_protocol_invalid;
        if (!protocol::energy_connection_supported(link.protocol, link.connection) ||
            link.address == 0 || link.baud < 1200 || link.baud > 115200 || link.parity > 2 ||
            (link.connection == protocol::EnergyConnection::usb_rs485 && link.parity != 0) ||
            (link.protocol == protocol::EnergyProtocol::aohai_modbus && link.baud != inverter_baud))
            return ConfigValidation::inverter_connection_invalid;
        const bool uart = link.connection == protocol::EnergyConnection::uart_rs232 ||
            link.connection == protocol::EnergyConnection::uart_rs485 ||
            link.connection == protocol::EnergyConnection::uart_ttl;
        if (uart && (link.uart_port != 2 ||
            !transport::inverter_uart_pin_allowed(link.tx_pin, true) ||
            !transport::inverter_uart_pin_allowed(link.rx_pin, false) || link.tx_pin == link.rx_pin ||
            (link.connection == protocol::EnergyConnection::uart_rs485 &&
            (!transport::inverter_uart_pin_allowed(link.direction_pin, true) ||
             link.direction_pin == link.tx_pin || link.direction_pin == link.rx_pin))))
            return ConfigValidation::inverter_connection_invalid;
        if (!terminated(link.bluetooth_address, sizeof(link.bluetooth_address)))
            return ConfigValidation::inverter_connection_invalid;
        if (link.connection == protocol::EnergyConnection::bluetooth) {
            if (std::strlen(link.bluetooth_address) != 17 || link.bluetooth_address_type > 1)
                return ConfigValidation::inverter_connection_invalid;
            bool all_zero = true, all_ff = true;
            for (unsigned i=0;i<17;++i) {
                const auto ch=static_cast<unsigned char>(link.bluetooth_address[i]);
                if ((i%3==2 && ch!=':') || (i%3!=2 && !std::isxdigit(ch)))
                    return ConfigValidation::inverter_connection_invalid;
                if (i%3!=2) { all_zero &= ch=='0'; all_ff &= std::tolower(ch)=='f'; }
            }
            if (all_zero || all_ff) return ConfigValidation::inverter_connection_invalid;
        }
    }
    const InverterRole expected_inverter_role =
        config.profile.device_role == DeviceRole::parallel_member
            ? InverterRole::member
            : role_is_espnow_coordinator(config.profile.device_role)
                  ? InverterRole::host
                  : config.profile.device_role == DeviceRole::standalone_combined
                        ? InverterRole::standalone
                        : InverterRole::none;
    if (config.profile.inverter_role != expected_inverter_role) {
        return ConfigValidation::bad_header;
    }
    if (config.profile.expected_inverter_count == 0 ||
        config.profile.expected_inverter_count > max_saved_peers ||
        config.profile.expected_member_count > max_saved_peers ||
        config.profile.expected_member_count + 1 > config.profile.expected_inverter_count) {
        return ConfigValidation::invalid_counts;
    }
    const bool parallel_topology =
        config.profile.topology == SystemTopology::parallel_single_phase ||
        config.profile.topology == SystemTopology::parallel_three_phase_groups ||
        config.profile.topology == SystemTopology::parallel_native_three_phase;
    const bool parallel_role = config.profile.device_role == DeviceRole::parallel_coordinator ||
                               config.profile.device_role == DeviceRole::parallel_member ||
                               config.profile.device_role == DeviceRole::internet_gateway ||
                               config.profile.device_role == DeviceRole::coordinator_gateway_combined;
    if (parallel_topology != parallel_role ||
        (parallel_topology &&
         (config.profile.expected_inverter_count < 2 ||
          config.profile.expected_member_count != config.profile.expected_inverter_count - 1)) ||
        (!parallel_topology &&
         (config.profile.expected_inverter_count != 1 ||
          config.profile.expected_member_count != 0))) {
        return ConfigValidation::invalid_counts;
    }
    if (role_is_espnow_member(config.profile.device_role) &&
        (config.profile.logical_member_id == 0 ||
         config.profile.logical_member_id >= config.profile.expected_inverter_count)) {
        return ConfigValidation::invalid_member_id;
    }
    if (!role_is_espnow_member(config.profile.device_role) &&
        config.profile.logical_member_id != 0) {
        return ConfigValidation::invalid_member_id;
    }
    if (config.profile.modbus_slave_address == 0 ||
        config.profile.modbus_slave_address > 247) {
        return ConfigValidation::invalid_counts;
    }
    if (config.profile.connected_pv_inputs > 16) {
        return ConfigValidation::invalid_pv_input_count;
    }
    if (!config.profile.battery_installed && config.profile.bms_connected) {
        return ConfigValidation::invalid_battery_configuration;
    }
    if (config.profile.espnow_channel > 14) {
        return ConfigValidation::espnow_channel_invalid;
    }
    if (!terminated(config.site_id, sizeof(config.site_id)) ||
        !terminated(config.wifi_ssid, sizeof(config.wifi_ssid)) ||
        !terminated(config.wifi_password, sizeof(config.wifi_password)) ||
        !terminated(config.mqtt_uri, sizeof(config.mqtt_uri)) ||
        !terminated(config.mqtt_username, sizeof(config.mqtt_username)) ||
        !terminated(config.mqtt_password, sizeof(config.mqtt_password)) ||
        !terminated(config.mqtt_topic_prefix, sizeof(config.mqtt_topic_prefix))) {
        return ConfigValidation::bad_header;
    }
    if (!valid_site_id(config.site_id)) return ConfigValidation::site_id_required;
    if (role_uses_mqtt(config.profile.device_role)) {
        if (mqtt_require_secure_credentials &&
            ((std::strncmp(config.mqtt_uri, "mqtts://", 8) != 0 &&
              std::strncmp(config.mqtt_uri, "wss://", 6) != 0) ||
             !has_text(config.mqtt_username, sizeof(config.mqtt_username)) ||
             !has_text(config.mqtt_password, sizeof(config.mqtt_password))))
            return ConfigValidation::mqtt_required;
        if (!has_text(config.wifi_ssid, sizeof(config.wifi_ssid))) {
            return ConfigValidation::wifi_required;
        }
        if (!has_text(config.mqtt_uri, sizeof(config.mqtt_uri)) ||
            !valid_mqtt_uri(config.mqtt_uri) ||
            !valid_topic_segment(config.mqtt_topic_prefix)) {
            return ConfigValidation::mqtt_required;
        }
    }
    return ConfigValidation::valid;
}

const char *config_validation_name(ConfigValidation result)
{
    switch (result) {
    case ConfigValidation::valid: return "valid";
    case ConfigValidation::bad_header: return "bad_header";
    case ConfigValidation::role_missing: return "role_missing";
    case ConfigValidation::topology_missing: return "topology_missing";
    case ConfigValidation::invalid_counts: return "invalid_counts";
    case ConfigValidation::invalid_member_id: return "invalid_member_id";
    case ConfigValidation::invalid_pv_input_count: return "invalid_pv_input_count";
    case ConfigValidation::invalid_battery_configuration: return "invalid_battery_configuration";
    case ConfigValidation::site_id_required: return "site_id_required";
    case ConfigValidation::wifi_required: return "wifi_required";
    case ConfigValidation::mqtt_required: return "mqtt_required";
    case ConfigValidation::espnow_channel_invalid: return "espnow_channel_invalid";
    case ConfigValidation::inverter_protocol_invalid: return "inverter_protocol_invalid";
    case ConfigValidation::inverter_connection_invalid: return "inverter_connection_invalid";
    }
    return "unknown";
}

const char *device_role_name(DeviceRole role)
{
    switch (role) {
    case DeviceRole::unprovisioned: return "unprovisioned";
    case DeviceRole::standalone_combined: return "standalone_combined";
    case DeviceRole::parallel_coordinator: return "parallel_coordinator";
    case DeviceRole::parallel_member: return "parallel_member";
    case DeviceRole::internet_gateway: return "internet_gateway";
    case DeviceRole::coordinator_gateway_combined: return "coordinator_gateway_combined";
    }
    return "unprovisioned";
}

bool parse_device_role(const char *text, DeviceRole &role)
{
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(DeviceRole::coordinator_gateway_combined); ++raw) {
        const auto candidate = static_cast<DeviceRole>(raw);
        if (text != nullptr && std::strcmp(text, device_role_name(candidate)) == 0) {
            role = candidate;
            return true;
        }
    }
    return false;
}

const char *topology_name(SystemTopology topology)
{
    switch (topology) {
    case SystemTopology::unknown: return "unknown";
    case SystemTopology::standalone_single_phase: return "standalone_single_phase";
    case SystemTopology::standalone_native_three_phase: return "standalone_native_three_phase";
    case SystemTopology::parallel_single_phase: return "parallel_single_phase";
    case SystemTopology::parallel_three_phase_groups: return "parallel_three_phase_groups";
    case SystemTopology::parallel_native_three_phase: return "parallel_native_three_phase";
    }
    return "unknown";
}

bool parse_topology(const char *text, SystemTopology &topology)
{
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(SystemTopology::parallel_native_three_phase); ++raw) {
        const auto candidate = static_cast<SystemTopology>(raw);
        if (text != nullptr && std::strcmp(text, topology_name(candidate)) == 0) {
            topology = candidate;
            return true;
        }
    }
    return false;
}

const char *phase_assignment_name(PhaseAssignment phase)
{
    switch (phase) {
    case PhaseAssignment::none: return "none";
    case PhaseAssignment::parallel_single_phase: return "parallel_single_phase";
    case PhaseAssignment::phase_1: return "phase_1";
    case PhaseAssignment::phase_2: return "phase_2";
    case PhaseAssignment::phase_3: return "phase_3";
    }
    return "none";
}

bool parse_phase_assignment(const char *text, PhaseAssignment &phase)
{
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(PhaseAssignment::phase_3); ++raw) {
        const auto candidate = static_cast<PhaseAssignment>(raw);
        if (text != nullptr && std::strcmp(text, phase_assignment_name(candidate)) == 0) {
            phase = candidate;
            return true;
        }
    }
    return false;
}

esp_err_t ConfigStore::load(SystemConfig &config) const
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(nvs_namespace, NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    SystemConfig candidate{};
    std::size_t size = sizeof(candidate);
    result = nvs_get_blob(handle, nvs_key, &candidate, &size);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    // Schema 4 appended rating; schema 5 appends the inverter link configuration.
    if (candidate.magic == SystemConfig::magic_value && candidate.schema_version == 3 &&
        size == offsetof(SystemConfig, output_rated_power_kw)) {
        candidate.output_rated_power_kw = 0.0F;
        candidate.schema_version = 4;
        size = offsetof(SystemConfig, inverter_link);
    }
    if (candidate.magic == SystemConfig::magic_value && candidate.schema_version == 4 &&
        size == offsetof(SystemConfig, inverter_link)) {
        candidate.inverter_link = {};
        candidate.inverter_link.address = candidate.profile.modbus_slave_address;
        candidate.schema_version = SystemConfig::current_schema_version;
        size = sizeof(candidate);
    }
    if (size != sizeof(candidate) || validate_config(candidate) == ConfigValidation::bad_header) {
        return ESP_ERR_INVALID_VERSION;
    }
    config = candidate;
    return ESP_OK;
}

esp_err_t ConfigStore::save(const SystemConfig &config) const
{
    if (validate_config(config) != ConfigValidation::valid) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_blob(handle, nvs_key, &config, sizeof(config));
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result;
}

esp_err_t ConfigStore::clear() const
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
    // Factory reset owns the complete application namespace. Erasing the
    // namespace also removes monitor preferences and any future setup keys.
    if (result == ESP_OK) result = nvs_erase_all(handle);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result;
}

esp_err_t ConfigStore::load_verification_pending(bool &pending) const
{
    pending = false;
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(nvs_namespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    std::uint8_t stored = 0;
    result = nvs_get_u8(handle, verification_pending_key, &stored);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result == ESP_OK) pending = stored != 0;
    return result;
}

esp_err_t ConfigStore::save_verification_pending(bool pending) const
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, verification_pending_key, pending ? 1 : 0);
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result;
}

} // namespace inverter_gateway::app
