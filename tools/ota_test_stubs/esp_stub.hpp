#pragma once
// OFFLINE HOST TESTS ONLY. Never add this directory to the ESP-IDF build.
#ifdef ESP_PLATFORM
#error Offline OTA test stubs must never be linked into firmware
#endif
#include <cstddef>
#include <cstdint>

using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
inline constexpr esp_err_t ESP_ERR_NO_MEM = 0x101, ESP_ERR_INVALID_ARG = 0x102;
inline constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_INVALID_SIZE = 0x104;
inline constexpr esp_err_t ESP_ERR_INVALID_CRC = 0x109, ESP_ERR_TIMEOUT = 0x107;
inline constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
using TickType_t = std::uint32_t;
using BaseType_t = int;
using portMUX_TYPE = int;
inline constexpr int portMUX_INITIALIZER_UNLOCKED = 0, pdPASS = 1;
inline constexpr unsigned MALLOC_CAP_8BIT = 1, MALLOC_CAP_INTERNAL = 2;
inline constexpr unsigned MALLOC_CAP_SPIRAM = 4;
inline constexpr std::uint16_t CONFIG_IDF_FIRMWARE_CHIP_ID = 9;
inline constexpr std::uint8_t ESP_IMAGE_HEADER_MAGIC = 0xe9;
inline constexpr std::uint32_t ESP_APP_DESC_MAGIC_WORD = 0xabcd5432;
inline constexpr std::size_t OTA_WITH_SEQUENTIAL_WRITES = static_cast<std::size_t>(-2);
using gpio_num_t = int;
inline constexpr gpio_num_t GPIO_NUM_0 = 0, GPIO_NUM_17 = 17, GPIO_NUM_18 = 18, GPIO_NUM_48 = 48;
#define pdMS_TO_TICKS(value) (value)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
void test_error_check(esp_err_t);
#define ESP_ERROR_CHECK(expression) test_error_check(expression)
void test_log(const char *, const char *, ...);
#define ESP_LOGI(tag, ...) test_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) test_log(tag, __VA_ARGS__)

// Layouts used by the actual worker, checked against ESP-IDF 5.5.1 headers.
#pragma pack(push, 1)
struct esp_image_header_t {
    std::uint8_t magic, segment_count, spi_mode, spi_speed_size;
    std::uint32_t entry_addr;
    std::uint8_t wp_pin, spi_pin_drv[3];
    std::uint16_t chip_id;
    std::uint8_t min_chip_rev;
    std::uint16_t min_chip_rev_full, max_chip_rev_full;
    std::uint8_t reserved[4], hash_appended;
};
#pragma pack(pop)
struct esp_image_segment_header_t { std::uint32_t load_addr, data_len; };
struct esp_app_desc_t {
    std::uint32_t magic_word, secure_version, reserv1[2];
    char version[32], project_name[32], time[16], date[16], idf_ver[32];
    std::uint8_t app_elf_sha256[32];
    std::uint16_t min_efuse_blk_rev_full, max_efuse_blk_rev_full;
    std::uint8_t mmu_page_size, reserv3[3];
    std::uint32_t reserv2[18];
};
static_assert(sizeof(esp_image_header_t) == 24);
static_assert(sizeof(esp_image_segment_header_t) == 8);
static_assert(sizeof(esp_app_desc_t) == 256);
static_assert(offsetof(esp_app_desc_t, version) == 16);
static_assert(offsetof(esp_image_header_t, chip_id) == 12);
struct esp_partition_t { std::uint32_t size; };
using esp_ota_handle_t = std::uint32_t;
enum esp_ota_img_states_t { ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY };
using esp_http_client_handle_t = void *;
struct esp_http_client_config_t {
    const char *url = nullptr;
    esp_err_t (*crt_bundle_attach)(void *) = nullptr;
    int timeout_ms = 0, buffer_size = 0, buffer_size_tx = 0;
    bool disable_auto_redirect = false;
};
using esp_timer_handle_t = void *;
struct esp_timer_create_args_t {
    void (*callback)(void *) = nullptr;
    const char *name = nullptr;
};
using nvs_handle_t = std::uint32_t;
inline constexpr int NVS_READWRITE = 1;

const esp_app_desc_t *esp_app_get_description();
esp_err_t esp_crt_bundle_attach(void *);
std::size_t heap_caps_get_free_size(unsigned);
std::size_t heap_caps_get_largest_free_block(unsigned);
void *heap_caps_malloc(std::size_t, unsigned);
void heap_caps_free(void *);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
std::int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
const esp_partition_t *esp_ota_get_running_partition();
esp_err_t esp_ota_begin(const esp_partition_t *, std::size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, std::size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
std::uint32_t esp_random();
[[noreturn]] void esp_restart();
const char *esp_err_to_name(esp_err_t);
std::int64_t esp_timer_get_time();
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_start_once(esp_timer_handle_t, std::uint64_t);
esp_err_t esp_timer_stop(esp_timer_handle_t);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
BaseType_t xTaskCreate(void (*)(void *), const char *, std::uint32_t, void *, unsigned, void *);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, std::size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
