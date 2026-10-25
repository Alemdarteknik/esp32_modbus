// Settings regression fixtures are synthetic. This test never opens a transport.
struct SetterVector {
    EnergyProtocol id;
    const char *key;
    const char *command;
    const char *readback;
    const char *prefix;
    bool no_crc;
};
#include "pi_setter_vectors.generated.inc"

std::vector<std::uint8_t> expected_setting_request(const SetterVector &item)
{
    std::string body = item.command;
    const bool pi17 = item.id >= EnergyProtocol::pi17 && item.id <= EnergyProtocol::pi17infini;
    const bool pi18 = item.id >= EnergyProtocol::pi18 && item.id <= EnergyProtocol::pi18sv;
    if (pi17) {
        char prefix[16]{};
        std::snprintf(prefix, sizeof(prefix), "^S%03u", static_cast<unsigned>(body.size() + 1));
        body = prefix + body;
    } else if (pi18) body = item.prefix + body;
    std::vector<std::uint8_t> result(body.begin(), body.end());
    if (item.id == EnergyProtocol::pi30revo) {
        unsigned sum = 1;
        for (const auto byte : result) sum += byte;
        result.push_back(sum & 255);
    } else if (!pi17 && !(pi18 && item.no_crc)) {
        const auto sum = crc(result.data(), result.size());
        result.push_back(escape(sum >> 8)); result.push_back(escape(sum & 255));
    }
    if (item.id != EnergyProtocol::pi30revo || std::strncmp(item.command, "PSET", 4)) result.push_back('\r');
    return result;
}

void every_setting_wire_test()
{
    unsigned setters = 0;
    for (const auto &item : setter_vectors) {
        ++setters;
        EnergyCommandInfo info{};
        check(pi_command_info(item.id, item.command, info) && info.setter);
        check(!std::strcmp(info.name, !std::strcmp(item.key, "_PSET") ? "PSET" : item.key));
        check((!item.readback && !info.readback_command) ||
              (item.readback && info.readback_command && !std::strcmp(item.readback, info.readback_command)));
        check(request(item.id, item.command) == expected_setting_request(item));
        for (const char *suffix : {"|junk", "\rPF", "\n"}) {
            const auto bad = std::string(item.command) + suffix;
            check(!pi_command_info(item.id, bad.c_str(), info));
            std::uint8_t bytes[160]{}; std::size_t size = 99;
            check(pi_build_request(item.id, bad.c_str(), 1, bytes, sizeof(bytes), size)
                == CodecStatus::invalid_command && !size);
        }
        for (bool accepted : {true, false}) {
            EnergyResponse response{};
            const bool caret = item.id >= EnergyProtocol::pi17 && item.id <= EnergyProtocol::pi18sv;
            auto reply = caret ? std::vector<std::uint8_t>{'^', accepted ? std::uint8_t('1') : std::uint8_t('0'),
                accepted ? std::uint8_t(0x0b) : std::uint8_t(0x1b),
                accepted ? std::uint8_t(0xc2) : std::uint8_t(0xe3), '\r'}
                : framed(item.id, accepted ? "ACK" : "NAK");
            const auto expected = accepted ? CodecStatus::ok : CodecStatus::rejected;
            check(pi_decode_response(item.id, item.command, 1, reply.data(), reply.size(), response) == expected);
            check(response.acknowledged == accepted && response.rejected != accepted && !response.count);
            reply[reply.size() - 2] ^= 1;
            check(pi_decode_response(item.id, item.command, 1, reply.data(), reply.size(), response) == CodecStatus::checksum_error);
        }
    }
    check(setters == 326);
    std::printf("%u setter definitions: valid requests, exact framing, ACK/NAK, damaged replies, rejected suffixes\n", setters);
}

