# Backline in Omarchy

Backline's everyday jobs live inside Omarchy's own workflow. There is one Omarchy plugin, `io.github.manuaudio.backline`, holding five bar panels, `omarchy-backline-*` commands in Omarchy's command style, rows in the Omarchy menu, and `Super + Ctrl` hotkeys on the planes Omarchy gives them. Everything is built from Omarchy's shell components and theme tokens, so it follows the current theme and language the way Omarchy's own panels do.

| Instruments | Record | Plugins | Transport |
|---|---|---|---|
| ![Instruments: the picker](../docs/screenshots/2-instruments-picker.png) | ![Record: recording two armed sources](../docs/screenshots/6-record-recording.png) | ![Plugins: a failing plugin and why](../docs/screenshots/9-plugins-finding.png) | ![Transport: locked to LTC](../docs/screenshots/10-transport.png) |

The jobs, and the fewest keystrokes for each (measured), are in [`ui/UX-FLOWS.md`](../ui/UX-FLOWS.md). In short:

| Job | Keys |
|---|---|
| play an instrument from a keyboard, first time | `Super+Ctrl+U` `Enter` `s u r` `Enter`: 6 |
| the same, again | `Super+Ctrl+U` `a` `Enter`: 3 |
| record a set, first time (two sources) | `Super+Ctrl+M` `Space` `j Space` `r`: 5 |
| record, from anywhere, again | `Super+Ctrl+Alt+M` to start, and again to stop: 1 each |
| virtual soundcheck: one input on playback | `Super+Ctrl+M` `p` `l`: 3 |
| check a plugin | `Super+Ctrl+U` `Tab` `/` `c a r`: 6, with no `Enter`; the grade and the finding are on the row |
| lock to timecode, first time | `Super+Ctrl+G` `l l` `Enter` `j` `l`: 6 |
| lock to timecode, again | `Super+Ctrl+G` `l l` `Enter`: 4 |
| open the mixer | `o` in any Backline panel |

## What's here

| Path | What |
|---|---|
| `../manifest.json` | the plugin's manifest, at the repository root, so `omarchy plugin add` takes the repository as the plugin |
| `plugin/Backline.qml` | the bar widget: the five panels side by side, `Tab` between them |
| `plugin/{Instruments,Record,Plugins,Transport,Setup}.qml` | the panels; Setup is Audio Setup (below) |
| `plugin/Model.js`, `plugin/i18n/` | the panels' pure logic (picker order, filters, grades, sources, timecode, status tones) and their catalogs |
| `bin/omarchy-backline-*` | commands: `instrument-add`, `instrument-remove`, `instrument-midi`, `record`, `soundcheck`, `transport`, `plugins-scan`, `mixer`, `pro`, `aggregate`, `midi`, `setup`, and the panels' `state` and `watch` |
| `lib/omarchy-backline.sh` | their shared D-Bus, MIDI and catalog helpers |
| `i18n/en.json` | the commands' notification and prompt strings |
| `extensions/omarchy-menu.jsonc` | the menu rows (Trigger › Backline, Trigger › Toggle › Pro Audio, Setup › Audio) |
| `hypr/backline.lua` | the hotkeys |
| `install` | installs it all from a checkout for the current user; `--uninstall` takes it out again |
| `dev/` | the bench, the headless compositor and the flows (below) |
| `test/` | tests in Omarchy's shape |

## Audio Setup: pro audio, combined devices, MIDI

The fifth panel is what macOS keeps in Audio MIDI Setup and Omarchy's own audio panel leaves out. Volume, outputs and the app mixer stay in Omarchy's audio panel (`Super + Ctrl + A`). This work started as Omarchy pull requests (`manuaudio/omarchy-audio`) and moved here, so Omarchy itself only gets small invisible defaults and fixes.

