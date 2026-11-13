# `io.github.manuaudio.Backline.Offload1`: network DSP offload

**Status, 2026-11-13:**

| Part | State |
|---|---|
| The server node, `backline-offloadd` | **Built** |
| The console's data plane, `backline/offload1.h` (send, wait, mirror failover) | **Built** |
| The `BLO1` frame | **v1** |
| The D-Bus control plane on the console box (below) | Still a **draft** |

Proofs:
- [docs/proofs/offload-failover-proof.txt](../proofs/offload-failover-proof.txt):
  the server, mirror takeover, one box with three network stacks.
- [docs/proofs/offload-proof.txt](../proofs/offload-proof.txt): the first
  loop, and capacity. Licence: MIT, like the rest of
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
| `backline-offloadd` (built) | each DSP server | Runs the slots. Each slot is a plugin chain (CLAP, VST3, LV2) in a `backline-slot-host`, over [Slot1](Slot1.md) shared memory, exactly as a mixing engine hosts a strip's inserts. A lockstep loop per slot is driven by packet arrival, on its own core with `--spin`. The server has no clock of its own. A supervisor per slot restarts a crashed chain on the same memory, with its saved state. |
| `backline/offload1.h` (built) | console process | Data plane, header-only C, called from the engine's realtime thread: `blo1_client_send()` sends the block to the primary and the mirror; `blo1_client_wait(deadline)` returns the active server's block, the mirror's (a takeover), or the console's delayed dry. No allocation, no locks; the syscalls are `send`, `recv` and (unless spinning) `ppoll`. |
| Offload1 D-Bus object | console box | Control plane: discover servers, create and move slots, read status. It is never on the audio path. |

## Data plane: the `BLO1` frame (proof: `offload/offload-proof.cpp`)

**v1 as built (`backline/offload1.h`): UDP, one slot per port** (slot *k*
on `BLO1_DEFAULT_PORT` 49800 + *k*), with the 32-byte header below. The
header's last word is now `slot u16 | part u16`; `part` must be 0 in v1.
Control frames use the same header: `BLO1_F_HELLO` asks for the slot's
latency, which the reply carries in `server_us`. `BLO1_F_PARAM` carries up
to 32 Slot1 events, each `insert u8 | type u8 | offset u16 | id u32 |
value f64`, and `part` is the count; they are applied with the next block.
On a block the server sends back, `BLO1_F_DRY` means the samples are the
server's delayed dry, so do not use them. The raw-Ethernet transport below
is the production target. It is measured in `offload/offload-proof.cpp`, but
the server does not speak it yet.

**Transport target: raw Ethernet (L2), EtherType 0x88B5.** 0x88B5 is the IEEE "local
experimental" EtherType, so a production version needs its own registered
value. There is no IP, UDP, RTP or PTP on the audio path. The transport is
`AF_XDP` (zero-copy, busy-polled) where the NIC supports it, and otherwise
`AF_PACKET` with `PACKET_QDISC_BYPASS`. UDP over a link exists only for
debugging and for the proof's loopback runs.

```
Ethernet header (14 B) | Hdr (32 B) | samples
Hdr: magic u32 "BLO1" | seq u32 (block number, console's count)
     frames u16 | channels u16 | flags u32 (bit0 DRY, bit1 PARAM, bit2 HELLO)
     t_send_ns u64 (console clock, echoed) | server_us u32 | slot u16 | part u16
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

- **`redundancy=mirror`** (built):
  - **The setup.** A hot spare runs the same slot on the same input: the
    console sends every block to both servers.
  - **Which block plays.** The console listens to one server, the *active*
    one. At its deadline, if the active server's block is missing or marked
    `DRY`, it uses the mirror's block for that same `seq`. The mirror then
    becomes active (`BLO1_TAKEOVER`).
  - **Why one active server, not the first block to arrive.** A server that
    comes back after a crash starts its plugins from silence. Taking the
    first valid block would switch back to that server while its output
    differs from the mirror's.
  - **The gap is 0 blocks.** The measured takeover is the block the primary
    failed on, with every sample right (§ Proof below).
  - **Determinism.** Mirrors match only as far as the plugin is
    deterministic.
  - **What counts as failed.** A server whose chain is late, restarting, or
    just back after a gap (Slot1's `RESUMED`) marks its blocks `DRY`, so the
    mirror's block is used.
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

## Proof: the server, and a mirror takeover (`offload/tests/failover-proof`)

Run on one box with no sudo and no audio server, and registered as a ctest
(`offload-failover`). The setup:

- **Three network stacks.** The console is in a user and network namespace;
  each server is in a network namespace of its own inside it, joined by a
  veth pair.
- **The chain.** Each server runs `backline-offloadd --spin` with one slot:
  the Slot1 test effect (gain −6 dB, a 37-frame latency).
- **The console** (`backline-test-offload-console --spin`) sends one stereo
  block of 64 frames per period (48 kHz) and checks every returned sample
  against g·x[n−37].
- **Priority.** Normal: this box has no realtime limits yet (P5).

Measured on 2026-11-13, 6,000 blocks (8 s) per run:

| Run | Blocks served (primary / mirror) | Takeover | Gap blocks | Max error | Round trip p50 / p99 / max |
|---|---|---|---|---|---|
| One server, lockstep | 6000 / — | — | 0 | 3e-8 | 10 / 31 / 275 µs |
| Primary server killed (`SIGKILL`) at block 3000 | 3000 / 3000 | at block 3000 | **0** | 3e-8 | 17 / 76 / 252 µs |
| Primary's plugin crashes at block 3000 | 3000 / 3000 | at block 3000 | **0** | 3e-8 | 14 / 64 / 251 µs |

- **The killed primary** costs no block: block 3000 comes from the mirror,
  and so does every block after it.
- **The crashed plugin.** The primary's chain is restarted on the same
  memory 2.6 ms after the crash, with its saved gain. It answers `DRY` until
  it is back, and the console stays on the mirror.
- **Blocking waits miss.** With sleeping waits instead of `--spin`, the
  same loop missed about 2% of blocks at normal priority. That matches the
  first proof: spinning on a core of its own is how a server runs.
- **An occasional miss.** In about one run in three, one block (in 6,000)
  outside the failure window had no server answer. That happened while the
  crashed primary was dumping core on a box with no realtime priority. The
  console then played its dry, delayed by the latency, with every sample
  still right. The test allows 0.1% of such blocks, and none in the failure
  window.

**Next:** the raw-Ethernet (AF_PACKET / AF_XDP) transport in the server, the
`next-cycle` mode, the console's D-Bus control plane, and a run on two
machines.