using Override = std::pair<unsigned, const char *>;
unsigned verified_setting_cases = 0;
void setting_equal(EnergyProtocol id, const char *setting, const char *query,
                   std::initializer_list<Override> values, unsigned count = 30)
{
    EnergyCommandInfo info{};
    check(pi_command_info(id, setting, info) && info.setter);
    check(info.readback_command && !std::strcmp(info.readback_command, query));
    std::vector<std::string> tokens(count, "0");
    for (const auto &item : values) { check(item.first < count); tokens[item.first] = item.second; }
    const bool comma = id >= EnergyProtocol::pi17 && id <= EnergyProtocol::pi18sv;
    std::string body;
    for (const auto &token : tokens) { if (!body.empty()) body += comma ? ',' : ' '; body += token; }
    EnergyResponse response{};
    decode(id, query, framed(id, body), response);
    bool equal = false;
    if (!pi_setting_readback(id, setting, query, response, equal) || !equal) {
        std::printf("Readback comparison failed: profile %u, %s, query %s\n", static_cast<unsigned>(id), setting, query);
        std::fflush(stdout);
    }
    check(pi_setting_readback(id, setting, query, response, equal) && equal);
    ++verified_setting_cases;
    auto changed = response;
    for (std::size_t i = 0; i < changed.count; ++i) {
        if (changed.fields[i].numeric) changed.fields[i].value += 1;
        else std::strcpy(changed.fields[i].text, "changed");
    }
    check(pi_setting_readback(id, setting, query, changed, equal) && !equal);
    changed = response; changed.count = 0;
    check(!pi_setting_readback(id, setting, query, changed, equal) && !equal);
    changed = response; changed.acknowledged = true;
    check(!pi_setting_readback(id, setting, query, changed, equal));
    changed = response; changed.rejected = true;
    check(!pi_setting_readback(id, setting, query, changed, equal));
    changed = response;
    for (std::size_t i = 0; i < changed.count; ++i) std::strcpy(changed.fields[i].unit, "wrong");
    check(!pi_setting_readback(id, setting, query, changed, equal));
    changed = response;
    for (std::size_t i = 0; i < changed.count; ++i) changed.fields[i].numeric = !changed.fields[i].numeric;
    check(!pi_setting_readback(id, setting, query, changed, equal));
    changed = response;
    check(changed.count * 2 <= EnergyResponse::maximum_fields);
    for (std::size_t i = 0; i < response.count; ++i) changed.fields[changed.count++] = response.fields[i];
    check(!pi_setting_readback(id, setting, query, changed, equal));
    check(!pi_setting_readback(id, setting, "QPI", response, equal));
}

