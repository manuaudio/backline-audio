/* Surface profiles: *.surface.json (docs/api/Surface1.md, "Profiles").
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_SURFACE_PROFILE_H
#define BACKLINE_SURFACE_PROFILE_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF_MAX_MATCH 8
#define SF_MAX_BINDINGS 96

enum sf_handshake { SF_HS_MACKIE, SF_HS_LOGIC, SF_HS_NONE };

struct sf_binding_default {
	char control[48];      /* with modifiers: "shift+bank-left" */
	char binding[96];
};

struct sf_profile {
	char id[64];
	char name[96];
	char file[512];
	char driver[16];
	bool master;           /* unit: master (globals) or extender */
	int strips;
	bool master_fader;
	uint8_t model;
	char match_src[SF_MAX_MATCH][128], exclude_src[SF_MAX_MATCH][128];
	regex_t match[SF_MAX_MATCH], exclude[SF_MAX_MATCH];
	int n_match, n_exclude;
	enum sf_handshake handshake;
	bool lcd, colour, assignment, timecode, meters, touch_sense, motor;
	uint32_t bytes_per_second;
	uint32_t send_delay_us;
	bool no_flash, assignment_swapped;
	char labels[128][32];  /* by MCU note id: the printed label ("" = the MCU's) */
	struct sf_binding_default bindings[SF_MAX_BINDINGS];
	int n_bindings;
};

/* Parses one profile file. 0, or -1 with a sentence in err. */
int sf_profile_load(const char *path, struct sf_profile *p, char *err, size_t n);
/* Parses from a JSON string (tests). */
int sf_profile_parse(const char *json, const char *origin, struct sf_profile *p, char *err, size_t n);
void sf_profile_free(struct sf_profile *p);
/* Does a port ("node:port | alias") belong to this profile? */
bool sf_profile_matches(const struct sf_profile *p, const char *port);
/* The default binding of a control (with modifiers), or NULL. */
const char *sf_profile_binding(const struct sf_profile *p, const char *control);
/* A JSON description (json_object *) for Profiles(). */
void *sf_profile_to_json(const struct sf_profile *p);

#ifdef __cplusplus
}
#endif
#endif
