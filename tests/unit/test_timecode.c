/* Unit test: timecode labels, LTC generation (decoded by libltc), MTC.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ltc.h>

#include "timecode.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void labels(double fps)
{
	struct tc_rate r;
	CHECK(tc_rate_from_fps(fps, &r));
	int64_t day = tc_label_to_frames(&r, &(struct tc_label){ 24, 0, 0, 0 });
	int bad = 0;
	for (int64_t k = 0; k < day; k++) {
		struct tc_label l;
		tc_frames_to_label(&r, k, &l);
		if (tc_label_to_frames(&r, &l) != k)
			bad++;
		if (r.drop && l.s == 0 && l.f < 2 && l.m % 10)
			bad++;   /* a label drop-frame must skip */
	}
	CHECK(bad == 0);
	if (r.drop)
		CHECK(day == 2589408);   /* 29.97 DF frames in 24 h */
	char s[32];
	int64_t f;
	tc_format(&r, 123456, s, sizeof(s));
	CHECK(tc_parse(&r, s, &f) && f == 123456);
	/* timecode frame k holds samples [start(k), start(k+1)) */
	for (int64_t k = 0; k < 100000; k += 7) {
		int64_t a = tc_frame_start(&r, 48000, k), b = tc_frame_start(&r, 48000, k + 1);
		CHECK(tc_frame_at(&r, 48000, a) == k && tc_frame_at(&r, 48000, b - 1) == k);
	}
	uint8_t bits[80];
	ltc_bits(&r, 98765, bits);
	int zeros = 0;
	for (int i = 0; i < 80; i++)
		zeros += !bits[i];
	CHECK(zeros % 2 == 0);
	CHECK(ltc_parse_bits(&r, bits, &f) && f == 98765);
}

/* Generate LTC with ltc_gen and decode it with libltc: every frame must
 * decode to the right time, starting where tc_frame_start says. */
static void ltc_roundtrip(double fps, uint32_t rate)
{
	struct tc_rate r;
	tc_rate_from_fps(fps, &r);
	struct ltc_gen g;
	ltc_gen_init(&g, &r, rate, 0.5f);
	LTCDecoder *d = ltc_decoder_create((int)(rate / fps), 32);
	int64_t x0 = tc_frame_start(&r, rate, 90000) - 333;   /* starts mid-frame */
	float buf[1024];
	int decoded = 0, wrong = 0;
	int64_t max_off = 0, max_off_s = 0;
	for (int64_t x = x0; x < x0 + 10 * (int64_t)rate; x += 1024) {
		for (int i = 0; i < 1024; i++)
			buf[i] = ltc_gen_sample(&g, x + i);
		ltc_decoder_write_float(d, buf, 1024, x);
		LTCFrameExt fx;
		while (ltc_decoder_read(d, &fx)) {
			SMPTETimecode st;
			ltc_frame_to_time(&st, &fx.ltc, 0);
			struct tc_label l = { st.hours, st.mins, st.secs, st.frame };
			int64_t k = tc_label_to_frames(&r, &l);
			int64_t off = fx.off_end + 1 - tc_frame_start(&r, rate, k + 1);
			int64_t off_s = fx.off_start - tc_frame_start(&r, rate, k);
			if (llabs(off_s) > max_off_s)
				max_off_s = llabs(off_s);
			if (llabs(off) > max_off)
				max_off = llabs(off);
			if (llabs(off) > 2)
				wrong++;
			decoded++;
		}
	}
	ltc_decoder_free(d);
	printf("  ltc %.2f fps @ %u: %d frames decoded by libltc; frame ends within %lld samples (off_start within %lld)\n",
	       fps, rate, decoded, (long long)max_off, (long long)max_off_s);
	CHECK(decoded >= (int)(fps * 10) - 2);
	CHECK(wrong == 0);
}

static void mtc(double fps)
{
	struct tc_rate r;
	tc_rate_from_fps(fps, &r);
	struct mtc_rx rx;
	mtc_rx_init(&rx);
	int refs = 0, wrong = 0;
	int64_t q0 = 4 * 10000;   /* a sequence starts on an even frame */
	for (int64_t q = q0; q < q0 + 400; q++) {
		int64_t ref;
		mtc_rx_quarter(&rx, mtc_qf_byte(&r, q), mtc_qf_start(&r, 48000, q), &ref);
		if (q - q0 >= 7) {
			refs++;
			if (ref != q)
				wrong++;
		}
	}
	CHECK(refs == 393 && wrong == 0);
	uint8_t ff[10];
	int code;
	int64_t f;
	mtc_full_frame(&r, 77777, ff);
	CHECK(mtc_rx_full(ff, 10, &code, &f) && f == 77777 && code == mtc_rate_code(&r));
}

int main(void)
{
	double rates[] = { 24, 25, 29.97, 30 };
	for (int i = 0; i < 4; i++) {
		labels(rates[i]);
		ltc_roundtrip(rates[i], 48000);
		ltc_roundtrip(rates[i], 44100);
		mtc(rates[i]);
	}
	if (fails)
		return 1;
	puts("timecode: ok");
	return 0;
}
