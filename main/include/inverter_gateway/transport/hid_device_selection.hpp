#pragma once

#include <cstddef>
#include <cstdint>

#include "inverter_gateway/transport/hid_report_layout.hpp"

namespace inverter_gateway::transport {

inline bool usb_hid_ids_match(std::uint16_t actual_vendor, std::uint16_t actual_product,
    std::uint16_t filter_vendor, std::uint16_t filter_product)
{
    return (filter_vendor == 0 || filter_vendor == actual_vendor) &&
        (filter_product == 0 || filter_product == actual_product);
}

struct UsbHidInterface {
    std::uint8_t interface_number = 0;
    std::uint8_t endpoint = 0;
    std::uint16_t report_descriptor_size = 0;
};

// Parse the complete configuration before returning a candidate. A valid
// first interface cannot conceal a truncated or malformed trailing block.
// candidate_index allows the caller to try another interface when the first
// one's report descriptor describes a different HID device.
inline bool select_usb_hid_interface(const std::uint8_t *data, std::size_t size,
    UsbHidInterface &selected, unsigned candidate_index = 0)
{
    if (data == nullptr || size < 9 || size > 2048 || data[0] != 9 || data[1] != 2 ||
        (std::size_t(data[2]) | (std::size_t(data[3]) << 8U)) != size) return false;
    UsbHidInterface current{}, result{};
    bool candidate = false, found = false;
    bool have_interface = false, have_hid_descriptor = false;
    unsigned candidates = 0, declared_endpoints = 0, observed_endpoints = 0;
    std::uint32_t endpoints = 0;
    auto finish_interface = [&]() {
        if (have_interface && observed_endpoints != declared_endpoints) return false;
        if (candidate && current.endpoint != 0 && current.report_descriptor_size != 0 &&
            current.report_descriptor_size <= 256) {
            if (candidates++ == candidate_index) { result = current; found = true; }
        }
        return true;
    };
    for (std::size_t offset = 9; offset < size;) {
        if (size - offset < 2) return false;
        const auto length = data[offset];
        const auto type = data[offset + 1];
        if (length < 2 || length > size - offset) return false;
        if (type == 4) { // Interface descriptor.
            if (length < 9 || !finish_interface()) return false;
            have_interface = true;
            candidate = data[offset + 3] == 0 && data[offset + 5] == 3 &&
                data[offset + 6] == 0 && data[offset + 7] == 0;
            current = {};
            current.interface_number = data[offset + 2];
            declared_endpoints = data[offset + 4];
            observed_endpoints = 0;
            endpoints = 0;
            have_hid_descriptor = false;
        } else if (type == 5) { // Endpoint descriptor.
            if (!have_interface || length < 7) return false;
            const auto address = data[offset + 2];
            if ((address & 0x0fU) == 0 || (address & 0x70U) != 0) return false;
            const auto mask = std::uint32_t(1) << ((address & 0x0fU) + ((address & 0x80U) != 0 ? 16 : 0));
            if ((endpoints & mask) != 0) return false;
            endpoints |= mask;
            ++observed_endpoints;
            const auto attributes = data[offset + 3];
            const auto packet_size = data[offset + 4] | (data[offset + 5] << 8U);
            if (candidate && (address & 0x80U) != 0 && (attributes & 3U) == 3U && packet_size == 8) {
                if (current.endpoint != 0) return false;
                current.endpoint = address;
            }
        } else if (candidate && type == 0x21) { // HID descriptor.
            if (have_hid_descriptor || length < 6) return false;
            have_hid_descriptor = true;
            const auto descriptors = data[offset + 5];
            if (descriptors == 0 || length != 6U + descriptors * 3U) return false;
            bool have_report_descriptor = false;
            for (unsigned i = 0; i < descriptors; ++i) {
                const auto pos = offset + 6 + i * 3U;
                if (data[pos] == 0x22) {
                    if (have_report_descriptor) return false;
                    have_report_descriptor = true;
                    current.report_descriptor_size = data[pos + 1] | (data[pos + 2] << 8U);
                }
            }
        } else if (type == 2 || type == 1) {
            return false; // A configuration cannot contain another root descriptor.
        }
        offset += length;
    }
    if (!finish_interface() || !found) return false;
    selected = result;
    return true;
}

// In automatic mode require the vendor-defined application used by inverter
// byte bridges. Generic Desktop/keyboard/gamepad applications are excluded
// even when an unusual peripheral happens to expose eight-byte reports.
// Explicit VID+PID filters retain support for an atypical application usage,
// while report sizes, IDs, item bounds and collection balance remain checked.
inline bool is_usb_hid_inverter_report(const std::uint8_t *data, std::size_t size,
    bool automatic = true)
{
    if (!is_eight_byte_hid_pipe(data, size)) return false;
    std::uint32_t usage_page = 0, saved_pages[4]{}, local_usage_page = 0;
    std::size_t global_depth = 0, collection_depth = 0;
    bool within_application[8]{}, within_vendor[8]{};
    bool have_local_usage = false, saw_application = false;
    for (std::size_t offset = 0; offset < size;) {
        const auto prefix = data[offset++];
        const auto length = (prefix & 3U) == 3U ? 4U : prefix & 3U;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < length; ++i) value |= std::uint32_t(data[offset++]) << (i * 8U);
        const auto type = (prefix >> 2U) & 3U;
        const auto tag = prefix >> 4U;
        if (type == 1U) {
            if (tag == 0U) usage_page = value;
            else if (tag == 10U) saved_pages[global_depth++] = usage_page;
            else if (tag == 11U) usage_page = saved_pages[--global_depth];
        } else if (type == 2U && tag == 0U) {
            local_usage_page = length == 4 ? value >> 16U : usage_page;
            have_local_usage = true;
        } else if (type == 0U) {
            if (tag == 10U) { // Collection.
                if (collection_depth == 8 || length != 1) return false;
                const auto application = value == 1;
                const auto page = have_local_usage ? local_usage_page : usage_page;
                const auto vendor = page >= 0xff00 && page <= 0xffff;
                if (application && (!have_local_usage || (automatic && !vendor))) return false;
                within_application[collection_depth] = application ||
                    (collection_depth != 0 && within_application[collection_depth - 1]);
                within_vendor[collection_depth] = (application && vendor) ||
                    (collection_depth != 0 && within_vendor[collection_depth - 1]);
                saw_application = saw_application || application;
                ++collection_depth;
            } else if (tag == 12U) { // End collection.
                if (collection_depth == 0 || length != 0) return false;
                --collection_depth;
            } else if (tag == 8U || tag == 9U) { // Input/output.
                if (collection_depth == 0 || !within_application[collection_depth - 1] ||
                    (automatic && !within_vendor[collection_depth - 1])) return false;
            }
            have_local_usage = false; // HID local state resets after every main item.
        }
    }
    return global_depth == 0 && collection_depth == 0 && saw_application;
}

inline bool is_automatic_hid_byte_pipe(const std::uint8_t *data, std::size_t size)
{
    return is_usb_hid_inverter_report(data, size, true);
}

} // namespace inverter_gateway::transport
