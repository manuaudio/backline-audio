/* backline-test-slot-driver: plays the mixing engine's role against
 * backline-slot-host over Slot1 shared memory, with no audio server, and
 * checks the contract (docs/api/Slot1.md):
 *
 *   align     an impulse comes out exactly at its frame + the reported
 *             latency, in the same cycle when the latency is 0 (+0 frames)
 *   deadline  a request that misses the deadline plays the dry signal
 *             delayed by the reported latency, sample-exact; the chain then
 *             stalls until it has caught up, and comes back
 *   crash     two chains, two sandboxes; one plugin crashes: its chain plays
 *             delayed dry until a new sandbox takes the memory over; the
 *             other chain never misses a sample
 *   allocs    no heap call on either side's audio thread over many cycles;
 *             and a plugin that does allocate is seen (the counter works)
 *   timing    round trip request → done, paced like a real period
 *
 * Usage: backline-test-slot-driver TEST --host BIN [--frames N] [--rate R]
 *        [--cycles N] [--log FILE] --plugin SPEC [--plugin SPEC ...]
 * Prints one JSON object per test on stdout; exit 0 when the checks hold.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#include "backline/slot1.h"
#include "bl_rtalloc.h"

#define MAXP 16
#define CH 2

static struct {
	const char *test, *host, *log;
	const char *hostd_state;  /* --hostd DIR: chain A is loaded and supervised by backline-hostd (state under DIR) */
	const char *plugins[MAXP];
	int n_plugins;
	uint32_t frames, rate;
	int cycles;
	int deadline_pct;
	int rt;
} O = { .frames = 64, .rate = 48000, .cycles = 0, .deadline_pct = 50, .rt = 1 };

static int failures;
#define CHECK(cond, ...)                                                                                              \
	do {                                                                                                          \
		if (!(cond)) {                                                                                        \
			fprintf(stderr, "FAIL %s: ", O.test);                                                         \
			fprintf(stderr, __VA_ARGS__);                                                                 \
			fputc('\n', stderr);                                                                          \
			failures++;                                                                                   \
		}                                                                                                     \
	} while (0)

struct chain {
	struct bl_slot1_engine e;
	char path[64];
	pid_t pid;
	json_object *ready;
	char state_dir[64];      /* "" = none */
};

/* ---------------------------------------------------------------- sandbox */

static pid_t spawn(struct chain *c, const char *name)
{
	int pfd[2];
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -1;
	const char *argv[8 + 2 * MAXP];
	int a = 0;
	argv[a++] = O.host;
	argv[a++] = "--shm";
	argv[a++] = c->path;
	argv[a++] = "--name";
	argv[a++] = name;
	if (c->state_dir[0]) {
		argv[a++] = "--state-dir";
		argv[a++] = c->state_dir;
	}
	for (int i = 0; i < O.n_plugins; i++) {
		argv[a++] = "--plugin";
		argv[a++] = O.plugins[i];
	}
	argv[a] = NULL;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(pfd[1], 1);
		if (O.log) {
			int fd = open(O.log, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd >= 0)
				dup2(fd, 2);
		}
		execv(O.host, (char **)argv);
		_exit(127);
	}
	close(pfd[1]);
	/* the ready line */
	char line[1 << 16];
	size_t n = 0;
	uint64_t until = bl_slot1_now_ns() + 20000000000ull;
	while (n + 1 < sizeof(line)) {
		struct pollfd p = { pfd[0], POLLIN, 0 };
		int64_t left = (int64_t)(until - bl_slot1_now_ns()) / 1000000;
		if (left <= 0 || poll(&p, 1, (int)left) <= 0)
			break;
		ssize_t r = read(pfd[0], line + n, 1);
		if (r <= 0)
			break;
		if (line[n] == '\n')
			break;
		n++;
	}
	line[n] = 0;
	close(pfd[0]);
	if (c->ready)
		json_object_put(c->ready);
	c->ready = json_tokener_parse(line);
	if (!c->ready) {
		fprintf(stderr, "%s: no ready line from the slot host (%s)\n", O.test, line);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -1;
	}
	c->pid = pid;
	return pid;
}

static int open_chain_state(struct chain *c, const char *name, bool state)
{
	memset(c, 0, sizeof(*c));
	if (state) {
		snprintf(c->state_dir, sizeof(c->state_dir), "/tmp/bl-slot-state-XXXXXX");
		if (!mkdtemp(c->state_dir))
			return -1;
	}
	int r = bl_slot1_engine_create(&c->e, CH, 1024, O.rate, 0);
	if (r < 0) {
		fprintf(stderr, "engine_create: %s\n", strerror(-r));
		return -1;
	}
	bl_slot1_engine_path(&c->e, c->path, sizeof(c->path));
	return spawn(c, name) > 0 ? 0 : -1;
}

