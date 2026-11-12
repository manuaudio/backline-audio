/* backline-recorderd: the recorder as a D-Bus service (docs/api/Recorder1.md).
 * SPDX-License-Identifier: MIT
 *
 * One process on the PipeWire main loop, with sd-bus attached to the same
 * loop. Recording is rec_engine.h, soundcheck playback is soundcheck.h; this
 * file is the API, the take list and the link moves of virtual soundcheck.
 *
 * Virtual soundcheck. The take's inputs are the nodes it was recorded from.
 * Switching input X to playback moves every link from X's source ports
 * (monitor ports for a sink) to a node that isn't Backline's own onto the
 * matching soundcheck port, so the consumer hears the take where it heard X.
 * While X is in playback, links that reappear from X to a consumer (a
 * session manager relinking a stream) are moved again. Moved links are
 * written to $XDG_RUNTIME_DIR/backline/soundcheck.json first, so they are put
 * back on live, on StopSoundcheck, on exit, and at the next start after a
 * crash.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <pipewire/pipewire.h>
#include <spa/utils/string.h>
#include <systemd/sd-bus.h>

#include "bl_graph.h"
#include "rec_engine.h"
#include "soundcheck.h"

#define BUS_NAME "io.github.manuaudio.Backline.Recorder"
#define OBJ_PATH "/io/github/manuaudio/Backline/Recorder"
#define IFACE "io.github.manuaudio.Backline.Recorder1"
#define ERR_BUSY "io.github.manuaudio.Backline.Error.Busy"
#define ERR_NOTFOUND "io.github.manuaudio.Backline.Error.NotFound"
#define ERR_STATE "io.github.manuaudio.Backline.Error.State"
#define ERR_TIMEOUT "io.github.manuaudio.Backline.Error.Timeout"
#define MAX_MOVES 512

struct move {
	char input[256];        /* the take's input node */
	char out_port[128];     /* its port name */
	char in_node[256];      /* consumer node and port */
	char in_port[128];
	int track, channel;     /* soundcheck port t<track>_<channel> (1-based) */
	bool active;
};

static struct {
	struct pw_main_loop *loop;
	struct pw_loop *l;
	struct pw_context *context;
	struct pw_core *core;
	struct bl_graph *graph;
	sd_bus *bus;
	struct spa_source *bus_io, *level_timer, *idle_timer, *arm_timer, *reconcile_ev;

	struct rec_session *rec;
	char sources_str[REC_MAX_TRACKS][300];
	int n_sources;
	char dir[512];
	char take[128];
	char last_take[1100];
	sd_bus_message *pending_arm, *pending_stop;
	uint64_t last_overruns;
	bool quitting;

	struct sc_player *sc;
	struct sc_take sc_take;
	char sc_mode[SC_MAX_TRACKS][16];   /* per track: live / playback */
	struct move moves[MAX_MOVES];
	int n_moves;
	char moves_file[600];

	int idle_exit;   /* seconds, 0 = never */
} D;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * SPA_NSEC_PER_SEC + ts.tv_nsec;
}

static const char *state_name(void)
{
	if (!D.rec)
		return "idle";
	switch (rec_session_state(D.rec)) {
	case REC_ARMING: return "idle";      /* not armed until the links are up */
	case REC_ARMED: return "armed";
	case REC_RECORDING:
	case REC_FINISHING: return "recording";
	default: return "idle";
	}
}

static void props_changed(const char *first, ...)
{
	char *names[16];
	int n = 0;
	va_list ap;
	va_start(ap, first);
	for (const char *s = first; s && n < 15; s = va_arg(ap, const char *))
		names[n++] = (char *)s;
	va_end(ap);
	names[n] = NULL;
	sd_bus_emit_properties_changed_strv(D.bus, OBJ_PATH, IFACE, names);
}

static void default_dir(char *out, size_t n)
{
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(out, n, "%s/backline/takes", x);
	else
		snprintf(out, n, "%s/.local/share/backline/takes", h ? h : "/tmp");
}

static int mkdir_p(const char *path)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) < 0 && errno != EEXIST ? -errno : 0;
}

/* --------------------------------------------------------------- idle exit */
static bool busy(void)
{
	return D.rec || D.sc || D.n_moves;
}

static void arm_idle(void)
{
	if (!D.idle_exit || !D.idle_timer)
		return;
	struct timespec t = { busy() ? 0 : D.idle_exit, 0 }, z = { 0, 0 };
	pw_loop_update_timer(D.l, D.idle_timer, busy() ? &z : &t, &z, false);
}

static void on_idle(void *data, uint64_t exp)
{
	if (!busy())
		pw_main_loop_quit(D.loop);
}

/* --------------------------------------------------------------- recording */
static void reply_arm_error(const char *name, const char *msg)
{
	if (!D.pending_arm)
		return;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_error_set(&e, name, msg);
	sd_bus_reply_method_error(D.pending_arm, &e);
	sd_bus_error_free(&e);
	sd_bus_message_unref(D.pending_arm);
	D.pending_arm = NULL;
}

static void ev_armed(void *data)
{
	if (D.arm_timer) {
		struct timespec z = { 0, 0 };
		pw_loop_update_timer(D.l, D.arm_timer, &z, &z, false);
	}
	if (D.pending_arm) {
		sd_bus_reply_method_return(D.pending_arm, "s", D.take);
		sd_bus_message_unref(D.pending_arm);
		D.pending_arm = NULL;
	}
	props_changed("State", "Take", "Sources", "Rate", "CaptureLatency", "Dir", NULL);
}

