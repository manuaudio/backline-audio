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
| `SoundcheckInputs` | `a{ss}` | Input node → `live` or `playback` |
| `SoundcheckPosition` | `t` | Playback frame in the take (*polled*) |

## Signals

| Signal | When |
|---|---|
| `Level(t frames, ad peaks)` | About 5 times a second while armed or recording: the frames recorded so far and, per track, the peak since the last signal in dBFS (−inf as −200). Meant for a coarse "is it getting signal" indicator; fine metering is the UI's own PipeWire peak tap. |
| `Overrun(t frames)` | When rings were full and frames were lost; `frames` is this take's running total. All tracks lose the same frames, so they stay aligned. |
| `Marker(s take, s name, t frame)` | After `AddMarker`. |
| `TakeFinished(s take, s sidecar, t frames, t overruns)` | When a take's files and sidecar are complete. |
| `SoundcheckInput(s node, s mode)` | After an input switched. |

## CLI and library

`backline rec arm|record|stop|marker|takes|take|soundcheck|input` speaks this
interface (`--json` gives the raw JSON). The one-shot recorder
(`backline-recorder`, `backline rec` with `-s` flags) is unchanged.
