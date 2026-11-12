/* Params1 in controld (paramsd.h, docs/api/Params1.md).
 * SPDX-License-Identifier: MIT
 *
 * The program that registers a root is the source of truth: a Set from a
 * client is validated (type, range, read-only, role) and passed to the owner
 * as a unicast signal; the store changes only on the owner's Update. Main
 * loop only.
 */
#define _GNU_SOURCE
#include "paramsd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#include "bl_meters.h"
#include "policy.h"

#define ERR_BUSY "io.github.manuaudio.Backline.Error.Busy"
#define ERR_NOTFOUND "io.github.manuaudio.Backline.Error.NotFound"
#define ERR_ACCESS "org.freedesktop.DBus.Error.AccessDenied"
#define SCHEMA "backline.params/1"

#define PD_MAX_ROOTS 16
#define PD_MAX_PARAMS 20000
#define PD_MAX_METERS 1024
#define PD_MAX_SUBS 64
#define PD_SUBS_PER_PEER 32
#define PD_PEERS_PER_LISTENER 32
#define PD_MAX_GEST 1024
#define PD_UDP_SUB_TTL_NS (30ull * 1000000000ull)
#define PD_UDP_GEST_TTL_NS (60ull * 1000000000ull)
#define PD_TICK_NS (5ull * 1000000ull)
#define PD_MAX_LISTENERS 8
#define PD_UDP_MAX_ANSWER 256  /* values one UDP request may produce (no amplification) */

struct root {
	bool used;
	char root[PRM_SEG_MAX + 2];
	char owner[128];
	char token[40];
	struct prm_role roles[PRM_MAX_ROLES];
	int n_roles;
	int meter_fd;
	const struct bl_meters_hdr *meters;
	size_t meters_len;
	char (*meter_ids)[PRM_SEG_MAX + 1];
	uint32_t n_meters;
};

struct sub {
	bool used;
	struct pd_peer peer;
	char pattern[PRM_PATH_MAX];
	bool norm, meters;
	double rate;
	uint64_t next_ns, expires_ns;
	uint32_t *dirty;
	size_t n_dirty, cap_dirty;
};

enum gest_kind { G_DBUS, G_OSC, G_INTERNAL };
struct gest {
	bool used;
	enum gest_kind kind;
	char path[PRM_PATH_MAX];
	char client[128];
	char role[32];
	struct pd_peer peer;
	uint64_t last_ns;
};

static struct {
	sd_bus *bus;
	struct pw_loop *loop;
	struct spa_source *tick;
	bool tick_on;
	struct pd_hooks hooks;
	bool verbose;
	struct prm_store *store;
	struct root roots[PD_MAX_ROOTS];
	struct sub subs[PD_MAX_SUBS];
	struct gest gests[PD_MAX_GEST];
	struct ctl_bucket err_bucket[PD_MAX_LISTENERS];
	uint64_t set_requests, refused;
	sd_bus_slot *vtable_slot, *match_slot;
} P;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void plog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "backline-controld: params: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#define VLOG(...) do { if (P.verbose) plog(__VA_ARGS__); } while (0)

static void tick_update(void);

/* ============================================================ roots */
static int root_index(const char *root)
{
	for (int i = 0; i < PD_MAX_ROOTS; i++)
		if (P.roots[i].used && !strcmp(P.roots[i].root, root))
			return i;
	return -1;
}

static struct root *root_of_path(const char *path, int *idx)
{
	char r[PRM_SEG_MAX + 2];
	if (!prm_path_root(path, r, sizeof(r)))
		return NULL;
	int i = root_index(r);
	if (idx)
		*idx = i;
	return i >= 0 ? &P.roots[i] : NULL;
}

static void meters_drop(struct root *r)
{
	if (r->meters)
		munmap((void *)r->meters, r->meters_len);
	if (r->meter_fd > 0)
		close(r->meter_fd);
	r->meters = NULL;
	r->meter_fd = -1;
}

static void emit_structure(const char *root)
{
	sd_bus_emit_signal(P.bus, PD_OBJ_PATH, PD_IFACE, "Structure", "s", root);
	sd_bus_emit_properties_changed(P.bus, PD_OBJ_PATH, PD_IFACE, "Roots", "ParamCount", NULL);
}

static void gest_drop_root(const char *root);

static void root_remove(int i, const char *why)
{
	struct root *r = &P.roots[i];
	char name[sizeof(r->root)];
	snprintf(name, sizeof(name), "%s", r->root);
	int n = prm_store_remove_matching(P.store, NULL, i);
	gest_drop_root(name);
	meters_drop(r);
	free(r->meter_ids);
	memset(r, 0, sizeof(*r));
	r->meter_fd = -1;
	plog("%s removed (%s), %d parameters\n", name, why, n);
	emit_structure(name);
}

static struct root *root_for_token(sd_bus_message *m, const char *token, sd_bus_error *err)
{
	const char *sender = sd_bus_message_get_sender(m);
	for (int i = 0; i < PD_MAX_ROOTS; i++) {
		struct root *r = &P.roots[i];
		if (r->used && !strcmp(r->token, token)) {
			if (sender && strcmp(sender, r->owner)) {
				sd_bus_error_setf(err, ERR_ACCESS, "the token belongs to another client");
				return NULL;
			}
			return r;
		}
	}
	sd_bus_error_setf(err, ERR_NOTFOUND, "no root has that token");
	return NULL;
}

/* ============================================================ values */
struct pval {
	char type;            /* d i b s */
	double num;
	const char *str;
};

