/* rf64check: read a (large) take file end to end and check it.
 * SPDX-License-Identifier: MIT
 *
 *   rf64check FILE BASE_HZ [REF_FILE REF_OFFSET_FRAMES]
 *
 * With REF_FILE (a second, independent recording of the same node, whose
 * frame 0 is REF_OFFSET_FRAMES before FILE's frame 0, or after it when
 * negative) every frame of FILE is also compared with the reference: a
 * glitch the graph delivered to both recorders is in both files, a frame a
 * recorder lost is not.
 *
 * The rf64 proof feeds channel c a sine of (c + 1) x BASE_HZ. A sine obeys
 * x[n+1] = 2 cos(w) x[n] - x[n-1], so a lost, repeated or corrupted frame
 * anywhere shows up as a large residual. Prints the container, the ds64
 * sizes, frame count, and the residual statistics per channel as JSON.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <sndfile.h>

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	double base = atof(argv[2]);
	struct stat st;
	if (stat(argv[1], &st) < 0)
		return 1;
	FILE *fp = fopen(argv[1], "rb");
	unsigned char h[48] = { 0 };
	if (!fp || fread(h, 1, sizeof(h), fp) != sizeof(h))
		return 1;
	fclose(fp);
	uint64_t riff = 0, data = 0, samples = 0;
	if (!memcmp(h, "RF64", 4) && !memcmp(h + 12, "ds64", 4)) {
		memcpy(&riff, h + 20, 8);
		memcpy(&data, h + 28, 8);
		memcpy(&samples, h + 36, 8);
	}
	SF_INFO info = { 0 };
	SNDFILE *f = sf_open(argv[1], SFM_READ, &info);
	if (!f) {
		fprintf(stderr, "%s\n", sf_strerror(NULL));
		return 1;
	}
	int ch = info.channels;
	enum { CHUNK = 65536 };
	float *buf = malloc(sizeof(float) * CHUNK * ch);
	double *prev1 = calloc(ch, sizeof(double)), *prev2 = calloc(ch, sizeof(double)), *maxres = calloc(ch, sizeof(double));
	double *coef = calloc(ch, sizeof(double)), *peak = calloc(ch, sizeof(double));
	uint64_t *bad = calloc(ch, sizeof(uint64_t)), *first_bad = calloc(ch, sizeof(uint64_t));
	for (int c = 0; c < ch; c++) {
		coef[c] = 2.0 * cos(2.0 * M_PI * base * (c + 1) / info.samplerate);
		first_bad[c] = UINT64_MAX;
	}
	uint64_t n = 0, ref_cmp = 0, ref_diff = 0;
	int64_t ref_first_diff = -1;
	SNDFILE *rf = NULL;
	float *rbuf = NULL;
	if (argc >= 5) {
		SF_INFO ri = { 0 };
		rf = sf_open(argv[3], SFM_READ, &ri);
		long long off = atoll(argv[4]);
		if (!rf || ri.channels != ch)
			return 1;
		rbuf = malloc(sizeof(float) * CHUNK * ch);
		if (off >= 0)
			sf_seek(rf, off, SEEK_SET);
		else
			sf_seek(f, -off, SEEK_SET), n = (uint64_t)-off;
	}
	sf_count_t got;
	while ((got = sf_readf_float(f, buf, CHUNK)) > 0) {
		if (rf) {
			sf_count_t rg = sf_readf_float(rf, rbuf, got);
			for (sf_count_t i = 0; i < rg; i++) {
				ref_cmp++;
				if (memcmp(buf + i * ch, rbuf + i * ch, sizeof(float) * ch)) {
					ref_diff++;
					if (ref_first_diff < 0)
						ref_first_diff = (int64_t)(n + i);
				}
			}
		}
		for (sf_count_t i = 0; i < got; i++, n++) {
			for (int c = 0; c < ch; c++) {
				double x = buf[i * ch + c];
				if (fabs(x) > peak[c])
					peak[c] = fabs(x);
				if (n >= 2) {
					double r = fabs(x - (coef[c] * prev1[c] - prev2[c]));
					if (r > maxres[c])
						maxres[c] = r;
					if (r > 1e-3) {
						bad[c]++;
						if (first_bad[c] == UINT64_MAX)
							first_bad[c] = n;
					}
				}
				prev2[c] = prev1[c];
				prev1[c] = x;
			}
		}
	}
	sf_close(f);
	if (rf)
		sf_close(rf);
	uint64_t total_bad = 0;
	double worst = 0;
	for (int c = 0; c < ch; c++) {
		total_bad += bad[c];
		if (maxres[c] > worst)
			worst = maxres[c];
	}
	printf("{\"file_bytes\": %" PRIu64 ", \"container\": \"%.4s\", \"ds64\": {\"riff_size\": %" PRIu64 ", \"data_size\": %" PRIu64
	       ", \"sample_count\": %" PRIu64 "}, \"rate\": %d, \"channels\": %d, \"frames_header\": %" PRId64 ", \"frames_read\": %" PRIu64
	       ", \"discontinuities\": %" PRIu64 ", \"max_residual\": %.3g, \"min_channel_peak\": %.4f, \"first_bad_frame\": %" PRId64
	       ", \"ref_compared\": %" PRIu64 ", \"ref_differ\": %" PRIu64 ", \"ref_first_diff\": %" PRId64 "}\n",
	       (uint64_t)st.st_size, (const char *)h, riff, data, samples, info.samplerate, ch, (int64_t)info.frames, n, total_bad, worst,
	       ({ double m = 1; for (int c = 0; c < ch; c++) if (peak[c] < m) m = peak[c]; m; }),
	       ({ int64_t fb = -1; for (int c = 0; c < ch; c++) if (first_bad[c] != UINT64_MAX && (fb < 0 || (int64_t)first_bad[c] < fb)) fb = (int64_t)first_bad[c]; fb; }),
	       ref_cmp, ref_diff, ref_first_diff);
	return 0;
}
