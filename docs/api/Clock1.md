# `io.github.manuaudio.Backline.Clock1`

The clock service, `backline-clockd`: one system transport (rolling,
position, tempo, meter) on PipeWire's own timeline, plus Ableton Link and LTC
and MTC generation and chase.

- Bus name `io.github.manuaudio.Backline.Clock`
- Object `/io/github/manuaudio/Backline/Clock`
- Status: **0.x, additive-only from here** (see [README](README.md)).

clockd is a separate process because Ableton Link is GPL (docs/DECISIONS.md
D3). Its D-Bus interface is the only way in, so clients stay MIT.

## The timeline

clockd owns PipeWire's position segment for the graph its node runs in
(`spa_io_position.segment`, the `bar` with bpm and signature): JACK clients
see it as the JACK transport and timebase master, and Backline's plugin
worker fills CLAP `transport` from it. The transport position is in frames
at the graph rate (`Rate`). Timecode is that position at `Fps`, plus
`TimecodeOffset`.

**Sources.** The transport follows one source at a time (`Source`):

| Source | Who moves the transport |
|---|---|
| `internal` | `Play`, `Stop`, `Locate` |
| `link` | Ableton Link: tempo and beat phase from the session; start/stop sync if the session uses it |
| `ltc` | Incoming LTC on the chase port: position and rolling |
| `mtc` | Incoming MTC (quarter frames and full frames): position and rolling |

Tempo is always clockd's; with Link on it is the session's tempo.

## Methods

### `Play() → ()`, `Stop() → ()`

Start and stop the transport. With `Source` `ltc` or `mtc` they're an error
(`State`): the chased timecode moves the transport.

### `Locate(s position) → (t frame)`

Move the transport. `position` is one of:

- frames: `"48000"` or `"frame:48000"`;
- timecode: `"tc:01:00:00:00"` (`;` before frames for drop-frame is accepted);
- bars, beats and ticks: `"bbt:5|1|0"` (1-based bar and beat, ticks of 1920 per beat).

Returns the frame it located to. Errors: `InvalidArgs`, `State` (chasing).

### `Resolve(s position) → (t frame)`

The frame `position` names (the same forms as `Locate`), with the current
frame rate and tempo map, without moving the transport. The player places
clips with it. Errors: `InvalidArgs`.

### `SetTempo(d bpm) → ()`

20 to 999 BPM. With Link on, it proposes the tempo to the Link session.

### `SetMeter(d beats_per_bar, d beat_type) → ()`

Time signature, e.g. `4, 4` or `7, 8`.

### `Link(b enable) → ()`

Join or leave the local Ableton Link session. On join, `Source` becomes
`link`; on leave, `internal`. Error `Unsupported` if built without Link.

### `Ltc(s mode, a{sv} options) → ()`

`mode`: `off`, `generate`, `chase`.

| Option | Type | Default | Meaning |
|---|---|---|---|
| `fps` | `d` | `25` | `24`, `25`, `29.97` (drop-frame), `30` |
| `target` | `s` | `""` | generate: link the LTC output port to this node (else leave it unlinked) |
| `source` | `s` | `""` | chase: the node whose (first) audio output or monitor carries LTC (required) |
| `level` | `d` | `-18` | generate: output level in dBFS |

Generate puts an LTC signal, locked to the transport position, on clockd's
`ltc_out` port. It is silent while stopped (LTC itself stops) unless the
transport is rolling. Chase decodes LTC from the source and makes
`Source` `ltc`. Errors: `InvalidArgs`, `NotFound` (node).

### `Mtc(s mode, a{sv} options) → ()`

`mode`: `off`, `generate`, `chase`. Options: `fps` as for LTC; `target`
(generate: link `mtc_out` to this MIDI node); `source` (chase: the MIDI node
to read). Generate sends quarter-frame messages while rolling and a full
frame message on every locate and stop. Chase follows both and makes
`Source` `mtc`.

### `Get() → (a{sv} state)`

A snapshot of every property below in one call, with `Frame` and the
timecode computed for the same cycle.

## Properties

| Name | Type | Meaning |
|---|---|---|
| `Rolling` | `b` | Transport rolling |
| `Frame` | `t` | Transport position in frames (*polled*; emitted in `Transport`) |
| `Rate` | `u` | Graph rate |
| `Tempo` | `d` | BPM |
| `BeatsPerBar` | `d` | Meter numerator |
| `BeatType` | `d` | Meter denominator |
| `Fps` | `d` | Timecode frame rate (LTC and MTC share it) |
| `Timecode` | `s` | `HH:MM:SS:FF` of `Frame` (*polled*) |
| `Bbt` | `s` | `bar|beat|tick` of `Frame` (*polled*) |
| `Source` | `s` | `internal`, `link`, `ltc`, `mtc` |
| `LinkEnabled` | `b` | In a Link session |
| `LinkPeers` | `u` | Other Link peers |
| `LtcMode` | `s` | `off`, `generate`, `chase` |
| `MtcMode` | `s` | `off`, `generate`, `chase` |
| `Locked` | `b` | Chasing and locked to the incoming timecode |
| `ChaseError` | `x` | Chasing: last measured (transport − incoming) in frames (*polled*) |
| `Node` | `s` | clockd's PipeWire node name (`backline.clock`) |

## Signals

| Signal | When |
|---|---|
| `Transport(b rolling, t frame, s timecode, s bbt)` | On every start, stop and locate, and every 250 ms while rolling |
| `Tempo(d bpm, d beats_per_bar, d beat_type)` | When tempo or meter change (from a call, or from Link) |
| `LinkPeers(u peers)` | When the number of Link peers changes |
| `Lock(s source, b locked)` | When a chase gains or loses lock |

## OSC

`/backline/transport/play`, `/stop`, `/locate s`, `/tempo f` on
`127.0.0.1:<port>` (property-free: the port is the node's
`backline.osc.port`).

## CLI

`backline transport play|stop|locate POS|tempo BPM|meter N D|link on|off|ltc
MODE ...|mtc MODE ...|status [--json]`.
