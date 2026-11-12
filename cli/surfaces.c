/* backline CLI: control surfaces (backline surfaces ...). docs/api/Surface1.md.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "backline/backline.h"
#include "services.h"

static int usage_surfaces(void)
{
	fputs("usage: backline surfaces                 what is connected, and how\n"
	      "       backline surfaces add ADDRESS     a network surface at IP[:PORT] (when it isn't found by itself)\n"
	      "       backline surfaces remove ADDRESS\n"
	      "       backline surfaces activate|standby   a standby engine holds its sessions and drives nothing\n", stderr);
	return 2;
}

static const char *str(json_object *o, const char *k)
{
	json_object *v;
	return json_object_object_get_ex(o, k, &v) && json_object_get_type(v) != json_type_null ? json_object_get_string(v) : "";
}

static int num(json_object *o, const char *k, int def)
{
	json_object *v;
	return json_object_object_get_ex(o, k, &v) ? json_object_get_int(v) : def;
}

static int fail(bl_client *c, const char *what)
{
	const char *e = bl_client_error(c);
	if (strstr(e, "ServiceUnknown") || strstr(e, "not provided") || strstr(e, "was not provided"))
		fprintf(stderr, "backline: the surface service isn't running (systemctl --user start backline-surfaced)\n");
	else
		fprintf(stderr, "backline: %s: %s\n", what, e);
	return 1;
}

static const char *peer_note(const char *state)
{
	if (!strcmp(state, "refused"))
		return "the device refused: its session is taken (it may allow one host only); asked again every 5 s";
	if (!strcmp(state, "inviting") || !strcmp(state, "lost"))
		return "no answer yet: on, cabled, in MC mode with Interface Network?";
	if (!strcmp(state, "open"))
		return "connected, but its name matches no surface profile";
	return state;
}

static int list(bl_client *c, bool as_json)
{
	char *uj = NULL, *pj = NULL;
	bool active = true, params = false;
	uint16_t port = 0;
	if (bl_surface_units(c, &uj) < 0)
		return fail(c, "Units");
	if (bl_surface_peers(c, &pj) < 0 || bl_surface_status(c, &active, &port, &params) < 0) {
		bl_free(uj);
		return fail(c, "Peers");
	}
	json_object *units = json_tokener_parse(uj), *peers = json_tokener_parse(pj);
	bl_free(uj);
	bl_free(pj);
	if (!units)
		units = json_object_new_array();
	if (!peers)
		peers = json_object_new_array();
	if (as_json) {
		json_object *o = json_object_new_object();
		json_object_object_add(o, "active", json_object_new_boolean(active));
		json_object_object_add(o, "params-connected", json_object_new_boolean(params));
		json_object_object_add(o, "network-port", json_object_new_int(port));
		json_object_object_add(o, "units", json_object_get(units));
		json_object_object_add(o, "peers", json_object_get(peers));
		puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
		json_object_put(o);
	} else {
		size_t nu = json_object_array_length(units);
		if (!active)
			puts("STANDBY: network sessions are held open, nothing is driven (backline surfaces activate)");
		if (!nu) {
			puts("no surfaces. Plug one in by USB in Mackie Control (MC) mode, or see docs/SURFACES.md for the network.");
		} else {
			printf("%-30s %-30s %-24s %-9s %-6s %s\n", "UNIT", "MODEL", "VIA", "STATE", "GROUP", "TOUCHED");
			for (size_t i = 0; i < nu; i++) {
				json_object *u = json_object_array_get_idx(units, i);
				char via[96], grp[48];
				const char *t = str(u, "transport");
				if (!strcmp(t, "network"))
					snprintf(via, sizeof(via), "network %s", str(u, "address"));
				else
					snprintf(via, sizeof(via), "%s", *t ? "usb/midi" : "-");
				int pos = num(u, "position", -1);
				if (*str(u, "group"))
					snprintf(grp, sizeof(grp), "%s/%d", str(u, "group"), pos + 1);
				else
					snprintf(grp, sizeof(grp), "-");
				printf("%-30s %-30s %-24s %-9s %-6s %d\n", str(u, "id"), *str(u, "name") ? str(u, "name") : "-", via,
				       str(u, "state"), grp, num(u, "touched", 0));
			}
		}
		bool head = false;
		for (size_t i = 0; i < json_object_array_length(peers); i++) {
			json_object *p = json_object_array_get_idx(peers, i);
			if (*str(p, "unit"))
				continue;          /* listed above */
			if (!head)
				puts("\nnetwork devices:");
			head = true;
			const char *st = str(p, "state");
			printf("  %-24s %-22s %s\n", str(p, "name"), str(p, "address"),
			       !active && !strcmp(st, "open") ? "session held (standby)" : peer_note(st));
		}
		if (!params)
			puts("\n(no program has published parameters yet: the surfaces show and move nothing until one does)");
	}
	json_object_put(units);
	json_object_put(peers);
	return 0;
}

int cli_surfaces(bl_client *c, bool json, int argc, char **argv)
{
	if (argc == 0 || !strcmp(argv[0], "list"))
		return list(c, json);
	if (!strcmp(argv[0], "-h") || !strcmp(argv[0], "--help")) {
		usage_surfaces();
		return 0;
	}
	if ((!strcmp(argv[0], "add") || !strcmp(argv[0], "remove")) && argc == 2) {
		bool add = !strcmp(argv[0], "add");
		if ((add ? bl_surface_add_network(c, argv[1]) : bl_surface_remove_network(c, argv[1])) < 0)
			return fail(c, argv[0]);
		if (!json)
			printf(add ? "inviting %s; `backline surfaces` shows when it answers\n" : "removed %s\n", argv[1]);
		return 0;
	}
	if ((!strcmp(argv[0], "activate") || !strcmp(argv[0], "standby")) && argc == 1) {
		if (bl_surface_set_active(c, !strcmp(argv[0], "activate")) < 0)
			return fail(c, argv[0]);
		return 0;
	}
	return usage_surfaces();
}
