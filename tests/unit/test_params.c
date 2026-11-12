/* Unit tests for the Params1 parameter tree (control/params.c).
 * SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "params.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

static struct prm *parse(const char *js, const char *root, char *err)
{
	json_object *o = json_tokener_parse(js);
	struct prm *p = NULL;
	if (!o) {
		snprintf(err, 256, "bad test json");
		return NULL;
	}
	prm_from_json(o, root, &p, err, 256);
	json_object_put(o);
	return p;
}

static void test_paths(void)
{
	CHECK(prm_path_valid("/console/in.1/fader", false));
	CHECK(prm_path_valid("/console", true));
	CHECK(!prm_path_valid("/console", false));            /* a parameter is below a root */
	CHECK(!prm_path_valid("/console/in.1", true));
	CHECK(!prm_path_valid("/backline", true));            /* reserved */
	CHECK(!prm_path_valid("/backline/x", false));
	CHECK(!prm_path_valid("console/x", false));
	CHECK(!prm_path_valid("/console//x", false));
	CHECK(!prm_path_valid("/console/x/", false));
	CHECK(!prm_path_valid("/console/a b", false));
	CHECK(!prm_path_valid("/console/x/norm", false));     /* OSC suffixes */
	CHECK(!prm_path_valid("/console/x/touch", false));
	CHECK(!prm_path_valid("/console/x/text", false));
	CHECK(prm_path_valid("/console/x/normal", false));
	CHECK(!prm_path_valid("/a/b/c/d/e/f/g/h/i", false));  /* 9 deep */
	CHECK(prm_path_valid("/a/b/c/d/e/f/g/h", false));
	char r[64];
	CHECK(prm_path_root("/console/in.1/fader", r, sizeof(r)) && !strcmp(r, "/console"));

	CHECK(prm_match("/console/in.*/fader", "/console/in.12/fader"));
	CHECK(!prm_match("/console/in.*/fader", "/console/in.12/pan"));
	CHECK(!prm_match("/console/in.*", "/console/in.12/fader"));
	CHECK(prm_match("/console/**", "/console/in.12/fader"));
	CHECK(prm_match("/console/**", "/console/x"));
	CHECK(prm_match("/**", "/console/x"));
	CHECK(prm_match("/console/**/level", "/console/aux.3/send.in.12/level"));
	CHECK(prm_match("/console/**/level", "/console/level"));
	CHECK(!prm_match("/console/**/level", "/console/aux.3/pan"));
	CHECK(prm_match("/console/in.?/mute", "/console/in.7/mute"));
	CHECK(!prm_match("/console/in.?/mute", "/console/in.17/mute"));
	CHECK(prm_match("/console/in.1/fader", "/console/in.1/fader"));
	CHECK(!prm_match("/console/in.1", "/console/in.1/fader"));
	CHECK(!prm_match("/other/**", "/console/x"));
}

static void test_formats(void)
{
	CHECK(prm_format_valid("%.1f"));
	CHECK(prm_format_valid("%+.1f dB"));
	CHECK(prm_format_valid("%3d%%"));
	CHECK(prm_format_valid("L%02d"));
	CHECK(!prm_format_valid("%s"));
	CHECK(!prm_format_valid("%n"));
	CHECK(!prm_format_valid("%f %f"));
	CHECK(!prm_format_valid("plain"));
	CHECK(!prm_format_valid("%*d"));
	CHECK(!prm_format_valid("%123f"));
	CHECK(!prm_format_valid("%ld"));
}

