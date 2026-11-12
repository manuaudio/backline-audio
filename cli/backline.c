/* backline: the command line for the Backline audio services.
 * SPDX-License-Identifier: MIT
 *
 * Every command takes --json and then prints the same facts as JSON on
 * stdout (one document per command; `host watch` prints one per line).
 * Exit status: 0 ok, 1 the service said no, 2 usage.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <json-c/json.h>

#include "backline/backline.h"
#include "services.h"

static bool g_json;
static bl_client *C;

static void usage(FILE *f)
{
	fputs("usage: backline [--json] COMMAND ...\n"
	      "\n"
	      "  host load PLUGIN [--id ID] [--plugin-id ID] [--format clap|vst3|lv2] [--name NODE] [--target NODE]\n"
	      "                   [--midi-follow REGEX] [--state FILE] [--memory-max SIZE]\n"
	      "                   [--cpu-quota PCT] [--autoconnect|--no-autoconnect] [--no-restart]\n"
	      "                   [--force]   (run it even if the registry grades it fail)\n"
	      "  host unload ID         stop an instance\n"
	      "  host restart ID        restart an instance (keeps its state)\n"
	      "  host list              all instances\n"
	      "  host show ID           one instance\n"
	      "  host watch [--count N] [--timeout SECS]   print Loaded/Crashed/Restarted/... signals\n"
	      "  host presets INSTANCE  the plugin's presets (id, name, location)\n"
	      "  host preset INSTANCE ID|NAME   load a preset\n"
	      "  host save-preset INSTANCE NAME|PATH   save the current state as a preset (VST3)\n"
	      "     PLUGIN: a .clap file, a .vst3 bundle, an LV2 URI or .lv2 bundle, or a name\n"
	      "\n"
	      "  params INSTANCE [FILTER]        parameters (name substring filter)\n"
	      "  get INSTANCE KEY                one parameter value\n"
	      "  set INSTANCE KEY VALUE          set a parameter (clamped; prints the applied value)\n"
	      "  note INSTANCE on|off KEY [VELOCITY] [--channel N]\n"
	      "  state save|load INSTANCE PATH   plugin state to or from a file\n"
	      "  counter INSTANCE NAME           Cycles, MidiEvents, StateSerial, StateSaved, ...\n"
	      "     INSTANCE: a host instance id, a worker's node name, or its D-Bus name\n"
	      "\n"
	      "  plugins list [--format F] [--kind K] [--grade G] [--text T]\n"
	      "  plugins show ID|NAME\n"
	      "  plugins scan [--validate]\n"
	      "\n"
	      "  rec arm NODE[:CH]... [opts]  arm the recorder daemon; rec record|stop|marker [NAME]|status\n"
	      "  rec takes|take TAKE    list takes, show one take's sidecar\n"
	      "  rec soundcheck TAKE [--once]|stop; rec input NODE live|playback   virtual soundcheck\n"
	      "  rec -s NODE ... ARGS   run the one-shot recorder (see backline rec -h)\n"
	      "  transport play|stop|locate POS|tempo BPM|meter N D|link on|off|status\n"
	      "  transport ltc|mtc off|generate|chase [--fps F] [--target NODE] [--source NODE]\n"
	      "  play load FILE [--at POS] [--gain DB] [--target NODE] [--id ID]   a file on the timeline\n"
	      "  play unload ID|gain ID DB|start ID POS|list|show ID\n"
	      "  map add SOURCE TARGET [--min X --max Y ...] | learn TARGET | list | remove ID | status\n"
	      "  map listen udp|tcp ADDRESS PORT [--allow CIDR] [--secret S] | unlisten ID | follow REGEX | save\n"
	      "     OSC and MIDI to any parameter (backline map -h; docs/api/Control1.md)\n"
	      "  surfaces               connected control surfaces (Mackie Control, USB or network) and their state\n"
	      "  surfaces add IP | remove IP   a network surface that isn't found by itself (docs/SURFACES.md)\n"
	      "  version\n",
	      f);
}

static int die_usage(void)
{
	usage(stderr);
	return 2;
}

static int failed(const char *what)
{
	if (g_json) {
		json_object *o = json_object_new_object();
		json_object_object_add(o, "error", json_object_new_string(bl_client_error(C)));
		puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
		json_object_put(o);
	}
	fprintf(stderr, "backline: %s: %s\n", what, bl_client_error(C));
	return 1;
}

static void print_json(json_object *o)
{
	puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(o);
}

static json_object *instance_json(const bl_instance *in)
{
	json_object *o = json_object_new_object();
#define S(k, v) json_object_object_add(o, k, json_object_new_string(v))
#define I(k, v) json_object_object_add(o, k, json_object_new_int64(v))
	S("id", in->id);
	S("plugin", in->plugin);
	S("plugin_path", in->plugin_path);
	S("plugin_id", in->plugin_id);
	S("node", in->node);
	S("dbus", in->dbus);
	S("target", in->target);
	S("midi_follow", in->midi_follow);
	S("state", in->state);
	I("pid", in->pid);
	I("osc_port", in->osc_port);
	I("restarts", in->restarts);
	I("last_signal", in->last_signal);
	I("last_exit_status", in->last_exit_status);
	json_object_object_add(o, "restart", json_object_new_boolean(in->restart));
	S("unit", in->unit);
	S("state_file", in->state_file);
	S("log_file", in->log_file);
	if (in->fail_reason[0])
		S("fail_reason", in->fail_reason);
#undef S
#undef I
	return o;
}

struct watch_state {
	long left;
};

static int watch_cb(const bl_event *e, void *u)
{
	struct watch_state *s = u;
	json_object *o = json_object_new_object();
	json_object_object_add(o, "event", json_object_new_string(e->name));
	json_object_object_add(o, "id", json_object_new_string(e->id ? e->id : ""));
	json_object_object_add(o, "monotonic_ns", json_object_new_uint64(e->monotonic_ns));
	if (e->type == BL_EVENT_CRASHED) {
		json_object_object_add(o, "signal", json_object_new_int(e->signal));
		json_object_object_add(o, "exit_status", json_object_new_int(e->exit_status));
	} else if (e->type == BL_EVENT_RESTARTED) {
		json_object_object_add(o, "pid", json_object_new_int64(e->pid));
		json_object_object_add(o, "restarts", json_object_new_int64(e->restarts));
	} else if (e->type == BL_EVENT_FAILED) {
		json_object_object_add(o, "reason", json_object_new_string(e->reason ? e->reason : ""));
	}
	if (g_json)
		puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	else if (e->type == BL_EVENT_CRASHED)
		printf("%s %s (%s %d)\n", e->name, e->id, e->signal ? "signal" : "exit status",
		       e->signal ? e->signal : e->exit_status);
	else
		printf("%s %s\n", e->name, e->id);
	fflush(stdout);
	json_object_put(o);
	return s->left > 0 && --s->left == 0;
}

/* ------------------------------------------------------------------- host */
static int cmd_host(int argc, char **argv)
{
	if (argc < 1)
		return die_usage();
	const char *sub = argv[0];
	if (!strcmp(sub, "load")) {
		static const struct option opts[] = {
			{ "id", required_argument, NULL, 'i' }, { "plugin-id", required_argument, NULL, 'p' },
			{ "name", required_argument, NULL, 'n' }, { "target", required_argument, NULL, 't' },
			{ "midi-follow", required_argument, NULL, 'm' }, { "state", required_argument, NULL, 's' },
			{ "memory-max", required_argument, NULL, 'M' }, { "cpu-quota", required_argument, NULL, 'Q' },
			{ "autoconnect", no_argument, NULL, 'a' }, { "no-autoconnect", no_argument, NULL, 'A' },
			{ "no-restart", no_argument, NULL, 'R' }, { "force", no_argument, NULL, 'F' },
			{ "format", required_argument, NULL, 'f' }, { 0 } };
		bl_load_options o = BL_LOAD_OPTIONS_INIT;
		const char *format = NULL;
		int c;
		optind = 1;
		while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
			switch (c) {
			case 'i': o.id = optarg; break;
			case 'p': o.plugin_id = optarg; break;
			case 'n': o.name = optarg; break;
			case 't': o.target = optarg; break;
			case 'm': o.midi_follow = optarg; break;
			case 's': o.state = optarg; break;
			case 'M': o.memory_max = optarg; break;
			case 'Q': o.cpu_quota = optarg; break;
			case 'a': o.autoconnect = 1; break;
			case 'A': o.autoconnect = 0; break;
			case 'R': o.restart = 0; break;
			case 'F': o.force = 1; break;
			case 'f': format = optarg; break;
			default: return die_usage();
			}
		}
		if (optind >= argc)
			return die_usage();
		char state_abs[PATH_MAX];
		if (o.state && realpath(o.state, state_abs))
			o.state = state_abs; /* hostd resolves paths in its own cwd */
		char id[64];
		if (bl_host_load_format(C, argv[optind], format, &o, id, sizeof(id)) < 0)
			return failed("host load");
		bl_instance in;
		if (bl_host_get(C, id, &in) < 0)
			return failed("host show");
		if (g_json)
			print_json(instance_json(&in));
		else
			printf("%s: %s as node %s (pid %u, OSC 127.0.0.1:%u)\n", in.id, in.plugin_id, in.node, in.pid,
			       in.osc_port);
		return 0;
	}
	if (!strcmp(sub, "unload") || !strcmp(sub, "restart")) {
		if (argc != 2)
			return die_usage();
		int r = !strcmp(sub, "unload") ? bl_host_unload(C, argv[1]) : bl_host_restart(C, argv[1]);
		if (r < 0)
			return failed(sub);
		if (g_json) {
			json_object *o = json_object_new_object();
			json_object_object_add(o, "id", json_object_new_string(argv[1]));
			json_object_object_add(o, sub[0] == 'u' ? "unloaded" : "restarted", json_object_new_boolean(1));
			print_json(o);
		}
		return 0;
	}
	if (!strcmp(sub, "list")) {
		bl_instance *v;
		size_t n;
		if (bl_host_list(C, &v, &n) < 0)
			return failed("host list");
		if (g_json) {
			json_object *a = json_object_new_array();
			for (size_t i = 0; i < n; i++)
				json_object_array_add(a, instance_json(&v[i]));
			print_json(a);
		} else {
			printf("%-20s %-10s %8s %6s %8s  %s\n", "ID", "STATE", "PID", "OSC", "RESTARTS", "NODE / PLUGIN");
			for (size_t i = 0; i < n; i++)
				printf("%-20s %-10s %8u %6u %8u  %s / %s\n", v[i].id, v[i].state, v[i].pid, v[i].osc_port,
				       v[i].restarts, v[i].node, v[i].plugin_id[0] ? v[i].plugin_id : v[i].plugin);
		}
		bl_free(v);
		return 0;
	}
	if (!strcmp(sub, "show")) {
		if (argc != 2)
			return die_usage();
		bl_instance in;
		if (bl_host_get(C, argv[1], &in) < 0)
			return failed("host show");
		json_object *o = instance_json(&in);
		if (g_json)
			print_json(o);
		else {
			json_object_object_foreach(o, k, v) printf("%-18s %s\n", k, json_object_get_string(v));
			json_object_put(o);
		}
		return 0;
	}
	if (!strcmp(sub, "presets")) {
		if (argc != 2)
			return die_usage();
		bl_preset *v;
		size_t n;
		if (bl_instance_presets(C, argv[1], &v, &n) < 0)
			return failed("host presets");
		if (g_json) {
			json_object *a = json_object_new_array();
			for (size_t i = 0; i < n; i++) {
				json_object *o = json_object_new_object();
				json_object_object_add(o, "id", json_object_new_string(v[i].id));
				json_object_object_add(o, "name", json_object_new_string(v[i].name));
				json_object_object_add(o, "location", json_object_new_string(v[i].location));
				json_object_array_add(a, o);
			}
			print_json(a);
		} else {
			for (size_t i = 0; i < n; i++)
				printf("%-32s %s\n", v[i].name, v[i].id);
		}
		bl_free(v);
		return 0;
	}
	if (!strcmp(sub, "preset") || !strcmp(sub, "save-preset")) {
		if (argc != 3)
			return die_usage();
		char path[PATH_MAX] = "";
		const char *arg = argv[2];
		char abs[PATH_MAX];
		if (strchr(arg, '/') && arg[0] != '/' && getcwd(abs, sizeof(abs)) &&
		    strlen(abs) + strlen(arg) + 2 < sizeof(abs)) { /* the worker resolves paths in its own cwd */
			strcat(abs, "/");
			strcat(abs, arg);
			arg = abs;
		}
		bool save = sub[0] == 's';
		int r = save ? bl_instance_save_preset(C, argv[1], arg, path, sizeof(path))
			     : bl_instance_load_preset(C, argv[1], arg);
		if (r < 0)
			return failed(save ? "host save-preset" : "host preset");
		if (g_json) {
			json_object *o = json_object_new_object();
			json_object_object_add(o, "instance", json_object_new_string(argv[1]));
			json_object_object_add(o, save ? "saved" : "loaded", json_object_new_string(save ? path : argv[2]));
			print_json(o);
		} else if (save) {
			printf("%s\n", path);
		}
		return 0;
	}
	if (!strcmp(sub, "watch")) {
		static const struct option opts[] = { { "count", required_argument, NULL, 'c' },
						      { "timeout", required_argument, NULL, 't' }, { 0 } };
		long count = -1;
		double timeout = -1;
		int c;
		optind = 1;
		while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
			if (c == 'c') count = atol(optarg);
			else if (c == 't') timeout = atof(optarg);
			else return die_usage();
		}
		struct watch_state st = { count };
		if (bl_host_watch(C, watch_cb, &st, timeout < 0 ? -1 : (int)(timeout * 1000)) < 0)
			return failed("host watch");
		return 0;
	}
	return die_usage();
}

