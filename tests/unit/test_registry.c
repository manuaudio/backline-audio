/* Unit tests for the registry's pure logic. SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reglib.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { checks++; const char *_a = (a), *_b = (b); \
	if (!_a || !_b || strcmp(_a, _b)) { failures++; fprintf(stderr, "%s:%d: '%s' != '%s'\n", __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)

static void test_sha256(void)
{
	char h[65];
	reg_sha256_buf("", 0, h);
	CHECK_STR(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	reg_sha256_buf("abc", 3, h);
	CHECK_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	reg_sha256_buf(m, strlen(m), h);
	CHECK_STR(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	/* a file bigger than the read buffer: one million 'a' */
	char path[] = "/tmp/bl-test-sha-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	char *buf = malloc(1000000);
	memset(buf, 'a', 1000000);
	CHECK(write(fd, buf, 1000000) == 1000000);
	close(fd);
	CHECK(reg_sha256_file(path, h) == 0);
	CHECK_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	unlink(path);
	free(buf);
	CHECK(reg_sha256_file("/nonexistent/backline", h) < 0);
}

static int hash_calls;
static const char *hash_value;
static int fake_hash(void *ctx, char out[65])
{
	(void)ctx;
	hash_calls++;
	if (!hash_value) return -5;
	snprintf(out, 65, "%s", hash_value);
	return 0;
}

static void test_cache(void)
{
	const char *A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	const char *B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	struct reg_fp cached = { 100, 5000, "" }, now;
	snprintf(cached.sha256, sizeof(cached.sha256), "%s", A);

	/* same size + mtime: hit without hashing */
	now = (struct reg_fp){ 100, 5000, "" };
	hash_calls = 0; hash_value = B;
	CHECK(reg_cache_check(&cached, &now, fake_hash, NULL) == REG_CACHE_HIT_STAT);
	CHECK(hash_calls == 0);
	CHECK_STR(now.sha256, A);

	/* mtime changed, content equal: hash hit */
	now = (struct reg_fp){ 100, 6000, "" };
	hash_calls = 0; hash_value = A;
	CHECK(reg_cache_check(&cached, &now, fake_hash, NULL) == REG_CACHE_HIT_HASH);
	CHECK(hash_calls == 1);

	/* mtime changed, content changed: miss, new hash recorded */
	now = (struct reg_fp){ 100, 6000, "" };
	hash_value = B;
	CHECK(reg_cache_check(&cached, &now, fake_hash, NULL) == REG_CACHE_MISS);
	CHECK_STR(now.sha256, B);

	/* size changed: never a hit, even with an equal hash (it can't be equal) */
	now = (struct reg_fp){ 101, 5000, "" };
	hash_value = A;
	CHECK(reg_cache_check(&cached, &now, fake_hash, NULL) == REG_CACHE_MISS);

	/* nothing cached */
	now = (struct reg_fp){ 100, 5000, "" };
	hash_value = A;
	CHECK(reg_cache_check(NULL, &now, fake_hash, NULL) == REG_CACHE_MISS);
	CHECK_STR(now.sha256, A);

	/* unreadable file: miss, no hash */
	now = (struct reg_fp){ 100, 7000, "" };
	hash_value = NULL;
	CHECK(reg_cache_check(&cached, &now, fake_hash, NULL) == REG_CACHE_MISS);
	CHECK(now.sha256[0] == 0);

	/* a cached entry without a hash never stat-hits */
	struct reg_fp nohash = { 100, 5000, "" };
	now = (struct reg_fp){ 100, 5000, "" };
	hash_value = A; hash_calls = 0;
	CHECK(reg_cache_check(&nohash, &now, fake_hash, NULL) == REG_CACHE_MISS);
	CHECK(hash_calls == 1);
	CHECK_STR(reg_cache_result_name(REG_CACHE_HIT_HASH), "hash-hit");
}

static const char *grade(const char *fmt, const char *load, const char *check, const char **checked, json_object **reasons)
{
	json_object *l = load ? json_tokener_parse(load) : NULL, *c = check ? json_tokener_parse(check) : NULL;
	*reasons = json_object_new_array();
	const char *g = reg_grade(fmt, l, c, *reasons, checked);
	json_object_put(l);
	json_object_put(c);
	return g;
}

