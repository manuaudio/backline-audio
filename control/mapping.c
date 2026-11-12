/* Control mappings (mapping.h).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "mapping.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_int(const char *s, int lo, int hi, int *out)
{
	char *end;
	if (!s || !*s)
		return -1;
	long v = strtol(s, &end, 10);
	if (*end || v < lo || v > hi)
		return -1;
	*out = (int)v;
	return 0;
}

static int parse_channel(const char *s, int *out)
{
	if (s && !strcmp(s, "*")) {
		*out = 0;
		return 0;
	}
	return parse_int(s, 1, 16, out);
}

int ctl_parse_source(const char *s, struct ctl_source *out, char *err, size_t err_size)
{
	memset(out, 0, sizeof(*out));
	out->number = -1;
	if (!s) s = "";
	if (!strncmp(s, "osc:", 4)) {
		const char *a = s + 4;
		if (a[0] != '/' || strlen(a) >= sizeof(out->address) || strpbrk(a, " #*?,[]{}")) {
			snprintf(err, err_size, "'%s': an OSC source is osc:/an/address (no spaces or OSC patterns)", s);
			return -1;
		}
		out->kind = CTL_SRC_OSC;
		strcpy(out->address, a);
		return 0;
	}
	if (strncmp(s, "midi:", 5)) {
		snprintf(err, err_size, "'%s': a source is midi:cc:CH:NUM, midi:cc14:CH:MSB, midi:nrpn:CH:PARAM, "
			 "midi:rpn:CH:PARAM, midi:note:CH:KEY, midi:pitchbend:CH, midi:program:CH or osc:/ADDRESS", s);
		return -1;
	}
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", s + 5);
	char *f[4] = { 0 };
	int n = 0;
	for (char *save = NULL, *t = strtok_r(buf, ":", &save); t && n < 4; t = strtok_r(NULL, ":", &save))
		f[n++] = t;
	bool ok = false;
	if (n == 3 && !strcmp(f[0], "cc")) {
		out->kind = CTL_SRC_CC;
		ok = !parse_channel(f[1], &out->channel) && !parse_int(f[2], 0, 127, &out->number);
	} else if (n == 3 && !strcmp(f[0], "cc14")) {
		out->kind = CTL_SRC_CC14;
		ok = !parse_channel(f[1], &out->channel) && !parse_int(f[2], 0, 31, &out->number);
	} else if (n == 3 && (!strcmp(f[0], "nrpn") || !strcmp(f[0], "rpn"))) {
		out->kind = f[0][0] == 'n' ? CTL_SRC_NRPN : CTL_SRC_RPN;
		ok = !parse_channel(f[1], &out->channel) && !parse_int(f[2], 0, 16383, &out->number);
	} else if (n == 3 && !strcmp(f[0], "note")) {
		out->kind = CTL_SRC_NOTE;
		ok = !parse_channel(f[1], &out->channel) && !parse_int(f[2], 0, 127, &out->number);
	} else if (n == 2 && !strcmp(f[0], "pitchbend")) {
		out->kind = CTL_SRC_PITCHBEND;
		ok = !parse_channel(f[1], &out->channel);
	} else if (n == 2 && !strcmp(f[0], "program")) {
		out->kind = CTL_SRC_PROGRAM;
		ok = !parse_channel(f[1], &out->channel);
	}
	if (!ok) {
		snprintf(err, err_size, "'%s': expected midi:cc:CH:NUM, midi:cc14:CH:MSB, midi:nrpn:CH:PARAM, midi:rpn:CH:PARAM, "
			 "midi:note:CH:KEY, midi:pitchbend:CH or midi:program:CH (CH 1-16 or *, NUM and KEY 0-127, MSB 0-31, "
			 "PARAM 0-16383)", s);
		return -1;
	}
	return 0;
}

void ctl_format_source(const struct ctl_source *s, char *out, size_t n)
{
	char ch[8];
	if (s->channel)
		snprintf(ch, sizeof(ch), "%d", s->channel);
	else
		strcpy(ch, "*");
	switch (s->kind) {
	case CTL_SRC_CC: snprintf(out, n, "midi:cc:%s:%d", ch, s->number); break;
	case CTL_SRC_NOTE: snprintf(out, n, "midi:note:%s:%d", ch, s->number); break;
	case CTL_SRC_PITCHBEND: snprintf(out, n, "midi:pitchbend:%s", ch); break;
	case CTL_SRC_PROGRAM: snprintf(out, n, "midi:program:%s", ch); break;
	case CTL_SRC_OSC: snprintf(out, n, "osc:%s", s->address); break;
	case CTL_SRC_CC14: snprintf(out, n, "midi:cc14:%s:%d", ch, s->number); break;
	case CTL_SRC_NRPN: snprintf(out, n, "midi:nrpn:%s:%d", ch, s->number); break;
	case CTL_SRC_RPN: snprintf(out, n, "midi:rpn:%s:%d", ch, s->number); break;
	}
}

static int node_and_key(const char *rest, struct ctl_target *t, bool need_key)
{
	const char *slash = strchr(rest, '/');
	size_t nl = slash ? (size_t)(slash - rest) : strlen(rest);
	if (nl == 0 || nl >= sizeof(t->node))
		return -1;
	memcpy(t->node, rest, nl);
	t->node[nl] = 0;
	if (need_key) {
		if (!slash || !slash[1] || strlen(slash + 1) >= sizeof(t->key))
			return -1;
		strcpy(t->key, slash + 1);
	} else if (slash) {
		return -1;
	}
	return 0;
}

int ctl_parse_target(const char *s, struct ctl_target *out, char *err, size_t err_size)
{
	memset(out, 0, sizeof(*out));
	if (!s) s = "";
	int r = -1;
	if (!strncmp(s, "instance:", 9)) {
		out->kind = CTL_T_INSTANCE;
		r = node_and_key(s + 9, out, true);
	} else if (!strncmp(s, "volume:", 7)) {
		out->kind = CTL_T_VOLUME;
		r = node_and_key(s + 7, out, false);
	} else if (!strncmp(s, "mute:", 5)) {
		out->kind = CTL_T_MUTE;
		r = node_and_key(s + 5, out, false);
	} else if (!strncmp(s, "control:", 8)) {
		out->kind = CTL_T_CONTROL;
		r = node_and_key(s + 8, out, true);
	} else if (!strncmp(s, "param:", 6)) {
		out->kind = CTL_T_PARAM;
		const char *p = s + 6;
		if (p[0] == '/' && strlen(p) < sizeof(out->key) && strchr(p + 1, '/') && !strpbrk(p, " *?[]{}#")) {
			strcpy(out->key, p);
			r = 0;
		}
	} else if (!strncmp(s, "transport:", 10)) {
		out->kind = CTL_T_TRANSPORT;
		const char *a = s + 10;
		if (!strcmp(a, "play") || !strcmp(a, "stop") || !strcmp(a, "toggle") || !strcmp(a, "tempo")) {
			strcpy(out->action, a);
			r = 0;
		} else if (!strncmp(a, "locate:", 7) && a[7] && strlen(a + 7) < sizeof(out->arg)) {
			strcpy(out->action, "locate");
			strcpy(out->arg, a + 7);
			r = 0;
		}
	} else if (!strncmp(s, "recorder:", 9)) {
		out->kind = CTL_T_RECORDER;
		const char *a = s + 9;
		if (!strcmp(a, "arm") || !strcmp(a, "record") || !strcmp(a, "stop") || !strcmp(a, "marker")) {
			strcpy(out->action, a);
			r = 0;
		}
	}
	if (r < 0)
		snprintf(err, err_size, "'%s': a target is instance:NODE/PARAM, transport:play|stop|toggle|tempo|locate:POS, "
			 "recorder:arm|record|stop|marker, volume:NODE, mute:NODE, control:NODE/NAME or param:/ROOT/PATH", s);
	return r;
}

bool ctl_target_is_trigger(const struct ctl_target *t)
{
	if (t->kind == CTL_T_RECORDER)
		return true;
	if (t->kind == CTL_T_TRANSPORT)
		return strcmp(t->action, "tempo") != 0;
	return false;
}

bool ctl_target_default_range(const struct ctl_target *t, double *min, double *max)
{
	switch (t->kind) {
	case CTL_T_VOLUME: *min = -60; *max = 0; return true;
	case CTL_T_MUTE: *min = 0; *max = 1; return true;
	case CTL_T_CONTROL: *min = 0; *max = 1; return true;
	case CTL_T_PARAM: *min = 0; *max = 1; return true;
	case CTL_T_TRANSPORT: *min = 60; *max = 180; return true;
	default: *min = 0; *max = 1; return t->kind == CTL_T_RECORDER;
	}
}

bool ctl_midi_event(const uint8_t *m, size_t size, struct ctl_source *src, double *value)
{
	if (size < 2 || !(m[0] & 0x80))
		return false;
	memset(src, 0, sizeof(*src));
	src->channel = (m[0] & 0x0f) + 1;
	src->number = -1;
	switch (m[0] & 0xf0) {
	case 0xb0:
		if (size < 3) return false;
		src->kind = CTL_SRC_CC;
		src->number = m[1] & 0x7f;
		*value = (m[2] & 0x7f) / 127.0;
		return true;
	case 0x90:
	case 0x80:
		if (size < 3) return false;
		src->kind = CTL_SRC_NOTE;
		src->number = m[1] & 0x7f;
		*value = (m[0] & 0xf0) == 0x90 ? (m[2] & 0x7f) / 127.0 : 0;
		return true;
	case 0xe0:
		if (size < 3) return false;
		src->kind = CTL_SRC_PITCHBEND;
		*value = ((m[2] & 0x7f) << 7 | (m[1] & 0x7f)) / 16383.0;
		return true;
	case 0xc0:
		src->kind = CTL_SRC_PROGRAM;
		*value = (m[1] & 0x7f) / 127.0;
		return true;
	default:
		return false;
	}
}

bool ctl_source_matches(const struct ctl_source *m, const struct ctl_source *e)
{
	if (m->kind != e->kind)
		return false;
	if (m->kind == CTL_SRC_OSC)
		return !strcmp(m->address, e->address);
	if (m->channel && m->channel != e->channel)
		return false;
	if ((m->kind == CTL_SRC_CC || m->kind == CTL_SRC_NOTE || m->kind == CTL_SRC_CC14 || m->kind == CTL_SRC_NRPN ||
	     m->kind == CTL_SRC_RPN) && m->number != e->number)
		return false;
	return true;
}

double ctl_scale(double v, bool invert, double min, double max)
{
	if (!(v >= 0)) v = 0; /* NaN too */
	if (v > 1) v = 1;
	if (invert) v = 1 - v;
	return min + v * (max - min);
}

