#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"

#ifdef FRAME_BRIDGE
#include "bridge.h"
#include "ble_host.h"
#include "pairing.h"
#endif

#define PEER_COUNT 2
#define RETRY_US INT64_C(5000000)
static const char *TAG = "frame";

// NimBLE takes each 128-bit UUID least significant octet first.
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0xd4, 0x86, 0x48, 0x24, 0x54, 0xb3, 0x43, 0xa1,
    0xbc, 0x20, 0x97, 0x8f, 0xc3, 0x76, 0xc2, 0x75);
static const ble_uuid128_t channel_uuid = BLE_UUID128_INIT(
    0xed, 0xaa, 0x20, 0x11, 0x92, 0xe7, 0x43, 0x5a,
    0xaa, 0xe9, 0x94, 0x43, 0x35, 0x6a, 0xd4, 0xd3);
static const ble_uuid16_t cccd_uuid = BLE_UUID16_INIT(0x2902);

typedef struct {
    uint16_t conn, service_start, service_end, value, char_end, cccd;
    uint8_t properties;
    bool closing;
    uint32_t packets;
    int64_t next_sample, quiet_notice;
} link_t;

typedef struct {
    const char *side;
    const char *address;
    int64_t retry_after;
    link_t link;
#ifdef FRAME_BRIDGE
    bool default_timing;
#endif
} controller_t;

static controller_t controllers[PEER_COUNT] = {
    {.side = "left"},
    {.side = "right"},
};
static uint8_t own_addr_type;
static struct ble_npl_callout timer;
static bool host_ready;
static int gap_event(struct ble_gap_event *event, void *arg);

static bool current_link(controller_t *peer, uint16_t conn)
{
    return peer->link.conn == conn && !peer->link.closing;
}

static void close_link(controller_t *peer)
{
    peer->link.closing = true;
    int rc = ble_gap_terminate(peer->link.conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "%s disconnect request rc=%d", peer->side, rc);
    }
}

static void check_step(controller_t *peer, const char *step, int rc)
{
    if (rc) {
        ESP_LOGW(TAG, "%s %s failed rc=%d", peer->side, step, rc);
        close_link(peer);
    }
}

#ifndef FRAME_BRIDGE
static void log_payload(controller_t *peer, const char *kind, struct os_mbuf *om)
{
    uint8_t bytes[64];
    char hex[2 * sizeof(bytes) + 1];
    static const char digits[] = "0123456789abcdef";
    unsigned length = OS_MBUF_PKTLEN(om);
    unsigned shown = length < sizeof(bytes) ? length : sizeof(bytes);
    if (shown && os_mbuf_copydata(om, 0, shown, bytes)) {
        ESP_LOGW(TAG, "%s %s could not copy payload", peer->side, kind);
        return;
    }
    for (unsigned i = 0; i < shown; ++i) {
        hex[2 * i] = digits[bytes[i] >> 4];
        hex[2 * i + 1] = digits[bytes[i] & 15];
    }
    hex[2 * shown] = '\0';
    ESP_LOGI(TAG, "%s %s notifications=%" PRIu32 " len=%u hex=%s%s",
             peer->side, kind, peer->link.packets, length, hex, shown < length ? "..." : "");
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg)
{
    controller_t *peer = arg;
    if (!current_link(peer, conn)) return 0;
    if (!err->status) {
        log_payload(peer, "READ", attr->om);
    } else {
        ESP_LOGW(TAG, "%s initial read rc=%d", peer->side, err->status);
    }
    return 0;
}

#endif

static int on_subscribe(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    controller_t *peer = arg;
    if (!current_link(peer, conn)) return 0;
    if (err->status) {
        check_step(peer, "enable notifications", err->status);
        return 0;
    }
    peer->link.quiet_notice = esp_timer_get_time() + 10000000;
    ESP_LOGI(TAG, "%s SUBSCRIBED value=0x%04x cccd=0x%04x",
             peer->side, peer->link.value, peer->link.cccd);
#ifdef FRAME_BRIDGE
    bridge_connection((unsigned)(peer - controllers), true);
#else
    if (peer->link.properties & BLE_GATT_CHR_PROP_READ) {
        int rc = ble_gattc_read(conn, peer->link.value, on_read, peer);
        if (rc) ESP_LOGW(TAG, "%s initial read request rc=%d", peer->side, rc);
    }
#endif
    return 0;
}

