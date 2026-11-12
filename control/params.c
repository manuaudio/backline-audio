/* The parameter tree (params.h).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "params.h"

#include <ctype.h>
#include <fnmatch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

/* ------------------------------------------------------------ paths */
static bool seg_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
	       c == '~' || c == '-';
}

bool prm_path_valid(const char *path, bool root_only)
{
	if (!path || path[0] != '/' || strlen(path) >= PRM_PATH_MAX)
		return false;
	int depth = 0;
	const char *s = path;
	const char *last = NULL;
	size_t last_len = 0;
	while (*s == '/') {
		s++;
		const char *b = s;
		while (*s && *s != '/') {
			if (!seg_char(*s))
				return false;
			s++;
		}
		size_t len = (size_t)(s - b);
		if (len == 0 || len > PRM_SEG_MAX)
			return false;
		if (++depth > PRM_MAX_DEPTH)
			return false;
		last = b;
		last_len = len;
	}
	if (*s)
		return false;
	if (root_only)
		return depth == 1 && strncmp(path, "/backline", 10) != 0;
	if (depth < 2 || !strncmp(path, "/backline/", 10))
		return false;
	if ((last_len == 4 && (!strncmp(last, "norm", 4) || !strncmp(last, "text", 4))) ||
	    (last_len == 5 && !strncmp(last, "touch", 5)))
		return false;
	return true;
}

bool prm_path_root(const char *path, char *out, size_t n)
{
	if (!path || path[0] != '/')
		return false;
	const char *e = strchr(path + 1, '/');
	size_t len = e ? (size_t)(e - path) : strlen(path);
	if (len < 2 || len >= n)
		return false;
	memcpy(out, path, len);
	out[len] = 0;
	return true;
}

/* pattern and path both start after a '/' */
static bool match_segs(const char *pat, const char *path)
{
	for (;;) {
		if (!*pat)
			return !*path;
		const char *pe = strchr(pat, '/');
		size_t pl = pe ? (size_t)(pe - pat) : strlen(pat);
		if (pl == 2 && pat[0] == '*' && pat[1] == '*') {
			const char *rest = pe ? pe + 1 : "";
			if (!*rest)
				return true; /* ** at the end: anything, including nothing more */
			/* try the rest at this segment and every later one */
			for (const char *p = path;;) {
				if (match_segs(rest, p))
					return true;
				const char *n = strchr(p, '/');
				if (!n)
					return false;
				p = n + 1;
			}
		}
		if (!*path)
			return false;
		const char *se = strchr(path, '/');
		size_t sl = se ? (size_t)(se - path) : strlen(path);
		char ps[PRM_SEG_MAX * 2 + 1], ss[PRM_SEG_MAX + 1];
		if (pl >= sizeof(ps) || sl >= sizeof(ss))
			return false;
		memcpy(ps, pat, pl);
		ps[pl] = 0;
		memcpy(ss, path, sl);
		ss[sl] = 0;
		if (fnmatch(ps, ss, FNM_NOESCAPE) != 0)
			return false;
		if (!pe)
			return !se;
		if (!se)
			/* the path ends; a trailing '**' still matches */
			return !strcmp(pe + 1, "**");
		pat = pe + 1;
		path = se + 1;
	}
}

bool prm_match(const char *pattern, const char *path)
{
	if (!pattern || !path || pattern[0] != '/' || path[0] != '/')
		return false;
	return match_segs(pattern + 1, path + 1);
}

/* ------------------------------------------------------------ formats */
bool prm_format_valid(const char *f)
{
	if (!f || strlen(f) >= PRM_FORMAT_MAX)
		return false;
	int conv = 0;
	for (const char *s = f; *s; s++) {
		if (*s != '%')
			continue;
		s++;
		if (*s == '%')
			continue;
		while (*s && strchr("-+ 0#", *s))
			s++;
		for (int i = 0; i < 2 && isdigit((unsigned char)*s); i++)
			s++;
		if (*s == '.') {
			s++;
			for (int i = 0; i < 2 && isdigit((unsigned char)*s); i++)
				s++;
		}
		if (!*s || !strchr("fgdi", *s))
			return false;
		conv++;
	}
	return conv == 1;
}

