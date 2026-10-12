#include "inverter_gateway/protocol/energy_protocol.hpp"
#include "inverter_gateway/protocol/pi_protocols.hpp"
#include "inverter_gateway/protocol/bms_protocols.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace inverter_gateway::protocol {
namespace {
constexpr EnergyProtocolInfo catalog[] = {
 {EnergyProtocol::aohai_modbus,"AOHAI_MODBUS_RTU","AOHAI Modbus RTU",nullptr,9600,EnergyConnection::usb_rs485,true},
 {EnergyProtocol::pi16,"PI16","Voltronic PI16","QPIGS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi17,"PI17","Voltronic PI17","GS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi17m058,"PI17M058","Voltronic PI17 M058","GS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi17infini,"PI17INFINI","InfiniSolar PI17","GS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi18,"PI18","Voltronic PI18","GS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi18lvx,"PI18LVX","Voltronic PI18 LVX","GS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi18sv,"PI18SV","Voltronic PI18 SV","GS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi30,"PI30","Voltronic / MPP-Solar PI30","QPIGS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi30max,"PI30MAX","Voltronic PI30 MAX","QPIGS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi30mst,"PI30MST","Voltronic PI30 MST","QPIGS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi30revo,"PI30REVO","REVO PI30 variant","PQSE",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi30m044,"PI30M044","Voltronic PI30 M044","QPIGS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi30m045,"PI30M045","Voltronic PI30 M045","QPIGS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::pi41,"PI41","Voltronic PI41","QPIGS",2400,EnergyConnection::usb_hid,true},
 {EnergyProtocol::jk02,"JK02","JK BMS Bluetooth 24-cell","getCellData",115200,EnergyConnection::bluetooth,true},
 {EnergyProtocol::jk02_32,"JK02_32","JK BMS Bluetooth 32-cell","getCellData",115200,EnergyConnection::bluetooth,true},
 {EnergyProtocol::jk04,"JK04","JK BMS Bluetooth JK04","getCellData",115200,EnergyConnection::bluetooth,true},
 {EnergyProtocol::jk232,"JK232","JBD-compatible JK232","getBalancerData",9600,EnergyConnection::uart_rs232,true},
 {EnergyProtocol::jk485,"JK485","JK RS485 balancer","getBalancerData",9600,EnergyConnection::uart_rs485,true},
 {EnergyProtocol::jkpb,"JKPB","JK inverter BMS RS485","getCellData",115200,EnergyConnection::uart_rs485,true},
 {EnergyProtocol::jkv11,"JKV11","JK BMS Bluetooth V11","getCellData",115200,EnergyConnection::bluetooth,true},
 {EnergyProtocol::jkserial,"JKSERIAL","JK tagged serial BMS","getBalancerData",115200,EnergyConnection::uart_ttl,true},
 {EnergyProtocol::daly,"DALY","Daly BMS UART","SOC",9600,EnergyConnection::uart_ttl,true},
 {EnergyProtocol::daly40,"DALY40","Daly address 0x40 variant","SOC",9600,EnergyConnection::uart_ttl,true},
 {EnergyProtocol::ved,"VED","Victron VE.Direct","vedtext",19200,EnergyConnection::uart_ttl,true},
};
static_assert(sizeof(catalog)/sizeof(catalog[0])==static_cast<unsigned>(EnergyProtocol::count));
bool pi(EnergyProtocol id){return id>=EnergyProtocol::pi16&&id<=EnergyProtocol::pi41;}

// Fixed, documented queries only. Each is admitted through the selected
// profile's command catalog; historical/date and parallel-index reads remain
// explicit user queries because they require additional context.
constexpr const char *extra_background_commands[] = {
    "QVFW", "QVFW2", "QVFW3", "VERFW", "VFW", "VFW2",
    "QGMN", "QMN", "MD", "DM", "QBOOT",
    "QPIBI", "INGS", "EMINFO", "BMS", "Q1", "QBMS", "QLITH0",
    "GPMP", "HECS", "DI", "MCHGCR", "MUCHGCR", "ACCT", "ACLT",
    "QDI", "QMCHGCR", "QMUCHGCR", "QOPM", "QOPPT", "QCHPT",
    "QBEQI", "QLED", "QDOP", "batteryCapacity",
    "QET", "QLT", "ET", "QT", "T",
};

bool eq(const char *a, const char *b) { return a && b && std::strcmp(a, b) == 0; }
bool bluetooth_jk(EnergyProtocol id)
{
    return id == EnergyProtocol::jk02 || id == EnergyProtocol::jk02_32 ||
        id == EnergyProtocol::jk04 || id == EnergyProtocol::jkv11;
}

EnergyPollInfo classify_poll(EnergyProtocol id, const char *command)
{
    EnergyPollInfo result{command, EnergyPollClass::live, 5000, false, false, false};
    if (eq(command, "QPI") || eq(command, "PI") || eq(command, "QID") ||
        eq(command, "QSID") || eq(command, "ID") || eq(command, "getInfo") ||
        eq(command, "QVFW") || eq(command, "QVFW2") || eq(command, "QVFW3") ||
        eq(command, "VERFW") || eq(command, "VFW") || eq(command, "VFW2") ||
        eq(command, "QGMN") || eq(command, "QMN") || eq(command, "MD") ||
        eq(command, "DM") ||
        eq(command, "QBOOT") ||
        (id == EnergyProtocol::jkpb && eq(command, "getBalancerData"))) {
        result.poll_class = EnergyPollClass::identity;
        result.interval_ms = 0;
        result.prerequisite = bluetooth_jk(id) && eq(command, "getInfo");
    } else if (eq(command, "QPIRI") || eq(command, "PIRI") ||
        eq(command, "QFLAG") || eq(command, "FLAG") || eq(command, "BATS") ||
        eq(command, "PQSE") || eq(command, "getAdditionalInfo") ||
        eq(command, "QDI") || eq(command, "DI") ||
        eq(command, "QMCHGCR") || eq(command, "QMUCHGCR") ||
        eq(command, "MCHGCR") || eq(command, "MUCHGCR") ||
        eq(command, "QOPM") || eq(command, "QOPPT") || eq(command, "QCHPT") ||
        eq(command, "QBEQI") || eq(command, "QLED") || eq(command, "QDOP") ||
        eq(command, "GPMP") || eq(command, "GPMP0") || eq(command, "HECS") ||
        eq(command, "ACCT") || eq(command, "ACLT") || eq(command, "batteryCapacity") ||
        eq(command, "PRI")) {
        result.poll_class = EnergyPollClass::configuration;
        result.interval_ms = 60000;
    } else if (eq(command, "ET") || eq(command, "QET") || eq(command, "QLT") ||
        eq(command, "QEH") || eq(command, "QED") || eq(command, "QEM") ||
        eq(command, "QEY") || eq(command, "QLY") || eq(command, "QLM") ||
        eq(command, "QLD") || eq(command, "EY") || eq(command, "EM") ||
        eq(command, "ED") || eq(command, "EH")) {
        result.poll_class = EnergyPollClass::energy;
        result.interval_ms = 60000;
    } else if (eq(command, "QMOD") || eq(command, "MOD") ||
        eq(command, "QPIWS") || eq(command, "WS") || eq(command, "FWS") ||
        eq(command, "mosStatus") || eq(command, "batteryFailureStatus") ||
        eq(command, "status") || eq(command, "cellBalanceState") ||
        eq(command, "QT") || eq(command, "T")) {
        result.poll_class = EnergyPollClass::status;
    }
    // PI17 needs both GS (electrical readings) and PS (measured power). REVO's
    // legacy default PQSE is settings; its telemetry comes from QPIGS/QALL.
    result.primary = eq(command, "QPIGS") || eq(command, "GS") || eq(command, "PS") ||
        eq(command, "SOC") || eq(command, "vedtext") ||
        (eq(command, "getCellData") && id != EnergyProtocol::jk232) ||
        (eq(command, "getBalancerData") && id != EnergyProtocol::jkpb);
    if (result.primary) result.interval_ms = 2000;
    result.publish_on_change = result.poll_class == EnergyPollClass::identity ||
        result.poll_class == EnergyPollClass::configuration ||
        result.poll_class == EnergyPollClass::status;
    return result;
}

bool extra_background_supported(EnergyProtocol id, const char *command)
{
    EnergyCommandInfo info{};
    if (!energy_command_info(id, command, info) || info.setter) return false;
    for (std::size_t i = 0; i < energy_poll_command_count(id); ++i)
        if (eq(command, energy_poll_command(id, i))) return false;
    return true;
}

void field_key(char (&out)[40], const char *label)
{
    std::uint32_t hash=2166136261U;
    char full[192]{};
    std::size_t n=0;
    bool separator=false;
    for(const unsigned char *p=reinterpret_cast<const unsigned char *>(label?label:"");*p;++p){
        hash=(hash^*p)*16777619U;
        if(std::isalnum(*p)){
            if(separator&&n&&n<sizeof(full)-1)full[n++]='_';
            separator=false;
            if(n<sizeof(full)-1)full[n++]=static_cast<char>(std::tolower(*p));
        }else separator=true;
    }
    if(n==0){std::strcpy(out,"value");return;}
    if(n<sizeof(out)){std::memcpy(out,full,n+1);return;}
    std::snprintf(out,sizeof(out),"%.30s_%08x",full,static_cast<unsigned>(hash));
}
}

