/* libbackline: D-Bus client for the Backline services.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "backline/backline.h"
#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#define HOST_NAME "io.github.manuaudio.Backline.Host"
#define HOST_PATH "/io/github/manuaudio/Backline/Host"
#define HOST_IFACE "io.github.manuaudio.Backline.Host1"
#define INST_PREFIX "io.github.manuaudio.Backline.Instance."
#define INST_PATH "/io/github/manuaudio/Backline/Instance"
#define INST_IFACE "io.github.manuaudio.Backline.Instance1"
#define REG_NAME "io.github.manuaudio.Backline.Registry"
#define REG_PATH "/io/github/manuaudio/Backline/Registry"
#define REG_IFACE "io.github.manuaudio.Backline.Registry1"

#define USEC_PER_SEC 1000000ull

struct bl_client {
	sd_bus *bus;
	char error[1024];
};

const char *bl_version(void)
{
	return "0.2.0";
}

void bl_free(void *p)
{
	free(p);
}

static int fail(bl_client *c, int r, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(bl_client *c, int r, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->error, sizeof(c->error), fmt, ap);
	va_end(ap);
	return r < 0 ? r : -EIO;
}

static int fail_bus(bl_client *c, int r, const sd_bus_error *e, const char *what)
{
	if (e && sd_bus_error_is_set(e))
		return fail(c, r, "%s: %s", what, e->message ? e->message : e->name);
	return fail(c, r, "%s: %s", what, strerror(-r));
}

/* for the other files of the library (internal.h) */
sd_bus *bl__bus(bl_client *c) { return c->bus; }
int bl__fail_bus(bl_client *c, int r, const sd_bus_error *e, const char *what) { return fail_bus(c, r, e, what); }
int bl__fail(bl_client *c, int r, const char *msg) { return fail(c, r, "%s", msg); }

int bl_client_open(bl_client **out)
{
	bl_client *c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;
	int r = sd_bus_open_user(&c->bus);
	if (r < 0) {
		free(c);
		return r;
	}
	*out = c;
	return 0;
}

void bl_client_close(bl_client *c)
{
	if (!c)
		return;
	sd_bus_flush_close_unref(c->bus);
	free(c);
}

const char *bl_client_error(const bl_client *c)
{
	return c ? c->error : "";
}

void bl_bus_name_for_node(const char *node, char *out, size_t n)
{
	size_t k = (size_t)snprintf(out, n, "%s", INST_PREFIX);
	if (k >= n)
		return;
	if (!node || !*node || isdigit((unsigned char)node[0]))
		k += (size_t)snprintf(out + k, n - k, "n_");
	for (; node && *node && k + 1 < n; node++)
		out[k++] = isalnum((unsigned char)*node) && (unsigned char)*node < 0x80 ? *node : '_';
	if (k < n)
		out[k] = 0;
}

/* --------------------------------------------------------------- a{sv} */
static void copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src ? src : "");
}