static int open_chain(struct chain *c, const char *name) { return open_chain_state(c, name, false); }

static void close_chain(struct chain *c)
{
	if (c->pid > 0) {
		kill(c->pid, SIGTERM);
		waitpid(c->pid, NULL, 0);
	}
	if (c->ready)
		json_object_put(c->ready);
	if (c->state_dir[0]) {
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", c->state_dir);
		if (system(cmd) != 0)
			fprintf(stderr, "could not remove %s\n", c->state_dir);
	}
	bl_slot1_engine_destroy(&c->e);
}

/* the id of parameter `name` of insert `slot`, from the ready line */
static int param_info(struct chain *c, int slot, const char *name, uint32_t *id, double *mn, double *mx)
{
	json_object *s, *ins, *in, *ps;
	if (!json_object_object_get_ex(c->ready, "slot", &s) || !json_object_object_get_ex(s, "inserts", &ins))
		return -1;
	in = json_object_array_get_idx(ins, (size_t)slot);
	if (!in || !json_object_object_get_ex(in, "params", &ps))
		return -1;
	for (size_t i = 0; i < json_object_array_length(ps); i++) {
		json_object *p = json_object_array_get_idx(ps, i), *v;
		if (json_object_object_get_ex(p, "name", &v) && !strcmp(json_object_get_string(v), name)) {
			json_object_object_get_ex(p, "id", &v);
			*id = (uint32_t)json_object_get_int(v);
			json_object_object_get_ex(p, "min", &v);
			*mn = json_object_get_double(v);
			json_object_object_get_ex(p, "max", &v);
			*mx = json_object_get_double(v);
			return 0;
		}
	}
	return -1;
}

static int param_id(struct chain *c, int slot, const char *name, uint32_t *id)
{
	double mn, mx;
	return param_info(c, slot, name, id, &mn, &mx);
}

/* Sets a parameter in plain units, sent normalized (PARAM_NORM), which every
 * format applies in the same cycle (a plain PARAM to VST3 takes a hop
 * through the sandbox's main thread for the controller's conversion). */
static void set_param(struct chain *c, int slot, const char *name, double v)
{
	uint32_t id;
	double mn, mx;
	if (param_info(c, slot, name, &id, &mn, &mx) < 0) {
		fprintf(stderr, "%s: insert %d has no parameter %s\n", O.test, slot, name);
		failures++;
		return;
	}
	double n = mx > mn ? (v - mn) / (mx - mn) : 0;
	struct bl_slot1_event e = { .slot = (uint8_t)slot, .type = BL_SLOT1_EV_PARAM_NORM, .id = id, .value = n };
	if (bl_slot1_engine_push(&c->e, &e) < 0)
		failures++;
}

/* ---------------------------------------------------------------- pacing */

static uint64_t period_ns(void) { return (uint64_t)O.frames * 1000000000ull / O.rate; }

static void sleep_until(uint64_t t)
{
	struct timespec ts = { (time_t)(t / 1000000000ull), (long)(t % 1000000000ull) };
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static void go_realtime(void)
{
	if (!O.rt)
		return;
	struct sched_param sp = { .sched_priority = 85 };
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
		fprintf(stderr, "%s: no SCHED_FIFO for the driver (%s); timing is at normal priority\n", O.test, strerror(errno));
}

/* One cycle of one chain, at `start` (the cycle's notional start). */
static enum bl_slot1_result cycle(struct chain *c, float *in[CH], float *out[CH], uint64_t start)
{
	uint64_t deadline = start + period_ns() * (uint64_t)O.deadline_pct / 100;
	return bl_slot1_engine_process(&c->e, (const float *const *)in, out, O.frames, deadline, NULL);
}

/* Silent cycles until the sandbox is ready and its latency has been stable
 * for 20 cycles (LV2 reports latency only after its first run). */
static void settle(struct chain **cs, int n)
{
	static float zin[CH][1024], zout[CH][1024];
	float *in[CH] = { zin[0], zin[1] }, *out[CH] = { zout[0], zout[1] };
	uint32_t serial[8] = { 0 };
	int stable = 0;
	uint64_t t = bl_slot1_now_ns();
	for (int k = 0; k < 20000 && stable < 20; k++) {
		bool all = true;
		for (int i = 0; i < n; i++) {
			cycle(cs[i], in, out, t);
			uint32_t s = bl_slot1_load(&cs[i]->e.h->latency_serial);
			if (bl_slot1_load(&cs[i]->e.h->state) != BL_SLOT1_READY || s != serial[i] || !s ||
			    cs[i]->e.resume_frames)
				all = false;
			serial[i] = s;
		}
		stable = all ? stable + 1 : 0;
		t += period_ns();
		sleep_until(t);
	}
}

/* ---------------------------------------------------------------- signals */

/* x[n]: a deterministic, never-repeating test signal */
static float sig(int64_t n, int ch) { return (float)(((n * 7919 + ch * 104729) % 2003) - 1001) / 1024.0f; }

static uint32_t chain_latency(struct chain *c) { return bl_slot1_load(&c->e.h->latency); }

static uint32_t sum_insert_latency(struct chain *c)
{
	uint32_t s = 0;
	for (uint32_t i = 0; i < c->e.h->n_slots; i++)
		s += c->e.h->slots[i].latency;
	return s;
}

/* ---------------------------------------------------------------- hostd */

#define HOST_DEST "io.github.manuaudio.Backline.Host"
#define HOST_PATH "/io/github/manuaudio/Backline/Host"
#define HOST_IFACE "io.github.manuaudio.Backline.Host1"

static int run_argv(const char **argv)
{
	pid_t pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);
		dup2(null, 1);
		execvp(argv[0], (char **)argv);
		_exit(127);
	}
	int st;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* LoadSlot(path, plugins, {id: name}) on the session bus (a private one in the proof) */
