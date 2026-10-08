# CLAUDE.md

Read [AGENTS.md](AGENTS.md) first: it is the full rebuild/port specification
(architecture, protocols, invariants, bench facts, acceptance test).

Quick rules for working in this repo:

- Never add a blocking call to the library; the purifier exchange is polled.
- Keep `onBambuReport()` allocation-free and bounded by `len`.
- Never log the purifier token beyond 4 characters; keep `config.h` out of git.
- After protocol changes run `python tools/miio.py selftest` and compare
  against the test vector in `docs/miio-protocol.md`.
- Compile both examples for at least `esp32:esp32:esp32` and
  `esp32:esp32:esp32s3` with `arduino-cli compile --library . examples/<name>`.
- Record new hardware observations, with the date, in `docs/bench-notes.md`.
