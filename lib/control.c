/* libbackline: Control1 (docs/api/Control1.md).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backline/backline.h"
#include "internal.h"

#define CTL_NAME "io.github.manuaudio.Backline.Control"
#define CTL_PATH "/io/github/manuaudio/Backline/Control"
#define CTL_IFACE "io.github.manuaudio.Backline.Control1"

static int done(bl_client *c, int r, sd_bus_error *e, const char *what)
{
	if (r < 0)
		r = bl__fail_bus(c, r, e, what);
	sd_bus_error_free(e);
	return r;
}

static int reply_str(sd_bus_message *reply, char *out, size_t n)
{
	const char *s;
	int r = sd_bus_message_read(reply, "s", &s);
	if (r >= 0 && out && n)
		snprintf(out, n, "%s", s);
	return r;
}

static int append_map_options(sd_bus_message *m, const bl_map_options *o)
{
	int r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (o && r >= 0) {
		if (o->has_min) r = sd_bus_message_append(m, "{sv}", "min", "d", o->min);
		if (r >= 0 && o->has_max) r = sd_bus_message_append(m, "{sv}", "max", "d", o->max);
		if (r >= 0 && o->invert) r = sd_bus_message_append(m, "{sv}", "invert", "b", 1);
		if (r >= 0 && o->has_in) r = sd_bus_message_append(m, "{sv}", "in-min", "d", o->in_min);
		if (r >= 0 && o->has_in) r = sd_bus_message_append(m, "{sv}", "in-max", "d", o->in_max);
		if (r >= 0 && o->id) r = sd_bus_message_append(m, "{sv}", "id", "s", o->id);
		if (r >= 0 && o->n_args) {
			r = sd_bus_message_open_container(m, 'e', "sv");
			if (r >= 0) r = sd_bus_message_append(m, "s", "args");
			if (r >= 0) r = sd_bus_message_open_container(m, 'v', "as");
			if (r >= 0) r = sd_bus_message_open_container(m, 'a', "s");
			for (size_t i = 0; r >= 0 && i < o->n_args; i++)
				r = sd_bus_message_append(m, "s", o->args[i]);
			if (r >= 0) r = sd_bus_message_close_container(m);
			if (r >= 0) r = sd_bus_message_close_container(m);
			if (r >= 0) r = sd_bus_message_close_container(m);
		}
	}
	return r < 0 ? r : sd_bus_message_close_container(m);
}

int bl_control_map(bl_client *c, const char *source, const char *target, const bl_map_options *o, char *id, size_t n)
{
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, CTL_NAME, CTL_PATH, CTL_IFACE, "Map");
	if (r >= 0) r = sd_bus_message_append(m, "ss", source, target);
	if (r >= 0) r = append_map_options(m, o);
	if (r >= 0) r = sd_bus_call(bl__bus(c), m, 0, &e, &reply);
	if (r >= 0) r = reply_str(reply, id, n);
	sd_bus_message_unref(m);
	sd_bus_message_unref(reply);
	return done(c, r, &e, "Control Map");
}

int bl_control_add_json(bl_client *c, const char *json, char *id, size_t n)
{
	sd_bus_message *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, CTL_IFACE, "AddMapping", &e, &reply, "s", json);
	if (r >= 0) r = reply_str(reply, id, n);
	sd_bus_message_unref(reply);
	return done(c, r, &e, "Control AddMapping");
}

int bl_control_remove(bl_client *c, const char *id)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, CTL_IFACE, "RemoveMapping", &e, NULL, "s", id);
	return done(c, r, &e, "Control RemoveMapping");
}

int bl_control_mappings(bl_client *c, char **json)
{
	sd_bus_message *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	const char *s;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, CTL_IFACE, "Mappings", &e, &reply, "");
	if (r >= 0) r = sd_bus_message_read(reply, "s", &s);
	if (r >= 0) *json = strdup(s);
	sd_bus_message_unref(reply);
	return done(c, r, &e, "Control Mappings");
}

struct learn_wait {
	char id[64];
	bool finished, ok;
	char source[160], reason[256];
};

static int on_learn_signal(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	struct learn_wait *w = ud;
	const char *id, *a, *b = "";
	bool learned = !strcmp(sd_bus_message_get_member(m), "Learned");
	if ((learned ? sd_bus_message_read(m, "sss", &id, &a, &b) : sd_bus_message_read(m, "ss", &id, &a)) < 0)
		return 0;
	if (w->id[0] && strcmp(id, w->id))
		return 0;
	w->finished = true;
	w->ok = learned;
	snprintf(learned ? w->source : w->reason, learned ? sizeof(w->source) : sizeof(w->reason), "%s", a);
	return 0;
}

static uint64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

int bl_control_learn(bl_client *c, const char *target, const bl_map_options *o, const char *from, unsigned timeout_s,
		     char *id, size_t id_size, char *source, size_t source_size)
{
	sd_bus *bus = bl__bus(c);
	struct learn_wait w = { 0 };
	sd_bus_slot *s1 = NULL, *s2 = NULL;
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	if (!timeout_s)
		timeout_s = 30;
	int r = sd_bus_match_signal(bus, &s1, CTL_NAME, CTL_PATH, CTL_IFACE, "Learned", on_learn_signal, &w);
	if (r >= 0) r = sd_bus_match_signal(bus, &s2, CTL_NAME, CTL_PATH, CTL_IFACE, "LearnCancelled", on_learn_signal, &w);
	if (r >= 0) r = sd_bus_message_new_method_call(bus, &m, CTL_NAME, CTL_PATH, CTL_IFACE, "Learn");
	if (r >= 0) r = sd_bus_message_append(m, "s", target);
	if (r >= 0) {
		/* the map options plus from and timeout */
		bl_map_options none = BL_MAP_OPTIONS_INIT;
		const bl_map_options *mo = o ? o : &none;
		r = sd_bus_message_open_container(m, 'a', "{sv}");
		if (r >= 0 && mo->has_min) r = sd_bus_message_append(m, "{sv}", "min", "d", mo->min);
		if (r >= 0 && mo->has_max) r = sd_bus_message_append(m, "{sv}", "max", "d", mo->max);
		if (r >= 0 && mo->invert) r = sd_bus_message_append(m, "{sv}", "invert", "b", 1);
		if (r >= 0 && mo->has_in) r = sd_bus_message_append(m, "{sv}", "in-min", "d", mo->in_min);
		if (r >= 0 && mo->has_in) r = sd_bus_message_append(m, "{sv}", "in-max", "d", mo->in_max);
		if (r >= 0 && mo->id) r = sd_bus_message_append(m, "{sv}", "id", "s", mo->id);
		if (r >= 0 && from) r = sd_bus_message_append(m, "{sv}", "from", "s", from);
		if (r >= 0) r = sd_bus_message_append(m, "{sv}", "timeout", "u", timeout_s);
		if (r >= 0 && mo->n_args) {
			r = sd_bus_message_open_container(m, 'e', "sv");
			if (r >= 0) r = sd_bus_message_append(m, "s", "args");
			if (r >= 0) r = sd_bus_message_open_container(m, 'v', "as");
			if (r >= 0) r = sd_bus_message_open_container(m, 'a', "s");
			for (size_t i = 0; r >= 0 && i < mo->n_args; i++)
				r = sd_bus_message_append(m, "s", mo->args[i]);
			for (int k = 0; r >= 0 && k < 3; k++)
				r = sd_bus_message_close_container(m);
		}
		if (r >= 0) r = sd_bus_message_close_container(m);
	}
	if (r >= 0) r = sd_bus_call(bus, m, 0, &e, &reply);
	if (r >= 0) r = reply_str(reply, w.id, sizeof(w.id));
	sd_bus_message_unref(m);
	sd_bus_message_unref(reply);
	if (r >= 0) {
		uint64_t deadline = mono_us() + (uint64_t)(timeout_s + 2) * 1000000ull;
		while (!w.finished) {
			r = sd_bus_process(bus, NULL);
			if (r < 0)
				break;
			if (r > 0)
				continue;
			uint64_t now = mono_us();
			if (now >= deadline) {
				r = -ETIMEDOUT;
				break;
			}
			r = sd_bus_wait(bus, deadline - now);
			if (r < 0)
				break;
		}
		if (w.finished && !w.ok)
			r = -ETIMEDOUT;
	}
	sd_bus_slot_unref(s1);
	sd_bus_slot_unref(s2);
	if (id && id_size)
		snprintf(id, id_size, "%s", w.id);
	if (source && source_size)
		snprintf(source, source_size, "%s", w.source);
	if (r == -ETIMEDOUT) {
		sd_bus_error_free(&e);
		char msg[300];
		snprintf(msg, sizeof(msg), "Control Learn: nothing learned (%s)", w.reason[0] ? w.reason : "timeout");
		return bl__fail(c, -ETIMEDOUT, msg);
	}
	return done(c, r < 0 ? r : 0, &e, "Control Learn");
}