| Command | What it does |
|---|---|
| `omarchy-backline-pro on [--buffer N] [--rate R]`, `off`, `toggle`, `status [--json]` | Forces a small buffer (256 frames, or 128 once PipeWire runs realtime) and locks the sample rate in PipeWire's `settings` metadata, live, with nothing restarted. A user unit sets it again each time PipeWire starts; `off` removes it and never clears a buffer something else forced. |
| `omarchy-backline-aggregate create [--mirror] <name> <device>...`, `remove`, `list [--json]` | An aggregate device (members' channels stacked in order) or, with `--mirror`, a multi-output device (the same stereo on every output). Each is PipeWire's `combine-stream` in its own PipeWire client and user unit, so making or removing one restarts nothing. With no arguments it asks through Omarchy's menu picker. |
| `omarchy-backline-midi list [--all] [--json]`, `connect [<source> [<destination>]]`, `disconnect` | The ALSA sequencer's ports with kind, protocol (MIDI 1.0, MIDI 2.0) and connections; `connect <source>` opens the menu picker with its destinations ticked. |

The panel: the PRO AUDIO switch on its header line, with buffer and sample-rate pills while it is on; the combined devices, with Aggregate Device and Multi-Output Device; the MIDI ports that send, each with where it sends. `p` turns pro audio on or off, `x` removes a combined device, Enter on a MIDI port opens the picker, and right-clicking the bar icon turns pro audio on or off. `Super + Ctrl + Alt + A` is the quick action (pro audio on or off), and _Trigger › Toggle_ gets Pro Audio On and Off like Omarchy's own toggles.

## How it follows Omarchy

The rules are from Omarchy's `AGENTS.md`, `agents/skills/shell-dev.md`, `docs/menu.md`, `docs/i18n.md` and the hotkeys manual (`quattro`, read 2026-11-11).

**Panels.**

- Each panel is `Panel` with `KeyboardPanel { content: PanelKeyCatcher { … } }`, so it's built on first open.
- There is one cursor model shared by keyboard and mouse. Hover moves the cursor; visuals come from `hasCursor`/`current` through `CursorSurface`.
- Keys: `j`/`k` and the arrows move, `h`/`l` move along pills or adjust, `Enter`/`Space` act, `x` removes, `Esc` closes, and `Tab` goes to the next panel.
- Only Omarchy's components are used: `PanelSectionHeader`, `PanelSeparator`, `Button` pills, `PanelActionButton`, `FilterInput`, `BarIconButton`. Colours come from `Commons.Color` and `Style`; there are no hard-coded colours.
- Each panel answers its own IPC target (`backline.instruments`, …) with `open`, `toggle`, `refresh` and a `state` the tests read; the hotkeys and menu rows open panels through these targets.
- Repeater rows are primitive snapshots, never `PwNode`s.
- Strings come from `Commons.Translations` with the plugin's own catalog. Every `Text` declares `textFormat`.
- The plugin (the repository root, and an installed copy) passes Omarchy's `omarchy-plugin-validate`: one manifest, no symlinks.

**No polling.**

- A panel reads its state once on open.
- While open, it follows the services' D-Bus signals through `omarchy-backline-watch`, an event stream run only then.
- A closed panel runs nothing. The exceptions are Record during a take or soundcheck and Transport while rolling or chasing, which keep the bar icon true. Otherwise a closed panel re-reads only when a Backline node appears or goes in PipeWire (a load, a crash, a restart), or on a push from a command.
- Measured: with every panel closed, the shell starts no processes and uses 0.1% of a core.

**Commands.**

- `#!/bin/bash`, `omarchy:summary`/`args` metadata, and a comment saying why the command exists and what it doesn't do.
- Notifications go through `omarchy-notification-send` with glyphs and translated text.
- Pickers use `omarchy-menu-select`.
- After a change, a command pushes `omarchy-shell -q <panel> refresh`.
- Choices are remembered under `~/.local/state/backline/omarchy/`.
- No sudo, and the default devices are never changed.

**Menu.** Rows live in the user's menu extension, with cheap `when` guards: a process check, or one D-Bus property read that never starts a service. There are no aliases.

**Hotkeys.**

- `Super + Ctrl + U`, `M`, `G` open the Instruments, Record and Transport panels, on the panels plane. (`Super + Ctrl + I` is Omarchy's "Toggle locking on idle".)
- `Super + Ctrl + Alt + M`, `G` are the quick-action variants of `M` and `G` (record or stop; play or stop), as `Super + Ctrl + Alt + C` is to the capture menu.
- Where the panels can't open (a shell before Quattro, such as Omarchy 4.0.4's, or no bar plugin), setup leaves out every key and menu row that would open one, and `Super + Ctrl + M` opens the Backline Mixer instead. The quick actions and the command rows stay.
- Setup checks each key against what is already bound on the system (`hyprctl binds -j`, or Omarchy's default bindings and the user's files without Hyprland) and doesn't bind a key that is taken; it says which and why.
- Setup backs each file up before it changes it (`<file>.bak.backline-<date>-<time>`), changes nothing when run again, and `--uninstall` restores both files byte for byte.

**Third-party, not first-party.** This installs as a third-party plugin under `~/.config/omarchy/plugins/io.github.manuaudio.backline`, with its own commands and catalogs from the `backline-audio` package. The `backline` command group isn't in Omarchy's `GROUP_DESCRIPTIONS`. Proposing any of it to Omarchy itself would follow `ARCHITECTURE.md` §5.8: one problem per PR, with the measured evidence.

## Install

Backline is an Omarchy plugin plus a package: the package (`backline-audio`, AUR) brings the services and the `omarchy-backline-*` commands; the plugin brings the panels. Everything about the format, updates, removal and the plugin marketplace is in [`docs/OMARCHY-PLUGIN.md`](../docs/OMARCHY-PLUGIN.md).

```sh
yay -S backline-audio
omarchy plugin add https://github.com/manuaudio/backline-audio --enable
omarchy-backline-setup          # asks before adding the menu rows and the hotkeys
```

Remove: `omarchy-backline-setup --uninstall`, `omarchy plugin remove io.github.manuaudio.backline`, `yay -R backline-audio`.

The panels need Omarchy's quattro shell (the plugin API with `ShellIpc`, `FilterInput`, `Commons.I18n` and `Commons.Translations`). Omarchy 4.0.4's shell predates it, and the panels won't load there; the commands, menu rows and mixer don't depend on it.

From a checkout, without the package:

```sh
cmake -S ui -B ui/build && cmake --build ui/build    # the mixer (optional)
omarchy/install                                       # asks before the menu and hotkeys; --yes, --no-hotkeys, --no-menu
omarchy/install --uninstall
```

The panels sit in one slot in the bar's right section, in the order Transport, Instruments, Plugins, Record, Audio Setup. `Tab` moves to the next of them, and past the last to the bar's next panel (around the five when the bar has no other). Move the slot with `omarchy bar move`.

## Tests

Tests run in Omarchy's style: `pass`/`fail` TAP lines, a stubbed world, and the real code.

| Test | What it covers |
|---|---|
| `test/model-test.sh` | the panels' model in Node: picker order, filters, MIDI pills, plugin findings, sources, drop-frame timecode, chase status |
| `test/commands-test.sh` | the commands against stubbed `busctl`, `pw-dump` and `backline`, in a fake HOME: MIDI matches checked as the worker's regex, picker order and ids, the fail-grade refusal, arming and re-arming, notifications in pt-BR → pt → English |
| `test/audio-setup-test.sh` | pro audio, combined devices and MIDI against stubbed `pw-metadata`, `pw-dump`, `systemctl` and a sequencer fixture: the 256/128 defaults, choices restored after a PipeWire restart, `off` leaving a foreign buffer, channel stacking and mirroring, refused names, a device that never appears undone, the menu flows |
| `test/i18n-test.sh` | the catalogs are current, every key is used and exists, and every `Text` has a `textFormat` |
| `test/install-test.sh` | the installer and `omarchy-backline-setup` in a fake HOME: nothing changes without a yes, the plugin passes Omarchy's validator (installed and as the repository), an old four-plugin install is replaced, the menu is merged and parsed by Omarchy's own `MenuModel.js`, the user's rows and bindings are kept, a rerun is idempotent, and uninstall restores each file byte for byte |

End to end:

- `dev/flows` drives every flow of [`ui/UX-FLOWS.md`](../ui/UX-FLOWS.md) with real key presses into the real panels. It runs on a private headless compositor (`dev/headless`) against the real services on throwaway devices (`dev/bench`), times each step, and takes the screenshots. Report: [`test/flows-report.txt`](test/flows-report.txt).
- The bench never touches the live desktop. It uses its own session bus, its own Quickshell instance and stubbed desktop helpers. Its nodes are `backline-bench.*`, in their own node group. A watchdog stops it the moment anything of it links to a real device.
