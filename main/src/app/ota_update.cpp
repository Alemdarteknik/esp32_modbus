#include "inverter_gateway/app/ota_update.hpp"
#include "inverter_gateway/app/memory_budget.hpp"
#include "inverter_gateway/app/project_config.hpp"
#include <array>
#include <cstring>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace inverter_gateway::app {
namespace {
constexpr char tag[] = "ota_update";
OtaHooks callbacks{};
esp_timer_handle_t trial_timer = nullptr;
bool trial = false;
portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
OtaStatus status{ota_enabled ? "not_checked" : "disabled"};
struct MemorySnapshot {
    std::uint32_t free_bytes;
    std::uint32_t largest_block_bytes;
};
struct MemoryBudget {
    std::uint32_t minimum_free_bytes;
    std::uint32_t minimum_largest_block_bytes;
};
constexpr MemoryBudget connection_memory_budget{
    ota_minimum_free_heap, ota_minimum_largest_block};
constexpr MemoryBudget streaming_memory_budget{
    ota_stream_minimum_free_heap, ota_stream_minimum_largest_block};
MemorySnapshot read_internal_memory()
{
    // TLS uses internal RAM in this build; PSRAM cannot satisfy these guards.
    constexpr unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    return {static_cast<std::uint32_t>(heap_caps_get_free_size(caps)),
            static_cast<std::uint32_t>(heap_caps_get_largest_free_block(caps))};
}
bool memory_ready(const MemoryBudget &budget, MemorySnapshot &snapshot)
{
    snapshot = read_internal_memory();
    return snapshot.free_bytes >= budget.minimum_free_bytes &&
        snapshot.largest_block_bytes >= budget.minimum_largest_block_bytes;
}
void set_status(const char *state, const char *available = nullptr, bool checking = false)
{
    const auto time = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&status_lock);
    status.state = state;
    if (available) {
        std::strncpy(status.available_version, available, sizeof(status.available_version) - 1);
        status.available_version[31] = '\0';
    }
    if (checking) {
        status.last_check_ms = time;
        status.failure_stage = "";
        status.failure_code = 0;
        status.failure_http_status = 0;
        status.failure_internal_free_bytes = 0;
        status.failure_internal_largest_block_bytes = 0;
        status.failure_required_free_bytes = 0;
        status.failure_required_largest_block_bytes = 0;
    }
    portEXIT_CRITICAL(&status_lock);
}
void record_failure(const char *stage, esp_err_t error, int http_status,
                    const MemorySnapshot &memory, const MemoryBudget &budget)
{
    // Stage strings are static literals; publishing diagnostics never changes
    // admission, verification, retry scheduling, or boot selection.
    portENTER_CRITICAL(&status_lock);
    status.state = "check_failed";
    status.failure_stage = stage;
    status.failure_code = error;
    status.failure_http_status = http_status;
    status.failure_internal_free_bytes = memory.free_bytes;
    status.failure_internal_largest_block_bytes = memory.largest_block_bytes;
    status.failure_required_free_bytes = budget.minimum_free_bytes;
    status.failure_required_largest_block_bytes = budget.minimum_largest_block_bytes;
    portEXIT_CRITICAL(&status_lock);
    ESP_LOGW(tag, "Update not activated: stage=%s error=%s HTTP=%d "
             "internal_free=%u largest=%u required_free=%u required_largest=%u",
             stage, esp_err_to_name(error), http_status,
             static_cast<unsigned>(memory.free_bytes),
             static_cast<unsigned>(memory.largest_block_bytes),
             static_cast<unsigned>(budget.minimum_free_bytes),
             static_cast<unsigned>(budget.minimum_largest_block_bytes));
}
std::int64_t now_ms() { return esp_timer_get_time() / 1000; }
void delay(std::uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

bool version(const char *text, std::array<unsigned, 3> &parts)
{
    const auto length = strnlen(text, 32);
    if (length == 0 || length == 32) return false;
    unsigned field = 0, digits = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '.') {
            if (!digits || field == 2) return false;
            ++field; digits = 0;
        } else {
            if (c < '0' || c > '9' || ++digits > 5) return false;
            parts[field] = parts[field] * 10 + static_cast<unsigned>(c - '0');
        }
    }
    return field == 2 && digits != 0;
}

