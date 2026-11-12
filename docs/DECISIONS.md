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
