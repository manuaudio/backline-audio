/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "reglib.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>

/* ------------------------------------------------------------------ hash */
static void to_hex(const unsigned char *md, unsigned n, char hex[65])
{
	static const char d[] = "0123456789abcdef";
	for (unsigned i = 0; i < n && i < 32; i++) {
		hex[2 * i] = d[md[i] >> 4];
		hex[2 * i + 1] = d[md[i] & 15];
	}
	hex[64] = 0;
}

void reg_sha256_buf(const void *data, size_t len, char hex[65])
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned n = 0;
	EVP_Digest(data, len, md, &n, EVP_sha256(), NULL);
	to_hex(md, n, hex);
}

int reg_sha256_file(const char *path, char hex[65])
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	EVP_MD_CTX *c = EVP_MD_CTX_new();
	EVP_DigestInit_ex(c, EVP_sha256(), NULL);
	static __thread unsigned char buf[1 << 20];
	ssize_t r;
	while ((r = read(fd, buf, sizeof(buf))) > 0)
		EVP_DigestUpdate(c, buf, (size_t)r);
	int err = r < 0 ? -errno : 0;
	close(fd);
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned n = 0;
	EVP_DigestFinal_ex(c, md, &n);
	EVP_MD_CTX_free(c);
	if (err)
		return err;
	to_hex(md, n, hex);
	return 0;
}

/* ----------------------------------------------------------------- cache */
enum reg_cache_result reg_cache_check(const struct reg_fp *cached, struct reg_fp *now,
		int (*hash)(void *ctx, char out[65]), void *ctx)
{
	if (cached && cached->sha256[0] && cached->size == now->size && cached->mtime_ns == now->mtime_ns) {
		memcpy(now->sha256, cached->sha256, sizeof(now->sha256));
		return REG_CACHE_HIT_STAT;
	}
	now->sha256[0] = 0;
	if (hash(ctx, now->sha256) < 0) {
		now->sha256[0] = 0;
		return REG_CACHE_MISS;
	}
	if (cached && cached->sha256[0] && cached->size == now->size && !strcmp(cached->sha256, now->sha256))
		return REG_CACHE_HIT_HASH;
	return REG_CACHE_MISS;
}

const char *reg_cache_result_name(enum reg_cache_result r)
{
	return r == REG_CACHE_HIT_STAT ? "hit" : r == REG_CACHE_HIT_HASH ? "hash-hit" : "miss";
}

/* ---------------------------------------------------------------- grades */
static const char *jget_str(json_object *o, const char *k)
{
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) && json_object_is_type(v, json_type_string)
		? json_object_get_string(v) : NULL;
}
static int64_t jget_int(json_object *o, const char *k, int64_t def)
{
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) ? json_object_get_int64(v) : def;
}

void reg_result_problem(json_object *r, char *out, size_t n)
{
	const char *st = jget_str(r, "status");
	if (!r || !st) {
		snprintf(out, n, "no result");
	} else if (!strcmp(st, "crashed")) {
		const char *sn = jget_str(r, "signal_name");
		const char *se = jget_str(r, "stderr");
		const char *what = se ? strstr(se, "what():") : NULL;
		int sig = (int)jget_int(r, "signal", 0);
		if (what) {
			/* a C++ exception nobody caught: say what the plugin said, without
			 * the runtime's "terminate called ... what():" wrapping */
			what += strlen("what():");
			while (*what == ' ' || *what == '\t') what++;
			size_t len = strcspn(what, "\n");
			while (len && (what[len - 1] == ' ' || what[len - 1] == '\r')) len--;
			snprintf(out, n, "crashed (%s, signal %d): the plugin stopped on its own error: \"%.*s\"",
				 sn ? sn : "?", sig, (int)(len > 300 ? 300 : len), what);
		} else {
			while (se && (*se == ' ' || *se == '\t')) se++;
			snprintf(out, n, "crashed (%s, signal %d)%s%.300s", sn ? sn : "?", sig,
				 se && *se ? "; the plugin's last output: " : "", se && *se ? se : "");
		}
	} else if (!strcmp(st, "timeout")) {
		json_object *v;
		double s = json_object_object_get_ex(r, "seconds", &v) ? json_object_get_double(v) : 0;
		int k = (int)jget_int(r, "killed_processes", 0);
		if (k > 0)
			snprintf(out, n, "timed out after %.0f s; %d process%s killed with its cgroup", s, k, k == 1 ? "" : "es");
		else
			snprintf(out, n, "timed out after %.0f s (killed)", s);
	} else {
		const char *stage = jget_str(r, "stage"), *e = jget_str(r, "error");
		if (stage)
			snprintf(out, n, "%s: %s", stage, e ? e : st);
		else
			snprintf(out, n, "%s", e ? e : st);
	}
}

