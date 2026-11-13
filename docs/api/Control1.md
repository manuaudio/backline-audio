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
| `midi:cc14:CH:MSB` | a 14-bit pair, CC `MSB` (0-31) with CC `MSB`+32: (msb·128 + lsb) / 16383 |
| `midi:nrpn:CH:PARAM`, `midi:rpn:CH:PARAM` | data entry (CC 6, and 38 for 14 bits) of parameter `PARAM` (0-16383) selected with CC 99/98 (NRPN) or 101/100 (RPN); increment and decrement (CC 96, 97) move it |
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
| `param:PATH` | a parameter a program published through [Params1](Params1.md): the 0..1 is its normalised value, and the program applies it | 0..1 |

The value after `invert` is `min + v·(max − min)`. **Triggers** act when the
value crosses above 0.5 (a button press, a note-on, a CC to 127), not on
release; an OSC message with no argument is a press every time.

`volume:` sets the same per-node gain that `wpctl set-volume` and Backline's
mixer set (the mixer shows dB = 20·log10 of it). For a hardware sink that
WirePlumber drives through a device route, the route volume is separate.

### 14-bit values, NRPN, relative encoders, takeover, feedback

- **14-bit pairs** (`cc14`, and NRPN/RPN data entry): a device that has never
  sent an LSB is treated as 7-bit, so its MSB alone moves the target at once.
  After the first LSB, an MSB waits up to 20 ms for its LSB.
- **NRPN/RPN**: the parameter selection is kept per channel; RPN 127/127
  (null) deselects. Option `bits` (`7` or `14`, default 14) says whether the
  data is the MSB alone or the pair.
- **Relative encoders**, option `encoding` (`midi:cc`, `nrpn`, `rpn` sources):

  | `encoding` | A tick up | A tick down | Used by |
  |---|---|---|---|
  | `abs` (default) | | | faders, absolute knobs |
  | `rel-2c` | 1 | 127 (two's complement) | many "relative 1" modes |
  | `rel-sm` | 1 | 65 (bit 6 is the sign) | Mackie V-Pots |
  | `rel-offset` | 65 | 63 (64 ± n) | "binary offset" encoders |
  | `rel-inc-dec` | CC 96 | CC 97 | NRPN data increment/decrement only |

  Each tick moves the normalised value by `step` (default 1/127; for an
  `int`, `enum` or `bool` parameter one value); `accel` (1-4, default 1)
  makes fast turns go further (|ticks|^accel). The vendors' own names for
  these modes differ; check the device's manual for which it sends.
- **Soft takeover**, option `takeover`, for controls without a motor:
  `jump` (default) follows at once; `pickup` ignores the control until it
  reaches or crosses the target's current value; `scale` moves the target in
  proportion to the travel the control has left until both meet. Once picked
  up, the control follows until the target is changed from elsewhere. The
  target's current value comes from Params1 (`param:`), the node (`volume:`,
  `mute:`), or the last value the mapping sent.
- **Feedback**, option `feedback: true` (MIDI sources): the target's value is
  sent back on the same CC, CC pair, NRPN, note or pitch bend (channel `*`
  sends on 1), through controld's MIDI output (`SetMidiOut`). That drives motor
  faders and LED rings on generic controllers. `param:` targets feed back what
  the program confirms (also when its own UI moves it); `volume:` and `mute:`
  what the node reports; other targets echo what was sent. A relative source
  gets the absolute value, which is what LED-ring controllers take.
- **Gestures**, option `gesture: "auto"` (`param:` targets): a gesture begins
  before the first value of a burst and ends 300 ms after the last, so touch
  automation works from a fader without a touch sensor. Option `role` (`s`,
  default `all`) is the Params1 role the sets are made in.

**Delivery.** Values go out as they arrive. D-Bus targets are called
asynchronously; while one call of a mapping is in flight, newer values of
that mapping replace each other and only the latest is sent when the call
returns, so a fast fader never queues up a backlog. Volume, mute and control
targets go straight to PipeWire.

## MIDI profiles

A **profile** describes one MIDI controller: its physical controls under
abstract names (`fader.1`, `knob.a.1`, `button.mute.1`, `transport.play`),
each with the message it sends in a named factory mode, and the **bindings**
the user learned for them (a control to a target, with the options of a
mapping). Learning stays what it is (`Learn`); a profile is how what was
learned for one device is kept, comes back when the device is plugged in, and
can be shared.

