# `io.github.manuaudio.Backline.Surface1`

The surface service, `backline-surfaced`: hardware control surfaces (Mackie
Control and its family, with HUI and others to follow) for any program. A
program pushes **views**, ordered lists of channel **slots** whose controls
are bound to [Params1](Params1.md) paths, and surfaced puts them on whatever
is plugged in: it banks, flips, follows touch, drives the motors, the
scribble strips, the LED rings and the meters, and tells the program what the
engineer did.

**The rule of the boundary.** surfaced knows devices, views, slots, paths and
pages. It does not know what an input, an aux, a DCA or a role *is*. The
program knows its session and never sees a MIDI byte. Spill, "mix on faders"
and follow-selection are a program's policy, built from the primitives here
(`PushView`, `SetPotMode`, `SetSelected`, `SetFlip`) in answer to `Action`s.

- Bus name `io.github.manuaudio.Backline.Surface`
- Object `/io/github/manuaudio/Backline/Surface`
- Status: **0.x, additive-only from here** (see [README](README.md)).

## Words

| Word | Meaning |
|---|---|
| **Unit** | One physical device on one MIDI port pair, or one RTP-MIDI session on the network: an X-Touch, an X-Touch Extender, an MCU Pro. It has strips (8 for these), and maybe a master fader and the global buttons (a **master** unit; an **extender** has strips only) |
| **Profile** | What a model is: its driver, strips, displays, quirks, button labels. Data, in `*.surface.json` (below) |
| **Group** | An ordered list of units used as one wide surface: `[X-Touch Ext, X-Touch]` is 16 strips. Each group has its own views, window, pot mode and role |
| **View** | An ordered list of slots with an id, a label and a 2-character code for the assignment display |
| **Slot** | One strip's worth of bindings: fader, pots, buttons, name, colour, meter. `null` is an empty slot (fader down, display blank, LEDs off, controls inert) |
| **Layer stack** | The views a group has open, the top one shown. `PushView` (a spill) and `PopView` (back) return to exactly the view and window that were there |
| **Window** | The visible slice of the view: `[offset, offset + width)`, width = the strips of the group's units that are plugged in, dealt left to right. The offset is kept per view |
| **Pot mode** | What the pots do: **across** (each slot's pot does the same function for its slot: pan, a send) or **strip** (all pots work on the selected slot, through pages of up to 8 parameters) |
| **Flip** | Faders and the pots' current function swap (in a strip mode, the page goes onto the faders) |

## Methods

### `Units() → (aa{sv} units)`

Every unit surfaced knows, plugged in or remembered: `id` (`s`, stable:
`PROFILE@SERIAL`, or `PROFILE@PORT` when the device gives no serial),
`profile` (`s`), `name` (`s`, the model), `port` (`s`), `serial` (`s`),
`group` (`s`), `position` (`i`, in the group; -1 in none), `state` (`s`: `handshake`,
`online`, `gone`), `strips` (`u`), `master` (`b`), `touched` (`u`, faders held
now), `transport` (`s`: `midi`, a MIDI port, USB or DIN; `network`, RTP-MIDI),
`address` (`s`, a network unit's `IP:PORT` while it is connected). A network
unit's `port` is `rtpmidi:NAME | NAME`, NAME being its session or mDNS name.

### `Peers() → (aa{sv} peers)`

The network devices surfaced knows, driven or not: `name` (`s`), `address`
(`s`), `state` (`s`: `inviting`, `open`, `refused`, which means the device
said no because its sessions are taken, `lost` or `closed`), `found` (`s`:
`mdns`, `invited` when the device called us, or `address` when it was added),
`advertised` (`b`, still on mDNS), `latency-ms` (`d`, one way, from the last
clock sync), `lost` (`u`, how many times the session dropped), `unit` (`s`,
its unit id while driven, `""` otherwise).

### `AddNetwork(s address)`, `RemoveNetwork(s address)` → `()`

A device that isn't found by mDNS and doesn't call us (an X-Touch in RTP
SLAVE mode): an IPv4 address, `IP` or `IP:PORT` (default 5004), as on the
unit's display; names are not looked up. surfaced invites it, again
every second until it answers, and keeps the address in its state file.
Errors: `InvalidArgs` (not an IPv4 address), `NotFound` (`RemoveNetwork` of an
address never added).

### `SetActive(b active) → ()`

`false`: this engine is a **standby**. Its network sessions stay open and
nothing is driven: no handshake, nothing drawn, input ignored. `true`: every
connected device becomes a unit at once (handshake, then drawn). Going to
standby ends any held fader's gesture. `backline-surfaced --standby`
starts as a standby.

### `Groups() → (aa{sv} groups)`

`id`, `units` (`as`, in order, gone ones included), `role` (`s`), `view` (`s`),
`stack` (`as`, bottom first), `offset` (`i`), `width` (`u`), `pot-mode` (`s`),
`page` (`u`), `flip` (`b`), `selected` (`s`, a slot id), `locked` (`b`).

### `SetGroup(s group, as units, a{sv} options) → ()`

Creates or changes a group: its units left to right (moved out of any other
group; ids of units that aren't plugged in are kept for when they are).
Options: `role` (`s`: what `Params1` sets are sent as; default `all`),
`lock-view` (`s`: pin this group to one view, so it doesn't follow banking or
view changes from the program, e.g. an extender that always shows DCAs; `""`
unlocks), `lock-offset` (`i`). Errors: `InvalidArgs`.

### `RemoveGroup(s group) → ()`

Its units go back to automatic grouping.

### `SetViews(s group, s views_json) → ()`

The views and pot modes of a group (`*`: every group that has none of its
own, now and later):

```json
{"schema": "backline.views/1",
 "views": [
  {"id": "inputs", "label": "Inputs", "code": "In", "layer": 1,
   "slots": [
    {"id": "in.1",
     "name": "/console/in.1/name",
     "colour": "#e04040",
     "fader": "/console/in.1/fader",
     "pots": {"pan": "/console/in.1/pan", "send": "/console/in.1/send.a1"},
     "mute": {"path": "/console/in.1/mute", "flash": "/console/in.1/implicit-mute"},
     "solo": "/console/in.1/solo",
     "select": {"action": "select"},
     "rec": "/console/in.1/rec",
     "meter": "/console:in.1"},
    null]}],
 "pot-modes": [
  {"id": "pan", "label": "Pan", "binding": "pan", "ring": "dot", "push": "reset"},
  {"id": "send", "label": "Send", "binding": "send", "ring": "wrap"},
  {"id": "eq", "label": "EQ", "strip": true, "pages": [
    [{"path": "/console/{slot}/eq.hpf", "label": "HPF"}, {"path": "/console/{slot}/eq.lf.gain", "label": "LF G", "ring": "boost-cut"}]]}],
 "home": {"view": "inputs", "pot-mode": "pan"}}
```

- `name` and `colour` are a literal, or a path (it starts with `/`) to a
  `string` parameter. A colour is `#rrggbb`; devices with fewer colours get
  the nearest (the X-Touch's 7).
- `fader` and the `pots` values are paths. A pot mode's `binding` picks the
  slot's pot in **across** modes. A **strip** mode's pages apply to the
  selected slot: `{slot}` in a path is replaced with that slot's `id`.
- `mute`, `solo`, `select`, `rec` are a path, or an object: `path`, `mode`
  (`toggle`, the default, flips a `bool`; `momentary` is 1 while held; a
  `trigger` fires on press), `action` (instead of a path: the `Action` signal
  with `NAME:SLOT`), `led` (a path for the LED instead of `path`), `flash` (a
  `bool` path that makes the LED flash while true, and `on` doesn't apply).
- `meter` is `ROOT:ID`, a meter of the root's [meter table](Params1.md#metersstoken-h-memfd-as-ids---).
- `layer` (1-8) puts a view on the device's layer buttons (MCU: the Global
  View row). Several views on the same layer: repeated presses cycle them.
- A pot mode's `ring` is `dot`, `boost-cut`, `wrap` or `spread`; `push` is
  `reset` (to the parameter's default), `none`, or `action:NAME`; `line2` is
  `value` (the default) or `label`.
- `home`: the view and pot mode a group starts on.

The program sends the whole set again whenever its layout changes (a send
destination, a renamed bus, members of a spill); windows, stacks and the
selection are kept where the ids still exist.

### `PushView(s group, s view)`, `PopView(s group)`, `ShowView(s group, s view)` → `()`

The layer stack: push a view on top (a spill), pop back to the previous one
with its window, or replace the stack with one view (select a layer).
Errors: `NotFound`, `State` (`PopView` with one view open).

### `SetWindow(s group, i offset)`, `SetPotMode(s group, s mode, i page)`, `SetSelected(s group, s slot)`, `SetFlip(s group, b on)` → `()`

Navigation from the program's side: its UI moved the bank, picked a mode,
selected a channel (a strip mode's pots follow it), or flipped.

### `SetGlobalBinding(s group, s control, s binding) → ()`

What a global button does: `control` is a control name (below), optionally
with modifiers (`shift+f1`, `option+mute-1` isn't one: strips are bound by
views). `binding` is one of:

| Binding | Does |
|---|---|
| `action:NAME` | The `Action` signal with `NAME` (this is what every unbound button does, with the control's name) |
| `param:PATH` | Toggles a `bool`, fires a `trigger` (`momentary:PATH` holds it at 1 while pressed) |
| `bank-left`, `bank-right`, `channel-left`, `channel-right`, `bank-first`, `bank-last` | Move the window by the group's width or by one, or to an end |
| `flip`, `name-value`, `pop` | Flip; line 1 shows slot ids instead of names; pop the layer stack |
| `layer:N` | Show (or cycle) the views on layer N |
| `pot-mode:ID`, `page-next`, `page-prev` | Pot mode and strip page |
| `none` | Nothing |

or a JSON object with one of those as `do` and an `led`: a path, or `flip`,
`pushed` (flashes while a view is pushed), `name-value`. Displays bind the
same way: `timecode` and `assignment` take `param:PATH` of a `string`
parameter (`SetGlobalBinding(g, "timecode", "param:/clock/timecode")`).

### `Text(s group, s unit, s text, u ms) → ()`

A message across the displays for `ms` (0: until the next `Text`): `LOCKED`,
`taken: MON`. `unit` `""` is every unit of the group. Line 1 and 2 split at
a newline.

### `Profiles() → (s json)`

The profiles found, `{"schema": "backline.surface/1", "profiles": [...]}`.

## Signals

| Signal | When |
|---|---|
| `Navigated(s group, s view, i offset, s mode, i page)` | The surface changed view, window or pot mode itself (bank buttons, layer buttons), so the program's UI can follow |
| `Action(s group, s name, b down)` | A button bound to an action, or unbound: `f1`, `shift+f1`, `select:in.1` (a slot's button bound to `{"action": "select"}`); the jog wheel as `jog:+3` / `jog:-1`, the pedal as `pedal:64` |
| `UnitAdded(s unit)`, `UnitRemoved(s unit)` | Hot-plug |

## Properties

| Name | Type | Meaning |
|---|---|---|
| `UnitCount` | `u` | Units online |
| `Groups` | `as` | Group ids |
| `ParamsConnected` | `b` | controld's `Params1` is on the bus |
| `ProfileDirs` | `as` | Where profiles are read from |
| `MidiIn` | `t` | MIDI messages from devices (*polled*) |
| `MidiOut` | `t` | MIDI messages sent to devices (*polled*) |
| `Active` | `b` | `false` on a standby engine (`SetActive`) |
| `NetworkPort` | `q` | The RTP-MIDI control port surfaced listens on (5004, or the next free pair; 0 if none); data is the port after it |

## What surfaced does on its own

- **Network surfaces** (RTP-MIDI, [SURFACES.md](../SURFACES.md)): surfaced
  listens on UDP 5004/5005 and browses mDNS `_apple-midi._udp` through
  avahi-daemon. A device whose name matches a profile is driven: one found
  on mDNS or added by address is invited, and one that invites us is accepted
  if it is on one of this computer's own networks (nothing routed from
  further away).
  Others (a Mac's session) are ignored or refused. Its MIDI is batched into
  one packet per 5 ms tick. The side that invited syncs clocks every second,
  and surfaced probes when the device invited. When nothing is heard for 3 s,
  the session is lost, which is an **unplug** (below). surfaced invites again
  every second, after a refusal every 5 s.
- **Detection** (in order, first match wins): the port's
  `node:port | alias` against each profile's `match`, then the MCU device
  query (`F0 00 00 66 <model> 00 F7`), whose answer carries the unit's
  serial. Ports pair by node and alias, so a duplex device is one unit.
- **Handshake**: Mackie mode treats the device's host-connection query as
  ready; Logic Control mode answers its challenge. Then everything is zeroed
  and redrawn: faders, LEDs, rings, displays, meters.
- **Grouping**: a unit goes back to the group and position it had (by its
  id); a new master unit starts a new group; a new extender joins the first
  group with a master, on its right. Groups are remembered in
  `$XDG_STATE_HOME/backline/surfaces.json`, with the network addresses
  (`"network": ["10.0.0.5:5004"]`).
- **Unplug**: every fader of that unit that is touched gets its gesture
  ended at once (automation never writes forever), the unit becomes `gone`,
  and the window is dealt again over the units that remain. A replug puts it
  back where it was and redraws it.
- **Faders**: a move is only applied while the fader is touched (on
  touch-sense devices: no motor-overshoot echo ever moves a parameter), and
  the position is echoed straight back so the servo holds. Touch is a
  `Params1.Gesture`. A fader moved without a touch on a device whose profile
  says `"touch-sense": false` opens a gesture that ends 500 ms after the last
  move. A touch held over 60 s with no movement ends its gesture and is
  logged.
- **Pots**: ticks are applied to the normalised value (`step` per tick,
  default 1/100; `int`, `enum` and `bool` move by one value), ÷4 with
  Control held, ×4 with Shift. A turn opens a gesture that ends 300 ms after
  the last tick. Line 2 shows the value for 1 s.
- **Paced output**: one queue per port, bytes per second from the profile,
  in priority order (fader motors, then LEDs, rings, meters, text), always the
  newest value of each. Caps: faders 50 Hz, rings 30 Hz, a display line
  20 Hz, meters 15 Hz (the device decays them; they are sent again while
  there is signal).
- **The master fader** (a master unit's ninth) is bound by each view's
  optional `"master": {"fader": PATH, "name": …}`.
- **The select LED** follows `SetSelected` when the slot's `select` button
  has no path of its own.

## Profiles

`*.surface.json` files in `$XDG_CONFIG_HOME/backline/surfaces/` (yours,
first) and `/usr/share/backline/surfaces/` (shipped); `--profile-dir` adds
more. The format, `backline.surface/1`:

| Key | Meaning |
|---|---|
| `id`, `name` | `behringer-xtouch`, `Behringer X-Touch` |
| `driver` | `mcu` |
| `unit` | `master` or `extender` |
| `strips` | 8 |
| `master-fader` | `true` on a master unit with a ninth fader |
| `model` | The MCU model byte in the SysEx header: 20 (`0x14`, Mackie Control), 21 (`0x15`, extender), 16/17 (Logic Control/XT) |
| `match` | `{"port": [regex…], "exclude": [regex…]}`, POSIX extended, case-insensitive |
| `handshake` | `mackie`, `logic` or `none` |
| `display` | `{"lcd": "2x56", "colour": "xtouch" or "none", "assignment": true, "timecode": true}` |
| `meters` | `led` or `none` |
| `touch-sense`, `motor` | booleans |
| `bytes-per-second` | The output pace (USB devices ~ 12000; DIN 3125) |
| `quirks` | `no-flash` (send on instead of flash), `send-delay-us`, `assignment-swapped` |
| `labels` | Control name → the label printed on this model, for on-screen help |
| `bindings` | Control name → the default global binding |
| `verified`, `comment` | Whether a bench pass confirmed it; notes |

The shipped MCU-family profiles (`behringer-xtouch`, `behringer-xtouch-ext`,
`mackie-mcu-pro`, `mackie-mcu-xt`) bind the master unit's globals like this;
everything else is an `Action` with the control's name:

| Control | Binding |
|---|---|
| `bank-left`, `bank-right`, `channel-left`, `channel-right` | the same names |
| `shift+bank-left`, `shift+bank-right` | `bank-first`, `bank-last` |
| `flip`, `name-value` | the same names |
| The Global View row, `view-midi` … `view-user` (left to right) | `layer:1` … `layer:8` |
| `assign-track`, `assign-send`, `assign-pan`, `assign-plugin`, `assign-eq`, `assign-instrument` | `pot-mode:input`, `pot-mode:send`, `pot-mode:pan`, `pot-mode:inserts`, `pot-mode:eq`, `pot-mode:dynamics` |

A program that names its views' `layer`s and its pot modes to match gets the
device's buttons for free; any binding can be replaced with `SetGlobalBinding`.

**Control names** (MCU note numbers, A.3.3 of the console spec): `rec-1`…`rec-8`,
`solo-1`…, `mute-1`…, `select-1`…, `vpot-1`… (push), `fader-touch-1`…,
`fader-touch-master`, `assign-track`, `assign-send`, `assign-pan`,
`assign-plugin`, `assign-eq`, `assign-instrument`, `bank-left`, `bank-right`,
`channel-left`, `channel-right`, `flip`, `global-view`, `name-value`,
`smpte-beats`, `f1`…`f8`, `view-midi`, `view-inputs`, `view-audio`,
`view-instrument`, `view-aux`, `view-buses`, `view-outputs`, `view-user`,
`shift`, `option`, `control`, `alt`, `read`, `write`, `trim`, `touch`,
`latch`, `group`, `save`, `undo`, `cancel`, `enter`, `marker`, `nudge`,
`cycle`, `drop`, `replace`, `click`, `solo-clear`, `rewind`, `forward`,
`stop`, `play`, `record`, `up`, `down`, `left`, `right`, `zoom`, `scrub`,
`user-a`, `user-b`, `smpte-led`, `beats-led`, `rude-solo-led`, `relay`.
Pots are `vpot-N`, the jog wheel `jog`, the pedal `pedal`.

## Example

```
S=(io.github.manuaudio.Backline.Surface /io/github/manuaudio/Backline/Surface io.github.manuaudio.Backline.Surface1)
busctl --user call "${S[@]}" Units
busctl --user call "${S[@]}" SetGroup sasa{sv} foh 2 behringer-xtouch-ext@1 behringer-xtouch@2 0
busctl --user call "${S[@]}" SetViews ss foh "$(cat views.json)"
busctl --user call "${S[@]}" Text sssu foh "" "LOCKED" 2000
busctl --user call "${S[@]}" AddNetwork s 192.168.1.50
```

`backline surfaces` lists the units and network devices, and `backline
surfaces add|remove ADDRESS` and `activate|standby` call the methods above.
libbackline: `bl_surface_units`, `bl_surface_peers`,
`bl_surface_add_network`, `bl_surface_remove_network`,
`bl_surface_set_active`, `bl_surface_status`.
