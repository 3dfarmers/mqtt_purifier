# Changelog

## 1.0.0 - 2026-10-08

First public release, extracted from the FarmLoop FarmBoard firmware
(`FL_S2_FB2_U_1.1-beta`) where it was bench-tested.

- `FumeExtractor`: merged Bambu report view (gcode_state, AMS slots, external
  spool, active slot, job slot list), state machine OFF / ON / RUN-ON,
  transition-only commands, manual-use respect, paced retries, NVS config,
  `fume ...` console, PM2.5 logging, `status()` snapshot.
- `MiioClient`: non-blocking Xiaomi miIO client, in-place AES-128-CBC, fresh
  hello per attempt, random request ids, checksum verified before decrypting.
- `MiniJson`: bounded, allocation-free JSON walker for unterminated buffers.
- Generic `onPrintState()` input for non-Bambu printers; configurable MIoT
  property map for other purifiers; compile-time defaults.
- Examples: `BambuPurifier` (complete firmware), `PurifierBench` (purifier
  console + print simulator).
- `tools/miio.py`: zero-dependency desk tool, AES self-test, protocol test vector.
- Bench findings built in: ON is two requests (mode refused with -4004 when
  sent together with power to an idle 3C); MIoT code 1 counts as success;
  quick first retry; WiFi modem sleep off in the examples.
- Job slot list (`ams_mapping` from the `project_file` echo) is new in this
  release and not yet confirmed on hardware; it is ORed with the proven
  active-slot path.
