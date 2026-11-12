/* Backline: which MIDI sources an instrument follows.
 * SPDX-License-Identifier: MIT
 *
 * "Follow every keyboard" (and any --midi-follow match) links MIDI
 * controllers only: keyboards, pads and control surfaces, as PipeWire shows
 * them, on a MIDI device node (media.class Midi/..., the ALSA sequencer
 * bridge, BLE MIDI, UMP devices). Never Midi Through (it echoes whatever
 * else is sent to it), never one of Backline's own nodes (the clock's MTC
 * and MIDI clock outputs, another instrument), and never an application's
 * MIDI output (a JACK or PipeWire client with no Midi/ class): those carry
 * timecode, clock or a sequence, and are linked on purpose, not followed.
 */
#ifndef BACKLINE_MIDI_H
#define BACKLINE_MIDI_H

#include <stdbool.h>
#include <string.h>

static inline bool bl_starts_with(const char *s, const char *prefix)
{
	return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

/* media_class and node_name are the owning node's; alias is port.alias (the
 * ALSA sequencer client:port name on the bridge). NULL counts as "". */
static inline bool bl_midi_is_controller(const char *media_class, const char *node_name,
		const char *port_name, const char *alias)
{
	if (!bl_starts_with(media_class, "Midi/"))
		return false;
	if (bl_starts_with(node_name, "backline") || bl_starts_with(alias, "backline.") ||
	    bl_starts_with(alias, "backline:"))
		return false;
	if ((port_name && strstr(port_name, "Midi Through")) || (alias && strstr(alias, "Midi Through")))
		return false;
	return true;
}

#endif
