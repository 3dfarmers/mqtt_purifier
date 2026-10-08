// MiioClient - see MiioClient.h for the protocol summary (part of MqttPurifier).
#include "MiioClient.h"
#include <WiFi.h>
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "esp_random.h"
#include "MiniJson.h"

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// AES-128-CBC over a buffer, in place, one block at a time. Done by hand on
// top of ECB rather than with mbedtls_aes_crypt_cbc() so the input and output
// can be the same buffer whatever the AES backend (the ESP32 port may use
// DMA), which saves a second packet-sized buffer.
static void cbcInPlace(bool encrypt, const uint8_t key[16], const uint8_t iv[16],
                       uint8_t* buf, size_t len) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (encrypt) mbedtls_aes_setkey_enc(&aes, key, 128);
  else         mbedtls_aes_setkey_dec(&aes, key, 128);
  uint8_t prev[16], in[16], out[16];
  memcpy(prev, iv, 16);
  for (size_t off = 0; off + 16 <= len; off += 16) {
    uint8_t* blk = buf + off;
    if (encrypt) {
      for (int j = 0; j < 16; j++) in[j] = blk[j] ^ prev[j];
      mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
      memcpy(blk, out, 16);
      memcpy(prev, out, 16);
    } else {
      memcpy(in, blk, 16);
      mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, in, out);
      for (int j = 0; j < 16; j++) blk[j] = out[j] ^ prev[j];
      memcpy(prev, in, 16);
    }
  }
  mbedtls_aes_free(&aes);
}

static inline void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void put32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool MiioClient::setToken(const char* hex) {
  if (!hex || strlen(hex) != 32) return false;
  uint8_t t[16];
  for (int i = 0; i < 16; i++) {
    int hi = hexNibble(hex[2 * i]), lo = hexNibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    t[i] = (uint8_t)(hi << 4 | lo);
  }
  memcpy(_token, t, 16);
  mbedtls_md5(_token, 16, _key);
  uint8_t kt[32];
  memcpy(kt, _key, 16);
  memcpy(kt + 16, _token, 16);
  mbedtls_md5(kt, 32, _iv);
  _hasToken = true;
  return true;
}

bool MiioClient::request(const char* method, const char* paramsJson) {
  if (busy())   { snprintf(_err, sizeof(_err), "busy");             return false; }
  if (!ready()) { snprintf(_err, sizeof(_err), "no IP or token");   return false; }
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(_err, sizeof(_err), "WiFi not connected");
    return false;
  }
  // {"id":NNNN,"method":"...","params":...} adds at most ~40 bytes
  if (strlen(method) >= sizeof(_method) ||
      strlen(method) + strlen(paramsJson) + 40 >= MAX_PLAIN) {
    snprintf(_err, sizeof(_err), "request too long");
    return false;
  }
  if (!_udpOpen) {
    if (!_udp.begin(0)) {   // any free local port
      snprintf(_err, sizeof(_err), "cannot open UDP socket");
      return false;
    }
    _udpOpen = true;
  }
  strlcpy(_method, method, sizeof(_method));
  strlcpy(_params, paramsJson, sizeof(_params));
  if (_id == 0) _id = 1 + esp_random() % 9000;   // devices answer a repeated id
                                                 // from cache, so never restart at 1
  _attempt  = 0;
  _reqStart = millis();
  _err[0]   = 0;
  _hint     = nullptr;
  _reply    = nullptr;
  startAttempt();
  return true;
}

void MiioClient::closeSocket() {
  _udp.stop();
  _udpOpen = false;
}

bool MiioClient::sendRaw(const uint8_t* b, size_t n) {
  if (!_udp.beginPacket(_ip, PORT)) return false;
  _udp.write(b, n);
  return _udp.endPacket() == 1;
}

void MiioClient::startAttempt() {
  // A late reply to an earlier attempt must not be read as this one's.
  while (_udp.parsePacket() > 0) _udp.clear();

  uint8_t hello[32];
  memset(hello, 0xFF, sizeof(hello));
  put16(hello, 0x2131);
  put16(hello + 2, 32);
  _phase = P_HELLO;
  _t0 = millis();
  _sendFailed = !sendRaw(hello, sizeof(hello));
}

