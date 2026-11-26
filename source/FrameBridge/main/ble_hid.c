#include "ble_hid.h"
#include "ble_host.h"
#include "ble_report.h"
#include "bridge.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"

enum {
    UUID_HID = 0x1812, UUID_DEVICE_INFO = 0x180a, UUID_BATTERY = 0x180f,
    UUID_PNP_ID = 0x2a50, UUID_BATTERY_LEVEL = 0x2a19, UUID_HID_INFO = 0x2a4a,
    UUID_REPORT_MAP = 0x2a4b, UUID_CONTROL_POINT = 0x2a4c, UUID_REPORT = 0x2a4d,
    UUID_REPORT_REFERENCE = 0x2908,
    ENCRYPTED_READ = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
};

static const uint8_t ble_pad_pnp_id[] = {
    2, // USB-IF vendor ID source used by the Bluetooth Pro 3 profile.
    BLE_PAD_VENDOR_ID & 0xff, BLE_PAD_VENDOR_ID >> 8,
    BLE_PAD_PRODUCT_ID & 0xff, BLE_PAD_PRODUCT_ID >> 8,
    BLE_PAD_VERSION & 0xff, BLE_PAD_VERSION >> 8
};
static const uint8_t hid_info[] = {0x11, 0x01, 0, 0x02}; // HID 1.11, normally connectable.
static const uint8_t report_reference[] = {BLE_PAD_INPUT_ID, 1}; // Input report
static uint16_t input_handle;
_Static_assert(BLE_PAD_INPUT_SIZE <= BLE_ATT_MTU_DFLT - 3, "input must fit a default ATT notification");

static int append(struct os_mbuf *om, const uint8_t *value, size_t length)
{
    return os_mbuf_append(om, value, length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_value(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC)
        return append(ctxt->om, report_reference, sizeof(report_reference));

    switch (ble_uuid_u16(ctxt->chr->uuid)) {
    case UUID_HID_INFO:
        return append(ctxt->om, hid_info, sizeof(hid_info));
    case UUID_REPORT_MAP:
        // NimBLE applies the requested offset to this complete value for Read Blob.
        return append(ctxt->om, ble_pad_report_map, ble_pad_report_map_size);
    case UUID_REPORT: {
        if (!ble_host_authorized(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
        pad_report report;
        bridge_get_output(true, &report);
        uint8_t value[BLE_PAD_INPUT_SIZE];
        size_t length = pro3_encode_controls(&report, value);
        return append(ctxt->om, value, length);
    }
    case UUID_CONTROL_POINT: {
        if (!ble_host_authorized(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
        uint8_t command;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1 || os_mbuf_copydata(ctxt->om, 0, 1, &command) != 0)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (command > 1) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        ble_host_suspend(conn, command == 0);
        return 0;
    }
    case UUID_PNP_ID:
        return append(ctxt->om, ble_pad_pnp_id, sizeof(ble_pad_pnp_id));
    case UUID_BATTERY_LEVEL: {
        // BAS 1.1 §3.1.1: zero means no battery is present in this USB-powered bridge.
        static const uint8_t level = 0;
        return append(ctxt->om, &level, sizeof(level));
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static struct ble_gatt_dsc_def input_descriptors[] = {
    {.uuid = BLE_UUID16_DECLARE(UUID_REPORT_REFERENCE), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
     .access_cb = access_value},
    {0}
};

// This gamepad uses Report Protocol only. NimBLE adds the CCCD to the input report.
static const struct ble_gatt_chr_def hid_chars[] = {
    {.uuid = BLE_UUID16_DECLARE(UUID_HID_INFO), .flags = ENCRYPTED_READ, .access_cb = access_value},
    {.uuid = BLE_UUID16_DECLARE(UUID_REPORT_MAP), .flags = ENCRYPTED_READ, .access_cb = access_value},
    {.uuid = BLE_UUID16_DECLARE(UUID_CONTROL_POINT), .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
     .access_cb = access_value},
    {.uuid = BLE_UUID16_DECLARE(UUID_REPORT),
     .flags = ENCRYPTED_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
     .access_cb = access_value, .descriptors = input_descriptors, .val_handle = &input_handle},
    {0}
};
static const struct ble_gatt_chr_def device_chars[] = {
    {.uuid = BLE_UUID16_DECLARE(UUID_PNP_ID), .flags = ENCRYPTED_READ, .access_cb = access_value},
    {0}
};
static const struct ble_gatt_chr_def battery_chars[] = {
    {.uuid = BLE_UUID16_DECLARE(UUID_BATTERY_LEVEL), .flags = ENCRYPTED_READ, .access_cb = access_value},
    {0}
};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(UUID_HID), .characteristics = hid_chars},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(UUID_DEVICE_INFO), .characteristics = device_chars},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(UUID_BATTERY), .characteristics = battery_chars},
    {0}
};

int ble_hid_register(void)
{
    int rc = ble_gatts_count_cfg(services);
    return rc ? rc : ble_gatts_add_svcs(services);
}

uint16_t ble_hid_input_handle(void)
{
    return input_handle;
}

int ble_hid_notify(uint16_t conn, const pad_report *report)
{
    if (!ble_host_authorized(conn)) return BLE_HS_ENOTCONN;
    uint8_t value[BLE_PAD_INPUT_SIZE];
    size_t length = pro3_encode_controls(report, value);
    // The Report Reference descriptor supplies the ID. HOGP carries only the payload.
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value, length);
    if (!om) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(conn, input_handle, om);
}
