/* Backline registry: the pure logic (no processes, no D-Bus), unit-tested.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_REGLIB_H
#define BACKLINE_REGLIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <json-c/json.h>

#define REG_SCHEMA "backline.plugins/1"
#define REG_CACHE_SCHEMA "backline.probe-cache/1"

/* ---- hashing (SHA-256, lowercase hex) */
void reg_sha256_buf(const void *data, size_t len, char hex[65]);
int reg_sha256_file(const char *path, char hex[65]); /* 0 or -errno */

/* ---- cache: a binary is identified by path; its probe results are reused
 * while it is the same file. Fast path: size and mtime unchanged, so it is
 * not even read. If they changed, the content hash decides: equal hash (a
 * touch, a reinstall of the same build) is still a hit. */
struct reg_fp {
	int64_t size;
	int64_t mtime_ns;
	char sha256[65]; /* "" = not computed */
};
enum reg_cache_result {
	REG_CACHE_MISS = 0,
	REG_CACHE_HIT_STAT,  /* size + mtime equal: no hashing */
	REG_CACHE_HIT_HASH,  /* size or mtime changed, content identical */
};
/* `now` has size and mtime filled in; on return its sha256 is filled in too
 * (copied on a stat hit, else computed with hash()). hash returns 0 or -errno;
 * on error the result is a miss with sha256 "". */
enum reg_cache_result reg_cache_check(const struct reg_fp *cached, struct reg_fp *now,
		int (*hash)(void *ctx, char out[65]), void *ctx);
const char *reg_cache_result_name(enum reg_cache_result r);

/* ---- grades. A probe result is the JSON object a probe printed, or one the
 * runner made up: {"status":"crashed","signal":N,"signal_name":"SIGSEGV"},
 * {"status":"timeout","seconds":S}, {"status":"error","error":"..."}.
 *   load:  clap-list / vst3-list / lv2-list result (NULL: metadata from cache)
 *   check: clap-check / lv2-check result (NULL: not validated)
 * Returns "pass", "warn", "fail" or "unvalidated"; appends human-readable
 * reasons to `reasons` (a JSON array) and sets *checked to "run", "load",
 * "metadata" or "none". */
const char *reg_grade(const char *format, json_object *load, json_object *check,
		json_object *reasons, const char **checked);
/* One line about a failed probe result: "crashed (SIGSEGV, signal 11)". */
void reg_result_problem(json_object *r, char *out, size_t n);

/* ---- kinds: one vocabulary across formats */
void reg_kinds_clap(json_object *features, json_object *kinds_out);
void reg_kinds_vst3(const char *subcategories, json_object *kinds_out);
void reg_kinds_lv2(json_object *lv2_entry, json_object *kinds_out);

/* ---- products: the same plugin across formats ("Surge XT" CLAP/VST3/LV2) */
void reg_product_key(const char *vendor, const char *name, char *out, size_t n);
/* Sets "product" and "preferred" on every entry of `plugins` (CLAP, then
 * VST3, then LV2; a failing entry loses to any non-failing one). Returns the
 * number of products. */
int reg_group_products(json_object *plugins);

/* ---- queries. filter keys (all optional, ANDed): format, kind, grade, id
 * (exact), text (case-insensitive substring of name, vendor or id). */
bool reg_filter_match(json_object *plugin, json_object *filter);
json_object *reg_filter(json_object *registry, json_object *filter); /* new {"schema","plugins"} */

/* Summary counts for a plugin list. */
json_object *reg_summary(json_object *plugins);

#endif
