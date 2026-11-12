/* The clip's cycle-to-file mapping (player/clip.h pl_span).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include "clip.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void span(int64_t start, uint64_t frames, int64_t t, uint32_t n, int ok, uint32_t oo, uint64_t fo, uint32_t len)
{
	uint32_t a = 0, l = 0;
	uint64_t f = 0;
	bool r = pl_span(start, frames, t, n, &a, &f, &l);
	CHECK(r == (bool)ok);
	if (r && ok) {
		CHECK(a == oo);
		CHECK(f == fo);
		CHECK(l == len);
	}
}

int main(void)
{
	/* the start falls inside the cycle: silence, then file frame 0 at the start */
	span(1000, 48000, 900, 256, 1, 100, 0, 156);
	/* exactly on a cycle boundary */
	span(1024, 48000, 1024, 256, 1, 0, 0, 256);
	/* cycle ends just before the start */
	span(1024, 48000, 768, 256, 0, 0, 0, 0);
	/* last sample of the cycle is the first of the file */
	span(1023, 48000, 768, 256, 1, 255, 0, 1);
	/* in the middle */
	span(0, 48000, 4096, 128, 1, 0, 4096, 128);
	/* the end inside the cycle */
	span(0, 1000, 900, 256, 1, 0, 900, 100);
	/* past the end */
	span(0, 1000, 1000, 256, 0, 0, 0, 0);
	/* a file shorter than the cycle, inside it */
	span(10, 5, 0, 256, 1, 10, 0, 5);
	/* a large timeline position (10 h at 48 kHz) */
	span(1728000000, 48000, 1728000000 - 3, 64, 1, 3, 0, 61);
	if (fails)
		return 1;
	puts("player span: ok");
	return 0;
}
