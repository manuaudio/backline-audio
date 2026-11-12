#!/usr/bin/env python3
"""Send one OSC message to 127.0.0.1:PORT; print any replies as JSON lines.

  oscsend.py PORT /address [ARG...]   ints -> i, numbers with '.' -> f, else s
"""
import json
import socket
import struct
import sys


def pad(b):
    return b + b"\0" * (4 - len(b) % 4)


def encode(addr, args):
    tags, body = ",", b""
    for a in args:
        try:
            v = int(a)
            tags += "i"
            body += struct.pack(">i", v)
            continue
        except ValueError:
            pass
        try:
            v = float(a)
            tags += "f"
            body += struct.pack(">f", v)
            continue
        except ValueError:
            pass
        tags += "s"
        body += pad(a.encode())
    return pad(addr.encode()) + pad(tags.encode()) + body


def decode(b):
    def s(pos):
        end = b.index(b"\0", pos)
        return b[pos:end].decode(), (end + 4) & ~3
    addr, pos = s(0)
    tags, pos = s(pos)
    out = []
    for t in tags[1:]:
        if t == "i":
            out.append(struct.unpack(">i", b[pos:pos + 4])[0]); pos += 4
        elif t == "f":
            out.append(struct.unpack(">f", b[pos:pos + 4])[0]); pos += 4
        elif t == "d":
            out.append(struct.unpack(">d", b[pos:pos + 8])[0]); pos += 8
        elif t == "s":
            v, pos = s(pos); out.append(v)
    return addr, out


port, addr, args = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(0.3)
sock.sendto(encode(addr, args), ("127.0.0.1", port))
try:
    while True:
        data, _ = sock.recvfrom(4096)
        a, vals = decode(data)
        print(json.dumps({"address": a, "args": vals}))
except socket.timeout:
    pass
