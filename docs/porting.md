# Porting: other purifiers, printers, boards and frameworks

## Other purifiers

Any Xiaomi device that speaks **MIoT** over miIO works if you know three
properties: power, mode, PM2.5. The library takes them as a `MiotMap`:

```cpp
MiotMap m;
m.powerSiid = 2; m.powerPiid = 1;   // bool on/off
m.modeSiid  = 2; m.modePiid  = 4;   // operating mode
m.aqiSiid   = 3; m.aqiPiid   = 4;   // PM2.5 density
m.autoMode  = 0;                    // the mode value meaning "auto"
fume.setMiotMap(m);                 // before fume.begin()
fume.begin(Serial);
```

How to find the values:

1. Get the model string: `python tools/miio.py info --ip ...` or `fume info`.
2. Open `https://home.miot-spec.com/spec/<model>` (for example
   `https://home.miot-spec.com/spec/zhimi.airp.mb4a`).
3. In the **air-purifier** service, take its `siid`, then the `piid` of the
   **on** property (bool) and of **mode** (enum; note the value list and pick
   the one named Auto). In the **environment** service take **pm2.5-density**.
4. Verify with `python tools/miio.py status --ip ... --map ps,pp,ms,mp,qs,qp,auto`
   before flashing. A wrong id answers with a negative `code`, which the
   firmware logs as `REFUSED`.

Verified on hardware: Mi Air Purifier 3C (`zhimi.airp.mb4a`, firmware 2.1.7):
power 2/1, mode 2/4 (auto 0, sleep 1, favorite 2), PM2.5 3/4. python-miio
uses the same map for `zhimi.airpurifier.mb4`. Other models are untested;
read their spec.

Old **miIO-only** devices (pre-MIoT, e.g. some first-generation purifiers) use
`get_prop` / `set_power` style methods instead of `get_properties`; they need
different request bodies in `FumeExtractor::buildParams()` and the reply
parsing.

Fans, smart plugs or anything else on MIoT work the same way: only the
property map changes. For a plug, point power at its switch property and set
mode and PM2.5 to properties it has, or remove those requests.

## Other printers

Feed the state machine with `onPrintState(state, filament)` using Bambu's
state words. Call it whenever your printer reports, or at least on changes.

| Printer API | Print under way | Print over | Filament |
|---|---|---|---|
| Klipper / Moonraker `print_stats.state` | `printing` -> `RUNNING`, `paused` -> `PAUSE` | `complete` -> `FINISH`, `cancelled`/`error` -> `FAILED`, `standby` -> `IDLE` | slicer metadata (`filament_type` in `server/files/metadata`) or a macro variable |
| PrusaLink `/api/v1/status` `printer.state` | `PRINTING` -> `RUNNING`, `PAUSED` -> `PAUSE` | `FINISHED` -> `FINISH`, `STOPPED`/`ERROR` -> `FAILED`, `IDLE` | `/api/v1/job` file metadata, or a filename convention |
| OctoPrint `/api/job` `state` | `Printing` -> `RUNNING`, `Paused` -> `PAUSE` | `Operational` after a job -> `FINISH`, `Cancelling`/`Error` -> `FAILED` | file name or a plugin |
| Elegoo SDCP (Centauri Carbon) status | printing -> `RUNNING` | finished / stopped -> `FINISH` / `FAILED` | slicer metadata |

If you cannot get the filament at all, use `fume always on`: every print
switches the purifier on.

`examples/PurifierBench` shows the call (its `sim` command).

## Other ESP32 boards

| Chip | WiFi | Status |
|---|---|---|
| ESP32 | yes | compiles (Arduino-ESP32 3.3.8): BambuPurifier 1.03 MB flash, 50.6 KB globals |
| ESP32-S2 | yes | compiles: 1.01 MB, single core |
| ESP32-S3 | yes | compiles: 1.01 MB; **bench-tested** (FarmBoard 2, no PSRAM) |
| ESP32-C3 | yes | compiles: 1.10 MB, single core RISC-V |
| ESP32-C6 | yes | compiles: 1.12 MB |
| ESP32-H2, -P4 | no built-in WiFi | needs a WiFi companion; out of scope |

Board-specific notes:

- **Native USB boards** (S2/S3/C3/C6 with USB-Serial-JTAG or TinyUSB CDC): set
  "USB CDC On Boot" to Enabled to see `Serial`. Opening the port from Windows
  can reset the chip (`rst:0x15`), and the port disappears during resets.
- **No PSRAM**: keep the MQTT buffer at 24 KB or below; the TLS handshake needs a
  ~30 KB contiguous heap block. The library itself uses ~3 KB static RAM.
- **Modem sleep** (`WiFi.setSleep(true)`, the default) delays or drops the
  first inbound UDP packet; the examples turn it off.
- **Signal**: below about -80 dBm UDP exchanges start to fail while MQTT still
  works. Log `WiFi.RSSI()`.

## ESP-IDF without Arduino

| Arduino piece | ESP-IDF replacement |
|---|---|
| `WiFiUDP` | lwIP socket: `socket(AF_INET, SOCK_DGRAM, 0)`, `fcntl(O_NONBLOCK)`, `sendto`, `recvfrom(..., MSG_DONTWAIT)` |
| `Preferences` | `nvs_open("fume", ...)`, `nvs_get_str`/`nvs_set_str`, `nvs_get_u16`, `nvs_get_u8` |
| `Print& log` | an `ESP_LOGI` wrapper |
| `millis()` | `esp_timer_get_time() / 1000` |
| `String` (console only) | `std::string` or C strings |
| PubSubClient | `esp-mqtt` (`esp_mqtt_client_config_t`: `broker.address.uri = "mqtts://IP:8883"`, `credentials.username = "bblp"`, the access code as password, `buffer.size` >= the largest report). For the self-signed certificate enable `CONFIG_ESP_TLS_INSECURE` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`, or pin the printer's certificate. |

mbedTLS AES/MD5 calls are identical. `MiniJson.h` is plain C++.

## Other languages / MCUs

You need: UDP, AES-128 (single-block ECB is enough; CBC is a 10-line loop),
MD5, a JSON reader, a monotonic millisecond clock. Steps:

1. Port `tools/miio.py` first and check it against the test vector in
   [miio-protocol.md](miio-protocol.md#7-worked-example-test-vector).
2. Port the merge rules and the state machine from
   [bambu-mqtt.md](bambu-mqtt.md) and [state-machine.md](state-machine.md).
3. Run the acceptance test in [AGENTS.md](../AGENTS.md#8-acceptance-test-run-on-hardware).

## Adding a feature: checklist

- Anything that touches the network goes through `MiioClient`'s single
  in-flight request; never add a blocking call.
- New printer fields: extend `parsePrint()`; keep the merge rules.
- New console commands: extend `handleCommand()`; validate input; never echo
  secrets.
- New settings: NVS keys are limited to 15 characters; keep the `fume_` prefix.
