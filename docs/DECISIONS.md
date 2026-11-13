# Decisions

Short records of the choices that shape Backline. Newest last. Each says what
was decided, why, and what would make us revisit it.

## D1. One repository for now (2026-11-11)

**Decision.** Everything lives in one repository, `manuaudio/backline-audio`:
the services, the worker, `libbackline`, the `backline` CLI, the tests and
the proofs.

**Why.** The D-Bus interfaces (`Host1`, `Instance1`, `Registry1`, and the
recorder's next) are still moving, and they move together. One repository
means one version, one CI run and one place to change an interface and all
of its callers in the same commit. PipeWire versions its own pieces the same
way.

**Revisit when** the D-Bus APIs settle (a 1.0 of the interfaces). Then the
registry's JSON schema and probe are the first candidates to split out, since
other hosts and distributions could use them without the rest.

## D2. Instruments appear as output streams (2026-11-11)

**Decision.** A hosted instrument is a `Stream/Output/Audio` node. By default
the session manager (WirePlumber) links it to the default output, like any
app that plays sound. An explicit target (`--audio-target`, or the `target`
option of `Host1.Load`) replaces the auto-link with links Backline makes
itself. There is no separate "instrument as input device" (`Audio/Source`)
mode yet.

**Why.** "Play a system instrument from a keyboard" should make sound with no
setup, and an output stream is what every mixer, volume control and
per-app routing UI already understands. The instrument's output is still
recordable: `backline-recorder` takes any node's output ports or a sink's
monitor, so a separate source node isn't needed to capture it.

**Revisit when** a real use case needs the instrument to show up in apps'
*input* device lists (for example a browser call or a DAW that only records
from sources). That would be a second, opt-in node class, not a change of
default.

## D3. Licensing: MIT surface, GPL behind process boundaries (2026-11-11)

**Decision.** Everything Backline exposes is MIT: the C client library
(`libbackline`), the `backline` CLI, the D-Bus interfaces and their
documentation, and all of the services in this repository. Components under
the GPL that Backline uses or plans to use (Ableton Link in the clock
service, Carla's plugin bridges in the host) run in their own processes and
are reached over IPC only (D-Bus, OSC or pipes). No MIT component links GPL
code.

**Why.** Any application, open or proprietary, can then link `libbackline`
and drive every service, while the GPL components keep their licence intact
in their own processes. LGPL libraries (libsndfile, liblo, libltc) are linked
dynamically.

**Note.** This boundary is a design choice made to keep licensing simple. It
is not legal advice, and whether a process boundary is sufficient in a
particular product is a question for that product's own legal review.

## D4. The clock is a JACK client of pipewire-jack (2026-11-11)

**Decision.** `backline-clockd` owns PipeWire's transport and musical
position by being the JACK timebase master through pipewire-jack, and moves
the transport with `jack_transport_*`. Its LTC and MTC ports are JACK ports.

**Why.** The transport and the bar live in the driver's `spa_io_position`
(`state`, `segments[0]`, `segments[0].bar`). A client takes the segment and
moves the transport through its node *activation* (`segment_owner`,
`reposition`, `command`), and the one public implementation of that is
pipewire-jack. `pw_filter` and `pw_stream` don't expose it. Going through
pipewire-jack means no private protocol and no second clock: native PipeWire
nodes read the result from `spa_io_position` (§4.8 measures this with a
native node), JACK clients read it as the JACK transport, and pipewire-jack
is MIT and part of every PipeWire install that runs JACK apps.

**Measured consequences.** What the timebase callback writes is published
with the next cycle, and a locate lands two cycles after it is asked for;
clockd writes the bar for the next cycle and aims chase locates two cycles
ahead (§4.8). The bar's beat has tick resolution (1/1920 beat).

**Revisit when** PipeWire offers a native API for the segment owner (a
`pw_filter` flag, or a metadata-based transport); clockd's D-Bus interface
would not change.

## D5. PipeWire's latency directions (2026-11-11, a correction)

**Decision.** `SPA_PARAM_Latency` in the OUTPUT direction is capture latency
(from the sources up to the port) and the INPUT direction is playback latency
(from the port to the sinks). A device reports its own delay in the
direction of its ports: an ALSA capture port reports its converter delay in
OUTPUT. This is the convention pipewire-jack maps to `JackCaptureLatency` and
`JackPlaybackLatency`.

**Why it is recorded.** The first recorder read the two the other way round
(its comment said INPUT was capture), so the "playback latency" it logged for
sinks was the capture latency arriving at them. Input-latency compensation
(§2.3) found it: a fake capture device's declared delay never showed up. The
recorder's alignment never used these numbers, so no measured alignment
changes; the sidecar field now reads the right direction.

## D6. Link tests run in their own network namespace (2026-11-11)

**Decision.** The clock proof runs inside `unshare -rn` (an unprivileged user
and network namespace with only loopback, multicast on).

**Why.** Ableton Link joins every session it can reach on the LAN. A test that
sets the tempo must not change the tempo of somebody's Live set on the same
network. In the namespace, the second Link peer is the only one there is. The
PipeWire and D-Bus sockets are filesystem sockets, so the test still talks to
the real (throwaway) graph.

## D7. One player node per file (2026-11-12)

**Decision.** `backline-playerd` makes one PipeWire node per loaded file,
not one node per "player" with a port group per track.

**Why.** Every clip then appears as its own stream in mixers, volume
controls and patchbays, and can be routed and recorded on its own, the way
D2 treats instruments. Alignment doesn't need a shared node: each clip reads
the same transport frame from the same `spa_io_position` in the same cycle,
so clips line up to the sample (§4.9 measures it). Loading or unloading one
clip never touches another's node.

**Revisit when** sessions need hundreds of clips at once (one node each has
a cost per cycle), or a clip needs to be mixed inside the player before it
reaches the graph.

## Open infrastructure for a proprietary-friendly console (2026-11-12)
Manu: "I just don't want any of this to be proprietary except the actual PROGRAM that is the mixer, the infrastructure below should be all free, open source, OMARCHY."
Backline is the open infrastructure layer. Control-surface drivers and profiles (MCU, HUI, FIT, generic MIDI learn) and the network DSP offload (server node, transport, clocking, failover) belong in Backline, MIT-licensed. Mixer programs, open or proprietary, sit on top and use only Backline's public APIs.

## D8. Params1 lives in controld; surfaces are their own daemon (2026-11-12)

**Decision.** The parameter tree a program publishes (`Params1`) is served by
`backline-controld`, on its bus name, next to `Control1`. Control surfaces
are a separate process, `backline-surfaced` (`Surface1`), a client of
`Params1` like any other.

**Why.** controld already owns everything that reaches parameters from the
room (OSC listeners and their security rules, MIDI mapping), so a tree there
is reachable from OSC, from generic MIDI mappings (`param:` targets) and from
surfaces with one set of rules, roles and rate limits. Surfaces parse input
from USB devices of every quality and pace output to slow ones; a buggy
device or driver must not stall OSC control or the program, so they get
their own process (§0.2, "one process per risk"). The program stays the
source of truth: neither process ever changes a published value itself.

**Revisit when** a surface needs a round trip to the program faster than the
bus allows (it doesn't today: a fader move reaches the program in about a
millisecond), or the surface code moves to a control head machine of its own
(then `Params1` needs an authenticated network transport; see the spec's
redundancy note).

## D9. Surface proofs run on a private PipeWire instance (2026-11-12)

**Decision.** `tests/surfaced-proof` starts its own PipeWire daemon (its own
socket, a dummy driver, no devices or session manager) and runs every node of
the proof there, instead of on the user's graph with throwaway nodes.

**Why.** A software control surface is a MIDI source, and other programs on
the machine may follow every MIDI source (an instrument host set to "any
keyboard" would play the surface's button presses as notes). On a private
instance nothing outside the proof can see its devices, and the proof can't
touch anything outside it. The user's graph is listed before and after to
show it is untouched. Real-hardware checks run on the real graph, by hand
(docs/HARDWARE-TEST.md).

## D10. Network surfaces: surfaced speaks RTP-MIDI itself (2026-11-12)

**Decision.** Mackie Control surfaces on the network (the X-Touch and
X-Touch Extender's Ethernet port) are driven by a small RTP-MIDI
implementation inside `backline-surfaced`. It has two parts:
`surface/rtpmidi.c`, the AppleMIDI session and the RFC 6295 payload, pure and
unit-tested; `surface/rtpnet.c`, the UDP sockets and mDNS through
avahi-daemon. PipeWire's RTP-MIDI module is not used. A network device is a
unit with the same profile as on USB.

**Why.** PipeWire's AppleMIDI module (`libpipewire-module-rtp-session`) is
the only alternative without extra software, and it doesn't fit:

- It isn't in Arch's main `pipewire` package. It ships in `pipewire-zeroconf`,
  so it is one more package and a root install.
- It has to be configured into the user's PipeWire for every session, which
  is setup a surface shouldn't need.
- It turns each session into an anonymous MIDI node, so surfaced would lose
  the device's name and the session's state. It couldn't end a held fader's
  gesture on a drop any faster than PipeWire notices one.

PipeWire's plain RTP sink and source (`module-rtp-sink`/`-source` with
`sess.media = "midi"`) send RTP-MIDI without the session handshake, and the
X-Touch needs the handshake. Speaking the protocol
in surfaced keeps it all in one process with no setup. surfaced invites,
accepts invitations, sees every session's state, notices a silent device in
3 s, invites again until it is back, and refuses a second session it doesn't
want. It is about 1,100 lines and needs no new dependency beyond the optional
avahi-client.

**Standby.** `--standby` (or `SetActive(false)`) holds network sessions open
and drives nothing. That gives a second engine an instant takeover of devices
that allow two sessions, and a takeover within 5 s of the main's session
ending for devices that allow one. Which kind the X-Touch is, is a bench item
(docs/SURFACES.md).

**Revisit when** PipeWire's session module ships in the base package and
exposes per-session state, or devices need the recovery journal for lossy
links (Wi-Fi). surfaced sends no journal today, as the X-Touch's wired link
doesn't lose packets in practice. The session code would then grow a journal
writer, or the module would take over.

## D11. Sample rate is agnostic to the user; SRC is shown, quietly (2026-11-12)

Manu, verbatim:

> "sample rate should be agnostic to the user, just letting you know in a subtle way as a user if you look for it that SRC is on, and that is a conversion process, but you should be able to play at whatever sample rate and digital audio should flow and flow cleanly"

**Decision.** Nobody has to choose a sample rate to play something. Three
things follow:

- **The rate follows the source wherever the hardware allows.** That is
  Omarchy's own PipeWire default once omacom/omarchy#17520 lands (allowed
  rates 44.1, 48, 88.2 and 96 kHz). Backline doesn't duplicate it. Pro audio's
  locked rate is the one deliberate exception, for recording, and it is a
  choice the user makes.
- **When conversion can't be avoided, it is transparent and silent.** That is
  a core default proposed for Omarchy (`resample.quality = 10`, measured in
  `manuaudio/omarchy-audio`: flat within 0.002 dB to 20 kHz, every artefact
  below −140 dBFS, about 0.1% of one core per stereo stream on an i7-12650H).
  There's no setting, toggle or warning for it.
- **SRC is visible only to someone who looks.** `omarchy-backline-src` lists
  every stream whose rate differs from its device's. The Audio Setup panel
  shows the same in muted caption text under the pro-audio header ("SRC 44.1
  → 48 kHz · Spotify") and on a combined device's row, only while it is
  happening. There's no colour, icon, notification or badge on the bar.

**How it is read.** From `pw-dump`: each stream's `Format` rate against the
`Format` rate of the hardware device at the end of its links, through
filters and combined devices by `node.link-group`. JACK clients run at the
graph rate and never show. The panel reads it on open, and again when
PipeWire's node list changes while it is open. Closed, it reads nothing.
Tests use hand-written `pw-dump` fixtures (`omarchy/test/fixtures/pw-dump`).

**Revisit when** Quickshell's PipeWire service exposes node formats (then the
panel can drop the `pw-dump` read), or when a stream converted twice (stream
and device both off the graph rate, at the same rate) turns out to matter:
the stream-against-device comparison does not show that case.

## D12. Slot mode: one process per strip chain, a futex pair, delayed dry on a miss (2026-11-12)

**Decision.** A mixing engine's same-cycle inserts run in
`backline-slot-host`, one process per strip chain. The engine owns the
cycle and the memory: a sealed memfd with a 4 KiB header, two event rings
and the audio, in place. It wakes the chain through a shared futex (`req`)
and waits on another (`done`) until a deadline it picks. If the chain is
late, the block plays the dry input delayed by the chain's reported latency,
and the chain is stalled: no new request goes out until the sandbox has
caught up or a new one has taken the memory over (`generation` changed). The
contract is one header-only C file, `lib/include/backline/slot1.h`, holding
both sides.

**Why.**

- **One process per chain, not per plugin.** One round trip per strip
  instead of one per insert, and a crash still costs one strip, never the
  mix. One process per plugin would multiply the wake-ups. One process for
  every strip would make a crash cost the mix.
- **Futexes on shared memory, not a socket or eventfd.** A wake and a wait
  are one syscall each. The engine can spin briefly first, and the wait takes
  an absolute deadline (`FUTEX_WAIT_BITSET`, `CLOCK_MONOTONIC`).
- **Stall instead of overlap.** After a miss the sandbox may still be
  reading the buffer, so the engine must not write it. Stalling until
  `done == req` keeps one buffer and no locks, at the cost of the stalled
  blocks also being dry.
- **Delayed dry, not silence.** The strip keeps playing, and stays aligned
  with every other path through the engine's PDC.
- **Begin and end are separate calls,** so an engine sends every chain's
  request before it waits on any. The first crash proof waited chain by
  chain, and one dead chain used up the next chain's deadline.
- **A takeover generation, not a reset handshake.** A restarted sandbox
  sets `done = req` and adds one to `generation`. The engine needs no
  message from the supervisor.

**Revisit when** the console's engine runs it live (verify when the hold
lifts). Two signs would call for a change: deadline misses at realtime
priority, or a need for a second buffer, so a slow chain could keep
running while the engine moves on.