static bool ok_status(json_object *r)
{
	const char *st = jget_str(r, "status");
	return st && !strcmp(st, "ok");
}

static void add_reason(json_object *reasons, const char *prefix, const char *s)
{
	char buf[640];
	snprintf(buf, sizeof(buf), "%s%s", prefix, s);
	json_object_array_add(reasons, json_object_new_string(buf));
}

static void add_findings(json_object *reasons, json_object *r)
{
	json_object *f;
	if (r && json_object_object_get_ex(r, "findings", &f) && json_object_is_type(f, json_type_array))
		for (size_t i = 0; i < json_object_array_length(f); i++)
			add_reason(reasons, "", json_object_get_string(json_object_array_get_idx(f, i)));
}

const char *reg_grade(const char *format, json_object *load, json_object *check,
		json_object *reasons, const char **checked)
{
	char p[512];
	*checked = "none";
	if (load && !ok_status(load)) {
		reg_result_problem(load, p, sizeof(p));
		add_reason(reasons, "load ", p);
		add_findings(reasons, load);
		*checked = "load";
		return "fail";
	}
	if (!strcmp(format, "vst3")) {
		/* Only the module is loaded: a full VST3 run needs the SDK hosting classes. */
		*checked = "load";
		return load ? "pass" : "unvalidated";
	}
	if (!check) {
		*checked = load ? "load" : "metadata";
		return "unvalidated";
	}
	const char *st = jget_str(check, "status");
	if (st && !strcmp(st, "skipped")) {
		const char *e = jget_str(check, "error");
		add_reason(reasons, "not run: ", e ? e : "skipped");
		*checked = "metadata";
		return "warn";
	}
	*checked = "run";
	if (!ok_status(check)) {
		reg_result_problem(check, p, sizeof(p));
		add_reason(reasons, "check ", p);
		add_findings(reasons, check);
		return "fail";
	}
	json_object *f;
	int nf = 0;
	if (json_object_object_get_ex(check, "findings", &f) && json_object_is_type(f, json_type_array)) {
		nf = (int)json_object_array_length(f);
		for (int i = 0; i < nf; i++)
			add_reason(reasons, "", json_object_get_string(json_object_array_get_idx(f, i)));
	}
	return nf ? "warn" : "pass";
}

/* ----------------------------------------------------------------- kinds */
static void add_kind(json_object *kinds, const char *k)
{
	for (size_t i = 0; i < json_object_array_length(kinds); i++)
		if (!strcmp(json_object_get_string(json_object_array_get_idx(kinds, i)), k))
			return;
	json_object_array_add(kinds, json_object_new_string(k));
}

void reg_kinds_clap(json_object *features, json_object *kinds)
{
	size_t n = features ? json_object_array_length(features) : 0;
	for (size_t i = 0; i < n; i++) {
		const char *f = json_object_get_string(json_object_array_get_idx(features, i));
		if (!f) continue;
		if (!strcmp(f, "instrument")) add_kind(kinds, "instrument");
		else if (!strcmp(f, "audio-effect")) add_kind(kinds, "effect");
		else if (!strcmp(f, "note-effect")) add_kind(kinds, "midi-effect");
		else if (!strcmp(f, "analyzer")) add_kind(kinds, "analyzer");
	}
	if (!json_object_array_length(kinds)) add_kind(kinds, "other");
}

void reg_kinds_vst3(const char *sub, json_object *kinds)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", sub ? sub : "");
	char *save = NULL;
	for (char *t = strtok_r(buf, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
		if (!strcmp(t, "Instrument")) add_kind(kinds, "instrument");
		else if (!strcmp(t, "Fx")) add_kind(kinds, "effect");
		else if (!strcmp(t, "Analyzer")) add_kind(kinds, "analyzer");
	}
	if (!json_object_array_length(kinds)) add_kind(kinds, "other");
}

