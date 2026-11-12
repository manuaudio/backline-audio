#!/usr/bin/env python3
"""Onsets of a real tempo-synced plugin against clockd's beat grid.

usage: transport-onsets.py LOG TAP STEP LABEL [THRESHOLD [LOCATES]]
  LOG, TAP   as for transport-analyze.py (observer + tap of the plugin's output)
  STEP       the grid in beats (0.5: eighth notes in 4/4)
  LOCATES    frames the proof located to (see transport-analyze.py)

An onset is the first sample above THRESHOLD (default 0.01) after at least
20 ms below it. The grid comes from clockd's tempo map as PipeWire's segment
carries it: within each stretch of constant tempo while rolling, beat k x STEP
is at f0 + (k x STEP - b0) x 60 / bpm x rate. Each onset is matched to the
nearest grid point; a synth's attack and its own block or event handling
show up as a constant offset, a transport that it doesn't follow as a drift
or as onsets off the grid.
"""
import math
import struct
import sys

log, tap, step, label = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
thr = float(sys.argv[5]) if len(sys.argv) > 5 else 0.01
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
allowed = set(int(x) for x in sys.argv[6].split(",")) if len(sys.argv) > 6 else None
moves = foreign_moves(cyc, allowed) if allowed is not None else []
if moves:
    print(f"  [{label}] VOID: the transport jumped where this proof never located it: {moves[:4]} (another clock on this machine)")
    sys.exit(2)
data = open(tap, "rb").read()
left = struct.unpack(f"<{len(data)//4}f", data)[0::2]

def song_beat(o):
    return o["beat"] - o["num"] + o["off"] * o["bpm"] / 60.0 / RATE

stretches, cur, prev, k = [], None, None, 0
gaps_seen = 0
onsets, quiet, rolled_at = [], RATE, None
for c in cyc:
    n = c["n"]
    o = obs.get(c["pos"])
    seg = left[k:k + n]
    k += n
    rolling = bool(o and o["roll"] and o["valid"] and not c["drop"])
    for i, v in enumerate(seg):
        if abs(v) > thr:
            if quiet >= 0.02 * RATE:
                onsets.append((c["frame"] + i, rolling, c["pos"] + i))
            quiet = 0
        else:
            quiet += 1
    if not rolling:
        cur = prev = None
        continue
    key = (o["bpm"], o["num"], o["den"])
    # a missed graph cycle (the transport rolled on, this node didn't run)
    # doesn't end the stretch: frame and graph position moved together
    cont = prev is not None and c["frame"] - prev[0] == c["pos"] - prev[2]
    if prev is not None and cont and c["frame"] != prev[0] + prev[1]:
        gaps_seen += 1
    per = o["bpm"] / 60.0 / RATE
    if cur is None or key != cur["key"] or not cont:
        cur = dict(key=key, f0=c["frame"], b0=song_beat(o), end=None, per=per, p0=c["pos"])
        stretches.append(cur)
    else:
        cur["b0"] = max(cur["b0"], song_beat(o) - (c["frame"] - cur["f0"]) * per)
    cur["end"] = c["frame"] + n
    cur["p_end"] = c["pos"] + n
    prev = (c["frame"], n, c["pos"])

before = [f for f, r, _ in onsets if not r]
print(f"  [{label}] onsets while the transport was stopped (a held note, arpeggiator on): {len(before)}")
offsets_all = []
for st in stretches:
    bpm = st["key"][0]
    spb = 1.0 / st["per"]                # samples per beat
    on = [f for f, r, p in onsets if r and st["p0"] <= p < st["p_end"]]
    offs, idx = [], []
    for f in on:
        b = st["b0"] + (f - st["f0"]) * st["per"]
        g = round(b / step)              # nearest grid step
        gf = st["f0"] + (g * step - st["b0"]) * spb
        offs.append(f - gf)
        idx.append(g)
    if not offs:
        continue
    missing = (max(idx) - min(idx) + 1) - len(set(idx))
    offsets_all += offs
    spread = max(offs) - min(offs)
    print(f"  [{label}] {bpm:g} BPM: {len(on)} onsets on {len(set(idx))} consecutive grid steps of {step:g} beat"
          f" ({step * spb:.0f} samples), {missing} steps missing; onset - grid: mean {sum(offs) / len(offs):.1f},"
          f" min {min(offs):.1f}, max {max(offs):.1f} samples (spread {spread:.1f})")
    st["offs"] = offs
    st["missing"] = missing
quantum = max((c["n"] for c in cyc), default=0)
if gaps_seen:
    print(f"  [{label}] graph cycles this node missed while rolling (no realtime priority, a loaded machine): {gaps_seen}")
# Following the transport means: no drift within a stretch (a plugin on its
# own clock at another tempo drifts by thousands of samples in a few steps),
# no step missing, and every onset well inside its step (within 1/8 of it).
# The offset itself is the plugin's own (attack, block handling); it is
# reported, not judged. A plugin may re-phase by up to a cycle when it gets
# a new position mid-roll (Odin2's LV2 build does, after a missed cycle), so
# the spread allowed is two cycles.
ok = len(stretches) >= 2
for st in stretches:
    offs = st.get("offs") or []
    step_samples = step / st["per"]
    drift = (offs[-1] - offs[0]) if len(offs) > 1 else 0
    st_ok = len(offs) >= 4 and st.get("missing", 1) == 0 and max(offs) - min(offs) <= 2 * quantum and \
        max(abs(x) for x in offs) < step_samples / 8
    ok = ok and st_ok
    if offs:
        print(f"  [{label}] {st['key'][0]:g} BPM: drift first -> last step {drift:+.1f} samples over {len(offs)} steps")
print(f"  [{label}] RESULT:", "PASS" if ok else "FAIL",
      f"(every step on clockd's grid at every tempo: no step missing, spread and drift within two {quantum}-frame cycles,"
      f" every onset within 1/8 step)")
sys.exit(0 if ok else 1)