double ctl_normalize(double x, double in_min, double in_max)
{
	if (in_max == in_min)
		return x >= in_max ? 1 : 0;
	double v = (x - in_min) / (in_max - in_min);
	if (!(v >= 0)) return 0;
	return v > 1 ? 1 : v;
}

double ctl_db_to_gain(double db)
{
	if (!(db > -120))
		return 0;
	return pow(10.0, db / 20.0);
}

double ctl_gain_to_db(double g)
{
	if (!(g > 0))
		return -INFINITY;
	return 20.0 * log10(g);
}

/* ============================================================ additions */
int ctl_parse_encoding(const char *s, enum ctl_encoding *out)
{
	static const char *n[] = { "abs", "rel-2c", "rel-sm", "rel-offset", "rel-inc-dec" };
	for (int i = 0; i < 5; i++)
		if (s && !strcmp(s, n[i])) {
			*out = (enum ctl_encoding)i;
			return 0;
		}
	return -1;
}

const char *ctl_encoding_name(enum ctl_encoding e)
{
	static const char *n[] = { "abs", "rel-2c", "rel-sm", "rel-offset", "rel-inc-dec" };
	return n[e];
}

int ctl_parse_takeover(const char *s, enum ctl_takeover_mode *out)
{
	static const char *n[] = { "jump", "pickup", "scale" };
	for (int i = 0; i < 3; i++)
		if (s && !strcmp(s, n[i])) {
			*out = (enum ctl_takeover_mode)i;
			return 0;
		}
	return -1;
}

