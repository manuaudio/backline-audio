/* backline-controld: OSC and MIDI to any parameter (docs/api/Control1.md).
 * SPDX-License-Identifier: MIT
 *
 * One process on the PipeWire main loop. sd-bus, the OSC sockets and the
 * MIDI ring's wake-up are all sources on that loop. The only realtime code is
 * the MIDI input port's process callback, which copies MIDI 1.0 messages
 * into a wait-free SPSC ring and signals main.
 *
 * Security (Control1.md "Security"): no socket is opened unless a listener is
 * configured or asked for over the session bus; every listener names a
 * literal IP; a non-loopback listener needs an allow-list, TCP needs a
 * shared secret, UDP needs insecure-udp. A peer can only move existing
 * mappings: nothing received over the network creates, changes or removes a
 * mapping or a listener.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <regex.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>
#include <systemd/sd-bus.h>

#include "bl_graph.h"
#include "bl_midibuf.h"
#include "bl_midi_pw.h"
#include "mapping.h"
#include "midiprof.h"
#include "osc.h"
#include "params.h"
#include "paramsd.h"
#include "policy.h"

#define BUS_NAME "io.github.manuaudio.Backline.Control"
#define OBJ_PATH "/io/github/manuaudio/Backline/Control"
#define IFACE "io.github.manuaudio.Backline.Control1"
#define ERR_BUSY "io.github.manuaudio.Backline.Error.Busy"
#define ERR_NOTFOUND "io.github.manuaudio.Backline.Error.NotFound"
#define ERR_STATE "io.github.manuaudio.Backline.Error.State"
#define INST_PREFIX "io.github.manuaudio.Backline.Instance."
#define INST_PATH "/io/github/manuaudio/Backline/Instance"
#define INST_IFACE "io.github.manuaudio.Backline.Instance1"
#define CLK_NAME "io.github.manuaudio.Backline.Clock"
#define CLK_PATH "/io/github/manuaudio/Backline/Clock"
#define CLK_IFACE "io.github.manuaudio.Backline.Clock1"
#define REC_NAME "io.github.manuaudio.Backline.Recorder"
#define REC_PATH "/io/github/manuaudio/Backline/Recorder"
#define REC_IFACE "io.github.manuaudio.Backline.Recorder1"
#define SCHEMA "backline.control/1"

#define MAX_MAPPINGS 512
#define MAX_ARGS 8
#define MAX_LISTENERS 8
#define MAX_CONNS 64
#define MAX_PEERS 64
#define MAX_VNODES 64
#define MIDI_RING 1024           /* power of two */
#define AUTH_TIMEOUT_NS (5ull * 1000000000ull)
#define TCP_OUT_MAX (4u << 20)   /* bytes queued for one TCP peer before it counts as stuck */
#define BLOCK_AFTER_FAILS 3
#define BLOCK_NS (30ull * 1000000000ull)

/* ------------------------------------------------------------------ types */
struct mapping {
	bool used;
	char id[48];
	char source_str[160];
	char target_str[600];
	struct ctl_source src;
	struct ctl_target tgt;
	bool has_min, has_max, invert;
	double min, max, in_min, in_max;
	char args[MAX_ARGS][256];
	int nargs;
	char origin[256];        /* the config file it came from; "" = made at run time */
	/* run time */
	bool pressed;
	int last_mute;           /* -1 unknown */
	bool inflight, has_pending;
	double pending;          /* the latest output value while a call is in flight */
	bool range_known, range_inflight, have_norm;
	double rmin, rmax, norm_pending;
	uint64_t sends;
	/* Control1 additions (docs/api/Control1.md) */
	enum ctl_encoding enc;
	enum ctl_takeover_mode takeover;
	double step, accel;
	bool has_step;
	int bits;                /* NRPN/RPN data: 7 or 14 */
	bool feedback, gesture_auto;
	char role[32];           /* param: targets */
	struct ctl_hires hires;
	struct ctl_takeover to;
	double cur;              /* the last input-space value 0..1 sent */
	bool cur_known;
	double fb_last;
	bool fb_sent;
	bool gest_open;
	uint64_t gest_last_ns;
};

struct peer {
	bool used;
	struct sockaddr_storage addr;
	uint64_t last_ns, blocked_until;
	struct ctl_bucket bucket;
	int fails;
};

struct listener;
struct conn {
	int fd;
	struct spa_source *src;
	struct sockaddr_storage peer;
	uint8_t *buf;
	size_t have;
	bool authed;
	uint64_t opened_ns;
	struct listener *l;
	/* what the socket did not take yet: frames are never cut, so a slow
	 * reader gets everything in order or is disconnected (TCP_OUT_MAX) */
	uint8_t *out;
	size_t out_off, out_len, out_cap;
	bool out_watch, slow;
};

struct listener {
	bool used;
	char id[128];
	struct ctl_listen_spec spec;
	bool loopback;
	int fd;
	struct spa_source *src;
	struct conn *conns[MAX_CONNS];
	int n_conns;
	struct peer peers[MAX_PEERS];
	char origin[256];
	uint8_t *udp_buf;
};

struct vnode {
	bool used;
	char name[256];
	uint32_t id;
	struct pw_proxy *proxy;
	struct spa_hook listener, proxy_listener;
	int nch;                 /* channel volumes; 0 = not known yet */
	int muted;               /* -1 unknown */
	float gain;              /* the first channel's volume, once known */
	bool have_gain;
	bool pend_vol;
	float pend_gain;
};

#define MAX_ACTIVE_PROFILES 16
struct active_profile {
	bool used;
	char id[MP_NAME_MAX];
	char device[700];        /* the port that matched; "" = loaded by hand with no device */
	bool manual;             /* LoadProfile: stays when the device goes */
	regex_t re;              /* its match, for linking the device */
	bool re_ok;
};

struct midi_ev {
	uint64_t ns;
	uint8_t size;
	uint8_t data[3];
};

static struct {
	struct pw_main_loop *loop;
	struct pw_loop *l;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct bl_graph *graph;
	struct pw_filter *filter;
	struct spa_hook filter_listener;
	void *midi_port, *midi_out_port;
	uint32_t node_id, midi_port_id, midi_out_port_id;
	struct bl_midi_ring out_ring;    /* main -> process callback */
	uint64_t midi_sent;
	struct ctl_nrpn nrpn;
	struct spa_source *tick;
	bool tick_on;
	char node_name[128];
	sd_bus *bus;
	struct spa_source *bus_io, *midi_ev, *house_timer, *learn_timer;

	struct mapping maps[MAX_MAPPINGS];
	int next_id;
	struct listener lis[MAX_LISTENERS];
	struct vnode vnodes[MAX_VNODES];

	/* MIDI ring: data thread writes, main reads */
	struct midi_ev ring[MIDI_RING];
	atomic_uint ring_w, ring_r;
	atomic_uint_fast64_t midi_dropped;

	/* MIDI follow */
	char follow[256];
	regex_t follow_re;
	bool follow_on;
	uint32_t followed[64];
	int n_followed;

	/* MIDI out (feedback) */
	char out_follow[256];
	regex_t out_re;
	bool out_on;
	uint32_t out_linked[64];
	int n_out_linked;

	/* learn */
	bool learning;
	char learn_id[48], learn_target[600], learn_from[8];
	struct mapping learn_tmpl;

	char config_dir[600];
	bool no_config;
	bool no_graph;           /* --no-graph: Params1 and OSC only, no PipeWire */

	/* MIDI learn profiles (docs/api/Control1.md "MIDI profiles") */
	struct mp_set profiles;
	char profile_dirs[2][700];      /* the user's, then the system's */
	int n_profile_dirs;
	struct active_profile active[MAX_ACTIVE_PROFILES];
	int idle_exit;
	uint64_t idle_since;
	bool verbose;

	uint64_t received, midi_events, rejected, auth_failures, malformed, dispatched, target_errors;
} D;

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
	fprintf(stderr, "backline-controld: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)
#define VLOG(...) do { if (D.verbose) logf_(__VA_ARGS__); } while (0)

static void props_changed(const char *first, ...)
{
	if (!D.bus)
		return;
	char *names[8];
	int n = 0;
	va_list ap;
	va_start(ap, first);
	for (const char *s = first; s && n < 7; s = va_arg(ap, const char *))
		names[n++] = (char *)s;
	va_end(ap);
	names[n] = NULL;
	sd_bus_emit_properties_changed_strv(D.bus, OBJ_PATH, IFACE, names);
}

static int n_mappings(void)
{
	int n = 0;
	for (int i = 0; i < MAX_MAPPINGS; i++)
		n += D.maps[i].used;
	return n;
}

static int n_listeners(void)
{
	int n = 0;
	for (int i = 0; i < MAX_LISTENERS; i++)
		n += D.lis[i].used;
	return n;
}

static struct mapping *find_mapping(const char *id)
{
	for (int i = 0; i < MAX_MAPPINGS; i++)
		if (D.maps[i].used && !strcmp(D.maps[i].id, id))
			return &D.maps[i];
	return NULL;
}

/* ============================================================ MIDI input */
static void push_midi(uint64_t ns, const uint8_t *m, size_t size)
{
	if (size < 1 || size > 3 || m[0] == 0xf0)
		return;
	unsigned w = atomic_load_explicit(&D.ring_w, memory_order_relaxed);
	unsigned r = atomic_load_explicit(&D.ring_r, memory_order_acquire);
	if (w - r >= MIDI_RING) {
		atomic_fetch_add_explicit(&D.midi_dropped, 1, memory_order_relaxed);
		return;
	}
	struct midi_ev *e = &D.ring[w & (MIDI_RING - 1)];
	e->ns = ns;
	e->size = (uint8_t)size;
	memcpy(e->data, m, size);
	atomic_store_explicit(&D.ring_w, w + 1, memory_order_release);
}

static void push_ump(uint64_t ns, const void *body, size_t size)
{
#ifdef BL_HAVE_SPA_PARSER_BODY
	const uint32_t *u = body;
	uint64_t state = 0;
	while (size > 0) {
		uint8_t midi[16];
		int sz = spa_ump_to_midi(&u, &size, midi, sizeof(midi), &state);
		if (sz < 0)
			break;
		if (sz > 0)
			push_midi(ns, midi, (size_t)sz);
	}
#else
	uint8_t midi[16];
	int sz = spa_ump_to_midi((uint32_t *)body, size, midi, sizeof(midi));
	if (sz > 0)
		push_midi(ns, midi, (size_t)sz);
#endif
}

static void on_process(void *data, struct spa_io_position *pos)
{
	if (D.midi_out_port) {
		struct pw_buffer *ob = pw_filter_dequeue_buffer(D.midi_out_port);
		if (ob) {
			bl_midi_pw_write(&ob->buffer->datas[0], &D.out_ring);
			pw_filter_queue_buffer(D.midi_out_port, ob);
		}
	}
	if (!D.midi_port)
		return;
	struct pw_buffer *b = pw_filter_dequeue_buffer(D.midi_port);
	if (!b)
		return;
	struct spa_data *d = &b->buffer->datas[0];
	uint64_t base = pos ? pos->clock.nsec : 0;
	uint32_t rate = pos && pos->clock.rate.denom ? pos->clock.rate.denom : 48000;
	unsigned before = atomic_load_explicit(&D.ring_w, memory_order_relaxed);
	if (d->data && d->chunk) {
#ifdef BL_HAVE_SPA_PARSER_BODY
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;
		struct spa_pod_sequence seq;
		const void *seq_body, *c_body;
		struct spa_pod_control c;
		spa_pod_parser_init_from_data(&parser, d->data, d->maxsize, d->chunk->offset, d->chunk->size);
		if (spa_pod_parser_push_sequence_body(&parser, &frame, &seq, &seq_body) >= 0) {
			while (spa_pod_parser_get_control_body(&parser, &c, &c_body) >= 0) {
				uint64_t ns = base + (uint64_t)c.offset * 1000000000ull / rate;
				if (c.type == SPA_CONTROL_Midi)
					push_midi(ns, c_body, c.value.size);
				else if (c.type == SPA_CONTROL_UMP)
					push_ump(ns, c_body, c.value.size);
			}
		}
#else
		struct spa_pod *pod = spa_pod_from_data(d->data, d->maxsize, d->chunk->offset, d->chunk->size);
		if (pod && spa_pod_is_sequence(pod)) {
			struct spa_pod_control *c;
			SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence *)pod, c) {
				uint64_t ns = base + (uint64_t)c->offset * 1000000000ull / rate;
				if (c->type == SPA_CONTROL_Midi)
					push_midi(ns, SPA_POD_BODY(&c->value), SPA_POD_BODY_SIZE(&c->value));
				else if (c->type == SPA_CONTROL_UMP)
					push_ump(ns, SPA_POD_BODY(&c->value), SPA_POD_BODY_SIZE(&c->value));
			}
		}
#endif
	}
	pw_filter_queue_buffer(D.midi_port, b);
	if (atomic_load_explicit(&D.ring_w, memory_order_relaxed) != before)
		pw_loop_signal_event(D.l, D.midi_ev);
}

static void on_filter_state(void *data, enum pw_filter_state old, enum pw_filter_state state, const char *error)
{
	if (state == PW_FILTER_STATE_ERROR)
		LOG("filter error: %s\n", error ? error : "?");
	if ((state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) && D.node_id == SPA_ID_INVALID)
		D.node_id = pw_filter_get_node_id(D.filter);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_filter_state,
	.process = on_process,
};

/* ============================================================ node props */
static void vnode_changed(struct vnode *v);
/* Sends the pending volume once the channel count is known. */
static void vnode_apply(struct vnode *v)
{
	if (v->nch <= 0 || !v->pend_vol || !v->proxy)
		return;
	v->pend_vol = false;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	float g[64];
	for (int i = 0; i < v->nch; i++)
		g[i] = v->pend_gain;
	struct spa_pod *p = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
		SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, v->nch, g));
	pw_node_set_param((struct pw_node *)v->proxy, SPA_PARAM_Props, 0, p);
	VLOG("volume %s: gain %.6f on %d channels (t=%" PRIu64 ")\n", v->name, v->pend_gain, v->nch, now_ns());
}

static void vnode_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param)
{
	struct vnode *v = data;
	if (id != SPA_PARAM_Props || !param || !spa_pod_is_object(param))
		return;
	const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
	const struct spa_pod_prop *prop;
	SPA_POD_OBJECT_FOREACH(obj, prop) {
		if (prop->key == SPA_PROP_channelVolumes) {
			float vols[64];
			uint32_t n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols, 64);
			if (n > 0) {
				v->nch = (int)n;
				v->gain = vols[0];
				v->have_gain = true;
			}
		} else if (prop->key == SPA_PROP_mute) {
			bool m;
			if (spa_pod_get_bool(&prop->value, &m) >= 0)
				v->muted = m;
		}
	}
	vnode_apply(v);
	vnode_changed(v);
}

static const struct pw_node_events vnode_events = {
	PW_VERSION_NODE_EVENTS,
	.param = vnode_param,
};

static void vnode_drop(struct vnode *v)
{
	if (v->proxy) {
		spa_hook_remove(&v->listener);
		spa_hook_remove(&v->proxy_listener);
		pw_proxy_destroy(v->proxy);
	}
	memset(v, 0, sizeof(*v));
}

static void vnode_removed(void *data)
{
	struct vnode *v = data;
	spa_hook_remove(&v->listener);
	spa_hook_remove(&v->proxy_listener);
	pw_proxy_destroy(v->proxy);
	v->proxy = NULL;
	memset(v, 0, sizeof(*v));
}

static const struct pw_proxy_events vnode_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = vnode_removed,
};

/* The bound node for a name, (re)bound if it is new or came back. */
static struct vnode *vnode_get(const char *name)
{
	if (!D.graph)
		return NULL; /* --no-graph */
	const struct bl_node *n = bl_graph_find_node(D.graph, name);
	if (!n)
		return NULL;
	struct vnode *free_slot = NULL;
	for (int i = 0; i < MAX_VNODES; i++) {
		struct vnode *v = &D.vnodes[i];
		if (v->used && !strcmp(v->name, name)) {
			if (v->id == n->id && v->proxy)
				return v;
			vnode_drop(v);
			free_slot = v;
			break;
		}
		if (!v->used && !free_slot)
			free_slot = v;
	}
	if (!free_slot)
		return NULL;
	struct vnode *v = free_slot;
	memset(v, 0, sizeof(*v));
	v->used = true;
	v->id = n->id;
	v->muted = -1;
	snprintf(v->name, sizeof(v->name), "%s", name);
	v->proxy = pw_registry_bind(D.registry, n->id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!v->proxy) {
		v->used = false;
		return NULL;
	}
	pw_node_add_listener((struct pw_node *)v->proxy, &v->listener, &vnode_events, v);
	pw_proxy_add_listener(v->proxy, &v->proxy_listener, &vnode_proxy_events, v);
	uint32_t ids[] = { SPA_PARAM_Props };
	pw_node_subscribe_params((struct pw_node *)v->proxy, ids, 1);
	pw_node_enum_params((struct pw_node *)v->proxy, 0, SPA_PARAM_Props, 0, 8, NULL);
	return v;
}

