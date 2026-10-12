#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
struct esp_now_recv_info_t {const std::uint8_t *src_addr;};
struct esp_now_peer_info_t {std::uint8_t peer_addr[6];std::uint8_t channel;int ifidx;bool encrypt;};
using esp_now_recv_cb_t=void (*)(const esp_now_recv_info_t *,const std::uint8_t *,int);
bool esp_now_is_peer_exist(const std::uint8_t *);
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *);
esp_err_t esp_now_init();
esp_err_t esp_now_deinit();
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t);
esp_err_t esp_now_unregister_recv_cb();
esp_err_t esp_now_send(const std::uint8_t *,const std::uint8_t *,std::size_t);
