/* libbackline: Recorder1 and Clock1 (docs/api/Recorder1.md, Clock1.md).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backline/backline.h"
#include "internal.h"

#define REC_NAME "io.github.manuaudio.Backline.Recorder"
#define REC_PATH "/io/github/manuaudio/Backline/Recorder"
#define REC_IFACE "io.github.manuaudio.Backline.Recorder1"
#define CLK_NAME "io.github.manuaudio.Backline.Clock"
#define CLK_PATH "/io/github/manuaudio/Backline/Clock"
#define CLK_IFACE "io.github.manuaudio.Backline.Clock1"

/* Call a method; *reply may be NULL when the caller doesn't need it. */
static int callv(bl_client *c, bool rec, const char *method, uint64_t timeout_us, sd_bus_message **reply,
		 const char *types, va_list ap)
{
	sd_bus_message *m = NULL, *rp = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, rec ? REC_NAME : CLK_NAME, rec ? REC_PATH : CLK_PATH,
					       rec ? REC_IFACE : CLK_IFACE, method);
	if (r >= 0 && types && *types)
		r = sd_bus_message_appendv(m, types, ap);
	if (r >= 0)
		r = sd_bus_call(bl__bus(c), m, timeout_us, &e, &rp);
	sd_bus_message_unref(m);
	if (r < 0) {
		char what[64];
		snprintf(what, sizeof(what), "%s %s", rec ? "Recorder" : "Clock", method);
		r = bl__fail_bus(c, r, &e, what);
		sd_bus_error_free(&e);
		return r;
	}
	if (reply)
		*reply = rp;
	else
		sd_bus_message_unref(rp);
	return 0;
}

static int call(bl_client *c, bool rec, const char *method, uint64_t timeout_us, sd_bus_message **reply, const char *types, ...)
{
	va_list ap;
	va_start(ap, types);
	int r = callv(c, rec, method, timeout_us, reply, types, ap);
	va_end(ap);
	return r;
}

static int reply_string(bl_client *c, sd_bus_message *reply, char *buf, size_t n, char **dup)
{
	const char *s;
	int r = sd_bus_message_read(reply, "s", &s);
	if (r >= 0) {
		if (buf)
			snprintf(buf, n, "%s", s);
		if (dup && !(*dup = strdup(s)))
			r = -ENOMEM;
	}
	sd_bus_message_unref(reply);
	return r < 0 ? bl__fail(c, r, "bad reply") : 0;
}

/* -------------------------------------------------------------- Recorder1 */
int bl_recorder_arm(bl_client *c, const char *const *sources, size_t n, const bl_rec_options *o, char *take, size_t take_size)
{
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, REC_NAME, REC_PATH, REC_IFACE, "Arm");
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "s");
	for (size_t i = 0; r >= 0 && i < n; i++)
		r = sd_bus_message_append(m, "s", sources[i]);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (o) {
		const char *keys[] = { "dir", "name", "format", "align", "latency" };
		const char *vals[] = { o->dir, o->name, o->format, o->align, o->latency };
		for (int i = 0; r >= 0 && i < 5; i++)
			if (vals[i])
				r = sd_bus_message_append(m, "{sv}", keys[i], "s", vals[i]);
		if (r >= 0 && o->pre_roll >= 0)
			r = sd_bus_message_append(m, "{sv}", "pre-roll", "d", o->pre_roll);
		if (r >= 0 && o->duration >= 0)
			r = sd_bus_message_append(m, "{sv}", "duration", "d", o->duration);
		if (r >= 0 && o->ring_seconds >= 0)
			r = sd_bus_message_append(m, "{sv}", "ring-seconds", "d", o->ring_seconds);
	}
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r >= 0)
		r = sd_bus_call(bl__bus(c), m, 10 * 1000000ull, &e, &reply);
	sd_bus_message_unref(m);
	if (r < 0) {
		r = bl__fail_bus(c, r, &e, "Recorder Arm");
		sd_bus_error_free(&e);
		return r;
	}
	return reply_string(c, reply, take, take_size, NULL);
}

int bl_recorder_record(bl_client *c, char *take, size_t n)
{
	sd_bus_message *reply;
	int r = call(c, true, "Record", 0, &reply, "");
	return r < 0 ? r : reply_string(c, reply, take, n, NULL);
}

int bl_recorder_stop(bl_client *c, char **sidecar)
{
	sd_bus_message *reply;
	/* a take can need a while to reach the disk after a stall */
	int r = call(c, true, "Stop", 600 * 1000000ull, &reply, "");
	return r < 0 ? r : reply_string(c, reply, NULL, 0, sidecar);
}

