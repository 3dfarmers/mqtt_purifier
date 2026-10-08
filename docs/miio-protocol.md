# Xiaomi miIO protocol (LAN, UDP 54321)

What this library implements in `src/MiioClient.cpp` and `tools/miio.py`,
described precisely enough to write a compatible implementation from scratch.
Sources: OpenMiHome
[mihome-binary-protocol](https://github.com/OpenMiHome/mihome-binary-protocol/blob/master/doc/PROTOCOL.md),
[python-miio](https://github.com/rytilahti/python-miio) (`miio/protocol.py`,
`miio/miioprotocol.py`), and our own bench tests (marked **bench**).

## 1. Transport

- UDP, device port **54321**. Any local source port; the device answers to
  the source address and port of the request.
- One request at a time per device. No connection state on the device beyond
  a short-lived cache of recent request ids.
- No broadcast needed when the device IP is known (give it a DHCP reservation).

## 2. Packet layout

All integers big endian. Every packet starts with a 32-byte header:

| Offset | Size | Field | Request | Hello |
|---:|---:|---|---|---|
| 0 | 2 | magic | `0x2131` | `0x2131` |
| 2 | 2 | length | 32 + ciphertext length | `0x0020` |
| 4 | 4 | unknown | `0x00000000` | `0xFFFFFFFF` |
| 8 | 4 | device id | from the hello reply | `0xFFFFFFFF` |
| 12 | 4 | stamp | see section 4 | `0xFFFFFFFF` |
| 16 | 16 | checksum | MD5, see section 3 | 16 x `0xFF` |
| 32 | n | payload | AES-128-CBC ciphertext | none |

## 3. Keys and checksum

```
token     16 bytes (the 32-hex-character string, decoded)
key       = MD5(token)
iv        = MD5(key || token)
payload   = AES-128-CBC(key, iv, PKCS#7(json bytes))      # padding 1..16 bytes, always present
checksum  = MD5(header[0..15] || token || payload)
```

Equivalent, and how the firmware does it: write the header with the **token**
in the checksum field, MD5 the whole packet, then overwrite the checksum
field with that digest. Replies are checked the same way: save the received
checksum, put the token in its place, MD5 the packet (up to `length`),
compare.

## 4. Handshake ("hello") and the stamp

1. Send the 32-byte hello: `21 31 00 20` followed by 28 bytes `ff`.
2. The device answers with a 32-byte header-only packet:
   - bytes 8..11: **device id** (a 32-bit number, also shown as a decimal "ID" by the token extractor),
   - bytes 12..15: **stamp** = device uptime in seconds,
   - bytes 16..31: all `ff` on any device that has been set up in Mi Home.
     Old firmware used to put the token here; current devices do not, so the
     token cannot be obtained from the LAN (**bench**: `ff..ff` on a 3C).
3. Requests carry `stamp = hello_stamp + 1 + seconds_since_hello`
   (python-miio uses hello stamp + 1; we send right after the hello, so the
   elapsed part is 0). Devices may ignore a request whose stamp lags their clock.

Do a fresh hello before **every** request and every retry. It is cheap and
avoids stale stamps after the device rebooted.

## 5. JSON payload

Request:

```json
{"id": 4711, "method": "get_properties", "params": [ ... ]}
```

- `id`: start at a **random** value (1..9000), increment per request and per
  retry, wrap at 9999. Devices keep a cache of recent ids and answer a
  repeated id with the cached reply (or not at all). Starting at 1 after every
  reboot collides with ids the device saw minutes ago.
- No whitespace needed; keep requests short (ours are under 200 bytes).

Reply (after decrypting, strip padding, then strip trailing `\0` bytes):

```json
{"id": 4711, "result": [ ... ]}
{"id": 4711, "error": {"code": -9999, "message": "..."}}
```

- Match `id` to the request. A reply with another id is a late answer to an
  earlier attempt: ignore it and keep waiting.
- `error` means the device understood and refused: report it, do not retry.

## 6. MIoT methods used

Properties are addressed by service id (`siid`) and property id (`piid`).
`did` is a free label the device echoes back.

**Read** (`get_properties`):

```json
{"id":1,"method":"get_properties","params":[
  {"did":"power","siid":2,"piid":1},
  {"did":"mode","siid":2,"piid":4},
  {"did":"aqi","siid":3,"piid":4}]}
```

```json
{"id":1,"result":[
  {"did":"power","siid":2,"piid":1,"code":0,"value":true},
  {"did":"mode","siid":2,"piid":4,"code":0,"value":0},
  {"did":"aqi","siid":3,"piid":4,"code":0,"value":4}]}
```

**Write** (`set_properties`):

```json
{"id":2,"method":"set_properties","params":[{"did":"power","siid":2,"piid":1,"value":true}]}
```

```json
{"id":2,"result":[{"did":"power","siid":2,"piid":1,"code":0}]}
```

Result codes: `0` done, `1` accepted and still completing (treat as
success), negative = refused. Match results on `siid`/`piid`, not on `did`.

**Identify** (`miIO.info`, params `[]`): returns `model`, `fw_ver`, `hw_ver`,
`mac`, network details (`ap`, `netif`) and on some firmware the `token`.
**bench**: about 500-700 bytes; model `zhimi.airp.mb4a`, fw `2.1.7`, hw `esp32`.
Print only what you need; never log the whole reply.

### Mi Air Purifier 3C property map (bench-verified)

| Property | siid | piid | Values |
|---|---:|---:|---|
| power | 2 | 1 | `true` / `false` |
| mode | 2 | 4 | `0` auto, `1` sleep, `2` favorite |
| PM2.5 density ("aqi") | 3 | 4 | integer, ug/m3, read only |

Same map for `zhimi.airpurifier.mb4` (python-miio `_MAPPING_MB4`).

### Quirk: power and mode in one request (**bench**)

`set_properties [power=true, mode=0]` sent to a 3C that is **off** returned
`power code 0, mode code -4004`: it switched on and refused the mode. Sent as
two requests, power first and mode after the power confirmation, both return
`0`. Always switch on with two requests.

## 7. Worked example (test vector)

Generated by `python tools/miio.py vector` and cross-checked with the .NET
AES/MD5 implementation (independent code). Use it as a unit test for any port.

```
token     00112233445566778899aabbccddeeff   (example, not a real device)
key       6e8311168ee16d6aa1aa48c64145003c   = MD5(token)
iv        6f434fa9acd75da73e5fb999f641cda2   = MD5(key + token)
device id 0x12345678
stamp     0x00001001
payload   {"id":1,"method":"miIO.info","params":[]}      (41 bytes -> 48 after PKCS#7)

header    2131 0050 00000000 12345678 00001001
checksum  ce1b4199e8ca47292e3fabc9a5f00711
cipher    a5516ec6151955dc2bb2d43e7c84c18352a39a7f22fd2903d282a5783c6d56ac
          8c9c7629130a25fac17d38175c1a7799

packet (80 bytes)
21310050000000001234567800001001ce1b4199e8ca47292e3fabc9a5f00711
a5516ec6151955dc2bb2d43e7c84c18352a39a7f22fd2903d282a5783c6d56ac
8c9c7629130a25fac17d38175c1a7799
```

AES itself: FIPS-197 appendix C.1 (key `000102...0f`, plaintext
`00112233...ff` -> `69c4e0d86a7b0430d8cdb78070b4c55a`) is checked by
`python tools/miio.py selftest`.

## 8. Timeouts and retries (what works in practice)

| Setting | Value | Why |
|---|---|---|
| Reply timeout | 2 s per hello and per command | Replies arrive in 10-200 ms on a good link (**bench**) |
| Attempts per exchange | 1 + 3 retries, fresh hello + new id each | Single lost packets are common on WiFi |
| After a failed exchange | retry after 2 s, then every 30 s, ~10 min total | The first exchange after a quiet spell sometimes fails completely and the next one works at once (**bench**, weak link) |
| A status poll succeeds while a command waits | send the command now | The device is clearly reachable again |

**bench**: on a good link a whole exchange (hello + command + reply) took
0.35-1.1 s. With the ESP32 at -85 to -90 dBm, about half the exchanges were
lost entirely; at better RSSI none were. On an ESP32 disable modem sleep
(`WiFi.setSleep(false)`) so inbound UDP is not delayed by power save.

## 9. Implementation notes for small MCUs

- **AES in place.** `MiioClient.cpp` does CBC by hand on top of single-block
  ECB, so the plaintext can be encrypted inside the outgoing packet buffer and
  a reply decrypted where it was received. One buffer per direction; works
  whatever the AES backend (the ESP32 port can use DMA, where in-place CBC is
  not guaranteed).
- **Buffers.** Requests < 256 bytes; replies up to ~700 bytes (`miIO.info`),
  so a 1 KB receive buffer. Total static RAM of the client: ~1.6 KB.
- **Drain stale packets** before each hello, and accept a packet only if it
  comes from the device's IP.
- **A failed `sendto`** means the station link is down: fail the exchange at
  once instead of burning retries, close and reopen the socket next time.
