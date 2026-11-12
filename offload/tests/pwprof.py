#!/usr/bin/env python3
# Reads `pw-profiler -J` on stdin (one object per line) and judges every cycle
# against the deadline real hardware would impose: the graph must be finished
# one period after the cycle's ideal start (clock.nsec). A dummy driver only
# counts an xrun when the *next* cycle starts before this one ends, measured
# from its own (possibly late) wake-up, so it under-counts; this does not.
# SPDX-License-Identifier: MIT
import json, sys

period_ns = None
cycles = late = 0
ends, drv_wake, hop = [], [], []
cur = None
incomplete = [0]

def flush():
    global cycles, late
    if not cur or cur.get("nsec") is None:
        return
    cycles += 1
    end = max(cur["finish"]) - cur["nsec"] if cur["finish"] else 10**12
    if cur["incomplete"]:
        end = max(end, period_ns + 1)
        incomplete[0] += 1
    ends.append(end)
    if end > period_ns:
        late += 1
    if cur.get("signal"):
        drv_wake.append(cur["signal"] - cur["nsec"])

for line in sys.stdin:
    line = line.strip().rstrip(",").lstrip("[").rstrip("]")
    if not line:
        continue
    try:
        o = json.loads(line)
    except ValueError:
        continue
    t = o.get("type")
    if t == "clock":
        flush()
        period_ns = o["next_nsec"] - o["nsec"]
        cur = {"nsec": o["nsec"], "finish": [], "signal": None, "incomplete": False}
    elif t == "driver" and cur is not None:
        cur["signal"] = o["signal"]
    elif t == "follower" and cur is not None:
        # a node that was not signalled this cycle, or did not finish, makes the cycle late
        if o.get("status") != "finished" or o["signal"] < cur["nsec"] or o["finish"] < o["signal"]:
            cur["incomplete"] = True
        else:
            cur["finish"].append(o["finish"])
            hop.append(o["awake"] - o["signal"])
flush()

def pct(v, p):
    if not v:
        return 0
    v = sorted(v)
    return v[min(len(v) - 1, int(p * (len(v) - 1)))]

us = lambda x: round(x / 1000, 1)
print(json.dumps({
    "cycles": cycles, "period_us": us(period_ns or 0),
    "late_cycles": late, "incomplete_cycles": incomplete[0], "late_per_min": round(late / (cycles * (period_ns or 1) / 6e10), 1) if cycles else 0,
    "graph_end_p50_us": us(pct(ends, .5)), "graph_end_p99_us": us(pct(ends, .99)),
    "graph_end_p999_us": us(pct(ends, .999)), "graph_end_max_us": us(pct(ends, 1)),
    "driver_wake_p50_us": us(pct(drv_wake, .5)), "driver_wake_p99_us": us(pct(drv_wake, .99)),
    "driver_wake_max_us": us(pct(drv_wake, 1)),
    "hop_wake_p50_us": us(pct(hop, .5)), "hop_wake_p99_us": us(pct(hop, .99)), "hop_wake_max_us": us(pct(hop, 1)),
}))
