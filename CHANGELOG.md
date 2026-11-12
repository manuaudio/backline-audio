# Changelog

Backline's D-Bus interfaces are 0.x: within an interface (`Player1`,
`Clock1`, …) methods, properties and signals are only ever added. Measured
evidence for every item is in [docs/proofs/](docs/proofs/).

## Unreleased

Fixes from the first real install (Omarchy 4.0.4, PipeWire 1.6.8,
WirePlumber 0.5.17).

### Fixed

- **Instruments play to the default output again** (DECISIONS D2).
  WirePlumber rejected the node ("no usable format found"), so an instrument
  loaded without `--target` stayed silent. The node now shows its main
  output's format as an app stream does and answers WirePlumber's port
  configuration; it is linked like any app and follows a change of default
  output. Proof: `tests/autolink-proof` (a ctest, on a private PipeWire and
  WirePlumber).
- **"Follow every keyboard" follows controllers only**: never Backline's own
  clock (its MTC output was followed), Midi Through, or an app's MIDI output.
- **`omarchy-backline-setup`** backs each file up before changing it
  (`<file>.bak.backline-<date>-<time>`), changes nothing when run again, and
  `--uninstall` restores both files byte for byte (it left a blank line,
  one more per run). Each hotkey is checked against what is bound on the
  system, and a taken key isn't bound. The Instruments panel moves to
  `Super + Ctrl + U` (`Super + Ctrl + I` is Omarchy's Toggle locking on
  idle). Where the panels can't load (Omarchy 4.0.4's shell, or no bar
  plugin), no key or menu row points at one, and `Super + Ctrl + M` opens
  the Backline Mixer.
- **`backline-registry serve --idle 0`** stays up instead of exiting at once.
- **User units** install where systemd reads them for a `~/.local` prefix
  (`$XDG_DATA_HOME/systemd/user`), and the pacman hook only under `/usr`.
- **Mixer**: no overlapping text in a narrow tiled window.
- **`--help`** on every `omarchy-backline-*` command prints its usage
  (`instrument-remove --help` tried to remove an instrument called `--help`).
- **Registry**: a crash reason quotes the plugin's own error (Cardinal Synth
  aborts on a corrupt settings file and is graded fail, correctly).

### New

The surface layer below is tested against software devices only; a pass on
real hardware comes first ([docs/HARDWARE-TEST.md](docs/HARDWARE-TEST.md)).

- **`Params1`: a program publishes its parameter tree**
  ([Params1](docs/api/Params1.md)), served by controld. Typed parameters
  (ranges, units, linear/log/console-fader tapers, value text, automatable,
  read-only), confirmed values with `Update`, a sealed shared-memory meter
  table, roles; clients ask with `Set` and `Gesture` and the program decides.
  Over OSC (opt-in per listener, `params` role): get, set, `/norm`, `/touch`,
  rate-limited subscriptions with echo, meter streams, error replies.
- **Control service additions** ([Control1](docs/api/Control1.md)):
  `midi:cc14`, `midi:nrpn` and `midi:rpn` sources; relative encoders
  (`rel-2c`, `rel-sm`, `rel-offset`, `rel-inc-dec`, with step and
  acceleration); soft takeover (`pickup`, `scale`); `param:` targets with
  automatic gestures; a MIDI output port (`SetMidiOut`) with feedback to
  motor faders and LED rings. `backline map out`, `backline map listen
  --params`, new `backline map add` options; libbackline
  `bl_control_set_midi_out`, `bl_control_listen_params`.
- **Surface service, `backline-surfaced`** ([Surface1](docs/api/Surface1.md)).
  Mackie Control surfaces for any program: detection by port and handshake,
  profiles as data (`*.surface.json`: MCU Pro, MCU XT, X-Touch, X-Touch
  Extender), groups of units as one wide surface, views of slots bound to
  `Params1` paths, banking, the 8 view layers, flip, push/pop (spill), pot
  modes across and per channel strip, motor faders with touch and gestures,
  scribble strips with X-Touch colours, meters, paced output, hot-plug that
  ends a held fader's gesture and puts a replugged unit back in its place.
- **Test tools**: `backline-test-mcu-sim` (a software X-Touch/MCU on
  PipeWire, also a generic CC/NRPN controller) and `backline-test-mixer` (a
  stub program on the public APIs). Proof: `tests/surfaced-proof`, 82 checks
  on a private PipeWire instance (`docs/proofs/surfaced-proof.txt`); in CI
  the engine against two device models, 233 checks.
- **Network surfaces** ([SURFACES.md](docs/SURFACES.md)): an X-Touch or
  X-Touch Extender on Ethernet (MC mode, Interface Network) works exactly as
  on USB, with the same profiles, banking, touch safety and grouping, and no
  helper software. surfaced speaks RTP-MIDI itself (DECISIONS D10): it is
  found on mDNS (`_apple-midi._udp`, through avahi-daemon), invited by the
  device (RTP MASTER), or added once with `backline surfaces add ADDRESS`
  (RTP SLAVE, no firewall change). A drop ends a held fader's gesture within
  3 s, and the unit comes back to its place when the network does. A
  standby engine (`backline-surfaced --standby`, `SetActive`) holds its own
  sessions and takes over on `backline surfaces activate`. `Surface1`
  additions: `Peers`, `AddNetwork`, `RemoveNetwork`, `SetActive`, the
  `Active` and `NetworkPort` properties, and `transport`/`address` in
  `Units`. Proof: `tests/surfaced-net-proof`, 50 checks in a user network
  namespace with a dummy interface and a private avahi-daemon
  (`docs/proofs/surfaced-net-proof.txt`); in CI the protocol and session
  code, `tests/unit/test_rtpmidi.c`.