bool EnergyResponse::add_number(const char *label,const char *unit,double value)
{
    if(count>=maximum_fields||!std::isfinite(value))return false;
    auto &field=fields[count++];field={};field_key(field.key,label);
    std::snprintf(field.unit,sizeof(field.unit),"%s",unit?unit:"");
    field.value=value;field.numeric=true;return true;
}
bool EnergyResponse::add_text(const char *label,const char *unit,const char *value)
{
    if(count>=maximum_fields||!value)return false;
    auto &field=fields[count++];field={};field_key(field.key,label);
    std::snprintf(field.unit,sizeof(field.unit),"%s",unit?unit:"");
    std::snprintf(field.text,sizeof(field.text),"%s",value);
    if (std::strlen(value) >= sizeof(field.text))
        std::memcpy(field.text + sizeof(field.text) - 4, "...", 4);
    field.numeric=false;return true;
}
const EnergyProtocolInfo &energy_protocol_info(EnergyProtocol id)
{
    const auto index=static_cast<unsigned>(id);
    return catalog[index<static_cast<unsigned>(EnergyProtocol::count)?index:0];
}
bool parse_energy_protocol(const char *name,EnergyProtocol &id)
{
    if(!name)return false;
    for(const auto &item:catalog)if(std::strcmp(name,item.name)==0){id=item.id;return true;}
    return false;
}
const char *energy_connection_name(EnergyConnection connection)
{
    constexpr const char *names[]={"usb_rs485","usb_hid","uart_rs232","uart_rs485","uart_ttl","bluetooth"};
    const auto i=static_cast<unsigned>(connection);return i<6?names[i]:"unknown";
}
bool parse_energy_connection(const char *name,EnergyConnection &connection)
{
    if(!name)return false;
    for(unsigned i=0;i<6;++i)if(std::strcmp(name,energy_connection_name(static_cast<EnergyConnection>(i)))==0){connection=static_cast<EnergyConnection>(i);return true;}
    return false;
}
bool energy_connection_supported(EnergyProtocol id,EnergyConnection connection)
{
    if(id>=EnergyProtocol::count)return false;
    if(id==EnergyProtocol::aohai_modbus)return connection==EnergyConnection::usb_rs485;
    if(id==EnergyProtocol::jk02||id==EnergyProtocol::jk02_32||id==EnergyProtocol::jk04||id==EnergyProtocol::jkv11)return connection==EnergyConnection::bluetooth;
    if(connection==EnergyConnection::bluetooth)return false;
    if(connection==EnergyConnection::usb_hid)return pi(id);
    if(id==EnergyProtocol::ved)return connection==EnergyConnection::uart_ttl;
    return connection==EnergyConnection::usb_rs485||connection==EnergyConnection::uart_rs232||connection==EnergyConnection::uart_rs485||connection==EnergyConnection::uart_ttl;
}
const char *codec_status_name(CodecStatus status)
{
    constexpr const char *names[]={"ok","unsupported","invalid_command","invalid_frame","checksum_error","buffer_too_small","rejected"};
    const auto i=static_cast<unsigned>(status);return i<7?names[i]:"invalid_frame";
}
std::size_t energy_poll_command_count(EnergyProtocol id){return pi(id)?pi_poll_command_count(id):bms_poll_command_count(id);}
const char *energy_poll_command(EnergyProtocol id,std::size_t index){return pi(id)?pi_poll_command(id,index):bms_poll_command(id,index);}
std::size_t energy_background_poll_count(EnergyProtocol id)
{
    std::size_t count = energy_poll_command_count(id);
    for (const auto *command : extra_background_commands)
        if (extra_background_supported(id, command)) ++count;
    return count;
}
bool energy_background_poll_command(EnergyProtocol id, std::size_t index,
                                    EnergyPollInfo &poll)
{
    poll = {};
    const auto base_count = energy_poll_command_count(id);
    const char *command = nullptr;
    if (index < base_count) command = energy_poll_command(id, index);
    else {
        index -= base_count;
        for (const auto *candidate : extra_background_commands) {
            if (!extra_background_supported(id, candidate)) continue;
            if (index-- == 0) { command = candidate; break; }
        }
    }
    if (!command) return false;
    EnergyCommandInfo info{};
    if (!energy_command_info(id, command, info) || info.setter) return false;
    poll = classify_poll(id, command);
    return true;
}
bool energy_poll_info(EnergyProtocol id, const char *command, EnergyPollInfo &poll)
{
    poll = {};
    EnergyCommandInfo info{};
    if (!energy_command_info(id, command, info) || info.setter) return false;
    poll = classify_poll(id, info.name);
    // Preserve the full, possibly parameterized command for correlation.
    poll.command = command;
    return true;
}
bool energy_command_info(EnergyProtocol id,const char *command,EnergyCommandInfo &info){return pi(id)?pi_command_info(id,command,info):bms_command_info(id,command,info);}
CodecStatus energy_build_request(EnergyProtocol id,const char *command,std::uint8_t address,std::uint8_t *out,std::size_t capacity,std::size_t &size){return pi(id)?pi_build_request(id,command,address,out,capacity,size):bms_build_request(id,command,address,out,capacity,size);}
FrameState energy_frame_state(EnergyProtocol id,const char *command,const std::uint8_t *data,std::size_t size,std::size_t &frame_size){return pi(id)?pi_frame_state(id,command,data,size,frame_size):bms_frame_state(id,command,data,size,frame_size);}
CodecStatus energy_decode_response(EnergyProtocol id,const char *command,std::uint8_t address,const std::uint8_t *data,std::size_t size,EnergyResponse &response){response.count=0;response.acknowledged=false;response.rejected=false;return pi(id)?pi_decode_response(id,command,address,data,size,response):bms_decode_response(id,command,address,data,size,response);}
} // namespace inverter_gateway::protocol