int bl_control_cancel_learn(bl_client *c)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, CTL_IFACE, "CancelLearn", &e, NULL, "");
	return done(c, r, &e, "Control CancelLearn");
}

int bl_control_listen(bl_client *c, const char *proto, const char *address, uint16_t port, const bl_listen_options *o,
		      char *id, size_t n)
{
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, CTL_NAME, CTL_PATH, CTL_IFACE, "Listen");
	if (r >= 0) r = sd_bus_message_append(m, "ssq", proto, address, port);
	if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (o && r >= 0) {
		if (o->secret) r = sd_bus_message_append(m, "{sv}", "secret", "s", o->secret);
		if (r >= 0 && o->insecure_udp) r = sd_bus_message_append(m, "{sv}", "insecure-udp", "b", 1);
		if (r >= 0 && o->max_rate) r = sd_bus_message_append(m, "{sv}", "max-rate", "u", o->max_rate);
		if (r >= 0 && o->max_clients) r = sd_bus_message_append(m, "{sv}", "max-clients", "u", o->max_clients);
		if (r >= 0 && o->max_size) r = sd_bus_message_append(m, "{sv}", "max-size", "u", o->max_size);
		if (r >= 0 && o->n_allow) {
			r = sd_bus_message_open_container(m, 'e', "sv");
			if (r >= 0) r = sd_bus_message_append(m, "s", "allow");
			if (r >= 0) r = sd_bus_message_open_container(m, 'v', "as");
			if (r >= 0) r = sd_bus_message_open_container(m, 'a', "s");
			for (size_t i = 0; r >= 0 && i < o->n_allow; i++)
				r = sd_bus_message_append(m, "s", o->allow[i]);
			for (int k = 0; r >= 0 && k < 3; k++)
				r = sd_bus_message_close_container(m);
		}
	}
	if (r >= 0) r = sd_bus_message_close_container(m);
	if (r >= 0) r = sd_bus_call(bl__bus(c), m, 0, &e, &reply);
	if (r >= 0) r = reply_str(reply, id, n);
	sd_bus_message_unref(m);
	sd_bus_message_unref(reply);
	return done(c, r, &e, "Control Listen");
}

