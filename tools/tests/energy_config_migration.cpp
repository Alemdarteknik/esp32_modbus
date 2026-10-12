#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "nvs.h"
#include "inverter_gateway/app/system_config.hpp"

using namespace inverter_gateway;

// Snapshot of the persisted schema3 layout, before the rating/link appends.
struct LegacyConfig3 {
    std::uint32_t magic=app::SystemConfig::magic_value;
    std::uint16_t schema_version=3;
    bool provisioned=true;
    app::DeviceProfile profile{};
    char site_id[33]{};
    char wifi_ssid[33]{};
    char wifi_password[65]{};
    char mqtt_uri[129]{};
    char mqtt_username[65]{};
    char mqtt_password[65]{};
    char mqtt_topic_prefix[65]{};
    std::array<app::SavedPeer,9> peers{};
};
struct LegacyConfig4 { LegacyConfig3 previous; float output_rated_power_kw=8.2F; };
static_assert(std::is_standard_layout_v<LegacyConfig3>);
static_assert(sizeof(LegacyConfig3)==548);
static_assert(sizeof(LegacyConfig4)==552);
static_assert(sizeof(LegacyConfig3)==offsetof(app::SystemConfig,output_rated_power_kw));
static_assert(sizeof(LegacyConfig4)==offsetof(app::SystemConfig,inverter_link));
static_assert(offsetof(LegacyConfig3,peers)==offsetof(app::SystemConfig,peers));
static_assert(offsetof(LegacyConfig3,wifi_password)==offsetof(app::SystemConfig,wifi_password));

