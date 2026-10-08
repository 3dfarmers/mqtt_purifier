// FumeExtractor - see FumeExtractor.h for what it does and how a host wires it in.
// Part of MqttPurifier. Design notes: docs/state-machine.md, docs/bambu-mqtt.md.
#include "FumeExtractor.h"
#include <Preferences.h>
#include "MiniJson.h"

FumeExtractor fume;

static bool isActiveState(const char* s) {   // a print is under way
  return !strcmp(s, "PREPARE") || !strcmp(s, "RUNNING") || !strcmp(s, "PAUSE");
}
static bool isEndState(const char* s) {      // finished, failed / cancelled, idle
  return !strcmp(s, "FINISH") || !strcmp(s, "FAILED") || !strcmp(s, "IDLE");
}

// Slot names as Bambu Studio shows them: A1..D4, Ext for the external spool.
static String slotLabel(long slot) {
  if (slot == 16 || slot == 254) return "Ext";
  if (slot == 255) return "none";
  if (slot < 0 || slot > 15) return "?";
  char b[3] = { (char)('A' + slot / 4), (char)('1' + slot % 4), 0 };
  return String(b);
}

// -----------------------------------------------------------------------------
// Setup and config (NVS namespace "fume")
// -----------------------------------------------------------------------------
void FumeExtractor::setMiotMap(const MiotMap& m) {
  _map = m;
  buildParams();
}

// Request params for the configured property ids. ON is two requests, power
// then mode: sent together to a purifier that is off, the 3C (mb4a, fw 2.1.7)
// powered on but refused the mode write with code -4004.
void FumeExtractor::buildParams() {
  snprintf(_pOn, sizeof(_pOn), "[{\"did\":\"power\",\"siid\":%u,\"piid\":%u,\"value\":true}]",
           _map.powerSiid, _map.powerPiid);
  snprintf(_pOff, sizeof(_pOff), "[{\"did\":\"power\",\"siid\":%u,\"piid\":%u,\"value\":false}]",
           _map.powerSiid, _map.powerPiid);
  snprintf(_pAuto, sizeof(_pAuto), "[{\"did\":\"mode\",\"siid\":%u,\"piid\":%u,\"value\":%d}]",
           _map.modeSiid, _map.modePiid, _map.autoMode);
  snprintf(_pStatus, sizeof(_pStatus),
           "[{\"did\":\"power\",\"siid\":%u,\"piid\":%u},"
           "{\"did\":\"mode\",\"siid\":%u,\"piid\":%u},"
           "{\"did\":\"aqi\",\"siid\":%u,\"piid\":%u}]",
           _map.powerSiid, _map.powerPiid, _map.modeSiid, _map.modePiid,
           _map.aqiSiid, _map.aqiPiid);
}

void FumeExtractor::begin(Print& log, const FumeDefaults* defaults) {
  _log = &log;
  buildParams();
  loadConfig(defaults);
  applyConnection();
  _log->printf("[FUME] Purifier automation %s\n",
               _cfg.enabled ? "ENABLED" : "off (type `fume help`)");
  if (_cfg.enabled || _cfg.ip[0])
    _log->printf("[FUME] Purifier %s (token %s) | filaments %s%s | run-on %u min\n",
                 _cfg.ip[0] ? _cfg.ip : "[no IP]", tokenHint().c_str(), _cfg.types,
                 _cfg.always ? " (ALWAYS: every print)" : "", _cfg.runonMin);
}

// Compile-time defaults first, then whatever NVS holds on top of them.
void FumeExtractor::loadConfig(const FumeDefaults* d) {
  if (d) {
    if (d->ip && d->ip[0])       strlcpy(_cfg.ip, d->ip, sizeof(_cfg.ip));
    if (d->token && d->token[0]) strlcpy(_cfg.token, d->token, sizeof(_cfg.token));
    if (d->types && d->types[0]) strlcpy(_cfg.types, d->types, sizeof(_cfg.types));
    if (d->runonMin >= 0 && d->runonMin <= 240) _cfg.runonMin = (uint16_t)d->runonMin;
    if (d->enabled >= 0) _cfg.enabled = d->enabled != 0;
    if (d->always >= 0)  _cfg.always  = d->always != 0;
  }
  Preferences p;
  if (!p.begin("fume", true)) return;   // namespace absent until the first save
  if (p.isKey("fume_enabled"))   _cfg.enabled = p.getBool("fume_enabled", _cfg.enabled);
  if (p.isKey("fume_ip"))        p.getString("fume_ip", _cfg.ip, sizeof(_cfg.ip));
  if (p.isKey("fume_token"))     p.getString("fume_token", _cfg.token, sizeof(_cfg.token));
  if (p.isKey("fume_types"))     p.getString("fume_types", _cfg.types, sizeof(_cfg.types));
  if (p.isKey("fume_runon_min")) _cfg.runonMin = p.getUShort("fume_runon_min", _cfg.runonMin);
  if (p.isKey("fume_always"))    _cfg.always = p.getBool("fume_always", _cfg.always);
  p.end();
}