static int read_variant(sd_bus_message *m, struct pval *v)
{
	char t;
	const char *c;
	int r = sd_bus_message_peek_type(m, &t, &c);
	if (r <= 0 || t != 'v' || !c)
		return -EINVAL;
	if ((r = sd_bus_message_enter_container(m, 'v', c)) < 0)
		return r;
	v->str = NULL;
	switch (c[0]) {
	case 'd': v->type = 'd'; r = sd_bus_message_read(m, "d", &v->num); break;
	case 'i': { int32_t x; r = sd_bus_message_read(m, "i", &x); v->type = 'd'; v->num = x; break; }
	case 'u': { uint32_t x; r = sd_bus_message_read(m, "u", &x); v->type = 'd'; v->num = x; break; }
	case 'x': { int64_t x; r = sd_bus_message_read(m, "x", &x); v->type = 'd'; v->num = (double)x; break; }
	case 't': { uint64_t x; r = sd_bus_message_read(m, "t", &x); v->type = 'd'; v->num = (double)x; break; }
	case 'n': { int16_t x; r = sd_bus_message_read(m, "n", &x); v->type = 'd'; v->num = x; break; }
	case 'q': { uint16_t x; r = sd_bus_message_read(m, "q", &x); v->type = 'd'; v->num = x; break; }
	case 'y': { uint8_t x; r = sd_bus_message_read(m, "y", &x); v->type = 'd'; v->num = x; break; }
	case 'b': { int x; r = sd_bus_message_read(m, "b", &x); v->type = 'b'; v->num = x ? 1 : 0; break; }
	case 's': v->type = 's'; r = sd_bus_message_read(m, "s", &v->str); break;
	default:
		sd_bus_message_skip(m, c);
		sd_bus_message_exit_container(m);
		return -EINVAL;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

/* A requested value in the parameter's terms: *num for numbers, str for a
 * string. 0, or -1 with a reason. */
static int convert(const struct prm *p, const struct pval *v, bool norm, double *num, char *str, size_t sn, char *why,
		   size_t wn)
{
	*num = 0;
	str[0] = 0;
	if (p->type == PRM_STRING) {
		if (v->type != 's') {
			snprintf(why, wn, "%s is a string", p->path);
			return -1;
		}
		if (strlen(v->str) >= sn) {
			snprintf(why, wn, "a string value is at most %zu bytes", sn - 1);
			return -1;
		}
		snprintf(str, sn, "%s", v->str);
		return 0;
	}
	if (p->type == PRM_TRIGGER) {
		*num = 1;
		return 0;
	}
	if (v->type == 's') {
		int i = p->type == PRM_ENUM || p->type == PRM_BOOL ? prm_label_index(p, v->str) : -1;
		if (i < 0 && p->type == PRM_BOOL && (!strcasecmp(v->str, "on") || !strcasecmp(v->str, "true")))
			i = 1;
		if (i < 0 && p->type == PRM_BOOL && (!strcasecmp(v->str, "off") || !strcasecmp(v->str, "false")))
			i = 0;
		if (i < 0) {
			snprintf(why, wn, "'%s' is not a value of %s", v->str, p->path);
			return -1;
		}
		*num = i;
		return 0;
	}
	if (!isfinite(v->num)) {
		snprintf(why, wn, "not a finite number");
		return -1;
	}
	*num = norm ? prm_from_norm(p, v->num) : prm_clamp(p, v->num);
	return 0;
}

static int append_value(sd_bus_message *m, const struct prm *p, double num, const char *str)
{
	switch (p->type) {
	case PRM_STRING: return sd_bus_message_append(m, "v", "s", str);
	case PRM_BOOL:
	case PRM_TRIGGER: return sd_bus_message_append(m, "v", "b", num > 0.5);
	case PRM_INT:
	case PRM_ENUM: return sd_bus_message_append(m, "v", "i", (int32_t)lround(num));
	default: return sd_bus_message_append(m, "v", "d", num);
	}
}

static void signal_owner(struct root *r, const char *member, const struct prm *p, double num, const char *str,
			 int begin, const char *client, const char *role)
{
	sd_bus_message *m = NULL;
	if (sd_bus_message_new_signal(P.bus, &m, PD_OBJ_PATH, PD_IFACE, member) < 0)
		return;
	sd_bus_message_set_destination(m, r->owner);
	int rr = sd_bus_message_append(m, "s", p->path);
	if (rr >= 0)
		rr = begin < 0 ? append_value(m, p, num, str) : sd_bus_message_append(m, "b", begin);
	if (rr >= 0)
		rr = sd_bus_message_append(m, "ss", client ? client : "", role && *role ? role : "all");
	if (rr >= 0)
		sd_bus_send(P.bus, m, NULL);
	sd_bus_message_unref(m);
}

static int check_write(const struct prm *p, struct root *r, const char *role, char *why, size_t n)
{
	if (!prm_role_may_write(r->roles, r->n_roles, role, p, why, n)) {
		P.refused++;
		return -1;
	}
	return 0;
}

static int request_set(struct prm *p, const struct pval *v, bool norm, const char *client, const char *role, char *why,
		       size_t wn)
{
	struct root *r = &P.roots[p->root];
	if (check_write(p, r, role, why, wn) < 0)
		return -EACCES;
	double num;
	char str[PRM_STRING_MAX];
	if (convert(p, v, norm, &num, str, sizeof(str), why, wn) < 0)
		return -EINVAL;
	signal_owner(r, "Set", p, num, str, -1, client, role);
	P.set_requests++;
	VLOG("set %s = %g%s%s by %s (%s)\n", p->path, num, str[0] ? " " : "", str, client, role && *role ? role : "all");
	return 0;
}

/* ============================================================ gestures */
static struct gest *gest_find(enum gest_kind k, const char *path, const char *client)
{
	for (int i = 0; i < PD_MAX_GEST; i++)
		if (P.gests[i].used && P.gests[i].kind == k && !strcmp(P.gests[i].path, path) &&
		    !strcmp(P.gests[i].client, client))
			return &P.gests[i];
	return NULL;
}

static void gest_end(struct gest *g, const char *why)
{
	struct prm *p = prm_store_get(P.store, g->path);
	if (p && P.roots[p->root].used)
		signal_owner(&P.roots[p->root], "Gesture", p, 0, NULL, 0, g->client, g->role);
	VLOG("gesture end %s by %s (%s)\n", g->path, g->client, why);
	g->used = false;
}

static void gest_drop_root(const char *root)
{
	size_t rl = strlen(root);
	for (int i = 0; i < PD_MAX_GEST; i++)
		if (P.gests[i].used && !strncmp(P.gests[i].path, root, rl) && P.gests[i].path[rl] == '/')
			P.gests[i].used = false; /* its owner is gone: nobody to tell */
}

static int request_gesture(struct prm *p, bool begin, enum gest_kind kind, const char *client, const char *role,
			   const struct pd_peer *peer, char *why, size_t wn)
{
	struct root *r = &P.roots[p->root];
	if (check_write(p, r, role, why, wn) < 0)
		return -EACCES;
	if (!p->automatable) {
		snprintf(why, wn, "%s is not automatable", p->path);
		return -EINVAL;
	}
	struct gest *g = gest_find(kind, p->path, client);
	if (begin) {
		if (g) {
			g->last_ns = now_ns();
			return 0; /* already open: once is enough */
		}
		for (int i = 0; i < PD_MAX_GEST && !g; i++)
			if (!P.gests[i].used)
				g = &P.gests[i];
		if (!g) {
			snprintf(why, wn, "too many open gestures");
			return -EBUSY;
		}
		memset(g, 0, sizeof(*g));
		g->used = true;
		g->kind = kind;
		snprintf(g->path, sizeof(g->path), "%s", p->path);
		snprintf(g->client, sizeof(g->client), "%s", client);
		snprintf(g->role, sizeof(g->role), "%s", role && *role ? role : "all");
		if (peer)
			g->peer = *peer;
		g->last_ns = now_ns();
		signal_owner(r, "Gesture", p, 0, NULL, 1, client, g->role);
		VLOG("gesture begin %s by %s\n", p->path, client);
		return 0;
	}
	if (g)
		gest_end(g, "released");
	return 0;
}

/* ============================================================ OSC replies */
static size_t value_msg(const struct prm *p, bool norm, uint8_t *buf, size_t cap)
{
	if (p->type == PRM_TRIGGER)
		return 0;
	if (norm) {
		char a[PRM_PATH_MAX + 8];
		snprintf(a, sizeof(a), "%s/norm", p->path);
		return osc_build(buf, cap, a, "f", prm_to_norm(p, p->value));
	}
	if (p->type == PRM_STRING)
		return osc_build(buf, cap, p->path, "ss", p->str, p->text);
	return osc_build(buf, cap, p->path, "fs", p->value, p->text);
}

static size_t bundle_cap(const struct pd_peer *peer)
{
	return peer->tcp ? 8192 : 1400;
}

struct bundler {
	const struct pd_peer *peer;
	uint8_t buf[8192];
	size_t len, cap;
	int msgs;
};

static void bundler_init(struct bundler *b, const struct pd_peer *peer)
{
	b->peer = peer;
	b->cap = bundle_cap(peer);
	b->len = osc_bundle_begin(b->buf, b->cap);
	b->msgs = 0;
}

static void bundler_flush(struct bundler *b)
{
	if (b->msgs)
		P.hooks.osc_send(P.hooks.data, b->peer, b->buf, b->len);
	b->len = osc_bundle_begin(b->buf, b->cap);
	b->msgs = 0;
}

static void bundler_add(struct bundler *b, const uint8_t *msg, size_t n)
{
	if (!n)
		return;
	size_t l = osc_bundle_add(b->buf, b->len, b->cap, msg, n);
	if (!l) {
		bundler_flush(b);
		l = osc_bundle_add(b->buf, b->len, b->cap, msg, n);
		if (!l)
			return; /* larger than a packet on its own */
	}
	b->len = l;
	b->msgs++;
}

static void reply_error(const struct pd_peer *peer, const char *path, const char *why)
{
	if (peer->listener < 0 || peer->listener >= PD_MAX_LISTENERS)
		return;
	if (!ctl_bucket_take(&P.err_bucket[peer->listener], 10, now_ns()))
		return;
	uint8_t buf[640];
	size_t n = osc_build(buf, sizeof(buf), "/backline/error", "ss", path, why);
	if (n)
		P.hooks.osc_send(P.hooks.data, peer, buf, n);
}

/* ============================================================ subscriptions */
static bool same_peer(const struct pd_peer *a, const struct pd_peer *b)
{
	if (a->listener != b->listener || a->tcp != b->tcp)
		return false;
	if (a->tcp)
		return a->conn == b->conn;
	return a->alen == b->alen && !memcmp(&a->addr, &b->addr, a->alen);
}

static void sub_free(struct sub *s)
{
	free(s->dirty);
	memset(s, 0, sizeof(*s));
	sd_bus_emit_properties_changed(P.bus, PD_OBJ_PATH, PD_IFACE, "Subscriptions", NULL);
}

static void mark_dirty(struct prm *p)
{
	for (int i = 0; i < PD_MAX_SUBS; i++) {
		struct sub *s = &P.subs[i];
		if (!s->used || s->meters || (p->dirty & (1ull << i)) || !prm_match(s->pattern, p->path))
			continue;
		if (s->n_dirty == s->cap_dirty) {
			size_t nc = s->cap_dirty ? s->cap_dirty * 2 : 64;
			uint32_t *nd = realloc(s->dirty, nc * sizeof(*nd));
			if (!nd)
				continue;
			s->dirty = nd;
			s->cap_dirty = nc;
		}
		s->dirty[s->n_dirty++] = p->slot;
		p->dirty |= 1ull << i;
	}
	tick_update();
}

struct each_ctx {
	struct bundler *b;
	const char *pattern;
	bool norm;
	int count, max;
};

static int send_match(void *data, struct prm *p)
{
	struct each_ctx *c = data;
	if (!prm_match(c->pattern, p->path))
		return 0;
	if (c->count >= c->max)
		return 1;
	uint8_t msg[700];
	bundler_add(c->b, msg, value_msg(p, c->norm, msg, sizeof(msg)));
	c->count++;
	return 0;
}

static void send_all_matching(const struct pd_peer *peer, const char *pattern, bool norm)
{
	struct bundler *b = malloc(sizeof(*b));
	if (!b)
		return;
	bundler_init(b, peer);
	/* over UDP the source address can be forged: one packet never makes more
	 * than PD_UDP_MAX_ANSWER replies (use TCP or a narrower pattern) */
	struct each_ctx c = { b, pattern, norm, 0, peer->tcp ? INT32_MAX : PD_UDP_MAX_ANSWER };
	bool cut = prm_store_each(P.store, send_match, &c) != 0;
	bundler_flush(b);
	free(b);
	if (cut)
		reply_error(peer, pattern, "more matches than one UDP answer carries: use TCP or a narrower pattern");
}

static void flush_sub(int i, uint64_t now)
{
	struct sub *s = &P.subs[i];
	struct bundler *b = malloc(sizeof(*b));
	if (!b)
		return;
	bundler_init(b, &s->peer);
	if (s->meters) {
		for (int r = 0; r < PD_MAX_ROOTS; r++) {
			struct root *rt = &P.roots[r];
			if (!rt->used || !rt->meters || !rt->n_meters)
				continue;
			struct bl_meter *m = malloc(rt->n_meters * sizeof(*m));
			if (m && bl_meters_read(rt->meters, rt->n_meters, m)) {
				for (uint32_t k = 0; k < rt->n_meters; k++) {
					char name[PRM_PATH_MAX], addr[PRM_PATH_MAX + 8];
					snprintf(name, sizeof(name), "%s/%s", rt->root, rt->meter_ids[k]);
					if (!prm_match(s->pattern, name))
						continue;
					snprintf(addr, sizeof(addr), "%s/meter/%s", rt->root, rt->meter_ids[k]);
					uint8_t msg[400];
					bundler_add(b, msg, osc_build(msg, sizeof(msg), addr, "fi", (double)m[k].db,
								     (int)(m[k].flags & BL_METER_CLIP)));
				}
			}
			free(m);
		}
	} else {
		for (size_t k = 0; k < s->n_dirty; k++) {
			struct prm *p = prm_store_slot(P.store, s->dirty[k]);
			if (!p)
				continue;
			p->dirty &= ~(1ull << i);
			if (!prm_match(s->pattern, p->path))
				continue;
			uint8_t msg[700];
			bundler_add(b, msg, value_msg(p, s->norm, msg, sizeof(msg)));
		}
		s->n_dirty = 0;
	}
	bundler_flush(b);
	free(b);
	s->next_ns = now + (uint64_t)(1e9 / s->rate);
}

static void on_tick(void *data, uint64_t exp)
{
	uint64_t now = now_ns();
	for (int i = 0; i < PD_MAX_SUBS; i++) {
		struct sub *s = &P.subs[i];
		if (s->used && (s->meters || s->n_dirty) && now >= s->next_ns)
			flush_sub(i, now);
	}
	tick_update();
}

static void tick_update(void)
{
	bool need = false;
	for (int i = 0; i < PD_MAX_SUBS && !need; i++)
		need = P.subs[i].used && (P.subs[i].meters || P.subs[i].n_dirty);
	if (need == P.tick_on || !P.tick)
		return;
	P.tick_on = need;
	struct timespec v = { 0, need ? (long)PD_TICK_NS : 0 };
	pw_loop_update_timer(P.loop, P.tick, &v, need ? &v : NULL, false);
}

static void subscribe(const struct pd_peer *peer, const char *pattern, double rate, bool norm, bool meters)
{
	uint64_t now = now_ns();
	int same = -1, n_peer = 0, free_i = -1;
	int peers_on_listener = 0;
	struct pd_peer seen[PD_MAX_SUBS];
	int n_seen = 0;
	for (int i = 0; i < PD_MAX_SUBS; i++) {
		struct sub *s = &P.subs[i];
		if (!s->used) {
			if (free_i < 0)
				free_i = i;
			continue;
		}
		if (s->peer.listener == peer->listener) {
			bool known = false;
			for (int k = 0; k < n_seen && !known; k++)
				known = same_peer(&seen[k], &s->peer);
			if (!known) {
				seen[n_seen++] = s->peer;
				peers_on_listener++;
			}
		}
		if (!same_peer(&s->peer, peer))
			continue;
		n_peer++;
		s->expires_ns = now + PD_UDP_SUB_TTL_NS; /* any sub renews them all */
		if (s->meters == meters && !strcmp(s->pattern, pattern))
			same = i;
	}
	if (pattern[0] != '/') {
		reply_error(peer, pattern, "a pattern starts with /");
		return;
	}
	struct sub *s;
	if (same >= 0) {
		s = &P.subs[same];
	} else {
		if (n_peer >= PD_SUBS_PER_PEER || free_i < 0 || (n_peer == 0 && peers_on_listener >= PD_PEERS_PER_LISTENER)) {
			reply_error(peer, pattern, "too many subscriptions");
			return;
		}
		s = &P.subs[free_i];
		memset(s, 0, sizeof(*s));
		s->used = true;
		s->peer = *peer;
		snprintf(s->pattern, sizeof(s->pattern), "%s", pattern);
		sd_bus_emit_properties_changed(P.bus, PD_OBJ_PATH, PD_IFACE, "Subscriptions", NULL);
	}
	double maxr = meters ? 30 : 60, defr = meters ? 15 : 30;
	if (!(rate > 0))
		rate = defr;
	if (rate > maxr)
		rate = maxr;
	s->rate = rate;
	s->norm = norm;
	s->meters = meters;
	s->expires_ns = now + PD_UDP_SUB_TTL_NS;
	s->next_ns = now;
	VLOG("%s subscribed %s%s at %g/s (%s)\n", peer->label, meters ? "meters " : "", pattern, rate, norm ? "norm" : "values");
	if (!meters)
		send_all_matching(peer, pattern, norm);
	tick_update();
}

static void unsubscribe(const struct pd_peer *peer, const char *pattern)
{
	for (int i = 0; i < PD_MAX_SUBS; i++) {
		struct sub *s = &P.subs[i];
		if (s->used && same_peer(&s->peer, peer) && (!strcmp(pattern, "*") || !strcmp(s->pattern, pattern)))
			sub_free(s);
	}
	tick_update();
}

void pd_peer_gone(int listener, void *conn)
{
	for (int i = 0; i < PD_MAX_SUBS; i++) {
		struct sub *s = &P.subs[i];
		if (s->used && s->peer.listener == listener && (!conn || s->peer.conn == conn))
			sub_free(s);
	}
	for (int i = 0; i < PD_MAX_GEST; i++) {
		struct gest *g = &P.gests[i];
		if (g->used && g->kind == G_OSC && g->peer.listener == listener && (!conn || g->peer.conn == conn))
			gest_end(g, "connection closed");
	}
	tick_update();
}

void pd_house(uint64_t now)
{
	for (int i = 0; i < PD_MAX_SUBS; i++) {
		struct sub *s = &P.subs[i];
		if (s->used && !s->peer.tcp && now > s->expires_ns) {
			VLOG("%s: subscription %s expired\n", s->peer.label, s->pattern);
			sub_free(s);
		}
	}
	for (int i = 0; i < PD_MAX_GEST; i++) {
		struct gest *g = &P.gests[i];
		if (g->used && g->kind == G_OSC && !g->peer.tcp && now - g->last_ns > PD_UDP_GEST_TTL_NS)
			gest_end(g, "60 s without a message");
	}
	tick_update();
}

/* ============================================================ OSC in */
static bool osc_value(const struct osc_arg *a, struct pval *v)
{
	v->str = NULL;
	switch (a->type) {
	case 's': v->type = 's'; v->str = a->s; return true;
	case 'T': v->type = 'b'; v->num = 1; return true;
	case 'F': v->type = 'b'; v->num = 0; return true;
	default: v->type = 'd'; return osc_arg_number(a, &v->num);
	}
}

static void touch_peer_gestures(const struct pd_peer *peer)
{
	uint64_t now = now_ns();
	for (int i = 0; i < PD_MAX_GEST; i++)
		if (P.gests[i].used && P.gests[i].kind == G_OSC && same_peer(&P.gests[i].peer, peer))
			P.gests[i].last_ns = now;
}

bool pd_osc(const struct pd_peer *peer, const struct osc_msg *m)
{
	const char *a = m->address;
	touch_peer_gestures(peer);
	if (!strncmp(a, "/backline/", 10)) {
		const char *cmd = a + 10;
		const char *s0 = m->argc > 0 && m->argv[0].type == 's' ? m->argv[0].s : NULL;
		double rate = 0;
		bool norm = false;
		for (int i = 1; i < m->argc; i++) {
			double x;
			if (m->argv[i].type == 's' && !strcmp(m->argv[i].s, "norm"))
				norm = true;
			else if (osc_arg_number(&m->argv[i], &x))
				rate = x;
		}
		if (!strcmp(cmd, "get")) {
			if (s0)
				send_all_matching(peer, s0, norm);
			return true;
		}
		if (!strcmp(cmd, "sub") || !strcmp(cmd, "meters")) {
			if (s0 && strlen(s0) < PRM_PATH_MAX)
				subscribe(peer, s0, rate, norm, cmd[0] == 'm');
			return true;
		}
		if (!strcmp(cmd, "unsub")) {
			if (s0)
				unsubscribe(peer, s0);
			return true;
		}
		if (!strcmp(cmd, "renew")) {
			uint64_t now = now_ns();
			for (int i = 0; i < PD_MAX_SUBS; i++)
				if (P.subs[i].used && same_peer(&P.subs[i].peer, peer))
					P.subs[i].expires_ns = now + PD_UDP_SUB_TTL_NS;
			return true;
		}
		return false;
	}
	/* PATH, PATH/norm, PATH/touch */
	char path[PRM_PATH_MAX];
	if (strlen(a) >= sizeof(path))
		return false;
	snprintf(path, sizeof(path), "%s", a);
	int suffix = 0; /* 1 norm, 2 touch */
	char *slash = strrchr(path, '/');
	if (slash && !strcmp(slash, "/norm")) {
		*slash = 0;
		suffix = 1;
	} else if (slash && !strcmp(slash, "/touch")) {
		*slash = 0;
		suffix = 2;
	}
	if (!root_of_path(path, NULL))
		return false; /* not a registered root: Control1 mappings may want it */
	struct prm *p = prm_store_get(P.store, path);
	if (!p) {
		reply_error(peer, a, "no such parameter");
		return true;
	}
	char why[256] = "";
	if (m->argc == 0 && suffix != 2) {
		uint8_t msg[700];
		size_t n = value_msg(p, suffix == 1, msg, sizeof(msg));
		if (n)
			P.hooks.osc_send(P.hooks.data, peer, msg, n);
		return true;
	}
	struct pval v;
	if (m->argc == 0 || !osc_value(&m->argv[0], &v)) {
		reply_error(peer, a, "expected a value");
		return true;
	}
	int r;
	if (suffix == 2)
		r = request_gesture(p, v.type == 's' ? false : v.num > 0.5, G_OSC, peer->label, peer->role, peer, why,
				    sizeof(why));
	else
		r = request_set(p, &v, suffix == 1, peer->label, peer->role, why, sizeof(why));
	if (r < 0)
		reply_error(peer, a, why);
	return true;
}

/* ============================================================ internal */
int pd_set(const char *path, bool norm, double value, const char *client, const char *role, char *why, size_t n)
{
	struct prm *p = prm_store_get(P.store, path);
	if (!p) {
		snprintf(why, n, "no parameter %s", path);
		return -1;
	}
	struct pval v = { 'd', value, NULL };
	return request_set(p, &v, norm, client, role, why, n) < 0 ? -1 : 0;
}

int pd_gesture(const char *path, bool begin, const char *client, const char *role, char *why, size_t n)
{
	struct prm *p = prm_store_get(P.store, path);
	if (!p) {
		snprintf(why, n, "no parameter %s", path);
		return -1;
	}
	return request_gesture(p, begin, G_INTERNAL, client, role, NULL, why, n) < 0 ? -1 : 0;
}

struct prm *pd_get(const char *path)
{
	return prm_store_get(P.store, path);
}

bool pd_busy(void)
{
	for (int i = 0; i < PD_MAX_ROOTS; i++)
		if (P.roots[i].used)
			return true;
	return pd_subscriptions() > 0;
}

uint32_t pd_subscriptions(void)
{
	uint32_t n = 0;
	for (int i = 0; i < PD_MAX_SUBS; i++)
		n += P.subs[i].used;
	return n;
}

/* ============================================================ D-Bus: program */
static int parse_params_into(json_object *arr, int ri, struct prm ***out, int *n_out, char *err, size_t en)
{
	*out = NULL;
	*n_out = 0;
	if (!json_object_is_type(arr, json_type_array)) {
		snprintf(err, en, "params is an array");
		return -1;
	}
	size_t n = json_object_array_length(arr);
	if (prm_store_count(P.store) + n > PD_MAX_PARAMS) {
		snprintf(err, en, "too many parameters (at most %d in all roots)", PD_MAX_PARAMS);
		return -1;
	}
	struct prm **v = calloc(n ? n : 1, sizeof(*v));
	for (size_t i = 0; i < n; i++) {
		char e[300];
		if (prm_from_json(json_object_array_get_idx(arr, i), P.roots[ri].root, &v[i], e, sizeof(e)) < 0) {
			snprintf(err, en, "parameter %zu: %s", i, e);
			for (size_t k = 0; k < i; k++)
				prm_free(v[k]);
			free(v);
			return -1;
		}
		v[i]->root = ri;
	}
	*out = v;
	*n_out = (int)n;
	return 0;
}

static int set_meter_ids(struct root *r, json_object *arr, char *err, size_t en)
{
	if (!json_object_is_type(arr, json_type_array)) {
		snprintf(err, en, "meters is an array of ids");
		return -1;
	}
	size_t n = json_object_array_length(arr);
	if (n > PD_MAX_METERS) {
		snprintf(err, en, "at most %d meters", PD_MAX_METERS);
		return -1;
	}
	char (*ids)[PRM_SEG_MAX + 1] = calloc(n ? n : 1, sizeof(*ids));
	for (size_t i = 0; i < n; i++) {
		const char *s = json_object_get_string(json_object_array_get_idx(arr, i));
		char test[PRM_PATH_MAX];
		snprintf(test, sizeof(test), "/m/%s", s ? s : "");
		if (!s || strlen(s) > PRM_SEG_MAX || strchr(s, '/') || !prm_path_valid(test, false)) {
			snprintf(err, en, "meter id '%s' is not one path segment", s ? s : "");
			free(ids);
			return -1;
		}
		snprintf(ids[i], sizeof(ids[i]), "%s", s);
	}
	free(r->meter_ids);
	r->meter_ids = ids;
	r->n_meters = (uint32_t)n;
	meters_drop(r); /* the table must be sent again for the new ids */
	return 0;
}

static int m_register(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *root, *js;
	int r = sd_bus_message_read(m, "ss", &root, &js);
	if (r < 0)
		return r;
	if (!prm_path_valid(root, true))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
					 "'%s' is not a root: one segment like /console (not /backline)", root);
	const char *sender = sd_bus_message_get_sender(m);
	int ri = root_index(root);
	if (ri >= 0 && sender && strcmp(P.roots[ri].owner, sender))
		return sd_bus_error_setf(err, ERR_BUSY, "%s is registered by %s", root, P.roots[ri].owner);
	json_object *o = json_tokener_parse(js), *v;
	if (!o || !json_object_is_type(o, json_type_object)) {
		json_object_put(o);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "not a JSON object");
	}
	if (!json_object_object_get_ex(o, "schema", &v) || strcmp(json_object_get_string(v), SCHEMA)) {
		json_object_put(o);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "the schema is %s", SCHEMA);
	}
	json_object_object_foreach(o, key, val) {
		(void)val;
		if (strcmp(key, "schema") && strcmp(key, "params") && strcmp(key, "meters")) {
			json_object_put(o);
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown key '%s'", key);
		}
	}
	bool fresh = ri < 0;
	if (fresh) {
		for (int i = 0; i < PD_MAX_ROOTS && ri < 0; i++)
			if (!P.roots[i].used)
				ri = i;
		if (ri < 0) {
			json_object_put(o);
			return sd_bus_error_setf(err, ERR_BUSY, "at most %d roots", PD_MAX_ROOTS);
		}
		memset(&P.roots[ri], 0, sizeof(P.roots[ri]));
		P.roots[ri].meter_fd = -1;
		snprintf(P.roots[ri].root, sizeof(P.roots[ri].root), "%s", root);
	} else {
		prm_store_remove_matching(P.store, NULL, ri); /* a re-register replaces the tree */
	}
	char e[400] = "";
	struct prm **ps = NULL;
	int np = 0;
	json_object *jp = NULL, *jm = NULL;
	json_object_object_get_ex(o, "params", &jp);
	json_object_object_get_ex(o, "meters", &jm);
	if ((jp && parse_params_into(jp, ri, &ps, &np, e, sizeof(e)) < 0) ||
	    (jm && set_meter_ids(&P.roots[ri], jm, e, sizeof(e)) < 0)) {
		json_object_put(o);
		for (int i = 0; i < np; i++)
			prm_free(ps[i]);
		free(ps);
		if (fresh)
			memset(&P.roots[ri], 0, sizeof(P.roots[ri]));
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	}
	json_object_put(o);
	struct root *rt = &P.roots[ri];
	rt->used = true;
	snprintf(rt->owner, sizeof(rt->owner), "%s", sender ? sender : "");
	if (!rt->token[0]) {
		uint8_t rnd[16];
		if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd))
			for (int i = 0; i < 16; i++)
				rnd[i] = (uint8_t)(now_ns() >> (i * 3));
		for (int i = 0; i < 16; i++)
			snprintf(rt->token + 2 * i, 3, "%02x", rnd[i]);
	}
	for (int i = 0; i < np; i++)
		mark_dirty(prm_store_put(P.store, ps[i]));
	free(ps);
	plog("%s registered by %s: %d parameters, %u meters\n", root, rt->owner, np, rt->n_meters);
	emit_structure(root);
	return sd_bus_reply_method_return(m, "s", rt->token);
}

