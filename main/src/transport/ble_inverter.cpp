#include "inverter_gateway/transport/ble_inverter.hpp"

#include <cstring>

#include "byte_transport_detail.hpp"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
extern "C" {
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
}
#endif

namespace inverter_gateway::transport {
#if CONFIG_BT_NIMBLE_ENABLED
namespace {
constexpr char tag[] = "inverter_ble";
enum TxPhase : unsigned { idle, queued, started, done, canceled };
struct BleState {
    detail::ReceivePipe receive_pipe;
    SemaphoreHandle_t transaction_mutex = nullptr;
    ble_addr_t peer{};
    ble_uuid16_t service_uuid{};
    ble_uuid16_t write_uuid{};
    ble_uuid16_t notify_uuid{};
    std::atomic<bool> installed{false};
    std::atomic<bool> ready{false};
    std::atomic<std::uint32_t> session{0};
    std::atomic<unsigned> tx_phase{idle};
    std::atomic<bool> tx_accepted{false};
    std::atomic<bool> disconnect_requested{false};
    std::atomic<std::uint32_t> tx_id{0};
    struct ble_npl_event tx_event{};
    struct ble_npl_callout timer{};
    std::uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
    std::uint16_t service_start = 0, service_end = 0;
    std::uint16_t write_handle = 0, notify_handle = 0, notify_end = 0, cccd = 0;
    std::uint8_t own_address_type = 0, write_properties = 0;
    bool connecting = false;
    bool closing = false;
    std::int64_t setup_started = 0;
    std::uint8_t tx_data[20]{};
    std::size_t tx_size = 0;
    std::int64_t tx_deadline = 0;
    std::uint32_t tx_session = 0;
    esp_err_t tx_result = ESP_OK;
};
BleState state;

int gap_event(struct ble_gap_event *event, void *);

void disconnect()
{
    state.ready.store(false, std::memory_order_release);
    state.receive_pipe.invalidate();
    state.closing = true;
    if (state.connection != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(state.connection, BLE_ERR_REM_USER_CONN_TERM);
}

bool same_session(std::uint16_t connection)
{
    return connection == state.connection && connection != BLE_HS_CONN_HANDLE_NONE && !state.closing;
}

int subscribed(std::uint16_t connection, const struct ble_gatt_error *error,
    struct ble_gatt_attr *, void *)
{
    if (!same_session(connection)) return 0;
    if (error->status != 0) { disconnect(); return 0; }
    state.session.fetch_add(1, std::memory_order_release);
    state.ready.store(true, std::memory_order_release);
    ESP_LOGI(tag, "Configured BLE BMS connected; notifications subscribed");
    return 0;
}

int descriptor_found(std::uint16_t connection, const struct ble_gatt_error *error,
    std::uint16_t characteristic, const struct ble_gatt_dsc *descriptor, void *)
{
    if (!same_session(connection)) return 0;
    if (error->status == 0 && descriptor != nullptr) {
        const ble_uuid16_t wanted{.u = {.type = BLE_UUID_TYPE_16}, .value = 0x2902};
        if (characteristic == state.notify_handle && ble_uuid_cmp(&wanted.u, &descriptor->uuid.u) == 0) {
            if (state.cccd != 0) { disconnect(); return 0; }
            state.cccd = descriptor->handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE || state.cccd == 0) { disconnect(); return 0; }
    const std::uint8_t subscribe[2]{1, 0};
    if (ble_gattc_write_flat(connection, state.cccd, subscribe, sizeof(subscribe), subscribed, nullptr) != 0)
        disconnect();
    return 0;
}

int characteristic_found(std::uint16_t connection, const struct ble_gatt_error *error,
    const struct ble_gatt_chr *characteristic, void *)
{
    if (!same_session(connection)) return 0;
    if (error->status == 0 && characteristic != nullptr) {
        if (state.notify_handle != 0 && characteristic->def_handle > state.notify_handle &&
            state.notify_end == state.service_end) state.notify_end = characteristic->def_handle - 1;
        if (ble_uuid_cmp(&state.write_uuid.u, &characteristic->uuid.u) == 0) {
            if (state.write_handle != 0 || (characteristic->properties &
                (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) == 0) {
                disconnect(); return 0;
            }
            state.write_handle = characteristic->val_handle;
            state.write_properties = characteristic->properties;
        }
        if (ble_uuid_cmp(&state.notify_uuid.u, &characteristic->uuid.u) == 0) {
            if (state.notify_handle != 0 || (characteristic->properties & BLE_GATT_CHR_PROP_NOTIFY) == 0) {
                disconnect(); return 0;
            }
            state.notify_handle = characteristic->val_handle;
            state.notify_end = state.service_end;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE || state.write_handle == 0 || state.notify_handle == 0 ||
        state.notify_end <= state.notify_handle) { disconnect(); return 0; }
    if (ble_gattc_disc_all_dscs(connection, state.notify_handle, state.notify_end,
        descriptor_found, nullptr) != 0) disconnect();
    return 0;
}

int service_found(std::uint16_t connection, const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service, void *)
{
    if (!same_session(connection)) return 0;
    if (error->status == 0 && service != nullptr) {
        if (state.service_start != 0) { disconnect(); return 0; }
        state.service_start = service->start_handle;
        state.service_end = service->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE || state.service_start == 0) { disconnect(); return 0; }
    if (ble_gattc_disc_all_chrs(connection, state.service_start, state.service_end,
        characteristic_found, nullptr) != 0) disconnect();
    return 0;
}

int gap_event(struct ble_gap_event *event, void *)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        state.connecting = false;
        if (event->connect.status != 0) return 0;
        state.connection = event->connect.conn_handle;
        state.closing = false;
        state.setup_started = esp_timer_get_time();
        state.service_start = state.service_end = 0;
        state.write_handle = state.notify_handle = state.notify_end = state.cccd = 0;
        state.write_properties = 0;
        if (ble_gattc_disc_svc_by_uuid(state.connection, &state.service_uuid.u, service_found, nullptr) != 0)
            disconnect();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        state.ready.store(false, std::memory_order_release);
        state.session.fetch_add(1, std::memory_order_release);
        state.receive_pipe.invalidate();
        state.connection = BLE_HS_CONN_HANDLE_NONE;
        state.closing = false;
        ESP_LOGW(tag, "BLE BMS disconnected reason=%d", event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (!same_session(event->notify_rx.conn_handle) || !state.ready.load() ||
            event->notify_rx.attr_handle != state.notify_handle) return 0;
        const auto total = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (total == 0 || total > 512) { state.receive_pipe.invalidate(); return 0; }
        // Announce whole-notification overflow before copying any prefix.
        if (total > xStreamBufferSpacesAvailable(state.receive_pipe.stream)) state.receive_pipe.invalidate();
        std::uint8_t bytes[128];
        for (std::size_t offset = 0; offset < total;) {
            const auto n = std::min<std::size_t>(sizeof(bytes), total - offset);
            if (os_mbuf_copydata(event->notify_rx.om, static_cast<int>(offset), static_cast<int>(n), bytes) != 0) {
                state.receive_pipe.invalidate(); return 0;
            }
            state.receive_pipe.append(bytes, n);
            offset += n;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int written(std::uint16_t connection, const struct ble_gatt_error *error,
    struct ble_gatt_attr *, void *arg)
{
    if (static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(arg)) != state.tx_id.load()) return 0;
    state.tx_result = error->status == 0 && same_session(connection) &&
        state.tx_session == state.session.load() ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    state.tx_phase.store(done, std::memory_order_release);
    return 0;
}

void output_event(struct ble_npl_event *)
{
    auto phase = state.tx_phase.load(std::memory_order_acquire);
    if (phase == canceled) { state.tx_phase.store(idle, std::memory_order_release); return; }
    if (phase != queued) return;
    const bool current_session = state.ready.load() && state.tx_session == state.session.load();
    if (!current_session || esp_timer_get_time() >= state.tx_deadline) {
        state.tx_result = current_session ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
        unsigned expected = queued;
        state.tx_phase.compare_exchange_strong(expected, done, std::memory_order_release);
        return;
    }
    unsigned expected = queued;
    if (!state.tx_phase.compare_exchange_strong(expected, started, std::memory_order_acq_rel)) return;
    state.tx_accepted.store(true, std::memory_order_release);
    int rc;
    if ((state.write_properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0) {
        rc = ble_gattc_write_no_rsp_flat(state.connection, state.write_handle, state.tx_data, state.tx_size);
        state.tx_result = rc == 0 ? ESP_OK : ESP_FAIL;
        state.tx_phase.store(done, std::memory_order_release);
    } else {
        rc = ble_gattc_write_flat(state.connection, state.write_handle, state.tx_data, state.tx_size, written,
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(state.tx_id.load())));
        if (rc != 0) {
            state.tx_result = ESP_FAIL;
            state.tx_phase.store(done, std::memory_order_release);
        }
    }
}

void connection_timer(struct ble_npl_event *)
{
    if (state.disconnect_requested.exchange(false)) disconnect();
    if (state.connecting && esp_timer_get_time() - state.setup_started > 12000000) {
        ble_gap_conn_cancel();
        state.connecting = false;
    } else if (state.connection != BLE_HS_CONN_HANDLE_NONE && !state.ready.load() &&
        esp_timer_get_time() - state.setup_started > 12000000) disconnect();
    else if (state.connection == BLE_HS_CONN_HANDLE_NONE && !state.connecting) {
        state.setup_started = esp_timer_get_time();
        const auto rc = ble_gap_connect(state.own_address_type, &state.peer, 10000, nullptr, gap_event, nullptr);
        if (rc == 0) state.connecting = true;
    }
    ble_npl_callout_reset(&state.timer, ble_npl_time_ms_to_ticks32(2000));
}

void synced()
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &state.own_address_type) != 0) {
        ESP_LOGE(tag, "Cannot obtain local BLE address"); return;
    }
    connection_timer(nullptr);
}

void reset(int reason)
{
    state.ready.store(false, std::memory_order_release);
    state.session.fetch_add(1, std::memory_order_release);
    state.receive_pipe.invalidate();
    state.connection = BLE_HS_CONN_HANDLE_NONE;
    state.connecting = false;
    state.closing = false;
    ble_npl_callout_stop(&state.timer);
    ESP_LOGW(tag, "BLE host reset reason=%d", reason);
}

void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool parse_address(const char *text, ble_addr_t &address)
{
    if (text == nullptr || std::strlen(text) != 17) return false;
    const auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 6; ++i) {
        const int a = hex(text[i * 3]), b = hex(text[i * 3 + 1]);
        if (a < 0 || b < 0 || (i < 5 && text[i * 3 + 2] != ':')) return false;
        address.val[5 - i] = (a << 4) | b;
    }
    bool nonzero = false, not_broadcast = false;
    for (const auto byte : address.val) { nonzero |= byte != 0; not_broadcast |= byte != 0xff; }
    return nonzero && not_broadcast;
}
} // namespace

esp_err_t BleInverter::install(const BleInverterConfig &config)
{
    if (state.installed.load()) return ESP_ERR_INVALID_STATE;
    if (config.mac_address[17] != '\0' || !parse_address(config.mac_address, state.peer) ||
        config.address_type > 1 || config.service_uuid == 0 || config.write_uuid == 0 || config.notify_uuid == 0)
        return ESP_ERR_INVALID_ARG;
    state.peer.type = config.address_type;
    state.service_uuid.u.type = state.write_uuid.u.type = state.notify_uuid.u.type = BLE_UUID_TYPE_16;
    state.service_uuid.value = config.service_uuid;
    state.write_uuid.value = config.write_uuid;
    state.notify_uuid.value = config.notify_uuid;
    state.receive_pipe.stream = xStreamBufferCreate(2048, 1);
    state.transaction_mutex = xSemaphoreCreateMutex();
    if (state.receive_pipe.stream == nullptr || state.transaction_mutex == nullptr) {
        if (state.receive_pipe.stream != nullptr) vStreamBufferDelete(state.receive_pipe.stream);
        if (state.transaction_mutex != nullptr) vSemaphoreDelete(state.transaction_mutex);
        state.receive_pipe.stream = nullptr;
        state.transaction_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    const auto err = nimble_port_init();
    if (err != ESP_OK) {
        vStreamBufferDelete(state.receive_pipe.stream);
        vSemaphoreDelete(state.transaction_mutex);
        state.receive_pipe.stream = nullptr;
        state.transaction_mutex = nullptr;
        return err;
    }
    ble_hs_cfg.sync_cb = synced;
    ble_hs_cfg.reset_cb = reset;
    ble_npl_event_init(&state.tx_event, output_event, nullptr);
    ble_npl_callout_init(&state.timer, nimble_port_get_dflt_eventq(), connection_timer, nullptr);
    state.installed.store(true, std::memory_order_release);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool BleInverter::connected() const
{
    return state.installed.load(std::memory_order_acquire) && state.ready.load(std::memory_order_acquire) &&
        !state.disconnect_requested.load(std::memory_order_acquire);
}

std::uint32_t BleInverter::session_id() const { return state.session.load(std::memory_order_acquire); }

esp_err_t BleInverter::exchange(const std::uint8_t *command, std::size_t command_size,
    std::uint8_t *response, std::size_t &response_size, std::uint32_t timeout_ms,
    ReceiveRule rule, bool &transmitted)
{
    const auto capacity = response_size;
    response_size = 0;
    transmitted = false;
    if (!detail::valid_exchange(command, command_size, response, capacity, timeout_ms, rule) ||
        command_size == 0 || command_size > sizeof(state.tx_data)) return ESP_ERR_INVALID_ARG;
    if (!state.installed.load()) return ESP_ERR_INVALID_STATE;
    const detail::Deadline deadline(timeout_ms);
    if (!detail::take_until(state.transaction_mutex, deadline)) return ESP_ERR_TIMEOUT;
    while (!connected() && !deadline.expired()) vTaskDelay(1);
    esp_err_t err = connected() ? ESP_OK : ESP_ERR_TIMEOUT;
    const auto session = state.session.load(std::memory_order_acquire);
    if (err == ESP_OK && rule.expected_session != 0 && rule.expected_session != session)
        err = ESP_ERR_INVALID_STATE;
    std::uint32_t snapshot = 0;
    if (err == ESP_OK && !state.receive_pipe.prepare(snapshot)) err = ESP_ERR_INVALID_RESPONSE;
    auto phase = state.tx_phase.load(std::memory_order_acquire);
    if (phase == done) { state.tx_phase.store(idle); phase = idle; }
    if (err == ESP_OK && phase != idle) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && deadline.expired()) err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        std::memcpy(state.tx_data, command, command_size);
        state.tx_size = command_size;
        state.tx_session = session;
        state.tx_id.fetch_add(1, std::memory_order_release);
        state.tx_deadline = deadline.end();
        state.tx_accepted.store(false);
        state.tx_phase.store(queued, std::memory_order_release);
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &state.tx_event);
        while (!deadline.expired()) {
            phase = state.tx_phase.load(std::memory_order_acquire);
            if (phase == done) break;
            vTaskDelay(1);
        }
        phase = state.tx_phase.load(std::memory_order_acquire);
        transmitted = state.tx_accepted.load(std::memory_order_acquire) || phase == started;
        if (phase == done) {
            err = state.tx_result;
            state.tx_phase.store(idle, std::memory_order_release);
            if (deadline.expired()) err = ESP_ERR_TIMEOUT;
        } else {
            unsigned expected = queued;
            if (!state.tx_phase.compare_exchange_strong(expected, canceled, std::memory_order_acq_rel) &&
                expected == started) {
                transmitted = true;
                state.disconnect_requested.store(true, std::memory_order_release);
            }
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (err == ESP_OK) err = detail::receive(state.receive_pipe, snapshot, response, capacity,
        response_size, deadline, rule, [this, session] { return connected() && session == state.session.load(); });
    xSemaphoreGive(state.transaction_mutex);
    return err;
}
#else
esp_err_t BleInverter::install(const BleInverterConfig &) { return ESP_ERR_NOT_SUPPORTED; }
bool BleInverter::connected() const { return false; }
std::uint32_t BleInverter::session_id() const { return 0; }
esp_err_t BleInverter::exchange(const std::uint8_t *, std::size_t,
    std::uint8_t *, std::size_t &size, std::uint32_t, ReceiveRule, bool &transmitted)
{
    size = 0;
    transmitted = false;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
} // namespace inverter_gateway::transport
