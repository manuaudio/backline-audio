# `io.github.manuaudio.Backline.Offload1`: network DSP offload (draft)

**Status: design draft plus a single-box proof** (`offload/`,
[docs/proofs/offload-proof.txt](../proofs/offload-proof.txt)). Nothing in this
file is implemented as a service yet. The method and frame names are the
proposal that the proof code was built to test. Licence: MIT, like the rest of
Backline. The offload is open infrastructure. Any mixer program uses it
through this API, and no part of it is specific to one console.

Design rule: **minimum latency decides.** Every choice below either adds zero
buffers or names exactly how many frames it adds and why.

## What it does

The service adds DSP capacity by adding Linux boxes ("DSP servers") on a
dedicated Ethernet link, the way SoundGrid servers do for LV1 and SuperRack.

- A **console** (any Backline client) sends selected plugin chains ("slots")
  to servers.
- Every period, the console sends each slot's input block on the wire and gets
  the processed block back. The return arrives either in the **same period**
  (0 added frames) or in the **next period** (exactly one block added).
- The added latency of a slot is **fixed when the slot is created**, and it is
  reported so the console's delay compensation can absorb it. It never changes
  during a show.

## Pieces

| Piece | Where | What |
|---|---|---|
| `backline-offloadd` | each DSP server | Runs the slots. Each slot is a CLAP chain in a lockstep loop, on its own isolated core, driven by packet arrival. The server has no clock of its own. Plugins are hosted by `backline-plugin-worker` code (`host/plugin.h`). For the proof, `offload/rack.h` runs them in process. |
| `libbackline-offload` | console process | Data plane, called from the engine's realtime thread: `blo_send(slot, block)`, `blo_wait(slot, deadline)`. No allocation, no locks. The only syscall is the send. With the AF_XDP path, there are no syscalls at all. |
| Offload1 D-Bus object | console box | Control plane: discover servers, create and move slots, read status. It is never on the audio path. |

## Data plane: the `BLO1` frame (proof: `offload/offload-proof.cpp`)

**Transport: raw Ethernet (L2), EtherType 0x88B5.** 0x88B5 is the IEEE "local
experimental" EtherType, so a production version needs its own registered
value. There is no IP, UDP, RTP or PTP on the audio path. The transport is
`AF_XDP` (zero-copy, busy-polled) where the NIC supports it, and otherwise
`AF_PACKET` with `PACKET_QDISC_BYPASS`. UDP over a link exists only for
debugging and for the proof's loopback runs.

```
Ethernet header (14 B) | Hdr (32 B) | samples
Hdr: magic u32 "BLO1" | seq u32 (block number, console's count)
     frames u16 | channels u16 | flags u32 (bit0: server error, block is dry)
     t_send_ns u64 (console clock, echoed) | server_us u32 | reserved u32
samples: planar float32, channels x frames
```

- **One frame carries one block of up to 5 channels** (float32, 64 frames:
  5 x 256 B + 46 B = 1326 B). Frames are never jumbo, because a jumbo frame
  adds store-and-forward delay at every switch hop.
- A slot wider than 5 channels uses several frames per block, in the order the
  server's chain needs them.
- The production format will carry 24-bit packed samples (7 channels per
  frame) as an option. Float32 is the default, because it costs no conversion
  and keeps the result bit-exact.

## Modes (fixed per slot at creation)

| Mode | Added latency | Condition |
|---|---|---|
| `same-cycle` | **0 frames** | The round trip, plus the server's processing, plus the console's own work after the mix point must all fit in one period. The console sends early in the period (offloaded strips are scheduled first) and mixes the return at the mix point. |
| `half-block` | 32 frames (at 64) | The server processes in two 32-frame sub-blocks and the console pipelines by half a block. Use it when same-cycle does not fit. |
| `next-cycle` | 64 frames (one block) | The server has a whole period: block *k* goes out in period *k* and is mixed in period *k*+1. This is the robust default for heavy chains. SoundGrid's minimum is 40 samples round trip at 48 kHz. |

