/* The surface engine (engine.h).
 * SPDX-License-Identifier: MIT
 *
 * Feedback is computed, not queued: every tick the engine works out what
 * each device *should* show (fader positions, LEDs, rings, the two LCD lines,
 * colours, meters, the 7-segment displays) from the views and the parameter
 * values, compares it with what it last sent, and sends the differences in
 * priority order within the unit's byte budget (motors, LEDs, rings, meters,
 * text). That coalesces by construction: a value that changed ten times
 * since the last send is sent once, as its newest value, and a replugged
 * device is redrawn by forgetting what it was sent.
 */
#define _GNU_SOURCE
#include "engine.h"

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "mcu.h"

#define MS 1000000ull
#define SF_MAX_VIEWS 64
#define SF_MAX_MODES 32
#define SF_MAX_PAGES 16
#define SF_MAX_BINDS 96
#define SF_STACK 8
#define SF_POTS 8

/* ------------------------------------------------------------ views */
enum { B_REC, B_SOLO, B_MUTE, B_SELECT, B_COUNT };
static const char *btn_names[B_COUNT] = { "rec", "solo", "mute", "select" };

struct sf_btn {
	char *path, *led, *flash, *action;
	bool momentary;
};

struct sf_pot {
	char *key, *path;
};

struct sf_slot {
	bool empty;
	char *id;
	char *name;            /* literal or path */
	char *colour;          /* literal or path */
	char *fader;
	struct sf_pot pots[SF_POTS];
	int n_pots;
	struct sf_btn btn[B_COUNT];
	char *meter_root, *meter_id;
};

struct sf_view {
	char *id, *label;
	char code[3];
	int layer;
	struct sf_slot *slots;
	int n_slots;
	struct sf_slot master;
	bool has_master;
};

struct sf_entry {
	char *path, *label;
	int ring;
};

struct sf_mode {
	char *id, *label, *binding;
	bool strip, line2_label;
	int ring;
	int push;              /* 0 reset, 1 none, 2 action */
	char *push_action;
	double step;
	struct sf_entry pages[SF_MAX_PAGES][8];
	int page_len[SF_MAX_PAGES];
	int n_pages;
};

struct sf_views {
	struct sf_view views[SF_MAX_VIEWS];
	int n_views;
	struct sf_mode modes[SF_MAX_MODES];
	int n_modes;
	char *home_view, *home_mode;
};

/* ------------------------------------------------------------ bindings */
enum bkind { K_NONE, K_ACTION, K_PARAM, K_MOMENTARY, K_BANK_L, K_BANK_R, K_CH_L, K_CH_R, K_BANK_FIRST, K_BANK_LAST,
	     K_FLIP, K_NAMEVALUE, K_POP, K_LAYER, K_POTMODE, K_PAGE_NEXT, K_PAGE_PREV };

struct sf_bind {
	char control[48];
	enum bkind kind;
	char arg[256];
	int n;
	char led[256];
};

/* ------------------------------------------------------------ units */
struct dev {
	int fader[9];          /* 14-bit; -1 unknown */
	uint8_t led[MCU_NOTES];/* 0 off 1 flash 2 on; 255 unknown */
	uint8_t ring[8];       /* 255 unknown */
	char lcd[MCU_LCD_CELLS]; /* 0 unknown */
	uint8_t colour[8];     /* 255 unknown */
	int meter[8];          /* -1 unknown */
	uint8_t tc[MCU_TC_DIGITS]; /* 255 unknown */
	uint8_t assign[2];
};

struct unit {
	enum sf_unit_state state;
	char id[160];
	char port[256];
	char serial[32];
	const struct sf_profile *prof;
	uint64_t hs_ns;
	int hs_tries;
	struct dev sent, want;
	/* input */
	bool touched[9];
	int echo[9];
	uint64_t touch_ns[9], move_ns[9];
	char fader_gest[9][PRM_PATH_MAX];   /* the path a fader gesture is open on */
	bool motion[9];                     /* a gesture opened by motion (no touch sense) */
	bool stale_touch[9];                /* > 60 s without movement: gesture ended */
	char pot_gest[8][PRM_PATH_MAX];
	uint64_t pot_ns[8], pot_show[8];
	double pot_acc[8];
	bool shift, option, control, alt;
	/* pacing */
	double tokens;
	uint64_t last_ns;
	uint64_t fader_ns[9], ring_ns[8], line_ns[2], meter_ns[8];
	bool clip_on[8];
	uint64_t clip_until[8];
	char text[2][57];
	uint64_t text_until;
	bool text_on;
	uint64_t gone_ns;
};

struct off {
	char view[64];
	int offset;
};

struct group {
	bool used;
	char id[64];
	char units[SF_GROUP_UNITS][160];
	int n_units;
	char role[32];
	char lock_view[64];
	int lock_offset;
	struct sf_views *views;           /* own, or NULL: the "*" views */
	char stack[SF_STACK][64];
	int depth;
	struct off offs[SF_MAX_VIEWS];
	int n_offs;
	char mode[64];
	int page;
	char selected[64];
	bool flip, name_value;
	struct sf_bind binds[SF_MAX_BINDS];
	int n_binds;
	uint64_t bank_until;
};

struct sf_engine {
	struct sf_host host;
	void *data;
	struct prm_store *params;
	struct sf_profile profiles[SF_MAX_PROFILES];
	int n_profiles;
	struct unit units[SF_MAX_UNITS];
	struct group groups[SF_MAX_GROUPS];
	struct sf_views *defaults;
	uint64_t now;
	uint64_t msgs_in, msgs_out;
	int next_group;
};

static void elog(struct sf_engine *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void elog(struct sf_engine *e, const char *fmt, ...)
{
	if (!e->host.log)
		return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	e->host.log(e->data, buf);
}

static char *sdup(const char *s)
{
	return s ? strdup(s) : NULL;
}

/* ============================================================ views JSON */
static void free_btn(struct sf_btn *b)
{
	free(b->path); free(b->led); free(b->flash); free(b->action);
	memset(b, 0, sizeof(*b));
}

static void free_slot(struct sf_slot *s)
{
	free(s->id); free(s->name); free(s->colour); free(s->fader); free(s->meter_root); free(s->meter_id);
	for (int i = 0; i < s->n_pots; i++) {
		free(s->pots[i].key);
		free(s->pots[i].path);
	}
	for (int i = 0; i < B_COUNT; i++)
		free_btn(&s->btn[i]);
	memset(s, 0, sizeof(*s));
}

static void free_views(struct sf_views *v)
{
	if (!v)
		return;
	for (int i = 0; i < v->n_views; i++) {
		struct sf_view *w = &v->views[i];
		for (int k = 0; k < w->n_slots; k++)
			free_slot(&w->slots[k]);
		free(w->slots);
		free_slot(&w->master);
		free(w->id);
		free(w->label);
	}
	for (int i = 0; i < v->n_modes; i++) {
		struct sf_mode *m = &v->modes[i];
		free(m->id); free(m->label); free(m->binding); free(m->push_action);
		for (int p = 0; p < m->n_pages; p++)
			for (int k = 0; k < m->page_len[p]; k++) {
				free(m->pages[p][k].path);
				free(m->pages[p][k].label);
			}
	}
	free(v->home_view);
	free(v->home_mode);
	free(v);
}

static bool path_ok(const char *s)
{
	return s && s[0] == '/' && strlen(s) < PRM_PATH_MAX;
}

static int parse_btn(json_object *o, struct sf_btn *b, const char *what, char *err, size_t en)
{
	if (json_object_is_type(o, json_type_string)) {
		if (!path_ok(json_object_get_string(o))) {
			snprintf(err, en, "%s: a path starts with /", what);
			return -1;
		}
		b->path = sdup(json_object_get_string(o));
		return 0;
	}
	if (!json_object_is_type(o, json_type_object)) {
		snprintf(err, en, "%s is a path or an object", what);
		return -1;
	}
	json_object_object_foreach(o, k, v) {
		const char *s = json_object_get_string(v);
		if (!strcmp(k, "path") || !strcmp(k, "led") || !strcmp(k, "flash")) {
			if (!path_ok(s)) {
				snprintf(err, en, "%s.%s: a path starts with /", what, k);
				return -1;
			}
			char **dst = !strcmp(k, "path") ? &b->path : !strcmp(k, "led") ? &b->led : &b->flash;
			free(*dst);
			*dst = sdup(s);
		} else if (!strcmp(k, "action")) {
			if (!s || !*s || strlen(s) > 48) {
				snprintf(err, en, "%s.action is a name", what);
				return -1;
			}
			b->action = sdup(s);
		} else if (!strcmp(k, "mode")) {
			if (!s || (strcmp(s, "toggle") && strcmp(s, "momentary"))) {
				snprintf(err, en, "%s.mode is toggle or momentary", what);
				return -1;
			}
			b->momentary = !strcmp(s, "momentary");
		} else {
			snprintf(err, en, "%s: unknown key '%s'", what, k);
			return -1;
		}
	}
	return 0;
}

static int parse_slot(json_object *o, struct sf_slot *s, int idx, char *err, size_t en)
{
	memset(s, 0, sizeof(*s));
	if (!o || json_object_is_type(o, json_type_null)) {
		s->empty = true;
		return 0;
	}
	if (!json_object_is_type(o, json_type_object)) {
		snprintf(err, en, "slot %d is an object or null", idx);
		return -1;
	}
	char what[64];
	json_object_object_foreach(o, k, v) {
		const char *str = json_object_get_string(v);
		snprintf(what, sizeof(what), "slot %d %s", idx, k);
		if (!strcmp(k, "id")) {
			if (!str || !*str || strlen(str) >= 64) {
				snprintf(err, en, "%s: an id of 1-63 characters", what);
				return -1;
			}
			s->id = sdup(str);
		} else if (!strcmp(k, "name") || !strcmp(k, "colour")) {
			if (!str || strlen(str) >= PRM_PATH_MAX) {
				snprintf(err, en, "%s is a string", what);
				return -1;
			}
			if (!strcmp(k, "name"))
				s->name = sdup(str);
			else
				s->colour = sdup(str);
		} else if (!strcmp(k, "fader")) {
			if (!path_ok(str)) {
				snprintf(err, en, "%s: a path starts with /", what);
				return -1;
			}
			s->fader = sdup(str);
		} else if (!strcmp(k, "pots")) {
			if (!json_object_is_type(v, json_type_object)) {
				snprintf(err, en, "%s is an object of pot bindings", what);
				return -1;
			}
			json_object_object_foreach(v, pk, pv) {
				if (s->n_pots >= SF_POTS || !path_ok(json_object_get_string(pv)) || strlen(pk) > 31) {
					snprintf(err, en, "%s: at most %d bindings, each a path", what, SF_POTS);
					return -1;
				}
				s->pots[s->n_pots].key = sdup(pk);
				s->pots[s->n_pots].path = sdup(json_object_get_string(pv));
				s->n_pots++;
			}
		} else if (!strcmp(k, "meter")) {
			const char *c = str ? strchr(str, ':') : NULL;
			if (!c || str[0] != '/' || !c[1] || c - str > 64 || strlen(c + 1) > 64) {
				snprintf(err, en, "%s is ROOT:ID", what);
				return -1;
			}
			s->meter_root = strndup(str, (size_t)(c - str));
			s->meter_id = sdup(c + 1);
		} else if (!strcmp(k, "flags") || !strcmp(k, "comment")) {
			/* passed through, not interpreted */
		} else {
			int b = -1;
			for (int i = 0; i < B_COUNT; i++)
				if (!strcmp(k, btn_names[i]))
					b = i;
			if (b < 0) {
				snprintf(err, en, "slot %d: unknown key '%s'", idx, k);
				return -1;
			}
			if (parse_btn(v, &s->btn[b], what, err, en) < 0)
				return -1;
		}
	}
	if (!s->id) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", idx + 1);
		s->id = sdup(buf);
	}
	return 0;
}

