#pragma once
#include <cstdint>
namespace inverter_gateway::app {
// Call once at boot, before any cJSON objects or concurrent tasks exist.
void initialize_json_memory();
std::uint32_t json_allocation_failures();
class JsonAllocationWatch {
public:
    JsonAllocationWatch() : before_(json_allocation_failures()) {}
    bool ok() const { return before_ == json_allocation_failures(); }
private:
    std::uint32_t before_;
};
}
