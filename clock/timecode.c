/* Backline timecode math (timecode.h).
 * SPDX-License-Identifier: MIT
 */
#include "timecode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool tc_rate_from_fps(double fps, struct tc_rate *r)
{
	if (fabs(fps - 24) < 1e-3) *r = (struct tc_rate){ 24, 1, 24, false };
	else if (fabs(fps - 25) < 1e-3) *r = (struct tc_rate){ 25, 1, 25, false };
	else if (fabs(fps - 29.97) < 1e-2) *r = (struct tc_rate){ 30000, 1001, 30, true };
	else if (fabs(fps - 30) < 1e-3) *r = (struct tc_rate){ 30, 1, 30, false };
	else return false;
	return true;
}

double tc_rate_fps(const struct tc_rate *r) { return (double)r->num / r->den; }

void tc_frames_to_label(const struct tc_rate *r, int64_t frames, struct tc_label *l)
{
	if (frames < 0)
		frames = 0;
	int64_t n = frames;
	if (r->drop) {
		int64_t d = n / 17982, m = n % 17982;
		n += 18 * d + (m > 1 ? 2 * ((m - 2) / 1798) : 0);
	}
	int fps = (int)r->nominal;
	l->f = (int)(n % fps);
	n /= fps;
	l->s = (int)(n % 60);
	n /= 60;
	l->m = (int)(n % 60);
	l->h = (int)((n / 60) % 24);
}

int64_t tc_label_to_frames(const struct tc_rate *r, const struct tc_label *l)
{
	int64_t fps = r->nominal;
	int64_t f = ((int64_t)l->h * 3600 + l->m * 60 + l->s) * fps + l->f;
	if (r->drop) {
		int64_t mins = (int64_t)l->h * 60 + l->m;
		f -= 2 * (mins - mins / 10);
	}
	return f;
}

bool tc_parse(const struct tc_rate *r, const char *s, int64_t *frames)
{
	struct tc_label l;
	char sep;
	if (sscanf(s, "%d:%d:%d%c%d", &l.h, &l.m, &l.s, &sep, &l.f) != 5 || (sep != ':' && sep != ';' && sep != '.'))
		return false;
	if (l.h < 0 || l.h > 23 || l.m < 0 || l.m > 59 || l.s < 0 || l.s > 59 || l.f < 0 || l.f >= (int)r->nominal)
		return false;
	if (r->drop && l.s == 0 && l.f < 2 && l.m % 10)
		return false; /* labels that drop-frame skips */
	*frames = tc_label_to_frames(r, &l);
	return true;
}

void tc_format(const struct tc_rate *r, int64_t frames, char *out, int n)
{
	struct tc_label l;
	tc_frames_to_label(r, frames, &l);
	snprintf(out, (size_t)n, "%02d:%02d:%02d%c%02d", l.h, l.m, l.s, r->drop ? ';' : ':', l.f);
}

int64_t tc_frame_start(const struct tc_rate *r, uint32_t rate, int64_t k)
{
	/* ceil(k x rate x den / num) */
	__int128 a = (__int128)k * rate * r->den;
	return (int64_t)((a + r->num - 1) / r->num);
}

int64_t tc_frame_at(const struct tc_rate *r, uint32_t rate, int64_t x)
{
	__int128 a = (__int128)x * r->num;
	return (int64_t)(a / ((__int128)rate * r->den));
}

/* ---- LTC ---------------------------------------------------------------- */
static void put_bcd(uint8_t *bits, int at, int nbits, int v)
{
	for (int i = 0; i < nbits; i++)
		bits[at + i] = (uint8_t)((v >> i) & 1);
}

static int get_bits(const uint8_t *bits, int at, int nbits)
{
	int v = 0;
	for (int i = 0; i < nbits; i++)
		v |= bits[at + i] << i;
	return v;
}

