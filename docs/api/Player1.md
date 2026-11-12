# `io.github.manuaudio.Backline.Player1`

The player service, `backline-playerd`: audio files played into the graph
on the shared transport timeline, each starting on an exact transport frame,
each with its own gain.

- Bus name `io.github.manuaudio.Backline.Player`
- Object `/io/github/manuaudio/Backline/Player`
- Status: **0.x, additive-only from here** (see [README](README.md)).

## The model

A loaded file is a **clip**: one PipeWire node, `backline.player.<id>`
(`Stream/Output/Audio`), with one output port per file channel
(`output_FL`, `output_FR`, `output_MONO`, `output_AUX<n>`; `audio.channels`
and `audio.position` on the node, `backline.player.id` and
`backline.player.file` too).

A clip has a **start**: the transport frame at which its first sample
plays. The player never moves the transport. Every cycle, each clip reads
the transport frame *T* and the rolling state from `spa_io_position` (the
segment clockd owns, [Clock1](Clock1.md)) and writes file frame
*T − start + i* to output sample *i*, times the clip's gain; outside the
file, and whenever the transport is stopped, it writes silence. So:

- **Play, stop and locate are the clock's** (`Clock1.Play`, `Stop`,
  `Locate`, or any other segment owner, such as a JACK transport master).
  A locate before, into or past a clip is followed on the next cycle.
- A start is sample-accurate: it can fall anywhere inside a cycle, and
  every clip on the same timeline lines up to the sample (measured with two
  clips in `docs/proofs/player-proof.txt`).
- The first `preload` seconds of each file are in memory, so a start reached
  by rolling, or by a locate anywhere before the end of that head, plays
  from its first sample. Past the head, a disk thread streams the file into
  a ring of `ring` seconds. A locate *into* the streamed part while rolling
  can leave up to a few milliseconds of silence while the ring refills
  (counted in `underrun_frames`); a locate there while stopped is ready by
  the next `Play`.
- A clip's node must run under the same driver as the clock (the session
  manager puts a node linked to the same device there; tests use
  `node.group`).

**Rates.** A file plays only at its own rate. If the graph runs at another
rate the clip is silent and reports `rate_ok: false`; resampling is not
built yet.

## Methods

### `Load(s file, a{sv} options) → (s id)`

`file` is an absolute path to anything libsndfile reads (WAV, BWF, RF64,
FLAC, AIFF, Ogg, …). It replies once the node is in the graph and, with a
`target`, once its links are up.

| Option | Type | Default | Meaning |
|---|---|---|---|
| `id` | `s` | from the file name | The clip's id (letters, digits, `-_.`); also its node name `backline.player.<id>` |
| `at` | `s` | `"0"` | The start: frames (`"96000"`, `"frame:96000"`), `"tc:HH:MM:SS:FF"` or `"bbt:BAR|BEAT|TICK"` |
| `gain` | `d` | `0` | Gain in dB, −120 to +24 |
| `target` | `s` | `""` | Link the outputs to this node's inputs (by channel name, else in order; a mono clip feeds every input). The node is then not autoconnected |
| `autoconnect` | `b` | `true` without a target | Let the session manager link it to the default output, like any app |
| `preload` | `d` | `4` | Seconds held in memory from the start of the file (0.1–600) |
| `ring` | `d` | `8` | Seconds of streaming ring (0.5–120) |

`tc:` and `bbt:` are resolved by clockd (`Clock1.Resolve`) with its frame
rate and tempo map at the time of the call; the start is then a frame, and
it does not move if the tempo changes later.

Errors: `InvalidArgs` (bad option, position, gain, or not audio),
`NotFound` (no such file, or no such target node), `Busy` (the id is
taken), `Unsupported` (too many channels, or a `tc:`/`bbt:` start without
clockd), `Timeout` (the node didn't come up in 3 s).

### `Unload(s id) → ()`

Removes the clip and its node (its links go with it).

### `SetGain(s id, d db) → ()`

Takes effect on the next cycle, ramped linearly over that one cycle; after
it, samples are exactly file × gain.

### `SetStart(s id, s position) → (t frame)`

Moves the clip on the timeline; returns the resolved frame.

### `Get(s id) → (s json)`, `List() → (s json)`

`Get` returns a `backline.clip/1` object, `List` a `backline.player/1`
object with `clips`, an array of them:

| Key | Meaning |
|---|---|
| `id`, `node`, `node_id` | Clip id, node name, PipeWire node id |
| `channels` | Port channel names, in order |
| `rate`, `frames` | The file's rate and length |
| `start`, `end` | Timeline frames of the first sample and one past the last |
| `gain_db`, `target` | As set |
| `graph_rate`, `rate_ok` | The graph's rate at the last cycle; whether the file plays |
| `rolling`, `transport`, `playing` | The transport at the last cycle; whether the clip is sounding |
| `cycles`, `played_frames` | Cycles run, frames of file played |
| `underruns`, `underrun_frames` | Cycles (and frames) where streamed data wasn't ready |
| `seeks` | Times the streaming ring was repositioned (locates) |

## Properties

| Name | Type | Meaning |
|---|---|---|
| `Clips` | `as` | Loaded clip ids |
| `Underruns` | `t` | Sum over clips (*polled*) |

## Signals

| Signal | When |
|---|---|
| `Loaded(s id, s node)` | A clip's node is up (and linked) |
| `Unloaded(s id)` | A clip was removed |

## Activation and lifetime

D-Bus activated (`data/dbus/io.github.manuaudio.Backline.Player.service`).
It stays up while any clip is loaded and exits after 60 s with none
(`--idle-exit`). Nodes and links die with the process.

## CLI

`backline play load FILE [--at POS] [--gain DB] [--target NODE] [--id ID]
[--no-autoconnect]`, `play unload ID`, `play gain ID DB`, `play start ID
POS`, `play list`, `play show ID`, all with `--json`.

## Not built yet

Loop ranges, cue lists, resampling, and an OSC endpoint
(`/backline/player/...`).
