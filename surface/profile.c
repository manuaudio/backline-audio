/* Surface profiles (profile.h).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "mcu.h"

#define SCHEMA "backline.surface/1"

void sf_profile_free(struct sf_profile *p)
{
	for (int i = 0; i < p->n_match; i++)
		regfree(&p->match[i]);
	for (int i = 0; i < p->n_exclude; i++)
		regfree(&p->exclude[i]);
	p->n_match = p->n_exclude = 0;
}

static int regexes(json_object *a, char src[][128], regex_t *re, int *n, char *err, size_t en)
{
	if (!json_object_is_type(a, json_type_array)) {
		snprintf(err, en, "match lists are arrays of regular expressions");
		return -1;
	}
	for (size_t i = 0; i < json_object_array_length(a); i++) {
		const char *s = json_object_get_string(json_object_array_get_idx(a, i));
		if (*n >= SF_MAX_MATCH || !s || strlen(s) >= 128) {
			snprintf(err, en, "at most %d regular expressions of < 128 characters", SF_MAX_MATCH);
			return -1;
		}
		if (regcomp(&re[*n], s, REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
			snprintf(err, en, "bad regular expression '%s'", s);
			return -1;
		}
		snprintf(src[*n], 128, "%s", s);
		(*n)++;
	}
	return 0;
}

static bool jbool(json_object *o, const char *k, bool def)
{
	json_object *v;
	return json_object_object_get_ex(o, k, &v) ? json_object_get_boolean(v) : def;
}

static int parse_obj(json_object *o, const char *origin, struct sf_profile *p, char *err, size_t en)
{
	memset(p, 0, sizeof(*p));
	snprintf(p->file, sizeof(p->file), "%s", origin ? origin : "");
	json_object *v;
	if (!json_object_is_type(o, json_type_object) || !json_object_object_get_ex(o, "schema", &v) ||
	    strcmp(json_object_get_string(v), SCHEMA)) {
		snprintf(err, en, "not a %s file", SCHEMA);
		return -1;
	}
	static const char *known[] = { "schema", "id", "name", "driver", "unit", "strips", "master-fader", "model", "match",
				       "handshake", "display", "meters", "touch-sense", "motor", "bytes-per-second", "quirks",
				       "labels", "bindings", "comment", "verified", NULL };
	json_object_object_foreach(o, key, val) {
		(void)val;
		bool ok = false;
		for (int i = 0; known[i] && !ok; i++)
			ok = !strcmp(known[i], key);
		if (!ok) {
			snprintf(err, en, "unknown key '%s'", key);
			return -1;
		}
	}
	if (!json_object_object_get_ex(o, "id", &v) || strlen(json_object_get_string(v)) >= sizeof(p->id) ||
	    !json_object_get_string(v)[0]) {
		snprintf(err, en, "a profile needs an id");
		return -1;
	}
	snprintf(p->id, sizeof(p->id), "%s", json_object_get_string(v));
	snprintf(p->name, sizeof(p->name), "%s",
		 json_object_object_get_ex(o, "name", &v) ? json_object_get_string(v) : p->id);
	snprintf(p->driver, sizeof(p->driver), "%s",
		 json_object_object_get_ex(o, "driver", &v) ? json_object_get_string(v) : "mcu");
	if (strcmp(p->driver, "mcu")) {
		snprintf(err, en, "driver '%s' is not built (mcu)", p->driver);
		return -1;
	}
	const char *unit = json_object_object_get_ex(o, "unit", &v) ? json_object_get_string(v) : "master";
	if (strcmp(unit, "master") && strcmp(unit, "extender")) {
		snprintf(err, en, "unit is master or extender");
		return -1;
	}
	p->master = !strcmp(unit, "master");
	p->strips = json_object_object_get_ex(o, "strips", &v) ? json_object_get_int(v) : 8;
	if (p->strips < 1 || p->strips > 8) {
		snprintf(err, en, "strips is 1 to 8 per unit (a wider device is several units)");
		return -1;
	}
	p->master_fader = jbool(o, "master-fader", p->master);
	p->model = (uint8_t)(json_object_object_get_ex(o, "model", &v) ? json_object_get_int(v)
							       : (p->master ? MCU_MODEL_MCU : MCU_MODEL_XT));
	if (json_object_object_get_ex(o, "match", &v)) {
		json_object *a;
		if (json_object_object_get_ex(v, "port", &a) && regexes(a, p->match_src, p->match, &p->n_match, err, en) < 0)
			goto fail;
		if (json_object_object_get_ex(v, "exclude", &a) &&
		    regexes(a, p->exclude_src, p->exclude, &p->n_exclude, err, en) < 0)
			goto fail;
	}
	const char *hs = json_object_object_get_ex(o, "handshake", &v) ? json_object_get_string(v) : "mackie";
	if (!strcmp(hs, "mackie")) p->handshake = SF_HS_MACKIE;
	else if (!strcmp(hs, "logic")) p->handshake = SF_HS_LOGIC;
	else if (!strcmp(hs, "none")) p->handshake = SF_HS_NONE;
	else {
		snprintf(err, en, "handshake is mackie, logic or none");
		goto fail;
	}
	p->lcd = true;
	if (json_object_object_get_ex(o, "display", &v)) {
		json_object *d;
		p->lcd = !json_object_object_get_ex(v, "lcd", &d) || strcmp(json_object_get_string(d), "none");
		p->colour = json_object_object_get_ex(v, "colour", &d) && !strcmp(json_object_get_string(d), "xtouch");
		p->assignment = jbool(v, "assignment", false);
		p->timecode = jbool(v, "timecode", false);
	}
	p->meters = !json_object_object_get_ex(o, "meters", &v) || strcmp(json_object_get_string(v), "none");
	p->touch_sense = jbool(o, "touch-sense", true);
	p->motor = jbool(o, "motor", true);
	p->bytes_per_second = json_object_object_get_ex(o, "bytes-per-second", &v) ? (uint32_t)json_object_get_int(v) : 12000;
	if (p->bytes_per_second < 500)
		p->bytes_per_second = 500;
	if (json_object_object_get_ex(o, "quirks", &v)) {
		json_object *q;
		p->no_flash = jbool(v, "no-flash", false);
		p->assignment_swapped = jbool(v, "assignment-swapped", false);
		if (json_object_object_get_ex(v, "send-delay-us", &q))
			p->send_delay_us = (uint32_t)json_object_get_int(q);
	}
	if (json_object_object_get_ex(o, "labels", &v)) {
		json_object_object_foreach(v, name, lab) {
			int id = mcu_control_id(name);
			if (id < 0) {
				snprintf(err, en, "labels: unknown control '%s'", name);
				goto fail;
			}
			snprintf(p->labels[id], sizeof(p->labels[id]), "%s", json_object_get_string(lab));
		}
	}
	if (json_object_object_get_ex(o, "bindings", &v)) {
		json_object_object_foreach(v, name, b) {
			const char *base = strrchr(name, '+') ? strrchr(name, '+') + 1 : name;
			if (mcu_control_id(base) < 0 && strcmp(base, "jog") && strcmp(base, "pedal") &&
			    strcmp(base, "timecode") && strcmp(base, "assignment")) {
				snprintf(err, en, "bindings: unknown control '%s'", name);
				goto fail;
			}
			if (p->n_bindings >= SF_MAX_BINDINGS) {
				snprintf(err, en, "too many bindings");
				goto fail;
			}
			struct sf_binding_default *d = &p->bindings[p->n_bindings++];
			snprintf(d->control, sizeof(d->control), "%s", name);
			snprintf(d->binding, sizeof(d->binding), "%s", json_object_get_string(b));
		}
	}
	return 0;
fail:
	sf_profile_free(p);
	return -1;
}

int sf_profile_parse(const char *js, const char *origin, struct sf_profile *p, char *err, size_t n)
{
	json_object *o = json_tokener_parse(js);
	if (!o) {
		snprintf(err, n, "not JSON");
		return -1;
	}
	int r = parse_obj(o, origin, p, err, n);
	json_object_put(o);
	return r;
}

int sf_profile_load(const char *path, struct sf_profile *p, char *err, size_t n)
{
	json_object *o = json_object_from_file(path);
	if (!o) {
		snprintf(err, n, "not readable JSON");
		return -1;
	}
	int r = parse_obj(o, path, p, err, n);
	json_object_put(o);
	return r;
}

bool sf_profile_matches(const struct sf_profile *p, const char *port)
{
	bool hit = false;
	for (int i = 0; i < p->n_match && !hit; i++)
		hit = regexec(&p->match[i], port, 0, NULL, 0) == 0;
	for (int i = 0; i < p->n_exclude && hit; i++)
		if (regexec(&p->exclude[i], port, 0, NULL, 0) == 0)
			hit = false;
	return hit;
}

const char *sf_profile_binding(const struct sf_profile *p, const char *control)
{
	for (int i = 0; i < p->n_bindings; i++)
		if (!strcmp(p->bindings[i].control, control))
			return p->bindings[i].binding;
	return NULL;
}

void *sf_profile_to_json(const struct sf_profile *p)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "id", json_object_new_string(p->id));
	json_object_object_add(o, "name", json_object_new_string(p->name));
	json_object_object_add(o, "file", json_object_new_string(p->file));
	json_object_object_add(o, "driver", json_object_new_string(p->driver));
	json_object_object_add(o, "unit", json_object_new_string(p->master ? "master" : "extender"));
	json_object_object_add(o, "strips", json_object_new_int(p->strips));
	json_object_object_add(o, "master-fader", json_object_new_boolean(p->master_fader));
	json_object_object_add(o, "model", json_object_new_int(p->model));
	json_object *m = json_object_new_array();
	for (int i = 0; i < p->n_match; i++)
		json_object_array_add(m, json_object_new_string(p->match_src[i]));
	json_object_object_add(o, "match", m);
	json_object *labels = json_object_new_object();
	for (size_t i = 0; i < mcu_n_controls; i++) {
		const struct mcu_control *c = &mcu_controls[i];
		json_object_object_add(labels, c->name, json_object_new_string(p->labels[c->id][0] ? p->labels[c->id] : c->label));
	}
	json_object_object_add(o, "labels", labels);
	return o;
}