static bool jget_bool(json_object *o, const char *k)
{
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) && json_object_get_boolean(v);
}

void reg_kinds_lv2(json_object *p, json_object *kinds)
{
	if (jget_bool(p, "instrument")) add_kind(kinds, "instrument");
	if (jget_bool(p, "midi_plugin")) add_kind(kinds, "midi-effect");
	if (jget_bool(p, "analyser")) add_kind(kinds, "analyzer");
	json_object *io;
	if (!json_object_array_length(kinds) && json_object_object_get_ex(p, "io", &io)) {
		if (jget_int(io, "audio_in", 0) && jget_int(io, "audio_out", 0)) add_kind(kinds, "effect");
		else if (jget_int(io, "midi_in", 0) && jget_int(io, "audio_out", 0)) add_kind(kinds, "instrument");
	}
	if (!json_object_array_length(kinds)) add_kind(kinds, "other");
}

/* -------------------------------------------------------------- products */
/* Lowercase, keep [a-z0-9], drop format words ("LSP CLAP" -> "lsp"). */
static void squash(const char *in, char *out, size_t n, bool drop_formats)
{
	char words[512];
	snprintf(words, sizeof(words), "%s", in ? in : "");
	size_t o = 0;
	char *save = NULL;
	for (char *t = strtok_r(words, " \t-_/.,()", &save); t; t = strtok_r(NULL, " \t-_/.,()", &save)) {
		if (drop_formats && (!strcasecmp(t, "clap") || !strcasecmp(t, "vst") || !strcasecmp(t, "vst3") ||
				     !strcasecmp(t, "lv2") || !strcasecmp(t, "ladspa")))
			continue;
		for (char *c = t; *c && o + 1 < n; c++)
			if (isalnum((unsigned char)*c))
				out[o++] = (char)tolower((unsigned char)*c);
	}
	out[o] = 0;
}

void reg_product_key(const char *vendor, const char *name, char *out, size_t n)
{
	char v[256], nm[512];
	squash(vendor, v, sizeof(v), true);
	squash(name, nm, sizeof(nm), false);
	size_t vl = strlen(v);
	const char *np = nm;
	/* "LSP Filter Mono" by "LSP": the vendor repeated in the name */
	if (vl && !strncmp(nm, v, vl) && strlen(nm) > vl)
		np = nm + vl;
	if (vl > 12) v[12] = 0;
	snprintf(out, n, "%s/%s", v, np);
}

static int format_rank(const char *f)
{
	return !strcmp(f, "clap") ? 0 : !strcmp(f, "vst3") ? 1 : !strcmp(f, "lv2") ? 2 : 3;
}

int reg_group_products(json_object *plugins)
{
	size_t n = json_object_array_length(plugins);
	char (*keys)[640] = calloc(n ? n : 1, sizeof(*keys));
	int products = 0;
	for (size_t i = 0; i < n; i++) {
		json_object *e = json_object_array_get_idx(plugins, i);
		reg_product_key(jget_str(e, "vendor"), jget_str(e, "name"), keys[i], sizeof(keys[i]));
		json_object_object_add(e, "product", json_object_new_string(keys[i]));
	}
	for (size_t i = 0; i < n; i++) {
		bool first = true;
		for (size_t k = 0; k < i; k++)
			if (!strcmp(keys[k], keys[i])) { first = false; break; }
		if (!first) continue;
		products++;
		/* pick the best entry of this product */
		long best = -1;
		int best_score = 1 << 30;
		for (size_t k = i; k < n; k++) {
			if (strcmp(keys[k], keys[i])) continue;
			json_object *e = json_object_array_get_idx(plugins, k);
			const char *g = jget_str(e, "grade");
			int score = (g && !strcmp(g, "fail") ? 100 : 0) + format_rank(jget_str(e, "format") ? jget_str(e, "format") : "");
			if (score < best_score) { best_score = score; best = (long)k; }
		}
		for (size_t k = i; k < n; k++)
			if (!strcmp(keys[k], keys[i]))
				json_object_object_add(json_object_array_get_idx(plugins, k), "preferred",
						json_object_new_boolean((long)k == best));
	}
	free(keys);
	return products;
}

