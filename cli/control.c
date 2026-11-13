/* backline CLI: the control service (map ...). docs/api/Control1.md.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
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

static void out_kv(bool json, const char *key, const char *val, const char *text_fmt)
{
	if (json) {
		json_object *o = json_object_new_object();
		json_object_object_add(o, key, json_object_new_string(val));
		puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
		json_object_put(o);
	} else {
		printf(text_fmt, val);
		putchar('\n');
	}
}

static int usage_map(void)
{
	fputs("usage: backline map add SOURCE TARGET [--min X] [--max Y] [--invert] [--in-min A --in-max B] [--arg S]... [--id ID]\n"
	      "                                [--encoding abs|rel-2c|rel-sm|rel-offset|rel-inc-dec] [--step D] [--accel A]\n"
	      "                                [--takeover jump|pickup|scale] [--feedback] [--bits 7|14] [--gesture auto]\n"
	      "                                [--role ROLE]\n"
	      "       backline map learn TARGET [--from midi|osc] [--timeout S] [same options]\n"
	      "       backline map list | remove ID | cancel | save | status\n"
	      "       backline map follow REGEX        link matching MIDI sources to controld ('' = none)\n"
	      "       backline map out REGEX           link controld's MIDI feedback to matching MIDI inputs ('' = none)\n"
	      "       backline map listen udp|tcp ADDRESS PORT [--allow CIDR]... [--secret S | --secret-file F]\n"
	      "                                        [--insecure-udp] [--max-rate N] [--max-clients N] [--params ROLE]\n"
	      "       backline map unlisten ID\n"
	      "       backline map profile list | show ID | load ID | unload ID | delete ID\n"
	      "       backline map profile save ID [--name N] [--match REGEX] [--no-auto] [--mapping MAPPING-ID]...\n"
	      "  SOURCE: midi:cc:CH:NUM  midi:cc14:CH:MSB  midi:nrpn:CH:PARAM  midi:rpn:CH:PARAM  midi:note:CH:KEY\n"
	      "          midi:pitchbend:CH  midi:program:CH  osc:/ADDRESS  (CH 1-16 or *)\n"
	      "  TARGET: instance:NODE/PARAM  transport:play|stop|toggle|tempo|locate:POS  recorder:arm|record|stop|marker\n"
	      "          volume:NODE  mute:NODE  control:NODE/NAME  param:/ROOT/PATH\n", stderr);
	return 2;
}

/* The Control1 additions' options, sent as JSON (bl_control_add_json). */
static json_object *extra;

static void extra_add(const char *k, json_object *v)
{
	if (!extra)
		extra = json_object_new_object();
	json_object_object_add(extra, k, v);
}

/* Parses the mapping options from argv[i...]; returns the index after them,
 * or -1. args points into argv. */
static int map_opts(int argc, char **argv, int i, bl_map_options *o, const char **args, const char **from, unsigned *timeout)
{
	for (; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!from && v && (!strcmp(a, "--encoding") || !strcmp(a, "--takeover") || !strcmp(a, "--gesture") ||
				   !strcmp(a, "--role"))) {
			extra_add(a + 2, json_object_new_string(v));
			i++;
			continue;
		}
		if (!from && v && (!strcmp(a, "--step") || !strcmp(a, "--accel"))) {
			extra_add(a + 2, json_object_new_double(atof(v)));
			i++;
			continue;
		}
		if (!from && v && !strcmp(a, "--bits")) {
			extra_add("bits", json_object_new_int(atoi(v)));
			i++;
			continue;
		}
		if (!from && !strcmp(a, "--feedback")) {
			extra_add("feedback", json_object_new_boolean(1));
			continue;
		}
		if (!strcmp(a, "--min") && v) { o->min = atof(v); o->has_min = true; i++; }
		else if (!strcmp(a, "--max") && v) { o->max = atof(v); o->has_max = true; i++; }
		else if (!strcmp(a, "--invert")) o->invert = true;
		else if (!strcmp(a, "--in-min") && v) { o->in_min = atof(v); o->has_in = true; i++; }
		else if (!strcmp(a, "--in-max") && v) { o->in_max = atof(v); o->has_in = true; i++; }
		else if (!strcmp(a, "--arg") && v) { if (o->n_args < 8) args[o->n_args++] = v; i++; }
		else if (!strcmp(a, "--id") && v) { o->id = v; i++; }
		else if (from && !strcmp(a, "--from") && v) { *from = v; i++; }
		else if (timeout && !strcmp(a, "--timeout") && v) { *timeout = (unsigned)atoi(v); i++; }
		else {
			fprintf(stderr, "backline map: unknown option '%s'\n", a);
			return -1;
		}
	}
	o->args = args;
	return i;
}

