/* backline-registry: the system plugin index (CLAP, VST3, LV2).
 * SPDX-License-Identifier: MIT
 *
 *   backline-registry scan [--no-validate] [--jobs N] [--timeout S] [--path FMT:DIR]... [--force] [--json]
 *   backline-registry list [--kind K] [--format F] [--grade G] [--text T] [--json]
 *   backline-registry show ID|NAME [--json]
 *   backline-registry serve [--idle SECONDS]
 *
 * scan finds every plugin in the formats' search paths, probes each binary in
 * its own sandboxed process (runner.c), grades it pass / warn / fail and
 * writes $XDG_CACHE_HOME/backline/plugins.json (schema backline.plugins/1).
 * Probe results are cached per binary by size + mtime, then SHA-256.
 * serve answers the same questions on the session bus
 * (io.github.manuaudio.Backline.Registry1) and exits when idle.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "reglib.h"
#include "runner.h"

#define PROBE_VERSION "1"

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *jstr(json_object *o, const char *k)
{
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) && json_object_is_type(v, json_type_string)
		? json_object_get_string(v) : NULL;
}
static json_object *jobj(json_object *o, const char *k)
{
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) ? v : NULL;
}

/* ---------------------------------------------------------------- options */
struct paths {
	char *dirs[3][32];
	int n[3];
};
enum { F_CLAP, F_VST3, F_LV2 };
static const char *fmt_names[] = { "clap", "vst3", "lv2" };

struct scan_opts {
	bool validate, force, json, sandbox, quiet;
	int jobs;
	double timeout;
	const char *memory_max;
	struct paths paths;
};

static char cache_dir[PATH_MAX];

static void init_cache_dir(void)
{
	const char *e = getenv("BACKLINE_CACHE_DIR");
	if (e && *e) {
		snprintf(cache_dir, sizeof(cache_dir), "%s", e);
	} else {
		const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
		if (x && *x) snprintf(cache_dir, sizeof(cache_dir), "%s/backline", x);
		else snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/backline", h ? h : "/tmp");
	}
}

static void mkdir_p(const char *dir)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", dir);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
	mkdir(tmp, 0755);
}

static void add_path(struct paths *P, int f, const char *dir)
{
	for (int i = 0; i < P->n[f]; i++)
		if (!strcmp(P->dirs[f][i], dir)) return;
	if (P->n[f] < 32) P->dirs[f][P->n[f]++] = strdup(dir);
}

static void default_paths(struct paths *P)
{
	const char *home = getenv("HOME");
	char buf[PATH_MAX];
	const char *env[] = { "CLAP_PATH", "VST3_PATH", "LV2_PATH" };
	const char *defs[3][3] = { { "~/.clap", "/usr/lib/clap", NULL },
				   { "~/.vst3", "/usr/lib/vst3", "/usr/local/lib/vst3" },
				   { "~/.lv2", "/usr/local/lib/lv2", "/usr/lib/lv2" } };
	for (int f = 0; f < 3; f++) {
		const char *e = getenv(env[f]);
		if (e && *e) {
			char *copy = strdup(e), *save = NULL;
			for (char *t = strtok_r(copy, ":", &save); t; t = strtok_r(NULL, ":", &save))
				add_path(P, f, t);
			free(copy);
			continue;
		}
		for (int i = 0; i < 3 && defs[f][i]; i++) {
			if (defs[f][i][0] == '~') snprintf(buf, sizeof(buf), "%s%s", home ? home : "", defs[f][i] + 1);
			else snprintf(buf, sizeof(buf), "%s", defs[f][i]);
			add_path(P, f, buf);
		}
	}
}

static const char *probe_path(void)
{
	static char p[PATH_MAX];
	const char *e = getenv("BACKLINE_PROBE");
	if (e && *e) return e;
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n > 0) {
		self[n] = 0;
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = 0;
			snprintf(p, sizeof(p), "%s/backline-probe", self);
			if (access(p, X_OK) == 0) return p;
		}
	}
	return "backline-probe";
}

/* -------------------------------------------------------------- discovery */
struct strv { char **v; int n, cap; };
static void strv_add(struct strv *s, const char *x)
{
	if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
	s->v[s->n++] = strdup(x);
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcmp(s + a - b, suf);
}

/* CLAP: *.clap files, also inside *.clap bundle directories (DPF ships those). */
static void find_clap(const char *dir, struct strv *out, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	if (!d || depth > 6) { if (d) closedir(d); return; }
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(p, &st) < 0) continue;
		if (S_ISDIR(st.st_mode)) find_clap(p, out, depth + 1);
		else if (S_ISREG(st.st_mode) && ends_with(de->d_name, ".clap")) strv_add(out, p);
	}
	closedir(d);
}

/* VST3: *.vst3 bundle directories (not descending into them). */
static void find_vst3(const char *dir, struct strv *out, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	if (!d || depth > 6) { if (d) closedir(d); return; }
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(p, &st) < 0 || !S_ISDIR(st.st_mode)) continue;
		if (ends_with(de->d_name, ".vst3")) strv_add(out, p);
		else find_vst3(p, out, depth + 1);
	}
	closedir(d);
}

static void vst3_binary(const char *bundle, char *out, size_t n)
{
	struct utsname u;
	uname(&u);
	const char *base = strrchr(bundle, '/');
	base = base ? base + 1 : bundle;
	char name[512];
	snprintf(name, sizeof(name), "%s", base);
	char *dot = strstr(name, ".vst3");
	if (dot) *dot = 0;
	snprintf(out, n, "%s/Contents/%s-linux/%s.so", bundle, u.machine, name);
}

/* ------------------------------------------------------------- binaries */
struct bin {
	char *path;
	struct reg_fp fp;
	bool exists, need_hash, hashed_ok;
	char hash[65]; /* computed by the hash workers */
	enum reg_cache_result cr;
	json_object *results; /* the new cache entry's "results" (borrowed from newcache) */
};
static struct bin *bins;
static int n_bins, cap_bins;