static bool set_volume(const char *node, double gain)
{
	struct vnode *v = vnode_get(node);
	if (!v)
		return false;
	v->pend_gain = (float)gain;
	v->pend_vol = true;
	vnode_apply(v); /* now, or when the node's Props arrive */
	return true;
}

static bool set_mute(const char *node, int mute /* 0, 1, -1 toggle */)
{
	struct vnode *v = vnode_get(node);
	if (!v)
		return false;
	bool m = mute < 0 ? !(v->muted > 0) : mute;
	uint8_t buf[256];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod *p = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(m));
	pw_node_set_param((struct pw_node *)v->proxy, SPA_PARAM_Props, 0, p);
	v->muted = m;
	VLOG("mute %s: %d\n", node, m);
	return true;
}

static bool set_control(const char *node, const char *name, double value)
{
	struct vnode *v = vnode_get(node);
	if (!v)
		return false;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&b, &f[1]);
	spa_pod_builder_string(&b, name);
	spa_pod_builder_float(&b, (float)value);
	spa_pod_builder_pop(&b, &f[1]);
	struct spa_pod *p = spa_pod_builder_pop(&b, &f[0]);
	pw_node_set_param((struct pw_node *)v->proxy, SPA_PARAM_Props, 0, p);
	VLOG("control %s/%s: %g\n", node, name, value);
	return true;
}

/* ============================================================ D-Bus targets */
enum call_kind { CALL_VALUE, CALL_RANGE, CALL_TOGGLE, CALL_FIRE };
struct call_ctx {
	enum call_kind kind;
	char id[48];
	uint64_t sent_ns;
};

static void inst_bus(const char *node, char *out, size_t n)
{
	if (!strncmp(node, INST_PREFIX, strlen(INST_PREFIX))) {
		snprintf(out, n, "%s", node);
		return;
	}
	size_t k = (size_t)snprintf(out, n, "%s", INST_PREFIX);
	if (!*node || (*node >= '0' && *node <= '9'))
		k += (size_t)snprintf(out + k, n - k, "n_");
	for (; *node && k + 1 < n; node++) {
		unsigned char ch = (unsigned char)*node;
		out[k++] = (ch < 0x80 && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) ? (char)ch : '_';
	}
	out[k] = 0;
}

static void send_value(struct mapping *m, double out);
static void tick_arm(void);
static void mapping_feedback(struct mapping *m, double in);
static int call_async(struct mapping *m, enum call_kind kind, const char *dest, const char *path, const char *iface,
		      const char *method, const char *types, ...);
static void apply_norm(struct mapping *m, double norm);

static void target_error(struct mapping *m, const sd_bus_error *e)
{
	D.target_errors++;
	if (D.target_errors <= 50 || D.verbose)
		LOG("%s -> %s: %s\n", m ? m->source_str : "?", m ? m->target_str : "?",
		    e && e->message ? e->message : e && e->name ? e->name : "failed");
}

/* Instance1 ListParams: the range of the mapped parameter, matched the way
 * the worker matches keys (id, exact name, module/name, unique substring). */
static bool range_from_list(struct mapping *m, sd_bus_message *reply)
{
	const char *key = m->tgt.key;
	char *end;
	unsigned long want_id = strtoul(key, &end, 10);
	bool numeric = *key && !*end;
	int r = sd_bus_message_enter_container(reply, 'a', "(ussddd)");
	if (r < 0)
		return false;
	uint32_t id;
	const char *name, *module;
	double val, mn, mx, fmin = 0, fmax = 1, smin = 0, smax = 1;
	int exact = 0, sub = 0;
	while (sd_bus_message_read(reply, "(ussddd)", &id, &name, &module, &val, &mn, &mx) > 0) {
		char full[600];
		snprintf(full, sizeof(full), "%s%s%s", module, *module ? "/" : "", name);
		if ((numeric && id == want_id) || !strcasecmp(name, key) || !strcasecmp(full, key)) {
			if (!exact++) { fmin = mn; fmax = mx; }
		} else if (strcasestr(full, key)) {
			if (!sub++) { smin = mn; smax = mx; }
		}
	}
	sd_bus_message_exit_container(reply);
	if (exact >= 1) {
		m->rmin = fmin;
		m->rmax = fmax;
		return true;
	}
	if (sub == 1) {
		m->rmin = smin;
		m->rmax = smax;
		return true;
	}
	return false;
}

static int on_reply(sd_bus_message *reply, void *ud, sd_bus_error *ret_error)
{
	struct call_ctx *c = ud;
	struct mapping *m = find_mapping(c->id);
	const sd_bus_error *e = sd_bus_message_get_error(reply);
	if (e) {
		target_error(m, e);
	} else if (m && D.verbose && c->kind != CALL_RANGE) {
		VLOG("%s -> %s: done in %.2f ms (t=%" PRIu64 ")\n", m->source_str, m->target_str,
		     (now_ns() - c->sent_ns) / 1e6, now_ns());
	}
	if (m) {
		switch (c->kind) {
		case CALL_VALUE:
			m->inflight = false;
			if (m->has_pending) {
				m->has_pending = false;
				send_value(m, m->pending);
			}
			break;
		case CALL_RANGE:
			m->range_inflight = false;
			if (!e && range_from_list(m, reply)) {
				m->range_known = true;
				VLOG("%s: parameter range %g..%g\n", m->target_str, m->rmin, m->rmax);
				if (m->have_norm) {
					m->have_norm = false;
					apply_norm(m, m->norm_pending);
				}
			} else if (!e) {
				sd_bus_error ne = SD_BUS_ERROR_MAKE_CONST("io.github.manuaudio.Backline.Error.NoSuchParam",
									   "no single parameter matches the key");
				target_error(m, &ne);
			}
			break;
		case CALL_TOGGLE:
			if (!e) {
				int rolling = 0;
				if (sd_bus_message_enter_container(reply, 'v', "b") >= 0)
					sd_bus_message_read(reply, "b", &rolling);
				call_async(m, CALL_FIRE, CLK_NAME, CLK_PATH, CLK_IFACE, rolling ? "Stop" : "Play", "");
			}
			break;
		case CALL_FIRE:
			break;
		}
	}
	free(c);
	return 0;
}

/* sd_bus_call_method_async with a floating slot whose userdata is freed by
 * on_reply (which always runs: reply, error or timeout). */
static int call_async(struct mapping *m, enum call_kind kind, const char *dest, const char *path, const char *iface,
		      const char *method, const char *types, ...)
{
	struct call_ctx *c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;
	c->kind = kind;
	c->sent_ns = now_ns();
	snprintf(c->id, sizeof(c->id), "%s", m->id);
	sd_bus_message *msg = NULL;
	int r = sd_bus_message_new_method_call(D.bus, &msg, dest, path, iface, method);
	if (r >= 0 && types && *types) {
		va_list ap;
		va_start(ap, types);
		r = sd_bus_message_appendv(msg, types, ap);
		va_end(ap);
	}
	if (r >= 0)
		r = sd_bus_call_async(D.bus, NULL, msg, on_reply, c, 30 * 1000000ull);
	sd_bus_message_unref(msg);
	if (r < 0) {
		free(c);
		sd_bus_error e = SD_BUS_ERROR_NULL;
		sd_bus_error_set_errno(&e, r);
		target_error(m, &e);
		sd_bus_error_free(&e);
		return r;
	}
	if (kind != CALL_RANGE)
		D.dispatched++;
	m->sends++;
	return 0;
}

static void query_range(struct mapping *m)
{
	if (m->range_inflight)
		return;
	char bus[300];
	inst_bus(m->tgt.node, bus, sizeof(bus));
	if (call_async(m, CALL_RANGE, bus, INST_PATH, INST_IFACE, "ListParams", "") >= 0)
		m->range_inflight = true;
}

static void send_value(struct mapping *m, double out)
{
	switch (m->tgt.kind) {
	case CTL_T_VOLUME:
		if (set_volume(m->tgt.node, isinf(out) && out < 0 ? 0 : ctl_db_to_gain(out)))
			D.dispatched++, m->sends++;
		else
			target_error(m, &SD_BUS_ERROR_MAKE_CONST(ERR_NOTFOUND, "no such node"));
		return;
	case CTL_T_MUTE: {
		int mu = out > 0.5;
		if (mu == m->last_mute)
			return;
		if (set_mute(m->tgt.node, mu)) {
			m->last_mute = mu;
			D.dispatched++, m->sends++;
		} else {
			target_error(m, &SD_BUS_ERROR_MAKE_CONST(ERR_NOTFOUND, "no such node"));
		}
		return;
	}
	case CTL_T_CONTROL:
		if (set_control(m->tgt.node, m->tgt.key, out))
			D.dispatched++, m->sends++;
		else
			target_error(m, &SD_BUS_ERROR_MAKE_CONST(ERR_NOTFOUND, "no such node"));
		return;
	case CTL_T_PARAM: {
		char why[256], client[80];
		snprintf(client, sizeof(client), "control:%s", m->id);
		if (m->gesture_auto) {
			if (!m->gest_open && pd_gesture(m->tgt.key, true, client, m->role, why, sizeof(why)) == 0)
				m->gest_open = true;
			m->gest_last_ns = now_ns();
			tick_arm();
		}
		if (pd_set(m->tgt.key, true, out, client, m->role, why, sizeof(why)) == 0) {
			D.dispatched++, m->sends++;
		} else {
			sd_bus_error e = SD_BUS_ERROR_MAKE_CONST(ERR_NOTFOUND, why);
			target_error(m, &e);
		}
		return;
	}
	default:
		break;
	}
	if (m->inflight) {
		m->pending = out;
		m->has_pending = true;
		return;
	}
	int r = -1;
	if (m->tgt.kind == CTL_T_INSTANCE) {
		char bus[300];
		inst_bus(m->tgt.node, bus, sizeof(bus));
		r = call_async(m, CALL_VALUE, bus, INST_PATH, INST_IFACE, "SetParam", "sd", m->tgt.key, out);
		VLOG("%s -> %s = %g (t=%" PRIu64 ")\n", m->source_str, m->target_str, out, now_ns());
	} else if (m->tgt.kind == CTL_T_TRANSPORT && !strcmp(m->tgt.action, "tempo")) {
		r = call_async(m, CALL_VALUE, CLK_NAME, CLK_PATH, CLK_IFACE, "SetTempo", "d", out);
	}
	if (r >= 0)
		m->inflight = true;
}

static void fire(struct mapping *m)
{
	const struct ctl_target *t = &m->tgt;
	VLOG("%s -> %s (trigger, t=%" PRIu64 ")\n", m->source_str, m->target_str, now_ns());
	if (t->kind == CTL_T_TRANSPORT) {
		if (!strcmp(t->action, "play"))
			call_async(m, CALL_FIRE, CLK_NAME, CLK_PATH, CLK_IFACE, "Play", "");
		else if (!strcmp(t->action, "stop"))
			call_async(m, CALL_FIRE, CLK_NAME, CLK_PATH, CLK_IFACE, "Stop", "");
		else if (!strcmp(t->action, "locate"))
			call_async(m, CALL_FIRE, CLK_NAME, CLK_PATH, CLK_IFACE, "Locate", "s", t->arg);
		else if (!strcmp(t->action, "toggle")) {
			if (call_async(m, CALL_TOGGLE, CLK_NAME, CLK_PATH, "org.freedesktop.DBus.Properties", "Get", "ss", CLK_IFACE,
				       "Rolling") >= 0)
				D.dispatched--; /* counted when the Play/Stop goes out */
		}
	} else if (t->kind == CTL_T_RECORDER) {
		if (!strcmp(t->action, "arm")) {
			sd_bus_message *msg = NULL;
			struct call_ctx *c = calloc(1, sizeof(*c));
			c->kind = CALL_FIRE;
			c->sent_ns = now_ns();
			snprintf(c->id, sizeof(c->id), "%s", m->id);
			int r = sd_bus_message_new_method_call(D.bus, &msg, REC_NAME, REC_PATH, REC_IFACE, "Arm");
			if (r >= 0)
				r = sd_bus_message_open_container(msg, 'a', "s");
			for (int i = 0; r >= 0 && i < m->nargs; i++)
				r = sd_bus_message_append(msg, "s", m->args[i]);
			if (r >= 0)
				r = sd_bus_message_close_container(msg);
			if (r >= 0)
				r = sd_bus_message_append(msg, "a{sv}", 0);
			if (r >= 0)
				r = sd_bus_call_async(D.bus, NULL, msg, on_reply, c, 30 * 1000000ull);
			sd_bus_message_unref(msg);
			if (r < 0) {
				free(c);
				sd_bus_error e = SD_BUS_ERROR_NULL;
				sd_bus_error_set_errno(&e, r);
				target_error(m, &e);
				sd_bus_error_free(&e);
			} else {
				D.dispatched++;
				m->sends++;
			}
		} else if (!strcmp(t->action, "record")) {
			call_async(m, CALL_FIRE, REC_NAME, REC_PATH, REC_IFACE, "Record", "");
		} else if (!strcmp(t->action, "stop")) {
			call_async(m, CALL_FIRE, REC_NAME, REC_PATH, REC_IFACE, "Stop", "");
		} else if (!strcmp(t->action, "marker")) {
			call_async(m, CALL_FIRE, REC_NAME, REC_PATH, REC_IFACE, "AddMarker", "s", m->nargs ? m->args[0] : "");
		}
	} else if (t->kind == CTL_T_MUTE) {
		if (set_mute(t->node, -1))
			D.dispatched++, m->sends++;
	}
}

static void apply_norm(struct mapping *m, double norm)
{
	double lo = m->min, hi = m->max;
	if (m->tgt.kind == CTL_T_INSTANCE && !(m->has_min && m->has_max)) {
		if (!m->range_known) {
			m->norm_pending = norm;
			m->have_norm = true;
			query_range(m);
			return;
		}
		if (!m->has_min) lo = m->rmin;
		if (!m->has_max) hi = m->rmax;
	}
	double out = ctl_scale(norm, m->invert, lo, hi);
	if (m->tgt.kind == CTL_T_VOLUME && !m->has_min && ctl_scale(norm, m->invert, 0, 1) <= 0)
		out = -INFINITY; /* the bottom of a fader is silence */
	send_value(m, out);
}

/* One input value for a mapping. impulse: an OSC message without a value
 * (or with I): a press, every time. */
static void mapping_input(struct mapping *m, double norm, bool impulse)
{
	if (ctl_target_is_trigger(&m->tgt) || (impulse && m->tgt.kind == CTL_T_MUTE)) {
		bool on = impulse || ctl_scale(norm, m->invert, 0, 1) > 0.5;
		if (impulse || (on && !m->pressed))
			fire(m);
		m->pressed = on && !impulse;
		return;
	}
	if (!(norm >= 0)) norm = 0;
	if (norm > 1) norm = 1;
	m->cur = norm;
	m->cur_known = true;
	apply_norm(m, norm);
	/* targets that never report back are echoed; volume, mute and param:
	 * feed back what the node or the program confirms */
	if (m->tgt.kind == CTL_T_INSTANCE || m->tgt.kind == CTL_T_CONTROL || m->tgt.kind == CTL_T_TRANSPORT)
		mapping_feedback(m, norm);
}

/* ============================================================ additions */
/* Out-of-range output (the target's own units) back to the 0..1 input. */
static double unscale(const struct mapping *m, double out)
{
	double lo = m->min, hi = m->max;
	if (m->tgt.kind == CTL_T_INSTANCE && m->range_known) {
		if (!m->has_min) lo = m->rmin;
		if (!m->has_max) hi = m->rmax;
	}
	double v = hi != lo ? (out - lo) / (hi - lo) : 0;
	if (!(v >= 0)) v = 0;
	if (v > 1) v = 1;
	return m->invert ? 1 - v : v;
}

static struct vnode *vnode_known(const char *name)
{
	for (int i = 0; i < MAX_VNODES; i++)
		if (D.vnodes[i].used && !strcmp(D.vnodes[i].name, name))
			return &D.vnodes[i];
	return NULL;
}

