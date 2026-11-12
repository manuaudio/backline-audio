/* backline-playerd: files into the graph on the shared timeline
 * (docs/api/Player1.md).
 * SPDX-License-Identifier: MIT
 *
 * One process on the PipeWire main loop with sd-bus on the same loop. Each
 * loaded file is a clip (clip.h): one node, placed at a transport frame.
 * The transport is clockd's (or any segment owner's): playerd never starts,
 * stops or moves it; it follows what every node sees in spa_io_position.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <pipewire/pipewire.h>
#include <systemd/sd-bus.h>

#include "bl_graph.h"
#include "clip.h"

#define BUS_NAME "io.github.manuaudio.Backline.Player"
#define OBJ_PATH "/io/github/manuaudio/Backline/Player"
#define IFACE "io.github.manuaudio.Backline.Player1"
#define CLK_NAME "io.github.manuaudio.Backline.Clock"
#define CLK_PATH "/io/github/manuaudio/Backline/Clock"
#define CLK_IFACE "io.github.manuaudio.Backline.Clock1"
#define ERR_BUSY "io.github.manuaudio.Backline.Error.Busy"
#define ERR_NOTFOUND "io.github.manuaudio.Backline.Error.NotFound"
#define ERR_TIMEOUT "io.github.manuaudio.Backline.Error.Timeout"
#define ERR_UNSUPPORTED "io.github.manuaudio.Backline.Error.Unsupported"
#define MAX_CLIPS 256

struct slot {
	bool used;
	char id[128];
	char target[256];
	struct pl_clip *clip;
	sd_bus_message *pending;   /* Load waiting for the node (and its links) */
	uint64_t deadline_ns;
	int linked;     /* links wanted to the target, once made */
};

static struct {
	struct pw_main_loop *loop;
	struct pw_loop *l;
	struct pw_context *context;
	struct pw_core *core;
	struct bl_graph *graph;
	sd_bus *bus;
	struct spa_source *bus_io, *tick, *idle_timer;
	struct slot s[MAX_CLIPS];
	int idle_exit;
	unsigned counter;
} D;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int n_clips(void)
{
	int n = 0;
	for (int i = 0; i < MAX_CLIPS; i++)
		n += D.s[i].used;
	return n;
}

static struct slot *find(const char *id)
{
	for (int i = 0; i < MAX_CLIPS; i++)
		if (D.s[i].used && !strcmp(D.s[i].id, id))
			return &D.s[i];
	return NULL;
}

static void arm_idle(void)
{
	struct timespec t = { n_clips() || D.idle_exit <= 0 ? 0 : D.idle_exit, 0 };
	pw_loop_update_timer(D.l, D.idle_timer, &t, NULL, false);
}

static void on_idle(void *data, uint64_t exp)
{
	if (!n_clips()) {
		fprintf(stderr, "backline-playerd: idle, exiting\n");
		pw_main_loop_quit(D.loop);
	}
}

static void clips_changed(void)
{
	sd_bus_emit_properties_changed(D.bus, OBJ_PATH, IFACE, "Clips", NULL);
	arm_idle();
}

