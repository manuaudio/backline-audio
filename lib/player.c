/* libbackline: Player1 (docs/api/Player1.md).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backline/backline.h"
#include "internal.h"

#define PL_NAME "io.github.manuaudio.Backline.Player"
#define PL_PATH "/io/github/manuaudio/Backline/Player"
#define PL_IFACE "io.github.manuaudio.Backline.Player1"

static int finish(bl_client *c, sd_bus_message *m, const char *method, sd_bus_message **reply)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message *rp = NULL;
	int r = sd_bus_call(bl__bus(c), m, 0, &e, &rp);
	sd_bus_message_unref(m);
	if (r < 0) {
		char what[64];
		snprintf(what, sizeof(what), "Player %s", method);
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

static int call(bl_client *c, const char *method, sd_bus_message **reply, const char *types, ...)
{
	sd_bus_message *m = NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, PL_NAME, PL_PATH, PL_IFACE, method);
	if (r >= 0 && types && *types) {
		va_list ap;
		va_start(ap, types);
		r = sd_bus_message_appendv(m, types, ap);
		va_end(ap);
	}
	if (r < 0) {
		sd_bus_message_unref(m);
		return bl__fail(c, r, "cannot build the call");
	}
	return finish(c, m, method, reply);
}

static int read_str(bl_client *c, sd_bus_message *reply, char *buf, size_t n, char **dup)
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

int bl_player_load(bl_client *c, const char *file, const bl_play_options *o, char *id, size_t id_size)
{
	bl_play_options d = BL_PLAY_OPTIONS_INIT;
	if (!o)
		o = &d;
	sd_bus_message *m = NULL, *reply = NULL;
	int r = sd_bus_message_new_method_call(bl__bus(c), &m, PL_NAME, PL_PATH, PL_IFACE, "Load");
	if (r >= 0)
		r = sd_bus_message_append(m, "s", file);
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (r >= 0 && o->id)
		r = sd_bus_message_append(m, "{sv}", "id", "s", o->id);
	if (r >= 0 && o->at)
		r = sd_bus_message_append(m, "{sv}", "at", "s", o->at);
	if (r >= 0 && o->target)
		r = sd_bus_message_append(m, "{sv}", "target", "s", o->target);
	if (r >= 0 && o->gain_db != 0 && isfinite(o->gain_db))
		r = sd_bus_message_append(m, "{sv}", "gain", "d", o->gain_db);
	if (r >= 0 && o->autoconnect >= 0)
		r = sd_bus_message_append(m, "{sv}", "autoconnect", "b", o->autoconnect);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r < 0) {
		sd_bus_message_unref(m);
		return bl__fail(c, r, "cannot build the call");
	}
	if ((r = finish(c, m, "Load", &reply)) < 0)
		return r;
	return read_str(c, reply, id, id_size, NULL);
}

int bl_player_unload(bl_client *c, const char *id) { return call(c, "Unload", NULL, "s", id); }
int bl_player_set_gain(bl_client *c, const char *id, double db) { return call(c, "SetGain", NULL, "sd", id, db); }

int bl_player_set_start(bl_client *c, const char *id, const char *position, uint64_t *frame)
{
	sd_bus_message *reply = NULL;
	int r = call(c, "SetStart", &reply, "ss", id, position);
	if (r < 0)
		return r;
	uint64_t f = 0;
	r = sd_bus_message_read(reply, "t", &f);
	sd_bus_message_unref(reply);
	if (r < 0)
		return bl__fail(c, r, "bad reply");
	if (frame)
		*frame = f;
	return 0;
}

int bl_player_list(bl_client *c, char **json)
{
	sd_bus_message *reply = NULL;
	int r = call(c, "List", &reply, "");
	return r < 0 ? r : read_str(c, reply, NULL, 0, json);
}

int bl_player_get(bl_client *c, const char *id, char **json)
{
	sd_bus_message *reply = NULL;
	int r = call(c, "Get", &reply, "s", id);
	return r < 0 ? r : read_str(c, reply, NULL, 0, json);
}