static void ev_started(void *data)
{
	props_changed("State", NULL);
}

static void ev_finished(void *data, const char *sidecar_in)
{
	char sc_copy[1100];
	const char *sidecar = NULL;
	if (sidecar_in) {
		snprintf(sc_copy, sizeof(sc_copy), "%s", sidecar_in); /* it lives in the session */
		sidecar = sc_copy;
	}
	uint64_t frames = rec_session_frames(D.rec), over = rec_session_overruns(D.rec);
	char take[128];
	snprintf(take, sizeof(take), "%s", D.take);
	if (sidecar)
		snprintf(D.last_take, sizeof(D.last_take), "%s", sidecar);
	if (D.pending_stop) {
		sd_bus_reply_method_return(D.pending_stop, "s", sidecar ? sidecar : "");
		sd_bus_message_unref(D.pending_stop);
		D.pending_stop = NULL;
	}
	rec_session_destroy(D.rec);
	D.rec = NULL;
	D.take[0] = 0;
	D.n_sources = 0;
	D.last_overruns = 0;
	if (sidecar)
		sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "TakeFinished", "sstt", take, sidecar, frames, over);
	props_changed("State", "Take", "Sources", "LastTake", NULL);
	arm_idle();
	if (D.quitting && !busy())
		pw_main_loop_quit(D.loop);
}

static const struct rec_events rec_events = { ev_armed, ev_started, ev_finished };

static void on_arm_timeout(void *data, uint64_t exp)
{
	if (D.rec && rec_session_state(D.rec) == REC_ARMING) {
		reply_arm_error(ERR_TIMEOUT, "the recorder's links did not come up in 5 s");
		rec_session_destroy(D.rec);
		D.rec = NULL;
		D.take[0] = 0;
		D.n_sources = 0;
		arm_idle();
	}
}

static void on_level(void *data, uint64_t exp)
{
	if (!D.rec || rec_session_state(D.rec) == REC_ARMING || rec_session_state(D.rec) == REC_DONE)
		return;
	int n = rec_session_n_tracks(D.rec);
	double peaks[REC_MAX_TRACKS];
	rec_session_take_peaks(D.rec, peaks, n);
	sd_bus_message *m = NULL;
	if (sd_bus_message_new_signal(D.bus, &m, OBJ_PATH, IFACE, "Level") >= 0) {
		sd_bus_message_append(m, "t", rec_session_frames(D.rec));
		sd_bus_message_append_array(m, 'd', peaks, n * sizeof(double));
		sd_bus_send(D.bus, m, NULL);
		sd_bus_message_unref(m);
	}
	uint64_t o = rec_session_overruns(D.rec);
	if (o != D.last_overruns) {
		D.last_overruns = o;
		sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "Overrun", "t", o);
		props_changed("Overruns", NULL);
	}
}

static int parse_source(const char *spec, struct rec_source *s)
{
	memset(s, 0, sizeof(*s));
	s->channels = 2;
	s->latency_frames = -1;
	char buf[300];
	snprintf(buf, sizeof(buf), "%s", spec);
	char *colon = strrchr(buf, ':');
	if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
		s->channels = atoi(colon + 1);
		*colon = 0;
	}
	if (!buf[0] || s->channels < 1 || s->channels > REC_MAX_CH)
		return -EINVAL;
	snprintf(s->node, sizeof(s->node), "%s", buf);
	return 0;
}

