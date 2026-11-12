# Control surfaces: what works, and how to connect it

Backline drives **Mackie Control (MCU)** surfaces with `backline-surfaced`.
It needs no vendor software and no bridge. A surface either sends class-compliant
USB MIDI, or speaks RTP-MIDI on its own Ethernet port. HUI, the Waves FIT's
native mode and EuCon are not supported. `backline surfaces` lists what is
connected.

Everything here has been tested against software devices only
(`tests/surfaced-proof`, `tests/surfaced-net-proof`). A bench pass on real
hardware comes first: [HARDWARE-TEST.md](HARDWARE-TEST.md).

## Support

| Device | Works natively over USB | Works natively over the network | Notes |
|---|---|---|---|
| **Behringer X-Touch** | **Yes**: class compliant, MC mode | **Yes**: RTP-MIDI on its REMOTE (Ethernet) port, MC mode, Interface "Network" | Profile `behringer-xtouch`. Network setup below |
| **Behringer X-Touch Extender** | **Yes**: class compliant, MC mode | **Yes**: RTP-MIDI, as the X-Touch | Profile `behringer-xtouch-ext`. Each unit has its own IP and session |
| **Mackie MCU Pro** | **Yes**, Mackie mode | No: USB and DIN only | Profile `mackie-mcu-pro` |
| **Mackie MCU XT Pro** | **Yes** | No | Profile `mackie-mcu-xt` |
| X-Touch Compact, X-Touch Mini, X-Touch One | USB MIDI (MC mode); no profile shipped yet | No | |
| iCON Platform M+/X+, PreSonus FaderPort 8/16 (MCU mode) | USB MIDI; no profile shipped yet | No | |
| Behringer X-Touch in **Xctl** mode | Not supported | Not supported | Behringer's own UDP protocol for X Air mixers, not MCU. Use MC mode |
| SSL UF8/UF1/UC1 | **Not supported** | Not supported | Needs SSL 360 (Mac/Win) for its MIDI ports |
| Softube Console 1 Fader | Not supported (needs Softube software) | Not supported | |
| Avid S1/S3/S4/S6, Dock | Not supported | Not supported | EuCon, closed |

"Natively" means no vendor or helper software between the device and
Backline: the in-kernel USB MIDI driver and PipeWire for USB, and plain
RTP-MIDI (AppleMIDI) for the network, which surfaced speaks itself.

The two network devices answer the three questions as follows (sources
below):

- **Does the X-Touch's Ethernet port speak RTP-MIDI natively?** Yes. Behringer's
  guide: "REMOTE – this port is an Ethernet interface for connection to a DAW
  supporting RTP MIDI", and "For OS X systems, no external driver is
  required for RTP MIDI operation". The Mac's built-in Network MIDI talks to
  it directly, and so does surfaced.
- **Which mode supports MCU over Ethernet?** Set emulation mode **MC**, then
  Interface **Network**. The X-Touch offers HUI or MC, each on USB, MIDI or Network.
  **Xctl** (firmware 1.15 and later) is a different protocol, for Behringer's
  X Air mixers. In its combined modes (Xctl+MCU), Mackie Control runs on
  USB, not on the network.
- **What is the setup on the device?** The steps are below.

## Network setup on the X-Touch (and the Extender)

1. Hold **Channel 1 SELECT** while switching the unit on (about 2 s): the
   configuration menu.
2. **Encoder 1**: the mode, set to **MC**.
3. **Encoder 2**: the interface, set to **Network**.
4. **Encoder 3** chooses between **MODE** and **DHCP**:
   - **DHCP** on (the default): the unit takes an address from the network.
     When it is off, enter "my IP", "my Mask" and "my Gway" with encoders 4–7.
   - **MODE**: **RTP MASTER** or **RTP SLAVE**, below.
5. Press **Channel 1 SELECT** again to leave. The display shows `IP: …
   Wait…`, then the unit's address. "Wait" goes away once a computer is
   connected.

Which MODE to pick:

