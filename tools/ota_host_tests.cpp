// Executes the REAL OTA worker source against offline fake ESP-IDF services.
// Build ONLY with tools/ota_test_stubs first on the host include path.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "esp_stub.hpp"
#include "inverter_gateway/app/project_config.hpp"
#include "inverter_gateway/app/memory_budget.hpp"

namespace fake {
struct Restart {};
struct StopWorker {};
// Heap samples are selected by SDK lifecycle phase, never heap-query count.
constexpr std::size_t prefix_size = sizeof(esp_image_header_t) +
    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
enum class HeapPhase : std::size_t {
    before_open, tls_open, headers, prefix_read, prefix_written,
    body_read, body_written, body_eof, cleaned, count
};
struct HeapSample {
    std::size_t free_bytes = 256000, largest_block = 128000;
};
struct State {
    std::vector<std::string> events;
    std::vector<TickType_t> delays;
    std::vector<unsigned> allocation_caps;
    std::vector<unsigned char> body, flash_image;
    esp_app_desc_t running{};
    esp_partition_t partition{3U * 1024U * 1024U};
    std::int64_t time_us = 1000000, content_length = 8192;
    std::size_t position = 0, read_limit = std::numeric_limits<std::size_t>::max();
    std::size_t read_chunk = std::numeric_limits<std::size_t>::max();
    // Separate active TLS and recovered cleanup memory from pre-open headroom.
    std::array<HeapSample, static_cast<std::size_t>(HeapPhase::count)> heap{};
    HeapPhase heap_phase = HeapPhase::before_open;
    unsigned reads = 0, fail_read_on = 0, writes = 0, fail_write_on = 0;
    unsigned guard_acquires = 0, guard_releases = 0, blocker_calls = 0, block_from = 0;
    unsigned begins = 0, ends = 0, aborts = 0, quiesces = 0, resumes = 0;
    unsigned boot_calls = 0, restarts = 0, opens = 0, closes = 0, cleanups = 0;
    unsigned allocations = 0, frees = 0, nvs_opens = 0, nvs_closes = 0;
    unsigned stop_delay_number = 0, read_advance_ms = 0, open_advance_ms = 0;
    unsigned completion_checks = 0;
    int http_status = 200;
    esp_err_t nvs_open_result = ESP_OK, nvs_get_result = ESP_ERR_NVS_NOT_FOUND;
    esp_err_t open_result = ESP_OK, begin_result = ESP_OK, end_result = ESP_OK;
    esp_err_t nvs_set_result = ESP_OK, commit_result = ESP_OK, boot_result = ESP_OK;
    bool fail_client_init = false, fail_buffer_alloc = false, missing_partition = false;
    bool fail_psram_alloc = false;
    bool guard_available = true, guard_held = false, client_live = false;
    bool buffer_live = false, store_open = false, ota_active = false;
    bool healthy = true, quiesce_result = true, boot_selected = false, committed = false;
    bool task_create_ok = true, http_was_live_at_quiesce = false;
    bool chunked = false, response_complete = true, eof_observed = false;
    bool completion_requires_eof = false, parser_complete_early = false;
    std::uint32_t entropy = 7;
    std::string remembered_version, written_version;
    std::function<void()> before_nvs_open;
    std::function<void()> after_heap_sample;
    std::function<void(TickType_t)> before_delay;
};
State state;
HeapSample &heap_at(HeapPhase phase)
{
    return state.heap[static_cast<std::size_t>(phase)];
}
const HeapSample &current_heap() { return heap_at(state.heap_phase); }
void stream_heap(HeapSample sample)
{
    for (auto phase = static_cast<std::size_t>(HeapPhase::tls_open);
         phase <= static_cast<std::size_t>(HeapPhase::body_eof); ++phase) {
        state.heap[phase] = sample;
    }
}
void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
void event(const char *text) { state.events.emplace_back(text); }
void set_incoming(const std::function<void(esp_image_header_t &, esp_app_desc_t &)> &change)
{
    esp_image_header_t header{};
    esp_app_desc_t desc{};
    std::memcpy(&header, state.body.data(), sizeof(header));
    std::memcpy(&desc, state.body.data() + 32, sizeof(desc));
    change(header, desc);
    std::memcpy(state.body.data(), &header, sizeof(header));
    std::memcpy(state.body.data() + 32, &desc, sizeof(desc));
}
void incoming_version(const char *value)
{
    set_incoming([&](auto &, auto &desc) {
        std::memset(desc.version, 0, sizeof(desc.version));
        std::strncpy(desc.version, value, sizeof(desc.version) - 1);
    });
}
void chunked_response()
{
    state.content_length = 0; // IDF fetch_headers return for a chunked response.
    state.chunked = true;
}
void initialize_image()
{
    state.body.resize(8192, 0x5a);
    esp_image_header_t header{};
    header.magic = ESP_IMAGE_HEADER_MAGIC;
    header.chip_id = CONFIG_IDF_FIRMWARE_CHIP_ID;
    header.hash_appended = 1;
    header.segment_count = 1;
    esp_image_segment_header_t segment{0, sizeof(esp_app_desc_t)};
    esp_app_desc_t desc{};
    desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    std::strcpy(desc.version, "1.0.4");
    std::strcpy(desc.project_name, "esp32_modbus");
    std::memcpy(state.body.data(), &header, sizeof(header));
    std::memcpy(state.body.data() + sizeof(header), &segment, sizeof(segment));
    std::memcpy(state.body.data() + 32, &desc, sizeof(desc));
    state.running = desc;
    std::strcpy(state.running.version, "1.0.3");
}
bool healthy(void *) { return state.healthy; }
const char *blocker(void *)
{
    ++state.blocker_calls;
    return state.block_from && state.blocker_calls >= state.block_from ? "write_pending" : nullptr;
}
bool quiesce(void *)
{
    event("quiesce");
    ++state.quiesces;
    state.http_was_live_at_quiesce = state.client_live;
    return state.quiesce_result;
}
void resume(void *) { event("resume"); ++state.resumes; }
void assert_clean(bool single_check = true)
{
    require(!state.client_live && !state.buffer_live && !state.store_open && !state.ota_active,
            "HTTP/buffer/NVS/OTA resources leaked");
    require(!state.guard_held && state.guard_acquires == state.guard_releases, "memory gate leaked");
    require(state.allocations == state.frees, "buffer allocations unbalanced");
    require(state.nvs_opens == state.nvs_closes, "NVS handles unbalanced");
    require(!single_check || state.opens <= 1, "mutable endpoint requested more than once in a check");
}
std::size_t index_of(const char *name)
{
    const auto it = std::find(state.events.begin(), state.events.end(), name);
    require(it != state.events.end(), std::string("missing event ") + name);
    return static_cast<std::size_t>(it - state.events.begin());
}
}