static bool valid_take_name(const char *n)
{
	if (!*n || strlen(n) >= 120 || n[0] == '.')
		return false;
	for (const char *p = n; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_'))
			return false;
	return true;
}

static int m_arm(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	if (D.rec)
		return sd_bus_error_setf(err, ERR_BUSY, "already %s (take %s)", state_name(), D.take);
	struct rec_source src[REC_MAX_TRACKS];
	int n = 0, r;
	char *spec;
	if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0)
		return r;
	while ((r = sd_bus_message_read(m, "s", &spec)) > 0) {
		if (n == REC_MAX_TRACKS)
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "at most %d sources", REC_MAX_TRACKS);
		if (parse_source(spec, &src[n]) < 0)
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "bad source \"%s\" (NODE[:CHANNELS], 1-16 channels)", spec);
		snprintf(D.sources_str[n], sizeof(D.sources_str[n]), "%s", spec);
		n++;
	}
	if (r < 0)
		return r;
	sd_bus_message_exit_container(m);
	if (n == 0)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "no sources");

	struct rec_options o;
	rec_options_defaults(&o);
	o.wait_record = true;
	default_dir(o.dir, sizeof(o.dir));
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k, *s;
		double d;
		sd_bus_message_read(m, "s", &k);
		if (!strcmp(k, "dir") || !strcmp(k, "name") || !strcmp(k, "format") || !strcmp(k, "align") || !strcmp(k, "latency")) {
			if (sd_bus_message_read(m, "v", "s", &s) < 0)
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s must be a string", k);
			if (!strcmp(k, "dir")) snprintf(o.dir, sizeof(o.dir), "%s", s);
			else if (!strcmp(k, "name")) {
				if (!valid_take_name(s))
					return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "take name \"%s\": use [A-Za-z0-9._-]", s);
				snprintf(o.take, sizeof(o.take), "%s", s);
			} else if (!strcmp(k, "format")) snprintf(o.format, sizeof(o.format), "%s", s);
			else if (!strcmp(k, "align")) {
				if (spa_streq(s, "signal")) o.align_signal = true;
				else if (spa_streq(s, "graph")) o.align_signal = false;
				else return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "align is signal or graph");
			} else {
				if (spa_streq(s, "off")) o.latency = REC_LATENCY_OFF;
				else if (spa_streq(s, "hardware")) o.latency = REC_LATENCY_HARDWARE;
				else if (spa_streq(s, "reported")) o.latency = REC_LATENCY_REPORTED;
				else return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "latency is off, hardware or reported");
			}
		} else if (!strcmp(k, "pre-roll") || !strcmp(k, "duration") || !strcmp(k, "ring-seconds")) {
			if (sd_bus_message_read(m, "v", "d", &d) < 0)
				return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s must be a double", k);
			if (!strcmp(k, "pre-roll")) o.pre_roll = d;
			else if (!strcmp(k, "duration")) o.duration = d;
			else o.ring_seconds = d;
		} else if (!strcmp(k, "latency-frames")) {
			if ((r = sd_bus_message_enter_container(m, 'v', "a{su}")) < 0 ||
			    (r = sd_bus_message_enter_container(m, 'a', "{su}")) < 0)
				return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "latency-frames must be a{su}");
			const char *node;
			uint32_t f;
			while ((r = sd_bus_message_read(m, "{su}", &node, &f)) > 0) {
				bool found = false;
				for (int i = 0; i < n; i++)
					if (spa_streq(src[i].node, node)) {
						src[i].latency_frames = f;
						found = true;
					}
				if (!found)
					return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "latency-frames: %s is not a source", node);
			}
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else {
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown option \"%s\"", k);
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	if (mkdir_p(o.dir) < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED, "cannot create %s: %s", o.dir, strerror(errno));
	for (int i = 0; i < n; i++)
		if (!bl_graph_find_node(D.graph, src[i].node))
			return sd_bus_error_setf(err, ERR_NOTFOUND, "no node named \"%s\"", src[i].node);
	char take_json[1200];
	snprintf(take_json, sizeof(take_json), "%s/%s.json", o.dir, o.take);
	if (access(take_json, F_OK) == 0)
		return sd_bus_error_setf(err, ERR_BUSY, "take \"%s\" already exists in %s", o.take, o.dir);

	char e[256];
	D.rec = rec_session_new(D.core, D.l, src, n, &o, &rec_events, NULL, e, sizeof(e));
	if (!D.rec)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, e);
	D.n_sources = n;
	snprintf(D.dir, sizeof(D.dir), "%s", o.dir);
	snprintf(D.take, sizeof(D.take), "%s", o.take);
	D.pending_arm = sd_bus_message_ref(m);
	struct timespec t = { 5, 0 }, z = { 0, 0 };
	pw_loop_update_timer(D.l, D.arm_timer, &t, &z, false);
	arm_idle();
	return 1;
}

static int m_record(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	if (!D.rec || rec_session_state(D.rec) != REC_ARMED)
		return sd_bus_error_setf(err, ERR_STATE, "not armed (state %s)", state_name());
	rec_session_record(D.rec);
	return sd_bus_reply_method_return(m, "s", D.take);
}

static int m_stop(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	if (!D.rec)
		return sd_bus_error_set(err, ERR_STATE, "nothing armed or recording");
	if (D.pending_stop)
		return sd_bus_error_set(err, ERR_BUSY, "already stopping");
	if (rec_session_state(D.rec) == REC_ARMING) {
		reply_arm_error(ERR_STATE, "stopped before the links came up");
		rec_session_destroy(D.rec);
		D.rec = NULL;
		D.take[0] = 0;
		D.n_sources = 0;
		arm_idle();
		props_changed("State", "Take", NULL);
		return sd_bus_reply_method_return(m, "s", "");
	}
	D.pending_stop = sd_bus_message_ref(m);
	rec_session_stop(D.rec);
	return 1;
}

static int m_marker(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *name;
	int r = sd_bus_message_read(m, "s", &name);
	if (r < 0)
		return r;
	uint64_t frame;
	if (!D.rec || rec_session_marker(D.rec, name, &frame) < 0)
		return sd_bus_error_set(err, ERR_STATE, "not recording");
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "Marker", "sst", D.take, *name ? name : "marker", frame);
	return sd_bus_reply_method_return(m, "t", frame);
}

/* ------------------------------------------------------------------- takes */
static const char *takes_dir(void)
{
	static char d[512];
	if (D.dir[0])
		return D.dir;
	default_dir(d, sizeof(d));
	return d;
}

static int resolve_take(const char *take, char *path, size_t n)
{
	if (strchr(take, '/'))
		snprintf(path, n, "%s", take);
	else if (valid_take_name(take))
		snprintf(path, n, "%s/%s.json", takes_dir(), take);
	else
		return -EINVAL;
	return access(path, R_OK) == 0 ? 0 : -ENOENT;
}

