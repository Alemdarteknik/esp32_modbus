#include "inverter_gateway/protocol/pi_protocols.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace inverter_gateway::protocol;

namespace {
int checks = 0;
void check(bool condition) { ++checks; assert(condition); }

std::uint16_t crc(const std::uint8_t *data, std::size_t size)
{
    // Independent byte-oriented implementation, unlike the upstream nibble table.
    unsigned value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value ^= static_cast<unsigned>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) value = ((value << 1) ^ ((value & 0x8000) ? 0x1021 : 0)) & 0xffff;
    }
    return static_cast<std::uint16_t>(value);
}
std::uint8_t escape(std::uint8_t value)
{
    return value == 0 || value == 10 || value == 13 || value == 40 ? value + 1 : value;
}
std::vector<std::uint8_t> framed(EnergyProtocol id, const std::string &text)
{
    const bool caret = id >= EnergyProtocol::pi17 && id <= EnergyProtocol::pi18sv;
    std::string body;
    if (caret) {
        char header[8];
        std::snprintf(header, sizeof(header), "^D%03u", static_cast<unsigned>(text.size() + 3));
        body = header + text;
    } else body = "(" + text;
    std::vector<std::uint8_t> result(body.begin(), body.end());
    const auto sum = crc(result.data(), result.size());
    const bool raw = id >= EnergyProtocol::pi17 && id <= EnergyProtocol::pi17infini;
    result.push_back(raw ? sum >> 8 : escape(sum >> 8));
    result.push_back(raw ? sum & 255 : escape(sum & 255));
    result.push_back('\r');
    return result;
}
std::vector<std::uint8_t> checksum_frame(const std::string &text, char terminal = '\r')
{
    std::vector<std::uint8_t> result(text.begin(), text.end());
    unsigned sum = 1;
    for (auto value : result) sum += value;
    result.push_back(sum & 255);
    result.push_back(terminal);
    return result;
}
const EnergyField *field(const EnergyResponse &response, const char *key)
{
    for (std::size_t i = 0; i < response.count; ++i) {
        if (!std::strcmp(response.fields[i].key, key)) return &response.fields[i];
    }
    return nullptr;
}
double value(const EnergyResponse &response, const char *key)
{
    const auto *found = field(response, key);
    check(found && found->numeric);
    return found->value;
}
std::vector<std::uint8_t> request(EnergyProtocol id, const char *command)
{
    std::uint8_t bytes[160]{};
    std::size_t size = 0;
    check(pi_build_request(id, command, 1, bytes, sizeof(bytes), size) == CodecStatus::ok);
    return {bytes, bytes + size};
}
void exact_request(EnergyProtocol id, const char *command, const std::vector<std::uint8_t> &expected)
{
    check(request(id, command) == expected);
}
void decode(EnergyProtocol id, const char *command, const std::vector<std::uint8_t> &bytes,
            EnergyResponse &response)
{
    std::size_t size = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        check(pi_frame_state(id, command, bytes.data(), i, size) == FrameState::incomplete);
    }
    check(pi_frame_state(id, command, bytes.data(), bytes.size(), size) == FrameState::complete);
    check(size == bytes.size());
    check(pi_decode_response(id, command, 1, bytes.data(), bytes.size(), response) == CodecStatus::ok);
}
}

