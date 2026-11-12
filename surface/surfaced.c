/* backline-surfaced: control surfaces for any program (docs/api/Surface1.md).
 * SPDX-License-Identifier: MIT
 *
 * One process on the PipeWire main loop: the graph watcher (devices come and
 * go as MIDI ports), one small PipeWire node per unit (MIDI in and out,
 * linked to the device's ports), sd-bus for Surface1 and as a client of
 * controld's Params1, and a 5 ms timer that runs the engine (engine.c). The
 * only realtime code is each unit node's process callback: it copies the
 * device's MIDI into a wait-free ring and drains another ring out.
 *
 * Its own process, next to controld (ARCHITECTURE "one process per risk"): a
 * surface driver parses input from devices, and a misbehaving device must
 * never stall OSC control or the mixer.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <systemd/sd-bus.h>

#include "bl_graph.h"
#include "bl_meters.h"
#include "bl_midibuf.h"
#include "bl_midi_pw.h"
#include "engine.h"
#include "mcu.h"
#include "params.h"

#define BUS_NAME "io.github.manuaudio.Backline.Surface"
#define OBJ_PATH "/io/github/manuaudio/Backline/Surface"
#define IFACE "io.github.manuaudio.Backline.Surface1"
#define CTL_NAME "io.github.manuaudio.Backline.Control"
#define PRM_PATH "/io/github/manuaudio/Backline/Params"
#define PRM_IFACE "io.github.manuaudio.Backline.Params1"
#define ERR_NOTFOUND "io.github.manuaudio.Backline.Error.NotFound"
#define ERR_STATE "io.github.manuaudio.Backline.Error.State"

#define MAX_DEV 16
#define MAX_ROOTS 16
#define MAX_INFLIGHT 128
#define MS 1000000ull

/* A device: a pair of MIDI ports (to and from it) on one node, and our node
 * that talks to it. */
struct dport {
	bool used;
	uint32_t node_id;
	char alias[256];
	char hay[700];
	uint32_t dev_out, dev_in;        /* the device's ports: it sends on dev_out */
	const struct sf_profile *prof;
	struct pw_filter *filter;
	struct spa_hook listener;
	void *in_port, *out_port;
	uint32_t node, my_in, my_out;    /* our node's ids, once known */
	bool linked;
	int unit;
	struct bl_midi_ring rx, tx;
	struct bl_midi_parser parser;
};

struct meters {
	bool used;
	char root[PRM_SEG_MAX + 2];
	const struct bl_meters_hdr *map;
	size_t len;
	char (*ids)[PRM_SEG_MAX + 1];
	uint32_t n;
	struct bl_meter *cache;
	uint64_t read_ns;
	bool asked;
};

struct inflight {
	bool used, busy, has_pending;
	char path[PRM_PATH_MAX];
	double pending;
	char role[32], client[160];
};

static struct {
	struct pw_main_loop *loop;
	struct pw_loop *l;
	struct pw_context *context;
	struct pw_core *core;
	struct bl_graph *graph;
	sd_bus *bus;
	struct spa_source *bus_io, *tick, *rx_ev, *save_timer;
	struct sf_engine *eng;
	struct prm_store *params;
	struct dport devs[MAX_DEV];
	int unit_dev[SF_MAX_UNITS];
	int attaching;
	struct meters meters[MAX_ROOTS];
	struct inflight inflight[MAX_INFLIGHT];
	bool params_up, listing;
	char prefix[96];
	char state_file[700];
	char *profile_dirs[8];
	int n_profile_dirs;
	regex_t only;
	bool only_on;
	bool verbose;
	int n_nodes;
	uint64_t midi_in, midi_out;
	bool quitting;
} S;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "backline-surfaced: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)
#define VLOG(...) do { if (S.verbose) logf_(__VA_ARGS__); } while (0)

/* ============================================================ unit nodes */
/* The device's MIDI goes into a ring the main loop drains. */
static void rx_push(void *data, const uint8_t *m, size_t n)
{
	struct dport *d = data;
	bl_midi_ring_push(&d->rx, m, n);
}

static void on_process_rx(void *data, struct spa_io_position *pos)
{
	struct dport *d = data;
	if (d->out_port) {
		struct pw_buffer *b = pw_filter_dequeue_buffer(d->out_port);
		if (b) {
			bl_midi_pw_write(&b->buffer->datas[0], &d->tx);
			pw_filter_queue_buffer(d->out_port, b);
		}
	}
	if (d->in_port) {
		struct pw_buffer *b = pw_filter_dequeue_buffer(d->in_port);
		if (b) {
			uint32_t before = atomic_load_explicit(&d->rx.w, memory_order_relaxed);
			bl_midi_pw_read(&b->buffer->datas[0], rx_push, d);
			pw_filter_queue_buffer(d->in_port, b);
			if (atomic_load_explicit(&d->rx.w, memory_order_relaxed) != before)
				pw_loop_signal_event(S.l, S.rx_ev);
		}
	}
}

static void on_filter_state(void *data, enum pw_filter_state old, enum pw_filter_state st, const char *error)
{
	struct dport *d = data;
	if (st == PW_FILTER_STATE_ERROR)
		LOG("%s: node error: %s\n", d->alias, error ? error : "?");
	if ((st == PW_FILTER_STATE_PAUSED || st == PW_FILTER_STATE_STREAMING) && d->node == SPA_ID_INVALID)
		d->node = pw_filter_get_node_id(d->filter);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_filter_state,
	.process = on_process_rx,
};

