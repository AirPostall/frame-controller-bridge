#include "pairing.h"
#include "sdkconfig.h"

_Static_assert(sizeof(paired_controllers) == 54, "offline setup record layout");

const paired_controllers pairing = {
    .marker = "FRAMEBRIDGE-PAIR",
    .left = CONFIG_FRAME_LEFT_ADDRESS,
    .right = CONFIG_FRAME_RIGHT_ADDRESS,
    .usb_layout = USB_LAYOUT_PRO3
};