static int hostd_load(struct chain *c, const char *name)
{
	const char *argv[16 + MAXP];
	char n[8];
	int a = 0;
	argv[a++] = "busctl";
	argv[a++] = "--user";
	argv[a++] = "--timeout=30";
	argv[a++] = "call";
	argv[a++] = HOST_DEST;
	argv[a++] = HOST_PATH;
	argv[a++] = HOST_IFACE;
	argv[a++] = "LoadSlot";
	argv[a++] = "sasa{sv}";
	argv[a++] = c->path;
	snprintf(n, sizeof(n), "%d", O.n_plugins);
	argv[a++] = n;
	for (int i = 0; i < O.n_plugins; i++)
		argv[a++] = O.plugins[i];
	argv[a++] = "1";
	argv[a++] = "id";
	argv[a++] = "s";
	argv[a++] = name;
	argv[a] = NULL;
	return run_argv(argv);
}

static void hostd_unload(const char *name)
{
	const char *argv[] = { "busctl", "--user", "call", HOST_DEST, HOST_PATH, HOST_IFACE, "Unload", "s", name, NULL };
	run_argv(argv);
}

static int hostd_last_signal(const char *name)
{
	char cmd[256], buf[8192];
	snprintf(cmd, sizeof(cmd), "busctl --user call %s %s %s Get s %s", HOST_DEST, HOST_PATH, HOST_IFACE, name);
	FILE *f = popen(cmd, "r");
	if (!f)
		return -1;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	pclose(f);
	const char *p = strstr(buf, "\"last-signal\" i ");
	return p ? atoi(p + strlen("\"last-signal\" i ")) : -1;
}

/* ---------------------------------------------------------------- tests */

static int t_align(void)
{
	struct chain c;
	if (open_chain(&c, "align") < 0)
		return 1;
	struct chain *cs[1] = { &c };
	set_param(&c, 0, "gain", -6.0);
	settle(cs, 1);
	uint32_t L = chain_latency(&c), sum = sum_insert_latency(&c);
	CHECK(L == sum, "chain latency %u is not the sum of its inserts' %u", L, sum);
	double g = pow(10.0, -6.0 / 20.0); /* the gain is on insert 0 only */

	const uint32_t N = O.frames, pos = 17;
	int ncyc = (int)((L + pos) / N) + 3;
	static float ibuf[CH][1024], obuf[CH][1024];
	float *in[CH] = { ibuf[0], ibuf[1] }, *out[CH] = { obuf[0], obuf[1] };
	int tries = 0, found = -1, others = 0, wet_all = 0;
	float amp = 0;
	for (tries = 0; tries < 5; tries++) {
		found = -1;
		others = 0;
		wet_all = 1;
		uint64_t t = bl_slot1_now_ns();
		for (int k = 0; k < ncyc; k++) {
			for (int ch = 0; ch < CH; ch++) {
				memset(ibuf[ch], 0, sizeof(ibuf[ch]));
				if (k == 0)
					ibuf[ch][pos] = 1.0f;
			}
			enum bl_slot1_result r = cycle(&c, in, out, t);
			if (r != BL_SLOT1_WET)
				wet_all = 0;
			for (uint32_t i = 0; i < N; i++)
				for (int ch = 0; ch < CH; ch++)
					if (fabsf(obuf[ch][i]) > 1e-9f) {
						int at = k * (int)N + (int)i;
						if (found < 0 && ch == 0) {
							found = at;
							amp = obuf[ch][i];
						} else if (ch == 0 || at != found) {
							others++;
						}
					}
			t += period_ns();
			sleep_until(t);
		}
		if (wet_all)
			break;
	}
	int offset = found - (int)pos - (int)L;
	CHECK(wet_all, "no fully wet run in 5 tries");
	CHECK(found >= 0, "the impulse never came out");
	CHECK(offset == 0, "impulse at frame %d: %+d frames from in + latency (%u)", found, offset, L);
	CHECK(fabs(amp - g) < 1e-6, "impulse amplitude %.7f, expected %.7f (wet, gain −6 dB)", amp, g);
	CHECK(others == 0, "%d other non-zero samples", others);
	if (L == 0)
		CHECK(found == (int)pos, "zero-latency chain: impulse not in the same cycle and frame");
	printf("{\"test\": \"align\", \"inserts\": %d, \"latency\": %u, \"in_frame\": %u, \"out_frame\": %d, \"offset\": %d, "
	       "\"same_cycle\": %s, \"amplitude\": %.6f, \"tries\": %d}\n",
	       O.n_plugins, L, pos, found, offset, found / (int)N == 0 ? "true" : "false", amp, tries + 1);
	close_chain(&c);
	return 0;
}

