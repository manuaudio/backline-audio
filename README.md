# Backline

**System audio services for Linux, built on PipeWire.** Backline hosts audio
plugins as ordinary PipeWire nodes. It keeps an index of every installed
plugin and records any set of nodes to sample-aligned, timestamped files.
Every piece is a small, separate process that apps, the desktop shell and
scripts can drive over D-Bus, OSC or a command line. It covers the part of
macOS's CoreAudio system layer, AudioComponent registry and AUv3 hosting
that Linux has no shared answer for yet.

> **Status: early, working slices; not a product.** The parts below are built,
> unit-tested and measured on a real PipeWire 1.6.8 system (see
> [Proofs](#proofs)). The D-Bus interfaces are 0.x and will change. There are no
> packages yet.

The design and the reasoning behind it are in [ARCHITECTURE.md](ARCHITECTURE.md).
The choices that shape the project are in [docs/DECISIONS.md](docs/DECISIONS.md).

## What it does today

- **Run a plugin as a system instrument or effect.** Each instance runs as one
  process and one PipeWire node, with MIDI from any PipeWire MIDI source. It
  follows keyboards as they are plugged in, and you can set its parameters,
  play notes and save its state over D-Bus and OSC.
- **Survive plugin crashes.** A supervisor runs each instance in its own systemd
  user scope. If the plugin crashes, it comes back in about 80 ms with the
  same parameters, node name and OSC port, and keeps playing. A crash loop
  backs off and then stops. Nothing else in the graph is affected.
- **Know what's installed.** One scan finds every CLAP, VST3 and LV2 plugin.
  Each plugin is probed in its own sandboxed process with a time limit, so
  crashes and hangs are contained. It is graded pass, warn or fail, and the
  results are cached by mtime and SHA-256. You can query the index over D-Bus
  or as JSON.
- **Record several nodes in one take, sample-aligned.** Output is WAV/BWF or
  FLAC, with a BWF time reference and a JSON sidecar. Tracks stay aligned
  even when one of the recorded sinks drives the graph, and there is a
  pre-roll so a take never misses its first notes.

## On the desktop: Omarchy panels and a mixer

On [Omarchy](https://omarchy.org), Backline lives in the desktop's own workflow:

- **Four bar panels:** Instruments, Record, Plugins and Transport. They're built from Omarchy's own shell components and theme tokens.
- **`omarchy-backline-*` commands.**
- **Menu rows** in the Omarchy menu.
- **`Super + Ctrl` hotkeys.**

A larger **mixer window** has a channel strip with a live meter for every input, instrument and output, plus the transport and the takes. It follows the Omarchy theme and language live. Every job takes a few keystrokes, measured end to end against the real services.

| Play an instrument | Record a set | Soundcheck | Check a plugin | Lock to timecode |
|---|---|---|---|---|
| ![Instruments: the picker](docs/screenshots/2-instruments-picker.png) | ![Record: recording two sources](docs/screenshots/6-record-recording.png) | ![Record: a take in place of its inputs](docs/screenshots/8-soundcheck.png) | ![Plugins: a failing plugin and why](docs/screenshots/9-plugins-finding.png) | ![Transport: locked to LTC](docs/screenshots/10-transport.png) |
| 6 keys first time, 3 again | 5 keys; 1 from anywhere after | 3 keys | 6 keys, no Enter | 6 keys first time, 4 again |

![The mixer, recording while a keyboard plays an instrument](docs/screenshots/11-mixer.png)

Measured:

- **Meter latency:** a key press on a MIDI keyboard reaches the meter's pixel in 21–39 ms (median, over two runs). Of that, the mixer's own part is 16–18 ms; the rest is the MIDI bridge, the plugin and the graph.
- **Mixer CPU:** about 5% of one core while metering.
- **Closed panels:** they start no processes.

See [omarchy/README.md](omarchy/README.md) (panels, commands, menu, hotkeys, install), [ui/README.md](ui/README.md) (the mixer), [ui/UX-FLOWS.md](ui/UX-FLOWS.md) (the jobs, key by key) and [ui/DESIGN-LANGUAGE.md](ui/DESIGN-LANGUAGE.md) (the look). The screenshots are from the flows test, which runs on a private headless compositor with a stand-in bar.

## Components

| Component | What it is | State |
|---|---|---|
| `backline-plugin-worker` | One CLAP plugin as one PipeWire node, in its own process. MIDI in (follows hotplugged keyboards, releases stuck notes on unplug), audio in/out, params, notes and state over D-Bus (`Instance1`) and localhost OSC | Built, measured |
| `backline-hostd` | The supervisor (D-Bus `Host1`). It runs one worker per instance in a transient systemd user scope with resource limits. After a crash it restarts the worker with the saved CLAP state, with backoff and a give-up limit. It refuses plugins the registry grades `fail` unless forced | Built, measured |
| `backline-registry` + `backline-probe` | The plugin index (D-Bus `Registry1`, JSON `backline.plugins/1`). Each plugin is probed in its own process, sandboxed with timeouts, then graded and cached | Built, measured |
| `backline-recorder` | N nodes into one take: WAV float/24-bit or FLAC, BWF `bext`, a sidecar with UTC, monotonic and graph timestamps. Signal alignment across driver and follower sinks, and pre-roll | Built, measured |
| `libbackline` | The MIT C client library (sd-bus only, no PipeWire dependency): `Host1`, `Instance1`, `Registry1` calls and a signal watcher | Skeleton, used by the CLI |
| `backline` | The command line over `libbackline`; `--json` on every command | Built |
| Clock/transport, player, control mapping, QML module | Ableton Link, LTC/MTC, playing files on the timeline, OSC/MIDI mapping | Design only ([ARCHITECTURE.md](ARCHITECTURE.md)) |

In-graph CLAP effects (a plugin inside PipeWire's filter-chain, for device
effects) are a separate project,
[clap-pipewire](https://github.com/manuaudio/clap-pipewire).

## Build

You need PipeWire ≥ 1.4 development headers (1.6 is preferred: the worker
then uses the newer pod parser), plus libsystemd, libsndfile, alsa-lib,
json-c, OpenSSL's libcrypto, and optionally lilv (for LV2). You also need
CMake ≥ 3.20 and a C11/C++20 compiler. The CLAP headers are vendored (MIT).

```sh
# Arch
sudo pacman -S --needed base-devel cmake ninja pipewire libsndfile alsa-lib json-c lilv openssl
# Debian 13 / Ubuntu 25.04+
sudo apt install build-essential cmake ninja-build pkg-config libpipewire-0.3-dev libsystemd-dev \
  libsndfile1-dev libasound2-dev libjson-c-dev liblilv-dev libssl-dev

cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build        # unit tests: no PipeWire daemon or session bus needed
```

## Try it

Run the services from the build tree. Nothing is installed, and nothing
touches your default devices.

```sh
build/backline-hostd &                                   # the supervisor, on the session bus
build/backline host load "Surge XT" --id synth           # plays to your default output (DECISIONS D2)
build/backline params synth volume                       # find parameters
build/backline set synth "Global Volume" 0.5
build/backline note synth on 60 100; sleep 1; build/backline note synth off 60
build/backline --json host show synth                    # pid, node, OSC port, scope, state file
build/backline host watch                                # Loaded / Crashed / Restarted / Failed
build/backline host unload synth

build/registry/backline-registry scan                    # index ~/.clap ~/.vst3 ~/.lv2 and the system dirs
build/backline plugins list --kind instrument --grade pass

build/backline rec -s <sink-or-node> -o takes -d 10      # or -P 5 for 5 s of pre-roll, then: kill -USR1
```

To play from a hardware keyboard, add
`--midi-follow 'Midi-Bridge:.*(capture)'` to `host load`. The worker links
every matching MIDI source, now and whenever one is plugged in.

## Proofs

Each component has a script in `tests/` that builds a throwaway test graph:
null sinks owned by the script, nothing linked to real devices. The script
measures what the component claims, then checks that the default sink and
every other node are unchanged. The reports from the development machine are
in [`docs/proofs/`](docs/proofs/).

| Proof | What is measured | Result (2026-11-11) |
|---|---|---|
| [`host-proof`](docs/proofs/host-proof.txt) | Surge XT played by a virtual keyboard through PipeWire's MIDI bridge; a live parameter over OSC/D-Bus; SIGKILL one of two instances | Pitch within 0.12 cent by FFT. The parameter took effect in ≤ 27 ms. The survivor was unchanged and gap-free across the kill |
| [`hostd-proof`](docs/proofs/hostd-proof.txt) | hostd + CLI: three parameters moved, SIGKILL while a keyboard plays, crash loop, registry gate | All 775 parameter values identical after the restart. Restarted ≈ 80 ms after the kill. Level and pitch identical (±0.00 dB, ±0.00 cent), and every later note sounds. Backoff 0/250/500/1000/2000 ms, then `Failed`. A `fail`-graded plugin was refused, and when forced it left no processes |
| [`registry-proof`](docs/proofs/registry-proof.txt) | Full scan of `~/.clap`, `~/.vst3` and `~/.lv2`, plus crash and hang test plugins | 645 plugins in 13.4 s cold (10 s of that is the hang plugin's timeout) and 1.6 s warm. The crash (SIGSEGV) and the hang were graded fail, and their helper processes were killed |
| [`recorder-proof`](docs/proofs/recorder-proof.txt) | One signal into two sinks, one of them the graph driver, at quantum 2048 and 256; pre-roll | Bit-identical tracks at lag 0, where raw graph alignment is off by exactly one quantum. The pre-roll take is identical to the matching stretch of a reference take |

```sh
tests/host-proof; tests/hostd-proof; tests/registry-proof; tests/recorder-proof
```

The proofs need a running PipeWire (≥ 1.4) with WirePlumber, a systemd user
session, `pw-cli`, `pw-play`, `pw-record`, `jq`, `busctl`, `python3` and
`metaflac`. `host-proof` and `hostd-proof` also need Surge XT (CLAP) in
`~/.clap` or `/usr/lib/clap`.

## Limits (as of now)

- **CLAP only in the host.** VST3 and LV2 instruments come next. The registry
  indexes all three, but grades VST3 on loading only, since running VST3
  needs the SDK's hosting classes.
- **No plugin GUIs, no presets index, no transport** (`clap_process.transport`
  is null). The clock service is still design.
- **State is whatever the plugin saves.** hostd restores the CLAP state blob;
  a plugin that leaves something out of its state loses it on a restart. A
  note held at the moment of a crash is cut, and the next note-on sounds.
- **Recorder alignment** follows PipeWire's scheduling, as measured: a sink
  that drives the graph is one cycle late on its monitor. It holds through
  quantum and driver changes, but the cycle where the driver changes can
  glitch on that track. PipeWire's reported port latencies are logged, not
  applied, and hardware capture latency isn't compensated yet.
- **Realtime scheduling was off** in the proofs (no rtkit on the test machine).
  They pass at quantum 128 anyway, but this is not a latency benchmark.
- **Probes run with your real `$HOME`.** A plugin can write its own settings
  while it is scanned; the registry flags files written to the working
  directory, not to `~/.config`.

## Licence

MIT; see [LICENSE](LICENSE). The vendored `third_party/clap` is free-audio/clap
(MIT).

Everything Backline exposes stays MIT: the C library, the CLI and the D-Bus
API. GPL components that Backline will use (Ableton Link in the clock
service, Carla's bridges for odd plugin formats) run in their own processes
and are reached over IPC only. **That boundary is a design choice to keep the
licensing simple, not legal advice**; see
[docs/DECISIONS.md](docs/DECISIONS.md) (D3).

## How this was made

Backline is developed with AI assistance (Anthropic's Claude, working in
Claude Code) under human direction. The code, the tests and the proofs in
this repository were written that way. The proofs exist so that claims are
measured, not just asserted. Bug reports with a reproducer are very welcome.
