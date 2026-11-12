/* backline-test-mixer: a stub mixer program for the surface proofs. It uses
 * only the public APIs, the way any mixer (open or not) would:
 *  - Params1: registers /mix (N input channels: fader, pan, send, EQ LF,
 *    mute, solo, rec, name, colour; main and two aux masters; a timecode
 *    string), a sealed meter table with moving levels, applies every Set it
 *    is asked for and confirms it with Update, and logs Sets and Gestures;
 *  - Surface1: pushes views (inputs on layer 2, aux on layer 5) and pot modes
 *    (pan, send, an EQ strip mode), and follows Select actions with
 *    SetSelected (a minimal "follow selection" policy).
 * --extra N adds N plain parameters (/mix/bulk/p.N) for load tests.
 * stdin: "set PATH VALUE" (a change from the program's own side),
 * "name N TEXT", "quit". Log: JSON lines to --log.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <systemd/sd-bus.h>

#include "bl_meters.h"

#define CTL_NAME "io.github.manuaudio.Backline.Control"
#define PRM_PATH "/io/github/manuaudio/Backline/Params"
#define PRM_IFACE "io.github.manuaudio.Backline.Params1"
#define SRF_NAME "io.github.manuaudio.Backline.Surface"
#define SRF_PATH "/io/github/manuaudio/Backline/Surface"
#define SRF_IFACE "io.github.manuaudio.Backline.Surface1"

static struct {
	sd_bus *bus;
	char token[64];
	char root[64];
	int channels, extra;
	FILE *log;
	struct bl_meters_hdr *meters;
	int meter_fd;
	bool views;
	bool quit;
	bool moving_meters;
	double meter_db[256];
} X;

static const char *names[] = { "Kick In", "Kick Out", "Snare Top", "Snare Bot", "HiHat", "Tom 1", "Tom 2", "Floor Tom",
			       "OH L", "OH R", "Bass DI", "Bass Mic", "Gtr L", "Gtr R", "Keys L", "Keys R",
			       "Lead Vox", "BV 1", "BV 2", "Sax", "Trumpet", "Trombone", "Click", "Talkback" };
static const char *colours[] = { "#e04040", "#e0a020", "#40c040", "#3060ff", "#c040c0", "#40c0c0", "#ffffff", "#808080" };

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void logj(const char *kind, const char *path, const char *val, const char *client, const char *role)
{
	if (!X.log)
		return;
	json_object *o = json_object_new_object();
	json_object_object_add(o, "t", json_object_new_int64((int64_t)now_ns()));
	json_object_object_add(o, "kind", json_object_new_string(kind));
	json_object_object_add(o, "path", json_object_new_string(path));
	if (val)
		json_object_object_add(o, "value", json_object_new_string(val));
	if (client)
		json_object_object_add(o, "client", json_object_new_string(client));
	if (role)
		json_object_object_add(o, "role", json_object_new_string(role));
	fprintf(X.log, "%s\n", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	fflush(X.log);
	json_object_put(o);
}

static void add(json_object *a, const char *js)
{
	json_object_array_add(a, json_tokener_parse(js));
}

static char *schema(void)
{
	json_object *o = json_object_new_object(), *p = json_object_new_array(), *m = json_object_new_array();
	json_object_object_add(o, "schema", json_object_new_string("backline.params/1"));
	char js[512];
	for (int i = 1; i <= X.channels; i++) {
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/fader\",\"type\":\"float\",\"name\":\"in %d fader\",\"short\":\"Fader\",\"min\":-144,"
			 "\"max\":10,\"default\":0,\"value\":%d,\"unit\":\"dB\",\"taper\":\"db-fader\",\"flags\":[\"automatable\"]}", i, i, -2 * i);
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/pan\",\"type\":\"float\",\"short\":\"Pan\",\"min\":-100,\"max\":100,\"default\":0,"
			 "\"format\":\"%%+.0f\",\"flags\":[\"automatable\"]}", i);
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/send.a1\",\"type\":\"float\",\"short\":\"A1\",\"min\":-144,\"max\":10,\"value\":-144,"
			 "\"taper\":\"db-fader\",\"unit\":\"dB\",\"flags\":[\"automatable\"]}", i);
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/eq.lf\",\"type\":\"float\",\"short\":\"LF G\",\"min\":-15,\"max\":15,\"default\":0,"
			 "\"unit\":\"dB\",\"flags\":[\"automatable\"]}", i);
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/mute\",\"type\":\"bool\"}", i);
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/solo\",\"type\":\"bool\"}", i);
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/rec\",\"type\":\"bool\"}", i);
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/name\",\"type\":\"string\",\"value\":\"%s\"}", i,
			 i <= 24 ? names[i - 1] : "Input");
		add(p, js);
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/colour\",\"type\":\"string\",\"value\":\"%s\"}", i, colours[(i - 1) % 8]);
		add(p, js);
		snprintf(js, sizeof(js), "in.%d", i);
		json_object_array_add(m, json_object_new_string(js));
	}
	add(p, "{\"path\":\"main/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"value\":0,\"unit\":\"dB\",\"taper\":\"db-fader\",\"flags\":[\"automatable\"]}");
	add(p, "{\"path\":\"aux.1/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"value\":-5,\"unit\":\"dB\",\"taper\":\"db-fader\",\"flags\":[\"automatable\"]}");
	add(p, "{\"path\":\"aux.2/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"value\":-10,\"unit\":\"dB\",\"taper\":\"db-fader\",\"flags\":[\"automatable\"]}");
	add(p, "{\"path\":\"clock/tc\",\"type\":\"string\",\"value\":\"00:00:00:00\"}");
	add(p, "{\"path\":\"talkback\",\"type\":\"bool\",\"name\":\"Talkback\"}");
	for (int i = 1; i <= X.extra; i++) {
		snprintf(js, sizeof(js), "{\"path\":\"bulk/p.%d\",\"type\":\"float\"}", i);
		add(p, js);
	}
	json_object_object_add(o, "params", p);
	json_object_object_add(o, "meters", m);
	char *s = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return s;
}

static char *views(void)
{
	json_object *o = json_object_new_object(), *vs = json_object_new_array(), *slots = json_object_new_array();
	json_object_object_add(o, "schema", json_object_new_string("backline.views/1"));
	char js[1024];
	const char *r = X.root;
	for (int i = 1; i <= X.channels; i++) {
		snprintf(js, sizeof(js),
			 "{\"id\":\"in.%d\",\"name\":\"%s/in.%d/name\",\"colour\":\"%s/in.%d/colour\",\"fader\":\"%s/in.%d/fader\","
			 "\"pots\":{\"pan\":\"%s/in.%d/pan\",\"send\":\"%s/in.%d/send.a1\"},\"mute\":\"%s/in.%d/mute\","
			 "\"solo\":\"%s/in.%d/solo\",\"rec\":\"%s/in.%d/rec\",\"select\":{\"action\":\"select\"},\"meter\":\"%s:in.%d\"}",
			 i, r, i, r, i, r, i, r, i, r, i, r, i, r, i, r, i, r, i);
		add(slots, js);
	}
	json_object *in = json_object_new_object();
	json_object_object_add(in, "id", json_object_new_string("inputs"));
	json_object_object_add(in, "label", json_object_new_string("Inputs"));
	json_object_object_add(in, "code", json_object_new_string("In"));
	json_object_object_add(in, "layer", json_object_new_int(2));
	json_object_object_add(in, "slots", slots);
	snprintf(js, sizeof(js), "{\"fader\":\"%s/main/fader\",\"name\":\"Main\"}", r);
	json_object_object_add(in, "master", json_tokener_parse(js));
	json_object_array_add(vs, in);
	snprintf(js, sizeof(js),
		 "{\"id\":\"aux\",\"label\":\"Aux\",\"code\":\"Au\",\"layer\":5,\"slots\":["
		 "{\"id\":\"aux.1\",\"name\":\"Wedge 1\",\"colour\":\"#40ff40\",\"fader\":\"%s/aux.1/fader\"},"
		 "{\"id\":\"aux.2\",\"name\":\"Wedge 2\",\"colour\":\"#40ff40\",\"fader\":\"%s/aux.2/fader\"}],"
		 "\"master\":{\"fader\":\"%s/main/fader\"}}", r, r, r);
	add(vs, js);
	json_object_object_add(o, "views", vs);
	snprintf(js, sizeof(js),
		 "[{\"id\":\"pan\",\"label\":\"Pan\",\"binding\":\"pan\",\"ring\":\"dot\",\"push\":\"reset\"},"
		 "{\"id\":\"send\",\"label\":\"Send\",\"binding\":\"send\",\"ring\":\"wrap\"},"
		 "{\"id\":\"eq\",\"label\":\"EQ\",\"strip\":true,\"pages\":[[{\"path\":\"%s/{slot}/eq.lf\",\"label\":\"LF G\",\"ring\":\"boost-cut\"},"
		 "{\"path\":\"%s/{slot}/pan\",\"label\":\"Pan\"}]]}]", r, r);
	json_object_object_add(o, "pot-modes", json_tokener_parse(js));
	json_object_object_add(o, "home", json_tokener_parse("{\"view\":\"inputs\",\"pot-mode\":\"pan\"}"));
	char *s = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return s;
}

static int update(const char *path, char type, double num, const char *str)
{
	sd_bus_message *m = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(X.bus, &m, CTL_NAME, PRM_PATH, PRM_IFACE, "Update");
	if (r >= 0)
		r = sd_bus_message_append(m, "s", X.token);
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "(sv)");
	if (r >= 0)
		r = type == 's' ? sd_bus_message_append(m, "(sv)", path, "s", str)
		    : type == 'b' ? sd_bus_message_append(m, "(sv)", path, "b", num > 0.5)
				  : sd_bus_message_append(m, "(sv)", path, "d", num);
	if (r >= 0)
		r = sd_bus_message_close_container(m);
	if (r >= 0)
		r = sd_bus_call(X.bus, m, 5000000, &e, NULL);
	if (r < 0)
		fprintf(stderr, "backline-test-mixer: Update %s: %s\n", path, e.message ? e.message : strerror(-r));
	sd_bus_error_free(&e);
	sd_bus_message_unref(m);
	return r;
}

/* Params1.Set: the mixer applies it (here: as asked) and confirms. */
static int on_set(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *path, *c, *client, *role, *str = NULL;
	char t;
	double num = 0;
	if (sd_bus_message_read(m, "s", &path) < 0 || sd_bus_message_peek_type(m, &t, &c) < 0)
		return 0;
	sd_bus_message_enter_container(m, 'v', c);
	char type = c[0];
	switch (type) {
	case 'd': sd_bus_message_read(m, "d", &num); break;
	case 'i': { int32_t x; sd_bus_message_read(m, "i", &x); num = x; type = 'd'; break; }
	case 'b': { int x; sd_bus_message_read(m, "b", &x); num = x; break; }
	case 's': sd_bus_message_read(m, "s", &str); break;
	}
	sd_bus_message_exit_container(m);
	sd_bus_message_read(m, "ss", &client, &role);
	char v[300];
	if (str)
		snprintf(v, sizeof(v), "%s", str);
	else
		snprintf(v, sizeof(v), "%.6f", num);
	logj("set", path, v, client, role);
	update(path, type, num, str);
	return 0;
}

