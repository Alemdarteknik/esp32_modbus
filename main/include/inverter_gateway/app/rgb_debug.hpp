#pragma once

namespace inverter_gateway::app {
enum class RgbDebugState { starting, setup, wifi_wait, mqtt_wait, link_wait, data_wait, ready };
using RgbDebugProbe = RgbDebugState (*)(void *context);
// Context must remain alive for the lifetime of the firmware.
void start_rgb_debug(RgbDebugProbe probe, void *context);
void rgb_debug_publish_error();
}
