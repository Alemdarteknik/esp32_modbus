#pragma once
#include <cstdint>
namespace inverter_gateway::app {
struct OtaStatus {
    const char *state;
    char available_version[32]{};
    std::int64_t last_check_ms = 0;
    // Monotonic deadline for the initial check or scheduled retry; zero while
    // checking, disabled, or waiting for the trial boot to become healthy.
    std::int64_t next_check_ms = 0;
    const char *reason = "";
    std::uint32_t internal_free_bytes = 0;
    std::uint32_t internal_largest_block_bytes = 0;
    // Last failed attempt, cleared when a new actual check starts. Not persisted.
    const char *failure_stage = "";
    std::int32_t failure_code = 0;
    std::int32_t failure_http_status = 0;
    // Snapshot before cleanup, plus the active phase's required reserve.
    // Required values do not imply memory caused a non-memory failure.
    std::uint32_t failure_internal_free_bytes = 0;
    std::uint32_t failure_internal_largest_block_bytes = 0;
    std::uint32_t failure_required_free_bytes = 0;
    std::uint32_t failure_required_largest_block_bytes = 0;
};
OtaStatus ota_status();
void arm_ota_boot_trial();
struct OtaHooks {
    void *context;
    bool (*healthy)(void *);
    // nullptr means ready; otherwise return a stable, static reason string.
    const char *(*download_blocker)(void *);
    bool (*quiesce)(void *);
    void (*resume)(void *);
};
// Context must outlive the task. Only standalone Wi-Fi role is supported.
void start_ota_update(OtaHooks hooks);
}
