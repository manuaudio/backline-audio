/* Unit tests: recorder per-cycle alignment rule. SPDX-License-Identifier: MIT */
#include <stdio.h>

#include "rec_align.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	/* two sink monitors and an app's output ports */
	struct rec_align_track t[3] = { { true, 70 }, { true, 101 }, { false, 93 } };
	bool u[3];
	CHECK(rec_align_pairing(t, 3, true));
	CHECK(!rec_align_pairing(t, 3, false));
	CHECK(!rec_align_pairing(t + 2, 1, true));     /* only app outputs: nothing to pair */
	/* sink 70 drives: its monitor is written as it comes, the others one cycle late */
	CHECK(rec_align_select(t, 3, true, 70, u) == 1 && u[0] && !u[1] && !u[2]);
	/* the driver moves to sink 101 */
	CHECK(rec_align_select(t, 3, true, 101, u) == 1 && !u[0] && u[1] && !u[2]);
	/* a third node drives: everything one cycle late, still aligned */
	CHECK(rec_align_select(t, 3, true, 5, u) == 0 && !u[0] && !u[1] && !u[2]);
	/* an app's output ports never count as the late driver monitor */
	CHECK(rec_align_select(t, 3, true, 93, u) == 0 && !u[2]);
	/* no pairing (graph mode): everything from the current cycle */
	CHECK(rec_align_select(t, 3, false, 70, u) == 3 && u[0] && u[1] && u[2]);
	if (failures)
		fprintf(stderr, "%d failure(s)\n", failures);
	else
		puts("test_rec_align: all passed");
	return failures != 0;
}
