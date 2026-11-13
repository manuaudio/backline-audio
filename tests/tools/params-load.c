/* Params1 load test, offline: a private D-Bus daemon (no service files, so
 * nothing can be activated), backline-controld --no-graph (no PipeWire), one
 * program publishing a 5000-parameter tree, and 8 OSC clients (4 TCP, 4 UDP)
 * each subscribed to 2000 of them. Measures register, bulk get/set, OSC set
 * echo latency, bulk fan-out, and controld's CPU.
 *
 *   backline-test-params-load CONTROLD [--json FILE]
 *
 * Exit 0 when every value arrived and the limits below held; 77 (skip) when
 * dbus-daemon is missing.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include "osc.h"

/* Names: the same strings controld and paramsd.h use. */
#define CTL_BUS "io.github.manuaudio.Backline.Control"
#define CTL_PATH "/io/github/manuaudio/Backline/Control"
#define CTL_IFACE "io.github.manuaudio.Backline.Control1"
#define PRM_PATH "/io/github/manuaudio/Backline/Params"
#define PRM_IFACE "io.github.manuaudio.Backline.Params1"

#define ROOT "/load"
#define GROUPS 40
#define PER_GROUP 125
#define NPARAMS (GROUPS * PER_GROUP)   /* 5000 */
#define NCLIENTS 8
#define SUB_GROUPS 16                  /* 16 x 125 = 2000 per client */
#define OWN_GROUPS 5                   /* 5 x 125 = 625 set by each client */
#define ROUNDS 4
#define BUNDLE 25
#define PACE_NS (5ull * 1000000ull)    /* one bundle of 25 per client every 5 ms */
#define SUB_RATE 60

/* Limits the test enforces (generous: CI containers are slow). */
#define LIM_REGISTER_MS 3000.0
#define LIM_BULK_FANOUT_MS 3000.0
#define LIM_ECHO_P99_MS 250.0

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static double ms(uint64_t ns) { return ns / 1e6; }

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "params-load: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static char tmpdir[256];
static pid_t bus_pid = -1, ctl_pid = -1;

static void cleanup(void)
{
	if (ctl_pid > 0) {
		kill(ctl_pid, SIGTERM);
		waitpid(ctl_pid, NULL, 0);
	}
	if (bus_pid > 0) {
		kill(bus_pid, SIGTERM);
		waitpid(bus_pid, NULL, 0);
	}
	if (tmpdir[0]) {
		char cmd[300];
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
		(void)!system(cmd);
	}
}

/* ------------------------------------------------------------ the bus */
static sd_bus *bus_connect(const char *addr)
{
	sd_bus *b = NULL;
	if (sd_bus_new(&b) < 0 || sd_bus_set_address(b, addr) < 0 || sd_bus_set_bus_client(b, 1) < 0 ||
	    sd_bus_start(b) < 0)
		die("cannot connect to the private bus %s", addr);
	return b;
}

static bool have_dbus_daemon(void)
{
	return system("command -v dbus-daemon >/dev/null 2>&1") == 0;
}

static void start_bus(char *addr, size_t n)
{
	char cfg[300], sock[300];
	snprintf(cfg, sizeof(cfg), "%s/bus.conf", tmpdir);
	snprintf(sock, sizeof(sock), "%s/bus", tmpdir);
	FILE *f = fopen(cfg, "w");
	if (!f)
		die("cannot write %s", cfg);
	/* no <servicedir>, no <standard_session_servicedirs/>: nothing can be
	 * activated, so no real service ever starts from this bus */
	fprintf(f, "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
		   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
		   "<busconfig><type>session</type><listen>unix:path=%s</listen><auth>EXTERNAL</auth>\n"
		   "<policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/>"
		   "<allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>\n",
		sock);
	fclose(f);
	bus_pid = fork();
	if (bus_pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		dup2(nul, 1);
		execlp("dbus-daemon", "dbus-daemon", "--config-file", cfg, "--nofork", "--nopidfile", NULL);
		_exit(127);
	}
	for (int i = 0; i < 500; i++) {
		struct stat st;
		if (stat(sock, &st) == 0)
			break;
		usleep(10000);
	}
	snprintf(addr, n, "unix:path=%s", sock);
}

