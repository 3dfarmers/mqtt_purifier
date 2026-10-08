# Bambu Lab local MQTT: what this project reads

How the library learns what the printer is doing. Facts marked **bench** were
observed on our P1S (one AMS, LAN only mode, October 2026). The rest is
community knowledge (ha-bambulab, the FarmLoop firmware) and is marked where
it is unverified.

## 1. Connection

| Item | Value |
|---|---|
| Broker | `mqtts://<printer-ip>:8883`, TLS with a self-signed certificate: skip verification (`WiFiClientSecure::setInsecure()`) |
| User / password | `bblp` / the LAN access code from the printer screen |
| Client id | anything unique, e.g. `purifier-<mac>` |
| Subscribe | `device/<SERIAL>/report` |
| Publish | `device/<SERIAL>/request` (only for `pushall`) |
| Keepalive | 60 s works |

- `<SERIAL>` is the printer serial number in **upper case**. Topics are case
  sensitive: a lower-case serial connects fine and then receives nothing.
- After subscribing, publish
  `{"pushing":{"sequence_id":"0","command":"pushall"}}`. The printer does not
  send its state on subscribe; without this, P1/A1 printers stay silent until
  something changes.
- **Client limit.** The printers accept only a few local MQTT clients, the A1
  reliably only one. Bambu Studio, Bambu Handy (LAN), Home Assistant and this
  ESP32 compete. Share one connection where you can (`onBambuReport()`).
- This project only reads plus `pushall`; Developer Mode is not needed for that
  (**bench**: P1S in LAN only mode). Sending print commands is a different story
  and out of scope.
- **Connect blocks.** The TLS handshake inside `connect()` took 1-8 s on the
  bench and blocks up to the socket timeout on a dead host. Back off (e.g. 5 s, then 60 s
  after 3 failures) so the rest of the firmware gets time.
- Return codes seen: `rc=5` not authorised (wrong access code, **or another
  device at that IP**: **bench**, the board pointed at the wrong IP got
  `rc=5`), `rc=-2` network failure.

## 2. Message size and buffers

Largest `/report` payloads measured (FarmLoop fleet logs):

| Printer | Bytes |
|---|---:|
| H2S | 18101 |
| P2S | 13735 |
| X1C | 9370 |
| P1S (one AMS, **bench**) | ~4600 |

PubSubClient **silently drops** a packet larger than its buffer
(`setBufferSize()`) and drains it byte by byte without calling your callback.
Too small = no data from big printers; too large = less heap for the TLS
handshake, which needs a ~30 KB contiguous block on reconnect. 24 KB is a good
default without PSRAM; with PSRAM you can go higher.

PubSubClient may call the callback several times inside one `loop()` when
packets queue up. Keep the callback fast; `onBambuReport()` is a single
linear walk with no copy.

## 3. Full report vs. deltas

- **X1** sends the complete `print` object every time.
- **P1, A1, P2S** send only the fields that changed after the initial
  `pushall` reply.

So: keep a merged state, update only what a message contains, and **never
treat a missing field as empty**.

## 4. Fields used

All under the top-level `"print"` object.

| Field | Type | Meaning |
|---|---|---|
| `gcode_state` | string | `IDLE`, `PREPARE`, `RUNNING`, `PAUSE`, `FINISH`, `FAILED` (also `SLICING` on X1 cloud jobs). `FAILED` covers both user cancel and errors. |
| `ams.tray_now` | string | Active slot. `"0"`..`"15"` = AMS slot (unit = n / 4, tray = n % 4), `"254"` = external spool, `"255"` = none loaded |
| `ams.ams[]` | array | One object per AMS unit, `"id"` = unit number as a string |
| `ams.ams[].tray[]` | array | One object per slot, `"id"` = `"0"`..`"3"` |
| `ams.ams[].tray[].tray_type` | string | Filament type: `PLA`, `PETG`, `ABS`, `ASA`, `PLA-CF`, `ABS-GF`, `PA6-CF`, `TPU`, ... |
| `vt_tray.tray_type` | string | External spool's filament type (used when `tray_now` = 254) |
| `command` | string | `push_status` for reports; `project_file` when the printer echoes a print job |
| `ams_mapping` | int array | In the `project_file` echo: the job's slots, see section 6 |

### Merge rules (follow them exactly)

1. Match AMS units and trays by their `"id"` field, **never by array position**:
   a delta can carry a single unit or tray.
2. A tray object that contains **only** `"id"` means that slot is **empty**
   (same rule as ha-bambulab).
