// =============================================================================
// MiniJson - tiny read-only JSON walker (part of MqttPurifier)
// =============================================================================
// Bambu printers report a nested JSON object (print -> ams -> ams[] -> tray[])
// that repeats the same keys ("id", "tray_type") at every level, so plain
// strstr() matching is not enough. The purifier replies are walked the same way.
//
// Design rules, all deliberate:
//   - never allocates, never copies the input, never writes to it;
//   - works on a buffer that is NOT NUL-terminated (a raw MQTT payload),
//     bounded by an end pointer `e`; nothing reads past `e`;
//   - a value is handed to a callback only once it is complete, so a truncated
//     payload makes a call return nullptr / false instead of delivering junk.
//
// Cost: one linear pass per walked level. A 18 KB Bambu report takes well
// under a millisecond on an ESP32 at 160-240 MHz.
// =============================================================================
#pragma once
#include <stddef.h>
#include <string.h>

namespace mjson {

inline const char* ws(const char* p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  return p;
}

// p at '"' -> just past the closing quote, or nullptr.
inline const char* skipString(const char* p, const char* e) {
  if (p >= e || *p != '"') return nullptr;
  for (p++; p < e; p++) {
    if (*p == '\\') { p++; continue; }
    if (*p == '"') return p + 1;
  }
  return nullptr;
}

// p at the first character of any value -> just past it, or nullptr.
inline const char* skipValue(const char* p, const char* e) {
  p = ws(p, e);
  if (p >= e) return nullptr;
  if (*p == '"') return skipString(p, e);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (p < e) {
      char c = *p;
      if (c == '"') { p = skipString(p, e); if (!p) return nullptr; continue; }
      if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') { if (--depth == 0) return p + 1; }
      p++;
    }
    return nullptr;
  }
  // number, true, false, null: runs to the next delimiter
  const char* s = p;
  while (p < e && *p != ',' && *p != '}' && *p != ']' &&
         *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
  return (p < e && p > s) ? p : nullptr;   // a scalar must be followed by something
}

inline bool keyIs(const char* k, size_t n, const char* lit) {
  return strlen(lit) == n && memcmp(k, lit, n) == 0;
}

// Calls fn(key, keyLen, value, valueEnd) for each member of the object at p.
// fn returns false to stop early. Returns false if the object is malformed or
// truncated; members before the damage have still been delivered.
template <typename F>
bool eachMember(const char* p, const char* e, F fn) {
  p = ws(p, e);
  if (p >= e || *p != '{') return false;
  p = ws(p + 1, e);
  if (p < e && *p == '}') return true;
  while (p < e) {
    if (*p != '"') return false;
    const char* k  = p + 1;
    const char* ke = skipString(p, e);
    if (!ke) return false;
    size_t klen = (size_t)(ke - 1 - k);
    p = ws(ke, e);
    if (p >= e || *p != ':') return false;
    const char* v  = ws(p + 1, e);
    const char* ve = skipValue(v, e);
    if (!ve) return false;
    if (!fn(k, klen, v, ve)) return true;
    p = ws(ve, e);
    if (p < e && *p == ',') { p = ws(p + 1, e); continue; }
    return p < e && *p == '}';
  }
  return false;
}

// Calls fn(value, valueEnd) for each element of the array at p.
template <typename F>
bool eachElement(const char* p, const char* e, F fn) {
  p = ws(p, e);
  if (p >= e || *p != '[') return false;
  p = ws(p + 1, e);
  if (p < e && *p == ']') return true;
  while (p < e) {
    const char* ve = skipValue(p, e);
    if (!ve) return false;
    if (!fn(p, ve)) return true;
    p = ws(ve, e);
    if (p < e && *p == ',') { p = ws(p + 1, e); continue; }
    return p < e && *p == ']';
  }
  return false;
}

// String value at p into out (always NUL-terminated, truncated to fit).
// Escapes are reduced to the escaped character; none of the values this
// library reads contain any.
inline bool readStr(const char* p, const char* e, char* out, size_t n) {
  p = ws(p, e);
  const char* end = skipString(p, e);
  if (!end || n == 0) return false;
  size_t o = 0;
  for (const char* q = p + 1; q < end - 1 && o + 1 < n; q++) {
    if (*q == '\\' && q + 1 < end - 1) q++;
    out[o++] = *q;
  }
  out[o] = 0;
  return true;
}

// Integer from a number or a numeric string: 3 and "3" both give 3.
// Bambu sends tray_now and the AMS ids as strings.
inline bool readInt(const char* p, const char* e, long* out) {
  p = ws(p, e);
  if (p < e && *p == '"') p++;
  bool neg = false;
  if (p < e && *p == '-') { neg = true; p++; }
  if (p >= e || *p < '0' || *p > '9') return false;
  long v = 0;
  while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  *out = neg ? -v : v;
  return true;
}

inline bool readBool(const char* p, const char* e, bool* out) {
  p = ws(p, e);
  if (e - p >= 4 && memcmp(p, "true", 4) == 0)  { *out = true;  return true; }
  if (e - p >= 5 && memcmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

}  // namespace mjson