static int parse_mode(json_object *o, struct sf_mode *m, char *err, size_t en)
{
	memset(m, 0, sizeof(*m));
	m->step = 0.01;
	if (!json_object_is_type(o, json_type_object)) {
		snprintf(err, en, "a pot mode is an object");
		return -1;
	}
	json_object_object_foreach(o, k, v) {
		const char *s = json_object_get_string(v);
		if (!strcmp(k, "id")) m->id = sdup(s);
		else if (!strcmp(k, "label")) m->label = sdup(s);
		else if (!strcmp(k, "binding")) m->binding = sdup(s);
		else if (!strcmp(k, "strip")) m->strip = json_object_get_boolean(v);
		else if (!strcmp(k, "line2")) m->line2_label = s && !strcmp(s, "label");
		else if (!strcmp(k, "step")) m->step = json_object_get_double(v);
		else if (!strcmp(k, "ring")) {
			m->ring = mcu_parse_ring_mode(s);
			if (m->ring < 0) {
				snprintf(err, en, "ring is dot, boost-cut, wrap or spread");
				return -1;
			}
		} else if (!strcmp(k, "push")) {
			if (s && !strcmp(s, "reset")) m->push = 0;
			else if (s && !strcmp(s, "none")) m->push = 1;
			else if (s && !strncmp(s, "action:", 7) && s[7]) {
				m->push = 2;
				m->push_action = sdup(s + 7);
			} else {
				snprintf(err, en, "push is reset, none or action:NAME");
				return -1;
			}
		} else if (!strcmp(k, "pages")) {
			if (!json_object_is_type(v, json_type_array) || json_object_array_length(v) > SF_MAX_PAGES) {
				snprintf(err, en, "pages is an array of at most %d pages", SF_MAX_PAGES);
				return -1;
			}
			for (size_t p = 0; p < json_object_array_length(v); p++) {
				json_object *pg = json_object_array_get_idx(v, p);
				if (!json_object_is_type(pg, json_type_array) || json_object_array_length(pg) > 8) {
					snprintf(err, en, "a page is an array of at most 8 entries");
					return -1;
				}
				for (size_t i = 0; i < json_object_array_length(pg); i++) {
					json_object *en_ = json_object_array_get_idx(pg, i), *x;
					struct sf_entry *ent = &m->pages[p][i];
					ent->ring = MCU_RING_DOT;
					if (json_object_is_type(en_, json_type_null))
						continue;
					if (!json_object_object_get_ex(en_, "path", &x) || !path_ok(json_object_get_string(x))) {
						snprintf(err, en, "a page entry has a path");
						return -1;
					}
					ent->path = sdup(json_object_get_string(x));
					if (json_object_object_get_ex(en_, "label", &x))
						ent->label = sdup(json_object_get_string(x));
					if (json_object_object_get_ex(en_, "ring", &x)) {
						ent->ring = mcu_parse_ring_mode(json_object_get_string(x));
						if (ent->ring < 0) {
							snprintf(err, en, "ring is dot, boost-cut, wrap or spread");
							return -1;
						}
					}
				}
				m->page_len[p] = (int)json_object_array_length(pg);
			}
			m->n_pages = (int)json_object_array_length(v);
		} else if (!strcmp(k, "code") || !strcmp(k, "comment")) {
		} else {
			snprintf(err, en, "pot mode: unknown key '%s'", k);
			return -1;
		}
	}
	if (!m->id || !*m->id || strlen(m->id) >= 64) {
		snprintf(err, en, "a pot mode needs an id");
		return -1;
	}
	if (!m->label)
		m->label = sdup(m->id);
	if (!m->strip && !m->binding)
		m->binding = sdup(m->id);
	if (m->strip && !m->n_pages) {
		snprintf(err, en, "pot mode %s: a strip mode has pages", m->id);
		return -1;
	}
	if (!(m->step > 0 && m->step <= 1))
		m->step = 0.01;
	return 0;
}

static struct sf_views *parse_views(const char *js, char *err, size_t en)
{
	json_object *o = json_tokener_parse(js), *v;
	if (!o || !json_object_is_type(o, json_type_object)) {
		json_object_put(o);
		snprintf(err, en, "not a JSON object");
		return NULL;
	}
	struct sf_views *vs = calloc(1, sizeof(*vs));
	if (!json_object_object_get_ex(o, "schema", &v) || strcmp(json_object_get_string(v), "backline.views/1")) {
		snprintf(err, en, "the schema is backline.views/1");
		goto fail;
	}
	json_object_object_foreach(o, key, val) {
		if (!strcmp(key, "schema") || !strcmp(key, "comment"))
			continue;
		if (!strcmp(key, "views")) {
			if (!json_object_is_type(val, json_type_array) || json_object_array_length(val) > SF_MAX_VIEWS) {
				snprintf(err, en, "views is an array of at most %d", SF_MAX_VIEWS);
				goto fail;
			}
			for (size_t i = 0; i < json_object_array_length(val); i++) {
				json_object *w = json_object_array_get_idx(val, i), *x;
				struct sf_view *vw = &vs->views[vs->n_views++];
				if (!json_object_is_type(w, json_type_object) || !json_object_object_get_ex(w, "id", &x) ||
				    !json_object_get_string(x)[0] || strlen(json_object_get_string(x)) >= 64) {
					snprintf(err, en, "view %zu needs an id", i);
					goto fail;
				}
				vw->id = sdup(json_object_get_string(x));
				json_object_object_foreach(w, wk, wv) {
					const char *s = json_object_get_string(wv);
					if (!strcmp(wk, "id") || !strcmp(wk, "comment")) continue;
					if (!strcmp(wk, "label")) vw->label = sdup(s);
					else if (!strcmp(wk, "code")) snprintf(vw->code, sizeof(vw->code), "%s", s ? s : "");
					else if (!strcmp(wk, "layer")) {
						vw->layer = json_object_get_int(wv);
						if (vw->layer < 0 || vw->layer > 8) {
							snprintf(err, en, "view %s: layer is 1-8", vw->id);
							goto fail;
						}
					} else if (!strcmp(wk, "slots")) {
						if (!json_object_is_type(wv, json_type_array) || json_object_array_length(wv) > 4096) {
							snprintf(err, en, "view %s: slots is an array (at most 4096)", vw->id);
							goto fail;
						}
						vw->n_slots = (int)json_object_array_length(wv);
						vw->slots = calloc((size_t)(vw->n_slots ? vw->n_slots : 1), sizeof(*vw->slots));
						for (int k = 0; k < vw->n_slots; k++) {
							char e2[256];
							if (parse_slot(json_object_array_get_idx(wv, (size_t)k), &vw->slots[k], k, e2, sizeof(e2)) < 0) {
								snprintf(err, en, "view %s: %s", vw->id, e2);
								goto fail;
							}
						}
					} else if (!strcmp(wk, "master")) {
						char e2[256];
						if (parse_slot(wv, &vw->master, 0, e2, sizeof(e2)) < 0) {
							snprintf(err, en, "view %s master: %s", vw->id, e2);
							goto fail;
						}
						vw->has_master = !vw->master.empty;
					} else {
						snprintf(err, en, "view %s: unknown key '%s'", vw->id, wk);
						goto fail;
					}
				}
				if (!vw->label)
					vw->label = sdup(vw->id);
				if (!vw->code[0])
					snprintf(vw->code, sizeof(vw->code), "%.2s", vw->id);
				for (int k = 0; k < vs->n_views - 1; k++)
					if (!strcmp(vs->views[k].id, vw->id)) {
						snprintf(err, en, "view id %s twice", vw->id);
						goto fail;
					}
			}
		} else if (!strcmp(key, "pot-modes")) {
			if (!json_object_is_type(val, json_type_array) || json_object_array_length(val) > SF_MAX_MODES) {
				snprintf(err, en, "pot-modes is an array of at most %d", SF_MAX_MODES);
				goto fail;
			}
			for (size_t i = 0; i < json_object_array_length(val); i++)
				if (parse_mode(json_object_array_get_idx(val, i), &vs->modes[vs->n_modes++], err, en) < 0)
					goto fail;
		} else if (!strcmp(key, "home")) {
			json_object *x;
			if (json_object_object_get_ex(val, "view", &x))
				vs->home_view = sdup(json_object_get_string(x));
			if (json_object_object_get_ex(val, "pot-mode", &x))
				vs->home_mode = sdup(json_object_get_string(x));
		} else {
			snprintf(err, en, "unknown key '%s'", key);
			goto fail;
		}
	}
	json_object_put(o);
	return vs;
fail:
	json_object_put(o);
	free_views(vs);
	return NULL;
}

/* ============================================================ lookups */
static struct sf_views *gviews(struct sf_engine *e, struct group *g)
{
	return g->views ? g->views : e->defaults;
}

static struct sf_view *find_view(struct sf_views *vs, const char *id)
{
	if (!vs || !id)
		return NULL;
	for (int i = 0; i < vs->n_views; i++)
		if (!strcmp(vs->views[i].id, id))
			return &vs->views[i];
	return NULL;
}

static struct sf_mode *find_mode(struct sf_views *vs, const char *id)
{
	if (!vs || !id)
		return NULL;
	for (int i = 0; i < vs->n_modes; i++)
		if (!strcmp(vs->modes[i].id, id))
			return &vs->modes[i];
	return NULL;
}

static struct group *find_group(struct sf_engine *e, const char *id)
{
	for (int i = 0; i < SF_MAX_GROUPS; i++)
		if (e->groups[i].used && !strcmp(e->groups[i].id, id))
			return &e->groups[i];
	return NULL;
}

static struct unit *unit_online(struct sf_engine *e, const char *id)
{
	for (int i = 0; i < SF_MAX_UNITS; i++)
		if (e->units[i].state == SF_ONLINE && !strcmp(e->units[i].id, id))
			return &e->units[i];
	return NULL;
}

static struct group *group_of(struct sf_engine *e, const struct unit *u)
{
	if (!u->id[0])
		return NULL;
	for (int i = 0; i < SF_MAX_GROUPS; i++) {
		struct group *g = &e->groups[i];
		if (!g->used)
			continue;
		for (int k = 0; k < g->n_units; k++)
			if (!strcmp(g->units[k], u->id))
				return g;
	}
	return NULL;
}

static const char *cur_view_id(struct group *g)
{
	if (g->lock_view[0])
		return g->lock_view;
	return g->depth ? g->stack[g->depth - 1] : NULL;
}

static struct sf_view *cur_view(struct sf_engine *e, struct group *g)
{
	return find_view(gviews(e, g), cur_view_id(g));
}

static int *offset_ref(struct group *g, const char *view)
{
	for (int i = 0; i < g->n_offs; i++)
		if (!strcmp(g->offs[i].view, view))
			return &g->offs[i].offset;
	if (g->n_offs >= SF_MAX_VIEWS)
		return &g->offs[SF_MAX_VIEWS - 1].offset;
	struct off *o = &g->offs[g->n_offs++];
	snprintf(o->view, sizeof(o->view), "%s", view);
	o->offset = 0;
	return &o->offset;
}

static int cur_offset(struct group *g)
{
	const char *v = cur_view_id(g);
	if (!v)
		return 0;
	if (g->lock_view[0])
		return g->lock_offset;
	return *offset_ref(g, v);
}