3. A tray object with other fields but no `tray_type`: keep the stored type.
4. `tray_now` may arrive without the `ams.ams` array, and the other way round.
5. Filament matching: case-insensitive **prefix**, so `ABS` covers `ABS-GF`.

## 5. Samples

Reduced to the fields this library reads; real reports carry many more
(temperatures, progress, colours, humidity...). The shapes are what the parser
handles; the slot contents match our bench P1S (A1 PLA, A2 PETG, A3 PLA,
A4 PLA, external PLA). Use them as unit-test input.

Initial `pushall` reply (full state):

```json
{"print": {
  "command": "push_status", "msg": 0, "sequence_id": "1",
  "gcode_state": "IDLE",
  "ams": {
    "ams": [
      {"id": "0", "tray": [
        {"id": "0", "tray_type": "PLA"},
        {"id": "1", "tray_type": "PETG"},
        {"id": "2", "tray_type": "PLA"},
        {"id": "3", "tray_type": "PLA"}
      ]}
    ],
    "tray_now": "255"
  },
  "vt_tray": {"id": "254", "tray_type": "PLA"}
}}
```

Slot A2 changed to ASA in Bambu Studio (a delta, illustrative shape):

```json
{"print": {"command": "push_status", "msg": 1, "sequence_id": "2",
  "ams": {"ams": [{"id": "0", "tray": [{"id": "1", "tray_type": "ASA"}]}]}}}
```

The AMS loaded slot A2 (delta):

```json
{"print": {"command": "push_status", "msg": 1, "sequence_id": "3",
  "ams": {"tray_now": "1"}}}
```

Slot A3 emptied (id only):

```json
{"print": {"ams": {"ams": [{"id": "0", "tray": [{"id": "2"}]}]}}}
```

## 6. Timing: when do you know the filament? (**bench**)

P1S, one AMS, ASA in slot A2, print sent from Bambu Studio:

```
16:28:36  [FUME] Slot A2: ASA                         (slot set in Studio)
16:28:55  project_file: Cube.gcode.3mf                (job echoed)
16:29:05  gcode_state PREPARE
16:29:11  gcode_state RUNNING
16:30:49  nozzle target 260 C
16:31:29  tray_now "1"  -> active slot A2 = ASA       (2 min 18 s after RUNNING)
```

`tray_now` stays `255` through `PREPARE` and the start of `RUNNING`; the
printer heats and levels first and loads filament afterwards. A decision based
on `tray_now` alone therefore comes ~2 minutes into the print. (Physically
fine, fumes start with extrusion, but not "at print start".)

It also means `tray_now` at `PREPARE` can still name whatever filament was
left loaded from the **previous** print.

### The job's slot list (implemented, not yet confirmed on hardware)

When Bambu Studio starts a LAN print, its request carries the job's slot
assignment, and the printer echoes the request on the report topic as a
`project_file` message (**bench**: the echo exists, it carries the file URL).
Request fields as sent by the slicer:

```json
{"print": {"command": "project_file", "param": "Metadata/plate_1.gcode",
  "url": "file:///.../Cube.gcode.3mf", "use_ams": true,
  "ams_mapping": [1], "sequence_id": "..."}}
```

- `ams_mapping`: one entry per filament of the job: global tray id
  (`unit*4 + tray`), `-1` = not used, `254`/`255` = external spool.
- `ams_mapping2` (newer firmware): `[{"ams_id":0,"slot_id":1}]`.
- `use_ams: false`: the job prints from the external spool.

The library reads these if present and ORs them with the `tray_now` path.
Whether every printer model echoes `ams_mapping` is **not verified**; if
yours does, `fume status` shows `Current job uses: A2` right after the job
starts and the purifier switches on at `PREPARE`. If not, nothing is lost:
the `tray_now` path still works. Prints started from the printer screen or
SD card may not produce this echo; they use `tray_now` only.

## 7. Model notes

| Model | Notes |
|---|---|
| P1P / P1S | Deltas. **bench** (P1S): everything in this document. |
| A1 / A1 mini | Deltas. The AMS lite is expected to report like an AMS (unverified). One local MQTT client only. |
| X1C / X1E | Full reports, ~9 KB. |
| P2S | ~14 KB reports. |
| H2S / H2D | ~18 KB reports. H2D (dual nozzle) also reports per-extruder slot state; whether `tray_now` reflects the active slot there is **unverified**: use `fume always on` or `onPrintState()` if not. |
| AMS HT | Reported by the community with unit ids from 128; outside the 0-3 range this library tracks (unverified). |