static int read_instance(sd_bus_message *m, bl_instance *in)
{
	memset(in, 0, sizeof(*in));
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r <= 0)
		return r < 0 ? r : -EBADMSG;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key, *t;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0)
			return r;
		if ((r = sd_bus_message_peek_type(m, NULL, &t)) < 0)
			return r;
		if ((r = sd_bus_message_enter_container(m, 'v', t)) < 0)
			return r;
		if (!strcmp(t, "s")) {
			const char *v;
			if ((r = sd_bus_message_read(m, "s", &v)) < 0)
				return r;
			struct { const char *k; char *d; size_t n; } map[] = {
				{ "id", in->id, sizeof(in->id) }, { "plugin", in->plugin, sizeof(in->plugin) },
				{ "plugin-path", in->plugin_path, sizeof(in->plugin_path) },
				{ "plugin-id", in->plugin_id, sizeof(in->plugin_id) }, { "node", in->node, sizeof(in->node) },
				{ "dbus", in->dbus, sizeof(in->dbus) }, { "target", in->target, sizeof(in->target) },
				{ "midi-follow", in->midi_follow, sizeof(in->midi_follow) },
				{ "state", in->state, sizeof(in->state) }, { "unit", in->unit, sizeof(in->unit) },
				{ "state-file", in->state_file, sizeof(in->state_file) },
				{ "log-file", in->log_file, sizeof(in->log_file) },
				{ "fail-reason", in->fail_reason, sizeof(in->fail_reason) },
			};
			for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
				if (!strcmp(key, map[i].k))
					copy(map[i].d, map[i].n, v);
		} else if (!strcmp(t, "u")) {
			uint32_t v;
			if ((r = sd_bus_message_read(m, "u", &v)) < 0)
				return r;
			if (!strcmp(key, "pid")) in->pid = v;
			else if (!strcmp(key, "osc-port")) in->osc_port = v;
			else if (!strcmp(key, "restarts")) in->restarts = v;
		} else if (!strcmp(t, "i")) {
			int32_t v;
			if ((r = sd_bus_message_read(m, "i", &v)) < 0)
				return r;
			if (!strcmp(key, "last-signal")) in->last_signal = v;
			else if (!strcmp(key, "last-exit-status")) in->last_exit_status = v;
		} else if (!strcmp(t, "b")) {
			int v;
			if ((r = sd_bus_message_read(m, "b", &v)) < 0)
				return r;
			if (!strcmp(key, "restart")) in->restart = v;
		} else {
			if ((r = sd_bus_message_skip(m, t)) < 0)
				return r;
		}
		sd_bus_message_exit_container(m); /* v */
		sd_bus_message_exit_container(m); /* e */
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

static int append_s(sd_bus_message *m, const char *k, const char *v)
{
	if (!v)
		return 0;
	return sd_bus_message_append(m, "{sv}", k, "s", v);
}

static int append_b(sd_bus_message *m, const char *k, int v)
{
	if (v < 0)
		return 0;
	return sd_bus_message_append(m, "{sv}", k, "b", v);
}

/* ------------------------------------------------------------------ Host1 */
static int host_load(bl_client *c, const char *plugin, const char *format, const bl_load_options *opt, char *id_out,
		     size_t id_size);

int bl_host_load(bl_client *c, const char *plugin, const bl_load_options *opt, char *id_out, size_t id_size)
{
	return host_load(c, plugin, NULL, opt, id_out, id_size);
}

int bl_host_load_format(bl_client *c, const char *plugin, const char *format, const bl_load_options *opt,
			char *id_out, size_t id_size)
{
	return host_load(c, plugin, format, opt, id_out, id_size);
}

static int host_load(bl_client *c, const char *plugin, const char *format, const bl_load_options *opt, char *id_out,
		     size_t id_size)
{
	bl_load_options def = BL_LOAD_OPTIONS_INIT;
	if (!opt)
		opt = &def;
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(c->bus, &m, HOST_NAME, HOST_PATH, HOST_IFACE, "Load");
	if (r >= 0) r = sd_bus_message_append(m, "s", plugin);
	if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{sv}");
	if (r >= 0) r = append_s(m, "id", opt->id);
	if (r >= 0) r = append_s(m, "plugin-id", opt->plugin_id);
	if (r >= 0) r = append_s(m, "name", opt->name);
	if (r >= 0) r = append_s(m, "target", opt->target);
	if (r >= 0) r = append_s(m, "midi-follow", opt->midi_follow);
	if (r >= 0) r = append_s(m, "state", opt->state);
	if (r >= 0) r = append_s(m, "memory-max", opt->memory_max);
	if (r >= 0) r = append_s(m, "cpu-quota", opt->cpu_quota);
	if (r >= 0) r = append_s(m, "format", format);
	if (r >= 0) r = append_b(m, "autoconnect", opt->autoconnect);
	if (r >= 0) r = append_b(m, "restart", opt->restart);
	if (r >= 0) r = append_b(m, "force", opt->force);
	if (r >= 0) r = sd_bus_message_close_container(m);
	if (r < 0) {
		sd_bus_message_unref(m);
		return fail(c, r, "Load: building the call: %s", strerror(-r));
	}
	/* a big plugin can take a while to come up; hostd's own limit is 20 s */
	r = sd_bus_call(c->bus, m, 30 * USEC_PER_SEC, &e, &reply);
	sd_bus_message_unref(m);
	if (r < 0) {
		r = fail_bus(c, r, &e, "Load");
		sd_bus_error_free(&e);
		return r;
	}
	const char *id;
	r = sd_bus_message_read(reply, "s", &id);
	if (r >= 0 && id_out)
		copy(id_out, id_size, id);
	sd_bus_message_unref(reply);
	return r < 0 ? fail(c, r, "Load: bad reply") : 0;
}

