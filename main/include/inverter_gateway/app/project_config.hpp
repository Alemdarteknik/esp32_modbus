#pragma once

// Central firmware defaults. Change these values before building when a
// product variant needs different hardware, timing, or commissioning defaults.

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"

namespace inverter_gateway::app {

// OTA opt-in for bench testing first. HTTPS trust only; signing not provisioned.
inline constexpr bool ota_enabled = true;
inline constexpr char ota_download_url[] = "https://stock.alemdarteknik.com/download";
inline constexpr std::uint32_t ota_check_interval_ms = 6 * 60 * 60 * 1000;
inline constexpr std::uint32_t ota_jitter_ms = 60 * 60 * 1000;
inline constexpr std::uint32_t ota_download_timeout_ms = 15 * 60 * 1000;
// Internal-memory admission BEFORE creating the HTTPS connection.
inline constexpr std::size_t ota_minimum_free_heap = 64000;
inline constexpr std::size_t ota_minimum_largest_block = 32768;
// Once TLS/HTTP buffers exist, retain operating headroom instead of demanding
// room for a second handshake. These are guards, not reserved allocations or
// a hardware-validated guarantee that concurrent workloads always fit.
inline constexpr std::size_t ota_stream_minimum_free_heap = 32768;
inline constexpr std::size_t ota_stream_minimum_largest_block = 8192;
static_assert(ota_stream_minimum_free_heap >= ota_stream_minimum_largest_block &&
              ota_stream_minimum_largest_block >= 8192);
static_assert(ota_minimum_free_heap >= ota_stream_minimum_free_heap &&
              ota_minimum_largest_block >= ota_stream_minimum_largest_block &&
              ota_minimum_free_heap >= ota_minimum_largest_block);
static_assert(ota_check_interval_ms > 0 && ota_jitter_ms > 0);

// Serial monitor
inline constexpr bool serial_monitor_output_default = false;

// One addressable WS2812 RGB LED (not a three-pin discrete RGB LED).
// Verify the PCB pin: N16R8 identifies memory, not the LED wiring.
inline constexpr bool rgb_debug_enabled = true;
inline constexpr gpio_num_t rgb_debug_gpio = GPIO_NUM_48;
inline constexpr std::uint8_t rgb_debug_brightness = 16; // 0..255
inline constexpr bool rgb_debug_grb_order = true;
inline constexpr std::uint32_t rgb_debug_tick_ms = 250;
inline constexpr std::uint32_t rgb_debug_data_timeout_ms = 15000;
static_assert(rgb_debug_tick_ms > 0 && rgb_debug_data_timeout_ms > 0);

// Inverter Modbus and USB-RS485
inline constexpr std::uint32_t inverter_baud = 9600;
inline constexpr std::uint8_t default_modbus_slave_address = 1;
inline constexpr std::uint32_t modbus_response_timeout_ms = 1500;
inline constexpr std::uint32_t modbus_minimum_command_gap_ms = 850;
inline constexpr std::uint32_t usb_settle_time_ms = 750;
inline constexpr std::size_t usb_buffer_size = 512;
inline constexpr std::uint32_t usb_open_timeout_ms = 1000;
inline constexpr std::uint32_t usb_reconnect_delay_ms = 250;
inline constexpr std::uint32_t usb_configure_retry_ms = 500;
inline constexpr std::uint32_t inverter_loop_delay_ms = 50;
// Require a recent successful poll before accepting inverter commands.
inline constexpr std::uint32_t command_link_max_age_ms = 15000;
// ESP-IDF task stack sizes are bytes. Leave affinity to the SMP scheduler.
inline constexpr std::uint32_t inverter_task_stack_bytes = 8192;
inline constexpr unsigned inverter_task_priority = 4;
inline constexpr std::uint32_t inverter_task_diagnostics_ms = 30000;

// Production polling
// Full sweeps exceed 10 s with the 850 ms bus gap and interleaved status reads.
// This is a freshness ceiling, not a guarantee of simultaneous measurements.
inline constexpr std::uint32_t live_value_max_age_ms = 120000;
// Calculation inputs must be recent AND close together in source time.
inline constexpr std::uint32_t power_calculation_max_age_ms = 15000;
inline constexpr std::uint32_t power_calculation_max_skew_ms = 5000;
// Set only from the inverter nameplate. Zero means unknown; never infer from model ID.
// Rated active output power (kW) is selected and persisted through the setup webpage.
inline constexpr double output_rated_apparent_kva = 0.0;
inline constexpr std::uint32_t slow_value_max_age_ms = 900000;
inline constexpr std::uint32_t health_publish_interval_ms = 30000;
inline constexpr std::uint32_t mqtt_reconnect_interval_ms = 5000;
inline constexpr std::uint32_t mqtt_startup_jitter_ms = 15000;
inline constexpr std::uint32_t mqtt_reconnect_jitter_ms = 30000;
inline constexpr std::uint32_t mqtt_live_publish_interval_ms = 5000;
// Reserve before TLS/JSON allocation. PSRAM is used only when available.
// Internal fallback covers the observed ~27 KiB live payload, with headroom.
inline constexpr std::size_t mqtt_live_buffer_internal_bytes = 32U * 1024U;
inline constexpr std::size_t mqtt_live_buffer_psram_bytes = 48U * 1024U;
static_assert(mqtt_live_buffer_internal_bytes >= 32U * 1024U &&
              mqtt_live_buffer_internal_bytes <= 48U * 1024U);
static_assert(mqtt_live_buffer_psram_bytes >= mqtt_live_buffer_internal_bytes &&
              mqtt_live_buffer_psram_bytes <= 48U * 1024U);
inline constexpr bool mqtt_require_secure_credentials = false; // Enable for authenticated TLS fleet deployments.
static_assert(mqtt_live_publish_interval_ms > 0 && mqtt_reconnect_interval_ms > 0);
static_assert(mqtt_startup_jitter_ms < 60000 && mqtt_reconnect_jitter_ms < 60000);
inline constexpr std::uint32_t mqtt_network_timeout_ms = 10000;
inline constexpr std::size_t mqtt_command_max_bytes = 2048;
inline constexpr std::uint32_t critical_poll_interval_ms = 1000;
inline constexpr std::uint32_t fast_poll_interval_ms = 5000;
inline constexpr std::uint32_t normal_poll_interval_ms = 30000;
inline constexpr std::uint32_t slow_poll_interval_ms = 300000;
inline constexpr std::uint32_t inverter_state_freshness_ms = 5000;
inline constexpr std::uint32_t diagnostic_poll_interval_ms = 5000;

// Setup access point and webpage
inline constexpr char setup_ap_ssid_prefix[] = "INVERTER-";
inline constexpr char setup_ap_password[] = "12345678";
inline constexpr char setup_page_url[] = "http://esp32.local";
inline constexpr std::uint8_t setup_ap_default_channel = 1;
inline constexpr std::uint8_t setup_ap_max_connections = 4;
inline constexpr std::uint16_t setup_wifi_scan_max_results = 20;
inline constexpr std::uint32_t setup_status_refresh_ms = 3000;
inline constexpr std::uint32_t setup_verification_check_ms = 500;
inline constexpr std::uint32_t setup_verification_stable_ms = 3000;
inline constexpr std::uint32_t mqtt_wifi_ready_wait_ms = 15000;
inline constexpr std::uint32_t setup_http_task_stack_size = 6144;
inline constexpr std::size_t setup_request_max_bytes = 2048;

// Runtime setup button
inline constexpr gpio_num_t provisioning_button_gpio = GPIO_NUM_0;
inline constexpr std::uint32_t provisioning_button_hold_ms = 3000;

// Separate coordinator-to-Internet-Gateway cable
inline constexpr int coordinator_uart_port = 1;
inline constexpr gpio_num_t coordinator_uart_tx_gpio = GPIO_NUM_17;
inline constexpr gpio_num_t coordinator_uart_rx_gpio = GPIO_NUM_18;
inline constexpr std::uint32_t coordinator_uart_baud = 115200;
inline constexpr std::uint32_t coordinator_heartbeat_interval_ms = 2000;
inline constexpr std::uint32_t coordinator_link_timeout_ms = 10000;

// ESP-NOW discovery
inline constexpr std::uint32_t espnow_announce_interval_ms = 5000;

static_assert(default_modbus_slave_address >= 1 &&
              default_modbus_slave_address <= 247,
              "The default Modbus address must be between 1 and 247");
static_assert(critical_poll_interval_ms > 0 &&
              fast_poll_interval_ms >= critical_poll_interval_ms &&
              normal_poll_interval_ms >= fast_poll_interval_ms &&
              slow_poll_interval_ms >= normal_poll_interval_ms,
              "Polling intervals must be nonzero and ordered from critical to slow");
static_assert(sizeof(setup_ap_password) - 1 >= 8 &&
              sizeof(setup_ap_password) - 1 <= 63,
              "The setup AP password must contain 8 to 63 characters");
static_assert(setup_ap_default_channel >= 1 && setup_ap_default_channel <= 14,
              "The setup AP channel must be between 1 and 14");
static_assert(setup_verification_check_ms > 0 &&
              setup_verification_stable_ms >= setup_verification_check_ms,
              "Setup verification timing must be nonzero and ordered");
static_assert(mqtt_wifi_ready_wait_ms > 0,
              "MQTT Wi-Fi readiness wait must be nonzero");

} // namespace inverter_gateway::app