- **`backline surfaces`**: the connected surfaces, how each is connected
  (USB/MIDI or network and its address), its state, group and position, and
  the network devices that aren't driven (refused, not answering). It also
  takes `add`, `remove`, `activate` and `standby`. libbackline `bl_surface_*`.
- **surfaced starts at login**: a systemd user unit
  (`backline-surfaced.service`), installed enabled, so a surface works with
  nothing to set up. D-Bus activation goes through it. To turn it off:
  `systemctl --user mask backline-surfaced`.

### Packaging

- `backline-surfaced` links avahi-client when it is found (Arch `avahi`,
  Debian `libavahi-client-dev`). Without it, network surfaces still work by
  invitation or address, with no mDNS. The next PKGBUILD should list `avahi`.


## 0.4.0 (2026-11-12)

Mac-style pro audio for Omarchy Linux: files on the shared timeline,
control surfaces, tempo-synced plugins, and the Omarchy integration as one
installable plugin.

### New

- **Player service, `backline-playerd`** ([Player1](docs/api/Player1.md)).
  Plays audio files into the graph on clockd's timeline: one node per file,
  each starting on an exact transport frame (`frame`, `tc:` or `bbt:`), with
  its own gain, following play, stop and locate. The first seconds of each
  file are held in memory and the rest streams from a disk thread.
  `backline play load|unload|gain|start|list|show`, libbackline
  `bl_player_*`. Proof: every received sample bit-identical to the file at
  (transport − start) × gain, starts on their exact frame, at quantum 128
  and 1000 (`docs/proofs/player-proof.txt`).
- **Control service, `backline-controld`** ([Control1](docs/api/Control1.md)).
  MIDI (CC, notes, pitch bend, program change) and OSC mapped to plugin
  parameters, node volume and mute (the mixer's gain), filter-chain
  controls, the transport and the recorder; MIDI and OSC learn; mappings in
  `~/.config/backline/control/`. **Security:** no network socket by default;
  listeners need a literal IP (no wildcards or host names); beyond localhost
  an allow-list, TCP a shared secret (with lockout), UDP an explicit
  `insecure-udp`; the network can only move existing mappings. `backline map
  ...`, libbackline `bl_control_*` (`docs/proofs/controld-proof.txt`).
- **Clock transport in hosted plugins.** The worker passes clockd's
  transport to CLAP (`clap_event_transport`), VST3 (`ProcessContext`) and LV2
  (`time:Position`). Beat-click test plugins click on the exact transport
  sample in all three formats; Odin2's arpeggiator follows clockd's grid
  through a tempo change (`docs/proofs/transport-proof.txt`).
- **`Clock1.Resolve(position)`**: a position (frames, `tc:`, `bbt:`) to a
  frame with clockd's tempo map, without moving the transport.
- **One Omarchy plugin.** The repository is an Omarchy plugin
  (`io.github.manuaudio.backline`, `manifest.json` at the root): `omarchy
  plugin add https://github.com/manuaudio/backline-audio --enable` puts the
  Instruments, Record, Plugins and Transport panels in one bar slot (Tab
  still moves between them). The `omarchy-backline-*` commands install with
  the package; the new `omarchy-backline-setup` adds menu rows and hotkeys
  only when asked, and `--uninstall` takes them out again
  ([docs/OMARCHY-PLUGIN.md](docs/OMARCHY-PLUGIN.md)).
- **Arch package** ready for the AUR (`packaging/arch/PKGBUILD`, verified in
  a clean container with `packaging/arch/container-check`;
  [docs/AUR.md](docs/AUR.md)).

### Changed

- clockd's beat is exact to the frame: it sets the bar offset below
  PipeWire's 1/1920-beat tick. Its tempo map now keeps frame 0 at bar 1,
  beat 1 (a tempo change while stopped sets the song tempo; while rolling it
  takes effect from that point). Before, after a tempo change a locate to 0
  froze the beat for up to 0.75 s, longer with every take.
- The panels' hotkeys and menu rows open panels through their IPC targets
  (`omarchy-shell -q backline.record toggle`); the old per-panel plugins
  (`backline.record`, …) are replaced on install.
- `backline-registry` no longer compiles in the build tree's probe path.
- `tests/rf64-proof` runs in its own node group and driver at quantum 1024
  and logs the machine load. Re-run: a 4.18 GiB RF64 take with all
  70 080 000 frames continuous and identical to an independent reference,
  0 missed cycles (other agents' work ran at load ≈ 2; no realtime
  priority).

### Fixed

- The UI bench's leak watchdog could outlive its bench and poll the graph
  forever; it now ends with the bench.
- The proofs' pw-cli feeders no longer outlive a killed script, and the
  proofs' exit traps no longer stop half-way (exit status 2 after a pass).
- The default-device watch in the proofs no longer reports a failed
  `pw-metadata` read as a move.

### Known limits

- The player has no resampling (a file at another rate stays silent and
  says so), loops or cue lists; a locate into the streamed part of a file
  while rolling can leave one cycle of silence.
- A meter change while rolling doesn't renumber earlier bars (no full
  multi-meter map); `Clock1.Tempo` shows the last rolled tempo while stopped.
- The panels need Omarchy's Quattro shell (4.x with `ShellIpc`).
- No realtime priority on the test machine (no rtkit): the proofs are not
  latency benchmarks.

## 0.3.0 (2026-11-11)

Plugin host (CLAP, VST3, LV2) with presets and a crash-restarting
supervisor, the plugin registry with sandboxed probes and auto-rescan, the
recorder and `recorderd` (aligned multitrack, RF64, input-latency
compensation, virtual soundcheck), clockd (transport, LTC/MTC generate and
chase, Ableton Link), libbackline and the `backline` CLI, the Omarchy panels
and the mixer.
