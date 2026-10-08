#!/usr/bin/env python3
"""
miio.py - zero-dependency Xiaomi miIO desk tool and protocol reference.

Talk to a Xiaomi MIoT air purifier from a laptop, with nothing but the Python
standard library (AES-128 is implemented below, MD5 comes from hashlib). Use
it to check IP + token before flashing anything, and as an executable
reference when porting the protocol to another language or MCU.

    python miio.py hello  --ip 192.168.1.50                  # no token needed
    python miio.py info   --ip 192.168.1.50                  # model + firmware
    python miio.py status --ip 192.168.1.50                  # power, mode, PM2.5
    python miio.py on     --ip 192.168.1.50                  # power on, then auto mode
    python miio.py off    --ip 192.168.1.50
    python miio.py raw    --ip 192.168.1.50 --method get_properties \
                          --params '[{"did":"x","siid":2,"piid":1}]'
    python miio.py selftest                                  # AES + packet checks, offline
    python miio.py vector                                    # worked example for the docs

The token is read from --token or, better (keeps it out of shell history),
the MIIO_TOKEN environment variable. It is never printed.

Property ids default to the Mi Air Purifier 3C (zhimi.airp.mb4a). Another
model: --map power_siid,power_piid,mode_siid,mode_piid,aqi_siid,aqi_piid,auto
(look them up at https://home.miot-spec.com/spec/<model>).

Part of MqttPurifier (MIT). The firmware implementation is src/MiioClient.cpp;
both follow docs/miio-protocol.md byte for byte.
"""
import argparse
import hashlib
import json
import os
import random
import socket
import struct
import sys
import time

# ---------------------------------------------------------------------------
# AES-128 (FIPS-197), encrypt + decrypt of single blocks. Small and slow, which
# is fine for packets of a few hundred bytes.
# ---------------------------------------------------------------------------


def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _gmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xtime(a)
        b >>= 1
    return r


def _build_sbox():
    sbox = [0] * 256
    for x in range(256):
        # multiplicative inverse in GF(2^8), 0 maps to 0
        inv = 0
        if x:
            for y in range(1, 256):
                if _gmul(x, y) == 1:
                    inv = y
                    break
        s = inv
        for i in range(1, 5):
            s ^= ((inv << i) | (inv >> (8 - i))) & 0xFF
        sbox[x] = s ^ 0x63
    inv_sbox = [0] * 256
    for i, v in enumerate(sbox):
        inv_sbox[v] = i
    return sbox, inv_sbox


SBOX, INV_SBOX = _build_sbox()


def _expand_key(key):
    assert len(key) == 16
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(11)]


def _shift_rows(s, inv=False):
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            if inv:
                out[r + 4 * ((c + r) % 4)] = s[r + 4 * c]
            else:
                out[r + 4 * c] = s[r + 4 * ((c + r) % 4)]
    return out


def _mix_columns(s, inv=False):
    m = (14, 11, 13, 9) if inv else (2, 3, 1, 1)
    out = [0] * 16
    for c in range(4):
        a = s[4 * c:4 * c + 4]
        for r in range(4):
            out[4 * c + r] = (_gmul(a[0], m[(0 - r) % 4]) ^ _gmul(a[1], m[(1 - r) % 4])
                              ^ _gmul(a[2], m[(2 - r) % 4]) ^ _gmul(a[3], m[(3 - r) % 4]))
    return out


def aes_encrypt_block(rk, block):
    s = [b ^ k for b, k in zip(block, rk[0])]
    for rnd in range(1, 11):
        s = [SBOX[b] for b in s]
        s = _shift_rows(s)
        if rnd != 10:
            s = _mix_columns(s)
        s = [b ^ k for b, k in zip(s, rk[rnd])]
    return bytes(s)


def aes_decrypt_block(rk, block):
    s = [b ^ k for b, k in zip(block, rk[10])]
    for rnd in range(9, -1, -1):
        s = _shift_rows(s, inv=True)
        s = [INV_SBOX[b] for b in s]
        s = [b ^ k for b, k in zip(s, rk[rnd])]
        if rnd != 0:
            s = _mix_columns(s, inv=True)
    return bytes(s)


def cbc_encrypt(key, iv, data):
    pad = 16 - len(data) % 16                      # PKCS#7, always 1..16 bytes
    data = data + bytes([pad]) * pad
    rk, prev, out = _expand_key(key), iv, b""
    for i in range(0, len(data), 16):
        prev = aes_encrypt_block(rk, bytes(a ^ b for a, b in zip(data[i:i + 16], prev)))
        out += prev
    return out


