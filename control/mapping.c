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
		snprintf(err, err_size, "'%s': a source is midi:cc:CH:NUM, midi:note:CH:KEY, midi:pitchbend:CH, "
			 "midi:program:CH or osc:/ADDRESS", s);
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
		snprintf(err, err_size, "'%s': expected midi:cc:CH:NUM, midi:note:CH:KEY, midi:pitchbend:CH or "
			 "midi:program:CH (CH 1-16 or *, NUM and KEY 0-127)", s);
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
			 "recorder:arm|record|stop|marker, volume:NODE, mute:NODE or control:NODE/NAME", s);
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
	if ((m->kind == CTL_SRC_CC || m->kind == CTL_SRC_NOTE) && m->number != e->number)
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