void test_error_check(esp_err_t result) { fake::require(result == ESP_OK, "ESP_ERROR_CHECK failed"); }
void test_log(const char *, const char *, ...) {}
const esp_app_desc_t *esp_app_get_description() { return &fake::state.running; }
esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
std::size_t heap_caps_get_free_size(unsigned caps)
{
    fake::require(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), "OTA must inspect internal heap");
    return fake::current_heap().free_bytes;
}
std::size_t heap_caps_get_largest_free_block(unsigned caps)
{
    fake::require(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), "OTA must inspect internal largest block");
    const auto largest = fake::current_heap().largest_block;
    if (fake::state.after_heap_sample) fake::state.after_heap_sample();
    return largest;
}
void *heap_caps_malloc(std::size_t size, unsigned caps)
{
    fake::state.allocation_caps.push_back(caps);
    if (fake::state.fail_buffer_alloc) return nullptr;
    if (fake::state.fail_psram_alloc && (caps & MALLOC_CAP_SPIRAM)) return nullptr;
    fake::require(!fake::state.buffer_live, "more than one download buffer");
    auto *p = std::malloc(size);
    fake::require(p != nullptr, "host malloc failed");
    fake::event("buffer_alloc");
    fake::state.buffer_live = true;
    ++fake::state.allocations;
    return p;
}
void heap_caps_free(void *p)
{
    if (!p) return;
    fake::require(fake::state.buffer_live, "double free");
    fake::event("buffer_free");
    fake::state.buffer_live = false;
    ++fake::state.frees;
    std::free(p);
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    fake::require(config->disable_auto_redirect, "mutable download must not follow redirects");
    fake::require(config->crt_bundle_attach != nullptr, "HTTPS trust configuration missing");
    fake::require(config->timeout_ms > 0, "HTTP timeout missing");
    fake::require(config->buffer_size == 1024 && config->buffer_size_tx == 512,
                  "OTA HTTP buffers regressed to excess internal allocations");
    fake::require(std::string(config->url).rfind("https://", 0) == 0, "non-HTTPS endpoint");
    fake::event("http_init");
    if (fake::state.fail_client_init) return nullptr;
    fake::state.client_live = true;
    return &fake::state;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t, int)
{
    fake::event("http_open");
    fake::require(fake::state.guard_held, "TLS handshake not protected by memory admission gate");
    ++fake::state.opens;
    fake::state.time_us += static_cast<std::int64_t>(fake::state.open_advance_ms) * 1000;
    fake::state.heap_phase = fake::HeapPhase::tls_open;
    return fake::state.open_result;
}
std::int64_t esp_http_client_fetch_headers(esp_http_client_handle_t)
{
    fake::require(!fake::state.guard_held, "memory gate held beyond TLS handshake");
    fake::state.heap_phase = fake::HeapPhase::headers;
    return fake::state.content_length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t) { return fake::state.http_status; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t)
{
    return fake::state.chunked;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t)
{
    auto &s = fake::state;
    ++s.completion_checks;
    // IDF's parser can complete while decoded body bytes remain buffered.
    // Other responses need a final zero-byte read to consume chunk framing.
    const auto expected = s.chunked ? s.body.size() :
        static_cast<std::size_t>(std::max<std::int64_t>(s.content_length, 0));
    return s.response_complete && (s.parser_complete_early || s.position == expected) &&
        (!s.completion_requires_eof || s.eof_observed);
}
int esp_http_client_read(esp_http_client_handle_t, char *target, int requested)
{
    fake::require(requested > 0, "zero/negative-length read cannot establish HTTP EOF");
    auto &s = fake::state;
    ++s.reads;
    s.time_us += static_cast<std::int64_t>(s.read_advance_ms) * 1000;
    if (s.reads == s.fail_read_on) return -1;
    const auto end = std::min(s.body.size(), s.read_limit);
    if (s.position >= end) {
        s.eof_observed = true;
        s.heap_phase = fake::HeapPhase::body_eof;
        return 0;
    }
    const auto count = std::min({static_cast<std::size_t>(requested), end - s.position, s.read_chunk});
    std::memcpy(target, s.body.data() + s.position, count);
    s.position += count;
    if (s.ota_active) s.heap_phase = fake::HeapPhase::body_read;
    else if (s.position >= fake::prefix_size) s.heap_phase = fake::HeapPhase::prefix_read;
    return static_cast<int>(count);
}
esp_err_t esp_http_client_close(esp_http_client_handle_t)
{
    fake::require(fake::state.client_live, "close dead client");
    fake::event("http_close");
    ++fake::state.closes;
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t)
{
    fake::require(fake::state.client_live, "double client cleanup");
    fake::event("http_cleanup");
    fake::state.client_live = false;
    fake::state.heap_phase = fake::HeapPhase::cleaned;
    ++fake::state.cleanups;
    return ESP_OK;
}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *)
{ return fake::state.missing_partition ? nullptr : &fake::state.partition; }
const esp_partition_t *esp_ota_get_running_partition() { return &fake::state.partition; }
esp_err_t esp_ota_begin(const esp_partition_t *, std::size_t size, esp_ota_handle_t *handle)
{
    fake::require(!fake::state.guard_held, "memory gate held across flash transfer");
    fake::require(size == OTA_WITH_SEQUENTIAL_WRITES, "expected sequential OTA writes");
    fake::event("ota_begin");
    ++fake::state.begins;
    if (fake::state.begin_result != ESP_OK) return fake::state.begin_result;
    fake::state.ota_active = true;
    *handle = 1;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void *bytes, std::size_t length)
{
    fake::require(fake::state.ota_active, "write without OTA handle");
    fake::require(fake::state.flash_image.size() <= fake::state.partition.size &&
                  length <= fake::state.partition.size - fake::state.flash_image.size(),
                  "worker attempted a write past the inactive OTA partition");
    fake::event("ota_write");
    ++fake::state.writes;
    if (fake::state.writes == fake::state.fail_write_on) return ESP_FAIL;
    const auto *first = static_cast<const unsigned char *>(bytes);
    fake::state.flash_image.insert(fake::state.flash_image.end(), first, first + length);
    fake::state.heap_phase = fake::state.writes == 1 ?
        fake::HeapPhase::prefix_written : fake::HeapPhase::body_written;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t)
{
    fake::require(fake::state.ota_active, "end without OTA handle");
    fake::require(fake::state.eof_observed && fake::state.completion_checks != 0,
                  "image verification started without draining EOF and checking HTTP completion");
    fake::event("ota_end");
    ++fake::state.ends;
    fake::state.ota_active = false; // IDF end consumes the handle even on validation failure.
    return fake::state.end_result;
}
esp_err_t esp_ota_abort(esp_ota_handle_t)
{
    fake::require(fake::state.ota_active, "abort inactive OTA handle");
    fake::event("ota_abort");
    ++fake::state.aborts;
    fake::state.ota_active = false;
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *)
{
    fake::event("set_boot");
    ++fake::state.boot_calls;
    fake::require(fake::state.ends == 1 && fake::state.end_result == ESP_OK,
                  "activation attempted before successful image validation");
    fake::require(fake::state.committed, "activation attempted before recording previous attempt");
    if (fake::state.boot_result == ESP_OK) fake::state.boot_selected = true;
    return fake::state.boot_result;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *state)
{ *state = ESP_OTA_IMG_VALID; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback() { return ESP_OK; }
std::uint32_t esp_random() { return fake::state.entropy; }
[[noreturn]] void esp_restart()
{
    fake::event("restart");
    ++fake::state.restarts;
    fake::require(fake::state.boot_selected, "unexpected restart without activation");
    throw fake::Restart{};
}
const char *esp_err_to_name(esp_err_t value) { return value == ESP_OK ? "ESP_OK" : "simulated_error"; }
std::int64_t esp_timer_get_time() { return fake::state.time_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *timer)
{ *timer = &fake::state; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t, std::uint64_t) { return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t) { return ESP_OK; }
void vTaskDelay(TickType_t ticks)
{
    auto &s = fake::state;
    s.delays.push_back(ticks);
    if (s.before_delay) s.before_delay(ticks);
    if (s.stop_delay_number && s.delays.size() == s.stop_delay_number) throw fake::StopWorker{};
    s.time_us += static_cast<std::int64_t>(ticks) * 1000;
}
void vTaskDelete(void *) { throw fake::StopWorker{}; }
BaseType_t xTaskCreate(void (*)(void *), const char *, std::uint32_t, void *, unsigned, void *)
{ return fake::state.task_create_ok ? pdPASS : 0; }
esp_err_t nvs_open(const char *, int, nvs_handle_t *handle)
{
    if (fake::state.before_nvs_open) fake::state.before_nvs_open();
    if (fake::state.nvs_open_result != ESP_OK) return fake::state.nvs_open_result;
    fake::event("nvs_open");
    fake::state.store_open = true;
    ++fake::state.nvs_opens;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *target, std::size_t *size)
{
    if (fake::state.nvs_get_result != ESP_OK) return fake::state.nvs_get_result;
    const auto &value = fake::state.remembered_version;
    fake::require(*size >= value.size() + 1, "test remembered string too long");
    std::memcpy(target, value.c_str(), value.size() + 1);
    *size = value.size() + 1;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *value)
{
    fake::event("nvs_set");
    if (fake::state.nvs_set_result == ESP_OK) fake::state.written_version = value;
    return fake::state.nvs_set_result;
}
esp_err_t nvs_commit(nvs_handle_t)
{
    fake::event("nvs_commit");
    if (fake::state.commit_result == ESP_OK) fake::state.committed = true;
    return fake::state.commit_result;
}
void nvs_close(nvs_handle_t)
{
    fake::require(fake::state.store_open, "double NVS close");
    fake::event("nvs_close");
    fake::state.store_open = false;
    ++fake::state.nvs_closes;
}

namespace inverter_gateway::app {
bool try_begin_memory_work()
{
    if (!fake::state.guard_available || fake::state.guard_held) return false;
    fake::state.guard_held = true;
    ++fake::state.guard_acquires;
    return true;
}
void end_memory_work()
{
    fake::require(fake::state.guard_held, "release unowned memory gate");
    fake::event("memory_release");
    fake::state.guard_held = false;
    ++fake::state.guard_releases;
}
MemoryWorkGuard::MemoryWorkGuard(TickType_t) : acquired_(try_begin_memory_work()) {}
MemoryWorkGuard::~MemoryWorkGuard() { release(); }
void MemoryWorkGuard::release()
{
    if (acquired_) { acquired_ = false; end_memory_work(); }
}
}

// This is the production implementation, not a duplicate model of its logic.
#include "../main/src/app/ota_update.cpp"
namespace ota = inverter_gateway::app;

namespace {
unsigned passed = 0;
void reset_case()
{
    fake::state = fake::State{};
    fake::initialize_image();
    ota::callbacks = {nullptr, fake::healthy, fake::blocker, fake::quiesce, fake::resume};
    ota::status = ota::OtaStatus{"not_checked"};
    ota::trial = false;
    ota::trial_timer = nullptr;
}
struct Expected {
    const char *state = "check_failed";
    unsigned begins = 0, aborts = 0, ends = 0, quiesces = 0, resumes = 0, boot_calls = 0;
    bool restart = false;
};
void expect_clear_diagnostics()
{
    const auto snapshot = ota::ota_status();
    fake::require(snapshot.failure_stage != nullptr && snapshot.failure_stage[0] == '\0' &&
                  snapshot.failure_code == ESP_OK && snapshot.failure_http_status == 0 &&
                  snapshot.failure_internal_free_bytes == 0 &&
                  snapshot.failure_internal_largest_block_bytes == 0 &&
                  snapshot.failure_required_free_bytes == 0 &&
                  snapshot.failure_required_largest_block_bytes == 0,
                  "old failure diagnostics survived a new actual check");
}
void expect_failure(const char *stage, esp_err_t code, int http_status = 200)
{
    const auto snapshot = ota::ota_status();
    fake::require(snapshot.failure_stage != nullptr &&
                  std::string(snapshot.failure_stage) == stage,
                  std::string("wrong failure stage; expected ") + stage);
    fake::require(snapshot.failure_code == code, std::string(stage) + ": wrong error code");
    fake::require(snapshot.failure_http_status == http_status,
                  std::string(stage) + ": wrong HTTP status");
}
void expect_failure_memory(fake::HeapSample observed, fake::HeapSample required)
{
    const auto snapshot = ota::ota_status();
    fake::require(snapshot.failure_internal_free_bytes == observed.free_bytes &&
                  snapshot.failure_internal_largest_block_bytes == observed.largest_block,
                  "failure memory does not match the sample at failure");
    fake::require(snapshot.failure_required_free_bytes == required.free_bytes &&
                  snapshot.failure_required_largest_block_bytes == required.largest_block,
                  "failure diagnostics report the wrong phase budget");
    const auto live = fake::current_heap();
    fake::require(snapshot.internal_free_bytes == live.free_bytes &&
                  snapshot.internal_largest_block_bytes == live.largest_block,
                  "live heap diagnostics stopped following current memory");
}
void run(const char *name, const std::function<void()> &setup, Expected expected = {})
{
    reset_case();
    setup();
    bool restarted = false;
    try { (void)ota::check_release(); }
    catch (const fake::Restart &) { restarted = true; }
    fake::assert_clean();
    const auto &s = fake::state;
    fake::require(std::string(ota::status.state) == expected.state, std::string(name) + ": wrong status " + ota::status.state);
    fake::require(s.begins == expected.begins && s.aborts == expected.aborts && s.ends == expected.ends,
                  std::string(name) + ": wrong begin/abort/end counts");
    fake::require(s.quiesces == expected.quiesces && s.resumes == expected.resumes && s.boot_calls == expected.boot_calls,
                  std::string(name) + ": wrong quiesce/resume/boot counts");
    fake::require(restarted == expected.restart && s.boot_selected == expected.restart,
                  std::string(name) + ": wrong activation result");
    if (std::string(expected.state) == "check_failed") {
        fake::require(ota::status.failure_stage != nullptr && ota::status.failure_stage[0] != '\0' &&
                      ota::status.failure_code != ESP_OK,
                      std::string(name) + ": failed attempt has no useful diagnostics");
    } else expect_clear_diagnostics();
    if (s.aborts) fake::require(fake::index_of("ota_abort") < fake::index_of("http_cleanup"), "abort after HTTP cleanup");
    if (s.resumes) fake::require(fake::index_of("http_cleanup") < fake::index_of("resume"), "resume before resource cleanup");
    if (restarted) {
        fake::require(s.flash_image == s.body, "activated image differs from downloaded bytes");
        fake::require(fake::index_of("ota_end") < fake::index_of("quiesce"), "quiesce before validation");
        fake::require(fake::index_of("nvs_commit") < fake::index_of("set_boot"), "boot selection before attempt commit");
        fake::require(fake::index_of("http_cleanup") < fake::index_of("restart") &&
                      fake::index_of("buffer_free") < fake::index_of("restart") &&
                      fake::index_of("nvs_close") < fake::index_of("restart"), "restart before cleanup");
    }
    ++passed;
    std::cout << "PASS " << name << '\n';
}
void schedule_test(const char *name, const std::function<void()> &setup, std::uint32_t expected_retry)
{
    reset_case();
    setup();
    fake::state.stop_delay_number = 2;
    try { ota::worker(nullptr); }
    catch (const fake::StopWorker &) {}
    fake::assert_clean();
    fake::require(fake::state.delays.size() == 2, "scheduler did not reach retry delay");
    fake::require(fake::state.delays[0] == ota::ota_initial_check_min_ms +
                  fake::state.entropy % ota::ota_initial_check_jitter_ms,
                  "wrong randomized startup delay");
    fake::require(fake::state.delays[1] == expected_retry, "wrong retry interval");
    fake::require(ota::ota_status().next_check_ms ==
                  fake::state.time_us / 1000 + expected_retry,
                  "status does not expose the scheduled retry deadline");
    ++passed;
    std::cout << "PASS " << name << '\n';
}
void memory_policy_tests()
{
    const fake::HeapSample connection{
        ota::ota_minimum_free_heap, ota::ota_minimum_largest_block};
    const fake::HeapSample streaming{
        ota::ota_stream_minimum_free_heap, ota::ota_stream_minimum_largest_block};
    // Pin reserves while permitting a TLS record (16 KiB plus overhead) without
    // the previous unrelated 32 KiB contiguous-block requirement.
    fake::require(connection.free_bytes == 64000 && connection.largest_block == 24576 &&
                  streaming.free_bytes == 32768 && streaming.largest_block == 8192,
                  "OTA phase budgets differ from the reviewed memory policy");
    run("handshake admission accepts both exact boundaries", [&] {
        fake::heap_at(fake::HeapPhase::before_open) = connection;
        fake::incoming_version("1.0.3");
    }, {"up_to_date"});
    fake::require(fake::state.opens == 1, "exact handshake boundary did not start a check");
    run("reported 89.2 KiB free and 31 KiB largest block permits actual check", [&] {
        fake::heap_at(fake::HeapPhase::before_open) = {91340, 31744};
        fake::incoming_version("1.0.3");
    }, {"up_to_date"});
    fake::require(fake::state.opens == 1 && ota::status.last_check_ms != 0,
                  "reported device memory remains stuck before the first GET");
    run("one byte below handshake free reserve defers without TLS", [&] {
        fake::heap_at(fake::HeapPhase::before_open) =
            {connection.free_bytes - 1, connection.largest_block};
    }, {"deferred"});
    fake::require(fake::state.opens == 0 && fake::state.nvs_opens == 0 &&
                  std::string(ota::ota_status().reason) == "low_internal_heap",
                  "insufficient handshake heap contacted the endpoint or lost its reason");
    run("one byte below handshake contiguous reserve defers without TLS", [&] {
        fake::heap_at(fake::HeapPhase::before_open) =
            {connection.free_bytes, connection.largest_block - 1};
    }, {"deferred"});
    fake::require(fake::state.opens == 0 && fake::state.nvs_opens == 0 &&
                  std::string(ota::ota_status().reason) == "internal_heap_fragmented",
                  "fragmented handshake heap contacted the endpoint or lost its reason");

    struct Checkpoint {
        fake::HeapPhase phase;
        const char *name;
        const char *stage;
        std::size_t written;
        int http_status;
    };
    const Checkpoint checkpoints[] = {
        {fake::HeapPhase::tls_open, "after TLS", "post_tls_memory", 0, 0},
        {fake::HeapPhase::headers, "after headers", "post_headers_memory", 0, 200},
        {fake::HeapPhase::prefix_read, "before erase", "pre_write_memory", 0, 200},
        {fake::HeapPhase::prefix_written, "before first body read", "download_memory", fake::prefix_size, 200},
        {fake::HeapPhase::body_read, "after body read", "post_read_memory", fake::prefix_size, 200},
        {fake::HeapPhase::body_written, "mid-download", "download_memory", fake::prefix_size + 4096, 200},
        {fake::HeapPhase::body_eof, "before verification", "verification_memory", 8192, 200},
    };
    for (const bool chunked : {false, true}) {
        const std::string framing = chunked ? "chunked " : "known-length ";
        run((framing + "stream succeeds below both handshake reserves").c_str(), [&] {
            if (chunked) fake::chunked_response();
            fake::stream_heap({48000, 16384});
            fake::state.read_chunk = 61;
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run((framing + "stream accepts exact free and contiguous boundaries").c_str(), [&] {
            if (chunked) fake::chunked_response();
            fake::heap_at(fake::HeapPhase::before_open) = connection;
            fake::stream_heap(streaming);
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        for (const auto &checkpoint : checkpoints) {
            for (const bool fragmented : {false, true}) {
                const fake::HeapSample denied = fragmented
                    ? fake::HeapSample{streaming.free_bytes + 16000, streaming.largest_block - 1}
                    : fake::HeapSample{streaming.free_bytes - 1, streaming.largest_block + 4096};
                const auto name = framing + (fragmented ? "fragmented " : "low free ") + checkpoint.name;
                const unsigned began = checkpoint.written ? 1 : 0;
                run(name.c_str(), [&] {
                    if (chunked) fake::chunked_response();
                    fake::stream_heap({48000, 16384});
                    fake::heap_at(checkpoint.phase) = denied;
                }, {"check_failed", began, began});
                expect_failure(checkpoint.stage, ESP_ERR_NO_MEM, checkpoint.http_status);
                expect_failure_memory(denied, streaming);
                fake::require(fake::state.flash_image.size() == checkpoint.written,
                              name + ": flash progressed after memory denial");
                fake::require(fake::state.heap_phase == fake::HeapPhase::cleaned &&
                              fake::current_heap().free_bytes > denied.free_bytes &&
                              fake::current_heap().largest_block > denied.largest_block,
                              name + ": cleanup recovery was not modeled");
                fake::require(fake::state.written_version.empty() && !fake::state.committed,
                              name + ": memory denial recorded an activation attempt");
                if (checkpoint.phase == fake::HeapPhase::tls_open ||
                    checkpoint.phase == fake::HeapPhase::headers) {
                    fake::require(fake::state.reads == 0, name + ": body read before memory admission");
                }
            }
        }
    }

    const fake::HeapSample rejected{streaming.free_bytes - 1, streaming.largest_block + 4096};
    bool memory_recovered_after_sample = false;
    run("memory denial retains its exact sample despite immediate recovery", [&] {
        fake::heap_at(fake::HeapPhase::headers) = rejected;
        fake::state.after_heap_sample = [&] {
            if (fake::state.heap_phase == fake::HeapPhase::headers) {
                fake::heap_at(fake::HeapPhase::headers) = {128000, 64000};
                memory_recovered_after_sample = true;
            }
        };
    });
    fake::require(memory_recovered_after_sample, "heap recovery injection did not occur");
    expect_failure("post_headers_memory", ESP_ERR_NO_MEM);
    expect_failure_memory(rejected, streaming);

    const fake::HeapSample tls_failed{41000, 17000};
    run("TLS error captures failure heap and connection requirements before cleanup", [&] {
        fake::heap_at(fake::HeapPhase::tls_open) = tls_failed;
        fake::state.open_result = ESP_FAIL;
    });
    expect_failure("https_open", ESP_FAIL, 0);
    expect_failure_memory(tls_failed, connection);
    const fake::HeapSample stream_failed{42000, 18000};
    run("flash error captures active stream heap and requirements before cleanup", [&] {
        fake::stream_heap(stream_failed);
        fake::state.fail_write_on = 2;
    }, {"check_failed", 1, 1});
    expect_failure("body_write", ESP_FAIL);
    expect_failure_memory(stream_failed, streaming);
    run("verification error preserves failure heap after OTA handle consumption", [&] {
        fake::stream_heap(stream_failed);
        fake::state.end_result = ESP_ERR_INVALID_CRC;
    }, {"check_failed", 1, 0, 1});
    expect_failure("image_verification", ESP_ERR_INVALID_CRC);
    expect_failure_memory(stream_failed, streaming);
}
void retry_scheduler_tests()
{
    schedule_test("network failure retries after 5-10 minutes", [] {
        fake::state.open_result = ESP_FAIL;
    }, 300007);
    schedule_test("HTTP 429 retries after 5-10 minutes", [] {
        fake::state.http_status = 429;
    }, 300007);
    schedule_test("HTTP 503 retries after 5-10 minutes", [] {
        fake::state.http_status = 503;
    }, 300007);
    schedule_test("header transport failure retries after 5-10 minutes", [] {
        fake::state.content_length = -1;
    }, 300007);
    schedule_test("memory allocation failure retries after 5-10 minutes", [] {
        fake::state.fail_client_init = true;
    }, 300007);
    schedule_test("permanent HTTP 404 retains fleet interval", [] {
        fake::state.http_status = 404;
    }, ota::ota_check_interval_ms + 7);
    schedule_test("permanent corrupt image retains fleet interval", [] {
        fake::set_incoming([](auto &h, auto &) { h.hash_appended = 0; });
    }, ota::ota_check_interval_ms + 7);

    reset_case();
    fake::state.open_result = ESP_FAIL;
    fake::state.stop_delay_number = 8;
    fake::state.before_delay = [](auto ticks) {
        fake::require(ota::ota_status().next_check_ms ==
                      fake::state.time_us / 1000 + ticks,
                      "scheduled deadline missing while the worker sleeps");
    };
    fake::state.before_nvs_open = [] {
        fake::require(ota::ota_status().next_check_ms == 0,
                      "old retry deadline survived into actual check");
    };
    try { ota::worker(nullptr); }
    catch (const fake::StopWorker &) {}
    fake::assert_clean(false);
    const std::array<TickType_t, 7> retries{
        300007, 600007, 1200007, 1800007, 1800007, 1800007, 1800007};
    fake::require(fake::state.opens == retries.size(), "transient retries did not run automatically");
    for (std::size_t i = 0; i < retries.size(); ++i)
        fake::require(fake::state.delays[i + 1] == retries[i],
                      "transient backoff did not grow and saturate below one hour");
    ++passed;
    std::cout << "PASS repeated transient failures retry with bounded backoff and accurate deadlines\n";

    reset_case();
    fake::state.open_result = ESP_FAIL;
    fake::state.stop_delay_number = 4;
    fake::state.before_delay = [](auto) {
        if (fake::state.delays.size() == 2) fake::state.guard_available = false;
        if (fake::state.delays.size() == 3) {
            fake::state.guard_available = true;
            fake::state.open_result = ESP_OK;
            fake::incoming_version("1.0.3");
        }
    };
    try { ota::worker(nullptr); }
    catch (const fake::StopWorker &) {}
    fake::assert_clean(false);
    fake::require(fake::state.opens == 2 && fake::state.delays[1] == 300007 &&
                  fake::state.delays[2] == 300007 &&
                  fake::state.delays[3] == ota::ota_check_interval_ms + 7 &&
                  std::string(ota::status.state) == "up_to_date",
                  "local blocker recovery did not restore normal successful check scheduling");
    ++passed;
    std::cout << "PASS failed check followed by local deferral recovers automatically\n";
}
void memory_diagnostic_lifecycle_tests()
{
    const fake::HeapSample streaming{
        ota::ota_stream_minimum_free_heap, ota::ota_stream_minimum_largest_block};
    const fake::HeapSample failed_heap{streaming.free_bytes - 1, streaming.largest_block + 4096};
    reset_case();
    fake::heap_at(fake::HeapPhase::prefix_read) = failed_heap;
    (void)ota::check_release();
    fake::assert_clean();
    expect_failure("pre_write_memory", ESP_ERR_NO_MEM);
    expect_failure_memory(failed_heap, streaming);
    const auto failed_at = ota::status.last_check_ms;
    const std::array<std::function<void()>, 4> local_deferrals{{
        [] { fake::state.block_from = 1; },
        [] { fake::state.guard_available = false; },
        [] { fake::heap_at(fake::HeapPhase::before_open).free_bytes = 1; },
        [] { fake::heap_at(fake::HeapPhase::before_open).largest_block = 1; },
    }};
    for (const auto &defer : local_deferrals) {
        fake::state = fake::State{};
        fake::initialize_image();
        fake::state.time_us = (failed_at + 1000) * 1000;
        defer();
        fake::require(ota::check_release() == ota::CheckOutcome::local_deferred,
                      "local resource pressure started an actual retry");
        fake::assert_clean();
        expect_failure("pre_write_memory", ESP_ERR_NO_MEM);
        expect_failure_memory(failed_heap, streaming);
        fake::require(ota::status.last_check_ms == failed_at &&
                      std::string(ota::status.available_version) == "1.0.4" &&
                      fake::state.opens == 0 && fake::state.nvs_opens == 0,
                      "local deferral changed failed attempt context or contacted the endpoint");
    }
    ++passed;
    std::cout << "PASS failure heap and requirements survive runtime gate low-heap and fragmentation deferrals\n";

    fake::state = fake::State{};
    fake::initialize_image();
    fake::state.time_us = (failed_at + 2000) * 1000;
    fake::state.nvs_open_result = ESP_FAIL;
    bool observed_cleared = false;
    fake::state.before_nvs_open = [&] {
        expect_clear_diagnostics();
        observed_cleared = true;
    };
    (void)ota::check_release();
    fake::assert_clean();
    expect_failure("nvs_open", ESP_FAIL, 0);
    expect_failure_memory(fake::current_heap(),
                          {ota::ota_minimum_free_heap, ota::ota_minimum_largest_block});
    fake::require(observed_cleared && ota::status.last_check_ms > failed_at &&
                  ota::status.available_version[0] == '\0',
                  "new failed check did not reset and replace previous failure diagnostics");
    ++passed;
    std::cout << "PASS new check clears memory diagnostics before NVS and replaces phase requirements on failure\n";

    fake::state = fake::State{};
    fake::initialize_image();
    fake::incoming_version("1.0.3");
    (void)ota::check_release();
    fake::assert_clean();
    expect_clear_diagnostics();
    fake::require(std::string(ota::status.state) == "up_to_date",
                  "subsequent nonfailure did not clear failure memory");
    ++passed;
    std::cout << "PASS subsequent nonfailure contains no retained failure memory\n";
}
void diagnostic_lifecycle_tests()
{
    reset_case();
    fake::state.http_status = 503;
    (void)ota::check_release();
    fake::assert_clean();
    expect_failure("response_headers", ESP_ERR_INVALID_SIZE, 503);
    const auto failed_at = ota::status.last_check_ms;
    const auto request_count = fake::state.opens;
    fake::state.time_us += 1000000;
    fake::state.block_from = fake::state.blocker_calls + 1;
    fake::require(ota::check_release() == ota::CheckOutcome::local_deferred,
                  "runtime blocker did not defer retry");
    fake::assert_clean();
    expect_failure("response_headers", ESP_ERR_INVALID_SIZE, 503);
    fake::require(std::string(ota::status.state) == "deferred" &&
                  ota::status.last_check_ms == failed_at && fake::state.opens == request_count,
                  "local deferral reset last failed attempt or contacted the server");
    fake::state.block_from = 0;
    fake::state.guard_available = false;
    fake::require(ota::check_release() == ota::CheckOutcome::local_deferred,
                  "memory gate did not defer retry");
    fake::assert_clean();
    expect_failure("response_headers", ESP_ERR_INVALID_SIZE, 503);

    // Reset fake SDK resources, not the real worker's persisted RAM status.
    fake::state = fake::State{};
    fake::initialize_image();
    fake::incoming_version("1.0.3");
    fake::state.time_us = (failed_at + 2000) * 1000;
    bool observed_cleared = false;
    fake::state.before_nvs_open = [&] {
        expect_clear_diagnostics();
        fake::require(std::string(ota::status.state) == "checking",
                      "actual attempt did not enter checking before NVS");
        observed_cleared = true;
    };
    fake::require(ota::check_release() == ota::CheckOutcome::attempted,
                  "ready retry was not attempted");
    fake::assert_clean();
    expect_clear_diagnostics();
    fake::require(observed_cleared && std::string(ota::status.state) == "up_to_date" &&
                  ota::status.last_check_ms > failed_at,
                  "new successful attempt did not clear and replace old failure status");
    ++passed;
    std::cout << "PASS failure survives local deferrals and clears when real retry begins\n";

    reset_case();
    fake::set_incoming([](auto &h, auto &) { h.hash_appended = 0; });
    (void)ota::check_release();
    fake::assert_clean();
    expect_failure("image_hash", ESP_ERR_INVALID_CRC);
    fake::require(std::string(ota::status.available_version) == "1.0.4",
                  "confirmed release disappeared from post-descriptor failure");
    fake::state = fake::State{};
    fake::initialize_image();
    fake::state.open_result = ESP_FAIL;
    (void)ota::check_release();
    fake::assert_clean();
    expect_failure("https_open", ESP_FAIL, 0);
    fake::require(ota::status.available_version[0] == '\0',
                  "new early failure retained previous offered version");
    ++passed;
    std::cout << "PASS new failed attempt replaces stage HTTP status and offered version\n";
}
}

int main()
{
    try {
        run("runtime preflight defers without request", [] { fake::state.block_from = 1; }, {"deferred"});
        fake::require(fake::state.opens == 0 && ota::status.last_check_ms == 0, "deferred state started a network check");
        run("memory gate contention defers", [] { fake::state.guard_available = false; }, {"deferred"});
        run("low total internal heap defers", [] {
            fake::heap_at(fake::HeapPhase::before_open).free_bytes = 1;
        }, {"deferred"});
        run("fragmented internal heap defers", [] {
            fake::heap_at(fake::HeapPhase::before_open).largest_block = 1;
        }, {"deferred"});
        run("NVS open fails", [] { fake::state.nvs_open_result = ESP_FAIL; });
        expect_failure("nvs_open", ESP_FAIL, 0);
        run("HTTP client allocation fails", [] { fake::state.fail_client_init = true; });
        expect_failure("allocation", ESP_ERR_NO_MEM, 0);
        run("download buffer allocation fails", [] { fake::state.fail_buffer_alloc = true; });
        expect_failure("allocation", ESP_ERR_NO_MEM, 0);
        run("TLS/open fails", [] { fake::state.open_result = ESP_FAIL; });
        expect_failure("https_open", ESP_FAIL, 0);
        run("post-TLS low heap fails closed", [] {
            fake::heap_at(fake::HeapPhase::tls_open).free_bytes = 1;
        });
        expect_failure("post_tls_memory", ESP_ERR_NO_MEM, 0);
        run("post-TLS fragmentation fails closed", [] {
            fake::heap_at(fake::HeapPhase::tls_open).largest_block = 1;
        });
        expect_failure("post_tls_memory", ESP_ERR_NO_MEM, 0);
        run("HTTP 404 rejected", [] { fake::state.http_status = 404; });
        expect_failure("response_headers", ESP_ERR_INVALID_SIZE, 404);
        run("HTTP 429 rejected", [] { fake::state.http_status = 429; });
        expect_failure("response_headers", ESP_ERR_INVALID_SIZE, 429);
        run("HTTP 503 rejected", [] { fake::state.http_status = 503; });
        expect_failure("response_headers", ESP_ERR_INVALID_SIZE, 503);
        run("redirect rejected without following", [] { fake::state.http_status = 302; });
        run("header fetch error rejected", [] { fake::state.content_length = -1; });
        run("missing nonchunked content length rejected", [] { fake::state.content_length = 0; });
        expect_failure("response_headers", ESP_ERR_INVALID_SIZE);
        run("short content length rejected", [] { fake::state.content_length = fake::prefix_size - 1; });
        run("oversized image rejected", [] { fake::state.content_length = fake::state.partition.size + 1; });
        run("missing OTA slot rejected", [] { fake::state.missing_partition = true; });
        run("truncated header rejected", [] { fake::state.read_limit = fake::prefix_size - 1; });
        run("prefix read error rejected", [] { fake::state.fail_read_on = 1; });
        expect_failure("image_prefix", ESP_FAIL);
        run("timeout before prefix rejected", [] { fake::state.open_advance_ms = ota::ota_download_timeout_ms + 1; });
        run("invalid image magic rejected", [] { fake::set_incoming([](auto &h, auto &) { h.magic = 0; }); }, {"incompatible"});
        run("invalid application magic rejected", [] { fake::set_incoming([](auto &, auto &d) { d.magic_word = 0; }); }, {"incompatible"});
        run("wrong chip rejected", [] { fake::set_incoming([](auto &h, auto &) { ++h.chip_id; }); }, {"incompatible"});
        run("wrong project rejected", [] { fake::set_incoming([](auto &, auto &d) { std::strcpy(d.project_name, "other"); }); }, {"incompatible"});
        run("invalid incoming version rejected", [] { fake::incoming_version("1.0.4-beta"); }, {"incompatible"});
        run("unterminated version rejected", [] { fake::set_incoming([](auto &, auto &d) { std::memset(d.version, '1', 32); }); }, {"incompatible"});
        run("invalid running version rejected", [] { std::strcpy(fake::state.running.version, "unknown"); }, {"incompatible"});
        run("same version skips before erase", [] { fake::incoming_version("1.0.3"); }, {"up_to_date"});
        run("older version skips before erase", [] { fake::incoming_version("1.0.2"); }, {"server_older"});
        run("previous activation attempt is blocked", [] {
            fake::state.nvs_get_result = ESP_OK; fake::state.remembered_version = "1.0.4";
        }, {"release_blocked"});
        run("missing SHA flag rejected before erase", [] { fake::set_incoming([](auto &h, auto &) { h.hash_appended = 0; }); });
        expect_failure("image_hash", ESP_ERR_INVALID_CRC);
        run("write arrives before erase", [] { fake::state.block_from = 2; });
        expect_failure("write_admission", ESP_ERR_INVALID_STATE);
        run("low heap before erase", [] {
            fake::heap_at(fake::HeapPhase::prefix_read).free_bytes = 1;
        });
        expect_failure("pre_write_memory", ESP_ERR_NO_MEM);
        run("OTA begin failure", [] { fake::state.begin_result = ESP_FAIL; }, {"check_failed", 1});
        expect_failure("ota_begin", ESP_FAIL);
        run("prefix flash write failure aborts", [] { fake::state.fail_write_on = 1; }, {"check_failed", 1, 1});
        expect_failure("prefix_write", ESP_FAIL);
        run("body flash write failure aborts", [] { fake::state.fail_write_on = 2; }, {"check_failed", 1, 1});
        expect_failure("body_write", ESP_FAIL);
        run("body read error aborts", [] { fake::state.fail_read_on = 2; }, {"check_failed", 1, 1});
        expect_failure("body_read", ESP_FAIL);
        run("truncated body aborts", [] { fake::state.read_limit = fake::prefix_size + 100; }, {"check_failed", 1, 1});
        run("low heap during transfer aborts", [] {
            fake::heap_at(fake::HeapPhase::prefix_written).free_bytes = 1;
        }, {"check_failed", 1, 1});
        expect_failure("download_memory", ESP_ERR_NO_MEM);
        run("elapsed download timeout aborts", [] { fake::state.read_advance_ms = ota::ota_download_timeout_ms + 1; }, {"check_failed", 1, 1});
        expect_failure("download_timeout", ESP_ERR_TIMEOUT);
        run("persistent write blocker times out without replay", [] { fake::state.block_from = 3; }, {"check_failed", 1, 1});
        run("image validation failure cannot activate", [] { fake::state.end_result = ESP_ERR_INVALID_CRC; }, {"check_failed", 1, 0, 1});
        expect_failure("image_verification", ESP_ERR_INVALID_CRC);
        run("failed quiesce cannot activate", [] { fake::state.quiesce_result = false; }, {"check_failed", 1, 0, 1, 1});
        expect_failure("restart_admission", ESP_ERR_INVALID_STATE);
        run("NVS set failure resumes safely", [] { fake::state.nvs_set_result = ESP_FAIL; }, {"check_failed", 1, 0, 1, 1, 1});
        expect_failure("release_record", ESP_FAIL);
        run("NVS commit failure resumes safely", [] { fake::state.commit_result = ESP_FAIL; }, {"check_failed", 1, 0, 1, 1, 1});
        expect_failure("release_record", ESP_FAIL);
        run("boot selection failure resumes safely", [] { fake::state.boot_result = ESP_FAIL; }, {"check_failed", 1, 0, 1, 1, 1, 1});
        expect_failure("boot_selection", ESP_FAIL);
        run("newer image activates only after validation and cleanup", [] {}, {"restarting", 1, 0, 1, 1, 0, 1, true});
        fake::require(fake::state.allocation_caps == std::vector<unsigned>{MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT},
                      "download scratch buffer did not prefer PSRAM");
        fake::require(fake::state.http_was_live_at_quiesce, "current TLS-lifetime characterization changed: review expectations");
        run("absent PSRAM falls back to the normal allocator", [] {
            fake::state.fail_psram_alloc = true;
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        fake::require(fake::state.allocation_caps == std::vector<unsigned>{
                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT},
                      "missing PSRAM did not retain the internal-memory fallback");
        run("fragmented HTTP reads reconstruct exact image", [] { fake::state.read_chunk = 61; }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run("chunked unknown-length image completes safely", [] {
            fake::chunked_response();
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        fake::require(fake::state.completion_checks != 0 && fake::state.eof_observed,
                      "chunked success skipped parser completion or EOF draining");
        run("fragmented chunked reads reconstruct exact image", [] {
            fake::chunked_response(); fake::state.read_chunk = 61;
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run("chunked terminator consumed after final body bytes", [] {
            fake::chunked_response(); fake::state.completion_requires_eof = true;
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run("parser completion does not skip buffered chunked body", [] {
            fake::chunked_response(); fake::state.parser_complete_early = true;
            fake::state.read_chunk = 61;
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run("parser completion does not skip buffered known-length body", [] {
            fake::state.parser_complete_early = true; fake::state.read_chunk = 61;
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run("exact-slot chunked image allows terminating read", [] {
            fake::chunked_response(); fake::state.completion_requires_eof = true;
            fake::state.partition.size = static_cast<std::uint32_t>(fake::state.body.size());
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run("chunked body without completed parser aborts", [] {
            fake::chunked_response(); fake::state.response_complete = false;
        }, {"check_failed", 1, 1});
        expect_failure("body_completion", ESP_ERR_INVALID_SIZE);
        run("truncated chunked body aborts", [] {
            fake::chunked_response(); fake::state.read_limit = fake::prefix_size + 100;
        }, {"check_failed", 1, 1});
        run("oversized chunked body never writes past slot", [] {
            fake::chunked_response();
            fake::state.partition.size = static_cast<std::uint32_t>(fake::state.body.size() - 1);
        }, {"check_failed", 1, 1});
        expect_failure("image_size", ESP_ERR_INVALID_SIZE);
        run("exact-slot body with missing chunk terminator aborts", [] {
            fake::chunked_response(); fake::state.response_complete = false;
            fake::state.partition.size = static_cast<std::uint32_t>(fake::state.body.size());
        }, {"check_failed", 1, 1});
        run("chunked body read error aborts", [] {
            fake::chunked_response(); fake::state.fail_read_on = 2;
        }, {"check_failed", 1, 1});
        run("chunked prefix read error rejected before erase", [] {
            fake::chunked_response(); fake::state.fail_read_on = 1;
        });
        run("empty chunked body rejected before erase", [] {
            fake::chunked_response(); fake::state.body.clear();
        });
        run("chunked EOF before complete prefix rejected before erase", [] {
            fake::chunked_response(); fake::state.body.resize(fake::prefix_size - 1);
        });
        run("chunked wrong project rejected before erase", [] {
            fake::chunked_response();
            fake::set_incoming([](auto &, auto &d) { std::strcpy(d.project_name, "other"); });
        }, {"incompatible"});
        run("chunked missing SHA flag rejected before erase", [] {
            fake::chunked_response();
            fake::set_incoming([](auto &h, auto &) { h.hash_appended = 0; });
        });
        run("chunked image validation failure cannot activate", [] {
            fake::chunked_response(); fake::state.end_result = ESP_ERR_INVALID_CRC;
        }, {"check_failed", 1, 0, 1});
        run("chunked flag cannot disguise header fetch error", [] {
            fake::chunked_response(); fake::state.content_length = -1;
        });
        run("known-length body without completed parser aborts", [] {
            fake::state.response_complete = false;
        }, {"check_failed", 1, 1});
        expect_failure("body_completion", ESP_ERR_INVALID_SIZE);
        run("exact-slot known-length image completes safely", [] {
            fake::state.partition.size = static_cast<std::uint32_t>(fake::state.body.size());
        }, {"restarting", 1, 0, 1, 1, 0, 1, true});
        run("known-length overflow rejected before extra write", [] {
            fake::state.content_length = 4096;
        }, {"check_failed", 1, 1});
        expect_failure("image_size", ESP_ERR_INVALID_SIZE);
        run("chunked prefix cannot exceed slot before erase", [] {
            fake::chunked_response(); fake::state.partition.size = fake::prefix_size - 1;
        });
        memory_policy_tests();
        memory_diagnostic_lifecycle_tests();
        diagnostic_lifecycle_tests();
        schedule_test("local deferral scheduler uses 5-10 minute window", [] { fake::state.block_from = 1; }, 300007);
        schedule_test("successful check scheduler uses configured fleet interval", [] { fake::incoming_version("1.0.3"); }, ota::ota_check_interval_ms + 7);
        retry_scheduler_tests();
        reset_case();
        fake::state.task_create_ok = false;
        ota::start_ota_update({nullptr, fake::healthy, fake::blocker, fake::quiesce, fake::resume});
        fake::require(std::string(ota::ota_status().state) == "task_failed", "task creation failure invisible");
        ++passed;
        std::cout << "PASS task creation failure reported\n";
        std::cout << "PASS " << passed << " offline actual-worker tests\n";
        std::cout << "LIMITATIONS: fake image verification result, no real TLS/flash/power-cut/SMP/watchdog coverage.\n";
        std::cout << "CHARACTERIZED: HTTPS resources remain alive during quiesce; failed quiesce is not retained for later activation.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