int bl_recorder_marker(bl_client *c, const char *name, uint64_t *frame)
{
	sd_bus_message *reply;
	int r = call(c, true, "AddMarker", 0, &reply, "s", name ? name : "");
	if (r < 0)
		return r;
	uint64_t f = 0;
	r = sd_bus_message_read(reply, "t", &f);
	sd_bus_message_unref(reply);
	if (frame)
		*frame = f;
	return r < 0 ? bl__fail(c, r, "bad reply") : 0;
}

int bl_recorder_takes(bl_client *c, char **json)
{
	sd_bus_message *reply;
	int r = call(c, true, "Takes", 0, &reply, "");
	return r < 0 ? r : reply_string(c, reply, NULL, 0, json);
}

int bl_recorder_take(bl_client *c, const char *take, char **json)
{
	sd_bus_message *reply;
	int r = call(c, true, "GetTake", 0, &reply, "s", take);
	return r < 0 ? r : reply_string(c, reply, NULL, 0, json);
}

int bl_recorder_soundcheck(bl_client *c, const char *take, int loop, uint64_t start)
{
	if (loop < 0)
		return call(c, true, "Soundcheck", 0, NULL, "sa{sv}", take, 1, "start", "t", start);
	return call(c, true, "Soundcheck", 0, NULL, "sa{sv}", take, 2, "loop", "b", loop, "start", "t", start);
}

int bl_recorder_set_input(bl_client *c, const char *node, const char *mode)
{
	return call(c, true, "SetInput", 0, NULL, "ss", node, mode);
}

int bl_recorder_set_channel_input(bl_client *c, const char *node, uint32_t channel, const char *mode,
				  const char *consumer)
{
	return call(c, true, "SetChannelInput", 0, NULL, "suss", node, channel, mode, consumer ? consumer : "");
}

int bl_recorder_set_channel_inputs(bl_client *c, const bl_channel_input *v, size_t n)
{
	sd_bus_message *m = NULL, *rp = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, REC_NAME, REC_PATH, REC_IFACE, "SetChannelInputs");
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "(suss)");
	for (size_t i = 0; r >= 0 && i < n; i++)
		r = sd_bus_message_append(m, "(suss)", v[i].node, v[i].channel, v[i].mode, v[i].consumer ? v[i].consumer : "");
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r >= 0)
		r = sd_bus_call(bl__bus(c), m, 0, &e, &rp);
	sd_bus_message_unref(m);
	sd_bus_message_unref(rp);
	if (r < 0) {
		r = bl__fail_bus(c, r, &e, "Recorder SetChannelInputs");
		sd_bus_error_free(&e);
	}
	return r;
}

int bl_recorder_stop_soundcheck(bl_client *c)
{
	return call(c, true, "StopSoundcheck", 0, NULL, "");
}

static int get_prop(bl_client *c, bool rec, const char *name, char type, void *out)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r;
	if (type == 's') {
		char *s = NULL;
		r = sd_bus_get_property_string(bl__bus(c), rec ? REC_NAME : CLK_NAME, rec ? REC_PATH : CLK_PATH,
					       rec ? REC_IFACE : CLK_IFACE, name, &e, &s);
		if (r >= 0) {
			snprintf((char *)out, 128, "%s", s);
			free(s);
		}
	} else {
		r = sd_bus_get_property_trivial(bl__bus(c), rec ? REC_NAME : CLK_NAME, rec ? REC_PATH : CLK_PATH,
						rec ? REC_IFACE : CLK_IFACE, name, &e, type, out);
	}
	if (r < 0) {
		r = bl__fail_bus(c, r, &e, name);
		sd_bus_error_free(&e);
	}
	return r;
}

int bl_recorder_status(bl_client *c, bl_rec_status *st)
{
	memset(st, 0, sizeof(*st));
	char tmp[128];
	int r;
	if ((r = get_prop(c, true, "State", 's', tmp)) < 0) return r;
	snprintf(st->state, sizeof(st->state), "%s", tmp);
	if ((r = get_prop(c, true, "Take", 's', st->take)) < 0) return r;
	if ((r = get_prop(c, true, "Soundcheck", 's', st->soundcheck)) < 0) return r;
	char *last = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	if (sd_bus_get_property_string(bl__bus(c), REC_NAME, REC_PATH, REC_IFACE, "LastTake", &e, &last) >= 0) {
		snprintf(st->last_take, sizeof(st->last_take), "%s", last);
		free(last);
	}
	sd_bus_error_free(&e);
	if ((r = get_prop(c, true, "Rate", 'u', &st->rate)) < 0) return r;
	if ((r = get_prop(c, true, "Frames", 't', &st->frames)) < 0) return r;
	return get_prop(c, true, "Overruns", 't', &st->overruns);
}