static int m_add(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *token, *js;
	int r = sd_bus_message_read(m, "ss", &token, &js);
	if (r < 0)
		return r;
	struct root *rt = root_for_token(m, token, err);
	if (!rt)
		return -EACCES;
	json_object *o = json_tokener_parse(js), *jp;
	if (!o || !json_object_object_get_ex(o, "params", &jp)) {
		json_object_put(o);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "expected {\"params\": [...]}");
	}
	char e[400];
	struct prm **ps;
	int np;
	if (parse_params_into(jp, (int)(rt - P.roots), &ps, &np, e, sizeof(e)) < 0) {
		json_object_put(o);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	}
	json_object_put(o);
	for (int i = 0; i < np; i++)
		mark_dirty(prm_store_put(P.store, ps[i]));
	free(ps);
	emit_structure(rt->root);
	return sd_bus_reply_method_return(m, "");
}

static int m_remove(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *token;
	int r = sd_bus_message_read(m, "s", &token);
	if (r < 0)
		return r;
	struct root *rt = root_for_token(m, token, err);
	if (!rt)
		return -EACCES;
	int ri = (int)(rt - P.roots);
	if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0)
		return r;
	const char *path;
	int n = 0;
	while ((r = sd_bus_message_read(m, "s", &path)) > 0) {
		size_t l = strlen(path);
		if (l > 3 && !strcmp(path + l - 3, "/**"))
			n += prm_store_remove_matching(P.store, path, ri);
		else {
			struct prm *p = prm_store_get(P.store, path);
			if (p && p->root == ri)
				n += prm_store_remove(P.store, path);
		}
	}
	sd_bus_message_exit_container(m);
	if (n)
		emit_structure(rt->root);
	return sd_bus_reply_method_return(m, "");
}