static json_object *take_summary(const char *path)
{
	json_object *j = json_object_from_file(path), *v, *s;
	if (!j)
		return NULL;
	const char *schema = json_object_object_get_ex(j, "schema", &v) ? json_object_get_string(v) : "";
	if (strncmp(schema, "backline.take/", 14)) {
		json_object_put(j);
		return NULL;
	}
	s = json_object_new_object();
	if (json_object_object_get_ex(j, "take", &v)) json_object_object_add(s, "take", json_object_get(v));
	json_object_object_add(s, "sidecar", json_object_new_string(path));
	json_object *start;
	if (json_object_object_get_ex(j, "start", &start) && json_object_object_get_ex(start, "utc", &v))
		json_object_object_add(s, "utc", json_object_get(v));
	int64_t rate = json_object_object_get_ex(j, "rate", &v) ? json_object_get_int64(v) : 0;
	int64_t frames = json_object_object_get_ex(j, "frames", &v) ? json_object_get_int64(v) : 0;
	json_object_object_add(s, "rate", json_object_new_int64(rate));
	json_object_object_add(s, "frames", json_object_new_int64(frames));
	json_object_object_add(s, "seconds", json_object_new_double(rate ? (double)frames / rate : 0));
	json_object *tracks = json_object_new_array(), *tr;
	if (json_object_object_get_ex(j, "tracks", &tr))
		for (size_t i = 0; i < json_object_array_length(tr); i++) {
			json_object *t = json_object_array_get_idx(tr, i), *o = json_object_new_object();
			const char *keys[] = { "node", "channels", "file" };
			for (int k = 0; k < 3; k++)
				if (json_object_object_get_ex(t, keys[k], &v))
					json_object_object_add(o, keys[k], json_object_get(v));
			json_object_array_add(tracks, o);
		}
	json_object_object_add(s, "tracks", tracks);
	json_object *mk;
	json_object_object_add(s, "markers", json_object_new_int(
		json_object_object_get_ex(j, "markers", &mk) ? (int)json_object_array_length(mk) : 0));
	json_object_put(j);
	return s;
}

static int cmp_utc_desc(const void *a, const void *b)
{
	json_object *x = *(json_object *const *)a, *y = *(json_object *const *)b, *u;
	const char *ux = json_object_object_get_ex(x, "utc", &u) ? json_object_get_string(u) : "";
	const char *uy = json_object_object_get_ex(y, "utc", &u) ? json_object_get_string(u) : "";
	return strcmp(uy, ux);
}

static int m_takes(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *dir = takes_dir();
	json_object *root = json_object_new_object(), *arr = json_object_new_array();
	json_object_object_add(root, "schema", json_object_new_string("backline.takes/1"));
	json_object_object_add(root, "dir", json_object_new_string(dir));
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			size_t l = strlen(e->d_name);
			if (l < 6 || strcmp(e->d_name + l - 5, ".json"))
				continue;
			char p[1200];
			snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
			json_object *s = take_summary(p);
			if (s)
				json_object_array_add(arr, s);
		}
		closedir(d);
	}
	json_object_array_sort(arr, cmp_utc_desc);
	json_object_object_add(root, "takes", arr);
	int r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
	json_object_put(root);
	return r;
}

static int m_get_take(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *take;
	int r = sd_bus_message_read(m, "s", &take);
	if (r < 0)
		return r;
	char path[1200];
	if (resolve_take(take, path, sizeof(path)) < 0)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no take \"%s\" in %s", take, takes_dir());
	json_object *j = json_object_from_file(path);
	if (!j)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "%s is not readable JSON", path);
	r = sd_bus_reply_method_return(m, "s", json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN));
	json_object_put(j);
	return r;
}

/* -------------------------------------------------------------- soundcheck */
static bool is_ours(uint32_t node_id)
{
	const struct bl_node *n = bl_graph_node(D.graph, node_id);
	return n && !strncmp(n->name, "backline.", 9);
}

static int track_of_input(const char *node)
{
	for (int k = 0; k < D.sc_take.n_tracks; k++)
		if (spa_streq(D.sc_take.tracks[k].node, node))
			return k;
	return -1;
}

/* The input's source ports in graph order: the order the recorder used. */
struct srcports { uint32_t node; bool monitor; int n; uint32_t ids[64]; uint32_t idx[64]; char names[64][128]; };
static int collect_src(void *data, const struct bl_port *p)
{
	struct srcports *s = data;
	if (p->node_id == s->node && p->output && !p->midi && p->monitor == s->monitor && s->n < 64) {
		int k = s->n++;   /* in port order, as the recorder takes them */
		while (k > 0 && s->idx[k - 1] > p->index) {
			s->ids[k] = s->ids[k - 1];
			s->idx[k] = s->idx[k - 1];
			memcpy(s->names[k], s->names[k - 1], 128);
			k--;
		}
		s->ids[k] = p->id;
		s->idx[k] = p->index;
		snprintf(s->names[k], 128, "%s", p->name);
	}
	return 0;
}

static bool input_ports(const char *node, struct srcports *sp)
{
	const struct bl_node *n = bl_graph_find_node(D.graph, node);
	if (!n)
		return false;
	memset(sp, 0, sizeof(*sp));
	sp->node = n->id;
	sp->monitor = strstr(n->media_class, "Sink") != NULL;
	bl_graph_for_each_port(D.graph, collect_src, sp);
	if (sp->n == 0 && !sp->monitor) { /* a virtual source: monitor outputs only */
		sp->monitor = true;
		bl_graph_for_each_port(D.graph, collect_src, sp);
	}
	return true;
}