void ltc_bits(const struct tc_rate *r, int64_t frames, uint8_t bits[80])
{
	struct tc_label l;
	tc_frames_to_label(r, frames, &l);
	memset(bits, 0, 80);
	put_bcd(bits, 0, 4, l.f % 10);
	put_bcd(bits, 8, 2, l.f / 10);
	bits[10] = r->drop;
	put_bcd(bits, 16, 4, l.s % 10);
	put_bcd(bits, 24, 3, l.s / 10);
	put_bcd(bits, 32, 4, l.m % 10);
	put_bcd(bits, 40, 3, l.m / 10);
	put_bcd(bits, 48, 4, l.h % 10);
	put_bcd(bits, 56, 2, l.h / 10);
	static const uint8_t sync[16] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
	memcpy(bits + 64, sync, 16);
	/* polarity correction: bit 59 at 25 fps, bit 27 otherwise; an even
	 * number of zeros keeps every frame starting at the same level */
	int pc = r->nominal == 25 ? 59 : 27;
	int zeros = 0;
	for (int i = 0; i < 80; i++)
		zeros += bits[i] == 0;
	if (zeros & 1)
		bits[pc] = 1;
}

bool ltc_parse_bits(const struct tc_rate *r, const uint8_t bits[80], int64_t *frames)
{
	static const uint8_t sync[16] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
	if (memcmp(bits + 64, sync, 16))
		return false;
	struct tc_label l = {
		get_bits(bits, 48, 4) + 10 * get_bits(bits, 56, 2),
		get_bits(bits, 32, 4) + 10 * get_bits(bits, 40, 3),
		get_bits(bits, 16, 4) + 10 * get_bits(bits, 24, 3),
		get_bits(bits, 0, 4) + 10 * get_bits(bits, 8, 2),
	};
	*frames = tc_label_to_frames(r, &l);
	return true;
}

void ltc_gen_init(struct ltc_gen *g, const struct tc_rate *r, uint32_t rate, float amp)
{
	memset(g, 0, sizeof(*g));
	g->r = *r;
	g->rate = rate;
	g->next = -1;
	g->level = 1;
	g->amp = amp;
}

static void gen_frame(struct ltc_gen *g, int64_t k)
{
	g->frame = k;
	g->frame_start = tc_frame_start(&g->r, g->rate, k);
	g->frame_len = tc_frame_start(&g->r, g->rate, k + 1) - g->frame_start;
	ltc_bits(&g->r, k, g->bits);
	g->last_cell = -1;
}

float ltc_gen_sample(struct ltc_gen *g, int64_t x)
{
	if (x < 0)
		return 0.0f;
	if (x != g->next || x < g->frame_start || x >= g->frame_start + g->frame_len) {
		int64_t k = tc_frame_at(&g->r, g->rate, x);
		bool contiguous = x == g->next && k == g->frame + 1;
		gen_frame(g, k);
		if (!contiguous)
			g->level = 1;
	}
	g->next = x + 1;
	/* half-cell of this sample within the frame: 160 per frame */
	int cell = (int)(((x - g->frame_start) * 160) / g->frame_len);
	while (g->last_cell < cell) {
		g->last_cell++;
		int bit = g->last_cell / 2;
		/* a transition at the start of every bit, and mid-bit for a 1 */
		if ((g->last_cell & 1) == 0 || g->bits[bit])
			g->level = -g->level;
	}
	return g->level * g->amp;
}

/* ---- MTC ---------------------------------------------------------------- */
int64_t mtc_qf_start(const struct tc_rate *r, uint32_t rate, int64_t q)
{
	__int128 a = (__int128)q * rate * r->den;
	__int128 d = (__int128)4 * r->num;
	return (int64_t)((a + d - 1) / d);
}

int mtc_rate_code(const struct tc_rate *r)
{
	if (r->drop) return 2;
	return r->nominal == 24 ? 0 : r->nominal == 25 ? 1 : 3;
}