void FumeExtractor::saveConfig() {
  Preferences p;
  if (!p.begin("fume", false)) return;
  p.putBool("fume_enabled", _cfg.enabled);
  p.putString("fume_ip", _cfg.ip);
  p.putString("fume_token", _cfg.token);
  p.putString("fume_types", _cfg.types);
  p.putUShort("fume_runon_min", _cfg.runonMin);
  p.putBool("fume_always", _cfg.always);
  p.end();
}

void FumeExtractor::applyConnection() {
  IPAddress ip;
  _miio.setIp((_cfg.ip[0] && ip.fromString(_cfg.ip)) ? ip : IPAddress());
  if (_cfg.token[0] && !_miio.setToken(_cfg.token) && _log)
    _log->println("[FUME] Stored token is not 32 hex characters - ignored");
}

// Never log the token: first 4 characters only.
String FumeExtractor::tokenHint() const {
  if (!_cfg.token[0]) return "[none]";
  return String(_cfg.token).substring(0, 4) + "...";
}

String FumeExtractor::redact(const String& line) {
  String l = line;
  l.trim();
  if (l.length() < 10 || !l.substring(0, 10).equalsIgnoreCase("fume token")) return line;
  String tok = l.substring(10);
  tok.trim();
  if (tok.length() <= 4) return line;
  return l.substring(0, 10) + " " + tok.substring(0, 4) + "...";
}

// -----------------------------------------------------------------------------
// Printer side: merged view of the Bambu /report stream
// -----------------------------------------------------------------------------
// Called with EVERY report. P1, A1 and P2S send only the fields that changed,
// so the AMS picture is merged here and a field missing from a packet keeps
// its last value. X1 resends everything, which the merge handles the same way.
void FumeExtractor::onBambuReport(const char* json, size_t len) {
  if (!json || len < 2) return;
  const char* e = json + len;
  mjson::eachMember(json, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    if (!mjson::keyIs(k, kn, "print")) return true;
    parsePrint(v, ve);
    return false;
  });
  evaluate();
}

void FumeExtractor::onPrintState(const char* state, const char* filamentType) {
  if (state && state[0]) {
    size_t i = 0;
    for (; state[i] && i + 1 < sizeof(_gcode); i++) _gcode[i] = (char)toupper((unsigned char)state[i]);
    _gcode[i] = 0;
  }
  strlcpy(_hostType, filamentType ? filamentType : "", sizeof(_hostType));
  trackJobLifecycle();
  evaluate();
}

void FumeExtractor::parsePrint(const char* p, const char* e) {
  char cmd[24] = "";
  const char *map = nullptr, *mapEnd = nullptr, *map2 = nullptr, *map2End = nullptr;
  int useAms = -1;
  mjson::eachMember(p, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    if (mjson::keyIs(k, kn, "gcode_state")) {
      char s[sizeof(_gcode)];
      if (mjson::readStr(v, ve, s, sizeof(s)) && s[0]) strlcpy(_gcode, s, sizeof(_gcode));
    } else if (mjson::keyIs(k, kn, "ams")) {
      parseAms(v, ve);
    } else if (mjson::keyIs(k, kn, "vt_tray")) {
      parseTray(v, ve, -1);
    } else if (mjson::keyIs(k, kn, "command")) {
      mjson::readStr(v, ve, cmd, sizeof(cmd));
    } else if (mjson::keyIs(k, kn, "ams_mapping")) {
      map = v; mapEnd = ve;
    } else if (mjson::keyIs(k, kn, "ams_mapping2")) {
      map2 = v; map2End = ve;
    } else if (mjson::keyIs(k, kn, "use_ams")) {
      bool b;
      if (mjson::readBool(v, ve, &b)) useAms = b ? 1 : 0;
    }
    return true;
  });
  // The printer echoes the print command that started a job. When it carries
  // the slot list, the purifier can switch on at job start, long before the
  // AMS loads the filament.
  if (!strcmp(cmd, "project_file")) parseJob(map, mapEnd, map2, map2End, useAms);
  trackJobLifecycle();
}

