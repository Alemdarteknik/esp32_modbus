#include "inverter_gateway/transport/uart_inverter.hpp"

#include "byte_transport_detail.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "inverter_gateway/transport/uart_pin_policy.hpp"

namespace inverter_gateway::transport {
namespace {
constexpr auto port = UART_NUM_2;
constexpr char tag[] = "inverter_uart";
detail::ReceivePipe receive_pipe;
QueueHandle_t uart_events = nullptr;
SemaphoreHandle_t transaction_mutex = nullptr;
SemaphoreHandle_t receive_mutex = nullptr;
UartInverterConfig settings;
std::atomic<bool> installed{false};

void receive_task(void *)
{
    std::uint8_t bytes[128];
    while (true) {
        uart_event_t event{};
        const bool have_event = xQueueReceive(uart_events, &event, pdMS_TO_TICKS(10)) == pdTRUE;
        xSemaphoreTake(receive_mutex, portMAX_DELAY);
        if (uxQueueMessagesWaiting(uart_events) >= 32) receive_pipe.invalidate();
        if (have_event) {
            switch (event.type) {
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
            case UART_FRAME_ERR:
            case UART_PARITY_ERR:
            case UART_BREAK:
                receive_pipe.invalidate();
                uart_flush_input(port);
                break;
            default:
                break;
            }
            // Process all queued errors before admitting subsequent data.
            for (int i = 0; i < 32 && xQueueReceive(uart_events, &event, 0) == pdTRUE; ++i) {
                if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                    event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR ||
                    event.type == UART_BREAK) {
                    receive_pipe.invalidate();
                    uart_flush_input(port);
                }
            }
        }
        std::size_t pending = 0;
        if (uart_get_buffered_data_len(port, &pending) != ESP_OK) {
            receive_pipe.invalidate();
            xSemaphoreGive(receive_mutex);
            vTaskDelay(1);
            continue;
        }
        // Bounded read prevents a noisy sender monopolizing the worker.
        pending = std::min<std::size_t>(pending, 2048);
        while (pending != 0) {
            const int n = uart_read_bytes(port, bytes, std::min(pending, sizeof(bytes)), 0);
            if (n <= 0) break;
            receive_pipe.append(bytes, static_cast<std::size_t>(n));
            pending -= n;
        }
        xSemaphoreGive(receive_mutex);
        // This worker has a higher priority than the bus task. Yield while the
        // mutex is released, so transaction preparation cannot starve on noise.
        vTaskDelay(1);
    }
}

void cleanup()
{
    if (uart_is_driver_installed(port)) uart_driver_delete(port);
    if (receive_pipe.stream != nullptr) vStreamBufferDelete(receive_pipe.stream);
    if (transaction_mutex != nullptr) vSemaphoreDelete(transaction_mutex);
    if (receive_mutex != nullptr) vSemaphoreDelete(receive_mutex);
    receive_pipe.stream = nullptr;
    transaction_mutex = nullptr;
    receive_mutex = nullptr;
    uart_events = nullptr;
}
} // namespace