void known_setting_readbacks_test()
{
    for (auto id : {EnergyProtocol::pi17, EnergyProtocol::pi17m058, EnergyProtocol::pi17infini}) {
        setting_equal(id, "GPMP000500", "GPMP", {{0,"500"}}, 1);
        setting_equal(id, "LON0", "PS", {{16,"0"}});
        if (id != EnergyProtocol::pi17infini) {
            setting_equal(id, "PA1", "FLAG", {{0,"1"}});
            setting_equal(id, "PB1", "FLAG", {{1,"1"}});
            setting_equal(id, "ACCT2200-0259", "DI", {{16,"2200"},{17,"0259"}});
        } else {
            setting_equal(id, "PEA", "FLAG", {{0,"1"}});
            setting_equal(id, "PDA", "FLAG", {{0,"0"}});
        }
        setting_equal(id, "PC1", "FLAG", {{2,"1"}});
        setting_equal(id, "PF1", "FLAG", {{5,"1"}});
        setting_equal(id, "PEM", "FLAG", {{11,"1"}});
        setting_equal(id, "PDN", "FLAG", {{12,"0"}});
        setting_equal(id, "LST00", "DI", {{9,"0"}});
        setting_equal(id, "LST20", "DI", {{9,"20"}});
        setting_equal(id, "SEP01", "HECS", {{0,"01"}});
        for (char flag = 'A'; flag <= 'G'; ++flag) {
            char command[] = "EDA1"; command[2] = flag;
            setting_equal(id, command, "HECS", {{static_cast<unsigned>(flag - 'A' + 1),"1"}});
        }
        setting_equal(id, "BT1", "BATS", {{10,"1"}});
        setting_equal(id, "MCHGV0576,0540", "BATS", {{1,"0576"},{2,"0540"}});
        setting_equal(id, "MCHGC1200", "BATS", {{0,"1200"}});
        setting_equal(id, "MUCHGC0600", "BATS", {{16,"0600"}});
        setting_equal(id, "BDCM0100", "BATS", {{17,"0100"}});
        setting_equal(id, "BATDV0420,0440,0430,0450", "BATS", {{6,"0420"},{7,"0440"},{8,"0430"},{9,"0450"}});
        setting_equal(id, "ACCB1,0450", "BATS", {{13,"1"},{14,"0450"}});
        setting_equal(id, "BCA0000,060,0530", "BATS", {{3,"0000"},{4,"060"},{5,"0530"}});
    }
    for (auto id : {EnergyProtocol::pi18, EnergyProtocol::pi18lvx, EnergyProtocol::pi18sv}) {
        if (id == EnergyProtocol::pi18) {
            setting_equal(id, "PBT2", "PIRI", {{13,"2"}});
            setting_equal(id, "PSP1", "PIRI", {{23,"1"}});
            setting_equal(id, "PSDV450", "PIRI", {{10,"450"}});
        } else {
            setting_equal(id, "LON1", "GS", {{23,"1"}});
            for (char flag = 'A'; flag <= 'H'; ++flag) {
                char command[] = "PEA"; command[2] = flag;
                setting_equal(id, command, "FLAG", {{static_cast<unsigned>(flag - 'A'),"1"}});
                command[1] = 'D';
                setting_equal(id, command, "FLAG", {{static_cast<unsigned>(flag - 'A'),"0"}});
            }
            setting_equal(id, "MCHGC1,030", "PRI1", {{0,"1"},{4,"30"}});
            setting_equal(id, "MUCHGC1,030", "PRI1", {{0,"1"},{5,"30"}});
            setting_equal(id, "MCHGC0,030", "PRI0", {{0,"1"},{4,"30"}});
            setting_equal(id, "MUCHGC0,030", "PRI0", {{0,"1"},{5,"30"}});
        }
        setting_equal(id, "POP1", "PIRI", {{17,"1"}});
        setting_equal(id, "MCHGV552,540", "PIRI", {{11,"552"},{12,"540"}});
        setting_equal(id, "BUCD440,480", "PIRI", {{8,"440"},{9,"480"}});
    }
    for (auto id : {EnergyProtocol::pi30, EnergyProtocol::pi30max, EnergyProtocol::pi30mst,
                   EnergyProtocol::pi30m044, EnergyProtocol::pi30m045, EnergyProtocol::pi41}) {
        setting_equal(id, "F50", "QPIRI", {{3,"50"}});
        setting_equal(id, "MCHGC040", "QPGS0", {{0,"1"},{22,"40"}});
        setting_equal(id, "MCHGC130", "QPGS1", {{0,"1"},{22,"30"}});
        setting_equal(id, "MNCHGC0120", "QPGS0", {{0,"1"},{24,"120"}});
        setting_equal(id, "MNCHGC1120", "QPGS1", {{0,"1"},{24,"120"}});
        setting_equal(id, id == EnergyProtocol::pi30m045 ? "MUCHGC0030" : "MUCHGC030", "QPGS0", {{0,"1"},{24,"30"}});
        setting_equal(id, id == EnergyProtocol::pi30m045 ? "MUCHGC1030" : "MUCHGC130", "QPGS1", {{0,"1"},{24,"30"}});
        setting_equal(id, "PBCV44.0", "QPIRI", {{8,"44.0"}});
        setting_equal(id, "PBDV48.0", "QPIRI", {{22,"48.0"}});
        setting_equal(id, "PBFT54.0", "QPIRI", {{11,"54.0"}});
        setting_equal(id, "PCVV56.0", "QPIRI", {{10,"56.0"}});
        setting_equal(id, "PSDV40.0", "QPIRI", {{9,"40.0"}});
        setting_equal(id, "PBT02", "QPIRI", {{12,"2"}});
        setting_equal(id, "PCP01", "QPIRI", {{17,"1"}});
        setting_equal(id, "PGR00", "QPIRI", {{15,"0"}});
        setting_equal(id, "POP02", "QPIRI", {{16,"2"}});
        if (id != EnergyProtocol::pi41) setting_equal(id, "POPLG01", "QPIRI", {{26,"1"}});
        setting_equal(id, "POPM01", "QPGS0", {{0,"1"},{20,"1"}});
        setting_equal(id, "POPM11", "QPGS1", {{0,"1"},{20,"1"}});
        setting_equal(id, "PPCP001", "QPGS0", {{0,"1"},{21,"1"}});
        setting_equal(id, "PPCP101", "QPGS1", {{0,"1"},{21,"1"}});
        setting_equal(id, "PPVOKC1", "QPIRI", {{23,"1"}});
        setting_equal(id, "PSPB1", "QPIRI", {{24,"1"}});
        setting_equal(id, "PEab", "QFLAG", {{0,"Eab"}}, 1);
        setting_equal(id, "PDab", "QFLAG", {{0,"Dab"}}, 1);
        setting_equal(id, "PBATMAXDISC100", "QBMS", {{9,"100"}}, 10);
        if (id != EnergyProtocol::pi30 && id != EnergyProtocol::pi41) {
            setting_equal(id, "PLEDE1", "QLED", {{0,"1"}}, 6);
            setting_equal(id, "PLEDS1", "QLED", {{1,"1"}}, 6);
            setting_equal(id, "PLEDM2", "QLED", {{2,"2"}}, 6);
            setting_equal(id, "PLEDB5", "QLED", {{3,"5"}}, 6);
            setting_equal(id, "PLEDT2", "QLED", {{4,"2"}}, 6);
        }
    }
    std::printf("%u verified setter/readback cases, including mismatches and unavailable/malformed observations\n", verified_setting_cases);
}