//   "ams": { "ams": [ { "id":"0", "tray":[ {"id":"0","tray_type":"PLA",...},
//                                           {"id":"1"},   <- id only = empty
//                                           ... ] }, ... ],
//            "tray_now":"1", ... }
// Units and trays are matched by their "id", never by array position, because
// a delta may carry only the unit or tray that changed.
void FumeExtractor::parseAms(const char* p, const char* e) {
  long trayNow = _trayNow;
  mjson::eachMember(p, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    if (mjson::keyIs(k, kn, "tray_now")) {
      mjson::readInt(v, ve, &trayNow);
    } else if (mjson::keyIs(k, kn, "ams")) {
      mjson::eachElement(v, ve, [&](const char* u, const char* ue) {
        long unit = -1;
        const char* trays = nullptr;
        const char* traysEnd = nullptr;
        mjson::eachMember(u, ue, [&](const char* k2, size_t kn2, const char* v2, const char* ve2) {
          if (mjson::keyIs(k2, kn2, "id")) mjson::readInt(v2, ve2, &unit);
          else if (mjson::keyIs(k2, kn2, "tray")) { trays = v2; traysEnd = ve2; }
          return true;
        });
        if (unit >= 0 && unit < SLOTS / 4 && trays) {
          mjson::eachElement(trays, traysEnd, [&](const char* t, const char* te) {
            parseTray(t, te, (int)unit);
            return true;
          });
        }
        return true;
      });
    }
    return true;
  });
  // Applied after the whole block so the log names the slot's NEW type.
  if (trayNow != _trayNow) {
    _trayNow = trayNow;
    if (_cfg.enabled && _log) {
      const char* t = activeType();
      _log->printf("[FUME] Active slot: %s%s%s\n", slotLabel(_trayNow).c_str(),
                   t[0] ? " = " : "", t);
    }
  }
}

// unit -1 = vt_tray (external spool).
void FumeExtractor::parseTray(const char* p, const char* e, int unit) {
  long id = -1;
  int members = 0;
  char type[TYPE_LEN] = "";
  bool haveType = false;
  mjson::eachMember(p, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    members++;
    if (mjson::keyIs(k, kn, "id")) mjson::readInt(v, ve, &id);
    else if (mjson::keyIs(k, kn, "tray_type")) haveType = mjson::readStr(v, ve, type, sizeof(type));
    return true;
  });

  int slot;
  if (unit < 0) {
    slot = EXT;
    if (!haveType) return;
  } else {
    if (id < 0 || id > 3) return;
    slot = unit * 4 + (int)id;
    if (!haveType) {
      if (members != 1) return;   // tray_type not in this delta: keep ours
      type[0] = 0;                // a tray object holding ONLY its id is an empty slot
    }
  }
  if (_known[slot] && strcmp(_type[slot], type) == 0) return;
  strlcpy(_type[slot], type, TYPE_LEN);
  _known[slot] = true;
  if (_cfg.enabled && _log)
    _log->printf("[FUME] Slot %s: %s\n", slotLabel(slot).c_str(), type[0] ? type : "empty");
}

// The job's slot list, from the echoed print command:
//   "ams_mapping":  [-1, 1]   one entry per filament of the job: the global
//                             tray id (unit*4 + tray), -1 = not used,
//                             254/255 = external spool
//   "ams_mapping2": [{"ams_id":0,"slot_id":1}]   newer firmware
//   "use_ams": false          the job prints from the external spool
// Not yet confirmed on hardware that every model echoes these; without them
// the decision falls back to the active slot (tray_now), which is proven.
void FumeExtractor::parseJob(const char* map, const char* mapEnd, const char* map2,
                             const char* map2End, int useAms) {
  uint32_t mask = 0;
  if (map2) {
    mjson::eachElement(map2, map2End, [&](const char* o, const char* oe) {
      long ams = -1, slot = -1;
      mjson::eachMember(o, oe, [&](const char* k, size_t kn, const char* v, const char* ve) {
        if (mjson::keyIs(k, kn, "ams_id")) mjson::readInt(v, ve, &ams);
        else if (mjson::keyIs(k, kn, "slot_id")) mjson::readInt(v, ve, &slot);
        return true;
      });
      if (ams >= 0 && ams < SLOTS / 4 && slot >= 0 && slot < 4) mask |= 1UL << (ams * 4 + slot);
      else if (ams == 254 || ams == 255) mask |= 1UL << EXT;
      return true;
    });
  }
  if (!mask && map) {
    mjson::eachElement(map, mapEnd, [&](const char* o, const char* oe) {
      long v;
      if (mjson::readInt(o, oe, &v)) {
        if (v >= 0 && v < SLOTS) mask |= 1UL << v;
        else if (v == 254 || v == 255) mask |= 1UL << EXT;
      }
      return true;
    });
  }
  if (useAms == 0) mask = 1UL << EXT;
  _jobMask = mask;
  _jobRunning = false;

  if (!_cfg.enabled || !_log) return;
  if (!mask) {
    _log->println("[FUME] New job, no slot list in the print command - deciding by the active slot");
    return;
  }
  String s;
  for (int i = 0; i <= EXT; i++) {
    if (!((mask >> i) & 1)) continue;
    s += " " + slotLabel(i) + "=" + (_known[i] ? (_type[i][0] ? _type[i] : "empty") : "?");
  }
  _log->printf("[FUME] New job uses:%s\n", s.c_str());
}

