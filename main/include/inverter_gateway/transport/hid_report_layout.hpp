#pragma once

#include <cstddef>
#include <cstdint>

namespace inverter_gateway::transport {

// Only the known byte-pipe layout is supported. Reject report IDs and larger
// or bit-packed layouts instead of stripping bytes based on assumptions.
inline bool is_eight_byte_hid_pipe(const std::uint8_t *data, std::size_t size)
{
    struct Globals { std::uint32_t bits = 0, count = 0; } globals, stack[4];
    std::size_t depth = 0;
    std::uint32_t input_bits = 0, output_bits = 0;
    if (data == nullptr || size == 0 || size > 256) return false;
    for (std::size_t offset = 0; offset < size;) {
        const auto prefix = data[offset++];
        if (prefix == 0xfe) return false; // No long items in supported layout.
        const auto length = (prefix & 3U) == 3U ? 4U : prefix & 3U;
        if (length > size - offset) return false;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < length; ++i) value |= std::uint32_t(data[offset++]) << (i * 8);
        const auto type = (prefix >> 2U) & 3U;
        const auto tag = prefix >> 4U;
        if (type == 1U) {
            if (tag == 7U) globals.bits = value;
            else if (tag == 9U) globals.count = value;
            else if (tag == 8U) return false; // Report ID.
            else if (tag == 10U) {
                if (depth == 4) return false;
                stack[depth++] = globals;
            } else if (tag == 11U) {
                if (depth == 0) return false;
                globals = stack[--depth];
            }
        } else if (type == 0U && (tag == 8U || tag == 9U)) {
            if (globals.bits != 8 || globals.count == 0 || globals.count > 8 ||
                (value & 1U) != 0) return false;
            auto &total = tag == 8U ? input_bits : output_bits;
            total += globals.bits * globals.count;
            if (total > 64) return false;
        }
    }
    return depth == 0 && input_bits == 64 && output_bits == 64;
}

} // namespace inverter_gateway::transport