def cbc_decrypt(key, iv, data):
    if len(data) % 16:
        raise ValueError("ciphertext is not a multiple of 16 bytes")
    rk, prev, out = _expand_key(key), iv, b""
    for i in range(0, len(data), 16):
        blk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(aes_decrypt_block(rk, blk), prev))
        prev = blk
    pad = out[-1]
    if not 1 <= pad <= 16 or out[-pad:] != bytes([pad]) * pad:
        raise ValueError("bad PKCS#7 padding (wrong token?)")
    return out[:-pad]


# ---------------------------------------------------------------------------
# miIO packet format (docs/miio-protocol.md)
# ---------------------------------------------------------------------------
MAGIC = 0x2131
PORT = 54321
HELLO = bytes.fromhex("21310020") + b"\xff" * 28


def md5(b):
    return hashlib.md5(b).digest()


def key_iv(token):
    key = md5(token)
    return key, md5(key + token)


def build_packet(token, device_id, stamp, payload):
    """Encrypt payload (bytes) into a complete miIO packet."""
    key, iv = key_iv(token)
    enc = cbc_encrypt(key, iv, payload)
    header = struct.pack(">HHIII", MAGIC, 32 + len(enc), 0, device_id, stamp)
    checksum = md5(header + token + enc)
    return header + checksum + enc


def parse_packet(token, pkt):
    """Verify and decrypt a reply. Returns (device_id, stamp, payload bytes)."""
    magic, length, _unknown, did, stamp = struct.unpack(">HHIII", pkt[:16])
    if magic != MAGIC:
        raise ValueError("not a miIO packet")
    pkt = pkt[:length]
    if md5(pkt[:16] + token + pkt[32:]) != pkt[16:32]:
        raise ValueError("checksum mismatch (wrong token?)")
    key, iv = key_iv(token)
    payload = cbc_decrypt(key, iv, pkt[32:]) if length > 32 else b""
    return did, stamp, payload.rstrip(b"\x00")


class Device:
    """One device, one request at a time; a fresh hello before each attempt."""

    def __init__(self, ip, token, timeout=2.0, retries=3):
        self.ip, self.token, self.timeout, self.retries = ip, token, timeout, retries
        self.id = random.randint(1, 9000)  # devices answer a repeated id from cache
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)

    def hello(self):
        self.sock.sendto(HELLO, (self.ip, PORT))
        while True:
            data, addr = self.sock.recvfrom(4096)
            if addr[0] == self.ip and len(data) == 32:
                _, _, _, did, stamp = struct.unpack(">HHIII", data[:16])
                return did, stamp

    def send(self, method, params):
        last = None
        for attempt in range(self.retries + 1):
            self.id = self.id % 9999 + 1
            try:
                did, stamp = self.hello()
                req = json.dumps({"id": self.id, "method": method, "params": params},
                                 separators=(",", ":")).encode()
                self.sock.sendto(build_packet(self.token, did, stamp + 1, req), (self.ip, PORT))
                deadline = time.time() + self.timeout
                while time.time() < deadline:
                    data, addr = self.sock.recvfrom(4096)
                    if addr[0] != self.ip or len(data) <= 32:
                        continue
                    _, _, payload = parse_packet(self.token, data)
                    reply = json.loads(payload)
                    if reply.get("id") == self.id:
                        return reply
            except (socket.timeout, ValueError) as e:
                last = e
                print(f"  attempt {attempt + 1}: {e or 'timeout'}", file=sys.stderr)
        raise RuntimeError(f"no answer after {self.retries + 1} attempts ({last or 'timeout'})")


# ---------------------------------------------------------------------------
# Self test and the documented worked example
# ---------------------------------------------------------------------------
VECTOR = {
    "token": "00112233445566778899aabbccddeeff",   # example only, not a real device
    "device_id": 0x12345678,
    "stamp": 0x00001001,
    "payload": '{"id":1,"method":"miIO.info","params":[]}',
}


def make_vector():
    token = bytes.fromhex(VECTOR["token"])
    key, iv = key_iv(token)
    pkt = build_packet(token, VECTOR["device_id"], VECTOR["stamp"], VECTOR["payload"].encode())
    return token, key, iv, pkt