// The job's slot list belongs to one print: drop it when that print ends.
void FumeExtractor::trackJobLifecycle() {
  if (isActiveState(_gcode)) {
    _jobRunning = true;
  } else if (isEndState(_gcode) && _jobRunning) {
    _jobRunning = false;
    _jobMask = 0;
  }
}

const char* FumeExtractor::activeType() const {
  long slot = (_trayNow == 254) ? EXT : _trayNow;
  if (slot < 0 || slot > EXT || !_known[slot]) return "";
  return _type[slot];
}

const char* FumeExtractor::jobFumeType() const {
  for (int i = 0; i <= EXT; i++)
    if (((_jobMask >> i) & 1) && _known[i] && isFumeType(_type[i])) return _type[i];
  return nullptr;
}

// Case-insensitive PREFIX match against the configured list, so "ABS" also
// covers "ABS-GF" and "ASA" covers "ASA-Aero".
bool FumeExtractor::isFumeType(const char* type) const {
  if (!type || !type[0]) return false;
  const char* p = _cfg.types;
  while (*p) {
    while (*p == ',' || *p == ' ') p++;
    const char* s = p;
    while (*p && *p != ',') p++;
    const char* t = p;
    while (t > s && t[-1] == ' ') t--;
    size_t n = (size_t)(t - s);
    if (n && strncasecmp(type, s, n) == 0) return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// State machine. Runs after every printer update and on every loop() (timer).
// -----------------------------------------------------------------------------
void FumeExtractor::evaluate() {
  if (!_cfg.enabled || !_log) return;
  uint32_t now = millis();

  // Three sources, any one is enough: the active slot (proven on a P1S, but
  // the printer only reports it once the filament is loaded, ~2 min into the
  // print), the job's slot list (known at job start) and a host feed.
  const char* active = activeType();
  bool activeFume    = isFumeType(active);
  const char* job    = jobFumeType();
  bool hostFume      = isFumeType(_hostType);
  const char* why    = activeFume ? active : job ? job : hostFume ? _hostType : nullptr;

  // OFF / RUNON -> ON. Also reached mid-print when a multi-material job
  // switches to a fume slot; from then on it stays ON until the print ends.
  if (_state != S_ON && isActiveState(_gcode) && (why || _cfg.always)) {
    if (why) {
      snprintf(_trigger, sizeof(_trigger), "%s", why);
    } else {
      strlcpy(_trigger, "any (always)", sizeof(_trigger));
    }
    String src;
    if (activeFume)  src = "slot " + slotLabel(_trayNow);
    else if (job)    src = "job";
    else if (why)    src = "host";
    else             src = "test mode";
    if (_state == S_RUNON) {
      _log->printf("[FUME] %s with %s (%s) - run-on cancelled, purifier stays ON\n",
                   _gcode, _trigger, src.c_str());
    } else {
      _log->printf("[FUME] %s with %s (%s) -> purifier ON, auto mode\n",
                   _gcode, _trigger, src.c_str());
      request(C_POWER_ON);
    }
    _state    = S_ON;
    _since    = now;
    _lastPoll = now - POLL_MS;   // first AQI reading right after the ON command
    return;
  }

  // ON -> RUNON. PAUSE keeps it on; an unknown state (link down) holds.
  if (_state == S_ON && isEndState(_gcode)) {
    _state = S_RUNON;
    _since = now;
    _log->printf("[FUME] Print %s -> purifier OFF in %u min\n", _gcode, _cfg.runonMin);
  }

  // RUNON -> OFF
  if (_state == S_RUNON && now - _since >= (uint32_t)_cfg.runonMin * 60000UL) {
    _state = S_OFF;
    _since = now;
    _log->println("[FUME] Run-on over -> purifier OFF");
    request(C_POWER_OFF);
  }
}

// A transition command. Replaces any older one still waiting.
void FumeExtractor::request(Cmd c) {
  _want       = c;
  _wantRounds = 0;
  _wantAt     = millis();
  _wantDelay  = 0;
}

// -----------------------------------------------------------------------------
// Purifier side
// -----------------------------------------------------------------------------
void FumeExtractor::loop() {
  if (!_log) return;   // begin() not called yet
  evaluate();

  MiioClient::Result r = _miio.poll();
  if (r == MiioClient::OK || r == MiioClient::FAILED) onExchangeDone(r == MiioClient::OK);
  if (_miio.busy()) return;

  // One exchange at a time: the mode half of an ON, a console command, the
  // transition command, then the AQI poll.
  uint32_t now = millis();
  if (_follow != C_NONE) {
    Cmd c = _follow;
    _follow = C_NONE;
    if (_want != C_POWER_OFF) {   // an OFF already waiting makes the mode moot
      startExchange(c, _followManual);
      return;
    }
  }
  if (_manual != C_NONE) {
    Cmd c = _manual;
    _manual = C_NONE;
    startExchange(c, true);
    return;
  }
  if (!_cfg.enabled) return;
  if (_want != C_NONE && now - _wantAt >= _wantDelay) {
    startExchange(_want, false);
    return;
  }
  if (_state != S_OFF && now - _lastPoll >= POLL_MS) {
    _lastPoll = now;
    startExchange(C_STATUS, false);
  }
}

void FumeExtractor::startExchange(Cmd c, bool manual) {
  const char* method = (c == C_STATUS) ? "get_properties"
                     : (c == C_INFO)   ? "miIO.info" : "set_properties";
  const char* params = (c == C_POWER_ON)  ? _pOn
                     : (c == C_MODE_AUTO) ? _pAuto
                     : (c == C_POWER_OFF) ? _pOff
                     : (c == C_INFO)      ? "[]" : _pStatus;
  _inflight = c;
  _inflightManual = manual;
  if (!_miio.request(method, params)) onExchangeDone(false);   // error() says why
}

void FumeExtractor::onExchangeDone(bool ok) {
  Cmd c = _inflight;
  bool manual = _inflightManual;
  _inflight = C_NONE;
  _inflightManual = false;
  if (c == C_NONE) return;

  if (c == C_INFO) {
    if (ok) printInfo(_miio.reply());
    else    _log->printf("[FUME] Info failed: %s\n", _miio.error());
    return;
  }

  // Second half of ON. The purifier is already running, so a refusal here
  // only means it stays in its current mode: logged, not retried.
  if (c == C_MODE_AUTO) {
    char codes[48] = "";
    if (ok && setAccepted(_miio.reply(), codes, sizeof(codes))) {
      _mode = _map.autoMode;
      _log->printf("[FUME] Auto mode set (codes %s)\n", codes);
    } else {
      _log->printf("[FUME] Auto mode not set, purifier keeps its mode: %s%s\n",
                   ok ? "codes " : "", ok ? codes : _miio.error());
    }
    return;
  }

  if (c == C_STATUS) {
    if (ok && parseStatus(_miio.reply())) {
      if (_pollFails) _log->println("[FUME] Purifier answering again");
      _pollFails = 0;
      if (manual) {
        _log->printf("[FUME] Purifier: power %s | mode %s | AQI %d ug/m3 | device id %lu | %lu ms\n",
                     _power == 1 ? "on" : _power == 0 ? "off" : "?", modeName(_mode), _aqi,
                     (unsigned long)_miio.deviceId(), (unsigned long)_miio.lastMs());
      } else {
        logAqi();
      }
      // Reachable again: deliver a command that is waiting out its retry pause.
      if (_want != C_NONE) _wantDelay = 0;
      // Someone used the purifier's own button or app: note it, do not undo it.
      if (_confirmed >= 0 && _power >= 0 && _power != _confirmed) {
        _log->printf("[FUME] Purifier switched %s by hand - leaving it\n", _power ? "on" : "off");
        _confirmed = _power;
      }
    } else if (manual) {
      _log->printf("[FUME] Status failed: %s\n", ok ? "unexpected reply" : _miio.error());
    } else if (++_pollFails == 1 || _pollFails % 10 == 0) {
      _log->printf("[FUME] Purifier not answering (%u polls): %s\n", _pollFails,
                   ok ? "unexpected reply" : _miio.error());
    }
    return;
  }

  bool on = (c == C_POWER_ON);
  char codes[48] = "";
  if (ok && setAccepted(_miio.reply(), codes, sizeof(codes))) {
    _confirmed = on ? 1 : 0;
    _power = _confirmed;
    _log->printf("[FUME] Purifier %s confirmed (%lu ms; codes %s)\n", on ? "ON" : "OFF",
                 (unsigned long)_miio.lastMs(), codes);
    if (on) { _follow = C_MODE_AUTO; _followManual = manual; }
    if (!manual && _want == c) _want = C_NONE;
    return;
  }

  if (ok) _log->printf("[FUME] Purifier %s REFUSED: codes %s\n", on ? "ON" : "OFF", codes);
  else    _log->printf("[FUME] Purifier %s FAILED: %s\n", on ? "ON" : "OFF", _miio.error());
  if (manual || _want != c) return;
  if (++_wantRounds >= MAX_ROUNDS) {
    _log->printf("[FUME] Giving up on %s after %u rounds; the next print event tries again\n",
                 on ? "ON" : "OFF", _wantRounds);
    _want = C_NONE;
  } else {
    _wantAt    = millis();
    _wantDelay = (_wantRounds == 1) ? FIRST_RETRY_MS : RETRY_MS;
    _log->printf("[FUME] Retrying in %lu s (round %u of %u)\n",
                 (unsigned long)(_wantDelay / 1000), _wantRounds + 1, MAX_ROUNDS);
  }
}

// {"id":7,"result":[{"did":"power","siid":2,"piid":1,"code":0,"value":true},...]}
// Matched on siid/piid; "did" is only an echo of what we sent.
bool FumeExtractor::parseStatus(const char* json) {
  const char* e = json + strlen(json);
  bool any = false;
  mjson::eachMember(json, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    if (!mjson::keyIs(k, kn, "result")) return true;
    mjson::eachElement(v, ve, [&](const char* o, const char* oe) {
      long siid = -1, piid = -1, code = -1;
      const char* val = nullptr;
      const char* valEnd = nullptr;
      mjson::eachMember(o, oe, [&](const char* k2, size_t kn2, const char* v2, const char* ve2) {
        if (mjson::keyIs(k2, kn2, "siid")) mjson::readInt(v2, ve2, &siid);
        else if (mjson::keyIs(k2, kn2, "piid")) mjson::readInt(v2, ve2, &piid);
        else if (mjson::keyIs(k2, kn2, "code")) mjson::readInt(v2, ve2, &code);
        else if (mjson::keyIs(k2, kn2, "value")) { val = v2; valEnd = ve2; }
        return true;
      });
      if (code != 0 || !val) return true;
      if (siid == _map.powerSiid && piid == _map.powerPiid) {
        bool b;
        if (mjson::readBool(val, valEnd, &b)) { _power = b ? 1 : 0; any = true; }
      } else if (siid == _map.modeSiid && piid == _map.modePiid) {
        long m;
        if (mjson::readInt(val, valEnd, &m)) { _mode = (int)m; any = true; }
      } else if (siid == _map.aqiSiid && piid == _map.aqiPiid) {
        long a;
        if (mjson::readInt(val, valEnd, &a)) { _aqi = (int)a; any = true; }
      }
      return true;
    });
    return false;
  });
  if (any) _statusAt = millis();
  return any;
}

// miIO.info: {"id":3,"result":{"model":"zhimi.airp.mb4a","fw_ver":"...",...}}
// Prints only what identifies the device. The result also carries network
// details and, on some firmware, the token itself, so nothing else is shown.
void FumeExtractor::printInfo(const char* json) {
  const char* e = json + strlen(json);
  char model[40] = "?", fw[24] = "?", hw[24] = "?";
  mjson::eachMember(json, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    if (!mjson::keyIs(k, kn, "result")) return true;
    mjson::eachMember(v, ve, [&](const char* k2, size_t kn2, const char* v2, const char* ve2) {
      if (mjson::keyIs(k2, kn2, "model"))       mjson::readStr(v2, ve2, model, sizeof(model));
      else if (mjson::keyIs(k2, kn2, "fw_ver")) mjson::readStr(v2, ve2, fw, sizeof(fw));
      else if (mjson::keyIs(k2, kn2, "hw_ver")) mjson::readStr(v2, ve2, hw, sizeof(hw));
      return true;
    });
    return false;
  });
  _log->printf("[FUME] Purifier model %s | firmware %s | hardware %s | device id %lu | %lu ms\n",
               model, fw, hw, (unsigned long)_miio.deviceId(), (unsigned long)_miio.lastMs());
}

