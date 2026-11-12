/* The parameter tree of Params1 (docs/api/Params1.md): paths, patterns,
 * parameter descriptions, tapers, value text, roles, and a store. Pure (no
 * PipeWire, no D-Bus), shared by controld and surfaced, unit-tested.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_PARAMS_H
#define BACKLINE_PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRM_PATH_MAX 256      /* bytes, NUL included */
#define PRM_MAX_DEPTH 8       /* segments, root included */
#define PRM_SEG_MAX 64
#define PRM_NAME_MAX 65
#define PRM_SHORT_MAX 8
#define PRM_UNIT_MAX 9
#define PRM_FORMAT_MAX 32
#define PRM_TEXT_MAX 65
#define PRM_STRING_MAX 257
#define PRM_MAX_ENUM 64
#define PRM_DB_INF -120.0     /* a db-fader min at or below this is -inf */

enum prm_type { PRM_FLOAT, PRM_INT, PRM_BOOL, PRM_ENUM, PRM_STRING, PRM_TRIGGER };
enum prm_taper { PRM_LINEAR, PRM_LOG, PRM_DB_FADER };

struct prm {
	char path[PRM_PATH_MAX];
	enum prm_type type;
	enum prm_taper taper;
	char name[PRM_NAME_MAX];
	char short_name[PRM_SHORT_MAX];
	char unit[PRM_UNIT_MAX];
	char format[PRM_FORMAT_MAX];
	double min, max, def, step;
	char def_str[PRM_STRING_MAX];
	char *labels;          /* enum/bool labels, NUL-separated, malloc'd (or NULL) */
	int n_labels;
	bool automatable, read_only;
	/* the current value */
	double value;          /* numeric types */
	char str[PRM_STRING_MAX]; /* string type */
	char text[PRM_TEXT_MAX];  /* the display text (formatted, or the program's) */
	bool text_override;
	/* owner bookkeeping (opaque to this module) */
	int root;
	uint64_t dirty;        /* per-subscriber bits (controld) */
	uint32_t slot;         /* index in the store */
};

/* "/console/in.1/fader": a valid path (segments, depth, reserved suffixes).
 * root_only: one segment (a root). */
bool prm_path_valid(const char *path, bool root_only);
/* The root segment of a path ("/console"), into out. false if none. */
bool prm_path_root(const char *path, char *out, size_t n);

/* OSC-style patterns: '*' within a segment, '?' one character, '**' any
 * number of whole segments. */
bool prm_match(const char *pattern, const char *path);

/* Value text: one %f %g %d %i conversion with flags/width/precision and
 * literal text. */
bool prm_format_valid(const char *fmt);

const char *prm_type_name(enum prm_type t);
const char *prm_taper_name(enum prm_taper t);

/* Clamp/round a numeric value to the parameter's type and range. */
double prm_clamp(const struct prm *p, double v);
double prm_to_norm(const struct prm *p, double v);
double prm_from_norm(const struct prm *p, double norm);
/* The display text of a value (format, unit, labels, -inf). */
void prm_format_value(const struct prm *p, double v, char *out, size_t n);
/* The current text (override or formatted). */
void prm_refresh_text(struct prm *p);
/* An enum label's index, or -1. */
int prm_label_index(const struct prm *p, const char *label);
const char *prm_label(const struct prm *p, int i);

/* Parses one parameter object (docs/api/Params1.md) under root. The json is a
 * json_object * (json-c), passed as void * to keep json-c out of this
 * header. 0, or -1 with a sentence in err. *out is malloc'd; free with
 * prm_free. */
int prm_from_json(const void *json_object, const char *root, struct prm **out, char *err, size_t n);
void prm_free(struct prm *p);
/* A JSON object (json_object *) describing p and its value. */
void *prm_to_json(const struct prm *p);

/* The store: an open-addressing hash of paths. Slots are stable while a
 * parameter exists. */
struct prm_store;
struct prm_store *prm_store_new(void);
void prm_store_free(struct prm_store *s);
/* Takes ownership; an existing path is replaced (returns the new entry). */
struct prm *prm_store_put(struct prm_store *s, struct prm *p);
struct prm *prm_store_get(struct prm_store *s, const char *path);
struct prm *prm_store_slot(struct prm_store *s, uint32_t slot);
bool prm_store_remove(struct prm_store *s, const char *path);
/* Removes every parameter matching pattern (or of root, if root >= 0 and
 * pattern is NULL). Returns the count. */
int prm_store_remove_matching(struct prm_store *s, const char *pattern, int root);
size_t prm_store_count(const struct prm_store *s);
uint32_t prm_store_capacity(const struct prm_store *s);
/* Iterates in slot order; stops when cb returns non-zero. */
int prm_store_each(struct prm_store *s, int (*cb)(void *data, struct prm *p), void *data);

/* Roles: name -> write patterns. */
#define PRM_MAX_ROLES 16
#define PRM_MAX_ROLE_PATTERNS 32
struct prm_role {
	char name[32];
	char patterns[PRM_MAX_ROLE_PATTERNS][PRM_PATH_MAX];
	int n;
};
/* May a client in role write p? roles: the root's program-defined roles. */
bool prm_role_may_write(const struct prm_role *roles, int n_roles, const char *role, const struct prm *p,
			char *why, size_t n);
bool prm_role_name_valid(const char *role);

#ifdef __cplusplus
}
#endif
#endif
