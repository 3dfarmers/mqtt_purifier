// =============================================================================
// BambuPurifier configuration
// =============================================================================
// Copy this file to config.h (same folder) and fill it in. config.h is in
// .gitignore so your passwords never end up in a commit. The sketch falls back
// to this example file when config.h is missing, so it always compiles.
//
// The purifier settings below are only FIRST-BOOT defaults: once you change
// them from the serial console (`fume ip ...`, `fume token ...`) the saved
// values win. WiFi and printer settings are read from here on every boot.
// =============================================================================
#pragma once

// --- WiFi (2.4 GHz: no ESP32 can join a 5 GHz-only network) -----------------
#define WIFI_SSID            "your-ssid"
#define WIFI_PASSWORD        "your-wifi-password"

// --- Bambu Lab printer, LAN mode --------------------------------------------
// Printer screen: network settings show the IP and the LAN access code, the
// device page shows the serial number. Tested with the printer in LAN only
// mode. Developer Mode is not needed: this firmware only reads the report
// topic and sends the standard "pushall" state request.
#define PRINTER_IP           "192.168.1.60"
#define PRINTER_SERIAL       "01P00A000000000"   // upper case, as printed on the device
#define PRINTER_ACCESS_CODE  "12345678"

// --- Xiaomi purifier ---------------------------------------------------------
// Give the purifier a DHCP reservation in your router. Token: 32 hex
// characters from the Xiaomi Cloud Tokens Extractor (see README). Leave ""
// to set them later from the serial console.
#define PURIFIER_IP          ""
#define PURIFIER_TOKEN       ""

// --- Automation ---------------------------------------------------------------
#define FUME_TYPES           "ABS,ASA"   // prefix match: ABS also covers ABS-GF
#define FUME_RUNON_MIN       10          // minutes the purifier keeps running after the print
#define FUME_ENABLED         1           // 1 = automatic control on from the first boot

// --- Advanced -----------------------------------------------------------------
// Largest Bambu report measured: H2S 18101 B, P2S 13735 B, X1C 9370 B,
// P1S ~4.6 KB. PubSubClient silently DROPS any packet bigger than its buffer.
#define MQTT_BUFFER_SIZE     24576
