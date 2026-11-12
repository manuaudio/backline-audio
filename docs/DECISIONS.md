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
