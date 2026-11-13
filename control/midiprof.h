/* MIDI learn profiles: a device's controls under abstract names, and what
 * the user learned for them (docs/api/Control1.md "MIDI profiles"). Pure:
 * no PipeWire, no bus; controld does the glue. Unit-tested in
 * tests/unit/test_midiprof.c.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_CTL_MIDIPROF_H
#define BACKLINE_CTL_MIDIPROF_H

#include <json-c/json.h>
#include <regex.h>
#include <stdbool.h>
#include <stddef.h>

#include "mapping.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SCHEMA "backline.midi-profile/1"
/* The directory name under $XDG_CONFIG_HOME/<product>/ and the datadir. */
#define MP_DIR_NAME "midi-profiles"
#define MP_FILE_SUFFIX ".json"
#define MP_MAX_CONTROLS 512
#define MP_NAME_MAX 64

/* One physical control, in its factory mode. */
struct mp_control {
	char name[MP_NAME_MAX];     /* abstract: fader.1, knob.a.1, button.mute.1, transport.play */
	char source[160];           /* a Control1 MIDI source, canonical (midi:cc:1:0) */
	struct ctl_source src;
	char kind[16];              /* fader, knob, encoder, button, pad, key, wheel, other ("" = not given) */
	char label[64];             /* what is printed on the device, if useful */
	bool has_encoding, has_bits, has_feedback;
	enum ctl_encoding enc;      /* what the control sends (relative encoders) */
	int bits;                   /* NRPN/RPN data: 7 or 14 */
	bool feedback;              /* bindings get feedback by default (the control lights or moves) */
};

struct mp_profile {
	char id[MP_NAME_MAX];       /* the file's stem; what the API takes */
	char name[128];
	char vendor[64], model[64], mode[128];
	char match[256];            /* POSIX ERE, case-insensitive, on "node:port | alias" of the device's MIDI ports */
	regex_t re;
	bool re_ok;
	bool verified;              /* tried on the hardware */
	char note[512];
	bool autoload;              /* load the bindings when the device appears (key "auto", default true) */
	int n_controls;
	struct mp_control *controls;
	json_object *sources;       /* array of strings: the documents it was built from (owned; may be NULL) */
	json_object *bindings;      /* array of {control, target, options...} (owned, never NULL after parse) */
	/* where it came from (not in the file) */
	char path[700];
	bool system;
};

/* Ids and control names: [a-z0-9][a-z0-9._-]*, at most 63 characters. */
bool mp_name_valid(const char *s);

/* Parses and validates. id: the file stem (used when the object has no
 * "id"). 0, or -1 with a sentence in err. On failure p is left freed. */
int mp_parse(json_object *o, const char *id, struct mp_profile *p, char *err, size_t n);
int mp_load_file(const char *path, struct mp_profile *p, char *err, size_t n);
void mp_free(struct mp_profile *p);
/* A new empty profile (no controls, no bindings) for SaveProfile of a new id. */
int mp_init(struct mp_profile *p, const char *id, const char *name, const char *match, char *err, size_t n);
/* The file's JSON (new reference). */
json_object *mp_to_json(const struct mp_profile *p);
/* Written atomically (temp file, fsync, rename), mode 0644; the directory
 * is created. */
int mp_save_file(const struct mp_profile *p, const char *path, char *err, size_t n);

bool mp_matches_device(const struct mp_profile *p, const char *hay);
const struct mp_control *mp_find_control(const struct mp_profile *p, const char *name);
/* The control a MIDI source is (same kind, channel and number; a control on
 * channel * matches any channel). NULL: not one of this device's. */
const struct mp_control *mp_control_for_source(const struct mp_profile *p, const struct ctl_source *s);

/* Binding i as a Control1 mapping object ({"id", "source", "target",
 * options}), with the control's defaults (encoding, bits, feedback) where
 * the binding doesn't say. id: "<profile>.<control>", and ".2", ".3" for a
 * control's further bindings. New reference, or NULL with err. */
json_object *mp_binding_mapping(const struct mp_profile *p, size_t i, char *err, size_t n);

/* Replaces the bindings with the given Control1 mapping objects (the
 * Mappings() schema). A mapping whose source is not one of the profile's
 * controls is skipped, unless add_controls: then a control is made for it,
 * named from its source (cc.1.7, note.10.36, ...). OSC mappings are always
 * skipped. Returns the number bound (and *skipped), or -1 with err. */
int mp_bind_mappings(struct mp_profile *p, json_object *mappings, bool add_controls, int *skipped, char *err, size_t n);

/* A set of profiles from several directories, earlier directories first: a
 * profile id in an earlier directory hides the same id in later ones (the
 * user's copy over the system's). Files that do not validate are skipped
 * and reported through warn (may be NULL). */
struct mp_set {
	struct mp_profile **p;
	int n;
};
typedef void (*mp_warn_fn)(void *ud, const char *path, const char *why);
int mp_set_load(struct mp_set *s, const char *const *dirs, int n_dirs, const bool *system, mp_warn_fn warn, void *ud);
struct mp_profile *mp_set_find(const struct mp_set *s, const char *id);
void mp_set_free(struct mp_set *s);

#ifdef __cplusplus
}
#endif
#endif
