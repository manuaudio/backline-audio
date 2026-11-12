/* Unit tests: hostd restart policy, ids, ready-line parsing. No D-Bus needed.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "policy.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_backoff(void)
{
	struct bl_restart_policy p;
	bl_restart_policy_init(&p);
	uint64_t t = 1000 * BL_NSEC, ms = BL_NSEC / 1000;
	/* first crash after a healthy run: immediately */
	CHECK(bl_restart_policy_crash(&p, t, 60 * BL_NSEC) == 0);
	/* then quick crashes back off: 250, 500, 1000, 2000 ms */
	CHECK(bl_restart_policy_crash(&p, t + 100 * ms, 50 * ms) == 250 * ms);
	CHECK(bl_restart_policy_crash(&p, t + 400 * ms, 50 * ms) == 500 * ms);
	CHECK(bl_restart_policy_crash(&p, t + 1000 * ms, 50 * ms) == 1000 * ms);
	CHECK(bl_restart_policy_crash(&p, t + 2100 * ms, 50 * ms) == 2000 * ms);
	/* the 6th crash inside 60 s gives up */
	CHECK(bl_restart_policy_crash(&p, t + 4200 * ms, 50 * ms) == BL_RESTART_GIVE_UP);

	/* a reset forgets the history */
	bl_restart_policy_reset(&p);
	CHECK(bl_restart_policy_crash(&p, t + 5000 * ms, 50 * ms) == 0);

	/* crashes spread out over more than the window never give up */
	bl_restart_policy_init(&p);
	for (int i = 0; i < 40; i++)
		CHECK(bl_restart_policy_crash(&p, t + (uint64_t)i * 20 * BL_NSEC, 20 * BL_NSEC) == 0);

	/* the backoff is capped */
	bl_restart_policy_init(&p);
	p.max_crashes = 100;
	uint64_t d = 0;
	for (int i = 0; i < 12; i++)
		d = bl_restart_policy_crash(&p, t + (uint64_t)i * ms, ms);
	CHECK(d == p.max_ns);
}

static void test_slug(void)
{
	char b[64];
	CHECK(bl_slug("Surge XT", b, sizeof(b)) == 8 && !strcmp(b, "surge-xt"));
	bl_slug("  --Odin2.clap--  ", b, sizeof(b));
	CHECK(!strcmp(b, "odin2-clap"));
	bl_slug("/home/x/.clap/Surge XT Effects.clap", b, sizeof(b));
	CHECK(!strcmp(b, "home-x-clap-surge-xt-effects-clap"));
	CHECK(bl_slug("äöü", b, sizeof(b)) == 0 && b[0] == 0);
	bl_slug("abcdefghij", b, 5);
	CHECK(!strcmp(b, "abcd"));
}

static void test_bus_name(void)
{
	char b[256];
	bl_instance_bus_name("backline.surge-a", b, sizeof(b));
	CHECK(!strcmp(b, "io.github.manuaudio.Backline.Instance.backline_surge_a"));
	bl_instance_bus_name("9lives", b, sizeof(b));
	CHECK(!strcmp(b, "io.github.manuaudio.Backline.Instance.n_9lives"));
}

static void test_ready(void)
{
	struct bl_ready r;
	CHECK(bl_parse_ready("{\"node\": \"backline.synth\", \"plugin\": \"org.x\", \"plugin_path\": \"/a \\\"b\\\".clap\", "
			     "\"osc_port\": 40000, \"dbus\": \"io.x\", \"pid\": 42}", &r) == 0);
	CHECK(!strcmp(r.node, "backline.synth") && r.osc_port == 40000 && r.pid == 42 && !strcmp(r.plugin_path, "/a \"b\".clap"));
	CHECK(bl_parse_ready("{\"node\": \"x\"}", &r) < 0);          /* no port/pid */
	CHECK(bl_parse_ready("Surge banner text", &r) < 0);
	CHECK(bl_parse_ready("[1, 2]", &r) < 0);
}

int main(void)
{
	test_backoff();
	test_slug();
	test_bus_name();
	test_ready();
	if (failures)
		fprintf(stderr, "%d failure(s)\n", failures);
	else
		puts("test_hostd_policy: all passed");
	return failures != 0;
}
