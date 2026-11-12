/* Mackie Control protocol (mcu.h).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "mcu.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A.3.3: note numbers on channel 1, with the MCU's printed labels. */
const struct mcu_control mcu_controls[] = {
	{ "rec-1", 0x00, "REC 1" }, { "rec-2", 0x01, "REC 2" }, { "rec-3", 0x02, "REC 3" }, { "rec-4", 0x03, "REC 4" },
	{ "rec-5", 0x04, "REC 5" }, { "rec-6", 0x05, "REC 6" }, { "rec-7", 0x06, "REC 7" }, { "rec-8", 0x07, "REC 8" },
	{ "solo-1", 0x08, "SOLO 1" }, { "solo-2", 0x09, "SOLO 2" }, { "solo-3", 0x0a, "SOLO 3" }, { "solo-4", 0x0b, "SOLO 4" },
	{ "solo-5", 0x0c, "SOLO 5" }, { "solo-6", 0x0d, "SOLO 6" }, { "solo-7", 0x0e, "SOLO 7" }, { "solo-8", 0x0f, "SOLO 8" },
	{ "mute-1", 0x10, "MUTE 1" }, { "mute-2", 0x11, "MUTE 2" }, { "mute-3", 0x12, "MUTE 3" }, { "mute-4", 0x13, "MUTE 4" },
	{ "mute-5", 0x14, "MUTE 5" }, { "mute-6", 0x15, "MUTE 6" }, { "mute-7", 0x16, "MUTE 7" }, { "mute-8", 0x17, "MUTE 8" },
	{ "select-1", 0x18, "SELECT 1" }, { "select-2", 0x19, "SELECT 2" }, { "select-3", 0x1a, "SELECT 3" },
	{ "select-4", 0x1b, "SELECT 4" }, { "select-5", 0x1c, "SELECT 5" }, { "select-6", 0x1d, "SELECT 6" },
	{ "select-7", 0x1e, "SELECT 7" }, { "select-8", 0x1f, "SELECT 8" },
	{ "vpot-1", 0x20, "V-POT 1" }, { "vpot-2", 0x21, "V-POT 2" }, { "vpot-3", 0x22, "V-POT 3" }, { "vpot-4", 0x23, "V-POT 4" },
	{ "vpot-5", 0x24, "V-POT 5" }, { "vpot-6", 0x25, "V-POT 6" }, { "vpot-7", 0x26, "V-POT 7" }, { "vpot-8", 0x27, "V-POT 8" },
	{ "assign-track", 0x28, "TRACK" }, { "assign-send", 0x29, "SEND" }, { "assign-pan", 0x2a, "PAN/SURROUND" },
	{ "assign-plugin", 0x2b, "PLUG-IN" }, { "assign-eq", 0x2c, "EQ" }, { "assign-instrument", 0x2d, "INSTRUMENT" },
	{ "bank-left", 0x2e, "BANK <" }, { "bank-right", 0x2f, "BANK >" },
	{ "channel-left", 0x30, "CHANNEL <" }, { "channel-right", 0x31, "CHANNEL >" },
	{ "flip", 0x32, "FLIP" }, { "global-view", 0x33, "GLOBAL VIEW" }, { "name-value", 0x34, "NAME/VALUE" },
	{ "smpte-beats", 0x35, "SMPTE/BEATS" },
	{ "f1", 0x36, "F1" }, { "f2", 0x37, "F2" }, { "f3", 0x38, "F3" }, { "f4", 0x39, "F4" },
	{ "f5", 0x3a, "F5" }, { "f6", 0x3b, "F6" }, { "f7", 0x3c, "F7" }, { "f8", 0x3d, "F8" },
	{ "view-midi", 0x3e, "MIDI TRACKS" }, { "view-inputs", 0x3f, "INPUTS" }, { "view-audio", 0x40, "AUDIO TRACKS" },
	{ "view-instrument", 0x41, "AUDIO INSTRUMENT" }, { "view-aux", 0x42, "AUX" }, { "view-buses", 0x43, "BUSSES" },
	{ "view-outputs", 0x44, "OUTPUTS" }, { "view-user", 0x45, "USER" },
	{ "shift", 0x46, "SHIFT" }, { "option", 0x47, "OPTION" }, { "control", 0x48, "CONTROL" }, { "alt", 0x49, "ALT/CMD" },
	{ "read", 0x4a, "READ/OFF" }, { "write", 0x4b, "WRITE" }, { "trim", 0x4c, "TRIM" }, { "touch", 0x4d, "TOUCH" },
	{ "latch", 0x4e, "LATCH" }, { "group", 0x4f, "GROUP" },
	{ "save", 0x50, "SAVE" }, { "undo", 0x51, "UNDO" }, { "cancel", 0x52, "CANCEL" }, { "enter", 0x53, "ENTER" },
	{ "marker", 0x54, "MARKER" }, { "nudge", 0x55, "NUDGE" }, { "cycle", 0x56, "CYCLE" }, { "drop", 0x57, "DROP" },
	{ "replace", 0x58, "REPLACE" }, { "click", 0x59, "CLICK" }, { "solo-clear", 0x5a, "SOLO" },
	{ "rewind", 0x5b, "REWIND" }, { "forward", 0x5c, "FAST FWD" }, { "stop", 0x5d, "STOP" }, { "play", 0x5e, "PLAY" },
	{ "record", 0x5f, "RECORD" },
	{ "up", 0x60, "CURSOR UP" }, { "down", 0x61, "CURSOR DOWN" }, { "left", 0x62, "CURSOR LEFT" },
	{ "right", 0x63, "CURSOR RIGHT" }, { "zoom", 0x64, "ZOOM" }, { "scrub", 0x65, "SCRUB" },
	{ "user-a", 0x66, "USER SWITCH A" }, { "user-b", 0x67, "USER SWITCH B" },
	{ "fader-touch-1", 0x68, "FADER 1 TOUCH" }, { "fader-touch-2", 0x69, "FADER 2 TOUCH" },
	{ "fader-touch-3", 0x6a, "FADER 3 TOUCH" }, { "fader-touch-4", 0x6b, "FADER 4 TOUCH" },
	{ "fader-touch-5", 0x6c, "FADER 5 TOUCH" }, { "fader-touch-6", 0x6d, "FADER 6 TOUCH" },
	{ "fader-touch-7", 0x6e, "FADER 7 TOUCH" }, { "fader-touch-8", 0x6f, "FADER 8 TOUCH" },
	{ "fader-touch-master", 0x70, "MASTER FADER TOUCH" },
	{ "smpte-led", 0x71, "SMPTE LED" }, { "beats-led", 0x72, "BEATS LED" }, { "rude-solo-led", 0x73, "RUDE SOLO LED" },
	{ "relay", 0x76, "RELAY" },
};
const size_t mcu_n_controls = sizeof(mcu_controls) / sizeof(mcu_controls[0]);