static void append_changed(sd_bus_message *s, struct prm *p)
{
	sd_bus_message_open_container(s, 'r', "svds");
	sd_bus_message_append(s, "s", p->path);
	append_value(s, p, p->value, p->str);
	sd_bus_message_append(s, "ds", prm_to_norm(p, p->value), p->text);
	sd_bus_message_close_container(s);
}

static void emit_changed(struct root *rt, struct prm **ps, int n)
{
	if (!n)
		return;
	sd_bus_message *s = NULL;
	if (sd_bus_message_new_signal(P.bus, &s, PD_OBJ_PATH, PD_IFACE, "Changed") < 0)
		return;
	sd_bus_message_append(s, "s", rt->root);
	sd_bus_message_open_container(s, 'a', "(svds)");
	for (int i = 0; i < n; i++)
		append_changed(s, ps[i]);
	sd_bus_message_close_container(s);
	sd_bus_send(P.bus, s, NULL);
	sd_bus_message_unref(s);
}

#define MAX_UPDATE 4096

static int m_update(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *token;
	int r = sd_bus_message_read(m, "s", &token);
	if (r < 0)
		return r;
	struct root *rt = root_for_token(m, token, err);
	if (!rt)
		return -EACCES;
	int ri = (int)(rt - P.roots);
	struct upd {
		struct prm *p;
		double num;
		char str[PRM_STRING_MAX];
	} *u = malloc(MAX_UPDATE * sizeof(*u));
	if (!u)
		return -ENOMEM;
	int n = 0;
	if ((r = sd_bus_message_enter_container(m, 'a', "(sv)")) < 0) {
		free(u);
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, 'r', "sv")) > 0) {
		const char *path;
		struct pval v;
		if (sd_bus_message_read(m, "s", &path) < 0 || read_variant(m, &v) < 0) {
			free(u);
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "each value is (s path, v value) of d, i, b or s");
		}
		sd_bus_message_exit_container(m);
		struct prm *p = prm_store_get(P.store, path);
		if (!p || p->root != ri) {
			free(u);
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s is not a parameter of %s", path, rt->root);
		}
		if (n >= MAX_UPDATE) {
			free(u);
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "at most %d values per Update", MAX_UPDATE);
		}
		char why[256];
		u[n].p = p;
		if (p->type != PRM_TRIGGER && convert(p, &v, false, &u[n].num, u[n].str, sizeof(u[n].str), why, sizeof(why)) < 0) {
			free(u);
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", why);
		}
		n++;
	}
	sd_bus_message_exit_container(m);
	struct prm **ch = malloc((n ? n : 1) * sizeof(*ch));
	for (int i = 0; i < n; i++) {
		struct prm *p = u[i].p;
		if (p->type == PRM_STRING)
			snprintf(p->str, sizeof(p->str), "%s", u[i].str);
		else if (p->type != PRM_TRIGGER)
			p->value = u[i].num;
		p->text_override = false;
		prm_refresh_text(p);
		ch[i] = p;
		mark_dirty(p);
		if (P.hooks.changed)
			P.hooks.changed(P.hooks.data, p);
	}
	emit_changed(rt, ch, n);
	free(ch);
	free(u);
	return sd_bus_reply_method_return(m, "");
}