/* The target's current value as the mapping's input 0..1, when known. */
static bool mapping_current(struct mapping *m, double *in)
{
	if (m->tgt.kind == CTL_T_PARAM) {
		struct prm *p = pd_get(m->tgt.key);
		if (!p || p->type == PRM_STRING || p->type == PRM_TRIGGER)
			return false;
		*in = unscale(m, prm_to_norm(p, p->value));
		return true;
	}
	if (m->tgt.kind == CTL_T_VOLUME) {
		struct vnode *v = vnode_known(m->tgt.node);
		if (v && v->have_gain) {
			*in = unscale(m, ctl_gain_to_db(v->gain));
			return true;
		}
	} else if (m->tgt.kind == CTL_T_MUTE) {
		struct vnode *v = vnode_known(m->tgt.node);
		if (v && v->muted >= 0) {
			*in = unscale(m, v->muted);
			return true;
		}
	}
	if (m->cur_known) {
		*in = m->cur;
		return true;
	}
	return false;
}

static void out_push(const uint8_t *b, size_t n)
{
	/* channel messages, one control each */
	for (size_t i = 0; i < n;) {
		int dl = bl_midi_data_len(b[i]);
		size_t len = dl < 0 ? n - i : (size_t)dl + 1;
		if (i + len > n)
			break;
		if (bl_midi_ring_push(&D.out_ring, b + i, len))
			D.midi_sent++;
		i += len;
	}
}

static void mapping_feedback(struct mapping *m, double in)
{
	if (!m->feedback || m->src.kind == CTL_SRC_OSC || !D.midi_out_port)
		return;
	if (m->fb_sent && fabs(in - m->fb_last) < 1e-9)
		return;
	uint8_t b[16];
	size_t n = ctl_feedback_midi(&m->src, in, m->bits, b, sizeof(b));
	if (!n)
		return;
	out_push(b, n);
	m->fb_last = in;
	m->fb_sent = true;
	VLOG("feedback %s: %.4f\n", m->source_str, in);
}

/* A target changed from elsewhere: feedback, and takeover must pick up again. */
static void target_moved(struct mapping *m, double in)
{
	if (m->cur_known && fabs(in - m->cur) > 1e-6)
		ctl_takeover_lost(&m->to);
	m->cur = in;
	m->cur_known = true;
	mapping_feedback(m, in);
}

static void vnode_changed(struct vnode *v)
{
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		struct mapping *m = &D.maps[i];
		if (!m->used || strcmp(m->tgt.node, v->name))
			continue;
		if (m->tgt.kind == CTL_T_VOLUME && v->have_gain)
			target_moved(m, unscale(m, ctl_gain_to_db(v->gain)));
		else if (m->tgt.kind == CTL_T_MUTE && v->muted >= 0)
			target_moved(m, unscale(m, v->muted));
	}
}

static void on_param_changed(void *data, struct prm *p)
{
	if (p->type == PRM_STRING || p->type == PRM_TRIGGER)
		return;
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		struct mapping *m = &D.maps[i];
		if (m->used && m->tgt.kind == CTL_T_PARAM && !strcmp(m->tgt.key, p->path))
			target_moved(m, unscale(m, prm_to_norm(p, p->value)));
	}
}

/* An absolute 0..1 from a control, through soft takeover. */
static void mapping_abs(struct mapping *m, double norm)
{
	if (m->takeover != CTL_TO_JUMP && !ctl_target_is_trigger(&m->tgt)) {
		double t = 0, out;
		bool known = mapping_current(m, &t);
		if (!ctl_takeover_apply(m->takeover, &m->to, norm, known, t, &out)) {
			VLOG("%s: takeover waits (control %.3f, target %.3f)\n", m->source_str, norm, t);
			return;
		}
		norm = out;
	}
	mapping_input(m, norm, false);
}

static double default_step(const struct mapping *m)
{
	if (m->has_step)
		return m->step;
	if (m->tgt.kind == CTL_T_PARAM) {
		struct prm *p = pd_get(m->tgt.key);
		if (p && (p->type == PRM_INT || p->type == PRM_ENUM || p->type == PRM_BOOL) && p->max > p->min)
			return (p->step > 0 ? p->step : 1) / (p->max - p->min);
	}
	return 1.0 / 127;
}

/* A relative move of so many ticks. */
static void mapping_rel(struct mapping *m, int ticks)
{
	if (!ticks)
		return;
	double cur = 0;
	mapping_current(m, &cur);
	mapping_input(m, ctl_rel_apply(cur, ticks, default_step(m), m->accel), false);
}

static bool enc_relative(enum ctl_encoding e)
{
	return e == CTL_ENC_REL_2C || e == CTL_ENC_REL_SM || e == CTL_ENC_REL_OFFSET;
}

static void tick_arm(void)
{
	if (D.tick_on || !D.tick)
		return;
	D.tick_on = true;
	struct timespec v = { 0, 10 * 1000000L };
	pw_loop_update_timer(D.l, D.tick, &v, &v, false);
}

/* 10 ms while something waits: an MSB for its LSB, an automatic gesture's end. */
static void on_tick(void *data, uint64_t exp)
{
	uint64_t now = now_ns();
	bool busy = false;
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		struct mapping *m = &D.maps[i];
		if (!m->used)
			continue;
		double n;
		if (ctl_hires_expire(&m->hires, now, &n))
			mapping_abs(m, n);
		busy |= m->hires.pending;
		if (m->gest_open && now - m->gest_last_ns > 300ull * 1000000ull) {
			char why[256], client[80];
			snprintf(client, sizeof(client), "control:%s", m->id);
			pd_gesture(m->tgt.key, false, client, m->role, why, sizeof(why));
			m->gest_open = false;
		}
		busy |= m->gest_open;
	}
	if (!busy) {
		D.tick_on = false;
		struct timespec z = { 0, 0 };
		pw_loop_update_timer(D.l, D.tick, &z, NULL, false);
	}
}

/* ============================================================ mappings */
static void mapping_defaults(struct mapping *m)
{
	memset(m, 0, sizeof(*m));
	m->in_min = 0;
	m->in_max = 1;
	m->last_mute = -1;
	m->bits = 14;
	m->accel = 1;
	snprintf(m->role, sizeof(m->role), "all");
}

/* true: hay is a port of a device whose profile is active */
static bool profile_port(const char *hay)
{
	for (int i = 0; i < MAX_ACTIVE_PROFILES; i++)
		if (D.active[i].used && D.active[i].re_ok && regexec(&D.active[i].re, hay, 0, NULL, 0) == 0)
			return true;
	return false;
}

/* Fills the parsed fields and the output range; 0 or -1 with err. */
static int mapping_finish(struct mapping *m, char *err, size_t n)
{
	if (ctl_parse_source(m->source_str, &m->src, err, n) < 0)
		return -1;
	if (ctl_parse_target(m->target_str, &m->tgt, err, n) < 0)
		return -1;
	double dmin, dmax;
	if (ctl_target_default_range(&m->tgt, &dmin, &dmax)) {
		if (!m->has_min) m->min = dmin;
		if (!m->has_max) m->max = dmax;
	}
	if (m->tgt.kind == CTL_T_RECORDER && !strcmp(m->tgt.action, "arm") && m->nargs == 0) {
		snprintf(err, n, "recorder:arm needs the sources to arm in args (e.g. [\"backline-test.sink\"])");
		return -1;
	}
	if (!(m->in_max != m->in_min) && m->src.kind == CTL_SRC_OSC) {
		snprintf(err, n, "in-min and in-max must differ");
		return -1;
	}
	bool nrpn = m->src.kind == CTL_SRC_NRPN || m->src.kind == CTL_SRC_RPN;
	if (enc_relative(m->enc) && m->src.kind != CTL_SRC_CC && !nrpn) {
		snprintf(err, n, "encoding %s is for midi:cc, midi:nrpn and midi:rpn sources", ctl_encoding_name(m->enc));
		return -1;
	}
	if (m->enc == CTL_ENC_REL_INCDEC && !nrpn) {
		snprintf(err, n, "encoding rel-inc-dec is for midi:nrpn and midi:rpn sources");
		return -1;
	}
	if (m->enc != CTL_ENC_ABS && m->takeover != CTL_TO_JUMP) {
		snprintf(err, n, "takeover is for absolute controls (a relative encoder never jumps)");
		return -1;
	}
	if (m->bits != 7 && m->bits != 14) {
		snprintf(err, n, "bits is 7 or 14");
		return -1;
	}
	if (m->feedback && m->src.kind == CTL_SRC_OSC) {
		snprintf(err, n, "feedback is for MIDI sources (OSC clients subscribe through Params1)");
		return -1;
	}
	if (m->gesture_auto && m->tgt.kind != CTL_T_PARAM) {
		snprintf(err, n, "gesture: auto is for param: targets");
		return -1;
	}
	if (strcmp(m->role, "all") && strcmp(m->role, "read-only") && !prm_role_name_valid(m->role)) {
		snprintf(err, n, "'%s' is not a role name", m->role);
		return -1;
	}
	if (m->has_step && !(m->step > 0 && m->step <= 1)) {
		snprintf(err, n, "step is above 0 and at most 1");
		return -1;
	}
	if (!(m->accel >= 1 && m->accel <= 4)) {
		snprintf(err, n, "accel is 1 to 4");
		return -1;
	}
	return 0;
}

static struct mapping *mapping_insert(const struct mapping *tmpl, char *err, size_t n)
{
	if (tmpl->id[0] && find_mapping(tmpl->id)) {
		snprintf(err, n, "a mapping with id '%s' already exists", tmpl->id);
		return NULL;
	}
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		if (D.maps[i].used)
			continue;
		D.maps[i] = *tmpl;
		struct mapping *m = &D.maps[i];
		m->used = true;
		m->last_mute = -1;
		if (!m->id[0]) {
			do
				snprintf(m->id, sizeof(m->id), "m%d", ++D.next_id);
			while (find_mapping(m->id) != m);
		}
		return m;
	}
	snprintf(err, n, "too many mappings (%d)", MAX_MAPPINGS);
	return NULL;
}

/* A mapping from JSON (the schema of Mappings()). */
static int mapping_from_json(json_object *o, struct mapping *m, char *err, size_t n)
{
	mapping_defaults(m);
	json_object *v;
	if (!json_object_is_type(o, json_type_object)) {
		snprintf(err, n, "a mapping is a JSON object");
		return -1;
	}
	json_object_object_foreach(o, key, val) {
		if (!strcmp(key, "id")) snprintf(m->id, sizeof(m->id), "%s", json_object_get_string(val));
		else if (!strcmp(key, "source")) snprintf(m->source_str, sizeof(m->source_str), "%s", json_object_get_string(val));
		else if (!strcmp(key, "target")) snprintf(m->target_str, sizeof(m->target_str), "%s", json_object_get_string(val));
		else if (!strcmp(key, "min")) { m->min = json_object_get_double(val); m->has_min = true; }
		else if (!strcmp(key, "max")) { m->max = json_object_get_double(val); m->has_max = true; }
		else if (!strcmp(key, "invert")) m->invert = json_object_get_boolean(val);
		else if (!strcmp(key, "in-min")) m->in_min = json_object_get_double(val);
		else if (!strcmp(key, "in-max")) m->in_max = json_object_get_double(val);
		else if (!strcmp(key, "args")) {
			if (!json_object_is_type(val, json_type_array)) {
				snprintf(err, n, "args is an array of strings");
				return -1;
			}
			for (size_t i = 0; i < json_object_array_length(val) && m->nargs < MAX_ARGS; i++)
				snprintf(m->args[m->nargs++], sizeof(m->args[0]), "%s",
					 json_object_get_string(json_object_array_get_idx(val, i)));
		} else if (!strcmp(key, "encoding")) {
			if (ctl_parse_encoding(json_object_get_string(val), &m->enc) < 0) {
				snprintf(err, n, "encoding is abs, rel-2c, rel-sm, rel-offset or rel-inc-dec");
				return -1;
			}
		} else if (!strcmp(key, "takeover")) {
			if (ctl_parse_takeover(json_object_get_string(val), &m->takeover) < 0) {
				snprintf(err, n, "takeover is jump, pickup or scale");
				return -1;
			}
		} else if (!strcmp(key, "step")) { m->step = json_object_get_double(val); m->has_step = true; }
		else if (!strcmp(key, "accel")) m->accel = json_object_get_double(val);
		else if (!strcmp(key, "bits")) m->bits = json_object_get_int(val);
		else if (!strcmp(key, "feedback")) m->feedback = json_object_get_boolean(val);
		else if (!strcmp(key, "gesture")) {
			const char *g = json_object_get_string(val);
			if (!g || (strcmp(g, "auto") && strcmp(g, "none"))) {
				snprintf(err, n, "gesture is auto or none");
				return -1;
			}
			m->gesture_auto = !strcmp(g, "auto");
		} else if (!strcmp(key, "role")) snprintf(m->role, sizeof(m->role), "%s", json_object_get_string(val));
		else if (!strcmp(key, "sends") || !strcmp(key, "origin") || !strcmp(key, "profile") || !strcmp(key, "control")) {
			/* output-only fields of Mappings() */
		} else {
			snprintf(err, n, "unknown mapping key '%s'", key);
			return -1;
		}
	}
	(void)v;
	if (strlen(m->id) && (strspn(m->id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != strlen(m->id))) {
		snprintf(err, n, "an id is letters, digits, '-', '_' and '.'");
		return -1;
	}
	return mapping_finish(m, err, n);
}

static bool mapping_control(const struct ctl_source *src, const char **profile, const char **control);
static json_object *mapping_to_json(const struct mapping *m, bool runtime)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "id", json_object_new_string(m->id));
	json_object_object_add(o, "source", json_object_new_string(m->source_str));
	json_object_object_add(o, "target", json_object_new_string(m->target_str));
	if (m->has_min) json_object_object_add(o, "min", json_object_new_double(m->min));
	if (m->has_max) json_object_object_add(o, "max", json_object_new_double(m->max));
	if (m->invert) json_object_object_add(o, "invert", json_object_new_boolean(1));
	if (m->src.kind == CTL_SRC_OSC && (m->in_min != 0 || m->in_max != 1)) {
		json_object_object_add(o, "in-min", json_object_new_double(m->in_min));
		json_object_object_add(o, "in-max", json_object_new_double(m->in_max));
	}
	if (m->nargs) {
		json_object *a = json_object_new_array();
		for (int i = 0; i < m->nargs; i++)
			json_object_array_add(a, json_object_new_string(m->args[i]));
		json_object_object_add(o, "args", a);
	}
	if (m->enc != CTL_ENC_ABS) json_object_object_add(o, "encoding", json_object_new_string(ctl_encoding_name(m->enc)));
	if (m->takeover != CTL_TO_JUMP) json_object_object_add(o, "takeover", json_object_new_string(ctl_takeover_name(m->takeover)));
	if (m->has_step) json_object_object_add(o, "step", json_object_new_double(m->step));
	if (m->accel != 1) json_object_object_add(o, "accel", json_object_new_double(m->accel));
	if (m->bits != 14) json_object_object_add(o, "bits", json_object_new_int(m->bits));
	if (m->feedback) json_object_object_add(o, "feedback", json_object_new_boolean(1));
	if (m->gesture_auto) json_object_object_add(o, "gesture", json_object_new_string("auto"));
	if (strcmp(m->role, "all")) json_object_object_add(o, "role", json_object_new_string(m->role));
	if (runtime) {
		json_object_object_add(o, "sends", json_object_new_int64((int64_t)m->sends));
		if (m->origin[0])
			json_object_object_add(o, "origin", json_object_new_string(m->origin));
		const char *prof, *ctl;
		if (mapping_control(&m->src, &prof, &ctl)) {
			json_object_object_add(o, "profile", json_object_new_string(prof));
			json_object_object_add(o, "control", json_object_new_string(ctl));
		}
	}
	return o;
}

/* ============================================================ learn */
static void learn_stop(void)
{
	D.learning = false;
	D.learn_id[0] = D.learn_target[0] = 0;
	struct timespec z = { 0, 0 };
	pw_loop_update_timer(D.l, D.learn_timer, &z, NULL, false);
	props_changed("Learning", NULL);
}

static void on_learn_timeout(void *data, uint64_t exp)
{
	if (!D.learning)
		return;
	LOG("learn %s timed out\n", D.learn_id);
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "LearnCancelled", "ss", D.learn_id, "timeout");
	learn_stop();
}

/* true: this input was learned (and is not dispatched) */
static bool learn_input(const struct ctl_source *src)
{
	if (!D.learning)
		return false;
	bool midi = src->kind != CTL_SRC_OSC;
	if ((!strcmp(D.learn_from, "midi") && !midi) || (!strcmp(D.learn_from, "osc") && midi))
		return false;
	struct mapping m = D.learn_tmpl;
	ctl_format_source(src, m.source_str, sizeof(m.source_str));
	snprintf(m.id, sizeof(m.id), "%s", D.learn_id);
	char err[256];
	if (mapping_finish(&m, err, sizeof(err)) < 0 || !mapping_insert(&m, err, sizeof(err))) {
		LOG("learn %s: %s\n", D.learn_id, err);
		sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "LearnCancelled", "ss", D.learn_id, err);
		learn_stop();
		return true;
	}
	LOG("learned %s: %s -> %s\n", m.id, m.source_str, m.target_str);
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "Learned", "sss", m.id, m.source_str, m.target_str);
	learn_stop();
	props_changed("MappingCount", NULL);
	return true;
}

