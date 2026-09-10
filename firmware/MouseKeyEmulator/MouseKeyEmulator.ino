/*
 * MouseKey Emulator - ESP32-S2 HID bridge firmware
 * ================================================
 *
 * The board plugs into the USB port of the machine you want to control and
 * appears there as an ordinary keyboard + mouse (see usb_descriptors.h -- the
 * USB identity is spoofed so nothing looks like an ESP32).  It joins your
 * Wi-Fi, listens for UDP packets from the control PC application and replays
 * them as real HID reports.
 *
 * Board:   ESP32-S2 (e.g. ESP32-S2-Saola / -FN4R2).  The S2 has a native
 *          USB-OTG peripheral, which is what lets it be a USB device at all.
 *
 * Arduino IDE setup
 * -----------------
 *   Tools -> Board            : "ESP32S2 Dev Module" (esp32 core >= 2.0.x)
 *   Tools -> USB Mode         : "USB-OTG (TinyUSB)"
 *   Tools -> Upload Mode      : "Internal USB" (or use the boot button)
 *   Libraries                 : "Adafruit TinyUSB Library" (Library Manager)
 *
 * First run
 * ---------
 *   1. Edit WIFI_SSID / WIFI_PASS below (or send them over serial, see
 *      handleSerial()).  2. Flash.  3. Open Serial Monitor at 115200 -- it
 *      prints the assigned IP and the shared key.  4. In the PC app either
 *      press "Найти в сети" or add that IP by hand.
 *
 * Protocol -- must stay in sync with app/core/protocol.py.
 */

#include <Adafruit_TinyUSB.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include "usb_descriptors.h"

// ======================= user configuration ================================
// You can hard-code your network here, or leave blank and configure it once
// over the serial console (commands: "ssid <name>", "pass <secret>", "save").
static const char *DEFAULT_WIFI_SSID = "";
static const char *DEFAULT_WIFI_PASS = "";
static const char *DEFAULT_SECRET    = "mousekey";   // must match the PC app
static const uint16_t LISTEN_PORT    = 45123;

// A friendly name reported to the PC app during discovery (not the USB name).
static const char *DEVICE_LABEL      = "ESP32-S2 Bridge";

// ======================= protocol (mirror of protocol.py) ==================
static const uint8_t  PROTO_MAGIC0 = 'M';
static const uint8_t  PROTO_MAGIC1 = 'K';
static const uint8_t  PROTO_VERSION = 1;
static const uint8_t  HEADER_SIZE  = 12;

enum : uint8_t {
  T_KEYBOARD    = 0x01,
  T_MOUSE       = 0x02,
  T_RELEASE_ALL = 0x03,
  T_PING        = 0x04,
  T_PONG        = 0x05,
  T_DISCOVER    = 0x06,
  T_HELLO       = 0x07,
  T_CONSUMER    = 0x08,
};

// ======================= HID setup =========================================
uint8_t const desc_hid_report[] = { MKE_HID_REPORT_DESC() };
Adafruit_USBD_HID usb_hid;

// ======================= globals ===========================================
Preferences prefs;
WiFiUDP udp;
String wifiSsid, wifiPass, secret;
uint32_t sharedToken = 0;
char serialNumber[17];

uint8_t rxbuf[256];
uint32_t lastKeepaliveMs = 0;
uint32_t packetsHandled = 0;

// last keyboard report we sent, so we only emit HID on real change
uint8_t curMods = 0;
uint8_t curKeys[6] = {0};
uint8_t curButtons = 0;

// ======================= FNV-1a (mirror of protocol.token_of) ==============
static uint32_t fnv1a(const String &s) {
  uint32_t h = 0x811C9DC5UL;
  for (size_t i = 0; i < s.length(); ++i) {
    h ^= (uint8_t)s[i];
    h *= 0x01000193UL;
  }
  return h;
}

static uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ======================= USB identity spoofing =============================
// TinyUSB asks the sketch for the device descriptor strings and ids through
// these weak callbacks; overriding them is what makes the board present the
// spoofed identity instead of the Espressif default.
extern "C" {

// serial string is built from the MAC so multiple boards are distinct
static void buildSerial() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(serialNumber, sizeof(serialNumber), "%04X%08X",
           (uint16_t)(mac >> 32), (uint32_t)mac);
}

}  // extern "C"

// ======================= HID emit helpers ==================================
static void sendKeyboard(uint8_t mods, const uint8_t keys[6]) {
  if (!usb_hid.ready()) return;
  usb_hid.keyboardReport(REPORT_ID_KEYBOARD, mods, (uint8_t *)keys);
}