static void free_slot(struct slot *s)
{
	if (s->pending) {
		sd_bus_message_unref(s->pending);
		s->pending = NULL;
	}
	pl_clip_destroy(s->clip);
	memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------- positions */
/* "48000", "frame:48000", or (through clockd, with its tempo map and frame
 * rate) "tc:HH:MM:SS:FF" and "bbt:BAR|BEAT|TICK". */
static int resolve_position(const char *p, int64_t *frame, sd_bus_error *e)
{
	const char *q = !strncmp(p, "frame:", 6) ? p + 6 : p;
	if (isdigit((unsigned char)*q)) {
		char *end;
		long long v = strtoll(q, &end, 10);
		if (*end || v < 0)
			return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "position \"%s\": frames, tc:HH:MM:SS:FF or bbt:BAR|BEAT|TICK", p);
		*frame = v;
		return 0;
	}
	if (strncmp(p, "tc:", 3) && strncmp(p, "bbt:", 4))
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "position \"%s\": frames, tc:HH:MM:SS:FF or bbt:BAR|BEAT|TICK", p);
	sd_bus_error ce = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int r = sd_bus_call_method(D.bus, CLK_NAME, CLK_PATH, CLK_IFACE, "Resolve", &ce, &reply, "s", p);
	if (r < 0) {
		if (sd_bus_error_has_name(&ce, SD_BUS_ERROR_INVALID_ARGS))
			r = sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "%s", ce.message ? ce.message : p);
		else
			r = sd_bus_error_setf(e, ERR_UNSUPPORTED, "position \"%s\" needs the clock service to resolve it: %s",
					      p, ce.message ? ce.message : strerror(-r));
		sd_bus_error_free(&ce);
		return r;
	}
	uint64_t f = 0;
	r = sd_bus_message_read(reply, "t", &f);
	sd_bus_message_unref(reply);
	if (r < 0)
		return sd_bus_error_setf(e, SD_BUS_ERROR_FAILED, "Clock1.Resolve: bad reply");
	*frame = (int64_t)f;
	return 0;
}

/* ------------------------------------------------------------------ JSON */
static json_object *clip_json(struct slot *s)
{
	struct pl_clip_info in;
	pl_clip_get_info(s->clip, &in);
	json_object *o = json_object_new_object();
	json_object_object_add(o, "id", json_object_new_string(s->id));
	char node[200];
	snprintf(node, sizeof(node), "backline.player.%s", s->id);
	json_object_object_add(o, "node", json_object_new_string(node));
	json_object_object_add(o, "node_id", json_object_new_int64(in.node_id == SPA_ID_INVALID ? -1 : (int64_t)in.node_id));
	json_object *ch = json_object_new_array();
	for (uint32_t i = 0; i < in.channels; i++)
		json_object_array_add(ch, json_object_new_string(pl_clip_channel_name(s->clip, i)));
	json_object_object_add(o, "channels", ch);
	json_object_object_add(o, "rate", json_object_new_int64(in.file_rate));
	json_object_object_add(o, "frames", json_object_new_int64((int64_t)in.frames));
	json_object_object_add(o, "start", json_object_new_int64(in.start));
	json_object_object_add(o, "end", json_object_new_int64(in.start + (int64_t)in.frames));
	json_object_object_add(o, "gain_db", json_object_new_double(in.gain_db));
	json_object_object_add(o, "target", json_object_new_string(s->target));
	json_object_object_add(o, "graph_rate", json_object_new_int64(in.graph_rate));
	json_object_object_add(o, "rate_ok", json_object_new_boolean(in.rate_ok));
	json_object_object_add(o, "rolling", json_object_new_boolean(in.rolling));
	json_object_object_add(o, "transport", json_object_new_int64(in.transport));
	int64_t at = in.transport - in.start;
	json_object_object_add(o, "playing", json_object_new_boolean(in.rolling && in.rate_ok && at >= 0 && at < (int64_t)in.frames));
	json_object_object_add(o, "cycles", json_object_new_int64((int64_t)in.cycles));
	json_object_object_add(o, "played_frames", json_object_new_int64((int64_t)in.played_frames));
	json_object_object_add(o, "underruns", json_object_new_int64((int64_t)in.underruns));
	json_object_object_add(o, "underrun_frames", json_object_new_int64((int64_t)in.underrun_frames));
	json_object_object_add(o, "seeks", json_object_new_int64((int64_t)in.seeks));
	return o;
}

/* --------------------------------------------------------------- linking */
struct portlist { uint32_t node; bool output; uint32_t ids[64]; char ch[64][16]; uint32_t idx[64]; int n; };