| | RTP SLAVE (recommended) | RTP MASTER |
|---|---|---|
| Who connects | The computer calls the X-Touch | The X-Touch calls the computer |
| On the X-Touch | Nothing more | One more tick right on encoder 3, then this computer's IP with encoders 4–7 |
| In Backline | Found by itself if the unit announces itself on mDNS (`_apple-midi._udp`; Behringer doesn't say whether it does). Otherwise once, with the address on its display: `backline surfaces add 192.168.1.50` (kept across restarts) | Nothing: surfaced listens on UDP 5004 and accepts the call |
| Firewall | Nothing to open: surfaced makes the call, and the replies are its own traffic | UDP 5004–5005 must be let in. Omarchy denies all incoming by default, so this takes `sudo ufw allow 5004:5005/udp` |
| Two engines (main + standby) | Yes, if the unit takes two sessions (not documented; see below) | No: the unit calls one address |

With `add`, give the unit a fixed address: either DHCP off on the unit, or a
reservation on the router. The port is **5004**, the standard RTP-MIDI port
("You may use Port 5004 in both cases"). Data goes on 5005, next to it. surfaced only connects to
devices whose name matches a surface profile (`X-Touch`, `X-Touch-Ext`, …).
That is the same trust as a USB port. Anything on this computer's own networks that calls itself
"X-Touch" and reaches UDP 5004 is driven as a surface (calls from routed,
further networks are ignored). Keep surfaces
on a network you trust; Omarchy's default firewall already blocks calls in.
A Mac's or another computer's session is ignored. If the X-Touch announces
itself under a name no profile matches, the log says so and gives the name.
That name then goes into the profile's `match`.

## How surfaced does it

- **The same as USB.** A network surface is a unit with the same profile,
  detection by name, handshake, serial, grouping, views, banking, touch
  safety and paced output. Only the bytes travel differently. An X-Touch on
  the network and an Extender on USB make one 16-strip group.
- **Native RTP-MIDI, not PipeWire's module.** PipeWire's AppleMIDI module
  (`libpipewire-module-rtp-session`) is not in Arch's main `pipewire` package
  (it is in `pipewire-zeroconf`). It would have to be configured into the
  user's PipeWire. It also makes one generic MIDI node per session, so
  surfaced would see neither the device's name nor the session's state.
  surfaced's own implementation has no setup, sees every session, and notices
  a drop itself. It is about 1,100 lines in two files: `surface/rtpmidi.c`,
  the protocol, pure and unit-tested; `surface/rtpnet.c`, the sockets and
  mDNS. See [DECISIONS D10](DECISIONS.md).
- **Discovery.** mDNS through the machine's avahi-daemon. surfaced never
  opens port 5353 itself. Without avahi, a device can still invite surfaced
  or be added by address.
- **Drops.** The side that invited sends a clock sync (CK) every second, and
  surfaced probes the same way when the device invited it. When nothing is heard for 3 s, the
  session is lost. A held fader's gesture ends then, so automation never
  writes forever, and the unit shows as `gone`. surfaced invites again every
  second, or a MASTER-mode X-Touch calls again. On the next open session,
  the unit is back in its group and place and fully redrawn. A touch held
  across the drop never applies: the fader must be touched again.
  Measured: the gesture ends 2.8 s after the cable is pulled, and the unit
  is back 1 s after it is plugged in again.
- **Two engines.** Run the standby's surfaced with `--standby`. It opens its
  own sessions to devices that allow a second one and holds them, driving
  nothing. `SetActive(true)` (`backline surfaces activate`) makes it draw
  and drive every unit at once. A device that allows only one session
  refuses the standby (`refused` in `backline surfaces`). The standby asks
  again every 5 s, so when the main engine stops (BY) or its session times
  out, the standby gets the device. Behringer does not document how many
  sessions the X-Touch takes. This is a bench item.

## Sources

- Behringer X-Touch Quick Start Guide (Rev. 1, 2017-02-27), the sections
  "REMOTE", "Configuration menu", "Preparing the X-TOUCH for RTP MIDI
  operation", read from a mirror of Behringer's PDF:
  <https://img.audiomania.ru/data/behringer_x-touch.pdf>. Behringer's own copy
  (V6.0, which may show a newer menu) wasn't reachable at the time:
  <https://cdn.mediavalet.com/aunsw/musictribe/wiU3hm3k2ECzn8GzHCf76A/lSzlI_grYU-ScKqMJCT3rw/Original/QSG_BE_0808-AAD_X-TOUCH_WW.pdf>
- Behringer X-Touch Extender Quick Start Guide (Rev. 11, 2017-03-27):
  <https://img.audiomania.ru/data/behringer_x-touch_extender.pdf>. Emulation
  modes are MC, HUI, Ctrl and CtrlRel. The interface is USB or Network. The
  RTP-MIDI section is the same as the X-Touch's.
- X-Touch firmware 1.15 release notes (the Xctl modes; in the combined
  modes MCU is on USB):
  <https://archive.org/download/behringer-xr18-software/Firmware/X-Touch/X-Touch_Release-Notes_1.15_djvu.txt>.
  Firmware 1.22 notes (no feature changes; do not downgrade):
  <https://archive.org/download/behringer-xr18-software/Firmware/X-Touch/X-Touch_Release-Notes_1.22_djvu.txt>
- X-Touch Compact QSG (USB/MIDI only): <https://imagescdn.juno.co.uk/manual/584397-01U.pdf>.
  X-Touch One QSG (USB only): <https://img.audiomania.ru/data/behringer_x-touch_one.pdf>
- Mackie MCU Pro quick start (USB and DIN):
  <https://images.static-thomann.de/pics/atg/atgdata/document/manual/111996_quickstartmanual.pdf>
- SSL UF8 (USB and SSL 360): <https://www.soundonsound.com/reviews/ssl-uf8>. SSL's support page
  on UF8 compatibility: <https://support.solidstatelogic.com/hc/en-gb/articles/4410043451537-UF8-Compatibility>
- RTP-MIDI: RFC 6295 (the payload), and Apple's session protocol (IN, OK, NO, BY, CK, RS).

**Not confirmed by any primary source** (the bench decides; HARDWARE-TEST.md):

- whether the X-Touch announces itself on mDNS;
- its RTP session name (assumed to contain "X-Touch");
- how many sessions it takes at once;
- whether it answers clock syncs from the side it invited (surfaced falls
  back to a 30 s timeout if not);
- the V6.0 guide's menu.