static int on_gesture(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *path, *client, *role;
	int begin;
	if (sd_bus_message_read(m, "sbss", &path, &begin, &client, &role) < 0)
		return 0;
	logj(begin ? "gesture-begin" : "gesture-end", path, NULL, client, role);
	return 0;
}

/* A minimal policy: Select on a strip selects that channel (the EQ strip mode follows it). */
static int on_action(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *group, *name;
	int down;
	if (sd_bus_message_read(m, "ssb", &group, &name, &down) < 0)
		return 0;
	logj(down ? "action-down" : "action-up", name, NULL, group, NULL);
	if (down && !strncmp(name, "select:", 7))
		sd_bus_call_method(X.bus, SRF_NAME, SRF_PATH, SRF_IFACE, "SetSelected", NULL, NULL, "ss", group, name + 7);
	if (down && !strcmp(name, "talkback"))
		update("/mix/talkback", 'b', 1, NULL);
	if (!down && !strcmp(name, "talkback"))
		update("/mix/talkback", 'b', 0, NULL);
	return 0;
}

static void push_views(void)
{
	char *v = views();
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(X.bus, SRF_NAME, SRF_PATH, SRF_IFACE, "SetViews", &e, NULL, "ss", "*", v);
	if (r < 0)
		fprintf(stderr, "backline-test-mixer: SetViews: %s\n", e.message ? e.message : strerror(-r));
	else
		logj("views", "*", NULL, NULL, NULL);
	sd_bus_error_free(&e);
	free(v);
}

