// =============================================================================
// BambuPurifier - complete firmware for any ESP32
// =============================================================================
// Watches a Bambu Lab printer over its local MQTT and switches a Xiaomi air
// purifier on for ABS/ASA prints, off again 10 min after the print ends.
// Logs the purifier's PM2.5 every 10 s while it runs.
//
// Boards:   any ESP32 with WiFi (ESP32, -S2, -S3, -C3, -C6). No pins used.
// Core:     Arduino-ESP32 3.x (tested 3.3.8)
// Library:  PubSubClient 2.8 (Library Manager), plus this library
// Setup:    copy config.example.h to config.h and fill it in, then flash.
//           Serial monitor at 115200 baud, type `help`.
//
// IMPORTANT: this sketch opens its OWN MQTT connection to the printer. Bambu
// printers accept only a few local clients (the A1 reliably only one). If your
// ESP32 firmware already talks to the printer, do not run this sketch; call
// fume.onBambuReport() from your existing MQTT callback instead (README,
// "Add it to your own firmware").
// =============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <MqttPurifier.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"   // copy it to config.h and fill it in
#endif

static WiFiClientSecure tls;
static PubSubClient     mqtt(tls);
static char topicReport[48], topicRequest[48];
static char printerSerial[24];

static uint32_t mqttLastAttempt = 0;
static uint8_t  mqttFails       = 0;
static uint32_t mqttLastRx      = 0;
static uint32_t wifiLastAttempt = 0;
static bool     wifiWasUp       = false;

// -----------------------------------------------------------------------------
// Printer: every /report packet goes to the library, unfiltered and unthrottled
// -----------------------------------------------------------------------------
static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  mqttLastRx = millis();
  fume.onBambuReport((const char*)payload, length);
}

static void requestPushall() {
  // Bambu printers do not send their state on subscribe; ask for it.
  mqtt.publish(topicRequest, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}");
}

static bool mqttConnect() {
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "purifier-%06lx",
           (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));
  Serial.printf("[MQTT] Connecting to %s:8883 (heap %u, largest block %u)\n", PRINTER_IP,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  // Blocks for the TLS handshake (seconds; up to the timeout on a dead host).
  if (!mqtt.connect(clientId, "bblp", PRINTER_ACCESS_CODE)) {
    // rc 5 = wrong access code (or another host at that IP), -2 = unreachable
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
    return false;
  }
  mqtt.subscribe(topicReport);
  requestPushall();
  Serial.printf("[MQTT] Connected, subscribed to %s\n", topicReport);
  return true;
}

static void serviceMqtt() {
  if (mqtt.connected()) {
    mqtt.loop();
    // Connected but silent: usually a wrong serial number (topics are case
    // sensitive). Ask again now and then; the printer answers a pushall.
    static uint32_t lastNag = 0;
    if (millis() - mqttLastRx > 60000 && millis() - lastNag > 60000) {
      lastNag = millis();
      Serial.println("[MQTT] No data from the printer for 60 s - check PRINTER_SERIAL (upper case)");
      requestPushall();
    }
    return;
  }
  // Reconnect every 5 s, once a minute after 3 straight failures: each
  // attempt blocks for the TLS handshake, which would starve the purifier.
  uint32_t gap = (mqttFails >= 3) ? 60000 : 5000;
  if (mqttLastAttempt && millis() - mqttLastAttempt < gap) return;
  mqttLastAttempt = millis();
  if (mqttConnect()) { mqttFails = 0; mqttLastRx = millis(); }
  else if (mqttFails < 3) mqttFails++;
}

// -----------------------------------------------------------------------------
// WiFi: started once, re-joined in the background if it drops
// -----------------------------------------------------------------------------
static void serviceWifi() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wifiWasUp) {
    wifiWasUp = up;
    if (up) Serial.printf("[WiFi] Connected, IP %s, RSSI %d dBm%s\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          WiFi.RSSI() < -75 ? " (weak: expect lost purifier packets)" : "");
    else    Serial.println("[WiFi] Lost");
  }
  if (!up && millis() - wifiLastAttempt > 15000) {
    wifiLastAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// -----------------------------------------------------------------------------
// Serial console: `fume ...` lines go to the library, plus a few of our own
// -----------------------------------------------------------------------------
static void handleLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  if (fume.handleCommand(line, Serial)) return;
  if (line.equalsIgnoreCase("help")) {
    Serial.println("Commands: status, pushall, reboot, fume help");
  } else if (line.equalsIgnoreCase("status")) {
    Serial.printf("WiFi %s (RSSI %d dBm) | MQTT %s | last printer data %lu s ago | heap %u\n",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "down",
                  WiFi.RSSI(), mqtt.connected() ? "connected" : "down",
                  (unsigned long)((millis() - mqttLastRx) / 1000), (unsigned)ESP.getFreeHeap());
    FumeStatus s = fume.status();
    Serial.printf("Purifier %s | power %d | AQI %d | printer %s | active filament %s\n",
                  s.state, s.power, s.aqi, s.printerState, s.activeType);
  } else if (line.equalsIgnoreCase("pushall")) {
    if (mqtt.connected()) requestPushall();
  } else if (line.equalsIgnoreCase("reboot")) {
    ESP.restart();
  } else {
    Serial.println("Unknown command - type `help`");
  }
}

static void serviceConsole() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) handleLine(buf);
      buf = "";
    } else if (buf.length() < 200) {
      buf += c;
    }
  }
}

// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);   // native-USB boards: give the host a moment to open the port
  Serial.printf("\nBambuPurifier (MqttPurifier %s)\n", MQTT_PURIFIER_VERSION);

  // Topics are case sensitive and Bambu serials are upper case.
  size_t i = 0;
  for (const char* p = PRINTER_SERIAL; *p && i + 1 < sizeof(printerSerial); p++)
    if (*p != ' ') printerSerial[i++] = (char)toupper((unsigned char)*p);
  printerSerial[i] = 0;
  snprintf(topicReport, sizeof(topicReport), "device/%s/report", printerSerial);
  snprintf(topicRequest, sizeof(topicRequest), "device/%s/request", printerSerial);

  FumeDefaults d;
  d.ip       = PURIFIER_IP;
  d.token    = PURIFIER_TOKEN;
  d.types    = FUME_TYPES;
  d.runonMin = FUME_RUNON_MIN;
  d.enabled  = FUME_ENABLED;
  fume.begin(Serial, &d);
  // Another MIoT purifier? Set its property ids before begin(), e.g.
  //   MiotMap m; m.powerSiid = 2; m.powerPiid = 2; ... fume.setMiotMap(m);

  WiFi.mode(WIFI_STA);
  // No modem sleep: with it, the first UDP packet to a sleeping station is
  // often late or lost. This board is mains powered; the cost is ~80 mA.
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiLastAttempt = millis();

  tls.setInsecure();                 // the printer uses a self-signed certificate
  mqtt.setServer(PRINTER_IP, 8883);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(onMqttMessage);
  Serial.println("Type `help` for commands, `fume help` for the purifier.");
}

void loop() {
  serviceWifi();
  if (WiFi.status() == WL_CONNECTED) serviceMqtt();
  fume.loop();        // purifier state machine + UDP exchange, never blocks
  serviceConsole();
}
