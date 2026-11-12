/* The software MCU / X-Touch (mcusim.h).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "mcusim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

int mcusim_init(struct mcusim *s, const char *model, const char *serial, mcusim_out out, void *data)
{
	memset(s, 0, sizeof(*s));
	if (!strcmp(model, "xtouch")) { s->model = MCU_MODEL_MCU; s->master = true; s->colour_lcd = true; }
	else if (!strcmp(model, "xtouch-ext")) { s->model = MCU_MODEL_XT; s->colour_lcd = true; }
	else if (!strcmp(model, "mcu-pro")) { s->model = MCU_MODEL_MCU; s->master = true; }
	else if (!strcmp(model, "mcu-xt")) { s->model = MCU_MODEL_XT; }
	else return -1;
	snprintf(s->model_name, sizeof(s->model_name), "%s", model);
	s->strips = 8;
	for (size_t i = 0; i < 7 && serial && serial[i]; i++)
		s->serial[i] = (uint8_t)serial[i] & 0x7f;
	s->challenge[0] = 0x12; s->challenge[1] = 0x34; s->challenge[2] = 0x05; s->challenge[3] = 0x02;
	s->out = out;
	s->data = data;
	mcusim_reset(s);
	return 0;
}

void mcusim_reset(struct mcusim *s)
{
	s->connected = s->offline = false;
	memset(s->fader, 0, sizeof(s->fader));
	memset(s->host_fader, 0, sizeof(s->host_fader));
	memset(s->touched, 0, sizeof(s->touched));
	memset(s->led, 0, sizeof(s->led));
	memset(s->ring, 0, sizeof(s->ring));
	memset(s->lcd, ' ', sizeof(s->lcd));
	memset(s->colour, 0, sizeof(s->colour));
	memset(s->meter, 0, sizeof(s->meter));
	memset(s->clip, 0, sizeof(s->clip));
	memset(s->tc, 0x20, sizeof(s->tc));
	memset(s->assign, 0x20, sizeof(s->assign));
}

static void emit(struct mcusim *s, const uint8_t *m, size_t n)
{
	if (s->out && n)
		s->out(s->data, m, n);
}

static void sysex_body(struct mcusim *s, const uint8_t *b, size_t n)
{
	uint8_t m[64];
	switch (b[0]) {
	case 0x00: { /* device query: the host connection query back */
		s->rx_query++;
		uint8_t r[12];
		r[0] = 0x01;
		memcpy(r + 1, s->serial, 7);
		memcpy(r + 8, s->challenge, 4);
		emit(s, m, mcu_sysex(s->model, r, 12, m, sizeof(m)));
		if (s->model == MCU_MODEL_MCU || s->model == MCU_MODEL_XT)
			s->connected = true; /* Mackie mode: ready, no challenge */
		return;
	}
	case 0x02: { /* Logic Control: the host's answer to the challenge */
		uint8_t want[4];
		mcu_lc_response(s->challenge, want);
		bool ok = n >= 12 && !memcmp(b + 8, want, 4);
		uint8_t r[8];
		r[0] = ok ? 0x03 : 0x04;
		memcpy(r + 1, s->serial, 7);
		emit(s, m, mcu_sysex(s->model, r, ok ? 8 : 1, m, sizeof(m)));
		s->connected = ok;
		return;
	}
	case 0x0f:
		s->offline = true;
		s->connected = false;
		return;
	case 0x12: { /* LCD */
		if (n < 2)
			return;
		int off = b[1];
		for (size_t i = 2; i < n && off < MCU_LCD_CELLS; i++, off++)
			s->lcd[off] = (char)b[i];
		s->rx_lcd++;
		s->lcd_bytes += n - 2;
		return;
	}
	case 0x72: /* X-Touch colours */
		if (s->colour_lcd && n >= 9) {
			memcpy(s->colour, b + 1, 8);
			s->rx_colour++;
		}
		return;
	case 0x13: { /* version */
		uint8_t r[] = { 0x14, '1', '.', '0', '0', '0' };
		emit(s, m, mcu_sysex(s->model, r, sizeof(r), m, sizeof(m)));
		return;
	}
	case 0x61: memset(s->fader, 0, sizeof(s->fader)); return;
	case 0x62: memset(s->led, 0, sizeof(s->led)); return;
	default:
		s->rx_other++;
		return;
	}
}

