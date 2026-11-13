/* Unit tests for MIDI learn profiles (control/midiprof.c) and every profile
 * shipped in data/midi-profiles. SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "midiprof.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *NOTE = "built from the published chart, not tried on hardware";

static int parse_str(const char *js, struct mp_profile *p, char *err, size_t n)
{
	json_object *o = json_tokener_parse(js);
	int r = mp_parse(o, "t", p, err, n);
	if (o)
		json_object_put(o);
	return r;
}

#define HEAD "{\"schema\":\"backline.midi-profile/1\",\"name\":\"T\",\"match\":\"^Test\",\"verified\":false,"

static void expect_error(const char *js, const char *needle)
{
	struct mp_profile p;
	char err[400] = "";
	int r = parse_str(js, &p, err, sizeof(err));
	if (r == 0 || !strstr(err, needle)) {
		fprintf(stderr, "expected an error with '%s' for %s, got %d '%s'\n", needle, js, r, err);
		fails++;
	}
	if (r == 0)
		mp_free(&p);
}

static void test_validation(void)
{
	struct mp_profile p;
	char err[400];
	CHECK(parse_str(HEAD "\"controls\":[{\"name\":\"fader.1\",\"source\":\"midi:cc:1:7\",\"kind\":\"fader\"}]}", &p, err,
			sizeof(err)) == 0);
	CHECK(p.n_controls == 1 && !strcmp(p.controls[0].source, "midi:cc:1:7") && p.autoload && !p.verified);
	CHECK(json_object_array_length(p.bindings) == 0);
	mp_free(&p);

	expect_error("{\"schema\":\"backline.control/1\"}", "not a backline.midi-profile/1");
	expect_error("{\"schema\":\"backline.midi-profile/1\",\"name\":\"T\",\"match\":\"x\",\"controls\":[]}", "verified");
	expect_error("{\"schema\":\"backline.midi-profile/1\",\"name\":\"T\",\"verified\":true,\"controls\":[]}", "match");
	expect_error("{\"schema\":\"backline.midi-profile/1\",\"name\":\"T\",\"match\":\"(\",\"verified\":true,\"controls\":[]}",
		     "bad regular expression");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:7\"},{\"name\":\"a\",\"source\":\"midi:cc:1:8\"}]}",
		     "two controls are named a");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:7\"},{\"name\":\"b\",\"source\":\"midi:cc:*:7\"}]}",
		     "send the same message");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc14:1:1\"},{\"name\":\"b\",\"source\":\"midi:cc:1:33\"}]}",
		     "send the same message");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:nrpn:1:300\"},{\"name\":\"b\",\"source\":\"midi:cc:1:6\"}]}",
		     "send the same message");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"osc:/x\"}]}", "sources are MIDI");
	expect_error(HEAD "\"controls\":[{\"name\":\"A\",\"source\":\"midi:cc:1:1\"}]}", "is not a name");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:note:1:1\",\"encoding\":\"rel-2c\"}]}", "relative encoding");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:1\",\"kind\":\"slider\"}]}", "kind is");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:1\",\"bits\":7}]}", "bits is for");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:1\"}],\"bindings\":[{\"control\":\"b\",\"target\":\"transport:play\"}]}",
		     "not one of this profile's");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:1\"}],\"bindings\":[{\"control\":\"a\",\"source\":\"midi:cc:1:1\",\"target\":\"transport:play\"}]}",
		     "the control has the source");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:1\"}],\"bindings\":[{\"control\":\"a\",\"target\":\"nowhere:x\"}]}",
		     "binding 0 (a)");
	expect_error(HEAD "\"controls\":[{\"name\":\"a\",\"source\":\"midi:cc:1:1\"}],\"bindings\":[{\"control\":\"a\",\"target\":\"transport:play\",\"takeover\":\"grab\"}]}",
		     "takeover is");
	CHECK(mp_name_valid("knob.a.1") && mp_name_valid("akai-midimix") && !mp_name_valid("") && !mp_name_valid(".x") &&
	      !mp_name_valid("a/b") && !mp_name_valid("Ab"));
}

static const char *SRCDIR = BL_MIDIPROF_SRCDIR;

/* Every shipped profile: valid, marked unverified with the note, cites a
 * source, and matches the port names its device shows (PipeWire's MIDI
 * bridge: "node:port | alias"). */