- **Where they live.** The user's profiles are in
  `$XDG_CONFIG_HOME/backline/midi-profiles/` (`ProfileDirs[0]`; beside
  `ConfigDir`, or `--profile-dir`); the shipped ones in
  `$datadir/backline/midi-profiles/` (`ProfileDirs[1]`). A user profile hides
  a system profile of the same id, so saving bindings for a shipped device
  writes the user's copy and leaves the shipped file alone. One file per
  profile, `<id>.json`.
- **Active.** A profile is active while its device is in the graph: when a
  MIDI port whose `node:port | alias` matches its `match` appears, the first
  matching profile (the user's before the system's, then by id) with `auto`
  on becomes active. That links the device to controld's MIDI input and
  output (as `SetMidiFollow`/`SetMidiOut` would, without changing them) and
  makes each binding a mapping, id `<profile>.<control>` (`.2`, `.3` for a
  control's further bindings), origin `profile:<id>`. When the device's last
  port goes, the profile and its mappings go too (`ProfileUnloaded`,
  `device-gone`). `LoadProfile` activates one by hand, device or not; it
  stays until `UnloadProfile`.
- **Mappings() and controls.** A mapping whose source is a control of an
  active profile lists `"profile"` and `"control"` with it, so a UI can say
  "fader 1" rather than `midi:cc:1:0`. Both are output-only and ignored on
  input.
- **Saving.** `SaveProfile(id)` binds the current MIDI mappings that are
  this device's controls (made at run time, from `control.json`, or the
  profile's own) and writes the user's file. Those mappings then belong to
  the profile: they are no longer written to `control.json` by `Save`, and go
  when the profile is unloaded. For a device no profile describes, give
  `match` (and `name`): the controls are made from the mappings' sources
  (`cc.1.7`, `note.10.36`).

### The file (`backline.midi-profile/1`)

```json
{"schema": "backline.midi-profile/1",
 "id": "korg-nanokontrol2",
 "name": "Korg nanoKONTROL2 (CC mode)",
 "vendor": "Korg", "model": "nanoKONTROL2",
 "mode": "factory scene, Control Mode CC, global MIDI channel 1, LED mode internal",
 "match": "nanoKONTROL2",
 "verified": false,
 "note": "built from the published chart, not tried on hardware. ...",
 "sources": ["KORG nanoKONTROL2 Parameter Guide and MIDI Implementation (korg.com)", "..."],
 "controls": [{"name": "fader.1", "source": "midi:cc:1:0", "kind": "fader"},
              {"name": "button.mute.1", "source": "midi:cc:1:48", "kind": "button", "label": "M"}],
 "bindings": [{"control": "fader.1", "target": "param:/console/in.1/fader", "gesture": "auto"}]}
```

| Key | Meaning |
|---|---|
| `id` | `[a-z0-9][a-z0-9._-]*`, at most 63; the file's name without `.json` |
| `name`, `vendor`, `model`, `mode` | for people; `mode` names the device setting the controls are for (a template, a scene, a preset) |
| `match` | POSIX extended regex, case-insensitive, on the device's MIDI ports (`node:port \| alias`) |
| `verified` | required: `true` only once the profile has been tried on the hardware |
| `note`, `sources` | how it was built, what the device needs (an editor setting for LEDs), the documents it comes from |
| `auto` | load by itself when the device appears (default `true`) |
| `controls[]` | `name` (as `id`), `source` (a Control1 MIDI source), `kind` (`fader`, `knob`, `encoder`, `button`, `pad`, `key`, `wheel`, `other`), `label` (what is printed on it), and what every binding to it gets unless it says otherwise: `encoding` (a relative encoder), `bits` (NRPN/RPN), `feedback` (the control lights or moves) |
| `bindings[]` | `control`, `target` and any option of a mapping (`min`, `max`, `invert`, `args`, `encoding`, `takeover`, `step`, `accel`, `bits`, `feedback`, `gesture`, `role`); never a `source` |

A profile is refused (and skipped, with the reason in the log) when it
doesn't validate: an unknown key, a missing `verified`, a bad regex, two
controls with one name, two controls a device can't tell apart (the same
message, a channel `*` over a channel, a `cc14` pair over a plain CC, a CC
that NRPN data entry uses), a binding to a control that isn't there, or a
target or option that doesn't parse.

### Shipped profiles

All three are built from the vendors' published MIDI charts (and the same
factory layouts in Ardour's MIDI maps), and **none is verified on hardware
yet**; each says so (`"verified": false`). A bench pass flips it.