/* the strips of the group's online units, and where unit u starts */
static int group_width(struct sf_engine *e, struct group *g, const struct unit *u, int *base)
{
	int w = 0;
	for (int k = 0; k < g->n_units; k++) {
		struct unit *x = unit_online(e, g->units[k]);
		if (!x)
			continue;
		if (x == u && base)
			*base = w;
		w += x->prof->strips;
	}
	return w;
}

static struct unit *group_master(struct sf_engine *e, struct group *g)
{
	for (int k = 0; k < g->n_units; k++) {
		struct unit *x = unit_online(e, g->units[k]);
		if (x && x->prof->master)
			return x;
	}
	return NULL;
}

static struct prm *param(struct sf_engine *e, const char *path)
{
	return path && path[0] == '/' ? prm_store_get(e->params, path) : NULL;
}

static double param_norm(struct sf_engine *e, const char *path, bool *known)
{
	struct prm *p = param(e, path);
	*known = p && p->type != PRM_STRING && p->type != PRM_TRIGGER;
	return *known ? prm_to_norm(p, p->value) : 0;
}

static bool param_on(struct sf_engine *e, const char *path)
{
	bool k;
	return param_norm(e, path, &k) > 0.5;
}

static const char *slot_text(struct sf_engine *e, const char *lit_or_path)
{
	if (!lit_or_path)
		return "";
	if (lit_or_path[0] != '/')
		return lit_or_path;
	struct prm *p = param(e, lit_or_path);
	return p ? (p->type == PRM_STRING ? p->str : p->text) : "";
}

static const struct sf_slot *slot_at(struct sf_view *v, int idx)
{
	if (!v || idx < 0 || idx >= v->n_slots || v->slots[idx].empty)
		return NULL;
	return &v->slots[idx];
}

static const struct sf_slot *selected_slot(struct sf_engine *e, struct group *g)
{
	if (!g->selected[0])
		return NULL;
	struct sf_views *vs = gviews(e, g);
	struct sf_view *v = cur_view(e, g);
	if (v)
		for (int i = 0; i < v->n_slots; i++)
			if (!v->slots[i].empty && !strcmp(v->slots[i].id, g->selected))
				return &v->slots[i];
	for (int k = 0; vs && k < vs->n_views; k++)
		for (int i = 0; i < vs->views[k].n_slots; i++)
			if (!vs->views[k].slots[i].empty && !strcmp(vs->views[k].slots[i].id, g->selected))
				return &vs->views[k].slots[i];
	return NULL;
}

static const char *slot_pot(const struct sf_slot *s, const char *key)
{
	if (!s || !key)
		return NULL;
	for (int i = 0; i < s->n_pots; i++)
		if (!strcmp(s->pots[i].key, key))
			return s->pots[i].path;
	return NULL;
}

static struct sf_mode *cur_mode(struct sf_engine *e, struct group *g)
{
	struct sf_views *vs = gviews(e, g);
	struct sf_mode *m = find_mode(vs, g->mode);
	if (!m && vs && vs->n_modes)
		m = find_mode(vs, vs->home_mode) ? find_mode(vs, vs->home_mode) : &vs->modes[0];
	return m;
}

/* "{slot}" in a page path, replaced with the selected slot's id */
static bool expand(const char *tmpl, const struct sf_slot *sel, char *out, size_t n)
{
	const char *p = strstr(tmpl, "{slot}");
	if (!p) {
		snprintf(out, n, "%s", tmpl);
		return true;
	}
	if (!sel)
		return false;
	snprintf(out, n, "%.*s%s%s", (int)(p - tmpl), tmpl, sel->id, p + 6);
	return true;
}

struct strip {
	const struct sf_slot *slot;
	char fader[PRM_PATH_MAX];
	char pot[PRM_PATH_MAX];
	const char *pot_label;
	int ring;
	bool line1_label;      /* strip mode: line 1 shows the parameter's label */
};

/* What global strip gs of a group is bound to right now. */
static void strip_of(struct sf_engine *e, struct group *g, int gs, struct strip *st)
{
	memset(st, 0, sizeof(*st));
	struct sf_view *v = cur_view(e, g);
	st->slot = slot_at(v, cur_offset(g) + gs);
	struct sf_mode *m = cur_mode(e, g);
	st->ring = m ? m->ring : MCU_RING_DOT;
	char across[PRM_PATH_MAX] = "";
	if (m && m->strip) {
		const struct sf_slot *sel = selected_slot(e, g);
		int page = g->page < m->n_pages ? g->page : 0;
		if (gs < 8 && gs < m->page_len[page] && m->pages[page][gs].path &&
		    expand(m->pages[page][gs].path, sel, across, sizeof(across))) {
			st->pot_label = m->pages[page][gs].label;
			st->ring = m->pages[page][gs].ring;
			st->line1_label = true;
		} else {
			across[0] = 0;
			st->line1_label = sel != NULL;
		}
	} else if (st->slot && m) {
		const char *p = slot_pot(st->slot, m->binding);
		snprintf(across, sizeof(across), "%s", p ? p : "");
	}
	const char *fader = st->slot && st->slot->fader ? st->slot->fader : "";
	if (g->flip) {
		snprintf(st->fader, sizeof(st->fader), "%s", across);
		snprintf(st->pot, sizeof(st->pot), "%s", m && m->strip ? "" : fader);
	} else {
		snprintf(st->fader, sizeof(st->fader), "%s", m && m->strip && !st->slot ? "" : fader);
		snprintf(st->pot, sizeof(st->pot), "%s", across);
	}
}

/* ============================================================ bindings */
static int parse_binding(const char *s, struct sf_bind *b, char *err, size_t en)
{
	memset(b, 0, sizeof(*b));
	json_object *o = NULL;
	char tmp[512];
	if (s[0] == '{') {
		o = json_tokener_parse(s);
		json_object *x;
		if (!o || !json_object_object_get_ex(o, "do", &x)) {
			json_object_put(o);
			snprintf(err, en, "a binding object has \"do\"");
			return -1;
		}
		snprintf(tmp, sizeof(tmp), "%s", json_object_get_string(x));
		if (json_object_object_get_ex(o, "led", &x))
			snprintf(b->led, sizeof(b->led), "%s", json_object_get_string(x));
		json_object_put(o);
		s = tmp;
	}
	static const struct { const char *n; enum bkind k; } simple[] = {
		{ "none", K_NONE }, { "bank-left", K_BANK_L }, { "bank-right", K_BANK_R }, { "channel-left", K_CH_L },
		{ "channel-right", K_CH_R }, { "bank-first", K_BANK_FIRST }, { "bank-last", K_BANK_LAST }, { "flip", K_FLIP },
		{ "name-value", K_NAMEVALUE }, { "pop", K_POP }, { "page-next", K_PAGE_NEXT }, { "page-prev", K_PAGE_PREV },
	};
	for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); i++)
		if (!strcmp(s, simple[i].n)) {
			b->kind = simple[i].k;
			return 0;
		}
	if (!strncmp(s, "action:", 7) && s[7] && strlen(s + 7) < 64) {
		b->kind = K_ACTION;
		snprintf(b->arg, sizeof(b->arg), "%s", s + 7);
	} else if ((!strncmp(s, "param:", 6) && path_ok(s + 6)) || (!strncmp(s, "momentary:", 10) && path_ok(s + 10))) {
		b->kind = s[0] == 'p' ? K_PARAM : K_MOMENTARY;
		snprintf(b->arg, sizeof(b->arg), "%s", strchr(s, ':') + 1);
	} else if (!strncmp(s, "layer:", 6) && atoi(s + 6) >= 1 && atoi(s + 6) <= 8) {
		b->kind = K_LAYER;
		b->n = atoi(s + 6);
	} else if (!strncmp(s, "pot-mode:", 9) && s[9] && strlen(s + 9) < 64) {
		b->kind = K_POTMODE;
		snprintf(b->arg, sizeof(b->arg), "%s", s + 9);
	} else {
		snprintf(err, en, "'%s' is not a binding (action:NAME, param:PATH, momentary:PATH, bank-left, bank-right, "
			 "channel-left, channel-right, bank-first, bank-last, flip, name-value, pop, layer:N, pot-mode:ID, "
			 "page-next, page-prev, none)", s);
		return -1;
	}
	return 0;
}

/* The binding of a control on a group: the group's own, the profile's
 * default, or none (the caller makes it an action). */
static bool binding_for(struct sf_engine *e, struct group *g, const struct sf_profile *prof, const char *control,
			struct sf_bind *out)
{
	for (int i = 0; i < g->n_binds; i++)
		if (!strcmp(g->binds[i].control, control)) {
			*out = g->binds[i];
			return true;
		}
	const char *d = prof ? sf_profile_binding(prof, control) : NULL;
	char err[64];
	if (d && parse_binding(d, out, err, sizeof(err)) == 0) {
		snprintf(out->control, sizeof(out->control), "%s", control);
		return true;
	}
	return false;
}

/* ============================================================ navigation */
static void navigated(struct sf_engine *e, struct group *g)
{
	struct sf_mode *m = cur_mode(e, g);
	const char *v = cur_view_id(g);
	if (e->host.navigated)
		e->host.navigated(e->data, g->id, v ? v : "", cur_offset(g), m ? m->id : "", g->page);
}

static void clamp_offset(struct sf_engine *e, struct group *g)
{
	struct sf_view *v = cur_view(e, g);
	if (!v || g->lock_view[0])
		return;
	int *o = offset_ref(g, v->id);
	int w = group_width(e, g, NULL, NULL);
	int max = v->n_slots - (w > 0 ? w : 1);
	if (max < 0) max = 0;
	if (*o > max) *o = max;
	if (*o < 0) *o = 0;
}

static void move_window(struct sf_engine *e, struct group *g, int delta, int absolute)
{
	struct sf_view *v = cur_view(e, g);
	if (!v || g->lock_view[0])
		return;
	int *o = offset_ref(g, v->id);
	*o = absolute >= 0 ? absolute : *o + delta;
	if (absolute == INT32_MAX)
		*o = v->n_slots;
	clamp_offset(e, g);
	g->bank_until = e->now + 1000 * MS;
	navigated(e, g);
}

static void show_layer(struct sf_engine *e, struct group *g, int layer)
{
	struct sf_views *vs = gviews(e, g);
	if (!vs)
		return;
	struct sf_view *cur = cur_view(e, g);
	int first = -1, next = -1;
	bool after = false;
	for (int i = 0; i < vs->n_views; i++) {
		if (vs->views[i].layer != layer)
			continue;
		if (first < 0)
			first = i;
		if (after && next < 0)
			next = i;
		if (cur == &vs->views[i])
			after = true;
	}
	int pick = cur && cur->layer == layer && next >= 0 ? next : first;
	if (pick < 0)
		return;
	snprintf(g->stack[0], sizeof(g->stack[0]), "%s", vs->views[pick].id);
	g->depth = 1;
	clamp_offset(e, g);
	navigated(e, g);
}

static void ensure_view(struct sf_engine *e, struct group *g)
{
	struct sf_views *vs = gviews(e, g);
	/* drop stack entries that no longer exist */
	int k = 0;
	for (int i = 0; i < g->depth; i++)
		if (find_view(vs, g->stack[i]))
			memmove(g->stack[k++], g->stack[i], sizeof(g->stack[0]));
	g->depth = k;
	if (!g->depth && vs && vs->n_views) {
		const char *h = find_view(vs, vs->home_view) ? vs->home_view : vs->views[0].id;
		snprintf(g->stack[0], sizeof(g->stack[0]), "%s", h);
		g->depth = 1;
	}
	if (vs && !find_mode(vs, g->mode) && vs->n_modes) {
		const char *h = find_mode(vs, vs->home_mode) ? vs->home_mode : vs->modes[0].id;
		snprintf(g->mode, sizeof(g->mode), "%s", h);
		g->page = 0;
	}
	clamp_offset(e, g);
}

