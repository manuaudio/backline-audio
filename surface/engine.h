/* The surface engine (docs/api/Surface1.md): units, groups, views, the layer
 * stack, windows, pot modes, flip, feedback with paced output, touch and
 * gesture safety, hot-plug. Pure: no PipeWire, no D-Bus, no clock. The
 * daemon (surfaced.c) and the tests drive it through sf_host callbacks and
 * pass the time in.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_SURFACE_ENGINE_H
#define BACKLINE_SURFACE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "params.h"
#include "profile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SF_MAX_UNITS 16
#define SF_MAX_GROUPS 8
#define SF_GROUP_UNITS 8
#define SF_MAX_PROFILES 64

enum sf_unit_state { SF_FREE, SF_HANDSHAKE, SF_ONLINE, SF_GONE };

struct sf_host {
	/* bytes to a unit's device (one MIDI message) */
	void (*send)(void *data, int unit, const uint8_t *msg, size_t n);
	/* Params1.Set(path, norm, {norm: true, role, client}) */
	void (*set)(void *data, const char *path, double norm, const char *role, const char *client);
	void (*gesture)(void *data, const char *path, bool begin, const char *role, const char *client);
	void (*action)(void *data, const char *group, const char *name, bool down);
	void (*navigated)(void *data, const char *group, const char *view, int offset, const char *mode, int page);
	void (*unit_event)(void *data, const char *unit, bool added);
	/* a meter's level, from the root's meter table */
	bool (*meter)(void *data, const char *root, const char *id, float *db, bool *clip);
	/* groups changed: persist them */
	void (*state_changed)(void *data);
	void (*log)(void *data, const char *msg);
};

struct sf_engine;

struct sf_engine *sf_new(const struct sf_host *host, void *data, struct prm_store *params);
/* A new parameter cache (the daemon rebuilds it from Params1.List). */
void sf_set_params(struct sf_engine *e, struct prm_store *params);
void sf_free(struct sf_engine *e);

/* Profiles: takes ownership of p's contents (copied in). */
int sf_add_profile(struct sf_engine *e, const struct sf_profile *p);
int sf_load_profile_dir(struct sf_engine *e, const char *dir);  /* count loaded */
const struct sf_profile *sf_profile_for_port(struct sf_engine *e, const char *port);
const struct sf_profile *sf_profile_by_id(struct sf_engine *e, const char *id);
int sf_n_profiles(struct sf_engine *e);
const struct sf_profile *sf_profile_at(struct sf_engine *e, int i);

/* A device appeared on a port: returns the unit index (handshake starts). */
int sf_unit_attach(struct sf_engine *e, const struct sf_profile *p, const char *port, uint64_t now);
/* The device went away (unplugged): gestures end now, the window is dealt again. */
void sf_unit_detach(struct sf_engine *e, int unit, uint64_t now);
/* One complete MIDI message from a unit's device. */
void sf_unit_input(struct sf_engine *e, int unit, const uint8_t *msg, size_t n, uint64_t now);
/* Clean shutdown: zero the displays and LEDs, faders down, go offline. */
void sf_unit_goodbye(struct sf_engine *e, int unit);
/* Every few ms: handshake retries, timers, feedback within the pacing budget. */
void sf_tick(struct sf_engine *e, uint64_t now);

/* Surface1 (errors as a sentence in err, return -1; -2 = not found, -3 = state) */
int sf_set_group(struct sf_engine *e, const char *group, const char *const *units, int n, const char *role,
		 const char *lock_view, int lock_offset, bool set_lock, char *err, size_t en);
int sf_remove_group(struct sf_engine *e, const char *group, char *err, size_t en);
int sf_set_views(struct sf_engine *e, const char *group, const char *json, char *err, size_t en);
int sf_push_view(struct sf_engine *e, const char *group, const char *view, char *err, size_t en);
int sf_pop_view(struct sf_engine *e, const char *group, char *err, size_t en);
int sf_show_view(struct sf_engine *e, const char *group, const char *view, char *err, size_t en);
int sf_set_window(struct sf_engine *e, const char *group, int offset, char *err, size_t en);
int sf_set_pot_mode(struct sf_engine *e, const char *group, const char *mode, int page, char *err, size_t en);
int sf_set_selected(struct sf_engine *e, const char *group, const char *slot, char *err, size_t en);
int sf_set_flip(struct sf_engine *e, const char *group, bool on, char *err, size_t en);
int sf_set_binding(struct sf_engine *e, const char *group, const char *control, const char *binding, char *err, size_t en);
int sf_text(struct sf_engine *e, const char *group, const char *unit, const char *text, uint32_t ms, uint64_t now,
	    char *err, size_t en);

/* Listings and state, as JSON strings (malloc'd) */
char *sf_units_json(struct sf_engine *e);
char *sf_groups_json(struct sf_engine *e);
char *sf_state_json(struct sf_engine *e);              /* what surfaces.json keeps */
int sf_state_load(struct sf_engine *e, const char *json, char *err, size_t en);
int sf_units_online(struct sf_engine *e);
int sf_group_ids(struct sf_engine *e, const char **out, int max);

/* Inspection (tests, the daemon's properties) */
enum sf_unit_state sf_unit_state(struct sf_engine *e, int unit);
const char *sf_unit_id(struct sf_engine *e, int unit);
int sf_unit_by_id(struct sf_engine *e, const char *id);
int sf_unit_touched(struct sf_engine *e, int unit);
int sf_group_width(struct sf_engine *e, const char *group);
int sf_group_offset(struct sf_engine *e, const char *group);
const char *sf_group_view(struct sf_engine *e, const char *group);
const char *sf_unit_group(struct sf_engine *e, int unit);
uint64_t sf_messages_in(struct sf_engine *e);
uint64_t sf_messages_out(struct sf_engine *e);

#ifdef __cplusplus
}
#endif
#endif