/* -------------------------------------------------------------- instance */
static int cmd_params(int argc, char **argv)
{
	if (argc < 2)
		return die_usage();
	bl_param *v;
	size_t n;
	if (bl_instance_params(C, argv[1], &v, &n) < 0)
		return failed("params");
	const char *filter = argc > 2 ? argv[2] : NULL;
	json_object *a = json_object_new_array();
	for (size_t i = 0; i < n; i++) {
		char full[520];
		snprintf(full, sizeof(full), "%s%s%s", v[i].module, v[i].module[0] ? "/" : "", v[i].name);
		if (filter && !strcasestr(full, filter))
			continue;
		if (g_json) {
			json_object *o = json_object_new_object();
			json_object_object_add(o, "id", json_object_new_int64(v[i].id));
			json_object_object_add(o, "name", json_object_new_string(v[i].name));
			json_object_object_add(o, "module", json_object_new_string(v[i].module));
			json_object_object_add(o, "value", json_object_new_double(v[i].value));
			json_object_object_add(o, "min", json_object_new_double(v[i].min));
			json_object_object_add(o, "max", json_object_new_double(v[i].max));
			json_object_array_add(a, o);
		} else {
			printf("%10u  %-48s %12.6g  [%g, %g]\n", v[i].id, full, v[i].value, v[i].min, v[i].max);
		}
	}
	if (g_json)
		print_json(a);
	else
		json_object_put(a);
	bl_free(v);
	return 0;
}

