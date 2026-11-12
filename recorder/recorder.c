/* backline-recorder: record N PipeWire nodes to sample-aligned, timestamped files.
 * SPDX-License-Identifier: MIT
 *
 *   backline-recorder -s NODE[:CHANNELS][@LATENCY] [-s ...] -o DIR [-n TAKE]
 *                     [-f wav|wav24|flac] [-d SECONDS] [-a signal|graph]
 *                     [-P SECONDS] [-L off|hardware|reported] [-R SECONDS]
 *
 * The one-shot front end of the recorder engine (rec_engine.h); the daemon
 * with the D-Bus API is backline-recorderd. For a sink the recorder takes its
 * monitor ports, for anything else its audio output ports, and it makes its
 * own links (they die with it).
 *
 * Pre-roll (-P): arm and keep the last SECONDS; the take starts on SIGUSR1
 * or "record" on stdin, and includes them. SIGINT/SIGTERM or "stop" stop on a
 * cycle boundary. "marker NAME" on stdin (or SIGUSR2) adds a marker.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/utils/string.h>

#include "rec_engine.h"

static struct {
	struct pw_main_loop *loop;
	struct rec_session *s;
	struct spa_source *stdin_src;
	bool started, done;
	int exit_code;
	int markers;
	double pre_roll;
} M;

static void ev_armed(void *d)
{
	struct rec_session *s = M.s;
	int links = 0;
	for (int t = 0; t < rec_session_n_tracks(s); t++) {
		int64_t l = rec_session_track_latency(s, t);
		if (l)
			fprintf(stderr, "backline-recorder: track %d (%s): input latency compensation %" PRId64 " frames\n",
				t + 1, rec_session_track_node(s, t), l);
		links++;
	}
	if (M.pre_roll > 0)
		fprintf(stderr, "backline-recorder: armed, %d tracks; standing by with %.2f s pre-roll (SIGUSR1 or 'record' on stdin to start)\n",
			links, M.pre_roll);
	else
		fprintf(stderr, "backline-recorder: armed, %d tracks\n", links);
}

static void ev_started(void *d) { M.started = true; fprintf(stderr, "backline-recorder: recording\n"); }

static void ev_finished(void *d, const char *sidecar)
{
	M.done = true;
	if (sidecar)
		printf("%s\n", sidecar);
	else {
		fprintf(stderr, "backline-recorder: never started (sources missing, or no record request)\n");
		M.exit_code = 1;
	}
	uint64_t o = rec_session_overruns(M.s);
	if (o)
		fprintf(stderr, "backline-recorder: WARNING %" PRIu64 " frames lost to overruns\n", o);
	pw_main_loop_quit(M.loop);
}

static const struct rec_events events = { ev_armed, ev_started, ev_finished };

static void marker(const char *name)
{
	uint64_t f;
	char n[64];
	if (!name || !*name) {
		snprintf(n, sizeof(n), "marker %d", ++M.markers);
		name = n;
	}
	if (rec_session_marker(M.s, name, &f) == 0)
		fprintf(stderr, "backline-recorder: marker \"%s\" at frame %" PRIu64 "\n", name, f);
}

static void on_signal(void *data, int sig)
{
	if (!M.s)
		return;
	if (sig == SIGUSR1) {
		if (rec_session_record(M.s) == 0)
			fprintf(stderr, "backline-recorder: record\n");
		return;
	}
	if (sig == SIGUSR2) {
		marker(NULL);
		return;
	}
	rec_session_stop(M.s);
}

static void on_stdin(void *data, int fd, uint32_t mask)
{
	char buf[512];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		pw_loop_destroy_source(pw_main_loop_get_loop(M.loop), M.stdin_src);
		M.stdin_src = NULL;
		return;
	}
	buf[n] = 0;
	for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
		if (!strncmp(line, "record", 6))
			on_signal(NULL, SIGUSR1);
		else if (!strncmp(line, "marker", 6))
			marker(line[6] == ' ' ? line + 7 : NULL);
		else if (!strncmp(line, "stop", 4))
			on_signal(NULL, SIGINT);
	}
}

static void usage(FILE *f)
{
	fputs("usage: backline-recorder -s NODE[:CHANNELS][@LATENCY] [-s ...] -o DIR [-n TAKE] [-f wav|wav24|flac]\n"
	      "                         [-d SECONDS] [-a signal|graph] [-P SECONDS] [-L off|hardware|reported] [-R SECONDS]\n"
	      "  -s NODE[:CH]  a track: a sink's monitor, or any node's audio outputs (default 2 ch);\n"
	      "                @LATENCY sets that input's capture latency in frames by hand\n"
	      "  -f FORMAT     wav (float32, RF64 past 4 GB; default), wav24, flac\n"
	      "  -d SECONDS    take length (default: until SIGINT/SIGTERM or 'stop' on stdin)\n"
	      "  -a signal     line up the signal across tracks (default); -a graph: raw graph cycles\n"
	      "  -P SECONDS    pre-roll: stand by and keep the last SECONDS; start on SIGUSR1 or 'record'\n"
	      "  -L MODE       input-latency compensation: off, hardware (physical inputs; default), reported\n"
	      "  -R SECONDS    ring buffer per track (default 30): how long the disk may stall\n"
	      "  stdin: record | stop | marker NAME     SIGUSR2: marker\n", f);
}

int main(int argc, char **argv)
{
	struct rec_options o;
	struct rec_source src[REC_MAX_TRACKS];
	int n_src = 0, c;
	rec_options_defaults(&o);

	while ((c = getopt(argc, argv, "s:o:n:f:d:a:P:L:R:h")) != -1) {
		switch (c) {
		case 's': {
			if (n_src == REC_MAX_TRACKS) { fprintf(stderr, "too many tracks\n"); return 2; }
			struct rec_source *s = &src[n_src++];
			memset(s, 0, sizeof(*s));
			s->channels = 2;
			s->latency_frames = -1;
			char *at = strrchr(optarg, '@');
			if (at && at[1] && strspn(at + 1, "0123456789") == strlen(at + 1)) {
				s->latency_frames = atoll(at + 1);
				*at = 0;
			}
			char *colon = strrchr(optarg, ':');
			if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
				s->channels = SPA_CLAMP(atoi(colon + 1), 1, REC_MAX_CH);
				*colon = 0;
			}
			snprintf(s->node, sizeof(s->node), "%s", optarg);
			break;
		}
		case 'o': snprintf(o.dir, sizeof(o.dir), "%s", optarg); break;
		case 'n': snprintf(o.take, sizeof(o.take), "%s", optarg); break;
		case 'f': snprintf(o.format, sizeof(o.format), "%s", optarg); break;
		case 'd': o.duration = atof(optarg); break;
		case 'a':
			if (spa_streq(optarg, "signal")) o.align_signal = true;
			else if (spa_streq(optarg, "graph")) o.align_signal = false;
			else { usage(stderr); return 2; }
			break;
		case 'P': o.pre_roll = atof(optarg); break;
		case 'L':
			if (spa_streq(optarg, "off")) o.latency = REC_LATENCY_OFF;
			else if (spa_streq(optarg, "hardware")) o.latency = REC_LATENCY_HARDWARE;
			else if (spa_streq(optarg, "reported")) o.latency = REC_LATENCY_REPORTED;
			else { usage(stderr); return 2; }
			break;
		case 'R': o.ring_seconds = atof(optarg); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (n_src == 0) { usage(stderr); return 2; }
	o.wait_record = o.pre_roll > 0;
	M.pre_roll = o.pre_roll;

	pw_init(&argc, &argv);
	M.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(M.loop);
	pw_loop_add_signal(l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(l, SIGTERM, on_signal, NULL);
	pw_loop_add_signal(l, SIGUSR1, on_signal, NULL);
	pw_loop_add_signal(l, SIGUSR2, on_signal, NULL);
	if (!isatty(0) || o.pre_roll > 0 || o.duration <= 0)
		M.stdin_src = pw_loop_add_io(l, 0, SPA_IO_IN | SPA_IO_HUP, false, on_stdin, NULL);
	struct pw_context *ctx = pw_context_new(l, NULL, 0);
	struct pw_core *core = pw_context_connect(ctx, NULL, 0);
	if (!core) { fprintf(stderr, "cannot connect to PipeWire\n"); return 1; }

	char err[256];
	M.s = rec_session_new(core, l, src, n_src, &o, &events, NULL, err, sizeof(err));
	if (!M.s) {
		fprintf(stderr, "backline-recorder: %s\n", err);
		return 2;
	}
	pw_main_loop_run(M.loop);
	rec_session_destroy(M.s);
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_main_loop_destroy(M.loop);
	pw_deinit();
	return M.exit_code;
}