static void start_controld(const char *exe, const char *addr)
{
	ctl_pid = fork();
	if (ctl_pid == 0) {
		/* belt and braces: even a PipeWire client could not find a server */
		setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
		setenv("XDG_RUNTIME_DIR", tmpdir, 1);
		setenv("XDG_CONFIG_HOME", tmpdir, 1);
		setenv("XDG_STATE_HOME", tmpdir, 1);
		setenv("PIPEWIRE_REMOTE", "/nonexistent", 1);
		unsetenv("DISPLAY");
		unsetenv("WAYLAND_DISPLAY");
		execl(exe, exe, "--no-graph", "--no-config", "--idle-exit", "0", getenv("PARAMS_LOAD_VERBOSE") ? "--verbose" : NULL, NULL);
		_exit(127);
	}
}

static void wait_name(sd_bus *b, const char *name)
{
	for (int i = 0; i < 1000; i++) {
		sd_bus_message *rep = NULL;
		int has = 0;
		if (sd_bus_call_method(b, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
				       "NameHasOwner", NULL, &rep, "s", name) >= 0 &&
		    sd_bus_message_read(rep, "b", &has) >= 0 && has) {
			sd_bus_message_unref(rep);
			return;
		}
		sd_bus_message_unref(rep);
		int st;
		if (waitpid(ctl_pid, &st, WNOHANG) == ctl_pid) {
			ctl_pid = -1;
			die("backline-controld exited (status %d)", WEXITSTATUS(st));
		}
		usleep(10000);
	}
	die("%s never appeared", name);
}

static uint16_t listen_osc(sd_bus *b, const char *proto)
{
	sd_bus_message *m = NULL, *rep = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message_new_method_call(b, &m, CTL_BUS, CTL_PATH, CTL_IFACE, "Listen");
	sd_bus_message_append(m, "ssq", proto, "127.0.0.1", (uint16_t)0);
	sd_bus_message_open_container(m, 'a', "{sv}");
	sd_bus_message_append(m, "{sv}", "params", "s", "all");
	sd_bus_message_append(m, "{sv}", "max-rate", "u", (uint32_t)100000);
	if (!strcmp(proto, "tcp"))
		sd_bus_message_append(m, "{sv}", "max-clients", "u", (uint32_t)8);
	sd_bus_message_close_container(m);
	if (sd_bus_call(b, m, 0, &e, &rep) < 0)
		die("Listen %s: %s", proto, e.message);
	const char *id;
	sd_bus_message_read(rep, "s", &id);
	const char *colon = strrchr(id, ':');
	uint16_t port = (uint16_t)atoi(colon ? colon + 1 : "0");
	sd_bus_message_unref(m);
	sd_bus_message_unref(rep);
	return port;
}

static uint64_t prop_t(sd_bus *b, const char *name)
{
	uint64_t v = 0;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_get_property_trivial(b, CTL_BUS, PRM_PATH, PRM_IFACE, name, &e, 't', &v);
	sd_bus_error_free(&e);
	return v;
}

/* controld's CPU time (user + system), ns */
static uint64_t ctl_cpu_ns(void)
{
	char p[64], buf[1024];
	snprintf(p, sizeof(p), "/proc/%d/stat", ctl_pid);
	FILE *f = fopen(p, "r");
	if (!f)
		return 0;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	char *s = strrchr(buf, ')');
	unsigned long ut = 0, st = 0;
	if (s)
		sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
	return (uint64_t)(ut + st) * (1000000000ull / (uint64_t)sysconf(_SC_CLK_TCK));
}

static long ctl_hwm_kb(void)
{
	char p[64], line[256];
	snprintf(p, sizeof(p), "/proc/%d/status", ctl_pid);
	FILE *f = fopen(p, "r");
	long kb = 0;
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "VmHWM:", 6))
			kb = atol(line + 6);
	if (f)
		fclose(f);
	return kb;
}

/* ------------------------------------------------------------ the program */
static const char *param_path(int i, char *buf, size_t n)
{
	snprintf(buf, n, ROOT "/g%02d/p%03d", i / PER_GROUP, i % PER_GROUP);
	return buf;
}

static int param_index(const char *path)
{
	int g, p;
	if (sscanf(path, ROOT "/g%2d/p%3d", &g, &p) != 2 || g < 0 || g >= GROUPS || p < 0 || p >= PER_GROUP)
		return -1;
	return g * PER_GROUP + p;
}

