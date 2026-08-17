#!/usr/bin/env python3
"""Find Tuya devices on the LAN by listening to their broadcasts.

    ./tools/tuya-discover.py [seconds]

Every Tuya device shouts on UDP 6666/6667 every ~10 seconds. The 6667 payload
is encrypted, but under a key that is the same for every device on earth
(md5 of a string baked into the SDK), so it decodes without credentials and
yields the device id, protocol version and IP.

Run this before anything else. It answers the question that wasted the most
time on this project: *which physical device am I actually talking to?*

There were two Tuya devices on this network. I assumed the light bar was the
one at .242 running protocol 3.4, spent a long stretch failing to speak 3.4 to
it, and then discovered the bar was the 3.3 device at .172 - and that .242
belonged to a different account entirely, which is why every cloud call for it
returned "permission deny". That error was correct and I read it as a bug.

The device id printed here is what the cloud console wants when you go looking
for the local key. The version tells you which protocol dialect to speak: 3.3
has no session handshake, 3.4 does.
"""

import hashlib
import json
import socket
import sys
import time

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    sys.exit("needs cryptography:  pip install cryptography")

# Not a secret: this key is identical on every Tuya device.
UDP_KEY = hashlib.md5(b"yGAdlopoPVldABfn").digest()


def decrypt(payload):
    c = Cipher(algorithms.AES(UDP_KEY), modes.ECB()).decryptor()
    out = c.update(payload) + c.finalize()
    return out[: -out[-1]]


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 25

    socks = []
    for port in (6666, 6667):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", port))
            s.settimeout(0.5)
            socks.append(s)
        except OSError as e:
            print(f"  udp {port}: {e}")
    if not socks:
        sys.exit("could not bind either discovery port")

    print(f"listening {secs}s...")
    seen, end = {}, time.time() + secs
    while time.time() < end:
        for s in socks:
            try:
                data, addr = s.recvfrom(2048)
            except socket.timeout:
                continue
            if addr[0] in seen:
                continue
            body = data[20:-8]
            try:
                info = json.loads(decrypt(body).decode())
            except Exception:
                try:
                    info = json.loads(body.decode())
                except Exception:
                    info = {}
            seen[addr[0]] = info
            print(f"\n  {addr[0]}")
            for k in ("gwId", "devId", "productKey", "version", "active"):
                if k in info:
                    print(f"      {k:11} {info[k]}")

    print(f"\n{len(seen)} device(s)." if seen else "\nnothing heard.")
    if seen:
        print("The local key is not broadcast - get it from the Tuya IoT "
              "console for the project your Smart Life account is linked to.")


if __name__ == "__main__":
    main()