/* ------------------------------------------------------------------ Clock1 */
int bl_clock_play(bl_client *c) { return call(c, false, "Play", 0, NULL, ""); }
int bl_clock_stop(bl_client *c) { return call(c, false, "Stop", 0, NULL, ""); }
int bl_clock_set_tempo(bl_client *c, double bpm) { return call(c, false, "SetTempo", 0, NULL, "d", bpm); }
int bl_clock_set_meter(bl_client *c, double n, double d) { return call(c, false, "SetMeter", 0, NULL, "dd", n, d); }
int bl_clock_link(bl_client *c, bool on) { return call(c, false, "Link", 0, NULL, "b", (int)on); }

int bl_clock_locate(bl_client *c, const char *position, uint64_t *frame)
{
	sd_bus_message *reply;
	int r = call(c, false, "Locate", 0, &reply, "s", position);
	if (r < 0)
		return r;
	uint64_t f = 0;
	r = sd_bus_message_read(reply, "t", &f);
	sd_bus_message_unref(reply);
	if (frame)
		*frame = f;
	return r < 0 ? bl__fail(c, r, "bad reply") : 0;
}

int bl_clock_timecode(bl_client *c, bool ltc, const char *mode, double fps, const char *target, const char *source)
{
	sd_bus_message *m = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, CLK_NAME, CLK_PATH, CLK_IFACE, ltc ? "Ltc" : "Mtc");
	if (r >= 0) r = sd_bus_message_append(m, "s", mode);
	if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (r >= 0 && fps > 0) r = sd_bus_message_append(m, "{sv}", "fps", "d", fps);
	if (r >= 0 && target) r = sd_bus_message_append(m, "{sv}", "target", "s", target);
	if (r >= 0 && source) r = sd_bus_message_append(m, "{sv}", "source", "s", source);
	if (r >= 0) r = sd_bus_message_close_container(m);
	if (r >= 0) r = sd_bus_call(bl__bus(c), m, 0, &e, NULL);
	sd_bus_message_unref(m);
	if (r < 0) {
		r = bl__fail_bus(c, r, &e, ltc ? "Clock Ltc" : "Clock Mtc");
		sd_bus_error_free(&e);
	}
	return r < 0 ? r : 0;
}

int bl_clock_get(bl_client *c, bl_clock_status *st)
{
	memset(st, 0, sizeof(*st));
	sd_bus_message *reply;
	int r = call(c, false, "Get", 0, &reply, "");
	if (r < 0)
		return r;
	if ((r = sd_bus_message_enter_container(reply, 'a', "{sv}")) < 0)
		goto out;
	while ((r = sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
		const char *k, *contents;
		sd_bus_message_read(reply, "s", &k);
		sd_bus_message_peek_type(reply, NULL, &contents);
		sd_bus_message_enter_container(reply, 'v', contents);
		char t = contents[0];
		if (t == 'b') {
			int v;
			sd_bus_message_read_basic(reply, 'b', &v);
			if (!strcmp(k, "Rolling")) st->rolling = v;
			else if (!strcmp(k, "LinkEnabled")) st->link_enabled = v;
			else if (!strcmp(k, "Locked")) st->locked = v;
		} else if (t == 'd') {
			double v;
			sd_bus_message_read_basic(reply, 'd', &v);
			if (!strcmp(k, "Tempo")) st->tempo = v;
			else if (!strcmp(k, "BeatsPerBar")) st->beats_per_bar = v;
			else if (!strcmp(k, "BeatType")) st->beat_type = v;
			else if (!strcmp(k, "Fps")) st->fps = v;
		} else if (t == 't') {
			uint64_t v;
			sd_bus_message_read_basic(reply, 't', &v);
			if (!strcmp(k, "Frame")) st->frame = v;
		} else if (t == 'x') {
			int64_t v;
			sd_bus_message_read_basic(reply, 'x', &v);
			if (!strcmp(k, "ChaseError")) st->chase_error = v;
		} else if (t == 'u') {
			uint32_t v;
			sd_bus_message_read_basic(reply, 'u', &v);
			if (!strcmp(k, "Rate")) st->rate = v;
			else if (!strcmp(k, "LinkPeers")) st->link_peers = v;
		} else if (t == 's') {
			const char *v;
			sd_bus_message_read_basic(reply, 's', &v);
			char *dst = !strcmp(k, "Timecode") ? st->timecode : !strcmp(k, "Bbt") ? st->bbt : !strcmp(k, "Source") ? st->source
				  : !strcmp(k, "LtcMode") ? st->ltc_mode : !strcmp(k, "MtcMode") ? st->mtc_mode : NULL;
			if (dst)
				snprintf(dst, 32, "%s", v);
		} else {
			sd_bus_message_skip(reply, contents);
		}
		sd_bus_message_exit_container(reply);
		sd_bus_message_exit_container(reply);
	}
	sd_bus_message_exit_container(reply);
out:
	sd_bus_message_unref(reply);
	return r < 0 ? bl__fail(c, r, "Clock Get: bad reply") : 0;
}