static char *schema_json(bool set_many)
{
	size_t cap = (size_t)NPARAMS * 200 + 256, len = 0;
	char *s = malloc(cap);
	len += (size_t)snprintf(s + len, cap - len, "{\"schema\":\"backline.params/1\",%s\"params\":[",
				set_many ? "\"features\":[\"set-many\"]," : "");
	for (int i = 0; i < NPARAMS; i++) {
		char p[64];
		param_path(i, p, sizeof(p));
		len += (size_t)snprintf(s + len, cap - len,
					"%s{\"path\":\"%s\",\"type\":\"float\",\"name\":\"Load %d\",\"min\":0,\"max\":10000000,"
					"\"format\":\"%%.0f\",\"flags\":[\"automatable\"],\"value\":0}",
					i ? "," : "", p + strlen(ROOT) + 1, i);
	}
	snprintf(s + len, cap - len, "]}");
	return s;
}

struct owner {
	sd_bus *bus;
	const char *token;
	atomic_bool stop;
	atomic_ulong sets, set_many, updates;
};

static void owner_update(struct owner *o, sd_bus_message *sig, bool many)
{
	sd_bus_message *m = NULL;
	sd_bus_message_new_method_call(o->bus, &m, CTL_BUS, PRM_PATH, PRM_IFACE, "Update");
	sd_bus_message_set_expect_reply(m, 0);
	sd_bus_message_append(m, "s", o->token);
	sd_bus_message_open_container(m, 'a', "(sv)");
	unsigned long n = 0;
	if (many)
		sd_bus_message_enter_container(sig, 'a', "(sv)");
	for (;;) {
		const char *path;
		double v;
		if (many && sd_bus_message_enter_container(sig, 'r', "sv") <= 0)
			break;
		if (sd_bus_message_read(sig, "s", &path) < 0 || sd_bus_message_read(sig, "v", "d", &v) < 0)
			break;
		if (many)
			sd_bus_message_exit_container(sig);
		sd_bus_message_append(m, "(sv)", path, "d", v);
		n++;
		if (!many)
			break;
	}
	sd_bus_message_close_container(m);
	sd_bus_send(o->bus, m, NULL);
	sd_bus_message_unref(m);
	atomic_fetch_add(&o->updates, 1);
	atomic_fetch_add(many ? &o->set_many : &o->sets, many ? 1 : n);
}

static int on_owner_set(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	owner_update(ud, m, false);
	return 0;
}

static int on_owner_set_many(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	owner_update(ud, m, true);
	return 0;
}

static void *owner_thread(void *data)
{
	struct owner *o = data;
	while (!atomic_load(&o->stop)) {
		int r = sd_bus_process(o->bus, NULL);
		if (r < 0)
			break;
		if (r == 0)
			sd_bus_wait(o->bus, 20000);
	}
	sd_bus_flush(o->bus);
	return NULL;
}

/* ------------------------------------------------------------ OSC clients */
struct client {
	int k;
	bool tcp;
	int fd;
	uint8_t in[1 << 16];
	size_t have;
	bool sub[NPARAMS];        /* in this client's subscription */
	float last[NPARAMS];      /* last value seen */
	int seen_init;            /* params with an initial value */
	bool got_init[NPARAMS];
	uint64_t sent_ns[NPARAMS][ROUNDS];
	bool echoed[NPARAMS][ROUNDS];
	int echoes;
	float bulk_target;        /* phase 3/4: the value every param should reach */
	int bulk_seen;
	uint64_t bulk_done_ns;
	uint64_t received;        /* OSC value messages */
};

static struct client *C[NCLIENTS];
static int phase;            /* 1 initial, 2 sweep, 3 bulk */
static double *lat;          /* echo latencies, ms */
static size_t n_lat;

