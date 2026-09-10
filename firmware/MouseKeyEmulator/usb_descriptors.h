// USB identity for the ESP32-S2 bridge.
//
// The whole point of this file is that the controlled machine sees an ordinary
// composite keyboard+mouse, not an ESP32.  Everything the OS looks at to name
// and classify a device is spoofed here:
//
//   * idVendor / idProduct       -> a real, widely trusted combo
//   * iManufacturer / iProduct   -> human readable strings shown in Device Mgr
//   * iSerialNumber              -> per-board, so several boards on one host do
//                                   not collide
//   * bcdDevice, power, HID report descriptors -> match a normal HID device
//
// The default identity below is a Logitech-style receiver.  Change VENDOR_ID /
// PRODUCT_ID and the two strings to mimic whatever keyboard/mouse you own; the
// closer they match a device you actually have, the less anything stands out.

#pragma once

// ---- spoofed identity ------------------------------------------------------
#define SPOOF_VENDOR_ID    0x046D          // Logitech, Inc.
#define SPOOF_PRODUCT_ID   0xC52B          // Unifying Receiver
#define SPOOF_BCD_DEVICE   0x1203
#define SPOOF_MANUFACTURER "Logitech"
#define SPOOF_PRODUCT      "USB Receiver"
// iSerialNumber is generated at runtime from the chip MAC (see main .ino).

// ---- HID report IDs (composite device) ------------------------------------
enum {
  REPORT_ID_KEYBOARD = 1,
  REPORT_ID_MOUSE    = 2,
  REPORT_ID_CONSUMER = 3,
};

// Standard boot-compatible keyboard + a 5-button mouse with wheel and pan,
// plus a consumer-control page for media keys.  These are the exact
// descriptors a normal composite HID device exposes, so the OS builds the
// same driver stack it would for a real keyboard/mouse.
#define MKE_HID_REPORT_DESC()                                       \
  TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))   \
  TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE))         \
  TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER))
