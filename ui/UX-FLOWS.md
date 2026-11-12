# Backline UX flows

The four jobs come first. For each one, this is the shortest path through Omarchy's own workflow: a `Super + Ctrl` hotkey or the bar opens a panel, `j`/`k`/`h`/`l` move, `Enter`/`Space` act, `Esc` closes, `Tab` goes to the next panel, and the Omarchy menu (`Super + Space`) finds everything by name. The screens are built around these paths. The mixer window is for sitting down and working, and none of the four jobs needs it.

Counting rules: a chord such as `Super + Ctrl + I` counts as one keystroke. Typed filter letters count one each. A mouse path counts clicks. "From the desktop" means no Backline panel is open.

Every count below is the measured one. [`omarchy/dev/flows`](../omarchy/dev/flows) runs each flow with real key presses into the real panels. It uses the real services (host, plugin worker, recorder, clock, registry) on the bench's throwaway devices, in a private headless compositor. It times each step from the key press to the moment the service or the graph shows the result. The report is [`omarchy/test/flows-report.txt`](../omarchy/test/flows-report.txt). Every step lands in 44 to 550 ms. That includes about 120 ms per key press, the harness's typing gap; the slowest steps are the ones with three or four keys. A single key lands in under 300 ms, the system's own work included.

## Defaults that save keystrokes

Defaults remove most of the steps, so each one is listed here.