static struct bin *bin_get(const char *path)
{
	for (int i = 0; i < n_bins; i++)
		if (!strcmp(bins[i].path, path)) return &bins[i];
	if (n_bins == cap_bins) { cap_bins = cap_bins ? cap_bins * 2 : 128; bins = realloc(bins, sizeof(*bins) * (size_t)cap_bins); }
	struct bin *b = &bins[n_bins++];
	memset(b, 0, sizeof(*b));
	b->path = strdup(path);
	return b;
}

/* parallel hashing of the binaries whose size or mtime changed */
static pthread_mutex_t hash_lock = PTHREAD_MUTEX_INITIALIZER;
static int hash_next;
static void *hash_worker(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&hash_lock);
		int i = hash_next++;
		pthread_mutex_unlock(&hash_lock);
		if (i >= n_bins) return NULL;
		struct bin *b = &bins[i];
		if (b->need_hash) b->hashed_ok = reg_sha256_file(b->path, b->hash) == 0;
	}
}
static int precomputed_hash(void *ctx, char out[65])
{
	struct bin *b = ctx;
	if (!b->hashed_ok) return -EIO;
	memcpy(out, b->hash, 65);
	return 0;
}

static json_object *old_cache, *new_cache, *new_files;
static int64_t bytes_hashed;

static void fingerprint_all(int threads)
{
	json_object *old_files = jobj(old_cache, "files");
	struct reg_fp *cached = calloc((size_t)n_bins + 1, sizeof(*cached));
	for (int i = 0; i < n_bins; i++) {
		struct bin *b = &bins[i];
		struct stat st;
		b->exists = stat(b->path, &st) == 0;
		b->fp.size = b->exists ? (int64_t)st.st_size : -1;
		b->fp.mtime_ns = b->exists ? (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : 0;
		json_object *oe = jobj(old_files, b->path);
		if (oe) {
			cached[i].size = json_object_get_int64(jobj(oe, "size"));
			cached[i].mtime_ns = json_object_get_int64(jobj(oe, "mtime_ns"));
			snprintf(cached[i].sha256, sizeof(cached[i].sha256), "%s", jstr(oe, "sha256") ? jstr(oe, "sha256") : "");
		}
		b->need_hash = b->exists && !(oe && cached[i].sha256[0] && cached[i].size == b->fp.size &&
					      cached[i].mtime_ns == b->fp.mtime_ns);
		if (b->need_hash) bytes_hashed += b->fp.size;
	}
	hash_next = 0;
	pthread_t th[64];
	int nt = threads < 1 ? 1 : threads > 64 ? 64 : threads;
	for (int t = 0; t < nt; t++) pthread_create(&th[t], NULL, hash_worker, NULL);
	for (int t = 0; t < nt; t++) pthread_join(th[t], NULL);
	for (int i = 0; i < n_bins; i++) {
		struct bin *b = &bins[i];
		json_object *oe = jobj(old_files, b->path);
		b->cr = b->exists ? reg_cache_check(oe ? &cached[i] : NULL, &b->fp, precomputed_hash, b) : REG_CACHE_MISS;
		json_object *ne = json_object_new_object();
		json_object_object_add(ne, "size", json_object_new_int64(b->fp.size));
		json_object_object_add(ne, "mtime_ns", json_object_new_int64(b->fp.mtime_ns));
		json_object_object_add(ne, "sha256", json_object_new_string(b->fp.sha256));
		json_object *res = (b->cr != REG_CACHE_MISS && jobj(oe, "results")) ? json_object_get(jobj(oe, "results"))
										   : json_object_new_object();
		json_object_object_add(ne, "results", res);
		json_object_object_add(new_files, b->path, ne);
		b->results = res;
	}
	free(cached);
}

/* ------------------------------------------------------------------ jobs */
struct plan {
	struct job *jobs;
	int n, cap;
};
struct jobtag { struct bin *bin; char key[1024]; };

static void plan_add(struct plan *p, struct bin *b, const char *key, const char *a0, const char *a1,
		     const char *a2, const char *a3, const char *lv2path)
{
	if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 64; p->jobs = realloc(p->jobs, sizeof(struct job) * (size_t)p->cap); }
	struct job *j = &p->jobs[p->n++];
	memset(j, 0, sizeof(*j));
	j->argv[0] = a0; j->argv[1] = a1 ? strdup(a1) : NULL; j->argv[2] = a2 ? strdup(a2) : NULL;
	j->argv[3] = a3 ? strdup(a3) : NULL;
	j->env_lv2_path = lv2path;
	struct jobtag *t = calloc(1, sizeof(*t));
	t->bin = b;
	snprintf(t->key, sizeof(t->key), "%s", key);
	j->user = t;
}

static int probes_run, probes_reused;

/* Run the plan; store results into the binaries' cache entries. */
static void plan_run(struct plan *p, const struct scan_opts *o, struct runner_stats *st)
{
	if (!p->n) return;
	struct runner_opts ro = { probe_path(), o->jobs, o->timeout, o->memory_max, 256, o->sandbox };
	runner_run(p->jobs, p->n, &ro, st);
	for (int i = 0; i < p->n; i++) {
		struct job *j = &p->jobs[i];
		struct jobtag *t = j->user;
		if (t->bin) {
			/* volatile results (timeouts, infrastructure errors) count for this
			 * scan but are not written to the cache */
			if (!j->cacheable) json_object_object_add(j->result, "_volatile", json_object_new_boolean(1));
			json_object_object_add(t->bin->results, t->key, json_object_get(j->result));
		}
		probes_run++;
	}
}
static void plan_free(struct plan *p)
{
	for (int i = 0; i < p->n; i++) {
		for (int k = 1; k < 4; k++) free((char *)p->jobs[i].argv[k]);
		free(p->jobs[i].user);
		json_object_put(p->jobs[i].result);
	}
	free(p->jobs);
	memset(p, 0, sizeof(*p));
}

static json_object *cached_result(struct bin *b, const char *key, bool force)
{
	json_object *r = jobj(b->results, key);
	if (!r || force) return NULL;
	return r;
}

/* ------------------------------------------------------------------ scan */
static json_object *strings_array(char *const *v, int n)
{
	json_object *a = json_object_new_array();
	for (int i = 0; i < n; i++) json_object_array_add(a, json_object_new_string(v[i]));
	return a;
}