void mcusim_receive(struct mcusim *s, const uint8_t *m, size_t n)
{
	if (!n)
		return;
	s->rx++;
	char *h = s->rxlog[s->rxlog_n++ % 64];
	size_t k = 0;
	for (size_t i = 0; i < n && k + 4 < sizeof(s->rxlog[0]); i++)
		k += (size_t)snprintf(h + k, sizeof(s->rxlog[0]) - k, "%s%02x", i ? " " : "", m[i]);
	if (m[0] == 0xf0) {
		if (n >= 7 && m[1] == 0 && m[2] == 0 && m[3] == 0x66 && m[4] == s->model && m[n - 1] == 0xf7)
			sysex_body(s, m + 5, n - 6);
		else
			s->rx_other++;
		return;
	}
	switch (m[0] & 0xf0) {
	case 0xe0: {
		int i = m[0] & 0x0f;
		if (n < 3 || i > 8 || (i == 8 && !s->master))
			return;
		int v = (m[2] & 0x7f) << 7 | (m[1] & 0x7f);
		s->host_fader[i] = v;
		if (!s->touched[i])
			s->fader[i] = v; /* the motor moves it (not while a finger holds it) */
		s->rx_fader++;
		return;
	}
	case 0x90:
		if (n < 3)
			return;
		s->led[m[1] & 0x7f] = m[2] == 0x7f ? 2 : m[2] == 0x01 ? 1 : 0;
		s->rx_led++;
		return;
	case 0xb0: {
		if (n < 3)
			return;
		int cc = m[1] & 0x7f;
		if (cc >= 0x30 && cc <= 0x37) {
			s->ring[cc - 0x30] = m[2] & 0x7f;
			s->rx_ring++;
		} else if (cc >= 0x40 && cc <= 0x49) {
			s->tc[cc - 0x40] = m[2] & 0x7f;
		} else if (cc == 0x4b) {
			s->assign[0] = m[2] & 0x7f;
		} else if (cc == 0x4a) {
			s->assign[1] = m[2] & 0x7f;
		} else {
			s->rx_other++;
		}
		return;
	}
	case 0xd0: {
		if (n < 2)
			return;
		int strip = (m[1] >> 4) & 7, l = m[1] & 0x0f;
		if (l == 0x0e) s->clip[strip] = true;
		else if (l == 0x0f) s->clip[strip] = false;
		else s->meter[strip] = l;
		s->rx_meter++;
		return;
	}
	default:
		s->rx_other++;
	}
}

void mcusim_touch(struct mcusim *s, int i, bool on)
{
	if (i < 0 || i > 8)
		return;
	s->touched[i] = on;
	uint8_t m[3] = { 0x90, (uint8_t)(i == 8 ? MCU_TOUCH_MASTER : MCU_TOUCH + i), on ? 0x7f : 0x00 };
	emit(s, m, 3);
	if (!on)
		s->fader[i] = s->host_fader[i]; /* released: the servo goes where the host said */
}

void mcusim_move(struct mcusim *s, int i, int v)
{
	if (i < 0 || i > 8)
		return;
	if (v < 0) v = 0;
	if (v > 16383) v = 16383;
	v &= ~0x0f; /* the MCU's faders are 10-bit: the low bits are zero */
	s->fader[i] = v;
	uint8_t m[3];
	emit(s, m, mcu_fader(i, v, m));
}

void mcusim_press(struct mcusim *s, int id, bool down)
{
	uint8_t m[3] = { 0x90, (uint8_t)(id & 0x7f), down ? 0x7f : 0x00 };
	emit(s, m, 3);
}

void mcusim_pot(struct mcusim *s, int i, int d)
{
	if (!d)
		return;
	int t = d < 0 ? -d : d;
	if (t > 63) t = 63;
	uint8_t m[3] = { 0xb0, (uint8_t)(0x10 + (i & 7)), (uint8_t)((d < 0 ? 0x40 : 0) | t) };
	emit(s, m, 3);
}

void mcusim_jog(struct mcusim *s, int d)
{
	int t = d < 0 ? -d : d;
	if (t > 63) t = 63;
	uint8_t m[3] = { 0xb0, 0x3c, (uint8_t)((d < 0 ? 0x40 : 0) | t) };
	emit(s, m, 3);
}

void mcusim_hello(struct mcusim *s)
{
	uint8_t r[12], m[32];
	r[0] = 0x01;
	memcpy(r + 1, s->serial, 7);
	memcpy(r + 8, s->challenge, 4);
	emit(s, m, mcu_sysex(s->model, r, 12, m, sizeof(m)));
}

void mcusim_raw(struct mcusim *s, const uint8_t *m, size_t n)
{
	emit(s, m, n);
}

void mcusim_lcd_line(const struct mcusim *s, int line, char out[57])
{
	memcpy(out, s->lcd + line * 56, 56);
	out[56] = 0;
}

void mcusim_lcd_cell(const struct mcusim *s, int line, int strip, char out[8])
{
	memcpy(out, s->lcd + line * 56 + strip * 7, 7);
	out[7] = 0;
	for (int i = 6; i >= 0 && out[i] == ' '; i--)
		out[i] = 0;
}

static char seg_char(uint8_t v)
{
	v &= 0x3f;
	return (char)(v < 0x20 ? v + 0x40 : v);
}

void mcusim_tc_text(const struct mcusim *s, char *out, size_t n)
{
	size_t k = 0;
	for (int d = MCU_TC_DIGITS - 1; d >= 0 && k + 2 < n; d--) {
		out[k++] = seg_char(s->tc[d]);
		if (s->tc[d] & 0x40)
			out[k++] = '.';
	}
	out[k] = 0;
}

