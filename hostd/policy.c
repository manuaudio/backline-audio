/* SPDX-License-Identifier: MIT */
#include "policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <json-c/json.h>

void bl_restart_policy_init(struct bl_restart_policy *p)
{
	memset(p, 0, sizeof(*p));
	p->max_crashes = 5;
	p->window_ns = 60 * BL_NSEC;
	p->healthy_ns = 10 * BL_NSEC;
	p->base_ns = BL_NSEC / 4;
	p->max_ns = 5 * BL_NSEC;
}

void bl_restart_policy_reset(struct bl_restart_policy *p)
{
	p->n_crashes = 0;
	p->consecutive = 0;
	memset(p->crash_ns, 0, sizeof(p->crash_ns));
}

uint64_t bl_restart_policy_crash(struct bl_restart_policy *p, uint64_t now, uint64_t ran_ns)
{
	const unsigned ring = sizeof(p->crash_ns) / sizeof(p->crash_ns[0]);
	p->crash_ns[p->n_crashes % ring] = now;
	p->n_crashes++;

	unsigned in_window = 0;
	for (unsigned i = 0; i < ring && i < p->n_crashes; i++)
		if (now - p->crash_ns[i] <= p->window_ns)
			in_window++;
	if (in_window > p->max_crashes)
		return BL_RESTART_GIVE_UP;

	if (ran_ns >= p->healthy_ns)
		p->consecutive = 0;
	uint64_t delay = 0;
	if (p->consecutive > 0) {
		delay = p->base_ns;
		for (unsigned i = 1; i < p->consecutive && delay < p->max_ns; i++)
			delay *= 2;
		if (delay > p->max_ns)
			delay = p->max_ns;
	}
	p->consecutive++;
	return delay;
}

size_t bl_slug(const char *in, char *out, size_t n)
{
	size_t k = 0;
	bool dash = false;
	if (n == 0)
		return 0;
	for (; in && *in && k + 1 < n; in++) {
		unsigned char c = (unsigned char)*in;
		if (isalnum(c) && c < 0x80) {
			if (dash && k > 0 && k + 2 < n)
				out[k++] = '-';
			dash = false;
			out[k++] = (char)tolower(c);
		} else {
			dash = true;
		}
	}
	out[k] = 0;
	return k;
}

void bl_instance_bus_name(const char *node, char *out, size_t n)
{
	const char *prefix = "io.github.manuaudio.Backline.Instance.";
	size_t k = (size_t)snprintf(out, n, "%s", prefix);
	if (k >= n)
		return;
	if (!node || !*node || isdigit((unsigned char)node[0]))
		k += (size_t)snprintf(out + k, n - k, "n_");
	for (; node && *node && k + 1 < n; node++)
		out[k++] = isalnum((unsigned char)*node) && (unsigned char)*node < 0x80 ? *node : '_';
	if (k < n)
		out[k] = 0;
}

static int get_str(struct json_object *o, const char *key, char *dst, size_t n, bool required)
{
	struct json_object *v;
	if (!json_object_object_get_ex(o, key, &v) || !json_object_is_type(v, json_type_string)) {
		if (n)
			dst[0] = 0;
		return required ? -1 : 0;
	}
	snprintf(dst, n, "%s", json_object_get_string(v));
	return 0;
}

int bl_parse_ready(const char *line, struct bl_ready *out)
{
	memset(out, 0, sizeof(*out));
	struct json_object *o = json_tokener_parse(line);
	if (!o)
		return -1;
	int r = -1;
	struct json_object *v;
	if (!json_object_is_type(o, json_type_object))
		goto done;
	if (get_str(o, "node", out->node, sizeof(out->node), true) < 0)
		goto done;
	get_str(o, "plugin", out->plugin, sizeof(out->plugin), false);
	get_str(o, "plugin_path", out->plugin_path, sizeof(out->plugin_path), false);
	get_str(o, "dbus", out->dbus, sizeof(out->dbus), false);
	get_str(o, "format", out->format, sizeof(out->format), false);
	if (!json_object_object_get_ex(o, "osc_port", &v) || !json_object_is_type(v, json_type_int))
		goto done;
	out->osc_port = (unsigned)json_object_get_int(v);
	if (!json_object_object_get_ex(o, "pid", &v) || !json_object_is_type(v, json_type_int))
		goto done;
	out->pid = json_object_get_int(v);
	r = 0;
done:
	json_object_put(o);
	return r;
}
