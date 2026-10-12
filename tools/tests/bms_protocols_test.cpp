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
    std::printf("BMS codecs: %u valid recorded fixtures; %u truncated fixtures rejected; framing, checksum, settings and VE.Direct assertions passed.\n",valid,invalid);
}
