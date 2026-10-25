#include "inverter_gateway/protocol/bms_protocols.hpp"
#include "bms_protocol_vectors.generated.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

using namespace inverter_gateway::protocol;

#ifdef ENERGY_BMS_STANDALONE
bool EnergyResponse::add_number(const char *key,const char *unit,double v) {
    if(count>=maximum_fields||!std::isfinite(v))return false;
    auto &f=fields[count++];f={};std::snprintf(f.key,sizeof(f.key),"%s",key);std::snprintf(f.unit,sizeof(f.unit),"%s",unit);f.value=v;return true;
}
bool EnergyResponse::add_text(const char *key,const char *unit,const char *v) {
    if(count>=maximum_fields)return false;
    auto &f=fields[count++];f={};f.numeric=false;std::snprintf(f.key,sizeof(f.key),"%s",key);std::snprintf(f.unit,sizeof(f.unit),"%s",unit);std::snprintf(f.text,sizeof(f.text),"%s",v);return true;
}
#endif
static EnergyProtocol profile(const char *name) {
    struct P {const char *name;EnergyProtocol id;};
    static constexpr P ids[]={{"JK02",EnergyProtocol::jk02},{"JK02_32",EnergyProtocol::jk02_32},{"JK04",EnergyProtocol::jk04},{"JKV11",EnergyProtocol::jkv11},{"JK232",EnergyProtocol::jk232},{"JK485",EnergyProtocol::jk485},{"JKSERIAL",EnergyProtocol::jkserial},{"JKPB",EnergyProtocol::jkpb},{"DALY",EnergyProtocol::daly}};
    for(const auto &p:ids)if(!std::strcmp(name,p.name))return p.id;
    std::abort();
}
static std::vector<std::uint8_t> fromhex(const char *text) {
    std::vector<std::uint8_t> bytes;
    while(*text) { char s[3]={text[0],text[1],0};bytes.push_back(static_cast<std::uint8_t>(std::strtoul(s,nullptr,16)));text+=2; }
    return bytes;
}
static double value(const EnergyResponse &response,const char *key) {
    for(std::size_t i=0;i<response.count;++i)if(!std::strcmp(response.fields[i].key,key))return response.fields[i].value;
    std::fprintf(stderr,"Missing field %s\n",key);std::abort();
}
static void near(double actual,double expected) { assert(std::fabs(actual-expected)<0.000001); }
static std::uint8_t sum(const std::vector<std::uint8_t> &p,std::size_t n) {std::uint8_t s=0;for(std::size_t i=0;i<n;++i)s+=p[i];return s;}
static std::vector<std::uint8_t> textframe(const std::string &fields) {
    std::vector<std::uint8_t> p(fields.begin(),fields.end());
    const std::string check="\r\nChecksum\t";p.insert(p.end(),check.begin(),check.end());p.push_back(0-sum(p,p.size()));return p;
}
static unsigned setting_checks=0;
#define SETTING_CHECK(condition) do { ++setting_checks; if(!(condition)) { \
    std::fprintf(stderr,"Setting check failed at line %d: %s\n",__LINE__,#condition);std::abort(); \
} } while(false)
static std::vector<std::uint8_t> jk485_ack(unsigned code,unsigned requested) {
    std::vector<std::uint8_t> bytes(74);bytes[0]=0xeb;bytes[1]=0x90;bytes[2]=1;bytes[3]=code;
    bytes[4]=requested>>8;bytes[5]=requested;bytes[73]=sum(bytes,73);return bytes;
}
static std::vector<std::uint8_t> ved_hex(unsigned operation,unsigned reg,unsigned flags,
                                         unsigned observed,bool include_value=true) {
    const unsigned checksum=(0x55-operation-(reg&255)-(reg>>8)-flags-
        (include_value?((observed&255)+(observed>>8)):0))&255;
    char text[32]{};
    if(include_value)std::snprintf(text,sizeof(text),":%X%02X%02X%02X%02X%02X%02X\n",
        operation,reg&255,reg>>8,flags,observed&255,observed>>8,checksum);
    else std::snprintf(text,sizeof(text),":%X%02X%02X%02X%02X\n",operation,reg&255,reg>>8,flags,checksum);
    return std::vector<std::uint8_t>(text,text+std::strlen(text));
}
static void settings_contracts() {
    struct Setter { EnergyProtocol id;const char *command;const char *request_hex;
        const char *readback;const char *label;const char *unit;double observed;
        unsigned code;unsigned raw; };
    // Golden request fields and ACK layouts are from the JK balancer V1.3
    // manufacturer tables, recorded JK02 commands, and Victron BMV HEX Rev7.
    const Setter setters[]={
        {EnergyProtocol::jk02,"setChargingOn","aa5590eb1d04010000000000000000000000009c","getCellData","Charge Enabled","",1,0x1d,1},
        {EnergyProtocol::jk02,"setChargingOff","aa5590eb1d04000000000000000000000000009b","getCellData","Charge Enabled","",0,0x1d,0},
        {EnergyProtocol::jk02,"setDischargingOn","aa5590eb1e04010000000000000000000000009d","getCellData","Discharge Enabled","",1,0x1e,1},
        {EnergyProtocol::jk02,"setDischargingOff","aa5590eb1e04000000000000000000000000009c","getCellData","Discharge Enabled","",0,0x1e,0},
        {EnergyProtocol::jk485,"setCellCount16","55aa01f0001000","getBalancerData","Set Number of Cells","",16,0xf0,16},
        {EnergyProtocol::jk485,"setBalanceTrigger10","55aa01f2000afc","getBalancerData","Balance Trigger Voltage","V",0.01,0xf2,10},
        {EnergyProtocol::jk485,"setMaxBalanceCurrent500","55aa01f401f4e9","getBalancerData","Max Balance Current","A",0.5,0xf4,500},
        {EnergyProtocol::jk485,"setBalanceOn","55aa01f60001f7","getBalancerData","Balance On / Off","",1,0xf6,1},
        {EnergyProtocol::jk485,"setBalanceOff","55aa01f60000f6","getBalancerData","Balance On / Off","",0,0xf6,0},
        {EnergyProtocol::ved,"setBatteryCapacity200","3a383030313030304338303037350a","batteryCapacity","Battery Capacity","Ah",200,8,200},
    };
    const char *normalized_keys[]={"charge_enabled","charge_enabled","discharge_enabled","discharge_enabled",
        "set_number_of_cells","balance_trigger_voltage","max_balance_current","balance_on_off","balance_on_off","battery_capacity"};
    std::uint8_t request[64]{};std::size_t request_size=0,frame_size=0;
    EnergyResponse response,readback;EnergyCommandInfo info;bool matches=true;
    for(const auto &s:setters) {
        SETTING_CHECK(bms_command_info(s.id,s.command,info)&&info.setter&&
                      std::strcmp(info.readback_command,s.readback)==0);
        SETTING_CHECK(bms_build_request(s.id,s.command,1,request,sizeof(request),request_size)==CodecStatus::ok);
        const auto expected=fromhex(s.request_hex);
        SETTING_CHECK(request_size==expected.size()&&!std::memcmp(request,expected.data(),request_size));
        std::vector<std::uint8_t> ack;
        if(s.id==EnergyProtocol::jk02)ack=fromhex("aa5590ebc8010100000000000000000000000044");
        else if(s.id==EnergyProtocol::jk485)ack=jk485_ack(s.code,s.raw);
        else ack=ved_hex(8,0x1000,0,s.raw);
        for(std::size_t n=0;n<ack.size();++n)
            SETTING_CHECK(bms_frame_state(s.id,s.command,ack.data(),n,frame_size)==FrameState::incomplete);
        SETTING_CHECK(bms_frame_state(s.id,s.command,ack.data(),ack.size(),frame_size)==FrameState::complete&&frame_size==ack.size());
        SETTING_CHECK(bms_decode_response(s.id,s.command,1,ack.data(),ack.size(),response)==CodecStatus::ok&&response.acknowledged&&!response.rejected);
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,s.readback,response,matches)&&!matches);
        auto corrupted=ack;corrupted[6]^=1;
        SETTING_CHECK(bms_decode_response(s.id,s.command,1,corrupted.data(),corrupted.size(),response)!=CodecStatus::ok&&!response.acknowledged&&!response.count);
        SETTING_CHECK(bms_decode_response(s.id,s.command,1,ack.data(),ack.size()-1,response)!=CodecStatus::ok&&!response.acknowledged&&!response.count);
        if(s.id==EnergyProtocol::jk02) {
            auto rejected=ack;rejected[6]=0;rejected[19]=sum(rejected,19);
            SETTING_CHECK(bms_decode_response(s.id,s.command,1,rejected.data(),rejected.size(),response)==CodecStatus::rejected&&response.rejected&&!response.acknowledged);
            auto unrelated=ack;unrelated[4]=0xc9;unrelated[19]=sum(unrelated,19);
            SETTING_CHECK(bms_decode_response(s.id,s.command,1,unrelated.data(),unrelated.size(),response)==CodecStatus::invalid_frame&&!response.acknowledged);
        } else if(s.id==EnergyProtocol::jk485) {
            const auto mismatched=jk485_ack(s.code,s.raw+1);
            SETTING_CHECK(bms_decode_response(s.id,s.command,1,mismatched.data(),mismatched.size(),response)==CodecStatus::rejected&&response.rejected&&!response.acknowledged);
            auto unrelated=ack;unrelated[2]=2;unrelated[73]=sum(unrelated,73);
            SETTING_CHECK(bms_decode_response(s.id,s.command,1,unrelated.data(),unrelated.size(),response)==CodecStatus::invalid_frame&&!response.acknowledged);
            unrelated=ack;unrelated[3]=0xff;unrelated[73]=sum(unrelated,73);
            SETTING_CHECK(bms_decode_response(s.id,s.command,1,unrelated.data(),unrelated.size(),response)==CodecStatus::invalid_frame&&!response.acknowledged);
        } else {
            const auto mismatched=ved_hex(8,0x1000,0,s.raw+1);
            SETTING_CHECK(bms_decode_response(s.id,s.command,1,mismatched.data(),mismatched.size(),response)==CodecStatus::rejected&&response.rejected&&!response.acknowledged);
            for(unsigned flag:{1u,2u,4u}) {
                const auto rejected=ved_hex(8,0x1000,flag,0,false);
                SETTING_CHECK(bms_decode_response(s.id,s.command,1,rejected.data(),rejected.size(),response)==CodecStatus::rejected&&response.rejected&&!response.acknowledged);
            }
            for(const auto &unrelated:{ved_hex(7,0x1000,0,s.raw),ved_hex(8,0x1001,0,s.raw),ved_hex(0xa,0x1000,0,s.raw)}) {
                SETTING_CHECK(bms_frame_state(s.id,s.command,unrelated.data(),unrelated.size(),frame_size)==FrameState::incomplete);
                SETTING_CHECK(bms_decode_response(s.id,s.command,1,unrelated.data(),unrelated.size(),response)==CodecStatus::invalid_frame&&!response.acknowledged);
            }
        }
        readback={};readback.add_number(s.label,s.unit,s.observed);
        SETTING_CHECK(bms_setting_readback(s.id,s.command,s.readback,readback,matches)&&matches);
        std::snprintf(readback.fields[0].key,sizeof(readback.fields[0].key),"%s",normalized_keys[&s-setters]);
        SETTING_CHECK(bms_setting_readback(s.id,s.command,s.readback,readback,matches)&&matches);
        readback.rejected=true;
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,s.readback,readback,matches));
        readback.rejected=false;readback.acknowledged=true;
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,s.readback,readback,matches));
        readback.acknowledged=false;
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,"wrongQuery",readback,matches)&&!matches);
        readback.fields[0].value=s.observed==0?1:0;
        SETTING_CHECK(bms_setting_readback(s.id,s.command,s.readback,readback,matches)&&!matches);
        readback.fields[0].value=s.observed;readback.fields[0].numeric=false;
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,s.readback,readback,matches));
        readback.fields[0].numeric=true;std::strcpy(readback.fields[0].unit,"bad");
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,s.readback,readback,matches));
        readback={};
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,s.readback,readback,matches));
        readback.add_number(s.label,s.unit,s.observed);readback.add_number(s.label,s.unit,s.observed);
        SETTING_CHECK(!bms_setting_readback(s.id,s.command,s.readback,readback,matches));
    }
    struct IntegerSetting {EnergyProtocol id;const char *prefix;unsigned low,high;};
    const IntegerSetting integers[]={
        {EnergyProtocol::jk485,"setCellCount",2,24},
        {EnergyProtocol::jk485,"setBalanceTrigger",2,1000},
        {EnergyProtocol::jk485,"setMaxBalanceCurrent",30,1000},
        {EnergyProtocol::ved,"setBatteryCapacity",1,9999},
    };
    for(const auto &s:integers) {
        for(unsigned valid:{s.low,s.high}) {
            const auto command=std::string(s.prefix)+std::to_string(valid);
            SETTING_CHECK(bms_command_info(s.id,command.c_str(),info)&&info.setter);
        }
        for(const auto &suffix:{std::string(),std::to_string(s.low-1),std::to_string(s.high+1),
            std::to_string(s.low)+".0",std::to_string(s.low)+".",std::string("+2"),std::string("-2"),
            std::string(" 2"),std::string("2 "),std::string("2e1"),std::string("NaN"),
            std::string("2\n"),std::string("2junk"),std::string("42949672962")}) {
            const auto command=std::string(s.prefix)+suffix;
            SETTING_CHECK(!bms_command_info(s.id,command.c_str(),info));
            SETTING_CHECK(bms_build_request(s.id,command.c_str(),1,request,sizeof(request),request_size)==CodecStatus::invalid_command&&request_size==0);
        }
    }
    constexpr EnergyProtocol readonly[]={EnergyProtocol::jk02_32,EnergyProtocol::jk04,EnergyProtocol::jk232,
        EnergyProtocol::jkpb,EnergyProtocol::jkv11,EnergyProtocol::jkserial,EnergyProtocol::daly,EnergyProtocol::daly40};
    for(const auto id:readonly)for(const auto &s:setters) {
        SETTING_CHECK(!bms_command_info(id,s.command,info));
        SETTING_CHECK(bms_build_request(id,s.command,1,request,sizeof(request),request_size)==CodecStatus::invalid_command&&request_size==0);
    }
    // Integration with actual status decoding: configured cell count differs
    // from the detected physical count; numeric settings carry SI units.
    auto status=jk485_ack(0xff,0);status[8]=12;status[22]=16;status[17]=0;status[18]=10;
    status[19]=1;status[20]=0xf4;status[21]=1;status[73]=sum(status,73);
    SETTING_CHECK(bms_decode_response(EnergyProtocol::jk485,"getBalancerData",1,status.data(),status.size(),response)==CodecStatus::ok);
    for(const char *command:{"setCellCount16","setBalanceTrigger10","setMaxBalanceCurrent500","setBalanceOn"})
        SETTING_CHECK(bms_setting_readback(EnergyProtocol::jk485,command,"getBalancerData",response,matches)&&matches);
    auto jk=fromhex(bms_vectors[0].hex);jk[166]=1;jk[167]=0;jk.back()=sum(jk,jk.size()-1);
    SETTING_CHECK(bms_decode_response(EnergyProtocol::jk02,"getCellData",1,jk.data(),jk.size(),response)==CodecStatus::ok);
    SETTING_CHECK(bms_setting_readback(EnergyProtocol::jk02,"setChargingOn","getCellData",response,matches)&&matches);
    SETTING_CHECK(bms_setting_readback(EnergyProtocol::jk02,"setDischargingOff","getCellData",response,matches)&&matches);
    jk[166]=2;jk.back()=sum(jk,jk.size()-1);
    SETTING_CHECK(bms_decode_response(EnergyProtocol::jk02,"getCellData",1,jk.data(),jk.size(),response)==CodecStatus::ok);
    SETTING_CHECK(!bms_setting_readback(EnergyProtocol::jk02,"setChargingOn","getCellData",response,matches));
    const auto capacity=ved_hex(7,0x1000,0,200);
    SETTING_CHECK(bms_decode_response(EnergyProtocol::ved,"batteryCapacity",1,capacity.data(),capacity.size(),response)==CodecStatus::ok);
    SETTING_CHECK(bms_setting_readback(EnergyProtocol::ved,"setBatteryCapacity200","batteryCapacity",response,matches)&&matches);
    SETTING_CHECK(!bms_setting_readback(EnergyProtocol::ved,"batteryCapacity","batteryCapacity",response,matches));
}
int main() {
    unsigned valid=0,invalid=0;
    EnergyResponse response;
    for(const auto &v:bms_vectors) {
        auto data=fromhex(v.hex);const auto id=profile(v.profile);
        const auto status=bms_decode_response(id,v.command,1,data.data(),data.size(),response);
        const bool truncated=(id==EnergyProtocol::jk02&&data.size()==299)||(id==EnergyProtocol::jkserial&&data.size()==50)||(id==EnergyProtocol::daly&&data.size()==135);
        if(truncated) {assert(status!=CodecStatus::ok);assert(!response.count);++invalid;continue;}
        if(status!=CodecStatus::ok){std::fprintf(stderr,"Rejected valid fixture %s %s size=%zu status=%u\n",v.profile,v.command,data.size(),unsigned(status));return 1;}
        assert(response.count);assert(!response.acknowledged);++valid;
        // A single-bit error in payload cannot escape checksum validation.
        data[10]^=1;
        assert(bms_decode_response(id,v.command,1,data.data(),data.size(),response)!=CodecStatus::ok);assert(!response.count);
    }
    auto jk=fromhex(bms_vectors[0].hex);
    assert(bms_decode_response(EnergyProtocol::jk02,"getCellData",1,jk.data(),jk.size(),response)==CodecStatus::ok);
    near(value(response,"Battery_Voltage"),53.692);near(value(response,"Percent_Remain"),99);near(value(response,"Voltage_Cell01"),3.374);

    // Signed JBD/JK232 current, corrected two's-complement query checksum,
    // and temperature count bounds; the upstream checksum helper was wrong.
    std::uint8_t request[64]{};std::size_t request_size=99;
    assert(bms_build_request(EnergyProtocol::jk232,"getBalancerData",1,request,sizeof(request),request_size)==CodecStatus::ok);
    assert(request_size==7&&request[4]==0xff&&request[5]==0xfd);
    assert(bms_build_request(EnergyProtocol::daly,"SOC",1,request,sizeof(request),request_size)==CodecStatus::ok&&request_size==13&&request[1]==0x80);
    assert(bms_build_request(EnergyProtocol::daly40,"SOC",1,request,sizeof(request),request_size)==CodecStatus::ok&&request[1]==0x40);
    assert(bms_build_request(EnergyProtocol::jkpb,"getCellData",1,request,sizeof(request),request_size)==CodecStatus::ok);
    const auto pb_request=fromhex("011016200001020000d6f1");assert(request_size==pb_request.size()&&!std::memcmp(request,pb_request.data(),request_size));
    assert(bms_build_request(EnergyProtocol::jkpb,"getCellData",2,request,sizeof(request),request_size)==CodecStatus::ok&&request[0]==2&&request[9]!=0xd6);

    EnergyCommandInfo info;
    assert(!bms_command_info(EnergyProtocol::jk02,"setCellOVP9.99",info));
    assert(!bms_command_info(EnergyProtocol::jk02_32,"setChargingOn",info));
    assert(bms_command_info(EnergyProtocol::jk485,"setCellCount16",info)&&info.setter);
    assert(!bms_command_info(EnergyProtocol::jk485,"setCellCount25",info));
    assert(!bms_command_info(EnergyProtocol::jk485,"setBalanceTrigger1",info));
    assert(!bms_command_info(EnergyProtocol::jk485,"setMaxBalanceCurrent1001",info));
    assert(!bms_command_info(EnergyProtocol::jk485,"setCellCount16junk",info));
    assert(bms_build_request(EnergyProtocol::jk485,"setBalanceTrigger10",1,request,sizeof(request),request_size)==CodecStatus::ok);
    const auto balance_request=fromhex("55aa01f2000afc");assert(request_size==7&&!std::memcmp(request,balance_request.data(),7));
    auto ack=fromhex("aa5590ebc8010100000000000000000000000044");
    assert(bms_decode_response(EnergyProtocol::jk02,"setChargingOn",1,ack.data(),ack.size(),response)==CodecStatus::ok&&response.acknowledged);
    ack[6]=0;ack[19]=sum(ack,19);
    assert(bms_decode_response(EnergyProtocol::jk02,"setChargingOn",1,ack.data(),ack.size(),response)==CodecStatus::rejected&&response.rejected&&!response.acknowledged);

    auto vd=textframe("\r\nPID\t0xA389\r\nV\t53509\r\nI\t-23866\r\nSOC\t548\r\nAC_OUT_V\t23000\r\nAC_OUT_I\t123");
    assert(bms_build_request(EnergyProtocol::ved,"vedtext",1,request,sizeof(request),request_size)==CodecStatus::ok&&request_size==0);
    assert(bms_decode_response(EnergyProtocol::ved,"vedtext",1,vd.data(),vd.size(),response)==CodecStatus::ok);
    near(value(response,"V"),53.509);near(value(response,"I"),-23.866);near(value(response,"SOC"),54.8);near(value(response,"AC_OUT_V"),230);near(value(response,"AC_OUT_I"),12.3);
    std::size_t frame_size=0;
    for(std::size_t n=0;n<vd.size();++n)assert(bms_frame_state(EnergyProtocol::ved,"vedtext",vd.data(),n,frame_size)==FrameState::incomplete);
    assert(bms_frame_state(EnergyProtocol::ved,"vedtext",vd.data(),vd.size(),frame_size)==FrameState::complete&&frame_size==vd.size());
    // Embedded HEX does not contribute to the TEXT checksum.
    auto mixed=vd;const std::string hex=":70010007800C6\n";mixed.insert(mixed.begin()+2,hex.begin(),hex.end());
    assert(bms_decode_response(EnergyProtocol::ved,"vedtext",1,mixed.data(),mixed.size(),response)==CodecStatus::ok);
    auto vh=fromhex("3a373030313030303738303043360a");
    assert(bms_decode_response(EnergyProtocol::ved,"batteryCapacity",1,vh.data(),vh.size(),response)==CodecStatus::ok);near(value(response,"Battery Capacity"),120);
    // An active HEX query can receive background TEXT and asynchronous HEX
    // notifications first. Neither completes the transaction before its own
    // matching command/register line, even when delivered one byte at a time.
    const std::string background="\r\nV\t53509\r\n:9FF0F00260210\n";
    std::vector<std::uint8_t> hex_stream(background.begin(),background.end());
    hex_stream.insert(hex_stream.end(),vh.begin(),vh.end());
    for(std::size_t n=0;n<hex_stream.size();++n)
        assert(bms_frame_state(EnergyProtocol::ved,"batteryCapacity",hex_stream.data(),n,frame_size)==FrameState::incomplete);
    assert(bms_frame_state(EnergyProtocol::ved,"batteryCapacity",hex_stream.data(),hex_stream.size(),frame_size)==FrameState::complete&&frame_size==hex_stream.size());
    assert(bms_decode_response(EnergyProtocol::ved,"batteryCapacity",1,hex_stream.data(),hex_stream.size(),response)==CodecStatus::ok);
    near(value(response,"Battery Capacity"),120);
    assert(bms_build_request(EnergyProtocol::ved,"batteryCapacity",1,request,sizeof(request),request_size)==CodecStatus::ok);
    assert(request_size==11&&!std::memcmp(request,":70010003E\n",11));
    assert(!bms_command_info(EnergyProtocol::ved,"setBatteryCapacity10000",info));
    assert(bms_command_info(EnergyProtocol::ved,"setBatteryCapacity200",info)&&info.setter);
    assert(bms_build_request(EnergyProtocol::jk02,"getCellData",1,request,1,request_size)==CodecStatus::buffer_too_small&&request_size==0);
    assert(bms_decode_response(EnergyProtocol::jk02,"getCellData",1,nullptr,0,response)==CodecStatus::invalid_frame);
    settings_contracts();
    std::printf("BMS codecs: %u valid recorded fixtures; %u truncated fixtures rejected; %u settings checks across all 10 setters and 8 read-only profiles passed.\n",valid,invalid,setting_checks);
}