static int read_secret_file(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -errno;
	size_t k = fread(out, 1, n - 1, f);
	fclose(f);
	out[k] = 0;
	while (k && (out[k - 1] == '\n' || out[k - 1] == '\r'))
		out[--k] = 0;
	return 0;
}

static void print_status_text(const bl_control_status *s)
{
	printf("mappings %u, listeners %zu%s\n", s->mappings, s->n_listeners, s->n_listeners ? "" : " (no network socket)");
	for (size_t i = 0; i < s->n_listeners; i++)
		printf("  %s\n", s->listeners[i]);
	printf("node %s, midi-follow '%s'%s%s\n", s->node, s->midi_follow, s->learning[0] ? ", learning " : "", s->learning);
	printf("received %" PRIu64 " OSC, %" PRIu64 " MIDI; dispatched %" PRIu64 ", target errors %" PRIu64 "\n", s->received,
	       s->midi_events, s->dispatched, s->target_errors);
	printf("rejected %" PRIu64 " (auth failures %" PRIu64 ", malformed %" PRIu64 ")\n", s->rejected, s->auth_failures,
	       s->malformed);
}

static void print_status_json(const bl_control_status *s)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "mappings", json_object_new_int64(s->mappings));
	json_object *l = json_object_new_array();
	for (size_t i = 0; i < s->n_listeners; i++)
		json_object_array_add(l, json_object_new_string(s->listeners[i]));
	json_object_object_add(o, "listeners", l);
	json_object_object_add(o, "node", json_object_new_string(s->node));
	json_object_object_add(o, "midi_follow", json_object_new_string(s->midi_follow));
	json_object_object_add(o, "learning", json_object_new_string(s->learning));
	json_object_object_add(o, "config_dir", json_object_new_string(s->config_dir));
	json_object_object_add(o, "received", json_object_new_int64((int64_t)s->received));
	json_object_object_add(o, "midi_events", json_object_new_int64((int64_t)s->midi_events));
	json_object_object_add(o, "dispatched", json_object_new_int64((int64_t)s->dispatched));
	json_object_object_add(o, "target_errors", json_object_new_int64((int64_t)s->target_errors));
	json_object_object_add(o, "rejected", json_object_new_int64((int64_t)s->rejected));
	json_object_object_add(o, "auth_failures", json_object_new_int64((int64_t)s->auth_failures));
	json_object_object_add(o, "malformed", json_object_new_int64((int64_t)s->malformed));
	puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
}

static const char *jget(json_object *o, const char *k)
{
	json_object *v;
	return json_object_object_get_ex(o, k, &v) ? json_object_get_string(v) : "";
}

static bool jbool(json_object *o, const char *k)
{
	json_object *v;
	return json_object_object_get_ex(o, k, &v) && json_object_get_boolean(v);
}