static void try_link(struct dport *d)
{
	if (d->linked || d->node == SPA_ID_INVALID || d->my_in == SPA_ID_INVALID || d->my_out == SPA_ID_INVALID)
		return;
	bl_graph_link(S.graph, d->dev_out, d->my_in);
	bl_graph_link(S.graph, d->my_out, d->dev_in);
	d->linked = true;
	VLOG("%s: linked\n", d->alias);
}

static void sanitize(const char *in, char *out, size_t n)
{
	size_t k = 0;
	for (; *in && k + 1 < n; in++)
		out[k++] = (*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z') || (*in >= '0' && *in <= '9') || *in == '-' ? *in : '_';
	out[k] = 0;
}

static void dev_open(struct dport *d)
{
	char name[200], tail[64];
	sanitize(d->alias, tail, sizeof(tail));
	snprintf(name, sizeof(name), "%s.%d.%s", S.prefix, ++S.n_nodes, tail);
	bl_midi_ring_init(&d->rx, 1 << 15);
	bl_midi_ring_init(&d->tx, 1 << 16);
	bl_midi_parser_init(&d->parser);
	d->node = d->my_in = d->my_out = SPA_ID_INVALID;
	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Midi", PW_KEY_MEDIA_CATEGORY, "Duplex", PW_KEY_MEDIA_CLASS, "Midi/Bridge",
		PW_KEY_NODE_NAME, name, PW_KEY_NODE_DESCRIPTION, "Backline surface", PW_KEY_NODE_AUTOCONNECT, "false",
		"node.passive", "true",
		"backline.dbus.name", BUS_NAME, "backline.surface.profile", d->prof->id, NULL);
	d->filter = pw_filter_new(S.core, name, props);
	pw_filter_add_listener(d->filter, &d->listener, &filter_events, d);
	d->in_port = pw_filter_add_port(d->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
					pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "from_device", NULL), NULL, 0);
	d->out_port = pw_filter_add_port(d->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
					 pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "to_device", NULL), NULL, 0);
	if (pw_filter_connect(d->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0)
		LOG("%s: cannot connect its node\n", d->alias);
	S.attaching = (int)(d - S.devs);
	d->unit = sf_unit_attach(S.eng, d->prof, d->hay, now_ns());
	S.attaching = -1;
	if (d->unit >= 0)
		S.unit_dev[d->unit] = (int)(d - S.devs);
	LOG("found %s: %s (%s)\n", d->prof->name, d->alias, name);
}

static void dev_close(struct dport *d, bool unplugged)
{
	if (d->unit >= 0) {
		if (unplugged)
			sf_unit_detach(S.eng, d->unit, now_ns());
		S.unit_dev[d->unit] = -1;
	}
	if (d->filter) {
		spa_hook_remove(&d->listener);
		pw_filter_destroy(d->filter);
	}
	bl_midi_ring_free(&d->rx);
	bl_midi_ring_free(&d->tx);
	memset(d, 0, sizeof(*d));
}

static bool our_node(uint32_t node)
{
	for (int i = 0; i < MAX_DEV; i++)
		if (S.devs[i].used && S.devs[i].node == node)
			return true;
	return false;
}

static void on_port_added(void *data, const struct bl_port *p)
{
	if (!p->midi)
		return;
	/* our own nodes' ports: link once both are known */
	for (int i = 0; i < MAX_DEV; i++) {
		struct dport *d = &S.devs[i];
		if (!d->used || d->node == SPA_ID_INVALID || p->node_id != d->node)
			continue;
		if (p->output)
			d->my_out = p->id;
		else
			d->my_in = p->id;
		try_link(d);
		return;
	}
	if (our_node(p->node_id) || !strncmp(p->node_name, S.prefix, strlen(S.prefix)))
		return;
	char hay[700];
	snprintf(hay, sizeof(hay), "%s:%s | %s", p->node_name, p->name, p->alias);
	if (S.only_on && regexec(&S.only, hay, 0, NULL, 0))
		return;
	const struct sf_profile *prof = sf_profile_for_port(S.eng, hay);
	if (!prof)
		return;
	/* pair by node and alias (a duplex device's two ports share both) */
	struct dport *d = NULL, *free_d = NULL;
	for (int i = 0; i < MAX_DEV; i++) {
		if (S.devs[i].used && S.devs[i].node_id == p->node_id && !strcmp(S.devs[i].alias, p->alias))
			d = &S.devs[i];
		else if (!S.devs[i].used && !free_d)
			free_d = &S.devs[i];
	}
	if (!d) {
		if (!free_d)
			return;
		d = free_d;
		memset(d, 0, sizeof(*d));
		d->used = true;
		d->node_id = p->node_id;
		d->unit = -1;
		d->dev_out = d->dev_in = SPA_ID_INVALID;
		snprintf(d->alias, sizeof(d->alias), "%s", p->alias);
		snprintf(d->hay, sizeof(d->hay), "%s", hay);
		d->prof = prof;
	}
	if (p->output)
		d->dev_out = p->id;
	else
		d->dev_in = p->id;
	if (d->dev_out != SPA_ID_INVALID && d->dev_in != SPA_ID_INVALID && !d->filter)
		dev_open(d);
	if (d->filter)
		try_link(d);
}

static void on_port_removed(void *data, uint32_t id)
{
	for (int i = 0; i < MAX_DEV; i++) {
		struct dport *d = &S.devs[i];
		if (!d->used)
			continue;
		if (id == d->dev_out || id == d->dev_in) {
			LOG("%s went away\n", d->alias);
			dev_close(d, true);
		} else if (id == d->my_in || id == d->my_out) {
			if (id == d->my_in) d->my_in = SPA_ID_INVALID;
			else d->my_out = SPA_ID_INVALID;
			d->linked = false;
		}
	}
}