static int collect_ports(void *data, const struct bl_port *p)
{
	struct portlist *pl = data;
	if (p->node_id != pl->node || p->output != pl->output || p->monitor || p->midi || pl->n >= 64)
		return 0;
	int k = pl->n++;
	pl->ids[k] = p->id;
	pl->idx[k] = p->index;
	snprintf(pl->ch[k], sizeof(pl->ch[k]), "%s", p->channel);
	/* keep them in port index order */
	for (int j = k; j > 0 && pl->idx[j] < pl->idx[j - 1]; j--) {
		uint32_t t = pl->ids[j]; pl->ids[j] = pl->ids[j - 1]; pl->ids[j - 1] = t;
		t = pl->idx[j]; pl->idx[j] = pl->idx[j - 1]; pl->idx[j - 1] = t;
		char c[16]; memcpy(c, pl->ch[j], 16); memcpy(pl->ch[j], pl->ch[j - 1], 16); memcpy(pl->ch[j - 1], c, 16);
	}
	return 0;
}

struct linkcount { uint32_t node; int n; };
static int count_links(void *data, const struct bl_link *l)
{
	struct linkcount *c = data;
	if (l->out_node == c->node)
		c->n++;
	return 0;
}

/* Link the clip's outputs to the target's inputs: by channel name where the
 * target has it, else in order. Returns links wanted (0: not yet possible). */
static int link_target(struct slot *s, uint32_t node_id)
{
	const struct bl_node *t = bl_graph_find_node(D.graph, s->target);
	if (!t)
		return 0;
	struct portlist ours = { .node = node_id, .output = true }, theirs = { .node = t->id, .output = false };
	bl_graph_for_each_port(D.graph, collect_ports, &ours);
	bl_graph_for_each_port(D.graph, collect_ports, &theirs);
	struct pl_clip_info in;
	pl_clip_get_info(s->clip, &in);
	if (ours.n < (int)in.channels || theirs.n == 0)
		return 0;
	int wanted = 0;
	for (int i = 0; i < ours.n; i++) {
		int j = -1;
		for (int k = 0; k < theirs.n; k++)
			if (ours.ch[i][0] && !strcmp(ours.ch[i], theirs.ch[k])) { j = k; break; }
		if (j < 0 && ours.n == 1 && theirs.n >= 1) {
			/* mono into a multichannel input: every channel */
			for (int k = 0; k < theirs.n; k++)
				bl_graph_link(D.graph, ours.ids[0], theirs.ids[k]), wanted++;
			continue;
		}
		if (j < 0 && i < theirs.n)
			j = i;
		if (j < 0)
			continue;
		bl_graph_link(D.graph, ours.ids[i], theirs.ids[j]);
		wanted++;
	}
	return wanted;
}

static void reply_error(struct slot *s, const char *name, const char *fmt, const char *arg)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_error_setf(&e, name, fmt, arg);
	sd_bus_reply_method_error(s->pending, &e);
	sd_bus_error_free(&e);
}

static void on_tick(void *data, uint64_t exp)
{
	for (int i = 0; i < MAX_CLIPS; i++) {
		struct slot *s = &D.s[i];
		if (!s->used || !s->pending)
			continue;
		bool done = false;
		if (pl_clip_ready(s->clip)) {
			struct pl_clip_info in;
			pl_clip_get_info(s->clip, &in);
			if (!s->target[0]) {
				done = true;
			} else {
				if (!s->linked)
					s->linked = link_target(s, in.node_id);
				struct linkcount lc = { .node = in.node_id };
				bl_graph_for_each_link(D.graph, count_links, &lc);
				done = s->linked > 0 && lc.n >= s->linked;
			}
		}
		if (done) {
			char node[200];
			snprintf(node, sizeof(node), "backline.player.%s", s->id);
			sd_bus_reply_method_return(s->pending, "s", s->id);
			sd_bus_message_unref(s->pending);
			s->pending = NULL;
			sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "Loaded", "ss", s->id, node);
			clips_changed();
			fprintf(stderr, "backline-playerd: loaded %s as %s%s%s\n", s->id, node, s->target[0] ? " -> " : "", s->target);
		} else if (now_ns() > s->deadline_ns) {
			if (s->target[0] && !bl_graph_find_node(D.graph, s->target))
				reply_error(s, ERR_NOTFOUND, "no node \"%s\" to play into", s->target);
			else
				reply_error(s, ERR_TIMEOUT, "the player node for \"%s\" did not come up in time", s->id);
			free_slot(s);
			clips_changed();
		}
	}
	sd_bus_flush(D.bus);
}