static int on_descriptor(uint16_t conn, const struct ble_gatt_error *err,
                         uint16_t value, const struct ble_gatt_dsc *dsc, void *arg)
{
    controller_t *peer = arg;
    if (!current_link(peer, conn)) return 0;
    int rc = err->status;
    if (!rc) {
        if (value == peer->link.value && !ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u))
            peer->link.cccd = dsc->handle;
        return 0;
    }
    if (rc == BLE_HS_EDONE) {
        if (!peer->link.cccd) {
            ESP_LOGW(TAG, "%s descriptor 0x2902 missing", peer->side);
            close_link(peer);
            return 0;
        }
        static const uint8_t enable[] = {1, 0};
        rc = ble_gattc_write_flat(conn, peer->link.cccd, enable, sizeof(enable),
                                  on_subscribe, peer);
    }
    check_step(peer, "descriptor discovery or subscription request", rc);
    return 0;
}

static int on_characteristic(uint16_t conn, const struct ble_gatt_error *err,
                             const struct ble_gatt_chr *chr, void *arg)
{
    controller_t *peer = arg;
    if (!current_link(peer, conn)) return 0;
    link_t *link = &peer->link;
    int rc = err->status;
    if (!rc) {
        if (!ble_uuid_cmp(&chr->uuid.u, &channel_uuid.u)) {
            link->value = chr->val_handle;
            link->properties = chr->properties;
            link->char_end = link->service_end;
        } else if (link->value && chr->def_handle > link->value && chr->def_handle <= link->char_end) {
            // The next characteristic declaration bounds the descriptor range.
            link->char_end = chr->def_handle - 1;
        }
        return 0;
    }
    if (rc == BLE_HS_EDONE) {
        if (!(link->properties & BLE_GATT_CHR_PROP_NOTIFY) || link->char_end <= link->value) {
            ESP_LOGW(TAG, "%s expected notify characteristic or descriptor range missing", peer->side);
            close_link(peer);
            return 0;
        }
        rc = ble_gattc_disc_all_dscs(conn, link->value, link->char_end, on_descriptor, peer);
    }
    check_step(peer, "characteristic or descriptor discovery", rc);
    return 0;
}

static int on_service(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *service, void *arg)
{
    controller_t *peer = arg;
    if (!current_link(peer, conn)) return 0;
    int rc = err->status;
    if (!rc) {
        peer->link.service_start = service->start_handle;
        peer->link.service_end = service->end_handle;
        return 0;
    }
    if (rc == BLE_HS_EDONE) {
        if (!peer->link.service_start) {
            ESP_LOGW(TAG, "%s expected Steam Frame service missing", peer->side);
            close_link(peer);
            return 0;
        }
        rc = ble_gattc_disc_all_chrs(conn, peer->link.service_start, peer->link.service_end,
                                     on_characteristic, peer);
    }
    check_step(peer, "service or characteristic discovery", rc);
    return 0;
}

static void discover_service(controller_t *peer)
{
    int rc = ble_gattc_disc_svc_by_uuid(peer->link.conn, &service_uuid.u, on_service, peer);
    check_step(peer, "service discovery", rc);
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    controller_t *peer = arg;
    if (!current_link(peer, conn)) return 0;
    ESP_LOGI(TAG, "%s MTU exchange rc=%d mtu=%u", peer->side, err->status, mtu);
    discover_service(peer);
    return 0;
}

static void on_advertisement(const struct ble_gap_disc_desc *adv)
{
    if (adv->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
        adv->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) return;

    char address[18];
    const uint8_t *b = adv->addr.val;
    snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[5], b[4], b[3], b[2], b[1], b[0]);
    for (size_t i = 0; i < PEER_COUNT; ++i) {
        controller_t *peer = &controllers[i];
        if (!peer->address || peer->link.conn != BLE_HS_CONN_HANDLE_NONE || strcasecmp(address, peer->address) ||
            esp_timer_get_time() < peer->retry_after) continue;

        ble_addr_t target = adv->addr; // Preserve the advertised address type.
        int rssi = adv->rssi;
        if (ble_gap_disc_cancel()) return;
        ESP_LOGI(TAG, "%s CONNECTING RSSI=%d", peer->side, rssi);
        const struct ble_gap_conn_params *timing = NULL;
#ifdef FRAME_BRIDGE
        static const struct ble_gap_conn_params fast = {
            .scan_itvl = 0x10, .scan_window = 0x10, .itvl_min = 6, .itvl_max = 12,
            .latency = 0, .supervision_timeout = 150,
        };
        if (!peer->default_timing) timing = &fast;
#endif
        int rc = ble_gap_connect(own_addr_type, &target, 5000, timing, gap_event, peer);
        if (rc) {
            ESP_LOGW(TAG, "%s connect request rc=%d", peer->side, rc);
            peer->retry_after = esp_timer_get_time() + RETRY_US;
        }
        return;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    controller_t *peer = arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        on_advertisement(&event->disc);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
#ifdef FRAME_BRIDGE
            peer->default_timing = !peer->default_timing;
#endif
            ESP_LOGW(TAG, "%s connect failed status=%d", peer->side, event->connect.status);
            peer->retry_after = esp_timer_get_time() + RETRY_US;
            break;
        }
        peer->link.conn = event->connect.conn_handle;
        ESP_LOGI(TAG, "%s CONNECTED handle=%u", peer->side, peer->link.conn);
        if (ble_gattc_exchange_mtu(peer->link.conn, on_mtu, peer)) discover_service(peer);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