static const struct bl_graph_events graph_events = {
	.port_added = on_port_added,
	.port_removed = on_port_removed,
};

struct rx_ctx {
	int unit;
	uint64_t now;
};

static void rx_msg(void *data, const uint8_t *m, size_t n)
{
	struct rx_ctx *c = data;
	if (m[0] >= 0xf8)
		return; /* realtime bytes: nothing for a surface */
	S.midi_in++;
	sf_unit_input(S.eng, c->unit, m, n, c->now);
}

static void on_rx(void *data, uint64_t count)
{
	uint8_t buf[BL_MIDI_SYSEX_MAX];
	uint64_t now = now_ns();
	for (int i = 0; i < MAX_DEV; i++) {
		struct dport *d = &S.devs[i];
		if (!d->used || !d->filter)
			continue;
		size_t n;
		struct rx_ctx c = { d->unit, now };
		while ((n = bl_midi_ring_pop(&d->rx, buf, sizeof(buf))) > 0)
			if (d->unit >= 0)
				bl_midi_parse(&d->parser, buf, n, rx_msg, &c);
	}
}

/* ============================================================ Params1 client */
static int on_call_done(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const sd_bus_error *err = sd_bus_message_get_error(m);
	if (err && S.verbose)
		VLOG("Params1: %s\n", err->message ? err->message : err->name);
	return 0;
}

static struct inflight *inflight_of(const char *path)
{
	struct inflight *free_i = NULL;
	for (int i = 0; i < MAX_INFLIGHT; i++) {
		if (S.inflight[i].used && !strcmp(S.inflight[i].path, path))
			return &S.inflight[i];
		if (!S.inflight[i].used && !free_i)
			free_i = &S.inflight[i];
	}
	if (free_i) {
		memset(free_i, 0, sizeof(*free_i));
		free_i->used = true;
		snprintf(free_i->path, sizeof(free_i->path), "%s", path);
	}
	return free_i;
}

static void send_set(struct inflight *f, double norm);

static int on_set_done(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	struct inflight *f = ud;
	on_call_done(m, NULL, e);
	f->busy = false;
	if (f->has_pending) {
		f->has_pending = false;
		send_set(f, f->pending);
	} else {
		f->used = false;
	}
	return 0;
}

static void send_set(struct inflight *f, double norm)
{
	sd_bus_message *msg = NULL;
	int r = sd_bus_message_new_method_call(S.bus, &msg, CTL_NAME, PRM_PATH, PRM_IFACE, "Set");
	if (r >= 0)
		r = sd_bus_message_append(msg, "sv", f->path, "d", norm);
	if (r >= 0)
		r = sd_bus_message_append(msg, "a{sv}", 3, "norm", "b", 1, "role", "s", f->role, "client", "s", f->client);
	if (r >= 0)
		r = sd_bus_call_async(S.bus, NULL, msg, on_set_done, f, 5 * 1000000ull);
	sd_bus_message_unref(msg);
	if (r >= 0)
		f->busy = true;
	else
		f->used = false;
}

/* Sets go out one at a time per path; while one is in flight the newest
 * value waits, so a fast fader never queues a backlog on the bus. */
static void h_set(void *data, const char *path, double norm, const char *role, const char *client)
{
	if (!S.params_up)
		return;
	struct inflight *f = inflight_of(path);
	if (!f)
		return;
	snprintf(f->role, sizeof(f->role), "%s", role);
	snprintf(f->client, sizeof(f->client), "surface:%s", client);
	if (f->busy) {
		f->pending = norm;
		f->has_pending = true;
		return;
	}
	send_set(f, norm);
}

static void h_gesture(void *data, const char *path, bool begin, const char *role, const char *client)
{
	if (!S.params_up)
		return;
	char cl[200];
	snprintf(cl, sizeof(cl), "surface:%s", client);
	sd_bus_message *msg = NULL;
	int r = sd_bus_message_new_method_call(S.bus, &msg, CTL_NAME, PRM_PATH, PRM_IFACE, "Gesture");
	if (r >= 0)
		r = sd_bus_message_append(msg, "sb", path, begin);
	if (r >= 0)
		r = sd_bus_message_append(msg, "a{sv}", 2, "role", "s", role, "client", "s", cl);
	if (r >= 0)
		sd_bus_call_async(S.bus, NULL, msg, on_call_done, NULL, 5 * 1000000ull);
	sd_bus_message_unref(msg);
	VLOG("gesture %s %s (%s)\n", begin ? "begin" : "end", path, client);
}

static void meters_drop(struct meters *m)
{
	if (m->map)
		munmap((void *)m->map, m->len);
	free(m->ids);
	free(m->cache);
	memset(m, 0, sizeof(*m));
}

