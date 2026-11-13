# `io.github.manuaudio.Backline.Params1`

A program publishes its **parameter tree** here, and everything else in the
room reaches it: OSC tablets and scripts through controld's listeners,
control surfaces through `backline-surfaced` ([Surface1](Surface1.md)),
generic MIDI controllers through Control1 mappings (`param:PATH` targets).

The program stays the **source of truth**. Nothing in Backline ever changes a
published value by itself: a client *asks* to set a value, the program
validates it, applies it its own way, and confirms with `Update`. Backline
only carries the request, the confirmation and the fan-out.

- Served by `backline-controld`, on its bus name
  `io.github.manuaudio.Backline.Control`
- Object `/io/github/manuaudio/Backline/Params`
- Status: **0.x, additive-only from here** (see [README](README.md)).

## The tree

A program claims a **root**, one path segment (`/console`, `/mix`, `/ardour`),
and publishes parameters under it. A parameter's **path** is the root, then up
to 7 more segments: `/console/in.1/fader`, `/console/aux.3/send.in.12/level`.

- A segment is 1-64 characters from `A-Z a-z 0-9 . _ ~ -`. A path is at most
  255 characters.
- The last segment may not be `norm`, `touch` or `text`: OSC uses those as
  suffixes (below).
- The root `/backline` is reserved.

