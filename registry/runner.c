/* SPDX-License-Identifier: MIT
 *
 * Each probe runs as:
 *   fork -> setpgid -> chdir(fresh scratch dir) -> stdin=/dev/null
 *        -> exec systemd-run --user --scope --collect --unit=backline-probe-<pid>-<n>
 *                -p MemoryMax=.. -p TasksMax=.. -- backline-probe MODE ARGS
 * systemd-run puts the process into a new cgroup scope and then execs the probe
 * in the same pid, so waitpid() still sees the probe's own exit or signal.
 * When the probe exits (or times out), whatever it left in its cgroup is
 * counted and killed: a plugin's helper process can't outlive its scan.
 * Without a user systemd (CI containers) it falls back to a plain process
 * group; this process is a child subreaper then, so escaped children are
 * still found and killed.
 */
#define _GNU_SOURCE
#include "runner.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

struct run {
	struct job *job;
	int index;
	pid_t pid;
	int out_fd, err_fd;
	char *out;
	size_t out_len, out_cap;
	char err[2048];
	size_t err_len;
	double start, deadline;
	bool exited, timed_out;
	int killed_at_timeout; /* processes in the scope when the timeout hit */
	int status;
	char unit[96];       /* systemd-run --unit= */
	char scope_name[112]; /* the unit as systemd names it: <unit>.scope */
	char scratch[512];
};

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static sd_bus *user_bus(void)
{
	static sd_bus *bus;
	static bool tried;
	if (!tried) {
		tried = true;
		if (sd_bus_open_user(&bus) < 0)
			bus = NULL;
	}
	return bus;
}

bool runner_scope_available(void)
{
	static int ok = -1;
	if (ok >= 0)
		return ok;
	ok = 0;
	if (!user_bus())
		return false;
	pid_t p = fork();
	if (p == 0) {
		int n = open("/dev/null", O_RDWR);
		dup2(n, 0); dup2(n, 1); dup2(n, 2);
		char unit[64];
		snprintf(unit, sizeof(unit), "backline-probe-%d-check", (int)getppid());
		execlp("systemd-run", "systemd-run", "--user", "--scope", "--quiet", "--collect", "--unit", unit,
		       "true", (char *)NULL);
		_exit(127);
	}
	int st;
	if (p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0)
		ok = 1;
	return ok;
}

/* Processes still in the probe's scope after it ended. -1: no such unit. */
static int scope_procs(const char *unit)
{
	sd_bus *bus = user_bus();
	if (!bus)
		return -1;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message *r = NULL;
	const char *path = NULL;
	int n = -1;
	if (sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
			"org.freedesktop.systemd1.Manager", "GetUnit", &e, &r, "s", unit) < 0)
		goto out;
	if (sd_bus_message_read(r, "o", &path) < 0)
		goto out;
	char *cg = NULL;
	if (sd_bus_get_property_string(bus, "org.freedesktop.systemd1", path, "org.freedesktop.systemd1.Scope",
			"ControlGroup", &e, &cg) < 0 || !cg || !*cg) {
		free(cg);
		goto out;
	}
	char file[1024];
	snprintf(file, sizeof(file), "/sys/fs/cgroup%s/cgroup.procs", cg);
	free(cg);
	FILE *f = fopen(file, "r");
	if (!f)
		goto out;
	n = 0;
	int pid;
	while (fscanf(f, "%d", &pid) == 1)
		n++;
	fclose(f);
out:
	sd_bus_error_free(&e);
	sd_bus_message_unref(r);
	return n;
}

static void kill_scope(const char *unit)
{
	sd_bus *bus = user_bus();
	if (!bus)
		return;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
			"org.freedesktop.systemd1.Manager", "KillUnit", &e, NULL, "ssi", unit, "all", SIGKILL);
	sd_bus_error_free(&e);
}

static int rm_cb(const char *p, const struct stat *s, int flag, struct FTW *f)
{
	(void)s; (void)flag; (void)f;
	return remove(p);
}

