# Backline design language

How Backline looks, and where each choice comes from. There are two surfaces:

- **The Omarchy panels** (Instruments, Record, Plugins, Transport) are built from Omarchy's own shell components and tokens and nothing else. They look like Omarchy's audio and network panels because they *are* made the same way. This document mostly doesn't apply to them; [`omarchy/README.md`](../omarchy/README.md) says how they follow Omarchy.
- **The mixer window** (`backline-mixer`) is the larger, pro-audio surface. Its layout, meters and density are Backline's own, mapped onto the Omarchy theme, so it changes with every theme switch, live. This document is mainly about it.

## "Like ours": what public material exists, and the gap

Manu asked for the windows to look like his own apps, RecSync and FX Control. Only public material was used, on 2026-11-11:

| Searched | Result |
|---|---|
| manuaudio.com: every page in its sitemap (`/`, `/live-audio-engineer/`, `/broadcast-audio-engineer/`, `/mixing-mastering/`, `/case-studies/running-sound-for-sir/`, `/open-source/`, `/open-source/macos-cli/`) | Neither app is mentioned. The site's one software page is `macos-cli`. `/recsync/`, `/fx-control/` and `/apps/` return 404. |
| Apple's public search API, iPhone/iPad and Mac, for "RecSync", "FX Control", "Manu Audio", "manuaudio" | Nothing from this developer. "VUMATRIX FX CONTROL" (Pro Video Instruments) and "UAFX Control" (Universal Audio) are other companies' products. |
| github.com/manuaudio (public repositories and the profile README) | Neither name appears in any repository name, description or README. The profile README describes the work in general: "metering, and loudness measurement", "LTC and MTC generation and capture, drift measurement", "Swift · Core Audio · AudioUnits · SwiftUI". |
| Web, press and YouTube, six searches | Nothing; only unrelated products of similar names. |

**The gap.** There is no public screenshot, store page or description of RecSync or FX Control. So nothing here copies their look, and no claim is made that it matches. No private repository or local copy was read. The one public signal is the profile README's subject matter (metering, loudness, timecode, drift). That is reflected in *what* the window shows first (meters, peak readouts, timecode and lock state), not in *how* it looks.

What was built instead is a clean, pro-audio-dense default on Omarchy's tokens. Every place where Manu's own design would change it is marked in the code as a `DESIGN SLOT`, and most of it can be overridden without code (see the last section).

## Tokens: everything is an Omarchy theme colour

The window reads the same files Omarchy's shell reads, `~/.local/state/omarchy/current/theme/colors.toml` (and `shell.toml` for the font), and re-reads them when the theme changes. It follows the theme with no restart, the way Omarchy's own apps (Omacut, Monologue) do. `src/theme.cpp` holds the mapping.

| Token | From the Omarchy theme | Used for |
|---|---|---|
| `background` | `background` (else `color0`) | window, meter tracks, readout wells |
| `foreground` | `foreground` (else `color7`) | text, fader caps, peak-hold ticks |
| `accent` | `accent` (else `color4`), as the shell picks it | the cursor (selected strip and control), instrument captions, the PLAYBACK state |
| `urgent` | `red` (else `color1`), the shell's `urgent` | recording, armed, the hot meter zone, clip lamps, failures |
| `ok` | `green` (else `color2`) | the meter's working zone, service lamps, timecode lock |
| `warn` | `yellow` (else `color3`) | the meter's −12 to −3 dB zone, mute, chasing without lock |
| `surface`, `raised`, `line`, `dim` | foreground over background at 4%, 8%, 16% and 55%: the alphas of Omarchy's `Style` fills, blended to opaque | strip bodies, the selected strip, hairlines, secondary text |
| font | `shell.toml [font] family`, else `fc-match monospace` (how the shell resolves it) | everything |
| radius | Hyprland's `decoration:rounding` via `hyprctl`, capped at 3–4 px for small parts | strips, buttons, readouts |

Omarchy's shell palette has no green or yellow role, so **the panels** use the shell's own roles for status: `urgent` for fail, recording and armed; `accent` for warn and lock; dimmed foreground for pass. **The window** adds the theme's green and yellow, because a meter without them reads wrong to anyone who has used a desk.