static void test_params(void)
{
	char err[256] = "";
	struct prm *f = parse("{\"path\":\"in.1/fader\",\"type\":\"float\",\"min\":-144,\"max\":10,\"unit\":\"dB\","
			      "\"taper\":\"db-fader\",\"default\":0,\"flags\":[\"automatable\"]}", "/console", err);
	CHECK(f && !strcmp(f->path, "/console/in.1/fader"));
	if (!f) { fprintf(stderr, "%s\n", err); fails++; return; }
	CHECK(f->automatable && !f->read_only && f->value == 0);
	/* the fader law */
	CHECK(NEAR(prm_to_norm(f, 10), 1));
	CHECK(NEAR(prm_to_norm(f, 0), 0.75));
	CHECK(NEAR(prm_to_norm(f, -10), 0.5));
	CHECK(NEAR(prm_to_norm(f, -30), 0.25));
	CHECK(NEAR(prm_to_norm(f, -50), 0.125));
	CHECK(NEAR(prm_to_norm(f, -144), 0));
	CHECK(NEAR(prm_from_norm(f, 0.75), 0));
	CHECK(NEAR(prm_from_norm(f, 0), -144));
	for (double n = 0; n <= 1.0001; n += 0.01)
		CHECK(fabs(prm_to_norm(f, prm_from_norm(f, n)) - n) < 1e-9);
	char t[64];
	prm_format_value(f, -144, t, sizeof(t));
	CHECK(!strcmp(t, "-inf dB"));
	prm_format_value(f, -6.04, t, sizeof(t));
	CHECK(!strcmp(t, "-6.0 dB"));
	CHECK(!strcmp(f->text, "0.0 dB"));
	CHECK(prm_clamp(f, 30) == 10 && prm_clamp(f, NAN) == 0);
	prm_free(f);

	struct prm *hz = parse("{\"path\":\"/console/in.1/hpf\",\"type\":\"float\",\"min\":20,\"max\":20000,\"taper\":\"log\",\"format\":\"%.0f\",\"unit\":\"Hz\"}", "/console", err);
	CHECK(hz && NEAR(prm_to_norm(hz, 632.455532), 0.5) && NEAR(prm_from_norm(hz, 1.0 / 3), 200));
	prm_format_value(hz, 199.6, t, sizeof(t));
	CHECK(!strcmp(t, "200 Hz"));
	prm_free(hz);

	struct prm *e = parse("{\"path\":\"x/type\",\"type\":\"enum\",\"values\":[\"bell\",\"shelf\",\"cut\"],\"default\":\"shelf\"}", "/c", err);
	CHECK(e && e->min == 0 && e->max == 2 && e->value == 1 && !strcmp(e->text, "shelf"));
	CHECK(e && NEAR(prm_to_norm(e, 2), 1) && prm_from_norm(e, 0.3) == 1 && prm_label_index(e, "CUT") == 2);
	prm_free(e);

	struct prm *b = parse("{\"path\":\"x/mute\",\"type\":\"bool\",\"value\":true}", "/c", err);
	CHECK(b && b->value == 1 && !strcmp(b->text, "on") && prm_from_norm(b, 0.4) == 0);
	prm_free(b);
	b = parse("{\"path\":\"x/phase\",\"type\":\"bool\",\"values\":[\"norm\",\"inv\"]}", "/c", err);
	CHECK(b && !strcmp(b->text, "norm"));
	prm_free(b);

	struct prm *i = parse("{\"path\":\"x/n\",\"type\":\"int\",\"min\":-5,\"max\":5,\"value\":2.6,\"format\":\"%+d\"}", "/c", err);
	CHECK(i && i->value == 3 && !strcmp(i->text, "+3") && i->step == 1);
	prm_free(i);

	struct prm *s = parse("{\"path\":\"x/name\",\"type\":\"string\",\"value\":\"Kick In\",\"flags\":[\"read-only\"]}", "/c", err);
	CHECK(s && !strcmp(s->str, "Kick In") && !strcmp(s->text, "Kick In") && s->read_only);
	prm_free(s);

	/* refusals, with a reason */
	const char *bad[] = {
		"{\"path\":\"x\",\"type\":\"float\",\"min\":1,\"max\":0}",
		"{\"path\":\"x\",\"type\":\"float\",\"taper\":\"log\",\"min\":0,\"max\":1}",
		"{\"path\":\"x\",\"type\":\"enum\"}",
		"{\"path\":\"x\",\"type\":\"string\",\"min\":0}",
		"{\"path\":\"x\",\"type\":\"float\",\"format\":\"%s\"}",
		"{\"path\":\"x\",\"type\":\"wobble\"}",
		"{\"path\":\"x\",\"type\":\"float\",\"colour\":1}",
		"{\"path\":\"/other/x\",\"type\":\"float\"}",
		"{\"path\":\"x/norm\",\"type\":\"float\"}",
		"{\"type\":\"float\"}",
		"{\"path\":\"x\",\"type\":\"bool\",\"values\":[\"a\"]}",
		"{\"path\":\"x\",\"type\":\"enum\",\"values\":[\"a\"],\"default\":\"b\"}",
		"[1]",
	};
	for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		err[0] = 0;
		struct prm *p = parse(bad[k], "/c", err);
		CHECK(!p && err[0]);
		prm_free(p);
	}

	/* JSON out has the value, norm and text */
	f = parse("{\"path\":\"g\",\"type\":\"float\",\"min\":-60,\"max\":0,\"value\":-30}", "/c", err);
	json_object *o = prm_to_json(f), *v;
	CHECK(json_object_object_get_ex(o, "norm", &v) && NEAR(json_object_get_double(v), 0.5));
	CHECK(json_object_object_get_ex(o, "text", &v) && !strcmp(json_object_get_string(v), "-30.0"));
	json_object_put(o);
	prm_free(f);
}