/* ============================================================ inputs */
static void nrpn_event(int channel, bool rpn, int param, enum ctl_nrpn_ev ev, int dv, uint64_t now);
static void on_midi_event(void *data, uint64_t count)
{
	for (;;) {
		unsigned r = atomic_load_explicit(&D.ring_r, memory_order_relaxed);
		unsigned w = atomic_load_explicit(&D.ring_w, memory_order_acquire);
		if (r == w)
			break;
		struct midi_ev e = D.ring[r & (MIDI_RING - 1)];
		atomic_store_explicit(&D.ring_r, r + 1, memory_order_release);
		struct ctl_source src;
		double v;
		if (!ctl_midi_event(e.data, e.size, &src, &v))
			continue;
		D.midi_events++;
		char s[64];
		if (D.verbose) {
			ctl_format_source(&src, s, sizeof(s));
			VLOG("midi %s %.4f (cycle t=%" PRIu64 ", main t=%" PRIu64 ")\n", s, v, e.ns, now_ns());
		}
		uint64_t now = now_ns();
		bool nrpn_cc = false;
		if (src.kind == CTL_SRC_CC) {
			int cc = src.number, val = e.data[2] & 0x7f;
			bool rpn;
			int prm, dv;
			enum ctl_nrpn_ev ev = ctl_nrpn_feed(&D.nrpn, src.channel - 1, cc, val, &rpn, &prm, &dv);
			nrpn_cc = cc == 6 || cc == 38 || (cc >= 96 && cc <= 101);
			if (ev != CTL_NRPN_NONE)
				nrpn_event(src.channel, rpn, prm, ev, dv, now);
			/* 14-bit pairs: CC n (MSB) with n+32 (LSB) */
			for (int i = 0; i < MAX_MAPPINGS; i++) {
				struct mapping *m = &D.maps[i];
				if (!m->used || m->src.kind != CTL_SRC_CC14 || (m->src.channel && m->src.channel != src.channel))
					continue;
				int part = cc == m->src.number ? 0 : cc == m->src.number + 32 ? 1 : -1;
				double n;
				if (part < 0)
					continue;
				if (ctl_hires_feed(&m->hires, part, val, now, &n))
					mapping_abs(m, n);
				else if (m->hires.pending)
					tick_arm();
			}
		}
		/* only note-ons and controller moves are learned (NRPN through nrpn_event) */
		if (!(src.kind == CTL_SRC_NOTE && v == 0) && !nrpn_cc && learn_input(&src))
			continue;
		for (int i = 0; i < MAX_MAPPINGS; i++) {
			struct mapping *m = &D.maps[i];
			if (!m->used || m->src.kind == CTL_SRC_OSC || !ctl_source_matches(&m->src, &src))
				continue;
			if (m->src.kind == CTL_SRC_CC && enc_relative(m->enc))
				mapping_rel(m, ctl_rel_ticks(m->enc, e.data[2]));
			else
				mapping_abs(m, v);
		}
	}
}

/* An NRPN/RPN data entry or increment on a channel (1-16). */
static void nrpn_event(int channel, bool rpn, int param, enum ctl_nrpn_ev ev, int dv, uint64_t now)
{
	struct ctl_source src = { .kind = rpn ? CTL_SRC_RPN : CTL_SRC_NRPN, .channel = channel, .number = param };
	if (ev == CTL_NRPN_DATA_MSB || ev == CTL_NRPN_INC || ev == CTL_NRPN_DEC)
		if (learn_input(&src))
			return;
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		struct mapping *m = &D.maps[i];
		if (!m->used || !ctl_source_matches(&m->src, &src))
			continue;
		if (ev == CTL_NRPN_INC || ev == CTL_NRPN_DEC) {
			mapping_rel(m, ev == CTL_NRPN_INC ? dv : -dv);
		} else if (enc_relative(m->enc)) {
			if (ev == CTL_NRPN_DATA_MSB)
				mapping_rel(m, ctl_rel_ticks(m->enc, dv));
		} else if (m->enc == CTL_ENC_REL_INCDEC) {
			/* data entry on an inc/dec mapping: ignored */
		} else if (m->bits == 7) {
			if (ev == CTL_NRPN_DATA_MSB)
				mapping_abs(m, dv / 127.0);
		} else {
			double n;
			if (ctl_hires_feed(&m->hires, ev == CTL_NRPN_DATA_MSB ? 0 : 1, dv, now, &n))
				mapping_abs(m, n);
			else if (m->hires.pending)
				tick_arm();
		}
	}
}

struct osc_ctx {
	struct listener *l;
	struct conn *c;
	struct sockaddr_storage peer;
	socklen_t plen;
};

static void peer_label(const struct sockaddr_storage *ss, bool tcp, char *out, size_t n)
{
	char a[INET6_ADDRSTRLEN] = "?";
	uint16_t port = 0;
	if (ss->ss_family == AF_INET) {
		inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, a, sizeof(a));
		port = ntohs(((const struct sockaddr_in *)ss)->sin_port);
	} else if (ss->ss_family == AF_INET6) {
		inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)ss)->sin6_addr, a, sizeof(a));
		port = ntohs(((const struct sockaddr_in6 *)ss)->sin6_port);
	}
	snprintf(out, n, "osc:%s:%s:%u", tcp ? "tcp" : "udp", a, port);
}

static void osc_dispatch(void *data, const struct osc_msg *msg)
{
	struct osc_ctx *ctx = data;
	if (ctx && ctx->l->spec.params[0]) {
		struct pd_peer pp;
		memset(&pp, 0, sizeof(pp));
		pp.listener = (int)(ctx->l - D.lis);
		pp.conn = ctx->c;
		pp.tcp = ctx->c != NULL;
		pp.addr = ctx->peer;
		pp.alen = ctx->plen;
		snprintf(pp.role, sizeof(pp.role), "%s", ctx->l->spec.params);
		peer_label(&ctx->peer, pp.tcp, pp.label, sizeof(pp.label));
		if (pd_osc(&pp, msg)) {
			D.received++;
			return;
		}
	}
	struct ctl_source src;
	memset(&src, 0, sizeof(src));
	src.kind = CTL_SRC_OSC;
	if (strlen(msg->address) >= sizeof(src.address) || !strcmp(msg->address, "/backline/auth"))
		return;
	strcpy(src.address, msg->address);
	D.received++;
	double x = 1;
	bool impulse = msg->argc == 0 || msg->argv[0].type == 'I';
	if (msg->argc > 0 && !impulse && !osc_arg_number(&msg->argv[0], &x))
		impulse = true; /* a string argument: a press */
	VLOG("osc %s %g%s (t=%" PRIu64 ")\n", msg->address, x, impulse ? " (press)" : "", now_ns());
	if (learn_input(&src))
		return;
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		struct mapping *m = &D.maps[i];
		if (!m->used || m->src.kind != CTL_SRC_OSC || strcmp(m->src.address, src.address))
			continue;
		if (impulse)
			mapping_input(m, 1, true);
		else
			mapping_abs(m, ctl_normalize(x, m->in_min, m->in_max));
	}
}

/* ============================================================ listeners */
static bool same_ip(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family)
		return false;
	if (a->ss_family == AF_INET)
		return ((const struct sockaddr_in *)a)->sin_addr.s_addr == ((const struct sockaddr_in *)b)->sin_addr.s_addr;
	return !memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr, &((const struct sockaddr_in6 *)b)->sin6_addr, 16);
}

static struct peer *peer_of(struct listener *l, const struct sockaddr_storage *ss, uint64_t now)
{
	struct peer *oldest = &l->peers[0];
	for (int i = 0; i < MAX_PEERS; i++) {
		struct peer *p = &l->peers[i];
		if (p->used && same_ip(&p->addr, ss)) {
			p->last_ns = now;
			return p;
		}
		if (!p->used || p->last_ns < oldest->last_ns)
			oldest = p;
	}
	memset(oldest, 0, sizeof(*oldest));
	oldest->used = true;
	oldest->addr = *ss;
	oldest->last_ns = now;
	return oldest;
}

static void reject(const char *why, const struct sockaddr_storage *ss)
{
	D.rejected++;
	if (D.rejected <= 20 || D.verbose) {
		char a[INET6_ADDRSTRLEN] = "?";
		if (ss->ss_family == AF_INET)
			inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, a, sizeof(a));
		else if (ss->ss_family == AF_INET6)
			inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)ss)->sin6_addr, a, sizeof(a));
		LOG("rejected %s: %s\n", a, why);
	}
}

static void on_udp(void *data, int fd, uint32_t mask)
{
	struct listener *l = data;
	for (int k = 0; k < 64; k++) {
		struct sockaddr_storage ss;
		socklen_t sl = sizeof(ss);
		ssize_t n = recvfrom(fd, l->udp_buf, l->spec.max_size + 1, MSG_TRUNC, (struct sockaddr *)&ss, &sl);
		if (n < 0)
			return;
		uint64_t now = now_ns();
		if (!ctl_peer_allowed(&l->spec, l->loopback, (struct sockaddr *)&ss)) {
			reject("not on the allow-list", &ss);
			continue;
		}
		struct peer *p = peer_of(l, &ss, now);
		if (!ctl_bucket_take(&p->bucket, l->spec.max_rate, now)) {
			reject("over the rate limit", &ss);
			continue;
		}
		if ((size_t)n > l->spec.max_size) {
			D.malformed++;
			reject("packet over max-size", &ss);
			continue;
		}
		struct osc_ctx x = { l, NULL, ss, sl };
		if (osc_parse(l->udp_buf, (size_t)n, osc_dispatch, &x) < 0) {
			D.malformed++;
			reject("malformed OSC", &ss);
		}
	}
}

static void conn_close(struct conn *c)
{
	struct listener *l = c->l;
	pd_peer_gone((int)(l - D.lis), c);
	pw_loop_destroy_source(D.l, c->src);
	close(c->fd);
	free(c->buf);
	free(c->out);
	for (int i = 0; i < l->n_conns; i++)
		if (l->conns[i] == c) {
			l->conns[i] = l->conns[--l->n_conns];
			break;
		}
	free(c);
}

static void conn_watch_out(struct conn *c, bool on)
{
	if (c->out_watch == on)
		return;
	c->out_watch = on;
	pw_loop_update_io(D.l, c->src, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP | (on ? SPA_IO_OUT : 0));
}

/* Writes what is queued; never closes (the caller may be iterating). */
static void conn_drain(struct conn *c)
{
	while (c->out_len) {
		ssize_t w = send(c->fd, c->out + c->out_off, c->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (w <= 0) {
			if (w < 0 && errno != EAGAIN && errno != EINTR)
				c->out_len = 0; /* the socket is broken; HUP/ERR closes it */
			break;
		}
		c->out_off += (size_t)w;
		c->out_len -= (size_t)w;
	}
	if (!c->out_len)
		c->out_off = 0;
	conn_watch_out(c, c->out_len > 0 || c->slow);
}

static void tcp_send(struct conn *c, const uint8_t *pkt, size_t n)
{
	uint8_t frame[4 + 8192];
	if (n > 8192 || c->slow)
		return;
	frame[0] = (uint8_t)(n >> 24); frame[1] = (uint8_t)(n >> 16); frame[2] = (uint8_t)(n >> 8); frame[3] = (uint8_t)n;
	memcpy(frame + 4, pkt, n);
	size_t done = 0;
	if (!c->out_len) {
		ssize_t w = send(c->fd, frame, n + 4, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (w == (ssize_t)(n + 4))
			return;
		if (w < 0 && errno != EAGAIN && errno != EINTR)
			return; /* broken: HUP/ERR closes it */
		done = w > 0 ? (size_t)w : 0;
	}
	size_t rest = n + 4 - done;
	if (c->out_len + rest > TCP_OUT_MAX) {
		c->slow = true; /* closed from its own I/O callback, never mid-iteration */
		conn_watch_out(c, true);
		return;
	}
	if (c->out_off && c->out_off + c->out_len + rest > c->out_cap) {
		memmove(c->out, c->out + c->out_off, c->out_len);
		c->out_off = 0;
	}
	if (c->out_off + c->out_len + rest > c->out_cap) {
		size_t nc = c->out_cap ? c->out_cap : 65536;
		while (nc < c->out_len + rest)
			nc *= 2;
		uint8_t *no = realloc(c->out, nc);
		if (!no) {
			c->slow = true;
			conn_watch_out(c, true);
			return;
		}
		c->out = no;
		c->out_cap = nc;
	}
	memcpy(c->out + c->out_off + c->out_len, frame + done, rest);
	c->out_len += rest;
	conn_watch_out(c, true);
}

struct auth_ctx {
	struct conn *c;
	int result; /* 0 not an auth message, 1 ok, -1 wrong */
};

static void auth_check(void *data, const struct osc_msg *m)
{
	struct auth_ctx *a = data;
	if (a->result)
		return;
	if (strcmp(m->address, "/backline/auth") || m->argc != 1 || m->argv[0].type != 's') {
		a->result = -1;
		return;
	}
	const char *given = m->argv[0].s;
	a->result = ctl_secret_equal(given, strlen(given), a->c->l->spec.secret) ? 1 : -1;
}

/* false: the connection was closed */
static bool tcp_packet(struct conn *c, const uint8_t *pkt, size_t n)
{
	struct listener *l = c->l;
	uint64_t now = now_ns();
	struct peer *p = peer_of(l, &c->peer, now);
	if (!ctl_bucket_take(&p->bucket, l->spec.max_rate, now)) {
		reject("over the rate limit", &c->peer);
		return true;
	}
	if (osc_parse(pkt, n, NULL, NULL) < 0) {
		D.malformed++;
		reject("malformed OSC, closing", &c->peer);
		conn_close(c);
		return false;
	}
	if (!c->authed) {
		struct auth_ctx a = { c, 0 };
		osc_parse(pkt, n, auth_check, &a);
		uint8_t reply[128];
		if (a.result == 1) {
			c->authed = true;
			p->fails = 0;
			size_t rn = osc_build(reply, sizeof(reply), "/backline/auth", "i", 1);
			tcp_send(c, reply, rn);
			VLOG("tcp peer authenticated\n");
			return true;
		}
		D.auth_failures++;
		if (++p->fails >= BLOCK_AFTER_FAILS)
			p->blocked_until = now + BLOCK_NS;
		reject("wrong or missing secret, closing", &c->peer);
		size_t rn = osc_build(reply, sizeof(reply), "/backline/auth", "i", 0);
		tcp_send(c, reply, rn);
		conn_close(c);
		return false;
	}
	struct osc_ctx x = { l, c, c->peer, sizeof(c->peer) };
	osc_parse(pkt, n, osc_dispatch, &x);
	return true;
}

static void on_tcp_conn(void *data, int fd, uint32_t mask)
{
	struct conn *c = data;
	struct listener *l = c->l;
	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		conn_close(c);
		return;
	}
	if (c->slow) {
		reject("a TCP peer that reads too slowly (4 MiB queued), closing", &c->peer);
		conn_close(c);
		return;
	}
	if (mask & SPA_IO_OUT)
		conn_drain(c);
	if (!(mask & SPA_IO_IN))
		return;
	size_t cap = l->spec.max_size + 4;
	ssize_t n = recv(fd, c->buf + c->have, cap - c->have, MSG_DONTWAIT);
	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EINTR))
			conn_close(c);
		return;
	}
	c->have += (size_t)n;
	while (c->have >= 4) {
		uint32_t len = (uint32_t)c->buf[0] << 24 | (uint32_t)c->buf[1] << 16 | (uint32_t)c->buf[2] << 8 | c->buf[3];
		if (len == 0 || len > l->spec.max_size) {
			D.malformed++;
			reject("frame size out of range, closing", &c->peer);
			conn_close(c);
			return;
		}
		if (c->have < 4 + (size_t)len)
			break;
		if (!tcp_packet(c, c->buf + 4, len))
			return;
		memmove(c->buf, c->buf + 4 + len, c->have - 4 - len);
		c->have -= 4 + len;
	}
}

