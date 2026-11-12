# Backline: architecture

Backline is a set of small system audio services for Linux, in the role
CoreAudio's system layer, AudioComponent and AUv3 hosting play on macOS. On
stage, the *backline* is the shared gear every act plays through; here it's
the shared audio services every app and the desktop shell use.

Written 2026-11-11 on the development machine: Omarchy (Arch Linux), kernel
7.2.5, PipeWire 1.6.8, WirePlumber 0.5.17. It treats
[clap-pipewire](https://github.com/manuaudio/clap-pipewire) as the
effect-insert component.

## 0. Principles

1. **PipeWire is the engine and the IPC for audio.** Every service that
   touches audio *is* a PipeWire node, or a client that makes links. There
   is no second graph, no proxy audio transport and no private mixer. Any
   app sees Backline's nodes in `pw-dump`, `qpwgraph` and the shell, and
   Backline sees theirs.
2. **One process per risk.** Anything that runs third-party code (a plugin)
   runs in its own process, as its own node. A crash costs one node, never
   the graph or another plugin. This is measured below (§4).
3. **Realtime rules are the same everywhere.** On the data thread: no
   locks, no allocation, no blocking syscalls, no logging. Control crosses
   into realtime through wait-free SPSC rings. Results come back the same
   way. The main thread is a PipeWire main loop. D-Bus, OSC and linking all
   live there.
4. **Control is D-Bus for the desktop and OSC for the room.** D-Bus (session
   bus) is the typed API for apps, the shell and `backline`. OSC is for
   control surfaces, show control and scripts. It binds to localhost by
   default and is exposed beyond that only through the control service
   (§2.6). `--json` on the CLI gives machines the same facts people see.
5. **Graph metadata is the discovery mechanism.** Every Backline node
   carries `backline.*` properties (plugin id, D-Bus name, OSC port). Finding
   an instance means reading the graph, so there's no extra registry daemon
   just to find services.
6. **Reuse first.** Build only what's missing (§3). Licences are chosen so
   that any application, proprietary ones included, can link the client
   library and talk to every service: Backline is MIT, and GPL code (Ableton
   Link, Carla) stays behind a process boundary (docs/DECISIONS.md).
7. **Throwaway by default.** Services make their links with
   `object.linger = false`, so links die with their maker. Nothing changes
   the user's default devices. State lives in `$XDG_STATE_HOME/backline`,
   caches in `$XDG_CACHE_HOME/backline`, and runtime data in
   `$XDG_RUNTIME_DIR/backline`.

## 1. The suite at a glance

```
                    apps / desktop shell panels / scripts / control surfaces
                          │ libbackline (C ABI)  · backline CLI (--json)  · QML module
        ┌─────────────────┼──────────────────── D-Bus (session) ─────────────────────────┐
        │                 │                                                             │
 backline-registry   backline-hostd ──spawns──► backline-plugin-worker (1 per instance)  │
 (plugin index,      (supervisor)                ├ PipeWire node: midi_in, in_*, out_*  │
  validation)              │                     ├ D-Bus Instance1 · OSC 127.0.0.1      │
                           │                     └ CLAP, VST3, LV2 behind one interface  │
 backline-recorderd   backline-playerd     backline-clockd                backline-controld
 (N nodes → aligned   (files → nodes,      (transport + tempo = the       (OSC/MIDI → any
  BWF/FLAC takes)      on the timeline)     PipeWire segment owner;         parameter, volume,
                                             Link, LTC/MTC gen + chase)     transport)
        └──────────────────────── PipeWire graph (one clock) ───────────────────────────┘
                 clap-pipewire: CLAP as filter-chain "type = clap" (device effects, in-graph)
```

| Component | One line | Language | Status |
|---|---|---|---|
| `backline-registry` + `backline-probe` | The system plugin index (CLAP, VST3, LV2), with crash-proof scanning and validation on install | C | **Built and measured** (§4.4): sandboxed probes, grades, cache, `Registry1` |
| `backline-hostd` + `backline-plugin-worker` | Out-of-process plugin host: each instance one process and one PipeWire node, with MIDI in, params and presets over D-Bus and OSC | C (hostd), C++ (worker) | **Built and measured** (§4.1, §4.3, §4.5): CLAP, VST3 and LV2; presets; supervisor with restart-with-state |
| `backline-recorderd` | Records any set of nodes to sample-aligned, timestamped multitrack WAV/BWF or FLAC | C | **Recorder built and measured** (§4.2), with signal alignment and pre-roll; the D-Bus daemon is next |
| `backline-playerd` | Plays files into the graph, locked to the shared timeline | C | Design |
| `backline-clockd` | Transport, tempo and timecode: the PipeWire timeline owner, Ableton Link, LTC/MTC generation and chase | C++ (Link is C++) | Design |
| `backline-controld` | Maps OSC and MIDI to any parameter in the system: Backline instances, filter-chain/clap-pipewire controls, volumes, transport | C | Design (the worker already speaks OSC) |
| `libbackline` + `backline` CLI + `Backline` QML module | The client surface for apps and Omarchy panels | C (lib), C (CLI), C++/QML (module) | Library skeleton and CLI built (host, instance, plugins, rec); QML is design |
| clap-pipewire (separate project) | CLAP effects inside filter-chain: device effects and virtual sinks, in-graph | C | v0.1, separate project |

## 2. Components

### 2.1 Plugin registry: `backline-registry` (the AudioComponent registry)

- **Job.** Know every installed CLAP, VST3 and LV2 plugin in the formats'
  standard paths. Group a product across formats and pick a preferred one
  (CLAP, then VST3, then LV2, unless it fails validation). Grade each one
  `pass`, `warn` or `fail`.
- **Process model.**
  - A short-lived **probe process per binary** (`backline-probe`, from the
    prototype's `plugprobe.c`). It runs inside `systemd-run --user --scope`
    with a time and memory limit, so a crash, a hang or a leftover
    validator child dies with its cgroup. The prototype found all three.
  - A probe always runs in a throwaway working directory (Cardinal writes
    into the cwd).
  - The registry itself is not a resident daemon. `backline-registry scan`
    updates `$XDG_CACHE_HOME/backline/plugins.json`. The probe cache is
    keyed on path, size and mtime, then on the binary's SHA-256: a touched
    but unchanged binary is not probed again. Timeouts are never cached.
  - `backline-registry serve` is the D-Bus service
    (`io.github.manuaudio.Backline.Registry1`). It answers `Query`, `Show`,
    `List` and `Scan`, emits `Changed`, and exits when idle. An activation
    file template is in `data/dbus/`.
  - What is left in a probe's cgroup after it exits or times out (helper
    processes) is killed with the scope. Without a user systemd (CI), the
    registry falls back to a process group under a child subreaper.
  - Grades: `fail` means a host would be hurt (a crash, a timeout, a load
    or instantiate error, non-finite output). `warn` means findings a host
    survives (a state round-trip mismatch, a slow load, files written into
    the working directory). VST3 is still graded on load only (`"checked":
    "load"`): the worker hosts VST3 now (§4.5); a run check in the probe
    is still open.
- **Triggers.** A pacman hook for `/usr/lib/{clap,vst3,lv2}` (root,
  Omarchy-side), and a user `.path` unit on `~/.clap`, `~/.vst3` and
  `~/.lv2`.
- **Interfaces.**
  - `backline plugins list|show|scan [--json]` (through D-Bus, or straight
    from the index when the service isn't running).
  - D-Bus `Query(s filter_json) → s`, `Show(s) → s`,
    `List(s kind, s format) → a(sssssss)`, `Scan(b) → s`, signal `Changed`,
    properties `Generated` and `Count`.
  - The JSON schema is versioned (`backline.plugins/1`). It's the
    prototype's schema plus an optional `product-id`.
- **Realtime.** None. Probing is offline by design.
- **Reuse.**
  - clap-validator 0.4.1 (out-of-process, JSON), run inside the scope.
  - Steinberg's `validator` from the VST3 SDK (MIT since 3.8).
  - `lilv` for LV2 metadata without loading code. VST3 `moduleinfo.json`
    before loading anything.
  - carla-discovery's one-process-per-binary design, as a pattern only
    (it's GPL).
  - clap-pipewire's `clap-pw-info -c` already activates and runs a CLAP
    effect in a child process, so the CLAP runtime check should be shared,
    not duplicated.
- **Omarchy.** Plausible later as a small bash front over
  `backline plugins --json`, with a panel section, *only together with a
  consumer*: device effects, Install › Audio or the instrument picker.
  The registry stays standalone, and is worth proposing on linux-audio-dev as
  a shared cache convention.

### 2.2 Plugin host: `backline-hostd` + `backline-plugin-worker` (the AUv3-style host)

- **Job.** Run CLAP, VST3 and LV2 plugins, instruments and effects,
  **as PipeWire nodes**. An instrument takes MIDI from any PipeWire MIDI
  source, so a USB keyboard plays a system instrument with no DAW open.
- **Process model.**
  - **`backline-plugin-worker`: one plugin instance, one process, one
    node.** It's a `pw_filter` with `midi_in` (for plugins with note
    input), `in_*` and `out_*` DSP ports per audio bus (CLAP audio port,
    VST3 bus, LV2 port group) and channel, a D-Bus object and an OSC socket.
    The node publishes its channel layout (`audio.channels`,
    `audio.position`; the first output bus keeps `MONO`/`FL,FR`, further
    channels are `AUX<n>`), so meters and mixers can read it.
  - **The format is chosen by the argument**: a `.clap` file, a `.vst3`
    bundle, an LV2 URI or `.lv2` bundle, or a bare name looked up in the
    standard directories (CLAP first; `--format` narrows it).
  - **`backline-hostd` is the supervisor** (built). Its D-Bus API is
    `io.github.manuaudio.Backline.Host1`:
    - `Load(s plugin, a{sv} options) → s id`. It replies once the node is
      up. `plugin` is anything the worker takes (VST3 bundles and LV2 URIs
      included). Options: `id`, `plugin-id`, `format`, `name`, `target`,
      `midi-follow`, `state`, `memory-max`, `cpu-quota`, `autoconnect`,
      `restart`, `force`.
    - `Unload(s)`, `Restart(s)`, `Get(s) → a{sv}`, `List() → aa{sv}`.
    - Signals `Loaded`, `Unloaded`, `Crashed(s id, i signal, i exit_status)`,
      `Restarted(s id, u pid, u restarts)` and `Failed(s id, s reason)`.
  - hostd starts each worker in a transient systemd user scope
    (`systemd-run --user --scope --unit=backline-worker-<id>-<generation>`,
    `MemoryMax`, `TasksMax`, optionally `CPUQuota`). That gives per-plugin
    resource limits, cgroup cleanup, journald attribution and `coredumpctl`
    attribution for free. hostd is the worker's parent (a scope, not a
    service), so it gets the exact exit status. Workers get
    `PR_SET_PDEATHSIG` and never outlive hostd. When a worker dies, hostd
    kills whatever is left in its scope.
  - **State.** hostd gives each instance a state file
    (`$XDG_STATE_HOME/backline/instances/<id>.clapstate`, `.vst3state` (a
    `.vstpreset` image: component and controller state) or `.lv2state`
    (LV2 state as Turtle, through `lilv_state`)). The worker
    restores it on start, and saves it atomically (temp file, fsync,
    rename) whenever it changes: 100 ms after the last change, at least
    once a second while changes continue, and *only once the audio thread
    has handed every pending parameter event to the plugin*. The first
    version saved on the next tick and lost the last of three quick
    parameter moves; §4.3 caught it. The `StateSerial` and `StateSaved`
    properties make "saved" observable.
  - **Restart policy.** The first crash after a healthy run restarts at
    once. Crashes in a row back off (250 ms, doubling to 5 s). Six crashes
    within 60 s give up (`Failed`) until an explicit `Restart`. A worker
    that never came up is a load error, not a crash loop. A restart keeps
    the node name, OSC port and D-Bus name, and the worker re-makes its
    links.
  - **Registry gate.** hostd looks a plugin up in the registry index, in
    its format (path for CLAP and VST3, URI for LV2). One graded `fail` is
    refused unless loaded with `force`.
  - The supervisor never loads plugin code itself.
- **Threads (implemented in the worker).**
  - *Main* is the PipeWire main loop. It makes every main-thread call of
    the plugin (CLAP `[main-thread]`: `init`, `activate`,
    `params.get_info`/`flush` while inactive, `state`, `on_main_thread`,
    timers, presets; VST3: component and edit controller; LV2:
    instantiate, activate, state, `work()` of the worker extension), and
    handles D-Bus, OSC, graph watching and linking.
  - *Audio* is PipeWire's data loop (`PW_FILTER_FLAG_RT_PROCESS`). It makes
    the `start_processing`/`setProcessing`, `process`/`run` and
    `stop_processing` calls. On
    teardown and re-activation, `stop_processing` runs there through a
    blocking `pw_loop_invoke` on the filter's data loop.
  - Control in (parameter values, notes) goes through an SPSC ring. Output
    parameter events come back through another. Events live in preallocated
    arrays (CLAP events, VST3 `EventList`/`ParameterChanges` sized at load,
    the LV2 atom sequence). LV2 worker requests and responses cross in SPSC
    rings of fixed slots. A VST3 controller callback from a plugin's own
    thread is queued under a lock only non-audio threads take.
  - LV2 state restore is in LV2's instantiation threading class, so the
    worker keeps the audio thread out of the plugin while it runs (the
    same `pw_loop_invoke` handshake as a stop).
  - If the graph rate isn't the activated rate, the audio thread outputs
    silence and asks main to re-activate.
- **MIDI.**
  - The `midi_in` port takes PipeWire control sequences, either
    `SPA_CONTROL_Midi` (MIDI 1.0 bytes, which 1.6.8's bridge sends) or
    `SPA_CONTROL_UMP` (converted with `spa_ump_to_midi`).
  - Events reach the plugin sample-accurately, at their offset in the
    cycle. CLAP: `CLAP_EVENT_MIDI` if its note port takes MIDI, otherwise
    CLAP note events. VST3: note on/off and poly pressure events on the
    event bus, CC, channel pressure and pitch bend through the controller's
    `IMidiMapping`. LV2: `midi:MidiEvent` atoms with frame times.
  - **`--midi-follow REGEX`** watches the registry and links every matching
    MIDI source, now or when hotplugged. So "my USB keyboard plays the
    system synth" is one option, and the link dies with the worker.
  - When a followed source disappears with keys down (a keyboard unplugged),
    the worker releases those notes itself. Found and fixed in the proof
    (§4.1b).
- **Parameters and presets.**
  - D-Bus interface `io.github.manuaudio.Backline.Instance1` on the bus
    name `io.github.manuaudio.Backline.Instance.<node>`:
    - `ListParams`, `GetParam`, `SetParam(key, value)`. The key is an id, an
      exact name, or a unique substring. VST3 values are plain values
      (`normalizedParamToPlain`); LV2 parameters are the control ports.
    - `NoteOn`, `NoteOff`.
    - `SaveState(path)`, `LoadState(path)`.
    - `ListPresets`, `LoadPreset(id)`, `SavePreset(name)` (VST3).
    - Properties: `PluginId`, `PluginFormat`, `NodeId`, `OscPort`,
      `SampleRate`, `Cycles`, `MidiEvents`, `DroppedEvents`,
      `ReleasedNotes`, `StateSerial`, `StateSaved`.
    - Full reference: [docs/api/Instance1.md](docs/api/Instance1.md).
  - OSC: `/backline/param/set s|i f`, `/backline/param/<key> f`,
    `/backline/param/get s` (replies `/backline/param i s d d d`),
    `/backline/param/list`, `/backline/note/on iii`, `/backline/note/off ii`,
    `/backline/preset/list`, `/backline/preset/load s`.
  - **Presets** (built). The worker lists them when asked: CLAP
    `preset-discovery` providers crawled for the plugin's id, loaded with
    `preset-load`; VST3 `.vstpreset` files in the standard directories
    (read and written with the SDK's `PresetFile`) and the plugin's program
    lists (loaded through the program-change parameter); LV2 `pset:Preset`
    resources, loaded with `lilv_state`. A loaded preset counts as a state
    change, so hostd's state file follows it. Still open: parameter names
    following clap-pipewire's sanitised control names (`Gain (dB)` →
    `Gain_dB`), so one name works for an in-graph and an out-of-process
    instance of the same plugin.
- **Formats** (all three built, §4.5). The worker hides the format behind
  one internal `PluginInstance` interface (`host/plugin.h`), so each format
  is one adapter.
  - **CLAP** (`plugin_clap.cpp`): the original glue, moved behind the
    interface unchanged.
  - **VST3** (`plugin_vst3.cpp`) on the VST3 SDK 3.8 hosting classes (MIT):
    `module_linux`, `plugprovider`, `hostclasses`, `eventlist`,
    `parameterchanges`, `vstpresetfile`, compiled into a static library from
    `BACKLINE_VST3SDK_DIR` (default: the `vst3sdk` pkg-config package).
    Optional: without the SDK the worker is built without VST3 and says so.
    The controller gets the component's state at load, as hosts do.
  - **LV2** (`plugin_lv2.cpp`) on `lilv`: URID map/unmap, options and
    `buf-size`, `log`, `worker`, `state:loadDefaultState`; audio ports by
    port group; control ports as parameters. Only the spec bundles and the
    bundles naming the plugin's URI are parsed, not every installed TTL.
    LV2 fixes the rate at instantiation, so a rate change re-instantiates
    and carries the state over. For in-graph LV2 effects, PipeWire's own
    filter-chain is already the answer.
- **GUIs.** Later. The worker opens the plugin's own window (CLAP `gui`,
  X11/XWayland today, Wayland when CLAP and VST3 settle), on request from
  hostd.
- **Reuse.**
  - **clap-pipewire's `clap-host.c` (`cph_*`).** It's PipeWire-free and
    tested against a rule-checking test plugin (thread and lifecycle rules,
    block splitting, aliasing, state). The worker should move onto it once
    it gains note ports and is installed as a static library or subproject.
    Checked again with the VST3/LV2 work: it still has no note ports or
    input events beyond parameters, and it isn't installed as a library, so
    the worker keeps its own CLAP glue (now `plugin_clap.cpp`, behind the
    same interface the core would sit behind). See §6.
  - **clap-helpers (MIT)** for VST3/CLAP-side C++ utilities.
  - **Carla's bridges** (`carla-bridge-native`, GPL) as the fallback for
    Windows VST via Wine and odd formats, *run as a separate process* so
    their GPL stays behind the process boundary.
  - **jalv** as the LV2 host reference.
- **Omarchy.** Not the host itself: it's too large and opinionated. Omarchy
  could plausibly ship a *small* "Instruments" row (a command that starts a
  registry-picked instrument following all hardware MIDI) once the host is
  packaged.

### 2.3 Recorder: `backline-recorderd`

- **Job.** Record any device, app or node, many at once, to multitrack
  files: WAV/BWF (float or 24-bit, RF64 past 4 GB) or FLAC. Every take
  carries timestamps.
- **Process model.** The recorder is one `pw_filter` with one input port per
  track channel. **All ports of a node are filled in the same graph cycle,
  so frame *k* of every track is the same graph sample.** That's the whole
  alignment argument, and §4.2 measures it. It makes its own links: to a
  sink's monitor ports, to a source's or app's output ports, or to a
  specific port list.
- **Threads.**
  - *Data* copies each cycle into one lock-free `spa_ringbuffer` per track
    (8 s deep). Either all tracks get a cycle or none do, so an overrun
    can't skew them; it's counted and reported.
  - *Writer* opens the files once the first recorded cycle is known (so the
    BWF time reference is exact), then drains to libsndfile.
  - *Main* does linking, arming (after all links are bound, plus two settle
    cycles), stop on a cycle boundary, and the sidecar.
- **Timestamps.**
  - **BWF `bext` `TimeReference`** is samples since local midnight at the
    first sample. It's derived from the cycle's `clock.nsec` (monotonic)
    correlated with `CLOCK_REALTIME`. Origination date and time, and a
    description with the graph position and clock id.
  - **FLAC** carries the same reference in a Vorbis comment.
  - A sidecar, `<take>.json` (`backline.take/1`), holds:
    - UTC start (ns), monotonic ns, graph position and clock id;
    - rate, the quantum range during the take, frames and overruns;
    - per-track node, channels and file.

    The clock service will add timecode and bar/beat to it.
- **Pre-roll** (built). `-P SECONDS` arms the recorder and keeps the last
  N seconds in the rings. SIGUSR1 (or `record` on stdin) starts the take,
  with those seconds included and timestamped from the first kept frame.
- **Signal alignment** (built, the default; `-a graph` turns it off).
  - The problem, measured in §4.2. The driver runs first in each cycle,
    before the nodes that feed it. So a sink that drives the graph delivers
    on its monitor what it was fed in the *previous* cycle, one quantum
    later than a follower sink's monitor of the same signal.
  - The fix works per cycle. In each cycle, the recorder writes the driver
    sink's monitor as it arrives and every other track from a one-cycle
    copy (`rec_align.h`). Frame *j* is then the same input sample on every
    track. It holds through quantum changes (a graph idles at 128 and jumps
    to 2048 when a player joins) and through driver changes. A first
    version that shifted by the quantum at take start broke on exactly
    those two cases.
  - The ports' reported `SPA_PARAM_Latency` can't drive the fix. A null
    sink reports 1 quantum of playback latency whether it drives or
    follows, and 0 until something feeds it. The recorder logs it in the
    sidecar.
  - Compensating capture latency of hardware sources (for "as heard in the
    room" alignment) is a separate, later mode.
- **Interfaces (next).**
  - D-Bus `Recorder1`: `Arm(sources, options)`, `Record`, `Stop`, `Takes`,
    and signals `Level`/`Overrun`.
  - `backline rec` runs the recorder today with its own flags.
- **Reuse.**
  - `pw-record` (one stream, no timestamps, so not alignable: −384 to −512
    frames apart in the control runs).
  - `jack_capture` (multichannel through JACK, GPL) and `timemachine`
    (pre-roll idea).
  - libsndfile (LGPL, dynamic) for WAV/BWF/RF64/FLAC.
- **Omarchy.** A "Record this app/device" action in the audio panel is
  plausible: a small bash command calling
  `backline rec`. The daemon stays standalone.

### 2.4 Player and transport: `backline-playerd`

- **Job.** Play files (multitrack sessions, cues, stems) into the graph on
  the shared timeline. Locate, loop, cue lists, and chase the transport.
- **Process model.** One process, one `pw_filter` per *player*, with
  output ports per track. A disk thread prefetches into rings, and the
  data thread only copies. Sample-accurate start comes from
  `spa_io_position` (a start at graph frame N).
- **Interfaces.** D-Bus `Player1`: `Load`, `Play`, `Stop`, `Locate`, `Cue`.
  OSC `/backline/player/...`.
- **Reuse.** libsndfile, plus libsamplerate/soxr when the file rate differs
  from the graph rate. `pw-play` covers the single-file case today.
- **Omarchy.** Nothing. This is a production tool.

### 2.5 Clock, transport and timecode: `backline-clockd`

- **Job.** One system transport (rolling, position, tempo, meter) that
  every app can follow. Also LTC and MTC generation and chase, and Ableton
  Link.
- **Mechanism.**
  - **Use PipeWire's own timeline.** `spa_io_position` carries a `segment`
    with an `owner` and a `bar` (bpm, signature, beat) to every node, every
    cycle. pipewire-jack maps that to the JACK transport and timebase
    master.
  - clockd becomes the **segment owner and JACK timebase master**. Ardour,
    Bitwig, the Backline worker (as CLAP `transport`) and every other
    JACK/PipeWire client then follow one transport without new APIs.
  - The worker's TODO, `clap_process.transport`, is filled from this.
- **Ableton Link.** It's open source (GPLv2+, or a commercial licence) and
  has Linux support. clockd embeds Link and bridges Link tempo and phase to
  the PipeWire segment both ways. Because Link is GPL, it stays inside
  clockd (its own process). libbackline only speaks D-Bus to it, so it stays
  MIT, and any application can use it.
- **LTC.**
  - Generate with x42's libltc (LGPL) on a `pw_filter` output port, locked
    to graph frames.
  - Chase by decoding LTC from a chosen input port and driving the segment.
- **MTC.** Quarter-frame and full-frame messages on an ALSA sequencer port
  (bridged into PipeWire), from the same position. Chase works the same
  way.
- **Realtime.** Generation runs on the data thread from position math only.
  Decoding runs on the data thread into a ring, and main publishes.
- **Interfaces.**
  - D-Bus `Clock1`: `Play`, `Stop`, `Locate(frame|tc|bbt)`, `SetTempo`,
    `Link(enable)`, `Ltc(out_port|chase_port, fps)`, `Mtc(...)`; signals
    `Transport`, `Tempo`.
  - OSC `/backline/transport/...`.
- **Omarchy.** Nothing beyond, perhaps, a tempo/transport chip in the panel
  later.

### 2.6 Control: `backline-controld`

- **Job.** Map OSC and MIDI to *any* parameter in the system:
  - Backline instance params, via their D-Bus or OSC;
  - filter-chain and clap-pipewire controls, via `Props` on the node;
  - node and device volumes and mutes;
  - transport (clockd) and recorder actions.
- **Process model.** One daemon, all on the main loop. It has a MIDI input
  port in PipeWire (a `pw_filter` with no realtime work: it drains into a
  ring that main reads), an OSC server (UDP and TCP), and a map file
  (`$XDG_CONFIG_HOME/backline/control/*.json`) with MIDI-learn.
- **Security.** It's the only component that listens beyond localhost. It
  needs an explicit bind address and an allow-list, and OSC-over-TCP with
  a shared secret for remote surfaces.
- **Reuse.**
  - liblo (LGPL) for OSC servers.
  - The worker's OSC codec is enough for the localhost endpoints.
  - mididings (GPL) only as a design reference for mapping rules.
- **Omarchy.** Nothing directly.

### 2.7 Client surface: `libbackline`, `backline`, QML module

- **`libbackline`.** A C ABI, MIT, small (built as a skeleton:
  `lib/include/backline/backline.h`). It's a typed wrapper over the D-Bus
  interfaces (sd-bus) for `Host1`, `Instance1` and `Registry1`, plus a
  signal watcher. Graph discovery through `backline.*` node properties
  comes with the recorder daemon. Its versioned headers are the contract
  for applications.
- **`backline` CLI.** Thin, with `--json` everywhere:
  - `backline plugins ...`
  - `backline host load|unload|list|params|set|note`
  - `backline rec ...`
  - `backline play ...`
  - `backline transport ...`
  - `backline map ...`

  It's not called `audioctl`, which already exists as a command on OpenBSD
  and NetBSD.
- **`Backline` QML module.** C++/Qt 6, for Omarchy-style apps and possibly
  the Quickshell panel. It's the only Qt code in the suite.

### 2.8 Effect insert: clap-pipewire (separate project, not edited here)

- **What it is.** CLAP effects as a filter-chain node type. They're
  in-graph and in-process with their host client, use filter-chain
  controls for parameters, and are WirePlumber smart filters for "effect on
  a device".
- **Roles.**
  - clap-pipewire is the light, zero-copy insert for trusted effects on
    devices.
  - The Backline worker is the isolated, instrument-capable host.
  - The same CLAP effect can run either way. The registry's grade decides
    which is offered by default: a `fail` grade never goes in-graph.
- **What Backline uses from it.**
  - Device effects: `clap-pw-host` configs, controlled through `Props`.
    controld maps to them.
  - Its host core, `cph_*`, as the worker's CLAP engine, once that's
    agreed (§6).

## 3. What exists and is reused (not rebuilt)

| Need | Reused | Why not more |
|---|---|---|
| Graph, clock, drift, aggregates, links | PipeWire core, `module-combine-stream`, WirePlumber | It's the engine |
| In-graph LV2/LADSPA/builtin effects | PipeWire filter-chain | Already a system effect |
| In-graph CLAP effects | clap-pipewire | Separate project |
| CLAP SDK | free-audio/clap 1.2.10 headers (vendored, MIT) | |
| CLAP host core | clap-pipewire `clap-host.c` (planned, §6), clap-helpers | Avoid two CLAP hosts; until then the worker's own adapter |
| CLAP validation | clap-validator | Run in a scope (it leaves hung children) |
| VST3 hosting and validation | VST3 SDK 3.8 (MIT) hosting classes (**used**, §4.5), `validator` | |
| LV2 hosting | lilv (**used**, §4.5), suil, jalv as reference | |
| Presets | CLAP `preset-discovery`/`preset-load`, the SDK's `PresetFile` (`.vstpreset`), `lilv_state` | |
| Odd formats, Wine VST | Carla bridges, out of process (GPL boundary) | |
| Single-stream capture/playback | `pw-record`, `pw-play`, `pw-cat --midi` | No alignment, no timestamps |
| Multichannel capture to disk | jack_capture (reference) | JACK-only, GPL, no BWF |
| Audio files | libsndfile (WAV, BWF bext, RF64, FLAC) | |
| Transport | PipeWire `spa_io_position` segment + pipewire-jack transport | Already the shared timeline |
| Tempo sync | Ableton Link | GPL: stays in clockd |
| LTC | libltc | |
| OSC | liblo for servers; a 150-line codec in the worker | |
| D-Bus | sd-bus (libsystemd) | Already on every systemd box |
| Process isolation | systemd transient user units and scopes | cgroup cleanup, limits, coredumps |

## 4. Measured proofs (2026-11-11, development machine)

All proofs run on throwaway null sinks owned by the test script
(`object.linger = false`). They never link to real devices, and they check
the default sink and every other node before and after. Full reports are in
`docs/proofs/`.

### 4.1 Plugin worker (`tests/host-proof`)

The path: Surge XT (CLAP), in `backline-plugin-worker`, as node
`backline.surge-a` → test sink. The keyboard is `backline-test-keyboard`, an
ALSA sequencer client like a USB keyboard. It reaches the worker through
PipeWire's `Midi-Bridge`, and the worker links it itself (`--midi-follow`).
`backline-recorder` records the sink's monitor.

| Note | Expected | FFT | Error | Level | MIDI → node output | Note-off → −60 dB | Gap peak |
|---|---|---|---|---|---|---|---|
| A4 | 440.000 Hz | 440.020 Hz | +0.08 cent | −21.9 dBFS | ≤ 1 quantum (0.15–2.8 ms over runs) | 22–26 ms (Surge's release) | below −134 dBFS |
| C4 | 261.626 Hz | 261.607 Hz | −0.12 cent | −21.9 dBFS | | | |
| E5 | 659.255 Hz | 659.261 Hz | +0.02 cent | −22.0 dBFS | | | |

- **1b, unplug with a key down.** The first run found a real defect. The
  keyboard's last note-off was lost because its sequencer port vanished
  before the bridge forwarded it, and the note stuck. The worker now
  releases held notes when a followed source goes away. Measured: the held
  note was at −21.9 dBFS, and from 300 ms after the unplug the peak is
  −144 to −148 dBFS over runs. `ReleasedNotes` = 1.
- **2, live parameter.** Surge "Global Volume" was set from 0.958 to 0.5
  over OSC: −21.93 → −43.90 dBFS (−21.97 dB). It took effect ≤ 19–24 ms after
  the send over runs, including the sender's start-up, and `/param/get` read back
  0.5. It was restored over D-Bus `SetParam`: −21.94 dBFS.
- **3, crash.** Two instances were playing on two sinks. SIGKILL hit
  instance A mid-note.
  - A's node, links and D-Bus name were gone.
  - B kept running: its cycle counter advanced (707 → 899 in 0.5 s) and it
    still answered D-Bus.
  - B's track: −21.92 dBFS before and after. The 20 ms window spread was
    identical before and across the kill (0.480 dB). No run of exact zeros.
  - Recorder overruns: 0.
  - Every other node kept its id. The pipewire and wireplumber pids were
    unchanged.
- **Caveats.**
  - Realtime scheduling was **off** for these runs (no rtkit on the
    test machine), and the proofs passed anyway at quantum 128.
  - MIDI-to-audio is measured to the node's output, not through a
    hardware DAC.
  - Rate re-activation is implemented but not exercised.

### 4.2 Recorder (`tests/recorder-proof`)

One test signal is played by one player and fanned out to two different
null sinks: irregular impulses, noise and a 1 kHz tone, with R = −0.5 × L.
Both sinks' monitors are recorded as two tracks in one take. As a control,
two `pw-record` processes record the same sinks at the same moment.

| Topology | `-a graph` (raw cycle alignment) | `-a signal` (default) | pw-record control |
|---|---|---|---|
| Both recorded sinks are followers (a dedicated driver) | — | **lag 0**, bit-identical over 240 000 frames, both channels | −384 to −512 samples apart, no timestamps |
| One recorded sink drives, quantum 2048 | **−2048 = exactly one quantum**, bit-identical at that lag | **lag 0**, bit-identical | −256 to −384 |
| The same at quantum 256 | **−256**, bit-identical at that lag | **lag 0**, bit-identical | — |

- In every take, the quantum changes when the player joins (the graph
  idles at 128). In the first row the driver also changes twice during the
  take. The per-cycle pairing holds through both; the shift-at-start
  version failed exactly here.
- Equal lengths, and equal BWF time references that match the sidecar's UTC
  start. FLAC (24-bit) takes carry the same reference in a Vorbis comment.
- **Pre-roll.** A recorder stood by with 2 s of pre-roll and was triggered
  (SIGUSR1) 1.4 s into playback. The take holds exactly 96 000 frames from
  before the trigger. It is sample-for-sample identical to the matching
  stretch of a reference take recorded from before the start, with 4 of the
  6 impulses before the trigger, and its two tracks are aligned at lag 0.

### 4.3 Supervisor (`tests/hostd-proof`)

The path: `backline` CLI → libbackline → hostd → Surge XT in
`backline-worker-synth-0.scope` (MemoryMax 4G, TasksMax 512). A virtual
keyboard plays A4 every 0.45 s through PipeWire's MIDI bridge.

- **SIGKILL while playing.** Three parameters had been moved away from
  their defaults (Global Volume 0.5, A Pitch 0.6, Send FX 1 Return 0.25).
  - `Crashed(signal 9)` arrived 3.4 ms after the kill, and
    `Restarted(new pid, restarts 1)` 84 ms after it, in a new scope.
  - **All 775 parameter values were identical before and after (exact
    comparison).** The node name, OSC port and D-Bus name were the same,
    and OSC answered on the old port.
  - Notes: 7 before the kill, 10 after it, and all 10 sound. The level was
    −43.90 → −43.92 dBFS and the pitch 477.12 → 477.12 Hz (+0.00 cent). The
    defaults would give −21.9 dBFS at 440 Hz.
  - The note held at the kill is cut. Sound returns with the first note-on
    after the restart (315 ms of silence at this note spacing).
  - The first run found a real bug: the autosave could run before the
    audio thread had delivered the last parameter change, so Send FX 1
    Return came back at its default. Fixed (§2.2), and checked by the exact
    comparison.
- **Crash loop.** Killed whenever it came back: crash to restart took 91,
  351, 592, 1082 and 2102 ms, then the 6th crash gave `Failed` ("6
  crashes in 60 s"). An explicit `Restart` brings it back with its state.
- **Registry gate.** A test plugin graded `fail` (it segfaults in
  `process` and leaves a helper process) is refused. With `--force` it
  crash-loops to `Failed`, and no scope and no process of it is left.
- Afterwards, there were no Backline nodes, processes or scopes. The other
  nodes, the PipeWire/WirePlumber pids and the default sink were unchanged.

### 4.4 Registry (`tests/registry-proof`)

A full scan of `~/.clap`, `~/.vst3` and `~/.lv2`, plus three test plugins
(ok, crash, hang), with 16 parallel probes and a 10 s timeout.

| Format | Plugins | Pass | Warn | Fail |
|---|---|---|---|---|
| CLAP | 218 | 215 | 0 | 3 |
| VST3 (load only) | 213 | 213 | 0 | 0 |
| LV2 | 214 | 212 | 1 | 1 |

- **Timing.** The cold scan took 13.5 s: 469 probes and 1.19 GB hashed in
  0.1 s. Of that, 10 s is the hang plugin's timeout; the slowest real
  plugin took 0.6 s. A warm rescan of the standard directories takes
  1.6 s, almost all of it the LV2 TTL parse.
- **Cache.** Unchanged → hit on size and mtime. Touched → hit on SHA-256,
  with no probe. One byte appended → probed again.
- **Containment.** The crash plugin was graded `fail` (SIGSEGV), and its
  helper was killed with the scope. The hang plugin was graded `fail` by
  timeout, and both of its processes were killed. The scan completed with
  0 errors. No probes, scopes or scratch directories were left, and the
  plugin directories were unchanged (50 453 entries compared).
- **A real finding.** Cardinal Synth aborts (SIGABRT) as CLAP and as LV2
  because of a corrupt settings file in the user's config, which a host
  would hit too. Cardinal Mini writes into the working directory (`warn`).
- **Queries.** A D-Bus `Query` takes about 3 ms, and the service exits
  when idle.

### 4.5 Formats and presets (`tests/formats-proof`)

The same worker hosting VST3 and LV2, on throwaway sinks. Full report in
`docs/proofs/formats-proof.txt`.

| Note (one keyboard, two instruments, one take) | VST3 Surge XT | LV2 Odin2 |
|---|---|---|
| A4 440.000 Hz | 440.020 Hz, +0.08 cent, −21.9 dBFS | 440.022 Hz, +0.09 cent, −20.7 dBFS |
| C4 261.626 Hz | 261.607 Hz, −0.12 cent | 261.607 Hz, −0.12 cent |
| E5 659.255 Hz | 659.261 Hz, +0.02 cent | 659.263 Hz, +0.02 cent |
| MIDI → node output (quantum 128) | 0.3–2.6 ms | 0.1–0.5 ms |

- **Parameters.** Set over OSC, read back over OSC and D-Bus, restored over
  D-Bus. Surge XT VST3 "Global Volume" 0.958 → 0.5: −21.98 dB, restored
  ±0.00 dB. Odin2 LV2 "Amplifier Gain" 0.714 → 0.25: −19.5 dB, restored
  −0.01 dB.
- **Effects.** A 1 kHz burst into a sink whose monitor feeds both effects
  (`--audio-source`). Dragonfly Hall Reverb (VST3): a tail at −38 dBFS
  0.2–1.0 s after the burst, where the dry signal is digital silence. CHOW
  Tape Model (LV2): third harmonic at −9.7 dB re the fundamental (dry:
  −88 dB).
- **Presets.** CLAP Surge XT: 2 986 presets from `preset-discovery`; loading
  "Attacky" changed 36 of 775 values and the note (440 → 110 Hz, −21.9 →
  −16.3 dBFS). VST3 Dexed: 32 programs from its program list; "LAURIE"
  changed 92 of 2 264 values and the sound (centroid 513 → 5 034 Hz). A
  `.vstpreset` written with `SavePreset` and loaded back: every value equal
  (Dexed's program-change parameter aside: a program sets it, its state
  doesn't keep it). LV2 CHOW Tape Model: 94 `pset:Preset`; "Bass Pusher I"
  changed 9 of 52 values and +6.2 dB on the burst.
- **Restart with state under hostd.** Three parameters moved on each, then
  SIGKILL. VST3 Surge XT: restarted 126 ms after the kill; 2 855 values
  equal (one, "A Pitch" 0.6, comes back as 0.6000000238: the plugin keeps it
  as a 32-bit float); the note −43.91 dBFS at 477.04 Hz before and after.
  LV2 Odin2: restarted 485 ms after the kill (JUCE's LV2 instantiation); 258 of 258 values exactly
  equal; −65.61 dBFS at 493.90 Hz before and after.
- **Registry gate by LV2 URI.** Cardinal Synth's LV2, graded fail (SIGABRT
  on its own corrupt settings file), was refused.
- **Channel layout.** Both instrument nodes show `audio.channels 2`,
  `audio.position FL,FR`; Surge XT as CLAP (three stereo buses) shows 6 and
  `FL,FR,AUX2,AUX3,AUX4,AUX5`.
- The other nodes, the PipeWire/WirePlumber pids and the default sink were
  unchanged; no node, process or scope of the proof was left.

## 5. Build order

1. **Host supervisor and client skeleton.** *Done (§4.3).*
   - `backline-hostd` (D-Bus `Host1`, a transient scope per worker, crash
     signals, restart with restored state, backoff and give-up).
   - The `backline` CLI and `libbackline` with the host, instance, plugin
     and recorder calls.
   - Still open: parameter naming aligned with clap-pipewire's sanitised
     control names.
2. **Registry service.** *Done (§4.4).*
   - `backline-probe` (from the prototype's `plugprobe`), scope-isolated
     probing and grading, the cache, `backline plugins --json` and the
     D-Bus query service. hostd refuses `fail` grades unless forced.
   - Still open: the pacman hook and the `.path` unit, the optional
     clap-validator pass, VST3 run checks, caching `lv2-list`.
3. **Recorder.** *Alignment and pre-roll done (§4.2).*
   - Still open: `recorderd` with `Recorder1`, RF64 for takes over 4 GB,
     and hardware capture-latency compensation.
4. **Clock and transport.**
   - clockd as segment owner and JACK timebase master. Fill CLAP
     `transport` in the worker. Then Link, then LTC, then MTC, each with a
     measured proof (Link phase versus `link-hut`; LTC round trip through a
     loopback; MTC chase against a generator).
5. **Host formats.** *VST3, LV2 and presets done (§4.5).*
   - Still open: moving the CLAP adapter onto the shared core (`cph_*`, §6),
     a VST3 run check in the registry, LV2 `patch:Parameter` parameters
     (Surge XT's LV2 has only two control ports), saving LV2/CLAP presets,
     GUIs last.
6. **Player.** Files on the timeline, chasing clockd.
7. **Control service.** OSC and MIDI mapping across Backline, filter-chain
   controls and volumes, with MIDI-learn.
8. **Omarchy PRs**, each one problem with measured evidence, in this order:
   - "Record this app or device" (a panel action over `backline rec`);
   - "Play a system instrument from any MIDI keyboard" (an instrument
     picker from the registry);
   - "Index installed audio plugins", only together with one of the above.

   Before any of this, Omarchy would need to package Backline (AUR first),
   and the PRs would only add the bash front and the panel rows.

## 6. Coordination with clap-pipewire (read-only here; via its README and API)

- **Shared CLAP core.** The worker should link `clap-host.c` (`cph_*`)
  instead of keeping its own glue. Checked again with the format work
  (2026-11-11): neither ask below is met yet, so the worker's CLAP adapter
  stays; it already sits behind the interface the core would plug into.
  Two asks for that project:
  1. Install it as a static library or meson subproject with its header.
  2. Add note ports (`CLAP_EXT_NOTE_PORTS`, input events beyond params) to
     the core. Its README already lists "instruments and note effects" as
     step 4 and "a later out-of-process worker can reuse the same core".
- **One parameter vocabulary.** Use the same sanitised control names across
  in-graph (filter-chain `Props`) and out-of-process (D-Bus/OSC) instances.
- **Registry grades decide placement.** A `fail` plugin is never offered
  in-graph.
- **Upstream stance.** clap-pipewire won't go to PipeWire upstream (that
  project's AI-contribution policy). Backline follows the same stance:
  standalone projects, with Omarchy PRs only where Omarchy's own rules allow
  them. PipeWire issues found here go upstream as *reports and reproducers*,
  not code.

## 7. Decisions

The open questions of the first draft are decided; see
[docs/DECISIONS.md](docs/DECISIONS.md): one repository for now; instruments
are output streams auto-linked to the default output; GPL components stay in
separate processes behind IPC.
