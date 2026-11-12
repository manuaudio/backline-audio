# `io.github.manuaudio.Backline.Instance1`

One hosted plugin: a `backline-plugin-worker` process, one PipeWire node.
Every worker serves this interface, whether hostd started it or someone ran
the worker by hand. The plugin may be CLAP, VST3 or LV2; the interface is the
same for all three.

- Bus name `io.github.manuaudio.Backline.Instance.<node>`, where `<node>` is
  the worker's `node.name` with every character that isn't `[A-Za-z0-9]`
  replaced by `_` (and `n_` in front if it would start with a digit).
  `backline.synth` → `io.github.manuaudio.Backline.Instance.backline_synth`.
  The node also carries the bus name as `backline.dbus.name`.
- Object `/io/github/manuaudio/Backline/Instance`
- Status: **0.x, additive-only** (see [README](README.md)).
- Main-thread service: calls are answered between audio cycles and never
  block the audio thread. Values reach the plugin's audio thread through a
  wait-free ring (they take effect in the next cycle).

## Parameters

A parameter has an `id` (`u`), a `name`, a `module`, a value and a range.
What those are depends on the format:

| | CLAP | VST3 | LV2 |
|---|---|---|---|
| `id` | `clap_id` | `ParamID` | control port index |
| `name` | `clap_param_info.name` | `ParameterInfo.title` | port `lv2:name` |
| `module` | `clap_param_info.module` | name of the parameter's unit (`IUnitInfo`), `""` for the root unit | `""` |
| value, min, max | plain values | **plain** values: the controller's `normalizedParamToPlain` (stepped parameters are whole steps) | port values (`lv2:sampleRate` ranges scaled by the rate) |
| read-only | `CLAP_PARAM_IS_READONLY` | `kIsReadOnly` | output control ports are not listed |

A parameter **key** in the calls below is one of: the numeric id, the exact
name (case-insensitive), `module/name`, or a substring that matches exactly
one `module/name`. Values are clamped to the range; stepped (CLAP stepped,
VST3 `stepCount > 0`, LV2 `integer`/`enumeration`/`toggled`) values are
rounded.

## Methods

### `ListParams() → a(ussddd)`

Every parameter as `(id, name, module, value, min, max)`.

### `GetParam(s key) → d`

The current value. Error `io.github.manuaudio.Backline.Error.NoSuchParam`
when no single parameter matches `key`.

### `SetParam(s key, d value) → d`

Sets a parameter and returns the value applied (clamped). The change reaches
the plugin with the next audio cycle (CLAP: a param-value event; VST3: a point
in `IParameterChanges` at offset 0, and the edit controller is updated at
once; LV2: the control port before `run()`). While the plugin isn't active it
goes to the plugin on the main thread (CLAP `params.flush`). Errors
`NoSuchParam`; `EPERM` for a read-only parameter; `EAGAIN` if the control
ring is full.

### `NoteOn(y channel, y key, y velocity)`, `NoteOff(y channel, y key)`

A MIDI note on or off, delivered at the start of the next cycle. Needs a
plugin with note input (CLAP note port, VST3 event bus, LV2 MIDI atom port);
otherwise it is counted in `MidiEvents` and dropped.

### `SaveState(s path)`, `LoadState(s path)`

Write the plugin's state to `path` (atomically: temp file, fsync, rename), or
load it from `path`. The file format is the format's own:

| | State file |
|---|---|
| CLAP | the `clap_plugin_state` stream, as the plugin writes it |
| VST3 | a `.vstpreset` image (component and controller state), so a state file is also a preset file |
| LV2 | an LV2 state in Turtle (`lilv_state_to_string`): port values and the plugin's `state:interface` properties |

`LoadState` re-reads every parameter value afterwards. For LV2 the audio
thread is kept out of the plugin during the restore (it is in the LV2
instantiation threading class): one or two cycles of silence. Error `Failed`
with the reason.

### `ListPresets() → a(sss)` *(added in 0.2)*

The presets the plugin offers, as `(id, name, location)`:

| | Where presets come from | `id` | `location` |
|---|---|---|---|
| CLAP | every `preset-discovery` provider in the plugin file, its declared locations crawled for this plugin's id; only listed if the plugin has the `preset-load` extension | the file path (`path#load_key` inside a container file), or `plugin:<load_key>` for presets inside the plugin | the file, or `(inside the plugin)` |
| VST3 | `.vstpreset` files for this class id in `~/.vst3/presets/<vendor>/<plugin>/`, `/usr/share/vst3/presets/…`, `/usr/local/share/vst3/presets/…` and the worker's `--preset-dir`s; and the programs of the plugin's program lists (`IUnitInfo`) that have a program-change parameter | the file path, or `program:<list id>:<index>` | the file, or `program list '<name>'` |
| LV2 | `pset:Preset` resources related to the plugin (its own bundle and preset bundles that name it) | the preset URI | the Turtle file that holds it |