static void test_grades(void)
{
	const char *checked;
	json_object *r;
	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"ok\",\"findings\":[]}", &checked, &r), "pass");
	CHECK_STR(checked, "run");
	CHECK(json_object_array_length(r) == 0);
	json_object_put(r);

	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"ok\",\"findings\":[\"state differs\"]}", &checked, &r), "warn");
	CHECK_STR(json_object_get_string(json_object_array_get_idx(r, 0)), "state differs");
	json_object_put(r);

	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"crashed\",\"signal\":11,\"signal_name\":\"SIGSEGV\"}", &checked, &r), "fail");
	CHECK_STR(json_object_get_string(json_object_array_get_idx(r, 0)), "check crashed (SIGSEGV, signal 11)");
	json_object_put(r);

	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"timeout\",\"seconds\":10}", &checked, &r), "fail");
	CHECK(strstr(json_object_get_string(json_object_array_get_idx(r, 0)), "timed out after 10 s") != NULL);
	json_object_put(r);
	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"timeout\",\"seconds\":3,\"killed_processes\":2}", &checked, &r), "fail");
	CHECK_STR(json_object_get_string(json_object_array_get_idx(r, 0)), "check timed out after 3 s; 2 processes killed with its cgroup");
	json_object_put(r);
	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"crashed\",\"signal\":11,\"signal_name\":\"SIGSEGV\",\"findings\":[\"left 1 process running\"]}", &checked, &r), "fail");
	CHECK(json_object_array_length(r) == 2);
	json_object_put(r);

	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"fail\",\"stage\":\"process\",\"error\":\"3 non-finite output samples\"}", &checked, &r), "fail");
	CHECK_STR(json_object_get_string(json_object_array_get_idx(r, 0)), "check process: 3 non-finite output samples");
	json_object_put(r);

	/* a failed load fails the plugin, whatever the check says */
	CHECK_STR(grade("clap", "{\"status\":\"fail\",\"stage\":\"load\",\"error\":\"dlopen: x\"}", NULL, &checked, &r), "fail");
	CHECK_STR(checked, "load");
	json_object_put(r);

	/* not validated */
	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", NULL, &checked, &r), "unvalidated");
	json_object_put(r);

	/* VST3: graded on load only */
	CHECK_STR(grade("vst3", "{\"status\":\"ok\"}", NULL, &checked, &r), "pass");
	CHECK_STR(checked, "load");
	json_object_put(r);
	/* Cardinal Synth with a corrupt settings file: the reason says what the
	 * plugin itself said, not the C++ runtime's wrapping */
	CHECK_STR(grade("clap", "{\"status\":\"ok\"}", "{\"status\":\"crashed\",\"signal\":6,\"signal_name\":\"SIGABRT\",\"stderr\":\"terminate called after throwing an instance of 'rack::Exception'\\n  what():  Settings file has invalid JSON at 62:3 end of file expected near '{'\\n\"}", &checked, &r), "fail");
	CHECK_STR(json_object_get_string(json_object_array_get_idx(r, 0)),
		  "check crashed (SIGABRT, signal 6): the plugin stopped on its own error: \"Settings file has invalid JSON at 62:3 end of file expected near '{'\"");
	json_object_put(r);
	CHECK_STR(grade("vst3", "{\"status\":\"crashed\",\"signal\":6,\"signal_name\":\"SIGABRT\"}", NULL, &checked, &r), "fail");
	json_object_put(r);

	/* LV2: skipped for a missing host feature is a warning, not a pass */
	CHECK_STR(grade("lv2", NULL, "{\"status\":\"skipped\",\"error\":\"needs host feature X\"}", &checked, &r), "warn");
	CHECK_STR(checked, "metadata");
	json_object_put(r);
	CHECK_STR(grade("lv2", NULL, "{\"status\":\"ok\"}", &checked, &r), "pass");
	json_object_put(r);
	CHECK_STR(grade("lv2", NULL, "{\"status\":\"error\",\"error\":\"no report\"}", &checked, &r), "fail");
	json_object_put(r);
}

static void test_kinds(void)
{
	json_object *f = json_tokener_parse("[\"instrument\",\"synthesizer\",\"stereo\"]"), *k = json_object_new_array();
	reg_kinds_clap(f, k);
	CHECK(json_object_array_length(k) == 1);
	CHECK_STR(json_object_get_string(json_object_array_get_idx(k, 0)), "instrument");
	json_object_put(f); json_object_put(k);

	k = json_object_new_array();
	reg_kinds_vst3("Fx|Analyzer", k);
	CHECK(json_object_array_length(k) == 2);
	json_object_put(k);

	k = json_object_new_array();
	reg_kinds_vst3("", k);
	CHECK_STR(json_object_get_string(json_object_array_get_idx(k, 0)), "other");
	json_object_put(k);

	json_object *lv = json_tokener_parse("{\"instrument\":false,\"midi_plugin\":false,\"analyser\":false,"
		"\"io\":{\"audio_in\":2,\"audio_out\":2,\"midi_in\":0}}");
	k = json_object_new_array();
	reg_kinds_lv2(lv, k);
	CHECK_STR(json_object_get_string(json_object_array_get_idx(k, 0)), "effect");
	json_object_put(lv); json_object_put(k);
}

