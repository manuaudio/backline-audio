/* Mackie Control (MCU) protocol: encode and decode, pure. Written from the
 * Logic Control MIDI implementation (Logic 7 manual, appendix B-D) and the
 * console spec's reference tables (A.3); Ardour's GPL surface code was read
 * for facts only, nothing copied. Every message lists where it comes from.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_MCU_H
#define BACKLINE_MCU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SysEx header F0 00 00 66 <model>: Mackie's manufacturer id. */
#define MCU_MODEL_LC 0x10    /* Logic Control */
#define MCU_MODEL_LCXT 0x11
#define MCU_MODEL_MCU 0x14   /* Mackie Control (and X-Touch in MC mode) */
#define MCU_MODEL_XT 0x15    /* extender */

/* Note numbers (A.3.3) */
#define MCU_REC 0x00
#define MCU_SOLO 0x08
#define MCU_MUTE 0x10
#define MCU_SELECT 0x18
#define MCU_VPOT 0x20        /* push */
#define MCU_TOUCH 0x68       /* fader touch 1-8; 0x70 master */
#define MCU_TOUCH_MASTER 0x70
#define MCU_NOTES 128

#define MCU_LCD_CELLS 112    /* 2 lines of 56 */
#define MCU_TC_DIGITS 10

struct mcu_control {
	const char *name;
	uint8_t id;
	const char *label;     /* the MCU's printed label */
};
extern const struct mcu_control mcu_controls[];
extern const size_t mcu_n_controls;
int mcu_control_id(const char *name);       /* -1 if unknown */
const char *mcu_control_name(int id);       /* NULL if none */
bool mcu_is_strip_button(int id);           /* rec/solo/mute/select/vpot/touch */

/* ---- host to device. Each returns the bytes written (0 if no room). */
size_t mcu_sysex(uint8_t model, const uint8_t *body, size_t n, uint8_t *out, size_t cap);
size_t mcu_device_query(uint8_t model, uint8_t *out, size_t cap);            /* Hdr 00 F7 */
size_t mcu_lc_reply(uint8_t model, const uint8_t serial[7], const uint8_t challenge[4], uint8_t *out, size_t cap);
size_t mcu_go_offline(uint8_t model, uint8_t *out, size_t cap);              /* Hdr 0F 7F F7 */
/* LCD: chars at cell offset (0-111): Hdr 12 off chars F7. */
size_t mcu_lcd(uint8_t model, int offset, const char *chars, size_t n, uint8_t *out, size_t cap);
/* X-Touch scribble colours, 8 strips: Hdr 72 c0..c7 F7. */
size_t mcu_colours(uint8_t model, const uint8_t colours[8], uint8_t *out, size_t cap);
/* Meter mode per strip: Hdr 20 strip mode F7 (bit 0 signal LED, 1 peak hold, 2 LCD meter). */
size_t mcu_meter_mode(uint8_t model, int strip, uint8_t mode, uint8_t *out, size_t cap);
size_t mcu_fader(int fader, int v14, uint8_t out[3]);                       /* Ei ll hh, master i = 8 */
size_t mcu_led(int id, int state, uint8_t out[3]);                           /* 90 id 00/01/7F: 0 off, 1 flash, 2 on */
size_t mcu_ring(int pot, uint8_t ring, uint8_t out[3]);                      /* B0 30+i XX */
size_t mcu_meter(int strip, int level, uint8_t out[2]);                      /* D0 (strip<<4 | level) */
size_t mcu_digit(int cc, uint8_t seg, uint8_t out[3]);                       /* B0 cc segment-char */

enum mcu_ring_mode { MCU_RING_DOT, MCU_RING_BOOST_CUT, MCU_RING_WRAP, MCU_RING_SPREAD };
/* The ring byte 0 c mm vvvv for a normalised value (pos 0 = off). */
uint8_t mcu_ring_byte(enum mcu_ring_mode mode, double norm, bool centre, bool off);
int mcu_parse_ring_mode(const char *s);                                      /* -1 if unknown */
/* 7-segment character: ASCII 0x40-0x5F minus 0x40, 0x20-0x3F as is, +0x40 a dot. */
uint8_t mcu_7seg(char c, bool dot);
/* dBFS to the meter level 0-12 (0x0C); (unverified per device: bench-test). */
int mcu_meter_level(float db);
/* Nearest of the X-Touch's 8 scribble colours: 0 off, 1 red, 2 green, 3 yellow,
 * 4 blue, 5 magenta, 6 cyan, 7 white (bit R=1, G=2, B=4). */
uint8_t mcu_colour_nearest(int r, int g, int b);
int mcu_parse_colour(const char *hex, int *r, int *g, int *b);               /* "#rrggbb": 0 or -1 */
/* The Logic Control challenge response (A.3.1). */
void mcu_lc_response(const uint8_t c[4], uint8_t r[4]);
/* A display name in width characters: spaces go first, then vowels from the
 * end (never the first letter), then it is cut. */
void mcu_abbrev(const char *in, char *out, int width);

/* ---- device to host */
enum mcu_in_kind {
	MCU_IN_NONE,
	MCU_IN_BUTTON,      /* id, down */
	MCU_IN_TOUCH,       /* index 0-7, 8 = master; down */
	MCU_IN_FADER,       /* index 0-8; value14 */
	MCU_IN_VPOT,        /* index 0-7; delta */
	MCU_IN_JOG,         /* delta */
	MCU_IN_PEDAL,       /* value (0-127) */
	MCU_IN_HOST_QUERY,  /* SysEx 01: serial, challenge */
	MCU_IN_CONFIRM,     /* SysEx 03: serial */
	MCU_IN_ERROR,       /* SysEx 04 */
	MCU_IN_VERSION,     /* SysEx 14 */
	MCU_IN_READY,       /* SysEx 06 (X-Touch Compact, per Ardour) */
	MCU_IN_OTHER,
};

struct mcu_in {
	enum mcu_in_kind kind;
	int id, index, value, delta;
	bool down;
	uint8_t model;
	uint8_t serial[7];
	uint8_t challenge[4];
};

bool mcu_decode(const uint8_t *msg, size_t n, struct mcu_in *out);
/* The serial as text: printable ASCII as is, otherwise hex. */
void mcu_serial_text(const uint8_t serial[7], char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
