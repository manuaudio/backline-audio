#!/usr/bin/env python3
"""OSC test client for the control service proof (UDP and TCP).

  oscclient.py udp HOST PORT [--from ADDR] [--count N] /address [ARG...]
  oscclient.py tcp HOST PORT [--from ADDR] [--secret S] [--raw HEX] /address [ARG...]

Arguments: ints -> i, numbers with '.' -> f, T/F -> true/false, else s.
Prints one JSON line: what was sent, replies, and whether the server closed
the connection (TCP). --count sends the message N times as fast as it can.
"""
import json
import socket
import struct
import sys
import time


def pad(b):
    return b + b"\0" * (4 - len(b) % 4)


def encode(addr, args):
    tags, body = ",", b""
    for a in args:
        if a in ("T", "F"):
            tags += a
            continue
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
    def rstr(p):
        e = b.index(b"\0", p)
        return b[p:e].decode(), (e + 4) & ~3
    addr, p = rstr(0)
    tags, p = rstr(p)
    out = []
    for t in tags[1:]:
        if t == "i":
            out.append(struct.unpack(">i", b[p:p + 4])[0]); p += 4
        elif t == "f":
            out.append(struct.unpack(">f", b[p:p + 4])[0]); p += 4
        elif t == "s":
            s, p = rstr(p); out.append(s)
    return [addr] + out


def main(argv):
    proto, host, port = argv[0], argv[1], int(argv[2])
    rest = argv[3:]
    src, secret, count, raw = None, None, 1, None
    while rest and rest[0].startswith("--"):
        k = rest.pop(0)
        if k == "--from": src = rest.pop(0)
        elif k == "--secret": secret = rest.pop(0)
        elif k == "--count": count = int(rest.pop(0))
        elif k == "--raw": raw = bytes.fromhex(rest.pop(0))
    pkt = raw if raw is not None else encode(rest[0], rest[1:])
    fam = socket.AF_INET6 if ":" in host else socket.AF_INET
    res = {"proto": proto, "to": f"{host}:{port}", "from": src, "msg": rest[:1] + rest[1:] if raw is None else "raw",
           "t_ns": time.monotonic_ns()}
    if proto == "udp":
        s = socket.socket(fam, socket.SOCK_DGRAM)
        if src:
            s.bind((src, 0))
        for _ in range(count):
            s.sendto(pkt, (host, port))
        res["sent"] = count
        print(json.dumps(res))
        return 0
    s = socket.socket(fam, socket.SOCK_STREAM)
    if src:
        s.bind((src, 0))
    s.settimeout(2)
    try:
        s.connect((host, port))
    except OSError as e:
        res["connect"] = str(e)
        print(json.dumps(res))
        return 0
    frame = lambda b: struct.pack(">I", len(b)) + b
    replies, closed = [], False
    try:
        if secret is not None:
            s.sendall(frame(encode("/backline/auth", [secret])))
            hdr = s.recv(4)
            if len(hdr) == 4:
                n = struct.unpack(">I", hdr)[0]
                replies.append(decode(s.recv(n)))
        res["t_ns"] = time.monotonic_ns()
        for _ in range(count):
            s.sendall(frame(pkt))
        # closed by the server? (a read that returns b"" within 0.3 s)
        s.settimeout(0.3)
        try:
            while True:
                hdr = s.recv(4)
                if not hdr:
                    closed = True
                    break
                n = struct.unpack(">I", hdr)[0]
                replies.append(decode(s.recv(n)))
        except socket.timeout:
            pass
    except (BrokenPipeError, ConnectionResetError):
        closed = True
    res["replies"] = replies
    res["closed_by_server"] = closed
    print(json.dumps(res))
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