static void test_shipped(void)
{
	struct { const char *id; int controls; const char *port; const char *probe_src, *probe_ctl; } want[] = {
		{ "korg-nanokontrol2", 51, "Midi-Bridge:nanoKONTROL2 0:(capture_0) nanoKONTROL2 _ CTRL | nanoKONTROL2:nanoKONTROL2 _ CTRL",
		  "midi:cc:1:41", "transport.play" },
		{ "akai-midimix", 59, "Midi-Bridge:MIDI Mix 0:(capture_0) MIDI Mix MIDI 1 | MIDI Mix:MIDI Mix MIDI 1",
		  "midi:cc:1:62", "fader.master" },
		{ "novation-launch-control-xl", 56, "Midi-Bridge:Launch Control XL 0:(capture_0) Launch Control XL MIDI 1 | Launch Control XL",
		  "midi:cc:9:77", "fader.1" },
	};
	const char *dirs[] = { SRCDIR };
	bool sys[] = { true };
	struct mp_set s = { 0 };
	int n = mp_set_load(&s, dirs, 1, sys, NULL, NULL);
	/* every file in the directory loaded */
	DIR *d = opendir(SRCDIR);
	int files = 0;
	for (struct dirent *e; d && (e = readdir(d));)
		if (strstr(e->d_name, ".json"))
			files++;
	if (d)
		closedir(d);
	CHECK(n == files && n >= 3);
	for (int i = 0; i < s.n; i++) {
		struct mp_profile *p = s.p[i];
		CHECK(p->system);
		CHECK(!p->verified);  /* nothing shipped is tried on hardware yet */
		CHECK(strstr(p->note, NOTE) != NULL);
		CHECK(p->sources && json_object_array_length(p->sources) >= 1);
		CHECK(p->vendor[0] && p->model[0] && p->mode[0]);
		for (int k = 0; k < p->n_controls; k++)
			CHECK(p->controls[k].kind[0]);
		/* round trip: the file's own JSON again */
		json_object *a = mp_to_json(p), *b = json_object_from_file(p->path);
		CHECK(a && b && json_object_equal(a, b));
		json_object_put(a);
		json_object_put(b);
	}
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		struct mp_profile *p = mp_set_find(&s, want[i].id);
		CHECK(p != NULL);
		if (!p)
			continue;
		CHECK(p->n_controls == want[i].controls);
		CHECK(mp_matches_device(p, want[i].port));
		for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++)
			if (k != i)
				CHECK(!mp_matches_device(p, want[k].port));
		CHECK(!mp_matches_device(p, "Midi-Bridge:Midi Through 0:(capture_0) Midi Through Port-0"));
		struct ctl_source src;
		char err[200];
		ctl_parse_source(want[i].probe_src, &src, err, sizeof(err));
		const struct mp_control *c = mp_control_for_source(p, &src);
		CHECK(c && !strcmp(c->name, want[i].probe_ctl));
	}
	/* spot checks against the charts */
	struct mp_profile *nk = mp_set_find(&s, "korg-nanokontrol2"), *mm = mp_set_find(&s, "akai-midimix"),
			  *lc = mp_set_find(&s, "novation-launch-control-xl");
	if (nk && mm && lc) {
		CHECK(!strcmp(mp_find_control(nk, "fader.8")->source, "midi:cc:1:7"));
		CHECK(!strcmp(mp_find_control(nk, "button.rec.1")->source, "midi:cc:1:64"));
		CHECK(!strcmp(mp_find_control(nk, "marker.next")->source, "midi:cc:1:62"));
		CHECK(!strcmp(mp_find_control(mm, "knob.c.8")->source, "midi:cc:1:60"));
		CHECK(!strcmp(mp_find_control(mm, "button.rec.8")->source, "midi:note:1:24"));
		CHECK(mp_find_control(mm, "button.mute.1")->feedback);
		CHECK(!strcmp(mp_find_control(lc, "knob.a.1")->source, "midi:cc:9:13"));
		CHECK(!strcmp(mp_find_control(lc, "button.control.8")->source, "midi:note:9:92"));
		CHECK(!strcmp(mp_find_control(lc, "nav.right")->source, "midi:cc:9:107"));
		/* channel 9 only: template 2 (channel 10) is not this profile */
		struct ctl_source t2;
		char err[100];
		ctl_parse_source("midi:cc:10:77", &t2, err, sizeof(err));
		CHECK(mp_control_for_source(lc, &t2) == NULL);
	}
	mp_set_free(&s);
}

static char tmpdir[256];

static void write_file(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");
	fputs(s, f);
	fclose(f);
}

static void rm_rf(const char *d)
{
	char cmd[400];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", d);
	if (system(cmd)) {}
}