static bool format_is_int(const char *f)
{
	for (const char *s = f; *s; s++)
		if (*s == '%') {
			if (s[1] == '%') {
				s++;
				continue;
			}
			while (*s && !strchr("fgdi", *s))
				s++;
			return *s == 'd' || *s == 'i';
		}
	return false;
}

const char *prm_type_name(enum prm_type t)
{
	static const char *n[] = { "float", "int", "bool", "enum", "string", "trigger" };
	return n[t];
}

const char *prm_taper_name(enum prm_taper t)
{
	static const char *n[] = { "linear", "log", "db-fader" };
	return n[t];
}

/* ------------------------------------------------------------ values */
double prm_clamp(const struct prm *p, double v)
{
	if (isnan(v))
		v = p->def;
	switch (p->type) {
	case PRM_BOOL:
		return v > 0.5 ? 1 : 0;
	case PRM_INT:
	case PRM_ENUM:
		v = round(v);
		/* fallthrough */
	case PRM_FLOAT:
		if (v < p->min) v = p->min;
		if (v > p->max) v = p->max;
		return v;
	default:
		return 0;
	}
}

/* db-fader breakpoints relative to the top T: (dB offset, norm) */
static const double dbf_db[] = { 0, -10, -20, -40, -60 };
static const double dbf_n[] = { 1, 0.75, 0.5, 0.25, 0.125 };

static double dbfader_to_norm(double db, double lo, double top)
{
	if (db >= top)
		return 1;
	for (int i = 0; i < 4; i++) {
		double a = top + dbf_db[i + 1], b = top + dbf_db[i];
		if (db >= a)
			return dbf_n[i + 1] + (dbf_n[i] - dbf_n[i + 1]) * (db - a) / (b - a);
	}
	double a = lo, b = top - 60;
	if (b <= a)
		return db <= a ? 0 : dbf_n[4];
	if (db <= a)
		return 0;
	return dbf_n[4] * (db - a) / (b - a);
}

static double dbfader_from_norm(double n, double lo, double top)
{
	if (n >= 1)
		return top;
	if (n <= 0)
		return lo;
	for (int i = 0; i < 4; i++)
		if (n >= dbf_n[i + 1]) {
			double a = top + dbf_db[i + 1], b = top + dbf_db[i];
			return a + (b - a) * (n - dbf_n[i + 1]) / (dbf_n[i] - dbf_n[i + 1]);
		}
	double a = lo, b = top - 60;
	if (b <= a)
		return lo;
	return a + (b - a) * n / dbf_n[4];
}

double prm_to_norm(const struct prm *p, double v)
{
	if (p->type == PRM_STRING || p->type == PRM_TRIGGER)
		return 0;
	if (p->type == PRM_BOOL)
		return v > 0.5 ? 1 : 0;
	if (!(p->max > p->min))
		return 0;
	v = prm_clamp(p, v);
	double n;
	if (p->taper == PRM_LOG && p->min > 0)
		n = log(v / p->min) / log(p->max / p->min);
	else if (p->taper == PRM_DB_FADER)
		n = dbfader_to_norm(v, p->min, p->max);
	else
		n = (v - p->min) / (p->max - p->min);
	if (!(n >= 0)) n = 0;
	if (n > 1) n = 1;
	return n;
}

double prm_from_norm(const struct prm *p, double n)
{
	if (!(n >= 0)) n = 0;
	if (n > 1) n = 1;
	if (p->type == PRM_BOOL)
		return n > 0.5 ? 1 : 0;
	if (p->type == PRM_STRING || p->type == PRM_TRIGGER)
		return 0;
	double v;
	if (p->taper == PRM_LOG && p->min > 0)
		v = p->min * pow(p->max / p->min, n);
	else if (p->taper == PRM_DB_FADER)
		v = dbfader_from_norm(n, p->min, p->max);
	else
		v = p->min + n * (p->max - p->min);
	return prm_clamp(p, v);
}

const char *prm_label(const struct prm *p, int i)
{
	if (!p->labels || i < 0 || i >= p->n_labels)
		return NULL;
	const char *s = p->labels;
	while (i-- > 0)
		s += strlen(s) + 1;
	return s;
}

int prm_label_index(const struct prm *p, const char *label)
{
	for (int i = 0; i < p->n_labels; i++)
		if (!strcasecmp(prm_label(p, i), label))
			return i;
	return -1;
}