// HID mouse deltas are signed 8-bit; a larger move is split into steps so a big
// packet still lands accurately instead of being clamped to +/-127.
static void sendMouseSimple(uint8_t buttons, int16_t dx, int16_t dy,
                            int8_t wheel, int8_t pan) {
  if (!usb_hid.ready()) return;
  if (dx == 0 && dy == 0) {
    usb_hid.mouseReport(REPORT_ID_MOUSE, buttons, 0, 0, wheel, pan);
    return;
  }
  bool wheelSent = false;
  while (dx || dy) {
    int8_t sx = (dx > 127) ? 127 : (dx < -127) ? -127 : dx;
    int8_t sy = (dy > 127) ? 127 : (dy < -127) ? -127 : dy;
    bool last = (dx == sx && dy == sy);
    usb_hid.mouseReport(REPORT_ID_MOUSE, buttons, sx, sy,
                        last && !wheelSent ? wheel : 0,
                        last && !wheelSent ? pan : 0);
    if (last) wheelSent = true;
    dx -= sx;
    dy -= sy;
    if (dx || dy) delayMicroseconds(400);
  }
}

static void releaseAll() {
  uint8_t empty[6] = {0};
  curMods = 0;
  memset(curKeys, 0, sizeof(curKeys));
  curButtons = 0;
  if (usb_hid.ready()) {
    usb_hid.keyboardReport(REPORT_ID_KEYBOARD, 0, empty);
    usb_hid.mouseReport(REPORT_ID_MOUSE, 0, 0, 0, 0, 0);
  }
}

// ======================= UDP packet handling ===============================
static void sendFrame(uint8_t type, const IPAddress &ip, uint16_t port,
                      const uint8_t *payload, uint16_t len, uint32_t seq) {
  uint8_t hdr[HEADER_SIZE];
  hdr[0] = PROTO_MAGIC0; hdr[1] = PROTO_MAGIC1;
  hdr[2] = PROTO_VERSION; hdr[3] = type;
  hdr[4] = sharedToken & 0xFF; hdr[5] = (sharedToken >> 8) & 0xFF;
  hdr[6] = (sharedToken >> 16) & 0xFF; hdr[7] = (sharedToken >> 24) & 0xFF;
  hdr[8] = seq & 0xFF; hdr[9] = (seq >> 8) & 0xFF;
  hdr[10] = (seq >> 16) & 0xFF; hdr[11] = (seq >> 24) & 0xFF;
  udp.beginPacket(ip, port);
  udp.write(hdr, HEADER_SIZE);
  if (payload && len) udp.write(payload, len);
  udp.endPacket();
}

static void sendPong(const IPAddress &ip, uint16_t port,
                     const uint8_t *tag, uint32_t seq) {
  uint8_t payload[9];
  memcpy(payload, tag, 8);
  payload[8] = usb_hid.ready() ? 1 : 0;     // hid_ready flag read by the app
  sendFrame(T_PONG, ip, port, payload, sizeof(payload), seq);
}

static void sendHello(const IPAddress &ip, uint16_t port, uint32_t seq) {
  uint8_t payload[2 + 32];
  payload[0] = PROTO_VERSION;
  payload[1] = usb_hid.ready() ? 1 : 0;
  size_t n = strlen(DEVICE_LABEL);
  if (n > 31) n = 31;
  memcpy(payload + 2, DEVICE_LABEL, n);
  payload[2 + n] = 0;
  sendFrame(T_HELLO, ip, port, payload, 2 + n + 1, seq);
}

static void handlePacket(int len, const IPAddress &ip, uint16_t port) {
  if (len < HEADER_SIZE) return;
  if (rxbuf[0] != PROTO_MAGIC0 || rxbuf[1] != PROTO_MAGIC1) return;
  if (rxbuf[2] != PROTO_VERSION) return;
  uint32_t token = rd_u32(rxbuf + 4);
  if (token != sharedToken) return;             // wrong shared key -> ignore
  uint32_t seq = rd_u32(rxbuf + 8);
  uint8_t type = rxbuf[3];
  const uint8_t *p = rxbuf + HEADER_SIZE;
  int plen = len - HEADER_SIZE;

  switch (type) {
    case T_KEYBOARD:
      if (plen >= 8) {
        curMods = p[0];
        for (int i = 0; i < 6; ++i) curKeys[i] = p[2 + i];
        sendKeyboard(curMods, curKeys);
        packetsHandled++;
      }
      break;

    case T_MOUSE:
      if (plen >= 6) {
        curButtons = p[0];
        int16_t dx = (int16_t)(p[1] | (p[2] << 8));
        int16_t dy = (int16_t)(p[3] | (p[4] << 8));
        int8_t wheel = (int8_t)p[5];
        int8_t pan = (plen >= 7) ? (int8_t)p[6] : 0;
        sendMouseSimple(curButtons, dx, dy, wheel, pan);
        packetsHandled++;
      }
      break;

    case T_CONSUMER:
      if (plen >= 2 && usb_hid.ready()) {
        uint16_t usage = p[0] | (p[1] << 8);
        usb_hid.sendReport16(REPORT_ID_CONSUMER, usage);
        delay(5);
        usb_hid.sendReport16(REPORT_ID_CONSUMER, 0);
      }
      break;

    case T_RELEASE_ALL:
      releaseAll();
      break;

    case T_PING:
      sendPong(ip, port, p, seq);
      break;

    case T_DISCOVER:
      sendHello(ip, port, seq);
      break;

    default:
      break;
  }
}