static int print_value(const char *key, double v)
{
	if (g_json) {
		json_object *o = json_object_new_object();
		json_object_object_add(o, "key", json_object_new_string(key));
		json_object_object_add(o, "value", json_object_new_double(v));
		print_json(o);
	} else {
		printf("%.17g\n", v);
	}
	return 0;
}

static int cmd_get(int argc, char **argv)
{
	if (argc != 3)
		return die_usage();
	double v;
	if (bl_instance_get_param(C, argv[1], argv[2], &v) < 0)
		return failed("get");
	return print_value(argv[2], v);
}

static int cmd_set(int argc, char **argv)
{
	if (argc != 4)
		return die_usage();
	char *end;
	double v = strtod(argv[3], &end);
	if (*end)
		return die_usage();
	double applied;
	if (bl_instance_set_param(C, argv[1], argv[2], v, &applied) < 0)
		return failed("set");
	return print_value(argv[2], applied);
}

static int cmd_note(int argc, char **argv)
{
	int channel = 0;
	int k = 1;
	char *pos[4];
	int np = 0;
	for (; k < argc; k++) {
		if (!strcmp(argv[k], "--channel") && k + 1 < argc)
			channel = atoi(argv[++k]) - 1;
		else if (np < 4)
			pos[np++] = argv[k];
	}
	if (np < 3)
		return die_usage();
	bool on = !strcmp(pos[1], "on");
	if (!on && strcmp(pos[1], "off"))
		return die_usage();
	int key = atoi(pos[2]), vel = np > 3 ? atoi(pos[3]) : 100;
	if (bl_instance_note(C, pos[0], on, (uint8_t)(channel & 15), (uint8_t)(key & 127), (uint8_t)(vel & 127)) < 0)
		return failed("note");
	if (g_json) {
		json_object *o = json_object_new_object();
		json_object_object_add(o, "note", json_object_new_string(on ? "on" : "off"));
		json_object_object_add(o, "key", json_object_new_int(key));
		print_json(o);
	}
	return 0;
}