void prm_format_value(const struct prm *p, double v, char *out, size_t n)
{
	if (n == 0)
		return;
	out[0] = 0;
	switch (p->type) {
	case PRM_STRING:
		snprintf(out, n, "%s", p->str);
		return;
	case PRM_TRIGGER:
		return;
	case PRM_BOOL: {
		const char *l = prm_label(p, v > 0.5 ? 1 : 0);
		snprintf(out, n, "%s", l ? l : (v > 0.5 ? "on" : "off"));
		return;
	}
	case PRM_ENUM: {
		const char *l = prm_label(p, (int)lround(v));
		if (l) {
			snprintf(out, n, "%s", l);
			return;
		}
		snprintf(out, n, "%ld", lround(v));
		return;
	}
	default:
		break;
	}
	if (p->taper == PRM_DB_FADER && p->min <= PRM_DB_INF && v <= p->min) {
		snprintf(out, n, "-inf%s%s", p->unit[0] ? " " : "", p->unit);
		return;
	}
	const char *f = p->format[0] ? p->format : (p->type == PRM_INT ? "%d" : "%.1f");
	size_t k;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
	if (format_is_int(f))
		k = (size_t)snprintf(out, n, f, (int)lround(v));
	else
		k = (size_t)snprintf(out, n, f, v);
#pragma GCC diagnostic pop
	if (k < n && p->unit[0])
		snprintf(out + k, n - k, " %s", p->unit);
}

void prm_refresh_text(struct prm *p)
{
	if (!p->text_override)
		prm_format_value(p, p->value, p->text, sizeof(p->text));
}

/* ------------------------------------------------------------ JSON */
static int jstr(json_object *v, char *out, size_t n, size_t maxlen)
{
	if (!json_object_is_type(v, json_type_string))
		return -1;
	const char *s = json_object_get_string(v);
	if (strlen(s) > maxlen || strlen(s) >= n)
		return -1;
	snprintf(out, n, "%s", s);
	return 0;
}

static bool jnum(json_object *v, double *out)
{
	if (!json_object_is_type(v, json_type_double) && !json_object_is_type(v, json_type_int))
		return false;
	*out = json_object_get_double(v);
	return isfinite(*out);
}

void prm_free(struct prm *p)
{
	if (!p)
		return;
	free(p->labels);
	free(p);
}

