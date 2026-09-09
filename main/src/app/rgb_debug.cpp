#include "inverter_gateway/app/rgb_debug.hpp"
#include "inverter_gateway/app/project_config.hpp"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace inverter_gateway::app {
namespace {
constexpr char tag[] = "rgb_debug";
rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t encoder = nullptr;
RgbDebugProbe state_probe = nullptr;
void *state_context = nullptr;
std::atomic<std::uint32_t> last_publish_error_ms{0};

void led_task(void *)
{
    unsigned tick = 0;
    unsigned previous = 0xFFFFFFFFU;
    // Buffer remains alive until TX completes, including on timeout.
    rmt_symbol_word_t symbols[25]{};
    for (;;) {
        const auto state = state_probe ? state_probe(state_context) : RgbDebugState::starting;
        unsigned colour = 0;
        const bool on = (tick++ % 4) < 2;
        switch (state) {
        case RgbDebugState::starting: colour = 0xFFFFFF; break;
        case RgbDebugState::setup: colour = 0x0000FF; break;
        case RgbDebugState::wifi_wait: colour = on ? 0xFFFF00 : 0; break;
        case RgbDebugState::mqtt_wait: colour = on ? 0xFF00FF : 0; break;
        case RgbDebugState::link_wait: colour = on ? 0x00FFFF : 0; break;
        case RgbDebugState::data_wait: colour = on ? 0xFF0000 : 0; break;
        case RgbDebugState::ready: colour = 0x00FF00; break;
        }
        const auto error_at = last_publish_error_ms.load();
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (state == RgbDebugState::ready && error_at != 0 &&
            now - error_at < rgb_debug_data_timeout_ms) colour = on ? 0xFF8000 : 0;
        if (colour != previous) {
            const unsigned r = ((colour >> 16) & 255) * rgb_debug_brightness / 255;
            const unsigned g = ((colour >> 8) & 255) * rgb_debug_brightness / 255;
            const unsigned b = (colour & 255) * rgb_debug_brightness / 255;
            const unsigned wire = rgb_debug_grb_order ? (g << 16) | (r << 8) | b
                                                     : (r << 16) | (g << 8) | b;
            for (unsigned bit = 0; bit < 24; ++bit) {
                const bool one = (wire & (1U << (23 - bit))) != 0;
                symbols[bit].level0 = 1;
                symbols[bit].duration0 = one ? 8 : 4;
                symbols[bit].level1 = 0;
                symbols[bit].duration1 = one ? 4 : 8;
            }
            symbols[24].level0 = symbols[24].level1 = 0;
            symbols[24].duration0 = symbols[24].duration1 = 1500; // 300 us latch
            rmt_transmit_config_t tx{};
            esp_err_t result = rmt_transmit(channel, encoder, symbols, sizeof(symbols), &tx);
            if (result == ESP_OK) result = rmt_tx_wait_all_done(channel, 100);
            if (result != ESP_OK) {
                ESP_LOGW(tag, "LED output stopped: %s", esp_err_to_name(result));
                // Suspend without releasing a possibly still-in-use TX buffer.
                vTaskSuspend(nullptr);
            }
            previous = colour;
        }
        vTaskDelay(pdMS_TO_TICKS(rgb_debug_tick_ms));
    }
}
}

void rgb_debug_publish_error()
{
    last_publish_error_ms.store(static_cast<std::uint32_t>(esp_timer_get_time() / 1000));
}

void start_rgb_debug(RgbDebugProbe probe, void *context)
{
    if (!rgb_debug_enabled || channel != nullptr) return;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(rgb_debug_gpio)) {
        ESP_LOGW(tag, "Invalid RGB LED GPIO");
        return;
    }
    rmt_tx_channel_config_t cfg{};
    cfg.gpio_num = rgb_debug_gpio;
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = 10000000;
    cfg.mem_block_symbols = 64;
    cfg.trans_queue_depth = 1;
    esp_err_t result = rmt_new_tx_channel(&cfg, &channel);
    rmt_copy_encoder_config_t copy{};
    if (result == ESP_OK) result = rmt_new_copy_encoder(&copy, &encoder);
    bool enabled = false;
    if (result == ESP_OK) {
        result = rmt_enable(channel);
        enabled = result == ESP_OK;
    }
    state_probe = probe;
    state_context = context;
    if (result == ESP_OK && xTaskCreate(led_task, "rgb_debug", 3072, nullptr, 1, nullptr) == pdPASS) return;
    ESP_LOGW(tag, "RGB debug unavailable; gateway continues normally");
    if (enabled) rmt_disable(channel);
    if (encoder) rmt_del_encoder(encoder);
    if (channel) rmt_del_channel(channel);
    encoder = nullptr;
    channel = nullptr;
}
}