/* backline map profile ... (MIDI learn profiles) */
static int cli_profile(bl_client *c, bool json, int argc, char **argv)
{
	if (argc < 1)
		return usage_map();
	const char *sub = argv[0];
	if (!strcmp(sub, "list") || (!strcmp(sub, "show") && argc == 2)) {
		char *js = NULL;
		bool list = !strcmp(sub, "list");
		if ((list ? bl_control_profiles(c, &js) : bl_control_profile(c, argv[1], &js)) < 0)
			return fail(c, list ? "map profile list" : "map profile show");
		json_object *o = json_tokener_parse(js);
		if (json || !o) {
			puts(o ? json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE) : js);
		} else if (list) {
			json_object *ps;
			if (json_object_object_get_ex(o, "profiles", &ps))
				for (size_t i = 0; i < json_object_array_length(ps); i++) {
					json_object *p = json_object_array_get_idx(ps, i), *v;
					int nc = json_object_object_get_ex(p, "controls", &v) ? json_object_get_int(v) : 0;
					int nb = json_object_object_get_ex(p, "bindings", &v) ? json_object_get_int(v) : 0;
					printf("%-28s %-44s %3d controls, %2d bindings  %s%s%s%s\n", jget(p, "id"), jget(p, "name"), nc, nb,
					       jbool(p, "system") ? "system" : "user", jbool(p, "verified") ? ", verified" : ", unverified",
					       jbool(p, "active") ? ", active" : "", jbool(p, "present") ? ", device present" : "");
				}
		} else {
			printf("%s: %s\n", jget(o, "id"), jget(o, "name"));
			printf("  %s, %s; match /%s/\n", jget(o, "model"), jget(o, "mode"), jget(o, "match"));
			printf("  %s%s; %s\n", jbool(o, "verified") ? "verified on hardware" : "not verified on hardware",
			       jbool(o, "system") ? ", shipped" : ", yours", jget(o, "path"));
			if (*jget(o, "note"))
				printf("  %s\n", jget(o, "note"));
			json_object *cs, *bs;
			if (json_object_object_get_ex(o, "controls", &cs))
				for (size_t i = 0; i < json_object_array_length(cs); i++) {
					json_object *x = json_object_array_get_idx(cs, i);
					printf("  %-22s %-18s %s\n", jget(x, "name"), jget(x, "source"), jget(x, "label"));
				}
			if (json_object_object_get_ex(o, "bindings", &bs))
				for (size_t i = 0; i < json_object_array_length(bs); i++) {
					json_object *x = json_object_array_get_idx(bs, i);
					printf("  bound %-16s -> %s\n", jget(x, "control"), jget(x, "target"));
				}
		}
		json_object_put(o);
		bl_free(js);
		return 0;
	}
	if (!strcmp(sub, "save") && argc >= 2) {
		bl_profile_options o = BL_PROFILE_OPTIONS_INIT;
		const char *ids[256];
		size_t n_ids = 0;
		for (int i = 2; i < argc; i++) {
			const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
			if (!strcmp(a, "--name") && v) { o.name = v; i++; }
			else if (!strcmp(a, "--match") && v) { o.match = v; i++; }
			else if (!strcmp(a, "--no-auto")) o.autoload = 0;
			else if (!strcmp(a, "--auto")) o.autoload = 1;
			else if (!strcmp(a, "--mapping") && v) { if (n_ids < 256) ids[n_ids++] = v; i++; }
			else {
				fprintf(stderr, "backline map profile save: unknown option '%s'\n", a);
				return 2;
			}
		}
		if (n_ids) {
			o.mappings = ids;
			o.n_mappings = n_ids;
		}
		char path[800];
		if (bl_control_save_profile(c, argv[1], &o, path, sizeof(path)) < 0)
			return fail(c, "map profile save");
		out_kv(json, "path", path, "saved %s");
		return 0;
	}
	if (!strcmp(sub, "load") && argc == 2) {
		int k = bl_control_load_profile(c, argv[1]);
		if (k < 0)
			return fail(c, "map profile load");
		if (json)
			printf("{\"profile\": \"%s\", \"mappings\": %d}\n", argv[1], k);
		else
			printf("loaded %s: %d mappings\n", argv[1], k);
		return 0;
	}
	if (!strcmp(sub, "unload") && argc == 2) {
		if (bl_control_unload_profile(c, argv[1]) < 0)
			return fail(c, "map profile unload");
		return 0;
	}
	if (!strcmp(sub, "delete") && argc == 2) {
		if (bl_control_delete_profile(c, argv[1]) < 0)
			return fail(c, "map profile delete");
		out_kv(json, "deleted", argv[1], "deleted %s");
		return 0;
	}
	return usage_map();
}