/* ============================================================ engine */
struct sf_engine *sf_new(const struct sf_host *host, void *data, struct prm_store *params)
{
	struct sf_engine *e = calloc(1, sizeof(*e));
	e->host = *host;
	e->data = data;
	e->params = params;
	e->next_group = 1;
	return e;
}

void sf_set_params(struct sf_engine *e, struct prm_store *params)
{
	e->params = params;
}

void sf_free(struct sf_engine *e)
{
	if (!e)
		return;
	for (int i = 0; i < e->n_profiles; i++)
		sf_profile_free(&e->profiles[i]);
	for (int i = 0; i < SF_MAX_GROUPS; i++)
		free_views(e->groups[i].views);
	free_views(e->defaults);
	free(e);
}

int sf_add_profile(struct sf_engine *e, const struct sf_profile *p)
{
	for (int i = 0; i < e->n_profiles; i++)
		if (!strcmp(e->profiles[i].id, p->id))
			return -1; /* the first (the user's) wins */
	if (e->n_profiles >= SF_MAX_PROFILES)
		return -1;
	e->profiles[e->n_profiles++] = *p;
	return 0;
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(a, b);
}

int sf_load_profile_dir(struct sf_engine *e, const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	struct dirent *de;
	int n = 0;
	char names[128][256];
	int nn = 0;
	while ((de = readdir(d)) && nn < 128) {
		size_t l = strlen(de->d_name);
		if (l > 13 && !strcmp(de->d_name + l - 13, ".surface.json"))
			snprintf(names[nn++], sizeof(names[0]), "%s", de->d_name);
	}
	closedir(d);
	qsort(names, (size_t)nn, sizeof(names[0]), name_cmp);
	for (int i = 0; i < nn; i++) {
		char path[800], err[256];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		struct sf_profile p;
		if (sf_profile_load(path, &p, err, sizeof(err)) < 0) {
			elog(e, "%s: %s", path, err);
			continue;
		}
		if (sf_add_profile(e, &p) < 0)
			sf_profile_free(&p);
		else
			n++;
	}
	return n;
}

const struct sf_profile *sf_profile_for_port(struct sf_engine *e, const char *port)
{
	for (int i = 0; i < e->n_profiles; i++)
		if (sf_profile_matches(&e->profiles[i], port))
			return &e->profiles[i];
	return NULL;
}

const struct sf_profile *sf_profile_by_id(struct sf_engine *e, const char *id)
{
	for (int i = 0; i < e->n_profiles; i++)
		if (!strcmp(e->profiles[i].id, id))
			return &e->profiles[i];
	return NULL;
}

int sf_n_profiles(struct sf_engine *e) { return e->n_profiles; }
const struct sf_profile *sf_profile_at(struct sf_engine *e, int i) { return i >= 0 && i < e->n_profiles ? &e->profiles[i] : NULL; }

static void dev_unknown(struct dev *d)
{
	for (int i = 0; i < 9; i++) d->fader[i] = -1;
	memset(d->led, 255, sizeof(d->led));
	memset(d->ring, 255, sizeof(d->ring));
	memset(d->lcd, 0, sizeof(d->lcd));
	memset(d->colour, 255, sizeof(d->colour));
	for (int i = 0; i < 8; i++) d->meter[i] = -1;
	memset(d->tc, 255, sizeof(d->tc));
	memset(d->assign, 255, sizeof(d->assign));
}

static void send(struct sf_engine *e, struct unit *u, const uint8_t *m, size_t n)
{
	if (!n)
		return;
	e->host.send(e->data, (int)(u - e->units), m, n);
	e->msgs_out++;
	u->tokens -= (double)n;
}

static void sanitize(const char *in, char *out, size_t n)
{
	size_t k = 0;
	for (; *in && k + 1 < n; in++)
		out[k++] = (*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z') || (*in >= '0' && *in <= '9') || *in == '-' ||
				   *in == '_' || *in == '.'
			   ? *in
			   : '_';
	out[k] = 0;
}

static void set_text(struct unit *u, const char *text, uint64_t until)
{
	memset(u->text, 0, sizeof(u->text));
	const char *nl = strchr(text, '\n');
	snprintf(u->text[0], sizeof(u->text[0]), "%.*s", nl ? (int)(nl - text) : 56, text);
	if (nl)
		snprintf(u->text[1], sizeof(u->text[1]), "%.56s", nl + 1);
	u->text_until = until;
	u->text_on = true;
}

static void save_state(struct sf_engine *e)
{
	if (e->host.state_changed)
		e->host.state_changed(e->data);
}

/* The unit is known: give it an id, put it back where it was, redraw. */
static void finalize(struct sf_engine *e, struct unit *u, const char *serial)
{
	char id[160], tail[128];
	if (serial && *serial) {
		sanitize(serial, tail, sizeof(tail));
		snprintf(u->serial, sizeof(u->serial), "%s", serial);
	} else {
		sanitize(u->port, tail, sizeof(tail));
	}
	snprintf(id, sizeof(id), "%s@%s", u->prof->id, tail);
	/* two devices with the same serial: the second gets #2 */
	for (int k = 2; unit_online(e, id) && k < 10; k++)
		snprintf(id, sizeof(id), "%s@%s#%d", u->prof->id, tail, k);
	/* forget a remembered (gone) record of the same id: this is it, back */
	for (int i = 0; i < SF_MAX_UNITS; i++) {
		struct unit *x = &e->units[i];
		if (x != u && x->state == SF_GONE && !strcmp(x->id, id))
			x->state = SF_FREE;
	}
	snprintf(u->id, sizeof(u->id), "%s", id);
	u->state = SF_ONLINE;
	dev_unknown(&u->sent);
	u->tokens = 0;
	struct group *g = group_of(e, u);
	if (!g) {
		struct group *target = NULL;
		/* an extender joins the first group with a master, on its right; a
		 * master joins the first group of extenders only, on their right */
		for (int i = 0; i < SF_MAX_GROUPS && !target; i++) {
			struct group *c = &e->groups[i];
			if (!c->used || c->n_units >= SF_GROUP_UNITS)
				continue;
			bool has_master = group_master(e, c) != NULL;
			int online = group_width(e, c, NULL, NULL);
			if (u->prof->master ? (!has_master && online > 0) : has_master)
				target = c;
		}
		if (!target) {
			for (int i = 0; i < SF_MAX_GROUPS && !target; i++)
				if (!e->groups[i].used)
					target = &e->groups[i];
			if (target) {
				memset(target, 0, sizeof(*target));
				target->used = true;
				do
					snprintf(target->id, sizeof(target->id), "g%d", e->next_group++);
				while (find_group(e, target->id) != target);
				snprintf(target->role, sizeof(target->role), "all");
			}
		}
		if (target) {
			snprintf(target->units[target->n_units++], sizeof(target->units[0]), "%s", id);
			g = target;
		}
		save_state(e);
	}
	if (g)
		ensure_view(e, g);
	char greet[64];
	snprintf(greet, sizeof(greet), "Backline %s", g ? g->role : "");
	set_text(u, greet, e->now + 1000 * MS);
	elog(e, "%s online (%s, port %s%s%s)", u->id, u->prof->name, u->port, g ? ", group " : "", g ? g->id : "");
	if (e->host.unit_event)
		e->host.unit_event(e->data, u->id, true);
}

int sf_unit_attach(struct sf_engine *e, const struct sf_profile *p, const char *port, uint64_t now)
{
	e->now = now;
	struct unit *u = NULL;
	for (int i = 0; i < SF_MAX_UNITS && !u; i++)
		if (e->units[i].state == SF_FREE)
			u = &e->units[i];
	/* full: reuse the oldest remembered unit */
	for (int i = 0; i < SF_MAX_UNITS && !u; i++)
		if (e->units[i].state == SF_GONE && (!u || e->units[i].gone_ns < u->gone_ns))
			u = &e->units[i];
	if (!u)
		return -1;
	memset(u, 0, sizeof(*u));
	u->prof = p;
	snprintf(u->port, sizeof(u->port), "%s", port);
	u->state = SF_HANDSHAKE;
	u->last_ns = now;
	dev_unknown(&u->sent);
	if (p->handshake == SF_HS_NONE) {
		finalize(e, u, NULL);
	} else {
		uint8_t m[16];
		u->tokens = 64;
		send(e, u, m, mcu_device_query(p->model, m, sizeof(m)));
		u->hs_ns = now;
		u->hs_tries = 1;
	}
	return (int)(u - e->units);
}

static void end_unit_gestures(struct sf_engine *e, struct unit *u, const char *why)
{
	struct group *g = group_of(e, u);
	const char *role = g ? g->role : "all";
	for (int i = 0; i < 9; i++) {
		if (u->fader_gest[i][0]) {
			e->host.gesture(e->data, u->fader_gest[i], false, role, u->id);
			elog(e, "%s: fader %d gesture on %s ended (%s)", u->id, i + 1, u->fader_gest[i], why);
			u->fader_gest[i][0] = 0;
		}
		u->touched[i] = false;
		u->motion[i] = false;
	}
	for (int i = 0; i < 8; i++)
		if (u->pot_gest[i][0]) {
			e->host.gesture(e->data, u->pot_gest[i], false, role, u->id);
			u->pot_gest[i][0] = 0;
		}
}

void sf_unit_detach(struct sf_engine *e, int ui, uint64_t now)
{
	if (ui < 0 || ui >= SF_MAX_UNITS)
		return;
	struct unit *u = &e->units[ui];
	e->now = now;
	bool was_online = u->state == SF_ONLINE;
	end_unit_gestures(e, u, "unplugged");
	if (!u->id[0]) {
		u->state = SF_FREE;
		return;
	}
	u->state = SF_GONE;
	u->gone_ns = now;
	elog(e, "%s gone", u->id);
	struct group *g = group_of(e, u);
	if (g)
		clamp_offset(e, g);
	if (was_online && e->host.unit_event)
		e->host.unit_event(e->data, u->id, false);
}

void sf_unit_goodbye(struct sf_engine *e, int ui)
{
	struct unit *u = &e->units[ui];
	if (u->state != SF_ONLINE)
		return;
	end_unit_gestures(e, u, "surfaced stopping");
	uint8_t m[160];
	char blank[MCU_LCD_CELLS];
	memset(blank, ' ', sizeof(blank));
	for (int i = 0; i < 9; i++)
		send(e, u, m, mcu_fader(i, 0, m));
	for (size_t i = 0; i < mcu_n_controls; i++)
		if (!mcu_is_strip_button(mcu_controls[i].id) || mcu_controls[i].id < MCU_TOUCH)
			send(e, u, m, mcu_led(mcu_controls[i].id, 0, m));
	for (int i = 0; i < 8; i++)
		send(e, u, m, mcu_ring(i, 0, m));
	if (u->prof->lcd)
		send(e, u, m, mcu_lcd(u->prof->model, 0, blank, MCU_LCD_CELLS, m, sizeof(m)));
	if (u->prof->colour) {
		uint8_t c[8] = { 0 };
		send(e, u, m, mcu_colours(u->prof->model, c, m, sizeof(m)));
	}
	send(e, u, m, mcu_go_offline(u->prof->model, m, sizeof(m)));
}

/* ============================================================ input */
static void action(struct sf_engine *e, struct group *g, const char *name, bool down)
{
	if (e->host.action)
		e->host.action(e->data, g ? g->id : "", name, down);
}

static void set_norm(struct sf_engine *e, struct group *g, struct unit *u, const char *path, double norm)
{
	e->host.set(e->data, path, norm, g ? g->role : "all", u->id);
}

static void press_param(struct sf_engine *e, struct group *g, struct unit *u, const char *path, bool momentary, bool down)
{
	struct prm *p = param(e, path);
	if (!p)
		return;
	if (p->type == PRM_TRIGGER) {
		if (down)
			set_norm(e, g, u, path, 1);
		return;
	}
	if (momentary) {
		set_norm(e, g, u, path, down ? 1 : 0);
		return;
	}
	if (down)
		set_norm(e, g, u, path, prm_to_norm(p, p->value) > 0.5 ? 0 : 1);
}

static void run_binding(struct sf_engine *e, struct group *g, struct unit *u, const struct sf_bind *b, const char *name,
			bool down)
{
	struct sf_mode *m;
	switch (b->kind) {
	case K_NONE:
		return;
	case K_ACTION:
		action(e, g, b->arg, down);
		return;
	case K_PARAM:
	case K_MOMENTARY:
		press_param(e, g, u, b->arg, b->kind == K_MOMENTARY, down);
		return;
	default:
		break;
	}
	if (!down)
		return;
	int w = group_width(e, g, NULL, NULL);
	switch (b->kind) {
	case K_BANK_L: move_window(e, g, -w, -1); break;
	case K_BANK_R: move_window(e, g, w, -1); break;
	case K_CH_L: move_window(e, g, -1, -1); break;
	case K_CH_R: move_window(e, g, 1, -1); break;
	case K_BANK_FIRST: move_window(e, g, 0, 0); break;
	case K_BANK_LAST: move_window(e, g, 0, INT32_MAX); break;
	case K_FLIP: g->flip = !g->flip; break;
	case K_NAMEVALUE: g->name_value = !g->name_value; break;
	case K_POP:
		if (g->depth > 1) {
			g->depth--;
			clamp_offset(e, g);
			navigated(e, g);
		}
		break;
	case K_LAYER: show_layer(e, g, b->n); break;
	case K_POTMODE:
		if (find_mode(gviews(e, g), b->arg)) {
			snprintf(g->mode, sizeof(g->mode), "%s", b->arg);
			g->page = 0;
			navigated(e, g);
		}
		break;
	case K_PAGE_NEXT:
	case K_PAGE_PREV:
		m = cur_mode(e, g);
		if (m && m->strip && m->n_pages) {
			g->page = (g->page + (b->kind == K_PAGE_NEXT ? 1 : m->n_pages - 1)) % m->n_pages;
			navigated(e, g);
		}
		break;
	default:
		break;
	}
	(void)name;
}

static void modifiers(const struct unit *u, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s%s", u->shift ? "shift+" : "", u->option ? "option+" : "", u->control ? "control+" : "",
		 u->alt ? "alt+" : "");
}

