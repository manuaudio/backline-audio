# Slot1: a strip's insert chain, same cycle, out of process

Slot1 runs **one channel strip's insert chain in one plugin process, inside
the engine's audio cycle**. The engine copies the strip's audio into shared
memory and wakes the chain's process. The plugins run in order, in place, and
the engine copies the result back in the same cycle. That adds no buffer of
latency: a chain adds only what its plugins report. If the chain is late, the
engine plays the strip's dry signal delayed by the chain's latency, so
alignment holds, and the strip keeps playing.

- **Header:** `lib/include/backline/slot1.h`. It is installed with libbackline,
  header-only C (usable from C++), MIT, and needs nothing beyond libc and
  Linux futexes. Both sides are in it: the engine's `bl_slot1_engine_*` calls
  and the sandbox's `bl_slot1_map` / `bl_slot1_sandbox_*` calls.
- **Sandbox:** `backline-slot-host`, which hosts CLAP, VST3 and LV2, in any
  mix, through the worker's own format adapters.
- **Supervisor:** `backline-hostd`, through `Host1.LoadSlot`. It restarts a
  crashed chain on the same memory, with its state.
- **Status:** version 1, additive-only. Fields are only ever added in
  reserved space, and `version` changes only for an incompatible layout.

Slot1 is the console's "slot" insert kind (spec B.4.4 mode 2). It replaces
the `port` kind's extra block: that kind is a worker linked in as its own
PipeWire node, and it costs one quantum.

## 1. Who does what

| | Engine (owns the cycle) | Sandbox (`backline-slot-host`) | Supervisor (`backline-hostd`) |
|---|---|---|---|
| Memory | creates it: a memfd, sealed against resizing, mapped and locked | maps it from `/proc/<engine pid>/fd/<n>` (or an inherited fd) | passes the path on; never maps it |
| Per cycle | `begin`: copy in, bump `req`, wake; `end`: wait until the deadline, copy out or play delayed dry | wait on `req`, run the chain in place, set `done = req`, wake | — |
| Parameters | pushes events into the in ring | applies them in the same cycle; plugin-reported changes go out on the out ring | — |
| Latency | reads `latency` each cycle, for the dry path and its PDC | publishes the sum of its inserts' reports | — |
| Crash | sees no `done`: delayed dry, stalled | dies; only this chain is lost | restarts it on the same memory; the new sandbox takes over |

**One process per chain:** the console has one per strip. Every insert of
that strip runs in it, as one round trip at the first slot's position.

## 2. The memory

One memfd per chain, named `backline-slot1`, sealed with `F_SEAL_SHRINK`,
`F_SEAL_GROW` and `F_SEAL_SEAL`. It is laid out in this order:

| Offset | Size | What |
|---|---|---|
| 0 | 4096 | `struct bl_slot1_header` |
| 4096 | `n_events` × 16 | the in ring (engine → sandbox) |
| … | `n_events` × 16 | the out ring (sandbox → engine) |
| `audio_offset` | `channels` × `max_frames` × 4 | the audio: float32, planar, in place |

`bl_slot1_engine_create()` uses a default of 512 events per ring and
`max_frames = 1024`. The limits are 32 channels, 8192 frames and 16 inserts.

### The header

The header is grouped into 64-byte lines, and **each line has one writer**.
`req` and `done` are the futex words: 32-bit, naturally aligned, and shared
(not `FUTEX_PRIVATE`). Everything else is plain data, published by those words
with release/acquire.

| Line | Writer | Fields |
|---|---|---|
| 0 | engine, once | `magic` (`"BLS1"`, 0x31534c42), `version` (1), `header_bytes` (4096), `total_bytes`, `channels`, `max_frames`, `rate`, `n_events`, `audio_offset`, `in_events_offset`, `out_events_offset`, `max_latency` |
| 64 | engine, per request | **`req`**, `frames`, `bypass` (bit n: insert n bypassed), `flags` (transport: valid, rolling, bar, changed), `steady`, `tr_frame`, `tr_bpm`, `tr_beat`, `tr_bar_start`, `deadline_ns` |
| 128 | engine | `in_head`, `tr_sig_num`, `tr_sig_den` |
| 192 | sandbox | `in_tail` |
| 256 | sandbox | **`done`**, `state`, `pid`, `generation`, `latency`, `latency_serial`, `n_slots`, `audio_allocs`, `served`, `process_ns_last`, `process_ns_max`, `process_errors`, `audio_frees` |
| 320 | sandbox | `out_head` |
| 384 | engine | `out_tail` |
| 448 | engine | statistics for anyone watching: `e_cycles`, `e_misses`, `e_dry`, `e_rtt_ns_last`, `e_rtt_ns_max`, `e_recoveries` |
| 512 | sandbox | `slots[16]`: per insert `latency`, `flags` (loaded, failed, instrument), `id` (the plugin id, 56 bytes) |
| 1536 | — | reserved, zero |

