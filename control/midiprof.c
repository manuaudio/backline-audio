/* MIDI learn profiles (midiprof.h). Pure: json-c, regex and files only.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "midiprof.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const kinds[] = { "fader", "knob", "encoder", "button", "pad", "key", "wheel", "other", NULL };
/* The keys a binding may carry besides "control": a Control1 mapping's
 * options (docs/api/Control1.md, AddMapping). */
static const char *const binding_keys[] = { "control", "target", "min", "max", "invert", "args", "encoding", "takeover",
					    "step", "accel", "bits", "feedback", "gesture", "role", NULL };

bool mp_name_valid(const char *s)
{
	if (!s || !*s || strlen(s) >= MP_NAME_MAX)
		return false;
	if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')))
		return false;
	return strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789._-") == strlen(s);
}

static bool in_list(const char *s, const char *const *l)
{
	for (; *l; l++)
		if (!strcmp(s, *l))
			return true;
	return false;
}

void mp_free(struct mp_profile *p)
{
	if (!p)
		return;
	if (p->re_ok)
		regfree(&p->re);
	free(p->controls);
	if (p->sources)
		json_object_put(p->sources);
	if (p->bindings)
		json_object_put(p->bindings);
	memset(p, 0, sizeof(*p));
}

static int set_match(struct mp_profile *p, const char *match, char *err, size_t n)
{
	if (!match || !*match) {
		snprintf(err, n, "match (a regular expression for the device's MIDI port) is required");
		return -1;
	}
	if (strlen(match) >= sizeof(p->match)) {
		snprintf(err, n, "match is too long");
		return -1;
	}
	regex_t re;
	int r = regcomp(&re, match, REG_EXTENDED | REG_ICASE | REG_NOSUB);
	if (r) {
		char e[160];
		regerror(r, &re, e, sizeof(e));
		snprintf(err, n, "match: bad regular expression: %s", e);
		return -1;
	}
	if (p->re_ok)
		regfree(&p->re);
	p->re = re;
	p->re_ok = true;
	snprintf(p->match, sizeof(p->match), "%s", match);
	return 0;
}

int mp_init(struct mp_profile *p, const char *id, const char *name, const char *match, char *err, size_t n)
{
	memset(p, 0, sizeof(*p));
	if (!mp_name_valid(id)) {
		snprintf(err, n, "'%s' is not a profile id (a-z, 0-9, '.', '_', '-'; at most 63)", id ? id : "");
		return -1;
	}
	snprintf(p->id, sizeof(p->id), "%s", id);
	snprintf(p->name, sizeof(p->name), "%s", name && *name ? name : id);
	p->autoload = true;
	p->controls = calloc(MP_MAX_CONTROLS, sizeof(*p->controls));
	p->bindings = json_object_new_array();
	if (!p->controls || set_match(p, match, err, n) < 0) {
		if (!p->controls)
			snprintf(err, n, "out of memory");
		mp_free(p);
		return -1;
	}
	return 0;
}

static bool channels_overlap(int a, int b) { return a == 0 || b == 0 || a == b; }

/* Two sources a device can't tell apart (or that share CCs). */
static bool sources_clash(const struct ctl_source *a, const struct ctl_source *b)
{
	if (!channels_overlap(a->channel, b->channel))
		return false;
	if (a->kind == b->kind)
		return a->number == b->number;
	const struct ctl_source *cc = NULL, *other = NULL;
	if (a->kind == CTL_SRC_CC) { cc = a; other = b; }
	else if (b->kind == CTL_SRC_CC) { cc = b; other = a; }
	if (!cc)
		return a->kind == CTL_SRC_CC14 && b->kind == CTL_SRC_CC14 && a->number == b->number;
	if (other->kind == CTL_SRC_CC14)
		return cc->number == other->number || cc->number == other->number + 32;
	if (other->kind == CTL_SRC_NRPN || other->kind == CTL_SRC_RPN)
		return cc->number == 6 || cc->number == 38 || (cc->number >= 96 && cc->number <= 101);
	return false;
}

