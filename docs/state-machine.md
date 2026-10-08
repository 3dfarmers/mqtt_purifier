# State machine, command scheduling and timing

Implemented in `FumeExtractor::evaluate()` (decisions) and
`FumeExtractor::loop()` / `onExchangeDone()` (delivery).

## 1. Inputs

| Input | Source | Notes |
|---|---|---|
| `state` | Bambu `print.gcode_state`, or `onPrintState()` | Words: `PREPARE`, `RUNNING`, `PAUSE` (active), `FINISH`, `FAILED`, `IDLE` (ended). Anything else, including empty, is **unknown**. |
| `activeFume` | type in `tray_now`'s slot (or `vt_tray` for 254) matches the list | Proven; arrives when the AMS loads the filament |
| `jobFume` | any slot in the job's `ams_mapping` holds a matching type | Known at job start if the printer echoes it; cleared when that job ends |
| `hostFume` | `onPrintState()`'s filament matches the list | For non-Bambu hosts |
| `always` | config | Test mode: every active print counts |

`fume = activeFume || jobFume || hostFume || always`

Matching is a case-insensitive prefix match against the comma-separated list
(default `ABS,ASA`).

## 2. States and transitions

| From | Condition | To | Purifier command |
|---|---|---|---|
| OFF | state active **and** fume | ON | power on, then mode auto |
| RUN-ON | state active **and** fume | ON | **none** (it is still running); timer cancelled |
| ON | state ended | RUN-ON | none; run-on timer starts |
| RUN-ON | timer >= run-on minutes | OFF | power off |
| any | state unknown (link lost, boot) | unchanged | none |
| ON | `PAUSE` | ON | none |
| any | `fume disable` | OFF | none: the purifier is left as it is |

Notes:

- `run-on = 0` goes ON -> RUN-ON -> OFF in one evaluation.
- A non-fume print during RUN-ON does not cancel the timer: the purifier goes
  off on schedule. If that print later switches to a fume slot, ON again.
- Once ON, it stays ON until the print ends, even if the job moves on to a
  non-fume slot (multi-material).
- `evaluate()` runs after every printer update and on every `loop()`, so the
  timer needs no separate tick.

## 3. Command delivery

Exactly one miIO exchange at a time. When the client is idle, `loop()` picks
the next one in this priority order:

1. **Mode follow-up**: after a confirmed power ON, set mode auto. Skipped if
   an OFF is already waiting.
2. **Console command**: `fume on/off/status/info`. Not retried.
3. **Transition command** (`want`): the latest ON or OFF from the state
   machine; a new transition replaces an older undelivered one.
4. **Status poll**: power, mode, PM2.5 every 10 s while ON or RUN-ON.

### Success and failure

| Reply | Meaning | Action |
|---|---|---|
| every result `code` is 0 or 1 | accepted | mark confirmed; for ON queue the mode follow-up |
| a negative `code` | refused (e.g. wrong property ids) | log `REFUSED` with the codes; retry as a failure |
| `error` object | refused | fail the round |
| no reply after 4 attempts | unreachable | fail the round |
| `sendto` failed | station link down | fail the round at once |

### Retry pacing for transition commands

| Round | Wait before it |
|---|---|
| 1 | none |
| 2 | 2 s (a cold first exchange often fails while the next succeeds) |
| 3..20 | 30 s |
| after 20 | give up; log it; the next print transition tries again |

A successful status poll while a command waits cancels the wait: the device
is reachable, send now.

### Respecting manual use

`confirmed` holds the power state of the last command the purifier
accepted. If a later poll reads a different power state, someone used the
button or the app: log `Purifier switched off by hand - leaving it`, adopt
the new state, send nothing. No command is ever repeated after confirmation.

## 4. Timing constants

| Constant | Value | Where |
|---|---|---|
| Reply timeout | 2 s | `MiioClient::TIMEOUT_MS` |
| Retries per exchange | 3 (4 attempts) | `MiioClient::RETRIES` |
| AQI poll | 10 s | `FumeExtractor::POLL_MS` |
| First retry | 2 s | `FIRST_RETRY_MS` |
| Later retries | 30 s | `RETRY_MS` |
| Rounds before giving up | 20 (~10 min) | `MAX_ROUNDS` |
| Run-on | 10 min default, 0-240 | config `fume_runon_min` |

## 5. Job slot list lifecycle

1. A `project_file` echo arrives: store its slots, mark the job "not started".
2. The state becomes active: the job is running.
3. The state becomes ended **after** having been active: clear the slots.

Clearing only after the job ran matters: the echo arrives while `gcode_state`
still says `FINISH` from the previous print.

## 6. Log lines (all prefixed `[FUME]`)

| Line | When |
|---|---|
| `RUNNING with ASA (slot A2) -> purifier ON, auto mode` | OFF -> ON; the source is `slot X`, `job`, `host` or `test mode` |
| `PREPARE with ABS (job) - run-on cancelled, purifier stays ON` | RUN-ON -> ON |
| `Print FINISH -> purifier OFF in 10 min` | ON -> RUN-ON |
| `Run-on over -> purifier OFF` | RUN-ON -> OFF |
| `Purifier ON confirmed (408 ms; codes power 0)` | power command accepted |
| `Auto mode set (codes mode 0)` | mode follow-up accepted |
| `01:02:03 AQI 12 ug/m3 \| power on \| auto \| ON 00:03:10 \| RUNNING` | each poll; uptime timestamp |
| `Purifier not answering (1 polls): no reply to hello (4 tries)` | first failed poll, then every 10th |
| `Retrying in 30 s (round 3 of 20)` | transition command failed |
| `Slot A2: ASA`, `Active slot: A2 = ASA`, `New job uses: A2=ASA` | printer view changes (only while enabled) |