// set_properties: each property answers with a MIoT code. 0 = done,
// 1 = accepted and still completing, negative = refused. `codes` gets
// "power 0, mode 0" for the log.
bool FumeExtractor::setAccepted(const char* json, char* codes, size_t n) {
  const char* e = json + strlen(json);
  bool seen = false, good = true;
  size_t used = 0;
  codes[0] = 0;
  mjson::eachMember(json, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    if (!mjson::keyIs(k, kn, "result")) return true;
    mjson::eachElement(v, ve, [&](const char* o, const char* oe) {
      long code = -9999;
      char did[12] = "?";
      mjson::eachMember(o, oe, [&](const char* k2, size_t kn2, const char* v2, const char* ve2) {
        if (mjson::keyIs(k2, kn2, "code")) mjson::readInt(v2, ve2, &code);
        else if (mjson::keyIs(k2, kn2, "did")) mjson::readStr(v2, ve2, did, sizeof(did));
        return true;
      });
      if (used < n) used += snprintf(codes + used, n - used, "%s%s %ld", seen ? ", " : "", did, code);
      seen = true;
      if (code != 0 && code != 1) good = false;
      return true;
    });
    return false;
  });
  if (!seen) snprintf(codes, n, "none in reply");
  return seen && good;
}

// -----------------------------------------------------------------------------
// Status and logging
// -----------------------------------------------------------------------------
FumeStatus FumeExtractor::status() const {
  FumeStatus s;
  uint32_t now = millis();
  s.state        = stateName(_state);
  s.enabled      = _cfg.enabled;
  s.power        = _power;
  s.mode         = _mode;
  s.aqi          = _aqi;
  s.readingAgeMs = _statusAt ? now - _statusAt : UINT32_MAX;
  s.printerState = _gcode;
  s.activeType   = activeType();
  s.trigger      = (_state == S_OFF) ? "" : _trigger;
  uint32_t total = (uint32_t)_cfg.runonMin * 60000UL, el = now - _since;
  s.runonLeftMs  = (_state == S_RUNON && total > el) ? total - el : 0;
  return s;
}