/* ------------------------------------------------------------------ calls */
static bool valid_id(const char *id)
{
	if (!*id || strlen(id) > 100)
		return false;
	for (const char *p = id; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.')
			return false;
	return true;
}

static void id_from_path(const char *path, char *out, size_t n)
{
	const char *b = strrchr(path, '/');
	b = b ? b + 1 : path;
	size_t k = 0;
	for (; *b && *b != '.' && k + 1 < n && k < 40; b++)
		out[k++] = isalnum((unsigned char)*b) ? (char)tolower((unsigned char)*b) : '-';
	out[k] = 0;
	if (!k)
		snprintf(out, n, "clip");
	char base[64];
	snprintf(base, sizeof(base), "%s", out);
	for (int i = 2; find(out); i++)
		snprintf(out, n, "%s-%d", base, i);
}

static int m_load(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *path;
	int r = sd_bus_message_read(m, "s", &path);
	if (r < 0)
		return r;
	char id[128] = "", target[256] = "", at[128] = "0";
	double gain = 0, preload = 4, ring = 8;
	int autoconnect = -1;
	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k, *v;
		if ((r = sd_bus_message_read(m, "s", &k)) < 0)
			return r;
		if (!strcmp(k, "id") || !strcmp(k, "target") || !strcmp(k, "at")) {
			if ((r = sd_bus_message_read(m, "v", "s", &v)) < 0)
				return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "option %s: a string", k);
			snprintf(!strcmp(k, "id") ? id : !strcmp(k, "target") ? target : at,
				 !strcmp(k, "target") ? sizeof(target) : sizeof(id), "%s", v);
		} else if (!strcmp(k, "gain") || !strcmp(k, "preload") || !strcmp(k, "ring")) {
			double d;
			if ((r = sd_bus_message_read(m, "v", "d", &d)) < 0)
				return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "option %s: a double", k);
			*(!strcmp(k, "gain") ? &gain : !strcmp(k, "preload") ? &preload : &ring) = d;
		} else if (!strcmp(k, "autoconnect")) {
			int b;
			if ((r = sd_bus_message_read(m, "v", "b", &b)) < 0)
				return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "option %s: a boolean", k);
			autoconnect = b;
		} else {
			return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "unknown option \"%s\"", k);
		}
		if ((r = sd_bus_message_exit_container(m)) < 0)
			return r;
	}
	sd_bus_message_exit_container(m);
	if (!isfinite(gain) || gain < -120 || gain > 24)
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "gain %g dB: -120 to +24", gain);
	if (!(preload >= 0.1 && preload <= 600) || !(ring >= 0.5 && ring <= 120))
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "preload 0.1-600 s, ring 0.5-120 s");
	if (path[0] != '/')
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "file \"%s\": an absolute path", path);
	if (id[0] && !valid_id(id))
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "id \"%s\": letters, digits, - _ . (at most 100)", id);
	if (id[0] && find(id))
		return sd_bus_error_setf(e, ERR_BUSY, "a clip \"%s\" is already loaded", id);
	if (!id[0])
		id_from_path(path, id, sizeof(id));
	int64_t start;
	if ((r = resolve_position(at, &start, e)) < 0)
		return r;
	struct slot *s = NULL;
	for (int i = 0; i < MAX_CLIPS && !s; i++)
		if (!D.s[i].used)
			s = &D.s[i];
	if (!s)
		return sd_bus_error_setf(e, ERR_BUSY, "%d clips are loaded; unload one first", MAX_CLIPS);

	struct pl_clip_params p = {
		.id = id, .path = path, .start = start, .gain_db = gain, .preload_s = preload, .ring_s = ring,
		.autoconnect = autoconnect < 0 ? !target[0] : autoconnect && !target[0],
	};
	char err[512];
	int code;
	struct pl_clip *c = pl_clip_new(D.core, &p, err, sizeof(err), &code);
	if (!c)
		return sd_bus_error_setf(e, code == -ENOENT ? ERR_NOTFOUND : code == -ENOTSUP ? ERR_UNSUPPORTED : SD_BUS_ERROR_INVALID_ARGS, "%s", err);
	s->used = true;
	snprintf(s->id, sizeof(s->id), "%s", id);
	snprintf(s->target, sizeof(s->target), "%s", target);
	s->clip = c;
	s->pending = sd_bus_message_ref(m);
	s->deadline_ns = now_ns() + 3000000000ull;
	arm_idle();
	return 1;  /* replied from on_tick */
}

