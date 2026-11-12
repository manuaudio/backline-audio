# `io.github.manuaudio.Backline.Control1`

The control service, `backline-controld`: maps OSC and MIDI to any
parameter in the system. A fader on a MIDI controller can move a hosted
plugin's parameter, a sink's volume or the tempo; a button on a tablet's OSC
surface can start the transport or the recorder.

- Bus name `io.github.manuaudio.Backline.Control`
- Object `/io/github/manuaudio/Backline/Control`
- Status: **0.x, additive-only from here** (see [README](README.md)).

controld is the **only Backline service that can listen beyond localhost**,
and it **opens no network socket at all unless told to** (see
[Security](#security)). Mappings and listeners are created and removed only
over the session bus, which only the logged-in user can reach.

## Mappings

A mapping is a **source**, a **target** and an optional range.

| Source | Value 0..1 |
|---|---|
| `midi:cc:CH:NUM` | controller value / 127 |
| `midi:note:CH:KEY` | velocity / 127 on note-on, 0 on note-off |
| `midi:pitchbend:CH` | 14-bit value / 16383 |
| `midi:program:CH` | program / 127 |
| `osc:/ADDRESS` | the first argument, scaled from `in-min..in-max` (default 0..1); no argument (or `I`) is a press |

`CH` is 1-16, or `*` for any channel. OSC addresses are matched exactly (no
OSC patterns).

| Target | What it does | Output range (default) |
|---|---|---|
| `instance:NODE/PARAM` | `Instance1.SetParam(PARAM, value)` on the worker whose node is `NODE` (or a full `io.github.manuaudio.Backline.Instance.*` bus name). `PARAM` is any key the worker takes (id, name, `module/name`, unique substring). | the parameter's own range, read once with `ListParams` |
| `volume:NODE` | the node's channel volumes (`Props` `channelVolumes`), in dB | -60..0 dB; the bottom of the range is silence unless `min` is given |
| `mute:NODE` | the node's `Props` `mute`: on above 0.5, off below; an OSC press toggles | |
| `control:NODE/NAME` | a filter-chain or clap-pipewire control (`Props` `params`, `NAME` value) | 0..1 |
| `transport:tempo` | `Clock1.SetTempo` | 60..180 BPM |
| `transport:play`, `stop`, `toggle`, `locate:POS` | `Clock1.Play`, `Stop`, either (from `Rolling`), `Locate(POS)` (`POS` as `Locate` takes it: frames, `tc:…`, `bbt:…`) | trigger |
| `recorder:arm`, `record`, `stop`, `marker` | `Recorder1.Arm(args)`, `Record`, `Stop`, `AddMarker(args[0])` | trigger |

The value after `invert` is `min + v·(max − min)`. **Triggers** act when the
value crosses above 0.5 (a button press, a note-on, a CC to 127), not on
release; an OSC message with no argument is a press every time.

`volume:` sets the same per-node gain that `wpctl set-volume` and Backline's
mixer set (the mixer shows dB = 20·log10 of it). For a hardware sink that
WirePlumber drives through a device route, the route volume is separate.

**Delivery.** Values go out as they arrive. D-Bus targets are called
asynchronously; while one call of a mapping is in flight, newer values of
that mapping replace each other and only the latest is sent when the call
returns, so a fast fader never queues up a backlog. Volume, mute and control
targets go straight to PipeWire.

## MIDI input

controld is one PipeWire node, `backline.control` (`Midi/Sink`), with one
MIDI input port `midi_in`. It is never autoconnected. MIDI reaches it through
links: `SetMidiFollow(regex)` links every MIDI output port whose
`node:port | alias` matches, now and when it is plugged in later (a USB
keyboard through PipeWire's `Midi-Bridge`), or anyone can link a port to it.
The port's process callback only copies MIDI 1.0 messages (or UMP, converted)
into a wait-free ring and wakes the main loop.

## Methods

### `Map(s source, s target, a{sv} options) → (s id)`

Adds a mapping. Options: `min` (`d`), `max` (`d`), `invert` (`b`), `in-min`
(`d`), `in-max` (`d`), `args` (`as`: the sources for `recorder:arm`, the name
for `recorder:marker`), `id` (`s`; default `m1`, `m2`, …). Errors:
`InvalidArgs` with the reason.

### `AddMapping(s json) → (s id)`

The same from one JSON mapping object, as `Mappings()` lists them:
`{"source": "midi:cc:1:7", "target": "volume:my.sink", "min": -40}`.

### `RemoveMapping(s id) → ()`

Error `NotFound`.

### `Mappings() → (s json)`

```json
{"schema": "backline.control/1",
 "mappings": [{"id": "m1", "source": "midi:cc:1:7", "target": "instance:backline.synth/Global Volume", "sends": 12}],
 "listeners": [{"id": "udp:127.0.0.1:9000", "proto": "udp", "address": "127.0.0.1", "port": 9000, "allow": [],
                "auth": false, "loopback": true, "max-rate": 500, "max-size": 8192}],
 "midi-follow": "^Midi-Bridge:nanoKONTROL"}
```

Secrets are never returned (`auth` says whether one is set).

### `Learn(s target, a{sv} options) → (s id)`

The next MIDI message (note-on, CC, pitch bend, program change) or OSC message
from an accepted peer becomes the source of a new mapping to `target`; that
input itself is not passed on. Returns the id the mapping will have, and the
signal `Learned` says when it exists. Options: those of `Map`, plus `from`
(`s`: `midi`, `osc`, `any`; default `any`) and `timeout` (`u` seconds; default
30, 0 = none). Errors: `Busy` (already learning), `InvalidArgs`.

### `CancelLearn() → ()`

Error `State` when not learning.

### `Listen(s proto, s address, q port, a{sv} options) → (s id)`

Opens an OSC listener. `proto` is `udp` or `tcp`; `address` a literal IPv4 or
IPv6 address of this machine; `port` 0 picks a free one. Returns
`udp:ADDRESS:PORT` (IPv6 in brackets). Options: `allow` (`as`, addresses or
CIDRs), `secret` (`s`), `insecure-udp` (`b`), `max-rate` (`u` messages per
second per peer, default 500), `max-clients` (`u`, TCP, default 8),
`max-size` (`u` bytes per packet, default 8192). Every refusal is an
`InvalidArgs` error with the reason (see [Security](#security)).

### `Unlisten(s id) → ()`

Closes a listener and its connections. Error `NotFound`.

### `SetMidiFollow(s regex) → ()`

POSIX extended regex, case-insensitive; `""` follows nothing. Links controld
made to sources that no longer match are removed.

### `Save() → (s path)`

Writes the mappings, listeners and MIDI follow made at run time (and those
loaded from it) to `control.json` in `ConfigDir`, atomically, mode 0600 (it
may hold a listener's secret). Error `State` when started with `--no-config`.

## Properties

| Name | Type | Meaning |
|---|---|---|
| `MappingCount` | `u` | Mappings |
| `Listeners` | `as` | Open listener ids |
| `Learning` | `s` | The target being learned, `""` when not learning |
| `MidiFollow` | `s` | The follow regex |
| `Node` | `s` | controld's PipeWire node name |
| `ConfigDir` | `s` | Where `Save` writes and the start reads |
| `Received` | `t` | OSC messages accepted from listeners (*polled*) |
| `MidiEvents` | `t` | MIDI messages received (*polled*) |
| `MidiDropped` | `t` | MIDI messages the ring had no room for (*polled*) |
| `Rejected` | `t` | Packets and connections refused: not allowed, over the rate, unauthenticated, malformed, oversized (*polled*) |
| `AuthFailures` | `t` | Wrong or missing secrets and authentication timeouts (*polled*) |
| `Malformed` | `t` | Packets that are not valid OSC (*polled*) |
| `Dispatched` | `t` | Calls and settings sent to targets (*polled*) |
| `TargetErrors` | `t` | Targets that answered with an error or were not found (*polled*) |

## Signals

| Signal | When |
|---|---|
| `Learned(s id, s source, s target)` | A learn made its mapping |
| `LearnCancelled(s id, s reason)` | A learn timed out (`timeout`), was cancelled, or its input could not be mapped |

## Configuration

At start, controld reads every `*.json` file in
`$XDG_CONFIG_HOME/backline/control/` (`--config-dir`, or
`BACKLINE_CONTROL_DIR`) in name order. Each has the schema of `Mappings()`
(`"schema": "backline.control/1"`), with `secret` in a listener when it has
one. A listener the [rules](#security) refuse is not opened and is logged.
`Save` only ever writes `control.json`; other files are left as they are, so
a hand-written `stage.json` can sit next to it.

## Security

controld is the one place in Backline where something outside this computer
can change something inside it. The rules:

1. **Nothing listens by default.** With no configuration controld has a
   PipeWire node and a session-bus name, and no network socket, not even on
   localhost. A listener exists only because a config file or a `Listen` call
   names it.
2. **Bind explicitly.** The address is a literal IP of one interface. Host
   names are refused (a DNS answer must not decide what is exposed), and so
   are the wildcards `0.0.0.0`, `::` and `::ffff:0.0.0.0`, broadcast and
   multicast addresses. A surface on a tailnet uses the tailnet address; one
   on the stage network uses that interface's address.
3. **Loopback** (`127.0.0.0/8`, `::1`) listeners accept only local peers (or
   the allow-list, if one is given). Any local user can send to them, as to
   the worker's own OSC port.
4. **Anything else needs an allow-list**: the addresses or CIDRs that may
   send. Packets and connections from other peers are dropped and counted
   (`Rejected`).
5. **TCP beyond loopback needs a shared secret** of at least 16 characters.
   The first message on a connection must be `/backline/auth s <secret>`;
   controld answers `/backline/auth i 1`, or `/backline/auth i 0` and closes
   the connection. Any other first message, or none within 5 s, closes it too.
   The comparison takes the same time whatever the input. After 3 failures a
   peer address is refused for 30 s. Frames are OSC 1.0 TCP framing (a 32-bit
   big-endian length, then the packet).
6. **UDP beyond loopback needs `insecure-udp: true`.** UDP has no connection
   to authenticate and its source address can be forged by anyone on the
   path, so the allow-list is the only check. Use it on a network you trust
   (a dedicated stage LAN, a WireGuard/tailnet link), or use TCP with a secret.
7. **The network can only move what is already mapped.** No OSC message
   creates, changes or removes a mapping or a listener, starts a learn, or
   reaches any target that has no mapping. A message to an unmapped address
   does nothing.
8. **Limits.** A token bucket per peer address (`max-rate`, default 500
   messages a second, a burst of one second's worth), `max-size` per packet
   (default 8192 bytes; larger UDP packets and TCP frames are refused, and a
   TCP connection is closed), `max-clients` TCP connections per listener
   (default 8), bundles nested at most 4 deep, 16 arguments per message.
   Packets are checked whole before any message in them is used, so a
   malformed bundle never half-applies.
9. **Secrets at rest.** `Save` writes the file with mode 0600. `Mappings()`
   never returns a secret. The CLI takes `--secret-file`, so a secret need
   not appear in a process list.

What it does not do: OSC itself has no encryption, so a secret sent over TCP
on an open network can be read by anyone who can see the traffic. Over a
network you don't control, run the surface over WireGuard or a tailnet (and
listen on that interface's address), not over the open LAN.

## CLI

```
backline map add SOURCE TARGET [--min X] [--max Y] [--invert] [--in-min A --in-max B] [--arg S]... [--id ID]
backline map learn TARGET [--from midi|osc] [--timeout S]
backline map list | remove ID | cancel | save | status [--json]
backline map follow REGEX
backline map listen udp|tcp ADDRESS PORT [--allow CIDR]... [--secret S | --secret-file F] [--insecure-udp]
backline map unlisten ID
```

Examples:

```
backline map follow '^Midi-Bridge:nanoKONTROL'
backline map add midi:cc:1:7 'instance:backline.synth/Global Volume'
backline map learn volume:alsa_output.usb-Focusrite_Scarlett-00.analog-stereo
backline map listen udp 127.0.0.1 9000
backline map add osc:/1/fader1 transport:tempo --min 60 --max 180
backline map listen tcp 100.64.0.7 9001 --allow 100.64.0.0/10 --secret-file ~/.config/backline/control/secret
backline map save
```