// One line per poll, timestamped with uptime (no wall clock needed), e.g.
//   [FUME] 01:02:03 AQI 12 ug/m3 | power on | auto | ON 00:03:10 | RUNNING
void FumeExtractor::logAqi() {
  uint32_t now = millis();
  char tail[48];
  if (_state == S_RUNON) {
    uint32_t total = (uint32_t)_cfg.runonMin * 60000UL, el = now - _since;
    snprintf(tail, sizeof(tail), "run-on, OFF in %s", hms(total > el ? total - el : 0).c_str());
  } else if (_state == S_ON) {
    snprintf(tail, sizeof(tail), "ON %s | %s", hms(now - _since).c_str(), _gcode);
  } else {
    snprintf(tail, sizeof(tail), "OFF");
  }
  _log->printf("[FUME] %s AQI %d ug/m3 | power %s | %s | %s\n", hms(now).c_str(), _aqi,
               _power == 1 ? "on" : _power == 0 ? "off" : "?", modeName(_mode), tail);
}

const char* FumeExtractor::stateName(uint8_t s) {
  switch (s) {
    case S_ON:    return "ON";
    case S_RUNON: return "RUN-ON";
    default:      return "OFF";
  }
}

// Mode names of the 3C; other purifiers show "auto" or the raw value.
const char* FumeExtractor::modeName(int m) const {
  static char buf[12];
  if (m < 0) return "?";
  if (m == _map.autoMode) return "auto";
  bool is3C = _map.modeSiid == 2 && _map.modePiid == 4 && _map.autoMode == 0;
  if (is3C && m == 1) return "sleep";
  if (is3C && m == 2) return "favorite";
  snprintf(buf, sizeof(buf), "mode %d", m);
  return buf;
}