int prm_from_json(const void *jo, const char *root, struct prm **out, char *err, size_t en)
{
	json_object *o = (json_object *)jo;
	*out = NULL;
	if (!json_object_is_type(o, json_type_object)) {
		snprintf(err, en, "a parameter is a JSON object");
		return -1;
	}
	struct prm *p = calloc(1, sizeof(*p));
	if (!p) {
		snprintf(err, en, "out of memory");
		return -1;
	}
	p->min = 0;
	p->max = 1;
	bool has_min = false, has_max = false, has_def = false, has_value = false, has_step = false;
	double value = 0;
	json_object *jvalue = NULL, *jdef = NULL;
	char path[PRM_PATH_MAX] = "";
#define FAIL(...) do { snprintf(err, en, __VA_ARGS__); prm_free(p); return -1; } while (0)
	json_object_object_foreach(o, key, v) {
		if (!strcmp(key, "path")) {
			if (jstr(v, path, sizeof(path), PRM_PATH_MAX - 1) < 0)
				FAIL("path is a string");
		} else if (!strcmp(key, "type")) {
			const char *t = json_object_get_string(v);
			if (!t) FAIL("type is a string");
			if (!strcmp(t, "float")) p->type = PRM_FLOAT;
			else if (!strcmp(t, "int")) p->type = PRM_INT;
			else if (!strcmp(t, "bool")) p->type = PRM_BOOL;
			else if (!strcmp(t, "enum")) p->type = PRM_ENUM;
			else if (!strcmp(t, "string")) p->type = PRM_STRING;
			else if (!strcmp(t, "trigger")) p->type = PRM_TRIGGER;
			else FAIL("type '%s' is not float, int, bool, enum, string or trigger", t);
		} else if (!strcmp(key, "name")) {
			if (jstr(v, p->name, sizeof(p->name), PRM_NAME_MAX - 1) < 0)
				FAIL("name is a string of at most %d characters", PRM_NAME_MAX - 1);
		} else if (!strcmp(key, "short")) {
			if (jstr(v, p->short_name, sizeof(p->short_name), PRM_SHORT_MAX - 1) < 0)
				FAIL("short is a string of at most %d characters", PRM_SHORT_MAX - 1);
		} else if (!strcmp(key, "unit")) {
			if (jstr(v, p->unit, sizeof(p->unit), PRM_UNIT_MAX - 1) < 0)
				FAIL("unit is a string of at most %d characters", PRM_UNIT_MAX - 1);
		} else if (!strcmp(key, "format")) {
			if (jstr(v, p->format, sizeof(p->format), PRM_FORMAT_MAX - 1) < 0 || !prm_format_valid(p->format))
				FAIL("format must have exactly one %%f, %%g, %%d or %%i conversion (flags, width, precision allowed)");
		} else if (!strcmp(key, "taper")) {
			const char *t = json_object_get_string(v);
			if (!t) FAIL("taper is a string");
			if (!strcmp(t, "linear")) p->taper = PRM_LINEAR;
			else if (!strcmp(t, "log")) p->taper = PRM_LOG;
			else if (!strcmp(t, "db-fader")) p->taper = PRM_DB_FADER;
			else FAIL("taper '%s' is not linear, log or db-fader", t);
		} else if (!strcmp(key, "min")) {
			if (!jnum(v, &p->min)) FAIL("min is a number");
			has_min = true;
		} else if (!strcmp(key, "max")) {
			if (!jnum(v, &p->max)) FAIL("max is a number");
			has_max = true;
		} else if (!strcmp(key, "step")) {
			if (!jnum(v, &p->step) || p->step < 0) FAIL("step is a positive number");
			has_step = true;
		} else if (!strcmp(key, "default")) {
			jdef = v;
			has_def = true;
		} else if (!strcmp(key, "value")) {
			jvalue = v;
			has_value = true;
		} else if (!strcmp(key, "text")) {
			if (jstr(v, p->text, sizeof(p->text), PRM_TEXT_MAX - 1) < 0)
				FAIL("text is a string of at most %d characters", PRM_TEXT_MAX - 1);
			p->text_override = true;
		} else if (!strcmp(key, "values")) {
			if (!json_object_is_type(v, json_type_array))
				FAIL("values is an array of strings");
			size_t nl = json_object_array_length(v), total = 0;
			if (nl > PRM_MAX_ENUM)
				FAIL("at most %d values", PRM_MAX_ENUM);
			for (size_t i = 0; i < nl; i++) {
				json_object *e = json_object_array_get_idx(v, i);
				if (!json_object_is_type(e, json_type_string) || strlen(json_object_get_string(e)) > 31)
					FAIL("values are strings of at most 31 characters");
				total += strlen(json_object_get_string(e)) + 1;
			}
			free(p->labels);
			p->labels = malloc(total ? total : 1);
			size_t k = 0;
			for (size_t i = 0; i < nl; i++) {
				const char *s = json_object_get_string(json_object_array_get_idx(v, i));
				memcpy(p->labels + k, s, strlen(s) + 1);
				k += strlen(s) + 1;
			}
			p->n_labels = (int)nl;
		} else if (!strcmp(key, "flags")) {
			if (!json_object_is_type(v, json_type_array))
				FAIL("flags is an array of strings");
			for (size_t i = 0; i < json_object_array_length(v); i++) {
				const char *f = json_object_get_string(json_object_array_get_idx(v, i));
				if (f && !strcmp(f, "automatable")) p->automatable = true;
				else if (f && !strcmp(f, "read-only")) p->read_only = true;
				else FAIL("flag '%s' is not automatable or read-only", f ? f : "");
			}
		} else if (!strcmp(key, "norm")) {
			/* output-only (List): the normalised value follows from value */
		} else {
			FAIL("unknown parameter key '%s'", key);
		}
	}
	/* the path, relative to the root or absolute under it */
	if (!path[0])
		FAIL("a parameter needs a path");
	if (path[0] == '/') {
		size_t rl = strlen(root);
		if (strncmp(path, root, rl) || path[rl] != '/')
			FAIL("'%s' is not under %s", path, root);
		snprintf(p->path, sizeof(p->path), "%s", path);
	} else if ((size_t)snprintf(p->path, sizeof(p->path), "%s/%s", root, path) >= sizeof(p->path)) {
		FAIL("'%s/%s' is too long", root, path);
	}
	if (!prm_path_valid(p->path, false))
		FAIL("'%s' is not a valid path (segments of A-Z a-z 0-9 . _ ~ -, at most %d deep, not ending in norm, touch or text)",
		     p->path, PRM_MAX_DEPTH);
	switch (p->type) {
	case PRM_ENUM:
		if (p->n_labels < 1)
			FAIL("%s: an enum needs values", p->path);
		p->min = 0;
		p->max = p->n_labels - 1;
		break;
	case PRM_BOOL:
		if (p->n_labels && p->n_labels != 2)
			FAIL("%s: a bool's values are [off, on]", p->path);
		p->min = 0;
		p->max = 1;
		break;
	case PRM_FLOAT:
	case PRM_INT:
		if (!(p->max > p->min))
			FAIL("%s: max must be above min", p->path);
		if (p->taper == PRM_LOG && !(p->min > 0))
			FAIL("%s: a log taper needs min > 0", p->path);
		if (p->n_labels)
			FAIL("%s: values are for enum and bool", p->path);
		break;
	default:
		if (has_min || has_max || p->taper != PRM_LINEAR)
			FAIL("%s: a %s has no range or taper", p->path, prm_type_name(p->type));
		break;
	}
	if (p->type == PRM_INT && !has_step)
		p->step = 1;
	/* default */
	p->def = p->min;
	if (has_def) {
		if (p->type == PRM_STRING) {
			if (jstr(jdef, p->def_str, sizeof(p->def_str), PRM_STRING_MAX - 1) < 0)
				FAIL("%s: default is a string", p->path);
		} else if (p->type == PRM_BOOL && json_object_is_type(jdef, json_type_boolean)) {
			p->def = json_object_get_boolean(jdef);
		} else if (p->type == PRM_ENUM && json_object_is_type(jdef, json_type_string)) {
			int i = prm_label_index(p, json_object_get_string(jdef));
			if (i < 0) FAIL("%s: default '%s' is not one of the values", p->path, json_object_get_string(jdef));
			p->def = i;
		} else if (!jnum(jdef, &p->def)) {
			FAIL("%s: default is a %s", p->path, p->type == PRM_BOOL ? "boolean" : "number");
		}
		if (p->type != PRM_STRING)
			p->def = prm_clamp(p, p->def);
	}
	/* value */
	p->value = p->def;
	snprintf(p->str, sizeof(p->str), "%s", p->def_str);
	if (has_value) {
		if (p->type == PRM_STRING) {
			if (jstr(jvalue, p->str, sizeof(p->str), PRM_STRING_MAX - 1) < 0)
				FAIL("%s: value is a string of at most %d bytes", p->path, PRM_STRING_MAX - 1);
		} else if (p->type == PRM_BOOL && json_object_is_type(jvalue, json_type_boolean)) {
			p->value = json_object_get_boolean(jvalue);
		} else if (p->type == PRM_ENUM && json_object_is_type(jvalue, json_type_string)) {
			int i = prm_label_index(p, json_object_get_string(jvalue));
			if (i < 0) FAIL("%s: value '%s' is not one of the values", p->path, json_object_get_string(jvalue));
			p->value = i;
		} else if (p->type != PRM_TRIGGER) {
			if (!jnum(jvalue, &value))
				FAIL("%s: value is a %s", p->path, p->type == PRM_BOOL ? "boolean" : "number");
			p->value = prm_clamp(p, value);
		}
	}
	if (!p->name[0])
		snprintf(p->name, sizeof(p->name), "%s", strrchr(p->path, '/') + 1);
	prm_refresh_text(p);
	*out = p;
	return 0;
#undef FAIL
}