enum class CheckOutcome { local_deferred, attempted };

CheckOutcome check_release()
{
    // Only local, network-free deferrals use a short retry. Once we contact
    // the server, keep the fleet-wide interval even if that request fails.
    if (callbacks.download_blocker(callbacks.context) != nullptr) {
        set_status("deferred"); return CheckOutcome::local_deferred;
    }
    MemoryWorkGuard memory_work(pdMS_TO_TICKS(500));
    MemorySnapshot memory{};
    if (!memory_work || !memory_ready(connection_memory_budget, memory)) {
        set_status("deferred"); return CheckOutcome::local_deferred;
    }
    set_status("checking", "", true);
    nvs_handle_t store;
    const auto store_result = nvs_open("ota_state", NVS_READWRITE, &store);
    if (store_result != ESP_OK) {
        record_failure("nvs_open", store_result, 0, read_internal_memory(), connection_memory_budget);
        return CheckOutcome::attempted;
    }
    esp_http_client_config_t http{};
    http.url = ota_download_url;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 5000;
    http.buffer_size = 4096;
    http.disable_auto_redirect = true;
    auto client = esp_http_client_init(&http);
    auto *buffer = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_8BIT));
    esp_ota_handle_t handle = 0;
    bool writing = false;
    bool parked = false;
    bool activate = false;
    auto result = ESP_FAIL;
    const char *failure_stage = "allocation";
    int http_status = 0;
    const MemoryBudget *active_memory_budget = &connection_memory_budget;
    bool memory_guard_failed = false;
    const auto streaming_memory_ready = [&] {
        active_memory_budget = &streaming_memory_budget;
        memory_guard_failed = !memory_ready(*active_memory_budget, memory);
        return !memory_guard_failed;
    };
    const auto started = now_ms();
    do {
        if (!client || !buffer) { result = ESP_ERR_NO_MEM; break; }
        // Streaming API: exactly one GET, accept only 200, never follow
        // redirects or resume Range requests against a mutable latest file.
        failure_stage = "https_open";
        result = esp_http_client_open(client, 0);
        if (result != ESP_OK) break;
        failure_stage = "post_tls_memory";
        // TLS record buffers already exist. Only require the running reserve.
        if (!streaming_memory_ready()) { result = ESP_ERR_NO_MEM; break; }
        // Do not hold the gate across the download: polling and live
        // publishing must continue between the short flash writes.
        memory_work.release();
        failure_stage = "response_headers";
        const auto size = esp_http_client_fetch_headers(client);
        http_status = esp_http_client_get_status_code(client);
        const auto *partition = esp_ota_get_next_update_partition(nullptr);
        constexpr std::size_t descriptor_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
        constexpr std::size_t prefix_size = descriptor_offset + sizeof(esp_app_desc_t);
        const bool known_size = size > 0;
        // IDF returns zero for streaming responses without Content-Length.
        // Bound the decoded body while reading instead of rejecting it here.
        if (http_status != 200 || !partition ||
            size < 0 || (!known_size && !esp_http_client_is_chunked_response(client)) ||
            partition->size < prefix_size ||
            (known_size && (size < static_cast<std::int64_t>(prefix_size) || size > partition->size))) {
            result = ESP_ERR_INVALID_SIZE; break;
        }
        // Header parsing may allocate header strings and cached decoded body.
        failure_stage = "post_headers_memory";
        if (!streaming_memory_ready()) { result = ESP_ERR_NO_MEM; break; }
        failure_stage = "image_prefix";
        std::size_t prefix_read = 0;
        while (prefix_read < prefix_size) {
            if (now_ms() - started > ota_download_timeout_ms) break;
            const int count = esp_http_client_read(client, buffer + prefix_read, prefix_size - prefix_read);
            if (count <= 0) break;
            prefix_read += count;
        }
        if (prefix_read != prefix_size) { result = ESP_FAIL; break; }
        esp_image_header_t header{};
        esp_app_desc_t incoming{};
        std::memcpy(&header, buffer, sizeof(header));
        std::memcpy(&incoming, buffer + descriptor_offset, sizeof(incoming));
        const auto *running = esp_app_get_description();
        std::array<unsigned, 3> next{}, current{};
        char attempted[32]{};
        std::size_t attempted_size = sizeof(attempted);
        const auto remembered = nvs_get_str(store, "attempted", attempted, &attempted_size);
        if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
            incoming.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            std::strncmp(incoming.project_name, running->project_name, sizeof(incoming.project_name)) != 0 ||
            !version(incoming.version, next) || !version(running->version, current)) {
            ESP_LOGI(tag, "No eligible release (version/project/chip/previous attempt)");
            set_status("incompatible");
            result = ESP_OK; break;
        }
        if (next == current) { set_status("up_to_date"); result = ESP_OK; break; }
        if (next < current) { set_status("server_older"); result = ESP_OK; break; }
        if (remembered == ESP_OK && std::strcmp(attempted, incoming.version) == 0) {
            set_status("release_blocked", incoming.version); result = ESP_OK; break;
        }
        set_status("update_available", incoming.version);
        // Normal IDF images include SHA-256. This is integrity, NOT signing.
        failure_stage = "image_hash";
        if (!header.hash_appended) { result = ESP_ERR_INVALID_CRC; break; }
        failure_stage = "write_admission";
        if (callbacks.download_blocker(callbacks.context) != nullptr) {
            result = ESP_ERR_INVALID_STATE; break;
        }
        failure_stage = "pre_write_memory";
        if (!streaming_memory_ready()) { result = ESP_ERR_NO_MEM; break; }
        ESP_LOGW(tag, "Downloading release %.31s", incoming.version);
        set_status("downloading");
        failure_stage = "ota_begin";
        result = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
        if (result != ESP_OK) break;
        writing = true;
        failure_stage = "prefix_write";
        result = esp_ota_write(handle, buffer, prefix_size);
        if (result != ESP_OK) break;
        std::int64_t received = prefix_size;
        while (true) {
            failure_stage = "download_timeout";
            if (now_ms() - started > ota_download_timeout_ms) {
                result = ESP_ERR_TIMEOUT; break;
            }
            failure_stage = "download_memory";
            if (!streaming_memory_ready()) { result = ESP_ERR_NO_MEM; break; }
            if (callbacks.download_blocker(callbacks.context) != nullptr) { delay(250); continue; }
            // Parser completion can precede draining cached decoded bytes.
            // Read through EOF, even at exact slot capacity; never read len=0.
            failure_stage = "body_read";
            const int count = esp_http_client_read(client, buffer, 4096);
            if (count < 0) { result = ESP_FAIL; break; }
            if (count == 0) {
                failure_stage = "body_completion";
                if (!esp_http_client_is_complete_data_received(client) ||
                    (known_size && received != size)) result = ESP_ERR_INVALID_SIZE;
                break;
            }
            const auto capacity = static_cast<std::int64_t>(partition->size);
            failure_stage = "image_size";
            if (received > capacity || count > capacity - received ||
                (known_size && (received > size || count > size - received))) {
                result = ESP_ERR_INVALID_SIZE; break;
            }
            failure_stage = "post_read_memory";
            if (!streaming_memory_ready()) { result = ESP_ERR_NO_MEM; break; }
            failure_stage = "body_write";
            result = esp_ota_write(handle, buffer, count);
            if (result != ESP_OK) break;
            received += count;
            delay(50);
        }
        if (result != ESP_OK) break;
        failure_stage = "verification_memory";
        if (!streaming_memory_ready()) { result = ESP_ERR_NO_MEM; break; }
        failure_stage = "image_verification";
        result = esp_ota_end(handle); // Verify image BEFORE selecting boot slot.
        writing = false;
        if (result != ESP_OK) break;
        set_status("waiting_to_restart");
        failure_stage = "restart_admission";
        parked = callbacks.quiesce(callbacks.context);
        if (!parked) { result = ESP_ERR_INVALID_STATE; break; }
        // A power cut here can conservatively skip this version next time.
        // A corrected/retried release must have a NEW version number.
        failure_stage = "release_record";
        result = nvs_set_str(store, "attempted", incoming.version);
        if (result == ESP_OK) result = nvs_commit(store);
        if (result != ESP_OK) break;
        failure_stage = "boot_selection";
        result = esp_ota_set_boot_partition(partition);
        activate = result == ESP_OK;
    } while (false);
    if (result != ESP_OK) {
        // Retain the exact rejected sample. For non-budget errors, sample now,
        // before any cleanup returns TLS/HTTP/OTA allocations to the heap.
        if (!memory_guard_failed) memory = read_internal_memory();
        record_failure(failure_stage, result, http_status, memory, *active_memory_budget);
    }
    if (writing) esp_ota_abort(handle);
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    heap_caps_free(buffer);
    nvs_close(store);
    if (activate) {
        set_status("restarting");
        ESP_LOGW(tag, "OTA verified; restarting into trial boot");
        esp_restart();
    }
    if (parked) callbacks.resume(callbacks.context);
    return CheckOutcome::attempted;
}

