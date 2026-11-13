# `io.github.manuaudio.Backline.Recorder1`

The recorder daemon, `backline-recorderd`. It records any set of PipeWire
nodes into one sample-aligned, timestamped multitrack take, keeps a list of
takes, and plays a take back into the graph in place of its inputs (virtual
soundcheck).

- Bus name `io.github.manuaudio.Backline.Recorder`
- Object `/io/github/manuaudio/Backline/Recorder`
- Status: **0.x, additive-only from here** (see [README](README.md)).

## State machine

```
            Arm                Record               Stop
  idle ───────────► armed ───────────► recording ───────────► idle
   ▲                  │  (pre-roll kept)    │ (duration reached)  │
   └──── Stop ────────┘                     └──────────► idle     │
                                                  TakeFinished ◄──┘
```

One take at a time. Soundcheck is independent of recording: a take can be
played back while another is being recorded (that's how a band records a
soundcheck pass while the engineer listens to the last one).

## Methods

### `Arm(as sources, a{sv} options) → (s take)`

Create the recorder node, link it to every source and stand by. Returns once
every link is up and two cycles have passed, with the take id that `Record`
will use.

`sources`: one string per track, `NODE[:CHANNELS]`. `NODE` is a `node.name`.
For an `Audio/Sink` the track is its monitor; for anything else, its audio
output ports. `CHANNELS` defaults to 2 (a mono source fills every channel).
At most 32 tracks and 16 channels per track.

| Option | Type | Default | Meaning |
|---|---|---|---|
| `dir` | `s` | `$XDG_DATA_HOME/backline/takes` | Where the take's files go (created). |
| `name` | `s` | `take-YYYYmmdd-HHMMSS` | Take id: `[A-Za-z0-9._-]`, also the file prefix. |
| `format` | `s` | `wav` | `wav` (float32), `wav24`, `flac` (24-bit). WAV takes switch to **RF64** by themselves when a file passes 4 GB. |
| `pre-roll` | `d` | `0` | Seconds kept before `Record` (0 to `ring-seconds` − 1). |
| `duration` | `d` | `0` | Stop by itself after this many seconds (0 = until `Stop`). |
| `align` | `s` | `signal` | `signal` or `graph` (ARCHITECTURE.md §2.3). |
| `latency` | `s` | `hardware` | Input-latency compensation: `off`; `hardware` (tracks whose source ports are physical use their reported capture latency); `reported` (every track uses its reported capture latency). |
| `latency-frames` | `a{su}` | `{}` | Per-source manual capture latency in frames, by node name. Overrides `latency` for that source. |
| `ring-seconds` | `d` | `30` | Ring buffer per track, in seconds at the graph rate (2 to 120). This is how long the disk may stall without losing audio. |

Errors: `Busy` (already armed or recording), `NotFound` (a source node does
not exist), `Timeout` (links not up in 5 s), `InvalidArgs`.

### `Record() → (s take)`

Start the take on the next cycle (with the pre-roll already held). Error
`State` when not armed.

### `Stop() → (s sidecar)`

While recording: stop on a cycle boundary, finish the files and return the
path of the take's sidecar JSON once it is written. While armed: disarm and
return `""`. Error `State` when idle.

### `AddMarker(s name) → (t frame)`

Put a marker at the current position of the take; returns its frame (from
the take's first frame). Markers go into the sidecar and, for WAV, a `cue `
chunk with labels. Error `State` when not recording.

### `Takes() → (s json)`

Every take in `dir` (the last `Arm`'s, or the default) as
`{"schema": "backline.takes/1", "dir": "...", "takes": [ <summary>, ... ]}`,
newest first. A summary is `{"take", "sidecar", "utc", "rate", "frames",
"seconds", "tracks": [{"node", "channels", "file"}], "markers": n}`.

### `GetTake(s take) → (s json)`

The take's full sidecar (`backline.take/2`). `take` is a take id in `dir` or a
sidecar path. Error `NotFound`.

### `Soundcheck(s take, a{sv} options) → ()`

Load a take for virtual soundcheck. For every track it creates an output port
on one node, `backline.soundcheck`, and starts playing all tracks in lockstep
(the same take frame in the same graph cycle). Every input starts in `live`
mode: nothing is relinked until `SetInput`. Calling it again replaces the
take (every input returns to `live` first).

| Option | Type | Default | Meaning |
|---|---|---|---|
| `loop` | `b` | `true` | Loop the take; otherwise stop at the end (inputs stay switched, play silence). |
| `start` | `t` | `0` | Start frame in the take. |

Errors: `NotFound` (take), `InvalidArgs`.

### `SetInput(s node, s mode) → ()`

Switch one of the take's original inputs (a `node` from its sidecar) between
`live` and `playback`.

- `playback`: every link from that node's output ports (or monitor ports, for
  a sink) to a node that isn't Backline's own is moved to the matching
  soundcheck port. The consumer (a mixer, a DAW, an effect chain) now hears
  the take where it heard the input.
- `live`: those links are put back as they were.

The moved links are recorded in `$XDG_RUNTIME_DIR/backline/soundcheck.json`,
and the daemon puts them back on `StopSoundcheck`, on exit and on start-up
after a crash. The recorder's own links to the input are never moved, so a
soundcheck can be recorded.

Errors: `State` (no soundcheck loaded), `NotFound` (not an input of the
take), `InvalidArgs` (mode).

Since per-channel switching, `SetInput(node, mode)` is the same as
`SetChannelInput(node, 0, mode, "")`: every channel, every consumer, and it
clears that input's per-channel and per-consumer settings.

### `SetChannelInput(s node, u channel, s mode, s consumer) → ()`

Switch **one channel** of an input, optionally for **one consumer** only.
Nothing else moves: not the input's other channels, and not other consumers
of the same channel.

- `node`: an input of the take, as for `SetInput`.
- `channel`: the input's source channel, 1-based, in port order (the order
  the recorder took them: `capture_FL` = 1, `capture_FR` = 2; a sink's
  monitor ports for a sink). `0` = every channel, including ports the node
  doesn't have yet.
