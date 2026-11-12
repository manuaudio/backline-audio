/* backline-hostd: the plugin-host supervisor.
 * SPDX-License-Identifier: MIT
 *
 * One plugin instance = one backline-plugin-worker process = one PipeWire
 * node. hostd starts each worker in its own transient systemd user scope
 * (backline-worker-<id>-<generation>.scope), so it gets its own cgroup,
 * resource limits, journald attribution and clean teardown, and it never
 * loads plugin code itself.
 *
 * When a worker dies without being asked to, hostd emits Crashed(), then
 * starts it again with the same options and the same state file. The worker
 * saves the plugin's state (CLAP, VST3 or LV2) to that file atomically
 * whenever it changes, and loads
 * it on start, so the new process comes back with the parameters the old one
 * had: same node name, same OSC port, same D-Bus name, same links.
 *
 * D-Bus (session): io.github.manuaudio.Backline.Host at
 * /io/github/manuaudio/Backline/Host, interface io.github.manuaudio.Backline.Host1.
 *
 * Event loop: sd-event (D-Bus, child exits, worker stdout, timers). Single
 * threaded. Workers get PR_SET_PDEATHSIG, so they never outlive hostd.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "policy.h"

#define BUS_NAME "io.github.manuaudio.Backline.Host"
#define OBJ_PATH "/io/github/manuaudio/Backline/Host"
#define IFACE "io.github.manuaudio.Backline.Host1"
#define ERR_PREFIX "io.github.manuaudio.Backline.Error."
#define VERSION "0.2.0"

enum inst_state { ST_STARTING, ST_RUNNING, ST_BACKOFF, ST_STOPPING, ST_FAILED };
static const char *state_names[] = { "starting", "running", "restarting", "stopping", "failed" };

struct instance {
	struct instance *next;
	/* what was asked for (kept across restarts) */
	char id[64];
	char plugin[1024];
	char plugin_id[256];
	char format[8];           /* "clap", "vst3", "lv2", or "" = from the plugin argument */
	char node[256];
	char target[256];
	char midi_follow[512];
	int autoconnect;          /* -1 default, 0, 1 */
	bool restart;
	bool force;               /* load even if the registry grades it fail */
	char memory_max[32];
	char cpu_quota[32];
	char tasks_max[16];
	char state_path[1200];
	char log_path[1200];

	/* runtime */
	enum inst_state st;
	pid_t pid;
	unsigned generation;      /* 0 = first start */
	unsigned restarts;
	bool ever_ready;
	bool unloading;
	int last_signal;          /* signal that killed the last worker, 0 if none */
	int last_status;          /* exit status of the last worker, -1 if killed */
	char unit[160];
	char dbus[256];
	char plugin_path[1024];
	char plugin_clap_id[256];  /* the plugin's id as the worker reports it (any format) */
	char plugin_format[8];     /* as the worker reports it */
	unsigned osc_port;
	uint64_t started_ns, ready_ns, crashed_ns;
	char fail_reason[256];
	struct bl_restart_policy policy;

	int out_fd;
	char line[4096];
	size_t line_len;
	sd_event_source *io_src, *child_src, *timer_src;
	sd_bus_message *pending_load;   /* Load() waits for the worker to be ready */
	sd_bus_message *pending_unload; /* Unload() waits for the worker to exit */
};

static struct {
	sd_event *event;
	sd_bus *bus;
	struct instance *instances;
	char worker[PATH_MAX];
	char state_dir[PATH_MAX];
	char log_dir[PATH_MAX];
	bool use_scope;
	bool quitting;
	unsigned counter;
	char default_memory_max[32];
	uint64_t ready_timeout_ns;
	char registry[PATH_MAX];  /* plugins.json of backline-registry ("" = none) */
} H;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("backline-hostd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * BL_NSEC + (uint64_t)ts.tv_nsec;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
				return -errno;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

static struct instance *find(const char *id)
{
	for (struct instance *i = H.instances; i; i = i->next)
		if (!strcmp(i->id, id) || !strcmp(i->node, id))
			return i;
	return NULL;
}

static void remove_instance(struct instance *in)
{
	struct instance **pp = &H.instances;
	while (*pp && *pp != in)
		pp = &(*pp)->next;
	if (*pp)
		*pp = in->next;
	sd_event_source_disable_unref(in->io_src);
	sd_event_source_disable_unref(in->child_src);
	sd_event_source_disable_unref(in->timer_src);
	if (in->out_fd >= 0)
		close(in->out_fd);
	if (in->pending_load)
		sd_bus_message_unref(in->pending_load);
	if (in->pending_unload)
		sd_bus_message_unref(in->pending_unload);
	free(in);
}

/* Last few lines of the worker's log: the reason a Load failed. */
static void log_tail(const struct instance *in, char *out, size_t n)
{
	out[0] = 0;
	FILE *f = fopen(in->log_path, "r");
	if (!f)
		return;
	char buf[8192];
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	long off = size > (long)sizeof(buf) - 1 ? size - (long)sizeof(buf) + 1 : 0;
	fseek(f, off, SEEK_SET);
	size_t len = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[len] = 0;
	while (len > 0 && buf[len - 1] == '\n')
		buf[--len] = 0;
	/* last 3 lines */
	int lines = 0;
	char *p = buf + len;
	while (p > buf && lines < 3)
		if (*--p == '\n')
			lines++;
	if (*p == '\n')
		p++;
	snprintf(out, n, "%s", p);
}

static void emit(const char *signal, const char *types, ...)
{
	if (!H.bus)
		return;
	sd_bus_message *m = NULL;
	if (sd_bus_message_new_signal(H.bus, &m, OBJ_PATH, IFACE, signal) < 0)
		return;
	va_list ap;
	va_start(ap, types);
	int r = sd_bus_message_appendv(m, types, ap);
	va_end(ap);
	if (r >= 0)
		sd_bus_send(H.bus, m, NULL);
	sd_bus_message_unref(m);
}

/* --------------------------------------------------------------- registry */
static bool ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcasecmp(s + a - b, suf);
}

