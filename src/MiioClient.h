// =============================================================================
// MiioClient - Xiaomi miIO over local UDP, non-blocking (part of MqttPurifier)
// =============================================================================
// The LAN protocol python-miio speaks to Xiaomi devices (UDP port 54321).
// No cloud, no pairing: the 16-byte device token is the only secret.
//
// request() queues ONE exchange and poll() drives it from loop(); nothing in
// here waits on the network. An exchange is a hello (which returns the device
// id and its uptime stamp) followed by the encrypted command. Each retry starts
// again with a fresh hello and a new request id.
//
// Packet = 32-byte header + AES-128-CBC payload (PKCS#7 padding), big endian:
//    0  u16  magic 0x2131            2  u16  total length incl. header
//    4  u32  0 (hello: 0xFFFFFFFF)   8  u32  device id
//   12  u32  stamp (device uptime)  16  16 bytes checksum
//   key      = MD5(token)
//   iv       = MD5(key + token)
//   checksum = MD5(header bytes 0..15 + token + encrypted payload)
// Full byte-level description with a worked example: docs/miio-protocol.md.
// References: OpenMiHome mihome-binary-protocol doc/PROTOCOL.md, python-miio
// miio/protocol.py + miio/miioprotocol.py (stamp rule and per-retry id bump).
//
// RAM: about 1.6 KB, all static (no heap use apart from the WiFiUDP socket).
// =============================================================================
#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

class MiioClient {
public:
  enum Result : uint8_t { IDLE, BUSY, OK, FAILED };

  static const uint16_t PORT       = 54321;
  static const uint32_t TIMEOUT_MS = 2000;   // per hello and per command reply
  static const uint8_t  RETRIES    = 3;      // after the first attempt

  // 32 hex characters (16 bytes). Returns false and keeps the old token if
  // the string is malformed.
  bool setToken(const char* hex);
  void setIp(const IPAddress& ip) { _ip = ip; _hasIp = !(ip == IPAddress()); }
  bool ready() const { return _hasToken && _hasIp; }
  bool busy() const  { return _phase != P_IDLE; }

  // Start one exchange. paramsJson is the JSON value for "params" ("[...]").
  // False (see error()) if busy, not configured, WiFi down or too long.
  bool request(const char* method, const char* paramsJson);

  // Drive the exchange. Returns OK or FAILED exactly once per request, BUSY
  // while it runs, IDLE when there is nothing in flight. Call every loop().
  Result poll();

  // Decrypted reply JSON after OK; valid until the next request().
  const char* reply() const { return _reply ? _reply : ""; }
  const char* error() const { return _err; }     // reason, after FAILED
  uint32_t deviceId() const { return _did; }     // from the last hello reply
  uint32_t lastMs() const   { return _lastMs; }  // duration of the last exchange

private:
  enum Phase : uint8_t { P_IDLE, P_HELLO, P_CMD };
  // Sized to what a purifier exchange needs. A firmware that also talks TLS
  // to a printer needs a ~30-40 KB contiguous heap block for the handshake,
  // so this stays small and static.
  static const size_t MAX_PLAIN = 256;           // whole request JSON; the largest is ~180
  static const size_t BUF       = 1024;          // miIO.info answers with ~700 bytes

  void   startAttempt();
  void   sendCommand();
  Result onReply(size_t len);
  Result retryOrFail(const char* why);
  bool   sendRaw(const uint8_t* b, size_t n);
  void   closeSocket();

  WiFiUDP   _udp;
  bool      _udpOpen  = false;
  IPAddress _ip;
  bool      _hasIp    = false;
  uint8_t   _token[16];
  uint8_t   _key[16];
  uint8_t   _iv[16];
  bool      _hasToken = false;

  Phase     _phase    = P_IDLE;
  uint8_t   _attempt  = 0;
  uint32_t  _t0       = 0;        // phase start (millis)
  uint32_t  _reqStart = 0;
  uint32_t  _lastMs   = 0;
  uint32_t  _did      = 0;
  uint32_t  _stamp    = 0;
  uint32_t  _stampAt  = 0;        // millis() when _stamp was received
  uint32_t  _id       = 0;        // request id of the command in flight

  char      _method[32];
  char      _params[MAX_PLAIN];
  // Both directions are en/decrypted IN PLACE (CBC done block by block), so
  // one buffer each way is all the RAM this costs.
  uint8_t   _pkt[32 + MAX_PLAIN + 16];   // outgoing packet: header + padded payload
  uint8_t   _rx[BUF];                    // incoming packet; decrypted where it lies
  char*     _reply = nullptr;            // -> the decrypted JSON inside _rx
  char      _err[96] = "";
  const char* _hint   = nullptr;         // why a reply was discarded, for the final error
  bool      _sendFailed = false;
};