const char *ctl_takeover_name(enum ctl_takeover_mode t)
{
	static const char *n[] = { "jump", "pickup", "scale" };
	return n[t];
}

int ctl_rel_ticks(enum ctl_encoding e, int v)
{
	v &= 0x7f;
	switch (e) {
	case CTL_ENC_REL_2C: return v < 64 ? v : v - 128;
	case CTL_ENC_REL_SM: return v & 0x40 ? -(v & 0x3f) : (v & 0x3f);
	case CTL_ENC_REL_OFFSET: return v - 64;
	default: return 0;
	}
}

double ctl_rel_apply(double cur, int ticks, double step, double accel)
{
	if (!(cur >= 0)) cur = 0;
	if (cur > 1) cur = 1;
	if (ticks == 0)
		return cur;
	double mag = fabs((double)ticks);
	if (accel > 1)
		mag = pow(mag, accel);
	double v = cur + (ticks < 0 ? -mag : mag) * step;
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	return v;
}

bool ctl_hires_feed(struct ctl_hires *h, int part, int v7, uint64_t now, double *norm)
{
	v7 &= 0x7f;
	if (part == 0) {
		h->msb = v7;
		h->have_msb = true;
		if (!h->seen_lsb) {
			*norm = v7 / 127.0;
			return true;
		}
		h->pending = true;
		h->msb_ns = now;
		return false;
	}
	h->lsb = v7;
	h->seen_lsb = true;
	if (!h->have_msb)
		return false;
	h->pending = false;
	*norm = (h->msb << 7 | h->lsb) / 16383.0;
	return true;
}