static bool is_uri(const char *s)
{
	return strstr(s, "://") || !strncmp(s, "urn:", 4);
}

/* The worker's plugin lookup, mirrored: a path (.clap file, .vst3 or .lv2
 * bundle), an LV2 URI, or a name in the standard directories (CLAP, then
 * VST3, then LV2; or only `format`'s). Fills the real path ("" for an LV2
 * URI or when not found) and the format. */
static void resolve_plugin(const char *p, const char *format, char *out, size_t n, char *fmt_out, size_t fn)
{
	char cand[PATH_MAX], c[PATH_MAX];
	const char *home = getenv("HOME");
	out[0] = 0;
	snprintf(fmt_out, fn, "%s", format && format[0] ? format : "clap");
	if (realpath(p, cand)) {
		struct stat st;
		snprintf(out, n, "%s", cand);
		if (!format || !format[0]) {
			bool dir = stat(cand, &st) == 0 && S_ISDIR(st.st_mode);
			snprintf(fmt_out, fn, "%s", ends_with(cand, ".vst3") ? "vst3" : ends_with(cand, ".lv2") && dir ? "lv2" : "clap");
		}
		return;
	}
	if (is_uri(p) && (!format || !format[0] || !strcmp(format, "lv2"))) {
		snprintf(fmt_out, fn, "lv2");
		return;
	}
	struct { const char *fmt, *base, *dir, *suffix; } dirs[] = {
		{ "clap", home, "/.clap", ".clap" }, { "clap", "", "/usr/lib/clap", ".clap" },
		{ "clap", "", "/usr/local/lib/clap", ".clap" },
		{ "vst3", home, "/.vst3", ".vst3" }, { "vst3", "", "/usr/lib/vst3", ".vst3" },
		{ "vst3", "", "/usr/local/lib/vst3", ".vst3" },
		{ "lv2", home, "/.lv2", ".lv2" }, { "lv2", "", "/usr/lib/lv2", ".lv2" }, { "lv2", "", "/usr/local/lib/lv2", ".lv2" },
	};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		if (format && format[0] && strcmp(format, dirs[i].fmt))
			continue;
		snprintf(c, sizeof(c), "%s%s/%s%s", dirs[i].base ? dirs[i].base : "", dirs[i].dir, p,
			 ends_with(p, dirs[i].suffix) ? "" : dirs[i].suffix);
		if (realpath(c, cand)) {
			snprintf(out, n, "%s", cand);
			snprintf(fmt_out, fn, "%s", dirs[i].fmt);
			return;
		}
	}
}

/* What the registry says about (path, id). id NULL: decided only if every
 * plugin in the file has the same grade. Returns 1 found (grade/reasons
 * filled), 0 unknown. */
static int registry_grade(const char *format, const char *path, const char *id, char *grade, size_t gn, char *reasons,
			  size_t rn)
{
	if (!H.registry[0] || (!path[0] && !(id && id[0])))
		return 0;
	char want[PATH_MAX] = "";
	if (path[0] && !realpath(path, want))
		snprintf(want, sizeof(want), "%s", path);
	struct json_object *root = json_object_from_file(H.registry), *list;
	if (!root)
		return 0;
	int found = 0, matches = 0;
	char first[16] = "";
	bool mixed = false;
	if (json_object_object_get_ex(root, "plugins", &list) && json_object_is_type(list, json_type_array)) {
		size_t n = json_object_array_length(list);
		for (size_t i = 0; i < n; i++) {
			struct json_object *e = json_object_array_get_idx(list, i), *v;
			if (!json_object_object_get_ex(e, "format", &v) || strcmp(json_object_get_string(v), format))
				continue;
			if (want[0]) {
				char rp[PATH_MAX];
				if (!json_object_object_get_ex(e, "path", &v) || !realpath(json_object_get_string(v), rp) ||
				    strcmp(rp, want))
					continue;
			}
			if (id && (!json_object_object_get_ex(e, "id", &v) || strcmp(json_object_get_string(v), id)))
				continue;
			const char *g = json_object_object_get_ex(e, "grade", &v) ? json_object_get_string(v) : "";
			if (!matches++)
				snprintf(first, sizeof(first), "%s", g);
			else if (strcmp(first, g))
				mixed = true;
			if (!found || !strcmp(g, "fail")) {
				snprintf(grade, gn, "%s", g);
				reasons[0] = 0;
				struct json_object *rs;
				if (json_object_object_get_ex(e, "reasons", &rs) && json_object_is_type(rs, json_type_array))
					for (size_t k = 0; k < json_object_array_length(rs); k++)
						snprintf(reasons + strlen(reasons), rn - strlen(reasons), "%s%s", k ? "; " : "",
							 json_object_get_string(json_object_array_get_idx(rs, k)));
				found = 1;
			}
		}
	}
	json_object_put(root);
	return found && !mixed ? 1 : 0;
}

/* Kill whatever is left in a dead worker's scope (helper processes a plugin
 * started), so a crash never leaves processes behind. */
