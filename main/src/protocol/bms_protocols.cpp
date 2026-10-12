// Original bounded codecs, based on manufacturer specifications and the
// jblance/mpp-solar reference vectors. See docs/protocol-support.md for sources.
#include "inverter_gateway/protocol/bms_protocols.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace inverter_gateway::protocol {
namespace {

using Id = EnergyProtocol;
bool eq(const char *a, const char *b) { return a && b && std::strcmp(a, b) == 0; }
bool ble(Id id) { return id == Id::jk02 || id == Id::jk02_32 || id == Id::jk04 || id == Id::jkv11; }
bool daly(Id id) { return id == Id::daly || id == Id::daly40; }
bool supported(Id id) { return ble(id) || daly(id) || id == Id::jk232 || id == Id::jk485 || id == Id::jkpb || id == Id::jkserial || id == Id::ved; }
std::uint16_t be16(const std::uint8_t *p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint16_t le16(const std::uint8_t *p) { return (std::uint16_t(p[1]) << 8) | p[0]; }
std::uint32_t le32(const std::uint8_t *p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
std::uint32_t be32(const std::uint8_t *p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
std::uint16_t sum16(const std::uint8_t *p, std::size_t n) { std::uint16_t s = 0; while (n--) s += *p++; return s; }
std::uint8_t sum8(const std::uint8_t *p, std::size_t n) { return static_cast<std::uint8_t>(sum16(p, n)); }
std::uint16_t modbus_crc(const std::uint8_t *p, std::size_t n) {
    std::uint16_t crc = 0xffff;
    while (n--) { crc ^= *p++; for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0); }
    return crc;
}
float lefloat(const std::uint8_t *p) { const std::uint32_t u = le32(p); float f; std::memcpy(&f, &u, sizeof(f)); return f; }
void indexed(EnergyResponse &r, const char *prefix, unsigned index, const char *unit, double value) {
    char name[40]; std::snprintf(name, sizeof(name), "%s%02u", prefix, index); r.add_number(name, unit, value);
}
void text_bytes(EnergyResponse &r, const char *label, const std::uint8_t *p, std::size_t n) {
    char s[32]{}; if (n > sizeof(s) - 1) n = sizeof(s) - 1;
    for (std::size_t i = 0; i < n && p[i]; ++i) s[i] = p[i] >= 32 && p[i] <= 126 ? static_cast<char>(p[i]) : '?';
    r.add_text(label, "", s);
}

struct Command {
    const char *name = nullptr;
    std::uint16_t code = 0;
    std::uint32_t value = 0;
    bool setter = false;
    const char *readback = nullptr;
};
struct Named { const char *name; std::uint16_t code; };
constexpr Named daly_commands[] = {{"SOC",0x90},{"cellMinMaxVoltages",0x91},{"cellMinMaxTemps",0x92},{"mosStatus",0x93},{"status",0x94},{"cellVoltages",0x95},{"cellTemperatures",0x96},{"cellBalanceState",0x97},{"batteryFailureStatus",0x98}};
// Group reads are enabled after the observed 0x94 status supplies their counts.
constexpr const char *daly_poll[] = {"SOC","cellMinMaxVoltages","cellMinMaxTemps","mosStatus","status","cellBalanceState","batteryFailureStatus","cellVoltages","cellTemperatures"};
// JK notification streams must be enabled with the device-information request
// before the first status request after connecting.
constexpr const char *ble_poll[] = {"getInfo","getCellData"};
constexpr const char *jk232_poll[] = {"getBalancerData","getCellData","getInfo"};
constexpr const char *jk485_poll[] = {"getBalancerData"};
constexpr const char *jkpb_poll[] = {"getCellData","getAdditionalInfo","getBalancerData"};
constexpr const char *jkserial_poll[] = {"getBalancerData"};
constexpr const char *ved_poll[] = {"vedtext"};

bool decimal_suffix(const char *s, const char *prefix, double scale, std::uint32_t low,
                    std::uint32_t high, std::uint32_t &value) {
    const auto n = std::strlen(prefix);
    if (!s || std::strncmp(s, prefix, n) || !s[n]) return false;
    const char *v = s + n;
    // Decimal commands do not admit signs, whitespace, exponent or junk suffixes.
    unsigned dots = 0;
    for (const char *p = v; *p; ++p) {
        if (*p == '.') { if (++dots > 1) return false; }
        else if (*p < '0' || *p > '9') return false;
    }
    char *end = nullptr; const double d = std::strtod(v, &end) * scale;
    if (end == v || *end || !std::isfinite(d) || d < low || d > high || std::fabs(d - std::round(d)) > 0.000001) return false;
    value = static_cast<std::uint32_t>(std::round(d)); return true;
}

bool parse(Id id, const char *s, Command &c) {
    c = {}; if (!s || !*s || !supported(id)) return false;
    c.name = s;
    if (daly(id)) { for (const auto &n : daly_commands) if (eq(s,n.name)) { c.name=n.name;c.code=n.code;return true; } return false; }
    if (ble(id)) {
        if (eq(s,"getCellData")) { c.name="getCellData";c.code=0x96;return true; }
        if (eq(s,"getInfo")) { c.name="getInfo";c.code=0x97;return true; }
        // These fixed JK02 operations are verified by captured command/ACK
        // vectors. Other firmware setters and numeric limits are not inferred.
        if (id == Id::jk02) {
            constexpr Named toggles[]={{"setChargingOn",0x1d},{"setChargingOff",0x1d},{"setDischargingOn",0x1e},{"setDischargingOff",0x1e}};
            for (const auto &n : toggles) if (eq(s,n.name)) { c.name=n.name;c.code=n.code;c.setter=true;c.value=eq(s,"setChargingOn")||eq(s,"setDischargingOn");c.readback="getCellData";return true; }
        }
        return false;
    }
    if (id == Id::jk232) {
        if (eq(s,"getBalancerData") || eq(s,"getStatus")) { c.name="getBalancerData";c.code=3;return true; }
        if (eq(s,"getCellData")) { c.name="getCellData";c.code=4;return true; }
        if (eq(s,"getInfo")) { c.name="getInfo";c.code=5;return true; }
        return false;
    }
    if (id == Id::jk485) {
        if (eq(s,"getBalancerData")) { c.name="getBalancerData";c.code=0xff;return true; }
        if (decimal_suffix(s,"setCellCount",1,2,24,c.value)) { c.name="setCellCount";c.code=0xf0; }
        else if (decimal_suffix(s,"setBalanceTrigger",1,2,1000,c.value)) { c.name="setBalanceTrigger";c.code=0xf2; }
        else if (decimal_suffix(s,"setMaxBalanceCurrent",1,30,1000,c.value)) { c.name="setMaxBalanceCurrent";c.code=0xf4; }
        else if (eq(s,"setBalanceOn") || eq(s,"setBalanceOff")) { c.name=eq(s,"setBalanceOn")?"setBalanceOn":"setBalanceOff";c.code=0xf6;c.value=eq(s,"setBalanceOn"); }
        else return false;
        c.setter=true;c.readback="getBalancerData";return true;
    }
    if (id == Id::jkserial) { if (eq(s,"getBalancerData") || eq(s,"getCellData")) { c.name="getBalancerData";return true; } return false; }
    if (id == Id::jkpb) {
        if (eq(s,"getCellData")) { c.name="getCellData";c.code=0x1620;return true; }
        if (eq(s,"getAdditionalInfo")) { c.name="getAdditionalInfo";c.code=0x161e;return true; }
        if (eq(s,"getBalancerData") || eq(s,"getInfo")) { c.name="getBalancerData";c.code=0x161c;return true; }
        return false;
    }
    if (id == Id::ved) {
        if (eq(s,"vedtext")) { c.name="vedtext";return true; }
        if (eq(s,"batteryCapacity")) { c.name="batteryCapacity";c.code=0x1000;return true; }
        // Manufacturer documented BMV-700/SmartShunt setting, not a generic
        // raw register writer. Other VE.Direct products can reject this ID.
        if (decimal_suffix(s,"setBatteryCapacity",1,1,9999,c.value)) { c.name="setBatteryCapacity";c.code=0x1000;c.setter=true;c.readback="batteryCapacity";return true; }
        return false;
    }
    return false;
}

FrameState fixed(const std::uint8_t *p, std::size_t n, std::size_t wanted, std::size_t &total) {
    (void)p; total=wanted; return n<wanted?FrameState::incomplete:n==wanted?FrameState::complete:FrameState::invalid;
}
int nibble(std::uint8_t b) { if (b>='0'&&b<='9') return b-'0';if(b>='A'&&b<='F')return b-'A'+10;return -1; }

// VE.Direct text records begin with CR/LF and finish at the *binary* checksum
// byte. HEX messages interleaved between records are excluded from text sums.
bool ved_block(const std::uint8_t *p, std::size_t n, std::size_t &begin, std::size_t &end) {
    constexpr char tag[]="\r\nChecksum\t";
    for (std::size_t start=0;start+2<n;++start) {
        if (p[start]!=13||p[start+1]!=10||p[start+2]<32||p[start+2]>126) continue;
        std::uint8_t sum=0;
        for (std::size_t at=start;at<n;) {
            if (p[at]==':' && at>start && (p[at-1]==10||p[at-1]==13)) {
                std::size_t h=at;while(h<n&&p[h]!=10)++h;if(h==n)break;at=h+1;continue;
            }
            if (at+sizeof(tag)<=n && std::memcmp(p+at,tag,sizeof(tag)-1)==0) {
                for (std::size_t j=0;j<sizeof(tag);++j) sum+=p[at+j];
                if(sum==0) { begin=start;end=at+sizeof(tag);return true; }
                break;
            }
            sum+=p[at++];
        }
    }
    return false;
}

// VE.Direct sends TEXT independently of HEX requests. A newline in that
// background stream cannot complete a HEX transaction; select its operation
// and little-endian register before admitting the full response line.
bool ved_hex_reply(const Command &c, const std::uint8_t *p, std::size_t n,
                   std::size_t &begin, std::size_t &end) {
    for (std::size_t at=0;at+6<n;++at) {
        if(p[at]!=':'||(at&&p[at-1]!=10&&p[at-1]!=13)||p[at+1]!=(c.setter?'8':'7'))continue;
        const int a=nibble(p[at+2]),b=nibble(p[at+3]),d=nibble(p[at+4]),e=nibble(p[at+5]);
        if(a<0||b<0||d<0||e<0||((a<<4)|b|((d<<4|e)<<8))!=c.code)continue;
        std::size_t finish=at+6;while(finish<n&&p[finish]!=10)++finish;
        if(finish==n)continue;
        begin=at;end=finish+1;return true;
    }
    return false;
}

CodecStatus decode_ble(Id id, const Command &c, const std::uint8_t *p, std::size_t n, EnergyResponse &r) {
    if (c.setter) {
        if(n!=20||p[0]!=0xaa||p[1]!=0x55||p[2]!=0x90||p[3]!=0xeb) return CodecStatus::invalid_frame;
        if(sum8(p,n-1)!=p[n-1])return CodecStatus::checksum_error;
        if(p[4]!=0xc8||p[5]!=1)return CodecStatus::invalid_frame;
        r.acknowledged=p[6]==1;r.rejected=!r.acknowledged;
        return r.rejected?CodecStatus::rejected:CodecStatus::ok;
    }
    if ((n!=300&&n!=320)||p[0]!=0x55||p[1]!=0xaa||p[2]!=0xeb||p[3]!=0x90||p[4]!=(c.code==0x97?3:2)) return CodecStatus::invalid_frame;
    if(sum8(p,n-1)!=p[n-1])return CodecStatus::checksum_error;
    if(c.code==0x97) {
        text_bytes(r,"Device Model",p+6,16);text_bytes(r,"Hardware Version",p+22,8);text_bytes(r,"Software Version",p+30,8);
        r.add_number("Up Time","s",le32(p+38));r.add_number("Power-on Times","",le32(p+42));
        text_bytes(r,"Device Name",p+46,16);text_bytes(r,"Manufacturing Date",p+78,8);text_bytes(r,"Serial Number",p+86,11);
        // Passcodes in device info are intentionally not emitted as telemetry.
        return CodecStatus::ok;
    }
    if(id==Id::jk04) {
        unsigned cells=0;
        for(unsigned i=0;i<24;++i) {
            const double v=lefloat(p+6+4*i);if(!std::isfinite(v)||v<0||v>10)return CodecStatus::invalid_frame;
            if(v>0){indexed(r,"Voltage_Cell",i+1,"V",v);++cells;}
            const double resistance=lefloat(p+102+4*i);
            if(std::isfinite(resistance)&&resistance>=0)indexed(r,"Resistance_Cell",i+1,"Ohm",resistance);
        }
        const double avg=lefloat(p+202),delta=lefloat(p+206);
        if(!std::isfinite(avg)||!std::isfinite(delta))return CodecStatus::invalid_frame;
        r.add_number("Cells_connected","",cells);r.add_number("Average_Cell_Voltage","V",avg);r.add_number("Delta_Cell_Voltage","V",delta);
        r.add_number("Highest Cell","",p[218]+1);r.add_number("Lowest Cell","",p[219]+1);
        r.add_number("Time","s",p[286]|(std::uint32_t(p[287])<<8)|(std::uint32_t(p[288])<<16));
        return CodecStatus::ok;
    }
    const unsigned cells=id==Id::jk02?24:32;
    const unsigned shift=cells==24?0:32;
    const std::uint32_t presence=le32(p+6+2*cells);
    unsigned present=0;
    for(unsigned i=0;i<cells;++i) if(presence&(std::uint32_t(1)<<i)) {
        indexed(r,"Voltage_Cell",i+1,"V",le16(p+6+2*i)/1000.0);
        indexed(r,"Resistance_Cell",i+1,"Ohm",le16(p+64+2*(cells-24)+2*i)/1000.0);++present;
    }
    r.add_number("Cells_connected","",present);
    r.add_number("Average_Cell_Voltage","V",le16(p+58+2*(cells-24))/1000.0);
    r.add_number("Delta_Cell_Voltage","V",le16(p+60+2*(cells-24))/1000.0);
    r.add_number("Battery_Voltage","V",le32(p+118+shift)/1000.0);
    r.add_number("Battery_Power","W",le32(p+122+shift)/1000.0);
    r.add_number("Battery_Current","A",static_cast<std::int32_t>(le32(p+126+shift))/1000.0);
    r.add_number("Battery_T1","C",static_cast<std::int16_t>(le16(p+130+shift))/10.0);
    r.add_number("Battery_T2","C",static_cast<std::int16_t>(le16(p+132+shift))/10.0);
    // Firmware 11 uses the older layout after expanding the cell array.
    const unsigned mos=id==Id::jk02_32?144:134+shift;
    r.add_number("MOS_Temp","C",static_cast<std::int16_t>(le16(p+mos))/10.0);
    const unsigned soc=id==Id::jk02_32?173:141+shift;
    if(p[soc]>100)return CodecStatus::invalid_frame;
    r.add_number("Percent_Remain","%",p[soc]);
    r.add_number("Capacity_Remain","Ah",le32(p+soc+1)/1000.0);
    r.add_number("Nominal_Capacity","Ah",le32(p+soc+5)/1000.0);
    r.add_number("Cycle_Count","",le32(p+soc+9));r.add_number("Cycle_Capacity","Ah",le32(p+soc+13)/1000.0);
    r.add_number("Charge Enabled","",p[166+shift]);r.add_number("Discharge Enabled","",p[167+shift]);
    return CodecStatus::ok;
}

CodecStatus decode_jk232(const Command &c,const std::uint8_t *p,std::size_t n,EnergyResponse &r) {
    if(n<7||p[0]!=0xdd||p[1]!=c.code||n!=std::size_t(p[3])+7||p[n-1]!=0x77)return CodecStatus::invalid_frame;
    const auto checksum=static_cast<std::uint16_t>(0-sum16(p+2,n-5));
    if(be16(p+n-3)!=checksum)return CodecStatus::checksum_error;
    if(p[2]!=0){r.rejected=true;r.add_number("Response Status","",p[2]);return CodecStatus::rejected;}
    const auto *d=p+4;const std::size_t length=p[3];
    if(c.code==3) {
        if(length<23||d[21]>32||d[22]>16||length!=23+std::size_t(d[22])*2||d[19]>100)return CodecStatus::invalid_frame;
        r.add_number("Total Battery Voltage","V",be16(d)/100.0);r.add_number("Total Current","A",static_cast<std::int16_t>(be16(d+2))/100.0);
        r.add_number("Remaining Capacity","Ah",be16(d+4)/100.0);r.add_number("Nominal Capacity","Ah",be16(d+6)/100.0);
        r.add_number("Cycles","cycles",be16(d+8));r.add_number("Production Date","",be16(d+10));
        r.add_number("Equilibrium State","",be16(d+12));r.add_number("Equilibrium State 2","",be16(d+14));
        r.add_number("Protection State","",be16(d+16));r.add_number("Remaining Battery","%",d[19]);r.add_number("FET Control Status","",d[20]);
        r.add_number("Number of Battery Strings","",d[21]);r.add_number("Number of NTC","",d[22]);
        for(unsigned i=0;i<d[22];++i)indexed(r,"NTC ",i+1,"C",(int(be16(d+23+2*i))-2731)/10.0);
    } else if(c.code==4) {
        if(!length||length%2||length>64)return CodecStatus::invalid_frame;
        for(unsigned i=0;i<length/2;++i)indexed(r,"Voltage_Cell",i+1,"V",be16(d+2*i)/1000.0);
    } else text_bytes(r,"Device Model",d,length);
    return CodecStatus::ok;
}

CodecStatus decode_jk485(const Command &c,std::uint8_t address,const std::uint8_t *p,std::size_t n,EnergyResponse &r) {
    if(n!=74||p[0]!=0xeb||p[1]!=0x90||p[2]!=address||p[3]!=c.code)return CodecStatus::invalid_frame;
    if(sum8(p,73)!=p[73])return CodecStatus::checksum_error;
    if(c.setter) {
        r.add_number("Setting Value","",be16(p+4));
        r.acknowledged=be16(p+4)==c.value;r.rejected=!r.acknowledged;
        return r.rejected?CodecStatus::rejected:CodecStatus::ok;
    }
    if(p[8]>24||p[22]>24)return CodecStatus::invalid_frame;
    r.add_number("Total Battery Voltage","V",be16(p+4)/100.0);r.add_number("Average Cell Voltage","V",be16(p+6)/1000.0);
    r.add_number("Number of Cells","",p[8]);r.add_number("Highest Cell","",p[9]);r.add_number("Lowest Cell","",p[10]);
    r.add_number("Charging / Discharging","",p[11]);r.add_number("Alarm","",p[12]);r.add_number("Voltage Difference","V",be16(p+13)/1000.0);
    r.add_number("Balance Current","A",be16(p+15)/1000.0);r.add_number("Balance Trigger Voltage","V",be16(p+17)/1000.0);
    r.add_number("Max Balance Current","A",be16(p+19)/1000.0);r.add_number("Balance On / Off","",p[21]);r.add_number("Set Number of Cells","",p[22]);
    for(unsigned i=0;i<p[8];++i)indexed(r,"Voltage_Cell",i+1,"V",be16(p+23+2*i)/1000.0);
    r.add_number("Temperature","C",static_cast<std::int16_t>(be16(p+71)));return CodecStatus::ok;
}

std::size_t jk_tag_size(std::uint8_t tag) {
    if(tag==0x79)return 0; // special length-prefixed cell array
    switch(tag) {
    case 0x85:case 0x86:case 0x9d:case 0xa9:case 0xab:case 0xac:case 0xae:case 0xaf:case 0xb1:case 0xb3:case 0xb8:case 0xbb:case 0xbc:case 0xbd:case 0xc0:case 0xd2:return 1;
    case 0x89:case 0xaa:case 0xb6:case 0xb9:return 4;
    case 0xbe:case 0xbf:return 2;
    case 0xb2:return 10;case 0xb4:return 8;case 0xb5:return 4;case 0xb7:return 15;case 0xba:return 24;
    default:
        return (tag>=0x80&&tag<=0x84)||(tag==0x87)||(tag>=0x8a&&tag<=0x8c)||
            (tag>=0x8e&&tag<=0x9c)||(tag>=0x9e&&tag<=0xa8)||tag==0xad||tag==0xb0?2:0;
    }
}
double jk_temperature(std::uint16_t raw) { return raw>100?100.0-raw:raw; }
CodecStatus decode_jkserial(const std::uint8_t *p,std::size_t n,EnergyResponse &r) {
    if(n<21||p[0]!=0x4e||p[1]!=0x57||be16(p+2)+2u!=n||p[n-5]!=0x68||p[n-4]||p[n-3]||(p[8]!=3&&p[8]!=6)||(p[10]!=1&&p[10]!=2))return CodecStatus::invalid_frame;
    if(sum16(p,n-4)!=be16(p+n-2))return CodecStatus::checksum_error;
    // Walk tagged data, including a variable cell count, before decoding. The
    // C0 tag selects the legacy biased current vs sign/magnitude representation.
    const std::size_t end=n-9;std::size_t at=11;bool sign_magnitude=false;
    while(at<end) {
        const auto tag=p[at++];if(tag==0x79) { if(at==end||p[at]%3||at+1+p[at]>end)return CodecStatus::invalid_frame;at+=1+p[at];continue; }
        const auto width=jk_tag_size(tag);if(!width||at+width>end)return CodecStatus::invalid_frame;
        if(tag==0xc0)sign_magnitude=p[at]==1;
        at+=width;
    }
    at=11;
    while(at<end) {
        const auto tag=p[at++];if(tag==0x79) {
            const unsigned length=p[at++];r.add_number("Cells_connected","",length/3);
            bool seen[49]{};
            for(unsigned i=0;i<length;i+=3) { const unsigned cell=p[at+i];if(!cell||cell>48||seen[cell])return CodecStatus::invalid_frame;seen[cell]=true;indexed(r,"Voltage_Cell",cell,"V",be16(p+at+i+1)/1000.0); }
            at+=length;continue;
        }
        const auto width=jk_tag_size(tag);const auto *d=p+at;
        switch(tag) {
        case 0x80:r.add_number("MOS_Temp","C",jk_temperature(be16(d)));break;
        case 0x81:r.add_number("Battery_T1","C",jk_temperature(be16(d)));break;
        case 0x82:r.add_number("Battery_T2","C",jk_temperature(be16(d)));break;
        case 0x83:r.add_number("Battery_Voltage","V",be16(d)/100.0);break;
        case 0x84: { const auto raw=be16(d);r.add_number("Battery_Current","A",sign_magnitude?((raw&0x8000)?1:-1)*(raw&0x7fff)/100.0:(10000-int(raw))/100.0);break; }
        case 0x85:if(d[0]>100)return CodecStatus::invalid_frame;r.add_number("Percent_Remain","%",d[0]);break;
        case 0x86:r.add_number("Number of Temperature Sensors","",d[0]);break;
        case 0x87:r.add_number("Cycle_Count","",be16(d));break;
        case 0x89:r.add_number("Cycle_Capacity","Ah",be32(d));break;
        case 0x8a:r.add_number("Number of Battery Strings","",be16(d));break;
        case 0x8b:r.add_number("Battery Warning Message","",be16(d));break;
        case 0x8c:r.add_number("Battery status information","",be16(d));break;
        case 0x9d:r.add_number("Balancer Active","",d[0]);break;
        case 0xaa:r.add_number("Nominal_Capacity","Ah",be32(d));break;
        case 0xab:r.add_number("Charge Enabled","",d[0]);break;
        case 0xac:r.add_number("Discharge Enabled","",d[0]);break;
        case 0xb7:text_bytes(r,"Software Version",d,width);break;
        case 0xba:text_bytes(r,"Device Model",d,width);break;
        case 0xc0:r.add_number("Current Encoding Version","",d[0]);break;
        default:break;
        }
        at+=width;
    }
    return CodecStatus::ok;
}

CodecStatus decode_jkpb(const Command &c,std::uint8_t address,const std::uint8_t *p,std::size_t n,EnergyResponse &r) {
    if(n!=308||p[0]!=0x55||p[1]!=0xaa||p[2]!=0xeb||p[3]!=0x90||p[4]!=(c.code==0x1620?2:c.code==0x161e?1:3))return CodecStatus::invalid_frame;
    if(sum8(p,299)!=p[299]||modbus_crc(p+300,6)!=le16(p+306))return CodecStatus::checksum_error;
    if(p[300]!=address||p[301]!=0x10||be16(p+302)!=c.code||be16(p+304)!=1)return CodecStatus::invalid_frame;
    if(c.code==0x161c) {
        text_bytes(r,"Device Model",p+6,16);text_bytes(r,"Hardware Version",p+22,8);text_bytes(r,"Software Version",p+30,8);
        r.add_number("Up Time","s",le32(p+38));r.add_number("Power-on Times","",le32(p+42));return CodecStatus::ok;
    }
    if(c.code==0x161e) {
        constexpr const char *volts[]={"Smart Sleep Voltage","Cell UVP","Cell UVP Recovery","Cell OVP","Cell OVP Recovery","Balance Trigger Voltage","SOC Full Voltage","SOC Empty Voltage","Request Charge Voltage","Request Float Voltage","Power Off Voltage"};
        for(unsigned i=0;i<11;++i)r.add_number(volts[i],"V",le32(p+6+4*i)/1000.0);
        r.add_number("Max Charge Current","A",le32(p+50)/1000.0);r.add_number("Max Discharge Current","A",le32(p+62)/1000.0);
        r.add_number("Max Balance Current","A",le32(p+78)/1000.0);r.add_number("Number of Cells","",le32(p+114));
        r.add_number("Charge Enabled","",le32(p+118));r.add_number("Discharge Enabled","",le32(p+122));r.add_number("Balance Enabled","",le32(p+126));
        r.add_number("Nominal_Capacity","Ah",le32(p+130)/1000.0);r.add_number("Balance Start Voltage","V",le32(p+138)/1000.0);return CodecStatus::ok;
    }
    const auto present=le32(p+70);unsigned cells=0;
    for(unsigned i=0;i<32;++i)if(present&(std::uint32_t(1)<<i)) { indexed(r,"Voltage_Cell",i+1,"V",le16(p+6+2*i)/1000.0);indexed(r,"Resistance_Cell",i+1,"Ohm",le16(p+80+2*i)/1000.0);++cells; }
    r.add_number("Cells_connected","",cells);r.add_number("Average_Cell_Voltage","V",le16(p+74)/1000.0);r.add_number("Delta_Cell_Voltage","V",le16(p+76)/1000.0);
    r.add_number("Battery_Voltage","V",le32(p+150)/1000.0);r.add_number("Battery_Power","W",le32(p+154)/1000.0);r.add_number("Battery_Current","A",static_cast<std::int32_t>(le32(p+158))/1000.0);
    r.add_number("MOS_Temp","C",static_cast<std::int16_t>(le16(p+144))/10.0);
    if(p[214]&2)r.add_number("Battery_T1","C",static_cast<std::int16_t>(le16(p+162))/10.0);
    if(p[214]&4)r.add_number("Battery_T2","C",static_cast<std::int16_t>(le16(p+164))/10.0);
    r.add_number("Battery Warning Message","",le32(p+166));r.add_number("Balance_Current","A",static_cast<std::int16_t>(le16(p+170))/1000.0);
    if(p[173]>100)return CodecStatus::invalid_frame;
    r.add_number("Percent_Remain","%",p[173]);r.add_number("Capacity_Remain","Ah",le32(p+174)/1000.0);r.add_number("Nominal_Capacity","Ah",le32(p+178)/1000.0);r.add_number("Cycle_Count","",le32(p+182));
    r.add_number("Charge Enabled","",p[198]);r.add_number("Discharge Enabled","",p[199]);return CodecStatus::ok;
}

CodecStatus decode_daly(const Command &c,const std::uint8_t *p,std::size_t n,EnergyResponse &r) {
    if(!n||n%13||n>16*13||(c.code!=0x95&&c.code!=0x96&&n!=13))return CodecStatus::invalid_frame;
    unsigned first=0;
    for(std::size_t at=0;at<n;at+=13) {
        const auto *f=p+at;
        if(f[0]!=0xa5||f[1]!=1||f[2]!=c.code||f[3]!=8)return CodecStatus::invalid_frame;
        if(sum8(f,12)!=f[12])return CodecStatus::checksum_error;
        if(c.code==0x95||c.code==0x96) {
            if(!at){first=f[4];if(first>1)return CodecStatus::invalid_frame;}
            if(f[4]!=first+at/13)return CodecStatus::invalid_frame;
        }
    }
    const auto *d=p+4;
    switch(c.code) {
    case 0x90:
        if(be16(d+6)>1000)return CodecStatus::invalid_frame;
        r.add_number("Battery Bank Voltage","V",be16(d)/10.0);r.add_number("Acquisition Voltage","V",be16(d+2)/10.0);r.add_number("Current","A",(int(be16(d+4))-30000)/10.0);r.add_number("SOC","%",be16(d+6)/10.0);break;
    case 0x91:r.add_number("Maximum Cell Voltage","V",be16(d)/1000.0);r.add_number("Maximum Cell Number","",d[2]);r.add_number("Minimum Cell Voltage","V",be16(d+3)/1000.0);r.add_number("Minimum Cell Number","",d[5]);break;
    case 0x92:r.add_number("Maximum Cell Temperature","C",int(d[0])-40);r.add_number("Maximum Temperature Cell","",d[1]);r.add_number("Minimum Cell Temperature","C",int(d[2])-40);r.add_number("Minimum Temperature Cell","",d[3]);break;
    case 0x93:r.add_number("Charge Status","",d[0]);r.add_number("Charge Enabled","",d[1]);r.add_number("Discharge Enabled","",d[2]);r.add_number("BMS Life","cycles",d[3]);r.add_number("Residual Capacity","Ah",be32(d+4)/1000.0);break;
    case 0x94:
        if(d[0]>48||d[1]>21)return CodecStatus::invalid_frame;
        r.add_number("Number of Cells","",d[0]);r.add_number("Number of Temperature Sensors","",d[1]);r.add_number("Charger Status","",d[2]);r.add_number("Load Status","",d[3]);r.add_number("Digital I/O Flags","",d[4]);break;
    case 0x95:
        for(std::size_t f=0;f<n/13;++f)for(unsigned i=0;i<3;++i) { const auto raw=be16(p+13*f+5+2*i);if(raw)indexed(r,"Voltage_Cell",static_cast<unsigned>(3*f+i+1),"V",raw/1000.0); }
        break;
    case 0x96:
        if(n>3*13)return CodecStatus::invalid_frame;
        for(std::size_t f=0;f<n/13;++f)for(unsigned i=0;i<7;++i)indexed(r,"Battery_T",static_cast<unsigned>(7*f+i+1),"C",int(p[13*f+5+i])-40);
        break;
    case 0x97:for(unsigned i=0;i<48;++i)indexed(r,"Balance_Cell",i+1,"",(d[i/8]>>(i%8))&1);break;
    case 0x98:for(unsigned i=0;i<8;++i)indexed(r,"Fault_Byte",i,"",d[i]);break;
    default:return CodecStatus::invalid_command;
    }
    return CodecStatus::ok;
}

void ved_field(EnergyResponse &r,const char *key,const char *value) {
    struct Scale { const char *key;const char *unit;double divisor; };
    constexpr Scale scales[]={{"V","V",1000},{"V2","V",1000},{"V3","V",1000},{"VS","V",1000},{"VM","V",1000},{"VPV","V",1000},{"I","A",1000},{"I2","A",1000},{"I3","A",1000},{"IL","A",1000},{"P","W",1},{"PPV","W",1},{"SOC","%",10},{"T","C",1},{"CE","Ah",1000},{"TTG","min",1},{"AC_OUT_V","V",100},{"AC_OUT_I","A",10},{"AC_OUT_S","VA",1},{"H1","Ah",1000},{"H2","Ah",1000},{"H3","Ah",1000},{"H6","Ah",1000},{"H7","V",1000},{"H8","V",1000},{"H15","V",1000},{"H16","V",1000},{"H17","kWh",100},{"H18","kWh",100},{"H19","kWh",100},{"H20","kWh",100},{"H21","W",1},{"H22","kWh",100},{"H23","W",1}};
    char *end=nullptr;const double number=std::strtod(value,&end);
    for(const auto &s:scales)if(eq(key,s.key)) { if(end!=value&&!*end&&std::isfinite(number))r.add_number(key,s.unit,number/s.divisor);return; }
    r.add_text(key,"",value);
}
CodecStatus decode_ved(const Command &c,const std::uint8_t *p,std::size_t n,EnergyResponse &r) {
    if(!c.code) {
        std::size_t begin=0,end=0;if(!ved_block(p,n,begin,end))return CodecStatus::checksum_error;
        std::size_t at=begin+2;
        while(at<end) {
            if(p[at]==':'){while(at<end&&p[at++]!=10){}continue;}
            std::size_t tab=at;while(tab<end&&p[tab]!=9&&p[tab]!=13)++tab;
            if(tab>=end||p[tab]!=9)return CodecStatus::invalid_frame;
            char key[40]{};const auto keylen=tab-at;if(!keylen||keylen>=sizeof(key))return CodecStatus::invalid_frame;std::memcpy(key,p+at,keylen);
            if(eq(key,"Checksum"))return CodecStatus::ok;
            std::size_t finish=tab+1;while(finish<end&&p[finish]!=13)++finish;
            if(finish+1>=end||p[finish+1]!=10)return CodecStatus::invalid_frame;
            char value[32]{};const auto count=finish-tab-1;if(count>=sizeof(value))return CodecStatus::invalid_frame;std::memcpy(value,p+tab+1,count);
            ved_field(r,key,value);at=finish+2;
        }
        return CodecStatus::invalid_frame;
    }
    std::size_t start=0,end=0;
    if(!ved_hex_reply(c,p,n,start,end)||end-start<11)return CodecStatus::invalid_frame;
    const unsigned code=nibble(p[start+1]);if(code!=(c.setter?8u:7u))return CodecStatus::invalid_frame;
    const std::size_t chars=end-start-3;if(chars%2||chars/2>32)return CodecStatus::invalid_frame;
    std::uint8_t decoded[32]{};std::uint8_t sum=code;
    for(std::size_t i=0;i<chars/2;++i) { const int high=nibble(p[start+2+2*i]),low=nibble(p[start+3+2*i]);if(high<0||low<0)return CodecStatus::invalid_frame;decoded[i]=(high<<4)|low;sum+=decoded[i]; }
    if(sum!=0x55)return CodecStatus::checksum_error;
    if(le16(decoded)!=c.code)return CodecStatus::invalid_frame;
    if(decoded[2]){r.rejected=true;r.add_number("Command response flag","",decoded[2]);return CodecStatus::rejected;}
    if(chars/2!=6)return CodecStatus::invalid_frame;
    r.add_number("Battery Capacity","Ah",le16(decoded+3));
    if(c.setter) { r.acknowledged=le16(decoded+3)==c.value;r.rejected=!r.acknowledged;return r.rejected?CodecStatus::rejected:CodecStatus::ok; }
    return CodecStatus::ok;
}
} // namespace

std::size_t bms_poll_command_count(Id id) {
    if(ble(id))return 2;
    if(daly(id))return 9;
    if(id==Id::jk232||id==Id::jkpb)return 3;
    if(id==Id::jk485||id==Id::jkserial||id==Id::ved)return 1;
    return 0;
}
const char *bms_poll_command(Id id,std::size_t index) {
    if(index>=bms_poll_command_count(id))return nullptr;
    if(ble(id))return ble_poll[index];
    if(daly(id))return daly_poll[index];
    switch(id) {case Id::jk232:return jk232_poll[index];case Id::jk485:return jk485_poll[index];case Id::jkpb:return jkpb_poll[index];case Id::jkserial:return jkserial_poll[index];case Id::ved:return ved_poll[index];default:return nullptr;}
}
bool bms_command_info(Id id,const char *command,EnergyCommandInfo &info) {
    Command c;if(!parse(id,command,c))return false;info={c.name,c.setter,c.readback};return true;
}

CodecStatus bms_build_request(Id id,const char *command,std::uint8_t address,std::uint8_t *out,std::size_t capacity,std::size_t &size) {
    size=0;if(!supported(id))return CodecStatus::unsupported;Command c;if(!parse(id,command,c))return CodecStatus::invalid_command;
    std::uint8_t bytes[32]{};std::size_t n=0;
    if(ble(id)) {
        n=20;bytes[0]=0xaa;bytes[1]=0x55;bytes[2]=0x90;bytes[3]=0xeb;bytes[4]=c.code;
        if(c.setter){bytes[5]=4;bytes[6]=c.value&255;bytes[7]=(c.value>>8)&255;}bytes[19]=sum8(bytes,19);
    } else if(daly(id)) { n=13;bytes[0]=0xa5;bytes[1]=id==Id::daly?0x80:0x40;bytes[2]=c.code;bytes[3]=8;bytes[12]=sum8(bytes,12); }
    else if(id==Id::jk232) { n=7;bytes[0]=0xdd;bytes[1]=0xa5;bytes[2]=c.code;const auto crc=std::uint16_t(0-c.code);bytes[4]=crc>>8;bytes[5]=crc&255;bytes[6]=0x77; }
    else if(id==Id::jk485) { if(!address)return CodecStatus::invalid_command;n=7;bytes[0]=0x55;bytes[1]=0xaa;bytes[2]=address;bytes[3]=c.code;bytes[4]=c.value>>8;bytes[5]=c.value&255;bytes[6]=sum8(bytes,6); }
    else if(id==Id::jkserial) { n=21;bytes[0]=0x4e;bytes[1]=0x57;bytes[3]=0x13;bytes[8]=3;bytes[9]=3;bytes[16]=0x68;const auto crc=sum16(bytes,17);bytes[19]=crc>>8;bytes[20]=crc&255; }
    else if(id==Id::jkpb) { if(!address)return CodecStatus::invalid_command;n=11;bytes[0]=address;bytes[1]=0x10;bytes[2]=c.code>>8;bytes[3]=c.code&255;bytes[5]=1;bytes[6]=2;const auto crc=modbus_crc(bytes,9);bytes[9]=crc&255;bytes[10]=crc>>8; }
    else if(id==Id::ved&&c.code) {
        const auto code=c.setter?8:7;std::uint8_t payload[5]={std::uint8_t(c.code&255),std::uint8_t(c.code>>8),0,std::uint8_t(c.value&255),std::uint8_t(c.value>>8)};
        std::uint8_t check=0x55-code-sum8(payload,c.setter?5:3);char text[32]{};
        const int length=c.setter?std::snprintf(text,sizeof(text),":8%02X%02X00%02X%02X%02X\n",payload[0],payload[1],payload[3],payload[4],check):std::snprintf(text,sizeof(text),":7%02X%02X00%02X\n",payload[0],payload[1],check);
        n=static_cast<std::size_t>(length);std::memcpy(bytes,text,n);
    }
    if(n>capacity||(n&&!out))return CodecStatus::buffer_too_small;
    if(n)std::memcpy(out,bytes,n);
    size=n;return CodecStatus::ok;
}

FrameState daly_group_frame_state(const char *command,const std::uint8_t *p,std::size_t n,std::size_t expected,std::size_t &frame_size) {
    const bool cells=eq(command,"cellVoltages"),temperatures=eq(command,"cellTemperatures");
    if((!cells&&!temperatures)||!expected||expected>(cells?16u:3u)||(n&&!p))return FrameState::invalid;
    frame_size=13*expected;if(n>frame_size)return FrameState::invalid;
    unsigned start=0;
    for(std::size_t f=0;f*13<n;++f) {
        const auto offset=f*13;const auto *d=p+offset;const auto available=n-offset;
        if(d[0]!=0xa5||(available>1&&d[1]!=1)||(available>2&&d[2]!=(cells?0x95:0x96))||(available>3&&d[3]!=8))return FrameState::invalid;
        if(available>4){if(!f){start=d[4];if(start>1)return FrameState::invalid;}if(d[4]!=start+f)return FrameState::invalid;}
        if(available>=13&&sum8(d,12)!=d[12])return FrameState::invalid;
    }
    return n==frame_size?FrameState::complete:FrameState::incomplete;
}

FrameState bms_frame_state(Id id,const char *command,const std::uint8_t *p,std::size_t n,std::size_t &frame_size) {
    frame_size=0;Command c;if(!parse(id,command,c)||(n&&!p))return FrameState::invalid;if(!n)return FrameState::incomplete;
    if(ble(id)) {
        constexpr std::uint8_t header[]={0x55,0xaa,0xeb,0x90},ack[]={0xaa,0x55,0x90,0xeb};const auto *h=c.setter?ack:header;
        for(std::size_t i=0;i<n&&i<4;++i)if(p[i]!=h[i])return FrameState::invalid;
        if(c.setter)return fixed(p,n,20,frame_size);
        frame_size=300;
        if(n<300)return FrameState::incomplete;
        if(n==300&&sum8(p,299)==p[299])return FrameState::complete;
        frame_size=320;return n<320?FrameState::incomplete:n==320?FrameState::complete:FrameState::invalid;
    }
    if(id==Id::jk232) { if(p[0]!=0xdd)return FrameState::invalid;if(n<4)return FrameState::incomplete;return fixed(p,n,std::size_t(p[3])+7,frame_size); }
    if(id==Id::jk485) { if(p[0]!=0xeb||(n>1&&p[1]!=0x90))return FrameState::invalid;return fixed(p,n,74,frame_size); }
    if(id==Id::jkserial) { if(p[0]!=0x4e||(n>1&&p[1]!=0x57))return FrameState::invalid;if(n<4)return FrameState::incomplete;const auto total=be16(p+2)+2u;if(total<21||total>1024)return FrameState::invalid;return fixed(p,n,total,frame_size); }
    if(id==Id::jkpb) { if(p[0]!=0x55||(n>1&&p[1]!=0xaa))return FrameState::invalid;return fixed(p,n,308,frame_size); }
    if(daly(id)) {
        // Unknown group counts must not be guessed from a first frame. Call the
        // explicit group inspector once the preceding status supplies them.
        if(c.code==0x95||c.code==0x96)return daly_group_frame_state(command,p,n,c.code==0x95?16:3,frame_size);
        if(p[0]!=0xa5||(n>1&&p[1]!=1)||(n>2&&p[2]!=c.code)||(n>3&&p[3]!=8))return FrameState::invalid;
        return fixed(p,n,13,frame_size);
    }
    if(id==Id::ved) {
        if(c.code) {
            if(n>1024)return FrameState::invalid;
            std::size_t start=0,end=0;
            if(ved_hex_reply(c,p,n,start,end)){frame_size=end;return FrameState::complete;}
            return FrameState::incomplete;
        }
        std::size_t start=0,end=0;if(ved_block(p,n,start,end)){frame_size=end;return FrameState::complete;}return n>=1024?FrameState::invalid:FrameState::incomplete;
    }
    return FrameState::invalid;
}

CodecStatus bms_decode_response(Id id,const char *command,std::uint8_t address,const std::uint8_t *p,std::size_t n,EnergyResponse &r) {
    r.count=0;r.acknowledged=false;r.rejected=false;
    if(!supported(id))return CodecStatus::unsupported;
    Command c;
    if(!parse(id,command,c))return CodecStatus::invalid_command;
    if(!p||!n)return CodecStatus::invalid_frame;
    CodecStatus result=CodecStatus::unsupported;
    if(ble(id))result=decode_ble(id,c,p,n,r);
    else if(daly(id))result=decode_daly(c,p,n,r);
    else switch(id) {case Id::jk232:result=decode_jk232(c,p,n,r);break;case Id::jk485:result=decode_jk485(c,address,p,n,r);break;case Id::jkserial:result=decode_jkserial(p,n,r);break;case Id::jkpb:result=decode_jkpb(c,address,p,n,r);break;case Id::ved:result=decode_ved(c,p,n,r);break;default:break;}
    if(result!=CodecStatus::ok&&result!=CodecStatus::rejected)r.count=0;
    return result;
}
} // namespace inverter_gateway::protocol