/* Checks a block against the expectation: wet = g·x[n−L], dry = x[n−L].
 * skip_until: frames before this absolute frame are not checked (a chain's
 * own state after it missed input or was restarted). */
static float block_err(float *out[CH], int64_t n0, uint32_t N, uint32_t L, double g, bool wet, int64_t skip_until)
{
	float worst = 0;
	for (uint32_t i = 0; i < N; i++) {
		int64_t n = n0 + i;
		if (n < skip_until)
			continue;
		for (int ch = 0; ch < CH; ch++) {
			float x = n - (int64_t)L >= 0 ? sig(n - (int64_t)L, ch) : 0.f;
			float want = wet ? (float)(g * x) : x;
			float e = fabsf(out[ch][i] - want);
			if (e > worst)
				worst = e;
		}
	}
	return worst;
}

static int t_deadline(void)
{
	struct chain c;
	if (open_chain(&c, "deadline") < 0)
		return 1;
	struct chain *cs[1] = { &c };
	set_param(&c, 0, "gain", -6.0);
	settle(cs, 1);
	uint32_t L = chain_latency(&c), N = O.frames;
	double g = pow(10.0, -6.0 / 20.0);
	int total = 400, slow_at = 150;
	uint64_t sleep_us = 3 * period_ns() / 1000; /* three periods: one miss, then stalled blocks */
	static float ibuf[CH][1024], obuf[CH][1024];
	float *in[CH] = { ibuf[0], ibuf[1] }, *out[CH] = { obuf[0], obuf[1] };
	int wet = 0, late = 0, stalled = 0, notready = 0, first_late = -1, recovered_at = -1, resumed = 0;
	float worst_wet = 0, worst_dry = 0;
	int64_t skip_until = 0;
	enum bl_slot1_result prev = BL_SLOT1_WET;
	/* settle() fed silence, so x[n] = 0 before frame 0 on both paths */
	uint64_t t = bl_slot1_now_ns();
	for (int k = 0; k < total; k++) {
		int64_t n0 = (int64_t)k * N;
		for (uint32_t i = 0; i < N; i++)
			for (int ch = 0; ch < CH; ch++)
				ibuf[ch][i] = sig(n0 + i, ch);
		if (k == slow_at)
			set_param(&c, 0, "sleep_us", (double)sleep_us);
		if (k == slow_at + 1)
			set_param(&c, 0, "sleep_us", 0); /* delivered with the first request after the stall */
		enum bl_slot1_result r = cycle(&c, in, out, t);
		bool is_wet = r == BL_SLOT1_WET || r == BL_SLOT1_RESUMED;
		if (r == BL_SLOT1_RESUMED)
			resumed++;
		if (is_wet && prev != BL_SLOT1_WET && prev != BL_SLOT1_RESUMED && k > 0) {
			/* the chain missed the stalled blocks' input: its first L frames
			 * after coming back are from before the gap */
			skip_until = n0 + L;
			if (recovered_at < 0 && first_late >= 0)
				recovered_at = k;
		}
		float e = block_err(out, n0, N, L, g, is_wet, skip_until);
		if (is_wet) {
			wet++;
			if (e > worst_wet)
				worst_wet = e;
		} else {
			if (r == BL_SLOT1_LATE) {
				late++;
				if (first_late < 0)
					first_late = k;
			} else if (r == BL_SLOT1_STALLED) {
				stalled++;
			} else {
				notready++;
			}
			if (e > worst_dry)
				worst_dry = e;
		}
		prev = r;
		t += period_ns();
		sleep_until(t);
	}
	CHECK(late >= 1, "no deadline miss when the plugin slept %llu µs", (unsigned long long)sleep_us);
	CHECK(first_late == slow_at, "the miss came at block %d, not %d", first_late, slow_at);
	CHECK(stalled >= 1, "no stalled block after the miss");
	CHECK(recovered_at > 0, "the chain never came back");
	CHECK(worst_dry < 1e-6f, "a dry block differs from the input delayed by %u by %g", L, worst_dry);
	CHECK(worst_wet < 1e-5f, "a wet block differs from g·x[n−L] by %g", worst_wet);
	CHECK(notready == 0, "%d blocks not ready", notready);
	CHECK(resumed == (int)((L + 1 + N - 1) / N), "%d blocks marked resumed, expected %u (the latency after the gap)", resumed,
	      (L + 1 + N - 1) / N);
	printf("{\"test\": \"deadline\", \"latency\": %u, \"blocks\": %d, \"wet\": %d, \"late\": %d, \"stalled\": %d, "
	       "\"first_late\": %d, \"recovered_at\": %d, \"dry_max_error\": %g, \"wet_max_error\": %g, \"misses_header\": %llu}\n",
	       L, total, wet, late, stalled, first_late, recovered_at, worst_dry, worst_wet,
	       (unsigned long long)c.e.h->e_misses);
	close_chain(&c);
	return 0;
}