static void kill_scope(const struct instance *in)
{
	if (!H.use_scope || !H.bus || !in->unit[0])
		return;
	char unit[200];
	snprintf(unit, sizeof(unit), "%s.scope", in->unit);
	sd_bus_call_method_async(H.bus, NULL, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
				 "org.freedesktop.systemd1.Manager", "KillUnit", NULL, NULL, "ssi", unit, "all", SIGKILL);
}

/* ---------------------------------------------------------------- workers */
static int spawn(struct instance *in);
static int on_child(sd_event_source *s, const siginfo_t *si, void *userdata);

static int on_restart_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct instance *in = userdata;
	in->timer_src = sd_event_source_disable_unref(in->timer_src);
	if (in->st != ST_BACKOFF)
		return 0;
	in->generation++;
	in->restarts++;
	if (spawn(in) < 0) {
		in->st = ST_FAILED;
		snprintf(in->fail_reason, sizeof(in->fail_reason), "could not start the worker: %s", strerror(errno));
		emit("Failed", "ss", in->id, in->fail_reason);
	}
	return 0;
}

static int on_kill_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct instance *in = userdata;
	in->timer_src = sd_event_source_disable_unref(in->timer_src);
	if (in->pid > 0) {
		logf_("%s: worker %d did not exit in time; SIGKILL", in->id, (int)in->pid);
		kill(in->pid, SIGKILL);
	}
	return 0;
}

static int on_ready_timeout(sd_event_source *s, uint64_t usec, void *userdata)
{
	struct instance *in = userdata;
	in->timer_src = sd_event_source_disable_unref(in->timer_src);
	if (in->st == ST_STARTING && in->pid > 0) {
		logf_("%s: worker %d not ready after %" PRIu64 " s; SIGKILL", in->id, (int)in->pid,
		      (uint64_t)(H.ready_timeout_ns / BL_NSEC));
		snprintf(in->fail_reason, sizeof(in->fail_reason), "worker not ready after %" PRIu64 " s",
			 (uint64_t)(H.ready_timeout_ns / BL_NSEC));
		kill(in->pid, SIGKILL);
	}
	return 0;
}

static void arm_timer(struct instance *in, uint64_t delay_ns, sd_event_time_handler_t cb)
{
	in->timer_src = sd_event_source_disable_unref(in->timer_src);
	sd_event_add_time_relative(H.event, &in->timer_src, CLOCK_MONOTONIC, delay_ns / 1000, 1000, cb, in);
}

static void stop_worker(struct instance *in);

static void became_ready(struct instance *in, const struct bl_ready *r)
{
	if (!in->ever_ready && !in->force) {
		char grade[16] = "", reasons[512] = "";
		if (registry_grade(r->format[0] ? r->format : "clap", r->plugin_path, r->plugin, grade, sizeof(grade), reasons,
				   sizeof(reasons)) == 1 &&
		    !strcmp(grade, "fail")) {
			logf_("%s: the registry grades %s fail (%s); stopping it (load with force to override)", in->id,
			      r->plugin, reasons);
			if (in->pending_load) {
				sd_bus_reply_method_errorf(in->pending_load, ERR_PREFIX "GradedFail",
							   "the registry grades %s fail: %s (load with force=true to run it anyway)",
							   r->plugin, reasons);
				in->pending_load = sd_bus_message_unref(in->pending_load);
			}
			in->unloading = true;
			stop_worker(in);
			return;
		}
	}
	in->st = ST_RUNNING;
	in->ready_ns = now_ns();
	in->osc_port = r->osc_port;
	snprintf(in->dbus, sizeof(in->dbus), "%s", r->dbus);
	snprintf(in->plugin_path, sizeof(in->plugin_path), "%s", r->plugin_path);
	snprintf(in->plugin_clap_id, sizeof(in->plugin_clap_id), "%s", r->plugin);
	snprintf(in->plugin_format, sizeof(in->plugin_format), "%s", r->format[0] ? r->format : "clap");
	in->timer_src = sd_event_source_disable_unref(in->timer_src);
	double ms = (double)(in->ready_ns - (in->generation ? in->crashed_ns : in->started_ns)) / 1e6;
	if (in->generation == 0) {
		logf_("%s: ready: pid %d, node %s, OSC %u, %s (%.0f ms)", in->id, (int)in->pid, in->node,
		      in->osc_port, in->dbus, ms);
		emit("Loaded", "s", in->id);
	} else {
		logf_("%s: restarted (#%u): pid %d, %.0f ms after the crash", in->id, in->restarts, (int)in->pid, ms);
		emit("Restarted", "suu", in->id, (uint32_t)in->pid, (uint32_t)in->restarts);
	}
	in->ever_ready = true;
	if (in->pending_load) {
		sd_bus_reply_method_return(in->pending_load, "s", in->id);
		in->pending_load = sd_bus_message_unref(in->pending_load);
	}
}

static int on_worker_out(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	struct instance *in = userdata;
	char buf[2048];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return 0;
		in->io_src = sd_event_source_disable_unref(in->io_src);
		close(in->out_fd);
		in->out_fd = -1;
		return 0;
	}
	for (ssize_t k = 0; k < n; k++) {
		char c = buf[k];
		if (c != '\n') {
			if (in->line_len + 1 < sizeof(in->line))
				in->line[in->line_len++] = c;
			continue;
		}
		in->line[in->line_len] = 0;
		in->line_len = 0;
		struct bl_ready r;
		if (in->st == ST_STARTING && in->line[0] == '{' && bl_parse_ready(in->line, &r) == 0)
			became_ready(in, &r);
	}
	return 0;
}