static void copy_keys(json_object *dst, json_object *src, const char *const *keys)
{
	for (int i = 0; keys[i]; i++) {
		json_object *v = jobj(src, keys[i]);
		if (v) json_object_object_add(dst, keys[i], json_object_get(v));
	}
}

static json_object *check_summary(json_object *check)
{
	if (!check) return NULL;
	static const char *const keys[] = { "status", "checked", "params", "audio_in", "audio_out", "note_input",
		"peak", "nonfinite", "state", "state_bytes", "ms", "wall_ms", "signal", "signal_name", "seconds",
		"stray_processes", "killed_processes", "stage", "error", NULL };
	json_object *s = json_object_new_object();
	copy_keys(s, check, keys);
	return s;
}

static void finish_entry(json_object *e, const char *format, json_object *load, json_object *check)
{
	json_object *reasons = json_object_new_array();
	const char *checked;
	const char *g = reg_grade(format, load, check, reasons, &checked);
	json_object_object_add(e, "grade", json_object_new_string(g));
	json_object_object_add(e, "checked", json_object_new_string(checked));
	json_object_object_add(e, "reasons", reasons);
	if (check) json_object_object_add(e, "check", check_summary(check));
}

static json_object *error_entry(const char *format, const char *path, json_object *load, json_object *errors)
{
	json_object *e = json_object_new_object();
	char id[PATH_MAX + 8], name[512];
	snprintf(id, sizeof(id), "file:%s", path);
	const char *base = strrchr(path, '/');
	snprintf(name, sizeof(name), "%s", base ? base + 1 : path);
	char *dot = strrchr(name, '.');
	if (dot) *dot = 0;
	json_object_object_add(e, "format", json_object_new_string(format));
	json_object_object_add(e, "id", json_object_new_string(id));
	json_object_object_add(e, "name", json_object_new_string(name));
	json_object_object_add(e, "vendor", json_object_new_string(""));
	json_object_object_add(e, "version", json_object_new_string(""));
	json_object *k = json_object_new_array();
	json_object_array_add(k, json_object_new_string("other"));
	json_object_object_add(e, "kinds", k);
	json_object_object_add(e, "features", json_object_new_array());
	json_object_object_add(e, "path", json_object_new_string(path));
	finish_entry(e, format, load, NULL);
	char p[512];
	reg_result_problem(load, p, sizeof(p));
	json_object *er = json_object_new_object();
	json_object_object_add(er, "format", json_object_new_string(format));
	json_object_object_add(er, "path", json_object_new_string(path));
	json_object_object_add(er, "error", json_object_new_string(p));
	json_object_array_add(errors, er);
	return e;
}

static bool ok(json_object *r) { return r && jstr(r, "status") && !strcmp(jstr(r, "status"), "ok"); }

static int cmp_entries(const void *a, const void *b)
{
	json_object *x = *(json_object *const *)a, *y = *(json_object *const *)b;
	int c = strcasecmp(jstr(x, "name") ? jstr(x, "name") : "", jstr(y, "name") ? jstr(y, "name") : "");
	if (c) return c;
	c = strcmp(jstr(x, "format"), jstr(y, "format"));
	return c ? c : strcmp(jstr(x, "id"), jstr(y, "id"));
}

static int write_json_atomic(const char *path, json_object *o, int flags)
{
	char tmp[PATH_MAX + 16];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
	if (json_object_to_file_ext(tmp, o, flags) < 0) return -1;
	return rename(tmp, path);
}

static json_object *load_registry(void)
{
	char p[PATH_MAX + 32];
	snprintf(p, sizeof(p), "%s/plugins.json", cache_dir);
	return json_object_from_file(p);
}