struct find_port_ctx { uint32_t node; const char *name; bool output; uint32_t id; };
static int find_port_cb(void *data, const struct bl_port *p)
{
	struct find_port_ctx *c = data;
	if (p->node_id == c->node && p->output == c->output && spa_streq(p->name, c->name)) {
		c->id = p->id;
		return 1;
	}
	return 0;
}

static uint32_t port_by_name(const char *node, const char *port, bool output)
{
	const struct bl_node *n = bl_graph_find_node(D.graph, node);
	if (!n)
		return SPA_ID_INVALID;
	struct find_port_ctx c = { n->id, port, output, SPA_ID_INVALID };
	bl_graph_for_each_port(D.graph, find_port_cb, &c);
	return c.id;
}

static uint32_t sc_port(int track, int channel)
{
	char name[32];
	snprintf(name, sizeof(name), "t%d_%d", track, channel);
	return port_by_name("backline.soundcheck", name, true);
}

static void save_moves(void)
{
	if (!D.moves_file[0])
		return;
	json_object *arr = json_object_new_array();
	for (int i = 0; i < D.n_moves; i++) {
		struct move *mv = &D.moves[i];
		json_object *o = json_object_new_object();
		json_object_object_add(o, "input", json_object_new_string(mv->input));
		json_object_object_add(o, "out_port", json_object_new_string(mv->out_port));
		json_object_object_add(o, "in_node", json_object_new_string(mv->in_node));
		json_object_object_add(o, "in_port", json_object_new_string(mv->in_port));
		json_object_array_add(arr, o);
	}
	json_object *root = json_object_new_object();
	json_object_object_add(root, "schema", json_object_new_string("backline.soundcheck-moves/1"));
	json_object_object_add(root, "moves", arr);
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s.tmp", D.moves_file);
	if (D.n_moves == 0) {
		unlink(D.moves_file);
	} else if (json_object_to_file_ext(tmp, root, JSON_C_TO_STRING_PRETTY) == 0) {
		rename(tmp, D.moves_file);
	}
	json_object_put(root);
}

struct move_ctx { struct srcports *sp; int track; const char *input; int moved; };

/* Move every foreign consumer link of one input onto the soundcheck ports. */
static int move_link_cb(void *data, const struct bl_link *l)
{
	struct move_ctx *c = data;
	int ch = -1;
	for (int i = 0; i < c->sp->n; i++)
		if (c->sp->ids[i] == l->out_port)
			ch = i;
	if (ch < 0 || is_ours(l->in_node))
		return 0;
	const struct bl_port *in = bl_graph_port(D.graph, l->in_port);
	const struct bl_node *in_node = bl_graph_node(D.graph, l->in_node);
	if (!in || !in_node)
		return 0;
	int channels = D.sc_take.tracks[c->track].channels;
	int sc_ch = SPA_MIN(ch, channels - 1) + 1;
	uint32_t scp = sc_port(c->track + 1, sc_ch);
	if (scp == SPA_ID_INVALID)
		return 0;
	/* remember it before touching anything */
	struct move *mv = NULL;
	for (int i = 0; i < D.n_moves; i++)
		if (spa_streq(D.moves[i].input, c->input) && spa_streq(D.moves[i].out_port, c->sp->names[ch]) &&
		    spa_streq(D.moves[i].in_node, in_node->name) && spa_streq(D.moves[i].in_port, in->name))
			mv = &D.moves[i];
	if (!mv) {
		if (D.n_moves == MAX_MOVES)
			return 0;
		mv = &D.moves[D.n_moves++];
		snprintf(mv->input, sizeof(mv->input), "%s", c->input);
		snprintf(mv->out_port, sizeof(mv->out_port), "%s", c->sp->names[ch]);
		snprintf(mv->in_node, sizeof(mv->in_node), "%s", in_node->name);
		snprintf(mv->in_port, sizeof(mv->in_port), "%s", in->name);
		mv->track = c->track + 1;
		mv->channel = sc_ch;
		save_moves();
	}
	mv->active = true;
	bl_graph_link(D.graph, scp, l->in_port);
	bl_graph_destroy_global(D.graph, l->id);
	c->moved++;
	return 0;
}

static int enforce_input(int track)
{
	struct srcports sp;
	const char *input = D.sc_take.tracks[track].node;
	if (!input_ports(input, &sp))
		return 0;
	struct move_ctx c = { &sp, track, input, 0 };
	bl_graph_for_each_link(D.graph, move_link_cb, &c);
	/* our soundcheck links for moves whose consumer is (back) in the graph */
	for (int i = 0; i < D.n_moves; i++) {
		struct move *mv = &D.moves[i];
		if (!spa_streq(mv->input, input))
			continue;
		uint32_t in = port_by_name(mv->in_node, mv->in_port, false), scp = sc_port(mv->track, mv->channel);
		if (in != SPA_ID_INVALID && scp != SPA_ID_INVALID)
			bl_graph_link(D.graph, scp, in);
	}
	return c.moved;
}