static int on_meter_table(sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	struct meters *m = ud;
	int fd;
	if (sd_bus_message_get_error(reply) || sd_bus_message_read(reply, "h", &fd) < 0)
		return 0;
	const char **ids = NULL;
	int n = 0;
	if (sd_bus_message_enter_container(reply, 'a', "s") >= 0) {
		const char *s;
		ids = calloc(1024, sizeof(*ids));
		while (n < 1024 && sd_bus_message_read(reply, "s", &s) > 0)
			ids[n++] = s;
	}
	size_t len;
	const struct bl_meters_hdr *h = bl_meters_map(fd, (uint32_t)n, &len);
	if (!h) {
		LOG("%s: the meter table is not usable\n", m->root);
		free(ids);
		return 0;
	}
	if (m->map)
		munmap((void *)m->map, m->len);
	free(m->ids);
	free(m->cache);
	m->map = h;
	m->len = len;
	m->n = (uint32_t)n;
	m->ids = calloc((size_t)(n ? n : 1), sizeof(*m->ids));
	m->cache = calloc((size_t)(n ? n : 1), sizeof(*m->cache));
	for (int i = 0; i < n; i++)
		snprintf(m->ids[i], sizeof(m->ids[i]), "%s", ids[i]);
	free(ids);
	VLOG("%s: meter table of %d meters\n", m->root, n);
	return 0;
}

static void ask_meters(const char *root)
{
	struct meters *m = NULL, *free_m = NULL;
	for (int i = 0; i < MAX_ROOTS; i++) {
		if (S.meters[i].used && !strcmp(S.meters[i].root, root))
			m = &S.meters[i];
		else if (!S.meters[i].used && !free_m)
			free_m = &S.meters[i];
	}
	if (!m) {
		if (!free_m)
			return;
		m = free_m;
		m->used = true;
		snprintf(m->root, sizeof(m->root), "%s", root);
	}
	sd_bus_call_method_async(S.bus, NULL, CTL_NAME, PRM_PATH, PRM_IFACE, "MeterTable", on_meter_table, m, "s", root);
}

static bool h_meter(void *data, const char *root, const char *id, float *db, bool *clip)
{
	for (int i = 0; i < MAX_ROOTS; i++) {
		struct meters *m = &S.meters[i];
		if (!m->used || !m->map || strcmp(m->root, root))
			continue;
		uint64_t now = now_ns();
		if (now - m->read_ns > 30 * MS) { /* read the table at most ~30 times a second */
			bl_meters_read(m->map, m->n, m->cache);
			m->read_ns = now;
		}
		for (uint32_t k = 0; k < m->n; k++)
			if (!strcmp(m->ids[k], id)) {
				*db = m->cache[k].db;
				*clip = m->cache[k].flags & BL_METER_CLIP;
				return true;
			}
	}
	return false;
}

static int on_list(sd_bus_message *reply, void *ud, sd_bus_error *e)
{
	S.listing = false;
	const char *js;
	if (sd_bus_message_get_error(reply) || sd_bus_message_read(reply, "s", &js) < 0) {
		S.params_up = false;
		return 0;
	}
	json_object *o = json_tokener_parse(js), *a, *roots;
	if (!o || !json_object_object_get_ex(o, "params", &a)) {
		json_object_put(o);
		return 0;
	}
	struct prm_store *fresh = prm_store_new();
	int n = 0;
	for (size_t i = 0; i < json_object_array_length(a); i++) {
		json_object *po = json_object_array_get_idx(a, i), *pv;
		if (!json_object_object_get_ex(po, "path", &pv))
			continue;
		char root[PRM_SEG_MAX + 2], err[256];
		struct prm *p;
		if (!prm_path_root(json_object_get_string(pv), root, sizeof(root)) || prm_from_json(po, root, &p, err, sizeof(err)) < 0)
			continue;
		prm_store_put(fresh, p);
		n++;
	}
	/* the engine keeps its pointer: swap the contents */
	struct prm_store *old = S.params;
	S.params = fresh;
	sf_set_params(S.eng, fresh);
	prm_store_free(old);
	if (json_object_object_get_ex(o, "roots", &roots))
		for (size_t i = 0; i < json_object_array_length(roots); i++) {
			json_object *r = json_object_array_get_idx(roots, i), *x;
			if (json_object_object_get_ex(r, "meter-table", &x) && json_object_get_boolean(x) &&
			    json_object_object_get_ex(r, "root", &x))
				ask_meters(json_object_get_string(x));
		}
	json_object_put(o);
	S.params_up = true;
	VLOG("Params1: %d parameters\n", n);
	sd_bus_emit_properties_changed(S.bus, OBJ_PATH, IFACE, "ParamsConnected", NULL);
	return 0;
}

static void relist(void)
{
	if (S.listing)
		return;
	S.listing = sd_bus_call_method_async(S.bus, NULL, CTL_NAME, PRM_PATH, PRM_IFACE, "List", on_list, NULL, "s", "/**") >= 0;
}

static int on_changed(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *root;
	if (sd_bus_message_read(m, "s", &root) < 0 || sd_bus_message_enter_container(m, 'a', "(svds)") < 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'r', "svds") > 0) {
		const char *path, *text, *c;
		char t;
		double norm;
		sd_bus_message_read(m, "s", &path);
		struct prm *p = prm_store_get(S.params, path);
		sd_bus_message_peek_type(m, &t, &c);
		sd_bus_message_enter_container(m, 'v', c);
		double v = 0;
		const char *str = NULL;
		switch (c[0]) {
		case 'd': sd_bus_message_read(m, "d", &v); break;
		case 'i': { int32_t x; sd_bus_message_read(m, "i", &x); v = x; break; }
		case 'b': { int x; sd_bus_message_read(m, "b", &x); v = x; break; }
		case 's': sd_bus_message_read(m, "s", &str); break;
		default: sd_bus_message_skip(m, c);
		}
		sd_bus_message_exit_container(m);
		sd_bus_message_read(m, "ds", &norm, &text);
		sd_bus_message_exit_container(m);
		if (!p)
			continue;
		if (p->type == PRM_STRING && str)
			snprintf(p->str, sizeof(p->str), "%s", str);
		else
			p->value = v;
		snprintf(p->text, sizeof(p->text), "%s", text);
		p->text_override = true;
	}
	return 0;
}