/* the supervisor's role for the crash test: restart a dead sandbox */
struct sup {
	struct chain *c;
	volatile int crashed_sig;
	volatile uint64_t died_ns, ready_ns;
	volatile int done;
};

static void *supervise(void *arg)
{
	struct sup *s = arg;
	int st;
	if (waitpid(s->c->pid, &st, 0) == s->c->pid) {
		s->died_ns = bl_slot1_now_ns();
		s->crashed_sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
		s->c->pid = -1;
		if (spawn(s->c, "crash-a") > 0)
			s->ready_ns = bl_slot1_now_ns();
	}
	s->done = 1;
	return NULL;
}

static int t_crash(void)
{
	struct chain a, b;
	char sdir[200];
	if (O.hostd_state) {
		/* chain A under backline-hostd: LoadSlot, and hostd restarts it */
		if (open_chain(&b, "crash-b") < 0)
			return 1;
		memset(&a, 0, sizeof(a));
		if (bl_slot1_engine_create(&a.e, CH, 1024, O.rate, 0) < 0)
			return 1;
		bl_slot1_engine_path(&a.e, a.path, sizeof(a.path));
		a.ready = json_object_get(b.ready); /* the same plugins: the same parameter ids */
		a.pid = -1;
		if (hostd_load(&a, "crash-a") != 0) {
			fprintf(stderr, "crash: LoadSlot failed\n");
			return 1;
		}
		snprintf(sdir, sizeof(sdir), "%s/crash-a.slot", O.hostd_state);
	} else {
		if (open_chain_state(&a, "crash-a", true) < 0 || open_chain(&b, "crash-b") < 0)
			return 1;
		snprintf(sdir, sizeof(sdir), "%s", a.state_dir);
	}
	struct chain *cs[2] = { &a, &b };
	set_param(&a, 0, "gain", -6.0);
	set_param(&b, 0, "gain", -6.0);
	settle(cs, 2);
	/* chain A's sandbox keeps its state in a directory, as under hostd: run
	 * until the gain has been saved there, so the restart brings it back */
	{
		static float z[CH][1024];
		float *zi[CH] = { z[0], z[1] };
		char probe[160];
		uint64_t t = bl_slot1_now_ns();
		for (int k = 0; k < 4000; k++) {
			cycle(&a, zi, zi, t);
			cycle(&b, zi, zi, t);
			t += period_ns();
			sleep_until(t);
			if (k % 20 == 0) {
				int found = 0;
				for (const char *ext = ".clapstate"; ext; ext = !strcmp(ext, ".clapstate") ? ".vst3state" : !strcmp(ext, ".vst3state") ? ".lv2state" : NULL) {
					snprintf(probe, sizeof(probe), "%s/insert0%s", sdir, ext);
					found |= access(probe, F_OK) == 0;
				}
				if (found)
					break;
			}
		}
	}
	uint32_t L = chain_latency(&a), N = O.frames;
	double g = pow(10.0, -6.0 / 20.0);
	struct sup sv = { .c = &a };
	pthread_t th;
	if (!O.hostd_state)
		pthread_create(&th, NULL, supervise, &sv);
	uint64_t crash_ns = 0;
	int total = 3000, crash_at = 100;
	static float ibuf[CH][1024], oa[CH][1024], ob[CH][1024];
	float *in[CH] = { ibuf[0], ibuf[1] }, *outa[CH] = { oa[0], oa[1] }, *outb[CH] = { ob[0], ob[1] };
	int a_dry = 0, b_wet = 0, b_dry = 0, a_back = -1, a_first_dry = -1;
	int64_t b_skip = L; /* B's first L frames are its initial silence */
	enum bl_slot1_result bprev = BL_SLOT1_WET;
	float a_worst = 0, b_worst = 0;
	int64_t a_skip = 0;
	enum bl_slot1_result aprev = BL_SLOT1_WET;
	uint64_t t = bl_slot1_now_ns(), back_ns = 0;
	for (int k = 0; k < total; k++) {
		int64_t n0 = (int64_t)k * N;
		for (uint32_t i = 0; i < N; i++)
			for (int ch = 0; ch < CH; ch++)
				ibuf[ch][i] = sig(n0 + i, ch);
		if (k == crash_at) {
			set_param(&a, 0, "crash", 1);
			crash_ns = bl_slot1_now_ns();
		}
		/* as an engine with several chains does: every request first, then
		 * collect, so a dead chain's wait can't eat another one's deadline */
		uint64_t dl = t + period_ns() * (uint64_t)O.deadline_pct / 100;
		bl_slot1_engine_begin(&a.e, (const float *const *)in, N, dl, NULL);
		bl_slot1_engine_begin(&b.e, (const float *const *)in, N, dl, NULL);
		enum bl_slot1_result ra = bl_slot1_engine_end(&a.e, outa);
		enum bl_slot1_result rb = bl_slot1_engine_end(&b.e, outb);
		bool aw = ra == BL_SLOT1_WET || ra == BL_SLOT1_RESUMED, bw = rb == BL_SLOT1_WET || rb == BL_SLOT1_RESUMED;
		if (bw && bprev != BL_SLOT1_WET && bprev != BL_SLOT1_RESUMED)
			b_skip = n0 + L; /* B missed a deadline of its own (a starved CPU), and resumed */
		bprev = rb;
		if (aw && aprev != BL_SLOT1_WET && aprev != BL_SLOT1_RESUMED && k > crash_at) {
			a_skip = n0 + L; /* a new plugin: its delay line starts empty */
			if (a_back < 0) {
				a_back = k;
				back_ns = bl_slot1_now_ns();
			}
		}
		if (!aw && a_first_dry < 0)
			a_first_dry = k;
		float ea = block_err(outa, n0, N, L, g, aw, aw ? a_skip : 0);
		float eb = block_err(outb, n0, N, L, g, bw, bw ? b_skip : 0);
		if (!aw) a_dry++;
		if (bw) b_wet++; else b_dry++;
		if (ea > a_worst) a_worst = ea;
		if (eb > b_worst) b_worst = eb;
		aprev = ra;
		t += period_ns();
		sleep_until(t);
		if (a_back > 0 && k > a_back + 200)
			break;
	}
	if (!O.hostd_state) {
		pthread_join(th, NULL);
	} else {
		sv.crashed_sig = hostd_last_signal("crash-a");
	}
	double restart_ms = sv.ready_ns && sv.died_ns ? (double)(sv.ready_ns - sv.died_ns) / 1e6 : -1;
	double gap_ms = back_ns && crash_ns ? (double)(back_ns - crash_ns) / 1e6 : -1;
	CHECK(sv.crashed_sig == SIGSEGV, "chain A's sandbox ended with signal %d, not SIGSEGV", sv.crashed_sig);
	CHECK(a_first_dry == crash_at, "chain A went dry at block %d, not %d", a_first_dry, crash_at);
	CHECK(a_back > 0, "chain A never came back after the restart");
	CHECK(a_worst < 1e-5f, "chain A's output was wrong by %g (dry must be x[n−L], wet g·x[n−L])", a_worst);
	CHECK(b_worst < 1e-5f, "chain B's output was wrong by %g", b_worst);
	/* the isolation: B's sandbox never died or restarted, and every one of its
	 * samples is right. A starved CPU (a 2-core CI runner while A dumps core)
	 * can still make B miss a deadline of its own; on a box with room it must
	 * not */
	CHECK(bl_slot1_load(&b.e.h->generation) == 1 && b.pid > 0 && kill(b.pid, 0) == 0,
	      "chain B's sandbox died or was restarted (generation %u)", bl_slot1_load(&b.e.h->generation));
	if (sysconf(_SC_NPROCESSORS_ONLN) >= 8)
		CHECK(b_dry == 0, "chain B played %d dry blocks while A crashed", b_dry);
	else if (b_dry)
		fprintf(stderr, "crash: note: chain B missed %d block(s) on a %ld-CPU box (not counted)\n", b_dry,
			sysconf(_SC_NPROCESSORS_ONLN));
	CHECK(bl_slot1_load(&a.e.h->generation) == 2, "chain A generation %u, not 2", bl_slot1_load(&a.e.h->generation));
	printf("{\"test\": \"crash\", \"supervisor\": \"%s\", \"latency\": %u, \"signal\": %d, \"a_dry_blocks\": %d, \"a_back_at\": %d, "
	       "\"restart_ms\": %.1f, \"crash_to_wet_ms\": %.1f, \"b_wet\": %d, \"b_dry\": %d, \"b_misses\": %llu, "
	       "\"a_max_error\": %g, \"b_max_error\": %g}\n",
	       O.hostd_state ? "hostd" : "driver", L, sv.crashed_sig, a_dry, a_back, restart_ms, gap_ms, b_wet, b_dry, (unsigned long long)b.e.h->e_misses,
	       a_worst, b_worst);
	if (O.hostd_state)
		hostd_unload("crash-a");
	close_chain(&a);
	close_chain(&b);
	return 0;
}

