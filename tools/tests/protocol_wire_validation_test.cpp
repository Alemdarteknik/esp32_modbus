// Compile the actual receiving validators and route-failure implementation.
// Hardware functions below abort if called. Tests exercise only receiving
// validators and pure route-failure attribution, without radio or UART access.
#ifdef TEST_COORDINATOR_WIRE
#include "../../main/src/network/coordinator_link.cpp"
#else
#include "../../main/src/network/espnow_mesh.cpp"
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>

[[noreturn]] static void unexpected_hardware_call() { std::abort(); }
QueueHandle_t xQueueCreate(std::size_t,std::size_t) { unexpected_hardware_call(); }
void vQueueDelete(QueueHandle_t) { unexpected_hardware_call(); }
BaseType_t xQueueReceive(QueueHandle_t,void *,TickType_t) { unexpected_hardware_call(); }
BaseType_t xQueueSend(QueueHandle_t,const void *,TickType_t) { unexpected_hardware_call(); }
BaseType_t xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *) { unexpected_hardware_call(); }
void vTaskDelete(TaskHandle_t) { unexpected_hardware_call(); }
TickType_t xTaskGetTickCount() { unexpected_hardware_call(); }
void vTaskDelay(TickType_t) { unexpected_hardware_call(); }
namespace inverter_gateway::app {
bool valid_site_id(const char *) { unexpected_hardware_call(); }
esp_err_t ConfigStore::save(const SystemConfig &) const { unexpected_hardware_call(); }
}
#ifdef TEST_COORDINATOR_WIRE
namespace inverter_gateway::protocol {
std::uint16_t calculate_modbus_crc(const std::uint8_t *,std::size_t) { unexpected_hardware_call(); }
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *) { unexpected_hardware_call(); }
SemaphoreHandle_t xSemaphoreCreateMutex() { unexpected_hardware_call(); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t,TickType_t) { unexpected_hardware_call(); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { unexpected_hardware_call(); }
void vSemaphoreDelete(SemaphoreHandle_t) { unexpected_hardware_call(); }
std::int64_t esp_timer_get_time() { unexpected_hardware_call(); }
const char *esp_err_to_name(esp_err_t) { unexpected_hardware_call(); }
void esp_restart() { unexpected_hardware_call(); }
esp_err_t uart_param_config(uart_port_t,const uart_config_t *) { unexpected_hardware_call(); }
esp_err_t uart_set_pin(uart_port_t,int,int,int,int) { unexpected_hardware_call(); }
esp_err_t uart_driver_install(uart_port_t,int,int,int,void *,int) { unexpected_hardware_call(); }
esp_err_t uart_driver_delete(uart_port_t) { unexpected_hardware_call(); }
int uart_tx_chars(uart_port_t,const char *,std::uint32_t) { unexpected_hardware_call(); }
esp_err_t uart_wait_tx_done(uart_port_t,TickType_t) { unexpected_hardware_call(); }
int uart_read_bytes(uart_port_t,void *,std::uint32_t,TickType_t) { unexpected_hardware_call(); }
#else
bool esp_now_is_peer_exist(const std::uint8_t *) { unexpected_hardware_call(); }
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *) { unexpected_hardware_call(); }
esp_err_t esp_now_init() { unexpected_hardware_call(); }
esp_err_t esp_now_deinit() { unexpected_hardware_call(); }
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t) { unexpected_hardware_call(); }
esp_err_t esp_now_unregister_recv_cb() { unexpected_hardware_call(); }
esp_err_t esp_now_send(const std::uint8_t *,const std::uint8_t *,std::size_t) { unexpected_hardware_call(); }
#endif

using namespace inverter_gateway::network;
using inverter_gateway::protocol::EnergyProtocol;
int main() {
    ProtocolTelemetryMessage t{};
    t.protocol=EnergyProtocol::pi30;t.chunk_count=1;t.final_chunk=true;
    std::strcpy(t.command,"QPIGS");t.field_count=1;
    std::strcpy(t.fields[0].key,"Battery Voltage");std::strcpy(t.fields[0].unit,"V");t.fields[0].value=53.1;
    assert(valid_protocol_telemetry(t));
    t.chunk_index=1;assert(!valid_protocol_telemetry(t));t.chunk_index=0;
    t.chunk_count=49;assert(!valid_protocol_telemetry(t));t.chunk_count=1;
    t.final_chunk=false;assert(!valid_protocol_telemetry(t));t.final_chunk=true;
    t.field_count=3;assert(!valid_protocol_telemetry(t));t.field_count=1;
    t.fields[0].value=std::numeric_limits<double>::quiet_NaN();assert(!valid_protocol_telemetry(t));t.fields[0].value=53;
    std::memset(t.fields[0].key,'x',sizeof(t.fields[0].key));assert(!valid_protocol_telemetry(t));std::strcpy(t.fields[0].key,"V");
    *reinterpret_cast<std::uint8_t *>(&t.fields[0].numeric)=2;assert(!valid_protocol_telemetry(t));t.fields[0].numeric=true;
    t.protocol=EnergyProtocol::count;assert(!valid_protocol_telemetry(t));t.protocol=EnergyProtocol::pi30;

    ProtocolCommand c{};c.command_id=123;c.target_member_id=4;c.origin=RemoteCommandOrigin::mqtt;
    c.protocol=EnergyProtocol::pi30;std::strcpy(c.command,"POP02");c.write=true;c.confirmed=true;
    assert(valid_protocol_command(c));
    c.origin=RemoteCommandOrigin::none;assert(!valid_protocol_command(c));c.origin=RemoteCommandOrigin::mqtt;
    std::memset(c.command,'x',sizeof(c.command));assert(!valid_protocol_command(c));std::strcpy(c.command,"POP02");
    *reinterpret_cast<std::uint8_t *>(&c.write)=2;assert(!valid_protocol_command(c));c.write=true;
    ProtocolCommandResult result{};result.protocol=c.protocol;std::strcpy(result.command,c.command);
    assert(valid_protocol_result(result));
    result.outcome=static_cast<ProtocolCommandOutcome>(255);assert(!valid_protocol_result(result));
    result.outcome=ProtocolCommandOutcome::uncertain;
    std::memset(result.detail,'x',sizeof(result.detail));assert(!valid_protocol_result(result));result.detail[0]=0;
    *reinterpret_cast<std::uint8_t *>(&result.write_sent_known)=2;assert(!valid_protocol_result(result));

#ifdef TEST_COORDINATOR_WIRE
    const auto failed=CoordinatorLink::protocol_command_route_failure(c,false);
    assert(failed.command_id==123&&failed.source_member_id==4&&failed.protocol==c.protocol);
    assert(failed.outcome==ProtocolCommandOutcome::not_sent&&!failed.write_sent&&failed.write_sent_known);
    const auto uncertain=CoordinatorLink::protocol_command_route_failure(c,true);
    assert(uncertain.outcome==ProtocolCommandOutcome::uncertain&&!uncertain.write_sent_known);
    assert(!std::strcmp(uncertain.command,"POP02"));
    assert(command_frame_type(type_protocol_command)&&command_frame_type(type_command));
    assert(!command_frame_type(type_protocol_telemetry));
    std::puts("Coordinator protocol wire validation and failure attribution passed.");
#else
    static_assert(WireType::protocol_telemetry==5&&WireType::protocol_command==6&&WireType::protocol_command_result==7);
    static_assert(wire_version==2);
    std::puts("ESP-NOW protocol wire validation and packet-size assertions passed.");
#endif
}