static void on_reply(void *data, const struct osc_msg *m)
{
	struct client *c = data;
	if (getenv("PARAMS_LOAD_VERBOSE") && strncmp(m->address, ROOT, strlen(ROOT)))
		fprintf(stderr, "client %d: %s %s %s\n", c->k, m->address, m->argc > 0 && m->argv[0].type == 's' ? m->argv[0].s : "",
			m->argc > 1 && m->argv[1].type == 's' ? m->argv[1].s : "");
	int i = param_index(m->address);
	if (i < 0 || m->argc < 1 || m->argv[0].type != 'f')
		return;
	float v = m->argv[0].v.f;
	c->received++;
	c->last[i] = v;
	if (!c->got_init[i]) {
		c->got_init[i] = true;
		c->seen_init++;
	}
	if (phase == 2 && v >= 1 && v < 1 + ROUNDS * 1000) {
		int round = (int)((v - 1) / 1000);
		int g = i / PER_GROUP;
		if (g >= c->k * OWN_GROUPS && g < (c->k + 1) * OWN_GROUPS && !c->echoed[i][round] && c->sent_ns[i][round]) {
			c->echoed[i][round] = true;
			c->echoes++;
			lat[n_lat++] = ms(now_ns() - c->sent_ns[i][round]);
		}
	}
	if (phase >= 3 && v == c->bulk_target && c->sub[i]) {
		c->bulk_seen++;
		if (c->bulk_seen == SUB_GROUPS * PER_GROUP)
			c->bulk_done_ns = now_ns();
	}
}

static void client_read(struct client *c)
{
	if (!c->tcp) {
		uint8_t buf[9000];
		for (;;) {
			ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);
			if (n <= 0)
				return;
			osc_parse(buf, (size_t)n, on_reply, c);
		}
	}
	for (;;) {
		ssize_t n = recv(c->fd, c->in + c->have, sizeof(c->in) - c->have, MSG_DONTWAIT);
		if (n <= 0)
			return;
		c->have += (size_t)n;
		size_t off = 0;
		while (c->have - off >= 4) {
			uint32_t len = (uint32_t)c->in[off] << 24 | (uint32_t)c->in[off + 1] << 16 | (uint32_t)c->in[off + 2] << 8 |
				       c->in[off + 3];
			if (c->have - off < 4 + (size_t)len)
				break;
			osc_parse(c->in + off + 4, len, on_reply, c);
			off += 4 + len;
		}
		memmove(c->in, c->in + off, c->have - off);
		c->have -= off;
	}
}

static void client_send(struct client *c, const uint8_t *pkt, size_t n)
{
	if (c->tcp) {
		uint8_t frame[4 + 8192];
		frame[0] = (uint8_t)(n >> 24); frame[1] = (uint8_t)(n >> 16); frame[2] = (uint8_t)(n >> 8); frame[3] = (uint8_t)n;
		memcpy(frame + 4, pkt, n);
		size_t off = 0;
		while (off < n + 4) {
			ssize_t w = send(c->fd, frame + off, n + 4 - off, MSG_NOSIGNAL);
			if (w <= 0)
				die("client %d: send: %s", c->k, strerror(errno));
			off += (size_t)w;
		}
	} else if (send(c->fd, pkt, n, 0) != (ssize_t)n) {
		die("client %d: send: %s", c->k, strerror(errno));
	}
}

