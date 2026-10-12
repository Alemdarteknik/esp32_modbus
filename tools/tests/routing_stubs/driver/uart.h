#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
using uart_port_t=int;
struct uart_config_t {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;};
inline constexpr int UART_DATA_8_BITS=8,UART_PARITY_DISABLE=0,UART_STOP_BITS_1=1,
    UART_HW_FLOWCTRL_DISABLE=0,UART_SCLK_DEFAULT=0,UART_PIN_NO_CHANGE=-1;
esp_err_t uart_param_config(uart_port_t,const uart_config_t *);
esp_err_t uart_set_pin(uart_port_t,int,int,int,int);
esp_err_t uart_driver_install(uart_port_t,int,int,int,void *,int);
esp_err_t uart_driver_delete(uart_port_t);
int uart_tx_chars(uart_port_t,const char *,std::uint32_t);
esp_err_t uart_wait_tx_done(uart_port_t,TickType_t);
int uart_read_bytes(uart_port_t,void *,std::uint32_t,TickType_t);