The list is built when called (files may have come or gone). A large preset
library takes a moment (Surge XT's 2 986 CLAP presets: about 1.3 s).

### `LoadPreset(s id)` *(added in 0.2)*

Loads a preset. `id` is an id from `ListPresets`, an exact name, or a
substring that matches exactly one name; a path to a preset file (with a `/`)
works too. CLAP: `preset-load.from_location`; VST3 file: the component and
controller state from the `.vstpreset`; VST3 program: the program-change
parameter, as `SetParam` would; LV2: `lilv_state_restore` (audio kept out as
for `LoadState`). Parameter values are re-read afterwards, and again 100 ms
and 500 ms later (plugins may apply a preset asynchronously). The state counts
as changed (`StateSerial`), so hostd's state file follows. Errors `NotFound`
(no unique match), `Failed` with the plugin's reason.

### `SavePreset(s name) → s path` *(added in 0.2)*

Saves the current state as a preset file and returns its path. VST3 only: a
`.vstpreset` in the first `--preset-dir`, else
`~/.vst3/presets/<vendor>/<plugin>/<name>.vstpreset`; a `name` with a `/` is
the path itself. Error `io.github.manuaudio.Backline.Error.Unsupported` for
CLAP and LV2.

## Properties

| Property | Type | Changes | Meaning |
|---|---|---|---|
| `PluginId` | `s` | const | CLAP plugin id, VST3 class id (32 hex digits, as the registry lists it), or LV2 URI |
| `PluginName` | `s` | const | The plugin's name |
| `PluginPath` | `s` | const | The `.clap` file, `.vst3` bundle or `.lv2` bundle |
| `PluginFormat` | `s` | const | `clap`, `vst3` or `lv2` *(added in 0.2)* |
| `NodeName` | `s` | const | `node.name` |
| `NodeId` | `u` | polled | The node's global id (`SPA_ID_INVALID` until the node is up) |
| `OscPort` | `u` | const | The OSC port on 127.0.0.1 |
| `SampleRate` | `u` | polled | The rate the plugin is activated at (re-activated when the graph rate changes) |
| `Cycles` | `t` | polled | Audio cycles run |
| `MidiEvents` | `t` | polled | MIDI messages received (port and `NoteOn`/`NoteOff`) |
| `DroppedEvents` | `t` | polled | Events that did not fit (rings or per-cycle arrays full) |
| `ReleasedNotes` | `t` | polled | Notes the worker released because a followed MIDI source went away with keys down |
| `StateFile` | `s` | const | `--state-file` (empty if none) |
| `StateSerial` | `t` | emits change | Counts changes a saved state would see (parameter sets, plugin "dirty", values the plugin reports, `LoadState`, `LoadPreset`) |
| `StateSaved` | `t` | emits change | The `StateSerial` the state file holds; equal to `StateSerial` means saved |
| `StateSaves` | `t` | polled | Autosaves written |

## Signals

None. `StateSerial` and `StateSaved` emit `PropertiesChanged`.

## OSC (127.0.0.1:`OscPort`, UDP)

The same control for control surfaces and scripts. The `/backline` prefix is
optional on incoming addresses.

| Address | Arguments | Reply |
|---|---|---|
| `/backline/param/set` | `s\|i key`, `f\|d\|i value` | none |
| `/backline/param/<key>` | `f\|d\|i value` | none |
| `/backline/param/get` | `s\|i key` | `/backline/param i id, s module/name, d value, d min, d max` |
| `/backline/param/list` | | one `/backline/param` per parameter, then `/backline/param/end i count` |
| `/backline/note/on` | `channel, key, velocity` (numbers) | none |
| `/backline/note/off` | `channel, key` | none |
| `/backline/preset/list` *(0.2)* | | one `/backline/preset s id, s name, s location` per preset, then `/backline/preset/end i count` |
| `/backline/preset/load` *(0.2)* | `s\|i key` | `/backline/preset/loaded i ok, s name-or-error` |

## The node

The worker's node carries `media.class` `Stream/Output/Audio` for
instruments and `Audio/Filter` for effects (DECISIONS D2), `node.name`,
`node.description` `<plugin name> (Backline)`, and:

| Property | Value |
|---|---|
| `backline.plugin.format` | `clap`, `vst3`, `lv2` |
| `backline.plugin.id`, `backline.plugin.path` | as `PluginId`, `PluginPath` |
| `backline.osc.port`, `backline.dbus.name` | as above |
| `audio.channels`, `audio.position` *(0.2)* | the output channel layout: the first output bus keeps `MONO` or `FL,FR`; every further output channel is `AUX<n>` (its index on the node). Each `out_*` port's `audio.channel` is its entry in this list. |

Ports: `midi_in` (8-bit raw MIDI, when the plugin takes notes), `in_<pos>`,
`in<k>_<pos>`, `out_<pos>`, `out<k>_<pos>` (32-bit float mono DSP), one per
channel of each audio bus (CLAP audio port, VST3 bus active by default, LV2
port group).