static json_object *do_scan(const struct scan_opts *o)
{
	double t_start = now_s(), t;
	struct runner_stats st = { 0 };
	json_object *phases = json_object_new_object();
	n_bins = 0;
	probes_run = probes_reused = 0;
	bytes_hashed = 0;
	mkdir_p(cache_dir);
	char cpath[PATH_MAX + 32];
	snprintf(cpath, sizeof(cpath), "%s/probe-cache.json", cache_dir);
	old_cache = o->force ? NULL : json_object_from_file(cpath);
	if (old_cache && (!jstr(old_cache, "probe_version") || strcmp(jstr(old_cache, "probe_version"), PROBE_VERSION))) {
		json_object_put(old_cache);
		old_cache = NULL;
	}
	new_cache = json_object_new_object();
	new_files = json_object_new_object();
	json_object_object_add(new_cache, "schema", json_object_new_string(REG_CACHE_SCHEMA));
	json_object_object_add(new_cache, "probe_version", json_object_new_string(PROBE_VERSION));
	json_object_object_add(new_cache, "files", new_files);

	/* 1. discover */
	t = now_s();
	struct strv clap = { 0 }, vst3 = { 0 };
	for (int i = 0; i < o->paths.n[F_CLAP]; i++) find_clap(o->paths.dirs[F_CLAP][i], &clap, 0);
	for (int i = 0; i < o->paths.n[F_VST3]; i++) find_vst3(o->paths.dirs[F_VST3][i], &vst3, 0);
	if (clap.n) qsort(clap.v, (size_t)clap.n, sizeof(char *), cmpstr);
	if (vst3.n) qsort(vst3.v, (size_t)vst3.n, sizeof(char *), cmpstr);
	char lv2path[8192] = "";
	for (int i = 0; i < o->paths.n[F_LV2]; i++) {
		size_t l = strlen(lv2path);
		snprintf(lv2path + l, sizeof(lv2path) - l, "%s%s", l ? ":" : "", o->paths.dirs[F_LV2][i]);
	}
	/* LV2 metadata comes from TTL only (no plugin code), in one probe */
	json_object *lv2list = NULL;
	if (o->paths.n[F_LV2]) {
		struct plan p = { 0 };
		plan_add(&p, NULL, "lv2-list", "lv2-list", NULL, NULL, NULL, lv2path);
		struct runner_opts ro = { probe_path(), 1, o->timeout * 4, o->memory_max, 256, o->sandbox };
		runner_run(p.jobs, 1, &ro, &st);
		lv2list = json_object_get(p.jobs[0].result);
		probes_run++;
		plan_free(&p);
	}
	json_object *lv2plugins = ok(lv2list) ? jobj(lv2list, "plugins") : NULL;
	size_t n_lv2 = lv2plugins ? json_object_array_length(lv2plugins) : 0;
	json_object_object_add(phases, "discover", json_object_new_double((int64_t)((now_s() - t) * 1000) / 1000.0));

	/* 2. fingerprint */
	t = now_s();
	char so[PATH_MAX];
	for (int i = 0; i < clap.n; i++) bin_get(clap.v[i]);
	for (int i = 0; i < vst3.n; i++) { vst3_binary(vst3.v[i], so, sizeof(so)); bin_get(so); }
	for (size_t i = 0; i < n_lv2; i++) {
		json_object *pl = json_object_array_get_idx(lv2plugins, i);
		const char *lib = jstr(pl, "library");
		char man[PATH_MAX];
		snprintf(man, sizeof(man), "%s/manifest.ttl", jstr(pl, "bundle") ? jstr(pl, "bundle") : "");
		bin_get(lib && *lib ? lib : man);
	}
	fingerprint_all(o->jobs);
	int hits = 0, hash_hits = 0, misses = 0;
	for (int i = 0; i < n_bins; i++) {
		if (bins[i].cr == REG_CACHE_HIT_STAT) hits++;
		else if (bins[i].cr == REG_CACHE_HIT_HASH) hash_hits++;
		else misses++;
	}
	json_object_object_add(phases, "fingerprint", json_object_new_double((int64_t)((now_s() - t) * 1000) / 1000.0));

	/* 3. load (CLAP factory listing, VST3 module load) */
	t = now_s();
	struct plan p = { 0 };
	for (int i = 0; i < clap.n; i++) {
		struct bin *b = bin_get(clap.v[i]);
		if (!cached_result(b, "clap-list", o->force)) plan_add(&p, b, "clap-list", "clap-list", clap.v[i], NULL, NULL, NULL);
		else probes_reused++;
	}
	for (int i = 0; i < vst3.n; i++) {
		vst3_binary(vst3.v[i], so, sizeof(so));
		struct bin *b = bin_get(so);
		if (!cached_result(b, "vst3-list", o->force)) plan_add(&p, b, "vst3-list", "vst3-list", vst3.v[i], NULL, NULL, NULL);
		else probes_reused++;
	}
	plan_run(&p, o, &st);
	plan_free(&p);
	json_object_object_add(phases, "load", json_object_new_double((int64_t)((now_s() - t) * 1000) / 1000.0));

	/* 4. check (run each plugin) */
	t = now_s();
	if (o->validate) {
		for (int i = 0; i < clap.n; i++) {
			struct bin *b = bin_get(clap.v[i]);
			json_object *l = jobj(b->results, "clap-list");
			if (!ok(l)) continue;
			json_object *pls = jobj(l, "plugins");
			for (size_t k = 0; pls && k < json_object_array_length(pls); k++) {
				const char *id = jstr(json_object_array_get_idx(pls, k), "id");
				char key[1024];
				snprintf(key, sizeof(key), "clap-check:%s", id);
				if (!cached_result(b, key, o->force)) plan_add(&p, b, key, "clap-check", clap.v[i], id, NULL, NULL);
				else probes_reused++;
			}
		}
		for (size_t i = 0; i < n_lv2; i++) {
			json_object *pl = json_object_array_get_idx(lv2plugins, i);
			const char *lib = jstr(pl, "library");
			char man[PATH_MAX], key[2048];
			snprintf(man, sizeof(man), "%s/manifest.ttl", jstr(pl, "bundle") ? jstr(pl, "bundle") : "");
			struct bin *b = bin_get(lib && *lib ? lib : man);
			snprintf(key, sizeof(key), "lv2-check:%s", jstr(pl, "uri"));
			if (!cached_result(b, key, o->force))
				plan_add(&p, b, key, "lv2-check", jstr(pl, "uri"), jstr(pl, "bundle"), NULL, lv2path);
			else probes_reused++;
		}
		plan_run(&p, o, &st);
		plan_free(&p);
	}
	json_object_object_add(phases, "check", json_object_new_double((int64_t)((now_s() - t) * 1000) / 1000.0));

	/* 5. index */
	t = now_s();
	json_object *plugins = json_object_new_array(), *errors = json_object_new_array();
	for (int i = 0; i < clap.n; i++) {
		struct bin *b = bin_get(clap.v[i]);
		json_object *l = jobj(b->results, "clap-list");
		if (!ok(l)) { json_object_array_add(plugins, error_entry("clap", clap.v[i], l, errors)); continue; }
		json_object *pls = jobj(l, "plugins");
		for (size_t k = 0; pls && k < json_object_array_length(pls); k++) {
			json_object *d = json_object_array_get_idx(pls, k), *e = json_object_new_object();
			static const char *const keys[] = { "id", "name", "vendor", "version", "description", "features", NULL };
			json_object_object_add(e, "format", json_object_new_string("clap"));
			copy_keys(e, d, keys);
			json_object *kinds = json_object_new_array();
			reg_kinds_clap(jobj(d, "features"), kinds);
			json_object_object_add(e, "kinds", kinds);
			json_object_object_add(e, "path", json_object_new_string(clap.v[i]));
			json_object_object_add(e, "sha256", json_object_new_string(b->fp.sha256));
			json_object *pr = json_object_new_object();
			json_object_object_add(pr, "discovery_factory", json_object_get(jobj(l, "preset_discovery")));
			json_object_object_add(e, "presets", pr);
			json_object_object_add(e, "clap_version", json_object_get(jobj(l, "clap_version")));
			char key[1024];
			snprintf(key, sizeof(key), "clap-check:%s", jstr(d, "id"));
			finish_entry(e, "clap", l, o->validate ? jobj(b->results, key) : NULL);
			json_object_array_add(plugins, e);
		}
	}
	for (int i = 0; i < vst3.n; i++) {
		vst3_binary(vst3.v[i], so, sizeof(so));
		struct bin *b = bin_get(so);
		json_object *l = jobj(b->results, "vst3-list");
		if (!ok(l)) { json_object_array_add(plugins, error_entry("vst3", vst3.v[i], l, errors)); continue; }
		json_object *cls = jobj(l, "classes");
		for (size_t k = 0; cls && k < json_object_array_length(cls); k++) {
			json_object *c = json_object_array_get_idx(cls, k), *e = json_object_new_object();
			json_object_object_add(e, "format", json_object_new_string("vst3"));
			json_object_object_add(e, "id", json_object_get(jobj(c, "cid")));
			static const char *const keys[] = { "name", "vendor", "version", NULL };
			copy_keys(e, c, keys);
			const char *sub = jstr(c, "subcategories");
			json_object *kinds = json_object_new_array(), *feat = json_object_new_array();
			reg_kinds_vst3(sub, kinds);
			char sb[256];
			snprintf(sb, sizeof(sb), "%s", sub ? sub : "");
			char *save = NULL;
			for (char *tk = strtok_r(sb, "|", &save); tk; tk = strtok_r(NULL, "|", &save))
				json_object_array_add(feat, json_object_new_string(tk));
			json_object_object_add(e, "kinds", kinds);
			json_object_object_add(e, "features", feat);
			json_object_object_add(e, "path", json_object_new_string(vst3.v[i]));
			json_object_object_add(e, "binary", json_object_new_string(so));
			json_object_object_add(e, "sha256", json_object_new_string(b->fp.sha256));
			json_object_object_add(e, "sdk", json_object_get(jobj(c, "sdk")));
			json_object_object_add(e, "metadata_from", json_object_new_string(
				jobj(l, "moduleinfo") && json_object_get_boolean(jobj(l, "moduleinfo")) ? "moduleinfo.json+factory" : "factory"));
			json_object_object_add(e, "load_ms", json_object_get(jobj(l, "load_ms")));
			finish_entry(e, "vst3", l, NULL);
			json_object_array_add(plugins, e);
		}
	}
	if (o->paths.n[F_LV2] && !ok(lv2list))
		json_object_array_add(plugins, error_entry("lv2", lv2path, lv2list, errors));
	for (size_t i = 0; i < n_lv2; i++) {
		json_object *pl = json_object_array_get_idx(lv2plugins, i), *e = json_object_new_object();
		const char *lib = jstr(pl, "library");
		char man[PATH_MAX], key[2048];
		snprintf(man, sizeof(man), "%s/manifest.ttl", jstr(pl, "bundle") ? jstr(pl, "bundle") : "");
		struct bin *b = bin_get(lib && *lib ? lib : man);
		json_object_object_add(e, "format", json_object_new_string("lv2"));
		json_object_object_add(e, "id", json_object_get(jobj(pl, "uri")));
		static const char *const keys[] = { "name", "vendor", "io", "has_ui", "required_features", NULL };
		copy_keys(e, pl, keys);
		json_object_object_add(e, "version", json_object_new_string(""));
		json_object *kinds = json_object_new_array(), *feat = json_object_new_array();
		reg_kinds_lv2(pl, kinds);
		json_object_array_add(feat, json_object_get(jobj(pl, "class")));
		json_object_object_add(e, "kinds", kinds);
		json_object_object_add(e, "features", feat);
		json_object_object_add(e, "path", json_object_get(jobj(pl, "bundle")));
		json_object_object_add(e, "binary", json_object_new_string(b->path));
		json_object_object_add(e, "sha256", json_object_new_string(b->fp.sha256));
		json_object *pr = json_object_new_object();
		json_object_object_add(pr, "count", json_object_get(jobj(pl, "presets")));
		json_object_object_add(e, "presets", pr);
		snprintf(key, sizeof(key), "lv2-check:%s", jstr(pl, "uri"));
		finish_entry(e, "lv2", NULL, o->validate ? jobj(b->results, key) : NULL);
		json_object_array_add(plugins, e);
	}
	json_object_array_sort(plugins, cmp_entries);
	int products = reg_group_products(plugins);
	json_object_object_add(phases, "index", json_object_new_double((int64_t)((now_s() - t) * 1000) / 1000.0));

	/* drop volatile results (timeouts, infrastructure errors) from the cache */
	json_object_object_foreach(new_files, path, fe) {
		(void)path;
		json_object *res = jobj(fe, "results");
		char *drop[1024];
		int nd = 0;
		json_object_object_foreach(res, k, v) {
			if (jobj(v, "_volatile") && nd < 1024) drop[nd++] = k;
		}
		for (int i = 0; i < nd; i++) json_object_object_del(res, drop[i]);
	}
	write_json_atomic(cpath, new_cache, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);

	/* the registry */
	json_object *reg = json_object_new_object();
	char gen[64];
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	strftime(gen, sizeof(gen), "%Y-%m-%dT%H:%M:%S%z", &lt);
	json_object_object_add(reg, "schema", json_object_new_string(REG_SCHEMA));
	json_object_object_add(reg, "generated", json_object_new_string(gen));
	json_object *scan = json_object_new_object();
	json_object_object_add(scan, "seconds", json_object_new_double((int64_t)((now_s() - t_start) * 1000) / 1000.0));
	json_object_object_add(scan, "phases_s", phases);
	json_object_object_add(scan, "validate", json_object_new_boolean(o->validate));
	json_object_object_add(scan, "jobs", json_object_new_int(o->jobs));
	json_object_object_add(scan, "timeout_s", json_object_new_double(o->timeout));
	json_object_object_add(scan, "sandbox", json_object_new_string(st.sandbox ? st.sandbox : (o->sandbox && runner_scope_available() ? "systemd-scope" : "fork")));
	json_object *cache = json_object_new_object();
	json_object_object_add(cache, "binaries", json_object_new_int(n_bins));
	json_object_object_add(cache, "hit", json_object_new_int(hits));
	json_object_object_add(cache, "hash_hit", json_object_new_int(hash_hits));
	json_object_object_add(cache, "miss", json_object_new_int(misses));
	json_object_object_add(cache, "bytes_hashed", json_object_new_int64(bytes_hashed));
	json_object_object_add(scan, "cache", cache);
	json_object *pj = json_object_new_object();
	json_object_object_add(pj, "run", json_object_new_int(probes_run));
	json_object_object_add(pj, "reused", json_object_new_int(probes_reused));
	json_object_object_add(pj, "crashed", json_object_new_int(st.crashed));
	json_object_object_add(pj, "timed_out", json_object_new_int(st.timed_out));
	json_object_object_add(pj, "errors", json_object_new_int(st.errors));
	json_object_object_add(pj, "left_processes", json_object_new_int(st.leftover_units));
	json_object_object_add(scan, "probes", pj);
	json_object_object_add(reg, "scan", scan);
	json_object *sp = json_object_new_object();
	for (int f = 0; f < 3; f++) json_object_object_add(sp, fmt_names[f], strings_array(o->paths.dirs[f], o->paths.n[f]));
	json_object_object_add(reg, "search_paths", sp);
	json_object *sum = reg_summary(plugins);
	json_object_object_add(sum, "products", json_object_new_int(products));
	json_object_object_add(reg, "summary", sum);
	json_object_object_add(reg, "plugins", plugins);
	json_object_object_add(reg, "errors", errors);
	char rpath[PATH_MAX + 32];
	snprintf(rpath, sizeof(rpath), "%s/plugins.json", cache_dir);
	write_json_atomic(rpath, reg, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_NOSLASHESCAPE);

	json_object_put(lv2list);
	json_object_put(old_cache);
	json_object_put(new_cache);
	old_cache = new_cache = new_files = NULL;
	for (int i = 0; i < n_bins; i++) free(bins[i].path);
	for (int i = 0; i < clap.n; i++) free(clap.v[i]);
	for (int i = 0; i < vst3.n; i++) free(vst3.v[i]);
	free(clap.v);
	free(vst3.v);
	return reg;
}

