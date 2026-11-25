#pragma once

enum { USB_LAYOUT_EXTENDED = 0, USB_LAYOUT_STANDARD = 1, USB_LAYOUT_PRO3 = 2 };

// An offline setup page personalizes this fixed-size record in the release image.
typedef struct {
    char marker[17];
    char left[18];
    char right[18];
    unsigned char usb_layout;
} paired_controllers;

extern const paired_controllers pairing;