bool ctl_hires_expire(struct ctl_hires *h, uint64_t now, double *norm)
{
	if (!h->pending || now - h->msb_ns < CTL_HIRES_TIMEOUT_NS)
		return false;
	h->pending = false;
	*norm = (h->msb << 7) / 16383.0;
	return true;
}

void ctl_nrpn_init(struct ctl_nrpn *s)
{
	for (int c = 0; c < 16; c++) {
		s->nrpn_msb[c] = s->nrpn_lsb[c] = s->rpn_msb[c] = s->rpn_lsb[c] = 127;
		s->sel[c] = 0;
	}
}

enum ctl_nrpn_ev ctl_nrpn_feed(struct ctl_nrpn *s, int ch, int cc, int val, bool *rpn, int *param, int *value)
{
	if (ch < 0 || ch > 15)
		return CTL_NRPN_NONE;
	val &= 0x7f;
	switch (cc) {
	case 99: s->nrpn_msb[ch] = val; s->sel[ch] = 1; return CTL_NRPN_NONE;
	case 98: s->nrpn_lsb[ch] = val; s->sel[ch] = 1; return CTL_NRPN_NONE;
	case 101: s->rpn_msb[ch] = val; s->sel[ch] = 2; break;
	case 100: s->rpn_lsb[ch] = val; s->sel[ch] = 2; break;
	case 6: case 38: case 96: case 97: {
		if (!s->sel[ch])
			return CTL_NRPN_NONE;
		*rpn = s->sel[ch] == 2;
		*param = *rpn ? (s->rpn_msb[ch] << 7 | s->rpn_lsb[ch]) : (s->nrpn_msb[ch] << 7 | s->nrpn_lsb[ch]);
		*value = val;
		if (cc == 6) return CTL_NRPN_DATA_MSB;
		if (cc == 38) return CTL_NRPN_DATA_LSB;
		if (!*value) *value = 1;
		return cc == 96 ? CTL_NRPN_INC : CTL_NRPN_DEC;
	}
	default:
		return CTL_NRPN_NONE;
	}
	/* RPN 127/127 is the null parameter: deselect */
	if (s->sel[ch] == 2 && s->rpn_msb[ch] == 127 && s->rpn_lsb[ch] == 127)
		s->sel[ch] = 0;
	return CTL_NRPN_NONE;
}

