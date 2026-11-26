#include "bridge.h"
#include "pairing.h"
#include "pro3.h"
#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static const uint8_t report_descriptor[] = {
    0x05,0x01, 0x09,0x05, 0xa1,0x01,                 // Desktop / Game Pad
    0x05,0x09, 0x19,0x01, 0x29,0x10,                 // Buttons 1..16
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x10, 0x81,0x02,
    0x05,0x01, 0x09,0x39, 0x15,0x00, 0x25,0x07,    // eight-way hat
    0x35,0x00, 0x46,0x3b,0x01, 0x65,0x14,
    0x75,0x04, 0x95,0x01, 0x81,0x42, 0x81,0x03,    // null state, padding
    0x65,0x00, 0x35,0x00, 0x45,0x00,                // clear angle units and bounds
    0x09,0x30, 0x09,0x31, 0x09,0x33, 0x09,0x34,    // X, Y, Rx, Ry
    0x16,0x00,0x80, 0x26,0xff,0x7f,
    0x75,0x10, 0x95,0x04, 0x81,0x02,
    0x05,0x09, 0x19,0x11, 0x29,0x12,                 // grip full-click buttons 17..18
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x02, 0x81,0x02,
    0x75,0x06, 0x95,0x01, 0x81,0x03, 0xc0
};
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_HID_DESC_LEN,0,100),
    TUD_HID_DESCRIPTOR(0,0,HID_ITF_PROTOCOL_NONE,sizeof(report_descriptor),0x81,16,1),
};
static const uint8_t standard_report_descriptor[] = { TUD_HID_REPORT_DESC_GAMEPAD() };
static const uint8_t standard_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_HID_DESC_LEN,0,100),
    TUD_HID_DESCRIPTOR(0,0,HID_ITF_PROTOCOL_NONE,sizeof(standard_report_descriptor),0x81,16,1),
};

static const tusb_desc_device_t pro3_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x2dc8, .idProduct = 0x6009, .bcdDevice = 0x0200,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};

// Wired D-mode gamepad inputs. Apple indexes the four stick axes followed by
// Accelerator/Brake and indexes all 18 Button usages, including PL/PR and L4/R4.
static const uint8_t pro3_report_descriptor[] = {
    0x05,0x01, 0x09,0x05, 0xa1,0x01, 0x85,PRO3_INPUT_ID,  // Desktop / Game Pad
    0x09,0x39, 0x15,0x00, 0x25,0x07, 0x35,0x00, 0x46,0x3b,0x01, 0x65,0x14,
    0x75,0x08, 0x95,0x01, 0x81,0x42,                    // hat with null state
    0x65,0x00, 0x35,0x00, 0x45,0x00,
    0x26,0xff,0x00, 0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35,
    0x95,0x04, 0x81,0x02,                               // X, Y, Z, Rz
    0x05,0x02, 0x09,0xc4, 0x09,0xc5, 0x95,0x02, 0x81,0x02,
    0x05,0x09, 0x19,0x01, 0x29,0x12, 0x25,0x01,
    0x75,0x01, 0x95,0x12, 0x81,0x02,                    // buttons 1..18
    0x75,0x06, 0x95,0x01, 0x81,0x03,                    // align to byte 10
    0x75,0x08, 0x95,PRO3_INPUT_PAYLOAD_SIZE - 10, 0x81,0x03, // unused vendor data
    0xc0,
};
static const uint8_t pro3_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_HID_DESC_LEN,0,100),
    TUD_HID_DESCRIPTOR(0,0,HID_ITF_PROTOCOL_NONE,sizeof(pro3_report_descriptor),
                       0x81,PRO3_INPUT_PAYLOAD_SIZE+1,1),
};
_Static_assert(PRO3_INPUT_PAYLOAD_SIZE + 1 <= CFG_TUD_HID_EP_BUFSIZE, "Pro 3 USB packet size");
_Static_assert(sizeof(hid_gamepad_report_t) == 11, "TinyUSB standard gamepad report");
static char serial_number[15];
static const char *strings[] = {"\x09\x04","Frame Bridge","Frame Bridge Gamepad",serial_number};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    if (pairing.usb_layout == USB_LAYOUT_PRO3) return pro3_report_descriptor;
    return pairing.usb_layout == USB_LAYOUT_STANDARD ? standard_report_descriptor : report_descriptor;
}

static uint8_t input_report_id(void)
{
    return pairing.usb_layout == USB_LAYOUT_PRO3 ? PRO3_INPUT_ID : 0;
}

static size_t encode_report(const pad_report *r, uint8_t buffer[PRO3_INPUT_PAYLOAD_SIZE])
{
    if (pairing.usb_layout == USB_LAYOUT_PRO3) return pro3_encode(r, buffer);
    if (pairing.usb_layout == USB_LAYOUT_STANDARD) return pad_standard_report(r, buffer);
    memcpy(buffer, r->bytes, sizeof(r->bytes));
    return sizeof(r->bytes);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t requested)
{
    if (instance || id != input_report_id() || type != HID_REPORT_TYPE_INPUT) return 0;
    pad_report report;
    bridge_get_output(false, &report);
    uint8_t encoded[PRO3_INPUT_PAYLOAD_SIZE];
    size_t n = encode_report(&report, encoded);
    if (n > requested) n = requested;
    memcpy(buffer, encoded, n);
    return n;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t size) {}
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t size)
{
    bridge_usb_complete();
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type,
                              uint8_t const *report, uint16_t transferred)
{
    if (type == HID_REPORT_TYPE_INPUT && tud_mounted() && !tud_suspended()) {
        // After a failed transfer, the host may still have the previous button state.
        bridge_usb_online(true);
    }
}

static void usb_event(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
    case TINYUSB_EVENT_RESUMED: bridge_usb_online(true); break;
    case TINYUSB_EVENT_DETACHED:
    case TINYUSB_EVENT_SUSPENDED: bridge_usb_online(false); break;
    default: break;
    }
}

bool usb_pad_send(const pad_report *r)
{
    uint8_t encoded[PRO3_INPUT_PAYLOAD_SIZE];
    size_t n = encode_report(r, encoded);
    return tud_hid_ready() && tud_hid_report(input_report_id(), encoded, n);
}

void usb_pad_init(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BASE));
    snprintf(serial_number, sizeof(serial_number), "%02X%02X%02X%02X%02X%02X",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event);
    if (pairing.usb_layout == USB_LAYOUT_PRO3) {
        strcat(serial_number, "-P");
        strings[1] = "8BitDo";
        strings[2] = "8BitDo Pro 3";
        config.descriptor.device = &pro3_device;
        config.descriptor.full_speed_config = pro3_configuration;
    } else if (pairing.usb_layout == USB_LAYOUT_STANDARD) {
        strcat(serial_number, "-S");
        strings[2] = "Frame Bridge Standard";
        config.descriptor.full_speed_config = standard_configuration;
    } else {
        config.descriptor.full_speed_config = configuration;
    }
    config.descriptor.string = strings;
    config.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&config));
}