static void build_argv(struct instance *in, char **argv, int max, char store[][1400])
{
	int a = 0, k = 0;
#define ARG(...) do { snprintf(store[k], sizeof(store[k]), __VA_ARGS__); argv[a++] = store[k++]; } while (0)
	if (H.use_scope) {
		ARG("systemd-run");
		ARG("--user");
		ARG("--scope");
		ARG("--quiet");
		ARG("--collect");
		ARG("--unit=%s", in->unit);
		ARG("--description=Backline plugin worker %s (%s)", in->id, in->plugin);
		if (in->memory_max[0])
			ARG("--property=MemoryMax=%s", in->memory_max);
		if (in->cpu_quota[0])
			ARG("--property=CPUQuota=%s", in->cpu_quota);
		if (in->tasks_max[0])
			ARG("--property=TasksMax=%s", in->tasks_max);
		ARG("--");
	}
	ARG("%s", H.worker);
	ARG("--plugin=%s", in->plugin);
	if (in->format[0])
		ARG("--format=%s", in->format);
	if (in->plugin_id[0])
		ARG("--id=%s", in->plugin_id);
	ARG("--name=%s", in->node);
	ARG("--state-file=%s", in->state_path);
	if (in->target[0])
		ARG("--audio-target=%s", in->target);
	if (in->midi_follow[0])
		ARG("--midi-follow=%s", in->midi_follow);
	if (in->autoconnect == 1)
		ARG("--autoconnect");
	else if (in->autoconnect == 0)
		ARG("--no-autoconnect");
	if (in->osc_port)
		ARG("--osc-port=%u", in->osc_port); /* a restart keeps the port clients know */
#undef ARG
	argv[a] = NULL;
	(void)max;
}

static int spawn(struct instance *in)
{
	char store[32][1400];
	char *argv[33];
	/* A new unit name per generation: the dead worker's scope may still be
	 * winding down when its successor starts. */
	snprintf(in->unit, sizeof(in->unit), "backline-worker-%s-%u", in->id, in->generation);
	build_argv(in, argv, 32, store);

	int pfd[2];
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -1;
	int logfd = open(in->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	pid_t parent = getpid();
	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		if (logfd >= 0)
			close(logfd);
		return -1;
	}
	if (pid == 0) {
		/* child: never outlive hostd; clean signal state; stdout = ready pipe */
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(127);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0)
			dup2(devnull, 0);
		dup2(pfd[1], 1);
		dup2(logfd >= 0 ? logfd : 2, 2);
		closefrom(3);
		execvp(argv[0], argv);
		fprintf(stderr, "backline-hostd: exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(pfd[1]);
	if (logfd >= 0) {
		dprintf(logfd, "---- backline-hostd: start %s generation %u, pid %d, unit %s\n", in->id, in->generation,
			(int)pid, H.use_scope ? in->unit : "(none)");
		close(logfd);
	}
	in->pid = pid;
	in->st = ST_STARTING;
	in->started_ns = now_ns();
	in->out_fd = pfd[0];
	in->line_len = 0;
	fcntl(in->out_fd, F_SETFL, O_NONBLOCK);
	sd_event_add_io(H.event, &in->io_src, in->out_fd, EPOLLIN, on_worker_out, in);
	sd_event_add_child(H.event, &in->child_src, pid, WEXITED, on_child, in);
	arm_timer(in, H.ready_timeout_ns, on_ready_timeout);
	return 0;
}

static int on_child(sd_event_source *s, const siginfo_t *si, void *userdata)
{
	struct instance *in = userdata;
	in->child_src = sd_event_source_disable_unref(in->child_src);
	in->timer_src = sd_event_source_disable_unref(in->timer_src);
	int sig = (si->si_code == CLD_KILLED || si->si_code == CLD_DUMPED) ? si->si_status : 0;
	int status = sig ? -1 : si->si_status;
	pid_t pid = in->pid;
	in->pid = 0;
	in->last_signal = sig;
	in->last_status = status;
	kill_scope(in);
	/* drain anything left on the pipe (a ready line racing the exit) */
	if (in->io_src) {
		in->io_src = sd_event_source_disable_unref(in->io_src);
		close(in->out_fd);
		in->out_fd = -1;
	}

	if (in->st == ST_STOPPING) {
		logf_("%s: worker %d stopped", in->id, (int)pid);
		if (in->unloading) {
			if (in->pending_unload)
				sd_bus_reply_method_return(in->pending_unload, "");
			emit("Unloaded", "s", in->id);
			remove_instance(in);
		} else {
			/* an explicit Restart(): start right away */
			in->st = ST_BACKOFF;
			arm_timer(in, 0, on_restart_timer);
		}
		return 0;
	}

	uint64_t now = now_ns();
	in->crashed_ns = now;
	logf_("%s: worker %d died (%s %d)%s", in->id, (int)pid, sig ? "signal" : "exit status", sig ? sig : status,
	      in->ever_ready ? "" : " before it was ready");
	emit("Crashed", "sii", in->id, (int32_t)sig, (int32_t)status);

	if (!in->ever_ready) {
		/* It never came up: a load error, not a crash to retry. */
		char tail[600];
		log_tail(in, tail, sizeof(tail));
		if (tail[0])
			logf_("%s: worker log: %s", in->id, tail);
		if (in->pending_load) {
			sd_bus_reply_method_errorf(in->pending_load, ERR_PREFIX "LoadFailed", "%s did not start (%s %d)%s%s%s%s",
						   in->plugin, sig ? "signal" : "exit status", sig ? sig : status,
						   in->fail_reason[0] ? "; " : "", in->fail_reason,
						   tail[0] ? "; worker said: " : "", tail);
			in->pending_load = sd_bus_message_unref(in->pending_load);
		}
		remove_instance(in);
		return 0;
	}
	if (!in->restart || H.quitting) {
		in->st = ST_FAILED;
		snprintf(in->fail_reason, sizeof(in->fail_reason), "worker died (%s %d), restart disabled",
			 sig ? "signal" : "exit status", sig ? sig : status);
		emit("Failed", "ss", in->id, in->fail_reason);
		return 0;
	}
	uint64_t ran = in->ready_ns > in->started_ns ? now - in->started_ns : 0;
	uint64_t delay = bl_restart_policy_crash(&in->policy, now, ran);
	if (delay == BL_RESTART_GIVE_UP) {
		in->st = ST_FAILED;
		snprintf(in->fail_reason, sizeof(in->fail_reason), "%u crashes in %" PRIu64 " s; giving up",
			 in->policy.max_crashes + 1, (uint64_t)(in->policy.window_ns / BL_NSEC));
		logf_("%s: %s", in->id, in->fail_reason);
		emit("Failed", "ss", in->id, in->fail_reason);
		return 0;
	}
	in->st = ST_BACKOFF;
	logf_("%s: restarting in %" PRIu64 " ms with state %s", in->id, (uint64_t)(delay / 1000000), in->state_path);
	arm_timer(in, delay, on_restart_timer);
	return 0;
}

static void stop_worker(struct instance *in)
{
	if (in->pid > 0) {
		in->st = ST_STOPPING;
		kill(in->pid, SIGTERM);
		arm_timer(in, 3 * BL_NSEC, on_kill_timer);
	}
}

/* ------------------------------------------------------------------ D-Bus */
static int append_info(sd_bus_message *r, const struct instance *in)
{
	int e = 0;
#define KV(k, t, v) do { e |= sd_bus_message_open_container(r, 'e', "sv"); \
	e |= sd_bus_message_append(r, "s", k); \
	e |= sd_bus_message_append(r, "v", t, v); \
	e |= sd_bus_message_close_container(r); } while (0)
	e |= sd_bus_message_open_container(r, 'a', "{sv}");
	KV("id", "s", in->id);
	KV("plugin", "s", in->plugin);
	KV("plugin-path", "s", in->plugin_path);
	KV("plugin-id", "s", in->plugin_clap_id[0] ? in->plugin_clap_id : in->plugin_id);
	KV("format", "s", in->plugin_format[0] ? in->plugin_format : in->format);
	KV("node", "s", in->node);
	KV("dbus", "s", in->dbus);
	KV("target", "s", in->target);
	KV("midi-follow", "s", in->midi_follow);
	KV("state", "s", state_names[in->st]);
	KV("pid", "u", (uint32_t)(in->pid > 0 ? in->pid : 0));
	KV("osc-port", "u", (uint32_t)in->osc_port);
	KV("restarts", "u", (uint32_t)in->restarts);
	KV("last-signal", "i", (int32_t)in->last_signal);
	KV("last-exit-status", "i", (int32_t)in->last_status);
	char unit[200];
	snprintf(unit, sizeof(unit), "%s%s", in->unit, in->unit[0] ? ".scope" : "");
	KV("unit", "s", H.use_scope ? unit : "");
	KV("state-file", "s", in->state_path);
	KV("log-file", "s", in->log_path);
	KV("restart", "b", (int)in->restart);
	KV("fail-reason", "s", in->fail_reason);
	e |= sd_bus_message_close_container(r);
#undef KV
	return e < 0 ? -EINVAL : 0;
}