bool ctl_takeover_apply(enum ctl_takeover_mode m, struct ctl_takeover *t, double c, bool known, double target,
			double *out)
{
	if (!(c >= 0)) c = 0;
	if (c > 1) c = 1;
	double prev = t->have_prev ? t->prev : c;
	bool had_prev = t->have_prev;
	t->prev = c;
	t->have_prev = true;
	if (m == CTL_TO_JUMP || !known || t->picked) {
		*out = c;
		return true;
	}
	bool meets = fabs(c - target) <= CTL_TAKEOVER_TOLERANCE || (had_prev && (prev - target) * (c - target) < 0);
	if (m == CTL_TO_PICKUP) {
		if (!meets)
			return false;
		t->picked = true;
		*out = c;
		return true;
	}
	if (!had_prev || c == prev) {
		if (!meets)
			return false;
		t->picked = true;
		*out = c;
		return true;
	}
	/* scale: close the gap in proportion to the travel the control has left */
	double v;
	if (c > prev)
		v = prev >= 1 ? 1 : target + (c - prev) * (1 - target) / (1 - prev);
	else
		v = prev <= 0 ? 0 : target - (prev - c) * target / prev;
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	if (fabs(c - v) <= CTL_TAKEOVER_TOLERANCE)
		t->picked = true;
	*out = v;
	return true;
}

void ctl_takeover_lost(struct ctl_takeover *t)
{
	t->picked = false;
}

size_t ctl_feedback_midi(const struct ctl_source *s, double norm, int bits, uint8_t *o, size_t cap)
{
	if (!(norm >= 0)) norm = 0;
	if (norm > 1) norm = 1;
	uint8_t ch = (uint8_t)((s->channel ? s->channel : 1) - 1);
	int v7 = (int)lround(norm * 127), v14 = (int)lround(norm * 16383);
	size_t n = 0;
#define B(x) do { if (n < cap) o[n] = (uint8_t)(x); n++; } while (0)
	switch (s->kind) {
	case CTL_SRC_CC: B(0xb0 | ch); B(s->number); B(v7); break;
	case CTL_SRC_CC14: B(0xb0 | ch); B(s->number); B(v14 >> 7); B(0xb0 | ch); B(s->number + 32); B(v14 & 0x7f); break;
	case CTL_SRC_NRPN:
	case CTL_SRC_RPN: {
		bool r = s->kind == CTL_SRC_RPN;
		B(0xb0 | ch); B(r ? 101 : 99); B(s->number >> 7);
		B(0xb0 | ch); B(r ? 100 : 98); B(s->number & 0x7f);
		if (bits == 7) {
			B(0xb0 | ch); B(6); B(v7);
		} else {
			B(0xb0 | ch); B(6); B(v14 >> 7);
			B(0xb0 | ch); B(38); B(v14 & 0x7f);
		}
		break;
	}
	case CTL_SRC_NOTE: B(0x90 | ch); B(s->number); B(norm > 0.5 ? 127 : 0); break;
	case CTL_SRC_PITCHBEND: B(0xe0 | ch); B(v14 & 0x7f); B(v14 >> 7); break;
	case CTL_SRC_PROGRAM: B(0xc0 | ch); B(v7); break;
	case CTL_SRC_OSC: break;
	}
#undef B
	return n <= cap ? n : 0;
}
