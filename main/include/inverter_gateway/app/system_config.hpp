#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "inverter_gateway/app/device_profile.hpp"
#include "inverter_gateway/protocol/energy_protocol.hpp"

namespace inverter_gateway::app {

inline constexpr std::size_t max_wifi_ssid_length = 32;
inline constexpr std::size_t max_wifi_password_length = 64;
inline constexpr std::size_t max_mqtt_uri_length = 128;
inline constexpr std::size_t max_mqtt_text_length = 64;
inline constexpr std::size_t max_site_id_length = 32;
inline constexpr std::size_t max_saved_peers = 9;

struct SavedPeer {
    std::uint8_t mac[6]{};
    std::uint8_t logical_member_id = 0;
    bool valid = false;
};

struct InverterLinkConfig {
    protocol::EnergyProtocol protocol = protocol::EnergyProtocol::aohai_modbus;
    protocol::EnergyConnection connection = protocol::EnergyConnection::usb_rs485;
    std::uint8_t address = 1;
    std::uint8_t uart_port = 2;
    std::uint32_t baud = 9600;
    std::int8_t tx_pin = 4;
    std::int8_t rx_pin = 5;
    std::int8_t direction_pin = 6;
    std::uint8_t parity = 0;
    char bluetooth_address[18]{};
    std::uint8_t bluetooth_address_type = 0;
    std::uint16_t usb_vid = 0; // USB HID: zero allows automatic VID discovery.
    std::uint16_t usb_pid = 0; // Each nonzero VID/PID is an optional match filter.
};

struct SystemConfig {
    static constexpr std::uint32_t magic_value = 0x49564757; // IVGW
    static constexpr std::uint16_t current_schema_version = 5;

    std::uint32_t magic = magic_value;
    std::uint16_t schema_version = current_schema_version;
    bool provisioned = false;
    DeviceProfile profile{};
    char site_id[max_site_id_length + 1]{};
    char wifi_ssid[max_wifi_ssid_length + 1]{};
    char wifi_password[max_wifi_password_length + 1]{};
    char mqtt_uri[max_mqtt_uri_length + 1]{};
    char mqtt_username[max_mqtt_text_length + 1]{};
    char mqtt_password[max_mqtt_text_length + 1]{};
    char mqtt_topic_prefix[max_mqtt_text_length + 1]{"inverter"};
    std::array<SavedPeer, max_saved_peers> peers{};
    float output_rated_power_kw = 12.0F; // User-requested default; select actual nameplate in setup.
    // Append only: preserve the complete schema 3/4 credential/peer layout.
    InverterLinkConfig inverter_link{};
};

// Historical NVS blobs store this exact layout. Catch accidental changes to
// the preserved prefix before compiling a firmware that would lose setup.
static_assert(sizeof(DeviceProfile) == 14 && sizeof(SavedPeer) == 8);
static_assert(offsetof(SystemConfig, output_rated_power_kw) == 548);
static_assert(offsetof(SystemConfig, inverter_link) == 552);

enum class ConfigValidation : std::uint8_t {
    valid,
    bad_header,
    role_missing,
    topology_missing,
    invalid_counts,
    invalid_member_id,
    invalid_pv_input_count,
    invalid_battery_configuration,
    site_id_required,
    wifi_required,
    mqtt_required,
    espnow_channel_invalid,
    inverter_protocol_invalid,
    inverter_connection_invalid,
};

ConfigValidation validate_config(const SystemConfig &config);
const char *config_validation_name(ConfigValidation result);
bool valid_site_id(const char *text);
esp_err_t ensure_hardware_site_id(SystemConfig &config);
bool site_id_is_hardware_default(const SystemConfig &config);
const char *device_role_name(DeviceRole role);
bool parse_device_role(const char *text, DeviceRole &role);
const char *topology_name(SystemTopology topology);
bool parse_topology(const char *text, SystemTopology &topology);
const char *phase_assignment_name(PhaseAssignment phase);
bool parse_phase_assignment(const char *text, PhaseAssignment &phase);

class ConfigStore {
public:
    esp_err_t load(SystemConfig &config) const;
    esp_err_t save(const SystemConfig &config) const;
    esp_err_t clear() const;
    esp_err_t load_verification_pending(bool &pending) const;
    esp_err_t save_verification_pending(bool pending) const;
};

} // namespace inverter_gateway::app