The console picks a mode from the slot's measured cost (`MeasuredCost`, below)
and the link's measured round trip (`LinkRtt`). It **never adds buffers
silently.** A slot that fits no mode at the current period is refused with
`Error.Unsupported` and a sentence that gives the numbers.

## Clock

- **The console is the only clock.** It sends one block per device period. The
  server processes each block when it arrives and sends it straight back. The
  server never resamples and never runs a timer of its own: this is lockstep,
  the way netjack2's sync mode and SoundGrid work.
- **PTP is not needed for the offload itself.** It is needed only when the same
  network also carries AES67, or for server-side timestamps in diagnostics. In
  those cases use linuxptp on an i210 or i226 with hardware timestamps.

## Load balancing

- Each server measures every plugin's cost per block (p50, p99.9 and the
  maximum) at load time and publishes it as `MeasuredCost`.
- The console bin-packs slots onto server cores. Each slot stays on one core,
  and a core is never planned past 50 % of the period by p50. The 50 % comes
  from this box: the p99.9 cost runs at about twice the p50.
- Slots move only on an explicit `Move` call or a failover, never on their own.
  A move copies the plugin state and crossfades over one block.

## Failover

- **`redundancy=mirror`:** a hot spare runs the same slot on the same input,
  because the console multicasts the input frame to both servers. The console
  takes the first valid return for each `seq`. When a server dies, the next
  block comes from the mirror, with **0 frames of gap** and no switchover mute.
  Mirrors match only as far as the plugin is deterministic.
- **No mirror, or both servers late:** the console substitutes the dry signal
  for that block, with a one-block crossfade, and counts a `Miss`. After 3
  misses in a row the slot is marked `Down`. It is then moved to a spare if one
  exists (state copied from the console's journal), or bypassed until an
  operator acts. **The show never stops.** Compare Waves: when a main server
  drops, audio stops until the backup kicks in; an extra-DSP server drop means
  bypass.
- The server watchdog restarts a crashed chain from its journaled state, as
  `backline-hostd` does today (§4.3 of ARCHITECTURE.md).

## Control plane (D-Bus on the console box, draft)

Bus `io.github.manuaudio.Backline.Offload`, object
`/io/github/manuaudio/Backline/Offload`.

| Member | Signature | Meaning |
|---|---|---|
| `ListServers` | `() → aa{sv}` | Servers found on the offload link: `name`, `mac`, `cores`, `free_cores`, `link_rtt_us` (p50, p99.9, max). |
| `CreateSlot` | `(as plugins, a{sv} options) → s slot` | Options: `channels`, `mode` (`same-cycle`, `half-block`, `next-cycle`, `auto`), `server`, `redundancy` (`none`, `mirror`), `state`. Replies when the slot runs and its latency is known. |
| `DestroySlot` | `(s slot) → ()` | |
| `Move` | `(s slot, s server) → ()` | Copies the state, crossfades over one block, and keeps the latency unchanged. |
| `GetSlot` | `(s slot) → a{sv}` | `latency_frames` (mode plus the plugins' own reported latency), `server`, `mirror`, `state` (`running`, `degraded`, `down`, `bypassed`), `misses`, `measured_cost_us`. |
| Signals | `SlotDegraded(s, s reason)`, `SlotFailedOver(s, s from, s to)`, `ServerLost(s)`, `Miss(s slot, u count)` (rate limited). |

Plugin parameters of an offloaded slot stay addressable exactly like local
ones, through `Instance1` and OSC (`/backline/param/...`). The server forwards
parameter events on the control channel, and they are applied at block
boundaries.

## What this API refuses to do for convenience (each one adds latency)

- Use RTP or AES67 with a receive buffer (link offset) on the processing loop.
  PipeWire's AES67 module works in whole milliseconds.
- Resample, or use a jitter buffer that grows on its own.
- Use TCP.
- Use jumbo frames, NIC interrupt coalescing, or EEE on the offload link.
- Accept lookahead plugins on a `same-cycle` slot without saying how much
  latency they add.