static int on_structure(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	relist();
	return 0;
}

static int on_owner(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *name, *old, *new_;
	if (sd_bus_message_read(m, "sss", &name, &old, &new_) < 0 || strcmp(name, CTL_NAME))
		return 0;
	if (*new_) {
		LOG("controld is on the bus: reading Params1\n");
		relist();
	} else {
		LOG("controld left the bus: parameters are gone until it is back\n");
		S.params_up = false;
		for (int i = 0; i < MAX_ROOTS; i++)
			if (S.meters[i].used)
				meters_drop(&S.meters[i]);
		struct prm_store *fresh = prm_store_new(), *old = S.params;
		S.params = fresh;
			sf_set_params(S.eng, fresh);
		prm_store_free(old);
		sd_bus_emit_properties_changed(S.bus, OBJ_PATH, IFACE, "ParamsConnected", NULL);
	}
	return 0;
}

/* ============================================================ engine hooks */
static void h_send(void *data, int unit, const uint8_t *msg, size_t n)
{
	int di = S.unit_dev[unit] >= 0 ? S.unit_dev[unit] : S.attaching;
	if (di < 0 || !S.devs[di].used)
		return;
	if (bl_midi_ring_push(&S.devs[di].tx, msg, n))
		S.midi_out++;
}

static void h_action(void *data, const char *group, const char *name, bool down)
{
	sd_bus_emit_signal(S.bus, OBJ_PATH, IFACE, "Action", "ssb", group, name, down);
	VLOG("action %s %s %s\n", group, name, down ? "down" : "up");
}

static void h_navigated(void *data, const char *group, const char *view, int offset, const char *mode, int page)
{
	sd_bus_emit_signal(S.bus, OBJ_PATH, IFACE, "Navigated", "ssisi", group, view, offset, mode, page);
}

static void h_unit(void *data, const char *unit, bool added)
{
	sd_bus_emit_signal(S.bus, OBJ_PATH, IFACE, added ? "UnitAdded" : "UnitRemoved", "s", unit);
	sd_bus_emit_properties_changed(S.bus, OBJ_PATH, IFACE, "UnitCount", "Groups", NULL);
}

static void h_state(void *data)
{
	if (!S.save_timer)
		return;
	struct timespec t = { 1, 0 };
	pw_loop_update_timer(S.l, S.save_timer, &t, NULL, false);
}

static void h_log(void *data, const char *m)
{
	LOG("%s\n", m);
}

static int mkdir_p(const char *path)
{
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
				return -errno;
			*p = '/';
		}
	return 0;
}

static void on_save(void *data, uint64_t exp)
{
	char *js = sf_state_json(S.eng);
	char tmp[800];
	mkdir_p(S.state_file);
	snprintf(tmp, sizeof(tmp), "%s.tmp", S.state_file);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fputs(js, f);
		fputc('\n', f);
		bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
		fclose(f);
		if (ok)
			rename(tmp, S.state_file);
	}
	free(js);
}

static void on_tick(void *data, uint64_t exp)
{
	sf_tick(S.eng, now_ns());
	sd_bus_flush(S.bus);
}

/* ============================================================ D-Bus API */
static int append_json_dict(sd_bus_message *m, json_object *o)
{
	int r = sd_bus_message_open_container(m, 'a', "{sv}");
	json_object_object_foreach(o, k, v) {
		if (r < 0)
			break;
		r = sd_bus_message_open_container(m, 'e', "sv");
		if (r >= 0)
			r = sd_bus_message_append(m, "s", k);
		switch (json_object_get_type(v)) {
		case json_type_boolean: r = sd_bus_message_append(m, "v", "b", json_object_get_boolean(v)); break;
		case json_type_int: {
			int64_t x = json_object_get_int64(v);
			if (!strcmp(k, "position") || !strcmp(k, "offset"))
				r = sd_bus_message_append(m, "v", "i", (int32_t)x);
			else
				r = sd_bus_message_append(m, "v", "u", (uint32_t)(x < 0 ? 0 : x));
			break;
		}
		case json_type_array: {
			r = sd_bus_message_open_container(m, 'v', "as");
			if (r >= 0)
				r = sd_bus_message_open_container(m, 'a', "s");
			for (size_t i = 0; r >= 0 && i < json_object_array_length(v); i++)
				r = sd_bus_message_append(m, "s", json_object_get_string(json_object_array_get_idx(v, i)));
			if (r >= 0)
				r = sd_bus_message_close_container(m);
			if (r >= 0)
				r = sd_bus_message_close_container(m);
			break;
		}
		default: r = sd_bus_message_append(m, "v", "s", json_object_get_string(v)); break;
		}
		if (r >= 0)
			r = sd_bus_message_close_container(m);
	}
	return r < 0 ? r : sd_bus_message_close_container(m);
}

static int reply_list(sd_bus_message *m, char *js)
{
	json_object *a = json_tokener_parse(js);
	free(js);
	sd_bus_message *reply = NULL;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0)
		r = sd_bus_message_open_container(reply, 'a', "a{sv}");
	for (size_t i = 0; r >= 0 && a && i < json_object_array_length(a); i++)
		r = append_json_dict(reply, json_object_array_get_idx(a, i));
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	json_object_put(a);
	return r;
}

static int m_units(sd_bus_message *m, void *ud, sd_bus_error *e) { return reply_list(m, sf_units_json(S.eng)); }
static int m_groups(sd_bus_message *m, void *ud, sd_bus_error *e) { return reply_list(m, sf_groups_json(S.eng)); }