static int on_owner(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *name, *old, *new_;
	if (sd_bus_message_read(m, "sss", &name, &old, &new_) >= 0 && !strcmp(name, SRF_NAME) && *new_ && X.views)
		push_views();
	return 0;
}

static void write_meters(uint64_t now)
{
	if (!X.meters)
		return;
	struct bl_meter *e = bl_meters_entries(X.meters);
	bl_meters_write_begin(X.meters);
	for (int i = 0; i < X.channels; i++) {
		double db = X.meter_db[i];
		if (X.moving_meters)
			db += 6 * sin((double)now / 1e9 * 2 + i);
		e[i].db = (float)db;
		e[i].flags = (db >= 0 ? 1u : 0u) | (db > -90 ? 2u : 0u);
	}
	bl_meters_write_end(X.meters, now);
}

static void stdin_line(char *l)
{
	char path[300], val[300];
	int n;
	if (sscanf(l, "set %299s %299[^\n]", path, val) == 2) {
		char *end;
		double d = strtod(val, &end);
		if (*end == 0)
			update(path, 'd', d, NULL);
		else if (!strcmp(val, "true") || !strcmp(val, "false"))
			update(path, 'b', !strcmp(val, "true"), NULL);
		else
			update(path, 's', 0, val);
	} else if (sscanf(l, "meter %d %299s", &n, val) == 2 && n >= 1 && n <= X.channels) {
		X.meter_db[n - 1] = atof(val);
	} else if (!strcmp(l, "views")) {
		push_views();
	} else if (!strcmp(l, "quit")) {
		X.quit = true;
	}
}

