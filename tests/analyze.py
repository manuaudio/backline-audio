#!/usr/bin/env python3
"""Measurement helpers for the Backline proofs. Pure Python (no numpy).

  analyze.py info FILE                  format, length, BWF bext chunk
  analyze.py notes FILE [--thresh dB]   sounding segments, each with f0 by FFT
  analyze.py level FILE START END       RMS dBFS per channel over [START, END) s
  analyze.py windows FILE START END MS  min/max windowed RMS (dropout check)
  analyze.py align A B                  lag between two recordings, and diff
  analyze.py match A B [CH_A CH_B]      where B sits inside A, sample-exact, and how many differ
  analyze.py cues FILE                  WAV cue points with their labels
All output is JSON.
"""
import cmath
import json
import math
import struct
import sys


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] not in (b"RIFF", b"RF64") or data[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file")
    pos, fmt, raw, bext = 12, None, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            raw = body
        elif cid == b"bext":
            bext = {
                "description": body[0:256].split(b"\0")[0].decode(errors="replace"),
                "originator": body[256:288].split(b"\0")[0].decode(errors="replace"),
                "originator_reference": body[288:320].split(b"\0")[0].decode(errors="replace"),
                "origination_date": body[320:330].decode(errors="replace"),
                "origination_time": body[330:338].decode(errors="replace"),
                "time_reference": struct.unpack("<Q", body[338:346])[0],
                "version": struct.unpack("<H", body[346:348])[0],
            }
        pos += 8 + size + (size & 1)
    tag, ch, rate, bits = fmt
    if tag == 3 and bits == 32:
        samples = struct.unpack("<%df" % (len(raw) // 4), raw)
    elif tag == 1 and bits == 16:
        samples = [s / 32768.0 for s in struct.unpack("<%dh" % (len(raw) // 2), raw)]
    elif tag == 1 and bits == 24:
        samples = [int.from_bytes(raw[i:i + 3], "little", signed=True) / 8388608.0
                   for i in range(0, len(raw) - 2, 3)]
    else:
        raise SystemExit(f"{path}: unsupported format tag={tag} bits={bits}")
    chans = [list(samples[c::ch]) for c in range(ch)]
    return {"rate": rate, "channels": ch, "frames": len(chans[0]), "data": chans, "bext": bext,
            "format": {1: "pcm", 3: "float"}.get(tag, str(tag)), "bits": bits}


def db(x):
    return 20 * math.log10(x) if x > 1e-12 else -240.0


def rms(seg):
    return math.sqrt(sum(v * v for v in seg) / len(seg)) if seg else 0.0


def fft(x):
    n = len(x)
    a = list(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        half = size // 2
        for start in range(0, n, size):
            wk = 1
            for k in range(half):
                t = wk * a[start + k + half]
                a[start + k + half] = a[start + k] - t
                a[start + k] += t
                wk *= w
        size *= 2
    return a


def f0(seg, rate, lo=40.0, hi=4000.0):
    """Fundamental by FFT: Hann window, strongest peak, Gaussian interpolation,
    then the lowest strong sub-multiple (so a saw's 2nd harmonic can't win)."""
    n = 1
    while n * 2 <= len(seg) and n < 32768:
        n *= 2
    seg = seg[:n]
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1)) for i in range(n)]
    spec = [abs(v) for v in fft([s * w for s, w in zip(seg, win)])[: n // 2]]
    b_lo, b_hi = int(lo * n / rate), int(hi * n / rate)

    def refine(k):
        a, b, c = (math.log(max(spec[k + d], 1e-20)) for d in (-1, 0, 1))
        den = a - 2 * b + c
        return (k + (0.5 * (a - c) / den if den else 0.0)) * rate / n

    peak = max(range(b_lo, b_hi), key=lambda k: spec[k])
    best = peak
    for div in (2, 3, 4):
        k = round(peak / div)
        if k > b_lo:
            k = max(range(k - 2, k + 3), key=lambda i: spec[i])
            if spec[k] > 0.25 * spec[peak]:
                best = k
    return refine(best), db(spec[best] / (n / 4))


def mono(w):
    return [max(vals, key=abs) for vals in zip(*w["data"])] if w["channels"] > 1 else w["data"][0]


def cmd_info(path):
    w = read_wav(path)
    return {k: w[k] for k in ("rate", "channels", "frames", "format", "bits", "bext")} | {
        "seconds": w["frames"] / w["rate"]}


def cmd_notes(path, thresh=-50.0, win_ms=5.0):
    w = read_wav(path)
    rate, x = w["rate"], mono(w)
    hop = int(rate * win_ms / 1000)
    env = [db(rms(x[i:i + hop])) for i in range(0, len(x) - hop, hop)]
    segs, start = [], None
    for i, e in enumerate(env + [-240.0]):
        if e > thresh and start is None:
            start = i
        elif e <= thresh and start is not None:
            if (i - start) * win_ms > 50:
                segs.append((start * hop, i * hop))
            start = None
    out = []
    for a, b in segs:
        mid = (a + b) // 2
        span = min(16384, (b - a) // 2)
        freq, _ = f0(x[mid - span // 2: mid + span // 2], rate)
        # silence after the segment: windowed RMS from 20 ms after its end to 200 ms after
        tail = x[b + int(0.02 * rate): b + int(0.2 * rate)]
        out.append({"start_s": round(a / rate, 4), "end_s": round(b / rate, 4),
                    "rms_dbfs": round(db(rms(x[a + int(0.05 * rate): b - int(0.05 * rate)] or x[a:b])), 2),
                    "f0_hz": round(freq, 3),
                    "after_end_dbfs": round(db(rms(tail)), 1) if tail else None})
    return {"rate": rate, "threshold_dbfs": thresh, "segments": out}


def cmd_level(path, start, end):
    w = read_wav(path)
    a, b = int(float(start) * w["rate"]), int(float(end) * w["rate"])
    return {"rms_dbfs": [round(db(rms(c[a:b])), 2) for c in w["data"]],
            "peak_dbfs": [round(db(max((abs(v) for v in c[a:b]), default=0)), 2) for c in w["data"]]}


def cmd_windows(path, start, end, ms):
    w = read_wav(path)
    x = mono(w)
    hop = int(w["rate"] * float(ms) / 1000)
    a, b = int(float(start) * w["rate"]), int(float(end) * w["rate"])
    vals = [db(rms(x[i:i + hop])) for i in range(a, b - hop, hop)]
    zeros = max_run = run = 0
    for v in x[a:b]:
        run = run + 1 if v == 0.0 else 0
        max_run = max(max_run, run)
    return {"windows": len(vals), "window_ms": float(ms), "min_dbfs": round(min(vals), 2),
            "max_dbfs": round(max(vals), 2), "longest_exact_zero_run_samples": max_run}


def onsets(x, thresh=0.1, gap=2000):
    """Rising edges: a sample above thresh after >= gap samples below it."""
    out, last_above = [], -gap
    for i, v in enumerate(x):
        if abs(v) > thresh:
            if i - last_above >= gap:
                out.append(i)
            last_above = i
    return out


def cmd_align(pa, pb):
    a, b = read_wav(pa), read_wav(pb)
    xa, xb = a["data"][0], b["data"][0]
    oa, ob = onsets(xa), onsets(xb)
    # candidate lag from the first onsets, refined by exact search +/- 64
    guess = (ob[0] - oa[0]) if oa and ob else 0
    best, best_err = guess, None
    n = min(len(xa), len(xb))
    for lag in range(guess - 64, guess + 65):
        lo, hi = max(0, -lag), min(n, n - lag)
        idx = range(max(lo, oa[0] - 4800 if oa else lo), min(hi, (oa[0] if oa else lo) + 48000))
        err = sum(abs(xa[i] - xb[i + lag]) for i in idx if 0 <= i + lag < len(xb))
        if best_err is None or err < best_err:
            best, best_err = lag, err
    lo, hi = max(0, -best), min(len(xa), len(xb) - best)
    diff = max((abs(xa[i] - xb[i + best]) for i in range(lo, hi)), default=None)
    sig = [i for i in range(lo, hi) if abs(xa[i]) > 1e-6]
    return {"frames_a": len(xa), "frames_b": len(xb), "onsets_a": oa[:8], "onsets_b": ob[:8],
            "lag_samples": best, "max_abs_diff_at_lag": diff,
            "identical_samples_compared": hi - lo, "nonzero_samples": len(sig),
            "bext_time_reference": [a["bext"] and a["bext"]["time_reference"], b["bext"] and b["bext"]["time_reference"]]}


def find_in(xa, xb, start=0):
    """Offset of xb (from its first non-silent stretch) inside xa, by an exact
    4-sample key. Returns (offset of xb[0] in xa, key index) or (None, None)."""
    k = next((i for i in range(start, len(xb) - 4) if all(abs(v) > 1e-4 for v in xb[i:i + 4])), None)
    if k is None:
        return None, None
    key = tuple(xb[k:k + 4])
    for i in range(len(xa) - 4):
        if xa[i] == key[0] and tuple(xa[i:i + 4]) == key:
            return i - k, k
    return None, k


def cmd_match(pa, pb, ca="0", cb="0"):
    a, b = read_wav(pa), read_wav(pb)
    xa, xb = a["data"][int(ca)], b["data"][int(cb)]
    off, k = find_in(xa, xb)
    if off is None:
        return {"found": False, "frames_a": len(xa), "frames_b": len(xb)}
    lo, hi = max(0, -off), min(len(xb), len(xa) - off)
    bad = [i for i in range(lo, hi) if xa[i + off] != xb[i]]
    return {"found": True, "offset": off, "compared": hi - lo, "frames_a": len(xa), "frames_b": len(xb),
            "mismatches": len(bad), "first_mismatch": bad[0] if bad else None,
            "nonzero": sum(1 for i in range(lo, hi) if xb[i] != 0.0)}


def cmd_cues(path):
    with open(path, "rb") as f:
        data = f.read()
    pos, cues, labels = 12, [], {}
    if data[:4] == b"RF64":
        pass
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"data" and data[:4] == b"RF64" and size == 0xFFFFFFFF:
            size = struct.unpack("<Q", data[28:36])[0]  # ds64 dataSize
        body = data[pos + 8:pos + 8 + size]
        if cid == b"cue ":
            n = struct.unpack("<I", body[:4])[0]
            for i in range(n):
                ident, position, _, _, _, off = struct.unpack("<II4sIII", body[4 + 24 * i:28 + 24 * i])
                cues.append({"id": ident, "frame": off})
        elif cid == b"LIST" and body[:4] == b"adtl":
            q = 4
            while q + 8 <= len(body):
                sid, ssz = body[q:q + 4], struct.unpack("<I", body[q + 4:q + 8])[0]
                if sid == b"labl":
                    labels[struct.unpack("<I", body[q + 8:q + 12])[0]] = body[q + 12:q + 8 + ssz].split(b"\0")[0].decode()
                q += 8 + ssz + (ssz & 1)
        pos += 8 + size + (size & 1)
    for c in cues:
        c["label"] = labels.get(c["id"])
    return {"container": data[:4].decode(), "cues": cues}


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "notes" and "--thresh" in args:
        i = args.index("--thresh")
        res = cmd_notes(args[0], float(args[i + 1]))
    else:
        res = {"info": cmd_info, "notes": cmd_notes, "level": cmd_level,
               "windows": cmd_windows, "align": cmd_align,
               "match": cmd_match, "cues": cmd_cues}[cmd](*args)
    print(json.dumps(res, indent=1))