int cli_map(bl_client *c, bool json, int argc, char **argv)
{
	if (argc < 1)
		return usage_map();
	const char *sub = argv[0];
	char id[160];
	if (!strcmp(sub, "profile"))
		return cli_profile(c, json, argc - 1, argv + 1);
	if (!strcmp(sub, "add")) {
		if (argc < 3)
			return usage_map();
		bl_map_options o = BL_MAP_OPTIONS_INIT;
		const char *args[8];
		if (map_opts(argc, argv, 3, &o, args, NULL, NULL) < 0)
			return 2;
		if (extra) {
			/* the newer options: the whole mapping as JSON */
			extra_add("source", json_object_new_string(argv[1]));
			extra_add("target", json_object_new_string(argv[2]));
			if (o.has_min) extra_add("min", json_object_new_double(o.min));
			if (o.has_max) extra_add("max", json_object_new_double(o.max));
			if (o.invert) extra_add("invert", json_object_new_boolean(1));
			if (o.has_in) {
				extra_add("in-min", json_object_new_double(o.in_min));
				extra_add("in-max", json_object_new_double(o.in_max));
			}
			if (o.id) extra_add("id", json_object_new_string(o.id));
			if (o.n_args) {
				json_object *a = json_object_new_array();
				for (size_t k = 0; k < o.n_args; k++)
					json_object_array_add(a, json_object_new_string(args[k]));
				extra_add("args", a);
			}
			int r = bl_control_add_json(c, json_object_to_json_string(extra), id, sizeof(id));
			json_object_put(extra);
			extra = NULL;
			if (r < 0)
				return fail(c, "map add");
		} else if (bl_control_map(c, argv[1], argv[2], &o, id, sizeof(id)) < 0) {
			return fail(c, "map add");
		}
		out_kv(json, "id", id, "%s");
		return 0;
	}
	if (!strcmp(sub, "learn")) {
		if (argc < 2)
			return usage_map();
		bl_map_options o = BL_MAP_OPTIONS_INIT;
		const char *args[8], *from = NULL;
		unsigned timeout = 30;
		if (map_opts(argc, argv, 2, &o, args, &from, &timeout) < 0)
			return 2;
		if (!json)
			fprintf(stderr, "move a control or send an OSC message (%u s)...\n", timeout);
		char source[160];
		if (bl_control_learn(c, argv[1], &o, from, timeout, id, sizeof(id), source, sizeof(source)) < 0)
			return fail(c, "map learn");
		if (json) {
			printf("{\"id\": \"%s\", \"source\": \"%s\", \"target\": \"%s\"}\n", id, source, argv[1]);
		} else {
			printf("%s: %s -> %s\n", id, source, argv[1]);
		}
		return 0;
	}
	if (!strcmp(sub, "list")) {
		char *js = NULL;
		if (bl_control_mappings(c, &js) < 0)
			return fail(c, "map list");
		json_object *o = json_tokener_parse(js);
		if (json || !o) {
			puts(o ? json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE) : js);
		} else {
			json_object *ms, *ls;
			if (json_object_object_get_ex(o, "mappings", &ms))
				for (size_t i = 0; i < json_object_array_length(ms); i++) {
					json_object *m = json_object_array_get_idx(ms, i), *v;
					const char *mid = json_object_object_get_ex(m, "id", &v) ? json_object_get_string(v) : "";
					const char *s = json_object_object_get_ex(m, "source", &v) ? json_object_get_string(v) : "";
					const char *t = json_object_object_get_ex(m, "target", &v) ? json_object_get_string(v) : "";
					const char *ctl = json_object_object_get_ex(m, "control", &v) ? json_object_get_string(v) : NULL;
					if (ctl)
						printf("%-8s %-24s -> %s  (%s)\n", mid, s, t, ctl);
					else
						printf("%-8s %-24s -> %s\n", mid, s, t);
				}
			if (json_object_object_get_ex(o, "listeners", &ls))
				for (size_t i = 0; i < json_object_array_length(ls); i++) {
					json_object *l = json_object_array_get_idx(ls, i), *v;
					printf("listening %s%s\n", json_object_object_get_ex(l, "id", &v) ? json_object_get_string(v) : "",
					       json_object_object_get_ex(l, "auth", &v) && json_object_get_boolean(v) ? " (secret)" : "");
				}
		}
		json_object_put(o);
		bl_free(js);
		return 0;
	}
	if (!strcmp(sub, "remove") && argc == 2) {
		if (bl_control_remove(c, argv[1]) < 0)
			return fail(c, "map remove");
		out_kv(json, "removed", argv[1], "removed %s");
		return 0;
	}
	if (!strcmp(sub, "cancel")) {
		if (bl_control_cancel_learn(c) < 0)
			return fail(c, "map cancel");
		return 0;
	}
	if (!strcmp(sub, "save")) {
		char path[800];
		if (bl_control_save(c, path, sizeof(path)) < 0)
			return fail(c, "map save");
		out_kv(json, "path", path, "saved %s");
		return 0;
	}
	if (!strcmp(sub, "out") && argc == 2) {
		if (bl_control_set_midi_out(c, argv[1]) < 0)
			return fail(c, "map out");
		return 0;
	}
	if (!strcmp(sub, "follow") && argc == 2) {
		if (bl_control_set_midi_follow(c, argv[1]) < 0)
			return fail(c, "map follow");
		return 0;
	}
	if (!strcmp(sub, "status")) {
		bl_control_status s;
		if (bl_control_get_status(c, &s) < 0)
			return fail(c, "map status");
		if (json)
			print_status_json(&s);
		else
			print_status_text(&s);
		return 0;
	}
	if (!strcmp(sub, "listen")) {
		if (argc < 4)
			return usage_map();
		bl_listen_options o = BL_LISTEN_OPTIONS_INIT;
		const char *allow[16], *params = NULL;
		static char secret[300];
		for (int i = 4; i < argc; i++) {
			const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
			if (!strcmp(a, "--allow") && v) { if (o.n_allow < 16) allow[o.n_allow++] = v; i++; }
			else if (!strcmp(a, "--secret") && v) { o.secret = v; i++; }
			else if (!strcmp(a, "--secret-file") && v) {
				int r = read_secret_file(v, secret, sizeof(secret));
				if (r < 0) {
					fprintf(stderr, "backline map listen: %s: %s\n", v, strerror(-r));
					return 1;
				}
				o.secret = secret;
				i++;
			}
			else if (!strcmp(a, "--insecure-udp")) o.insecure_udp = true;
			else if (!strcmp(a, "--max-rate") && v) { o.max_rate = (unsigned)atoi(v); i++; }
			else if (!strcmp(a, "--max-clients") && v) { o.max_clients = (unsigned)atoi(v); i++; }
			else if (!strcmp(a, "--max-size") && v) { o.max_size = (unsigned)atoi(v); i++; }
			else if (!strcmp(a, "--params") && v) { params = v; i++; }
			else {
				fprintf(stderr, "backline map listen: unknown option '%s'\n", a);
				return 2;
			}
		}
		o.allow = allow;
		if (bl_control_listen_params(c, argv[1], argv[2], (uint16_t)atoi(argv[3]), &o, params, id, sizeof(id)) < 0)
			return fail(c, "map listen");
		out_kv(json, "id", id, "listening on %s");
		return 0;
	}
	if (!strcmp(sub, "unlisten") && argc == 2) {
		if (bl_control_unlisten(c, argv[1]) < 0)
			return fail(c, "map unlisten");
		return 0;
	}
	return usage_map();
}
