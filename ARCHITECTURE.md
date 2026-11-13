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
 backline-recorderd   backline-playerd     backline-clockd                backline-controld ◄── backline-surfaced
 (N nodes → aligned   (files → nodes,      (transport + tempo = the       (OSC/MIDI → any    (MCU-family
  BWF/FLAC takes)      on the timeline)     PipeWire segment owner;         parameter; Params1: surfaces → Params1
                                             Link, LTC/MTC gen + chase)     programs' trees)   via Surface1 views)
        └──────────────────────── PipeWire graph (one clock) ───────────────────────────┘
                 clap-pipewire: CLAP as filter-chain "type = clap" (device effects, in-graph)
```

| Component | One line | Language | Status |
|---|---|---|---|
| `backline-registry` + `backline-probe` | The system plugin index (CLAP, VST3, LV2), with crash-proof scanning and validation on install | C | **Built and measured** (§4.4): sandboxed probes, grades, cache, `Registry1` |
| `backline-hostd` + `backline-plugin-worker` + `backline-slot-host` | Out-of-process plugin host: each instance one process and one PipeWire node, with MIDI in, params and presets over D-Bus and OSC; or, in slot mode, a strip's whole insert chain in one process inside an engine's cycle (`Slot1`) | C (hostd), C++ (worker, slot host) | **Built and measured** (§4.1, §4.3, §4.5, §4.14): CLAP, VST3 and LV2; presets; supervisor with restart-with-state; slot mode offline |
| `backline-recorderd` | Records any set of nodes to sample-aligned, timestamped multitrack WAV/BWF (RF64 past 4 GB) or FLAC; virtual soundcheck | C | **Built and measured** (§4.2, §4.6, §4.7): `Recorder1`, markers, takes, input-latency compensation, stall-proof disk thread, soundcheck |
| `backline-playerd` | Plays files into the graph on the shared timeline: each file a node, starting on an exact transport frame, with its own gain | C | **Built and measured** (§4.9): `Player1`, sample-exact starts, streaming past an in-memory head |
| `backline-clockd` | Transport, tempo and timecode: the PipeWire timeline owner, Ableton Link, LTC/MTC generation and chase | C++ (Link is C++) | **Built and measured** (§4.8): `Clock1`, LTC/MTC to the sample, Link |
| `backline-controld` | Maps OSC and MIDI to any parameter in the system: Backline instances, filter-chain/clap-pipewire controls, volumes, transport, recorder; serves the parameter trees programs publish (`Params1`) | C | **Built and measured** (§4.11, §4.12): `Control1`, MIDI and OSC learn, no socket unless asked, allow-lists and TCP secrets; `Params1` with OSC get/set/subscribe and meters; 14-bit CC, NRPN, relative encoders, takeover, MIDI feedback |
| `backline-surfaced` | Control surfaces (Mackie Control family: MCU Pro/XT, X-Touch/Extender) for any program, on USB/MIDI and on the network (RTP-MIDI): detection, handshake, groups of units, views, banking, layers, flip, motor faders, scribble strips, meters, hot-plug and network drops, a standby engine | C | **Built; measured against software devices only** (§4.12, §4.13): `Surface1`, `backline surfaces`, started at login; a real-hardware pass is pending ([HARDWARE-TEST.md](docs/HARDWARE-TEST.md)) |
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
- **Triggers** (built). A user `.path` unit
  (`backline-registry-rescan.path`) on `~/.clap`, `~/.vst3`, `~/.lv2`, the
  system plugin directories and `/var/lib/backline/plugins.stamp`; its
  service runs `backline plugins scan` after a 2 s debounce at idle I/O
  priority. A pacman hook for packagers (`data/pacman/backline-plugins.hook`)
  touches the stamp after any transaction that installs, upgrades or removes
  files under `/usr/lib/{clap,vst3,lv2}`, which also catches upgrades deep
  inside an existing bundle. The scan always runs as the user, never as
  root.
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
  - **Slot mode** (built, §4.14; [docs/api/Slot1.md](docs/api/Slot1.md)).
    For a mixing engine's inserts, one PipeWire node per plugin costs a
    quantum per hop. `backline-slot-host` instead runs **one strip's whole
    insert chain in one process, inside the engine's own cycle**. The
    engine creates a sealed memfd and copies the strip's audio in. It wakes
    the chain with a futex and waits on another until a deadline. If the
    chain is late, the engine plays the dry signal delayed by the chain's
    reported latency, so alignment holds. The chain is then stalled until
    it catches up or a new sandbox takes over. `Host1.LoadSlot(s shm, as
    plugins, a{sv})` supervises it like a worker: a scope, the registry
    gate, restart with state on the same memory. The slot host needs no
    PipeWire server; its loop serves only plugin timers. The contract is a
    header-only C file, `backline/slot1.h`, so an engine links nothing.
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
- **Transport** (built, §4.10). Every cycle the worker reads the shared
  transport from `spa_io_position` (the segment backline-clockd owns, or
  any JACK timebase master's): rolling, frame, tempo, meter and the beat at
  the cycle's first frame (`host/transport.h`). Each format gets it its own
  way: CLAP `clap_process.transport` (beats and tempo in quarter notes,
  seconds, bar start and number, signature, `IS_PLAYING`), VST3
  `ProcessContext` (`projectTimeSamples`, `projectTimeMusic`,
  `barPositionMusic` in quarter notes, tempo, signature, `kPlaying`,
  `systemTime`), LV2 a `time:Position` object (frame, speed, bar,
  barBeat, beat, beatUnit, beatsPerBar, beatsPerMinute) on every atom input
  that `atom:supports time:Position`, sent when the transport starts,
  stops, locates or changes tempo or meter, as jalv does. Without a segment
  owner, CLAP and VST3 get the frame and the rolling state only (no tempo
  flags) and LV2 gets frame and speed.
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
  - The ports' reported `SPA_PARAM_Latency` can't drive the fix: a null
    sink reports 0 frames of playback latency whether it drives or
    follows. (The first version read the capture direction by mistake and
    saw 1 quantum; D5.) The recorder logs it in the sidecar.
- **Input-latency compensation** (built). A capture port reports its
  converter delay as capture latency (`SPA_PARAM_Latency`, output
  direction, D5). Track *i* with latency L*i* is written from ring frame
  `begin + Li`, so frame *j* of every track is the same moment at the inputs;
  the take runs `max L` frames past the stop so all tracks end together.
  `latency`: `hardware` (physical ports only, the default), `reported`
  (every source), `off`, plus per-source manual frames. The applied values
  are in the sidecar and `CaptureLatency`.
- **Disk thread and rings.** One ring per track, 30 s by default
  (`ring-seconds` 2-120), mapped and pre-faulted at arm time, so the data
  thread never page-faults. The disk may stall for the ring's length without
  losing a frame (§4.6); past it, whole cycles are dropped from every track
  together and counted (`Overrun`), so tracks stay aligned.
- **RF64.** WAV takes are written as RF64 with automatic downgrade: a plain
  (extensible) WAV with a reserved `JUNK` chunk, which becomes RF64 with a
  `ds64` chunk only if a file passes 4 GB (EBU 3306). Markers are appended
  as a `cue ` chunk and `LIST/adtl` labels after the take, also to RF64.
- **Virtual soundcheck.** `Soundcheck(take)` plays a take on one node,
  `backline.soundcheck` (one port per track channel, all tracks in the same
  cycle; a disk thread reads ahead, the data thread only copies).
  `SetInput(node, "playback")` moves every link from that input to a
  consumer that isn't Backline's onto the matching soundcheck port, and
  `live` puts them back. Moves are written to
  `$XDG_RUNTIME_DIR/backline/soundcheck.json` first, so a crash doesn't
  strand a consumer: the next start puts the links back (§4.6).
- **Interfaces** (built). D-Bus `Recorder1` ([docs/api/Recorder1.md](docs/api/Recorder1.md)):
  `Arm`, `Record`, `Stop`, `AddMarker`, `Takes`, `GetTake`, `Soundcheck`,
  `SetInput`, `StopSoundcheck`; signals `Level`, `Overrun`, `Marker`,
  `TakeFinished`, `SoundcheckInput`. `backline rec arm|record|stop|...`
  speaks it; `backline rec -s ...` still runs the one-shot recorder.
- **Reuse.**
  - `pw-record` (one stream, no timestamps, so not alignable: −384 to −512
    frames apart in the control runs).
  - `jack_capture` (multichannel through JACK, GPL) and `timemachine`
    (pre-roll idea).
  - libsndfile (LGPL, dynamic) for WAV/BWF/RF64/FLAC.
- **Omarchy.** A "Record this app/device" action in the audio panel is
  plausible: a small bash command calling
  `backline rec`. The daemon stays standalone.

### 2.4 Player: `backline-playerd`

- **Job.** Play files (stems, cues, multitrack sessions) into the graph on
  the shared timeline: each file starts on an exact transport frame, with
  its own gain, and follows play, stop and locate of the transport.
- **Process model** (built). One process; one `pw_filter` per loaded file
  (a *clip*), `Stream/Output/Audio`, one output port per file channel, so
  every clip shows up in mixers and patchbays as its own stream (D7). The
  player never moves the transport: it follows what every node sees.
- **Timeline.** Every cycle a clip reads the transport frame *T* and the
  rolling state from `spa_io_position` (the segment clockd owns, computed as
  pipewire-jack does) and writes file frame *T − start + i* to sample *i*,
  times its gain; silence outside the file and while stopped. A start can
  fall anywhere inside a cycle. `tc:` and `bbt:` starts are resolved by
  clockd (`Clock1.Resolve`, added for this) with its tempo map.
- **Threads.**
  - *Data* only copies: from the in-memory head (the first `preload`
    seconds, 4 by default), then from a ring of tagged blocks. On a locate it
    posts a seek (generation counter + `sem_post`) and drops stale blocks by
    their tag, so the ring needs no reset handshake.
  - *Disk* fills the ring from the offset the data thread last asked for:
    while stopped it prepares the position the transport will start from.
  - *Main* is D-Bus, linking (`target`, by channel name) and stats.
- **Gain.** Per clip, in dB; a change ramps over one cycle, then samples are
  exactly file × gain.
- **Interfaces** (built). D-Bus `Player1`
  ([docs/api/Player1.md](docs/api/Player1.md)): `Load(file, {at, gain,
  target, ...})`, `Unload`, `SetGain`, `SetStart`, `Get`, `List`; signals
  `Loaded`, `Unloaded`. CLI `backline play load|unload|gain|start|list|show`.
- **Not built yet.** Resampling (a file at another rate than the graph is
  silent and says so: `rate_ok: false`), loop ranges, cue lists, OSC
  `/backline/player/...`.
- **Reuse.** libsndfile for every format it reads. `pw-play` covers the
  single-file, untimed case.
- **Omarchy.** Nothing. This is a production tool.

### 2.5 Clock, transport and timecode: `backline-clockd`

- **Job.** One system transport (rolling, position, tempo, meter) that
  every app can follow. Also LTC and MTC generation and chase, and Ableton
  Link.
- **Mechanism** (built; D4).
  - clockd is a JACK client of pipewire-jack: that is how a client owns
    the segment and moves the transport (node activation), and the result is
    in every node's `spa_io_position`.
  - **Use PipeWire's own timeline.** `spa_io_position` carries a `segment`
    with an `owner` and a `bar` (bpm, signature, beat) to every node, every
    cycle. pipewire-jack maps that to the JACK transport and timebase
    master.
  - clockd becomes the **segment owner and JACK timebase master**. Ardour,
    Bitwig, the Backline worker (as CLAP `transport`) and every other
    JACK/PipeWire client then follow one transport without new APIs.
  - The worker fills every hosted plugin's transport from it (CLAP
    `transport`, VST3 `ProcessContext`, LV2 `time:Position`; §2.2, §4.10).
  - **Beat to the frame.** pipewire-jack carries JACK's BBT as the bar's
    `beat`, floored to the tick (1/1920 beat: 12.5 frames at 120 BPM), and
    JACK's `bbt_offset` as the bar's `offset`. clockd sets `bbt_offset` to
    the frames since that tick, so a reader gets the beat at the cycle's
    first frame to within a frame (`bar.beat + offset × bpm / 60 / rate`).
    pipewire-jack writes `bar.beat` with JACK's 1-based bar (bar 1, beat 1
    is `beat = beats_per_bar`) and reads it back the same way; the worker
    uses that exact inverse.
  - **Tempo map.** Frame 0 is always bar 1, beat 1. A tempo or meter change
    while stopped sets the song's tempo from the start; a change while
    rolling adds a stretch at the transport's frame (the beat doesn't jump),
    and the map keeps it, so locating back before it plays the earlier
    tempo. (The first version kept one anchor at the last change and
    clamped earlier beats to 0: after a tempo change, `Locate 0` froze the
    beat until the transport passed the old anchor. §4.10 found it.) Bars
    are counted in the current meter from beat 0, so bar numbers after a
    meter change while rolling are not a real multi-meter map yet.
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

- **Job.** Map OSC and MIDI to *any* parameter in the system (built, §4.11):
  - Backline instance params (`instance:NODE/PARAM`, through `Instance1`);
  - filter-chain and clap-pipewire controls (`control:NODE/NAME`, `Props`
    `params` on the node);
  - node volumes and mutes (`volume:NODE`, `mute:NODE`: the node's
    `channelVolumes` and `mute`, the same gain `wpctl` and the mixer set);
  - transport (`transport:play|stop|toggle|tempo|locate:POS`, `Clock1`) and
    recorder actions (`recorder:arm|record|stop|marker`, `Recorder1`).
- **Process model.** One daemon, all on the main loop. One PipeWire node
  (`backline.control`, `Midi/Sink`) with a MIDI input port; its process
  callback only copies MIDI 1.0 (or UMP, converted) into a wait-free ring and
  wakes main. MIDI sources reach it through `SetMidiFollow(regex)`, as with
  the worker. OSC listeners (UDP, and TCP with OSC 1.0 size-prefix framing)
  are sources on the same loop. Mappings live in
  `$XDG_CONFIG_HOME/backline/control/*.json` (`backline.control/1`); `Save`
  writes `control.json`. MIDI and OSC learn (`Learn`, signal `Learned`).
- **Delivery.** D-Bus targets are called asynchronously; while a mapping's
  call is in flight, newer values replace each other and only the latest is
  sent, so a fast fader can't queue up a backlog. An instance parameter's
  range is read once with `ListParams`.
- **Security** ([Control1.md](docs/api/Control1.md#security)). It's the only
  component that can listen beyond localhost, so: **no network socket at all
  by default** (not even loopback); listeners only by config or `Listen`, on
  a literal IP (no host names, no wildcards); beyond loopback an allow-list
  is required, TCP needs a shared secret (`/backline/auth` first, constant-time
  compare, 30 s block after 3 failures), UDP needs an explicit
  `insecure-udp`; the network can only move existing mappings (nothing
  received creates or changes one); per-peer rate limits, size limits,
  client limits; whole-packet validation before use.
- **Reuse.**
  - Its own OSC codec (`control/osc.c`, bundles included) and network policy
    (`control/policy.c`), unit-tested with malformed and random packets:
    liblo would have been a new dependency, and owning the sockets keeps the
    security rules in one small file.
  - mididings (GPL) only as a design reference for mapping rules.
- **Params1: a program publishes its parameter tree** (built, §4.12,
  [Params1.md](docs/api/Params1.md)). A program (a mixer, open or
  proprietary, a DAW, a script) registers a root (`/console`) with typed
  parameters (ranges, units, tapers including a console fader law, value
  text, `automatable`/`read-only`), pushes confirmed values with `Update`, and
  hands over a sealed memfd meter table (a seqlock; no bus traffic per meter
  value). Clients ask (`Set`, `Gesture`); the owner gets a unicast signal,
  applies the change its own way and confirms. The open layer never owns the
  program's state. Roles name the path patterns a client may write. A client
  that leaves the bus has its gestures ended for it; an owner that leaves has
  its root removed.
- **OSC replies and subscriptions** (built): a listener given a `params`
  role serves the tree to its peers: get, set, `/norm`, `/touch`,
  subscriptions with a rate cap per path, meter streams, `/backline/error`
  answers. UDP subscriptions expire without a renewal; TCP ones live with the
  connection. Without that option a listener is exactly what it was
  (security rule 7 holds).
- **MIDI additions** (built): `midi:cc14`, `midi:nrpn`, `midi:rpn` sources;
  relative encodings (two's complement, sign-magnitude, binary offset,
  increment/decrement) with step and acceleration; soft takeover (pickup,
  scale); `param:` targets with automatic gestures; and a MIDI **output**
  port (`SetMidiOut`) for feedback to motor faders and LED rings. The pure
  parts (decoders, takeover, feedback bytes) are in `mapping.c`, unit-tested.
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

### 2.9 Surfaces: `backline-surfaced`

- **Job.** Hardware control surfaces for any program
  ([Surface1.md](docs/api/Surface1.md)). The program pushes **views**
  (ordered slots whose fader, pots, buttons, name, colour and meter are bound
  to `Params1` paths) and pot modes; surfaced puts them on whatever is
  plugged in. It knows devices, views, slots and paths, never what an aux or
  a DCA is: spill, "mix on faders" and follow-selection are a program's
  policy, built from `PushView`/`PopView`, `SetPotMode`, `SetSelected`,
  `SetFlip` in answer to `Action` signals.
- **Process model.** Its own process, next to controld (§0.2: a surface
  driver parses device input, and a misbehaving device must not stall OSC
  control or the mixer). All on the main loop. Devices are found as PipeWire
  MIDI ports (the MIDI bridge's, or any node's), matched against profiles
  (`*.surface.json`, data, user-overridable), paired by node and alias. Each
  unit gets a small node with a MIDI port each way, linked to the device
  (links die with surfaced); its process callback only moves bytes between
  the ports and two wait-free rings. A byte-stream parser reassembles SysEx.
- **The engine** (`surface/engine.c`, pure, unit-tested in CI against the
  software device model). Feedback is computed, not queued: every 5 ms it
  works out what each device should show and sends the differences in
  priority order (motors, LEDs, rings, meters, text) within the unit's byte
  budget and per-element rate caps. A value that changed ten times since the
  last send is sent once; a replugged device is redrawn by forgetting what
  it was sent. Safety: an untouched fader never moves a parameter (motor
  overshoot); touch is a `Params1` gesture; an unplug ends every gesture of
  that unit at once; a touch held 60 s without motion ends its gesture.
- **Groups.** Units of one group (an X-Touch and its Extender) are one wide
  window; a unit's id is its profile and serial (from the MCU handshake), so
  a replugged unit returns to its group and place. Groups, roles and locked
  views are kept in `$XDG_STATE_HOME/backline/surfaces.json`.
- **Drivers.** MCU now (Mackie and Logic Control handshakes, faders, touch,
  V-Pots and rings, 2×56 LCD, X-Touch colours, meters, 7-segment displays,
  every button id). HUI, the Waves FIT's native mode and others are next
  (§5). Written from the Logic Control MIDI implementation and the console
  spec's tables; Ardour's GPL surface code was read for facts only.
- **Network.** RTP-MIDI in surfaced itself (`surface/rtpmidi.c`, pure;
  `surface/rtpnet.c`, sockets and mDNS through avahi-daemon; DECISIONS D10).
  A network device is a unit like a USB one: same profile, same engine. It is
  found on mDNS, invites us, or is added by address. Its MIDI is batched into
  one packet per engine tick. Silence for 3 s is an unplug: gestures end, it
  is invited again, and it returns to its place. `--standby` holds sessions
  without driving them, for a second engine ([SURFACES.md](docs/SURFACES.md)).
- **Start.** A systemd user unit, installed enabled; D-Bus activation goes
  through it. `backline surfaces` lists what is connected.
- **Omarchy.** A panel row listing surfaces is plausible once a mixer uses
  them.

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
| OSC | small codecs of our own: the worker's (localhost) and controld's (bundles, TCP framing, unit-tested) | liblo would be a new dependency for what the codecs already do |
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

### 4.6 Recorder daemon (`tests/recorderd-proof`)

On a private D-Bus, against throwaway nodes.

- **API.** Arm returns in 28 ms with the links up; the errors are typed
  (`State`, `NotFound`, `Busy`, `InvalidArgs`). Two markers land in the
  sidecar and as WAV cue points with labels at the same frames; `Level`,
  `Marker`, `TakeFinished` and `PropertiesChanged` arrive.
- **Input latency.** Three fake capture devices (`backline-test-source`,
  one process, one start cycle) declare and apply 0, 480 (physical) and 256
  frames. Measured lag of b and c against a: `off` 480/256, `hardware`
  0/256 (c isn't physical), `reported` 0/0, `hardware` + manual 256 for c
  0/0; bit-identical at those lags, equal lengths. In 2 of about 50 such
  takes during development one fake source arrived one quantum late for a
  whole take, a different source each time (PipeWire ran the recorder
  before that node; to investigate, reported here rather than hidden).
- **Slow disk.** An `LD_PRELOAD` shim blocks every write to the take
  directory for 12 s from 3 s and 8 s from 20 s into a 40 s take, in the
  disk thread. **0 frames lost**: all 1 920 000 frames of both tracks equal
  an unstalled reference recorded at the same time. Control: the same stalls
  with a 4 s ring lose 9.1 s, say so (`overrun_frames`), and keep the
  tracks aligned at lag 0.
- **Virtual soundcheck.** Two virtual mics feed a FOH sink through links
  someone else made. Switching each mic to playback and back, the FOH gets
  the take bit-exact where it got that mic, the other mic stays live, both
  tracks play in lockstep (same take frame), and the links after `live` are
  identical to before. SIGKILL with a mic switched leaves the consumer
  without that link; the next start puts it back. (One development run had
  a single 128-frame silent cycle on both FOH channels at once: a graph
  xrun, with other benchmarks loading the machine and no realtime priority.)
- **Missed cycles.** The sidecar now counts the cycles the graph didn't run
  the recorder for (`graph_gaps`, `graph_gap_frames`), separately from ring
  overruns, so a scheduling xrun can't pass for a disk problem or hide.

### 4.7 RF64 (`tests/rf64-proof`)

A node generates 16 sines from the graph position in its realtime callback;
the recorder writes one 16-channel float track for 1460 s, 4 485 120 942
bytes in one file, while a second, independent recorder records the same
node. Measured: the file is RF64 with `ds64` sizes equal to the file and the
data, 70 080 000 frames read back (as asked), `bext` present, and both
markers (one past 4 GB) in the cue chunk; 0 ring overruns.

**Continuity (re-run for 0.4.0, 2026-11-12):** with the take in its own
node group under its own test driver at a forced quantum of 1024, **all
70 080 000 frames of all 16 channels are continuous (0 discontinuities)
and equal to the independent reference recording (0 of 70 080 000 frames
differ); neither recorder missed a graph cycle.** The machine was not idle:
two contribution coder runs and two background Claude Code sessions ran
throughout (load 1.85-2.86 on 16 CPUs, logged every minute), after a
90-minute wait for a quiet window that didn't come; no realtime priority.
The 0.3.0 runs, at the graph's quantum and sharing a driver with real
devices while three benchmarks loaded the machine, missed 61 and 48 cycles
and broke the sine accordingly.

### 4.8 Clock (`tests/clockd-proof`)

In its own network namespace (D6), on a private bus, with clockd and every
test node in one `node.group` driven by a test sink.

- **Transport.** A native PipeWire node logging `spa_io_position` every
  cycle sees the frame advance by exactly the quantum while rolling, locates
  by frames, timecode and bar/beat land exactly, and the bar follows tempo
  and meter changes to the tick (worst 0.76 tick of step error, from tick
  quantisation). pipewire-jack publishes the timebase one cycle late and
  lands a locate two cycles after it's asked: clockd compensates both (D4).
- **LTC generate**, decoded by libltc in the test node: frame starts on the
  transport within 0 samples at 25 and 30 fps, 1 at 29.97 DF, 2 at 24 (the
  decoder's own precision there), through a locate.
- **LTC chase** (25 and 29.97 DF, and through a sink's monitor): locked
  37-45 ms after the timecode starts or jumps, then 0 samples of error for
  every cycle; the transport stops when the timecode stops.
- **MTC** generate: every quarter frame on its exact transport sample.
  Chase: locked in 61-75 ms, then 0 samples of error, through a jump.
- **Link.** With a second peer (`backline-test-linkpeer`), peers = 1 on both
  sides, a tempo proposed by either side becomes the session's and the
  transport's, and the beat phase between PipeWire's bar and the peer's own
  Link timeline stays within 0.63 ms (two ticks) while rolling, excluding the one cycle in
  which a tempo change was still crossing the network.

### 4.9 Player (`tests/player-proof`)

In its own network namespace, on a private bus, with clockd, playerd and two
capture nodes in one `node.group` driven by a test sink. The capture node
(`backline-test-tap`) is a native node that keeps every sample it receives
with the transport frame of its cycle. A stereo file starts at frame 96000
(0 dB), a mono file at `bbt:3|1|0` (−6 dB; clockd resolved it to 192000 at
120 BPM). Moves: play from 0 across both starts and past the 4 s in-memory
head, a gain change to −12 dB while rolling, a locate into the middle while
rolling, stop + locate to 71999 + play (the starts then fall inside a cycle),
stop + locate into the streamed part + play. Every received sample is
compared with the file at (transport − start) × gain, exactly. Two runs, at
the graph's quantum (128) and a forced 1000:

| | quantum 128 | quantum 1000 |
|---|---|---|
| samples inside a clip while rolling | 1 830 589 | 1 821 997 |
| bit-identical to file × gain | all but the 256 of the gain-ramp cycle | all but the 2 000 of the gain-ramp cycle |
| wrong, or not silent where they must be | 0, 0 | 0, 0 |
| first sample of each start (on a cycle boundary, and 65 or 1 frames into a cycle) | at its exact transport frame | at its exact transport frame |
| underruns | 0 | 0 |

- One development run lost 128 frames (2.7 ms, one cycle) right after a
  locate into the streamed part while rolling, before the ring had refilled;
  the final runs lost none. A locate into the head, or any locate while
  stopped, is exact by construction.
- A 44.1 kHz file on the 48 kHz graph stays silent and reports
  `rate_ok: false` (no resampling yet). Errors are typed (`NotFound` for a
  missing file or target node, `InvalidArgs`, `Busy` for a taken id).
- No link to a non-test node at any time (a watchdog checks every 0.3 s);
  nothing left; the other nodes and the defaults unchanged.

### 4.10 Transport in hosted plugins (`tests/transport-proof`)

In its own network namespace, on a private bus, in a test node group, as
§4.8. Full report: `docs/proofs/transport-proof.txt`.

- **Beat-click test plugins**, one source built as CLAP, VST3 and LV2
  (`tests/plugins/beatclick*`): a unit impulse on the first sample at or
  after every beat the host reports. Each runs in the worker while clockd
  plays 120 BPM 4/4 from frame 0, changes to 90 BPM, then 7/8, locates to
  `bbt:9|1|0` while rolling, stops, locates to frame 96000 and plays again.
  An independent native node records the plugin's output with the
  `spa_io_position` of the same cycle, so every sample has its transport
  frame. **All three formats: every click on the sample PipeWire's segment
  says. Against clockd's tempo map, CLAP and VST3 are 0 samples off in every
  stretch, LV2 0-1** (one click per quarter note for CLAP and VST3, per 1/8
  in 7/8 for LV2; spacing exactly 24 000, 32 000 and 64 000 samples; a click
  on the first frame after each locate to a beat). LV2's 1 is the position
  sent at a tempo change, which the plugin then counts from: that cycle's
  beat is floored to a whole frame, which CLAP and VST3 get corrected on the
  next cycle (the worker carries the beat forward, `host/transport.h`).
  Without the bar's `offset` (the tick-floored beat alone), up to 7 of 12-14
  clicks land 0-15 samples off across runs.
- **A real tempo-synced plugin.** Odin2's arpeggiator (CLAP, VST3, LV2), one
  held note, 120 then 90 BPM: every eighth-note step is on clockd's grid,
  none missing, within two 128-frame cycles of spread and drift, at both
  tempos. Odin2 plays each step early: 260-291 samples as CLAP and VST3
  (identical in every run), 130-430 samples as LV2, varying from run to
  run, and re-phasing by about one cycle when it gets a new position
  mid-roll (after a missed graph cycle). With the test plugins exact, the
  offsets are Odin2's own: they scale with the block size (about 1 540
  samples at quantum 2048). It also plays the first step at note-on while
  the transport is stopped, then waits for it to roll.
- **Found and fixed.** clockd's tempo anchor (§2.5, *Tempo map*): after a
  tempo change, `Locate 0` froze the beat at 0 until the transport passed
  the old anchor, 0.25 s more with every take. And the beat's tick floor:
  clockd now sets `bbt_offset`.
- Another clockd on the same machine (another proof running at the same
  time), outside the test group, ended up under the test group's driver
  and moved its transport during development. PipeWire schedules a node
  without links under whichever driver is running, so concurrent proofs
  meet. The proof now waits for a graph without other Backline test nodes,
  forces quantum 128 on its driver, fails on any JACK client from outside
  the group under its driver, and voids a take whose transport jumps
  anywhere it didn't locate to. Load 3-5 from other agents, no realtime
  priority.

### 4.11 Control (`tests/controld-proof`)

In its own network namespace (only loopback and three test addresses, so no
listener can reach a real network), on a private bus, against throwaway
nodes, with a watchdog that stops the proof if any of its nodes is linked to a
non-Backline node. Full report in `docs/proofs/controld-proof.txt`.

- **Nothing listens by default.** With an empty config controld has no
  network socket. Eight unsafe listeners are refused with their reason:
  `0.0.0.0`, `::`, a host name, a LAN address without an allow-list, UDP
  without `insecure-udp`, TCP without a secret, a short secret, a secret on
  UDP.
- **MIDI CC → plugin parameter.** A virtual keyboard through PipeWire's MIDI
  bridge, CC 7 → Surge XT "Global Volume" in its worker: the value reads
  back exactly (CC 100 → 0.787402 = 100/127), the level for each CC equals
  the level the same value gives over D-Bus (worst 0.00 dB), keyboard send to
  controld's main loop about 2 ms, `SetParam` answered in 0.1-0.2 ms, send to
  sound 5-7 ms.
- **OSC → transport, recorder.** Play, locate to 10 s, tempo, stop over UDP;
  toggle twice over TCP with a secret; arm, record, marker and stop of
  recorderd (the take has the marker). controld to clockd answered in
  about 0.1-0.2 ms.
- **Volume and mute** of a throwaway sink, re the tone's own level: CC 64 →
  −29.764 dB, CC 100 → −12.756 dB, OSC 0.5 on a −40..0 range → −20.000 dB,
  each exact to 0.001 dB; CC 0 and mute are digital silence; unmute restores
  −20.000 dB. `wpctl` (what the mixer reads) shows the same volume.
- **Learn.** The first CC moved and the first OSC address sent became
  mappings that work; a learn with nothing coming times out.
- **Security, 10 cases.** A wrong secret gets `/backline/auth 0` and is
  closed; a mapped message before authenticating is closed and does nothing;
  a peer off the allow-list is dropped over TCP and UDP even with the right
  secret; a malformed packet is counted and does nothing; a flood of 2 000
  messages to a 100 msg/s listener gets 100 through; OSC messages that look
  like management (`/backline/map`, …) change nothing; after 3 wrong secrets
  an address is refused even with the right one, while another allowed
  address still gets in. `Save` writes mode 0600; a restart restores the same
  mappings and listeners; no socket is left after exit.

### 4.12 Surfaces and Params1 (`tests/surfaced-proof`)

On a **private PipeWire instance** (its own socket and a dummy driver, no
devices) in its own network namespace, on a private bus, with the user's
graph checked before and after (nothing of the proof ever appears in it).
A stub mixer (`tools/test-mixer.c`, public APIs only) registers 24 channels
(285 parameters, 64 of them bulk, and a meter table) and pushes views; two software devices
(`tools/mcu-sim.c`: an X-Touch Extender and an X-Touch, the device side of
the protocol with motor faders, touch and displays) stand in for hardware,
and a third plays a generic CC/NRPN controller. 82 checks; full report in
`docs/proofs/surfaced-proof.txt`.

- **Params1 over D-Bus and OSC.** A Set reaches the mixer as a unicast
  signal and comes back as an `Update`; an undefined role, a read-only
  listener, a non-automatable gesture, a wrong type and a second owner are
  refused. OSC set → mixer → echo to the subscriber ≈ 5 ms. 200 changes in
  1.3 s against a 30/s subscription: 35 replies, ending on the final value.
  Meter stream 15 frames a second.
- **MIDI additions.** A 14-bit CC (64/0) sets the middle of the fader law
  (-10.0 dB) with an automatic gesture; NRPN 291 at 127/127 sets +10 dB; both
  are fed back on the same CCs once the mixer confirms; a two's-complement
  encoder moves pan +3 and -1 ticks; pickup ignores the control until it
  crosses the parameter; a change from the mixer reaches the encoder's ring.
- **Surfaces.** Detection by port name and the MCU handshake (unit ids
  `behringer-xtouch-ext@EXT0001`, `behringer-xtouch@XT00002`), one 16-wide
  group; names, colours, faders, meters and the assignment display as the
  mixer has them. A touched fader move reaches the mixer in 0.7-5.3 ms
  (within one graph cycle: 5.3 ms at quantum 256); an untouched one never does; the position is
  echoed while held and the motor keeps the confirmed value after release; a
  change in the mixer moves the motor. Banking, channel and first-bank;
  Mute/Solo with their LEDs; the layer buttons (Aux, Inputs) with LEDs; flip;
  pots with rings and gestures; Select → the mixer's `SetSelected` → an EQ
  strip mode on that channel; a message across both displays; the timecode
  display bound to a string parameter; F2 bound to the mixer's talkback.
- **Hot-plug.** The X-Touch unplugged while a fader is held: the gesture
  ends well inside 50 ms; the window is dealt again over the extender (8
  wide); plugged back, it returns to its place and is redrawn (about 1 s,
  the greeting included).
- **Groups and restarts.** The extender alone in a group locked to the aux
  view, not following the other's banking; its sets go out in its group's
  role (refused: the mixer defined none). A clean stop leaves both devices
  dark and offline (a 112-character LCD write through PipeWire); a restart
  restores the saved groups.
- **In CI** (`tests/unit/test_surface.c`): the same engine against two
  device models in simulated time, 233 checks, including a DIN-paced full
  redraw (577 bytes, complete in 190 ms, at most 315 bytes in any 100 ms).
- **Not measured: real hardware.** Pending; [HARDWARE-TEST.md](docs/HARDWARE-TEST.md).

### 4.13 Network surfaces (`tests/surfaced-net-proof`)

In a user, network and mount namespace (no sudo), whose only interfaces are
`lo` and a dummy `bl0`. A private avahi-daemon runs on a private system bus,
plus a private PipeWire and session bus, and the user's graph is checked
before and after. An X-Touch Extender in RTP SLAVE mode (found on mDNS) and
an X-Touch in RTP MASTER mode (it invites surfaced on UDP 5004) are software
devices (`tools/mcu-sim.c --net`). 50 checks; full report in
`docs/proofs/surfaced-net-proof.txt`.

- **The same as USB.** Same profiles and unit ids (`profile@serial`), one
  16-strip group, names, colours, motors, meters, touch and gestures (an
  untouched move is never applied), banking, LEDs. A surface move reaches
  the mixer in about 0.2 ms; feedback goes out once per 5 ms engine tick, one
  packet per unit.
- **Drops.** With the X-Touch's cable pulled while a fader is held, the
  gesture ends at the mixer 2.8 s later and the window is dealt again. With
  the cable back, the unit is online in 1 s, in the same group and place,
  with a new handshake and a full redraw. A touch from before the drop never
  applies. The Extender's cable and its power switch behave the same way.
- **Two engines.** A standby surfaced, with its own bus, controld and mixer,
  holds its own session with the Extender (it allows two) and sends
  nothing. The X-Touch (one session) refuses it. When the main stops, the
  standby gets the X-Touch 2.5 s later, and `backline surfaces activate`
  draws both in about 1 s. Their faders then move the standby's mixer.
### 4.14 Slot mode (`tests/slot-proof`, `tests/unit/test_slot1.c`)

Offline, with no audio server. `backline-test-slot-driver` plays the
engine, against the real `backline-slot-host` and a test effect built as
CLAP, VST3 and LV2 (`tests/plugins/slotfx.h`). The effect is a gain with a
fixed, reported latency, and switches that crash, oversleep or allocate.
Numbers are in [Slot1.md §7](docs/api/Slot1.md); the full report is in
`docs/proofs/slot-proof.txt`.

- **+0 frames.** An impulse through a 0-latency chain comes out at the same
  frame of the same cycle. Through 37-frame and 0+37+300 chains, it comes
  out at exactly the reported latency. This holds in every format and in a
  mixed CLAP+VST3+LV2 chain.
- **Deadline.** An insert that sleeps three periods gives one late block and
  three stalled ones. Every one of them is the input delayed by 37 frames,
  sample-exact. The chain is back on the fourth block.
- **Crash.** Chain A's plugin segfaults. A plays delayed dry until a new
  sandbox takes the memory over: the driver restarts it, and so does hostd
  through `LoadSlot`, in 3 ms, with the saved gain. Chain B, in its own
  process, plays every sample wet and right.
- **Allocations.** Zero heap calls on the sandbox's audio thread or in the
  engine's cycle over 5,000 cycles of four inserts with parameter traffic.
  A plugin that allocates is counted.
- **Round trip** at 64 frames: p99 155–180 µs against a 667 µs deadline, at
  normal priority. The realtime run waits for the hold to lift and for the
  P5 realtime limits.

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
   - Auto-rescan *done*: a user `.path` unit on the plugin directories and a
     stamp the packagers' pacman hook touches (`tests/rescan-proof`: an
     install, a pacman-style stamp and a removal each rescan once, ≈ 2.1 s
     after the change, the 2 s debounce included).
   - Still open: the optional clap-validator pass, VST3 run checks, caching `lv2-list`.
3. **Recorder.** *Done (§4.2, §4.6, §4.7)*: alignment, pre-roll,
   `recorderd` with `Recorder1`, RF64, input-latency compensation, the
   stall-proof disk thread, virtual soundcheck.
   - Still open: latency compensation checked against a real converter's
     measured round trip; resampling in soundcheck.
4. **Clock and transport.** *Done (§4.8)*: clockd as segment owner, LTC
   and MTC generate and chase, Link.
   - The transport in hosted plugins *done (§4.10)*: CLAP, VST3 and LV2,
     to the sample.
   - Still open: Link phase against an output-latency-compensated timeline;
     bar numbering across meter changes (a multi-meter map).
5. **Host formats.** *VST3, LV2 and presets done (§4.5).*
   - Still open: moving the CLAP adapter onto the shared core (`cph_*`, §6),
     a VST3 run check in the registry, LV2 `patch:Parameter` parameters
     (Surge XT's LV2 has only two control ports), saving LV2/CLAP presets,
     GUIs last.
6. **Player.** *Done (§4.9)*: files on the timeline, sample-exact starts,
   per-file gain. Still open: resampling, loops and cue lists, OSC.
7. **Control service.** *Done (§4.11)*: OSC and MIDI mapping across
   Backline, filter-chain controls and volumes, with MIDI and OSC learn.
   - *Done (§4.12)*: OSC replies and subscriptions (through `Params1`),
     relative encoders, 14-bit CC pairs, NRPN/RPN, soft takeover, MIDI
     feedback out.
   - Still open: `slot:` targets (a generic box banking with a surface
     group's views) and `Clock1.Tempo` reading the anchor while the transport
     is stopped (today it shows the last rolled tempo).
8. **Surfaces.** *Built (§4.12)*: `Params1` in controld, `backline-surfaced`
   with `Surface1`, the MCU driver and profiles for the MCU Pro/XT and the
   X-Touch/Extender, tested against software devices.
   - Pending: the bench pass on real devices ([HARDWARE-TEST.md](docs/HARDWARE-TEST.md)).
   - Next: the HUI driver (keepalive, zones, 4-character text, stereo
     meters); the Waves FIT in MCU mode (two units, one profile) and then its
     native LV1-mode driver after a bench capture; more MCU profiles (X-Touch
     One, iCON, FaderPort in MCU mode, …); `Instance1` additions surfaces want
     (value text, gesture passthrough, `ParamsChanged`, latency).
   - *Done (§4.13)*: network surfaces (RTP-MIDI), the standby engine,
     `backline surfaces`, the user unit that starts surfaced at login.
9. **Omarchy PRs**, each one problem with measured evidence, in this order:
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