## Layout

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ● REC 00:03:12     01:00:12:04        TEMPO  METER  SYNC          BACKLINE  │  transport strip, 88 px
│   take-…           1801|4|1868        120.0  4/4    ● LTC LOCKED  48 kHz ·  │
├──────────────────────────────────────────────────────────┬───────────────────┤
│ IN    │ IN    │ INST  │ INST  │ OUT   │ OUT              │ TAKES             │
│ name  │ name  │ name  │ name  │ name  │ name             │ take · 3:12 · 2 tr│
│[ARM]  │[ARM]  │[ARM]  │[ARM]  │[ARM]  │[ARM]             │ …                 │
│-6.1 0.0                                                  │ SOUNDCHECK        │
│ ▌▌ 0  ═╪═                                                │ input [LIVE][PLAY]│
│ ▌▌ 6   │      channel strips, 96 px each,                │                   │
│ ▌▌ 12  │      inputs │ instruments │ outputs             │                   │
│ ▌▌ 20  │      grouped by a hairline                      │                   │
│[MUTE]                                                    │      330 px       │
├──────────────────────────────────────────────────────────┴───────────────────┤
│ h/l strip · j/k fader · a arm · Space record · …   ● HOST ● REC ● CLOCK ● PW │  key line, 26 px
└──────────────────────────────────────────────────────────────────────────────┘
```

- **The transport strip** reads left to right in order of urgency:
  - the record block (state, elapsed time and take; it turns `urgent` when recording);
  - the timecode, the largest text in the window, with bars|beats|ticks under it;
  - tempo, meter and sync source with the lock lamp;
  - on the right, the wordmark, the graph rate and the theme's name.
- **Channel strips** run in one horizontal row: inputs, then Backline instruments, then outputs. A hairline and the kind caption (`IN`, `INST`, `OUT`) mark each group. Top to bottom, a strip is:
  - kind and name (two lines);
  - **arm**;
  - peak-hold and fader readouts, side by side;
  - the meter, its scale, and the fader;
  - **mute**.

  An instrument has no volume of its own, so in place of a fader it shows its MIDI input and a MIDI activity lamp.
- **The side column** lists takes, newest first, with length, tracks, markers and rate. While a take is loaded for soundcheck, it shows each input with **LIVE** and **PLAYBACK**.
- **The key line** lists the keys, the service lamps (host, recorder, clock, PipeWire) and the meter tick's own cost. It turns `urgent` to show the last error in words.

## Meters

| Property | Value | Why |
|---|---|---|
| Orientation, channels | vertical, two bars per strip (a mono source fills both) | the desk convention |
| Scale (`meterPosition`) | 0 dBFS at the top, −20 at half, −40 at a quarter, −60 at the bottom, linear between | the working range gets most of the height |
| Zones | `ok` below −12 dBFS, `warn` from −12 to −3, `urgent` above −3 | a digital meter's "hot" is the last few dB |
| Ballistics | instant attack, 24 dB/s release | it reads peaks and settles in about 2 s |
| Peak hold | 1.5 s, then falls at 24 dB/s; a 2 px line (`urgent` above −3 dB) | |
| Clip | a lamp above each bar, latched at −0.1 dBFS until clicked or `c` | a clip is seen even when it happens off-screen |
| Readout | peak hold in dB, to 0.1, moving in 0.5 dB steps | text relayout stays rare |
| Rate | 30 Hz, only while the window is visible | |

**How they're drawn.** Each strip's meter is one scene-graph geometry node with per-vertex colour: one draw call, 72 vertices. One timer drives every meter. A meter that hasn't moved isn't redrawn. The data comes from the mixer's own PipeWire taps, passive capture streams that never keep a device awake. Each tap's data thread does one pass per buffer and two atomic stores. Measured cost: [`ui/tests/measure-report.txt`](tests/measure-report.txt).

## Faders

- **Law:** +6 dB at the top, 0 dB at 85%, −20 dB at half, −60 dB at the bottom.
- **Look:** a thin track, a mark at 0 dB, and a flat cap with a centre line.
- **Control:** a fader moves the node's own volume (WirePlumber's, through `wpctl`, cubic). So what the mixer shows is what the desktop's volume shows. Moving it:
  - **Keys:** `j`/`k` or `[`/`]` move it 1 dB; `0` sets 0 dB.
  - **Mouse:** a drag, the wheel (1 dB), or a double-click (0 dB).
- **The cursor:** a 2 px accent ring around the focused control.

## Typography and density

- **Font:** one family, the theme's (JetBrains Mono on Omarchy's default).
- **Labels:** captions are upper-case at 9–10 px, with 1–1.4 px letter spacing.
- **Names:** 11 px bold.
- **Numbers:** tabular figures (`tnum`), so readouts don't jitter. The timecode is 38 px bold; elapsed time, tempo and meter are 22 px.
- **Density:** 96 px per strip, so 14 strips fit in 1680 px beside the takes. The gap between strips is 2 px.

## Status semantics

One token per state, in the panels and the window alike (`statusTone` in the panels' model):

| State | Panels | Window |
|---|---|---|
| armed | `urgent` ring glyph | `urgent` outline, "● ARMED" |
| recording | `urgent` filled glyph, red bar icon | `urgent` filled arm button and record lamp |
| instrument running | dim caption | none (the strip is there) |
| restarting / starting | `accent` | `warn` caption |
| failed / grade fail | `urgent` | `urgent` |
| grade warn | `accent` | n/a |
| timecode locked | "LOCKED · n FR" in `accent`, lock glyph in the bar | `ok` lamp, "LTC LOCKED" |
| chasing without lock | `urgent`, open-lock glyph in the bar | `warn` lamp |
| soundcheck playback | selected PLAYBACK pill | `accent` pill and take |

## Motion

None, except what a meter must do. There are no animation loops, nothing pulses, and nothing bounces (Omarchy's `motion.md` rule). The recording state is a solid colour, not a blink.

## Where Manu's design slots in

Without code, `~/.config/backline/design.json` overrides any token, the font and the radius. It's watched, like the theme:

```json
{
  "colors": { "ok": "#00e08a", "warn": "#ffc24a", "urgent": "#ff4d5e", "accent": "#7aa2f7" },
  "fontFamily": "Inter",
  "radius": 4
}
```

With code, each place is marked `DESIGN SLOT` in `ui/src/qml`:

| Slot | File | What would go there |
|---|---|---|
| wordmark | `TopBar.qml` | the product wordmark in place of the BACKLINE text |
| record block | `TopBar.qml` | RecSync's record control and take/elapsed layout, if it differs |
| meter | `ChannelStrip.qml`, `meter.h` | colours, zones (`kWarnDb`, `kHotDb`) and ballistics (`kReleaseDbPerS`, `kHoldS`), if FX Control's meters use others |
| fader cap | `ChannelStrip.qml` | FX Control's fader look |
| app icon | `pkgbuild/` | `backline-mixer.svg` (none ships yet; the launcher falls back to a generic icon) |

To bring the look of RecSync or FX Control in, drop public screenshots (or the apps' own design tokens) next to this file. The table above says where each piece maps.