int bl_control_unlisten(bl_client *c, const char *id)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, CTL_IFACE, "Unlisten", &e, NULL, "s", id);
	return done(c, r, &e, "Control Unlisten");
}

int bl_control_set_midi_follow(bl_client *c, const char *regex)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, CTL_IFACE, "SetMidiFollow", &e, NULL, "s", regex ? regex : "");
	return done(c, r, &e, "Control SetMidiFollow");
}

int bl_control_save(bl_client *c, char *path, size_t n)
{
	sd_bus_message *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, CTL_IFACE, "Save", &e, &reply, "");
	if (r >= 0) r = reply_str(reply, path, n);
	sd_bus_message_unref(reply);
	return done(c, r, &e, "Control Save");
}

int bl_control_get_status(bl_client *c, bl_control_status *st)
{
	memset(st, 0, sizeof(*st));
	sd_bus_message *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bl__bus(c), CTL_NAME, CTL_PATH, "org.freedesktop.DBus.Properties", "GetAll", &e, &reply,
				   "s", CTL_IFACE);
	if (r >= 0) r = sd_bus_message_enter_container(reply, 'a', "{sv}");
	while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
		const char *k, *sig = NULL;
		if ((r = sd_bus_message_read(reply, "s", &k)) < 0)
			break;
		sd_bus_message_peek_type(reply, NULL, &sig);
		if (sig && !strcmp(sig, "s")) {
			const char *v;
			r = sd_bus_message_read(reply, "v", "s", &v);
			if (!strcmp(k, "Learning")) snprintf(st->learning, sizeof(st->learning), "%s", v);
			else if (!strcmp(k, "MidiFollow")) snprintf(st->midi_follow, sizeof(st->midi_follow), "%s", v);
			else if (!strcmp(k, "Node")) snprintf(st->node, sizeof(st->node), "%s", v);
			else if (!strcmp(k, "ConfigDir")) snprintf(st->config_dir, sizeof(st->config_dir), "%s", v);
		} else if (sig && !strcmp(sig, "t")) {
			uint64_t v;
			r = sd_bus_message_read(reply, "v", "t", &v);
			if (!strcmp(k, "Received")) st->received = v;
			else if (!strcmp(k, "MidiEvents")) st->midi_events = v;
			else if (!strcmp(k, "Rejected")) st->rejected = v;
			else if (!strcmp(k, "AuthFailures")) st->auth_failures = v;
			else if (!strcmp(k, "Malformed")) st->malformed = v;
			else if (!strcmp(k, "Dispatched")) st->dispatched = v;
			else if (!strcmp(k, "TargetErrors")) st->target_errors = v;
		} else if (sig && !strcmp(sig, "u")) {
			uint32_t v;
			r = sd_bus_message_read(reply, "v", "u", &v);
			if (!strcmp(k, "MappingCount")) st->mappings = v;
		} else if (sig && !strcmp(sig, "as")) {
			r = sd_bus_message_enter_container(reply, 'v', "as");
			if (r >= 0) r = sd_bus_message_enter_container(reply, 'a', "s");
			const char *v;
			while (r >= 0 && sd_bus_message_read(reply, "s", &v) > 0)
				if (st->n_listeners < 8)
					snprintf(st->listeners[st->n_listeners++], sizeof(st->listeners[0]), "%s", v);
			if (r >= 0) r = sd_bus_message_exit_container(reply);
			if (r >= 0) r = sd_bus_message_exit_container(reply);
		} else {
			r = sd_bus_message_skip(reply, "v");
		}
		if (r >= 0)
			r = sd_bus_message_exit_container(reply);
	}
	sd_bus_message_unref(reply);
	return done(c, r < 0 ? r : 0, &e, "Control status");
}