void MiioClient::sendCommand() {
  if (++_id > 9999) _id = 1;

  // Plaintext straight into the packet's payload area, encrypted there.
  char* plain = (char*)(_pkt + 32);
  int n = snprintf(plain, MAX_PLAIN, "{\"id\":%lu,\"method\":\"%s\",\"params\":%s}",
                   (unsigned long)_id, _method, _params);
  if (n <= 0 || n >= (int)MAX_PLAIN) {   // request() bounds this; belt and braces
    _hint = "request too long";
    _sendFailed = true;
    _phase = P_CMD;
    return;
  }
  size_t pad = 16 - (n % 16);                    // PKCS#7: 1..16 bytes of value pad
  memset(plain + n, (int)pad, pad);
  size_t clen  = n + pad;
  size_t total = 32 + clen;
  cbcInPlace(true, _key, _iv, _pkt + 32, clen);

  put16(_pkt, 0x2131);
  put16(_pkt + 2, (uint16_t)total);
  put32(_pkt + 4, 0);
  put32(_pkt + 8, _did);
  // python-miio sends the handshake stamp + 1 s; add the time since then too,
  // which is 0 here because the command follows its hello immediately.
  put32(_pkt + 12, _stamp + 1 + (millis() - _stampAt) / 1000);
  memcpy(_pkt + 16, _token, 16);                 // checksum is taken over the token...
  uint8_t sum[16];
  mbedtls_md5(_pkt, total, sum);
  memcpy(_pkt + 16, sum, 16);                    // ...then replaces it

  _phase = P_CMD;
  _t0 = millis();
  _sendFailed = !sendRaw(_pkt, total);
}

MiioClient::Result MiioClient::poll() {
  if (_phase == P_IDLE) return IDLE;

  int n = _udp.parsePacket();
  if (n > 0) {
    bool fromDevice = (_udp.remoteIP() == _ip);
    int len = _udp.read(_rx, sizeof(_rx));
    _udp.clear();                                // drop anything beyond the buffer
    if (fromDevice && len >= 32 && get16(_rx) == 0x2131) {
      if (_phase == P_HELLO && len == 32) {
        _did     = get32(_rx + 8);
        _stamp   = get32(_rx + 12);
        _stampAt = millis();
        sendCommand();
        return BUSY;
      }
      if (_phase == P_CMD && len > 32) {
        Result r = onReply((size_t)len);
        if (r != BUSY) return r;
      }
    }
  }

  // A failed send means the station link is down: retrying inside this
  // exchange cannot help, so fail now and let the caller try again later.
  if (_sendFailed) {
    closeSocket();                               // reopened by the next request()
    snprintf(_err, sizeof(_err), "%s", _hint ? _hint : "UDP send failed (WiFi?)");
    _sendFailed = false;
    _phase  = P_IDLE;
    _lastMs = millis() - _reqStart;
    return FAILED;
  }
  if (millis() - _t0 >= TIMEOUT_MS)
    return retryOrFail(_phase == P_HELLO ? "no reply to hello" : "no reply to command");
  return BUSY;
}

MiioClient::Result MiioClient::onReply(size_t len) {
  size_t total = get16(_rx + 2);
  if (total > len || total <= 32) return BUSY;   // malformed: keep waiting

  uint8_t sum[16], got[16];
  memcpy(got, _rx + 16, 16);
  memcpy(_rx + 16, _token, 16);
  mbedtls_md5(_rx, total, sum);
  if (memcmp(sum, got, 16) != 0) {
    _hint = "reply checksum mismatch (wrong token?)";
    return BUSY;
  }

  size_t clen = total - 32;
  if (clen % 16) return BUSY;
  cbcInPlace(false, _key, _iv, _rx + 32, clen);
  char* plain = (char*)(_rx + 32);

  size_t pad = (uint8_t)plain[clen - 1];
  if (pad < 1 || pad > 16) {
    _hint = "reply does not decrypt (wrong token?)";
    return BUSY;
  }
  size_t n = clen - pad;
  plain[n] = 0;                                  // inside the packet: n < clen
  while (n > 0 && (plain[n - 1] == 0 || plain[n - 1] == ' ')) plain[--n] = 0;

  // Answer to an earlier, timed-out attempt: keep waiting for ours.
  const char* e = plain + n;
  long id = -1;
  bool hasError = false;
  mjson::eachMember(plain, e, [&](const char* k, size_t kn, const char* v, const char* ve) {
    if (mjson::keyIs(k, kn, "id")) mjson::readInt(v, ve, &id);
    else if (mjson::keyIs(k, kn, "error")) hasError = true;
    return true;
  });
  if (id != (long)_id) return BUSY;

  _phase  = P_IDLE;
  _lastMs = millis() - _reqStart;
  if (hasError) {
    // The device understood us and refused; retrying the same request
    // would get the same answer. Hand the error text up.
    snprintf(_err, sizeof(_err), "device error: %.60s", plain);
    return FAILED;
  }
  _reply = plain;
  return OK;
}

MiioClient::Result MiioClient::retryOrFail(const char* why) {
  if (_attempt < RETRIES) {
    _attempt++;
    startAttempt();
    return BUSY;
  }
  snprintf(_err, sizeof(_err), "%s (%u tries)%s%s", why, (unsigned)(RETRIES + 1),
           _hint ? "; " : "", _hint ? _hint : "");
  _phase  = P_IDLE;
  _lastMs = millis() - _reqStart;
  return FAILED;
}