/* Put an input's moved links back (persistent: they were someone else's). */
static void restore_input(const char *input)
{
	for (int i = 0; i < D.n_moves;) {
		struct move *mv = &D.moves[i];
		if (input && !spa_streq(mv->input, input)) {
			i++;
			continue;
		}
		uint32_t out = port_by_name(mv->input, mv->out_port, true);
		uint32_t in = port_by_name(mv->in_node, mv->in_port, false);
		uint32_t scp = mv->track ? sc_port(mv->track, mv->channel) : SPA_ID_INVALID;
		if (scp != SPA_ID_INVALID && in != SPA_ID_INVALID)
			bl_graph_unlink(D.graph, scp, in);
		if (out != SPA_ID_INVALID && in != SPA_ID_INVALID && !bl_graph_is_linked(D.graph, out, in))
			bl_graph_link_persistent(D.graph, out, in);
		D.moves[i] = D.moves[--D.n_moves];
	}
	save_moves();
}

static void reconcile(void)
{
	if (!D.sc)
		return;
	for (int k = 0; k < D.sc_take.n_tracks; k++)
		if (spa_streq(D.sc_mode[k], "playback"))
			enforce_input(k);
}

static void on_reconcile(void *data, uint64_t count) { reconcile(); }

static void on_link_added(void *data, const struct bl_link *l)
{
	if (D.sc)
		pw_loop_signal_event(D.l, D.reconcile_ev);
}

static void on_port_added(void *data, const struct bl_port *p)
{
	if (D.sc || D.n_moves)
		pw_loop_signal_event(D.l, D.reconcile_ev);
}

static const struct bl_graph_events graph_events = {
	.port_added = on_port_added,
	.link_added = on_link_added,
};

static void stop_soundcheck(void)
{
	if (!D.sc && !D.n_moves)
		return;
	restore_input(NULL);
	if (D.sc) {
		sc_player_destroy(D.sc);
		D.sc = NULL;
	}
	memset(&D.sc_take, 0, sizeof(D.sc_take));
	memset(D.sc_mode, 0, sizeof(D.sc_mode));
	props_changed("Soundcheck", "SoundcheckInputs", NULL);
	arm_idle();
}

static int m_soundcheck(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *take;
	int r = sd_bus_message_read(m, "s", &take);
	if (r < 0)
		return r;
	int loop_play = 1;
	uint64_t start = 0;
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k;
		sd_bus_message_read(m, "s", &k);
		if (!strcmp(k, "loop")) {
			if (sd_bus_message_read(m, "v", "b", &loop_play) < 0)
				return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "loop must be a boolean");
		} else if (!strcmp(k, "start")) {
			if (sd_bus_message_read(m, "v", "t", &start) < 0)
				return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "start must be uint64 frames");
		} else {
			return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "unknown option \"%s\"", k);
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	char path[1200], e[256];
	if (resolve_take(take, path, sizeof(path)) < 0)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "no take \"%s\" in %s", take, takes_dir());
	struct sc_take t;
	if (sc_take_load(path, &t, e, sizeof(e)) < 0)
		return sd_bus_error_set(err, ERR_NOTFOUND, e);
	stop_soundcheck();
	D.sc = sc_player_new(D.core, D.l, &t, loop_play, start, e, sizeof(e));
	if (!D.sc)
		return sd_bus_error_set(err, SD_BUS_ERROR_FAILED, e);
	D.sc_take = t;
	for (int k = 0; k < t.n_tracks; k++)
		snprintf(D.sc_mode[k], sizeof(D.sc_mode[k]), "live");
	arm_idle();
	props_changed("Soundcheck", "SoundcheckInputs", NULL);
	return sd_bus_reply_method_return(m, "");
}

static int m_set_input(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *node, *mode;
	int r = sd_bus_message_read(m, "ss", &node, &mode);
	if (r < 0)
		return r;
	if (!D.sc)
		return sd_bus_error_set(err, ERR_STATE, "no soundcheck loaded");
	int k = track_of_input(node);
	if (k < 0)
		return sd_bus_error_setf(err, ERR_NOTFOUND, "\"%s\" is not an input of take %s", node, D.sc_take.take);
	if (!spa_streq(mode, "live") && !spa_streq(mode, "playback"))
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "mode is live or playback");
	/* several tracks may come from the same input node: switch them all */
	for (int i = 0; i < D.sc_take.n_tracks; i++) {
		if (!spa_streq(D.sc_take.tracks[i].node, node))
			continue;
		snprintf(D.sc_mode[i], sizeof(D.sc_mode[i]), "%s", mode);
		if (spa_streq(mode, "playback"))
			enforce_input(i);
	}
	if (spa_streq(mode, "live"))
		restore_input(node);
	sd_bus_emit_signal(D.bus, OBJ_PATH, IFACE, "SoundcheckInput", "ss", node, mode);
	props_changed("SoundcheckInputs", NULL);
	arm_idle();
	return sd_bus_reply_method_return(m, "");
}

static int m_stop_soundcheck(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	stop_soundcheck();
	return sd_bus_reply_method_return(m, "");
}