static const char *jstr(json_object *o, const char *k)
{
	json_object *v;
	return json_object_object_get_ex(o, k, &v) && json_object_is_type(v, json_type_string) ? json_object_get_string(v)
											      : NULL;
}

static int copy_str(json_object *o, const char *k, char *out, size_t sz, char *err, size_t n)
{
	json_object *v;
	if (!json_object_object_get_ex(o, k, &v))
		return 0;
	if (!json_object_is_type(v, json_type_string)) {
		snprintf(err, n, "%s is a string", k);
		return -1;
	}
	if (strlen(json_object_get_string(v)) >= sz) {
		snprintf(err, n, "%s is too long", k);
		return -1;
	}
	snprintf(out, sz, "%s", json_object_get_string(v));
	return 0;
}

static int parse_control(json_object *o, struct mp_control *c, char *err, size_t n)
{
	memset(c, 0, sizeof(*c));
	c->bits = 14;
	if (!json_object_is_type(o, json_type_object)) {
		snprintf(err, n, "a control is a JSON object");
		return -1;
	}
	json_object_object_foreach(o, key, val) {
		if (!strcmp(key, "name") || !strcmp(key, "source") || !strcmp(key, "kind") || !strcmp(key, "label")) {
			if (!json_object_is_type(val, json_type_string)) {
				snprintf(err, n, "control %s is a string", key);
				return -1;
			}
		} else if (!strcmp(key, "encoding")) {
			c->has_encoding = true;
			if (!json_object_is_type(val, json_type_string) || ctl_parse_encoding(json_object_get_string(val), &c->enc) < 0) {
				snprintf(err, n, "control encoding is abs, rel-2c, rel-sm, rel-offset or rel-inc-dec");
				return -1;
			}
		} else if (!strcmp(key, "bits")) {
			c->has_bits = true;
			c->bits = json_object_get_int(val);
			if (!json_object_is_type(val, json_type_int) || (c->bits != 7 && c->bits != 14)) {
				snprintf(err, n, "control bits is 7 or 14");
				return -1;
			}
		} else if (!strcmp(key, "feedback")) {
			if (!json_object_is_type(val, json_type_boolean)) {
				snprintf(err, n, "control feedback is true or false");
				return -1;
			}
			c->has_feedback = true;
			c->feedback = json_object_get_boolean(val);
		} else {
			snprintf(err, n, "unknown control key '%s'", key);
			return -1;
		}
	}
	const char *name = jstr(o, "name"), *src = jstr(o, "source"), *kind = jstr(o, "kind"), *label = jstr(o, "label");
	if (!mp_name_valid(name)) {
		snprintf(err, n, "control name '%s' is not a name (a-z, 0-9, '.', '_', '-')", name ? name : "");
		return -1;
	}
	snprintf(c->name, sizeof(c->name), "%s", name);
	char e[300];
	if (!src || ctl_parse_source(src, &c->src, e, sizeof(e)) < 0) {
		snprintf(err, n, "control %s: %s", c->name, src ? e : "source is required");
		return -1;
	}
	if (c->src.kind == CTL_SRC_OSC) {
		snprintf(err, n, "control %s: a MIDI profile's sources are MIDI", c->name);
		return -1;
	}
	ctl_format_source(&c->src, c->source, sizeof(c->source));
	if (kind && !in_list(kind, kinds)) {
		snprintf(err, n, "control %s: kind is fader, knob, encoder, button, pad, key, wheel or other", c->name);
		return -1;
	}
	snprintf(c->kind, sizeof(c->kind), "%s", kind ? kind : "");
	if (label && strlen(label) >= sizeof(c->label)) {
		snprintf(err, n, "control %s: label is too long", c->name);
		return -1;
	}
	snprintf(c->label, sizeof(c->label), "%s", label ? label : "");
	bool nrpn = c->src.kind == CTL_SRC_NRPN || c->src.kind == CTL_SRC_RPN;
	if (c->has_encoding && c->enc != CTL_ENC_ABS && c->src.kind != CTL_SRC_CC && !nrpn) {
		snprintf(err, n, "control %s: a relative encoding is for midi:cc, midi:nrpn and midi:rpn", c->name);
		return -1;
	}
	if (c->has_encoding && c->enc == CTL_ENC_REL_INCDEC && !nrpn) {
		snprintf(err, n, "control %s: rel-inc-dec is for midi:nrpn and midi:rpn", c->name);
		return -1;
	}
	if (c->has_bits && !nrpn) {
		snprintf(err, n, "control %s: bits is for midi:nrpn and midi:rpn", c->name);
		return -1;
	}
	return 0;
}

