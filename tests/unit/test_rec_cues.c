/* Unit test: markers as WAV cue chunks, for plain WAV, extensible WAV and RF64.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sndfile.h>

#include "rec_engine.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void make(const char *path, int major, int downgrade)
{
	SF_INFO i = { .samplerate = 48000, .channels = 2, .format = major | SF_FORMAT_FLOAT };
	SNDFILE *f = sf_open(path, SFM_WRITE, &i);
	CHECK(f != NULL);
	if (!f)
		return;
	if (major == SF_FORMAT_RF64)
		sf_command(f, SFC_RF64_AUTO_DOWNGRADE, NULL, downgrade ? SF_TRUE : SF_FALSE);
	float buf[2 * 1000];
	for (int k = 0; k < 2000; k++)
		buf[k] = (float)k / 2000.0f;
	for (int k = 0; k < 48; k++)
		sf_writef_float(f, buf, 1000);
	sf_close(f);
}

static void check(const char *path, const char *magic)
{
	struct rec_marker m[3] = { { "intro", 0, 0 }, { "verse one", 12345, 0 }, { "odd", 47999, 0 } };
	CHECK(rec_write_cues(path, m, 3) == 0);
	FILE *fp = fopen(path, "rb");
	char hdr[4] = { 0 };
	CHECK(fp && fread(hdr, 1, 4, fp) == 4);
	if (fp)
		fclose(fp);
	CHECK(!memcmp(hdr, magic, 4));

	SF_INFO i = { 0 };
	SNDFILE *f = sf_open(path, SFM_READ, &i);
	CHECK(f != NULL);
	if (!f)
		return;
	CHECK(i.frames == 48000);
	float buf[2 * 100];
	CHECK(sf_seek(f, 47900, SEEK_SET) == 47900);
	CHECK(sf_readf_float(f, buf, 100) == 100);   /* the audio is still all there */
	SF_CUES cues;
	memset(&cues, 0, sizeof(cues));
	int r = sf_command(f, SFC_GET_CUE, &cues, sizeof(cues));
	if (strcmp(magic, "RF64")) {      /* libsndfile 1.2 reads cues from RIFF only */
		CHECK(r == SF_TRUE);
		CHECK(cues.cue_count == 3);
		CHECK(cues.cue_points[1].sample_offset == 12345);
		CHECK(!strcmp(cues.cue_points[1].name, "verse one"));
		CHECK(cues.cue_points[2].sample_offset == 47999);
	}
	sf_close(f);
}

int main(void)
{
	char a[] = "/tmp/bl-cues-XXXXXX";
	int fd = mkstemp(a);
	close(fd);
	make(a, SF_FORMAT_WAV, 0);
	check(a, "RIFF");
	make(a, SF_FORMAT_RF64, 1);   /* what the recorder writes below 4 GB */
	check(a, "RIFF");
	make(a, SF_FORMAT_RF64, 0);   /* a real RF64 header (ds64) */
	check(a, "RF64");
	/* not a WAV */
	FILE *f = fopen(a, "wb");
	fputs("hello, not a wav file at all, but long enough", f);
	fclose(f);
	struct rec_marker m = { "x", 1, 0 };
	CHECK(rec_write_cues(a, &m, 1) < 0);
	unlink(a);
	if (fails)
		return 1;
	puts("rec_cues: ok");
	return 0;
}
