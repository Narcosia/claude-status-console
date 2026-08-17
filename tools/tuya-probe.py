#!/usr/bin/env python3
"""Talk to the light over the LAN, the same way the firmware does.

This is the tool that produced the datapoint table in README.md. The cloud DP
spec for this product is wrong in three places, and the only way to find that
out was to ask the device.

    ./tools/tuya-probe.py query                 # dump every datapoint
    ./tools/tuya-probe.py set 20 true           # switch on
    ./tools/tuya-probe.py set 21 scene          # hand back to the light's engine
    ./tools/tuya-probe.py set 24 00ff03e803e8   # HSV hex: hue, sat, val
    ./tools/tuya-probe.py watch 30              # poll for 30s, print changes

Credentials come from config.h - nothing is hardcoded here. An earlier set of
probe scripts had the local key pasted into each one, which is why they had to
be destroyed rather than kept.

Protocol 3.3, mirroring lights.cpp:
  - AES-128-ECB under the device's local key, PKCS#7 padded
  - 55AA <seq> <cmd> <len> <payload> <crc32> 0000AA55
  - CONTROL (7) carries a 15-byte plaintext "3.3" header before the ciphertext,
    DP_QUERY (10) does NOT. Get that backwards and the device ignores you in
    silence, which is a genuinely miserable thing to debug.
"""

import binascii
import json
import re
import socket
import struct
import sys
import time
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    sys.exit("needs cryptography:  pip install cryptography")

PREFIX = 0x000055AA
SUFFIX = 0x0000AA55
CMD_CONTROL = 7
CMD_DP_QUERY = 10
PORT = 6668

CONFIG = Path(__file__).resolve().parent.parent / "config.h"


def load_config():
    """Read LIGHT_IP / LIGHT_ID / LIGHT_KEY out of config.h."""
    if not CONFIG.exists():
        sys.exit(f"{CONFIG} not found - copy config.example.h and fill it in")
    text = CONFIG.read_text()
    out = {}
    for name in ("LIGHT_IP", "LIGHT_ID", "LIGHT_KEY"):
        m = re.search(rf'#define\s+{name}\s+"([^"]*)"', text)
        out[name] = m.group(1) if m else None
    if not all(out.values()):
        sys.exit("config.h has no light configured (LIGHT_KEY may be nullptr)")
    if len(out["LIGHT_KEY"]) != 16:
        sys.exit("LIGHT_KEY must be exactly 16 characters")
    return out


def pad(data):
    n = 16 - (len(data) % 16)
    return data + bytes([n]) * n


def unpad(data):
    return data[: -data[-1]] if data and 0 < data[-1] <= 16 else data


def encrypt(key, plain):
    c = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return c.update(pad(plain)) + c.finalize()


def decrypt(key, blob):
    c = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return unpad(c.update(blob) + c.finalize())


_seq = [1]


def build(cmd, key, payload_json):
    body = encrypt(key, payload_json.encode())
    if cmd == CMD_CONTROL:
        body = b"3.3" + b"\x00" * 12 + body      # version header, plaintext
    head = struct.pack(">IIII", PREFIX, _seq[0], cmd, len(body) + 8)
    _seq[0] += 1
    crc = binascii.crc32(head + body) & 0xFFFFFFFF
    return head + body + struct.pack(">II", crc, SUFFIX)


def parse(key, data):
    """Pull the JSON out of a reply frame, tolerating the return-code prefix."""
    i = data.find(b"\x00\x00\x55\xaa")
    if i < 0:
        return None
    length = struct.unpack(">I", data[i + 12 : i + 16])[0]
    body = data[i + 16 : i + 16 + length - 8]
    if body[:4] == b"\x00\x00\x00\x00":          # success return code
        body = body[4:]
    if body[:3] == b"3.3":
        body = body[15:]
    if not body:
        return None
    try:
        return json.loads(decrypt(key, body).decode())
    except Exception:
        try:
            return json.loads(body.decode())
        except Exception:
            return {"raw": body.hex()}


def send(cfg, cmd, payload):
    key = cfg["LIGHT_KEY"].encode()
    pkt = build(cmd, key, json.dumps(payload, separators=(",", ":")))
    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect((cfg["LIGHT_IP"], PORT))
        s.send(pkt)
        return parse(key, s.recv(4096))
    except socket.timeout:
        return None
    finally:
        s.close()


def query(cfg):
    return send(cfg, CMD_DP_QUERY,
                {"gwId": cfg["LIGHT_ID"], "devId": cfg["LIGHT_ID"],
                 "uid": cfg["LIGHT_ID"], "t": str(int(time.time()))})


def control(cfg, dp, value):
    return send(cfg, CMD_CONTROL,
                {"devId": cfg["LIGHT_ID"], "uid": cfg["LIGHT_ID"],
                 "t": str(int(time.time())), "dps": {str(dp): value}})


def coerce(text):
    if text.lower() == "true":
        return True
    if text.lower() == "false":
        return False
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    return text


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cfg = load_config()
    action = sys.argv[1]

    if action == "query":
        r = query(cfg)
        print(json.dumps(r, indent=2) if r else "no reply (offline, or wrong key)")

    elif action == "set":
        if len(sys.argv) < 4:
            sys.exit("usage: set <dp> <value>")
        dp, value = sys.argv[2], coerce(sys.argv[3])
        r = control(cfg, dp, value)
        print(f"dp{dp} = {value!r} ->", json.dumps(r) if r else "no reply")
        # A reply is not acceptance: dp 21 answers cheerfully and changes
        # nothing. Read it back before believing it.
        time.sleep(0.4)
        back = query(cfg)
        if back and "dps" in back:
            print(f"reads back as: {back['dps'].get(str(dp))!r}")

    elif action == "watch":
        secs = int(sys.argv[2]) if len(sys.argv) > 2 else 20
        print(f"watching {secs}s - change something in the app to see the DP move")
        last, end = None, time.time() + secs
        while time.time() < end:
            r = query(cfg)
            dps = r.get("dps") if r else None
            if dps and dps != last:
                print(f"{time.strftime('%H:%M:%S')}  {json.dumps(dps)}")
                last = dps
            time.sleep(1)
        # Twenty seconds of this during a running scene returned one unchanging
        # value: the animation happens inside the light and is invisible from
        # the LAN, so there is nothing to capture and replay.
        print("done")

    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
