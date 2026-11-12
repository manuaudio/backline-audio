/* Which MIDI sources an instrument follows (common/bl_midi.h): controllers
 * only, never Backline's own clock outputs, Midi Through or an app's MIDI. */
#include <stdio.h>
#include "../../common/bl_midi.h"

static int fails;
#define EXPECT(cond, what) do { if (!(cond)) { printf("not ok - %s\n", what); fails++; } else printf("ok - %s\n", what); } while (0)

int main(void)
{
	/* the shapes seen on a real system (pw-dump, PipeWire 1.6) */
	EXPECT(bl_midi_is_controller("Midi/Bridge", "Midi-Bridge", "Arturia KeyLab 49:KeyLab 49 MIDI (capture)", "KeyLab 49:KeyLab 49 MIDI"),
	       "a USB keyboard on the ALSA sequencer bridge is followed");
	EXPECT(bl_midi_is_controller("Midi/Bridge", "bluez_midi.server", "out", "out"), "a BLE MIDI keyboard is followed");
	EXPECT(bl_midi_is_controller("Midi/Bridge", "Midi-Bridge", "backline-test-kbd:keys (capture)", "backline-test-kbd:keys"),
	       "the test keyboard (a sequencer client, like a USB keyboard) is followed");
	EXPECT(bl_midi_is_controller("Midi/Source", "alsa_ump.Keystation", "Keystation (capture)", NULL), "a UMP device node is followed");
	EXPECT(!bl_midi_is_controller(NULL, "backline.clock", "mtc_out", "backline.clock:mtc_out"),
	       "Backline's clock MTC output is not followed");
	EXPECT(!bl_midi_is_controller("Midi/Bridge", "Midi-Bridge", "backline.clock:midi_clock (capture)", "backline.clock:midi_clock"),
	       "Backline's clock through the sequencer bridge is not followed");
	EXPECT(!bl_midi_is_controller("Stream/Output/Audio", "backline.surge-xt", "midi_out", NULL), "another instrument is not followed");
	EXPECT(!bl_midi_is_controller("Midi/Bridge", "Midi-Bridge", "Midi Through: Port-0 (capture)", "Midi Through:Midi Through Port-0"),
	       "Midi Through is not followed");
	EXPECT(!bl_midi_is_controller("", "Ardour", "MIDI Clock out", "ardour:MIDI Clock out"), "an app's MIDI output (JACK client) is not followed");
	EXPECT(!bl_midi_is_controller(NULL, NULL, NULL, NULL), "an unknown node is not followed");
	return fails ? 1 : 0;
}