static int status(sd_bus_message *m, sd_bus_error *e, int r, const char *err)
{
	if (r == -2)
		return sd_bus_error_setf(e, ERR_NOTFOUND, "%s", err);
	if (r == -3)
		return sd_bus_error_setf(e, ERR_STATE, "%s", err);
	if (r < 0)
		return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "%s", err);
	return sd_bus_reply_method_return(m, "");
}

static int m_set_group(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *group;
	char **units = NULL;
	int r = sd_bus_message_read(m, "s", &group);
	if (r < 0 || (r = sd_bus_message_read_strv(m, &units)) < 0)
		return r;
	const char *role = NULL, *lock = NULL;
	int32_t lock_off = 0;
	bool set_lock = false;
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		goto out;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k, *sig = NULL;
		sd_bus_message_read(m, "s", &k);
		sd_bus_message_peek_type(m, NULL, &sig);
		if (!strcmp(k, "role") && sig && !strcmp(sig, "s")) {
			sd_bus_message_read(m, "v", "s", &role);
		} else if (!strcmp(k, "lock-view") && sig && !strcmp(sig, "s")) {
			sd_bus_message_read(m, "v", "s", &lock);
			set_lock = true;
		} else if (!strcmp(k, "lock-offset") && sig && !strcmp(sig, "i")) {
			sd_bus_message_read(m, "v", "i", &lock_off);
		} else {
			r = sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "unknown option '%s' (or wrong type)", k);
			goto out;
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	int n = 0;
	while (units && units[n])
		n++;
	char err[256];
	r = status(m, e, sf_set_group(S.eng, group, (const char *const *)units, n, role, lock, lock_off, set_lock, err, sizeof(err)), err);
	sd_bus_emit_properties_changed(S.bus, OBJ_PATH, IFACE, "Groups", NULL);
out:
	if (units) {
		for (int i = 0; units[i]; i++)
			free(units[i]);
		free(units);
	}
	return r;
}

#define SIMPLE1(fn, call)                                                            \
	static int fn(sd_bus_message *m, void *ud, sd_bus_error *e)                  \
	{                                                                             \
		const char *g;                                                        \
		int r = sd_bus_message_read(m, "s", &g);                              \
		if (r < 0)                                                            \
			return r;                                                     \
		char err[256];                                                        \
		return status(m, e, call, err);                                       \
	}
#define SIMPLE2(fn, call)                                                            \
	static int fn(sd_bus_message *m, void *ud, sd_bus_error *e)                  \
	{                                                                             \
		const char *g, *a;                                                    \
		int r = sd_bus_message_read(m, "ss", &g, &a);                         \
		if (r < 0)                                                            \
			return r;                                                     \
		char err[256];                                                        \
		return status(m, e, call, err);                                       \
	}

SIMPLE1(m_remove_group, sf_remove_group(S.eng, g, err, sizeof(err)))
SIMPLE1(m_pop_view, sf_pop_view(S.eng, g, err, sizeof(err)))
SIMPLE2(m_set_views, sf_set_views(S.eng, g, a, err, sizeof(err)))
SIMPLE2(m_push_view, sf_push_view(S.eng, g, a, err, sizeof(err)))
SIMPLE2(m_show_view, sf_show_view(S.eng, g, a, err, sizeof(err)))
SIMPLE2(m_set_selected, sf_set_selected(S.eng, g, a, err, sizeof(err)))

static int m_set_window(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *g;
	int32_t off;
	int r = sd_bus_message_read(m, "si", &g, &off);
	if (r < 0)
		return r;
	char err[256];
	return status(m, e, sf_set_window(S.eng, g, off, err, sizeof(err)), err);
}

static int m_set_pot_mode(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *g, *mode;
	int32_t page;
	int r = sd_bus_message_read(m, "ssi", &g, &mode, &page);
	if (r < 0)
		return r;
	char err[256];
	return status(m, e, sf_set_pot_mode(S.eng, g, mode, page, err, sizeof(err)), err);
}

static int m_set_flip(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *g;
	int on;
	int r = sd_bus_message_read(m, "sb", &g, &on);
	if (r < 0)
		return r;
	char err[256];
	return status(m, e, sf_set_flip(S.eng, g, on, err, sizeof(err)), err);
}

static int m_set_binding(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *g, *c, *b;
	int r = sd_bus_message_read(m, "sss", &g, &c, &b);
	if (r < 0)
		return r;
	char err[512];
	return status(m, e, sf_set_binding(S.eng, g, c, b, err, sizeof(err)), err);
}

static int m_text(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *g, *u, *t;
	uint32_t ms;
	int r = sd_bus_message_read(m, "sssu", &g, &u, &t, &ms);
	if (r < 0)
		return r;
	char err[256];
	return status(m, e, sf_text(S.eng, g, u, t, ms, now_ns(), err, sizeof(err)), err);
}

static int m_profiles(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	json_object *o = json_object_new_object(), *a = json_object_new_array();
	json_object_object_add(o, "schema", json_object_new_string("backline.surface/1"));
	for (int i = 0; i < sf_n_profiles(S.eng); i++)
		json_object_array_add(a, sf_profile_to_json(sf_profile_at(S.eng, i)));
	json_object_object_add(o, "profiles", a);
	int r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return r;
}