static int host_call_s(bl_client *c, const char *method, const char *id)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(c->bus, HOST_NAME, HOST_PATH, HOST_IFACE, method, &e, NULL, "s", id);
	if (r < 0) {
		r = fail_bus(c, r, &e, method);
		sd_bus_error_free(&e);
		return r;
	}
	return 0;
}

int bl_host_unload(bl_client *c, const char *id)
{
	return host_call_s(c, "Unload", id);
}

int bl_host_restart(bl_client *c, const char *id)
{
	return host_call_s(c, "Restart", id);
}

int bl_host_get(bl_client *c, const char *id, bl_instance *out)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int r = sd_bus_call_method(c->bus, HOST_NAME, HOST_PATH, HOST_IFACE, "Get", &e, &reply, "s", id);
	if (r < 0) {
		r = fail_bus(c, r, &e, "Get");
		sd_bus_error_free(&e);
		return r;
	}
	r = read_instance(reply, out);
	sd_bus_message_unref(reply);
	return r < 0 ? fail(c, r, "Get: bad reply") : 0;
}

int bl_host_list(bl_client *c, bl_instance **out, size_t *n)
{
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	*out = NULL;
	*n = 0;
	int r = sd_bus_call_method(c->bus, HOST_NAME, HOST_PATH, HOST_IFACE, "List", &e, &reply, "");
	if (r < 0) {
		r = fail_bus(c, r, &e, "List");
		sd_bus_error_free(&e);
		return r;
	}
	size_t cap = 0;
	bl_instance *v = NULL;
	r = sd_bus_message_enter_container(reply, 'a', "a{sv}");
	while (r >= 0) {
		if (sd_bus_message_at_end(reply, false) > 0)
			break;
		if (*n == cap) {
			cap = cap ? cap * 2 : 8;
			bl_instance *nv = realloc(v, cap * sizeof(*v));
			if (!nv) {
				r = -ENOMEM;
				break;
			}
			v = nv;
		}
		r = read_instance(reply, &v[*n]);
		if (r >= 0)
			(*n)++;
	}
	sd_bus_message_unref(reply);
	if (r < 0) {
		free(v);
		*n = 0;
		return fail(c, r, "List: bad reply");
	}
	*out = v;
	return 0;
}

struct watch_ctx {
	bl_event_cb cb;
	void *userdata;
	bool stop;
};

static int on_host_signal(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	struct watch_ctx *w = userdata;
	bl_event ev;
	memset(&ev, 0, sizeof(ev));
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ev.monotonic_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	ev.name = sd_bus_message_get_member(m);
	if (!ev.name)
		return 0;
	int r = -EINVAL;
	if (!strcmp(ev.name, "Loaded")) {
		ev.type = BL_EVENT_LOADED;
		r = sd_bus_message_read(m, "s", &ev.id);
	} else if (!strcmp(ev.name, "Unloaded")) {
		ev.type = BL_EVENT_UNLOADED;
		r = sd_bus_message_read(m, "s", &ev.id);
	} else if (!strcmp(ev.name, "Crashed")) {
		ev.type = BL_EVENT_CRASHED;
		r = sd_bus_message_read(m, "sii", &ev.id, &ev.signal, &ev.exit_status);
	} else if (!strcmp(ev.name, "Restarted")) {
		ev.type = BL_EVENT_RESTARTED;
		r = sd_bus_message_read(m, "suu", &ev.id, &ev.pid, &ev.restarts);
	} else if (!strcmp(ev.name, "Failed")) {
		ev.type = BL_EVENT_FAILED;
		r = sd_bus_message_read(m, "ss", &ev.id, &ev.reason);
	}
	if (r < 0)
		return 0;
	if (w->cb(&ev, w->userdata))
		w->stop = true;
	return 0;
}

