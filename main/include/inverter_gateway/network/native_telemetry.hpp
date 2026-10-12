#pragma once

#include "inverter_gateway/protocol/energy_protocol.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace inverter_gateway::network {

// Decoders retain their manufacturer field labels. Only matching uses a common
// spelling; neither the observation nor its electrical unit is rewritten.
inline void native_field_key(const char *source, char (&key)[40])
{
    std::size_t used = 0;
    bool separator = false;
    for (; *source && used + 1 < sizeof(key); ++source) {
        const auto c = static_cast<unsigned char>(*source);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            if (separator && used && used + 2 < sizeof(key)) key[used++] = '_';
            key[used++] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : static_cast<char>(c);
            separator = false;
        } else separator = true;
    }
    key[used] = 0;
}

inline bool native_bms_profile(protocol::EnergyProtocol profile)
{
    return profile >= protocol::EnergyProtocol::jk02 && profile <= protocol::EnergyProtocol::daly40;
}

inline const char *native_field_section(protocol::EnergyProtocol profile, const char *name, const char *unit)
{
    char key[40]{}; native_field_key(name, key);
    const auto eq = [&](const char *candidate) { return std::strcmp(key, candidate) == 0; };
    if (eq("battery_bulk_charging_voltage_from_bms") || eq("battery_float_charging_voltage_from_bms") ||
        eq("battery_cut_off_voltage_from_bms") || eq("battery_max_charging_current") ||
        eq("battery_max_discharge_current") || eq("battery_max_charge_voltage") ||
        eq("battery_min_discharge_voltage")) return "settings";
    if (eq("serial_number") || eq("protocol_id") || eq("protocol_version") ||
        eq("firmware_version") || eq("software_version") || eq("hardware_version") ||
        eq("device_model") || eq("model") || eq("model_code") || eq("device_name") || eq("manufacturing_date") ||
        eq("production_date") || eq("up_time") || eq("power_on_times")) return "identity";
    if (profile == protocol::EnergyProtocol::ved) {
        if (eq("pid") || eq("fw") || eq("ser")) return "identity";
        if (key[0] == 'h' && key[1] >= '0' && key[1] <= '9') return "energy";
        if (eq("cs") || eq("err") || eq("alarm") || eq("ar") || eq("relay") ||
            eq("or") || eq("hsds") || eq("mppt")) return "status";
        if (eq("ppv") || eq("vpv")) return "pv";
        if (std::strncmp(key, "ac_out_", 7) == 0) return "output";
        if (eq("load") || eq("il")) return "load";
        if (eq("v") || eq("v2") || eq("v3") || eq("vs") || eq("vm") || eq("i") ||
            eq("i2") || eq("i3") || eq("p") || eq("soc") || eq("t") || eq("ce") || eq("ttg")) return "battery";
    }
    if (!std::strcmp(unit, "Wh") || !std::strcmp(unit, "kWh") || std::strstr(key, "energy")) return "energy";
    if (std::strncmp(key, "is_", 3) == 0 || !std::strcmp(unit, "bool") ||
        std::strstr(key, "fault") || std::strstr(key, "warning") || std::strstr(key, "alarm") ||
        std::strstr(key, "protection") || std::strstr(key, "status") || std::strstr(key, "enabled") ||
        std::strstr(key, "direction") || std::strstr(key, "mode") || std::strstr(key, "connection") || std::strstr(key, "flag") ||
        eq("setting_change_bit") || eq("setting_value_configuration_state") || eq("balancer_active")) return "status";
    if (native_bms_profile(profile)) return "battery";
    if (eq("solar_feed_to_grid_power") || eq("actfeedpow")) return "grid";
    if (eq("actpvpow")) return "pv";
    if (std::strncmp(key, "pv", 2) == 0 || std::strstr(key, "solar") || std::strstr(key, "scc") || std::strstr(key, "mppt"))
        return std::strncmp(key, "battery_", 8) == 0 ? "battery" : "pv";
    if (std::strstr(key, "battery") || std::strstr(key, "cell") || eq("soc") ||
        std::strstr(key, "capacity") || std::strstr(key, "balance")) return "battery";
    if (std::strstr(key, "grid") || std::strstr(key, "ac_input") || std::strncmp(key, "input_current", 13) == 0 ||
        std::strncmp(key, "utility", 7) == 0 || std::strncmp(key, "line", 4) == 0) return "grid";
    if (std::strstr(key, "load") || eq("ac_output_power_percentage") || eq("total_ac_output_percentage")) return "load";
    if (std::strstr(key, "output")) return "output";
    if (std::strstr(key, "temp") || std::strstr(key, "heat_sink")) return "status";
    return "other";
}

