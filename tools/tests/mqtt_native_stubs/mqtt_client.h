#pragma once
#include <cstdint>
using esp_event_base_t = const char *;
using esp_mqtt_client_handle_t = void *;
enum {
    MQTT_EVENT_CONNECTED = 1, MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_ERROR, MQTT_EVENT_DATA,
};
struct esp_mqtt_error_codes_t {
    int error_type{};
    int esp_tls_last_esp_err{};
    int esp_tls_stack_err{};
    int esp_transport_sock_errno{};
};
struct esp_mqtt_event_t {
    int event_id{};
    bool retain{};
    int total_data_len{};
    int current_data_offset{};
    int data_len{};
    int topic_len{};
    char *topic{};
    char *data{};
    esp_mqtt_error_codes_t *error_handle{};
};
using esp_mqtt_event_handle_t = esp_mqtt_event_t *;
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *, const char *, int, int, int);
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *, const char *, int, int, int, bool);