int bl_host_watch(bl_client *c, bl_event_cb cb, void *userdata, int timeout_ms)
{
	struct watch_ctx w = { cb, userdata, false };
	sd_bus_slot *slot = NULL;
	int r = sd_bus_match_signal(c->bus, &slot, NULL, HOST_PATH, HOST_IFACE, NULL, on_host_signal, &w);
	if (r < 0)
		return fail(c, r, "watch: %s", strerror(-r));
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t deadline = timeout_ms < 0 ? UINT64_MAX
		: (uint64_t)ts.tv_sec * USEC_PER_SEC + (uint64_t)ts.tv_nsec / 1000 + (uint64_t)timeout_ms * 1000;
	while (!w.stop) {
		r = sd_bus_process(c->bus, NULL);
		if (r < 0)
			break;
		if (r > 0)
			continue;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t now = (uint64_t)ts.tv_sec * USEC_PER_SEC + (uint64_t)ts.tv_nsec / 1000;
		if (now >= deadline) {
			r = 0;
			break;
		}
		r = sd_bus_wait(c->bus, deadline == UINT64_MAX ? (uint64_t)-1 : deadline - now);
		if (r < 0)
			break;
	}
	sd_bus_slot_unref(slot);
	return r < 0 ? fail(c, r, "watch: %s", strerror(-r)) : 0;
}

/* -------------------------------------------------------------- Instance1 */
int bl_instance_resolve(bl_client *c, const char *instance, char *bus_name, size_t n)
{
	if (!strncmp(instance, INST_PREFIX, strlen(INST_PREFIX))) {
		copy(bus_name, n, instance);
		return 0;
	}
	/* a hostd instance id (or a node name hostd knows) */
	bl_instance in;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int r = sd_bus_call_method(c->bus, HOST_NAME, HOST_PATH, HOST_IFACE, "Get", &e, &reply, "s", instance);
	sd_bus_error_free(&e);
	if (r >= 0 && read_instance(reply, &in) >= 0 && in.dbus[0]) {
		sd_bus_message_unref(reply);
		copy(bus_name, n, in.dbus);
		return 0;
	}
	sd_bus_message_unref(reply);
	/* any worker, by node name */
	bl_bus_name_for_node(instance, bus_name, n);
	return 0;
}

#define RESOLVE(c, instance, name) \
	char name[300]; \
	bl_instance_resolve(c, instance, name, sizeof(name))

static int inst_call(bl_client *c, const char *instance, const char *method, sd_bus_message **reply,
		     const char *types, ...)
{
	RESOLVE(c, instance, bus);
	sd_bus_message *m = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_message_new_method_call(c->bus, &m, bus, INST_PATH, INST_IFACE, method);
	if (r >= 0 && types) {
		va_list ap;
		va_start(ap, types);
		r = sd_bus_message_appendv(m, types, ap);
		va_end(ap);
	}
	if (r >= 0)
		r = sd_bus_call(c->bus, m, 0, &e, reply);
	sd_bus_message_unref(m);
	if (r < 0) {
		char what[400];
		snprintf(what, sizeof(what), "%s on '%s'", method, instance);
		if (sd_bus_error_has_name(&e, SD_BUS_ERROR_SERVICE_UNKNOWN) ||
		    sd_bus_error_has_name(&e, SD_BUS_ERROR_NAME_HAS_NO_OWNER)) {
			sd_bus_error_free(&e);
			return fail(c, -ENOENT, "%s: no such instance (looked for %s)", what, bus);
		}
		r = fail_bus(c, r, &e, what);
		sd_bus_error_free(&e);
	}
	return r;
}

