// =============================================================================
// FumeExtractor - switch a Xiaomi air purifier for fume-producing 3D prints
// (part of MqttPurifier)
// =============================================================================
// Turns a Xiaomi MIoT air purifier ON in auto mode when a print starts with a
// fume filament (ABS, ASA by default), and OFF a run-on time after the print
// ends. Logs the purifier's PM2.5 every 10 s while it runs. Defaults are for
// the Xiaomi Mi Air Purifier 3C (zhimi.airp.mb4a / zhimi.airpurifier.mb4);
// other MIoT purifiers work with their own property ids (setMiotMap()).
//
// Inputs, pick one:
//   - onBambuReport(payload, len): every /report payload of a Bambu printer's
//     local MQTT. The library keeps a merged view (P1/A1/P2S send only the
//     fields that changed), reads gcode_state, the AMS slots and the job's
//     slot list. It never sends anything to the printer.
//   - onPrintState("RUNNING", "ABS"): any other printer, fed by the host.
//
// Host integration (four calls, see examples/):
//   setup():          fume.begin(Serial);
//   MQTT callback:    fume.onBambuReport((const char*)payload, length);
//   loop():           fume.loop();
//   console:          if (fume.handleCommand(line, Serial)) ...;
//
// Logic (docs/state-machine.md has the full table):
//   OFF   -> ON     print PREPARE/RUNNING/PAUSE and a fume filament is in the
//                   job or the active slot (or "always"): power on, then auto
//   ON    -> RUNON  gcode_state FINISH / FAILED / IDLE: start the run-on timer
//   RUNON -> OFF    run-on elapsed: power off
//   RUNON -> ON     a fume print starts: cancel the timer, send nothing
// Commands go out on transitions only. Once the purifier has confirmed one,
// nothing is re-sent, so switching it by hand in between is respected.
//
// Never blocks: the UDP exchange is a state machine driven by loop().
// No pins, no board drivers. Settings persist in NVS namespace "fume".
// =============================================================================
#pragma once
#include <Arduino.h>
#include "MiioClient.h"

// MIoT property ids of the purifier (siid = service, piid = property).
// Defaults: Mi Air Purifier 3C, verified on hardware (zhimi.airp.mb4a fw 2.1.7).
// Look up another model at https://home.miot-spec.com/spec/<model>
// (docs/porting.md explains which properties to pick).
struct MiotMap {
  uint8_t powerSiid = 2, powerPiid = 1;   // bool: on / off
  uint8_t modeSiid  = 2, modePiid  = 4;   // int: operating mode
  uint8_t aqiSiid   = 3, aqiPiid   = 4;   // int: PM2.5 density, ug/m3
  int     autoMode  = 0;                  // value of "auto" for modePiid
};

// Compile-time defaults, applied to any setting that is not saved in NVS yet.
// Leave a field at its "unset" value to keep the library default.
struct FumeDefaults {
  const char* ip       = nullptr;   // purifier IP, "192.168.1.50"
  const char* token    = nullptr;   // 32 hex characters
  const char* types    = nullptr;   // fume filaments, "ABS,ASA"
  int         runonMin = -1;        // minutes after the print, 0-240
  int         enabled  = -1;        // 1 = automatic control on at first boot
  int         always   = -1;        // 1 = every print switches it on (test)
};

// Read-only snapshot for a host (web page, display, video overlay).
struct FumeStatus {
  const char* state;          // "OFF", "ON", "RUN-ON"
  bool        enabled;
  int         power;          // -1 unknown, 0 off, 1 on
  int         mode;           // -1 unknown, otherwise the purifier's mode value
  int         aqi;            // -1 unknown, PM2.5 in ug/m3
  uint32_t    readingAgeMs;   // age of power/mode/aqi; UINT32_MAX if never read
  const char* printerState;   // "RUNNING" etc., "" until known
  const char* activeType;     // filament in the active slot, "" if none/unknown
  const char* trigger;        // filament that switched it on, "" when OFF
  uint32_t    runonLeftMs;    // time left in RUN-ON, 0 otherwise
};

class FumeExtractor {
public:
  // Loads settings from NVS; `defaults` fills whatever was never saved.
  void begin(Print& log, const FumeDefaults* defaults = nullptr);
  // Property ids for a purifier other than the 3C. Call before begin().
  void setMiotMap(const MiotMap& m);