static void on_sig(int s) { X.quit = true; }

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "channels", required_argument, NULL, 'c' },
		{ "log", required_argument, NULL, 'l' },
		{ "views", no_argument, NULL, 'v' },
		{ "moving-meters", no_argument, NULL, 'm' },
		{ "extra", required_argument, NULL, 'x' },
		{ NULL, 0, NULL, 0 },
	};
	X.channels = 24;
	snprintf(X.root, sizeof(X.root), "/mix");
	int c;
	while ((c = getopt_long(argc, argv, "c:l:vmx:", opts, NULL)) != -1) {
		switch (c) {
		case 'c': X.channels = atoi(optarg); break;
		case 'l': X.log = fopen(optarg, "a"); break;
		case 'v': X.views = true; break;
		case 'm': X.moving_meters = true; break;
		case 'x': X.extra = atoi(optarg); break;
		default:
			fputs("usage: backline-test-mixer [--channels N] [--extra N] [--log FILE] [--views] [--moving-meters]\n", stderr);
			return 2;
		}
	}
	if (X.channels < 1 || X.channels > 256)
		X.channels = 24;
	for (int i = 0; i < X.channels; i++)
		X.meter_db[i] = -120;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	if (sd_bus_open_user(&X.bus) < 0) {
		fprintf(stderr, "backline-test-mixer: no session bus\n");
		return 1;
	}
	sd_bus_match_signal(X.bus, NULL, NULL, PRM_PATH, PRM_IFACE, "Set", on_set, NULL);
	sd_bus_match_signal(X.bus, NULL, NULL, PRM_PATH, PRM_IFACE, "Gesture", on_gesture, NULL);
	sd_bus_match_signal(X.bus, NULL, NULL, SRF_PATH, SRF_IFACE, "Action", on_action, NULL);
	sd_bus_match_signal(X.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
			    "NameOwnerChanged", on_owner, NULL);
	char *js = schema();
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int r = sd_bus_call_method(X.bus, CTL_NAME, PRM_PATH, PRM_IFACE, "Register", &e, &reply, "ss", X.root, js);
	free(js);
	if (r < 0) {
		fprintf(stderr, "backline-test-mixer: Register: %s\n", e.message ? e.message : strerror(-r));
		return 1;
	}
	const char *tok;
	sd_bus_message_read(reply, "s", &tok);
	snprintf(X.token, sizeof(X.token), "%s", tok);
	sd_bus_message_unref(reply);
	sd_bus_error_free(&e);
	X.meter_fd = bl_meters_create((uint32_t)X.channels, &X.meters);
	if (X.meter_fd >= 0) {
		sd_bus_message *m = NULL;
		r = sd_bus_message_new_method_call(X.bus, &m, CTL_NAME, PRM_PATH, PRM_IFACE, "Meters");
		if (r >= 0)
			r = sd_bus_message_append(m, "sh", X.token, X.meter_fd);
		if (r >= 0)
			r = sd_bus_message_open_container(m, 'a', "s");
		for (int i = 1; r >= 0 && i <= X.channels; i++) {
			char id[16];
			snprintf(id, sizeof(id), "in.%d", i);
			r = sd_bus_message_append(m, "s", id);
		}
		if (r >= 0)
			r = sd_bus_message_close_container(m);
		if (r >= 0)
			r = sd_bus_call(X.bus, m, 5000000, &e, NULL);
		if (r < 0)
			fprintf(stderr, "backline-test-mixer: Meters: %s\n", e.message ? e.message : strerror(-r));
		sd_bus_error_free(&e);
		sd_bus_message_unref(m);
	}
	printf("registered %s with %d channels\n", X.root, X.channels);
	fflush(stdout);
	if (X.views)
		push_views();
	char line[1024];
	size_t ll = 0;
	uint64_t last_meter = 0;
	bool in_open = true;
	while (!X.quit) {
		struct pollfd p[2] = { { sd_bus_get_fd(X.bus), POLLIN, 0 }, { in_open ? 0 : -1, POLLIN, 0 } };
		poll(p, 2, 20);
		while (sd_bus_process(X.bus, NULL) > 0) {}
		if (in_open && (p[1].revents & (POLLIN | POLLHUP))) {
			char b[256];
			ssize_t n = read(0, b, sizeof(b));
			if (n <= 0)
				in_open = false;
			for (ssize_t i = 0; i < n; i++) {
				if (b[i] == '\n' || ll >= sizeof(line) - 1) {
					line[ll] = 0;
					stdin_line(line);
					ll = 0;
				} else {
					line[ll++] = b[i];
				}
			}
		}
		uint64_t now = now_ns();
		if (now - last_meter > 33000000ull) {
			write_meters(now);
			last_meter = now;
		}
	}
	return 0;
}
