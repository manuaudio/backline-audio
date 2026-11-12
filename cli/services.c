/* backline CLI: the recorder daemon and the clock (rec ..., transport ...).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "backline/backline.h"
#include "services.h"

static int fail(bl_client *c, const char *what)
{
	fprintf(stderr, "backline: %s: %s\n", what, bl_client_error(c));
	return 1;
}

static void print_json_str(const char *s)
{
	json_object *o = json_tokener_parse(s);
	puts(o ? json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE) : s);
	json_object_put(o);
}

static void ok_json(bool json, const char *key, const char *val)
{
	if (!json)
		return;
	json_object *o = json_object_new_object();
	json_object_object_add(o, key, json_object_new_string(val));
	puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
}

bool cli_rec_is_service(const char *sub)
{
	static const char *subs[] = { "arm", "record", "stop", "marker", "takes", "take", "soundcheck", "input", "status", NULL };
	for (int i = 0; subs[i]; i++)
		if (sub && !strcmp(sub, subs[i]))
			return true;
	return false;
}

int cli_rec(bl_client *c, bool json, int argc, char **argv)
{
	const char *sub = argv[0];
	if (!strcmp(sub, "arm")) {
		bl_rec_options o = BL_REC_OPTIONS_INIT;
		const char *src[64];
		size_t n = 0;
		for (int i = 1; i < argc; i++) {
			const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
			if (!strcmp(a, "--dir") && v) o.dir = argv[++i];
			else if (!strcmp(a, "--name") && v) o.name = argv[++i];
			else if (!strcmp(a, "--format") && v) o.format = argv[++i];
			else if (!strcmp(a, "--align") && v) o.align = argv[++i];
			else if (!strcmp(a, "--latency") && v) o.latency = argv[++i];
			else if (!strcmp(a, "--pre-roll") && v) o.pre_roll = atof(argv[++i]);
			else if (!strcmp(a, "--duration") && v) o.duration = atof(argv[++i]);
			else if (!strcmp(a, "--ring-seconds") && v) o.ring_seconds = atof(argv[++i]);
			else if (a[0] != '-' && n < 64) src[n++] = a;
			else { fprintf(stderr, "backline rec arm: unknown option %s\n", a); return 2; }
		}
		if (!n) {
			fprintf(stderr, "usage: backline rec arm NODE[:CH]... [--dir D] [--name N] [--format wav|wav24|flac] [--pre-roll S]\n"
					"         [--duration S] [--align signal|graph] [--latency off|hardware|reported] [--ring-seconds S]\n");
			return 2;
		}
		char take[256];
		if (bl_recorder_arm(c, src, n, &o, take, sizeof(take)) < 0)
			return fail(c, "arm");
		json ? ok_json(true, "take", take) : (void)printf("armed: %s\n", take);
		return 0;
	}
	if (!strcmp(sub, "record")) {
		char take[256];
		if (bl_recorder_record(c, take, sizeof(take)) < 0)
			return fail(c, "record");
		json ? ok_json(true, "take", take) : (void)printf("recording: %s\n", take);
		return 0;
	}
	if (!strcmp(sub, "stop")) {
		char *sc = NULL;
		if (bl_recorder_stop(c, &sc) < 0)
			return fail(c, "stop");
		json ? ok_json(true, "sidecar", sc) : (void)printf("%s\n", *sc ? sc : "disarmed");
		bl_free(sc);
		return 0;
	}
	if (!strcmp(sub, "marker")) {
		uint64_t f;
		if (bl_recorder_marker(c, argc > 1 ? argv[1] : "", &f) < 0)
			return fail(c, "marker");
		json ? (void)printf("{\"frame\": %" PRIu64 "}\n", f) : (void)printf("marker at frame %" PRIu64 "\n", f);
		return 0;
	}
	if (!strcmp(sub, "takes") || !strcmp(sub, "take")) {
		char *j = NULL;
		int r = !strcmp(sub, "takes") ? bl_recorder_takes(c, &j) : argc > 1 ? bl_recorder_take(c, argv[1], &j) : -22;
		if (r == -22) {
			fprintf(stderr, "usage: backline rec take TAKE\n");
			return 2;
		}
		if (r < 0)
			return fail(c, sub);
		if (json || !strcmp(sub, "take")) {
			print_json_str(j);
		} else {
			json_object *o = json_tokener_parse(j), *arr;
			if (o && json_object_object_get_ex(o, "takes", &arr))
				for (size_t i = 0; i < json_object_array_length(arr); i++) {
					json_object *t = json_object_array_get_idx(arr, i), *v;
					printf("%-32s %8.1f s  %s  %d track(s)\n",
					       json_object_object_get_ex(t, "take", &v) ? json_object_get_string(v) : "?",
					       json_object_object_get_ex(t, "seconds", &v) ? json_object_get_double(v) : 0.0,
					       json_object_object_get_ex(t, "utc", &v) ? json_object_get_string(v) : "",
					       json_object_object_get_ex(t, "tracks", &v) ? (int)json_object_array_length(v) : 0);
				}
			json_object_put(o);
		}
		bl_free(j);
		return 0;
	}
	if (!strcmp(sub, "soundcheck")) {
		if (argc > 1 && !strcmp(argv[1], "stop")) {
			if (bl_recorder_stop_soundcheck(c) < 0)
				return fail(c, "soundcheck stop");
			return 0;
		}
		if (argc < 2) {
			fprintf(stderr, "usage: backline rec soundcheck TAKE [--once] | backline rec soundcheck stop\n");
			return 2;
		}
		int loop = argc > 2 && !strcmp(argv[2], "--once") ? 0 : -1;
		if (bl_recorder_soundcheck(c, argv[1], loop, 0) < 0)
			return fail(c, "soundcheck");
		return 0;
	}
	if (!strcmp(sub, "input")) {
		if (argc < 3) {
			fprintf(stderr, "usage: backline rec input NODE live|playback\n");
			return 2;
		}
		if (bl_recorder_set_input(c, argv[1], argv[2]) < 0)
			return fail(c, "input");
		return 0;
	}
	/* status */
	bl_rec_status st;
	if (bl_recorder_status(c, &st) < 0)
		return fail(c, "status");
	if (json)
		printf("{\"state\": \"%s\", \"take\": \"%s\", \"rate\": %u, \"frames\": %" PRIu64 ", \"overruns\": %" PRIu64
		       ", \"soundcheck\": \"%s\", \"last_take\": \"%s\"}\n",
		       st.state, st.take, st.rate, st.frames, st.overruns, st.soundcheck, st.last_take);
	else
		printf("%s%s%s, %" PRIu64 " frames, %" PRIu64 " overruns%s%s\n", st.state, *st.take ? " " : "", st.take, st.frames,
		       st.overruns, *st.soundcheck ? "; soundcheck " : "", st.soundcheck);
	return 0;
}