/* ---------------------------------------------------------------- filter */
static bool array_has(json_object *a, const char *s)
{
	size_t n = a && json_object_is_type(a, json_type_array) ? json_object_array_length(a) : 0;
	for (size_t i = 0; i < n; i++) {
		const char *v = json_object_get_string(json_object_array_get_idx(a, i));
		if (v && !strcmp(v, s)) return true;
	}
	return false;
}

bool reg_filter_match(json_object *p, json_object *filter)
{
	if (!filter || !json_object_is_type(filter, json_type_object))
		return true;
	const char *v;
	if ((v = jget_str(filter, "format")) && *v && (!jget_str(p, "format") || strcmp(v, jget_str(p, "format"))))
		return false;
	if ((v = jget_str(filter, "grade")) && *v && (!jget_str(p, "grade") || strcmp(v, jget_str(p, "grade"))))
		return false;
	if ((v = jget_str(filter, "id")) && *v && (!jget_str(p, "id") || strcmp(v, jget_str(p, "id"))))
		return false;
	if ((v = jget_str(filter, "kind")) && *v) {
		json_object *k;
		if (!json_object_object_get_ex(p, "kinds", &k) || !array_has(k, v))
			return false;
	}
	if ((v = jget_str(filter, "text")) && *v) {
		const char *fields[] = { "name", "vendor", "id", NULL };
		bool hit = false;
		for (int i = 0; fields[i] && !hit; i++) {
			const char *s = jget_str(p, fields[i]);
			hit = s && strcasestr(s, v);
		}
		if (!hit) return false;
	}
	json_object *pref;
	if (json_object_object_get_ex(filter, "preferred", &pref) && json_object_get_boolean(pref) && !jget_bool(p, "preferred"))
		return false;
	return true;
}

json_object *reg_filter(json_object *registry, json_object *filter)
{
	json_object *out = json_object_new_object(), *arr = json_object_new_array(), *plugins;
	json_object_object_add(out, "schema", json_object_new_string(REG_SCHEMA));
	const char *gen = jget_str(registry, "generated");
	json_object_object_add(out, "generated", json_object_new_string(gen ? gen : ""));
	if (registry && json_object_object_get_ex(registry, "plugins", &plugins))
		for (size_t i = 0; i < json_object_array_length(plugins); i++) {
			json_object *p = json_object_array_get_idx(plugins, i);
			if (reg_filter_match(p, filter))
				json_object_array_add(arr, json_object_get(p));
		}
	json_object_object_add(out, "plugins", arr);
	return out;
}

/* --------------------------------------------------------------- summary */
static void bump(json_object *o, const char *k)
{
	json_object *v;
	if (json_object_object_get_ex(o, k, &v))
		json_object_set_int64(v, json_object_get_int64(v) + 1);
	else
		json_object_object_add(o, k, json_object_new_int64(1));
}

json_object *reg_summary(json_object *plugins)
{
	json_object *s = json_object_new_object(), *fmt = json_object_new_object(),
		    *grades = json_object_new_object(), *kinds = json_object_new_object();
	const char *gs[] = { "pass", "warn", "fail", "unvalidated", NULL };
	for (int i = 0; gs[i]; i++) json_object_object_add(grades, gs[i], json_object_new_int64(0));
	size_t n = json_object_array_length(plugins);
	for (size_t i = 0; i < n; i++) {
		json_object *e = json_object_array_get_idx(plugins, i), *f, *k;
		const char *format = jget_str(e, "format"), *g = jget_str(e, "grade");
		if (!json_object_object_get_ex(fmt, format, &f)) {
			f = json_object_new_object();
			json_object_object_add(f, "plugins", json_object_new_int64(0));
			for (int j = 0; gs[j]; j++) json_object_object_add(f, gs[j], json_object_new_int64(0));
			json_object_object_add(fmt, format, f);
		}
		bump(f, "plugins");
		if (g) { bump(f, g); bump(grades, g); }
		if (json_object_object_get_ex(e, "kinds", &k))
			for (size_t j = 0; j < json_object_array_length(k); j++)
				bump(kinds, json_object_get_string(json_object_array_get_idx(k, j)));
	}
	json_object_object_add(s, "plugins", json_object_new_int64((int64_t)n));
	json_object_object_add(s, "by_format", fmt);
	json_object_object_add(s, "by_grade", grades);
	json_object_object_add(s, "by_kind", kinds);
	return s;
}