`state`: 0 empty, 1 loading, 2 **ready**, 3 failed, 4 exited.

### Events

An event is 16 bytes: `slot` (u8, 0-based insert), `type` (u8), `offset`
(u16, frame), `id` (u32), `value` (f64). Each direction is a
one-producer-one-consumer ring.

| Type | Meaning | When it lands |
|---|---|---|
| `PARAM_NORM` (2) | `value` 0..1 over the parameter's range (stepped parameters rounded) | **the same cycle**, every format |
| `PARAM` (1) | `value` in the plugin's own units (a CLAP value, an LV2 port value, a VST3 plain value) | the same cycle for CLAP and LV2. For VST3, a cycle or two later: plain → normalized needs the edit controller, which runs on the sandbox's main thread |
| `MIDI` (3) | `id` = status \| data1 << 8 \| data2 << 16, at `offset` | the same cycle |

Parameter ids are each format's own: the CLAP param id, the VST3 `ParamID`,
or the LV2 port index. The ready line (§5) lists every insert's parameters
with id, name, min, max and default. On the out ring, the sandbox reports the
plugin's own changes: `PARAM` for CLAP and LV2, `PARAM_NORM` for VST3.

## 3. The cycle and the deadline

```
engine                                  sandbox
begin:  feed the dry delay line
        stalled? not ready? → dry
        copy in, frames, transport
        req = ++seq ; futex_wake(req) ───► wakes (futex_wait on req)
   (the engine may begin other chains)  drain the in ring
end:    spin up to 20 µs on done        run inserts in order, in place
        futex_wait(done) until deadline ◄── done = seq ; futex_wake(done)
        done == seq → copy out (wet)
        else        → late: delayed dry, stalled
```

- **The deadline** is an absolute `CLOCK_MONOTONIC` time the engine picks. A
  good default is the cycle's start plus 50% of the period: 667 µs at 64
  frames and 48 kHz.
- **Begin and end are separate calls.** An engine with many strips should
  begin every chain of a level before it ends any of them, so a dead chain's
  wait never uses up another chain's time. `bl_slot1_engine_process()` is
  begin plus end, for one chain.
- **A request that is done when `end` looks is wet**, even if `end` runs
  after the deadline.

### The failure rule

When a request is not done by the deadline:

1. **This block** plays the strip's input delayed by the chain's reported
   `latency`. The engine keeps a dry delay line of `max_latency + max_frames`
   frames, fed every cycle, so a wet block followed by a dry one stays
   aligned to the sample.
2. **The chain is stalled.** It gets no new request until the sandbox has
   caught up (`done == req`), or until a new sandbox has taken the memory over
   (`generation` changed and `state` is ready). Until then every block is
   delayed dry. The engine never writes audio the sandbox might still be
   reading.
3. **After recovery**, the chain missed the stalled blocks' input. For its
   own latency's worth of frames, its output is what it held from before the
   gap. A restarted plugin starts from silence. An engine that wants this
   seamless crossfades over the switch.

`bl_slot1_engine_end()` reports what each block was: `WET`, `LATE`,
`STALLED` or `NOT_READY` (no sandbox serving: starting, crashed or
restarting). Every result except `WET` is delayed dry.

## 4. Latency

- Each insert's report goes into `slots[n].latency`: CLAP's `latency`
  extension, VST3's `getLatencySamples()`, or an LV2 output control port
  designated `lv2:latency` (or `lv2:reportsLatency`).
- The chain's `latency` is their sum, capped at `max_latency`.
  `latency_serial` goes up by one on every change.
- The sandbox re-reads the reports every 20 ms. It also re-reads them when a
  plugin asks for a restart (CLAP `request_restart`); that insert is then
  deactivated and re-activated while the chain keeps running.
- LV2 writes its latency port only when it runs. So a new LV2 insert reports
  0 until its first cycle, and a *restarted* one keeps the previous
  generation's value until then.
- A **bypassed** insert (a `bypass` bit) passes its input delayed by its own
  latency, so bypass never moves the chain's alignment.
- The engine's PDC uses `latency` as the chain's delay. Slot mode adds
  nothing to it.

## 5. The sandbox

```
backline-slot-host --shm /proc/<pid>/fd/<n> [--name ID] [--state-dir DIR] [--rt-priority N] \
                   [--format clap|vst3|lv2] --plugin P [--plugin P ...]
```

- **Plugins.** `P` is a `.clap` file, a `.vst3` bundle, an LV2 URI or bundle,
  or a bare name. `P#ID` picks a plugin inside a file or bundle: a CLAP id, or
  a VST3 class id or name.
- **Buses.** The chain's width is the strip's (`channels`). An insert's main
  input and main output take the chain's channels. Extra plugin channels get
  silence or are discarded. Chain channels the plugin's main output lacks pass
  through unchanged.