def selftest():
    ok = True
    rk = _expand_key(bytes.fromhex("000102030405060708090a0b0c0d0e0f"))
    ct = aes_encrypt_block(rk, bytes.fromhex("00112233445566778899aabbccddeeff"))
    good = ct.hex() == "69c4e0d86a7b0430d8cdb78070b4c55a"
    print(f"AES-128 FIPS-197 C.1 encrypt: {'ok' if good else 'FAIL ' + ct.hex()}")
    ok &= good
    pt = aes_decrypt_block(rk, ct)
    good = pt.hex() == "00112233445566778899aabbccddeeff"
    print(f"AES-128 FIPS-197 C.1 decrypt: {'ok' if good else 'FAIL'}")
    ok &= good
    token, _, _, pkt = make_vector()
    did, stamp, payload = parse_packet(token, pkt)
    good = (did, stamp, payload.decode()) == (VECTOR["device_id"], VECTOR["stamp"], VECTOR["payload"])
    print(f"packet build -> parse round trip: {'ok' if good else 'FAIL'}")
    ok &= good
    try:
        parse_packet(bytes(16), pkt)
        good = False
    except ValueError:
        good = True
    print(f"wrong token rejected: {'ok' if good else 'FAIL'}")
    ok &= good
    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


def print_vector():
    token, key, iv, pkt = make_vector()
    print(f"token     {token.hex()}")
    print(f"key       {key.hex()}   = MD5(token)")
    print(f"iv        {iv.hex()}   = MD5(key + token)")
    print(f"payload   {VECTOR['payload']}")
    print(f"length    {len(pkt)} bytes (32 header + {len(pkt) - 32} ciphertext)")
    print(f"header    {pkt[:16].hex()}")
    print(f"checksum  {pkt[16:32].hex()}")
    print(f"cipher    {pkt[32:].hex()}")
    print(f"packet    {pkt.hex()}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=["hello", "info", "status", "on", "off", "raw", "selftest", "vector"])
    ap.add_argument("--ip")
    ap.add_argument("--token", default=os.environ.get("MIIO_TOKEN", ""))
    ap.add_argument("--map", default="2,1,2,4,3,4,0",
                    help="power_siid,power_piid,mode_siid,mode_piid,aqi_siid,aqi_piid,auto_value")
    ap.add_argument("--method")
    ap.add_argument("--params", default="[]")
    a = ap.parse_args()

    if a.command == "selftest":
        return selftest()
    if a.command == "vector":
        print_vector()
        return 0
    if not a.ip:
        ap.error("--ip is required")

    if a.command == "hello":
        dev = Device(a.ip, b"")
        for attempt in range(4):
            try:
                did, stamp = dev.hello()
                print(f"device id {did} (0x{did:08x}), uptime stamp {stamp} s")
                return 0
            except socket.timeout:
                print(f"  attempt {attempt + 1}: timeout", file=sys.stderr)
        print("no answer: wrong IP, device off, or a different subnet/VLAN")
        return 1

    if len(a.token) != 32:
        ap.error("token: 32 hex characters via --token or the MIIO_TOKEN environment variable")
    ps, pp, ms, mp, qs, qp, auto = (int(x) for x in a.map.split(","))
    dev = Device(a.ip, bytes.fromhex(a.token))
    t0 = time.time()

    if a.command == "info":
        r = dev.send("miIO.info", [])["result"]
        print(f"model {r.get('model')} | firmware {r.get('fw_ver')} | hardware {r.get('hw_ver')}")
    elif a.command == "status":
        r = dev.send("get_properties", [{"did": "power", "siid": ps, "piid": pp},
                                        {"did": "mode", "siid": ms, "piid": mp},
                                        {"did": "aqi", "siid": qs, "piid": qp}])
        v = {p["did"]: p.get("value") for p in r["result"] if p.get("code") == 0}
        print(f"power {v.get('power')} | mode {v.get('mode')} | PM2.5 {v.get('aqi')} ug/m3")
    elif a.command in ("on", "off"):
        on = a.command == "on"
        r = dev.send("set_properties", [{"did": "power", "siid": ps, "piid": pp, "value": on}])
        print("power:", r["result"])
        if on:  # separate request: the 3C refuses the mode write with -4004 while it is off
            r = dev.send("set_properties", [{"did": "mode", "siid": ms, "piid": mp, "value": auto}])
            print("mode: ", r["result"])
    elif a.command == "raw":
        if not a.method:
            ap.error("raw needs --method")
        r = dev.send(a.method, json.loads(a.params))
        r.get("result", {}).pop("token", None) if isinstance(r.get("result"), dict) else None
        print(json.dumps(r, indent=2))
    print(f"({(time.time() - t0) * 1000:.0f} ms)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
