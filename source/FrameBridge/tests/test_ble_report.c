#include "ble_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Check the HOGP characteristic width against the descriptor sent to the host.
static void check_report_size(void)
{
    unsigned size = 0, count = 0, bits = 0, reports = 0;
    for (size_t pos = 0; pos < ble_pad_report_map_size;) {
        uint8_t prefix = ble_pad_report_map[pos++];
        unsigned length = prefix & 3;
        if (length == 3) length = 4;
        assert(prefix != 0xfe && pos + length <= ble_pad_report_map_size);
        unsigned item = 0;
        for (unsigned i = 0; i < length; ++i)
            item |= (unsigned)ble_pad_report_map[pos++] << (8*i);
        switch (prefix & 0xfc) {
        case 0x84: assert(item == BLE_PAD_INPUT_ID); ++reports; break;
        case 0x74: size = item; break;
        case 0x94: count = item; break;
        case 0x80: assert(reports == 1); bits += size * count; break;
        }
    }
    assert(reports == 1 && bits == BLE_PAD_INPUT_SIZE * 8);
}

int main(void)
{
    check_report_size();
    assert(BLE_PAD_VENDOR_ID == 0x2dc8 && BLE_PAD_PRODUCT_ID == 0x6009 && BLE_PAD_VERSION == 0x0100);
    uint8_t sample[BLE_PAD_INPUT_SIZE + 2], wired[PRO3_INPUT_PAYLOAD_SIZE];
    memset(sample, 0xee, sizeof(sample));
    pad_report report = {0};
    report.bytes[2] = 8;
    assert(pro3_encode_controls(&report, sample + 1) == BLE_PAD_INPUT_SIZE);
    assert(!memcmp(sample + 1, (uint8_t[]){8,127,127,127,127,0,0,0,0,0}, BLE_PAD_INPUT_SIZE));

    report.bytes[2] = 3;                 // down-right
    report.bytes[0] = 3;                 // A and B together
    report.bytes[11] = 3;                // L4 and R4
    report.trigger_level[0] = 42;
    report.trigger_level[1] = 150;
    assert(pro3_encode_controls(&report, sample + 1) == BLE_PAD_INPUT_SIZE);
    assert(!memcmp(sample + 1, (uint8_t[]){3,127,127,127,127,150,42,3,0,3}, BLE_PAD_INPUT_SIZE));
    assert(sample[0] == 0xee && sample[sizeof(sample)-1] == 0xee);
    assert(pro3_encode(&report, wired) == sizeof(wired));
    assert(!memcmp(wired, sample + 1, BLE_PAD_INPUT_SIZE));
    for (size_t i = BLE_PAD_INPUT_SIZE; i < sizeof(wired); ++i) assert(!wired[i]);
    puts("Bluetooth Pro 3 HID report checks passed.");
}