static int m_load(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *plugin;
	int r = sd_bus_message_read(m, "s", &plugin);
	if (r < 0)
		return r;
	if (!plugin[0])
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "empty plugin");

	struct instance *in = calloc(1, sizeof(*in));
	if (!in)
		return -ENOMEM;
	in->out_fd = -1;
	in->autoconnect = -1;
	in->restart = true;
	in->last_status = 0;
	snprintf(in->plugin, sizeof(in->plugin), "%s", plugin);
	snprintf(in->memory_max, sizeof(in->memory_max), "%s", H.default_memory_max);
	snprintf(in->tasks_max, sizeof(in->tasks_max), "512");
	bl_restart_policy_init(&in->policy);
	char initial_state[1024] = "";

	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		goto bad;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key, *contents;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0)
			goto bad;
		if ((r = sd_bus_message_peek_type(m, NULL, &contents)) < 0)
			goto bad;
		if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0)
			goto bad;
		if (!strcmp(contents, "s")) {
			const char *v;
			if ((r = sd_bus_message_read(m, "s", &v)) < 0)
				goto bad;
			char *dst = NULL;
			size_t n = 0;
			if (!strcmp(key, "id")) { dst = in->id; n = sizeof(in->id); }
			else if (!strcmp(key, "plugin-id")) { dst = in->plugin_id; n = sizeof(in->plugin_id); }
			else if (!strcmp(key, "name")) { dst = in->node; n = sizeof(in->node); }
			else if (!strcmp(key, "target")) { dst = in->target; n = sizeof(in->target); }
			else if (!strcmp(key, "midi-follow")) { dst = in->midi_follow; n = sizeof(in->midi_follow); }
			else if (!strcmp(key, "memory-max")) { dst = in->memory_max; n = sizeof(in->memory_max); }
			else if (!strcmp(key, "cpu-quota")) { dst = in->cpu_quota; n = sizeof(in->cpu_quota); }
			else if (!strcmp(key, "state")) { dst = initial_state; n = sizeof(initial_state); }
			else if (!strcmp(key, "format")) { dst = in->format; n = sizeof(in->format); }
			if (!dst) {
				sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown option '%s'", key);
				r = -EINVAL;
				goto out;
			}
			snprintf(dst, n, "%s", v);
		} else if (!strcmp(contents, "b")) {
			int v;
			if ((r = sd_bus_message_read(m, "b", &v)) < 0)
				goto bad;
			if (!strcmp(key, "autoconnect"))
				in->autoconnect = v ? 1 : 0;
			else if (!strcmp(key, "restart"))
				in->restart = v;
			else if (!strcmp(key, "force"))
				in->force = v;
			else {
				sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown option '%s'", key);
				r = -EINVAL;
				goto out;
			}
		} else {
			sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "option '%s' has unsupported type '%s'", key, contents);
			r = -EINVAL;
			goto out;
		}
		sd_bus_message_exit_container(m);
		sd_bus_message_exit_container(m);
	}
	if (r < 0)
		goto bad;
	sd_bus_message_exit_container(m);

	if (in->format[0] && strcmp(in->format, "clap") && strcmp(in->format, "vst3") && strcmp(in->format, "lv2")) {
		sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "format '%s' is not clap, vst3 or lv2", in->format);
		r = -EINVAL;
		goto out;
	}
	char rpath[PATH_MAX], rfmt[8];
	resolve_plugin(in->plugin, in->format, rpath, sizeof(rpath), rfmt, sizeof(rfmt));

	/* id: given, or <plugin slug>-<n> */
	if (in->id[0]) {
		char slug[64];
		if (bl_slug(in->id, slug, sizeof(slug)) == 0 || strcmp(slug, in->id)) {
			sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "id '%s' must be lower-case [a-z0-9-] (try '%s')", in->id, slug);
			r = -EINVAL;
			goto out;
		}
	} else {
		char trimmed[1024];
		snprintf(trimmed, sizeof(trimmed), "%s", plugin);
		for (size_t tl = strlen(trimmed); tl > 1 && trimmed[tl - 1] == '/'; tl--)
			trimmed[tl - 1] = 0;
		const char *base = strrchr(trimmed, '/');
		char slug[40];
		bl_slug(base && base[1] ? base + 1 : trimmed, slug, sizeof(slug));
		size_t sl = strlen(slug);
		if (sl > 5 && (!strcmp(slug + sl - 5, "-clap") || !strcmp(slug + sl - 5, "-vst3")))
			slug[sl - 5] = 0;
		else if (sl > 4 && !strcmp(slug + sl - 4, "-lv2"))
			slug[sl - 4] = 0;
		if (!slug[0])
			snprintf(slug, sizeof(slug), "plugin");
		do
			snprintf(in->id, sizeof(in->id), "%s-%u", slug, ++H.counter);
		while (find(in->id));
	}
	if (find(in->id)) {
		sd_bus_error_setf(err, ERR_PREFIX "Exists", "instance '%s' already exists", in->id);
		r = -EEXIST;
		goto out;
	}
	if (!in->node[0])
		snprintf(in->node, sizeof(in->node), "backline.%s", in->id);
	/* <id>.clapstate, <id>.vst3state (a .vstpreset image) or <id>.lv2state (LV2 state, Turtle) */
	snprintf(in->state_path, sizeof(in->state_path), "%s/%s.%sstate", H.state_dir, in->id, rfmt);
	snprintf(in->plugin_format, sizeof(in->plugin_format), "%s", rfmt);
	snprintf(in->log_path, sizeof(in->log_path), "%s/%s.log", H.log_dir, in->id);

	/* A new instance starts from defaults, or from the state it was given;
	 * never from a stale file of an earlier instance with the same id. */
	unlink(in->state_path);
	if (initial_state[0]) {
		FILE *src = fopen(initial_state, "rb");
		if (!src) {
			sd_bus_error_setf(err, SD_BUS_ERROR_FILE_NOT_FOUND, "state file %s: %s", initial_state, strerror(errno));
			r = -ENOENT;
			goto out;
		}
		FILE *dst = fopen(in->state_path, "wb");
		char buf[65536];
		size_t n;
		while (dst && (n = fread(buf, 1, sizeof(buf), src)) > 0)
			fwrite(buf, 1, n, dst);
		fclose(src);
		if (dst)
			fclose(dst);
	}
	unlink(in->log_path);

	if (!in->force) {
		char grade[16] = "", reasons[512] = "";
		/* an LV2 URI is its own id; a bundle names the plugin with plugin-id */
		const char *gid = in->plugin_id[0] ? in->plugin_id : !rpath[0] && !strcmp(rfmt, "lv2") ? in->plugin : NULL;
		if (registry_grade(rfmt, rpath, gid, grade, sizeof(grade), reasons, sizeof(reasons)) == 1 &&
		    !strcmp(grade, "fail")) {
			sd_bus_error_setf(err, ERR_PREFIX "GradedFail",
					  "the registry grades %s fail: %s (load with force=true to run it anyway)",
					  rpath[0] ? rpath : in->plugin, reasons);
			r = -EPERM;
			goto out;
		}
	}

	in->next = H.instances;
	H.instances = in;
	if (spawn(in) < 0) {
		r = -errno;
		H.instances = in->next;
		sd_bus_error_set_errnof(err, -r, "could not start the worker: %m");
		goto out;
	}
	in->pending_load = sd_bus_message_ref(m);
	return 1; /* reply comes when the worker is ready */