| id | Device, mode | Controls |
|---|---|---|
| `korg-nanokontrol2` | Korg nanoKONTROL2, factory scene in CC mode, channel 1 | 8 faders (CC 0-7), 8 knobs (16-23), S/M/R ×8 (32-39, 48-55, 64-71), transport, track and marker buttons (41-46, 58-62) |
| `akai-midimix` | Akai Professional MIDImix, factory preset, channel 1 | 3 × 8 knobs, 8 faders and master (CC), mute, solo (SOLO + MUTE) and rec arm ×8 and bank left/right (notes 1-26); mute and rec arm LEDs as feedback |
| `novation-launch-control-xl` | Novation Launch Control XL (mk1, mk2), factory template 1, channel 9 | 3 × 8 knobs (CC 13-20, 29-36, 49-56), 8 faders (77-84), track focus and control ×8 (notes 41-44, 57-60, 73-76, 89-92), device/mute/solo/record arm (105-108), the four arrows (CC 104-107) |

### Methods

- `Profiles() → (s json)`: `{"schema": "backline.midi-profile/1", "profiles": [{id, name, vendor, model, mode,
  match, verified, auto, system, path, controls, bindings, active, present, device}], "dirs": [user, system]}`.
  The directories are read again on every call, so a hand-edited file shows at once. `present`: a matching port
  is in the graph now; `device`: that port.