static int count_entries(const char *dir)
{
	DIR *d = opendir(dir);
	int n = 0;
	struct dirent *de;
	if (!d) return 0;
	while ((de = readdir(d)))
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			n++;
	closedir(d);
	return n;
}

static const char *scratch_root(void)
{
	static char root[512];
	if (!root[0]) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		char base[480];
		snprintf(base, sizeof(base), "%s/backline", rt && *rt ? rt : "/tmp");
		mkdir(base, 0700);
		snprintf(root, sizeof(root), "%s/probe", base);
		mkdir(root, 0700);
	}
	return root;
}

static void start(struct run *r, const struct runner_opts *o, bool scope)
{
	int out[2], err[2];
	snprintf(r->scratch, sizeof(r->scratch), "%s/job-XXXXXX", scratch_root());
	if (!mkdtemp(r->scratch))
		snprintf(r->scratch, sizeof(r->scratch), "/tmp");
	static int serial;
	snprintf(r->unit, sizeof(r->unit), "backline-probe-%d-%d", (int)getpid(), ++serial);
	snprintf(r->scope_name, sizeof(r->scope_name), "%s.scope", r->unit);
	if (pipe2(out, O_CLOEXEC) < 0 || pipe2(err, O_CLOEXEC) < 0) {
		r->exited = true;
		r->status = -1;
		return;
	}
	r->start = now_s();
	r->deadline = r->start + o->timeout;
	r->pid = fork();
	if (r->pid == 0) {
		setpgid(0, 0);
		if (chdir(r->scratch) < 0) _exit(126);
		int n = open("/dev/null", O_RDONLY);
		dup2(n, 0);
		dup2(out[1], 1);
		dup2(err[1], 2);
		setenv("DISPLAY", "", 1);
		setenv("WAYLAND_DISPLAY", "", 1);
		if (r->job->env_lv2_path)
			setenv("LV2_PATH", r->job->env_lv2_path, 1);
		struct rlimit rl = { 0, 0 };
		setrlimit(RLIMIT_CORE, &rl);
		const char *argv[32];
		int a = 0;
		char mem[64], tasks[64];
		if (scope) {
			snprintf(mem, sizeof(mem), "MemoryMax=%s", o->memory_max ? o->memory_max : "4G");
			snprintf(tasks, sizeof(tasks), "TasksMax=%d", o->tasks_max > 0 ? o->tasks_max : 256);
			const char *pre[] = { "systemd-run", "--user", "--scope", "--quiet", "--collect", "--unit", r->unit,
					      "-p", mem, "-p", tasks, "-p", "MemorySwapMax=0", "--", NULL };
			for (int i = 0; pre[i]; i++) argv[a++] = pre[i];
		}
		argv[a++] = o->probe;
		for (int i = 0; i < JOB_MAX_ARGS && r->job->argv[i]; i++) argv[a++] = r->job->argv[i];
		argv[a] = NULL;
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(out[1]);
	close(err[1]);
	r->out_fd = out[0];
	r->err_fd = err[0];
	fcntl(r->out_fd, F_SETFL, O_NONBLOCK);
	fcntl(r->err_fd, F_SETFL, O_NONBLOCK);
	if (r->pid < 0) {
		r->exited = true;
		r->status = -1;
	}
}

static void drain(struct run *r)
{
	char buf[65536];
	ssize_t n;
	while (r->out_fd >= 0 && (n = read(r->out_fd, buf, sizeof(buf))) > 0) {
		if (r->out_len + (size_t)n + 1 > r->out_cap) {
			size_t c = r->out_cap ? r->out_cap * 2 : 65536;
			while (c < r->out_len + (size_t)n + 1) c *= 2;
			if (c > (64u << 20)) break; /* 64 MB of report: ignore the rest */
			r->out = realloc(r->out, c);
			r->out_cap = c;
		}
		memcpy(r->out + r->out_len, buf, (size_t)n);
		r->out_len += (size_t)n;
		r->out[r->out_len] = 0;
	}
	while (r->err_fd >= 0 && (n = read(r->err_fd, buf, sizeof(buf))) > 0) {
		/* keep the tail */
		size_t cap = sizeof(r->err) - 1;
		if ((size_t)n >= cap) {
			memcpy(r->err, buf + n - cap, cap);
			r->err_len = cap;
		} else {
			if (r->err_len + (size_t)n > cap) {
				size_t drop = r->err_len + (size_t)n - cap;
				memmove(r->err, r->err + drop, r->err_len - drop);
				r->err_len -= drop;
			}
			memcpy(r->err + r->err_len, buf, (size_t)n);
			r->err_len += (size_t)n;
		}
		r->err[r->err_len] = 0;
	}
}

static const char *last_line(const char *s)
{
	static char buf[600];
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) n--;
	size_t b = n;
	while (b && s[b - 1] != '\n') b--;
	snprintf(buf, sizeof(buf), "%.*s", (int)(n - b > 500 ? 500 : n - b), s + b);
	return buf;
}