static int check_binding(const struct mp_profile *p, json_object *b, size_t i, char *err, size_t n)
{
	if (!json_object_is_type(b, json_type_object)) {
		snprintf(err, n, "binding %zu is not a JSON object", i);
		return -1;
	}
	json_object_object_foreach(b, key, val) {
		(void)val;
		if (!in_list(key, binding_keys)) {
			snprintf(err, n, "binding %zu: unknown key '%s'%s", i, key,
				 !strcmp(key, "source") ? " (a binding names its control; the control has the source)" : "");
			return -1;
		}
	}
	const char *c = jstr(b, "control"), *t = jstr(b, "target");
	if (!c || !mp_find_control(p, c)) {
		snprintf(err, n, "binding %zu: control '%s' is not one of this profile's", i, c ? c : "");
		return -1;
	}
	struct ctl_target tg;
	char e[300];
	if (!t || ctl_parse_target(t, &tg, e, sizeof(e)) < 0) {
		snprintf(err, n, "binding %zu (%s): %s", i, c, t ? e : "target is required");
		return -1;
	}
	const char *enc = jstr(b, "encoding"), *to = jstr(b, "takeover");
	enum ctl_encoding ee;
	enum ctl_takeover_mode tt;
	if (enc && ctl_parse_encoding(enc, &ee) < 0) {
		snprintf(err, n, "binding %zu (%s): encoding is abs, rel-2c, rel-sm, rel-offset or rel-inc-dec", i, c);
		return -1;
	}
	if (to && ctl_parse_takeover(to, &tt) < 0) {
		snprintf(err, n, "binding %zu (%s): takeover is jump, pickup or scale", i, c);
		return -1;
	}
	return 0;
}