int mcu_control_id(const char *name)
{
	for (size_t i = 0; i < mcu_n_controls; i++)
		if (!strcmp(mcu_controls[i].name, name))
			return mcu_controls[i].id;
	return -1;
}

const char *mcu_control_name(int id)
{
	for (size_t i = 0; i < mcu_n_controls; i++)
		if (mcu_controls[i].id == id)
			return mcu_controls[i].name;
	return NULL;
}

bool mcu_is_strip_button(int id)
{
	return (id >= 0 && id < 0x28) || (id >= MCU_TOUCH && id <= MCU_TOUCH_MASTER);
}

/* ------------------------------------------------------------ out */
size_t mcu_sysex(uint8_t model, const uint8_t *body, size_t n, uint8_t *out, size_t cap)
{
	if (n + 6 > cap)
		return 0;
	out[0] = 0xf0; out[1] = 0x00; out[2] = 0x00; out[3] = 0x66; out[4] = model;
	for (size_t i = 0; i < n; i++)
		out[5 + i] = body[i] & 0x7f;
	out[5 + n] = 0xf7;
	return n + 6;
}

size_t mcu_device_query(uint8_t model, uint8_t *out, size_t cap)
{
	uint8_t b[] = { 0x00 };
	return mcu_sysex(model, b, 1, out, cap);
}

size_t mcu_lc_reply(uint8_t model, const uint8_t serial[7], const uint8_t challenge[4], uint8_t *out, size_t cap)
{
	uint8_t b[12];
	b[0] = 0x02;
	memcpy(b + 1, serial, 7);
	mcu_lc_response(challenge, b + 8);
	return mcu_sysex(model, b, 12, out, cap);
}

size_t mcu_go_offline(uint8_t model, uint8_t *out, size_t cap)
{
	uint8_t b[] = { 0x0f, 0x7f };
	return mcu_sysex(model, b, 2, out, cap);
}

size_t mcu_lcd(uint8_t model, int offset, const char *chars, size_t n, uint8_t *out, size_t cap)
{
	if (offset < 0 || offset + (int)n > MCU_LCD_CELLS || n + 8 > cap)
		return 0;
	uint8_t b[2 + MCU_LCD_CELLS];
	b[0] = 0x12;
	b[1] = (uint8_t)offset;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)chars[i];
		b[2 + i] = c >= 0x20 && c < 0x7f ? c : ' ';
	}
	return mcu_sysex(model, b, n + 2, out, cap);
}