static void on_tcp_accept(void *data, int fd, uint32_t mask)
{
	struct listener *l = data;
	for (;;) {
		struct sockaddr_storage ss;
		socklen_t sl = sizeof(ss);
		int cfd = accept4(fd, (struct sockaddr *)&ss, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (cfd < 0)
			return;
		uint64_t now = now_ns();
		if (!ctl_peer_allowed(&l->spec, l->loopback, (struct sockaddr *)&ss)) {
			reject("not on the allow-list", &ss);
			close(cfd);
			continue;
		}
		struct peer *p = peer_of(l, &ss, now);
		if (p->blocked_until > now) {
			reject("blocked after failed authentication", &ss);
			close(cfd);
			continue;
		}
		if (l->n_conns >= (int)l->spec.max_clients || l->n_conns >= MAX_CONNS) {
			reject("too many clients", &ss);
			close(cfd);
			continue;
		}
		struct conn *c = calloc(1, sizeof(*c));
		c->buf = malloc(l->spec.max_size + 4);
		c->fd = cfd;
		c->peer = ss;
		c->l = l;
		c->opened_ns = now;
		c->authed = !l->spec.secret[0];
		c->src = pw_loop_add_io(D.l, cfd, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_tcp_conn, c);
		l->conns[l->n_conns++] = c;
	}
}

/* Params1 replies and subscriptions (paramsd.c) go out here. */
static void pd_send(void *data, const struct pd_peer *p, const uint8_t *pkt, size_t n)
{
	if (p->listener < 0 || p->listener >= MAX_LISTENERS || !D.lis[p->listener].used)
		return;
	struct listener *l = &D.lis[p->listener];
	if (p->tcp) {
		for (int i = 0; i < l->n_conns; i++)
			if (l->conns[i] == p->conn && l->conns[i]->authed) {
				tcp_send(l->conns[i], pkt, n);
				return;
			}
		return;
	}
	(void)!sendto(l->fd, pkt, n, MSG_DONTWAIT | MSG_NOSIGNAL, (const struct sockaddr *)&p->addr, p->alen);
}

static void listener_close(struct listener *l)
{
	while (l->n_conns)
		conn_close(l->conns[0]);
	pd_peer_gone((int)(l - D.lis), NULL);
	if (l->src)
		pw_loop_destroy_source(D.l, l->src);
	if (l->fd > 0)
		close(l->fd);
	free(l->udp_buf);
	memset(l, 0, sizeof(*l));
}

static void format_listener_id(const struct ctl_listen_spec *s, uint16_t port, char *out, size_t n)
{
	bool v6 = strchr(s->address, ':') != NULL;
	snprintf(out, n, "%s:%s%s%s:%u", s->tcp ? "tcp" : "udp", v6 ? "[" : "", s->address, v6 ? "]" : "", port);
}

static struct listener *listener_open(const struct ctl_listen_spec *spec, const char *origin, char *err, size_t n)
{
	if (ctl_check_listen(spec, err, n) < 0)
		return NULL;
	struct sockaddr_storage ss;
	socklen_t sl;
	bool loopback;
	ctl_parse_address(spec->address, spec->port, &ss, &sl, &loopback, err, n);
	struct listener *l = NULL;
	for (int i = 0; i < MAX_LISTENERS; i++)
		if (!D.lis[i].used) {
			l = &D.lis[i];
			break;
		}
	if (!l) {
		snprintf(err, n, "too many listeners (%d)", MAX_LISTENERS);
		return NULL;
	}
	int fd = socket(ss.ss_family, (spec->tcp ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		snprintf(err, n, "socket: %s", strerror(errno));
		return NULL;
	}
	int one = 1;
	if (ss.ss_family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
	/* a restart must get its port back while closed connections sit in
	 * TIME_WAIT (Linux still refuses a second listener on the port) */
	if (spec->tcp)
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr *)&ss, sl) < 0 || (spec->tcp && listen(fd, 8) < 0)) {
		snprintf(err, n, "cannot listen on %s port %u: %s", spec->address, spec->port, strerror(errno));
		close(fd);
		return NULL;
	}
	memset(l, 0, sizeof(*l));
	l->used = true;
	l->spec = *spec;
	l->loopback = loopback;
	l->fd = fd;
	snprintf(l->origin, sizeof(l->origin), "%s", origin ? origin : "");
	struct sockaddr_storage bound;
	socklen_t bl = sizeof(bound);
	getsockname(fd, (struct sockaddr *)&bound, &bl);
	uint16_t port = ntohs(bound.ss_family == AF_INET ? ((struct sockaddr_in *)&bound)->sin_port
							  : ((struct sockaddr_in6 *)&bound)->sin6_port);
	l->spec.port = port;
	format_listener_id(spec, port, l->id, sizeof(l->id));
	if (spec->tcp) {
		l->src = pw_loop_add_io(D.l, fd, SPA_IO_IN, false, on_tcp_accept, l);
	} else {
		l->udp_buf = malloc(spec->max_size + 1);
		l->src = pw_loop_add_io(D.l, fd, SPA_IO_IN, false, on_udp, l);
	}
	LOG("listening on %s%s%s%s%s\n", l->id, loopback ? " (loopback)" : "", l->spec.secret[0] ? ", secret required" : "",
	    l->spec.params[0] ? ", Params1 as " : "", l->spec.params);
	props_changed("Listeners", NULL);
	return l;
}

static void spec_defaults(struct ctl_listen_spec *s)
{
	memset(s, 0, sizeof(*s));
	s->max_rate = 500;
	s->max_clients = 8;
	s->max_size = 8192;
}

static int spec_from_json(json_object *o, struct ctl_listen_spec *s, char *err, size_t n)
{
	spec_defaults(s);
	if (!json_object_is_type(o, json_type_object)) {
		snprintf(err, n, "a listener is a JSON object");
		return -1;
	}
	json_object_object_foreach(o, key, val) {
		if (!strcmp(key, "proto")) {
			const char *p = json_object_get_string(val);
			if (!strcmp(p, "tcp")) s->tcp = true;
			else if (strcmp(p, "udp")) { snprintf(err, n, "proto is udp or tcp"); return -1; }
		} else if (!strcmp(key, "address")) snprintf(s->address, sizeof(s->address), "%s", json_object_get_string(val));
		else if (!strcmp(key, "port")) {
			int p = json_object_get_int(val);
			if (p < 0 || p > 65535) { snprintf(err, n, "port out of range"); return -1; }
			s->port = (uint16_t)p;
		} else if (!strcmp(key, "allow")) {
			if (!json_object_is_type(val, json_type_array)) { snprintf(err, n, "allow is an array"); return -1; }
			for (size_t i = 0; i < json_object_array_length(val); i++) {
				const char *c = json_object_get_string(json_object_array_get_idx(val, i));
				if (s->n_allow >= CTL_MAX_ALLOW || ctl_parse_cidr(c, &s->allow[s->n_allow]) < 0) {
					snprintf(err, n, "'%s' is not an IP address or CIDR (or too many)", c ? c : "");
					return -1;
				}
				s->n_allow++;
			}
		} else if (!strcmp(key, "secret")) snprintf(s->secret, sizeof(s->secret), "%s", json_object_get_string(val));
		else if (!strcmp(key, "insecure-udp")) s->insecure_udp = json_object_get_boolean(val);
		else if (!strcmp(key, "max-rate")) s->max_rate = (uint32_t)json_object_get_int64(val);
		else if (!strcmp(key, "max-clients")) s->max_clients = (uint32_t)json_object_get_int64(val);
		else if (!strcmp(key, "max-size")) s->max_size = (uint32_t)json_object_get_int64(val);
		else if (!strcmp(key, "params")) {
			const char *r = json_object_get_string(val);
			if (!r || (strcmp(r, "all") && strcmp(r, "read-only") && !prm_role_name_valid(r))) {
				snprintf(err, n, "params is a role: all, read-only, or a role the program defined");
				return -1;
			}
			snprintf(s->params, sizeof(s->params), "%s", r);
		}
		else { snprintf(err, n, "unknown listener key '%s'", key); return -1; }
	}
	return 0;
}

static json_object *spec_to_json(const struct listener *l, bool with_secret)
{
	const struct ctl_listen_spec *s = &l->spec;
	json_object *o = json_object_new_object();
	if (!with_secret)
		json_object_object_add(o, "id", json_object_new_string(l->id));
	json_object_object_add(o, "proto", json_object_new_string(s->tcp ? "tcp" : "udp"));
	json_object_object_add(o, "address", json_object_new_string(s->address));
	json_object_object_add(o, "port", json_object_new_int(s->port));
	json_object *a = json_object_new_array();
	for (int i = 0; i < s->n_allow; i++) {
		char buf[INET6_ADDRSTRLEN + 8];
		inet_ntop(s->allow[i].family, s->allow[i].addr, buf, sizeof(buf));
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "/%d", s->allow[i].prefix);
		json_object_array_add(a, json_object_new_string(buf));
	}
	json_object_object_add(o, "allow", a);
	if (with_secret && s->secret[0])
		json_object_object_add(o, "secret", json_object_new_string(s->secret));
	if (!with_secret) {
		json_object_object_add(o, "auth", json_object_new_boolean(s->secret[0] != 0));
		json_object_object_add(o, "loopback", json_object_new_boolean(l->loopback));
		if (s->tcp)
			json_object_object_add(o, "clients", json_object_new_int(l->n_conns));
	}
	if (s->insecure_udp)
		json_object_object_add(o, "insecure-udp", json_object_new_boolean(1));
	json_object_object_add(o, "max-rate", json_object_new_int64(s->max_rate));
	if (s->tcp)
		json_object_object_add(o, "max-clients", json_object_new_int64(s->max_clients));
	json_object_object_add(o, "max-size", json_object_new_int64(s->max_size));
	if (s->params[0])
		json_object_object_add(o, "params", json_object_new_string(s->params));
	return o;
}

/* ============================================================ MIDI follow */
static int follow_port(void *data, const struct bl_port *p)
{
	if (D.midi_port_id == SPA_ID_INVALID || !p->midi || !p->output || p->node_id == D.node_id)
		return 0;
	char hay[700];
	snprintf(hay, sizeof(hay), "%s:%s | %s", p->node_name, p->name, p->alias);
	bool want = (D.follow_on && regexec(&D.follow_re, hay, 0, NULL, 0) == 0) || profile_port(hay);
	if (want && !bl_graph_is_linked(D.graph, p->id, D.midi_port_id)) {
		LOG("midi-follow: linking %s\n", hay);
		bl_graph_link(D.graph, p->id, D.midi_port_id);
		if (D.n_followed < 64)
			D.followed[D.n_followed++] = p->id;
	}
	return 0;
}

/* MIDI out: link controld's midi_out to every MIDI input port (a device's
 * playback side) that matches the out regex. */
static int out_follow_port(void *data, const struct bl_port *p)
{
	if (D.midi_out_port_id == SPA_ID_INVALID || !p->midi || p->output || p->node_id == D.node_id)
		return 0;
	char hay[700];
	snprintf(hay, sizeof(hay), "%s:%s | %s", p->node_name, p->name, p->alias);
	bool want = (D.out_on && regexec(&D.out_re, hay, 0, NULL, 0) == 0) || profile_port(hay);
	if (want && !bl_graph_is_linked(D.graph, D.midi_out_port_id, p->id)) {
		LOG("midi-out: linking to %s\n", hay);
		bl_graph_link(D.graph, D.midi_out_port_id, p->id);
		if (D.n_out_linked < 64)
			D.out_linked[D.n_out_linked++] = p->id;
	}
	return 0;
}

static int find_own_port(void *data, const struct bl_port *p)
{
	if (D.node_id != SPA_ID_INVALID && p->node_id == D.node_id && p->midi) {
		if (!p->output)
			D.midi_port_id = p->id;
		else
			D.midi_out_port_id = p->id;
		return 1;
	}
	return 0;
}

static void profiles_port_added(const struct bl_port *p);
static void profiles_port_removed(void);

static void on_port_added(void *data, const struct bl_port *p)
{
	if ((D.midi_port_id == SPA_ID_INVALID || D.midi_out_port_id == SPA_ID_INVALID) && find_own_port(NULL, p)) {
		bl_graph_for_each_port(D.graph, follow_port, NULL);
		bl_graph_for_each_port(D.graph, out_follow_port, NULL);
		return;
	}
	profiles_port_added(p); /* first: an auto profile links its device */
	follow_port(NULL, p);
	out_follow_port(NULL, p);
}

static void on_port_removed(void *data, uint32_t id)
{
	if (id == D.midi_port_id)
		D.midi_port_id = SPA_ID_INVALID;
	if (id == D.midi_out_port_id)
		D.midi_out_port_id = SPA_ID_INVALID;
	for (int i = 0; i < D.n_out_linked; i++)
		if (D.out_linked[i] == id) {
			D.out_linked[i] = D.out_linked[--D.n_out_linked];
			break;
		}
	for (int i = 0; i < D.n_followed; i++)
		if (D.followed[i] == id) {
			D.followed[i] = D.followed[--D.n_followed];
			break;
		}
	profiles_port_removed();
}

static const struct bl_graph_events graph_events = {
	.port_added = on_port_added,
	.port_removed = on_port_removed,
};

