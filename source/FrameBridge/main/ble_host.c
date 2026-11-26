#include "ble_host.h"
#include "ble_hid.h"
#include "bridge.h"
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#if CONFIG_BT_NIMBLE_MAX_CONNECTIONS < 3 || !CONFIG_BT_NIMBLE_NVS_PERSIST
#error "FrameBridge needs CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= 3 and CONFIG_BT_NIMBLE_NVS_PERSIST=y. Update the existing sdkconfig in idf.py menuconfig or regenerate it from sdkconfig.defaults."
#endif

// ESP-IDF's NimBLE store example exports this initializer without a public prototype.
void ble_store_config_init(void);

enum { GAMEPAD_APPEARANCE = 0x03c4, PAIRING_SECONDS = 120, SECURITY_SECONDS = 30 };
static const char name[] = "FrameBridge Gamepad";
static const char *TAG = "ble-gamepad";
static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
static atomic_uint requested_mode;
static atomic_bool mode_event_ready;
static struct ble_npl_event mode_event;
static atomic_int state = BLE_HOST_OFF;

// All of the following are owned by the NimBLE host task.
static ble_addr_t preferred;
static bool bluetooth_mode, have_preferred, open_advertisement;
static int64_t pairing_until, security_until;
static uint16_t host_conn = BLE_HS_CONN_HANDLE_NONE;
static bool secure, subscribed, suspended, closing;

ble_host_state_t ble_host_state(void) { return atomic_load(&state); }

void ble_host_toggle_mode(void)
{
    atomic_fetch_xor(&requested_mode, 1u);
    if (atomic_load(&mode_event_ready))
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &mode_event);
}

bool ble_host_authorized(uint16_t conn)
{
    return bluetooth_mode && conn != BLE_HS_CONN_HANDLE_NONE && conn == host_conn && secure && !closing;
}

static void update_ready(void)
{
    bool ready = ble_host_authorized(host_conn) && subscribed && !suspended;
    if (ready != (ble_host_state() == BLE_HOST_READY))
        bridge_ble_online(host_conn, ready);
    atomic_store(&state, !bluetooth_mode ? BLE_HOST_OFF : ready ? BLE_HOST_READY : BLE_HOST_CONNECTED);
}

void ble_host_suspend(uint16_t conn, bool sleep)
{
    if (ble_host_authorized(conn)) {
        suspended = sleep;
        update_ready();
    }
}

static void drop_host(void)
{
    closing = true;
    update_ready();
    int rc = ble_gap_terminate(host_conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOTCONN)
        ESP_LOGW(TAG, "Host disconnect request rc=%d", rc);
}

static void apply_mode(struct ble_npl_event *unused)
{
    bool enabled = atomic_load(&requested_mode) != 0;
    if (enabled == bluetooth_mode) return;
    // Let the old output release held controls while its link is still authorized.
    bridge_set_bluetooth_mode(enabled);
    bluetooth_mode = enabled;
    pairing_until = enabled ? esp_timer_get_time() + INT64_C(1000000) * PAIRING_SECONDS : 0;
    if (!enabled) {
        if (host_conn != BLE_HS_CONN_HANDLE_NONE) drop_host();
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        atomic_store(&state, BLE_HOST_OFF);
    }
}

static bool stored_bond(const ble_addr_t *peer)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count;
    if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS)) return false;
    for (int i = 0; i < count; ++i)
        if (!ble_addr_cmp(peer, &peers[i])) return true;
    return false;
}

static esp_err_t remember_host(const ble_addr_t *peer)
{
    if (have_preferred && !ble_addr_cmp(peer, &preferred)) return ESP_OK;
    nvs_handle_t nvs;
    esp_err_t rc = nvs_open("framebridge", NVS_READWRITE, &nvs);
    if (rc != ESP_OK) return rc;
    rc = nvs_set_blob(nvs, "ble_host", peer, sizeof(*peer));
    if (rc == ESP_OK) rc = nvs_commit(nvs);
    nvs_close(nvs);
    if (rc == ESP_OK) {
        preferred = *peer;
        have_preferred = true;
    }
    return rc;
}

static bool in_pairing_window(void) { return esp_timer_get_time() < pairing_until; }

static void disconnected(void)
{
    if (ble_host_state() == BLE_HOST_READY) bridge_ble_online(host_conn, false);
    host_conn = BLE_HS_CONN_HANDLE_NONE;
    secure = subscribed = suspended = closing = false;
    atomic_store(&state, BLE_HOST_OFF);
}