/* A previous run crashed with links moved: put them back. */
static void restore_from_file(void)
{
	json_object *j = json_object_from_file(D.moves_file), *arr;
	if (!j)
		return;
	if (json_object_object_get_ex(j, "moves", &arr))
		for (size_t i = 0; i < json_object_array_length(arr) && D.n_moves < MAX_MOVES; i++) {
			json_object *o = json_object_array_get_idx(arr, i), *v;
			struct move *mv = &D.moves[D.n_moves++];
			memset(mv, 0, sizeof(*mv));
			if (json_object_object_get_ex(o, "input", &v)) snprintf(mv->input, sizeof(mv->input), "%s", json_object_get_string(v));
			if (json_object_object_get_ex(o, "out_port", &v)) snprintf(mv->out_port, sizeof(mv->out_port), "%s", json_object_get_string(v));
			if (json_object_object_get_ex(o, "in_node", &v)) snprintf(mv->in_node, sizeof(mv->in_node), "%s", json_object_get_string(v));
			if (json_object_object_get_ex(o, "in_port", &v)) snprintf(mv->in_port, sizeof(mv->in_port), "%s", json_object_get_string(v));
		}
	json_object_put(j);
	fprintf(stderr, "backline-recorderd: putting back %d soundcheck link(s) left by a previous run\n", D.n_moves);
	restore_input(NULL);
}

/* -------------------------------------------------------------- properties */
static int p_str(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err)
{
	if (!strcmp(prop, "State")) return sd_bus_message_append(reply, "s", state_name());
	if (!strcmp(prop, "Take")) return sd_bus_message_append(reply, "s", D.rec ? D.take : "");
	if (!strcmp(prop, "Dir")) return sd_bus_message_append(reply, "s", takes_dir());
	if (!strcmp(prop, "LastTake")) return sd_bus_message_append(reply, "s", D.last_take);
	if (!strcmp(prop, "Soundcheck")) return sd_bus_message_append(reply, "s", D.sc ? D.sc_take.take : "");
	return -EINVAL;
}

static int p_num(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err)
{
	if (!strcmp(prop, "Rate")) return sd_bus_message_append(reply, "u", D.rec ? rec_session_rate(D.rec) : 0);
	if (!strcmp(prop, "Frames")) return sd_bus_message_append(reply, "t", D.rec ? rec_session_frames(D.rec) : (uint64_t)0);
	if (!strcmp(prop, "Overruns")) return sd_bus_message_append(reply, "t", D.rec ? rec_session_overruns(D.rec) : (uint64_t)0);
	if (!strcmp(prop, "SoundcheckPosition")) return sd_bus_message_append(reply, "t", D.sc ? sc_player_position(D.sc) : (uint64_t)0);
	return -EINVAL;
}