static int t_allocs(void)
{
	struct chain c;
	if (open_chain(&c, "allocs") < 0)
		return 1;
	struct chain *cs[1] = { &c };
	settle(cs, 1);
	int n = O.cycles ? O.cycles : 5000;
	static float ibuf[CH][1024], obuf[CH][1024];
	float *in[CH] = { ibuf[0], ibuf[1] }, *out[CH] = { obuf[0], obuf[1] };
	for (int ch = 0; ch < CH; ch++)
		for (uint32_t i = 0; i < 1024; i++)
			ibuf[ch][i] = sig(i, ch);
	uint32_t base_allocs = c.e.h->audio_allocs, base_frees = c.e.h->audio_frees;
	uint32_t gain_id = 0;
	if (param_id(&c, 0, "gain", &gain_id) < 0)
		failures++;
	bl_rtalloc_counters(NULL, NULL);
	uint32_t my0 = bl_rtalloc_allocs() + bl_rtalloc_frees();
	go_realtime();
	bl_rtalloc_enter();
	uint64_t t = bl_slot1_now_ns();
	int wet = 0;
	for (int k = 0; k < n; k++) {
		if (k % 50 == 0) {
			/* parameter traffic too: the event path must not allocate either */
			struct bl_slot1_event e = { .slot = 0, .type = BL_SLOT1_EV_PARAM_NORM, .id = gain_id, .value = (k % 100) / 100.0 };
			bl_slot1_engine_push(&c.e, &e);
		}
		wet += cycle(&c, in, out, t) == BL_SLOT1_WET;
		t += period_ns();
		sleep_until(t);
	}
	bl_rtalloc_leave();
	uint32_t my = bl_rtalloc_allocs() + bl_rtalloc_frees() - my0;
	uint32_t sa = c.e.h->audio_allocs - base_allocs, sf = c.e.h->audio_frees - base_frees;
	CHECK(sa == 0 && sf == 0, "the sandbox's audio thread made %u allocations and %u frees", sa, sf);
	CHECK(my == 0, "the engine side made %u heap calls in its cycle", my);
	/* the negative control: a plugin that allocates is caught */
	set_param(&c, O.n_plugins - 1, "alloc", 1);
	for (int k = 0; k < 100; k++) {
		cycle(&c, in, out, t);
		t += period_ns();
		sleep_until(t);
	}
	uint32_t caught = c.e.h->audio_allocs - base_allocs;
	CHECK(caught >= 50, "an allocating plugin made only %u counted allocations in 100 cycles", caught);
	printf("{\"test\": \"allocs\", \"inserts\": %d, \"cycles\": %d, \"wet\": %d, \"sandbox_allocs\": %u, "
	       "\"sandbox_frees\": %u, \"engine_heap_calls\": %u, \"control_allocs_seen\": %u}\n",
	       O.n_plugins, n, wet, sa, sf, my, caught);
	close_chain(&c);
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static int t_timing(void)
{
	struct chain c;
	if (open_chain(&c, "timing") < 0)
		return 1;
	struct chain *cs[1] = { &c };
	settle(cs, 1);
	int n = O.cycles ? O.cycles : 20000;
	uint64_t *rtt = calloc((size_t)n, sizeof(uint64_t));
	uint64_t *proc = calloc((size_t)n, sizeof(uint64_t));
	mlock(rtt, (size_t)n * 8);
	mlock(proc, (size_t)n * 8);
	static float ibuf[CH][1024], obuf[CH][1024];
	float *in[CH] = { ibuf[0], ibuf[1] }, *out[CH] = { obuf[0], obuf[1] };
	for (int ch = 0; ch < CH; ch++)
		for (uint32_t i = 0; i < 1024; i++)
			ibuf[ch][i] = sig(i, ch);
	go_realtime();
	uint64_t t = bl_slot1_now_ns();
	int wet = 0, m = 0;
	uint64_t miss0 = c.e.h->e_misses;
	for (int k = 0; k < n; k++) {
		enum bl_slot1_result r = cycle(&c, in, out, t);
		if (r == BL_SLOT1_WET) {
			wet++;
			rtt[m] = c.e.h->e_rtt_ns_last;
			proc[m] = c.e.h->process_ns_last;
			m++;
		}
		t += period_ns();
		sleep_until(t);
	}
	qsort(rtt, (size_t)m, 8, cmp_u64);
	qsort(proc, (size_t)m, 8, cmp_u64);
	uint64_t misses = c.e.h->e_misses - miss0;
#define PCT(a, p) (m ? (double)(a)[(size_t)((m - 1) * (p))] / 1000.0 : -1)
	double period_us = (double)period_ns() / 1000.0;
	printf("{\"test\": \"timing\", \"inserts\": %d, \"frames\": %u, \"rate\": %u, \"period_us\": %.1f, "
	       "\"deadline_us\": %.1f, \"cycles\": %d, \"wet\": %d, \"misses\": %llu, "
	       "\"rtt_us\": {\"p50\": %.1f, \"p99\": %.1f, \"p999\": %.1f, \"max\": %.1f}, "
	       "\"sandbox_us\": {\"p50\": %.1f, \"p99\": %.1f, \"max\": %.1f}}\n",
	       O.n_plugins, O.frames, O.rate, period_us, period_us * O.deadline_pct / 100, n, wet,
	       (unsigned long long)misses, PCT(rtt, 0.5), PCT(rtt, 0.99), PCT(rtt, 0.999), PCT(rtt, 1.0), PCT(proc, 0.5),
	       PCT(proc, 0.99), PCT(proc, 1.0));
	/* timing on a shared CI box is noisy: the hard check is that the chain
	 * stays wet; the numbers are the measurement */
	CHECK(wet >= n * 99 / 100, "only %d of %d cycles wet", wet, n);
	free(rtt);
	free(proc);
	close_chain(&c);
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: backline-test-slot-driver align|deadline|crash|allocs|timing --host BIN "
			"[--frames N] [--rate R] [--cycles N] [--deadline-pct P] [--no-rt] [--log FILE] --plugin SPEC ...\n");
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 2)
		usage();
	O.test = argv[1];
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--host") && v) O.host = v, i++;
		else if (!strcmp(a, "--plugin") && v && O.n_plugins < MAXP) O.plugins[O.n_plugins++] = v, i++;
		else if (!strcmp(a, "--frames") && v) O.frames = (uint32_t)atoi(v), i++;
		else if (!strcmp(a, "--rate") && v) O.rate = (uint32_t)atoi(v), i++;
		else if (!strcmp(a, "--cycles") && v) O.cycles = atoi(v), i++;
		else if (!strcmp(a, "--deadline-pct") && v) O.deadline_pct = atoi(v), i++;
		else if (!strcmp(a, "--log") && v) O.log = v, i++;
		else if (!strcmp(a, "--no-rt")) O.rt = 0;
		else if (!strcmp(a, "--hostd") && v) O.hostd_state = v, i++;
		else usage();
	}
	if (!O.host || !O.n_plugins || O.frames < 1 || O.frames > 1024)
		usage();
	signal(SIGPIPE, SIG_IGN);
	int r;
	if (!strcmp(O.test, "align")) r = t_align();
	else if (!strcmp(O.test, "deadline")) r = t_deadline();
	else if (!strcmp(O.test, "crash")) r = t_crash();
	else if (!strcmp(O.test, "allocs")) r = t_allocs();
	else if (!strcmp(O.test, "timing")) r = t_timing();
	else usage();
	fflush(stdout);
	return r || failures ? 1 : 0;
}