static int warned;
static void on_warn(void *ud, const char *path, const char *why)
{
	(void)ud;
	(void)path;
	(void)why;
	warned++;
}

/* Learned mappings to bindings, saved, loaded in the user directory over
 * the system copy, and back to the mappings controld makes. */
static void test_round_trip(void)
{
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/bl-midiprof-%d", getpid());
	char user[300], sysd[300], path[400];
	snprintf(user, sizeof(user), "%s/user/" MP_DIR_NAME, tmpdir);
	snprintf(sysd, sizeof(sysd), "%s/system", tmpdir);
	mkdir(tmpdir, 0700);
	mkdir(sysd, 0700);

	struct mp_profile p;
	char err[400];
	snprintf(path, sizeof(path), "%s/korg-nanokontrol2.json", SRCDIR);
	CHECK(mp_load_file(path, &p, err, sizeof(err)) == 0);
	/* what Mappings() lists after two learns, an OSC mapping and one from another device */
	json_object *ms = json_tokener_parse(
		"[{\"id\":\"m1\",\"source\":\"midi:cc:1:0\",\"target\":\"param:/console/in.1/fader\",\"gesture\":\"auto\",\"sends\":3},"
		" {\"id\":\"m2\",\"source\":\"midi:cc:1:48\",\"target\":\"param:/console/in.1/mute\",\"feedback\":true},"
		" {\"id\":\"m3\",\"source\":\"midi:cc:1:0\",\"target\":\"volume:some.sink\",\"min\":-40,\"origin\":\"control.json\"},"
		" {\"id\":\"m4\",\"source\":\"osc:/1/fader1\",\"target\":\"transport:tempo\"},"
		" {\"id\":\"m5\",\"source\":\"midi:cc:2:0\",\"target\":\"transport:play\"}]");
	int skipped = -1;
	CHECK(mp_bind_mappings(&p, ms, false, &skipped, err, sizeof(err)) == 3 && skipped == 2);
	CHECK(json_object_array_length(p.bindings) == 3);
	/* the binding keeps the options and drops what is the mapping's own */
	json_object *b0 = json_object_array_get_idx(p.bindings, 0), *v;
	CHECK(json_object_object_get_ex(b0, "gesture", &v) && !json_object_object_get_ex(b0, "sends", &v) &&
	      !json_object_object_get_ex(b0, "id", &v) && !json_object_object_get_ex(b0, "source", &v));
	CHECK(json_object_object_get_ex(b0, "control", &v) && !strcmp(json_object_get_string(v), "fader.1"));

	/* saved as the user's copy; the system's copy is older */
	char spath[400], upath[400];
	snprintf(spath, sizeof(spath), "%s/korg-nanokontrol2.json", sysd);
	json_object *orig = json_object_from_file(path);
	json_object_to_file(spath, orig);
	json_object_put(orig);
	snprintf(upath, sizeof(upath), "%s/korg-nanokontrol2.json", user);
	CHECK(mp_save_file(&p, upath, err, sizeof(err)) == 0);
	struct stat st;
	CHECK(stat(upath, &st) == 0 && (st.st_mode & 0777) == 0644);
	char tmpname[400];
	snprintf(tmpname, sizeof(tmpname), "%s/.korg-nanokontrol2.json.tmp", user);
	CHECK(access(tmpname, F_OK) != 0);
	/* a broken file and a misnamed one sit beside it */
	snprintf(path, sizeof(path), "%s/broken.json", user);
	write_file(path, "{\"schema\":\"backline.midi-profile/1\",\"name\":\"x\"}");
	snprintf(path, sizeof(path), "%s/other-name.json", user);
	write_file(path, HEAD "\"id\":\"not-other\",\"controls\":[]}");

	const char *dirs[] = { user, sysd };
	bool sys[] = { false, true };
	struct mp_set s = { 0 };
	warned = 0;
	CHECK(mp_set_load(&s, dirs, 2, sys, on_warn, NULL) == 1 && warned == 2);
	struct mp_profile *q = mp_set_find(&s, "korg-nanokontrol2");
	CHECK(q && !q->system && json_object_array_length(q->bindings) == 3);
	if (q) {
		json_object *a = mp_to_json(&p), *b = mp_to_json(q);
		CHECK(json_object_equal(a, b));
		json_object_put(a);
		json_object_put(b);
		/* back to mappings: ids per control, sources from the controls */
		json_object *m0 = mp_binding_mapping(q, 0, err, sizeof(err)), *m1 = mp_binding_mapping(q, 1, err, sizeof(err)),
			    *m2 = mp_binding_mapping(q, 2, err, sizeof(err));
		CHECK(m0 && m1 && m2);
		if (m0 && m1 && m2) {
			CHECK(!strcmp(json_object_get_string(json_object_object_get(m0, "id")), "korg-nanokontrol2.fader.1"));
			CHECK(!strcmp(json_object_get_string(json_object_object_get(m0, "source")), "midi:cc:1:0"));
			CHECK(!strcmp(json_object_get_string(json_object_object_get(m1, "id")), "korg-nanokontrol2.button.mute.1"));
			CHECK(!strcmp(json_object_get_string(json_object_object_get(m2, "id")), "korg-nanokontrol2.fader.1.2"));
			CHECK(json_object_get_double(json_object_object_get(m2, "min")) == -40);
			/* what controld will parse */
			struct ctl_source cs;
			struct ctl_target ct;
			CHECK(ctl_parse_source(json_object_get_string(json_object_object_get(m1, "source")), &cs, err, sizeof(err)) == 0);
			CHECK(ctl_parse_target(json_object_get_string(json_object_object_get(m1, "target")), &ct, err, sizeof(err)) == 0);
		}
		json_object_put(m0);
		json_object_put(m1);
		json_object_put(m2);
		CHECK(mp_binding_mapping(q, 3, err, sizeof(err)) == NULL);
	}
	mp_set_free(&s);

	/* a control's feedback default reaches the mapping; the binding can say no */
	struct mp_profile mm;
	snprintf(path, sizeof(path), "%s/akai-midimix.json", SRCDIR);
	CHECK(mp_load_file(path, &mm, err, sizeof(err)) == 0);
	json_object *ms2 = json_tokener_parse(
		"[{\"source\":\"midi:note:1:1\",\"target\":\"mute:a.sink\"},"
		" {\"source\":\"midi:note:1:3\",\"target\":\"recorder:record\",\"feedback\":false}]");
	CHECK(mp_bind_mappings(&mm, ms2, false, NULL, err, sizeof(err)) == 2);
	json_object *f0 = mp_binding_mapping(&mm, 0, err, sizeof(err)), *f1 = mp_binding_mapping(&mm, 1, err, sizeof(err));
	CHECK(f0 && json_object_get_boolean(json_object_object_get(f0, "feedback")));
	CHECK(f1 && !json_object_get_boolean(json_object_object_get(f1, "feedback")));
	json_object_put(f0);
	json_object_put(f1);
	json_object_put(ms2);
	mp_free(&mm);

	/* a new profile for a device nobody shipped: controls made from the sources */
	struct mp_profile np;
	CHECK(mp_init(&np, "my-box", "My box", "^Midi-Bridge:My Box", err, sizeof(err)) == 0);
	CHECK(mp_bind_mappings(&np, ms, true, &skipped, err, sizeof(err)) == 4 && skipped == 1);
	CHECK(np.n_controls == 3 && mp_find_control(&np, "cc.1.0") && mp_find_control(&np, "cc.1.48") &&
	      mp_find_control(&np, "cc.2.0"));
	snprintf(path, sizeof(path), "%s/my-box.json", user);
	CHECK(mp_save_file(&np, path, err, sizeof(err)) == 0);
	struct mp_profile rl;
	CHECK(mp_load_file(path, &rl, err, sizeof(err)) == 0 && rl.n_controls == 3 && rl.autoload);
	mp_free(&rl);
	mp_free(&np);
	CHECK(mp_init(&np, "Bad Id", "x", "y", err, sizeof(err)) < 0);
	CHECK(mp_init(&np, "ok", "x", "", err, sizeof(err)) < 0);

	/* a binding that does not validate leaves the old bindings in place */
	json_object *bad = json_tokener_parse("[{\"source\":\"midi:cc:1:0\",\"target\":\"transport:play\",\"takeover\":\"grab\"}]");
	size_t before = json_object_array_length(p.bindings);
	CHECK(mp_bind_mappings(&p, bad, false, NULL, err, sizeof(err)) < 0 && json_object_array_length(p.bindings) == before);
	json_object_put(bad);

	json_object_put(ms);
	mp_free(&p);
	rm_rf(tmpdir);
}

int main(void)
{
	test_validation();
	test_shipped();
	test_round_trip();
	if (fails) {
		fprintf(stderr, "%d checks failed\n", fails);
		return 1;
	}
	puts("midi profiles: validation, shipped profiles, save/load and bindings ok");
	return 0;
}