static int p_sources(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err)
{
	int r = sd_bus_message_open_container(reply, 'a', "s");
	for (int i = 0; r >= 0 && D.rec && i < D.n_sources; i++)
		r = sd_bus_message_append(reply, "s", D.sources_str[i]);
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int p_latency(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err)
{
	int r = sd_bus_message_open_container(reply, 'a', "{su}");
	bool armed = D.rec && rec_session_state(D.rec) != REC_ARMING;
	for (int i = 0; r >= 0 && armed && i < rec_session_n_tracks(D.rec); i++)
		r = sd_bus_message_append(reply, "{su}", rec_session_track_node(D.rec, i), (uint32_t)rec_session_track_latency(D.rec, i));
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int p_inputs(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err)
{
	int r = sd_bus_message_open_container(reply, 'a', "{ss}");
	for (int k = 0; r >= 0 && D.sc && k < D.sc_take.n_tracks; k++) {
		bool dup = false;
		for (int j = 0; j < k; j++)
			dup |= spa_streq(D.sc_take.tracks[j].node, D.sc_take.tracks[k].node);
		if (!dup)
			r = sd_bus_message_append(reply, "{ss}", D.sc_take.tracks[k].node, D.sc_mode[k]);
	}
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("Arm", SD_BUS_ARGS("as", sources, "a{sv}", options), SD_BUS_RESULT("s", take), m_arm, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Record", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", take), m_record, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Stop", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", sidecar), m_stop, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("AddMarker", SD_BUS_ARGS("s", name), SD_BUS_RESULT("t", frame), m_marker, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Takes", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", json), m_takes, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("GetTake", SD_BUS_ARGS("s", take), SD_BUS_RESULT("s", json), m_get_take, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("Soundcheck", SD_BUS_ARGS("s", take, "a{sv}", options), SD_BUS_NO_RESULT, m_soundcheck, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("SetInput", SD_BUS_ARGS("s", node, "s", mode), SD_BUS_NO_RESULT, m_set_input, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD_WITH_ARGS("StopSoundcheck", SD_BUS_NO_ARGS, SD_BUS_NO_RESULT, m_stop_soundcheck, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("State", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Take", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Dir", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Sources", "as", p_sources, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Rate", "u", p_num, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Frames", "t", p_num, 0, 0),
	SD_BUS_PROPERTY("Overruns", "t", p_num, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("CaptureLatency", "a{su}", p_latency, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("LastTake", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Soundcheck", "s", p_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("SoundcheckInputs", "a{ss}", p_inputs, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("SoundcheckPosition", "t", p_num, 0, 0),
	SD_BUS_SIGNAL_WITH_ARGS("Level", SD_BUS_ARGS("t", frames, "ad", peaks), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Overrun", SD_BUS_ARGS("t", frames), 0),
	SD_BUS_SIGNAL_WITH_ARGS("Marker", SD_BUS_ARGS("s", take, "s", name, "t", frame), 0),
	SD_BUS_SIGNAL_WITH_ARGS("TakeFinished", SD_BUS_ARGS("s", take, "s", sidecar, "t", frames, "t", overruns), 0),
	SD_BUS_SIGNAL_WITH_ARGS("SoundcheckInput", SD_BUS_ARGS("s", node, "s", mode), 0),
	SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------- loops */
static void on_bus_io(void *data, int fd, uint32_t mask)
{
	while (sd_bus_process(D.bus, NULL) > 0) {}
	sd_bus_flush(D.bus);
}

static void on_signal(void *data, int sig)
{
	D.quitting = true;
	stop_soundcheck();
	if (D.rec) {
		if (rec_session_state(D.rec) == REC_ARMING || rec_session_state(D.rec) == REC_ARMED) {
			reply_arm_error(ERR_STATE, "the recorder is shutting down");
			rec_session_stop(D.rec);
		} else {
			rec_session_stop(D.rec);  /* finish the take, then quit (ev_finished) */
		}
		if (rec_session_state(D.rec) == REC_ARMING) {
			rec_session_destroy(D.rec);
			D.rec = NULL;
		}
	}
	if (!busy())
		pw_main_loop_quit(D.loop);
}

static void usage(FILE *f)
{
	fputs("usage: backline-recorderd [--idle-exit SECONDS] [--replace]\n"
	      "  The Backline recorder on the session bus (" BUS_NAME ", " IFACE ").\n"
	      "  --idle-exit N  exit after N s with nothing armed, recording or in soundcheck (default 60; 0 = never)\n", f);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "idle-exit", required_argument, NULL, 'i' },
		{ "replace", no_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c, replace = 0;
	D.idle_exit = 60;
	while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (c) {
		case 'i': D.idle_exit = atoi(optarg); break;
		case 'r': replace = 1; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	const char *rt = getenv("XDG_RUNTIME_DIR"), *own = getenv("BACKLINE_RUNTIME_DIR");
	if (own || rt) {
		char d[512];
		if (own)
			snprintf(d, sizeof(d), "%s", own);
		else
			snprintf(d, sizeof(d), "%s/backline", rt);
		mkdir(d, 0700);
		snprintf(D.moves_file, sizeof(D.moves_file), "%s/soundcheck.json", d);
	}

	pw_init(&argc, &argv);
	D.loop = pw_main_loop_new(NULL);
	D.l = pw_main_loop_get_loop(D.loop);
	pw_loop_add_signal(D.l, SIGINT, on_signal, NULL);
	pw_loop_add_signal(D.l, SIGTERM, on_signal, NULL);
	D.context = pw_context_new(D.l, NULL, 0);
	D.core = pw_context_connect(D.context, NULL, 0);
	if (!D.core) {
		fprintf(stderr, "backline-recorderd: cannot connect to PipeWire\n");
		return 1;
	}
	D.graph = bl_graph_new(D.core, &graph_events, NULL);

	int r = sd_bus_open_user(&D.bus);
	if (r < 0) {
		fprintf(stderr, "backline-recorderd: session bus: %s\n", strerror(-r));
		return 1;
	}
	r = sd_bus_add_object_vtable(D.bus, NULL, OBJ_PATH, IFACE, vtable, NULL);
	if (r >= 0)
		r = sd_bus_request_name(D.bus, BUS_NAME, replace ? SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT
								 : SD_BUS_NAME_ALLOW_REPLACEMENT);
	if (r < 0) {
		fprintf(stderr, "backline-recorderd: cannot own %s: %s\n", BUS_NAME, strerror(-r));
		return 1;
	}
	D.bus_io = pw_loop_add_io(D.l, sd_bus_get_fd(D.bus), SPA_IO_IN, false, on_bus_io, NULL);
	D.level_timer = pw_loop_add_timer(D.l, on_level, NULL);
	struct timespec lv = { 0, 200 * 1000000L };
	pw_loop_update_timer(D.l, D.level_timer, &lv, &lv, false);
	D.arm_timer = pw_loop_add_timer(D.l, on_arm_timeout, NULL);
	D.idle_timer = pw_loop_add_timer(D.l, on_idle, NULL);
	D.reconcile_ev = pw_loop_add_event(D.l, on_reconcile, NULL);

	/* the graph is known after one roundtrip; then fix any leftover moves */
	pw_core_sync(D.core, PW_ID_CORE, 0);
	for (int i = 0; i < 20; i++)
		pw_loop_iterate(D.l, 10);
	if (D.moves_file[0] && access(D.moves_file, F_OK) == 0)
		restore_from_file();
	arm_idle();
	on_bus_io(NULL, 0, 0);
	fprintf(stderr, "backline-recorderd: on the session bus as %s\n", BUS_NAME);

	pw_main_loop_run(D.loop);

	stop_soundcheck();
	if (D.rec)
		rec_session_destroy(D.rec);
	for (int i = 0; i < 20; i++)   /* let the restored links reach the server */
		pw_loop_iterate(D.l, 5);
	pw_core_sync(D.core, PW_ID_CORE, 0);
	pw_loop_iterate(D.l, 50);
	bl_graph_destroy(D.graph);
	sd_bus_flush_close_unref(D.bus);
	pw_core_disconnect(D.core);
	pw_context_destroy(D.context);
	pw_main_loop_destroy(D.loop);
	pw_deinit();
	(void)now_ns;
	return 0;
}
