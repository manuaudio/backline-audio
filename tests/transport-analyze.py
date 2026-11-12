#!/usr/bin/env python3
"""Analyse a beat-click take from tests/transport-proof.

usage: transport-analyze.py LOG TAP UNITS LABEL [LOCATES]
  LOG    backline-test-timecode log with C (observer) and A (tap) lines
  TAP    the tap's float32 interleaved stereo samples
  UNITS  qn   the plugin counts quarter notes (CLAP, VST3)
         beat the plugin counts the meter's beats (LV2)
  LOCATES  comma-separated frames the proof located to: a transport jump
           anywhere else means another clock moved it, and the take is void

Per cycle, the observer line (C) and the tap line (A) of the same graph
position say what PipeWire's segment held and what the plugin produced in
that cycle (the plugin's node runs before the tap in the same cycle).

1. Exact agreement: the clicks the plugin made against the clicks the
   segment's beat (bar.beat + bar.offset x bpm / 60 / rate) puts there,
   sample for sample, simulating the plugin's own rule.
2. Against clockd's tempo map: within each stretch of constant tempo and
   meter while rolling without a jump, the beat is linear in the frame, so
   beat k must land on the first frame at or after
   f0 + (k - b0) x 60 / bpm x rate. Error per click, in samples.
3. The same rule fed only the tick-floored bar.beat (no offset), to show what
   the offset buys.
"""
import math
import struct
import sys

log, tap, units, label = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
RATE = 48000

def foreign_moves(cyc, allowed):
    """Transport jumps to a frame the proof didn't ask for: another clock
    moved this transport, and the take is void."""
    bad, prev = [], None
    for c in cyc:
        if prev is not None:
            expect = prev["frame"] + (prev["n"] if prev["roll"] else 0)
            gap_ok = prev["roll"] and c["roll"] and c["frame"] - prev["frame"] == c["pos"] - prev["pos"]
            stop_or_start = c["frame"] in (prev["frame"], prev["frame"] + prev["n"])
            if c["frame"] != expect and not gap_ok and not stop_or_start and c["frame"] not in allowed:
                bad.append((prev["frame"], c["frame"]))
        prev = c
    return bad
obs, cyc = {}, []
for line in open(log):
    r = line.split()
    if r[0] == "C":
        obs[int(r[1])] = dict(roll=int(r[2]), frame=int(r[3]), valid=int(r[4]), bpm=float(r[5]), beat=float(r[6]),
                              num=int(r[7]), den=int(r[8]), off=int(r[10]) if len(r) > 10 else 0)
    elif r[0] == "A":
        cyc.append(dict(pos=int(r[1]), n=int(r[2]), frame=int(r[3]), roll=int(r[4]), drop=int(r[5])))
allowed = set(int(x) for x in sys.argv[5].split(",")) if len(sys.argv) > 5 else None
moves = foreign_moves(cyc, allowed) if allowed is not None else []
if moves:
    print(f"  [{label}] VOID: the transport jumped where this proof never located it: {moves[:4]} (another clock on this machine)")
    sys.exit(2)
data = open(tap, "rb").read()
samples = struct.unpack(f"<{len(data)//4}f", data)

def song_beat(o):
    # pipewire-jack's bar.beat is one bar ahead of the song (JACK's 1-based
    # bar x beats per bar); bar.offset is the frames since that tick
    return o["beat"] - o["num"] + o["off"] * o["bpm"] / 60.0 / RATE

def scale(o):
    return 4.0 / o["den"] if units == "qn" else 1.0

# walk the cycles: actual and expected clicks, keyed by (graph pos of sample)
actual, expect, expect_tick = [], [], []
k = 0
state = {"full": None, "tick": None}   # last (beat, step) of the plugin's rule
drops = missing = 0
prevc = None    # (frame, n, tempo key) of the previous rolling cycle
beatline = []   # (frame, plugin's beat output at sample 0) for a sanity check
for c in cyc:
    if c["drop"]:
        drops += 1
        state = {"full": None, "tick": None}
        prevc = None
        continue
    o = obs.get(c["pos"])
    n = c["n"]
    ch = samples[k * 2:(k + n) * 2]
    k += n
    for i in range(n):
        if ch[2 * i] > 0.5:
            actual.append((c["pos"] + i, c["frame"] + i, o and (o["bpm"], o["num"], o["den"])))
    if o is None:
        missing += 1
        continue
    if not (o["roll"] and o["valid"]):
        state = {"full": None, "tick": None}
        prevc = None
        continue
    beatline.append((c["frame"], ch[1], o))
    s = scale(o)
    step = o["bpm"] / 60.0 / RATE * s
    # LV2 gets a time:Position only when the transport changes (the worker's
    # rule: start, stop, locate, tempo or meter) and counts by itself in
    # between; CLAP and VST3 get the segment's beat every cycle.
    key_t = (o["bpm"], o["num"], o["den"])
    cont = prevc is not None and c["frame"] == prevc[0] + prevc[1] and key_t == prevc[2]
    prevc = (c["frame"], n, key_t)
    for key, b0 in (("full", song_beat(o) * s), ("tick", (o["beat"] - o["num"]) * s)):
        last = state[key]
        if units == "beat" and cont and last:
            b0 = last[0] + last[1]
        elif cont and last and 0 < (last[0] + last[1]) - b0 < step * 1.000001:
            b0 = last[0] + last[1]   # the worker's carry-forward (host/transport.h)
        if last and abs(b0 - (last[0] + last[1])) > 1e-6:
            last = None
        out = expect if key == "full" else expect_tick
        for i in range(n):
            b = b0 + i * step
            hit = (math.floor(b + 1e-9) > math.floor(last[0] + 1e-9)) if last else abs(b - math.floor(b + 1e-9)) < 1e-9
            if hit:
                out.append(c["pos"] + i)
            last = (b, step)
        state[key] = last