static int m_update_text(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *token;
	int r = sd_bus_message_read(m, "s", &token);
	if (r < 0)
		return r;
	struct root *rt = root_for_token(m, token, err);
	if (!rt)
		return -EACCES;
	int ri = (int)(rt - P.roots);
	/* check everything first, so a bad entry changes nothing */
	if ((r = sd_bus_message_enter_container(m, 'a', "(ss)")) < 0)
		return r;
	const char *path, *text;
	while ((r = sd_bus_message_read(m, "(ss)", &path, &text)) > 0) {
		struct prm *p = prm_store_get(P.store, path);
		if (!p || p->root != ri)
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s is not a parameter of %s", path, rt->root);
		if (strlen(text) >= PRM_TEXT_MAX)
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "text is at most %d bytes", PRM_TEXT_MAX - 1);
	}
	sd_bus_message_exit_container(m);
	sd_bus_message_rewind(m, true);
	sd_bus_message_skip(m, "s");
	sd_bus_message_enter_container(m, 'a', "(ss)");
	struct prm **ch = malloc(MAX_UPDATE * sizeof(*ch));
	int n = 0;
	while ((r = sd_bus_message_read(m, "(ss)", &path, &text)) > 0 && n < MAX_UPDATE) {
		struct prm *p = prm_store_get(P.store, path);
		snprintf(p->text, sizeof(p->text), "%s", text);
		p->text_override = true;
		ch[n++] = p;
		mark_dirty(p);
		if (P.hooks.changed)
			P.hooks.changed(P.hooks.data, p);
	}
	sd_bus_message_exit_container(m);
	emit_changed(rt, ch, n);
	free(ch);
	return sd_bus_reply_method_return(m, "");
}