int mp_parse(json_object *o, const char *id, struct mp_profile *p, char *err, size_t n)
{
	memset(p, 0, sizeof(*p));
	if (!o || !json_object_is_type(o, json_type_object)) {
		snprintf(err, n, "a profile is a JSON object");
		return -1;
	}
	const char *schema = jstr(o, "schema");
	if (!schema || strcmp(schema, MP_SCHEMA)) {
		snprintf(err, n, "not a %s profile", MP_SCHEMA);
		return -1;
	}
	const char *oid = jstr(o, "id");
	if (oid)
		id = oid;
	const char *name = jstr(o, "name"), *match = jstr(o, "match");
	if (!name || !*name) {
		snprintf(err, n, "name is required");
		return -1;
	}
	if (mp_init(p, id, name, match, err, n) < 0)
		return -1;
	static const char *const top[] = { "schema", "id", "name", "vendor", "model", "mode", "match", "verified", "note",
					   "sources", "auto", "controls", "bindings", NULL };
	json_object_object_foreach(o, key, val) {
		(void)val;
		if (!in_list(key, top)) {
			snprintf(err, n, "unknown profile key '%s'", key);
			goto fail;
		}
	}
	if (copy_str(o, "vendor", p->vendor, sizeof(p->vendor), err, n) < 0 ||
	    copy_str(o, "model", p->model, sizeof(p->model), err, n) < 0 ||
	    copy_str(o, "mode", p->mode, sizeof(p->mode), err, n) < 0 ||
	    copy_str(o, "note", p->note, sizeof(p->note), err, n) < 0 ||
	    copy_str(o, "name", p->name, sizeof(p->name), err, n) < 0)
		goto fail;
	json_object *v;
	if (!json_object_object_get_ex(o, "verified", &v) || !json_object_is_type(v, json_type_boolean)) {
		snprintf(err, n, "verified (true or false: tried on the hardware) is required");
		goto fail;
	}
	p->verified = json_object_get_boolean(v);
	if (json_object_object_get_ex(o, "auto", &v)) {
		if (!json_object_is_type(v, json_type_boolean)) {
			snprintf(err, n, "auto is true or false");
			goto fail;
		}
		p->autoload = json_object_get_boolean(v);
	}
	if (json_object_object_get_ex(o, "sources", &v)) {
		if (!json_object_is_type(v, json_type_array)) {
			snprintf(err, n, "sources is an array of strings");
			goto fail;
		}
		for (size_t i = 0; i < json_object_array_length(v); i++)
			if (!json_object_is_type(json_object_array_get_idx(v, i), json_type_string)) {
				snprintf(err, n, "sources is an array of strings");
				goto fail;
			}
		p->sources = json_object_get(v);
	}
	if (!json_object_object_get_ex(o, "controls", &v) || !json_object_is_type(v, json_type_array)) {
		snprintf(err, n, "controls (an array) is required");
		goto fail;
	}
	if (json_object_array_length(v) > MP_MAX_CONTROLS) {
		snprintf(err, n, "too many controls (at most %d)", MP_MAX_CONTROLS);
		goto fail;
	}
	for (size_t i = 0; i < json_object_array_length(v); i++) {
		struct mp_control *c = &p->controls[p->n_controls];
		char e[400];
		if (parse_control(json_object_array_get_idx(v, i), c, e, sizeof(e)) < 0) {
			snprintf(err, n, "control %zu: %s", i, e);
			goto fail;
		}
		for (int k = 0; k < p->n_controls; k++) {
			if (!strcmp(p->controls[k].name, c->name)) {
				snprintf(err, n, "two controls are named %s", c->name);
				goto fail;
			}
			if (sources_clash(&p->controls[k].src, &c->src)) {
				snprintf(err, n, "controls %s (%s) and %s (%s) send the same message", p->controls[k].name,
					 p->controls[k].source, c->name, c->source);
				goto fail;
			}
		}
		p->n_controls++;
	}
	if (json_object_object_get_ex(o, "bindings", &v)) {
		if (!json_object_is_type(v, json_type_array)) {
			snprintf(err, n, "bindings is an array");
			goto fail;
		}
		for (size_t i = 0; i < json_object_array_length(v); i++)
			if (check_binding(p, json_object_array_get_idx(v, i), i, err, n) < 0)
				goto fail;
		json_object_put(p->bindings);
		p->bindings = json_object_get(v);
	}
	return 0;
fail:
	mp_free(p);
	return -1;
}

int mp_load_file(const char *path, struct mp_profile *p, char *err, size_t n)
{
	json_object *o = json_object_from_file(path);
	if (!o) {
		memset(p, 0, sizeof(*p));
		snprintf(err, n, "not readable JSON");
		return -1;
	}
	char stem[MP_NAME_MAX + 8];
	const char *b = strrchr(path, '/');
	b = b ? b + 1 : path;
	snprintf(stem, sizeof(stem), "%s", b);
	size_t sl = strlen(stem), xl = strlen(MP_FILE_SUFFIX);
	if (sl > xl && !strcmp(stem + sl - xl, MP_FILE_SUFFIX))
		stem[sl - xl] = 0;
	int r = mp_parse(o, stem, p, err, n);
	json_object_put(o);
	if (r == 0 && strcmp(p->id, stem)) {
		snprintf(err, n, "id '%s' is not the file's name (%s%s)", p->id, stem, MP_FILE_SUFFIX);
		mp_free(p);
		return -1;
	}
	if (r == 0)
		snprintf(p->path, sizeof(p->path), "%s", path);
	return r;
}