static json_object *summary_of(json_object *reg)
{
	json_object *s = json_object_new_object();
	json_object_object_add(s, "schema", json_object_new_string(REG_SCHEMA));
	json_object_object_add(s, "generated", json_object_get(jobj(reg, "generated")));
	json_object_object_add(s, "scan", json_object_get(jobj(reg, "scan")));
	json_object_object_add(s, "summary", json_object_get(jobj(reg, "summary")));
	json_object_object_add(s, "errors", json_object_get(jobj(reg, "errors")));
	return s;
}

/* ---------------------------------------------------------------- printing */
static void print_scan(json_object *reg)
{
	json_object *sc = jobj(reg, "scan"), *c = jobj(sc, "cache"), *pr = jobj(sc, "probes"), *ph = jobj(sc, "phases_s");
	json_object *sum = jobj(reg, "summary"), *bf = jobj(sum, "by_format");
	printf("scanned %d plugins (%d products) in %.2f s [sandbox: %s, %d jobs, timeout %.0f s]\n",
	       (int)json_object_get_int(jobj(sum, "plugins")), (int)json_object_get_int(jobj(sum, "products")),
	       json_object_get_double(jobj(sc, "seconds")), jstr(sc, "sandbox"), json_object_get_int(jobj(sc, "jobs")),
	       json_object_get_double(jobj(sc, "timeout_s")));
	printf("  phases: discover %.2f s, fingerprint %.2f s, load %.2f s, check %.2f s, index %.2f s\n",
	       json_object_get_double(jobj(ph, "discover")), json_object_get_double(jobj(ph, "fingerprint")),
	       json_object_get_double(jobj(ph, "load")), json_object_get_double(jobj(ph, "check")),
	       json_object_get_double(jobj(ph, "index")));
	printf("  cache: %d binaries: %d hit (size+mtime), %d hash hit, %d miss; %.1f MB hashed\n",
	       json_object_get_int(jobj(c, "binaries")), json_object_get_int(jobj(c, "hit")),
	       json_object_get_int(jobj(c, "hash_hit")), json_object_get_int(jobj(c, "miss")),
	       json_object_get_int64(jobj(c, "bytes_hashed")) / 1e6);
	printf("  probes: %d run, %d reused from cache; %d crashed, %d timed out, %d errors; processes left behind and killed: %d\n",
	       json_object_get_int(jobj(pr, "run")), json_object_get_int(jobj(pr, "reused")),
	       json_object_get_int(jobj(pr, "crashed")), json_object_get_int(jobj(pr, "timed_out")),
	       json_object_get_int(jobj(pr, "errors")), json_object_get_int(jobj(pr, "left_processes")));
	printf("  %-5s %8s %6s %6s %6s %12s\n", "", "plugins", "pass", "warn", "fail", "unvalidated");
	json_object_object_foreach(bf, f, v) {
		printf("  %-5s %8d %6d %6d %6d %12d\n", f, json_object_get_int(jobj(v, "plugins")),
		       json_object_get_int(jobj(v, "pass")), json_object_get_int(jobj(v, "warn")),
		       json_object_get_int(jobj(v, "fail")), json_object_get_int(jobj(v, "unvalidated")));
	}
}