- `Profile(s profile) → (s json)`: the file, plus `system`, `path`, `active`, `present`, `device`. Error `NotFound`.
- `SaveProfile(s profile, a{sv} options) → (s path)`: options `name` (`s`), `match` (`s`, required for a new
  profile, refused for an existing one), `auto` (`b`), `mappings` (`as`: exactly these mapping ids, with controls
  made for sources the profile doesn't have; default: every MIDI mapping that is one of its controls). The
  profile becomes active. Errors `InvalidArgs`, `NotFound` (a mapping id), `State` (`--no-config`).
- `LoadProfile(s profile) → (as ids)`: activates it (again, if it was: an edited file takes effect); returns the
  mappings made. All or none: a binding that fails leaves nothing. Errors `NotFound`, `InvalidArgs`.
- `UnloadProfile(s profile) → ()`: removes its mappings and its links. Error `NotFound` (not active).
- `DeleteProfile(s profile) → ()`: deletes the user's file (and unloads it). A system profile can't be deleted
  (`State`); the user's copy of one can, and the shipped one is used again.

Properties `ActiveProfiles` (`as`) and `ProfileDirs` (`as`, the user's then the system's). Signals
`ProfileLoaded(s profile, s device, u mappings)` and `ProfileUnloaded(s profile, s reason)` (`unloaded`,
`device-gone`, `reloaded`, `deleted`).

## MIDI input and output

`backline-controld --no-graph` never connects to PipeWire: it serves Control1
listeners and Params1 only (no MIDI, no `volume:` or `instance:` targets;
`SetMidiFollow` and `SetMidiOut` are refused). It is for tests and for hosts
without an audio server; the Params1 load test runs it that way.

controld is one PipeWire node, `backline.control` (`Midi/Sink`), with one
MIDI input port `midi_in`. It is never autoconnected. MIDI reaches it through
links: `SetMidiFollow(regex)` links every MIDI output port whose
`node:port | alias` matches, now and when it is plugged in later (a USB
keyboard through PipeWire's `Midi-Bridge`), or anyone can link a port to it.
The port's process callback only copies MIDI 1.0 messages (or UMP, converted)
into a wait-free ring and wakes the main loop.

The same node has a MIDI output port, `midi_out`, for feedback. It is linked
by `SetMidiOut(regex)` to every MIDI *input* port (a device's playback side)
that matches, now and when it is plugged in later. The main loop fills a
wait-free ring that the process callback drains into the port.

## Methods

### `Map(s source, s target, a{sv} options) → (s id)`

Adds a mapping. Options: `min` (`d`), `max` (`d`), `invert` (`b`), `in-min`
(`d`), `in-max` (`d`), `args` (`as`: the sources for `recorder:arm`, the name
for `recorder:marker`), `id` (`s`; default `m1`, `m2`, …), and the
[additions](#14-bit-values-nrpn-relative-encoders-takeover-feedback):
`encoding` (`s`), `step` (`d`), `accel` (`d`), `takeover` (`s`), `feedback`
(`b`), `bits` (`u`), `gesture` (`s`: `auto` or `none`), `role` (`s`). Errors:
`InvalidArgs` with the reason (a relative encoding on a note, takeover on a
relative encoder, feedback from OSC, a gesture on a target that isn't
`param:`).

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

The next MIDI message (note-on, CC, pitch bend, program change, or an NRPN/RPN
data entry, which is learned as `midi:nrpn`/`midi:rpn`) or OSC message
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
`max-size` (`u` bytes per packet, default 8192), `params` (`s`: serve the
[Params1](Params1.md#osc) tree to this listener's peers in this role: `all`,
`read-only`, or a role the program defined; without it the listener serves
mappings only). Every refusal is an `InvalidArgs` error with the reason (see
[Security](#security)).

### `Unlisten(s id) → ()`

Closes a listener and its connections. Error `NotFound`.

### `SetMidiFollow(s regex) → ()`

POSIX extended regex, case-insensitive; `""` follows nothing. Links controld
made to sources that no longer match are removed.

### `SetMidiOut(s regex) → ()`

Like `SetMidiFollow`, for controld's MIDI output: feedback goes to every MIDI
input port whose `node:port | alias` matches. `""` links to nothing.

### `Save() → (s path)`

Writes the mappings, listeners and MIDI follow made at run time (and those
loaded from it; not the mappings of a [MIDI profile](#midi-profiles)) to `control.json` in `ConfigDir`, atomically, mode 0600 (it
may hold a listener's secret). Error `State` when started with `--no-config`.

## Properties

| Name | Type | Meaning |
|---|---|---|
| `MappingCount` | `u` | Mappings |
| `Listeners` | `as` | Open listener ids |
| `Learning` | `s` | The target being learned, `""` when not learning |
| `ActiveProfiles` | `as` | The [MIDI profiles](#midi-profiles) active now |
| `ProfileDirs` | `as` | The user's profile directory, then the system's |
| `MidiFollow` | `s` | The follow regex |
| `MidiOut` | `s` | The MIDI output's regex |
| `Node` | `s` | controld's PipeWire node name |
| `ConfigDir` | `s` | Where `Save` writes and the start reads |
| `Received` | `t` | OSC messages accepted from listeners (*polled*) |
| `MidiEvents` | `t` | MIDI messages received (*polled*) |
| `MidiDropped` | `t` | MIDI messages the ring had no room for (*polled*) |
| `MidiSent` | `t` | MIDI messages sent for feedback (*polled*) |
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
| `ProfileLoaded(s profile, s device, u mappings)` | A [MIDI profile](#midi-profiles) became active (`device` is `""` for a load by hand with no device) |
| `ProfileUnloaded(s profile, s reason)` | It stopped being active: `unloaded`, `device-gone`, `reloaded`, `deleted` |

## Configuration

At start, controld reads every `*.json` file in
`$XDG_CONFIG_HOME/backline/control/` (`--config-dir`, or
`BACKLINE_CONTROL_DIR`) in name order. Each has the schema of `Mappings()`
(`"schema": "backline.control/1"`), with `secret` in a listener when it has
one, and `midi-out` next to `midi-follow`. A listener the [rules](#security) refuse is not opened and is logged.
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
   does nothing. The one exception is opt-in per listener: a listener given a
   `params` role serves the [Params1](Params1.md#osc) tree a program
   published, within that role, and the program still validates every set.
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
                               [--encoding E] [--step D] [--accel A] [--takeover T] [--feedback] [--bits 7|14]
                               [--gesture auto] [--role R]
backline map learn TARGET [--from midi|osc] [--timeout S]
backline map list | remove ID | cancel | save | status [--json]
backline map follow REGEX
backline map out REGEX
backline map listen udp|tcp ADDRESS PORT [--allow CIDR]... [--secret S | --secret-file F] [--insecure-udp] [--params ROLE]
backline map unlisten ID
backline map profile list | show ID | load ID | unload ID | delete ID
backline map profile save ID [--name N] [--match REGEX] [--no-auto] [--mapping MAPPING-ID]...
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
# a BCF2000-style box with motor faders, onto a program's parameters
backline map follow '^Midi-Bridge:BCF2000'
backline map out '^Midi-Bridge:BCF2000'
backline map add midi:cc:1:81 param:/console/in.1/fader --feedback --gesture auto
backline map add midi:cc:1:1 param:/console/in.1/pan --encoding rel-2c --feedback
backline map add midi:nrpn:1:291 param:/console/in.1/eq.lf --feedback
backline map add midi:cc:1:7 param:/console/main/fader --takeover pickup
# a nanoKONTROL2: learn two controls, keep them as the device's profile
backline map profile show korg-nanokontrol2
backline map learn param:/console/in.1/fader      # move fader 1
backline map learn param:/console/in.1/mute       # press M 1
backline map profile save korg-nanokontrol2       # ~/.config/backline/midi-profiles/korg-nanokontrol2.json
# from now on, plugging it in brings both back (ActiveProfiles, ProfileLoaded)
# a device nobody ships a profile for
backline map profile save my-box --name "My box" --match '^Midi-Bridge:My Box'
```