void *prm_to_json(const struct prm *p)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "path", json_object_new_string(p->path));
	json_object_object_add(o, "type", json_object_new_string(prm_type_name(p->type)));
	json_object_object_add(o, "name", json_object_new_string(p->name));
	if (p->short_name[0])
		json_object_object_add(o, "short", json_object_new_string(p->short_name));
	if (p->type == PRM_FLOAT || p->type == PRM_INT || p->type == PRM_ENUM) {
		json_object_object_add(o, "min", json_object_new_double(p->min));
		json_object_object_add(o, "max", json_object_new_double(p->max));
	}
	if (p->type == PRM_FLOAT || p->type == PRM_INT) {
		json_object_object_add(o, "taper", json_object_new_string(prm_taper_name(p->taper)));
		if (p->step > 0)
			json_object_object_add(o, "step", json_object_new_double(p->step));
	}
	if (p->unit[0])
		json_object_object_add(o, "unit", json_object_new_string(p->unit));
	if (p->format[0])
		json_object_object_add(o, "format", json_object_new_string(p->format));
	if (p->n_labels) {
		json_object *a = json_object_new_array();
		for (int i = 0; i < p->n_labels; i++)
			json_object_array_add(a, json_object_new_string(prm_label(p, i)));
		json_object_object_add(o, "values", a);
	}
	if (p->automatable || p->read_only) {
		json_object *a = json_object_new_array();
		if (p->automatable) json_object_array_add(a, json_object_new_string("automatable"));
		if (p->read_only) json_object_array_add(a, json_object_new_string("read-only"));
		json_object_object_add(o, "flags", a);
	}
	switch (p->type) {
	case PRM_STRING:
		json_object_object_add(o, "default", json_object_new_string(p->def_str));
		json_object_object_add(o, "value", json_object_new_string(p->str));
		break;
	case PRM_BOOL:
		json_object_object_add(o, "default", json_object_new_boolean(p->def > 0.5));
		json_object_object_add(o, "value", json_object_new_boolean(p->value > 0.5));
		json_object_object_add(o, "norm", json_object_new_double(p->value > 0.5));
		break;
	case PRM_TRIGGER:
		break;
	default:
		json_object_object_add(o, "default", json_object_new_double(p->def));
		json_object_object_add(o, "value", json_object_new_double(p->value));
		json_object_object_add(o, "norm", json_object_new_double(prm_to_norm(p, p->value)));
		break;
	}
	json_object_object_add(o, "text", json_object_new_string(p->text));
	return o;
}