- **A new instrument listens to every MIDI keyboard** (every hardware MIDI source except `Midi Through`), now and when one is plugged in later. "I plugged in my keyboard and it plays" needs no MIDI step. Picking one input is a later, optional step.
- **A new instrument plays to the default output**, as `docs/DECISIONS.md` decides.
- **The instrument picker puts the last-used instrument first**, then `pass` grades, then `warn`. `fail` is listed last and dimmed, and loading one asks for `Shift + Enter` (the host's `force`).
- **The armed set is remembered.** Arming a source arms the recorder at once, with 5 s of pre-roll, so a take started late still has the downbeat.
- **One hotkey records** (`Super + Ctrl + Alt + M`, the `Alt` variant of the Record panel's hotkey, as `Super + Ctrl + Alt + C` is to the capture menu). It arms the remembered set and records. Pressed again, it stops and saves.
- **Timecode chase remembers its input and frame rate**, so after the first time, chasing is one pill.

## Hotkeys

These are optional. `omarchy/install` offers to add them to `~/.config/hypr/bindings.lua`. They are on the planes `manual/07-hotkeys.md` gives them, and use keys that are free in Omarchy's defaults.

| Hotkey | Plane | Opens or does |
|---|---|---|
| `Super + Ctrl + I` | panels | Instruments panel |
| `Super + Ctrl + M` | panels | Record panel |
| `Super + Ctrl + G` | panels | Transport panel |
| `Super + Ctrl + Alt + M` | quick action, the `Alt` variant of M | Record or stop the remembered set |
| `Super + Ctrl + Alt + G` | quick action, the `Alt` variant of G | Play or stop the transport |

The Plugins panel has no hotkey. It sits beside Instruments in the bar, one `Tab` away, and the menu finds it as "Plugins".

## 1. Play an instrument from a keyboard

**First time, from the desktop: 6 keystrokes.**

| # | Keys | What happens |
|---|---|---|
| 1 | `Super + Ctrl + I` | The Instruments panel opens. With nothing loaded, the cursor is already on **Add instrument**. |
| 2 | `Enter` | The picker opens in place with its filter focused. It lists the registry's instruments with their grades. |
| 3–5 | `s` `u` `r` | The list narrows to Surge XT. |
| 6 | `Enter` | The instrument is loaded, following every MIDI keyboard and playing to the default output. The row shows `STARTING`, then `RUNNING`, with a live level meter. Play the keyboard. |

- **Again, from the desktop: 3 keystrokes.** `Super + Ctrl + I`, `Enter`, `Enter`. The last-used instrument is first in the picker.
- **Mouse: 3 clicks.** The bar icon, **Add instrument**, then the instrument.
- **Through the Omarchy menu.** `Super + Space`, then type `instr`, then `Enter` on **Add Instrument**. The same picker opens as a menu-select list.
- **Choosing one keyboard (optional): 3 keystrokes or more in the open panel.** `j` to the instrument row, then `h`/`l` along its MIDI pills (**All keyboards**, then each connected input), then `Enter`. The instrument restarts with its state, so its sound is kept.
- **Removing: 2 keystrokes in the open panel.** `j` to the row, then `x`.
- **Status.** The bar icon shows a count while instruments run, and turns the urgent colour if the host reports `Failed`. A crash and restart shows as `RESTARTING`, then `RUNNING`, on the row.

## 2. Record a set

**First time, from the desktop: 4 keystrokes for two sources.**

| # | Keys | What happens |
|---|---|---|
| 1 | `Super + Ctrl + M` | The Record panel opens. The cursor is on the first source (inputs, then instruments, then outputs, each with a level meter). |
| 2 | `Space` | Arms that source. The recorder arms at once, with pre-roll. The hero reads `ARMED · 1 TRACK`. |
| 3 | `j` `Space` | Arms the next source. This is 2 keystrokes, so step 3 brings the total to 4 for two sources. |
| 4 | `r` | Records. The hero turns the urgent colour, reads `RECORDING 00:00:05` (the pre-roll is already in the take), and the bar icon turns red. |

- **Marker: 1 keystroke.** `m` while recording.
- **Stop: 1 keystroke.** `r` again. A notification names the take and its length.
- **Again, from anywhere: 1 keystroke to start and 1 to stop.** `Super + Ctrl + Alt + M`. The remembered set is armed and recorded. Notifications say "Recording 2 tracks" and "Saved take-… (3:12)".
- **Mouse: 2 clicks to start after arming.** The bar icon, then the record button. Each source is one click to arm.

## 3. Virtual soundcheck (a take in place of its inputs)

**From the desktop, after a take: 4 keystrokes to switch one input.**

| # | Keys | What happens |
|---|---|---|
| 1 | `Super + Ctrl + M` | The Record panel opens. |
| 2 | `p` | Plays the newest take for soundcheck. Its inputs are listed under **SOUNDCHECK**, each with **LIVE** and **PLAYBACK** pills. All start live. |
| 3 | `j` | Moves to the first input. |
| 4 | `l` | Switches it to **PLAYBACK**. Whatever listened to that input now hears the take. |

- **All inputs to playback: 1 keystroke.** `a`.
- **Stop soundcheck: 1 keystroke.** `p` again. Every input goes back to live.
- **An older take: 2 keystrokes or more.** `j` to it under **TAKES**, then `Enter`.

## 4. Check a plugin

**From the desktop: 6 keystrokes, with no `Enter`.**

| # | Keys | What happens |
|---|---|---|
| 1 | `Super + Ctrl + I` | The Instruments panel opens. |
| 2 | `Tab` | Goes to the Plugins panel beside it. It shows `pass`/`warn`/`fail` counts for the whole system. |
| 3 | `/` | Focuses the filter. |
| 4–6 | `d` `e` `x` | The list narrows to Dexed. The row under the cursor shows its grade, its formats, and the finding behind a `warn` or `fail` (for example `SIGABRT in process` or `writes into the working directory`). |

- **Details: 1 keystroke.** `Enter` on a row lists every format's grade, path and findings.
- **Load it as an instrument: 1 keystroke.** `a` on the row.
- **Rescan: 1 keystroke.** `s`. This is the registry's sandboxed scan, so a crashing plugin can't take the panel down.
- **Only the problems: 3 keystrokes.** `l` `l` `l` to the **ISSUES** pill.
- **Through the Omarchy menu.** `Super + Space`, then type `plug`, then `Enter`.

## 5. Lock to timecode

**First time, from the desktop: 6 keystrokes.**

| # | Keys | What happens |
|---|---|---|
| 1 | `Super + Ctrl + G` | The Transport panel opens, with the cursor on the **SYNC** pills (**Internal**, **Link**, **LTC**, **MTC**). The hero shows the timecode, bars and beats, tempo and the sync source. |
| 2–3 | `l` `l` | Moves to **LTC**. |
| 4 | `Enter` | Chases LTC from the first audio input. Pills for the input and frame rate appear under it. The status reads `NO SIGNAL` if there's no timecode on that input. |
| 5 | `j` | Moves to the input pills (every source and output). |
| 6 | `l` | The next input, applied at once. The flows test's LTC line is one pill along. The status reads `LOCKED · 0 FR`, the chase error in frames, and the timecode runs. |

- **Again, from the desktop: 4 keystrokes.** `Super + Ctrl + G`, `l` `l`, `Enter`. The input and frame rate are remembered. Measured: locked about 200 ms after `Enter`, chase error 0 frames.
- **Choosing the frame rate: 2 keystrokes or more.** `j` to the fps pills (24, 25, 29.97 DF, 30), then `h`/`l`, which applies at once.
- **Status everywhere.** While chasing, the bar widget shows the running timecode and a lock glyph (an open lock in the urgent colour while not locked). The panel need not be open.
- **Play/stop from anywhere: 1 keystroke.** `Super + Ctrl + Alt + G`. While chasing, it's an error notification, since the timecode moves the transport. In the panel, it's `Space`.

## 6. Open the mixer

**From any Backline panel: 1 keystroke.** `o` opens the Backline mixer, or focuses it if it's already open. It's also in the app launcher and the menu ("Backline Mixer").

In the window, `h`/`l` move between channel strips, `j`/`k` move between a strip's controls, `Space` records or stops, `a` arms the strip, `m` mutes it, `[`/`]` move its fader 1 dB, `?` lists every key, and `q` quits. The window is described in [`DESIGN-LANGUAGE.md`](DESIGN-LANGUAGE.md).

## Keys, the same everywhere

| Key | In every Backline panel |
|---|---|
| `j`/`k`, arrows | move the cursor |
| `h`/`l`, arrows | move along a pill row, or adjust the value under the cursor |
| `Enter`/`Space` | act on the cursor |
| `x`/`Delete` | remove (an instrument) |
| `Esc` | leave the filter or picker, or close the panel |
| `Tab` | next panel in the bar |
| `o` | open the mixer window |
| `/` | filter (Plugins, the instrument picker) |

| Key | Panel-specific |
|---|---|
| `r` | record or stop (Record) |
| `m` | marker (Record) |
| `p` | soundcheck the newest take, or stop it (Record) |
| `a` | arm every source (Record), all inputs to playback (soundcheck), load as instrument (Plugins) |
| `s` | rescan (Plugins) |
| `Space` | play or stop (Transport) |