class NativePowerSummary {
public:
    struct Reading {
        double value{};
        std::uint32_t age_ms{};
        const char *source_field{};
        unsigned priority{};
        bool present{};
        std::uint32_t observation{};
        bool fresh() const { return present && age_ms <= 15000; }
    };
    enum Metric : std::size_t { pv, grid, grid_apparent, output, output_apparent, load_percentage, battery, metric_count };
    static constexpr const char *names[metric_count] = {"pv_kw", "grid_kw", "grid_apparent_kva", "load_kw", "load_apparent_kva", "load_percentage", "battery_kw"};

    explicit NativePowerSummary(protocol::EnergyProtocol profile) : profile_(profile) {}

    void observe(const protocol::EnergyField &field, std::uint32_t age, std::uint32_t observation = 0)
    {
        char key[40]{}; native_field_key(field.key, key);
        const auto eq = [&](const char *name) { return std::strcmp(key, name) == 0; };
        if (eq("battery_power_direction") && !field.numeric) {
            const int direction = !std::strcmp(field.text, "Charging") ? -1 :
                !std::strcmp(field.text, "Discharging") ? 1 : !std::strcmp(field.text, "Idle") ? 0 : 2;
            if (direction != 2) save(battery_direction_, direction, age, field.key, 1, observation);
        }
        if (!field.numeric || !std::isfinite(field.value)) return;
        const bool active = !std::strcmp(field.unit, "W") || !std::strcmp(field.unit, "kW");
        const bool apparent = !std::strcmp(field.unit, "VA") || !std::strcmp(field.unit, "kVA");
        const double value = (!std::strcmp(field.unit, "W") || !std::strcmp(field.unit, "VA")) ? field.value / 1000.0 : field.value;
        const auto save = [&](Reading &target, unsigned priority = 1) {
            NativePowerSummary::save(target, value, age, field.key, priority, observation);
        };
        if (!std::strcmp(field.unit, "V")) {
            if (eq("battery_voltage") || eq("total_battery_voltage") || eq("battery_bank_voltage") ||
                eq("total_voltage") ||
                (profile_ == protocol::EnergyProtocol::ved && eq("v"))) save(battery_voltage_);
            if (eq("battery_voltage_from_bms")) save(bms_voltage_);
            if (eq("ac_input_voltage") || eq("grid_voltage")) save(grid_voltages_[0]);
            for (unsigned i = 0; i < 3; ++i) {
                char alias[40]{}; std::snprintf(alias, sizeof(alias), "ac_input_voltage_%c", "rst"[i]);
                if (eq(alias)) save(grid_voltages_[i]);
            }
        }
        if (!std::strcmp(field.unit, "A")) {
            if (eq("battery_charging_current") || eq("battery_charge_current")) save(battery_charge_current_);
            if (eq("battery_discharge_current") || eq("battery_discharging_current")) save(battery_discharge_current_);
            if (eq("battery_charging_current_from_bms")) save(bms_charge_current_);
            if (eq("battery_discharge_current_from_bms")) save(bms_discharge_current_);
            if (eq("battery_current") || eq("total_current") || eq("current") ||
                (profile_ == protocol::EnergyProtocol::ved && eq("i"))) save(battery_current_);
            if (eq("ac_input_current") || eq("grid_current")) save(grid_currents_[0]);
            for (unsigned i = 0; i < 3; ++i) {
                char alias[40]{}; std::snprintf(alias, sizeof(alias), "ac_input_current_%c", "rst"[i]);
                if (eq(alias)) save(grid_currents_[i]);
            }
        }
        if (active) {
            if (eq("pv_total_charging_power")) save(direct_[pv], 2);
            else if (eq("pv_input_power") || eq("pv_power") || eq("solar_power") ||
                     (profile_ == protocol::EnergyProtocol::ved && eq("ppv"))) save(direct_[pv]);
            if (eq("ac_input_total_active_power")) save(direct_[grid], 2);
            else if (eq("grid_active_power") || eq("ac_input_active_power") || eq("grid_power")) save(direct_[grid]);
            if (eq("ac_output_total_active_power") || eq("total_output_active_power") || eq("total_ac_output_active_power")) save(direct_[output], 2);
            else if (eq("ac_output_active_power") || eq("output_active_power") || eq("output_power")) save(direct_[output]);
            if (eq("total_battery_power")) save(direct_[battery], 2);
            else if (eq("battery_power") || (profile_ == protocol::EnergyProtocol::ved && eq("p"))) save(direct_[battery]);
            for (unsigned i = 0; i < 3; ++i) {
                char alias[40]{};
                std::snprintf(alias, sizeof(alias), "pv%u_input_power", i + 1);
                if (eq(alias)) save(pv_channels_[i]);
                std::snprintf(alias, sizeof(alias), "pv%u_charging_power", i + 1);
                if (eq(alias)) save(pv_channels_[i]);
                std::snprintf(alias, sizeof(alias), "solar_input_power_%u", i + 1);
                if (eq(alias)) save(pv_channels_[i]);
            }
            if (eq("l1_ac_output_active_power")) save(output_phases_[0]);
            if (eq("l2_ac_output_active_power")) save(output_phases_[1]);
        }
        if (apparent) {
            if (eq("ac_input_total_apparent_power")) save(direct_[grid_apparent], 2);
            else if (eq("grid_apparent_power") || eq("ac_input_apparent_power")) save(direct_[grid_apparent]);
            if (eq("ac_output_total_apparent_power") || eq("total_ac_output_apparent_power") || eq("total_output_apparent_power")) save(direct_[output_apparent], 2);
            else if (eq("ac_output_apparent_power") || eq("output_apparent_power") ||
                     (profile_ == protocol::EnergyProtocol::ved && eq("ac_out_s"))) save(direct_[output_apparent]);
            if (eq("l1_ac_output_apparent_power")) save(apparent_phases_[0]);
            if (eq("l2_ac_output_apparent_power")) save(apparent_phases_[1]);
        }
        if (!std::strcmp(field.unit, "%") && (eq("ac_output_load") || eq("output_load_percent") ||
            eq("load_percentage") || eq("ac_output_power_percentage") || eq("total_ac_output_percentage") ||
            eq("total_output_load_percent"))) save(direct_[load_percentage]);
    }