static void add_finding(json_object *res, const char *msg)
{
	json_object *f;
	if (!json_object_object_get_ex(res, "findings", &f) || !json_object_is_type(f, json_type_array)) {
		f = json_object_new_array();
		json_object_object_add(res, "findings", f);
	}
	json_object_array_add(f, json_object_new_string(msg));
}

static void finish(struct run *r, bool scope, struct runner_stats *st)
{
	struct job *j = r->job;
	drain(r);
	int stray = 0;
	if (scope) {
		stray = scope_procs(r->scope_name);
		if (stray > 0) {
			st->leftover_units++;
			kill_scope(r->scope_name);
		}
	} else {
		kill(-r->pid, SIGKILL); /* the probe's process group */
	}
	if (r->out_fd >= 0) close(r->out_fd);
	if (r->err_fd >= 0) close(r->err_fd);
	j->ms = (now_s() - r->start) * 1000.0;
	j->cwd_files = count_entries(r->scratch);
	if (strcmp(r->scratch, "/tmp"))
		nftw(r->scratch, rm_cb, 16, FTW_DEPTH | FTW_PHYS);

	json_object *res = NULL;
	j->cacheable = true;
	if (r->timed_out) {
		res = json_object_new_object();
		json_object_object_add(res, "status", json_object_new_string("timeout"));
		json_object_object_add(res, "seconds", json_object_new_double(j->ms / 1000.0));
		if (r->killed_at_timeout > 0)
			json_object_object_add(res, "killed_processes", json_object_new_int(r->killed_at_timeout));
		j->cacheable = false; /* load-dependent: try again next scan */
		st->timed_out++;
	} else if (r->status >= 0 && WIFSIGNALED(r->status)) {
		int sig = WTERMSIG(r->status);
		char name[32];
		snprintf(name, sizeof(name), "SIG%s", sigabbrev_np(sig) ? sigabbrev_np(sig) : "?");
		res = json_object_new_object();
		json_object_object_add(res, "status", json_object_new_string("crashed"));
		json_object_object_add(res, "signal", json_object_new_int(sig));
		json_object_object_add(res, "signal_name", json_object_new_string(name));
		if (r->err_len)
			json_object_object_add(res, "stderr", json_object_new_string(last_line(r->err)));
		st->crashed++;
	} else {
		/* the report: the last line that starts with '{' */
		const char *s = r->out ? r->out : "";
		const char *line = NULL;
		for (const char *p = s; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
			if (*p == '{') line = p;
		if (line)
			res = json_tokener_parse(line);
		if (!res) {
			res = json_object_new_object();
			json_object_object_add(res, "status", json_object_new_string("error"));
			char m[700];
			int code = r->status >= 0 && WIFEXITED(r->status) ? WEXITSTATUS(r->status) : -1;
			snprintf(m, sizeof(m), "no report (exit %d)%s%s", code, r->err_len ? ": " : "",
				 r->err_len ? last_line(r->err) : "");
			json_object_object_add(res, "error", json_object_new_string(m));
			j->cacheable = false;
			st->errors++;
		}
	}
	if (stray > 0) {
		char m[160];
		snprintf(m, sizeof(m), "left %d process%s running in its cgroup (killed)", stray, stray == 1 ? "" : "es");
		json_object_object_add(res, "stray_processes", json_object_new_int(stray));
		add_finding(res, m);
	}
	if (j->cwd_files > 0) {
		char m[160];
		snprintf(m, sizeof(m), "wrote %d file%s into its working directory", j->cwd_files, j->cwd_files == 1 ? "" : "s");
		add_finding(res, m);
	}
	json_object_object_add(res, "wall_ms", json_object_new_double((int64_t)(j->ms * 10) / 10.0));
	j->result = res;
	free(r->out);
	r->out = NULL;
	st->run++;
}

/* Fallback mode: kill children that escaped their process group (setsid)
 * and were re-parented to us as subreaper. */
static void kill_orphans(struct run *running, int n_running)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	pid_t me = getpid();
	if (!d) return;
	while ((de = readdir(d))) {
		pid_t pid = (pid_t)atoi(de->d_name);
		if (pid <= 0) continue;
		char path[64], buf[512];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		char *rp = strrchr(buf, ')');
		int ppid = 0;
		if (!rp || sscanf(rp + 2, "%*c %d", &ppid) != 1 || ppid != me) continue;
		bool ours = false;
		for (int i = 0; i < n_running; i++)
			if (running[i].pid == pid) ours = true;
		if (!ours) kill(pid, SIGKILL);
	}
	closedir(d);
}