- **Threads.** The main thread is a PipeWire loop with **no server
  connection**: it handles plugin timers, main-thread calls, latency polling,
  VST3 conversions and state saves. The audio thread runs at `SCHED_FIFO 80`
  when it is allowed to.
- **Takeover on start.** A new sandbox maps the memory and sets
  `state = loading`. It loads and activates every insert, sets
  `done = req` (it never serves the dead sandbox's request), adds 1 to
  `generation`, and sets `state = ready`.
- **The ready line.** Once ready, the sandbox prints one JSON line on stdout:
  `{"node", "pid", "format": "slot", "slot": {"latency", "channels", "rate",
  "generation", "inserts": [{"index", "format", "id", "name", "latency",
  "params": [{"id", "name", "min", "max", "default"}]}]}}`.
- **State.** With `--state-dir`, each insert is restored from
  `DIR/insert<k>.{clap,vst3,lv2}state` and saved there 100 ms after its last
  change, atomically (temp file, fsync, rename).
- **Allocations.** The sandbox counts every heap call on its audio thread
  into `audio_allocs` and `audio_frees`. This includes the plugins' own calls,
  through a wrapped malloc family that forwards to glibc. Both counts should
  stay 0.
- **Exit.** `SIGTERM` stops it cleanly (`state = exited`), and the engine
  then plays delayed dry.

## 6. Supervision: `Host1.LoadSlot`

```
LoadSlot(s shm, as plugins, a{sv} options) → s id
```

- **Arguments.** `shm` is the engine's path, `/proc/<pid>/fd/<n>`, and
  `plugins` lists the chain's inserts in order. Options: `id` (s),
  `memory-max` (s), `cpu-quota` (s), `restart` (b, default true), `force`
  (b), `rt-priority` (i or u, 0..99).
- **The reply** comes once the chain is ready.
- **Supervision is the same as `Load`'s.** The chain runs in its own systemd
  user scope (`backline-slot-<id>-<generation>`). Every insert goes through
  the registry gate. Restart and back-off follow the same policy, with the
  same `Crashed`, `Restarted` and `Failed` signals, and `Unload`, `Restart`,
  `Get` and `List` work as for `Load`.
- **State** lives in `$XDG_STATE_HOME/backline/instances/<id>.slot/`, and a
  restart brings it back.
- **`Get`** has `kind` (`"slot"`), `shm` and `inserts` as well.

## 7. Proofs (offline, no audio server)

Two tests check the contract:

- **`tests/unit/test_slot1.c`** (ctest `slot1_contract`) checks the contract
  in one process: layout, sealing, the rings, wet, late, stalled, catch-up,
  takeover by a new generation, and the not-ready state.
- **`tests/slot-proof`** (ctest `slot-proof`) runs
  `backline-test-slot-driver`, which plays the engine, against the real
  `backline-slot-host`. The plugin is a test effect in CLAP, VST3 and LV2
  (`tests/plugins/slotfx.h`): a gain with a fixed, reported latency, and
  switches that crash, oversleep or allocate. Measured on 2026-11-12 on the
  development machine (i7-12650H), with both sides at normal priority
  (realtime limits are not set on this box yet):

| Check | Result (CLAP, VST3, LV2, and a mixed chain) |
|---|---|
| Impulse alignment | 0-latency chain: out at the same frame of the same cycle (**+0**). A 37-frame insert: out at +37. A 0+37+300 chain: out at +337, which equals the reported latency |
| Deadline miss | an insert sleeps 3 periods: 1 late block, 3 stalled, back at the 4th. Every dry sample equals the input delayed by 37 frames, **error 0**; every wet sample is within 3e-8 of g·x[n−37] |
| Crash isolation | chain A's plugin segfaults: A plays delayed dry (error 3e-8) until a new sandbox takes over (hostd restarts it in 3 ms, with its saved gain). Most of the 85–145 ms crash-to-wet time is the kernel writing the core dump before the process can be reaped. Chain B: **0 dry blocks, 0 misses**, every sample right |
| Allocations | 4 inserts, 5,000 cycles with parameter traffic: **0** heap calls on the sandbox's audio thread, **0** in the engine's cycle. An allocating plugin is caught: 100 in 100 cycles |
| Round trip at 64 frames / 48 kHz (deadline 667 µs) | 1 or 4 inserts, 20,000 cycles: p50 24–132 µs, p99 155–180 µs, max 300–1,000 µs; 0 to 20 misses in 20,000 cycles. The sandbox's own time with 4 inserts: p50 2–4 µs |

**Verify when the hold lifts:** the same chains inside the console engine's
real PipeWire cycle, with realtime priority on both sides (P5's
boot-level settings). The round-trip numbers above are an upper bound,
measured at normal priority.