    Reading reading(Metric metric) const
    {
        const auto direct = metric == battery ? signed_battery() : direct_[metric];
        if (direct.fresh()) return direct;
        Reading aggregate{};
        if (metric == pv) {
            unsigned count = 0;
            using Id = protocol::EnergyProtocol;
            if (profile_ == Id::pi16 || profile_ == Id::pi30mst) count = 3;
            else if ((profile_ >= Id::pi17 && profile_ <= Id::pi18sv) || profile_ == Id::pi30max ||
                     profile_ == Id::pi30m044 || profile_ == Id::pi30m045) count = 2;
            if (count) aggregate = sum(pv_channels_.data(), count, "pv_channel_sum");
        } else if (metric == battery) aggregate = calculated_battery();
        else if (metric == grid_apparent) aggregate = calculated_grid_apparent();
        else if (profile_ == protocol::EnergyProtocol::pi41) {
            if (metric == output) aggregate = sum(output_phases_.data(), 2, "output_phase_sum");
            if (metric == output_apparent) aggregate = sum(apparent_phases_.data(), 2, "output_phase_sum");
        }
        return aggregate.fresh() || !direct.present ? aggregate : direct;
    }

private:
    static void save(Reading &target, double value, std::uint32_t age, const char *source,
                     unsigned priority, std::uint32_t observation)
    {
        Reading incoming{value, age, source, priority, true, observation};
        if (!target.present || (incoming.fresh() && !target.fresh()) ||
            (incoming.fresh() == target.fresh() && (priority > target.priority ||
             (priority == target.priority && age < target.age_ms)))) target = incoming;
    }

    static bool coherent(const Reading &left, const Reading &right)
    {
        return left.present && right.present && left.observation != 0 &&
            left.observation == right.observation;
    }