#ifdef FRAME_BRIDGE
        bridge_connection((unsigned)(peer - controllers), false);
#endif
        ESP_LOGW(TAG, "%s DISCONNECTED reason=%d notifications=%" PRIu32,
                 peer->side, event->disconnect.reason, peer->link.packets);
        peer->link = (link_t){.conn = BLE_HS_CONN_HANDLE_NONE};
        peer->retry_after = esp_timer_get_time() + RETRY_US;
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (!current_link(peer, event->notify_rx.conn_handle) ||
            !peer->link.value || event->notify_rx.attr_handle != peer->link.value) break;
        ++peer->link.packets;
#ifdef FRAME_BRIDGE
        uint8_t bytes[512];
        size_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (length <= sizeof(bytes) && os_mbuf_copydata(event->notify_rx.om, 0, length, bytes) == 0) {
            bridge_receive((unsigned)(peer - controllers), bytes, length);
        }
#else
        int64_t now = esp_timer_get_time();
        if (now >= peer->link.next_sample) {
            log_payload(peer, "NOTIFY", event->notify_rx.om);
            peer->link.next_sample = now + CONFIG_FRAME_LOG_INTERVAL_MS * INT64_C(1000);
        }
#endif
        break;
    }
    default:
        break;
    }
    return 0;
}

// GAP callbacks, GATT callbacks, and this timer share the NimBLE host queue.
static void maintain_links(struct ble_npl_event *event)
{
    if (!host_ready) return;
#ifdef FRAME_BRIDGE
    // Advertising the gamepad and scanning for the two hands are independent GAP roles.
    ble_host_tick(own_addr_type);
#endif
    bool need_scan = false;
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < PEER_COUNT; ++i) {
        controller_t *peer = &controllers[i];
        if (!peer->address) continue;
        if (peer->link.conn == BLE_HS_CONN_HANDLE_NONE) {
            if (now >= peer->retry_after) need_scan = true;
        } else if (peer->link.closing) {
            close_link(peer);
        } else if (!peer->link.packets && peer->link.quiet_notice && now >= peer->link.quiet_notice) {
            ESP_LOGI(TAG, "%s subscribed for 10 s, no notifications received", peer->side);
            peer->link.quiet_notice = 0;
        }
    }
    if (need_scan && !ble_gap_disc_active() && !ble_gap_conn_active()) {
        const struct ble_gap_disc_params params = {.passive = 1, .itvl = 0x50, .window = 0x30};
        int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
        if (rc) ESP_LOGW(TAG, "scan rc=%d", rc);
    }
    ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(1000));
}

static void on_reset(int reason)
{
    host_ready = false;
#ifdef FRAME_BRIDGE
    ble_host_reset();
    for (unsigned h = 0; h < PEER_COUNT; ++h) bridge_connection(h, false);
#endif
    ble_npl_callout_stop(&timer);
    ESP_LOGW(TAG, "BLE host reset reason=%d", reason);
}

static void on_sync(void)
{
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &own_addr_type));
    for (size_t i = 0; i < PEER_COUNT; ++i) {
        controllers[i].link = (link_t){.conn = BLE_HS_CONN_HANDLE_NONE};
        controllers[i].retry_after = 0;
        if (controllers[i].address)
            ESP_LOGI(TAG, "Looking for %s controller", controllers[i].side);
    }
    host_ready = true;
    maintain_links(NULL);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
#ifdef FRAME_BRIDGE
    controllers[0].address = pairing.left;
    controllers[1].address = pairing.right;
    bridge_init();
#else
    controllers[0].address = CONFIG_FRAME_LEFT_ADDRESS;
    controllers[1].address = CONFIG_FRAME_RIGHT_ADDRESS;
#endif
    for (size_t i = 0; i < PEER_COUNT; ++i) {
        if (!strcasecmp(controllers[i].address, "00:00:00:00:00:00"))
            controllers[i].address = NULL;
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
#ifdef FRAME_BRIDGE
    ble_host_init();
#endif
    ble_npl_callout_init(&timer, nimble_port_get_dflt_eventq(), maintain_links, NULL);
    nimble_port_freertos_init(host_task);
}