| Field | Type | Meaning |
|---|---|---|
| `path` | string | Relative to the root (`in.1/fader`) or absolute under it (`/console/in.1/fader`) |
| `type` | string | `float`, `int`, `bool`, `enum`, `string` or `trigger` |
| `name` | string | Display name, up to 64 characters (`Kick In fader`) |
| `short` | string | Optional short label, up to 7 characters, for scribble strips (`Fader`) |
| `min`, `max` | number | Range of `float` and `int` (default 0..1). `enum`: 0..n-1, set from `values` |
| `default` | number/bool/string | Default value (what a surface's "reset" sets). Default: `min`, false, `""` |
| `unit` | string | `dB`, `Hz`, `ms`, `%`, `st`, … shown after the value, up to 8 characters |
| `taper` | string | `linear` (default), `log` (needs `min` > 0), `db-fader` (below). How 0..1 maps to the range |
| `format` | string | How a value becomes text: one `%f`, `%g`, `%d` or `%i` conversion with optional flags, width and precision, plus literal text (`"%+.1f"`, `"%.0f Hz"`). Anything else is refused. Default `%.1f` (`float`), `%d` (`int`) |
| `values` | string array | `enum` labels, in order; `bool`: optional `["off", "on"]` labels |
| `step` | number | The smallest step a relative control should move (`int`: default 1) |
| `flags` | string array | `automatable` (it takes gestures), `read-only` (clients may only read it) |
| `value` | | The initial value (default: `default`) |
| `text` | string | Optional initial display text (default: formatted from `value`) |

### Values, normalised values and text

Every parameter has a **value** in its own type and range, a **normalised
value** 0..1 for faders and pots, and a **text** for displays.

- `linear`: `norm = (v − min) / (max − min)`.
- `log`: `norm = ln(v / min) / ln(max / min)`.
- `db-fader`, a console fader law with the top of the range `T = max`:

  | dB | norm |
  |---|---|
  | `T` (e.g. +10) | 1 |
  | `T − 10` (0 dB) | 0.75 |
  | `T − 20` | 0.5 |
  | `T − 40` | 0.25 |
  | `T − 60` | 0.125 |
  | `min` | 0 |

  linear in dB between the points. A `min` at or below −120 dB is **−∞**: it
  displays `-inf`, and a fader at the bottom sends exactly `min`.
- `int` and `enum` round to the nearest whole value; `bool` is 0 or 1. A
  `trigger` has no value; setting it with any value fires it.
- **Text** is the program's if it sent one (`UpdateText`), otherwise
  `format` applied to the value, followed by a space and `unit`; `enum` shows
  its label; `bool` shows `values` or `on`/`off`.

## Methods for the program

The caller's unique bus name owns what it registers. When it leaves the bus
(exits or crashes), its roots are removed, `Structure` is emitted, and every
open gesture on them is ended.

### `Register(s root, s schema_json) → (s token)`

Claims `root` and publishes its tree:

```json
{"schema": "backline.params/1",
 "params": [
   {"path": "in.1/fader", "type": "float", "name": "Kick In", "short": "Kick",
    "min": -144, "max": 10, "default": 0, "unit": "dB", "taper": "db-fader",
    "format": "%.1f", "flags": ["automatable"], "value": -144},
   {"path": "in.1/mute", "type": "bool", "name": "Kick In mute"},
   {"path": "in.1/name", "type": "string", "name": "Kick In name", "value": "Kick In"}],
 "meters": ["in.1", "in.2"]}
```

`meters` names the program's meters (see `Meters`). The same client
registering its root again replaces the tree and keeps its token. At most 16
roots and 20 000 parameters in all, 1 024 meters a root. Errors: `Busy`
(another client owns the root), `InvalidArgs` with the first bad parameter
and why.

`features` (optional) lists what the program can take beyond the base
contract. Names controld doesn't know are ignored, so a newer program works
with an older controld:

- `"set-many"`: the program takes the **`SetMany` signal** (below). Requests
  are then batched: every set that controld handles before its loop is idle
  (one OSC bundle, one `SetMany` call, a burst of `Set` calls) reaches the
  program as one signal per run of requests from the same client in the same
  role. Without it the program gets one `Set` signal per value, as before.

A 5 000-parameter `Register` takes about 16 ms (see [Big trees](#big-trees)).

### `Add(s token, s params_json) → ()`, `Remove(s token, as paths) → ()`

Sessions are built live: `Add` takes `{"params": [...]}` (an existing path is
replaced), `Remove` takes paths, or `prefix/**` for a whole subtree. Both emit
`Structure`.

### `Update(s token, a(sv) values) → ()`

The program's confirmed values: a path and its value (`d` for `float`, `i` or
`d` for `int` and `enum`, `b` for `bool`, `s` for `string`). This is what
everyone sees: subscribers, surfaces, MIDI feedback. An `Update` clears any
text the program set for that path. Unknown paths are an `InvalidArgs` error
and nothing in the call is applied. One call takes up to 20 000 values (the
whole tree); send a big change as one `Update`, not one call per value.

### `UpdateText(s token, a(ss) texts) → ()`

The program's own display text for paths (a plugin's `value_to_text`, a
`L23` pan, `off` for a muted send). It stays until the next `Update` of the
path. Text is at most 64 characters.

### `Meters(s token, h memfd, as ids) → ()`

A shared-memory meter table the program writes and controld and surfaced
read; there is no bus traffic per meter value. `ids` gives the meter ids in
table order (as in `meters` above). The file:

| Offset | Type | |
|---|---|---|
| 0 | `char[4]` | `BLMT` |
| 4 | `u32` | version, 1 |
| 8 | `u32` | count (= the number of ids) |
| 12 | `u32` | stride, 8 |
| 16 | `u64` | sequence: odd while the program writes, even when done (a seqlock) |
| 24 | `u64` | `CLOCK_MONOTONIC` ns of the last write |
| 32 | 32 bytes | reserved, 0 |
| 64 + 8·i | `f32`, `u32` | meter i: level in dBFS (−∞ allowed), flags: bit 0 clip, bit 1 signal present |

The program increments the sequence, writes, and increments it again; a
reader that sees an odd number or a change across its read reads again. The
fd must be a memfd sealed against shrinking (`F_SEAL_SHRINK`, so a reader
can never fault) and at least `64 + 8·count` bytes; otherwise `InvalidArgs`.

### `Roles(s token, a{sas} roles) → ()`

Names the **roles** clients act in, each with the path patterns it may write
(`{"monitor": ["/console/aux.*/send.*/**", "/console/aux.*/mute"]}`). Two
roles always exist: `all` writes anything that isn't `read-only`, and
`read-only` writes nothing. A client in a role nobody defined can only read.

### `Unregister(s token) → ()`

Removes the root now.

## Methods for clients

Any session-bus client can call these; the session bus is the logged-in
user's own. Over the network, OSC (below) applies the listener's role.

### `List(s pattern) → (s json)`

Every parameter whose path matches, with its fields, `value`, `norm` and
`text`, and the roots with their owners and meter ids:
`{"schema": "backline.params/1", "roots": [{"root": "/console", "owner": ":1.42", "meters": [...]}], "params": [...]}`.

**Patterns**: `*` matches within one segment, `?` one character, `**` any
number of whole segments (`/console/in.*/fader`, `/console/**`).

### `Get(as paths) → (a(svds) values)`

Path, value, normalised value and text, in the order asked. Error `NotFound`
for an unknown path.

### `Set(s path, v value, a{sv} options) → ()`

Asks the owner to set a value. Options: `norm` (`b`: the value is 0..1),
`role` (`s`, default `all`), `client` (`s`, a label for the owner's logs;
default the caller's bus name). An `enum` or `bool` also takes its label as
`s`. The value is converted to the parameter's
type and clamped to its range, then the owner gets the `Set` signal. Errors:
`NotFound`, `InvalidArgs` (a value of the wrong kind),
`org.freedesktop.DBus.Error.AccessDenied` (`read-only`, or not allowed for
the role). The call returns when the request is passed on, not when the
program has applied it: watch `Changed` for that.

### `Gesture(s path, b begin, a{sv} options) → ()`

Begins or ends a touch (a fader held, a pot turning) for touch automation.
Same options and errors as `Set`, and the parameter must be `automatable`.
controld keeps each client's open gestures and ends them itself when the
client leaves the bus, so a crashed surface can't leave automation writing.

### `SetMany(a(sv) values, a{sv} options) → (u passed)`

Many `Set`s in one call: each `(path, value)` as in `Set`, with one set of
options (`norm`, `role`, `client`) for all of them. **All or nothing**: every
value is checked first (path, role, kind, range), and one bad value refuses
the whole call (`NotFound`, `AccessDenied` or `InvalidArgs`, naming it)
with nothing passed on. Returns the number of values passed on. At most
20 000 values a call. An owner with the `set-many` feature gets them as one
`SetMany` signal; any other owner gets one `Set` signal per value, in order.

### `GetMatching(as patterns) → (a(svds) values)`

Path, value, normalised value and text of every parameter matching any of the
patterns, each once, in tree order. The values-only answer to a big read:
`List` carries every field as JSON, `GetMatching` only what changes (5 000
values in about 2.5 ms against about 15 ms for `List`).

### `MeterTable(s root) → (h memfd, as ids)`

A read-only copy of the root's meter table fd. Error `NotFound` when the
root has no meters.

## Signals

| Signal | To | When |
|---|---|---|
| `Set(s path, v value, s client, s role)` | the owner only | A client asks for a value, already converted and clamped: `d` for `float`, `i` for `int` and `enum`, `b` for `bool` and `trigger`, `s` for `string` |
| `SetMany(a(sv) values, s client, s role)` | the owner only, if it registered `set-many` | The same as `Set` for many values: one run of requests from one client in one role, in the order asked (a path can appear more than once; apply them in order) |
| `Gesture(s path, b begin, s client, s role)` | the owner only | A touch begins or ends |
| `Changed(s root, a(svds) values)` | everyone | Values confirmed by `Update` or `UpdateText`: path, value, normalised value, text. **Batched**: the updates controld handles together (until its loop is idle) become one signal per root, each path once with its latest value. 5 000 single-value `Update` calls in a burst arrive as a few hundred signals, one 5 000-value `Update` as one |
| `Structure(s root)` | everyone | A root was registered, removed, or had parameters added or removed |

## Properties

| Name | Type | Meaning |
|---|---|---|
| `Roots` | `as` | The registered roots |
| `ParamCount` | `u` | Parameters in all roots |
| `Subscriptions` | `u` | OSC subscriptions open (*polled*) |
| `SetRequests` | `t` | Set requests passed to owners (*polled*) |
| `Refused` | `t` | Sets and gestures refused for a role or `read-only` (*polled*) |
| `ChangedSignals` | `t` | `Changed` signals sent (*polled*; with `SetRequests`, shows the batching) |
| `SetSignals` | `t` | `SetMany` signals sent to owners (*polled*) |

## OSC

An OSC listener ([Control1 `Listen`](Control1.md)) exposes the parameter
tree **only when it is given a role** (`params` option, or `"params"` in the
listener's config): `"read-only"`, `"all"`, or a role from `Roles`. A listener
without it serves Control1 mappings only, as before, and every rule of
[Control1's security](Control1.md#security) still applies to both (bind
address, allow-list, TCP secret, rate and size limits).

| Message | Does |
|---|---|
| `PATH x` | Set (an `f`, `d`, `i`, `T`/`F`, or `s` for `string` and `enum` labels) |
| `PATH/norm f` | Set from 0..1 |
| `PATH/touch i` (or `T`/`F`) | Gesture begin (1) or end (0) |
| `PATH` (no argument) | Get: replies `PATH` |
| `/backline/get s PATTERN` | Replies `PATH` for every match |
| `/backline/sub s PATTERN [f RATE] [s "norm"]` | Subscribe: replies with every match now, then with each change, at most `RATE` times a second per path (default 30, at most 60). With `norm` the replies are `PATH/norm f` |
| `/backline/unsub s PATTERN` | Drops that subscription (`*` drops all of this peer's) |
| `/backline/meters s PATTERN [f RATE]` | Meter stream: `ROOT/meter/ID f dB i clip` for the matching meter ids (`/console/in.*`), at `RATE` per second (default 15, at most 30), in bundles |
| `/backline/renew` | Keeps this UDP peer's subscriptions alive |

- **A reply** to `PATH` is `PATH f value s text` (`s value s text` for a
  `string`), sent to where the request came from: a TCP connection, or the
  UDP source address and port.
- **Echo**: a client's own set comes back through its subscription once the
  program confirms it, so a tablet's fader shows what the console did, not
  what the finger asked.
- **Refusals** are answered with `/backline/error s PATH s reason` (a role
  that may not write it, `read-only`, an unknown path), at most 10 a second.
- **Over UDP, one request answers at most 256 values** (a `get` or a new
  subscription's first answer; the rest is refused with `/backline/error`):
  a UDP source address can be forged, and a short packet must not make
  controld flood someone else. Use TCP, or a narrower pattern, for more.
- **UDP subscriptions expire** 30 s after their last `sub` or `renew`; TCP
  subscriptions live with the connection. A peer has at most 32
  subscriptions, a listener at most 32 subscribed peers, controld at most
  256 in all.
- **Subscriptions are indexed.** Which subscriptions match a parameter is
  worked out once, when the subscription or the parameter is made, so a
  change costs a bit scan, not a pattern match per subscription. Use as
  many patterns as the client needs.
- **A TCP peer gets every reply, in order, or is disconnected.** What its
  socket can't take yet is queued (frames are never cut); a peer that
  leaves 4 MiB unread is closed (`Rejected` counts it).
- **Gestures** opened over OSC end when a TCP connection closes, or 60 s after
  the peer's last message on UDP.

## Big trees

A console publishes thousands of parameters. What to use for them:

| To | Use |
|---|---|
| publish the tree | one `Register` (or `Add`) with every parameter |
| confirm many values | one `Update` with all of them (up to 20 000) |
| take many requests at once | `"features": ["set-many"]` in `Register`, then the `SetMany` signal |
| set many values from a client | `SetMany` (all or nothing) |
| read many values | `Get` with all the paths, or `GetMatching` with patterns |
| follow many values over OSC | TCP, and one `/backline/sub` per group of paths |

**Measured** (`tests/tools/params-load.c`, the `params_load` test, on a
private bus with controld `--no-graph`, no PipeWire; i7-12650H, 2026-11-12):
a 5 000-parameter tree, 8 OSC clients (4 TCP, 4 UDP) each subscribed to
2 000 of them in 16 patterns at 60/s.

| | |
|---|---|
| `Register` of 5 000 parameters | 16 ms (10 ms of controld CPU) |
| `Get` of 5 000 paths / `GetMatching` / `List` | 2.9 ms / 2.5 ms / 15 ms |
| 8 clients subscribe, all 16 000 initial values delivered | 56 ms |
| OSC sweep: 20 000 sets (40 000 a second, bundles of 25), each echoed by the program | every echo arrived; set → echo p50 7-10 ms, p99 21 ms (the 60/s subscription rate is most of it); 63 000 values fanned out; 800 `SetMany` signals, about 100 `Changed`; controld 10 % of one core |
| one `SetMany` of 5 000 values to a `set-many` owner | call 2.7 ms; 1 signal, 1 `Update`, 1 `Changed`; all 16 000 subscriber values out in 16-60 ms |
| the same to an owner without `set-many` | call 45 ms; 5 000 `Set` signals and `Update`s, about 270 `Changed`; all out in 70 ms |
| controld peak memory | 32 MB |

## Control1 targets

A Control1 mapping can target a parameter: `param:PATH`. The input's 0..1 is
the normalised value. `gesture: auto` sends a gesture begin before the first
value of a burst and an end 300 ms after the last. `feedback: true` sends the
confirmed value back to the MIDI controller. See [Control1](Control1.md).

## Example

```
# a program (here busctl standing in for one) registers a root
busctl --user call io.github.manuaudio.Backline.Control /io/github/manuaudio/Backline/Params \
  io.github.manuaudio.Backline.Params1 Register ss /demo \
  '{"schema":"backline.params/1","params":[{"path":"gain","type":"float","min":-60,"max":6,"unit":"dB"}]}'
# a client reads it
busctl --user call io.github.manuaudio.Backline.Control /io/github/manuaudio/Backline/Params \
  io.github.manuaudio.Backline.Params1 Get as 1 /demo/gain
# an OSC tablet on a loopback listener with a role
backline map listen udp 127.0.0.1 9000 --params all
oscsend 127.0.0.1 9000 /backline/sub s '/demo/**'
```