static int m_meters(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *token;
	int fd;
	int r = sd_bus_message_read(m, "sh", &token, &fd);
	if (r < 0)
		return r;
	struct root *rt = root_for_token(m, token, err);
	if (!rt)
		return -EACCES;
	json_object *ids = json_object_new_array();
	if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) {
		json_object_put(ids);
		return r;
	}
	const char *id;
	while (sd_bus_message_read(m, "s", &id) > 0)
		json_object_array_add(ids, json_object_new_string(id));
	sd_bus_message_exit_container(m);
	char e[300];
	if (set_meter_ids(rt, ids, e, sizeof(e)) < 0) {
		json_object_put(ids);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", e);
	}
	json_object_put(ids);
	int own = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	size_t len = 0;
	const struct bl_meters_hdr *h = own >= 0 ? bl_meters_map(own, rt->n_meters, &len) : NULL;
	if (!h) {
		if (own >= 0)
			close(own);
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
					 "the meter table must be a memfd sealed with F_SEAL_SHRINK, version 1, of at least 64 + 8 x %u bytes",
					 rt->n_meters);
	}
	rt->meter_fd = own;
	rt->meters = h;
	rt->meters_len = len;
	plog("%s: meter table of %u meters\n", rt->root, rt->n_meters);
	emit_structure(rt->root);
	return sd_bus_reply_method_return(m, "");
}

