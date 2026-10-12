#pragma once
using gpio_num_t = int;
constexpr int GPIO_NUM_0=0, GPIO_NUM_17=17, GPIO_NUM_18=18, GPIO_NUM_48=48;
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) <= 48 && !((pin) >= 22 && (pin) <= 25))
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) (GPIO_IS_VALID_GPIO(pin) && (pin) != 46)