static void global_button(struct sf_engine *e, struct group *g, struct unit *u, int id, bool down)
{
	const char *name = mcu_control_name(id);
	if (!name)
		return;
	if (!strcmp(name, "shift")) u->shift = down;
	else if (!strcmp(name, "option")) u->option = down;
	else if (!strcmp(name, "control")) u->control = down;
	else if (!strcmp(name, "alt")) u->alt = down;
	if (!strcmp(name, "shift") || !strcmp(name, "option") || !strcmp(name, "control") || !strcmp(name, "alt"))
		return;
	char mods[48], full[96];
	modifiers(u, mods, sizeof(mods));
	snprintf(full, sizeof(full), "%s%s", mods, name);
	struct sf_bind b;
	if (g && binding_for(e, g, u->prof, full, &b))
		run_binding(e, g, u, &b, full, down);
	else
		action(e, g, full, down);
}

static void strip_button(struct sf_engine *e, struct group *g, struct unit *u, int id, bool down)
{
	int base = 0;
	group_width(e, g, u, &base);
	if (id >= MCU_VPOT && id < MCU_VPOT + 8) {
		int i = id - MCU_VPOT;
		struct strip st;
		strip_of(e, g, base + i, &st);
		struct sf_mode *m = cur_mode(e, g);
		if (!down || !st.pot[0])
			return;
		if (m && m->push == 2 && m->push_action) {
			char name[160];
			snprintf(name, sizeof(name), "%s:%s", m->push_action, st.slot ? st.slot->id : "");
			action(e, g, name, true);
			action(e, g, name, false);
		} else if (!m || m->push == 0) {
			struct prm *p = param(e, st.pot);
			if (p)
				set_norm(e, g, u, st.pot, prm_to_norm(p, p->def));
		}
		return;
	}
	int kind = id / 8, i = id % 8;
	if (kind >= B_COUNT || i >= u->prof->strips)
		return;
	struct strip st;
	strip_of(e, g, base + i, &st);
	if (!st.slot)
		return; /* an empty slot is inert */
	const struct sf_btn *b = &st.slot->btn[kind];
	if (b->action) {
		char name[160];
		snprintf(name, sizeof(name), "%s:%s", b->action, st.slot->id);
		action(e, g, name, down);
	} else if (b->path) {
		press_param(e, g, u, b->path, b->momentary, down);
	}
}

static double pot_mult(const struct unit *u)
{
	return u->control ? 0.25 : u->shift ? 4.0 : 1.0;
}

static struct unit *master_of(struct sf_engine *e, struct group *g, struct unit *self)
{
	struct unit *m = g ? group_master(e, g) : NULL;
	return m ? m : self;
}

static void vpot(struct sf_engine *e, struct group *g, struct unit *u, int i, int delta)
{
	int base = 0;
	group_width(e, g, u, &base);
	struct strip st;
	strip_of(e, g, base + i, &st);
	struct prm *p = param(e, st.pot);
	if (!p || p->type == PRM_STRING || p->type == PRM_TRIGGER || !delta)
		return;
	struct unit *mods = master_of(e, g, u); /* modifiers live on the master unit */
	struct sf_mode *m = cur_mode(e, g);
	double norm = prm_to_norm(p, p->value);
	if (p->type == PRM_FLOAT) {
		norm += delta * (m ? m->step : 0.01) * pot_mult(mods);
	} else {
		u->pot_acc[i] += delta * pot_mult(mods);
		int whole = (int)u->pot_acc[i];
		u->pot_acc[i] -= whole;
		if (!whole)
			return;
		double step = p->step > 0 ? p->step : 1;
		norm = prm_to_norm(p, p->value + whole * step);
	}
	if (norm < 0) norm = 0;
	if (norm > 1) norm = 1;
	if (p->automatable && strcmp(u->pot_gest[i], p->path)) {
		if (u->pot_gest[i][0])
			e->host.gesture(e->data, u->pot_gest[i], false, g->role, u->id);
		snprintf(u->pot_gest[i], sizeof(u->pot_gest[i]), "%s", p->path);
		e->host.gesture(e->data, p->path, true, g->role, u->id);
	}
	u->pot_ns[i] = e->now;
	u->pot_show[i] = e->now + 1000 * MS;
	set_norm(e, g, u, p->path, norm);
}

static void fader_path(struct sf_engine *e, struct group *g, struct unit *u, int i, char *out, size_t n)
{
	out[0] = 0;
	if (i == 8) {
		struct sf_view *v = cur_view(e, g);
		if (v && v->has_master && v->master.fader && u->prof->master_fader)
			snprintf(out, n, "%s", v->master.fader);
		return;
	}
	int base = 0;
	group_width(e, g, u, &base);
	struct strip st;
	strip_of(e, g, base + i, &st);
	snprintf(out, n, "%s", st.fader);
}

static void touch(struct sf_engine *e, struct group *g, struct unit *u, int i, bool down)
{
	if (i > 8 || (i == 8 && !u->prof->master_fader) || (i < 8 && i >= u->prof->strips))
		return;
	if (down) {
		if (u->touched[i])
			return;
		u->touched[i] = true;
		u->touch_ns[i] = u->move_ns[i] = e->now;
		u->stale_touch[i] = false;
		u->echo[i] = u->sent.fader[i] >= 0 ? u->sent.fader[i] : 0;
		char path[PRM_PATH_MAX];
		fader_path(e, g, u, i, path, sizeof(path));
		struct prm *p = param(e, path);
		if (u->motion[i]) {
			u->motion[i] = false; /* the touch takes over the motion gesture */
			return;
		}
		if (p && p->automatable) {
			snprintf(u->fader_gest[i], sizeof(u->fader_gest[i]), "%s", path);
			e->host.gesture(e->data, path, true, g->role, u->id);
		}
		return;
	}
	if (!u->touched[i])
		return;
	u->touched[i] = false;
	if (u->fader_gest[i][0]) {
		e->host.gesture(e->data, u->fader_gest[i], false, g->role, u->id);
		u->fader_gest[i][0] = 0;
	}
	/* the motor goes to the confirmed value: forget what we echoed */
	u->sent.fader[i] = -1;
}

static void fader_move(struct sf_engine *e, struct group *g, struct unit *u, int i, int v14)
{
	if (i > 8 || (i == 8 && !u->prof->master_fader) || (i < 8 && i >= u->prof->strips))
		return;
	char path[PRM_PATH_MAX];
	fader_path(e, g, u, i, path, sizeof(path));
	if (u->prof->touch_sense && !u->touched[i])
		return; /* never move a parameter from an untouched fader: motor overshoot echoes */
	struct prm *p = param(e, path);
	if (!p)
		return;
	if (!u->prof->touch_sense && !u->touched[i]) {
		if (!u->motion[i] && p->automatable) {
			u->motion[i] = true;
			snprintf(u->fader_gest[i], sizeof(u->fader_gest[i]), "%s", path);
			e->host.gesture(e->data, path, true, g->role, u->id);
		}
	}
	u->move_ns[i] = e->now;
	if (u->stale_touch[i] && p->automatable && !u->fader_gest[i][0]) {
		/* moving again after a stale touch: a new gesture */
		snprintf(u->fader_gest[i], sizeof(u->fader_gest[i]), "%s", path);
		e->host.gesture(e->data, path, true, g->role, u->id);
		u->stale_touch[i] = false;
	}
	u->echo[i] = v14;
	set_norm(e, g, u, path, v14 / 16383.0);
}

void sf_unit_input(struct sf_engine *e, int ui, const uint8_t *msg, size_t n, uint64_t now)
{
	if (ui < 0 || ui >= SF_MAX_UNITS)
		return;
	struct unit *u = &e->units[ui];
	e->now = now;
	struct mcu_in in;
	if (!mcu_decode(msg, n, &in))
		return;
	e->msgs_in++;
	if (in.kind == MCU_IN_HOST_QUERY || in.kind == MCU_IN_CONFIRM || in.kind == MCU_IN_READY) {
		if (in.kind == MCU_IN_HOST_QUERY && u->prof->handshake == SF_HS_LOGIC) {
			uint8_t m[32];
			send(e, u, m, mcu_lc_reply(u->prof->model, in.serial, in.challenge, m, sizeof(m)));
		}
		char serial[32] = "";
		if (in.kind != MCU_IN_READY)
			mcu_serial_text(in.serial, serial, sizeof(serial));
		if (u->state == SF_HANDSHAKE) {
			finalize(e, u, serial);
		} else if (u->state == SF_ONLINE && in.kind == MCU_IN_HOST_QUERY) {
			elog(e, "%s asked to connect again (it restarted): redrawing", u->id);
			dev_unknown(&u->sent);
		}
		return;
	}
	if (u->state != SF_ONLINE)
		return;
	struct group *g = group_of(e, u);
	if (!g)
		return;
	switch (in.kind) {
	case MCU_IN_BUTTON:
		if (mcu_is_strip_button(in.id))
			strip_button(e, g, u, in.id, in.down);
		else if (u->prof->master)
			global_button(e, g, u, in.id, in.down);
		break;
	case MCU_IN_TOUCH:
		touch(e, g, u, in.index, in.down);
		break;
	case MCU_IN_FADER:
		fader_move(e, g, u, in.index, in.value);
		break;
	case MCU_IN_VPOT:
		vpot(e, g, u, in.index, in.delta);
		break;
	case MCU_IN_JOG:
	case MCU_IN_PEDAL: {
		char name[48], mods[48];
		modifiers(u, mods, sizeof(mods));
		if (in.kind == MCU_IN_JOG)
			snprintf(name, sizeof(name), "%sjog:%+d", mods, in.delta);
		else
			snprintf(name, sizeof(name), "pedal:%d", in.value);
		action(e, g, name, true);
		break;
	}
	default:
		break;
	}
}