static void test_store(void)
{
	struct prm_store *st = prm_store_new();
	char err[256], js[256];
	for (int k = 0; k < 3000; k++) {
		snprintf(js, sizeof(js), "{\"path\":\"in.%d/fader\",\"type\":\"float\"}", k);
		struct prm *p = parse(js, "/console", err);
		p->root = k % 2;
		prm_store_put(st, p);
	}
	CHECK(prm_store_count(st) == 3000);
	struct prm *p = prm_store_get(st, "/console/in.2999/fader");
	CHECK(p && prm_store_slot(st, p->slot) == p);
	CHECK(!prm_store_get(st, "/console/in.3000/fader"));
	/* replace keeps the slot */
	uint32_t slot = p->slot;
	struct prm *q = parse("{\"path\":\"in.2999/fader\",\"type\":\"int\",\"max\":4}", "/console", err);
	prm_store_put(st, q);
	CHECK(prm_store_count(st) == 3000 && prm_store_get(st, "/console/in.2999/fader")->type == PRM_INT && q->slot == slot);
	CHECK(prm_store_remove(st, "/console/in.5/fader") && !prm_store_remove(st, "/console/in.5/fader"));
	CHECK(prm_store_count(st) == 2999 && !prm_store_get(st, "/console/in.5/fader"));
	CHECK(prm_store_get(st, "/console/in.6/fader"));
	CHECK(prm_store_remove_matching(st, "/console/in.1?/fader", -1) == 10);
	CHECK(prm_store_remove_matching(st, NULL, 1) > 1000);
	size_t left = prm_store_count(st);
	CHECK(prm_store_remove_matching(st, "/console/**", -1) == (int)left && prm_store_count(st) == 0);
	/* reuse after removal */
	prm_store_put(st, parse("{\"path\":\"a\",\"type\":\"float\"}", "/x", err));
	CHECK(prm_store_count(st) == 1 && prm_store_get(st, "/x/a"));
	prm_store_free(st);
}

static void test_roles(void)
{
	char err[256], why[128];
	struct prm *send = parse("{\"path\":\"aux.3/send.in.1/level\",\"type\":\"float\"}", "/console", err);
	struct prm *fader = parse("{\"path\":\"in.1/fader\",\"type\":\"float\"}", "/console", err);
	struct prm *ro = parse("{\"path\":\"in.1/name\",\"type\":\"string\",\"flags\":[\"read-only\"]}", "/console", err);
	struct prm_role roles[1] = { 0 };
	snprintf(roles[0].name, 32, "monitor");
	snprintf(roles[0].patterns[0], PRM_PATH_MAX, "/console/aux.*/send.*/**");
	roles[0].n = 1;
	CHECK(prm_role_may_write(roles, 1, "all", fader, why, sizeof(why)));
	CHECK(prm_role_may_write(roles, 1, "", fader, why, sizeof(why)));
	CHECK(!prm_role_may_write(roles, 1, "read-only", fader, why, sizeof(why)));
	CHECK(prm_role_may_write(roles, 1, "monitor", send, why, sizeof(why)));
	CHECK(!prm_role_may_write(roles, 1, "monitor", fader, why, sizeof(why)) && strstr(why, "monitor"));
	CHECK(!prm_role_may_write(roles, 1, "foh", fader, why, sizeof(why)));       /* undefined: read only */
	CHECK(!prm_role_may_write(roles, 1, "all", ro, why, sizeof(why)) && strstr(why, "read-only"));
	CHECK(prm_role_name_valid("monitor-2") && !prm_role_name_valid("Mon") && !prm_role_name_valid(""));
	prm_free(send);
	prm_free(fader);
	prm_free(ro);
}

int main(void)
{
	test_paths();
	test_formats();
	test_params();
	test_store();
	test_roles();
	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("params: all checks passed\n");
	return 0;
}