void worker(void *)
{
    auto healthy_since = now_ms();
    if (trial) set_status("verifying");
    while (trial) {
        if (!callbacks.healthy(callbacks.context)) healthy_since = now_ms();
        if (now_ms() - healthy_since >= 60000) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                esp_timer_stop(trial_timer);
                trial = false;
                ESP_LOGW(tag, "OTA boot confirmed after local health trial");
            }
        }
        delay(1000);
    }
    if (!ota_enabled) { set_status("disabled"); vTaskDelete(nullptr); return; }
    set_status("not_checked");
    delay(60000 + esp_random() % ota_jitter_ms);
    while (true) {
        const auto outcome = check_release();
        if (outcome == CheckOutcome::local_deferred) {
            constexpr std::uint32_t local_retry_ms = 5 * 60 * 1000;
            delay(local_retry_ms + esp_random() % local_retry_ms);
        } else {
            delay(ota_check_interval_ms + esp_random() % ota_jitter_ms);
        }
    }
}
}

void arm_ota_boot_trial()
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) return;
    trial = true;
    set_status("verifying");
    esp_timer_create_args_t args{};
    args.callback = [](void *) { esp_restart(); };
    args.name = "ota_trial";
    ESP_ERROR_CHECK(esp_timer_create(&args, &trial_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(trial_timer, 180000000));
}

