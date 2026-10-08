// =============================================================================
// PurifierBench - bring up the purifier and test the logic without a printer
// =============================================================================
// Step one on any new setup. Joins WiFi and gives you the purifier console:
//
//   fume info            model + firmware: proves IP and token are right
//   fume on / fume off   switch it now
//   fume status          power, mode, PM2.5
//
// and a print SIMULATOR that drives the same state machine a real printer
// would, through the library's printer-agnostic input onPrintState():
//
//   sim RUNNING ABS      a print with ABS is under way   -> purifier ON
//   sim PAUSE ABS        still on
//   sim FINISH           print over                      -> OFF after the run-on
//   sim FAILED           cancelled / failed              -> same
//   sim RUNNING PLA      not a fume filament             -> stays off
//
// Use `fume runon 1` and `fume enable` first, so a simulated print finishes
// its run-on in a minute. This is also the template for a non-Bambu printer
// (Klipper, Prusa, OctoPrint): call fume.onPrintState() with what your
// printer reports.
//
// Boards: any ESP32 with WiFi. Core: Arduino-ESP32 3.x. No other libraries.
// =============================================================================
#include <WiFi.h>
#include <MqttPurifier.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"   // copy it to config.h and fill it in
#endif

static void handleLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  if (fume.handleCommand(line, Serial)) return;

  if (line.length() >= 4 && line.substring(0, 4).equalsIgnoreCase("sim ")) {
    String rest = line.substring(4);
    rest.trim();
    int sp = rest.indexOf(' ');
    String state = (sp < 0) ? rest : rest.substring(0, sp);
    String type  = (sp < 0) ? "" : rest.substring(sp + 1);
    type.trim();
    Serial.printf("[SIM] printer %s, filament %s\n", state.c_str(), type.length() ? type.c_str() : "-");
    fume.onPrintState(state.c_str(), type.c_str());
  } else if (line.equalsIgnoreCase("help")) {
    Serial.println("Commands: sim <PREPARE|RUNNING|PAUSE|FINISH|FAILED|IDLE> [filament], wifi, fume help");
  } else if (line.equalsIgnoreCase("wifi")) {
    Serial.printf("WiFi %s, RSSI %d dBm\n",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "down",
                  WiFi.RSSI());
  } else {
    Serial.println("Unknown command - type `help`");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\nPurifierBench (MqttPurifier %s)\n", MQTT_PURIFIER_VERSION);

  FumeDefaults d;
  d.ip    = PURIFIER_IP;
  d.token = PURIFIER_TOKEN;
  fume.begin(Serial, &d);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // see BambuPurifier: modem sleep loses the first UDP packets
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Type `help`, then `fume info` once WiFi is up.");
}

void loop() {
  static bool wasUp = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) {
    wasUp = up;
    if (up) Serial.printf("[WiFi] Connected, IP %s, RSSI %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else    Serial.println("[WiFi] Lost (it reconnects by itself)");
  }

  fume.loop();

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