int bl_instance_params(bl_client *c, const char *instance, bl_param **out, size_t *n)
{
	sd_bus_message *reply = NULL;
	*out = NULL;
	*n = 0;
	int r = inst_call(c, instance, "ListParams", &reply, NULL);
	if (r < 0)
		return r;
	size_t cap = 0;
	bl_param *v = NULL;
	r = sd_bus_message_enter_container(reply, 'a', "(ussddd)");
	while (r >= 0) {
		uint32_t id;
		const char *name, *module;
		double val, mn, mx;
		r = sd_bus_message_read(reply, "(ussddd)", &id, &name, &module, &val, &mn, &mx);
		if (r <= 0)
			break;
		if (*n == cap) {
			cap = cap ? cap * 2 : 64;
			bl_param *nv = realloc(v, cap * sizeof(*v));
			if (!nv) {
				r = -ENOMEM;
				break;
			}
			v = nv;
		}
		bl_param *p = &v[(*n)++];
		p->id = id;
		copy(p->name, sizeof(p->name), name);
		copy(p->module, sizeof(p->module), module);
		p->value = val;
		p->min = mn;
		p->max = mx;
	}
	sd_bus_message_unref(reply);
	if (r < 0) {
		free(v);
		*n = 0;
		return fail(c, r, "ListParams: bad reply");
	}
	*out = v;
	return 0;
}

int bl_instance_get_param(bl_client *c, const char *instance, const char *key, double *value)
{
	sd_bus_message *reply = NULL;
	int r = inst_call(c, instance, "GetParam", &reply, "s", key);
	if (r < 0)
		return r;
	r = sd_bus_message_read(reply, "d", value);
	sd_bus_message_unref(reply);
	return r < 0 ? fail(c, r, "GetParam: bad reply") : 0;
}

int bl_instance_set_param(bl_client *c, const char *instance, const char *key, double value, double *applied)
{
	sd_bus_message *reply = NULL;
	int r = inst_call(c, instance, "SetParam", &reply, "sd", key, value);
	if (r < 0)
		return r;
	double a = value;
	r = sd_bus_message_read(reply, "d", &a);
	if (applied)
		*applied = a;
	sd_bus_message_unref(reply);
	return r < 0 ? fail(c, r, "SetParam: bad reply") : 0;
}

int bl_instance_note(bl_client *c, const char *instance, bool on, uint8_t channel, uint8_t key, uint8_t velocity)
{
	sd_bus_message *reply = NULL;
	int r = on ? inst_call(c, instance, "NoteOn", &reply, "yyy", channel, key, velocity)
		   : inst_call(c, instance, "NoteOff", &reply, "yy", channel, key);
	sd_bus_message_unref(reply);
	return r < 0 ? r : 0;
}

int bl_instance_save_state(bl_client *c, const char *instance, const char *path)
{
	sd_bus_message *reply = NULL;
	int r = inst_call(c, instance, "SaveState", &reply, "s", path);
	sd_bus_message_unref(reply);
	return r < 0 ? r : 0;
}

int bl_instance_load_state(bl_client *c, const char *instance, const char *path)
{
	sd_bus_message *reply = NULL;
	int r = inst_call(c, instance, "LoadState", &reply, "s", path);
	sd_bus_message_unref(reply);
	return r < 0 ? r : 0;
}