static struct slot *slot_arg(sd_bus_message *m, sd_bus_error *e, int *r)
{
	const char *id;
	if ((*r = sd_bus_message_read(m, "s", &id)) < 0)
		return NULL;
	struct slot *s = find(id);
	if (!s || s->pending)
		*r = sd_bus_error_setf(e, ERR_NOTFOUND, "no clip \"%s\"", id);
	return s && !s->pending ? s : NULL;
}

static int m_unload(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	int r;
	struct slot *s = slot_arg(m, e, &r);
	if (!s)
		return r;
	char id[128];
	snprintf(id, sizeof(id), "%s", s->id);
	free_slot(s);
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "Unloaded", "s", id);
	clips_changed();
	return sd_bus_reply_method_return(m, "");
}

static int m_set_gain(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	int r;
	struct slot *s = slot_arg(m, e, &r);
	if (!s)
		return r;
	double db;
	if ((r = sd_bus_message_read(m, "d", &db)) < 0)
		return r;
	if (!isfinite(db) || db < -120 || db > 24)
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "gain %g dB: -120 to +24", db);
	pl_clip_set_gain(s->clip, db);
	return sd_bus_reply_method_return(m, "");
}

static int m_set_start(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	int r;
	struct slot *s = slot_arg(m, e, &r);
	if (!s)
		return r;
	const char *pos;
	if ((r = sd_bus_message_read(m, "s", &pos)) < 0)
		return r;
	int64_t f;
	if ((r = resolve_position(pos, &f, e)) < 0)
		return r;
	pl_clip_set_start(s->clip, f);
	return sd_bus_reply_method_return(m, "t", (uint64_t)f);
}

static int reply_json(sd_bus_message *m, json_object *o)
{
	int r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return r;
}

static int m_get(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	int r;
	struct slot *s = slot_arg(m, e, &r);
	if (!s)
		return r;
	json_object *o = clip_json(s);
	json_object_object_add(o, "schema", json_object_new_string("backline.clip/1"));
	return reply_json(m, o);
}

static int m_list(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	json_object *o = json_object_new_object(), *a = json_object_new_array();
	json_object_object_add(o, "schema", json_object_new_string("backline.player/1"));
	for (int i = 0; i < MAX_CLIPS; i++)
		if (D.s[i].used && !D.s[i].pending)
			json_object_array_add(a, clip_json(&D.s[i]));
	json_object_object_add(o, "clips", a);
	return reply_json(m, o);
}

