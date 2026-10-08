// =============================================================================
// MqttPurifier - switch a Xiaomi air purifier from your 3D printer's events
// =============================================================================
// Umbrella header. Including this gives you:
//   FumeExtractor fume   the automation (global instance, like Serial)
//   MiioClient           the Xiaomi miIO LAN client, usable on its own
//   mjson::              the tiny JSON walker both use
//
// Start with README.md; AI agents start with AGENTS.md.
// =============================================================================
#pragma once
#include "FumeExtractor.h"
#include "MiioClient.h"
#include "MiniJson.h"

#define MQTT_PURIFIER_VERSION "1.0.0"