void runner_run(struct job *jobs, int n, const struct runner_opts *o, struct runner_stats *st)
{
	bool scope = o->sandbox && runner_scope_available();
	st->sandbox = scope ? "systemd-scope" : "fork";
	prctl(PR_SET_CHILD_SUBREAPER, 1);
	int par = o->jobs > 0 ? o->jobs : 1;
	struct run *running = calloc((size_t)par, sizeof(*running));
	int n_running = 0, next = 0;
	while (next < n || n_running > 0) {
		while (n_running < par && next < n) {
			struct run *r = &running[n_running++];
			memset(r, 0, sizeof(*r));
			r->job = &jobs[next];
			r->index = next++;
			r->out_fd = r->err_fd = -1;
			start(r, o, scope);
		}
		struct pollfd pfd[2 * 256];
		int np = 0;
		for (int i = 0; i < n_running && np < 510; i++) {
			if (running[i].out_fd >= 0) pfd[np++] = (struct pollfd){ running[i].out_fd, POLLIN, 0 };
			if (running[i].err_fd >= 0) pfd[np++] = (struct pollfd){ running[i].err_fd, POLLIN, 0 };
		}
		poll(pfd, (nfds_t)np, 20);
		/* reap: our probes, and (as subreaper) anything orphaned */
		int status;
		pid_t pid;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
			for (int i = 0; i < n_running; i++)
				if (running[i].pid == pid) {
					running[i].exited = true;
					running[i].status = status;
				}
		double t = now_s();
		for (int i = 0; i < n_running; i++) {
			struct run *r = &running[i];
			drain(r);
			if (!r->exited && !r->timed_out && t > r->deadline) {
				r->timed_out = true;
				if (scope) {
					r->killed_at_timeout = scope_procs(r->scope_name);
					kill_scope(r->scope_name);
				}
				kill(-r->pid, SIGKILL);
				kill(r->pid, SIGKILL);
			}
		}
		for (int i = 0; i < n_running;) {
			if (running[i].exited) {
				finish(&running[i], scope, st);
				if (!scope) kill_orphans(running, n_running);
				running[i] = running[--n_running];
			} else {
				i++;
			}
		}
	}
	if (!scope) kill_orphans(running, 0);
	while (waitpid(-1, NULL, WNOHANG) > 0) {}
	free(running);
}