static int p_clips(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	int r = sd_bus_message_open_container(reply, 'a', "s");
	for (int i = 0; r >= 0 && i < MAX_CLIPS; i++)
		if (D.s[i].used && !D.s[i].pending)
			r = sd_bus_message_append(reply, "s", D.s[i].id);
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int p_underruns(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	uint64_t n = 0;
	for (int i = 0; i < MAX_CLIPS; i++)
		if (D.s[i].used) {
			struct pl_clip_info in;
			pl_clip_get_info(D.s[i].clip, &in);
			n += in.underruns;
		}
	return sd_bus_message_append(reply, "t", n);
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("Load", SD_BUS_ARGS("s", file, "a{sv}", options), SD_BUS_RESULT("s", id), m_load, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Unload", SD_BUS_ARGS("s", id), SD_BUS_NO_RESULT, m_unload, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetGain", SD_BUS_ARGS("s", id, "d", db), SD_BUS_NO_RESULT, m_set_gain, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetStart", SD_BUS_ARGS("s", id, "s", position), SD_BUS_RESULT("t", frame), m_set_start, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Get", SD_BUS_ARGS("s", id), SD_BUS_RESULT("s", json), m_get, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("List", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", json), m_list, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("Clips", "as", p_clips, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Underruns", "t", p_underruns, 0, 0),
	SD_BUS_SIGNAL_WITH_ARGS("Loaded", SD_BUS_ARGS("s", id, "s", node), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Unloaded", SD_BUS_ARGS("s", id), 0),
	SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ loops */
static void on_bus_io(void *data, int fd, uint32_t mask)
{
	while (sd_bus_process(D.bus, NULL) > 0) {}
	sd_bus_flush(D.bus);
}

static void on_signal(void *data, int sig) { pw_main_loop_quit(D.loop); }

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "idle-exit", required_argument, NULL, 'i' },
		{ "replace", no_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c, replace = 0;
	D.idle_exit = 60;
	while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (c) {
		case 'i': D.idle_exit = atoi(optarg); break;
		case 'r': replace = 1; break;
		case 'h':
			puts("usage: backline-playerd [--idle-exit SECONDS] [--replace]\n"
			     "  The Backline player on the session bus (" BUS_NAME ", " IFACE ").\n"
			     "  --idle-exit N  exit after N s with no clip loaded (default 60; 0 = never)");
			return 0;
		default: return 2;
		}
	}
	pw_init(&argc, &argv);
	D.loop = pw_main_loop_new(NULL);
	D.l = pw_main_loop_get_loop(D.loop);
	pw_loop_add_signal(D.l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(D.l, SIGTERM, on_signal, NULL);
	D.context = pw_context_new(D.l, NULL, 0);
	D.core = pw_context_connect(D.context, NULL, 0);
	if (!D.core) {
		fprintf(stderr, "backline-playerd: cannot connect to PipeWire\n");
		return 1;
	}
	D.graph = bl_graph_new(D.core, NULL, NULL);
	int r = sd_bus_open_user(&D.bus);
	if (r < 0) {
		fprintf(stderr, "backline-playerd: session bus: %s\n", strerror(-r));
		return 1;
	}
	r = sd_bus_add_object_vtable(D.bus, NULL, OBJ_PATH, IFACE, vtable, NULL);
	if (r >= 0)
		r = sd_bus_request_name(D.bus, BUS_NAME, replace ? SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT
								 : SD_BUS_NAME_ALLOW_REPLACEMENT);
	if (r < 0) {
		fprintf(stderr, "backline-playerd: cannot own %s: %s\n", BUS_NAME, strerror(-r));
		return 1;
	}
	D.bus_io = pw_loop_add_io(D.l, sd_bus_get_fd(D.bus), SPA_IO_IN, false, on_bus_io, NULL);
	D.tick = pw_loop_add_timer(D.l, on_tick, NULL);
	struct timespec tk = { 0, 10 * 1000000L };
	pw_loop_update_timer(D.l, D.tick, &tk, &tk, false);
	D.idle_timer = pw_loop_add_timer(D.l, on_idle, NULL);
	arm_idle();
	on_bus_io(NULL, 0, 0);
	fprintf(stderr, "backline-playerd: on the session bus as %s\n", BUS_NAME);
	pw_main_loop_run(D.loop);

	for (int i = 0; i < MAX_CLIPS; i++)
		if (D.s[i].used)
			free_slot(&D.s[i]);
	bl_graph_destroy(D.graph);
	sd_bus_flush_close_unref(D.bus);
	pw_core_disconnect(D.core);
	pw_context_destroy(D.context);
	pw_main_loop_destroy(D.loop);
	pw_deinit();
	return 0;
}