static struct client *client_new(int k, bool tcp, uint16_t port)
{
	struct client *c = calloc(1, sizeof(*c));
	c->k = k;
	c->tcp = tcp;
	c->fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
	int big = 4 << 20;
	setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
	if (tcp) {
		int one = 1;
		setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	}
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(c->fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		die("client %d: connect: %s", k, strerror(errno));
	for (int j = 0; j < SUB_GROUPS; j++) {
		int g = (k * OWN_GROUPS + j) % GROUPS;
		for (int p = 0; p < PER_GROUP; p++)
			c->sub[g * PER_GROUP + p] = true;
	}
	return c;
}

static void pump(uint64_t until_ns, bool (*done)(void))
{
	struct pollfd pf[NCLIENTS];
	for (int k = 0; k < NCLIENTS; k++)
		pf[k] = (struct pollfd){ C[k]->fd, POLLIN, 0 };
	while (now_ns() < until_ns && !(done && done())) {
		uint64_t left = until_ns - now_ns();
		int to = (int)(left / 1000000) + 1;
		if (poll(pf, NCLIENTS, to > 20 ? 20 : to) > 0)
			for (int k = 0; k < NCLIENTS; k++)
				if (pf[k].revents & POLLIN)
					client_read(C[k]);
	}
}

static bool all_init(void)
{
	for (int k = 0; k < NCLIENTS; k++)
		if (C[k]->seen_init < SUB_GROUPS * PER_GROUP)
			return false;
	return true;
}

static int total_echoes(void)
{
	int n = 0;
	for (int k = 0; k < NCLIENTS; k++)
		n += C[k]->echoes;
	return n;
}

static bool all_echoed(void) { return total_echoes() == NCLIENTS * OWN_GROUPS * PER_GROUP * ROUNDS; }

static bool all_bulk(void)
{
	for (int k = 0; k < NCLIENTS; k++)
		if (C[k]->bulk_seen < SUB_GROUPS * PER_GROUP)
			return false;
	return true;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static double pct(double *v, size_t n, double p)
{
	if (!n)
		return NAN;
	size_t i = (size_t)ceil(p / 100.0 * (double)n) - 1;
	return v[i < n ? i : n - 1];
}

/* ------------------------------------------------------------ bulk phases */
static void bulk_reset(float target)
{
	for (int k = 0; k < NCLIENTS; k++) {
		C[k]->bulk_target = target;
		C[k]->bulk_seen = 0;
		C[k]->bulk_done_ns = 0;
	}
}

struct bulk_result {
	double call_ms, fanout_ms, cpu_ms;
	uint64_t set_signals, changed_signals;
	unsigned long owner_updates;
};

static struct bulk_result bulk_set(sd_bus *cl, sd_bus *probe, struct owner *o, float target)
{
	struct bulk_result r = { 0 };
	bulk_reset(target);
	phase = 3;
	uint64_t ss0 = prop_t(probe, "SetSignals"), cs0 = prop_t(probe, "ChangedSignals");
	unsigned long up0 = atomic_load(&o->updates);
	uint64_t cpu0 = ctl_cpu_ns();
	sd_bus_message *m = NULL, *rep = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message_new_method_call(cl, &m, CTL_BUS, PRM_PATH, PRM_IFACE, "SetMany");
	sd_bus_message_open_container(m, 'a', "(sv)");
	for (int i = 0; i < NPARAMS; i++) {
		char p[64];
		sd_bus_message_append(m, "(sv)", param_path(i, p, sizeof(p)), "d", (double)target);
	}
	sd_bus_message_close_container(m);
	sd_bus_message_append(m, "a{sv}", 1, "client", "s", "bulk");
	uint64_t t0 = now_ns();
	if (sd_bus_call(cl, m, 0, &e, &rep) < 0)
		die("SetMany: %s", e.message);
	r.call_ms = ms(now_ns() - t0);
	uint32_t passed = 0;
	sd_bus_message_read(rep, "u", &passed);
	if (passed != NPARAMS)
		die("SetMany passed %u of %d", passed, NPARAMS);
	sd_bus_message_unref(m);
	sd_bus_message_unref(rep);
	pump(now_ns() + 10ull * 1000000000ull, all_bulk);
	if (!all_bulk())
		die("bulk set: not every client saw every value (client 0: %d of %d)", C[0]->bulk_seen, SUB_GROUPS * PER_GROUP);
	uint64_t last = 0;
	for (int k = 0; k < NCLIENTS; k++)
		if (C[k]->bulk_done_ns > last)
			last = C[k]->bulk_done_ns;
	r.fanout_ms = ms(last - t0);
	r.cpu_ms = ms(ctl_cpu_ns() - cpu0);
	usleep(50000);
	r.set_signals = prop_t(probe, "SetSignals") - ss0;
	r.changed_signals = prop_t(probe, "ChangedSignals") - cs0;
	r.owner_updates = atomic_load(&o->updates) - up0;
	return r;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: backline-test-params-load CONTROLD [--json FILE]\n");
		return 2;
	}
	const char *json_out = argc > 3 && !strcmp(argv[2], "--json") ? argv[3] : NULL;
	if (!have_dbus_daemon()) {
		fprintf(stderr, "params-load: SKIP: no dbus-daemon\n");
		return 77;
	}
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/backline-params-load-XXXXXX");
	if (!mkdtemp(tmpdir))
		die("mkdtemp");
	atexit(cleanup);
	signal(SIGPIPE, SIG_IGN);
	char addr[400];
	start_bus(addr, sizeof(addr));
	start_controld(argv[1], addr);
	sd_bus *probe = bus_connect(addr);
	wait_name(probe, CTL_BUS);
	uint16_t tcp_port = listen_osc(probe, "tcp"), udp_port = listen_osc(probe, "udp");
	if (!tcp_port || !udp_port)
		die("no listener ports");

	/* ---- register */
	struct owner own = { 0 };
	own.bus = bus_connect(addr);
	char *schema = schema_json(true);
	sd_bus_message *rep = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	uint64_t cpu0 = ctl_cpu_ns(), t0 = now_ns();
	if (sd_bus_call_method(own.bus, CTL_BUS, PRM_PATH, PRM_IFACE, "Register", &e, &rep, "ss", ROOT, schema) < 0)
		die("Register: %s", e.message);
	double register_ms = ms(now_ns() - t0), register_cpu = ms(ctl_cpu_ns() - cpu0);
	const char *tok;
	sd_bus_message_read(rep, "s", &tok);
	own.token = strdup(tok);
	sd_bus_message_unref(rep);
	sd_bus_match_signal(own.bus, NULL, NULL, PRM_PATH, PRM_IFACE, "Set", on_owner_set, &own);
	sd_bus_match_signal(own.bus, NULL, NULL, PRM_PATH, PRM_IFACE, "SetMany", on_owner_set_many, &own);
	pthread_t ot;
	pthread_create(&ot, NULL, owner_thread, &own);

	/* ---- bulk reads over D-Bus */
	sd_bus *cl = bus_connect(addr);
	sd_bus_message *m = NULL;
	sd_bus_message_new_method_call(cl, &m, CTL_BUS, PRM_PATH, PRM_IFACE, "Get");
	sd_bus_message_open_container(m, 'a', "s");
	for (int i = 0; i < NPARAMS; i++) {
		char p[64];
		sd_bus_message_append(m, "s", param_path(i, p, sizeof(p)));
	}
	sd_bus_message_close_container(m);
	t0 = now_ns();
	if (sd_bus_call(cl, m, 0, &e, &rep) < 0)
		die("Get: %s", e.message);
	double get_ms = ms(now_ns() - t0);
	sd_bus_message_unref(m);
	sd_bus_message_unref(rep);
	t0 = now_ns();
	if (sd_bus_call_method(cl, CTL_BUS, PRM_PATH, PRM_IFACE, "GetMatching", &e, &rep, "as", 1, ROOT "/**") < 0)
		die("GetMatching: %s", e.message);
	double getm_ms = ms(now_ns() - t0);
	int got = 0;
	sd_bus_message_enter_container(rep, 'a', "(svds)");
	while (sd_bus_message_skip(rep, "(svds)") > 0)
		got++;
	sd_bus_message_unref(rep);
	if (got != NPARAMS)
		die("GetMatching returned %d of %d", got, NPARAMS);
	t0 = now_ns();
	if (sd_bus_call_method(cl, CTL_BUS, PRM_PATH, PRM_IFACE, "List", &e, &rep, "s", ROOT "/**") < 0)
		die("List: %s", e.message);
	double list_ms = ms(now_ns() - t0);
	sd_bus_message_unref(rep);

	/* ---- 8 OSC clients subscribe to 2000 parameters each */
	for (int k = 0; k < NCLIENTS; k++)
		C[k] = client_new(k, k < NCLIENTS / 2, k < NCLIENTS / 2 ? tcp_port : udp_port);
	phase = 1;
	cpu0 = ctl_cpu_ns();
	t0 = now_ns();
	for (int k = 0; k < NCLIENTS; k++)
		for (int j = 0; j < SUB_GROUPS; j++) {
			uint8_t b[256];
			char pat[64];
			snprintf(pat, sizeof(pat), ROOT "/g%02d/*", (k * OWN_GROUPS + j) % GROUPS);
			client_send(C[k], b, osc_build(b, sizeof(b), "/backline/sub", "sf", pat, (double)SUB_RATE));
		}
	pump(now_ns() + 10ull * 1000000000ull, all_init);
	if (!all_init())
		{ for (int k = 0; k < NCLIENTS; k++) fprintf(stderr, "client %d: %d initial values, %llu messages\n", k, C[k]->seen_init, (unsigned long long)C[k]->received); die("subscribe: not every initial value arrived"); }
	double sub_ms = ms(now_ns() - t0), sub_cpu = ms(ctl_cpu_ns() - cpu0);

	/* ---- every client moves its own 625 parameters, 4 times, in bundles
	 * of 25 every 5 ms; the program echoes every set */
	phase = 2;
	uint64_t fan0 = 0;
	for (int k = 0; k < NCLIENTS; k++)
		fan0 += C[k]->received;
	lat = calloc((size_t)NCLIENTS * OWN_GROUPS * PER_GROUP * ROUNDS, sizeof(*lat));
	uint64_t ss0 = prop_t(probe, "SetSignals"), cs0 = prop_t(probe, "ChangedSignals");
	unsigned long up0 = atomic_load(&own.updates);
	cpu0 = ctl_cpu_ns();
	t0 = now_ns();
	int per_client = OWN_GROUPS * PER_GROUP, steps = per_client / BUNDLE;
	for (int round = 0; round < ROUNDS; round++)
		for (int step = 0; step < steps; step++) {
			uint64_t at = t0 + (uint64_t)(round * steps + step) * PACE_NS;
			pump(at, NULL);
			for (int k = 0; k < NCLIENTS; k++) {
				uint8_t b[8192];
				size_t len = osc_bundle_begin(b, sizeof(b));
				uint64_t ts = now_ns();
				for (int x = 0; x < BUNDLE; x++) {
					int j = step * BUNDLE + x;
					int i = k * OWN_GROUPS * PER_GROUP + j;
					char p[64];
					uint8_t msg[128];
					size_t mn = osc_build(msg, sizeof(msg), param_path(i, p, sizeof(p)), "f",
							      (double)(1 + round * 1000 + j));
					len = osc_bundle_add(b, len, sizeof(b), msg, mn);
					C[k]->sent_ns[i][round] = ts;
				}
				client_send(C[k], b, len);
			}
		}
	uint64_t sent_end = now_ns();
	pump(now_ns() + 10ull * 1000000000ull, all_echoed);
	double sweep_ms = ms(now_ns() - t0), sweep_send_ms = ms(sent_end - t0), sweep_cpu = ms(ctl_cpu_ns() - cpu0);
	int echoes = total_echoes(), want_echoes = NCLIENTS * per_client * ROUNDS;
	uint64_t sweep_set_signals = prop_t(probe, "SetSignals") - ss0, sweep_changed = prop_t(probe, "ChangedSignals") - cs0;
	unsigned long sweep_updates = atomic_load(&own.updates) - up0;
	qsort(lat, n_lat, sizeof(*lat), cmp_d);
	double p50 = pct(lat, n_lat, 50), p99 = pct(lat, n_lat, 99), pmax = n_lat ? lat[n_lat - 1] : NAN;
	uint64_t fan = 0;
	for (int k = 0; k < NCLIENTS; k++)
		fan += C[k]->received;
	fan -= fan0;

	/* ---- checks: a SetMany with one unknown path passes nothing on; a
	 * client that drops and remakes its subscriptions (reusing index slots)
	 * gets every value again */
	{
		uint64_t req0 = prop_t(probe, "SetRequests");
		sd_bus_message_new_method_call(cl, &m, CTL_BUS, PRM_PATH, PRM_IFACE, "SetMany");
		sd_bus_message_append(m, "a(sv)a{sv}", 2, ROOT "/g00/p000", "d", 1.0, ROOT "/g00/nope", "d", 1.0, 0);
		sd_bus_error_free(&e);
		int r = sd_bus_call(cl, m, 0, &e, NULL);
		sd_bus_message_unref(m);
		if (r >= 0 || !sd_bus_error_has_name(&e, "io.github.manuaudio.Backline.Error.NotFound"))
			die("SetMany with an unknown path was not refused (%s)", e.name ? e.name : "accepted");
		if (prop_t(probe, "SetRequests") != req0)
			die("a refused SetMany passed values on");
		sd_bus_error_free(&e);
		uint8_t b[256];
		client_send(C[0], b, osc_build(b, sizeof(b), "/backline/unsub", "s", "*"));
		pump(now_ns() + 100ull * 1000000ull, NULL);
		memset(C[0]->got_init, 0, sizeof(C[0]->got_init));
		C[0]->seen_init = 0;
		for (int j = SUB_GROUPS - 1; j >= 0; j--) {
			char pat[64];
			snprintf(pat, sizeof(pat), ROOT "/g%02d/*", j % GROUPS);
			client_send(C[0], b, osc_build(b, sizeof(b), "/backline/sub", "sf", pat, (double)SUB_RATE));
		}
		pump(now_ns() + 5ull * 1000000000ull, all_init);
		if (!all_init())
			die("resubscribe: client 0 got %d of %d values", C[0]->seen_init, SUB_GROUPS * PER_GROUP);
	}

	/* ---- one SetMany of all 5000 values, to an owner with set-many, then
	 * to one without (5000 Set signals, 5000 Updates) */
	struct bulk_result b1 = bulk_set(cl, probe, &own, 5000000.0f);
	free(schema);
	schema = schema_json(false);
	/* sd-bus is not thread-safe: the owner thread stops while main uses its bus */
	atomic_store(&own.stop, true);
	pthread_join(ot, NULL);
	if (sd_bus_call_method(own.bus, CTL_BUS, PRM_PATH, PRM_IFACE, "Register", &e, &rep, "ss", ROOT, schema) < 0)
		die("re-Register: %s", e.message);
	sd_bus_message_unref(rep);
	atomic_store(&own.stop, false);
	pthread_create(&ot, NULL, owner_thread, &own);
	/* the new tree's values (0) reach the subscribers first */
	pump(now_ns() + 300ull * 1000000ull, NULL);
	struct bulk_result b2 = bulk_set(cl, probe, &own, 6000000.0f);
	long hwm = ctl_hwm_kb();

	atomic_store(&own.stop, true);
	pthread_join(ot, NULL);

	bool ok = echoes == want_echoes && register_ms < LIM_REGISTER_MS && b1.fanout_ms < LIM_BULK_FANOUT_MS &&
		  b2.fanout_ms < LIM_BULK_FANOUT_MS && p99 < LIM_ECHO_P99_MS && b1.set_signals == 1;
	char out[4096];
	snprintf(out, sizeof(out),
		 "{\"params\": %d, \"clients\": %d, \"per_client\": %d,\n"
		 " \"register_ms\": %.1f, \"register_cpu_ms\": %.1f,\n"
		 " \"get_5000_ms\": %.2f, \"get_matching_5000_ms\": %.2f, \"list_5000_ms\": %.2f,\n"
		 " \"subscribe_all_ms\": %.1f, \"subscribe_cpu_ms\": %.1f,\n"
		 " \"sweep\": {\"sets\": %d, \"send_ms\": %.1f, \"sets_per_s\": %.0f, \"echoes\": %d, \"echo_p50_ms\": %.2f,"
		 " \"echo_p99_ms\": %.2f, \"echo_max_ms\": %.2f, \"osc_values_delivered\": %llu,"
		 " \"set_many_signals\": %llu, \"owner_updates\": %lu, \"changed_signals\": %llu, \"cpu_ms\": %.1f,"
		 " \"cpu_pct\": %.1f},\n"
		 " \"bulk_set_many_owner\": {\"call_ms\": %.2f, \"fanout_16000_ms\": %.1f, \"cpu_ms\": %.1f,"
		 " \"set_many_signals\": %llu, \"owner_updates\": %lu, \"changed_signals\": %llu},\n"
		 " \"bulk_plain_owner\": {\"call_ms\": %.2f, \"fanout_16000_ms\": %.1f, \"cpu_ms\": %.1f,"
		 " \"owner_updates\": %lu, \"changed_signals\": %llu},\n"
		 " \"controld_peak_rss_kb\": %ld, \"ok\": %s}\n",
		 NPARAMS, NCLIENTS, SUB_GROUPS * PER_GROUP, register_ms, register_cpu, get_ms, getm_ms, list_ms, sub_ms, sub_cpu,
		 want_echoes, sweep_send_ms, want_echoes / (sweep_send_ms / 1000.0), echoes, p50, p99, pmax,
		 (unsigned long long)fan, (unsigned long long)sweep_set_signals, sweep_updates,
		 (unsigned long long)sweep_changed, sweep_cpu, 100.0 * sweep_cpu / sweep_ms, b1.call_ms, b1.fanout_ms, b1.cpu_ms,
		 (unsigned long long)b1.set_signals, b1.owner_updates, (unsigned long long)b1.changed_signals, b2.call_ms,
		 b2.fanout_ms, b2.cpu_ms, b2.owner_updates, (unsigned long long)b2.changed_signals, hwm, ok ? "true" : "false");
	fputs(out, stdout);
	if (json_out) {
		FILE *f = fopen(json_out, "w");
		if (f) {
			fputs(out, f);
			fclose(f);
		}
	}
	if (!ok)
		fprintf(stderr, "params-load: FAIL (echoes %d/%d, register %.0f ms, fan-out %.0f/%.0f ms, p99 %.1f ms, "
				"SetMany signals %llu)\n",
			echoes, want_echoes, register_ms, b1.fanout_ms, b2.fanout_ms, p99, (unsigned long long)b1.set_signals);
	return ok ? 0 : 1;
}