bool tc_rate_from_mtc_code(int code, struct tc_rate *r)
{
	static const double f[4] = { 24, 25, 29.97, 30 };
	return code >= 0 && code < 4 && tc_rate_from_fps(f[code], r);
}

uint8_t mtc_qf_byte(const struct tc_rate *r, int64_t q)
{
	int piece = (int)(q & 7);
	struct tc_label l;
	tc_frames_to_label(r, (q - piece) / 4, &l);
	int v;
	switch (piece) {
	case 0: v = l.f & 0xf; break;
	case 1: v = l.f >> 4; break;
	case 2: v = l.s & 0xf; break;
	case 3: v = l.s >> 4; break;
	case 4: v = l.m & 0xf; break;
	case 5: v = l.m >> 4; break;
	case 6: v = l.h & 0xf; break;
	default: v = (l.h >> 4) | (mtc_rate_code(r) << 1); break;
	}
	return (uint8_t)((piece << 4) | (v & 0xf));
}

void mtc_full_frame(const struct tc_rate *r, int64_t frames, uint8_t out[10])
{
	struct tc_label l;
	tc_frames_to_label(r, frames, &l);
	const uint8_t m[10] = { 0xF0, 0x7F, 0x7F, 0x01, 0x01, (uint8_t)((mtc_rate_code(r) << 5) | l.h),
				(uint8_t)l.m, (uint8_t)l.s, (uint8_t)l.f, 0xF7 };
	memcpy(out, m, 10);
}

void mtc_rx_init(struct mtc_rx *m)
{
	memset(m, 0, sizeof(*m));
	m->rate_code = -1;
}

bool mtc_rx_quarter(struct mtc_rx *m, uint8_t data, int64_t t, int64_t *ref_q4)
{
	int piece = data >> 4;
	bool fresh = false, in_order;
	if (piece == 0) {
		m->have = 1;
		m->pieces[0] = data & 0xf;
		m->next_piece = 1;
		m->piece0_time = t;
		if (m->valid)
			m->frames += 2;     /* the next sequence, two frames on */
		in_order = true;
	} else if (piece == m->next_piece && (m->have & 1)) {
		m->pieces[piece] = data & 0xf;
		m->have |= (uint8_t)(1 << piece);
		m->next_piece = piece + 1;
		in_order = true;
	} else {
		m->have = 0;
		m->next_piece = 0;
		in_order = false;
	}
	if (in_order && piece == 7) {
		int code = (m->pieces[7] >> 1) & 3;
		struct tc_rate r;
		if (tc_rate_from_mtc_code(code, &r)) {
			struct tc_label l = {
				m->pieces[6] | ((m->pieces[7] & 1) << 4),
				m->pieces[4] | (m->pieces[5] << 4),
				m->pieces[2] | (m->pieces[3] << 4),
				m->pieces[0] | (m->pieces[1] << 4),
			};
			int64_t d = tc_label_to_frames(&r, &l);
			fresh = !m->valid || d != m->frames;
			m->rate_code = code;
			m->frames = d;
			m->frames_time = m->piece0_time;
			m->valid = true;
		}
	}
	if (ref_q4)
		*ref_q4 = m->valid && in_order && (m->have & 1) ? 4 * m->frames + piece : -1;
	return fresh;
}

bool mtc_rx_full(const uint8_t *msg, int n, int *rate_code, int64_t *frames)
{
	if (n < 10 || msg[0] != 0xF0 || msg[1] != 0x7F || msg[3] != 0x01 || msg[4] != 0x01 || msg[9] != 0xF7)
		return false;
	int code = (msg[5] >> 5) & 3;
	struct tc_rate r;
	if (!tc_rate_from_mtc_code(code, &r))
		return false;
	struct tc_label l = { msg[5] & 0x1f, msg[6], msg[7], msg[8] };
	*rate_code = code;
	*frames = tc_label_to_frames(&r, &l);
	return true;
}