void setting_validation_test()
{
    EnergyCommandInfo info{};
    for (auto id : {EnergyProtocol::pi17, EnergyProtocol::pi17m058, EnergyProtocol::pi17infini}) {
        check(pi_command_info(id, "LST00", info));
        check(!pi_command_info(id, "LST03", info));
        check(pi_command_info(id, "ACCB1,0600", info));
        check(!pi_command_info(id, "ACCB1,0601", info));
        check(!pi_command_info(id, "ACCB1,0699", info));
        for (const char *bad : {"ACCT100-0259", "ACCT2400-0259", "ACCT2200-0260", "DAT260229123400"})
            check(!pi_command_info(id, bad, info));
    }
    check(pi_command_info(EnergyProtocol::pi18, "MCHGV584,584", info));
    check(!pi_command_info(EnergyProtocol::pi18, "MCHGV585,584", info));
    for (auto id : {EnergyProtocol::pi18lvx, EnergyProtocol::pi18sv}) {
        for (const char *bad : {"POP2", "BUCD000,000", "BUCD511,480", "BUCD440,581"})
            check(!pi_command_info(id, bad, info));
        check(pi_command_info(id, "BUCD510,580", info));
        check(pi_command_info(id, "BUCD440,000", info));
    }
    for (auto id : {EnergyProtocol::pi30, EnergyProtocol::pi30max, EnergyProtocol::pi30mst,
                   EnergyProtocol::pi30m044, EnergyProtocol::pi30m045, EnergyProtocol::pi41}) {
        for (const char *bad : {"PBATMAXDISC001", "PBATMAXDISC029", "PBATMAXDISC151", "PBATMAXDISC199"})
            check(!pi_command_info(id, bad, info));
        for (const char *good : {"PBATMAXDISC000", "PBATMAXDISC030", "PBATMAXDISC150"})
            check(pi_command_info(id, good, info));
    }
    for (auto id : {EnergyProtocol::pi30max, EnergyProtocol::pi30mst, EnergyProtocol::pi30m044, EnergyProtocol::pi30m045}) {
        for (const char *bad : {"PLEDC0000255255", "PLEDC4000255255", "PLEDC1256255255", "PLEDC1000256255", "PLEDC1000255256"})
            check(!pi_command_info(id, bad, info));
        for (const char *good : {"PLEDC1000000000", "PLEDC3255255255"}) check(pi_command_info(id, good, info));
    }
    for (const char *bad : {
        "PSET120103 56.3 54.6 43.8 42.6 040 020 2026 02 29 12 34 00",
        "PSET120103 56.3 54.6 43.8 42.6 040 020 2026 10 14 24 34 00",
        "PSET120103 56.3 54.6 43.8 42.6 040 020 2026 10 14 12 34 00junk",
        "PSET120103 56.3 54.6 43.8 42.6 040 020 2026 10 14 12 34 00 PF"})
        check(!pi_command_info(EnergyProtocol::pi30revo, bad, info));
    check(pi_command_info(EnergyProtocol::pi30revo,
        "PSET120103 56.3 54.6 43.8 42.6 040 020 2024 02 29 12 34 00 48.0", info));
    check(pi_command_info(EnergyProtocol::pi18, "MCHGC1,030", info) && !info.readback_command);
    check(pi_command_info(EnergyProtocol::pi18, "MCHGC0,030", info) && !info.readback_command);
    EnergyResponse response{}; bool matches = true;
    for (const char *unknown : {"PF", "PSAVE", "PBATCD000", "BTA+01", "DAT20261014123400"})
        check(!pi_setting_readback(EnergyProtocol::pi30, unknown, "QPIRI", response, matches) && !matches);

    // A partial compound setting cannot pass because its first field matches.
    decode(EnergyProtocol::pi17, "BATS", framed(EnergyProtocol::pi17, "0,0576"), response);
    check(!pi_setting_readback(EnergyProtocol::pi17, "MCHGV0576,0540", "BATS", response, matches) && !matches);
    decode(EnergyProtocol::pi30, "QFLAG", framed(EnergyProtocol::pi30, "Ea"), response);
    check(!pi_setting_readback(EnergyProtocol::pi30, "PEab", "QFLAG", response, matches) && !matches);

    // Bit meanings for this command and the calibration's pre-write baseline
    // are not documented by these responses. Observations do not establish it.
    response = {}; check(response.add_number("Battery Voltage", "V", 52));
    check(!pi_setting_readback(EnergyProtocol::pi30, "BTA+01", "QPIGS", response, matches));
    decode(EnergyProtocol::pi30, "QBMS", framed(EnergyProtocol::pi30, "0 100 0 0 0 532 532 450 0 0"), response);
    check(!pi_setting_readback(EnergyProtocol::pi30, "PBATCD000", "QBMS", response, matches));
    check(!pi_setting_readback(EnergyProtocol::pi30, "PBATMAXDISC000", "QBMS", response, matches));
    decode(EnergyProtocol::pi30revo, "PQSE", framed(EnergyProtocol::pi30revo,
        "120103 56.3 54.6 43.8 42.6 040 020 2026 10 14 12 34 00"), response);
    check(!pi_setting_readback(EnergyProtocol::pi30revo,
        "PSET120103 56.3 54.6 43.8 42.6 040 020 2026 10 14 12 34 00", "PQSE", response, matches));
}

void setting_tests()
{
    every_setting_wire_test();
    setting_validation_test();
    known_setting_readbacks_test();
}