void mcusim_assign_text(const struct mcusim *s, char out[3])
{
	out[0] = seg_char(s->assign[0]);
	out[1] = seg_char(s->assign[1]);
	out[2] = 0;
}

const char *mcusim_colour_name(uint8_t c)
{
	static const char *n[] = { "off", "red", "green", "yellow", "blue", "magenta", "cyan", "white" };
	return n[c & 7];
}

char *mcusim_state_json(const struct mcusim *s)
{
	json_object *o = json_object_new_object(), *f = json_object_new_array(), *hf = json_object_new_array(),
		    *t = json_object_new_array(), *leds = json_object_new_object(), *r = json_object_new_array(),
		    *c = json_object_new_array(), *mt = json_object_new_array(), *cl = json_object_new_array(),
		    *lcd = json_object_new_array(), *cells = json_object_new_array();
	json_object_object_add(o, "model", json_object_new_string(s->model_name));
	json_object_object_add(o, "connected", json_object_new_boolean(s->connected));
	json_object_object_add(o, "offline", json_object_new_boolean(s->offline));
	int nf = s->master ? 9 : 8;
	for (int i = 0; i < nf; i++) {
		json_object_array_add(f, json_object_new_int(s->fader[i]));
		json_object_array_add(hf, json_object_new_int(s->host_fader[i]));
		json_object_array_add(t, json_object_new_boolean(s->touched[i]));
	}
	json_object_object_add(o, "fader", f);
	json_object_object_add(o, "host-fader", hf);
	json_object_object_add(o, "touched", t);
	for (int id = 0; id < MCU_NOTES; id++)
		if (s->led[id]) {
			const char *nm = mcu_control_name(id);
			char buf[16];
			if (!nm) {
				snprintf(buf, sizeof(buf), "0x%02x", id);
				nm = buf;
			}
			json_object_object_add(leds, nm, json_object_new_string(s->led[id] == 2 ? "on" : "flash"));
		}
	json_object_object_add(o, "leds", leds);
	for (int i = 0; i < 8; i++) {
		json_object_array_add(r, json_object_new_int(s->ring[i]));
		json_object_array_add(c, json_object_new_string(mcusim_colour_name(s->colour[i])));
		json_object_array_add(mt, json_object_new_int(s->meter[i]));
		json_object_array_add(cl, json_object_new_boolean(s->clip[i]));
	}
	json_object_object_add(o, "ring", r);
	json_object_object_add(o, "colour", c);
	json_object_object_add(o, "meter", mt);
	json_object_object_add(o, "clip", cl);
	for (int line = 0; line < 2; line++) {
		char l[57];
		mcusim_lcd_line(s, line, l);
		json_object_array_add(lcd, json_object_new_string(l));
		json_object *row = json_object_new_array();
		for (int i = 0; i < 8; i++) {
			char cell[8];
			mcusim_lcd_cell(s, line, i, cell);
			json_object_array_add(row, json_object_new_string(cell));
		}
		json_object_array_add(cells, row);
	}
	json_object_object_add(o, "lcd", lcd);
	json_object_object_add(o, "cells", cells);
	char tc[32], as[3];
	mcusim_tc_text(s, tc, sizeof(tc));
	mcusim_assign_text(s, as);
	json_object_object_add(o, "timecode", json_object_new_string(tc));
	json_object_object_add(o, "assignment", json_object_new_string(as));
	json_object *rx = json_object_new_object();
	json_object_object_add(rx, "total", json_object_new_int64((int64_t)s->rx));
	json_object_object_add(rx, "fader", json_object_new_int64((int64_t)s->rx_fader));
	json_object_object_add(rx, "led", json_object_new_int64((int64_t)s->rx_led));
	json_object_object_add(rx, "ring", json_object_new_int64((int64_t)s->rx_ring));
	json_object_object_add(rx, "lcd", json_object_new_int64((int64_t)s->rx_lcd));
	json_object_object_add(rx, "lcd-bytes", json_object_new_int64((int64_t)s->lcd_bytes));
	json_object_object_add(rx, "meter", json_object_new_int64((int64_t)s->rx_meter));
	json_object_object_add(rx, "colour", json_object_new_int64((int64_t)s->rx_colour));
	json_object_object_add(rx, "query", json_object_new_int64((int64_t)s->rx_query));
	json_object_object_add(rx, "other", json_object_new_int64((int64_t)s->rx_other));
	json_object_object_add(o, "received", rx);
	json_object *log = json_object_new_array();
	unsigned first = s->rxlog_n > 64 ? s->rxlog_n - 64 : 0;
	for (unsigned i = first; i < s->rxlog_n; i++)
		json_object_array_add(log, json_object_new_string(s->rxlog[i % 64]));
	json_object_object_add(o, "rxlog", log);
	char *str = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return str;
}
