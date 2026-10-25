// Run the production AOHAI command services against scripted Modbus method
// boundaries. This test never creates a port, transmits bytes or reaches hardware.
#include "inverter_gateway/inverter/command_service.hpp"
#include "inverter_gateway/inverter/register_command_service.hpp"
#include "inverter_gateway/inverter/register_value.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace inverter_gateway;
using namespace inverter_gateway::inverter;
static unsigned checks=0, admitted_descriptors=0, denied_descriptors=0;
#define CHECK(condition) do { ++checks; if(!(condition)) { \
    std::fprintf(stderr,"AOHAI check failed at line %d: %s\n",__LINE__,#condition);std::abort(); \
} } while(false)
namespace scripted {
enum class Operation {holding,input,single,multiple};
struct Step {
    Operation operation;std::uint16_t address;std::vector<std::uint16_t> words;
    bool success=true;protocol::ModbusWriteStatus status=protocol::ModbusWriteStatus::success;
};
static std::vector<Step> steps;static std::size_t next=0;
static unsigned writes=0,inputs=0;
static void reset(std::initializer_list<Step> script={}) {steps=script;next=0;writes=0;inputs=0;}
static void set(std::vector<Step> script) {steps=std::move(script);next=0;writes=0;inputs=0;}
static Step take(Operation op,std::uint16_t address,std::size_t count) {
    if(next>=steps.size())std::fprintf(stderr,"Unexpected Modbus call op=%u address=%u count=%zu\n",unsigned(op),address,count);
    CHECK(next<steps.size());const auto step=steps[next++];
    CHECK(step.operation==op);CHECK(step.address==address);CHECK(step.words.size()==count);
    if(op==Operation::single||op==Operation::multiple)++writes;
    if(op==Operation::input)++inputs;
    return step;
}
static void done() {CHECK(next==steps.size());}
static Step h(std::uint16_t address,std::initializer_list<std::uint16_t> words,bool success=true) {return {Operation::holding,address,words,success};}
static Step r(std::uint16_t value,bool success=true) {return {Operation::input,0,{value},success};}
static Step w(std::uint16_t address,std::initializer_list<std::uint16_t> words,
              protocol::ModbusWriteStatus status=protocol::ModbusWriteStatus::success) {return {words.size()==1?Operation::single:Operation::multiple,address,words,true,status};}
}
namespace inverter_gateway::protocol {
ModbusRtuClient::ModbusRtuClient(CdcAcmDevice &device,std::uint8_t slave,std::uint32_t timeout,std::uint32_t gap)
    : device_(device),slave_address_(slave),response_timeout_ms_(timeout),minimum_command_gap_ticks_(gap) {}
bool ModbusRtuClient::read_holding(std::uint16_t address,std::uint16_t count,std::uint16_t *out,std::size_t capacity) {
    CHECK(out&&capacity>=count);const auto step=scripted::take(scripted::Operation::holding,address,count);
    if(step.success)std::copy(step.words.begin(),step.words.end(),out);
    return step.success;
}
bool ModbusRtuClient::read_input(std::uint16_t address,std::uint16_t count,std::uint16_t *out,std::size_t capacity) {
    CHECK(out&&capacity>=count);const auto step=scripted::take(scripted::Operation::input,address,count);
    if(step.success)std::copy(step.words.begin(),step.words.end(),out);
    return step.success;
}
ModbusWriteResult ModbusRtuClient::write_single(std::uint16_t address,std::uint16_t value) {
    const auto step=scripted::take(scripted::Operation::single,address,1);CHECK(step.words[0]==value);
    return {step.status,step.status==ModbusWriteStatus::exception_response?std::uint8_t(2):std::uint8_t(0)};
}
ModbusWriteResult ModbusRtuClient::write_multiple(std::uint16_t address,std::uint16_t count,const std::uint16_t *values) {
    const auto step=scripted::take(scripted::Operation::multiple,address,count);
    CHECK(std::equal(step.words.begin(),step.words.end(),values));return {step.status,0};
}
}
static InverterStateSnapshot stopped() {return {InverterOperatingState::stopped,0,true,true};}
static RegisterCommandRequest named(std::uint16_t address,std::uint16_t count=1) {
    RegisterCommandRequest request{};request.command_id=123;request.first_register=address;
    request.register_count=count;request.named_request=true;request.confirmed=true;
    request.guarded_interlock=true;request.commissioning_interlock=true;request.service_interlock=true;
    return request;
}
static void convenience_settings(protocol::ModbusRtuClient &client) {
    using namespace scripted;
    CommandService commands(client);auto state=stopped();
    struct Boolean {LocalOperation operation;std::uint16_t address;};
    const Boolean booleans[]={{LocalOperation::set_buzzer_enabled,207},
        {LocalOperation::set_bluetooth_enabled,231},{LocalOperation::set_overload_to_bypass_enabled,208}};
    for(const auto &setting:booleans) {
        LocalCommandRequest request{1,setting.operation,1};
        reset({h(setting.address,{0}),w(setting.address,{1}),h(setting.address,{1})});
        CHECK(commands.execute(request,state).outcome==CommandOutcome::confirmed);CHECK(writes==1);done();
        reset({h(setting.address,{1})});
        CHECK(commands.execute(request,state).outcome==CommandOutcome::confirmed);CHECK(writes==0);done();
        for(const int invalid:{-1,2,65536}) {
            request.value=invalid;reset();
            CHECK(commands.execute(request,state).outcome==CommandOutcome::invalid_request);done();
        }
    }
    struct Feature {LocalOperation operation;unsigned bit;};
    const Feature features[]={{LocalOperation::set_eco_mode_enabled,0},
        {LocalOperation::set_overload_restart_enabled,1},{LocalOperation::set_overtemperature_restart_enabled,2},
        {LocalOperation::set_input_change_reminder_enabled,3},{LocalOperation::set_output_split_phase_enabled,4},
        {LocalOperation::set_generator_auto_input_enabled,5},{LocalOperation::set_dual_channel_load_enabled,6},
        {LocalOperation::set_grid_feedback_enabled,7}};
    for(const auto &feature:features)for(const unsigned value:{0u,1u}) {
        const std::uint16_t before=value?0xa500:0xa5ff;
        const std::uint16_t after=value?std::uint16_t(before|(1u<<feature.bit)):std::uint16_t(before&~(1u<<feature.bit));
        LocalCommandRequest request{2,feature.operation,std::int32_t(value)};
        reset({h(201,{before}),w(201,{after}),h(201,{after})});
        const auto actual=commands.execute(request,state);
        CHECK(actual.outcome==CommandOutcome::confirmed&&actual.requested_register_value==after);
        CHECK((actual.requested_register_value&~(1u<<feature.bit))==(before&~(1u<<feature.bit)));CHECK(writes==1);done();
        reset({h(201,{after})});
        CHECK(commands.execute(request,state).outcome==CommandOutcome::confirmed);CHECK(writes==0);done();
        auto active=state;active.state=InverterOperatingState::operating;reset();
        CHECK(commands.execute(request,active).outcome==CommandOutcome::state_not_allowed);done();
        active.fresh=false;reset();
        CHECK(commands.execute(request,active).outcome==CommandOutcome::state_unavailable);done();
    }
    for(const int value:{0,1,2,3,4,5}) {
        LocalCommandRequest request{3,LocalOperation::set_battery_type,value};
        reset({h(125,{65535}),w(125,{std::uint16_t(value)}),h(125,{std::uint16_t(value)})});
        CHECK(commands.execute(request,state).outcome==CommandOutcome::confirmed);CHECK(writes==1);done();
        auto no_interlock=state;no_interlock.battery_commissioning_interlock=false;reset();
        CHECK(commands.execute(request,no_interlock).outcome==CommandOutcome::state_not_allowed);done();
    }
    for(const int value:{-1,6,65535}) {
        LocalCommandRequest request{3,LocalOperation::set_battery_type,value};reset();
        CHECK(commands.execute(request,state).outcome==CommandOutcome::invalid_request);done();
    }
}
static void runtime_commands(protocol::ModbusRtuClient &client) {
    using namespace scripted;
    RegisterCommandService registers(client);CommandService commands(client);
    auto request=named(0);auto state=stopped();state.state=InverterOperatingState::operating;
    // A zero command latch is not proof that Off was sent or the inverter stopped.
    reset({h(0,{0}),w(0,{0}),h(0,{0}),r(1)});
    auto result=registers.execute(request,state,true);
    CHECK(result.outcome==RegisterCommandOutcome::transition_pending);CHECK(writes==1&&inputs==1);done();
    reset({h(0,{0}),w(0,{0}),h(0,{0}),r(0)});
    result=registers.execute(request,state,true);
    CHECK(result.outcome==RegisterCommandOutcome::completed);CHECK(writes==1);done();
    // Off remains available even when state telemetry is missing or faulted.
    for(const auto blocked_state:{InverterOperatingState::unknown,InverterOperatingState::faulted,InverterOperatingState::updating}) {
        state.state=blocked_state;state.fresh=false;
        reset({h(0,{1}),w(0,{0}),h(0,{0}),r(0)});
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::completed);CHECK(writes==1);done();
    }
    // H0 pre-read/readback can fail or reset; a fresh R0 verifies the transition.
    LocalCommandRequest control{321,LocalOperation::set_inverter_enabled,0};
    for(const bool previous_valid:{false,true})for(const bool readback_valid:{false,true}) {
        reset({h(0,{1},previous_valid),w(0,{0}),h(0,{1},readback_valid),r(0)});
        const auto actual=commands.execute(control,state);
        CHECK(actual.outcome==CommandOutcome::completed&&actual.related_status_valid);
        CHECK(actual.previous_value_valid==previous_valid&&actual.readback_value_valid==readback_valid);
        CHECK(writes==1&&inputs==1);done();
    }
    reset({h(0,{0}),w(0,{0}),h(0,{0}),r(0,false)});
    CHECK(commands.execute(control,state).outcome==CommandOutcome::transition_pending);CHECK(writes==1);done();
    // Negative/uncertain write results never trigger readback or an automatic retry.
    for(const auto status:{protocol::ModbusWriteStatus::exception_response,protocol::ModbusWriteStatus::echo_mismatch,
        protocol::ModbusWriteStatus::timeout,protocol::ModbusWriteStatus::receive_busy,protocol::ModbusWriteStatus::transmit_failed}) {
        reset({h(0,{0}),w(0,{0},status)});
        const auto actual=commands.execute(control,state);
        CHECK(actual.outcome!=CommandOutcome::completed&&actual.outcome!=CommandOutcome::confirmed);
        CHECK(writes==1&&inputs==0&&!actual.related_status_valid);done();
    }
    // On requires fresh acceptable state; boolean control rejects restart magic.
    request.values[0]=1;
    for(const auto invalid_state:{InverterOperatingState::unknown,InverterOperatingState::faulted,InverterOperatingState::updating}) {
        state=stopped();state.state=invalid_state;reset();
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::state_not_allowed);done();
    }
    state=stopped();state.fresh=false;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::state_unavailable);done();
    state=stopped();
    for(const unsigned status:{1u,2u,5u,6u,7u}) {
        reset({h(0,{1}),w(0,{1}),h(0,{0}),r(status)});
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::completed);CHECK(writes==1);done();
    }
    for(const unsigned status:{0u,3u,4u,8u,65535u}) {
        reset({h(0,{0}),w(0,{1}),h(0,{1}),r(status)});
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::transition_pending);done();
    }
    request.values[0]=0xa5;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::blocked_unvalidated);done();
    request.values[0]=0;request.named_request=false;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::expected_values_required);done();
    request.expected_values_present=true;request.expected_values[0]=1;
    reset({h(0,{0})});
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::expected_value_mismatch);CHECK(writes==0);done();
    reset({h(0,{1}),h(0,{1}),w(0,{0}),h(0,{0}),r(0)});
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::completed);CHECK(writes==1);done();
}
static void catalog_settings(protocol::ModbusRtuClient &client) {
    using namespace scripted;
    RegisterCommandService registers(client);const auto state=stopped();
    const auto catalog=holding_register_catalog();
    for(std::size_t index=0;index<catalog.size;++index) {
        const auto &descriptor=catalog.data[index];
        if(descriptor.access!=RegisterAccess::read_write&&descriptor.access!=RegisterAccess::write_only)continue;
        auto request=named(descriptor.address);
        if(descriptor_starts_multiword_value(descriptor))request.register_count=2;
        if(descriptor_is_low_word(descriptor)){--request.first_register;request.register_count=2;}
        request.values.fill(1);request.confirmed=false;reset();
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::confirmation_required);done();
        request.confirmed=true;
        if(descriptor.write_guard==RegisterWriteGuard::blocked_unvalidated||descriptor.write_guard==RegisterWriteGuard::read_only) {
            ++denied_descriptors;reset();
            CHECK(registers.execute(request,state,true).outcome==(descriptor.write_guard==RegisterWriteGuard::read_only?
                RegisterCommandOutcome::read_only:RegisterCommandOutcome::blocked_unvalidated));done();continue;
        }
        ++admitted_descriptors;
        const auto first=request.first_register;const auto count=request.register_count;
        const std::vector<std::uint16_t> previous(count,0),desired(count,1);
        const auto write_kind=count==1?Operation::single:Operation::multiple;
        const bool write_only=descriptor.access==RegisterAccess::write_only;
        std::vector<Step> script;
        if(!write_only)script.push_back({Operation::holding,first,previous});
        script.push_back({write_kind,first,desired});
        if(!write_only)script.push_back({Operation::holding,first,desired});
        if(first==0)script.push_back(r(1));
        set(script);
        const auto result=registers.execute(request,state,true);
        CHECK(result.command_id==request.command_id&&result.first_register==first&&result.register_count==count);
        CHECK(result.outcome==(first==0?RegisterCommandOutcome::completed:write_only?
            RegisterCommandOutcome::sent_unverifiable:RegisterCommandOutcome::confirmed));
        CHECK(writes==1);done();
        if(descriptor.write_guard==RegisterWriteGuard::hot_edit_guarded) {
            request.guarded_interlock=false;reset();
            CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::guarded_interlock_required);done();request.guarded_interlock=true;
        }
        if(descriptor.write_guard==RegisterWriteGuard::commissioning_only) {
            request.commissioning_interlock=false;reset();
            CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::commissioning_interlock_required);done();request.commissioning_interlock=true;
        }
        if(descriptor.write_guard==RegisterWriteGuard::service_only) {
            request.service_interlock=false;reset();
            CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::service_interlock_required);done();request.service_interlock=true;
        }
        if(descriptor.write_guard==RegisterWriteGuard::standby_required||descriptor.write_guard==RegisterWriteGuard::commissioning_only||descriptor.write_guard==RegisterWriteGuard::service_only) {
            auto disallowed=state;disallowed.fresh=false;reset();
            CHECK(registers.execute(request,disallowed,true).outcome==RegisterCommandOutcome::state_unavailable);done();
            disallowed.fresh=true;disallowed.state=InverterOperatingState::operating;reset();
            CHECK(registers.execute(request,disallowed,true).outcome==RegisterCommandOutcome::state_not_allowed);done();
        }
        if(write_only||first==0)continue;
        // Nonstored runtime actions execute once even when their latch already
        // equals the request; persistent settings may skip an equal value.
        if(descriptor.write_guard==RegisterWriteGuard::runtime_command) {
            set({{Operation::holding,first,desired},{write_kind,first,desired},{Operation::holding,first,desired}});
            CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::confirmed);CHECK(writes==1);done();
        } else {
            set({{Operation::holding,first,desired}});
            CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::confirmed);CHECK(writes==0);done();
        }
        set({{Operation::holding,first,previous},{write_kind,first,desired},{Operation::holding,first,previous}});
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::readback_mismatch);CHECK(writes==1);done();
        request.named_request=false;reset();
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::expected_values_required);done();
        request.expected_values_present=true;request.expected_values.fill(1);
        set({{Operation::holding,first,previous}});
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::expected_value_mismatch);CHECK(writes==0);done();
        if(count==2) {
            auto partial=request;partial.named_request=true;partial.register_count=1;
            partial.first_register=first;reset();
            CHECK(registers.execute(partial,state,true).outcome==RegisterCommandOutcome::invalid_request);done();
            partial.first_register=first+1;reset();
            CHECK(registers.execute(partial,state,true).outcome==RegisterCommandOutcome::invalid_request);done();
        }
    }
    CHECK(admitted_descriptors==201);CHECK(denied_descriptors==3);
    auto request=named(207);request.values[0]=1;request.register_count=0;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::invalid_request);done();
    request.register_count=register_command_word_count+1;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::invalid_request);done();
    request.register_count=1;request.command_id=0;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::invalid_request);done();
    request.command_id=1;request.first_register=65535;request.register_count=2;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::invalid_request);done();
    request.first_register=207;request.register_count=1;request.space=RegisterSpace::input;reset();
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::read_only);done();
    // H147 has a sparse documented enum; 255 must not be treated as a range.
    request=named(147);
    request.values[0]=0;reset({h(147,{0}),w(147,{0}),h(147,{0})});
    CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::confirmed);CHECK(writes==1);done();
    for(const std::uint16_t value:{0,1,2,255}) {
        request.values[0]=value;reset({h(147,{65535}),w(147,{value}),h(147,{value})});
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::confirmed);done();
    }
    for(const std::uint16_t value:{3,254,256}) {
        request.values[0]=value;reset();
        CHECK(registers.execute(request,state,true).outcome==RegisterCommandOutcome::invalid_request);done();
    }
}
int main() {
    static_assert(sizeof(RegisterCommandOutcome)==1);
    static_assert(static_cast<unsigned>(RegisterCommandOutcome::sent_unverifiable)==15);
    static_assert(static_cast<unsigned>(RegisterCommandOutcome::completed)==16);
    static_assert(static_cast<unsigned>(RegisterCommandOutcome::transition_pending)==17);
    static_assert(static_cast<unsigned>(RegisterCommandOutcome::readback_mismatch)==18);
    CdcAcmDevice device;protocol::ModbusRtuClient client(device,1,1000,0);
    runtime_commands(client);convenience_settings(client);catalog_settings(client);
    CHECK(std::strcmp(register_command_outcome_name(RegisterCommandOutcome::completed),"completed")==0);
    CHECK(std::strcmp(register_command_outcome_name(RegisterCommandOutcome::transition_pending),"transition_pending")==0);
    CHECK(std::strcmp(register_command_outcome_name(RegisterCommandOutcome::readback_mismatch),"readback_mismatch")==0);
    std::printf("AOHAI settings: %u checks; all %u admitted descriptors and %u denied descriptors; H0/R0 transition, guards, readback and no-retry checks passed.\n",
        checks,admitted_descriptors,denied_descriptors);
}
