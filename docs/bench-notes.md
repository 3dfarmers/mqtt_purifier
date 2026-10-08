# Bench notes

What was measured on real hardware, so later changes can be checked against
it. Newest first. Times are local log timestamps.

## 2026-10-08 - first bench run

**Setup**

| Part | Detail |
|---|---|
| ESP32 | FarmBoard 2 (ESP32-S3, no PSRAM), Arduino-ESP32 3.3.8, FarmLoop firmware with this code embedded (shared MQTT connection) |
| Printer | Bambu Lab P1S, one AMS, LAN only mode |
| Purifier | Xiaomi Mi Air Purifier 3C, `zhimi.airp.mb4a`, firmware 2.1.7, hardware `esp32` |
| Network | 2.4 GHz mesh WiFi, all devices in one subnet |

**miIO from a laptop (Python, wired-quality WiFi)**

- Hello: answered with the device id and uptime stamp; the reply's checksum field is
  all `ff` (token not exposed).
- First test: hellos 1 and 2 timed out, the 3rd answered (~200 ms). Later
  tests: every hello answered in 16-202 ms.

**miIO from the ESP32**

| Exchange | Result | Duration |
|---|---|---|
| `miIO.info` | model `zhimi.airp.mb4a`, fw `2.1.7` | 378-1697 ms |
| `get_properties` power/mode/PM2.5 | e.g. on / auto / 4 ug/m3 | 349-920 ms |
| `set_properties` power + mode **in one request**, purifier off | `power 0, mode -4004`: on, mode refused | ~400 ms |
| `set_properties` power, then mode separately | `power 0`, then `mode 0` | 408-1118 ms, then ~500 ms |
| `set_properties` power off | `power 0` | 407 ms |

**Weak WiFi (before moving the board)**

- ESP32 RSSI -85 to -90 dBm. Ping from a laptop: 35% loss to the ESP32, 0% to
  the purifier.
- WiFi join at boot (20 s timeout) failed on 4 of 7 boots; the background
  reconnect then took 66-73 s.
- MQTT stayed usable (TCP retransmits) but reconnected now and then; one
  blocked TLS connect to a wrong printer IP held the loop for ~2 min.
- About half of all miIO exchanges failed completely (`no reply to hello
  (4 tries)` or `no reply to command`), in a pattern of a failed exchange
  followed by a working one.

**After moving the board closer**

- RSSI still reported -83 dBm, but ping loss 0% (10/10). Every exchange of the
  test sequence (info, on, mode, status, off, status) succeeded at the first
  attempt. MQTT still reconnected once during a print.
- Takeaway: log RSSI and ping loss; aim for better than -70 dBm as a margin.

**Print test (ASA in slot A2)**

```
16:28:36  [FUME] Slot A2: ASA                     slot changed in Bambu Studio
16:28:55  [PRINTER] project_file: Cube.gcode.3mf  the printer echoed the job
16:29:05  [PRINTER] State: PREPARE
16:29:11  [PRINTER] State: RUNNING
16:30:49  [PRINTER] Nozzle target: 260
16:30:56  [MQTT] Connecting ... (link had dropped; reconnected 16:31:03, pushall)
16:31:29  [FUME] Active slot: A2 = ASA
16:31:29  [FUME] RUNNING with ASA (slot A2) -> purifier ON, auto mode
16:31:29  [FUME] Purifier ON confirmed (408 ms; codes power 0)
16:31:29  [FUME] Auto mode set (codes mode 0)
16:31:30  [FUME] 00:19:14 AQI 2 ug/m3 | power on | auto | ON 00:00:01 | RUNNING
```

- `tray_now` stayed `255` until the AMS loaded the ASA, 2 min 18 s after
  `RUNNING` (the full state after the 16:31:03 reconnect still had no active
  slot). The purifier followed within a second.
- This run predates the `ams_mapping` (job slot list) code, so whether the P1S
  echo carries it is still open.

**Not yet observed on hardware**

- End of a print in automatic mode: run-on, OFF, run-on cancelled by a new
  fume print, purifier unplugged during a print.
- The job slot list path (`New job uses: ...`).
- Any printer other than the P1S; any purifier other than the 3C.

**Other findings**

- Opening the USB-Serial-JTAG port from Windows reset the ESP32-S3 once
  (`rst:0x15 USB_UART_CHIP_RESET`), and the COM port disappears while the chip
  resets: bench tools must reopen it.
- Cost inside the FarmLoop firmware: +17 KB flash, about 3 KB static RAM
  (estimated from the buffers). Largest
  free heap block during operation 34,804 bytes (64 KB log ring and 24 KB MQTT
  buffer in that firmware); the printer's TLS handshake kept working.