/* ============================================================ feedback */
static uint8_t led_state(struct sf_engine *e, const struct unit *u, bool on, bool flash)
{
	if (flash)
		return u->prof->no_flash ? 2 : 1;
	return on ? 2 : 0;
}

static void lcd_cell(char *line, int strip, const char *text)
{
	char cell[8];
	snprintf(cell, sizeof(cell), "%-6.6s ", text);
	memcpy(line + strip * 7, cell, 7);
}

static void bind_led(struct sf_engine *e, struct group *g, const struct sf_bind *b, uint8_t *st, const struct unit *u)
{
	const char *src = b->led;
	if (src[0] == '/') {
		*st = led_state(e, u, param_on(e, src), false);
		return;
	}
	if (!strcmp(src, "flip") || (!src[0] && b->kind == K_FLIP)) {
		*st = g->flip ? 2 : 0;
	} else if (!strcmp(src, "pushed")) {
		*st = g->depth > 1 ? led_state(e, u, false, true) : 0;
	} else if (!strcmp(src, "name-value") || (!src[0] && b->kind == K_NAMEVALUE)) {
		*st = g->name_value ? 2 : 0;
	} else if (!src[0] && b->kind == K_LAYER) {
		struct sf_view *v = cur_view(e, g);
		*st = v && v->layer == b->n ? 2 : 0;
	} else if (!src[0] && b->kind == K_POTMODE) {
		struct sf_mode *m = cur_mode(e, g);
		*st = m && !strcmp(m->id, b->arg) ? 2 : 0;
	} else if (!src[0] && (b->kind == K_PARAM || b->kind == K_MOMENTARY)) {
		*st = led_state(e, u, param_on(e, b->arg), false);
	}
}

static void compute_want(struct sf_engine *e, struct unit *u)
{
	struct dev *w = &u->want;
	memset(w->led, 0, sizeof(w->led));
	memset(w->lcd, ' ', sizeof(w->lcd));
	memset(w->colour, 0, sizeof(w->colour));
	memset(w->ring, 0, sizeof(w->ring));
	for (int i = 0; i < 9; i++) w->fader[i] = 0;
	for (int i = 0; i < 8; i++) w->meter[i] = 0;
	memset(w->tc, mcu_7seg(' ', false), sizeof(w->tc));
	memset(w->assign, mcu_7seg(' ', false), sizeof(w->assign));
	struct group *g = group_of(e, u);
	if (!g)
		return;
	int base = 0;
	group_width(e, g, u, &base);
	struct sf_mode *m = cur_mode(e, g);
	for (int i = 0; i < u->prof->strips; i++) {
		struct strip st;
		strip_of(e, g, base + i, &st);
		const struct sf_slot *s = st.slot;
		bool known;
		/* fader */
		double fn = param_norm(e, st.fader, &known);
		w->fader[i] = u->touched[i] || u->motion[i] ? u->echo[i] : (int)lround(fn * 16383);
		if (!s && !(st.fader[0]))
			w->fader[i] = 0;
		/* ring */
		struct prm *pp = param(e, st.pot);
		double pn = param_norm(e, st.pot, &known);
		w->ring[i] = mcu_ring_byte((enum mcu_ring_mode)st.ring, pn, false, !pp || !known);
		/* LEDs */
		if (s) {
			for (int b = 0; b < B_COUNT; b++) {
				const struct sf_btn *bt = &s->btn[b];
				bool flash = bt->flash && param_on(e, bt->flash);
				bool on = (bt->led && param_on(e, bt->led)) || (!bt->led && bt->path && param_on(e, bt->path));
				if (b == B_SELECT && !bt->path && !bt->led && g->selected[0] && !strcmp(g->selected, s->id))
					on = true; /* select LED follows SetSelected unless bound */
				w->led[b * 8 + i] = led_state(e, u, on, flash);
			}
		}
		/* text */
		char l1[64] = "", l2[64] = "";
		if (st.line1_label) {
			snprintf(l1, sizeof(l1), "%s", st.pot_label ? st.pot_label : (pp ? (pp->short_name[0] ? pp->short_name : pp->name) : ""));
		} else if (s) {
			const char *nm = g->name_value ? s->id : slot_text(e, s->name ? s->name : s->id);
			mcu_abbrev(nm, l1, 6);
		}
		if (u->touched[i] || u->motion[i]) {
			struct prm *fp = param(e, st.fader);
			if (fp)
				snprintf(l2, sizeof(l2), "%s", fp->text);
		} else if (pp && (u->pot_show[i] > e->now || !(m && m->line2_label))) {
			snprintf(l2, sizeof(l2), "%s", pp->text);
		} else if (pp && m) {
			snprintf(l2, sizeof(l2), "%s", st.pot_label ? st.pot_label : m->label);
		}
		if (g->flip && s && !st.line1_label && l2[0] == 0)
			snprintf(l2, sizeof(l2), "flip");
		char a1[8], a2[8];
		mcu_abbrev(l1, a1, 6);
		mcu_abbrev(l2, a2, 6);
		lcd_cell(w->lcd, i, a1);
		lcd_cell(w->lcd + 56, i, a2);
		/* colour */
		if (s && s->colour) {
			int r, gg, b;
			if (mcu_parse_colour(slot_text(e, s->colour), &r, &gg, &b) == 0)
				w->colour[i] = mcu_colour_nearest(r, gg, b);
			else
				w->colour[i] = 7;
		} else if (s) {
			w->colour[i] = 7;
		}
		/* meter */
		if (s && s->meter_root && e->host.meter) {
			float db;
			bool clip = false;
			if (e->host.meter(e->data, s->meter_root, s->meter_id, &db, &clip)) {
				w->meter[i] = mcu_meter_level(db);
				if (clip)
					w->meter[i] |= 0x100; /* clip flag, handled when sending */
			}
		}
	}
	/* master fader */
	if (u->prof->master_fader) {
		char path[PRM_PATH_MAX];
		fader_path(e, g, u, 8, path, sizeof(path));
		bool known;
		double fn = param_norm(e, path, &known);
		w->fader[8] = u->touched[8] || u->motion[8] ? u->echo[8] : (int)lround(fn * 16383);
	}
	/* globals: LEDs of bound buttons, 7-segment displays */
	if (u->prof->master) {
		for (size_t k = 0; k < mcu_n_controls; k++) {
			int id = mcu_controls[k].id;
			if (mcu_is_strip_button(id))
				continue;
			struct sf_bind b;
			if (binding_for(e, g, u->prof, mcu_controls[k].name, &b))
				bind_led(e, g, &b, &w->led[id], u);
		}
		if (u->prof->assignment) {
			struct sf_bind b;
			char a[8] = "  ";
			struct sf_view *v = cur_view(e, g);
			if (binding_for(e, g, u->prof, "assignment", &b) && (b.kind == K_PARAM || b.kind == K_MOMENTARY)) {
				snprintf(a, sizeof(a), "%-2.2s", slot_text(e, b.arg));
			} else if (v && g->bank_until > e->now) {
				int width = group_width(e, g, NULL, NULL);
				int bank = width ? (cur_offset(g) + width - 1) / width + 1 : 1;
				snprintf(a, sizeof(a), "%c%d", v->code[0] ? v->code[0] : ' ', bank % 10);
			} else if (v) {
				snprintf(a, sizeof(a), "%-2.2s", v->code);
			}
			w->assign[0] = mcu_7seg(a[0], false);
			w->assign[1] = mcu_7seg(a[1] ? a[1] : ' ', false);
		}
		if (u->prof->timecode) {
			struct sf_bind b;
			if (binding_for(e, g, u->prof, "timecode", &b) && (b.kind == K_PARAM || b.kind == K_MOMENTARY)) {
				const char *t = slot_text(e, b.arg);
				/* digits right-aligned; '.' marks the previous digit */
				uint8_t seg[MCU_TC_DIGITS];
				memset(seg, mcu_7seg(' ', false), sizeof(seg));
				int d = 0;
				for (int k = (int)strlen(t) - 1; k >= 0 && d < MCU_TC_DIGITS; k--) {
					bool dot = false;
					if ((t[k] == '.' || t[k] == ':') && k > 0) {
						dot = true;
						k--;
					}
					seg[d++] = mcu_7seg(t[k], dot);
				}
				memcpy(w->tc, seg, sizeof(seg));
			}
		}
	}
	/* a message across the displays */
	if (u->text_on) {
		if (u->text_until && e->now >= u->text_until) {
			u->text_on = false;
		} else {
			char line[57];
			snprintf(line, sizeof(line), "%-56.56s", u->text[0]);
			memcpy(w->lcd, line, 56);
			snprintf(line, sizeof(line), "%-56.56s", u->text[1]);
			memcpy(w->lcd + 56, line, 56);
		}
	}
}

static bool take(struct unit *u, size_t n)
{
	return u->tokens >= (double)n;
}