bad:
	sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "Load(s plugin, a{sv} options)");
out:
	free(in);
	return r < 0 ? r : -EINVAL;
}

static int m_unload(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	struct instance *in = find(id);
	if (!in)
		return sd_bus_error_setf(err, ERR_PREFIX "NoSuchInstance", "no instance '%s'", id);
	in->unloading = true;
	if (in->pid > 0) {
		if (in->pending_unload)
			return sd_bus_error_setf(err, ERR_PREFIX "Busy", "'%s' is already being unloaded", id);
		in->pending_unload = sd_bus_message_ref(m);
		stop_worker(in);
		return 1;
	}
	emit("Unloaded", "s", in->id);
	remove_instance(in);
	return sd_bus_reply_method_return(m, "");
}

static int m_restart(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	struct instance *in = find(id);
	if (!in)
		return sd_bus_error_setf(err, ERR_PREFIX "NoSuchInstance", "no instance '%s'", id);
	bl_restart_policy_reset(&in->policy);
	in->fail_reason[0] = 0;
	if (in->pid > 0)
		stop_worker(in); /* on_child restarts it */
	else {
		in->st = ST_BACKOFF;
		arm_timer(in, 0, on_restart_timer);
	}
	return sd_bus_reply_method_return(m, "");
}

static int m_get(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *id;
	int r = sd_bus_message_read(m, "s", &id);
	if (r < 0)
		return r;
	struct instance *in = find(id);
	if (!in)
		return sd_bus_error_setf(err, ERR_PREFIX "NoSuchInstance", "no instance '%s'", id);
	sd_bus_message *reply = NULL;
	if ((r = sd_bus_message_new_method_return(m, &reply)) < 0)
		return r;
	if ((r = append_info(reply, in)) >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int m_list(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	sd_bus_message *reply = NULL;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0)
		return r;
	r = sd_bus_message_open_container(reply, 'a', "a{sv}");
	/* oldest first */
	struct instance *v[256];
	int n = 0;
	for (struct instance *i = H.instances; i && n < 256; i = i->next)
		v[n++] = i;
	for (int k = n - 1; k >= 0 && r >= 0; k--)
		r = append_info(reply, v[k]);
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	if (r >= 0)
		r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int p_version(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *r,
		     void *userdata, sd_bus_error *err)
{
	return sd_bus_message_append(r, "s", VERSION);
}

static int p_count(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *r,
		   void *userdata, sd_bus_error *err)
{
	uint32_t n = 0;
	for (struct instance *i = H.instances; i; i = i->next)
		n++;
	return sd_bus_message_append(r, "u", n);
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("Load", SD_BUS_ARGS("s", plugin, "a{sv}", options), SD_BUS_RESULT("s", id), m_load,
				SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Unload", SD_BUS_ARGS("s", id), SD_BUS_NO_RESULT, m_unload, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Restart", SD_BUS_ARGS("s", id), SD_BUS_NO_RESULT, m_restart, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Get", SD_BUS_ARGS("s", id), SD_BUS_RESULT("a{sv}", info), m_get, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("List", SD_BUS_NO_ARGS, SD_BUS_RESULT("aa{sv}", instances), m_list, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL_WITH_ARGS("Loaded", SD_BUS_ARGS("s", id), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Unloaded", SD_BUS_ARGS("s", id), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Crashed", SD_BUS_ARGS("s", id, "i", signal, "i", exit_status), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Restarted", SD_BUS_ARGS("s", id, "u", pid, "u", restarts), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Failed", SD_BUS_ARGS("s", id, "s", reason), 0),
	SD_BUS_PROPERTY("Version", "s", p_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Instances", "u", p_count, 0, 0),
	SD_BUS_VTABLE_END,
};

/* -------------------------------------------------------------- lifecycle */
static int on_quit_check(sd_event_source *s, uint64_t usec, void *userdata)
{
	bool alive = false;
	for (struct instance *i = H.instances; i; i = i->next)
		alive |= i->pid > 0;
	if (!alive)
		sd_event_exit(H.event, 0);
	else
		sd_event_source_set_time_relative(s, 20000), sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
	return 0;
}

static int on_term(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata)
{
	if (H.quitting)
		return 0;
	H.quitting = true;
	logf_("signal %u: stopping all workers", si->ssi_signo);
	for (struct instance *i = H.instances; i; i = i->next) {
		i->unloading = true;
		if (i->pid > 0)
			stop_worker(i);
	}
	sd_event_add_time_relative(H.event, NULL, CLOCK_MONOTONIC, 0, 1000, on_quit_check, NULL);
	return 0;
}

static void usage(FILE *f)
{
	fputs("usage: backline-hostd [options]\n"
	      "  --worker PATH        backline-plugin-worker to run (default: next to this binary, then $PATH)\n"
	      "  --state-dir DIR      per-instance plugin state (default: $XDG_STATE_HOME/backline/instances)\n"
	      "  --log-dir DIR        per-instance worker logs (default: $XDG_RUNTIME_DIR/backline/workers)\n"
	      "  --no-scope           run workers as plain child processes, not systemd user scopes\n"
	      "  --memory-max SIZE    default MemoryMax for each worker scope (default: 4G; '' for none)\n"
	      "  --ready-timeout SECS how long a worker may take to come up (default: 20)\n"
	      "  --registry FILE      backline-registry index to check grades against (default:\n"
	      "                       $BACKLINE_CACHE_DIR or $XDG_CACHE_HOME/backline, plugins.json; '' = off)\n"
	      "  --replace            take over the bus name from a running hostd\n",
	      f);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "worker", required_argument, NULL, 'w' }, { "state-dir", required_argument, NULL, 's' },
		{ "log-dir", required_argument, NULL, 'l' }, { "no-scope", no_argument, NULL, 'S' },
		{ "memory-max", required_argument, NULL, 'm' }, { "ready-timeout", required_argument, NULL, 't' },
		{ "replace", no_argument, NULL, 'r' }, { "registry", required_argument, NULL, 'g' },
		{ "help", no_argument, NULL, 'h' }, { 0 } };
	bool replace = false;
	int c;
	H.use_scope = true;
	H.ready_timeout_ns = 20 * BL_NSEC;
	snprintf(H.default_memory_max, sizeof(H.default_memory_max), "4G");
	while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (c) {
		case 'w': snprintf(H.worker, sizeof(H.worker), "%s", optarg); break;
		case 's': snprintf(H.state_dir, sizeof(H.state_dir), "%s", optarg); break;
		case 'l': snprintf(H.log_dir, sizeof(H.log_dir), "%s", optarg); break;
		case 'S': H.use_scope = false; break;
		case 'm': snprintf(H.default_memory_max, sizeof(H.default_memory_max), "%s", optarg); break;
		case 't': H.ready_timeout_ns = (uint64_t)(atof(optarg) * BL_NSEC); break;
		case 'r': replace = true; break;
		case 'g': snprintf(H.registry, sizeof(H.registry), "%s", optarg); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (!H.worker[0]) {
		char self[PATH_MAX];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n > 0) {
			self[n] = 0;
			char *slash = strrchr(self, '/');
			if (slash) {
				*slash = 0;
				snprintf(H.worker, sizeof(H.worker), "%s/backline-plugin-worker", self);
			}
		}
		if (access(H.worker, X_OK) != 0)
			snprintf(H.worker, sizeof(H.worker), "backline-plugin-worker");
	}
	const char *home = getenv("HOME");
	bool registry_set = false;
	for (int i = 1; i < argc; i++)
		registry_set |= !strncmp(argv[i], "--registry", 10);
	if (!registry_set) {
		const char *cd = getenv("BACKLINE_CACHE_DIR"), *xc = getenv("XDG_CACHE_HOME");
		if (cd && *cd)
			snprintf(H.registry, sizeof(H.registry), "%s/plugins.json", cd);
		else if (xc && *xc)
			snprintf(H.registry, sizeof(H.registry), "%s/backline/plugins.json", xc);
		else
			snprintf(H.registry, sizeof(H.registry), "%s/.cache/backline/plugins.json", home ? home : "/tmp");
	}
	if (!H.state_dir[0]) {
		const char *x = getenv("XDG_STATE_HOME");
		if (x && *x)
			snprintf(H.state_dir, sizeof(H.state_dir), "%s/backline/instances", x);
		else
			snprintf(H.state_dir, sizeof(H.state_dir), "%s/.local/state/backline/instances", home ? home : "/tmp");
	}
	if (!H.log_dir[0]) {
		const char *x = getenv("XDG_RUNTIME_DIR");
		snprintf(H.log_dir, sizeof(H.log_dir), "%s/backline/workers", x && *x ? x : "/tmp");
	}
	if (mkdir_p(H.state_dir) < 0 || mkdir_p(H.log_dir) < 0) {
		logf_("cannot create %s or %s: %s", H.state_dir, H.log_dir, strerror(errno));
		return 1;
	}
	if (H.use_scope && system("systemctl --user is-system-running >/dev/null 2>&1 || "
				  "systemctl --user show-environment >/dev/null 2>&1") != 0) {
		logf_("no systemd user manager; running workers as plain child processes");
		H.use_scope = false;
	}

	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, SIGCHLD);
	sigaddset(&ss, SIGTERM);
	sigaddset(&ss, SIGINT);
	sigprocmask(SIG_BLOCK, &ss, NULL);
	signal(SIGPIPE, SIG_IGN);

	int r = sd_event_default(&H.event);
	if (r < 0) {
		logf_("sd_event: %s", strerror(-r));
		return 1;
	}
	sd_event_add_signal(H.event, NULL, SIGTERM, on_term, NULL);
	sd_event_add_signal(H.event, NULL, SIGINT, on_term, NULL);

	if ((r = sd_bus_open_user(&H.bus)) < 0) {
		logf_("session bus: %s", strerror(-r));
		return 1;
	}
	sd_bus_add_object_vtable(H.bus, NULL, OBJ_PATH, IFACE, vtable, NULL);
	r = sd_bus_request_name(H.bus, BUS_NAME, replace ? SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT
							 : SD_BUS_NAME_ALLOW_REPLACEMENT);
	if (r < 0) {
		logf_("cannot own %s: %s (is another backline-hostd running?)", BUS_NAME, strerror(-r));
		return 1;
	}
	sd_bus_attach_event(H.bus, H.event, 0);
	printf("{\"bus\": \"%s\", \"pid\": %d, \"worker\": \"%s\", \"scopes\": %s, \"state_dir\": \"%s\", \"registry\": \"%s\"}\n",
	       BUS_NAME, (int)getpid(), H.worker, H.use_scope ? "true" : "false", H.state_dir,
	       access(H.registry, R_OK) == 0 ? H.registry : "");
	fflush(stdout);
	logf_("ready on %s (workers %s)", BUS_NAME, H.use_scope ? "in systemd user scopes" : "as child processes");

	r = sd_event_loop(H.event);

	while (H.instances)
		remove_instance(H.instances);
	sd_bus_release_name(H.bus, BUS_NAME);
	sd_bus_flush_close_unref(H.bus);
	sd_event_unref(H.event);
	return r < 0 ? 1 : 0;
}