// ======================= Wi-Fi =============================================
static void connectWifi() {
  if (wifiSsid.length() == 0) {
    Serial.println("[wifi] SSID не задан. Введите: ssid <имя>, затем pass <пароль>, затем save");
    return;
  }
  Serial.printf("[wifi] Подключение к \"%s\"...\n", wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // latency matters more than power here
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    udp.begin(LISTEN_PORT);
    Serial.printf("[wifi] Подключено. IP: %s  порт: %u\n",
                  WiFi.localIP().toString().c_str(), LISTEN_PORT);
    Serial.printf("[key ] Общий ключ: \"%s\" (token 0x%08X)\n",
                  secret.c_str(), sharedToken);
  } else {
    Serial.println("[wifi] Не удалось подключиться, повтор через loop()");
  }
}

// ======================= serial console ====================================
static void saveConfig() {
  prefs.begin("mke", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("secret", secret);
  prefs.end();
  Serial.println("[cfg ] Сохранено во флеш");
}

static void loadConfig() {
  prefs.begin("mke", true);
  wifiSsid = prefs.getString("ssid", DEFAULT_WIFI_SSID);
  wifiPass = prefs.getString("pass", DEFAULT_WIFI_PASS);
  secret   = prefs.getString("secret", DEFAULT_SECRET);
  prefs.end();
  sharedToken = fnv1a(secret);
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) {
        if (line.startsWith("ssid ")) {
          wifiSsid = line.substring(5); wifiSsid.trim();
          Serial.println("[cfg ] SSID установлен");
        } else if (line.startsWith("pass ")) {
          wifiPass = line.substring(5);
          Serial.println("[cfg ] Пароль установлен");
        } else if (line.startsWith("key ")) {
          secret = line.substring(4); secret.trim();
          sharedToken = fnv1a(secret);
          Serial.printf("[cfg ] Ключ установлен (token 0x%08X)\n", sharedToken);
        } else if (line == "save") {
          saveConfig();
        } else if (line == "connect") {
          connectWifi();
        } else if (line == "status") {
          Serial.printf("[stat] Wi-Fi:%s IP:%s USB:%s пакетов:%u\n",
                        WiFi.status() == WL_CONNECTED ? "up" : "down",
                        WiFi.localIP().toString().c_str(),
                        usb_hid.ready() ? "ready" : "no",
                        packetsHandled);
        } else {
          Serial.println("[help] ssid <n> | pass <p> | key <k> | save | connect | status");
        }
      }
      line = "";
    } else if (line.length() < 128) {
      line += c;
    }
  }
}

// ======================= setup / loop ======================================
void setup() {
  buildSerial();

  // --- spoof the USB identity BEFORE TinyUSB starts ---
  TinyUSBDevice.setManufacturerDescriptor(SPOOF_MANUFACTURER);
  TinyUSBDevice.setProductDescriptor(SPOOF_PRODUCT);
  TinyUSBDevice.setSerialDescriptor(serialNumber);
  TinyUSBDevice.setID(SPOOF_VENDOR_ID, SPOOF_PRODUCT_ID);
  TinyUSBDevice.setVersion(0x0200);          // USB 2.0
  TinyUSBDevice.setDeviceVersion(SPOOF_BCD_DEVICE);

  usb_hid.setPollInterval(1);                // 1 ms -> 1000 Hz, like a gaming HID
  usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
  usb_hid.setStringDescriptor("HID");
  usb_hid.begin();

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== MouseKey Emulator - ESP32-S2 bridge ===");
  Serial.printf("[usb ] Представляюсь как %04X:%04X \"%s %s\" S/N %s\n",
                SPOOF_VENDOR_ID, SPOOF_PRODUCT_ID,
                SPOOF_MANUFACTURER, SPOOF_PRODUCT, serialNumber);

  loadConfig();
  connectWifi();
}

void loop() {
  handleSerial();

  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 5000) {
      lastTry = millis();
      connectWifi();
    }
    delay(10);
    return;
  }

  int len = udp.parsePacket();
  if (len > 0) {
    int n = udp.read(rxbuf, sizeof(rxbuf));
    if (n > 0) handlePacket(n, udp.remoteIP(), udp.remotePort());
  }

  // nothing else to do; keep the loop tight for low latency
}