- `mode`: `live` or `playback`.
- `consumer`: a consumer's `node.name`, or `""` for every consumer.

**The state.** For every input channel there is a default for every
consumer, plus overrides for named consumers. A link from channel *c* of the
input to consumer *N* carries the take if *N*'s override on *c* says
`playback`, or if there is no override and the default says `playback`.
`consumer = ""` sets the default and clears the overrides on those channels;
a named consumer sets (or, when it equals the default, removes) its own
override. So, on an 8-channel interface feeding a console and a DAW:

| Call | Effect |
|---|---|
| `SetChannelInput(iface, 3, playback, "console.engine")` | channel 3 into the console plays the take; the DAW still records channel 3 live |
| `SetChannelInput(iface, 3, playback, "")` | channel 3 into everyone plays the take; channels 1–2 and 4–8 untouched |
| `SetChannelInput(iface, 3, live, "daw")` | ... except the DAW, which hears channel 3 live again |
| `SetInput(iface, live)` | everything back, every override cleared |

A moved link goes from channel *c* to the soundcheck port
`backline.soundcheck:t<track>_<ch>`, where *track* is the first track the
take recorded from that node and *ch* is *c*, or the track's last channel if
the track has fewer (a mono source recorded into a stereo track uses `_1`).
Links into Backline's own nodes (the recorder) are never moved. Moves are
recorded and put back exactly as for `SetInput`.

Errors: `State` (no soundcheck loaded), `NotFound` (not an input of the
take), `InvalidArgs` (mode; a channel past the input's ports, or past the
track's channels while the node isn't in the graph; more than 64 named
consumers with live overrides at once).

### `SetChannelInputs(a(suss) switches) → ()`

Many `SetChannelInput`s at once, each `(node, channel, mode, consumer)`, in
order (a later one wins over an earlier one on the same channel and
consumer). **All or nothing**: every switch is checked first, and on any
error nothing changes; the error names the first bad switch by index and
says `(nothing was switched)`. Accepted, the switches are applied to the
state together and carried out as one plan, in one pass of the daemon's
main loop (every link create and destroy sent back to back). At most 1024
switches per call.

This is the call for a console's "soundcheck these 24 channels" button.
Link changes are separate PipeWire objects, so a batch may land over a few
graph cycles; a program that needs a sample-exact switch should instead
link the soundcheck ports to its own playback inputs and switch inside its
own process (the console engine does that with its `pb_` ports). *Verify
when the audio hold lifts:* how many cycles a 24-channel batch spreads over
(`tests/recorderd-proof`, per-channel section).

### `StopSoundcheck() → ()`

Every input back to `live`, and the playback node removed. Not an error when
no soundcheck is loaded.

## Properties

| Name | Type | Meaning |
|---|---|---|
| `State` | `s` | `idle`, `armed`, `recording` |
| `Take` | `s` | The armed or recording take id, else `""` |
| `Dir` | `s` | The take directory in use |
| `Sources` | `as` | The armed sources, as given to `Arm` |
| `Rate` | `u` | Graph rate of the take (0 while idle) |
| `Frames` | `t` | Frames recorded in this take (*polled*; also in `Level`) |
| `Overruns` | `t` | Frames lost to full rings in this take (normally 0) |
| `CaptureLatency` | `a{su}` | Frames of input-latency compensation applied per source |
| `LastTake` | `s` | The last finished take's sidecar path |
| `Soundcheck` | `s` | The take loaded for soundcheck, else `""` |
| `SoundcheckInputs` | `a{ss}` | Input node → `live`, `playback`, or `mixed` (some channels or consumers differ: see `SoundcheckChannels`) |
| `SoundcheckChannels` | `a(suss)` | The per-channel state, one row per setting: `(node, channel, mode, consumer)`. A channel's default appears as `consumer = ""` when it is `playback`; every per-consumer override appears with its mode. Empty = everything live |
| `SoundcheckPosition` | `t` | Playback frame in the take (*polled*) |

## Signals

| Signal | When |
|---|---|
| `Level(t frames, ad peaks)` | About 5 times a second while armed or recording: the frames recorded so far and, per track, the peak since the last signal in dBFS (−inf as −200). Meant for a coarse "is it getting signal" indicator; fine metering is the UI's own PipeWire peak tap. |
| `Overrun(t frames)` | When rings were full and frames were lost; `frames` is this take's running total. All tracks lose the same frames, so they stay aligned. |
| `Marker(s take, s name, t frame)` | After `AddMarker`. |
| `TakeFinished(s take, s sidecar, t frames, t overruns)` | When a take's files and sidecar are complete. |
| `SoundcheckInput(s node, s mode)` | After an input was switched (by any of the three calls), with its overall mode now: `live`, `playback` or `mixed`. |
| `SoundcheckChannels(a(suss) switches)` | After `SetInput`, `SetChannelInput` or `SetChannelInputs` was applied: the switches as given, `(node, channel, mode, consumer)` (channel 0 = every channel). |

## CLI and library

`backline rec arm|record|stop|marker|takes|take|soundcheck|input` speaks this
interface (`--json` gives the raw JSON). `backline rec input NODE
live|playback [--channel N[,N...]] [--for CONSUMER]` switches channels
(several channels go as one `SetChannelInputs`); libbackline has
`bl_recorder_set_channel_input` and `bl_recorder_set_channel_inputs`. The one-shot recorder
(`backline-recorder`, `backline rec` with `-s` flags) is unchanged.
