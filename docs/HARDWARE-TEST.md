# Control surfaces on real hardware: the bench checklist

Everything in the surface layer has been tested against software devices
only (`tests/surfaced-proof`, `tests/surfaced-net-proof`,
`tests/unit/test_surface.c`, `tests/unit/test_rtpmidi.c`). This list is
what to check, and write down, the first time a real device is plugged in:
by USB first, then on the network (below).
In ctest the hardware pass shows as **skipped** (`surface_real_hardware`)
until it is done.

## Set up (about 5 minutes)

1. Build this tree. Put the device in **Mackie Control** mode (X-Touch: "MC"
   mode in its power-on setup; the X-Touch Extender likewise; MCU Pro: Mackie
   mode, not Logic Control or HUI). Plug it in by USB.
2. Run the new services in place of any installed ones, and a stand-in
   mixer that logs what it is asked:

   ```
   build/control/backline-controld --replace --idle-exit 0 --verbose &
   build/surface/backline-surfaced --replace --verbose 2>/tmp/surfaced.log &
   build/surface/backline-test-mixer --views --moving-meters --log /tmp/mixer.jsonl &
   tail -f /tmp/surfaced.log /tmp/mixer.jsonl
   ```
3. `busctl --user call io.github.manuaudio.Backline.Surface /io/github/manuaudio/Backline/Surface io.github.manuaudio.Backline.Surface1 Units`

## Check and note

| # | Check | Write down |
|---|---|---|
| 1 | The device is found: `found …` in the log, the unit `online` | The port's `node:port \| alias` (`pw-link -lio`), USB VID:PID (`lsusb`); fix `match` in the profile if needed |
| 2 | Handshake: the unit id ends in the device's serial, not its port | Whether it answered the device query; how long it took |
| 3 | The greeting `Backline all`, then channel names on both LCD lines | Any missing text (long SysEx through PipeWire's MIDI bridge) |
| 4 | X-Touch: scribble colours match the mixer's (`Kick In` red, …) | Wrong colours |
| 5 | Faders: each moves to its channel's level when the view loads (motors) | Resolution: move one slowly and look at the values in the log |
| 6 | Touch a fader, move it: `gesture-begin`, sets, `gesture-end` in the mixer log; the fader stays where you left it | Any snap-back or fighting; touch sensitivity |
| 7 | Move a fader without touching it (with a pen): nothing is set | |
| 8 | V-Pots: right is up; the ring follows; push resets pan | Direction, the ring modes' look |
| 9 | Meters move (`--moving-meters`); in.1 and in.2's clip LEDs | How 0 dB and clip look; the decay |
| 10 | Every button: press each one, see its `action-down` name (or its effect) | Each button's id and its printed label, for the profile's `labels` |
| 11 | LEDs: Mute, Solo, Rec, Select, Flip, the view row | Any that don't light or flash (set `no-flash`) |
| 12 | The assignment display shows `IN`, and `I2` for a second after Bank ► | If the digits are swapped, set `assignment-swapped` |
| 13 | Bank ◄/►, Channel ◄/►, Shift+Bank: the names move by 8, 1, to the ends | |
| 14 | With an Extender: one 16-wide group, the extender on its side | Which side; set the order with `SetGroup` if needed |
| 15 | Unplug the USB cable while holding a fader: `gesture-end` within 50 ms (compare the log times); plug back: the same place, redrawn | The two times |
| 16 | Bank quickly back and forth for 10 s: no lost or stale text | If any, lower `bytes-per-second` |
| 17 | Stop surfaced (Ctrl-C): the device goes dark, faders down | |

Targets from the spec: unplug while touched → gesture end within 50 ms; a
surface move → the parameter within 10 ms; a full LCD redraw on a DIN-paced
profile within 250 ms (the simulator measures 190 ms).

**Generic MIDI controllers** (Control1's 14-bit CC, NRPN, relative
encoders, takeover, feedback): map a few controls with `backline map add
… param:/mix/in.1/fader --feedback` (see Control1.md), and note which
`encoding` its encoders send and whether its LEDs or motors follow.

## On the network (X-Touch, X-Touch Extender)

What the network adds is only the transport, so a USB pass first saves
time. Then:

### Set up (about 10 minutes)

1. Cable the unit's **REMOTE** port to the same switch as the computer (or
   straight to it). Note the computer's address: `ip -4 addr`.
2. On the unit: hold **Channel 1 SELECT** while switching it on.
   - **Encoder 1**: **MC**.
   - **Encoder 2**: **Network**.
   - **Encoder 3**: **DHCP** on (or set the IP, mask and gateway with
     encoders 4–7).
   - **Encoder 3** again: **MODE**, set to **RTP SLAVE** first.
   - Press **Channel 1 SELECT** to leave.
   - Write down the unit's IP, which the display shows after `Wait…`.
3. Run the services as in the USB setup above. surfaced's log shows
   `network UDP 5004` at the start. SLAVE mode needs no firewall change.
   MASTER mode (N6 onwards) needs UDP 5004–5005 let in. Omarchy's ufw
   denies incoming, so: `sudo ufw allow 5004:5005/udp`; remove the rule
   afterwards with `sudo ufw delete allow 5004:5005/udp`.

### Check and note

| # | Check | Write down |
|---|---|---|
| N1 | `avahi-browse -rt _apple-midi._udp` | **Whether the unit announces itself on mDNS**, and under which name |
| N2 | If N1 found it: the log says `found … (mDNS)`, then `online`, with nothing to do. If not: `backline surfaces add UNIT-IP`, then `online`; the display's "Wait" goes away | The **session name** the unit gives (`backline surfaces`, or the log). If the log says `no surface profile matches this name`, put that name in the profile's `match` |
| N3 | `backline surfaces` lists it: `network IP:5004`, `online` | The unit id (does it end in a serial?) |
| N4 | Checks 3–13 of the USB table, over the network | Anything that differs from USB (missing text: long SysEx in one packet) |
| N5 | Pull the network cable while holding a fader: `gesture-end` in the mixer log within ~3 s; `backline surfaces` says `gone`. Plug it back: online within ~1 s, same place, redrawn | The two times. Does the display say "Wait" while unplugged? |
| N6 | **RTP MASTER** with this computer's IP (and the ufw rule): `X-Touch (IP:PORT) invites us`, then `online` (first `backline surfaces remove UNIT-IP`) | The session name it calls with |
| N7 | In MASTER mode, the cable pull of N5, and the same with **no** fader held, for 60 s, then back | Does the unit call again by itself? |
| N8 | Clock syncs both ways: in MASTER mode, `busctl --user call io.github.manuaudio.Backline.Surface /io/github/manuaudio/Backline/Surface io.github.manuaudio.Backline.Surface1 Peers` shows `latency-ms` > 0 | Whether the unit answers surfaced's probes (if not, a drop is only noticed after 30 s: note it) |
| N9 | **Two sessions** (RTP SLAVE): a second computer running `backline-surfaced --standby` and `backline surfaces add UNIT-IP` | Does the unit take a second session (`session held (standby)`) or refuse it (`refused`)? Does it send to both? |
| N10 | With two: stop the main engine; `backline surfaces activate` on the standby | How long until the standby drives it |
| N11 | The Extender the same way (N1–N5) and next to an X-Touch: one 16-wide group | Its session name |

## When done

Put what you found into the profile (`match`, `labels`, `quirks`,
`bytes-per-second`), set `"verified": true`, and note the device, firmware
and date in the profile's `comment`. The network answers (mDNS, the session
name, how many sessions) go into [SURFACES.md](SURFACES.md), in place of its
"not confirmed" list.
