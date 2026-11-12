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

enum ctl_src_kind { CTL_SRC_CC, CTL_SRC_NOTE, CTL_SRC_PITCHBEND, CTL_SRC_PROGRAM, CTL_SRC_OSC,
		   CTL_SRC_CC14, CTL_SRC_NRPN, CTL_SRC_RPN };

struct ctl_source {
	enum ctl_src_kind kind;
	int channel;          /* 1-16; 0 = any channel */
	int number;           /* CC number (cc14: the MSB's, 0-31), note key, NRPN/RPN parameter 0-16383; -1 = any (program) */
	char address[128];    /* OSC */
};

enum ctl_tgt_kind {
	CTL_T_INSTANCE,       /* instance:NODE/PARAM */
	CTL_T_TRANSPORT,      /* transport:play|stop|toggle|locate:POS|tempo */
	CTL_T_RECORDER,       /* recorder:arm|record|stop|marker */
	CTL_T_VOLUME,         /* volume:NODE (dB) */
	CTL_T_MUTE,           /* mute:NODE */
	CTL_T_CONTROL,        /* control:NODE/NAME (a filter-chain / clap-pipewire control) */
	CTL_T_PARAM,          /* param:PATH (a Params1 parameter; key holds the path) */
};

struct ctl_target {
	enum ctl_tgt_kind kind;
	char node[256];       /* instance, volume, mute, control */
	char key[256];        /* instance parameter key, control name */
	char action[32];      /* transport, recorder */
	char arg[128];        /* transport:locate:POS */
};

/* "midi:cc:CH:NUM", "midi:cc14:CH:MSB", "midi:nrpn:CH:PARAM", "midi:rpn:CH:PARAM",
 * "midi:note:CH:KEY", "midi:pitchbend:CH", "midi:program:CH", "osc:/ADDRESS".
 * CH 1-16 or "*". 0, or -1 with a sentence in err. */
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
/* A linear gain to dB (0 is -inf). */
double ctl_gain_to_db(double gain);

/* ---- Control1 additions: relative encoders, 14-bit values, NRPN/RPN,
 * soft takeover, feedback (docs/api/Control1.md). */
enum ctl_encoding { CTL_ENC_ABS, CTL_ENC_REL_2C, CTL_ENC_REL_SM, CTL_ENC_REL_OFFSET, CTL_ENC_REL_INCDEC };
enum ctl_takeover_mode { CTL_TO_JUMP, CTL_TO_PICKUP, CTL_TO_SCALE };
int ctl_parse_encoding(const char *s, enum ctl_encoding *out);
const char *ctl_encoding_name(enum ctl_encoding e);
int ctl_parse_takeover(const char *s, enum ctl_takeover_mode *out);
const char *ctl_takeover_name(enum ctl_takeover_mode t);

/* Signed ticks of a relative encoder's 7-bit value (0: no movement).
 *  rel-2c     two's complement: 1..63 = +1..+63, 127..64 = -1..-64
 *  rel-sm     sign-magnitude, bit 6 negative (MCU V-Pots): 1..63 = +n, 65..127 = -(v-64)
 *  rel-offset binary offset: 64+n = +n, 64-n = -n */
int ctl_rel_ticks(enum ctl_encoding e, int v7);
/* A relative move: the new normalised value from cur and ticks. step is
 * the change per tick (0..1); accel >= 1 makes faster turns go further:
 * |ticks|^accel. Clamped to 0..1. */
double ctl_rel_apply(double cur, int ticks, double step, double accel);

/* 14-bit pairs (CC n with n+32, NRPN data entry 6 with 38). A device that has
 * never sent an LSB is treated as 7-bit: its MSB is used at once. After the
 * first LSB, an MSB waits up to the timeout for its LSB. */
struct ctl_hires {
	int msb, lsb;
	bool have_msb, pending, seen_lsb;
	uint64_t msb_ns;
};
#define CTL_HIRES_TIMEOUT_NS (20ull * 1000000ull)
/* part 0 = MSB, 1 = LSB. true with *norm when a value is complete. */
bool ctl_hires_feed(struct ctl_hires *h, int part, int v7, uint64_t now_ns, double *norm);
/* An MSB still waiting after the timeout: true with its value. */
bool ctl_hires_expire(struct ctl_hires *h, uint64_t now_ns, double *norm);

/* NRPN/RPN parameter selection and data entry, per channel (0-15). */
struct ctl_nrpn {
	int nrpn_msb[16], nrpn_lsb[16], rpn_msb[16], rpn_lsb[16];
	int sel[16];           /* 0 none, 1 NRPN, 2 RPN */
};
enum ctl_nrpn_ev { CTL_NRPN_NONE, CTL_NRPN_DATA_MSB, CTL_NRPN_DATA_LSB, CTL_NRPN_INC, CTL_NRPN_DEC };
void ctl_nrpn_init(struct ctl_nrpn *s);
/* A CC on channel ch. For data events: *rpn, *param and *value (the 7-bit
 * data byte, or the inc/dec amount, at least 1). */
enum ctl_nrpn_ev ctl_nrpn_feed(struct ctl_nrpn *s, int ch, int cc, int val, bool *rpn, int *param, int *value);

/* Soft takeover for controls without a motor. pickup: ignore the control
 * until it reaches or crosses the target's value; scale: move the target
 * proportionally until both meet; jump: always follow. After a pickup the
 * control follows until the target is changed from elsewhere (call
 * ctl_takeover_lost). Returns true with *out when the target should be set. */
struct ctl_takeover {
	bool picked, have_prev;
	double prev;
};
#define CTL_TAKEOVER_TOLERANCE 0.02
bool ctl_takeover_apply(enum ctl_takeover_mode m, struct ctl_takeover *t, double control, bool target_known,
			double target, double *out);
void ctl_takeover_lost(struct ctl_takeover *t);

/* The MIDI bytes that show norm on the control the source names (feedback
 * to LED rings, button LEDs, motor faders). bits: 7 or 14 (NRPN data). A
 * relative source sends the absolute value (what LED-ring devices take).
 * Returns the byte count (up to 12), 0 for OSC. */
size_t ctl_feedback_midi(const struct ctl_source *s, double norm, int bits, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