static void flush(struct sf_engine *e, struct unit *u)
{
	const struct sf_profile *pr = u->prof;
	struct dev *w = &u->want, *s = &u->sent;
	uint8_t m[160];
	uint64_t now = e->now;
	int max_msgs = pr->send_delay_us ? (int)(5000 / pr->send_delay_us) + 1 : 1 << 30;
	int sent = 0;
#define ROOM(n) (take(u, (n)) && sent < max_msgs)
	/* 1. motor faders (50 Hz each) */
	int nf = pr->master_fader ? 9 : pr->strips;
	for (int i = 0; i < nf; i++) {
		int fi = i < pr->strips ? i : 8;
		if (pr->motor && w->fader[fi] != s->fader[fi] && now - u->fader_ns[fi] >= 20 * MS) {
			if (!ROOM(3)) return;
			send(e, u, m, mcu_fader(fi, w->fader[fi], m));
			s->fader[fi] = w->fader[fi];
			u->fader_ns[fi] = now;
			sent++;
		}
	}
	/* 2. LEDs (immediate) */
	for (size_t k = 0; k < mcu_n_controls; k++) {
		int id = mcu_controls[k].id;
		if (id >= MCU_TOUCH && id <= MCU_TOUCH_MASTER)
			continue;
		if (mcu_is_strip_button(id) && (id % 8) >= pr->strips)
			continue;
		if (!pr->master && !mcu_is_strip_button(id))
			continue;
		if (w->led[id] != s->led[id]) {
			if (!ROOM(3)) return;
			send(e, u, m, mcu_led(id, w->led[id], m));
			s->led[id] = w->led[id];
			sent++;
		}
	}
	/* 3. rings (30 Hz each) */
	for (int i = 0; i < pr->strips; i++)
		if (w->ring[i] != s->ring[i] && now - u->ring_ns[i] >= 33 * MS) {
			if (!ROOM(3)) return;
			send(e, u, m, mcu_ring(i, w->ring[i], m));
			s->ring[i] = w->ring[i];
			u->ring_ns[i] = now;
			sent++;
		}
	/* 4. meters (15 Hz; again every 100 ms while there is signal: the device decays them) */
	if (pr->meters)
		for (int i = 0; i < pr->strips; i++) {
			int lvl = w->meter[i] & 0xff;
			bool clip = w->meter[i] & 0x100;
			bool due = lvl != s->meter[i] || (lvl > 0 && now - u->meter_ns[i] >= 100 * MS);
			if (due && now - u->meter_ns[i] >= 66 * MS) {
				if (!ROOM(2)) return;
				send(e, u, m, mcu_meter(i, lvl, m));
				s->meter[i] = lvl;
				u->meter_ns[i] = now;
				sent++;
			}
			if (clip) {
				u->clip_until[i] = now + 2000 * MS;
				if (!u->clip_on[i] && ROOM(2)) {
					send(e, u, m, mcu_meter(i, 0x0e, m));
					u->clip_on[i] = true;
					sent++;
				}
			} else if (u->clip_on[i] && now > u->clip_until[i] && ROOM(2)) {
				send(e, u, m, mcu_meter(i, 0x0f, m));
				u->clip_on[i] = false;
				sent++;
			}
		}
	/* 5. text: changed cells only, 20 Hz per line */
	if (pr->lcd)
		for (int line = 0; line < 2; line++) {
			int a = -1, b = -1;
			for (int c = 0; c < 56; c++)
				if (w->lcd[line * 56 + c] != s->lcd[line * 56 + c]) {
					if (a < 0) a = c;
					b = c;
				}
			if (a < 0 || now - u->line_ns[line] < 50 * MS)
				continue;
			size_t len = (size_t)(b - a + 1);
			if (!ROOM(len + 8)) return;
			send(e, u, m, mcu_lcd(pr->model, line * 56 + a, w->lcd + line * 56 + a, len, m, sizeof(m)));
			memcpy(s->lcd + line * 56 + a, w->lcd + line * 56 + a, len);
			u->line_ns[line] = now;
			sent++;
		}
	/* 6. colours */
	if (pr->colour && memcmp(w->colour, s->colour, 8)) {
		if (!ROOM(15)) return;
		send(e, u, m, mcu_colours(pr->model, w->colour, m, sizeof(m)));
		memcpy(s->colour, w->colour, 8);
		sent++;
	}
	/* 7. 7-segment displays */
	if (pr->master && pr->assignment)
		for (int k = 0; k < 2; k++)
			if (w->assign[k] != s->assign[k]) {
				if (!ROOM(3)) return;
				int cc = (k == 0) != pr->assignment_swapped ? 0x4b : 0x4a;
				send(e, u, m, mcu_digit(cc, w->assign[k], m));
				s->assign[k] = w->assign[k];
				sent++;
			}
	if (pr->master && pr->timecode)
		for (int d = 0; d < MCU_TC_DIGITS; d++)
			if (w->tc[d] != s->tc[d]) {
				if (!ROOM(3)) return;
				send(e, u, m, mcu_digit(0x40 + d, w->tc[d], m));
				s->tc[d] = w->tc[d];
				sent++;
			}
#undef ROOM
}

void sf_tick(struct sf_engine *e, uint64_t now)
{
	e->now = now;
	for (int i = 0; i < SF_MAX_UNITS; i++) {
		struct unit *u = &e->units[i];
		if (u->state == SF_HANDSHAKE) {
			if (now - u->hs_ns >= 500 * MS) {
				if (u->hs_tries >= 3) {
					elog(e, "%s: no answer to the device query: going on without a serial", u->port);
					finalize(e, u, NULL);
				} else {
					uint8_t m[16];
					u->tokens = 64;
					send(e, u, m, mcu_device_query(u->prof->model, m, sizeof(m)));
					u->hs_ns = now;
					u->hs_tries++;
				}
			}
			continue;
		}
		if (u->state != SF_ONLINE)
			continue;
		struct group *g = group_of(e, u);
		const char *role = g ? g->role : "all";
		/* gesture timers */
		for (int k = 0; k < 9; k++) {
			if (u->motion[k] && now - u->move_ns[k] > 500 * MS) {
				u->motion[k] = false;
				if (u->fader_gest[k][0]) {
					e->host.gesture(e->data, u->fader_gest[k], false, role, u->id);
					u->fader_gest[k][0] = 0;
				}
				u->sent.fader[k] = -1;
			}
			if (u->touched[k] && u->fader_gest[k][0] && now - u->move_ns[k] > 60000 * MS) {
				elog(e, "%s: fader %d touched for 60 s without moving: its gesture is ended (touch sensor?)", u->id, k + 1);
				e->host.gesture(e->data, u->fader_gest[k], false, role, u->id);
				u->fader_gest[k][0] = 0;
				u->stale_touch[k] = true;
			}
		}
		for (int k = 0; k < 8; k++)
			if (u->pot_gest[k][0] && now - u->pot_ns[k] > 300 * MS) {
				e->host.gesture(e->data, u->pot_gest[k], false, role, u->id);
				u->pot_gest[k][0] = 0;
			}
		/* pacing budget */
		double dt = (double)(now - u->last_ns) / 1e9;
		u->last_ns = now;
		double burst = u->prof->bytes_per_second / 20.0;
		if (burst < 600)
			burst = 600;
		u->tokens += dt * u->prof->bytes_per_second;
		if (u->tokens > burst)
			u->tokens = burst;
		compute_want(e, u);
		flush(e, u);
	}
}

/* ============================================================ Surface1 */
static struct group *need_group(struct sf_engine *e, const char *id, char *err, size_t en)
{
	struct group *g = find_group(e, id);
	if (!g)
		snprintf(err, en, "no group '%s'", id);
	return g;
}

int sf_set_group(struct sf_engine *e, const char *id, const char *const *units, int n, const char *role,
		 const char *lock_view, int lock_offset, bool set_lock, char *err, size_t en)
{
	if (!id || !*id || strlen(id) >= 64 || !strcmp(id, "*")) {
		snprintf(err, en, "a group id is 1-63 characters (not *)");
		return -1;
	}
	if (n > SF_GROUP_UNITS) {
		snprintf(err, en, "at most %d units in a group", SF_GROUP_UNITS);
		return -1;
	}
	for (int i = 0; i < n; i++)
		if (!units[i] || !*units[i] || strlen(units[i]) >= 160) {
			snprintf(err, en, "unit ids are 1-159 characters");
			return -1;
		}
	struct group *g = find_group(e, id);
	if (!g) {
		for (int i = 0; i < SF_MAX_GROUPS && !g; i++)
			if (!e->groups[i].used)
				g = &e->groups[i];
		if (!g) {
			snprintf(err, en, "at most %d groups", SF_MAX_GROUPS);
			return -1;
		}
		memset(g, 0, sizeof(*g));
		g->used = true;
		snprintf(g->id, sizeof(g->id), "%s", id);
		snprintf(g->role, sizeof(g->role), "all");
	}
	if (n >= 0 && units) {
		/* the units leave their other groups */
		for (int k = 0; k < SF_MAX_GROUPS; k++) {
			struct group *o = &e->groups[k];
			if (!o->used || o == g)
				continue;
			for (int a = 0; a < o->n_units;) {
				bool hit = false;
				for (int i = 0; i < n && !hit; i++)
					hit = !strcmp(o->units[a], units[i]);
				if (hit) {
					memmove(o->units[a], o->units[a + 1], (size_t)(o->n_units - a - 1) * sizeof(o->units[0]));
					o->n_units--;
				} else {
					a++;
				}
			}
			if (!o->n_units && !o->views)
				o->used = false; /* an automatic group left empty goes */
		}
		g->n_units = n;
		for (int i = 0; i < n; i++)
			snprintf(g->units[i], sizeof(g->units[i]), "%s", units[i]);
	}
	if (role && *role)
		snprintf(g->role, sizeof(g->role), "%s", role);
	if (set_lock) {
		snprintf(g->lock_view, sizeof(g->lock_view), "%s", lock_view ? lock_view : "");
		g->lock_offset = lock_offset;
	}
	ensure_view(e, g);
	save_state(e);
	return 0;
}

int sf_remove_group(struct sf_engine *e, const char *id, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	free_views(g->views);
	memset(g, 0, sizeof(*g));
	/* its online units are grouped again automatically */
	for (int i = 0; i < SF_MAX_UNITS; i++) {
		struct unit *u = &e->units[i];
		if (u->state == SF_ONLINE && !group_of(e, u)) {
			u->state = SF_HANDSHAKE;
			finalize(e, u, u->serial[0] ? u->serial : NULL);
		}
	}
	save_state(e);
	return 0;
}

int sf_set_views(struct sf_engine *e, const char *id, const char *json, char *err, size_t en)
{
	struct group *g = NULL;
	if (strcmp(id, "*") && !(g = need_group(e, id, err, en)))
		return -2;
	struct sf_views *vs = parse_views(json, err, en);
	if (!vs)
		return -1;
	if (g) {
		free_views(g->views);
		g->views = vs;
		ensure_view(e, g);
	} else {
		free_views(e->defaults);
		e->defaults = vs;
		for (int i = 0; i < SF_MAX_GROUPS; i++)
			if (e->groups[i].used && !e->groups[i].views)
				ensure_view(e, &e->groups[i]);
	}
	return 0;
}

int sf_push_view(struct sf_engine *e, const char *id, const char *view, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	if (!find_view(gviews(e, g), view)) {
		snprintf(err, en, "no view '%s'", view);
		return -2;
	}
	if (g->depth >= SF_STACK) {
		snprintf(err, en, "the layer stack holds %d views", SF_STACK);
		return -3;
	}
	snprintf(g->stack[g->depth++], sizeof(g->stack[0]), "%s", view);
	clamp_offset(e, g);
	return 0;
}

int sf_pop_view(struct sf_engine *e, const char *id, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	if (g->depth <= 1) {
		snprintf(err, en, "only one view is open");
		return -3;
	}
	g->depth--;
	clamp_offset(e, g);
	return 0;
}

int sf_show_view(struct sf_engine *e, const char *id, const char *view, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	if (!find_view(gviews(e, g), view)) {
		snprintf(err, en, "no view '%s'", view);
		return -2;
	}
	snprintf(g->stack[0], sizeof(g->stack[0]), "%s", view);
	g->depth = 1;
	clamp_offset(e, g);
	return 0;
}

int sf_set_window(struct sf_engine *e, const char *id, int offset, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	struct sf_view *v = cur_view(e, g);
	if (!v) {
		snprintf(err, en, "the group shows no view");
		return -3;
	}
	*offset_ref(g, v->id) = offset;
	clamp_offset(e, g);
	return 0;
}

int sf_set_pot_mode(struct sf_engine *e, const char *id, const char *mode, int page, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	struct sf_mode *m = find_mode(gviews(e, g), mode);
	if (!m) {
		snprintf(err, en, "no pot mode '%s'", mode);
		return -2;
	}
	if (page < 0 || (m->strip && page >= m->n_pages) || (!m->strip && page)) {
		snprintf(err, en, "pot mode %s has %d page(s)", mode, m->strip ? m->n_pages : 1);
		return -1;
	}
	snprintf(g->mode, sizeof(g->mode), "%s", mode);
	g->page = page;
	return 0;
}

int sf_set_selected(struct sf_engine *e, const char *id, const char *slot, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	snprintf(g->selected, sizeof(g->selected), "%s", slot ? slot : "");
	return 0;
}

