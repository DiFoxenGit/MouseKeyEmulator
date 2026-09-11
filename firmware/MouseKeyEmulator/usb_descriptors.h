// USB identity + HID report descriptor for the ESP32-S2 bridge.
//
// This build uses the ESP32 Arduino core's own USB stack (USB.h + USBHID),
// NOT the separate Adafruit TinyUSB library — that avoids the header clash
// with core 3.x.  The HID report descriptor is therefore written out as raw
// bytes here instead of via TUD_* macros.
//
// The whole point of the identity fields is that the controlled machine sees
// an ordinary composite keyboard+mouse, not an ESP32.  Change VENDOR/PRODUCT
// and the two strings to mimic a device you actually own; the default is a
// Logitech-style receiver.

#pragma once

// ---- spoofed identity ------------------------------------------------------
#define SPOOF_VENDOR_ID    0x046D          // Logitech, Inc.
#define SPOOF_PRODUCT_ID   0xC52B          // Unifying Receiver
#define SPOOF_BCD_DEVICE   0x1203
#define SPOOF_MANUFACTURER "Logitech"
#define SPOOF_PRODUCT      "USB Receiver"
// serial number is generated at runtime from the chip MAC (see the .ino)

// ---- HID report IDs (composite device) ------------------------------------
enum {
  REPORT_ID_KEYBOARD = 1,
  REPORT_ID_MOUSE    = 2,
  REPORT_ID_CONSUMER = 3,
};

// Report payload sizes (WITHOUT the leading report-id byte, which the USB
// stack prepends): keyboard = 8, mouse = 5, consumer = 2.

// Standard boot-compatible keyboard + 5-button mouse (X/Y/wheel/pan) +
// consumer control.  Hand-written so it does not depend on any TinyUSB macros.
static const uint8_t MKE_HID_REPORT_DESCRIPTOR[] = {
  // -------- Keyboard (Report ID 1) --------
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, REPORT_ID_KEYBOARD, //   Report ID
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,        //   Usage Minimum (Left Control)
  0x29, 0xE7,        //   Usage Maximum (Right GUI)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x81, 0x02,        //   Input (Data,Var,Abs) — modifier byte
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x08,        //   Report Size (8)
  0x81, 0x01,        //   Input (Const) — reserved byte
  0x95, 0x06,        //   Report Count (6)
  0x75, 0x08,        //   Report Size (8)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0xFF,        //   Logical Maximum (255)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0x00,        //   Usage Minimum (0)
  0x29, 0xFF,        //   Usage Maximum (255)
  0x81, 0x00,        //   Input (Data,Array) — 6 keys
  0xC0,              // End Collection

  // -------- Mouse (Report ID 2) --------
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x85, REPORT_ID_MOUSE, //   Report ID
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x05, 0x09,        //     Usage Page (Button)
  0x19, 0x01,        //     Usage Minimum (Button 1)
  0x29, 0x05,        //     Usage Maximum (Button 5)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x95, 0x05,        //     Report Count (5)
  0x75, 0x01,        //     Report Size (1)
  0x81, 0x02,        //     Input (Data,Var,Abs) — 5 buttons
  0x95, 0x01,        //     Report Count (1)
  0x75, 0x03,        //     Report Size (3)
  0x81, 0x01,        //     Input (Const) — 3-bit padding
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x02,        //     Report Count (2)
  0x81, 0x06,        //     Input (Data,Var,Rel) — X,Y
  0x09, 0x38,        //     Usage (Wheel)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x81, 0x06,        //     Input (Data,Var,Rel) — wheel
  0x05, 0x0C,        //     Usage Page (Consumer)
  0x0A, 0x38, 0x02,  //     Usage (AC Pan)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x81, 0x06,        //     Input (Data,Var,Rel) — pan
  0xC0,              //   End Collection
  0xC0,              // End Collection

  // -------- Consumer Control (Report ID 3) --------
  0x05, 0x0C,        // Usage Page (Consumer)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, REPORT_ID_CONSUMER, //   Report ID
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
  0x19, 0x00,        //   Usage Minimum (0)
  0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x00,        //   Input (Data,Array)
  0xC0,              // End Collection
};
