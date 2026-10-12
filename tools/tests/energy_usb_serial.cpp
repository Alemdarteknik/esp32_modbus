#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>

#include "freertos/stream_buffer.h"
#include "usb/vcp.hpp"
TickType_t host_ticks=0;
std::function<void()> host_after_send, host_after_receive, host_on_empty;
cdc_acm_host_device_config_t host_usb_config{};
std::function<esp_err_t(const std::uint8_t *,std::size_t)> host_tx;
unsigned host_tx_count=0;
#include "../../main/src/transport/usb_rs485.cpp"
#include "../../main/src/transport/usb_serial_protocol.cpp"

using namespace inverter_gateway::transport;
using Bytes=std::vector<std::uint8_t>;
FrameDecision framed(const std::uint8_t *data,std::size_t size,void *)
{
    if(data[0]!='(')return FrameDecision::invalid;
    return data[size-1]=='\r'?FrameDecision::complete:FrameDecision::incomplete;
}
int main()
{
    UsbSerialProtocol stream;
    assert(stream.install(2400)==ESP_OK);
    assert(stream.reconnect() && stream.connected());
    const auto first_session=stream.session_id();
    assert(stream.reconnect() && stream.session_id()!=first_session);
    const std::uint8_t command[]{'P','O','P','0','2','\r'};
    const Bytes valid{'(','A','C','K','\r'};
    std::uint8_t response[32]{};
    const ReceiveRule rule{framed,nullptr};
    bool sent=false;
    std::size_t size=sizeof(response);
    const auto inject=[](const Bytes &bytes){host_usb_config.data_cb(bytes.data(),bytes.size(),nullptr);};
    const auto exchange=[&](std::uint32_t timeout=37,std::size_t capacity=sizeof(response)) {
        size=capacity;
        return stream.exchange(command,sizeof(command),response,size,timeout,rule,sent);
    };
    const auto reset=[&] {
        host_after_send={};host_after_receive={};host_on_empty={};
        UsbRs485::clear_receive_buffer();host_ticks=0;
        host_tx=[&](const std::uint8_t *,std::size_t){inject(valid);return ESP_OK;};
    };
    // The owner validated an earlier handle; hotplug has already replaced it.
    // An otherwise connected adapter must reject before any TX submission.
    reset();const auto stale_before=host_tx_count;
    size=sizeof(response);sent=true;
    const ReceiveRule stale_rule{framed,nullptr,first_session};
    assert(stream.exchange(command,sizeof(command),response,size,37,stale_rule,sent)==ESP_ERR_INVALID_STATE);
    assert(!sent && size==0 && host_tx_count==stale_before);
    const ReceiveRule current_rule{framed,nullptr,stream.session_id()};
    size=sizeof(response);
    assert(stream.exchange(command,sizeof(command),response,size,37,current_rule,sent)==ESP_OK);
    assert(sent && size==valid.size() && host_tx_count==stale_before+1);
    reset();const auto tx_before=host_tx_count;
    assert(exchange()==ESP_OK && sent && size==valid.size());
    assert(host_tx_count==tx_before+1);
    reset();host_tx=[&](const std::uint8_t *,std::size_t){inject({'(','A'});host_ticks+=20;return ESP_OK;};
    assert(exchange()==ESP_ERR_TIMEOUT && sent && size==2 && host_ticks==37);
    reset();host_tx=[&](const std::uint8_t *,std::size_t){inject(valid);host_ticks+=37;return ESP_OK;};
    assert(exchange()==ESP_ERR_TIMEOUT && sent); // Full TX deadline never resets for RX.
    reset();const auto failure_before=host_tx_count;
    host_tx=[](const std::uint8_t *,std::size_t){return ESP_FAIL;};
    assert(exchange()==ESP_FAIL && sent && host_tx_count==failure_before+1); // No retry.
    reset();host_tx=[&](const std::uint8_t *,std::size_t){inject({0xa5,'\r'});return ESP_OK;};
    assert(exchange()==ESP_ERR_INVALID_RESPONSE && sent);
    reset();assert(exchange(37,2)==ESP_ERR_INVALID_SIZE && sent && size==2);

    reset();Bytes overflow=valid;overflow.resize(3000,0xa5);
    host_tx=[&](const std::uint8_t *,std::size_t){inject(overflow);return ESP_OK;};
    assert(exchange()==ESP_ERR_INVALID_RESPONSE && sent);
    reset();unsigned count=0;
    host_after_receive=[&]{if(++count==valid.size())inject(Bytes(3000,0xa5));};
    assert(exchange()==ESP_ERR_INVALID_RESPONSE && sent);

    // Receive callback active at admission prevents transmitting another write.
    reset();bool admitted=true;const auto busy_before=host_tx_count;
    host_after_send=[&]{admitted=exchange()==ESP_OK;assert(!sent);};
    inject({'a'});
    assert(!admitted && host_tx_count==busy_before);

    // A stale complete ACK is drained before transmitting a new request.
    reset();inject(valid);
    host_tx=[](const std::uint8_t *,std::size_t){return ESP_OK;};
    assert(exchange()==ESP_ERR_TIMEOUT && sent && size==0);

    // Disconnect at the final byte invalidates a valid response prefix.
    reset();count=0;
    host_after_receive=[&]{
        if(++count==valid.size()){
            cdc_acm_host_dev_event_data_t event{};event.type=CDC_ACM_HOST_DEVICE_DISCONNECTED;
            host_usb_config.event_cb(&event,nullptr);
        }
    };
    assert(exchange()==ESP_ERR_INVALID_STATE && sent);
    host_after_receive={};
    assert(!stream.connected());
    const auto old_session=stream.session_id();
    assert(stream.reconnect() && stream.session_id()!=old_session);
    reset();assert(exchange()==ESP_OK && sent);
    size=sizeof(response);sent=true;
    assert(stream.exchange(nullptr,1,response,size,37,rule,sent)==ESP_ERR_INVALID_ARG && !sent && size==0);
    std::puts("Actual USBserial adapter: overall deadlines, stale ACK drain, no retries, overflow and disconnect checks passed.");
}