static int set_follow(const char *re, char *err, size_t n)
{
	if (!D.graph && re && *re) {
		snprintf(err, n, "started with --no-graph: no MIDI input");
		return -1;
	}
	regex_t nr;
	if (re && *re) {
		int r = regcomp(&nr, re, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		if (r) {
			regerror(r, &nr, err, n);
			return -1;
		}
	}
	/* drop the links this daemon made to sources that no longer match */
	for (int i = 0; i < D.n_followed; i++)
		if (D.midi_port_id != SPA_ID_INVALID)
			bl_graph_unlink(D.graph, D.followed[i], D.midi_port_id);
	D.n_followed = 0;
	if (D.follow_on)
		regfree(&D.follow_re);
	D.follow_on = re && *re;
	snprintf(D.follow, sizeof(D.follow), "%s", re ? re : "");
	if (D.follow_on) {
		D.follow_re = nr;
		bl_graph_for_each_port(D.graph, follow_port, NULL);
	}
	props_changed("MidiFollow", NULL);
	return 0;
}

static int set_out_follow(const char *re, char *err, size_t n)
{
	if (!D.graph && re && *re) {
		snprintf(err, n, "started with --no-graph: no MIDI output");
		return -1;
	}
	regex_t nr;
	if (re && *re) {
		int r = regcomp(&nr, re, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		if (r) {
			regerror(r, &nr, err, n);
			return -1;
		}
	}
	for (int i = 0; i < D.n_out_linked; i++)
		if (D.midi_out_port_id != SPA_ID_INVALID)
			bl_graph_unlink(D.graph, D.midi_out_port_id, D.out_linked[i]);
	D.n_out_linked = 0;
	if (D.out_on)
		regfree(&D.out_re);
	D.out_on = re && *re;
	snprintf(D.out_follow, sizeof(D.out_follow), "%s", re ? re : "");
	if (D.out_on) {
		D.out_re = nr;
		bl_graph_for_each_port(D.graph, out_follow_port, NULL);
	}
	props_changed("MidiOut", NULL);
	return 0;
}

/* ============================================================ MIDI profiles */
/* A device's controls under abstract names, and what was learned for them
 * (midiprof.h; docs/api/Control1.md "MIDI profiles"). A profile is active
 * while its device is there (or after LoadProfile): its bindings are
 * mappings with origin "profile:<id>", and its device's MIDI ports are
 * linked to controld's input and output. */
static void relink_follow(void)
{
	for (int i = 0; i < D.n_followed; i++)
		if (D.midi_port_id != SPA_ID_INVALID)
			bl_graph_unlink(D.graph, D.followed[i], D.midi_port_id);
	D.n_followed = 0;
	for (int i = 0; i < D.n_out_linked; i++)
		if (D.midi_out_port_id != SPA_ID_INVALID)
			bl_graph_unlink(D.graph, D.midi_out_port_id, D.out_linked[i]);
	D.n_out_linked = 0;
	bl_graph_for_each_port(D.graph, follow_port, NULL);
	bl_graph_for_each_port(D.graph, out_follow_port, NULL);
}

static void profile_warn(void *ud, const char *path, const char *why)
{
	LOG("%s: not a usable MIDI profile, skipped: %s\n", path, why);
}

static void profiles_reload(void)
{
	mp_set_free(&D.profiles);
	const char *dirs[2];
	bool sys[2] = { false, true };
	for (int i = 0; i < D.n_profile_dirs; i++)
		dirs[i] = D.profile_dirs[i];
	mp_set_load(&D.profiles, dirs, D.n_profile_dirs, sys, profile_warn, NULL);
}

static struct active_profile *active_find(const char *id)
{
	for (int i = 0; i < MAX_ACTIVE_PROFILES; i++)
		if (D.active[i].used && !strcmp(D.active[i].id, id))
			return &D.active[i];
	return NULL;
}

struct present_ctx {
	const struct mp_profile *p;
	char *out;
	size_t n;
	bool found;
};

static int present_port(void *data, const struct bl_port *port)
{
	struct present_ctx *c = data;
	if (!port->midi || !port->output || port->node_id == D.node_id)
		return 0;
	char hay[700];
	snprintf(hay, sizeof(hay), "%s:%s | %s", port->node_name, port->name, port->alias);
	if (mp_matches_device(c->p, hay)) {
		snprintf(c->out, c->n, "%s", hay);
		c->found = true;
		return 1;
	}
	return 0;
}

/* The device's MIDI output port, if one is in the graph. */
static bool profile_present(const struct mp_profile *p, char *dev, size_t n)
{
	struct present_ctx c = { p, dev, n, false };
	if (n)
		dev[0] = 0;
	if (D.graph)
		bl_graph_for_each_port(D.graph, present_port, &c);
	return c.found;
}

static void origin_of(const char *id, char *out, size_t n) { snprintf(out, n, "profile:%s", id); }

static int remove_profile_mappings(const char *id)
{
	char origin[MP_NAME_MAX + 16];
	origin_of(id, origin, sizeof(origin));
	int k = 0;
	for (int i = 0; i < MAX_MAPPINGS; i++)
		if (D.maps[i].used && !strcmp(D.maps[i].origin, origin)) {
			D.maps[i].used = false;
			k++;
		}
	return k;
}

static void profile_deactivate(struct active_profile *a, const char *reason)
{
	char id[MP_NAME_MAX];
	snprintf(id, sizeof(id), "%s", a->id);
	int k = remove_profile_mappings(id);
	if (a->re_ok)
		regfree(&a->re);
	memset(a, 0, sizeof(*a));
	relink_follow();
	LOG("profile %s unloaded (%s): %d mappings removed\n", id, reason, k);
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "ProfileUnloaded", "ss", id, reason);
	props_changed("ActiveProfiles", "MappingCount", NULL);
}

/* Makes the profile's bindings mappings, all or none. ids (may be NULL)
 * gets the mapping ids. */
static int profile_activate(const struct mp_profile *p, const char *device, bool manual, sd_bus_message *ids, char *err,
			    size_t n)
{
	struct active_profile *a = active_find(p->id);
	if (a)
		profile_deactivate(a, "reloaded");
	for (int i = 0; i < MAX_ACTIVE_PROFILES && !a; i++)
		if (!D.active[i].used)
			a = &D.active[i];
	if (!a) {
		snprintf(err, n, "too many active profiles (%d)", MAX_ACTIVE_PROFILES);
		return -1;
	}
	char origin[MP_NAME_MAX + 16];
	origin_of(p->id, origin, sizeof(origin));
	size_t nb = json_object_array_length(p->bindings);
	for (size_t i = 0; i < nb; i++) {
		char e[400];
		json_object *mo = mp_binding_mapping(p, i, e, sizeof(e));
		struct mapping m;
		struct mapping *nm = NULL;
		if (mo && mapping_from_json(mo, &m, e, sizeof(e)) == 0) {
			snprintf(m.origin, sizeof(m.origin), "%s", origin);
			nm = mapping_insert(&m, e, sizeof(e));
		}
		if (mo)
			json_object_put(mo);
		if (!nm) {
			snprintf(err, n, "profile %s, binding %zu: %s", p->id, i, e);
			remove_profile_mappings(p->id);
			return -1;
		}
		ctl_format_source(&nm->src, nm->source_str, sizeof(nm->source_str));
		if (ids)
			sd_bus_message_append(ids, "s", nm->id);
	}
	memset(a, 0, sizeof(*a));
	a->used = true;
	a->manual = manual;
	snprintf(a->id, sizeof(a->id), "%s", p->id);
	snprintf(a->device, sizeof(a->device), "%s", device ? device : "");
	a->re_ok = regcomp(&a->re, p->match, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0;
	relink_follow();
	LOG("profile %s loaded%s%s: %zu mappings\n", p->id, a->device[0] ? " for " : "", a->device, nb);
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "ProfileLoaded", "ssu", p->id, a->device, (uint32_t)nb);
	props_changed("ActiveProfiles", "MappingCount", NULL);
	return 0;
}

/* A device appeared: the first profile (the user's before the system's,
 * then by name) that matches it and loads by itself becomes active. */
static void profiles_port_added(const struct bl_port *port)
{
	if (!port->midi || !port->output || port->node_id == D.node_id)
		return;
	char hay[700];
	snprintf(hay, sizeof(hay), "%s:%s | %s", port->node_name, port->name, port->alias);
	for (int i = 0; i < D.profiles.n; i++) {
		const struct mp_profile *p = D.profiles.p[i];
		if (!mp_matches_device(p, hay))
			continue;
		if (!p->autoload || active_find(p->id))
			return;
		char e[400];
		if (profile_activate(p, hay, false, NULL, e, sizeof(e)) < 0)
			LOG("%s\n", e);
		return;
	}
}

/* A port went: profiles that came with their device go with it. */
static void profiles_port_removed(void)
{
	for (int i = 0; i < MAX_ACTIVE_PROFILES; i++) {
		struct active_profile *a = &D.active[i];
		if (!a->used || a->manual)
			continue;
		const struct mp_profile *p = mp_set_find(&D.profiles, a->id);
		char dev[700];
		if (!p || !profile_present(p, dev, sizeof(dev)))
			profile_deactivate(a, "device-gone");
	}
}

static int auto_port(void *data, const struct bl_port *port)
{
	profiles_port_added(port);
	return 0;
}

/* Every device already in the graph (at start, after a delete). */
static void profiles_scan(void)
{
	if (D.graph)
		bl_graph_for_each_port(D.graph, auto_port, NULL);
}

static json_object *profile_summary(const struct mp_profile *p)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "id", json_object_new_string(p->id));
	json_object_object_add(o, "name", json_object_new_string(p->name));
	if (p->vendor[0]) json_object_object_add(o, "vendor", json_object_new_string(p->vendor));
	if (p->model[0]) json_object_object_add(o, "model", json_object_new_string(p->model));
	if (p->mode[0]) json_object_object_add(o, "mode", json_object_new_string(p->mode));
	json_object_object_add(o, "match", json_object_new_string(p->match));
	json_object_object_add(o, "verified", json_object_new_boolean(p->verified));
	json_object_object_add(o, "auto", json_object_new_boolean(p->autoload));
	json_object_object_add(o, "system", json_object_new_boolean(p->system));
	json_object_object_add(o, "path", json_object_new_string(p->path));
	json_object_object_add(o, "controls", json_object_new_int(p->n_controls));
	json_object_object_add(o, "bindings", json_object_new_int((int)json_object_array_length(p->bindings)));
	struct active_profile *a = active_find(p->id);
	char dev[700];
	bool present = profile_present(p, dev, sizeof(dev));
	json_object_object_add(o, "active", json_object_new_boolean(a != NULL));
	json_object_object_add(o, "present", json_object_new_boolean(present));
	json_object_object_add(o, "device", json_object_new_string(a && a->device[0] ? a->device : dev));
	return o;
}

/* The active profile control a MIDI source is, for Mappings(). */
static bool mapping_control(const struct ctl_source *src, const char **profile, const char **control)
{
	for (int i = 0; i < MAX_ACTIVE_PROFILES; i++) {
		if (!D.active[i].used)
			continue;
		const struct mp_profile *p = mp_set_find(&D.profiles, D.active[i].id);
		const struct mp_control *c = p ? mp_control_for_source(p, src) : NULL;
		if (c) {
			*profile = p->id;
			*control = c->name;
			return true;
		}
	}
	return false;
}

static int m_profiles(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	profiles_reload();
	json_object *o = json_object_new_object();
	json_object_object_add(o, "schema", json_object_new_string(MP_SCHEMA));
	json_object *ps = json_object_new_array();
	for (int i = 0; i < D.profiles.n; i++)
		json_object_array_add(ps, profile_summary(D.profiles.p[i]));
	json_object_object_add(o, "profiles", ps);
	json_object *ds = json_object_new_array();
	for (int i = 0; i < D.n_profile_dirs; i++)
		json_object_array_add(ds, json_object_new_string(D.profile_dirs[i]));
	json_object_object_add(o, "dirs", ds);
	int r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return r;
}

static int m_profile(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	profiles_reload();
	const struct mp_profile *p = mp_set_find(&D.profiles, id);
	if (!p)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no MIDI profile '%s'", id);
	json_object *o = mp_to_json(p), *s = profile_summary(p);
	json_object_object_foreach(s, key, val) {
		if (!strcmp(key, "system") || !strcmp(key, "path") || !strcmp(key, "active") || !strcmp(key, "present") ||
		    !strcmp(key, "device"))
			json_object_object_add(o, key, json_object_get(val));
	}
	json_object_put(s);
	r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return r;
}

static int m_load_profile(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	profiles_reload();
	const struct mp_profile *p = mp_set_find(&D.profiles, id);
	if (!p)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no MIDI profile '%s'", id);
	sd_bus_message *reply = NULL;
	if ((r = sd_bus_message_new_method_return(m, &reply)) < 0 || (r = sd_bus_message_open_container(reply, 'a', "s")) < 0)
		return r;
	char dev[700], e[500];
	profile_present(p, dev, sizeof(dev));
	if (profile_activate(p, dev, true, reply, e, sizeof(e)) < 0) {
		sd_bus_message_unref(reply);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	}
	if ((r = sd_bus_message_close_container(reply)) >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int m_unload_profile(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	struct active_profile *a = active_find(id);
	if (!a)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "MIDI profile '%s' is not loaded", id);
	profile_deactivate(a, "unloaded");
	return sd_bus_reply_method_return(m, "");
}

static bool mapping_is_midi(const struct mapping *mp) { return mp->src.kind != CTL_SRC_OSC; }

static int m_save_profile(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	if (D.n_profile_dirs < 1 || D.no_config)
		return sd_bus_error_setf(err, ERR_STATE, "started with --no-config: nothing is saved");
	if (!mp_name_valid(id))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
					 "'%s' is not a profile id (a-z, 0-9, '.', '_', '-'; at most 63)", id);
	char name[128] = "", match[256] = "";
	int autoload = -1;
	const char *sel[MAX_MAPPINGS];
	int n_sel = -1;
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key, *sig = NULL, *sv;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0)
			return r;
		sd_bus_message_peek_type(m, NULL, &sig);
		if ((!strcmp(key, "name") || !strcmp(key, "match")) && sig && !strcmp(sig, "s")) {
			sd_bus_message_read(m, "v", "s", &sv);
			snprintf(key[0] == 'n' ? name : match, key[0] == 'n' ? sizeof(name) : sizeof(match), "%s", sv);
		} else if (!strcmp(key, "auto") && sig && !strcmp(sig, "b")) {
			int b;
			sd_bus_message_read(m, "v", "b", &b);
			autoload = b;
		} else if (!strcmp(key, "mappings") && sig && !strcmp(sig, "as")) {
			n_sel = 0;
			sd_bus_message_enter_container(m, 'v', "as");
			sd_bus_message_enter_container(m, 'a', "s");
			while (sd_bus_message_read(m, "s", &sv) > 0)
				if (n_sel < MAX_MAPPINGS)
					sel[n_sel++] = sv;
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else {
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
						 "unknown option '%s' (name s, match s, auto b, mappings as)", key);
		}
		sd_bus_message_exit_container(m);
	}
	if ((r = sd_bus_message_exit_container(m)) < 0)
		return r;

	profiles_reload();
	const struct mp_profile *base = mp_set_find(&D.profiles, id);
	struct mp_profile p;
	char e[500];
	if (base) {
		if (mp_load_file(base->path, &p, e, sizeof(e)) < 0)
			return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "%s: %s", base->path, e);
	} else {
		if (!match[0])
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
						 "'%s' is a new profile: give match, a regular expression for its MIDI port", id);
		if (mp_init(&p, id, name, match, e, sizeof(e)) < 0)
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	}
	if (name[0])
		snprintf(p.name, sizeof(p.name), "%s", name);
	if (match[0] && base) {
		mp_free(&p);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
					 "'%s' exists: its match is in its file (DeleteProfile and save again to change it)", id);
	}
	if (autoload >= 0)
		p.autoload = autoload;

	/* the mappings to bind: the ones named, or every MIDI mapping that is
	 * this profile's, made at run time or from control.json */
	char origin[MP_NAME_MAX + 16];
	origin_of(id, origin, sizeof(origin));
	struct mapping *picked[MAX_MAPPINGS];
	int n_picked = 0;
	if (n_sel >= 0) {
		for (int i = 0; i < n_sel; i++) {
			struct mapping *mp = find_mapping(sel[i]);
			if (!mp) {
				mp_free(&p);
				return sd_bus_error_setf(err, ERR_NOTFOUND, "no mapping '%s'", sel[i]);
			}
			picked[n_picked++] = mp;
		}
	} else {
		for (int i = 0; i < MAX_MAPPINGS; i++) {
			struct mapping *mp = &D.maps[i];
			if (mp->used && mapping_is_midi(mp) &&
			    (!mp->origin[0] || !strcmp(mp->origin, "control.json") || !strcmp(mp->origin, origin)))
				picked[n_picked++] = mp;
		}
	}
	json_object *arr = json_object_new_array();
	for (int i = 0; i < n_picked; i++)
		json_object_array_add(arr, mapping_to_json(picked[i], false));
	bool add_controls = n_sel >= 0 || p.n_controls == 0;
	int skipped = 0;
	int bound = mp_bind_mappings(&p, arr, add_controls, &skipped, e, sizeof(e));
	json_object_put(arr);
	if (bound < 0) {
		mp_free(&p);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	}
	char path[800];
	snprintf(path, sizeof(path), "%s/%s%s", D.profile_dirs[0], id, MP_FILE_SUFFIX);
	if (mp_save_file(&p, path, e, sizeof(e)) < 0) {
		mp_free(&p);
		return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "%s", e);
	}
	/* the bound mappings are the profile's now: they go when it is unloaded
	 * and are no longer written to control.json */
	for (int i = 0; i < n_picked; i++)
		if (mp_control_for_source(&p, &picked[i]->src))
			snprintf(picked[i]->origin, sizeof(picked[i]->origin), "%s", origin);
	if (!active_find(id)) {
		struct active_profile *a = NULL;
		for (int i = 0; i < MAX_ACTIVE_PROFILES && !a; i++)
			if (!D.active[i].used)
				a = &D.active[i];
		if (a) {
			a->used = true;
			snprintf(a->id, sizeof(a->id), "%s", id);
			a->manual = !profile_present(&p, a->device, sizeof(a->device));
			a->re_ok = regcomp(&a->re, p.match, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0;
			relink_follow();
			props_changed("ActiveProfiles", NULL);
		}
	}
	LOG("saved MIDI profile %s: %d bindings (%d mappings not this device's) in %s\n", id, bound, skipped, path);
	mp_free(&p);
	profiles_reload();
	return sd_bus_reply_method_return(m, "s", path);
}

static int m_delete_profile(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	profiles_reload();
	const struct mp_profile *p = mp_set_find(&D.profiles, id);
	if (!p)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no MIDI profile '%s'", id);
	if (p->system)
		return sd_bus_error_setf(err, ERR_STATE, "'%s' is a system profile (%s): it can't be deleted, only hidden by a "
				       "profile of the same id in %s", id, p->path, D.profile_dirs[0]);
	if (unlink(p->path) < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "cannot delete %s: %s", p->path, strerror(errno));
	LOG("deleted MIDI profile %s (%s)\n", id, p->path);
	struct active_profile *a = active_find(id);
	if (a)
		profile_deactivate(a, "deleted");
	profiles_reload();
	profiles_scan(); /* a system profile of the same id may now take its device */
	return sd_bus_reply_method_return(m, "");
}

