/* Control mappings: a source (a MIDI message kind or an OSC address) to a
 * target (a parameter, an action, a volume), with scaling. Pure, unit-tested.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_CTL_MAPPING_H
#define BACKLINE_CTL_MAPPING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ctl_src_kind { CTL_SRC_CC, CTL_SRC_NOTE, CTL_SRC_PITCHBEND, CTL_SRC_PROGRAM, CTL_SRC_OSC };

struct ctl_source {
	enum ctl_src_kind kind;
	int channel;          /* 1-16; 0 = any channel */
	int number;           /* CC number, note key; -1 = any (program) */
	char address[128];    /* OSC */
};

enum ctl_tgt_kind {
	CTL_T_INSTANCE,       /* instance:NODE/PARAM */
	CTL_T_TRANSPORT,      /* transport:play|stop|toggle|locate:POS|tempo */
	CTL_T_RECORDER,       /* recorder:arm|record|stop|marker */
	CTL_T_VOLUME,         /* volume:NODE (dB) */
	CTL_T_MUTE,           /* mute:NODE */
	CTL_T_CONTROL,        /* control:NODE/NAME (a filter-chain / clap-pipewire control) */
};

struct ctl_target {
	enum ctl_tgt_kind kind;
	char node[256];       /* instance, volume, mute, control */
	char key[256];        /* instance parameter key, control name */
	char action[32];      /* transport, recorder */
	char arg[128];        /* transport:locate:POS */
};

/* "midi:cc:CH:NUM", "midi:note:CH:KEY", "midi:pitchbend:CH", "midi:program:CH",
 * "osc:/ADDRESS". CH 1-16 or "*". 0, or -1 with a sentence in err. */
int ctl_parse_source(const char *s, struct ctl_source *out, char *err, size_t err_size);
void ctl_format_source(const struct ctl_source *s, char *out, size_t n);
int ctl_parse_target(const char *s, struct ctl_target *out, char *err, size_t err_size);

/* Triggers: the target acts on a press (value crossing above 0.5), not on a
 * value. */
bool ctl_target_is_trigger(const struct ctl_target *t);
/* The default output range of a target that has one (volume dB, tempo BPM,
 * control 0-1). false: the range comes from elsewhere (an instance param). */
bool ctl_target_default_range(const struct ctl_target *t, double *min, double *max);

/* A MIDI 1.0 message to a source and a value 0..1 (note-off and note-on
 * with velocity 0 are 0). false: not a kind controld maps. */
bool ctl_midi_event(const uint8_t *msg, size_t size, struct ctl_source *src, double *value);
bool ctl_source_matches(const struct ctl_source *mapping, const struct ctl_source *event);

/* Scaling: v (0..1, clamped) -> invert -> min + v (max - min). */
double ctl_scale(double v, bool invert, double min, double max);
/* OSC input range in_min..in_max to 0..1 (clamped). */
double ctl_normalize(double x, double in_min, double in_max);

/* dB to a linear gain; at or below -120 dB (or -inf) it is 0. */
double ctl_db_to_gain(double db);

#ifdef __cplusplus
}
#endif
#endif
