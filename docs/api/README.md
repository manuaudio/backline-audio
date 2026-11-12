# Backline D-Bus APIs

Every Backline service is on the **session bus**. The naming is the same for
all of them:

| Service | Bus name | Object path | Interface |
|---|---|---|---|
| Plugin host supervisor | `io.github.manuaudio.Backline.Host` | `/io/github/manuaudio/Backline/Host` | `io.github.manuaudio.Backline.Host1` |
| One hosted plugin | `io.github.manuaudio.Backline.Instance.<node>` | `/io/github/manuaudio/Backline/Instance` | `io.github.manuaudio.Backline.Instance1` ([Instance1.md](Instance1.md)) |
| Plugin index | `io.github.manuaudio.Backline.Registry` | `/io/github/manuaudio/Backline/Registry` | `io.github.manuaudio.Backline.Registry1` |
| Recorder | `io.github.manuaudio.Backline.Recorder` | `/io/github/manuaudio/Backline/Recorder` | `io.github.manuaudio.Backline.Recorder1` ([Recorder1.md](Recorder1.md)) |
| Player | `io.github.manuaudio.Backline.Player` | `/io/github/manuaudio/Backline/Player` | `io.github.manuaudio.Backline.Player1` ([Player1.md](Player1.md)) |
| Clock and transport | `io.github.manuaudio.Backline.Clock` | `/io/github/manuaudio/Backline/Clock` | `io.github.manuaudio.Backline.Clock1` ([Clock1.md](Clock1.md)) |
| OSC and MIDI control | `io.github.manuaudio.Backline.Control` | `/io/github/manuaudio/Backline/Control` | `io.github.manuaudio.Backline.Control1` ([Control1.md](Control1.md)) |
| Network DSP offload (**draft, not built**) | `io.github.manuaudio.Backline.Offload` | `/io/github/manuaudio/Backline/Offload` | `io.github.manuaudio.Backline.Offload1` ([Offload1.md](Offload1.md)) |

The **bus name has no version number; the interface does.** A later,
incompatible `Recorder2` would be served on the same bus name and object,
next to `Recorder1`, so a client written against `Recorder1` keeps working.

## Rules every interface follows

- **Stability.** Within a `…1` interface, methods, signals and properties are
  only ever *added*. Nothing is removed or retyped, and an option a client
  doesn't send keeps its documented default. Unknown keys in an `a{sv}`
  options dictionary are an `InvalidArgs` error, so typos are caught.
- **Errors.** Standard D-Bus errors where they fit
  (`org.freedesktop.DBus.Error.InvalidArgs`, `…Error.Failed`), and these
  Backline errors, whose message is always a sentence a person can read:

  | Error | Meaning |
  |---|---|
  | `io.github.manuaudio.Backline.Error.Busy` | The service is in a state that doesn't allow the call (already recording, …). |
  | `io.github.manuaudio.Backline.Error.NotFound` | A named node, take, port or instance does not exist. |
  | `io.github.manuaudio.Backline.Error.State` | The call needs a state the service isn't in (`Record` when nothing is armed, …). |
  | `io.github.manuaudio.Backline.Error.Timeout` | A graph operation didn't complete in time (links not up, …). |
  | `io.github.manuaudio.Backline.Error.Unsupported` | Not built in, or not possible on this system (no Link, …). |

- **Properties** emit `org.freedesktop.DBus.Properties.PropertiesChanged`
  when they change, unless the property's row says *polled* (it changes every
  audio cycle; read it when you need it, or use the signal named there).
- **Frames** are `t` (uint64) at the graph rate in the `Rate` property.
  **Times** are nanoseconds, `t`, `CLOCK_MONOTONIC` unless named `utc`.
- **JSON results** are UTF-8 strings with a `"schema"` member naming their
  version (`backline.take/2`, …).
- Services make their own PipeWire links with `object.linger = false`, so a
  link a service makes dies with it. None of them changes the default
  devices.

## Activation

Each service ships a D-Bus activation file (`data/dbus/*.service.in`), so the
first call starts it. The recorder and clock stay up while they have work
(armed, recording, soundcheck playing, transport rolling, Link or timecode
on) and exit after 60 s idle otherwise; their state is in the properties, so
a client that reconnects reads where things are.