size_t mcu_colours(uint8_t model, const uint8_t colours[8], uint8_t *out, size_t cap)
{
	uint8_t b[9];
	b[0] = 0x72;
	for (int i = 0; i < 8; i++)
		b[1 + i] = colours[i] & 7;
	return mcu_sysex(model, b, 9, out, cap);
}

size_t mcu_meter_mode(uint8_t model, int strip, uint8_t mode, uint8_t *out, size_t cap)
{
	uint8_t b[] = { 0x20, (uint8_t)strip, mode };
	return mcu_sysex(model, b, 3, out, cap);
}

size_t mcu_fader(int fader, int v14, uint8_t out[3])
{
	if (v14 < 0) v14 = 0;
	if (v14 > 16383) v14 = 16383;
	out[0] = (uint8_t)(0xe0 | (fader & 0x0f));
	out[1] = (uint8_t)(v14 & 0x7f);
	out[2] = (uint8_t)(v14 >> 7);
	return 3;
}

size_t mcu_led(int id, int state, uint8_t out[3])
{
	out[0] = 0x90;
	out[1] = (uint8_t)(id & 0x7f);
	out[2] = state == 2 ? 0x7f : state == 1 ? 0x01 : 0x00;
	return 3;
}

size_t mcu_ring(int pot, uint8_t ring, uint8_t out[3])
{
	out[0] = 0xb0;
	out[1] = (uint8_t)(0x30 + (pot & 7));
	out[2] = ring & 0x7f;
	return 3;
}

size_t mcu_meter(int strip, int level, uint8_t out[2])
{
	out[0] = 0xd0;
	out[1] = (uint8_t)((strip & 7) << 4 | (level & 0x0f));
	return 2;
}

size_t mcu_digit(int cc, uint8_t seg, uint8_t out[3])
{
	out[0] = 0xb0;
	out[1] = (uint8_t)cc;
	out[2] = seg & 0x7f;
	return 3;
}

uint8_t mcu_ring_byte(enum mcu_ring_mode mode, double norm, bool centre, bool off)
{
	if (!(norm >= 0)) norm = 0;
	if (norm > 1) norm = 1;
	int pos = 0;
	if (!off)
		pos = mode == MCU_RING_SPREAD ? 1 + (int)lround(norm * 5) : 1 + (int)lround(norm * 10);
	return (uint8_t)((centre ? 0x40 : 0) | ((mode & 3) << 4) | (pos & 0x0f));
}

int mcu_parse_ring_mode(const char *s)
{
	if (!s || !strcmp(s, "dot")) return MCU_RING_DOT;
	if (!strcmp(s, "boost-cut")) return MCU_RING_BOOST_CUT;
	if (!strcmp(s, "wrap")) return MCU_RING_WRAP;
	if (!strcmp(s, "spread")) return MCU_RING_SPREAD;
	return -1;
}

uint8_t mcu_7seg(char c, bool dot)
{
	unsigned char u = (unsigned char)toupper((unsigned char)c);
	uint8_t v;
	if (u >= 0x40 && u <= 0x5f)
		v = (uint8_t)(u - 0x40);
	else if (u >= 0x20 && u <= 0x3f)
		v = u;
	else
		v = 0x20;
	return (uint8_t)(v | (dot ? 0x40 : 0));
}

int mcu_meter_level(float db)
{
	static const float steps[] = { -60, -50, -40, -30, -24, -18, -12, -9, -6, -3, 0, 3 };
	int l = 0;
	for (int i = 0; i < 12; i++)
		if (db >= steps[i])
			l = i + 1;
	return l;
}

uint8_t mcu_colour_nearest(int r, int g, int b)
{
	/* each channel on or off at half; all off is "off" only for black */
	int m = r > g ? r : g;
	if (b > m)
		m = b;
	if (m < 24)
		return 0;
	int t = m / 2;
	uint8_t c = (uint8_t)((r > t ? 1 : 0) | (g > t ? 2 : 0) | (b > t ? 4 : 0));
	return c ? c : 7;
}

int mcu_parse_colour(const char *s, int *r, int *g, int *b)
{
	if (!s || s[0] != '#' || strlen(s) != 7)
		return -1;
	for (int i = 1; i < 7; i++)
		if (!isxdigit((unsigned char)s[i]))
			return -1;
	unsigned v = (unsigned)strtoul(s + 1, NULL, 16);
	*r = (int)(v >> 16) & 0xff;
	*g = (int)(v >> 8) & 0xff;
	*b = (int)v & 0xff;
	return 0;
}

void mcu_lc_response(const uint8_t l[4], uint8_t r[4])
{
	r[0] = (uint8_t)(0x7f & (l[0] + (l[1] ^ 0x0a) - l[3]));
	r[1] = (uint8_t)(0x7f & ((l[2] >> l[3]) ^ (l[0] + l[3])));
	r[2] = (uint8_t)(0x7f & ((l[3] - (l[2] << 2)) ^ (l[0] | l[1])));
	r[3] = (uint8_t)(0x7f & (l[1] - l[2] + (0xf0 ^ (l[3] << 4))));
}