a_pos = [p for p, _, _ in actual]
same = sorted(set(a_pos) & set(expect))
print(f"  [{label}] {len(cyc)} cycles tapped ({drops} dropped, {missing} without an observer line), {len(a_pos)} clicks")
print(f"  [{label}] 1. clicks where the segment's beat says, sample for sample: {len(same)} of {len(expect)} expected,"
      f" {len(set(a_pos) - set(expect))} unexpected")
ex1 = len(same) == len(expect) == len(a_pos) and len(a_pos) > 0

# 2. constant-tempo stretches: anchor at the stretch's first rolling cycle
stretches, cur, prev = [], None, None
gaps_seen = 0
for c in cyc:
    o = obs.get(c["pos"])
    if not o or not (o["roll"] and o["valid"]) or c["drop"]:
        cur, prev = None, None
        continue
    key = (o["bpm"], o["num"], o["den"])
    # a missed graph cycle (the transport rolled on, this node didn't run)
    # doesn't end the stretch: frame and graph position moved together
    cont = prev is not None and c["frame"] - prev[0] == c["pos"] - prev[2]
    if prev is not None and cont and c["frame"] != prev[0] + prev[1]:
        gaps_seen += 1
    if cur is None or key != cur["key"] or not cont:
        s = scale(o)
        cur = dict(key=key, f0=c["frame"], b0=song_beat(o) * s, s=s, clicks=[], end=None, p0=c["pos"])
        stretches.append(cur)
    else:
        # every cycle's beat is at or below the true one by under a frame
        # (the floored offset); the stretch's true anchor is the largest
        # of them carried back to its start
        s = scale(o)
        cur["b0"] = max(cur["b0"], song_beat(o) * s - (c["frame"] - cur["f0"]) * o["bpm"] / 60.0 / RATE * s)
    cur["end"] = c["frame"] + c["n"]
    cur["p_end"] = c["pos"] + c["n"]
    prev = (c["frame"], c["n"], c["pos"])
fr_of = {p: f for p, f, _ in actual}
worst = 0
for st in stretches:
    bpm, num, den = st["key"]
    spb = 60.0 / (bpm * st["s"]) * RATE      # samples per counted beat
    errs, gaps = [], []
    cl = [f for p, f, _ in actual if st["p0"] <= p < st["p_end"]]   # by graph position: a take can revisit frames
    for f in cl:
        kk = round(st["b0"] + (f - st["f0"]) / spb)
        ideal = math.ceil(st["f0"] + (kk - st["b0"]) * spb - 1e-6)
        errs.append(f - ideal)
    gaps = [b - a for a, b in zip(cl, cl[1:])]
    on_beat = abs(st["b0"] - round(st["b0"])) < 1e-6
    first = f" starts on beat {round(st['b0'])}: click on its first frame {'yes' if st['f0'] in cl else 'NO'};" if on_beat else ""
    if on_beat and st["f0"] not in cl:
        worst = max(worst, 99)
    if errs:
        worst = max(worst, max(abs(e) for e in errs))
    unit = "quarter" if units == "qn" else f"1/{den}"
    print(f"  [{label}] 2. {bpm:g} BPM {num}/{den}, frames {st['f0']}-{st['end']}: {len(cl)} clicks, one per {unit} note"
          f" ({spb:.1f} samples);{first} vs the tempo map: error min {min(errs, default=0)} max {max(errs, default=0)} samples;"
          f" spacing {min(gaps, default=0)}-{max(gaps, default=0)}")
if gaps_seen:
    print(f"  [{label}]    graph cycles this node missed while rolling (no realtime priority, a loaded machine): {gaps_seen}")
tick_same = len(set(a_pos) & set(expect_tick))
# how far the tick-only clicks would be from the real ones
dist = []
for p in expect_tick:
    d = min((abs(p - q) for q in a_pos), default=None)
    if d is not None and d < 2000:
        dist.append(d)
print(f"  [{label}] 3. without bar.offset (tick-floored beat only): {tick_same} of {len(expect_tick)} clicks on the same sample;"
      f" the rest {min(dist, default=0)}-{max(dist, default=0)} samples off")
# the plugin's own beat output (float) against the segment's beat
bw = 0
for f, b, o in beatline:
    s = scale(o)
    want = song_beat(o) * s
    bw = max(bw, abs(b - want) * 60.0 / (o["bpm"] * s) * RATE)
print(f"  [{label}] the plugin's own beat output vs the segment's per-cycle beat (floored to whole frames): worst {bw:.3f} samples")
ok = ex1 and worst <= 1 and len(stretches) >= 2
print(f"  [{label}] RESULT:", "PASS" if ok else "FAIL", "(the plugin clicks on clockd's beats, to the sample)")
sys.exit(0 if ok else 1)