  // Printer input, Bambu: every /report payload, as received (no copy needed,
  // need not be NUL-terminated). Call it for EVERY packet, before any parse
  // throttling the host does: a delta may be the only copy of a slot change.
  void onBambuReport(const char* json, size_t len);
  // Printer input, anything else. state uses Bambu's words: PREPARE, RUNNING,
  // PAUSE (print under way), FINISH, FAILED, IDLE (print over). filamentType
  // is the material in use ("ABS"), or "" / nullptr if unknown.
  void onPrintState(const char* state, const char* filamentType);

  void loop();                                         // call every loop()
  bool handleCommand(const String& line, Print& out);  // "fume ..." lines; false otherwise
  FumeStatus status() const;

  // The line with a "fume token ..." argument cut to 4 characters, for hosts
  // that echo console input to a log or a web page.
  static String redact(const String& line);

private:
  enum State : uint8_t { S_OFF, S_ON, S_RUNON };
  enum Cmd   : uint8_t { C_NONE, C_POWER_ON, C_MODE_AUTO, C_POWER_OFF, C_STATUS, C_INFO };

  static const uint8_t  SLOTS          = 16;     // 4 AMS units x 4 trays
  static const uint8_t  EXT            = 16;     // index of the external spool
  static const uint8_t  TYPE_LEN       = 16;
  static const uint32_t POLL_MS        = 10000;  // AQI poll while ON / RUNON
  static const uint32_t FIRST_RETRY_MS = 2000;   // after a failed first round: a cold
                                                 // exchange can fail where the next works
  static const uint32_t RETRY_MS       = 30000;  // between later failed rounds
  static const uint8_t  MAX_ROUNDS     = 20;     // ~10 min, then wait for the next transition

  struct Cfg {
    bool     enabled   = false;
    char     ip[16]    = "";
    char     token[33] = "";
    char     types[64] = "ABS,ASA";
    uint16_t runonMin  = 10;
    bool     always    = false;
  } _cfg;

  void loadConfig(const FumeDefaults* d);
  void saveConfig();
  void applyConnection();
  void buildParams();

  // printer side
  void parsePrint(const char* p, const char* e);
  void parseAms(const char* p, const char* e);
  void parseTray(const char* p, const char* e, int unit);
  void parseJob(const char* map, const char* mapEnd, const char* map2, const char* map2End,
                int useAms);
  void trackJobLifecycle();
  const char* activeType() const;
  const char* jobFumeType() const;
  bool isFumeType(const char* type) const;
  void evaluate();

  // purifier side
  void request(Cmd c);
  void startExchange(Cmd c, bool manual);
  void onExchangeDone(bool ok);
  bool parseStatus(const char* json);
  void printInfo(const char* json);
  static bool setAccepted(const char* json, char* codes, size_t n);

  void printStatus(Print& out);
  void logAqi();
  static const char* stateName(uint8_t s);
  const char* modeName(int m) const;
  static String hms(uint32_t ms);       // "hh:mm:ss"
  String tokenHint() const;

  Print*     _log = nullptr;
  MiioClient _miio;
  MiotMap    _map;
  char       _pOn[80], _pAuto[80], _pOff[80], _pStatus[160];   // request params

  // merged printer view
  char  _gcode[16]          = "";
  long  _trayNow            = -1;   // -1 unknown, 0..15 AMS, 254 external, 255 none
  char  _type[SLOTS + 1][TYPE_LEN] = {};
  bool  _known[SLOTS + 1]   = {};
  uint32_t _jobMask         = 0;    // slots the current job uses (bit 16 = external)
  bool  _jobRunning         = false;
  char  _hostType[TYPE_LEN] = "";   // filament from onPrintState()

  State    _state     = S_OFF;
  uint32_t _since     = 0;          // entry time of the current state
  char     _trigger[TYPE_LEN + 8] = "";

  Cmd      _want      = C_NONE;     // transition command still to deliver
  uint8_t  _wantRounds = 0;
  uint32_t _wantAt    = 0;
  uint32_t _wantDelay = 0;
  Cmd      _manual    = C_NONE;     // queued console command
  Cmd      _follow    = C_NONE;     // mode write that completes a confirmed ON
  bool     _followManual = false;
  Cmd      _inflight  = C_NONE;
  bool     _inflightManual = false;
  int8_t   _confirmed = -1;         // power state our last command set (-1 none)

  uint32_t _lastPoll  = 0;
  uint16_t _pollFails = 0;
  int      _aqi       = -1;
  int8_t   _power     = -1;
  int      _mode      = -1;
  uint32_t _statusAt  = 0;
};

extern FumeExtractor fume;
