#pragma once

#include "driver/gpio.h"

namespace inverter_gateway::transport {

inline bool inverter_uart_pin_allowed(int pin, bool output)
{
    // ESP32-S3 board resources already used by this gateway: console, native
    // USB, coordinator UART, setup button, RGB LED and octal flash/PSRAM.
    return pin >= 1 && GPIO_IS_VALID_GPIO(pin) &&
        (!output || GPIO_IS_VALID_OUTPUT_GPIO(pin)) &&
        pin != 17 && pin != 18 && pin != 19 && pin != 20 &&
        pin != 43 && pin != 44 && pin != 48 && !(pin >= 26 && pin <= 37);
}

} // namespace inverter_gateway::transport