json_object *mp_to_json(const struct mp_profile *p)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "schema", json_object_new_string(MP_SCHEMA));
	json_object_object_add(o, "id", json_object_new_string(p->id));
	json_object_object_add(o, "name", json_object_new_string(p->name));
	if (p->vendor[0]) json_object_object_add(o, "vendor", json_object_new_string(p->vendor));
	if (p->model[0]) json_object_object_add(o, "model", json_object_new_string(p->model));
	if (p->mode[0]) json_object_object_add(o, "mode", json_object_new_string(p->mode));
	json_object_object_add(o, "match", json_object_new_string(p->match));
	json_object_object_add(o, "verified", json_object_new_boolean(p->verified));
	if (p->note[0]) json_object_object_add(o, "note", json_object_new_string(p->note));
	if (p->sources) json_object_object_add(o, "sources", json_object_get(p->sources));
	if (!p->autoload) json_object_object_add(o, "auto", json_object_new_boolean(0));
	json_object *cs = json_object_new_array();
	for (int i = 0; i < p->n_controls; i++) {
		const struct mp_control *c = &p->controls[i];
		json_object *co = json_object_new_object();
		json_object_object_add(co, "name", json_object_new_string(c->name));
		json_object_object_add(co, "source", json_object_new_string(c->source));
		if (c->kind[0]) json_object_object_add(co, "kind", json_object_new_string(c->kind));
		if (c->label[0]) json_object_object_add(co, "label", json_object_new_string(c->label));
		if (c->has_encoding) json_object_object_add(co, "encoding", json_object_new_string(ctl_encoding_name(c->enc)));
		if (c->has_bits) json_object_object_add(co, "bits", json_object_new_int(c->bits));
		if (c->has_feedback) json_object_object_add(co, "feedback", json_object_new_boolean(c->feedback));
		json_object_array_add(cs, co);
	}
	json_object_object_add(o, "controls", cs);
	json_object_object_add(o, "bindings", json_object_get(p->bindings));
	return o;
}

static int mkdir_parents(const char *path)
{
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *s = tmp + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				return -1;
			*s = '/';
		}
	return 0;
}

int mp_save_file(const struct mp_profile *p, const char *path, char *err, size_t n)
{
	if (mkdir_parents(path) < 0) {
		snprintf(err, n, "cannot create the directory of %s: %s", path, strerror(errno));
		return -1;
	}
	char tmp[760];
	const char *b = strrchr(path, '/');
	int dl = b ? (int)(b - path) : 0;
	snprintf(tmp, sizeof(tmp), "%.*s%s.%s.tmp", dl, path, b ? "/" : "", b ? b + 1 : path);
	json_object *o = mp_to_json(p);
	const char *s = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	int r = -1;
	if (fd >= 0) {
		size_t len = strlen(s);
		r = write(fd, s, len) == (ssize_t)len && write(fd, "\n", 1) == 1 && fsync(fd) == 0 ? 0 : -1;
		close(fd);
		if (r == 0)
			r = rename(tmp, path);
	}
	json_object_put(o);
	if (r < 0) {
		snprintf(err, n, "cannot write %s: %s", path, strerror(errno));
		unlink(tmp);
	}
	return r;
}

bool mp_matches_device(const struct mp_profile *p, const char *hay)
{
	return p->re_ok && hay && regexec(&p->re, hay, 0, NULL, 0) == 0;
}

const struct mp_control *mp_find_control(const struct mp_profile *p, const char *name)
{
	for (int i = 0; name && i < p->n_controls; i++)
		if (!strcmp(p->controls[i].name, name))
			return &p->controls[i];
	return NULL;
}