int main()
{
    LegacyConfig3 legacy;
    legacy.profile.device_role=app::DeviceRole::coordinator_gateway_combined;
    legacy.profile.inverter_role=app::InverterRole::host;
    legacy.profile.topology=app::SystemTopology::parallel_single_phase;
    legacy.profile.expected_inverter_count=3;
    legacy.profile.expected_member_count=2;
    legacy.profile.modbus_slave_address=7;
    std::strcpy(legacy.site_id,"legacy-installation");
    std::strcpy(legacy.wifi_ssid,"Saved Wi-Fi");
    std::strcpy(legacy.wifi_password,"saved-wifi-password");
    std::strcpy(legacy.mqtt_uri,"mqtt://saved-broker:1883");
    std::strcpy(legacy.mqtt_username,"saved-user");
    std::strcpy(legacy.mqtt_password,"saved-mqtt-password");
    std::strcpy(legacy.mqtt_topic_prefix,"saved-prefix");
    for(unsigned i=0;i<legacy.peers.size();++i) {
        legacy.peers[i].valid=i<2;
        legacy.peers[i].logical_member_id=i+1;
        for(unsigned j=0;j<6;++j)legacy.peers[i].mac[j]=i*16+j;
    }
    const app::ConfigStore store;
    const auto saved_credentials=[&](const app::SystemConfig &loaded) {
        assert(loaded.schema_version==5 && loaded.provisioned);
        assert(std::memcmp(&loaded.profile,&legacy.profile,sizeof(legacy.profile))==0);
        assert(std::strcmp(loaded.site_id,legacy.site_id)==0);
        assert(std::strcmp(loaded.wifi_ssid,legacy.wifi_ssid)==0);
        assert(std::strcmp(loaded.wifi_password,legacy.wifi_password)==0);
        assert(std::strcmp(loaded.mqtt_uri,legacy.mqtt_uri)==0);
        assert(std::strcmp(loaded.mqtt_username,legacy.mqtt_username)==0);
        assert(std::strcmp(loaded.mqtt_password,legacy.mqtt_password)==0);
        assert(std::strcmp(loaded.mqtt_topic_prefix,legacy.mqtt_topic_prefix)==0);
        assert(std::memcmp(loaded.peers.data(),legacy.peers.data(),sizeof(legacy.peers))==0);
        assert(loaded.inverter_link.protocol==protocol::EnergyProtocol::aohai_modbus);
        assert(loaded.inverter_link.connection==protocol::EnergyConnection::usb_rs485);
        assert(loaded.inverter_link.address==7);
        assert(app::validate_config(loaded)==app::ConfigValidation::valid);
    };
    app::SystemConfig loaded;
    nvs_set_blob(1,"system_config",&legacy,sizeof(legacy));
    assert(store.load(loaded)==ESP_OK);
    saved_credentials(loaded);
    assert(loaded.output_rated_power_kw==0.0F);
    LegacyConfig4 legacy4{legacy,8.2F}; legacy4.previous.schema_version=4;
    nvs_set_blob(1,"system_config",&legacy4,sizeof(legacy4));
    assert(store.load(loaded)==ESP_OK);
    saved_credentials(loaded);
    assert(loaded.output_rated_power_kw==8.2F);
    assert(store.save(loaded)==ESP_OK && host_nvs_blob.size()==sizeof(loaded));
    app::SystemConfig reloaded;
    assert(store.load(reloaded)==ESP_OK);
    saved_credentials(reloaded);
    assert(std::memcmp(&loaded,&reloaded,sizeof(loaded))==0);

    // A short or unknown blob must not overwrite the supplied configuration.
    host_nvs_blob.pop_back(); reloaded.site_id[0]='X';
    assert(store.load(reloaded)==ESP_ERR_INVALID_VERSION && reloaded.site_id[0]=='X');
    legacy4.previous.schema_version=2;
    nvs_set_blob(1,"system_config",&legacy4,sizeof(legacy4));
    assert(store.load(reloaded)==ESP_ERR_INVALID_VERSION);

    auto invalid=loaded;
    invalid.inverter_link.protocol=protocol::EnergyProtocol::pi30;
    invalid.inverter_link.connection=protocol::EnergyConnection::uart_rs232;
    assert(app::validate_config(invalid)==app::ConfigValidation::valid);
    for(const int pin:{0,17,18,19,20,22,25,26,27,32,33,34,35,36,37,43,44,48,49}) {
        invalid.inverter_link.tx_pin=pin;
        assert(app::validate_config(invalid)==app::ConfigValidation::inverter_connection_invalid);
    }
    invalid.inverter_link.tx_pin=46;
    assert(app::validate_config(invalid)==app::ConfigValidation::inverter_connection_invalid);
    invalid.inverter_link.tx_pin=4;invalid.inverter_link.rx_pin=46;
    assert(app::validate_config(invalid)==app::ConfigValidation::valid);
    invalid.inverter_link.rx_pin=5;
    invalid.inverter_link.uart_port=1;
    assert(app::validate_config(invalid)==app::ConfigValidation::inverter_connection_invalid);
    invalid.inverter_link.uart_port=2;
    invalid.inverter_link.connection=protocol::EnergyConnection::uart_rs485;
    for(const int pin:{-1,4,5,17,18,19,20,26,33,46,48}) {
        invalid.inverter_link.direction_pin=pin;
        assert(app::validate_config(invalid)==app::ConfigValidation::inverter_connection_invalid);
    }
    invalid.inverter_link.direction_pin=6;
    assert(app::validate_config(invalid)==app::ConfigValidation::valid);

    invalid.inverter_link.protocol=protocol::EnergyProtocol::jk02;
    invalid.inverter_link.connection=protocol::EnergyConnection::bluetooth;
    std::strcpy(invalid.inverter_link.bluetooth_address,"A1:B2:C3:D4:E5:F6");
    for(unsigned type=0;type<=1;++type) {
        invalid.inverter_link.bluetooth_address_type=type;
        assert(app::validate_config(invalid)==app::ConfigValidation::valid);
    }
    invalid.inverter_link.bluetooth_address_type=2;
    assert(app::validate_config(invalid)==app::ConfigValidation::inverter_connection_invalid);
    invalid.inverter_link.bluetooth_address_type=0;
    for(const char *address:{"00:00:00:00:00:00","FF:FF:FF:FF:FF:FF","Ff:fF:ff:FF:fF:Ff","GG:11:22:33:44:55","AA-BB-CC-DD-EE-FF",""}) {
        std::strcpy(invalid.inverter_link.bluetooth_address,address);
        assert(app::validate_config(invalid)==app::ConfigValidation::inverter_connection_invalid);
    }
    invalid.inverter_link.connection=protocol::EnergyConnection::usb_hid;
    assert(app::validate_config(invalid)==app::ConfigValidation::inverter_connection_invalid);
    std::puts("Schema3/4 migration preserves credentials, peer identities and Modbus defaults; GPIO/BLE validation passed.");
}