static void print_list(json_object *res)
{
	json_object *pl = jobj(res, "plugins");
	for (size_t i = 0; pl && i < json_object_array_length(pl); i++) {
		json_object *e = json_object_array_get_idx(pl, i), *k = jobj(e, "kinds");
		char kinds[128] = "";
		for (size_t j = 0; k && j < json_object_array_length(k); j++) {
			size_t l = strlen(kinds);
			snprintf(kinds + l, sizeof(kinds) - l, "%s%s", j ? "," : "", json_object_get_string(json_object_array_get_idx(k, j)));
		}
		json_object *pref = jobj(e, "preferred");
		printf("%s %-4s %-18s %-11s %-38.38s %-22.22s\n", pref && json_object_get_boolean(pref) ? "*" : " ",
		       jstr(e, "format"), kinds, jstr(e, "grade") ? jstr(e, "grade") : "?", jstr(e, "name") ? jstr(e, "name") : "",
		       jstr(e, "vendor") ? jstr(e, "vendor") : "");
	}
}

static json_object *show_query(json_object *reg, const char *q)
{
	json_object *out = json_object_new_object(), *arr = json_object_new_array(), *pl = jobj(reg, "plugins");
	json_object_object_add(out, "schema", json_object_new_string(REG_SCHEMA));
	for (size_t i = 0; pl && i < json_object_array_length(pl); i++) {
		json_object *e = json_object_array_get_idx(pl, i);
		const char *id = jstr(e, "id"), *name = jstr(e, "name");
		if ((id && !strcmp(id, q)) || (name && !strcasecmp(name, q)))
			json_object_array_add(arr, json_object_get(e));
	}
	json_object_object_add(out, "plugins", arr);
	return out;
}

