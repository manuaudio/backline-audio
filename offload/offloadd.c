/* backline-offloadd: a DSP server node for network offload (Offload1).
 * SPDX-License-Identifier: MIT
 *
 * Each slot is one plugin chain, hosted the same way a mixing engine hosts a
 * strip's inserts: in a backline-slot-host process over Slot1 shared memory
 * (lib/include/backline/slot1.h). Here the network plays the engine:
 *
 *   console ──BLO1 block k──► this server: copy into the slot's memory, wake
 *                             the chain, wait until the budget runs out
 *   console ◄──BLO1 block k── the chain's output (or, if the chain was late or
 *                             is down, its input delayed by the chain's
 *                             latency, marked BLO1_F_DRY)
 *
 * Lockstep: the server has no clock and no buffer. It processes a block when
 * the block arrives and sends it straight back. The console's period is the
 * only clock. A crashed chain is restarted on the same memory with its saved
 * state, while the slot answers dry, marked; a console with a mirror server
 * takes the mirror's block meanwhile (offload1.h).
 *
 *   backline-offloadd [--listen ADDR] [--port P] [--slot-host BIN] [--state-dir DIR]
 *                     [--rate R] [--budget-us U] [--rt-priority N] [--cpu N]
 *                     --slot CHANNELS --plugin P [--plugin P ...] [--slot ...]
 *
 * Slot k listens on UDP ADDR:P+k. Transport here: UDP. The raw-Ethernet path
 * (AF_PACKET, EtherType 0x88B5) is in offload/offload-proof.cpp, still a proof.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backline/offload1.h"
#include "backline/slot1.h"

#define MAX_SLOTS 16
#define LOG(...) fprintf(stderr, "backline-offloadd: " __VA_ARGS__)

struct slot {
	int index;
	uint16_t channels;
	const char *plugins[BL_SLOT1_MAX_SLOTS];
	int n_plugins;
	struct bl_slot1_engine e;
	char path[64], state_dir[1024];
	volatile pid_t pid;
	int fd;
	pthread_t net, sup;
	uint64_t blocks, dry, restarts;
};

static struct {
	const char *listen, *slot_host, *state_dir;
	uint16_t port;
	uint32_t rate, budget_us;
	int rt_priority, cpu;
	int spin;               /* busy-poll the socket and the chain: a core per slot, no sleeping wake-ups */
	struct slot slots[MAX_SLOTS];
	int n_slots;
	volatile sig_atomic_t quit;
} S = { .listen = "0.0.0.0", .port = BLO1_DEFAULT_PORT, .rate = 48000, .budget_us = 0, .rt_priority = 75, .cpu = -1 };