static int m_roles(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *token;
	int r = sd_bus_message_read(m, "s", &token);
	if (r < 0)
		return r;
	struct root *rt = root_for_token(m, token, err);
	if (!rt)
		return -EACCES;
	struct prm_role roles[PRM_MAX_ROLES];
	int n = 0;
	memset(roles, 0, sizeof(roles));
	if ((r = sd_bus_message_enter_container(m, 'a', "{sas}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sas")) > 0) {
		const char *name, *pat;
		sd_bus_message_read(m, "s", &name);
		if (!prm_role_name_valid(name) || !strcmp(name, "all") || !strcmp(name, "read-only"))
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
						 "'%s': a role name is a-z 0-9 - _ (and not all or read-only)", name);
		if (n >= PRM_MAX_ROLES)
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "at most %d roles", PRM_MAX_ROLES);
		snprintf(roles[n].name, sizeof(roles[n].name), "%s", name);
		sd_bus_message_enter_container(m, 'a', "s");
		while (sd_bus_message_read(m, "s", &pat) > 0) {
			if (roles[n].n >= PRM_MAX_ROLE_PATTERNS || strlen(pat) >= PRM_PATH_MAX || pat[0] != '/')
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "role %s: patterns start with / (at most %d)",
							 name, PRM_MAX_ROLE_PATTERNS);
			snprintf(roles[n].patterns[roles[n].n++], PRM_PATH_MAX, "%s", pat);
		}
		sd_bus_message_exit_container(m);
		sd_bus_message_exit_container(m);
		n++;
	}
	sd_bus_message_exit_container(m);
	memcpy(rt->roles, roles, sizeof(roles));
	rt->n_roles = n;
	plog("%s: %d roles\n", rt->root, n);
	return sd_bus_reply_method_return(m, "");
}

static int m_unregister(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *token;
	int r = sd_bus_message_read(m, "s", &token);
	if (r < 0)
		return r;
	struct root *rt = root_for_token(m, token, err);
	if (!rt)
		return -EACCES;
	root_remove((int)(rt - P.roots), "unregistered");
	return sd_bus_reply_method_return(m, "");
}

/* ============================================================ D-Bus: clients */
struct list_ctx {
	const char *pattern;
	json_object *arr;
};

static int list_one(void *data, struct prm *p)
{
	struct list_ctx *c = data;
	if (prm_match(c->pattern, p->path))
		json_object_array_add(c->arr, prm_to_json(p));
	return 0;
}

static int m_list(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *pattern;
	int r = sd_bus_message_read(m, "s", &pattern);
	if (r < 0)
		return r;
	json_object *o = json_object_new_object(), *roots = json_object_new_array(), *arr = json_object_new_array();
	json_object_object_add(o, "schema", json_object_new_string(SCHEMA));
	for (int i = 0; i < PD_MAX_ROOTS; i++) {
		struct root *rt = &P.roots[i];
		if (!rt->used)
			continue;
		json_object *ro = json_object_new_object(), *ms = json_object_new_array();
		json_object_object_add(ro, "root", json_object_new_string(rt->root));
		json_object_object_add(ro, "owner", json_object_new_string(rt->owner));
		for (uint32_t k = 0; k < rt->n_meters; k++)
			json_object_array_add(ms, json_object_new_string(rt->meter_ids[k]));
		json_object_object_add(ro, "meters", ms);
		json_object_object_add(ro, "meter-table", json_object_new_boolean(rt->meters != NULL));
		json_object *rl = json_object_new_array();
		for (int k = 0; k < rt->n_roles; k++)
			json_object_array_add(rl, json_object_new_string(rt->roles[k].name));
		json_object_object_add(ro, "roles", rl);
		json_object_array_add(roots, ro);
	}
	json_object_object_add(o, "roots", roots);
	struct list_ctx c = { pattern, arr };
	prm_store_each(P.store, list_one, &c);
	json_object_object_add(o, "params", arr);
	r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return r;
}

static int m_get(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	int r = sd_bus_message_enter_container(m, 'a', "s");
	if (r < 0)
		return r;
	sd_bus_message *reply = NULL;
	if ((r = sd_bus_message_new_method_return(m, &reply)) < 0)
		return r;
	sd_bus_message_open_container(reply, 'a', "(svds)");
	const char *path;
	while ((r = sd_bus_message_read(m, "s", &path)) > 0) {
		struct prm *p = prm_store_get(P.store, path);
		if (!p) {
			sd_bus_message_unref(reply);
			return sd_bus_error_setf(err, ERR_NOTFOUND, "no parameter %s", path);
		}
		append_changed(reply, p);
	}
	sd_bus_message_close_container(reply);
	r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

struct opts {
	bool norm;
	char role[32];
	char client[128];
};

static int read_opts(sd_bus_message *m, struct opts *o, bool allow_norm, sd_bus_error *err)
{
	memset(o, 0, sizeof(*o));
	snprintf(o->role, sizeof(o->role), "all");
	const char *sender = sd_bus_message_get_sender(m);
	snprintf(o->client, sizeof(o->client), "%s", sender ? sender : "dbus");
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key, *sig = NULL;
		sd_bus_message_read(m, "s", &key);
		sd_bus_message_peek_type(m, NULL, &sig);
		if (allow_norm && !strcmp(key, "norm") && sig && !strcmp(sig, "b")) {
			int b;
			sd_bus_message_read(m, "v", "b", &b);
			o->norm = b;
		} else if (!strcmp(key, "role") && sig && !strcmp(sig, "s")) {
			const char *s;
			sd_bus_message_read(m, "v", "s", &s);
			if (strcmp(s, "all") && strcmp(s, "read-only") && !prm_role_name_valid(s))
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "'%s' is not a role name", s);
			snprintf(o->role, sizeof(o->role), "%s", s);
		} else if (!strcmp(key, "client") && sig && !strcmp(sig, "s")) {
			const char *s;
			sd_bus_message_read(m, "v", "s", &s);
			snprintf(o->client, sizeof(o->client), "%s", s);
		} else {
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown option '%s' (or wrong type)", key);
		}
		sd_bus_message_exit_container(m);
	}
	return sd_bus_message_exit_container(m);
}