String FumeExtractor::hms(uint32_t ms) {
  uint32_t s = ms / 1000;
  char b[16];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", (unsigned long)(s / 3600),
           (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  return String(b);
}

void FumeExtractor::printStatus(Print& out) {
  uint32_t now = millis();
  out.printf("[FUME] %s | purifier %s | token %s | types %s | run-on %u min | always %s\n",
             _cfg.enabled ? "ENABLED" : "disabled", _cfg.ip[0] ? _cfg.ip : "[no IP]",
             tokenHint().c_str(), _cfg.types, _cfg.runonMin, _cfg.always ? "on" : "off");

  const char* t = activeType();
  out.printf("[FUME] State %s for %s", stateName(_state), hms(now - _since).c_str());
  if (_state != S_OFF) out.printf(" (trigger %s)", _trigger);
  out.printf(" | printer %s | active slot %s%s%s\n", _gcode[0] ? _gcode : "?",
             slotLabel(_trayNow).c_str(), t[0] ? " = " : "", t);

  String slots;
  for (int i = 0; i <= EXT; i++) {
    if (!_known[i]) continue;
    slots += " " + slotLabel(i) + " " + (_type[i][0] ? _type[i] : "-");
  }
  out.printf("[FUME] Slots:%s\n", slots.length() ? slots.c_str() : " no filament data from the printer yet");
  if (_jobMask) {
    String job;
    for (int i = 0; i <= EXT; i++)
      if ((_jobMask >> i) & 1) job += " " + slotLabel(i);
    out.printf("[FUME] Current job uses:%s\n", job.c_str());
  }
  if (_hostType[0]) out.printf("[FUME] Host reports filament %s\n", _hostType);

  if (_want != C_NONE)
    out.printf("[FUME] Waiting to deliver %s (round %u)\n", _want == C_POWER_ON ? "ON" : "OFF",
               _wantRounds + 1);
  if (_statusAt)
    out.printf("[FUME] Last reading %s ago: power %s, %s, AQI %d ug/m3\n",
               hms(now - _statusAt).c_str(), _power == 1 ? "on" : _power == 0 ? "off" : "?",
               modeName(_mode), _aqi);
}

// -----------------------------------------------------------------------------
// Console: any line source (USB serial, telnet, a web form) can feed this.
// -----------------------------------------------------------------------------
bool FumeExtractor::handleCommand(const String& lineIn, Print& out) {
  String line = lineIn;
  line.trim();
  if (line.length() < 4 || !line.substring(0, 4).equalsIgnoreCase("fume")) return false;
  if (line.length() > 4 && line[4] != ' ') return false;

  String rest = line.substring(4);
  rest.trim();
  int sp = rest.indexOf(' ');
  String verb = (sp < 0) ? rest : rest.substring(0, sp);
  String arg  = (sp < 0) ? "" : rest.substring(sp + 1);
  verb.toLowerCase();
  arg.trim();

  if (verb == "help") {
    out.println("Purifier automation (Xiaomi MIoT purifier over miIO, LAN only):");
    out.println("  fume status            settings, state, filament slots + ask the purifier");
    out.println("  fume info              purifier model + firmware (checks IP and token)");
    out.println("  fume on | fume off     switch the purifier now (test)");
    out.println("  fume enable | disable  automatic control from print events");
    out.println("  fume ip <a.b.c.d>      purifier IP (give it a DHCP reservation)");
    out.println("  fume token <32 hex>    miIO token (Xiaomi Cloud Tokens Extractor)");
    out.println("  fume types <ABS,ASA>   filaments that switch it on (prefix match)");
    out.println("  fume runon <minutes>   keep running after the print, 0-240");
    out.println("  fume always on|off     test mode: every print switches it on");
  }
  else if (verb.length() == 0 || verb == "status") {
    printStatus(out);
    _manual = C_STATUS;
  }
  else if (verb == "info") { _manual = C_INFO;      out.println("[FUME] Asking the purifier..."); }
  else if (verb == "on")   { _manual = C_POWER_ON;  out.println("[FUME] Sending ON..."); }
  else if (verb == "off")  { _manual = C_POWER_OFF; out.println("[FUME] Sending OFF..."); }
  else if (verb == "enable") {
    if (!_miio.ready()) { out.println("[FUME] Set `fume ip` and `fume token` first"); return true; }
    if (_cfg.enabled)   { out.println("[FUME] Already enabled"); return true; }
    _cfg.enabled = true;
    _state = S_OFF;
    _since = millis();
    saveConfig();
    out.println("[FUME] Enabled: the purifier follows the print events");
    evaluate();   // a fume print already running switches it on now
  }
  else if (verb == "disable") {
    _cfg.enabled = false;
    _state = S_OFF;
    _since = millis();
    _want  = C_NONE;
    saveConfig();
    out.println("[FUME] Disabled; the purifier is left as it is");
  }
  else if (verb == "ip") {
    IPAddress ip;
    if (!ip.fromString(arg) || ip == IPAddress()) { out.println("Usage: fume ip <a.b.c.d>"); return true; }
    strlcpy(_cfg.ip, ip.toString().c_str(), sizeof(_cfg.ip));
    saveConfig();
    applyConnection();
    out.printf("[FUME] Purifier IP %s\n", _cfg.ip);
  }
  else if (verb == "token") {
    // setToken() rejects anything that is not exactly 32 hex characters
    if (!_miio.setToken(arg.c_str())) {
      out.printf("[FUME] Token must be 32 hex characters (got %u)\n", arg.length());
      return true;
    }
    strlcpy(_cfg.token, arg.c_str(), sizeof(_cfg.token));
    saveConfig();
    out.printf("[FUME] Token saved (%s)\n", tokenHint().c_str());
  }
  else if (verb == "types") {
    String t = arg;
    t.replace(" ", "");
    t.toUpperCase();
    if (t.length() == 0 || t.length() >= sizeof(_cfg.types)) {
      out.println("Usage: fume types ABS,ASA[,...]");
      return true;
    }
    strlcpy(_cfg.types, t.c_str(), sizeof(_cfg.types));
    saveConfig();
    out.printf("[FUME] Fume filaments: %s\n", _cfg.types);
  }
  else if (verb == "runon") {
    long m = arg.toInt();
    if (arg.length() == 0 || m < 0 || m > 240 || (m == 0 && arg != "0")) {
      out.println("Usage: fume runon <0-240 minutes>");
      return true;
    }
    _cfg.runonMin = (uint16_t)m;
    saveConfig();
    out.printf("[FUME] Run-on %u min\n", _cfg.runonMin);
  }
  else if (verb == "always") {
    if (arg.equalsIgnoreCase("on"))       _cfg.always = true;
    else if (arg.equalsIgnoreCase("off")) _cfg.always = false;
    else { out.println("Usage: fume always on|off"); return true; }
    saveConfig();
    out.printf("[FUME] Always (every print): %s\n", _cfg.always ? "ON - test mode" : "off");
  }
  else {
    out.println("Unknown fume command - type `fume help`");
  }
  return true;
}