int cli_transport(bl_client *c, bool json, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "status";
	int r = 0;
	if (!strcmp(sub, "play")) r = bl_clock_play(c);
	else if (!strcmp(sub, "stop")) r = bl_clock_stop(c);
	else if (!strcmp(sub, "locate") && argc > 1) {
		uint64_t f;
		if ((r = bl_clock_locate(c, argv[1], &f)) >= 0 && !json)
			printf("frame %" PRIu64 "\n", f);
	} else if (!strcmp(sub, "tempo") && argc > 1) r = bl_clock_set_tempo(c, atof(argv[1]));
	else if (!strcmp(sub, "meter") && argc > 2) r = bl_clock_set_meter(c, atof(argv[1]), atof(argv[2]));
	else if (!strcmp(sub, "link") && argc > 1) r = bl_clock_link(c, !strcmp(argv[1], "on"));
	else if ((!strcmp(sub, "ltc") || !strcmp(sub, "mtc")) && argc > 1) {
		double fps = 0;
		const char *target = NULL, *source = NULL;
		for (int i = 2; i + 1 < argc; i += 2) {
			if (!strcmp(argv[i], "--fps")) fps = atof(argv[i + 1]);
			else if (!strcmp(argv[i], "--target")) target = argv[i + 1];
			else if (!strcmp(argv[i], "--source")) source = argv[i + 1];
		}
		r = bl_clock_timecode(c, sub[0] == 'l', argv[1], fps, target, source);
	} else if (strcmp(sub, "status")) {
		fprintf(stderr, "usage: backline transport play|stop|locate POS|tempo BPM|meter N D|link on|off|\n"
				"                          ltc|mtc off|generate|chase [--fps F] [--target NODE] [--source NODE]|status\n");
		return 2;
	}
	if (r < 0)
		return fail(c, sub);
	if (strcmp(sub, "status"))
		return 0;
	bl_clock_status st;
	if (bl_clock_get(c, &st) < 0)
		return fail(c, "status");
	if (json)
		printf("{\"rolling\": %s, \"frame\": %" PRIu64 ", \"rate\": %u, \"timecode\": \"%s\", \"bbt\": \"%s\", \"tempo\": %.6g, "
		       "\"meter\": [%g, %g], \"fps\": %.4g, \"source\": \"%s\", \"link\": %s, \"link_peers\": %u, \"ltc\": \"%s\", "
		       "\"mtc\": \"%s\", \"locked\": %s, \"chase_error\": %" PRId64 "}\n",
		       st.rolling ? "true" : "false", st.frame, st.rate, st.timecode, st.bbt, st.tempo, st.beats_per_bar, st.beat_type,
		       st.fps, st.source, st.link_enabled ? "true" : "false", st.link_peers, st.ltc_mode, st.mtc_mode,
		       st.locked ? "true" : "false", st.chase_error);
	else
		printf("%s  %s  %s  %.2f BPM %g/%g  source %s  link %s (%u peers)%s%s\n", st.rolling ? "rolling" : "stopped",
		       st.timecode, st.bbt, st.tempo, st.beats_per_bar, st.beat_type, st.source, st.link_enabled ? "on" : "off",
		       st.link_peers, st.locked ? "  locked" : "", strcmp(st.source, "ltc") && strcmp(st.source, "mtc") ? "" : "  chasing");
	return 0;
}