static int cmd_state(int argc, char **argv)
{
	if (argc != 4)
		return die_usage();
	char abs[PATH_MAX];
	const char *path = argv[3];
	if (!strcmp(argv[1], "load") && realpath(path, abs))
		path = abs;
	else if (path[0] != '/') {
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd)))
			snprintf(abs, sizeof(abs), "%s/%s", cwd, path), path = abs;
	}
	int r;
	if (!strcmp(argv[1], "save"))
		r = bl_instance_save_state(C, argv[2], path);
	else if (!strcmp(argv[1], "load"))
		r = bl_instance_load_state(C, argv[2], path);
	else
		return die_usage();
	if (r < 0)
		return failed("state");
	if (g_json) {
		json_object *o = json_object_new_object();
		json_object_object_add(o, argv[1], json_object_new_string(path));
		print_json(o);
	}
	return 0;
}

static int cmd_counter(int argc, char **argv)
{
	if (argc != 3)
		return die_usage();
	uint64_t v;
	if (bl_instance_counter(C, argv[1], argv[2], &v) < 0)
		return failed("counter");
	if (g_json) {
		json_object *o = json_object_new_object();
		json_object_object_add(o, argv[2], json_object_new_uint64(v));
		print_json(o);
	} else {
		printf("%llu\n", (unsigned long long)v);
	}
	return 0;
}

