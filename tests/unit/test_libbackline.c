/* Unit tests: libbackline helpers that need no bus, and that the library,
 * hostd and the worker agree on instance bus names.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "backline/backline.h"
#include "policy.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	const char *nodes[] = { "backline.synth", "backline.surge-a", "x y/z", "7up", "", "ünï" };
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		char a[300], b[300];
		bl_bus_name_for_node(nodes[i], a, sizeof(a));
		bl_instance_bus_name(nodes[i], b, sizeof(b));
		CHECK(!strcmp(a, b));
		CHECK(!strncmp(a, "io.github.manuaudio.Backline.Instance.", 38));
		for (const char *p = a + 38; *p; p++)
			CHECK((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_');
	}
	char small[10];
	bl_bus_name_for_node("backline.synth", small, sizeof(small));
	CHECK(strlen(small) == 9);
	CHECK(!strcmp(bl_version(), "0.2.0"));
	bl_load_options o = BL_LOAD_OPTIONS_INIT;
	CHECK(o.autoconnect == -1 && o.restart == -1 && o.id == NULL);
	CHECK(!strcmp(bl_client_error(NULL), ""));
	if (failures)
		fprintf(stderr, "%d failure(s)\n", failures);
	else
		puts("test_libbackline: all passed");
	return failures != 0;
}
