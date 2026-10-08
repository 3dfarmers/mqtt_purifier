# AGENTS.md - rebuild and port specification

This file is for AI coding agents (and humans) who need to **understand,
rebuild, port or extend** this project, on any ESP32, in any framework or
language. It is self-contained: everything needed to reimplement the system
is here or in the linked `docs/` files, including facts that were only learned
on real hardware. Read it top to bottom once before writing code.

If you only need to *use* the library, read [README.md](README.md) instead.

---

## 1. What the system does

An ESP32 sits on the same LAN as a 3D printer and a Xiaomi air purifier.

| Event | Action |
|---|---|
| A print is under way (`PREPARE`, `RUNNING`, `PAUSE`) and a **fume filament** (default `ABS`, `ASA`) is used by the job or loaded in the active slot | Purifier **power on**, then **mode auto** |
| The print ends (`FINISH`, `FAILED` = failed or cancelled, `IDLE`) | Start a **run-on** timer (default 10 min) |
| Run-on elapsed | Purifier **power off** |
| A fume print starts during the run-on | Cancel the timer, send nothing (it is still on) |
| While on or in run-on | Read the purifier's PM2.5 every 10 s and log it |

Constraints that shaped every decision:

1. **Local only.** No cloud calls, no Home Assistant, no smart plug.
2. **One MQTT connection.** Bambu printers accept only a few local MQTT
   clients; the A1 reliably only one. A host firmware that already holds the
   connection must share it (the library is fed, it does not connect).
3. **Never block the loop.** The host may drive motors or watchdogs; the
   purifier exchange is a polled state machine with no waits.
4. **Transitions only, respect manual use.** Send a command when the state
   changes, never on every MQTT message. After the purifier confirms a
   command, never re-send it, so a person can override with the button/app.
5. **Never log the token** beyond its first 4 characters.
6. **Self-contained.** No pins, no board drivers; config in its own NVS
   namespace. Must drop into unrelated firmware.

## 2. Architecture

```
            +---------------------------- ESP32 ----------------------------+
 printer    |  host MQTT client          FumeExtractor                       |
 MQTT/TLS --+--> callback(payload) ----> onBambuReport()                     |
 :8883      |                             parse (MiniJson, no copy)           |
            |                             merged view: gcode_state, slots,    |
            |                               tray_now, job slots               |
            |  host loop() ------------> loop()                               |
            |                             evaluate() -> state machine         |
            |                             command queue -> MiioClient ------+--> purifier
            |  host console -----------> handleCommand("fume ...")          |    UDP :54321
            +---------------------------------------------------------------+
```

| Unit | File | Responsibility | Depends on |
|---|---|---|---|
| `mjson` | `src/MiniJson.h` | Bounded, allocation-free JSON walking over non-NUL-terminated buffers | nothing |
| `MiioClient` | `src/MiioClient.*` | One miIO exchange at a time: hello, encrypt, send, receive, verify, decrypt, retry | WiFiUDP, mbedTLS AES + MD5 |
| `FumeExtractor` | `src/FumeExtractor.*` | Printer view, decision logic, command scheduling, retries, NVS config, console, logging | the two above, Preferences |
| Example host | `examples/BambuPurifier` | WiFi, the one Bambu MQTT connection, console | PubSubClient, WiFiClientSecure |

Public API (`src/FumeExtractor.h`):

```cpp
void begin(Print& log, const FumeDefaults* defaults = nullptr);
void setMiotMap(const MiotMap& m);                   // before begin()
void onBambuReport(const char* json, size_t len);    // every MQTT /report payload
void onPrintState(const char* state, const char* filamentType);  // non-Bambu hosts
void loop();                                         // every loop()
bool handleCommand(const String& line, Print& out);  // "fume ..." console lines
FumeStatus status() const;                           // snapshot for UIs
static String redact(const String& line);            // hide a typed token
extern FumeExtractor fume;                           // global instance
```

## 3. Rebuild plan (do it in this order, test each step)

1. **miIO client against the real purifier first.** Implement the packet
   format from [docs/miio-protocol.md](docs/miio-protocol.md). Unit-test it
   against the worked example there (it is cross-checked with two independent
   AES implementations). Then: hello -> `miIO.info` -> `get_properties` ->
   `set_properties`. If `tools/miio.py info` works from a laptop and yours
   does not, the bug is yours, not the network's.
2. **Non-blocking wrapper.** One request in flight; `poll()` from the loop;
   2 s timeout per reply; 3 retries, each with a fresh hello and a new id.
3. **Bambu report parsing.** Follow [docs/bambu-mqtt.md](docs/bambu-mqtt.md):
   merged state, match units/trays by `id`, the id-only-tray-is-empty rule.
   Test with the JSON samples in that file, fed in arbitrary chunks
   (full report, then deltas).
4. **State machine** from [docs/state-machine.md](docs/state-machine.md),
   including the command queue priorities and retry pacing.