/* --------------------------------------------------------------- plugins */
static int plugins_fallback(int argc, char **argv);
static bool no_service(void);

static int print_plugins_table(const char *json)
{
	json_object *root = json_tokener_parse(json), *list;
	if (!root || !json_object_object_get_ex(root, "plugins", &list) || !json_object_is_type(list, json_type_array)) {
		puts(json);
		json_object_put(root);
		return 0;
	}
	size_t n = json_object_array_length(list);
	for (size_t i = 0; i < n; i++) {
		json_object *e = json_object_array_get_idx(list, i), *v;
#define F(k) (json_object_object_get_ex(e, k, &v) ? json_object_get_string(v) : "")
		char kinds[64] = "";
		json_object *ka;
		if (json_object_object_get_ex(e, "kinds", &ka) && json_object_is_type(ka, json_type_array))
			for (size_t k = 0; k < json_object_array_length(ka); k++)
				snprintf(kinds + strlen(kinds), sizeof(kinds) - strlen(kinds), "%s%s", k ? "," : "",
					 json_object_get_string(json_object_array_get_idx(ka, k)));
		const char *grade = F("grade");
		json_object *val;
		if (!grade[0] && json_object_object_get_ex(e, "validation", &val) &&
		    json_object_object_get_ex(val, "status", &v))
			grade = json_object_get_string(v);
		bool pref = json_object_object_get_ex(e, "preferred", &v) && json_object_get_boolean(v);
		printf("%c %-4s %-18s %-5s %-40.40s %.24s\n", pref ? '*' : ' ', F("format"), kinds, grade, F("name"),
		       F("vendor"));
#undef F
	}
	fprintf(stderr, "%zu plugins (* = preferred format of a product)\n", n);
	json_object_put(root);
	return 0;
}

