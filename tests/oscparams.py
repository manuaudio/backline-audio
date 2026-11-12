#!/usr/bin/env python3
"""OSC client for the Params1 proof (docs/api/Params1.md): sends a script of
messages over UDP (or TCP with OSC 1.0 framing) and prints every reply.

  oscparams.py udp|tcp HOST PORT [--secret S] [--wait SECONDS] MSG...

Each MSG is one string: "/address arg ..." (ints -> i, numbers with '.' ->
f, T/F, else s), or "sleep SECONDS". Replies are read until --wait seconds
after the last message. Bundles are flattened. Prints one JSON object:
{"replies": [{"t": monotonic_ns, "addr": ..., "args": [...]}, ...],
 "sent_t": [monotonic_ns of each message sent]}.
"""
import json
import select
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
            body += struct.pack(">i", int(a)); tags += "i"; continue
        except ValueError:
            pass
        try:
            body += struct.pack(">f", float(a)); tags += "f"; continue
        except ValueError:
            pass
        tags += "s"
        body += pad(a.encode())
    return pad(addr.encode()) + pad(tags.encode()) + body


def rstr(b, p):
    e = b.index(b"\0", p)
    return b[p:e].decode(errors="replace"), (e + 4) & ~3


def decode(b, out, t):
    if b.startswith(b"#bundle\0"):
        p = 16
        while p + 4 <= len(b):
            n = struct.unpack(">i", b[p:p + 4])[0]
            decode(b[p + 4:p + 4 + n], out, t)
            p += 4 + n
        return
    addr, p = rstr(b, 0)
    args = []
    if p < len(b):
        tags, p = rstr(b, p)
        for c in tags[1:]:
            if c == "i":
                args.append(struct.unpack(">i", b[p:p + 4])[0]); p += 4
            elif c == "f":
                args.append(struct.unpack(">f", b[p:p + 4])[0]); p += 4
            elif c == "s":
                s, p = rstr(b, p); args.append(s)
            elif c in "TF":
                args.append(c == "T")
    out.append({"t": t, "addr": addr, "args": args})


def main():
    a = sys.argv[1:]
    proto, host, port = a[0], a[1], int(a[2])
    a = a[3:]
    secret, wait = None, 0.5
    while a and a[0].startswith("--"):
        if a[0] == "--secret":
            secret = a[1]; a = a[2:]
        elif a[0] == "--wait":
            wait = float(a[1]); a = a[2:]
        else:
            sys.exit("unknown option " + a[0])
    tcp = proto == "tcp"
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM if tcp else socket.SOCK_DGRAM)
    s.connect((host, port))
    replies, sent_t, buf = [], [], b""

    def send(pkt):
        if tcp:
            s.sendall(struct.pack(">I", len(pkt)) + pkt)
        else:
            s.send(pkt)

    def drain(until):
        nonlocal buf
        while True:
            left = until - time.monotonic()
            if left <= 0:
                return
            r, _, _ = select.select([s], [], [], left)
            if not r:
                return
            try:
                d = s.recv(65536)
            except ConnectionRefusedError:
                return
            t = time.monotonic_ns()
            if not d:
                return
            if tcp:
                buf += d
                while len(buf) >= 4:
                    n = struct.unpack(">I", buf[:4])[0]
                    if len(buf) < 4 + n:
                        break
                    decode(buf[4:4 + n], replies, t)
                    buf = buf[4 + n:]
            else:
                decode(d, replies, t)

    if secret:
        send(encode("/backline/auth", [secret]))
        drain(time.monotonic() + 0.2)
    for m in a:
        parts = m.split(" ")
        if parts[0] == "sleep":
            drain(time.monotonic() + float(parts[1]))
            continue
        sent_t.append(time.monotonic_ns())
        send(encode(parts[0], parts[1:]))
        drain(time.monotonic() + 0.005)
    drain(time.monotonic() + wait)
    print(json.dumps({"replies": replies, "sent_t": sent_t}))


main()