static int p_get(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
		 sd_bus_error *e)
{
	if (!strcmp(prop, "UnitCount"))
		return sd_bus_message_append(reply, "u", (uint32_t)sf_units_online(S.eng));
	if (!strcmp(prop, "ParamsConnected"))
		return sd_bus_message_append(reply, "b", S.params_up);
	if (!strcmp(prop, "MidiIn"))
		return sd_bus_message_append(reply, "t", S.midi_in);
	if (!strcmp(prop, "MidiOut"))
		return sd_bus_message_append(reply, "t", S.midi_out);
	int r = sd_bus_message_open_container(reply, 'a', "s");
	if (!strcmp(prop, "Groups")) {
		const char *ids[SF_MAX_GROUPS];
		int n = sf_group_ids(S.eng, ids, SF_MAX_GROUPS);
		for (int i = 0; r >= 0 && i < n; i++)
			r = sd_bus_message_append(reply, "s", ids[i]);
	} else {
		for (int i = 0; r >= 0 && i < S.n_profile_dirs; i++)
			r = sd_bus_message_append(reply, "s", S.profile_dirs[i]);
	}
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("Units", SD_BUS_NO_ARGS, SD_BUS_RESULT("aa{sv}", units), m_units, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Groups", SD_BUS_NO_ARGS, SD_BUS_RESULT("aa{sv}", groups), m_groups, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetGroup", SD_BUS_ARGS("s", group, "as", units, "a{sv}", options), SD_BUS_NO_RESULT, m_set_group, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("RemoveGroup", SD_BUS_ARGS("s", group), SD_BUS_NO_RESULT, m_remove_group, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetViews", SD_BUS_ARGS("s", group, "s", views_json), SD_BUS_NO_RESULT, m_set_views, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("PushView", SD_BUS_ARGS("s", group, "s", view), SD_BUS_NO_RESULT, m_push_view, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("PopView", SD_BUS_ARGS("s", group), SD_BUS_NO_RESULT, m_pop_view, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("ShowView", SD_BUS_ARGS("s", group, "s", view), SD_BUS_NO_RESULT, m_show_view, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetWindow", SD_BUS_ARGS("s", group, "i", offset), SD_BUS_NO_RESULT, m_set_window, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetPotMode", SD_BUS_ARGS("s", group, "s", mode, "i", page), SD_BUS_NO_RESULT, m_set_pot_mode, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetSelected", SD_BUS_ARGS("s", group, "s", slot), SD_BUS_NO_RESULT, m_set_selected, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetFlip", SD_BUS_ARGS("s", group, "b", on), SD_BUS_NO_RESULT, m_set_flip, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetGlobalBinding", SD_BUS_ARGS("s", group, "s", control, "s", binding), SD_BUS_NO_RESULT, m_set_binding, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Text", SD_BUS_ARGS("s", group, "s", unit, "s", text, "u", ms), SD_BUS_NO_RESULT, m_text, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Profiles", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", json), m_profiles, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("UnitCount", "u", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Groups", "as", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ParamsConnected", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ProfileDirs", "as", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("MidiIn", "t", p_get, 0, 0),
	SD_BUS_PROPERTY("MidiOut", "t", p_get, 0, 0),
	SD_BUS_SIGNAL_WITH_ARGS("Navigated", SD_BUS_ARGS("s", group, "s", view, "i", offset, "s", mode, "i", page), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Action", SD_BUS_ARGS("s", group, "s", name, "b", down), 0),
	SD_BUS_SIGNAL_WITH_ARGS("UnitAdded", SD_BUS_ARGS("s", unit), 0),
	SD_BUS_SIGNAL_WITH_ARGS("UnitRemoved", SD_BUS_ARGS("s", unit), 0),
	SD_BUS_VTABLE_END
};

/* ============================================================ main */
static void on_bus_io(void *data, int fd, uint32_t mask)
{
	while (sd_bus_process(S.bus, NULL) > 0) {}
	sd_bus_flush(S.bus);
}

static void on_signal(void *data, int sig)
{
	pw_main_loop_quit(S.loop);
}

static void usage(FILE *f)
{
	fputs("usage: backline-surfaced [--name PREFIX] [--profile-dir DIR]... [--state-file FILE] [--only REGEX]\n"
	      "                         [--replace] [--verbose]\n"
	      "  Control surfaces for any program, on the session bus (" BUS_NAME ", " IFACE ").\n"
	      "  Parameters come from controld's Params1 (docs/api/Params1.md, Surface1.md).\n"
	      "  --name PREFIX     node names (default backline.surface)\n"
	      "  --profile-dir DIR more *.surface.json (before the user's and the shipped ones)\n"
	      "  --state-file FILE groups (default $XDG_STATE_HOME/backline/surfaces.json)\n"
	      "  --only REGEX      consider only MIDI ports whose node:port | alias matches\n", f);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "profile-dir", required_argument, NULL, 'p' },
		{ "state-file", required_argument, NULL, 's' },
		{ "only", required_argument, NULL, 'o' },
		{ "replace", no_argument, NULL, 'r' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c, replace = 0;
	snprintf(S.prefix, sizeof(S.prefix), "backline.surface");
	S.attaching = -1;
	for (int i = 0; i < SF_MAX_UNITS; i++)
		S.unit_dev[i] = -1;
	while ((c = getopt_long(argc, argv, "n:p:s:o:rvh", opts, NULL)) != -1) {
		switch (c) {
		case 'n': snprintf(S.prefix, sizeof(S.prefix), "%s", optarg); break;
		case 'p': if (S.n_profile_dirs < 5) S.profile_dirs[S.n_profile_dirs++] = strdup(optarg); break;
		case 's': snprintf(S.state_file, sizeof(S.state_file), "%s", optarg); break;
		case 'o':
			if (regcomp(&S.only, optarg, REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
				fprintf(stderr, "backline-surfaced: bad --only regex\n");
				return 2;
			}
			S.only_on = true;
			break;
		case 'r': replace = 1; break;
		case 'v': S.verbose = true; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	const char *xc = getenv("XDG_CONFIG_HOME"), *xs = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
	char buf[700];
	if (xc && *xc)
		snprintf(buf, sizeof(buf), "%s/backline/surfaces", xc);
	else
		snprintf(buf, sizeof(buf), "%s/.config/backline/surfaces", home ? home : ".");
	S.profile_dirs[S.n_profile_dirs++] = strdup(buf);
	struct stat st;
	S.profile_dirs[S.n_profile_dirs++] = strdup(stat(BL_SURFACE_DATADIR, &st) == 0 ? BL_SURFACE_DATADIR : BL_SURFACE_SRCDIR);
	if (!S.state_file[0]) {
		if (xs && *xs)
			snprintf(S.state_file, sizeof(S.state_file), "%s/backline/surfaces.json", xs);
		else
			snprintf(S.state_file, sizeof(S.state_file), "%s/.local/state/backline/surfaces.json", home ? home : ".");
	}

	S.params = prm_store_new();
	struct sf_host host = { h_send, h_set, h_gesture, h_action, h_navigated, h_unit, h_meter, h_state, h_log };
	S.eng = sf_new(&host, NULL, S.params);
	int np = 0;
	for (int i = 0; i < S.n_profile_dirs; i++)
		np += sf_load_profile_dir(S.eng, S.profile_dirs[i]);
	LOG("%d profiles\n", np);
	pw_init(&argc, &argv);
	S.loop = pw_main_loop_new(NULL);
	S.l = pw_main_loop_get_loop(S.loop);
	pw_loop_add_signal(S.l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(S.l, SIGTERM, on_signal, NULL);
	S.context = pw_context_new(S.l, NULL, 0);
	S.core = pw_context_connect(S.context, NULL, 0);
	if (!S.core) {
		fprintf(stderr, "backline-surfaced: cannot connect to PipeWire\n");
		return 1;
	}
	S.rx_ev = pw_loop_add_event(S.l, on_rx, NULL);
	int r = sd_bus_open_user(&S.bus);
	if (r < 0) {
		fprintf(stderr, "backline-surfaced: session bus: %s\n", strerror(-r));
		return 1;
	}
	if ((r = sd_bus_add_object_vtable(S.bus, NULL, OBJ_PATH, IFACE, vtable, NULL)) < 0) {
		fprintf(stderr, "backline-surfaced: %s\n", strerror(-r));
		return 1;
	}
	sd_bus_match_signal(S.bus, NULL, CTL_NAME, PRM_PATH, PRM_IFACE, "Changed", on_changed, NULL);
	sd_bus_match_signal(S.bus, NULL, CTL_NAME, PRM_PATH, PRM_IFACE, "Structure", on_structure, NULL);
	sd_bus_match_signal(S.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
			    "NameOwnerChanged", on_owner, NULL);
	S.bus_io = pw_loop_add_io(S.l, sd_bus_get_fd(S.bus), SPA_IO_IN, false, on_bus_io, NULL);
	S.save_timer = pw_loop_add_timer(S.l, on_save, NULL);
	S.tick = pw_loop_add_timer(S.l, on_tick, NULL);
	struct timespec t5 = { 0, 5 * 1000000L };
	pw_loop_update_timer(S.l, S.tick, &t5, &t5, false);
	FILE *sf = fopen(S.state_file, "r");
	if (sf) {
		char *js = calloc(1, 1 << 20);
		size_t n = fread(js, 1, (1 << 20) - 1, sf);
		fclose(sf);
		char err[256];
		if (n && sf_state_load(S.eng, js, err, sizeof(err)) < 0)
			LOG("%s: %s\n", S.state_file, err);
		free(js);
	}


	relist();
	S.graph = bl_graph_new(S.core, &graph_events, NULL);
	r = sd_bus_request_name(S.bus, BUS_NAME, replace ? SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT
							 : SD_BUS_NAME_ALLOW_REPLACEMENT);
	if (r < 0) {
		fprintf(stderr, "backline-surfaced: cannot own %s: %s\n", BUS_NAME, strerror(-r));
		return 1;
	}
	LOG("on the session bus as %s; node prefix %s; state %s\n", BUS_NAME, S.prefix, S.state_file);
	on_bus_io(NULL, 0, 0);

	pw_main_loop_run(S.loop);

	/* clean shutdown: dark, faders down, offline; give the nodes a few
	 * cycles to send it */
	for (int i = 0; i < MAX_DEV; i++)
		if (S.devs[i].used && S.devs[i].unit >= 0)
			sf_unit_goodbye(S.eng, S.devs[i].unit);
	uint64_t until = now_ns() + 200 * MS;
	while (now_ns() < until)
		pw_loop_iterate(S.l, 10);
	on_save(NULL, 0);
	for (int i = 0; i < MAX_DEV; i++)
		if (S.devs[i].used)
			dev_close(&S.devs[i], false);
	bl_graph_destroy(S.graph);
	sd_bus_flush_close_unref(S.bus);
	pw_core_disconnect(S.core);
	pw_context_destroy(S.context);
	pw_main_loop_destroy(S.loop);
	sf_free(S.eng);
	prm_store_free(S.params);
	pw_deinit();
	return 0;
}