static int cmd_plugins(int argc, char **argv)
{
	if (argc < 1)
		return die_usage();
	char *out = NULL;
	if (!strcmp(argv[0], "list")) {
		json_object *f = json_object_new_object();
		for (int k = 1; k + 1 < argc; k += 2) {
			const char *key = argv[k];
			if (!strcmp(key, "--format") || !strcmp(key, "--kind") || !strcmp(key, "--grade") ||
			    !strcmp(key, "--text") || !strcmp(key, "--id"))
				json_object_object_add(f, key + 2, json_object_new_string(argv[k + 1]));
			else {
				json_object_put(f);
				return die_usage();
			}
		}
		if ((argc - 1) % 2) {
			json_object_put(f);
			return die_usage();
		}
		int r = bl_plugins_query(C, json_object_to_json_string(f), &out);
		json_object_put(f);
		if (r < 0 && no_service())
			return plugins_fallback(argc, argv);
		if (r < 0)
			return failed("plugins list");
		if (g_json)
			puts(out);
		else
			print_plugins_table(out);
	} else if (!strcmp(argv[0], "show") && argc == 2) {
		if (bl_plugins_show(C, argv[1], &out) < 0)
			return no_service() ? plugins_fallback(argc, argv) : failed("plugins show");
		puts(out);
	} else if (!strcmp(argv[0], "scan")) {
		bool validate = argc > 1 && !strcmp(argv[1], "--validate");
		if (bl_plugins_scan(C, validate, &out) < 0)
			return no_service() ? plugins_fallback(argc, argv) : failed("plugins scan");
		puts(out);
	} else {
		return die_usage();
	}
	bl_free(out);
	return 0;
}

/* -------------------------------------------------------------- siblings */
/* Run another Backline binary: next to this one, in the build tree's
 * subdirectory, or from $PATH. Does not return on success. */
static int exec_sibling(const char *name, const char *subdir, char **argv)
{
	char self[PATH_MAX], path[PATH_MAX + 64];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	snprintf(path, sizeof(path), "%s", name);
	if (n > 0) {
		self[n] = 0;
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = 0;
			char cand[PATH_MAX + 64];
			snprintf(cand, sizeof(cand), "%s/%s", self, name);
			if (access(cand, X_OK) == 0)
				snprintf(path, sizeof(path), "%s", cand);
			else if (subdir) {
				snprintf(cand, sizeof(cand), "%s/%s/%s", self, subdir, name);
				if (access(cand, X_OK) == 0)
					snprintf(path, sizeof(path), "%s", cand);
			}
		}
	}
	argv[0] = path;
	fflush(stdout);
	execvp(path, argv);
	fprintf(stderr, "backline: %s: %s\n", path, strerror(errno));
	return 1;
}