void start_ota_update(OtaHooks hooks)
{
    // Publish the callbacks coherently for the read-only status endpoint.
    portENTER_CRITICAL(&status_lock);
    callbacks = hooks;
    portEXIT_CRITICAL(&status_lock);
    if (xTaskCreate(worker, "ota_update", 8192, nullptr, 1, nullptr) != pdPASS) {
        set_status("task_failed");
        ESP_LOGE(tag, "OTA task allocation failed; automatic updates not running");
    }
}
OtaStatus ota_status()
{
    portENTER_CRITICAL(&status_lock);
    auto snapshot = status;
    const auto status_callbacks = callbacks;
    portEXIT_CRITICAL(&status_lock);
    // Current diagnostics only: do not change the updater state, schedule,
    // admission rules, download, flash writes, activation or rollback.
    constexpr unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    snapshot.internal_free_bytes = static_cast<std::uint32_t>(heap_caps_get_free_size(caps));
    snapshot.internal_largest_block_bytes = static_cast<std::uint32_t>(heap_caps_get_largest_free_block(caps));
    if (std::strcmp(snapshot.state, "deferred") == 0) {
        const char *blocker = status_callbacks.download_blocker != nullptr
            ? status_callbacks.download_blocker(status_callbacks.context) : "updater_starting";
        if (blocker != nullptr) snapshot.reason = blocker;
        else if (snapshot.internal_free_bytes < ota_minimum_free_heap)
            snapshot.reason = "low_internal_heap";
        else if (snapshot.internal_largest_block_bytes < ota_minimum_largest_block)
            snapshot.reason = "internal_heap_fragmented";
        else snapshot.reason = "awaiting_scheduled_retry";
    }
    return snapshot;
}
}