/* ------------------------------------------------------------ store */
struct prm_store {
	struct prm **slots;    /* dense, by slot index */
	uint32_t n_slots, cap_slots;
	uint32_t *free_slots;
	uint32_t n_free;
	uint32_t *hash;        /* slot+1, 0 empty, UINT32_MAX tombstone */
	uint32_t hcap;         /* power of two */
	size_t count, used_h;
};

static uint64_t fnv(const char *s)
{
	uint64_t h = 1469598103934665603ull;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ull;
	}
	return h;
}

struct prm_store *prm_store_new(void)
{
	struct prm_store *s = calloc(1, sizeof(*s));
	s->hcap = 64;
	s->hash = calloc(s->hcap, sizeof(uint32_t));
	return s;
}

void prm_store_free(struct prm_store *s)
{
	if (!s)
		return;
	for (uint32_t i = 0; i < s->n_slots; i++)
		prm_free(s->slots[i]);
	free(s->slots);
	free(s->free_slots);
	free(s->hash);
	free(s);
}

static uint32_t *find_h(struct prm_store *s, const char *path, bool for_insert)
{
	uint32_t mask = s->hcap - 1, i = (uint32_t)fnv(path) & mask;
	uint32_t *tomb = NULL;
	for (uint32_t k = 0; k < s->hcap; k++, i = (i + 1) & mask) {
		uint32_t v = s->hash[i];
		if (v == 0)
			return for_insert ? (tomb ? tomb : &s->hash[i]) : NULL;
		if (v == UINT32_MAX) {
			if (!tomb)
				tomb = &s->hash[i];
			continue;
		}
		if (!strcmp(s->slots[v - 1]->path, path))
			return &s->hash[i];
	}
	return for_insert ? tomb : NULL;
}

static void rehash(struct prm_store *s, uint32_t ncap)
{
	free(s->hash);
	s->hcap = ncap;
	s->hash = calloc(ncap, sizeof(uint32_t));
	s->used_h = 0;
	for (uint32_t i = 0; i < s->n_slots; i++)
		if (s->slots[i]) {
			*find_h(s, s->slots[i]->path, true) = i + 1;
			s->used_h++;
		}
}