int bl_instance_presets(bl_client *c, const char *instance, bl_preset **out, size_t *n)
{
	sd_bus_message *reply = NULL;
	*out = NULL;
	*n = 0;
	int r = inst_call(c, instance, "ListPresets", &reply, NULL);
	if (r < 0)
		return r;
	size_t cap = 0;
	bl_preset *v = NULL;
	r = sd_bus_message_enter_container(reply, 'a', "(sss)");
	while (r >= 0) {
		const char *id, *name, *loc;
		r = sd_bus_message_read(reply, "(sss)", &id, &name, &loc);
		if (r <= 0)
			break;
		if (*n == cap) {
			cap = cap ? cap * 2 : 64;
			bl_preset *nv = realloc(v, cap * sizeof(*v));
			if (!nv) {
				r = -ENOMEM;
				break;
			}
			v = nv;
		}
		bl_preset *p = &v[(*n)++];
		copy(p->id, sizeof(p->id), id);
		copy(p->name, sizeof(p->name), name);
		copy(p->location, sizeof(p->location), loc);
	}
	sd_bus_message_unref(reply);
	if (r < 0) {
		free(v);
		*n = 0;
		return fail(c, r, "ListPresets: bad reply");
	}
	*out = v;
	return 0;
}

int bl_instance_load_preset(bl_client *c, const char *instance, const char *id)
{
	sd_bus_message *reply = NULL;
	int r = inst_call(c, instance, "LoadPreset", &reply, "s", id);
	sd_bus_message_unref(reply);
	return r < 0 ? r : 0;
}

int bl_instance_save_preset(bl_client *c, const char *instance, const char *name_or_path, char *path_out, size_t n)
{
	sd_bus_message *reply = NULL;
	int r = inst_call(c, instance, "SavePreset", &reply, "s", name_or_path);
	if (r < 0)
		return r;
	const char *path;
	r = sd_bus_message_read(reply, "s", &path);
	if (r >= 0 && path_out)
		copy(path_out, n, path);
	sd_bus_message_unref(reply);
	return r < 0 ? fail(c, r, "SavePreset: bad reply") : 0;
}

int bl_instance_counter(bl_client *c, const char *instance, const char *name, uint64_t *out)
{
	RESOLVE(c, instance, bus);
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_get_property_trivial(c->bus, bus, INST_PATH, INST_IFACE, name, &e, 't', out);
	if (r < 0) {
		r = fail_bus(c, r, &e, name);
		sd_bus_error_free(&e);
	}
	return r;
}

/* -------------------------------------------------------------- Registry1 */
static int reg_call_string(bl_client *c, const char *method, uint64_t timeout_us, char **out, const char *types, ...)
{
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error e = SD_BUS_ERROR_NULL;
	*out = NULL;
	int r = sd_bus_message_new_method_call(c->bus, &m, REG_NAME, REG_PATH, REG_IFACE, method);
	if (r >= 0) {
		va_list ap;
		va_start(ap, types);
		r = sd_bus_message_appendv(m, types, ap);
		va_end(ap);
	}
	if (r >= 0)
		r = sd_bus_call(c->bus, m, timeout_us, &e, &reply);
	sd_bus_message_unref(m);
	if (r < 0) {
		char what[64];
		snprintf(what, sizeof(what), "Registry %s", method);
		r = fail_bus(c, r, &e, what);
		sd_bus_error_free(&e);
		return r;
	}
	const char *s;
	r = sd_bus_message_read(reply, "s", &s);
	if (r >= 0) {
		*out = strdup(s);
		if (!*out)
			r = -ENOMEM;
	}
	sd_bus_message_unref(reply);
	return r < 0 ? fail(c, r, "Registry %s: bad reply", method) : 0;
}

int bl_plugins_query(bl_client *c, const char *filter_json, char **json_out)
{
	return reg_call_string(c, "Query", 0, json_out, "s", filter_json ? filter_json : "");
}

int bl_plugins_show(bl_client *c, const char *id_or_name, char **json_out)
{
	return reg_call_string(c, "Show", 0, json_out, "s", id_or_name);
}

int bl_plugins_scan(bl_client *c, bool validate, char **summary_json_out)
{
	return reg_call_string(c, "Scan", 900 * USEC_PER_SEC, summary_json_out, "b", (int)validate);
}
