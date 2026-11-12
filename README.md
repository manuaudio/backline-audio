# Backline

System audio services for Linux, in the role macOS's CoreAudio system layer,
AudioComponent registry and AUv3 hosting play there. Every piece is a small
PipeWire-native component that any app, the desktop shell and scripts can
use. The design is in [ARCHITECTURE.md](ARCHITECTURE.md).

**Status: proof slices, not a product.** Two components exist, each measured:

| Binary | What it does |
|---|---|
| `backline-plugin-worker` | Runs one CLAP plugin (instrument or effect) as one PipeWire node, in its own process. It takes MIDI from any PipeWire MIDI source (`--midi-follow REGEX` links hotplugged keyboards), and exposes parameters, notes and state over D-Bus and OSC (localhost) |
| `backline-recorder` | Records N nodes in one take: sample-aligned tracks, WAV float / 24-bit / FLAC, BWF `bext` time reference, and a JSON sidecar with UTC, monotonic and graph timestamps |
| `backline-test-keyboard` | A scripted ALSA-sequencer keyboard for tests |

## Build

Needs PipeWire ≥ 1.4 headers, libsystemd, libsndfile, alsa-lib, CMake and
a C++20 compiler. The CLAP headers are vendored (MIT).

```sh
cmake -S . -B build -G Ninja && cmake --build build
```

## Try it (throwaway: makes its own null sinks, touches no real device)

```sh
tests/host-proof       # Surge XT played over MIDI, pitch by FFT, live params, crash isolation
tests/recorder-proof   # two nodes, one take, bit-identical alignment, BWF timestamps
```

The reports from 2026-11-11 are in `docs/proofs/`.

## Control a worker

```sh
backline-plugin-worker -p "Surge XT" -n backline.synth -t <sink node.name> -m 'Midi-Bridge:.*(capture)'
busctl --user call io.github.manuaudio.Backline.Instance.backline_synth \
  /io/github/manuaudio/Backline/Instance io.github.manuaudio.Backline.Instance1 SetParam sd "Global Volume" 0.5
tests/oscsend.py <osc_port> /backline/param/list
```

The worker prints its node name, OSC port, D-Bus name and pid as JSON on
start. The same facts are in its node properties (`backline.*` in `pw-dump`).

## License

MIT. See [LICENSE](LICENSE). `third_party/clap` is free-audio/clap (MIT).