static int reply_status(sd_bus_message *m, sd_bus_error *err, int r, const char *why)
{
	if (r == -EACCES)
		return sd_bus_error_setf(err, ERR_ACCESS, "%s", why);
	if (r < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", why);
	return sd_bus_reply_method_return(m, "");
}

static int m_set(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *path;
	int r = sd_bus_message_read(m, "s", &path);
	if (r < 0)
		return r;
	struct pval v;
	if (read_variant(m, &v) < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "the value is a d, i, u, b or s");
	struct opts o;
	if ((r = read_opts(m, &o, true, err)) < 0)
		return r;
	struct prm *p = prm_store_get(P.store, path);
	if (!p)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no parameter %s", path);
	char why[256] = "";
	return reply_status(m, err, request_set(p, &v, o.norm, o.client, o.role, why, sizeof(why)), why);
}

static int m_gesture(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *path;
	int begin;
	int r = sd_bus_message_read(m, "sb", &path, &begin);
	if (r < 0)
		return r;
	struct opts o;
	if ((r = read_opts(m, &o, false, err)) < 0)
		return r;
	struct prm *p = prm_store_get(P.store, path);
	if (!p)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no parameter %s", path);
	/* D-Bus gestures are kept per bus name, so a vanished client's are ended */
	const char *sender = sd_bus_message_get_sender(m);
	char client[300];
	snprintf(client, sizeof(client), "%s", o.client);
	struct gest *g;
	char why[256] = "";
	r = request_gesture(p, begin, G_DBUS, client, o.role, NULL, why, sizeof(why));
	if (r >= 0 && begin && (g = gest_find(G_DBUS, p->path, client)) && sender)
		snprintf(g->peer.label, sizeof(g->peer.label), "%s", sender); /* the bus name to watch */
	return reply_status(m, err, r, why);
}

static int m_meter_table(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *root;
	int r = sd_bus_message_read(m, "s", &root);
	if (r < 0)
		return r;
	int ri = root_index(root);
	if (ri < 0 || !P.roots[ri].meters)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "%s has no meter table", root);
	struct root *rt = &P.roots[ri];
	/* a read-only fd: reopen through /proc so the receiver can't write */
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", rt->meter_fd);
	int ro = open(proc, O_RDONLY | O_CLOEXEC);
	if (ro < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "cannot reopen the meter table: %s", strerror(errno));
	sd_bus_message *reply = NULL;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r >= 0)
		r = sd_bus_message_append(reply, "h", ro);
	if (r >= 0)
		r = sd_bus_message_open_container(reply, 'a', "s");
	for (uint32_t k = 0; r >= 0 && k < rt->n_meters; k++)
		r = sd_bus_message_append(reply, "s", rt->meter_ids[k]);
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	close(ro);
	return r;
}

static int p_props(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud,
		   sd_bus_error *err)
{
	if (!strcmp(prop, "Roots")) {
		int r = sd_bus_message_open_container(reply, 'a', "s");
		for (int i = 0; r >= 0 && i < PD_MAX_ROOTS; i++)
			if (P.roots[i].used)
				r = sd_bus_message_append(reply, "s", P.roots[i].root);
		return r < 0 ? r : sd_bus_message_close_container(reply);
	}
	if (!strcmp(prop, "ParamCount"))
		return sd_bus_message_append(reply, "u", (uint32_t)prm_store_count(P.store));
	if (!strcmp(prop, "Subscriptions"))
		return sd_bus_message_append(reply, "u", pd_subscriptions());
	if (!strcmp(prop, "SetRequests"))
		return sd_bus_message_append(reply, "t", P.set_requests);
	return sd_bus_message_append(reply, "t", P.refused);
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("Register", SD_BUS_ARGS("s", root, "s", schema_json), SD_BUS_RESULT("s", token), m_register, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Add", SD_BUS_ARGS("s", token, "s", params_json), SD_BUS_NO_RESULT, m_add, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Remove", SD_BUS_ARGS("s", token, "as", paths), SD_BUS_NO_RESULT, m_remove, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Update", SD_BUS_ARGS("s", token, "a(sv)", values), SD_BUS_NO_RESULT, m_update, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("UpdateText", SD_BUS_ARGS("s", token, "a(ss)", texts), SD_BUS_NO_RESULT, m_update_text, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Meters", SD_BUS_ARGS("s", token, "h", memfd, "as", ids), SD_BUS_NO_RESULT, m_meters, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Roles", SD_BUS_ARGS("s", token, "a{sas}", roles), SD_BUS_NO_RESULT, m_roles, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Unregister", SD_BUS_ARGS("s", token), SD_BUS_NO_RESULT, m_unregister, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("List", SD_BUS_ARGS("s", pattern), SD_BUS_RESULT("s", json), m_list, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Get", SD_BUS_ARGS("as", paths), SD_BUS_RESULT("a(svds)", values), m_get, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Set", SD_BUS_ARGS("s", path, "v", value, "a{sv}", options), SD_BUS_NO_RESULT, m_set, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Gesture", SD_BUS_ARGS("s", path, "b", begin, "a{sv}", options), SD_BUS_NO_RESULT, m_gesture, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("MeterTable", SD_BUS_ARGS("s", root), SD_BUS_RESULT("h", memfd, "as", ids), m_meter_table, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("Roots", "as", p_props, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ParamCount", "u", p_props, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Subscriptions", "u", p_props, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("SetRequests", "t", p_props, 0, 0),
	SD_BUS_PROPERTY("Refused", "t", p_props, 0, 0),
	SD_BUS_SIGNAL_WITH_ARGS("Set", SD_BUS_ARGS("s", path, "v", value, "s", client, "s", role), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Gesture", SD_BUS_ARGS("s", path, "b", begin, "s", client, "s", role), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Changed", SD_BUS_ARGS("s", root, "a(svds)", values), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Structure", SD_BUS_ARGS("s", root), 0),
	SD_BUS_VTABLE_END
};

/* A client left the bus: its roots go, its gestures end. */
static int on_name_owner(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *name, *old, *new_;
	if (sd_bus_message_read(m, "sss", &name, &old, &new_) < 0 || *new_ || name[0] != ':')
		return 0;
	for (int i = 0; i < PD_MAX_GEST; i++) {
		struct gest *g = &P.gests[i];
		if (g->used && g->kind == G_DBUS && !strcmp(g->peer.label, name))
			gest_end(g, "client left the bus");
	}
	for (int i = 0; i < PD_MAX_ROOTS; i++)
		if (P.roots[i].used && !strcmp(P.roots[i].owner, name))
			root_remove(i, "its owner left the bus");
	return 0;
}

int pd_init(sd_bus *bus, struct pw_loop *loop, const struct pd_hooks *hooks, bool verbose)
{
	memset(&P, 0, sizeof(P));
	P.bus = bus;
	P.loop = loop;
	P.hooks = *hooks;
	P.verbose = verbose;
	P.store = prm_store_new();
	for (int i = 0; i < PD_MAX_ROOTS; i++)
		P.roots[i].meter_fd = -1;
	int r = sd_bus_add_object_vtable(bus, &P.vtable_slot, PD_OBJ_PATH, PD_IFACE, vtable, NULL);
	if (r < 0)
		return r;
	r = sd_bus_match_signal(bus, &P.match_slot, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
				"NameOwnerChanged", on_name_owner, NULL);
	if (r < 0)
		return r;
	P.tick = pw_loop_add_timer(loop, on_tick, NULL);
	return 0;
}

void pd_fini(void)
{
	for (int i = 0; i < PD_MAX_SUBS; i++)
		if (P.subs[i].used)
			free(P.subs[i].dirty);
	for (int i = 0; i < PD_MAX_ROOTS; i++) {
		meters_drop(&P.roots[i]);
		free(P.roots[i].meter_ids);
	}
	if (P.tick)
		pw_loop_destroy_source(P.loop, P.tick);
	sd_bus_slot_unref(P.match_slot);
	sd_bus_slot_unref(P.vtable_slot);
	prm_store_free(P.store);
	memset(&P, 0, sizeof(P));
}