static bool vowel(char c)
{
	return strchr("aeiouAEIOU", c) != NULL;
}

void mcu_abbrev(const char *in, char *out, int width)
{
	char buf[256];
	size_t n = 0;
	for (const char *s = in ? in : ""; *s && n < sizeof(buf) - 1; s++)
		buf[n++] = (unsigned char)*s >= 0x20 && (unsigned char)*s < 0x7f ? *s : '?';
	buf[n] = 0;
	if ((int)n > width) { /* spaces first */
		size_t k = 0;
		for (size_t i = 0; i < n; i++)
			if (buf[i] != ' ')
				buf[k++] = buf[i];
		buf[k] = 0;
		n = k;
	}
	for (int i = (int)n - 1; (int)n > width && i > 0; i--) { /* then vowels from the end */
		if (vowel(buf[i]) && !(i > 0 && buf[i - 1] == ' ')) {
			memmove(buf + i, buf + i + 1, n - (size_t)i);
			n--;
		}
	}
	if ((int)n > width)
		n = (size_t)width;
	buf[n] = 0;
	snprintf(out, (size_t)width + 1, "%s", buf);
}

/* ------------------------------------------------------------ in */
bool mcu_decode(const uint8_t *m, size_t n, struct mcu_in *o)
{
	memset(o, 0, sizeof(*o));
	if (n < 1)
		return false;
	uint8_t st = m[0];
	if (st == 0xf0) {
		if (n < 7 || m[1] != 0 || m[2] != 0 || (m[3] != 0x66 && m[3] != 0x67) || m[n - 1] != 0xf7)
			return false;
		o->model = m[4];
		switch (m[5]) {
		case 0x01:
			o->kind = MCU_IN_HOST_QUERY;
			if (n >= 6 + 7 + 1)
				memcpy(o->serial, m + 6, 7);
			if (n >= 6 + 11 + 1)
				memcpy(o->challenge, m + 13, 4);
			return true;
		case 0x03:
			o->kind = MCU_IN_CONFIRM;
			if (n >= 6 + 7 + 1)
				memcpy(o->serial, m + 6, 7);
			return true;
		case 0x04: o->kind = MCU_IN_ERROR; return true;
		case 0x06: o->kind = MCU_IN_READY; return true;
		case 0x14: o->kind = MCU_IN_VERSION; return true;
		default: o->kind = MCU_IN_OTHER; return true;
		}
	}
	if (n < 3)
		return false;
	switch (st & 0xf0) {
	case 0x90:
	case 0x80: {
		int id = m[1] & 0x7f;
		bool down = (st & 0xf0) == 0x90 && (m[2] & 0x7f) > 0x40;
		if (id >= MCU_TOUCH && id <= MCU_TOUCH_MASTER) {
			o->kind = MCU_IN_TOUCH;
			o->index = id - MCU_TOUCH;
		} else {
			o->kind = MCU_IN_BUTTON;
		}
		o->id = id;
		o->down = down;
		return true;
	}
	case 0xe0:
		o->kind = MCU_IN_FADER;
		o->index = st & 0x0f;
		o->value = (m[2] & 0x7f) << 7 | (m[1] & 0x7f);
		return o->index <= 8;
	case 0xb0: {
		int cc = m[1] & 0x7f, v = m[2] & 0x7f;
		if (cc >= 0x10 && cc <= 0x17) {
			o->kind = MCU_IN_VPOT;
			o->index = cc - 0x10;
			int t = v & 0x3f;
			o->delta = (v & 0x40) ? -t : t;
			return true;
		}
		if (cc == 0x3c) {
			o->kind = MCU_IN_JOG;
			int t = v & 0x3f;
			o->delta = (v & 0x40) ? -t : t;
			return true;
		}
		if (cc == 0x2e) {
			o->kind = MCU_IN_PEDAL;
			o->value = v;
			return true;
		}
		o->kind = MCU_IN_OTHER;
		return true;
	}
	default:
		return false;
	}
}

void mcu_serial_text(const uint8_t s[7], char *out, size_t n)
{
	bool printable = true;
	int len = 7;
	while (len > 0 && s[len - 1] == 0)
		len--;
	for (int i = 0; i < len; i++)
		if (!isalnum(s[i]) && s[i] != '-' && s[i] != '_')
			printable = false;
	if (printable && len > 0) {
		snprintf(out, n, "%.*s", len, (const char *)s);
		return;
	}
	size_t k = 0;
	for (int i = 0; i < 7 && k + 3 <= n; i++)
		k += (size_t)snprintf(out + k, n - k, "%02x", s[i]);
}