5. **Config + console + redaction.**
6. **Bench test** (section 8) before any real use.

## 4. The miIO protocol in one screen

Full detail and a byte-exact worked example: [docs/miio-protocol.md](docs/miio-protocol.md).

- UDP to the device's port **54321**. Big-endian 32-byte header:
  `magic 0x2131 | u16 length incl. header | u32 0 | u32 device id | u32 stamp | 16-byte checksum`.
- **Hello:** `21 31 00 20` + 28 x `ff`. The reply's header carries the
  device id and stamp (device uptime, seconds). The reply's checksum field is
  all `ff` on provisioned devices: the token is **not** obtainable on the LAN.
- **Keys:** `key = MD5(token)`, `iv = MD5(key + token)`, AES-128-CBC, PKCS#7.
- **Request:** header with the device id from the hello and
  `stamp = hello stamp + 1 + seconds since the hello`, the token in the
  checksum field, then the ciphertext; then replace the checksum field with
  `MD5(whole packet)`. That is the same as `MD5(header[0..15] + token + ciphertext)`.
- **JSON:** `{"id":N,"method":"...","params":[...]}`. Start N at a random
  value (1..9000) and increment per request **and per retry**: devices answer
  a repeated id from a cache.
- **Reply:** verify the checksum the same way, decrypt, strip padding and
  trailing NULs, match `"id"`. Ignore replies with another id (late answers
  to earlier attempts). A reply with `"error"` is final, do not retry.
- **MIoT methods:** `get_properties` with `[{"did","siid","piid"}]`,
  `set_properties` with `[{"did","siid","piid","value"}]`, `miIO.info` with `[]`.
  Each result entry has a `code`: `0` done, `1` accepted and completing,
  negative = refused.

## 5. Bambu printer side in one screen

Full detail with JSON samples: [docs/bambu-mqtt.md](docs/bambu-mqtt.md).

- `mqtts://<printer-ip>:8883`, TLS with a self-signed cert (skip
  verification), user `bblp`, password = LAN access code, any client id.
- Subscribe `device/<SERIAL>/report` (serial upper case, topics are case
  sensitive). Publish `{"pushing":{"sequence_id":"0","command":"pushall"}}`
  to `device/<SERIAL>/request` after connecting: the printer does not send its
  state on subscribe.
- **X1 sends the full object each time; P1, A1, P2S send only changed
  fields.** Keep a merged state and never treat a missing field as empty.
- Fields used, all under `print`: `gcode_state`; `ams.tray_now` ("0".."15" =
  AMS slot unit*4+tray, "254" external spool, "255" none);
  `ams.ams[].id` + `ams.ams[].tray[].id` + `.tray_type`; `vt_tray.tray_type`
  (external spool); in the echoed print command (`"command":"project_file"`):
  `ams_mapping`, `ams_mapping2`, `use_ams`.
- A tray object that contains **only** its `id` means the slot is empty.
- Payload sizes measured: H2S 18101 B, P2S 13735 B, X1C 9370 B, P1S up to
  ~4.6 KB with one AMS. PubSubClient **silently drops** anything larger than
  its buffer.

## 6. Hard-won facts (measured, do not "simplify" these away)

Each was observed on hardware; dates and logs in [docs/bench-notes.md](docs/bench-notes.md).

1. **ON must be two requests.** `set_properties [power=true, mode=0]` sent to a
   3C that is off returned `power 0, mode -4004`: it powered on and refused
   the mode. Power alone, then mode alone, returns `0` and `0`.
2. **`code 1` is success.** Treat MIoT codes 0 and 1 as accepted. An earlier
   version that demanded 0 everywhere logged a working ON as FAILED and would
   have re-sent it every 30 s for 10 minutes.
3. **The active slot is reported late.** On a P1S, `tray_now` stayed `255`
   through `PREPARE` and the start of `RUNNING` and changed only when the AMS
   loaded the filament, 2 min 18 s after `RUNNING` (after heating to 260 C).
   That is why the job's slot list from `project_file` is also read.
4. **The job slot list is unverified.** `ams_mapping` parsing is implemented
   and ORed with the active-slot path, so a printer that does not echo it
   falls back to proven behaviour. Confirm on your model before relying on
   job-start timing.
5. **The first exchange after a quiet spell can fail completely** (no hello
   reply in 4 tries), while the next one straight after takes ~0.5 s. Hence
   the 2 s first retry before the 30 s cadence. `WiFi.setSleep(false)` helps.
6. **Weak WiFi breaks UDP long before TCP.** At -85 to -90 dBm: 35% ping loss,
   failed WiFi joins at boot, MQTT reconnects, and about half of all miIO
   exchanges lost. Moving the board fixed all of it (0% loss). Log the RSSI.
