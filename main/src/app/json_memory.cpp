#include "inverter_gateway/app/json_memory.hpp"
#include <atomic>
#include <cstdlib>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace inverter_gateway::app {
namespace {
std::atomic<std::uint32_t> failures{0};
void *json_malloc(std::size_t size)
{
    // Small JSON nodes otherwise compete with internal-only TLS allocations.
    // ESP-IDF free() handles either heap. Fall back when PSRAM is unavailable.
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) memory = std::malloc(size);
    if (!memory) failures.fetch_add(1, std::memory_order_relaxed);
    return memory;
}
}
void initialize_json_memory()
{
    cJSON_Hooks hooks{};
    hooks.malloc_fn = json_malloc;
    hooks.free_fn = std::free;
    cJSON_InitHooks(&hooks);
    const auto external = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGW("json_memory", "JSON allocator: %s; PSRAM free=%u internal free=%u",
             external ? "PSRAM preferred" : "internal RAM fallback",
             static_cast<unsigned>(external),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}
std::uint32_t json_allocation_failures()
{
    return failures.load(std::memory_order_relaxed);
}
}