const struct mp_control *mp_control_for_source(const struct mp_profile *p, const struct ctl_source *s)
{
	if (s->kind == CTL_SRC_OSC)
		return NULL;
	for (int i = 0; i < p->n_controls; i++) {
		const struct ctl_source *c = &p->controls[i].src;
		if (c->kind == s->kind && c->number == s->number && (c->channel == 0 || c->channel == s->channel))
			return &p->controls[i];
	}
	return NULL;
}

json_object *mp_binding_mapping(const struct mp_profile *p, size_t i, char *err, size_t n)
{
	if (i >= json_object_array_length(p->bindings)) {
		snprintf(err, n, "no binding %zu", i);
		return NULL;
	}
	json_object *b = json_object_array_get_idx(p->bindings, i);
	const struct mp_control *c = mp_find_control(p, jstr(b, "control"));
	if (!c) {
		snprintf(err, n, "binding %zu names no control of this profile", i);
		return NULL;
	}
	/* the k-th binding of this control gets ".k" from the second on */
	int k = 1;
	for (size_t j = 0; j < i; j++) {
		const char *cj = jstr(json_object_array_get_idx(p->bindings, j), "control");
		if (cj && !strcmp(cj, c->name))
			k++;
	}
	char id[48];
	if (k == 1)
		snprintf(id, sizeof(id), "%.24s.%.22s", p->id, c->name);
	else
		snprintf(id, sizeof(id), "%.24s.%.18s.%d", p->id, c->name, k);
	json_object *m = json_object_new_object();
	json_object_object_add(m, "id", json_object_new_string(id));
	json_object_object_add(m, "source", json_object_new_string(c->source));
	json_object_object_foreach(b, key, val) {
		if (strcmp(key, "control"))
			json_object_object_add(m, key, json_object_get(val));
	}
	json_object *v;
	if (c->has_encoding && c->enc != CTL_ENC_ABS && !json_object_object_get_ex(m, "encoding", &v))
		json_object_object_add(m, "encoding", json_object_new_string(ctl_encoding_name(c->enc)));
	if (c->has_bits && !json_object_object_get_ex(m, "bits", &v))
		json_object_object_add(m, "bits", json_object_new_int(c->bits));
	if (c->has_feedback && c->feedback && !json_object_object_get_ex(m, "feedback", &v))
		json_object_object_add(m, "feedback", json_object_new_boolean(1));
	return m;
}

static void control_name_for(const struct ctl_source *s, char *out, size_t n)
{
	const char *k = "cc";
	switch (s->kind) {
	case CTL_SRC_CC: k = "cc"; break;
	case CTL_SRC_CC14: k = "cc14"; break;
	case CTL_SRC_NRPN: k = "nrpn"; break;
	case CTL_SRC_RPN: k = "rpn"; break;
	case CTL_SRC_NOTE: k = "note"; break;
	case CTL_SRC_PITCHBEND: k = "pitchbend"; break;
	case CTL_SRC_PROGRAM: k = "program"; break;
	case CTL_SRC_OSC: k = "osc"; break;
	}
	char ch[8];
	if (s->channel) snprintf(ch, sizeof(ch), "%d", s->channel);
	else snprintf(ch, sizeof(ch), "any");
	if (s->number >= 0)
		snprintf(out, n, "%s.%s.%d", k, ch, s->number);
	else
		snprintf(out, n, "%s.%s", k, ch);
}