static json_object *entry(const char *fmt, const char *vendor, const char *name, const char *g)
{
	json_object *e = json_object_new_object();
	json_object_object_add(e, "format", json_object_new_string(fmt));
	json_object_object_add(e, "vendor", json_object_new_string(vendor));
	json_object_object_add(e, "name", json_object_new_string(name));
	json_object_object_add(e, "id", json_object_new_string(name));
	json_object_object_add(e, "grade", json_object_new_string(g));
	json_object *k = json_object_new_array();
	json_object_array_add(k, json_object_new_string("effect"));
	json_object_object_add(e, "kinds", k);
	return e;
}
static bool preferred(json_object *e)
{
	json_object *v;
	return json_object_object_get_ex(e, "preferred", &v) && json_object_get_boolean(v);
}

static void test_products(void)
{
	char k1[256], k2[256], k3[256];
	reg_product_key("LSP CLAP", "LSP Filter Mono", k1, sizeof(k1));
	reg_product_key("LSP LV2", "LSP Filter Mono", k2, sizeof(k2));
	reg_product_key("LSP", "Filter Mono", k3, sizeof(k3));
	CHECK_STR(k1, k2);
	CHECK_STR(k1, k3);
	CHECK_STR(k1, "lsp/filtermono");
	reg_product_key("Surge Synth Team", "Surge XT", k1, sizeof(k1));
	reg_product_key("Surge Synth Team", "Surge XT Effects", k2, sizeof(k2));
	CHECK(strcmp(k1, k2) != 0);

	json_object *a = json_object_new_array();
	json_object_array_add(a, entry("lv2", "Surge Synth Team", "Surge XT", "pass"));
	json_object_array_add(a, entry("vst3", "Surge Synth Team", "Surge XT", "pass"));
	json_object_array_add(a, entry("clap", "Surge Synth Team", "Surge XT", "warn"));
	json_object_array_add(a, entry("clap", "Odin", "Odin2", "fail"));
	json_object_array_add(a, entry("lv2", "Odin", "Odin2", "pass"));
	CHECK(reg_group_products(a) == 2);
	CHECK(!preferred(json_object_array_get_idx(a, 0)));
	CHECK(!preferred(json_object_array_get_idx(a, 1)));
	CHECK(preferred(json_object_array_get_idx(a, 2)));   /* CLAP first */
	CHECK(!preferred(json_object_array_get_idx(a, 3)));  /* ...unless it fails */
	CHECK(preferred(json_object_array_get_idx(a, 4)));

	/* filters */
	json_object *reg = json_object_new_object();
	json_object_object_add(reg, "plugins", a);
	json_object *f = json_tokener_parse("{\"format\":\"clap\"}");
	json_object *res = reg_filter(reg, f);
	json_object *pl;
	json_object_object_get_ex(res, "plugins", &pl);
	CHECK(json_object_array_length(pl) == 2);
	json_object_put(res); json_object_put(f);

	f = json_tokener_parse("{\"text\":\"odin\",\"grade\":\"pass\"}");
	res = reg_filter(reg, f);
	json_object_object_get_ex(res, "plugins", &pl);
	CHECK(json_object_array_length(pl) == 1);
	json_object_put(res); json_object_put(f);

	f = json_tokener_parse("{\"kind\":\"instrument\"}");
	res = reg_filter(reg, f);
	json_object_object_get_ex(res, "plugins", &pl);
	CHECK(json_object_array_length(pl) == 0);
	json_object_put(res); json_object_put(f);

	f = json_tokener_parse("{\"preferred\":true}");
	res = reg_filter(reg, f);
	json_object_object_get_ex(res, "plugins", &pl);
	CHECK(json_object_array_length(pl) == 2);
	json_object_put(res); json_object_put(f);

	res = reg_filter(reg, NULL);
	json_object_object_get_ex(res, "plugins", &pl);
	CHECK(json_object_array_length(pl) == 5);
	json_object_put(res);

	json_object *s = reg_summary(a), *bg, *v;
	json_object_object_get_ex(s, "by_grade", &bg);
	json_object_object_get_ex(bg, "pass", &v);
	CHECK(json_object_get_int(v) == 3);
	json_object_put(s);
	json_object_put(reg);
}

int main(void)
{
	test_sha256();
	test_cache();
	test_grades();
	test_kinds();
	test_products();
	printf("test_registry: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