7. **Exchange latency** with a good link: 0.35-1.1 s (`miIO.info` ~0.4-1.7 s).
8. **`miIO.info` replies are ~500-700 bytes** and may contain the token on some
   firmware. Size the receive buffer for 1 KB, print only model/firmware.
9. **Heap for TLS.** A firmware that also runs the printer's TLS needs a
   ~30 KB *contiguous* block for the handshake (more on reconnect). On an
   ESP32-S3 without PSRAM running a 64 KB log ring and a 24 KB MQTT buffer,
   the largest free block was ~34.8 KB. Keep this library's buffers static
   and small; never allocate per packet.
10. **Opening the USB-Serial-JTAG port on Windows can reset an ESP32-S3**
    (`rst:0x15 USB_UART_CHIP_RESET`), and the port vanishes during resets.
    Bench tooling must reconnect, and a reset re-runs the host's init.

## 7. Invariants checklist (verify in code review)

- [ ] No call can block longer than a few ms: no `delay()`, no blocking
      `recvfrom`, no waiting loops in the library.
- [ ] `onBambuReport()` is called for **every** packet, before host throttling.
- [ ] The parser never reads past `payload + length` and never needs a NUL.
- [ ] Units and trays matched by `id`; a missing field keeps the old value.
- [ ] Power commands only on state transitions; nothing re-sent after a
      confirmed command; a failed command retried with pacing, then dropped.
- [ ] Request ids random at start, incremented per attempt.
- [ ] Token: validated as 32 hex; never logged beyond 4 characters, including
      echoed console lines (`redact()`).
- [ ] Reply checksum verified before decrypting; replies with a foreign id ignored.
- [ ] `PAUSE` keeps it on; an unknown/empty printer state holds the current state.

## 8. Acceptance test (run on hardware)

1. `tools/miio.py hello` and `info` from a laptop work (network + token).
2. On the ESP32: `fume info` prints the model; `fume on` logs
   `ON confirmed` then `Auto mode set`; `fume status` shows power on, a PM2.5
   value; `fume off` logs `OFF confirmed`.
3. `fume runon 1`, `fume enable`. Then:
   1. PLA print: purifier stays off.
   2. ABS/ASA print: purifier on (at job start if the job slot list works on
      your printer, otherwise when the filament loads).
   3. Cancel: off one minute later.
   4. Start a fume print during the run-on: no OFF is sent, it stays on.
   5. Unplug the purifier during a print: the ESP32 keeps running, logs
      `Purifier not answering`, recovers when it is back.
4. Without a printer: `examples/PurifierBench`, `sim RUNNING ABS`,
   `sim FINISH`, and so on, exercise the same state machine.

## 9. Porting notes

Details in [docs/porting.md](docs/porting.md).

- **ESP32 variants.** Both examples compile for ESP32, ESP32-S2, ESP32-S3,
  ESP32-C3 and ESP32-C6 (Arduino-ESP32 3.3.8; CI checks it on every push);
  bench-tested on an ESP32-S3. Uses only WiFi, WiFiUDP, mbedTLS (`mbedtls_aes_*`,
  `mbedtls_md5`), Preferences, `esp_random()`. ESP32-H2 and ESP32-P4 have no
  built-in WiFi.
- **ESP-IDF without Arduino.** Replace WiFiUDP with a non-blocking lwIP socket
  (`O_NONBLOCK`, `recvfrom(..., MSG_DONTWAIT)`), Preferences with `nvs_*`,
  `Print` with your logger, `millis()` with `esp_timer_get_time()/1000`.
  `esp-mqtt` replaces PubSubClient; raise its buffer the same way.
- **Other MCUs / languages.** The protocol needs UDP, AES-128 (ECB is enough,
  CBC is done by hand in `MiioClient.cpp`), MD5 and a JSON reader.
  `tools/miio.py` is a complete reference in ~300 lines of standard Python.
- **Other purifiers.** Change `MiotMap` (power, mode, PM2.5 property ids and
  the auto-mode value) from the model's MIoT spec.
- **Other printers.** Call `onPrintState(state, filament)` with Bambu's state
  words; map your printer's states as in docs/porting.md.

## 10. Where things live in the code

| Concern | Location |
|---|---|
| Packet build / parse, retries | `MiioClient::sendCommand`, `onReply`, `retryOrFail` |
| In-place AES-CBC | `cbcInPlace()` in `MiioClient.cpp` |
| Report walking, merged slots | `FumeExtractor::parsePrint`, `parseAms`, `parseTray` |
| Job slot list | `FumeExtractor::parseJob`, `trackJobLifecycle` |
| Decision | `FumeExtractor::evaluate` |
| Command queue priorities | `FumeExtractor::loop` (mode follow-up, console, transition, poll) |
| Success rules | `FumeExtractor::setAccepted` (codes 0/1), `onExchangeDone` |
| Config / NVS | `loadConfig`, `saveConfig`, keys `fume_*` in namespace `fume` |
| Console | `FumeExtractor::handleCommand` |
