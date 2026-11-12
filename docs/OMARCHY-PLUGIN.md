# Backline as an Omarchy plugin

Backline ships to Omarchy Linux the way community plugins do: a git
repository with a `manifest.json` at its root, added with `omarchy plugin
add`, plus a package for what a plugin can't carry (the services and the
commands). This repository is that plugin, so there is no second repository
to keep in step.

| Piece | Comes from | What it is |
|---|---|---|
| The panels | `omarchy plugin add` (this repository) | One bar plugin, `io.github.manuaudio.backline`: Transport, Instruments, Plugins and Record side by side in one bar slot (`omarchy/plugin/`) |
| The services and commands | `backline-audio` (AUR) | `backline-*` services, the `backline` CLI, `omarchy-backline-*` commands |
| Menu rows and hotkeys | `omarchy-backline-setup` | Asked for, then written between `backline:begin`/`backline:end` markers in the user's own files |

## Install, update, remove

```sh
yay -S backline-audio
omarchy plugin add https://github.com/manuaudio/backline-audio --enable
omarchy-backline-setup                 # asks before the menu rows and the hotkeys
```

```sh
omarchy plugin update io.github.manuaudio.backline     # the panels (shows a diff, fast-forwards)
yay -Syu                                                # the services and commands
```

```sh
omarchy-backline-setup --uninstall                      # menu rows and hotkeys out, the files otherwise as they were
omarchy plugin remove io.github.manuaudio.backline
yay -R backline-audio
```

From a checkout without the package, `omarchy/install` does all three steps
for the current user (commands into `~/.local/bin`, the plugin copied into
`~/.config/omarchy/plugins/io.github.manuaudio.backline`, then setup), and
`omarchy/install --uninstall` takes them out. It also removes the four
per-panel plugins (`backline.instruments`, …) of versions before 0.4.

**Requirement.** The panels use the quattro shell's plugin API (`ShellIpc`,
`FilterInput`, `Commons.I18n`, `Commons.Translations`). Omarchy 4.0.4's
shell predates it and won't load them; the services, the commands, the menu
rows and the mixer work there regardless.

## The format, as used here

From Omarchy's `shell/README.md` and `bin/omarchy-plugin-validate` (quattro):

- `manifest.json` at the repository root: `schemaVersion` 1, `id`, `name`,
  `version`, `kinds`, `entryPoints`; the marketplace also wants `author`,
  `description` and a lowercase, permanent id. Ours:
  `io.github.manuaudio.backline`, kind `bar-widget`, entry point
  `omarchy/plugin/Backline.qml`, `barWidget.defaultSection` `right`,
  `license` MIT, `version` equal to the release tag.
- Entry points are relative paths inside the repository, with no `..`.
- No symlinks anywhere in the plugin folder (the whole clone), except `.git`.
- One manifest holds one bar widget, so the four panels live in one widget
  (`Backline.qml`, a row of four `Panel`s, each with its own icon, popup and
  IPC target). `Tab` moves between them, and past the last to the bar's
  next panel.
- `omarchy plugin add` only clones, validates and enables: it never runs
  plugin code, hooks or sudo. There is no manifest field for packages, menu
  rows or hotkeys. Hence the package and `omarchy-backline-setup`.
- Hotkeys and menu rows open a panel through its IPC target
  (`omarchy-shell -q backline.record toggle`), since the panels are no longer
  plugins of their own. With several monitors, that reaches the instance that
  holds the target, which may not be the focused monitor's.

Check it: `omarchy plugin validate .` (or
`/usr/share/omarchy/bin/omarchy-plugin-validate .`) from the repository
root. `omarchy/test/install-test.sh` runs the validator on the repository and
on an installed copy.

## The plugin marketplace (not submitted)

Omarchy's community plugin index is **plugins.omarchy.org**, built from the
repository `omacom/omarchy-plugin-marketplace` (its `registry.json`). A plugin
is submitted by opening an issue there with the **Submit plugin** form
(`.github/ISSUE_TEMPLATE/submit-plugin.yml`); a bot checks the structure and
quattro compatibility and runs a security baseline on the exact commit, and a
maintainer approves it (label `approved-and-verified`). Later versions go
through the `verify-plugin.yml` form with the new commit SHA.

What the listing needs, and where we stand:

| Requirement (SUBMISSION.md) | Here |
|---|---|
| Public GitHub repo, one plugin, `manifest.json` at the root | Yes |
| Root README with install and removal | The top-level README's Install section (and this file) |
| Root LICENSE | MIT |
| External dependencies documented | `backline-audio` (AUR), PipeWire, quattro shell |
| Permanent, unique, lowercase id | `io.github.manuaudio.backline` |
| `preview.png` at the root (optional) | Yes |

Expect **review-required** from the security scan: the repository contains
build and packaging files (a PKGBUILD, systemd units, a pacman hook) and
commands that start user services. None of it runs on `omarchy plugin add`,
but the scan looks at the whole commit, and a maintainer then reviews it.
Two checklist items need Manu's own yes before ticking: the plugin doesn't
overwrite user configuration without consent (true: setup asks, and writes
only between markers), and the maintainer agrees to the marketplace terms.

The form has six fields, in order. Ready to paste:

```
### Repository URL
https://github.com/manuaudio/backline-audio

### Category
Other

### Tags
media, quickshell, bar

### Suggest a missing tag
audio

### Maintainer notes
Backline is Mac-style pro audio for Omarchy Linux: system instruments
(CLAP, VST3, LV2 hosted out of process as PipeWire nodes), sample-aligned
multitrack recording, a plugin index, and one transport with LTC/MTC and
Ableton Link. The plugin is one bar widget with four panels (Transport,
Instruments, Plugins, Record); manifest.json is at the repository root, entry
point omarchy/plugin/Backline.qml.

It needs the backline-audio package (AUR) for the services and the
omarchy-backline-* commands; the README says so before the
`omarchy plugin add` line. Menu rows and hotkeys are optional and only added
by `omarchy-backline-setup` after asking, between backline:begin/end markers
that `--uninstall` removes exactly. The plugin code itself never calls sudo or
a package manager. The rest of the repository is the services' source.

### Submission checklist
- [x] The repository is public and has one plugin with manifest.json at its root
- [x] The README documents installation and removal
- [x] The repository has a license
- [x] External dependencies are documented
- [x] The plugin does not overwrite user configuration without explicit consent
```

The checklist wording above follows the form's five boxes as the research
read them on 2026-11-12; check them against the live form before submitting.
Category: there is no Audio category; `Other` (or `Desktop`).