int mp_bind_mappings(struct mp_profile *p, json_object *mappings, bool add_controls, int *skipped, char *err, size_t n)
{
	int sk = 0, bound = 0;
	if (!json_object_is_type(mappings, json_type_array)) {
		snprintf(err, n, "mappings is an array");
		return -1;
	}
	json_object *nb = json_object_new_array();
	int n_controls0 = p->n_controls;
	for (size_t i = 0; i < json_object_array_length(mappings); i++) {
		json_object *m = json_object_array_get_idx(mappings, i);
		const char *src = jstr(m, "source"), *tgt = jstr(m, "target");
		struct ctl_source s;
		char e[300];
		if (!src || !tgt || ctl_parse_source(src, &s, e, sizeof(e)) < 0 || s.kind == CTL_SRC_OSC) {
			sk++;
			continue;
		}
		const struct mp_control *c = mp_control_for_source(p, &s);
		if (!c && add_controls) {
			bool clash = false;
			for (int k = 0; k < p->n_controls; k++)
				clash |= sources_clash(&p->controls[k].src, &s);
			if (!clash && p->n_controls < MP_MAX_CONTROLS) {
				struct mp_control *nc = &p->controls[p->n_controls];
				memset(nc, 0, sizeof(*nc));
				nc->bits = 14;
				nc->src = s;
				ctl_format_source(&s, nc->source, sizeof(nc->source));
				control_name_for(&s, nc->name, sizeof(nc->name));
				if (!mp_find_control(p, nc->name)) {
					p->n_controls++;
					c = nc;
				}
			}
		}
		if (!c) {
			sk++;
			continue;
		}
		json_object *b = json_object_new_object();
		json_object_object_add(b, "control", json_object_new_string(c->name));
		json_object_object_foreach(m, key, val) {
			if (strcmp(key, "control") && in_list(key, binding_keys))
				json_object_object_add(b, key, json_object_get(val));
		}
		json_object_array_add(nb, b);
		bound++;
	}
	/* all of it or none of it */
	for (size_t i = 0; i < json_object_array_length(nb); i++) {
		json_object *b = json_object_array_get_idx(nb, i);
		if (check_binding(p, b, i, err, n) < 0) {
			json_object_put(nb);
			p->n_controls = n_controls0;
			return -1;
		}
	}
	json_object_put(p->bindings);
	p->bindings = nb;
	if (skipped)
		*skipped = sk;
	return bound;
}

/* ---------------------------------------------------------------- sets */
static int name_cmp(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
static int profile_file(const struct dirent *d)
{
	size_t l = strlen(d->d_name), xl = strlen(MP_FILE_SUFFIX);
	return d->d_name[0] != '.' && l > xl && !strcmp(d->d_name + l - xl, MP_FILE_SUFFIX);
}

struct mp_profile *mp_set_find(const struct mp_set *s, const char *id)
{
	for (int i = 0; id && i < s->n; i++)
		if (!strcmp(s->p[i]->id, id))
			return s->p[i];
	return NULL;
}

int mp_set_load(struct mp_set *s, const char *const *dirs, int n_dirs, const bool *system, mp_warn_fn warn, void *ud)
{
	int loaded = 0;
	for (int d = 0; d < n_dirs; d++) {
		struct dirent **list;
		int nf = dirs[d] ? scandir(dirs[d], &list, profile_file, name_cmp) : -1;
		if (nf < 0)
			continue;
		for (int i = 0; i < nf; i++) {
			char path[700], err[400];
			snprintf(path, sizeof(path), "%s/%s", dirs[d], list[i]->d_name);
			free(list[i]);
			struct mp_profile *p = calloc(1, sizeof(*p));
			if (!p)
				continue;
			if (mp_load_file(path, p, err, sizeof(err)) < 0) {
				if (warn)
					warn(ud, path, err);
				free(p);
				continue;
			}
			p->system = system && system[d];
			if (mp_set_find(s, p->id)) { /* hidden by an earlier directory */
				mp_free(p);
				free(p);
				continue;
			}
			struct mp_profile **np = realloc(s->p, (size_t)(s->n + 1) * sizeof(*np));
			if (!np) {
				mp_free(p);
				free(p);
				continue;
			}
			s->p = np;
			s->p[s->n++] = p;
			loaded++;
		}
		free(list);
	}
	return loaded;
}

void mp_set_free(struct mp_set *s)
{
	for (int i = 0; i < s->n; i++) {
		mp_free(s->p[i]);
		free(s->p[i]);
	}
	free(s->p);
	s->p = NULL;
	s->n = 0;
}