static int mkdir_p(const char *p)
{
	char b[1024];
	snprintf(b, sizeof(b), "%s", p);
	for (char *s = b + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(b, 0700);
			*s = '/';
		}
	return mkdir(b, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

/* Starts the slot's chain and waits for its ready line. */
static int spawn(struct slot *s)
{
	int pfd[2];
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -1;
	const char *argv[16 + 2 * BL_SLOT1_MAX_SLOTS];
	char name[32];
	int a = 0;
	snprintf(name, sizeof(name), "offload-slot%d", s->index);
	argv[a++] = S.slot_host;
	argv[a++] = "--shm";
	argv[a++] = s->path;
	argv[a++] = "--name";
	argv[a++] = name;
	if (s->state_dir[0]) {
		argv[a++] = "--state-dir";
		argv[a++] = s->state_dir;
	}
	if (S.spin) {
		argv[a++] = "--spin-us";
		argv[a++] = "5000";
	}
	for (int i = 0; i < s->n_plugins; i++) {
		argv[a++] = "--plugin";
		argv[a++] = s->plugins[i];
	}
	argv[a] = NULL;
	pid_t parent = getpid();
	pid_t pid = fork();
	if (pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(127);
		dup2(pfd[1], 1);
		execv(S.slot_host, (char **)argv);
		_exit(127);
	}
	close(pfd[1]);
	char line[4096];
	size_t n = 0;
	struct pollfd p = { pfd[0], POLLIN, 0 };
	while (n + 1 < sizeof(line) && poll(&p, 1, 20000) > 0) {
		if (read(pfd[0], line + n, 1) != 1)
			break;
		if (line[n] == '\n')
			break;
		n++;
	}
	line[n] = 0;
	close(pfd[0]);
	if (!n) {
		LOG("slot %d: the chain did not start\n", s->index);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -1;
	}
	s->pid = pid;
	return 0;
}

/* The slot's supervisor: a crashed chain comes back on the same memory, at
 * once after a healthy run, backing off when it keeps crashing. */
static void *supervise(void *arg)
{
	struct slot *s = arg;
	uint64_t backoff = 0, started = blo1_now_ns();
	while (!S.quit) {
		int st;
		pid_t pid = s->pid;
		if (pid <= 0 || waitpid(pid, &st, 0) != pid)
			break;
		if (S.quit)
			break;
		uint64_t died = blo1_now_ns();
		LOG("slot %d: chain %d died (%s %d); restarting\n", s->index, (int)pid, WIFSIGNALED(st) ? "signal" : "exit",
		    WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		s->pid = -1;
		/* a crash after a healthy run (10 s) restarts at once; crashes in a
		 * row back off from 250 ms, doubling to 5 s */
		if (died - started > 10000000000ull)
			backoff = 0;
		else
			backoff = backoff ? (backoff * 2 > 5000000000ull ? 5000000000ull : backoff * 2) : (s->restarts ? 250000000ull : 0);
		if (backoff)
			usleep((useconds_t)(backoff / 1000));
		while (!S.quit && spawn(s) < 0)
			usleep(250000);
		started = blo1_now_ns();
		s->restarts++;
		LOG("slot %d: back, pid %d, %.1f ms after the crash\n", s->index, (int)s->pid, (started - died) / 1e6);
	}
	return NULL;
}

static void pin_and_prioritize(int cpu_offset)
{
	if (S.cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(S.cpu + cpu_offset, &set);
		pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	}
	struct sched_param sp = { .sched_priority = S.rt_priority };
	if (S.rt_priority > 0 && pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0 && cpu_offset == 0)
		LOG("no realtime priority (%s): running at normal priority\n", strerror(errno));
}

/* One slot's lockstep loop. */
static void *serve(void *arg)
{
	struct slot *s = arg;
	pin_and_prioritize(s->index);
	static __thread uint8_t pkt[BLO1_MAX_PACKET] __attribute__((aligned(16)));
	float *io[BLO1_MAX_CHANNELS];
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t r;
		if (S.spin) {
			do
				r = recvfrom(s->fd, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
			while (r < 0 && errno == EAGAIN && !S.quit);
		} else {
			r = recvfrom(s->fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
		}
		if (S.quit)
			break;
		if (r < (ssize_t)sizeof(struct blo1_hdr))
			continue;
		uint64_t t0 = blo1_now_ns();
		struct blo1_hdr *h = (struct blo1_hdr *)pkt;
		if (h->magic != BLO1_MAGIC || h->slot != s->index)
			continue;
		if (h->flags & BLO1_F_HELLO) {
			h->server_us = bl_slot1_load(&s->e.h->latency);
			sendto(s->fd, h, sizeof(*h), 0, (struct sockaddr *)&from, fl);
			continue;
		}
		if (h->flags & BLO1_F_PARAM) {
			int n = h->part;
			const struct blo1_event *ev = (const struct blo1_event *)(pkt + 32);
			if (n > BLO1_MAX_EVENTS || r < (ssize_t)(32 + n * sizeof(*ev)))
				continue;
			for (int i = 0; i < n; i++) {
				struct bl_slot1_event e = { ev[i].insert, ev[i].type, ev[i].offset, ev[i].id, ev[i].value };
				bl_slot1_engine_push(&s->e, &e);
			}
			continue;
		}
		uint32_t frames = h->frames, ch = h->channels;
		if (ch != s->channels || !frames || frames > BLO1_MAX_FRAMES || r != (ssize_t)(32 + ch * frames * 4))
			continue;
		float *smp = (float *)(pkt + 32);
		for (uint32_t c = 0; c < ch; c++)
			io[c] = smp + (size_t)c * frames;
		/* the budget: what the console allows the server (default: half the period) */
		uint64_t budget = S.budget_us ? (uint64_t)S.budget_us * 1000 : (uint64_t)frames * 500000000ull / S.rate;
		bl_slot1_engine_begin(&s->e, (const float *const *)io, frames, t0 + budget, NULL);
		enum bl_slot1_result res = bl_slot1_engine_end(&s->e, io);
		/* anything but the chain's clean output is marked dry, so a console
		 * with a mirror takes the mirror's block (a chain that just resumed
		 * after a gap holds its pre-gap state for its latency) */
		h->flags = res == BL_SLOT1_WET ? 0 : BLO1_F_DRY;
		h->server_us = (uint32_t)((blo1_now_ns() - t0) / 1000);
		s->blocks++;
		if (res != BL_SLOT1_WET)
			s->dry++;
		sendto(s->fd, pkt, (size_t)r, 0, (struct sockaddr *)&from, fl);
	}
	return NULL;
}

static void on_signal(int sig)
{
	(void)sig;
	S.quit = 1;
}

static void usage(FILE *f)
{
	fprintf(f, "usage: backline-offloadd [--listen ADDR] [--port P] [--slot-host BIN] [--state-dir DIR] [--rate R]\n"
		   "                         [--budget-us U] [--rt-priority N] [--cpu N] [--spin]\n"
		   "                         --slot CHANNELS --plugin P [--plugin P ...] [--slot ...]\n"
		   "A DSP server for network offload (docs/api/Offload1.md): slot k answers BLO1 blocks on UDP ADDR:P+k\n"
		   "(default 0.0.0.0:%d) with its chain's output, in lockstep with the console.\n",
		BLO1_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "listen", required_argument, NULL, 'l' },     { "port", required_argument, NULL, 'p' },
		{ "slot-host", required_argument, NULL, 'H' },  { "state-dir", required_argument, NULL, 's' },
		{ "rate", required_argument, NULL, 'r' },       { "budget-us", required_argument, NULL, 'b' },
		{ "rt-priority", required_argument, NULL, 'P' }, { "cpu", required_argument, NULL, 'c' },
		{ "slot", required_argument, NULL, 'S' },       { "plugin", required_argument, NULL, 'g' },
		{ "spin", no_argument, NULL, 'x' },
		{ "help", no_argument, NULL, 'h' },             { 0 } };
	int c;
	while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (c) {
		case 'l': S.listen = optarg; break;
		case 'p': S.port = (uint16_t)atoi(optarg); break;
		case 'H': S.slot_host = optarg; break;
		case 's': S.state_dir = optarg; break;
		case 'r': S.rate = (uint32_t)atoi(optarg); break;
		case 'b': S.budget_us = (uint32_t)atoi(optarg); break;
		case 'P': S.rt_priority = atoi(optarg); break;
		case 'c': S.cpu = atoi(optarg); break;
		case 'x': S.spin = 1; break;
		case 'S':
			if (S.n_slots >= MAX_SLOTS) {
				LOG("at most %d slots\n", MAX_SLOTS);
				return 2;
			}
			S.slots[S.n_slots].index = S.n_slots;
			S.slots[S.n_slots].channels = (uint16_t)atoi(optarg);
			S.n_slots++;
			break;
		case 'g':
			if (!S.n_slots || S.slots[S.n_slots - 1].n_plugins >= (int)BL_SLOT1_MAX_SLOTS) {
				usage(stderr);
				return 2;
			}
			S.slots[S.n_slots - 1].plugins[S.slots[S.n_slots - 1].n_plugins++] = optarg;
			break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (!S.n_slots) {
		usage(stderr);
		return 2;
	}
	static char self_host[1024];
	if (!S.slot_host) {
		ssize_t n = readlink("/proc/self/exe", self_host, sizeof(self_host) - 32);
		if (n > 0) {
			self_host[n] = 0;
			char *sl = strrchr(self_host, '/');
			snprintf(sl ? sl + 1 : self_host, 32, "backline-slot-host");
		}
		S.slot_host = self_host;
	}
	struct sigaction sa = { 0 };
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	for (int k = 0; k < S.n_slots; k++) {
		struct slot *s = &S.slots[k];
		if (!s->n_plugins || !s->channels || s->channels > BLO1_MAX_CHANNELS) {
			LOG("slot %d needs 1..%d channels and at least one --plugin\n", k, BLO1_MAX_CHANNELS);
			return 2;
		}
		int r = bl_slot1_engine_create(&s->e, s->channels, BLO1_MAX_FRAMES, S.rate, 0);
		if (r < 0) {
			LOG("slot %d: Slot1 memory: %s\n", k, strerror(-r));
			return 1;
		}
		bl_slot1_engine_path(&s->e, s->path, sizeof(s->path));
		if (S.spin)
			s->e.spin_ns = 2000000; /* end() spins through the whole budget */
		if (S.state_dir) {
			snprintf(s->state_dir, sizeof(s->state_dir), "%s/slot%d", S.state_dir, k);
			mkdir_p(s->state_dir);
		}
		if (spawn(s) < 0)
			return 1;
		s->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)(S.port + k)) };
		if (inet_pton(AF_INET, S.listen, &a.sin_addr) != 1 || bind(s->fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
			LOG("slot %d: cannot listen on %s:%d: %s\n", k, S.listen, S.port + k, strerror(errno));
			return 1;
		}
		int prio = 6;
		setsockopt(s->fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
		struct timeval tv = { 0, 200000 }; /* wake now and then to notice a stop */
		setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		/* settle: the chain is ready and its latency known before the first block */
		uint64_t until = blo1_now_ns() + 5000000000ull;
		while (bl_slot1_load(&s->e.h->state) != BL_SLOT1_READY && blo1_now_ns() < until)
			usleep(1000);
		pthread_create(&s->sup, NULL, supervise, s);
		pthread_create(&s->net, NULL, serve, s);
		LOG("slot %d: %u channels, %d inserts, latency %u, on udp %s:%d\n", k, s->channels, s->n_plugins,
		    bl_slot1_load(&s->e.h->latency), S.listen, S.port + k);
	}
	printf("{\"ready\": true, \"pid\": %d, \"slots\": %d, \"port\": %d}\n", (int)getpid(), S.n_slots, S.port);
	fflush(stdout);
	while (!S.quit)
		pause();
	for (int k = 0; k < S.n_slots; k++) {
		struct slot *s = &S.slots[k];
		pthread_join(s->net, NULL);
		if (s->pid > 0)
			kill(s->pid, SIGTERM);
		pthread_join(s->sup, NULL);
		LOG("slot %d: %llu blocks, %llu dry, %llu restarts\n", k, (unsigned long long)s->blocks,
		    (unsigned long long)s->dry, (unsigned long long)s->restarts);
		bl_slot1_engine_destroy(&s->e);
	}
	return 0;
}