esp_err_t UartInverter::install(const UartInverterConfig &config)
{
    if (installed.load() || uart_is_driver_installed(port)) return ESP_ERR_INVALID_STATE;
    if (config.baud < 1200 || config.baud > 115200 ||
        !inverter_uart_pin_allowed(config.tx_pin, true) ||
        !inverter_uart_pin_allowed(config.rx_pin, false) || config.tx_pin == config.rx_pin ||
        (config.direction_pin != -1 && (!inverter_uart_pin_allowed(config.direction_pin, true) ||
          config.direction_pin == config.tx_pin || config.direction_pin == config.rx_pin)) ||
        (config.parity != UART_PARITY_DISABLE && config.parity != UART_PARITY_EVEN &&
         config.parity != UART_PARITY_ODD)) return ESP_ERR_INVALID_ARG;

    receive_pipe.stream = xStreamBufferCreate(2048, 1);
    transaction_mutex = xSemaphoreCreateMutex();
    receive_mutex = xSemaphoreCreateMutex();
    if (receive_pipe.stream == nullptr || transaction_mutex == nullptr || receive_mutex == nullptr) {
        cleanup();
        return ESP_ERR_NO_MEM;
    }
    uart_config_t uart_config{};
    uart_config.baud_rate = static_cast<int>(config.baud);
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = config.parity;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_param_config(port, &uart_config);
    if (err == ESP_OK) err = uart_set_pin(port, config.tx_pin, config.rx_pin,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // No TX ring: uart_tx_chars provides bounded nonblocking FIFO admission.
    if (err == ESP_OK) err = uart_driver_install(port, 2048, 0, 32, &uart_events, 0);
    if (err == ESP_OK && config.direction_pin >= 0) {
        err = gpio_set_direction(static_cast<gpio_num_t>(config.direction_pin), GPIO_MODE_OUTPUT);
        if (err == ESP_OK) err = gpio_set_level(static_cast<gpio_num_t>(config.direction_pin), 0);
    }
    if (err != ESP_OK) {
        cleanup();
        return err;
    }
    settings = config;
    if (xTaskCreate(receive_task, "inverter_uart_rx", 3072, nullptr, 9, nullptr) != pdPASS) {
        cleanup();
        return ESP_ERR_NO_MEM;
    }
    installed.store(true, std::memory_order_release);
    ESP_LOGI(tag, "UART2 ready TX=%d RX=%d direction=%d baud=%lu parity=%d",
        config.tx_pin, config.rx_pin, config.direction_pin,
        static_cast<unsigned long>(config.baud), static_cast<int>(config.parity));
    return ESP_OK;
}

bool UartInverter::connected() const
{
    // UART has no cable-presence indication; protocol health proves an inverter.
    return installed.load(std::memory_order_acquire);
}

std::uint32_t UartInverter::session_id() const { return connected() ? 1U : 0U; }

esp_err_t UartInverter::exchange(const std::uint8_t *command, std::size_t command_size,
    std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
    ReceiveRule rule, bool &transmitted)
{
    const auto capacity = response_size;
    response_size = 0;
    transmitted = false;
    if (!detail::valid_exchange(command, command_size, response, capacity, timeout_ms, rule))
        return ESP_ERR_INVALID_ARG;
    if (!connected()) return ESP_ERR_INVALID_STATE;
    const detail::Deadline deadline(timeout_ms);
    if (!detail::take_until(transaction_mutex, deadline)) return ESP_ERR_TIMEOUT;
    std::uint32_t snapshot = 0;
    esp_err_t err = connected() && (rule.expected_session == 0 || rule.expected_session == session_id())
        ? ESP_OK : ESP_ERR_INVALID_STATE;
    // A timed-out prior TX may still be shifting out of the hardware FIFO.
    if (err == ESP_OK && uart_wait_tx_done(port, 0) != ESP_OK) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        if (!detail::take_until(receive_mutex, deadline)) err = ESP_ERR_TIMEOUT;
        else {
            err = uart_flush_input(port);
            if (err == ESP_OK && !receive_pipe.prepare(snapshot)) err = ESP_ERR_INVALID_RESPONSE;
            xSemaphoreGive(receive_mutex);
        }
    }
    if (err == ESP_OK && command_size != 0) {
        if (settings.direction_pin >= 0)
            err = gpio_set_level(static_cast<gpio_num_t>(settings.direction_pin), 1);
        std::size_t sent = 0;
        while (err == ESP_OK && sent < command_size && !deadline.expired()) {
            const int n = uart_tx_chars(port, reinterpret_cast<const char *>(command + sent),
                static_cast<std::uint32_t>(command_size - sent));
            if (n < 0) { err = ESP_FAIL; break; }
            if (n > 0) { transmitted = true; sent += n; }
            else vTaskDelay(1);
            if (!receive_pipe.valid(snapshot)) err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK && sent != command_size) err = ESP_ERR_TIMEOUT;
        while (err == ESP_OK && !deadline.expired()) {
            err = uart_wait_tx_done(port, deadline.ticks());
            if (err != ESP_ERR_TIMEOUT) break;
            err = ESP_OK;
        }
        if (err == ESP_OK && deadline.expired()) err = ESP_ERR_TIMEOUT;
        if (settings.direction_pin >= 0)
            gpio_set_level(static_cast<gpio_num_t>(settings.direction_pin), 0);
    }
    if (err == ESP_OK) err = detail::receive(receive_pipe, snapshot, response, capacity,
        response_size, deadline, rule, [this] { return connected(); });
    xSemaphoreGive(transaction_mutex);
    return err;
}

} // namespace inverter_gateway::transport