/* ------------------------------------------------------------------- D-Bus */
#define BUS_NAME "io.github.manuaudio.Backline.Registry"
#define OBJ_PATH "/io/github/manuaudio/Backline/Registry"
#define IFACE "io.github.manuaudio.Backline.Registry1"

static struct {
	json_object *reg;
	sd_event *ev;
	sd_event_source *idle;
	uint64_t idle_usec;
	struct scan_opts scan;
	sd_bus *bus;
} S;

static void touch_idle(void)
{
	uint64_t now;
	sd_event_now(S.ev, CLOCK_MONOTONIC, &now);
	sd_event_source_set_time(S.idle, now + S.idle_usec);
	sd_event_source_set_enabled(S.idle, SD_EVENT_ONESHOT);
}

static int reply_json(sd_bus_message *m, json_object *o)
{
	int r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
	return r;
}

static int m_query(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u;
	const char *f;
	touch_idle();
	int r = sd_bus_message_read(m, "s", &f);
	if (r < 0) return r;
	json_object *filter = *f ? json_tokener_parse(f) : NULL;
	if (*f && !filter) return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "filter is not JSON");
	json_object *res = reg_filter(S.reg, filter);
	json_object_put(filter);
	return reply_json(m, res);
}

static int m_show(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u; (void)e;
	const char *q;
	touch_idle();
	int r = sd_bus_message_read(m, "s", &q);
	if (r < 0) return r;
	return reply_json(m, show_query(S.reg, q));
}

static int m_list(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u; (void)e;
	const char *kind, *format;
	touch_idle();
	int r = sd_bus_message_read(m, "ss", &kind, &format);
	if (r < 0) return r;
	json_object *filter = json_object_new_object();
	if (*kind) json_object_object_add(filter, "kind", json_object_new_string(kind));
	if (*format) json_object_object_add(filter, "format", json_object_new_string(format));
	json_object *res = reg_filter(S.reg, filter), *pl = jobj(res, "plugins");
	json_object_put(filter);
	sd_bus_message *reply = NULL;
	sd_bus_message_new_method_return(m, &reply);
	sd_bus_message_open_container(reply, 'a', "(sssssss)");
	for (size_t i = 0; pl && i < json_object_array_length(pl); i++) {
		json_object *p = json_object_array_get_idx(pl, i), *k = jobj(p, "kinds");
		char kinds[128] = "";
		for (size_t j = 0; k && j < json_object_array_length(k); j++) {
			size_t l = strlen(kinds);
			snprintf(kinds + l, sizeof(kinds) - l, "%s%s", j ? "," : "", json_object_get_string(json_object_array_get_idx(k, j)));
		}
		sd_bus_message_append(reply, "(sssssss)", jstr(p, "format") ?: "", jstr(p, "id") ?: "", jstr(p, "name") ?: "",
				      jstr(p, "vendor") ?: "", kinds, jstr(p, "grade") ?: "", jstr(p, "path") ?: "");
	}
	sd_bus_message_close_container(reply);
	r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	json_object_put(res);
	return r;
}

static int m_scan(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u; (void)e;
	int validate;
	int r = sd_bus_message_read(m, "b", &validate);
	if (r < 0) return r;
	S.scan.validate = validate;
	json_object *reg = do_scan(&S.scan);
	json_object_put(S.reg);
	S.reg = reg;
	sd_bus_emit_signal(S.bus, OBJ_PATH, IFACE, "Changed", "s", jstr(reg, "generated") ?: "");
	sd_bus_emit_properties_changed(S.bus, OBJ_PATH, IFACE, "Generated", "Count", NULL);
	touch_idle();
	return reply_json(m, summary_of(reg));
}

