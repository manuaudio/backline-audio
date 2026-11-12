/* backline CLI: the player (play ...).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "backline/backline.h"
#include "services.h"

static int fail(bl_client *c, bool json, const char *what)
{
	if (json)
		printf("{\"error\": %s}\n", json_object_to_json_string(json_object_new_string(bl_client_error(c))));
	fprintf(stderr, "backline: %s: %s\n", what, bl_client_error(c));
	return 1;
}

static void print_clip(json_object *o)
{
	json_object *v;
#define S(k) (json_object_object_get_ex(o, k, &v) ? json_object_get_string(v) : "")
#define I(k) (json_object_object_get_ex(o, k, &v) ? json_object_get_int64(v) : 0)
	json_object *ch = NULL;
	json_object_object_get_ex(o, "channels", &ch);
	printf("%-16s start %-10" PRId64 " end %-10" PRId64 " %s ch %s Hz  gain %s dB  %s%s%s\n", S("id"), I("start"), I("end"),
	       ch ? json_object_to_json_string_ext(ch, JSON_C_TO_STRING_PLAIN) : "", S("rate"), S("gain_db"),
	       S("node"), *S("target") ? " -> " : "", S("target"));
	if (json_object_object_get_ex(o, "rate_ok", &v) && !json_object_get_boolean(v))
		printf("%-16s silent: the file is %s Hz, the graph %s Hz (resampling is not built yet)\n", "", S("rate"), S("graph_rate"));
#undef S
#undef I
}

int cli_play(bl_client *c, bool json, int argc, char **argv)
{
	if (argc < 1) {
		fputs("usage: backline play load FILE [--at POS] [--gain DB] [--target NODE] [--id ID] [--no-autoconnect]\n"
		      "       backline play unload ID | gain ID DB | start ID POS | list | show ID\n"
		      "  POS: frames, tc:HH:MM:SS:FF or bbt:BAR|BEAT|TICK (on the clock's timeline)\n", stderr);
		return 2;
	}
	const char *sub = argv[0];
	if (!strcmp(sub, "load") && argc >= 2) {
		bl_play_options o = BL_PLAY_OPTIONS_INIT;
		char path[PATH_MAX];
		if (!realpath(argv[1], path)) {
			fprintf(stderr, "backline: %s: no such file\n", argv[1]);
			return 1;
		}
		for (int i = 2; i < argc; i++) {
			const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
			if (!strcmp(a, "--at") && v) o.at = argv[++i];
			else if (!strcmp(a, "--gain") && v) o.gain_db = atof(argv[++i]);
			else if (!strcmp(a, "--target") && v) o.target = argv[++i];
			else if (!strcmp(a, "--id") && v) o.id = argv[++i];
			else if (!strcmp(a, "--no-autoconnect")) o.autoconnect = 0;
			else { fprintf(stderr, "backline: play load: unknown option %s\n", a); return 2; }
		}
		char id[128];
		if (bl_player_load(c, path, &o, id, sizeof(id)) < 0)
			return fail(c, json, "play load");
		if (json)
			printf("{\"id\": \"%s\"}\n", id);
		else
			printf("%s\n", id);
		return 0;
	}
	if (!strcmp(sub, "unload") && argc == 2) {
		if (bl_player_unload(c, argv[1]) < 0)
			return fail(c, json, "play unload");
		return 0;
	}
	if (!strcmp(sub, "gain") && argc == 3) {
		if (bl_player_set_gain(c, argv[1], atof(argv[2])) < 0)
			return fail(c, json, "play gain");
		return 0;
	}
	if (!strcmp(sub, "start") && argc == 3) {
		uint64_t f;
		if (bl_player_set_start(c, argv[1], argv[2], &f) < 0)
			return fail(c, json, "play start");
		if (json)
			printf("{\"frame\": %" PRIu64 "}\n", f);
		else
			printf("%" PRIu64 "\n", f);
		return 0;
	}
	if ((!strcmp(sub, "list") && argc == 1) || (!strcmp(sub, "show") && argc == 2)) {
		char *s = NULL;
		int r = argc == 1 ? bl_player_list(c, &s) : bl_player_get(c, argv[1], &s);
		if (r < 0)
			return fail(c, json, argc == 1 ? "play list" : "play show");
		json_object *o = json_tokener_parse(s);
		if (json || !o) {
			puts(o ? json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE) : s);
		} else if (argc == 1) {
			json_object *a;
			if (json_object_object_get_ex(o, "clips", &a)) {
				size_t n = json_object_array_length(a);
				if (!n)
					puts("no clips loaded");
				for (size_t i = 0; i < n; i++)
					print_clip(json_object_array_get_idx(a, i));
			}
		} else {
			print_clip(o);
		}
		json_object_put(o);
		bl_free(s);
		return 0;
	}
	fprintf(stderr, "backline: play: unknown command or arguments (see backline play)\n");
	return 2;
}