static int p_active_profiles(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
			     void *ud, sd_bus_error *err)
{
	int r = sd_bus_message_open_container(reply, 'a', "s");
	if (!strcmp(prop, "ProfileDirs")) {
		for (int i = 0; r >= 0 && i < D.n_profile_dirs; i++)
			r = sd_bus_message_append(reply, "s", D.profile_dirs[i]);
	} else {
		for (int i = 0; r >= 0 && i < MAX_ACTIVE_PROFILES; i++)
			if (D.active[i].used)
				r = sd_bus_message_append(reply, "s", D.active[i].id);
	}
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

/* ============================================================ config */
static void default_config_dir(void)
{
	const char *own = getenv("BACKLINE_CONTROL_DIR"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (D.config_dir[0])
		return;
	if (own)
		snprintf(D.config_dir, sizeof(D.config_dir), "%s", own);
	else if (x && *x)
		snprintf(D.config_dir, sizeof(D.config_dir), "%s/backline/control", x);
	else
		snprintf(D.config_dir, sizeof(D.config_dir), "%s/.config/backline/control", h ? h : ".");
}

/* The user's profiles: --profile-dir, or MP_DIR_NAME beside the config dir
 * ($XDG_CONFIG_HOME/<product>/midi-profiles). The system's: the installed
 * datadir, or the source tree's when running from a build. */
static void default_profile_dirs(const char *user)
{
	char buf[700];
	if (user) {
		snprintf(buf, sizeof(buf), "%s", user);
	} else {
		snprintf(buf, sizeof(buf), "%s", D.config_dir);
		char *sl = strrchr(buf, '/');
		if (sl && sl != buf)
			snprintf(sl + 1, sizeof(buf) - (size_t)(sl + 1 - buf), "%s", MP_DIR_NAME);
		else
			snprintf(buf, sizeof(buf), "%s", MP_DIR_NAME);
	}
	snprintf(D.profile_dirs[0], sizeof(D.profile_dirs[0]), "%s", buf);
	struct stat st;
	snprintf(D.profile_dirs[1], sizeof(D.profile_dirs[1]), "%s",
		 stat(BL_MIDIPROF_DATADIR, &st) == 0 ? BL_MIDIPROF_DATADIR : BL_MIDIPROF_SRCDIR);
	D.n_profile_dirs = 2;
}

static int name_cmp(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
static int json_file(const struct dirent *d)
{
	size_t n = strlen(d->d_name);
	return d->d_name[0] != '.' && n > 5 && !strcmp(d->d_name + n - 5, ".json");
}

static void load_file(const char *path, const char *base)
{
	json_object *j = json_object_from_file(path);
	if (!j) {
		LOG("%s: not readable JSON, skipped\n", path);
		return;
	}
	json_object *v;
	if (!json_object_object_get_ex(j, "schema", &v) || strcmp(json_object_get_string(v), SCHEMA)) {
		LOG("%s: not a %s file, skipped\n", path, SCHEMA);
		json_object_put(j);
		return;
	}
	char err[300];
	if (json_object_object_get_ex(j, "mappings", &v) && json_object_is_type(v, json_type_array))
		for (size_t i = 0; i < json_object_array_length(v); i++) {
			struct mapping m;
			if (mapping_from_json(json_object_array_get_idx(v, i), &m, err, sizeof(err)) < 0 ||
			    (snprintf(m.origin, sizeof(m.origin), "%s", base), !mapping_insert(&m, err, sizeof(err))))
				LOG("%s: mapping %zu skipped: %s\n", path, i, err);
		}
	if (json_object_object_get_ex(j, "listeners", &v) && json_object_is_type(v, json_type_array))
		for (size_t i = 0; i < json_object_array_length(v); i++) {
			struct ctl_listen_spec s;
			if (spec_from_json(json_object_array_get_idx(v, i), &s, err, sizeof(err)) < 0 ||
			    !listener_open(&s, base, err, sizeof(err)))
				LOG("%s: listener %zu not opened: %s\n", path, i, err);
		}
	if (json_object_object_get_ex(j, "midi-follow", &v) && json_object_get_string(v)[0] && !D.follow_on)
		if (set_follow(json_object_get_string(v), err, sizeof(err)) < 0)
			LOG("%s: midi-follow: %s\n", path, err);
	if (json_object_object_get_ex(j, "midi-out", &v) && json_object_get_string(v)[0] && !D.out_on)
		if (set_out_follow(json_object_get_string(v), err, sizeof(err)) < 0)
			LOG("%s: midi-out: %s\n", path, err);
	json_object_put(j);
	LOG("loaded %s\n", path);
}

static void load_config(void)
{
	struct dirent **list;
	int n = scandir(D.config_dir, &list, json_file, name_cmp);
	if (n < 0)
		return;
	for (int i = 0; i < n; i++) {
		char p[1200];
		snprintf(p, sizeof(p), "%s/%s", D.config_dir, list[i]->d_name);
		load_file(p, list[i]->d_name);
		free(list[i]);
	}
	free(list);
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
	return mkdir(tmp, 0700) < 0 && errno != EEXIST ? -errno : 0;
}

/* Writes control.json: what was made at run time and what came from it. */
static int save_config(char *path_out, size_t n, char *err, size_t en)
{
	if (mkdir_p(D.config_dir) < 0) {
		snprintf(err, en, "cannot create %s: %s", D.config_dir, strerror(errno));
		return -1;
	}
	json_object *o = json_object_new_object();
	json_object_object_add(o, "schema", json_object_new_string(SCHEMA));
	if (D.follow_on)
		json_object_object_add(o, "midi-follow", json_object_new_string(D.follow));
	if (D.out_on)
		json_object_object_add(o, "midi-out", json_object_new_string(D.out_follow));
	json_object *ls = json_object_new_array();
	for (int i = 0; i < MAX_LISTENERS; i++)
		if (D.lis[i].used && (!D.lis[i].origin[0] || !strcmp(D.lis[i].origin, "control.json")))
			json_object_array_add(ls, spec_to_json(&D.lis[i], true));
	json_object_object_add(o, "listeners", ls);
	json_object *ms = json_object_new_array();
	for (int i = 0; i < MAX_MAPPINGS; i++)
		if (D.maps[i].used && (!D.maps[i].origin[0] || !strcmp(D.maps[i].origin, "control.json")))
			json_object_array_add(ms, mapping_to_json(&D.maps[i], false));
	json_object_object_add(o, "mappings", ms);
	char tmp[800], final[800];
	snprintf(final, sizeof(final), "%s/control.json", D.config_dir);
	snprintf(tmp, sizeof(tmp), "%s/.control.json.tmp", D.config_dir);
	const char *s = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); /* it may hold a secret */
	int r = -1;
	if (fd >= 0) {
		size_t len = strlen(s);
		r = write(fd, s, len) == (ssize_t)len && write(fd, "\n", 1) == 1 && fsync(fd) == 0 ? 0 : -1;
		close(fd);
		if (r == 0)
			r = rename(tmp, final);
	}
	json_object_put(o);
	if (r < 0) {
		snprintf(err, en, "cannot write %s: %s", final, strerror(errno));
		unlink(tmp);
		return -1;
	}
	for (int i = 0; i < MAX_MAPPINGS; i++)
		if (D.maps[i].used && !D.maps[i].origin[0])
			snprintf(D.maps[i].origin, sizeof(D.maps[i].origin), "control.json");
	for (int i = 0; i < MAX_LISTENERS; i++)
		if (D.lis[i].used && !D.lis[i].origin[0])
			snprintf(D.lis[i].origin, sizeof(D.lis[i].origin), "control.json");
	snprintf(path_out, n, "%s", final);
	return 0;
}

/* ============================================================ D-Bus API */
static int read_options(sd_bus_message *m, struct mapping *t, char *learn_from, sd_bus_error *err, uint32_t *timeout)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	const char *key;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		if ((r = sd_bus_message_read(m, "s", &key)) < 0)
			return r;
		const char *sig = NULL;
		sd_bus_message_peek_type(m, NULL, &sig);
		if (!strcmp(key, "min") || !strcmp(key, "max") || !strcmp(key, "in-min") || !strcmp(key, "in-max")) {
			double d;
			if (!sig || strcmp(sig, "d"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s is a double (d)", key);
			sd_bus_message_read(m, "v", "d", &d);
			if (!strcmp(key, "min")) { t->min = d; t->has_min = true; }
			else if (!strcmp(key, "max")) { t->max = d; t->has_max = true; }
			else if (!strcmp(key, "in-min")) t->in_min = d;
			else t->in_max = d;
		} else if (!strcmp(key, "invert")) {
			int b;
			if (!sig || strcmp(sig, "b"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "invert is a boolean (b)");
			sd_bus_message_read(m, "v", "b", &b);
			t->invert = b;
		} else if (!strcmp(key, "id")) {
			const char *s;
			if (!sig || strcmp(sig, "s"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "id is a string (s)");
			sd_bus_message_read(m, "v", "s", &s);
			snprintf(t->id, sizeof(t->id), "%s", s);
		} else if (!strcmp(key, "args")) {
			if (!sig || strcmp(sig, "as"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "args is an array of strings (as)");
			sd_bus_message_enter_container(m, 'v', "as");
			sd_bus_message_enter_container(m, 'a', "s");
			const char *s;
			while (sd_bus_message_read(m, "s", &s) > 0)
				if (t->nargs < MAX_ARGS)
					snprintf(t->args[t->nargs++], sizeof(t->args[0]), "%s", s);
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else if (!strcmp(key, "encoding") || !strcmp(key, "takeover") || !strcmp(key, "gesture") || !strcmp(key, "role")) {
			const char *sv;
			if (!sig || strcmp(sig, "s"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s is a string (s)", key);
			sd_bus_message_read(m, "v", "s", &sv);
			if (!strcmp(key, "encoding") && ctl_parse_encoding(sv, &t->enc) < 0)
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "encoding is abs, rel-2c, rel-sm, rel-offset or rel-inc-dec");
			if (!strcmp(key, "takeover") && ctl_parse_takeover(sv, &t->takeover) < 0)
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "takeover is jump, pickup or scale");
			if (!strcmp(key, "gesture")) {
				if (strcmp(sv, "auto") && strcmp(sv, "none"))
					return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "gesture is auto or none");
				t->gesture_auto = !strcmp(sv, "auto");
			}
			if (!strcmp(key, "role"))
				snprintf(t->role, sizeof(t->role), "%s", sv);
		} else if (!strcmp(key, "step") || !strcmp(key, "accel")) {
			double d;
			if (!sig || strcmp(sig, "d"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s is a double (d)", key);
			sd_bus_message_read(m, "v", "d", &d);
			if (key[0] == 's') { t->step = d; t->has_step = true; }
			else t->accel = d;
		} else if (!strcmp(key, "feedback")) {
			int b;
			if (!sig || strcmp(sig, "b"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "feedback is a boolean (b)");
			sd_bus_message_read(m, "v", "b", &b);
			t->feedback = b;
		} else if (!strcmp(key, "bits")) {
			uint32_t u;
			if (!sig || strcmp(sig, "u"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "bits is a number (u)");
			sd_bus_message_read(m, "v", "u", &u);
			t->bits = (int)u;
		} else if (learn_from && !strcmp(key, "from")) {
			const char *s;
			if (!sig || strcmp(sig, "s"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "from is a string (s)");
			sd_bus_message_read(m, "v", "s", &s);
			if (strcmp(s, "midi") && strcmp(s, "osc") && strcmp(s, "any"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "from is midi, osc or any");
			snprintf(learn_from, 8, "%s", s);
		} else if (timeout && !strcmp(key, "timeout")) {
			if (!sig || strcmp(sig, "u"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "timeout is seconds (u)");
			sd_bus_message_read(m, "v", "u", timeout);
		} else {
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown option '%s'", key);
		}
		sd_bus_message_exit_container(m);
	}
	return sd_bus_message_exit_container(m);
}

static int m_map(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *src, *tgt;
	int r = sd_bus_message_read(m, "ss", &src, &tgt);
	if (r < 0)
		return r;
	struct mapping t;
	mapping_defaults(&t);
	if ((r = read_options(m, &t, NULL, err, NULL)) < 0)
		return r;
	snprintf(t.source_str, sizeof(t.source_str), "%s", src);
	snprintf(t.target_str, sizeof(t.target_str), "%s", tgt);
	char e[400];
	struct mapping *nm;
	if (mapping_finish(&t, e, sizeof(e)) < 0 || !(nm = mapping_insert(&t, e, sizeof(e))))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	ctl_format_source(&nm->src, nm->source_str, sizeof(nm->source_str)); /* canonical */
	LOG("mapped %s: %s -> %s\n", nm->id, nm->source_str, nm->target_str);
	props_changed("MappingCount", NULL);
	return sd_bus_reply_method_return(m, "s", nm->id);
}

static int m_add_mapping(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *js;
	int r = sd_bus_message_read(m, "s", &js);
	if (r < 0)
		return r;
	json_object *o = json_tokener_parse(js);
	if (!o)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "not valid JSON");
	struct mapping t;
	char e[400];
	struct mapping *nm = NULL;
	r = mapping_from_json(o, &t, e, sizeof(e));
	json_object_put(o);
	if (r < 0 || !(nm = mapping_insert(&t, e, sizeof(e))))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	ctl_format_source(&nm->src, nm->source_str, sizeof(nm->source_str));
	LOG("mapped %s: %s -> %s\n", nm->id, nm->source_str, nm->target_str);
	props_changed("MappingCount", NULL);
	return sd_bus_reply_method_return(m, "s", nm->id);
}

static int m_remove(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	struct mapping *mp = find_mapping(id);
	if (!mp)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no mapping '%s'", id);
	mp->used = false; /* in-flight replies find no mapping and are dropped */
	props_changed("MappingCount", NULL);
	return sd_bus_reply_method_return(m, "");
}

static int m_mappings(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "schema", json_object_new_string(SCHEMA));
	json_object *ms = json_object_new_array();
	for (int i = 0; i < MAX_MAPPINGS; i++)
		if (D.maps[i].used)
			json_object_array_add(ms, mapping_to_json(&D.maps[i], true));
	json_object_object_add(o, "mappings", ms);
	json_object *ls = json_object_new_array();
	for (int i = 0; i < MAX_LISTENERS; i++)
		if (D.lis[i].used)
			json_object_array_add(ls, spec_to_json(&D.lis[i], false));
	json_object_object_add(o, "listeners", ls);
	json_object_object_add(o, "midi-follow", json_object_new_string(D.follow));
	json_object_object_add(o, "midi-out", json_object_new_string(D.out_follow));
	int r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return r;
}

static int m_learn(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *tgt;
	int r = sd_bus_message_read(m, "s", &tgt);
	if (r < 0)
		return r;
	if (D.learning)
		return sd_bus_error_setf(err, ERR_BUSY, "already learning %s (CancelLearn first)", D.learn_id);
	struct mapping t;
	mapping_defaults(&t);
	char from[8] = "any";
	uint32_t timeout = 30;
	if ((r = read_options(m, &t, from, err, &timeout)) < 0)
		return r;
	struct ctl_target pt;
	char e[400];
	if (ctl_parse_target(tgt, &pt, e, sizeof(e)) < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	if (t.id[0] && find_mapping(t.id))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "a mapping with id '%s' already exists", t.id);
	if (!t.id[0]) {
		do
			snprintf(t.id, sizeof(t.id), "m%d", ++D.next_id);
		while (find_mapping(t.id));
	}
	snprintf(t.target_str, sizeof(t.target_str), "%s", tgt);
	D.learn_tmpl = t;
	snprintf(D.learn_id, sizeof(D.learn_id), "%s", t.id);
	snprintf(D.learn_target, sizeof(D.learn_target), "%s", tgt);
	snprintf(D.learn_from, sizeof(D.learn_from), "%s", from);
	D.learning = true;
	if (timeout) {
		struct timespec ts = { timeout, 0 };
		pw_loop_update_timer(D.l, D.learn_timer, &ts, NULL, false);
	}
	LOG("learning %s for %s (from %s, %u s)\n", D.learn_id, tgt, from, timeout);
	props_changed("Learning", NULL);
	return sd_bus_reply_method_return(m, "s", D.learn_id);
}

static int m_cancel_learn(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	if (!D.learning)
		return sd_bus_error_setf(err, ERR_STATE, "not learning");
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "LearnCancelled", "ss", D.learn_id, "cancelled");
	learn_stop();
	return sd_bus_reply_method_return(m, "");
}

static int m_listen(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *proto, *addr;
	uint16_t port;
	int r = sd_bus_message_read(m, "ssq", &proto, &addr, &port);
	if (r < 0)
		return r;
	struct ctl_listen_spec s;
	spec_defaults(&s);
	if (!strcmp(proto, "tcp"))
		s.tcp = true;
	else if (strcmp(proto, "udp"))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "proto is udp or tcp");
	snprintf(s.address, sizeof(s.address), "%s", addr);
	s.port = port;
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	const char *key;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		sd_bus_message_read(m, "s", &key);
		const char *sig = NULL;
		sd_bus_message_peek_type(m, NULL, &sig);
		if (!strcmp(key, "allow")) {
			if (!sig || strcmp(sig, "as"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "allow is an array of strings (as)");
			sd_bus_message_enter_container(m, 'v', "as");
			sd_bus_message_enter_container(m, 'a', "s");
			const char *c;
			while (sd_bus_message_read(m, "s", &c) > 0) {
				if (s.n_allow >= CTL_MAX_ALLOW || ctl_parse_cidr(c, &s.allow[s.n_allow]) < 0)
					return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "'%s' is not an IP address or CIDR (or too many)", c);
				s.n_allow++;
			}
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else if (!strcmp(key, "secret")) {
			const char *v;
			if (!sig || strcmp(sig, "s"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "secret is a string (s)");
			sd_bus_message_read(m, "v", "s", &v);
			snprintf(s.secret, sizeof(s.secret), "%s", v);
		} else if (!strcmp(key, "insecure-udp")) {
			int b;
			if (!sig || strcmp(sig, "b"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "insecure-udp is a boolean (b)");
			sd_bus_message_read(m, "v", "b", &b);
			s.insecure_udp = b;
		} else if (!strcmp(key, "params")) {
			const char *v;
			if (!sig || strcmp(sig, "s"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "params is a string (s)");
			sd_bus_message_read(m, "v", "s", &v);
			if (strcmp(v, "all") && strcmp(v, "read-only") && !prm_role_name_valid(v))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
							 "params is a role: all, read-only, or a role the program defined");
			snprintf(s.params, sizeof(s.params), "%s", v);
		} else if (!strcmp(key, "max-rate") || !strcmp(key, "max-clients") || !strcmp(key, "max-size")) {
			uint32_t u;
			if (!sig || strcmp(sig, "u"))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s is a number (u)", key);
			sd_bus_message_read(m, "v", "u", &u);
			if (!strcmp(key, "max-rate")) s.max_rate = u;
			else if (!strcmp(key, "max-clients")) s.max_clients = u;
			else s.max_size = u;
		} else {
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown option '%s'", key);
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	char e[400];
	struct listener *l = listener_open(&s, NULL, e, sizeof(e));
	if (!l)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	return sd_bus_reply_method_return(m, "s", l->id);
}

static int m_unlisten(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	for (int i = 0; i < MAX_LISTENERS; i++)
		if (D.lis[i].used && !strcmp(D.lis[i].id, id)) {
			LOG("closed %s\n", id);
			listener_close(&D.lis[i]);
			props_changed("Listeners", NULL);
			return sd_bus_reply_method_return(m, "");
		}
	return sd_bus_error_setf(err, ERR_NOTFOUND, "no listener '%s'", id);
}

static int m_save(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	char path[800], e[400];
	if (D.no_config)
		return sd_bus_error_setf(err, ERR_STATE, "started with --no-config: nothing is saved");
	if (save_config(path, sizeof(path), e, sizeof(e)) < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "%s", e);
	LOG("saved %s\n", path);
	return sd_bus_reply_method_return(m, "s", path);
}

static int m_set_follow(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *re;
	int r = sd_bus_message_read(m, "s", &re);
	if (r < 0)
		return r;
	char e[300];
	if (set_follow(re, e, sizeof(e)) < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "bad regular expression: %s", e);
	return sd_bus_reply_method_return(m, "");
}

static int m_set_out(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *re;
	int r = sd_bus_message_read(m, "s", &re);
	if (r < 0)
		return r;
	char e[300];
	if (set_out_follow(re, e, sizeof(e)) < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "bad regular expression: %s", e);
	return sd_bus_reply_method_return(m, "");
}

static int p_str(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
		 sd_bus_error *err)
{
	if (!strcmp(prop, "Learning"))
		return sd_bus_message_append(reply, "s", D.learning ? D.learn_target : "");
	if (!strcmp(prop, "MidiFollow"))
		return sd_bus_message_append(reply, "s", D.follow);
	if (!strcmp(prop, "MidiOut"))
		return sd_bus_message_append(reply, "s", D.out_follow);
	if (!strcmp(prop, "Node"))
		return sd_bus_message_append(reply, "s", D.node_name);
	if (!strcmp(prop, "ConfigDir"))
		return sd_bus_message_append(reply, "s", D.no_config ? "" : D.config_dir);
	return sd_bus_message_append(reply, "s", "");
}

static int p_num(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
		 sd_bus_error *err)
{
	if (!strcmp(prop, "MappingCount"))
		return sd_bus_message_append(reply, "u", (uint32_t)n_mappings());
	uint64_t v = 0;
	if (!strcmp(prop, "Received")) v = D.received;
	else if (!strcmp(prop, "MidiEvents")) v = D.midi_events;
	else if (!strcmp(prop, "Rejected")) v = D.rejected;
	else if (!strcmp(prop, "AuthFailures")) v = D.auth_failures;
	else if (!strcmp(prop, "Malformed")) v = D.malformed;
	else if (!strcmp(prop, "Dispatched")) v = D.dispatched;
	else if (!strcmp(prop, "TargetErrors")) v = D.target_errors;
	else if (!strcmp(prop, "MidiDropped")) v = atomic_load(&D.midi_dropped);
	else if (!strcmp(prop, "MidiSent")) v = D.midi_sent;
	return sd_bus_message_append(reply, "t", v);
}

static int p_listeners(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
		       void *ud, sd_bus_error *err)
{
	int r = sd_bus_message_open_container(reply, 'a', "s");
	for (int i = 0; r >= 0 && i < MAX_LISTENERS; i++)
		if (D.lis[i].used)
			r = sd_bus_message_append(reply, "s", D.lis[i].id);
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("Map", SD_BUS_ARGS("s", source, "s", target, "a{sv}", options), SD_BUS_RESULT("s", id), m_map, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("AddMapping", SD_BUS_ARGS("s", json), SD_BUS_RESULT("s", id), m_add_mapping, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("RemoveMapping", SD_BUS_ARGS("s", id), SD_BUS_NO_RESULT, m_remove, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Mappings", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", json), m_mappings, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Learn", SD_BUS_ARGS("s", target, "a{sv}", options), SD_BUS_RESULT("s", id), m_learn, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("CancelLearn", SD_BUS_NO_ARGS, SD_BUS_NO_RESULT, m_cancel_learn, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Listen", SD_BUS_ARGS("s", proto, "s", address, "q", port, "a{sv}", options), SD_BUS_RESULT("s", id), m_listen, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Unlisten", SD_BUS_ARGS("s", id), SD_BUS_NO_RESULT, m_unlisten, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetMidiFollow", SD_BUS_ARGS("s", regex), SD_BUS_NO_RESULT, m_set_follow, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Save", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", path), m_save, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetMidiOut", SD_BUS_ARGS("s", regex), SD_BUS_NO_RESULT, m_set_out, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Profiles", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", json), m_profiles, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Profile", SD_BUS_ARGS("s", profile), SD_BUS_RESULT("s", json), m_profile, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SaveProfile", SD_BUS_ARGS("s", profile, "a{sv}", options), SD_BUS_RESULT("s", path), m_save_profile, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("LoadProfile", SD_BUS_ARGS("s", profile), SD_BUS_RESULT("as", ids), m_load_profile, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("UnloadProfile", SD_BUS_ARGS("s", profile), SD_BUS_NO_RESULT, m_unload_profile, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("DeleteProfile", SD_BUS_ARGS("s", profile), SD_BUS_NO_RESULT, m_delete_profile, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("MappingCount", "u", p_num, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ActiveProfiles", "as", p_active_profiles, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ProfileDirs", "as", p_active_profiles, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Listeners", "as", p_listeners, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Learning", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("MidiFollow", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("MidiOut", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("MidiSent", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("Node", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("ConfigDir", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Received", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("MidiEvents", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("MidiDropped", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("Rejected", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("AuthFailures", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("Malformed", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("Dispatched", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("TargetErrors", "t", p_num, 0, 0),
	SD_BUS_SIGNAL_WITH_ARGS("Learned", SD_BUS_ARGS("s", id, "s", source, "s", target), 0),
	SD_BUS_SIGNAL_WITH_ARGS("LearnCancelled", SD_BUS_ARGS("s", id, "s", reason), 0),
	SD_BUS_SIGNAL_WITH_ARGS("ProfileLoaded", SD_BUS_ARGS("s", profile, "s", device, "u", mappings), 0),
	SD_BUS_SIGNAL_WITH_ARGS("ProfileUnloaded", SD_BUS_ARGS("s", profile, "s", reason), 0),
	SD_BUS_VTABLE_END
};

/* ============================================================ loops */
static void on_bus_io(void *data, int fd, uint32_t mask)
{
	while (sd_bus_process(D.bus, NULL) > 0) {}
	sd_bus_flush(D.bus);
}

static void on_house(void *data, uint64_t exp)
{
	uint64_t now = now_ns();
	for (int i = 0; i < MAX_LISTENERS; i++) {
		struct listener *l = &D.lis[i];
		if (!l->used || !l->spec.tcp)
			continue;
		for (int k = l->n_conns - 1; k >= 0; k--) {
			struct conn *c = l->conns[k];
			if (!c->authed && now - c->opened_ns > AUTH_TIMEOUT_NS) {
				D.auth_failures++;
				reject("no authentication within 5 s, closing", &c->peer);
				conn_close(c);
			}
		}
	}
	pd_house(now);
	bool busy = n_mappings() || n_listeners() || D.learning || D.follow_on || D.out_on || pd_busy();
	if (busy || !D.idle_exit) {
		D.idle_since = 0;
	} else if (!D.idle_since) {
		D.idle_since = now;
	} else if (now - D.idle_since > (uint64_t)D.idle_exit * 1000000000ull) {
		LOG("idle for %d s with nothing mapped, exiting\n", D.idle_exit);
		pw_main_loop_quit(D.loop);
	}
	on_bus_io(NULL, 0, 0);
}

static void on_signal(void *data, int sig)
{
	pw_main_loop_quit(D.loop);
}

static void usage(FILE *f)
{
	fputs("usage: backline-controld [--name NODE] [--config-dir DIR | --no-config] [--midi-follow REGEX]\n"
	      "                         [--idle-exit SECONDS] [--replace] [--verbose]\n"
	      "  OSC and MIDI to any parameter, on the session bus (" BUS_NAME ", " IFACE ").\n"
	      "  It opens no network socket unless a listener is configured or asked for (docs/api/Control1.md).\n"
	      "  --config-dir DIR  mappings and listeners (default $XDG_CONFIG_HOME/backline/control)\n"
	      "  --no-config       start empty and never write a file\n"
	      "  --profile-dir DIR the user's MIDI profiles (default: " MP_DIR_NAME " next to the config dir)\n"
	      "  --idle-exit N     exit after N s with nothing mapped (default 60; 0 = never)\n"
	      "  --no-graph        Params1 and OSC only: never connect to PipeWire (no MIDI, no volume or\n"
	      "                    instance targets); for tests and hosts without an audio server\n", f);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "config-dir", required_argument, NULL, 'c' },
		{ "no-config", no_argument, NULL, 'N' },
		{ "profile-dir", required_argument, NULL, 'P' },
		{ "midi-follow", required_argument, NULL, 'm' },
		{ "idle-exit", required_argument, NULL, 'i' },
		{ "replace", no_argument, NULL, 'r' },
		{ "no-graph", no_argument, NULL, 'G' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c, replace = 0;
	const char *follow = NULL;
	D.idle_exit = 60;
	snprintf(D.node_name, sizeof(D.node_name), "backline.control");
	const char *profile_dir = NULL;
	while ((c = getopt_long(argc, argv, "n:c:NP:m:i:rGvh", opts, NULL)) != -1) {
		switch (c) {
		case 'n': snprintf(D.node_name, sizeof(D.node_name), "%s", optarg); break;
		case 'c': snprintf(D.config_dir, sizeof(D.config_dir), "%s", optarg); break;
		case 'N': D.no_config = true; break;
		case 'P': profile_dir = optarg; break;
		case 'm': follow = optarg; break;
		case 'i': D.idle_exit = atoi(optarg); break;
		case 'r': replace = 1; break;
		case 'G': D.no_graph = true; break;
		case 'v': D.verbose = true; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	default_config_dir();
	default_profile_dirs(profile_dir);
	D.node_id = D.midi_port_id = D.midi_out_port_id = SPA_ID_INVALID;

	pw_init(&argc, &argv);
	D.loop = pw_main_loop_new(NULL);
	D.l = pw_main_loop_get_loop(D.loop);
	pw_loop_add_signal(D.l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(D.l, SIGTERM, on_signal, NULL);
	if (bl_midi_ring_init(&D.out_ring, 1 << 16) < 0) {
		fprintf(stderr, "backline-controld: out of memory\n");
		return 1;
	}
	ctl_nrpn_init(&D.nrpn);
	if (!D.no_graph) {
		D.context = pw_context_new(D.l, NULL, 0);
		D.core = pw_context_connect(D.context, NULL, 0);
		if (!D.core) {
			fprintf(stderr, "backline-controld: cannot connect to PipeWire\n");
			return 1;
		}
		D.registry = pw_core_get_registry(D.core, PW_VERSION_REGISTRY, 0);
		D.graph = bl_graph_new(D.core, &graph_events, NULL);
		D.midi_ev = pw_loop_add_event(D.l, on_midi_event, NULL);

		/* The MIDI input: a node with one MIDI port and no audio. It is never
		 * autoconnected; sources reach it by midi-follow or by a link someone
		 * makes. */
		struct pw_properties *props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Midi", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS, "Midi/Sink",
			PW_KEY_NODE_DESCRIPTION, "Backline control (MIDI learn and mapping)", PW_KEY_NODE_AUTOCONNECT, "false",
			"node.passive", "true", "backline.dbus.name", BUS_NAME, NULL);
		D.filter = pw_filter_new(D.core, D.node_name, props);
		pw_filter_add_listener(D.filter, &D.filter_listener, &filter_events, NULL);
		D.midi_port = pw_filter_add_port(D.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
						 pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "midi_in", NULL),
						 NULL, 0);
		/* MIDI out: feedback to controllers (motor faders, LED rings), linked by
		 * SetMidiOut. The process callback drains a ring the main loop fills. */
		D.midi_out_port = pw_filter_add_port(D.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
						     pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "midi_out", NULL),
						     NULL, 0);
		if (pw_filter_connect(D.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
			fprintf(stderr, "backline-controld: pw_filter_connect failed\n");
			return 1;
		}
	}

	int r = sd_bus_open_user(&D.bus);
	if (r < 0) {
		fprintf(stderr, "backline-controld: session bus: %s\n", strerror(-r));
		return 1;
	}
	r = sd_bus_add_object_vtable(D.bus, NULL, OBJ_PATH, IFACE, vtable, NULL);
	if (r < 0) {
		fprintf(stderr, "backline-controld: %s\n", strerror(-r));
		return 1;
	}
	struct pd_hooks ph = { .osc_send = pd_send, .changed = on_param_changed };
	r = pd_init(D.bus, D.l, &ph, D.verbose);
	if (r < 0) {
		fprintf(stderr, "backline-controld: Params1: %s\n", strerror(-r));
		return 1;
	}
	D.tick = pw_loop_add_timer(D.l, on_tick, NULL);
	D.bus_io = pw_loop_add_io(D.l, sd_bus_get_fd(D.bus), SPA_IO_IN, false, on_bus_io, NULL);
	D.learn_timer = pw_loop_add_timer(D.l, on_learn_timeout, NULL);
	D.house_timer = pw_loop_add_timer(D.l, on_house, NULL);
	struct timespec hk = { 0, 250 * 1000000L };
	pw_loop_update_timer(D.l, D.house_timer, &hk, &hk, false);

	if (D.core) {
		pw_core_sync(D.core, PW_ID_CORE, 0);
		for (int i = 0; i < 20; i++)
			pw_loop_iterate(D.l, 10);
	}
	if (!D.no_config)
		load_config();
	profiles_reload();
	profiles_scan();
	if (follow) {
		char e[300];
		if (set_follow(follow, e, sizeof(e)) < 0) {
			fprintf(stderr, "backline-controld: --midi-follow: %s\n", e);
			return 2;
		}
	}
	/* the name last: a client that sees it sees the loaded mappings */
	r = sd_bus_request_name(D.bus, BUS_NAME, replace ? SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT
							 : SD_BUS_NAME_ALLOW_REPLACEMENT);
	if (r < 0) {
		fprintf(stderr, "backline-controld: cannot own %s: %s\n", BUS_NAME, strerror(-r));
		return 1;
	}
	LOG("on the session bus as %s; node %s; %d mappings, %d listeners%s\n", BUS_NAME, D.node_name, n_mappings(),
	    n_listeners(), n_listeners() ? "" : " (no network socket)");
	on_bus_io(NULL, 0, 0);

	pw_main_loop_run(D.loop);

	for (int i = 0; i < MAX_LISTENERS; i++)
		if (D.lis[i].used)
			listener_close(&D.lis[i]);
	for (int i = 0; i < MAX_VNODES; i++)
		if (D.vnodes[i].used)
			vnode_drop(&D.vnodes[i]);
	if (D.follow_on)
		regfree(&D.follow_re);
	if (D.out_on)
		regfree(&D.out_re);
	for (int i = 0; i < MAX_ACTIVE_PROFILES; i++)
		if (D.active[i].used && D.active[i].re_ok)
			regfree(&D.active[i].re);
	mp_set_free(&D.profiles);
	pd_fini();
	if (D.filter)
		pw_filter_destroy(D.filter);
	bl_midi_ring_free(&D.out_ring);
	if (D.graph)
		bl_graph_destroy(D.graph);
	if (D.registry)
		pw_proxy_destroy((struct pw_proxy *)D.registry);
	sd_bus_flush_close_unref(D.bus);
	if (D.core)
		pw_core_disconnect(D.core);
	if (D.context)
		pw_context_destroy(D.context);
	pw_main_loop_destroy(D.loop);
	pw_deinit();
	return 0;
}