int sf_set_flip(struct sf_engine *e, const char *id, bool on, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	g->flip = on;
	return 0;
}

int sf_set_binding(struct sf_engine *e, const char *id, const char *control, const char *binding, char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	const char *base = strrchr(control, '+') ? strrchr(control, '+') + 1 : control;
	if (strlen(control) >= 48 || (mcu_control_id(base) < 0 && strcmp(base, "jog") && strcmp(base, "timecode") &&
				      strcmp(base, "assignment") && strcmp(base, "pedal"))) {
		snprintf(err, en, "'%s' is not a control name (Surface1.md lists them)", control);
		return -1;
	}
	struct sf_bind b;
	if (parse_binding(binding, &b, err, en) < 0)
		return -1;
	snprintf(b.control, sizeof(b.control), "%s", control);
	for (int i = 0; i < g->n_binds; i++)
		if (!strcmp(g->binds[i].control, control)) {
			g->binds[i] = b;
			return 0;
		}
	if (g->n_binds >= SF_MAX_BINDS) {
		snprintf(err, en, "too many bindings");
		return -1;
	}
	g->binds[g->n_binds++] = b;
	return 0;
}

int sf_text(struct sf_engine *e, const char *id, const char *unit, const char *text, uint32_t ms, uint64_t now,
	    char *err, size_t en)
{
	struct group *g = need_group(e, id, err, en);
	if (!g)
		return -2;
	int hits = 0;
	for (int k = 0; k < g->n_units; k++) {
		struct unit *u = unit_online(e, g->units[k]);
		if (!u || (unit && *unit && strcmp(unit, u->id)))
			continue;
		set_text(u, text, ms ? now + (uint64_t)ms * MS : 0);
		if (!text[0])
			u->text_on = false;
		hits++;
	}
	if (unit && *unit && !hits) {
		snprintf(err, en, "no online unit '%s' in %s", unit, id);
		return -2;
	}
	return 0;
}

/* ============================================================ listings */
static json_object *unit_json(struct sf_engine *e, struct unit *u)
{
	json_object *o = json_object_new_object();
	static const char *st[] = { "free", "handshake", "online", "gone" };
	struct group *g = group_of(e, u);
	int pos = -1;
	if (g)
		for (int k = 0; k < g->n_units; k++)
			if (!strcmp(g->units[k], u->id))
				pos = k;
	json_object_object_add(o, "id", json_object_new_string(u->id));
	json_object_object_add(o, "profile", json_object_new_string(u->prof ? u->prof->id : ""));
	json_object_object_add(o, "name", json_object_new_string(u->prof ? u->prof->name : ""));
	json_object_object_add(o, "port", json_object_new_string(u->port));
	json_object_object_add(o, "serial", json_object_new_string(u->serial));
	json_object_object_add(o, "group", json_object_new_string(g ? g->id : ""));
	json_object_object_add(o, "position", json_object_new_int(pos));
	json_object_object_add(o, "state", json_object_new_string(st[u->state]));
	json_object_object_add(o, "strips", json_object_new_int(u->prof ? u->prof->strips : 0));
	json_object_object_add(o, "master", json_object_new_boolean(u->prof && u->prof->master));
	int t = 0;
	for (int i = 0; i < 9; i++)
		t += u->touched[i];
	json_object_object_add(o, "touched", json_object_new_int(t));
	return o;
}

static char *dump(json_object *o)
{
	char *s = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return s;
}

char *sf_units_json(struct sf_engine *e)
{
	json_object *a = json_object_new_array();
	for (int i = 0; i < SF_MAX_UNITS; i++)
		if (e->units[i].state != SF_FREE && e->units[i].id[0])
			json_object_array_add(a, unit_json(e, &e->units[i]));
	/* units remembered only by their group */
	for (int k = 0; k < SF_MAX_GROUPS; k++) {
		struct group *g = &e->groups[k];
		if (!g->used)
			continue;
		for (int i = 0; i < g->n_units; i++) {
			bool known = false;
			for (int u = 0; u < SF_MAX_UNITS && !known; u++)
				known = e->units[u].state != SF_FREE && !strcmp(e->units[u].id, g->units[i]);
			if (known)
				continue;
			json_object *o = json_object_new_object();
			json_object_object_add(o, "id", json_object_new_string(g->units[i]));
			json_object_object_add(o, "group", json_object_new_string(g->id));
			json_object_object_add(o, "position", json_object_new_int(i));
			json_object_object_add(o, "state", json_object_new_string("gone"));
			json_object_array_add(a, o);
		}
	}
	return dump(a);
}

static json_object *group_json(struct sf_engine *e, struct group *g)
{
	json_object *o = json_object_new_object(), *u = json_object_new_array(), *st = json_object_new_array();
	json_object_object_add(o, "id", json_object_new_string(g->id));
	for (int i = 0; i < g->n_units; i++)
		json_object_array_add(u, json_object_new_string(g->units[i]));
	json_object_object_add(o, "units", u);
	json_object_object_add(o, "role", json_object_new_string(g->role));
	const char *v = cur_view_id(g);
	json_object_object_add(o, "view", json_object_new_string(v ? v : ""));
	for (int i = 0; i < g->depth; i++)
		json_object_array_add(st, json_object_new_string(g->stack[i]));
	json_object_object_add(o, "stack", st);
	json_object_object_add(o, "offset", json_object_new_int(cur_offset(g)));
	json_object_object_add(o, "width", json_object_new_int(group_width(e, g, NULL, NULL)));
	struct sf_mode *m = cur_mode(e, g);
	json_object_object_add(o, "pot-mode", json_object_new_string(m ? m->id : ""));
	json_object_object_add(o, "page", json_object_new_int(g->page));
	json_object_object_add(o, "flip", json_object_new_boolean(g->flip));
	json_object_object_add(o, "selected", json_object_new_string(g->selected));
	json_object_object_add(o, "locked", json_object_new_boolean(g->lock_view[0] != 0));
	return o;
}

char *sf_groups_json(struct sf_engine *e)
{
	json_object *a = json_object_new_array();
	for (int i = 0; i < SF_MAX_GROUPS; i++)
		if (e->groups[i].used)
			json_object_array_add(a, group_json(e, &e->groups[i]));
	return dump(a);
}

char *sf_state_json(struct sf_engine *e)
{
	json_object *o = json_object_new_object(), *a = json_object_new_array();
	json_object_object_add(o, "schema", json_object_new_string("backline.surfaces/1"));
	for (int i = 0; i < SF_MAX_GROUPS; i++) {
		struct group *g = &e->groups[i];
		if (!g->used)
			continue;
		json_object *x = json_object_new_object(), *u = json_object_new_array(), *offs = json_object_new_object();
		json_object_object_add(x, "id", json_object_new_string(g->id));
		for (int k = 0; k < g->n_units; k++)
			json_object_array_add(u, json_object_new_string(g->units[k]));
		json_object_object_add(x, "units", u);
		json_object_object_add(x, "role", json_object_new_string(g->role));
		if (g->lock_view[0]) {
			json_object_object_add(x, "lock-view", json_object_new_string(g->lock_view));
			json_object_object_add(x, "lock-offset", json_object_new_int(g->lock_offset));
		}
		for (int k = 0; k < g->n_offs; k++)
			json_object_object_add(offs, g->offs[k].view, json_object_new_int(g->offs[k].offset));
		json_object_object_add(x, "offsets", offs);
		json_object_array_add(a, x);
	}
	json_object_object_add(o, "groups", a);
	json_object_object_add(o, "next-group", json_object_new_int(e->next_group));
	char *s = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return s;
}

int sf_state_load(struct sf_engine *e, const char *js, char *err, size_t en)
{
	json_object *o = json_tokener_parse(js), *a, *v;
	if (!o || !json_object_object_get_ex(o, "groups", &a) || !json_object_is_type(a, json_type_array)) {
		json_object_put(o);
		snprintf(err, en, "not a backline.surfaces/1 file");
		return -1;
	}
	if (json_object_object_get_ex(o, "next-group", &v))
		e->next_group = json_object_get_int(v);
	for (size_t i = 0; i < json_object_array_length(a) && i < SF_MAX_GROUPS; i++) {
		json_object *x = json_object_array_get_idx(a, i), *u;
		if (!json_object_object_get_ex(x, "id", &v))
			continue;
		const char *ids[SF_GROUP_UNITS];
		int n = 0;
		if (json_object_object_get_ex(x, "units", &u))
			for (size_t k = 0; k < json_object_array_length(u) && n < SF_GROUP_UNITS; k++)
				ids[n++] = json_object_get_string(json_object_array_get_idx(u, k));
		json_object *r = NULL, *lv = NULL, *lo = NULL, *offs = NULL;
		json_object_object_get_ex(x, "role", &r);
		json_object_object_get_ex(x, "lock-view", &lv);
		json_object_object_get_ex(x, "lock-offset", &lo);
		char e2[128];
		if (sf_set_group(e, json_object_get_string(v), ids, n, r ? json_object_get_string(r) : NULL,
				 lv ? json_object_get_string(lv) : NULL, lo ? json_object_get_int(lo) : 0, lv != NULL, e2, sizeof(e2)) < 0)
			continue;
		struct group *g = find_group(e, json_object_get_string(v));
		if (g && json_object_object_get_ex(x, "offsets", &offs)) {
			json_object_object_foreach(offs, view, off) {
				if (strlen(view) < 64)
					*offset_ref(g, view) = json_object_get_int(off);
			}
		}
	}
	json_object_put(o);
	return 0;
}

/* ============================================================ inspection */
int sf_units_online(struct sf_engine *e)
{
	int n = 0;
	for (int i = 0; i < SF_MAX_UNITS; i++)
		n += e->units[i].state == SF_ONLINE;
	return n;
}

int sf_group_ids(struct sf_engine *e, const char **out, int max)
{
	int n = 0;
	for (int i = 0; i < SF_MAX_GROUPS && n < max; i++)
		if (e->groups[i].used)
			out[n++] = e->groups[i].id;
	return n;
}

enum sf_unit_state sf_unit_state(struct sf_engine *e, int i) { return i >= 0 && i < SF_MAX_UNITS ? e->units[i].state : SF_FREE; }
const char *sf_unit_id(struct sf_engine *e, int i) { return i >= 0 && i < SF_MAX_UNITS ? e->units[i].id : ""; }

int sf_unit_by_id(struct sf_engine *e, const char *id)
{
	for (int i = 0; i < SF_MAX_UNITS; i++)
		if (e->units[i].state != SF_FREE && !strcmp(e->units[i].id, id))
			return i;
	return -1;
}

int sf_unit_touched(struct sf_engine *e, int i)
{
	int t = 0;
	for (int k = 0; k < 9; k++)
		t += e->units[i].touched[k];
	return t;
}

int sf_group_width(struct sf_engine *e, const char *id)
{
	struct group *g = find_group(e, id);
	return g ? group_width(e, g, NULL, NULL) : -1;
}

int sf_group_offset(struct sf_engine *e, const char *id)
{
	struct group *g = find_group(e, id);
	return g ? cur_offset(g) : -1;
}

const char *sf_group_view(struct sf_engine *e, const char *id)
{
	struct group *g = find_group(e, id);
	const char *v = g ? cur_view_id(g) : NULL;
	return v ? v : "";
}

const char *sf_unit_group(struct sf_engine *e, int i)
{
	struct group *g = i >= 0 && i < SF_MAX_UNITS ? group_of(e, &e->units[i]) : NULL;
	return g ? g->id : "";
}

uint64_t sf_messages_in(struct sf_engine *e) { return e->msgs_in; }
uint64_t sf_messages_out(struct sf_engine *e) { return e->msgs_out; }