int main()
{
    exact_request(EnergyProtocol::pi30, "QPI", {'Q','P','I',0xbe,0xac,13});
    exact_request(EnergyProtocol::pi18, "GS", {'^','P','0','0','5','G','S',0x58,0x14,13});
    exact_request(EnergyProtocol::pi18, "EY2022", {'^','P','0','0','9','E','Y','2','0','2','2',0x81,0x1c,13});
    exact_request(EnergyProtocol::pi17, "GS", {'^','P','0','0','3','G','S',13});
    exact_request(EnergyProtocol::pi18lvx, "PIRI", {'^','P','0','0','7','P','I','R','I',13});
    exact_request(EnergyProtocol::pi18sv, "GS", {'^','P','0','0','5','G','S',13});
    exact_request(EnergyProtocol::pi16, "QPI", {'Q','P','I',13});
    exact_request(EnergyProtocol::pi30revo, "PQSE", {'P','Q','S','E',0x3a,13});
    const char *pset = "PSET120103 56.3 54.6 43.8 42.6 040 020 2020 02 18 17 06 00";
    auto pset_expected = std::vector<std::uint8_t>(pset, pset + std::strlen(pset));
    pset_expected.push_back(0xf7);
    exact_request(EnergyProtocol::pi30revo, pset, pset_expected);

    EnergyCommandInfo info;
    for (unsigned i = static_cast<unsigned>(EnergyProtocol::pi16); i <= static_cast<unsigned>(EnergyProtocol::pi41); ++i) {
        const auto id = static_cast<EnergyProtocol>(i);
        check(pi_poll_command_count(id) >= 5);
        for (std::size_t n = 0; n < pi_poll_command_count(id); ++n) {
            const auto *command = pi_poll_command(id, n);
            check(pi_command_info(id, command, info) && !info.setter);
            request(id, command);
        }
        check(pi_poll_command(id, pi_poll_command_count(id)) == nullptr);
    }
    check(!pi_command_info(EnergyProtocol::pi16, "POP00", info));
    check(pi_command_info(EnergyProtocol::pi30, "POP02", info) && info.setter && !std::strcmp(info.readback_command, "QPIRI"));
    check(!pi_command_info(EnergyProtocol::pi30, "POP03", info));
    check(pi_command_info(EnergyProtocol::pi30m044, "PBT09", info));
    check(!pi_command_info(EnergyProtocol::pi30, "PBT09", info));
    check(pi_command_info(EnergyProtocol::pi17, "PA1", info));
    check(!pi_command_info(EnergyProtocol::pi17infini, "PA1", info));
    check(pi_command_info(EnergyProtocol::pi17infini, "PEA", info));
    check(pi_command_info(EnergyProtocol::pi17infini, "ACCT2200,2259,2300,2359", info));
    check(!pi_command_info(EnergyProtocol::pi17, "ACCT2200,2259,2300,2359", info));
    check(pi_command_info(EnergyProtocol::pi17, "ACCT2200-2359", info));
    check(pi_command_info(EnergyProtocol::pi30max, "PLEDC1000255255", info));
    check(!pi_command_info(EnergyProtocol::pi30max, "PLEDC1000255255x", info));
    check(pi_command_info(EnergyProtocol::pi16, "QEH2026101104", info));
    check(pi_command_info(EnergyProtocol::pi16, "QEM202610", info));
    check(pi_command_info(EnergyProtocol::pi16, "QEY2026", info));
    check(pi_command_info(EnergyProtocol::pi30, "PEab", info));
    check(!pi_command_info(EnergyProtocol::pi30, "PEfoo", info));
    check(!pi_command_info(EnergyProtocol::pi30, "PEaa", info));
    check(!pi_command_info(EnergyProtocol::pi30, "PEa\rPF", info));
    check(pi_command_info(EnergyProtocol::pi30, "DAT20240229010203", info));
    check(!pi_command_info(EnergyProtocol::pi30, "DAT20230229010203", info));
    check(!pi_command_info(EnergyProtocol::pi30, "DAT20241301010203", info));
    check(pi_command_info(EnergyProtocol::pi30m045, "DAT241011010203", info));
    check(!pi_command_info(EnergyProtocol::pi30m045, "DAT20241011010203", info));
    std::uint8_t tiny[2];
    std::size_t written = 99;
    check(pi_build_request(EnergyProtocol::pi30, "QPI", 1, tiny, sizeof(tiny), written) == CodecStatus::buffer_too_small && written == 0);

    EnergyResponse response;
    const char *pi30_status = "000.0 00.0 230.0 49.9 0161 0119 003 460 57.50 012 100 0069 0014 103.8 57.45 00000 00110110 00 00 00856 010";
    decode(EnergyProtocol::pi30, "QPIGS", framed(EnergyProtocol::pi30, pi30_status), response);
    check(std::abs(value(response, "battery_voltage") - 57.5) < 0.0001);
    check(value(response, "ac_output_active_power") == 119);
    check(value(response, "is_load_on") == 1);
    auto corrupted = framed(EnergyProtocol::pi30, pi30_status);
    corrupted[1] ^= 1;
    check(pi_decode_response(EnergyProtocol::pi30, "QPIGS", 1, corrupted.data(), corrupted.size(), response) == CodecStatus::checksum_error);
    decode(EnergyProtocol::pi30, "QPIGS", framed(EnergyProtocol::pi30, "000.0 00.0 230.0 49.9 0161 0119 003 460 --.-- 012 100"), response);
    check(field(response, "battery_voltage") == nullptr);
    auto invalid_numeric = framed(EnergyProtocol::pi30, "bad 00.0 230.0");
    check(pi_decode_response(EnergyProtocol::pi30, "QPIGS", 1, invalid_numeric.data(), invalid_numeric.size(), response) == CodecStatus::invalid_frame && response.count == 0);

    const char *pi17_status = "0000,0000,0000,0000,0394,000,+00000,2389,2427,2459,5002,0000,0000,0000,2378,2434,2455,5001,,,,029,029,000,0";
    for (auto id : {EnergyProtocol::pi17, EnergyProtocol::pi17m058, EnergyProtocol::pi17infini}) {
        decode(id, "GS", framed(id, pi17_status), response);
        check(std::abs(value(response, "battery_voltage") - 39.4) < 0.0001);
        check(field(response, "ac_output_current_r") == nullptr);
    }
    decode(EnergyProtocol::pi17m058, "BATS", framed(EnergyProtocol::pi17m058, "1750,0560,0540,0000,060,0530,0420,0480,0480,0540,0,,,0,0480,000,0100,0175,010,020,020,080,0"), response);
    check(value(response, "battery_maximum_charge_current") == 175);
    for (auto id : {EnergyProtocol::pi17, EnergyProtocol::pi17m058, EnergyProtocol::pi17infini}) {
        for (const char *soc : {"100", "0", "--", "unknown", "101"}) {
            const std::string body = std::string("BMS0522,") + soc + ",-0000,0,0568,0568,00000,0 1 0455 0500";
            decode(id, "BMS", framed(id, body), response);
            const auto *percentage = field(response, "soc");
            if (!std::strcmp(soc, "100") || !std::strcmp(soc, "0")) {
                check(percentage && percentage->numeric);
                check(value(response, "soc") == (!std::strcmp(soc, "100") ? 100 : 0));
            } else check(!percentage || !percentage->numeric);
        }
    }

    const char *pi18_status = "2300,500,2300,500,1000,900,18,540,540,540,2,3,85,35,30,30,600,400,3500,3400,0,2,2,1,1,2,1,0";
    for (auto id : {EnergyProtocol::pi18, EnergyProtocol::pi18lvx, EnergyProtocol::pi18sv}) {
        decode(id, "GS", framed(id, pi18_status), response);
        check(value(response, "battery_voltage") == 54);
    }
    for (auto id : {EnergyProtocol::pi30max, EnergyProtocol::pi30mst, EnergyProtocol::pi30m044, EnergyProtocol::pi30m045}) {
        decode(id, "QPIGS", framed(id, pi30_status), response);
        check(value(response, "battery_voltage") == 57.5);
        decode(id, "QPIGS2", framed(id, "03.1 327.3 01026 "), response);
        check(response.count >= 3);
    }
    decode(EnergyProtocol::pi41, "QPIGS", framed(EnergyProtocol::pi41, pi30_status), response);
    check(value(response, "battery_voltage") == 57.5);
    decode(EnergyProtocol::pi30m044, "QT", framed(EnergyProtocol::pi30m044, "20261011123456"), response);
    check(field(response, "device_time") && !std::strcmp(field(response, "device_time")->text, "2026-10-11 12:34:56"));
    decode(EnergyProtocol::pi30max, "QED20261011", framed(EnergyProtocol::pi30max, "01234567"), response);
    check(field(response, "month") && !std::strcmp(field(response, "month")->text, "October"));

    decode(EnergyProtocol::pi16, "QPIGS", framed(EnergyProtocol::pi16, "224.6 000000 49.9 0006.8 232.4 01594 49.9 006.8 029 415.0 415.0 057.9 ---.- 100 00000 00000 ----- 000.0 000.0 ---.- 035.0 D---110001"), response);
    check(value(response, "positive_battery_voltage") == 57.9);
    const std::vector<std::uint8_t> plain_pi16 = {'(','P','I','1','6',13};
    decode(EnergyProtocol::pi16, "QPI", plain_pi16, response);
    check(field(response, "protocol_version") && !std::strcmp(field(response, "protocol_version")->text, "PI16"));
    decode(EnergyProtocol::pi30revo, "PQSE", checksum_frame("(000100 56.0 54.0 44.0 42.0 030 010 2018 06 01 20 00 00 00", '\n'), response);
    check(value(response, "total_charging_current") == 30);
    check(field(response, "work_mode") && !std::strcmp(field(response, "work_mode")->text, "Utility Priority"));
    decode(EnergyProtocol::pi30revo, "QALL", checksum_frame("(000 00.0 000 00.0 0000 000 00.0 000 000 000 000 00.0 0000 000000 000000 S 00 00", '\n'), response);
    check(value(response, "pv_power") == 0);
    // Reference QBMS tenths-voltage units include a space; temperatures in
    // QLITH0 have a damaged degree sign. Both must retain physical scale.
    for (auto id : {EnergyProtocol::pi30, EnergyProtocol::pi30max, EnergyProtocol::pi30mst,
                    EnergyProtocol::pi30m044, EnergyProtocol::pi30m045, EnergyProtocol::pi41}) {
        decode(id, "QBMS", framed(id, "0 100 0 0 1 532 532 450 0000 0030"), response);
        check(std::abs(value(response, "battery_bulk_charging_voltage_from_bms") - 53.2) < 0.0001);
        check(std::abs(value(response, "battery_float_charging_voltage_from_bms") - 53.2) < 0.0001);
        check(value(response, "battery_cut_off_voltage_from_bms") == 45);
        check(!std::strcmp(field(response, "battery_bulk_charging_voltage_from_bms")->unit, "V"));
    }
    decode(EnergyProtocol::pi30revo, "QLITH0", framed(EnergyProtocol::pi30revo,
        "048.8 000.0 000.9 250 024 000 052.5 010.0 053.5 004.4 0 2"), response);
    check(value(response, "battery_temperature") == 25);
    check(!std::strcmp(field(response, "battery_temperature")->unit, "C"));
    decode(EnergyProtocol::pi30revo, "QLITH0", framed(EnergyProtocol::pi30revo,
        "052.5 000.0 000.0 032 036 000 070.0 007.0 057.0 004.6 0 5"), response);
    check(std::abs(value(response, "battery_temperature") - 3.2) < 0.0001);

    const auto ack = framed(EnergyProtocol::pi30, "ACK");
    check(pi_decode_response(EnergyProtocol::pi30, "POP02", 1, ack.data(), ack.size(), response) == CodecStatus::ok && response.acknowledged);
    // Auto-discovered USB interfaces must answer a read with observations;
    // a checksum-valid ACK or empty reply cannot establish device readiness.
    for (unsigned raw = static_cast<unsigned>(EnergyProtocol::pi16);
         raw <= static_cast<unsigned>(EnergyProtocol::pi41); ++raw) {
        const auto id = static_cast<EnergyProtocol>(raw);
        const char *query = pi_poll_command(id, 0);
        for (const char *payload : {"ACK", ""}) {
            const auto read_reply = framed(id, payload);
            check(pi_decode_response(id, query, 1, read_reply.data(), read_reply.size(), response)
                == CodecStatus::invalid_frame && response.count == 0 && !response.acknowledged);
        }
    }
    const auto nak = framed(EnergyProtocol::pi30, "NAK");
    check(pi_decode_response(EnergyProtocol::pi30, "POP02", 1, nak.data(), nak.size(), response) == CodecStatus::rejected && response.rejected);
    for (auto id : {EnergyProtocol::pi17, EnergyProtocol::pi18}) {
        const std::uint8_t caret_ack[] = {'^','1',0x0b,0xc2,13};
        const std::uint8_t caret_nak[] = {'^','0',0x1b,0xe3,13};
        const char *command = id == EnergyProtocol::pi17 ? "LON1" : "POP1";
        check(pi_decode_response(id, command, 1, caret_ack, sizeof(caret_ack), response) == CodecStatus::ok && response.acknowledged);
        check(pi_decode_response(id, command, 1, caret_nak, sizeof(caret_nak), response) == CodecStatus::rejected && response.rejected);
    }
    std::puts("PI protocol tests passed");
    std::printf("%d checks across all 14 PI profiles\n", checks);
}