static int cmd_rec(int argc, char **argv)
{
	/* backline rec arm|record|stop|... talks to backline-recorderd; anything
	 * else (-s NODE ...) runs the one-shot recorder */
	if (argc > 1 && cli_rec_is_service(argv[1])) {
		int r = C ? 0 : bl_client_open(&C);
		if (r < 0) {
			fprintf(stderr, "backline: session bus: %s\n", strerror(-r));
			return 1;
		}
		return cli_rec(C, g_json, argc - 1, argv + 1);
	}
	return exec_sibling("backline-recorder", NULL, argv);
}

/* No registry service on the bus: ask backline-registry directly (it reads
 * the same index file). */
static int plugins_fallback(int argc, char **argv)
{
	char *args[argc + 3];
	int n = 0;
	args[n++] = (char *)"backline-registry";
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--validate") || !strcmp(argv[i], "--id"))
			continue; /* scan validates by default; --id is a Query-only filter */
		args[n++] = argv[i];
	}
	if (g_json)
		args[n++] = (char *)"--json";
	args[n] = NULL;
	return exec_sibling("backline-registry", "registry", args);
}

static bool no_service(void)
{
	const char *e = bl_client_error(C);
	return strstr(e, "not provided by any .service") || strstr(e, "ServiceUnknown") || strstr(e, "has no owner") ||
	       strstr(e, "was not provided") || strstr(e, "not activatable");
}

int main(int argc, char **argv)
{
	int k = 1;
	for (; k < argc && argv[k][0] == '-'; k++) {
		if (!strcmp(argv[k], "--json"))
			g_json = true;
		else if (!strcmp(argv[k], "-h") || !strcmp(argv[k], "--help")) {
			usage(stdout);
			return 0;
		} else
			return die_usage();
	}
	if (k >= argc)
		return die_usage();
	/* --json is also accepted after the command */
	int n = 0;
	char *args[argc + 1];
	for (int i = k; i < argc; i++) {
		if (!strcmp(argv[i], "--json"))
			g_json = true;
		else
			args[n++] = argv[i];
	}
	args[n] = NULL;
	const char *cmd = args[0];
	if (!strcmp(cmd, "version")) {
		if (g_json)
			printf("{\"backline\": \"%s\"}\n", bl_version());
		else
			printf("backline %s\n", bl_version());
		return 0;
	}
	if (!strcmp(cmd, "rec"))
		return cmd_rec(n, args);
	int r = bl_client_open(&C);
	if (r < 0) {
		fprintf(stderr, "backline: session bus: %s\n", strerror(-r));
		return 1;
	}
	if (!strcmp(cmd, "host"))
		r = cmd_host(n - 1, args + 1);
	else if (!strcmp(cmd, "transport"))
		r = cli_transport(C, g_json, n - 1, args + 1);
	else if (!strcmp(cmd, "play"))
		r = cli_play(C, g_json, n - 1, args + 1);
	else if (!strcmp(cmd, "map"))
		r = cli_map(C, g_json, n - 1, args + 1);
	else if (!strcmp(cmd, "surfaces"))
		r = cli_surfaces(C, g_json, n - 1, args + 1);
	else if (!strcmp(cmd, "params"))
		r = cmd_params(n, args);
	else if (!strcmp(cmd, "get"))
		r = cmd_get(n, args);
	else if (!strcmp(cmd, "set"))
		r = cmd_set(n, args);
	else if (!strcmp(cmd, "note"))
		r = cmd_note(n, args);
	else if (!strcmp(cmd, "state"))
		r = cmd_state(n, args);
	else if (!strcmp(cmd, "counter"))
		r = cmd_counter(n, args);
	else if (!strcmp(cmd, "plugins"))
		r = cmd_plugins(n - 1, args + 1);
	else
		r = die_usage();
	bl_client_close(C);
	return r;
}