struct prm *prm_store_get(struct prm_store *s, const char *path)
{
	uint32_t *h = find_h(s, path, false);
	return h ? s->slots[*h - 1] : NULL;
}

struct prm *prm_store_slot(struct prm_store *s, uint32_t slot)
{
	return slot < s->n_slots ? s->slots[slot] : NULL;
}

struct prm *prm_store_put(struct prm_store *s, struct prm *p)
{
	uint32_t *h = find_h(s, p->path, false);
	if (h) {
		uint32_t slot = *h - 1;
		p->slot = slot;
		p->dirty = s->slots[slot]->dirty;
		prm_free(s->slots[slot]);
		s->slots[slot] = p;
		return p;
	}
	if ((s->used_h + 1) * 2 > s->hcap)
		rehash(s, s->hcap * 2);
	uint32_t slot;
	if (s->n_free) {
		slot = s->free_slots[--s->n_free];
	} else {
		if (s->n_slots == s->cap_slots) {
			s->cap_slots = s->cap_slots ? s->cap_slots * 2 : 64;
			s->slots = realloc(s->slots, s->cap_slots * sizeof(*s->slots));
			s->free_slots = realloc(s->free_slots, s->cap_slots * sizeof(*s->free_slots));
		}
		slot = s->n_slots++;
	}
	s->slots[slot] = p;
	p->slot = slot;
	uint32_t *ins = find_h(s, p->path, true);
	if (*ins == 0)
		s->used_h++;
	*ins = slot + 1;
	s->count++;
	return p;
}

bool prm_store_remove(struct prm_store *s, const char *path)
{
	uint32_t *h = find_h(s, path, false);
	if (!h)
		return false;
	uint32_t slot = *h - 1;
	*h = UINT32_MAX;
	prm_free(s->slots[slot]);
	s->slots[slot] = NULL;
	s->free_slots[s->n_free++] = slot;
	s->count--;
	if (s->count == 0)
		rehash(s, s->hcap);
	return true;
}

int prm_store_remove_matching(struct prm_store *s, const char *pattern, int root)
{
	int n = 0;
	for (uint32_t i = 0; i < s->n_slots; i++) {
		struct prm *p = s->slots[i];
		if (!p)
			continue;
		if (pattern ? prm_match(pattern, p->path) && (root < 0 || p->root == root) : p->root == root) {
			char path[PRM_PATH_MAX];
			snprintf(path, sizeof(path), "%s", p->path);
			prm_store_remove(s, path);
			n++;
		}
	}
	return n;
}

size_t prm_store_count(const struct prm_store *s)
{
	return s->count;
}

uint32_t prm_store_capacity(const struct prm_store *s)
{
	return s->n_slots;
}

int prm_store_each(struct prm_store *s, int (*cb)(void *data, struct prm *p), void *data)
{
	for (uint32_t i = 0; i < s->n_slots; i++)
		if (s->slots[i]) {
			int r = cb(data, s->slots[i]);
			if (r)
				return r;
		}
	return 0;
}

/* ------------------------------------------------------------ roles */
bool prm_role_name_valid(const char *r)
{
	if (!r || !*r || strlen(r) >= 32)
		return false;
	for (; *r; r++)
		if (!((*r >= 'a' && *r <= 'z') || (*r >= '0' && *r <= '9') || *r == '-' || *r == '_'))
			return false;
	return true;
}

bool prm_role_may_write(const struct prm_role *roles, int n_roles, const char *role, const struct prm *p, char *why,
			size_t n)
{
	if (p->read_only) {
		snprintf(why, n, "%s is read-only", p->path);
		return false;
	}
	if (!role || !*role || !strcmp(role, "all"))
		return true;
	if (!strcmp(role, "read-only")) {
		snprintf(why, n, "role read-only cannot write");
		return false;
	}
	for (int i = 0; i < n_roles; i++) {
		if (strcmp(roles[i].name, role))
			continue;
		for (int k = 0; k < roles[i].n; k++)
			if (prm_match(roles[i].patterns[k], p->path))
				return true;
		snprintf(why, n, "role %s may not write %s", role, p->path);
		return false;
	}
	snprintf(why, n, "no role %s is defined: read only", role);
	return false;
}