static int p_generated(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "s", jstr(S.reg, "generated") ?: "");
}
static int p_count(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)prop; (void)u; (void)e;
	json_object *pl = jobj(S.reg, "plugins");
	return sd_bus_message_append(r, "u", (uint32_t)(pl ? json_object_array_length(pl) : 0));
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("Query", SD_BUS_ARGS("s", filter_json), SD_BUS_RESULT("s", json), m_query, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Show", SD_BUS_ARGS("s", id_or_name), SD_BUS_RESULT("s", json), m_show, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("List", SD_BUS_ARGS("s", kind, "s", format), SD_BUS_RESULT("a(sssssss)", plugins), m_list, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Scan", SD_BUS_ARGS("b", validate), SD_BUS_RESULT("s", summary_json), m_scan, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL_WITH_ARGS("Changed", SD_BUS_ARGS("s", generated), 0),
	SD_BUS_PROPERTY("Generated", "s", p_generated, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Count", "u", p_count, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_VTABLE_END,
};

static int on_idle(sd_event_source *s, uint64_t usec, void *u)
{
	(void)s; (void)usec; (void)u;
	fprintf(stderr, "backline-registry: idle, exiting\n");
	sd_event_exit(S.ev, 0);
	return 0;
}

static int cmd_serve(double idle, const struct scan_opts *so)
{
	S.scan = *so;
	S.reg = load_registry();
	if (!S.reg) {
		fprintf(stderr, "backline-registry: no %s/plugins.json yet; scanning first\n", cache_dir);
		S.reg = do_scan(so);
	}
	S.idle_usec = (uint64_t)(idle * 1e6);
	int r;
	if ((r = sd_event_default(&S.ev)) < 0 || (r = sd_bus_open_user(&S.bus)) < 0) {
		fprintf(stderr, "backline-registry: session bus: %s\n", strerror(-r));
		return 1;
	}
	sd_bus_add_object_vtable(S.bus, NULL, OBJ_PATH, IFACE, vtable, NULL);
	if ((r = sd_bus_request_name(S.bus, BUS_NAME, 0)) < 0) {
		fprintf(stderr, "backline-registry: cannot own %s: %s\n", BUS_NAME, strerror(-r));
		return 1;
	}
	sd_bus_attach_event(S.bus, S.ev, 0);
	uint64_t now;
	sd_event_now(S.ev, CLOCK_MONOTONIC, &now);
	sd_event_add_time(S.ev, &S.idle, CLOCK_MONOTONIC, now + S.idle_usec, 0, on_idle, NULL);
	fprintf(stderr, "backline-registry: serving %s (%d plugins), idle exit after %.0f s\n", BUS_NAME,
		(int)json_object_array_length(jobj(S.reg, "plugins")), idle);
	r = sd_event_loop(S.ev);
	sd_bus_release_name(S.bus, BUS_NAME);
	sd_bus_flush_close_unref(S.bus);
	sd_event_unref(S.ev);
	json_object_put(S.reg);
	return r < 0 ? 1 : 0;
}

/* -------------------------------------------------------------------- main */
static void usage(FILE *f)
{
	fputs("usage: backline-registry scan [--no-validate] [--jobs N] [--timeout S] [--memory-max SIZE]\n"
	      "                              [--path clap|vst3|lv2:DIR]... [--only-paths] [--force] [--no-sandbox] [--json]\n"
	      "       backline-registry list [--kind K] [--format F] [--grade G] [--text T] [--json]\n"
	      "       backline-registry show ID|NAME [--json]\n"
	      "       backline-registry serve [--idle SECONDS]\n"
	      "cache: $BACKLINE_CACHE_DIR or $XDG_CACHE_HOME/backline (plugins.json, probe-cache.json)\n", f);
}

int main(int argc, char **argv)
{
	if (argc < 2) { usage(stderr); return 2; }
	init_cache_dir();
	const char *cmd = argv[1];
	struct scan_opts so = { .validate = true, .sandbox = true, .timeout = 30, .memory_max = "4G" };
	long np = sysconf(_SC_NPROCESSORS_ONLN);
	so.jobs = np > 0 ? (int)np : 4;
	bool only_paths = false, json = false;
	const char *kind = NULL, *format = NULL, *grade = NULL, *text = NULL;
	double idle = 60;
	struct paths extra = { 0 };
	static const struct option opts[] = {
		{ "validate", no_argument, NULL, 'V' }, { "no-validate", no_argument, NULL, 'N' },
		{ "jobs", required_argument, NULL, 'j' }, { "timeout", required_argument, NULL, 't' },
		{ "memory-max", required_argument, NULL, 'M' }, { "path", required_argument, NULL, 'p' },
		{ "only-paths", no_argument, NULL, 'O' }, { "force", no_argument, NULL, 'F' },
		{ "no-sandbox", no_argument, NULL, 'S' }, { "json", no_argument, NULL, 'J' },
		{ "kind", required_argument, NULL, 'k' }, { "format", required_argument, NULL, 'f' },
		{ "grade", required_argument, NULL, 'g' }, { "text", required_argument, NULL, 'x' },
		{ "idle", required_argument, NULL, 'i' }, { "help", no_argument, NULL, 'h' }, { 0 } };
	int c;
	optind = 2;
	while ((c = getopt_long(argc, argv, "j:t:p:Jh", opts, NULL)) != -1) {
		switch (c) {
		case 'V': so.validate = true; break;
		case 'N': so.validate = false; break;
		case 'j': so.jobs = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
		case 't': so.timeout = atof(optarg) > 0 ? atof(optarg) : 30; break;
		case 'M': so.memory_max = optarg; break;
		case 'p': {
			char *colon = strchr(optarg, ':');
			int f = -1;
			for (int i = 0; colon && i < 3; i++)
				if ((size_t)(colon - optarg) == strlen(fmt_names[i]) && !strncmp(optarg, fmt_names[i], (size_t)(colon - optarg))) f = i;
			if (f < 0) { fprintf(stderr, "--path wants clap:DIR, vst3:DIR or lv2:DIR\n"); return 2; }
			add_path(&extra, f, colon + 1);
			break;
		}
		case 'O': only_paths = true; break;
		case 'F': so.force = true; break;
		case 'S': so.sandbox = false; break;
		case 'J': json = true; break;
		case 'k': kind = optarg; break;
		case 'f': format = optarg; break;
		case 'g': grade = optarg; break;
		case 'x': text = optarg; break;
		case 'i': idle = atof(optarg); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (!only_paths) default_paths(&so.paths);
	for (int f = 0; f < 3; f++)
		for (int i = 0; i < extra.n[f]; i++) add_path(&so.paths, f, extra.dirs[f][i]);

	if (!strcmp(cmd, "scan")) {
		json_object *reg = do_scan(&so);
		if (json) {
			json_object *s = summary_of(reg);
			puts(json_object_to_json_string_ext(s, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_NOSLASHESCAPE));
			json_object_put(s);
		} else {
			print_scan(reg);
			printf("  -> %s/plugins.json\n", cache_dir);
		}
		json_object_put(reg);
		return 0;
	}
	if (!strcmp(cmd, "serve"))
		return cmd_serve(idle, &so);
	json_object *reg = load_registry();
	if (!reg) { fprintf(stderr, "backline-registry: no %s/plugins.json; run 'backline-registry scan'\n", cache_dir); return 1; }
	int rc = 0;
	if (!strcmp(cmd, "list")) {
		json_object *filter = json_object_new_object();
		if (kind) json_object_object_add(filter, "kind", json_object_new_string(kind));
		if (format) json_object_object_add(filter, "format", json_object_new_string(format));
		if (grade) json_object_object_add(filter, "grade", json_object_new_string(grade));
		if (text) json_object_object_add(filter, "text", json_object_new_string(text));
		json_object *res = reg_filter(reg, filter);
		if (json) puts(json_object_to_json_string_ext(res, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_NOSLASHESCAPE));
		else print_list(res);
		json_object_put(res);
		json_object_put(filter);
	} else if (!strcmp(cmd, "show") && optind < argc) {
		json_object *res = show_query(reg, argv[optind]);
		json_object *pl = jobj(res, "plugins");
		if (!json_object_array_length(pl)) { fprintf(stderr, "backline-registry: no plugin '%s'\n", argv[optind]); rc = 1; }
		else puts(json_object_to_json_string_ext(json ? res : pl, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_NOSLASHESCAPE));
		json_object_put(res);
	} else {
		usage(stderr);
		rc = 2;
	}
	json_object_put(reg);
	return rc;
}