static int host_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status) break;
        uint16_t conn = event->connect.conn_handle;
        if (!bluetooth_mode || host_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(conn, &desc)) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        host_conn = conn;
        update_ready();
        if (!in_pairing_window() && (!have_preferred || ble_addr_cmp(&preferred, &desc.peer_id_addr) ||
                                     !stored_bond(&preferred))) {
            drop_host();
            break;
        }
        security_until = esp_timer_get_time() + INT64_C(1000000) * SECURITY_SECONDS;
        if (!desc.sec_state.encrypted) {
            int rc = ble_gap_security_initiate(conn);
            if (rc && rc != BLE_HS_EALREADY && rc != BLE_HS_EBUSY) drop_host();
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle != host_conn || closing) break;
        // NimBLE persists the distributed keys and resolves the identity before ENC_CHANGE.
        if (ble_gap_conn_find(host_conn, &desc) ||
            !desc.sec_state.encrypted || !desc.sec_state.bonded || !stored_bond(&desc.peer_id_addr) ||
            (!in_pairing_window() && (!have_preferred || ble_addr_cmp(&preferred, &desc.peer_id_addr)))) {
            drop_host();
            break;
        }
        // A bonded central may finish encryption before our security request times out.
        // Accept that race only for the already remembered, verified encrypted peer.
        int rc = event->enc_change.status;
        if (rc && !(rc == BLE_HS_ETIMEOUT && have_preferred && !ble_addr_cmp(&preferred, &desc.peer_id_addr))) {
            drop_host();
            break;
        }
        if (remember_host(&desc.peer_id_addr) != ESP_OK) {
            ESP_LOGW(TAG, "Could not save paired gamepad host");
            drop_host();
            break;
        }
        secure = true;
        pairing_until = 0;
        update_ready();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == host_conn &&
            event->subscribe.attr_handle == ble_hid_input_handle()) {
            subscribed = event->subscribe.cur_notify;
            update_ready();
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (event->repeat_pairing.conn_handle != host_conn || closing || !in_pairing_window() ||
            ble_gap_conn_find(host_conn, &desc)) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        return ble_store_util_delete_peer(&desc.peer_id_addr) == 0 ?
               BLE_GAP_REPEAT_PAIRING_RETRY : BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == host_conn) disconnected();
        break;
    default:
        break;
    }
    return 0;
}

static int advertise(uint8_t own_addr_type, bool open, int32_t duration_ms)
{
    if (!open) {
        int rc = ble_gap_wl_set(&preferred, 1);
        if (rc) return rc;
    }
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_BREDR_UNSUP | (open ? BLE_HS_ADV_F_DISC_GEN : 0),
        .uuids16 = &hid_uuid, .num_uuids16 = 1, .uuids16_is_complete = 1,
        .appearance = GAMEPAD_APPEARANCE, .appearance_is_present = 1,
    };
    struct ble_hs_adv_fields response = {
        .name = (const uint8_t *)name, .name_len = sizeof(name)-1, .name_is_complete = 1,
    };
    int rc = ble_gap_adv_set_fields(&fields);
    if (!rc) rc = ble_gap_adv_rsp_set_fields(&response);
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = open ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON,
        .filter_policy = open ? BLE_HCI_ADV_FILT_NONE : BLE_HCI_ADV_FILT_CONN,
        .itvl_min = open ? 32 : 160, .itvl_max = open ? 40 : 192
    };
    if (!rc) rc = ble_gap_adv_start(own_addr_type, NULL, duration_ms, &params, host_event, NULL);
    return rc;
}

void ble_host_tick(uint8_t own_addr_type)
{
    apply_mode(NULL);
    int64_t now = esp_timer_get_time();
    if (host_conn != BLE_HS_CONN_HANDLE_NONE) {
        if (closing || (!secure && now >= security_until)) drop_host();
        else if (ble_host_state() == BLE_HOST_READY) bridge_ble_flush();
        return;
    }
    if (!bluetooth_mode) {
        // A stop can race a controller operation; retry until advertising is idle.
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        return;
    }
    bool open = now < pairing_until;
    if (ble_gap_adv_active()) {
        if (open != open_advertisement || (!open && !have_preferred)) {
            // Restart with the new filter on the next host tick, after cancellation settles.
            ble_gap_adv_stop();
            atomic_store(&state, BLE_HOST_OFF);
        }
        return;
    }
    if (!open && !have_preferred) {
        atomic_store(&state, BLE_HOST_OFF);
        return;
    }
    int32_t duration = open ? (int32_t)((pairing_until - now + 999) / 1000) : BLE_HS_FOREVER;
    int rc = advertise(own_addr_type, open, duration);
    if (rc) {
        atomic_store(&state, BLE_HOST_OFF);
        ESP_LOGW(TAG, "Advertising deferred rc=%d", rc);
    } else {
        open_advertisement = open;
        atomic_store(&state, open ? BLE_HOST_PAIRING : BLE_HOST_RECONNECTING);
    }
}

void ble_host_reset(void)
{
    disconnected();
}

void ble_host_init(void)
{
    ble_npl_event_init(&mode_event, apply_mode, NULL);
    atomic_store(&mode_event_ready, true);
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(name));
    ESP_ERROR_CHECK(ble_svc_gap_device_appearance_set(GAMEPAD_APPEARANCE));
    ESP_ERROR_CHECK(ble_hid_register());

    nvs_handle_t nvs;
    if (nvs_open("framebridge", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(preferred);
        have_preferred = nvs_get_blob(nvs, "ble_host", &preferred, &size) == ESP_OK &&
                         size == sizeof(preferred) && preferred.type <= BLE_ADDR_RANDOM &&
                         stored_bond(&preferred);
        nvs_close(nvs);
    }
}