    Reading signed_battery() const
    {
        auto power = direct_[battery];
        if (!power.present) return {};
        using Id = protocol::EnergyProtocol;
        // JK's raw power word is unsigned and cannot establish direction.
        if (profile_ == Id::jk02 || profile_ == Id::jk02_32 || profile_ == Id::jkv11 || profile_ == Id::jkpb)
            return {};
        if (profile_ == Id::ved) {
            power.value = -power.value;
            if (power.value == 0) power.value = 0; // Avoid negative zero.
            power.source_field = "p_charge_positive_inverted";
        }
        if (profile_ >= Id::pi17 && profile_ <= Id::pi17infini) {
            if (power.value == 0) return power;
            if (!coherent(power, battery_direction_) || battery_direction_.value == 0) return {};
            power.value = std::fabs(power.value) * battery_direction_.value;
            power.age_ms = std::max(power.age_ms, battery_direction_.age_ms);
            power.source_field = "battery_power_with_direction";
        }
        return power;
    }

    Reading calculated_battery() const
    {
        using Id = protocol::EnergyProtocol;
        const bool charge_positive = profile_ == Id::jk02 || profile_ == Id::jk02_32 ||
            profile_ == Id::jkv11 || profile_ == Id::jkpb || profile_ == Id::jk232 ||
            profile_ == Id::jkserial || profile_ == Id::ved;
        const bool discharge_positive = profile_ == Id::daly || profile_ == Id::daly40;
        if ((charge_positive || discharge_positive) && battery_voltage_.value > 0 &&
            coherent(battery_voltage_, battery_current_)) {
            Reading power = battery_voltage_;
            power.value *= battery_current_.value * (charge_positive ? -1 : 1) / 1000.0;
            if (power.value == 0) power.value = 0;
            power.age_ms = std::max(power.age_ms, battery_current_.age_ms);
            power.source_field = charge_positive ? "battery_voltage_times_charge_positive_current" :
                                                  "battery_voltage_times_discharge_positive_current";
            return power;
        }
        auto power = net_battery(battery_voltage_, battery_charge_current_, battery_discharge_current_,
                                 "battery_voltage_times_net_current");
        if (profile_ == Id::pi30revo) {
            const auto bms = net_battery(bms_voltage_, bms_charge_current_, bms_discharge_current_,
                                         "bms_battery_voltage_times_net_current");
            if ((!power.fresh() && bms.fresh()) || (!power.present && bms.present)) return bms;
        }
        return power;
    }

    static Reading net_battery(const Reading &voltage, const Reading &charge,
                               const Reading &discharge, const char *source)
    {
        if (voltage.value <= 0 || !coherent(voltage, charge) || !coherent(voltage, discharge) ||
            charge.value < 0 || discharge.value < 0) return {};
        Reading power = voltage;
        power.value *= (discharge.value - charge.value) / 1000.0;
        power.age_ms = std::max(power.age_ms, std::max(charge.age_ms, discharge.age_ms));
        power.source_field = source;
        return power;
    }

    Reading calculated_grid_apparent() const
    {
        using Id = protocol::EnergyProtocol;
        const unsigned count = profile_ >= Id::pi17 && profile_ <= Id::pi17infini ? 3 : 1;
        Reading total{};
        for (unsigned i = 0; i < count; ++i) {
            const auto &voltage = grid_voltages_[i], &current = grid_currents_[i];
            if (!coherent(voltage, current) || voltage.value < 0 ||
                (i && voltage.observation != total.observation)) return {};
            total.value += voltage.value * std::fabs(current.value) / 1000.0;
            total.age_ms = std::max(total.age_ms, std::max(voltage.age_ms, current.age_ms));
            total.observation = voltage.observation;
        }
        total.present = true;
        total.source_field = "grid_voltage_times_current";
        return total;
    }

    static Reading sum(const Reading *parts, unsigned count, const char *source)
    {
        Reading total{}; total.source_field = source;
        for (unsigned i = 0; i < count; ++i) {
            if (!parts[i].present) return {};
            total.value += parts[i].value;
            if (parts[i].age_ms > total.age_ms) total.age_ms = parts[i].age_ms;
        }
        total.present = true;
        return total;
    }
    protocol::EnergyProtocol profile_;
    std::array<Reading, metric_count> direct_{};
    std::array<Reading, 3> pv_channels_{};
    std::array<Reading, 2> output_phases_{}, apparent_phases_{};
    Reading battery_voltage_{}, battery_charge_current_{}, battery_discharge_current_{},
        battery_current_{}, battery_direction_{};
    Reading bms_voltage_{}, bms_charge_current_{}, bms_discharge_current_{};
    std::array<Reading, 3> grid_voltages_{}, grid_currents_{};
};

} // namespace inverter_gateway::network
