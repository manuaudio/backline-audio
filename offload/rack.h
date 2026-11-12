// rack.h: a minimal in-process CLAP chain for the offload proof and the DSP
// bench. One Rack = plugins in series, stereo, fixed block size. Everything is
// allocated before the first process(); process() does no allocation, no locks,
// no syscalls. Proof code: no GUI, no state, no presets, no MIDI.
// SPDX-License-Identifier: MIT
#pragma once

#include <clap/clap.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace blo {

struct ParamSet {
    std::string name; // substring of the CLAP param name (case sensitive)
    double value;
};

struct Spec {
    std::string path, id;
    std::vector<ParamSet> sets;
};

// "PATH:ID[:name=value;name=value]" (ID may be empty: first plugin)
inline Spec parse_spec(const std::string &s)
{
    Spec sp;
    size_t a = s.find(".clap:");
    if (a == std::string::npos) {
        sp.path = s;
        return sp;
    }
    sp.path = s.substr(0, a + 5);
    std::string rest = s.substr(a + 6);
    size_t b = rest.find(':');
    sp.id = rest.substr(0, b);
    if (b != std::string::npos) {
        std::string kv = rest.substr(b + 1);
        size_t p = 0;
        while (p < kv.size()) {
            size_t e = kv.find(';', p);
            std::string one = kv.substr(p, e == std::string::npos ? std::string::npos : e - p);
            size_t eq = one.rfind('=');
            if (eq != std::string::npos)
                sp.sets.push_back({one.substr(0, eq), atof(one.c_str() + eq + 1)});
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    return sp;
}

inline const void *host_ext(const clap_host *, const char *) { return nullptr; }
inline void host_noop(const clap_host *) {}

struct EvList {
    std::vector<clap_event_param_value> ev;
    static uint32_t size(const clap_input_events *l) { return (uint32_t)((EvList *)l->ctx)->ev.size(); }
    static const clap_event_header *get(const clap_input_events *l, uint32_t i) { return &((EvList *)l->ctx)->ev[i].header; }
};
inline bool out_push(const clap_output_events *, const clap_event_header *) { return true; }

struct Plugin {
    void *dl = nullptr;
    const clap_plugin_entry *entry = nullptr;
    const clap_plugin *p = nullptr;
    std::string name;
    EvList init_events; // param sets, sent once with the first block
    bool started = false;
    uint32_t in_ch = 2, out_ch = 2;
};

class Rack {
public:
    uint32_t frames, rate;
    std::vector<Plugin> plugins;
    // ping-pong stereo buffers
    std::vector<float> buf[2][2];
    clap_host host{};

    Rack(uint32_t rate_, uint32_t frames_) : frames(frames_), rate(rate_)
    {
        for (auto &b : buf)
            for (auto &c : b) c.assign(frames, 0.f);
        host.clap_version = CLAP_VERSION;
        host.name = "backline-offload-proof";
        host.vendor = "Backline";
        host.url = "";
        host.version = "0";
        host.get_extension = host_ext;
        host.request_restart = host_noop;
        host.request_process = host_noop;
        host.request_callback = host_noop;
    }

    // main thread
    bool add(const Spec &s, std::string &err)
    {
        Plugin pl;
        pl.dl = dlopen(s.path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!pl.dl) { err = dlerror(); return false; }
        pl.entry = (const clap_plugin_entry *)dlsym(pl.dl, "clap_entry");
        if (!pl.entry || !pl.entry->init(s.path.c_str())) { err = "no clap_entry / init failed"; return false; }
        auto *f = (const clap_plugin_factory *)pl.entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        const clap_plugin_descriptor *d = nullptr;
        for (uint32_t i = 0; f && i < f->get_plugin_count(f); i++) {
            auto *di = f->get_plugin_descriptor(f, i);
            if (s.id.empty() || s.id == di->id) { d = di; break; }
        }
        if (!d) { err = "plugin id not found: " + s.id; return false; }
        pl.name = d->name;
        pl.p = f->create_plugin(f, &host, d->id);
        if (!pl.p || !pl.p->init(pl.p)) { err = "create/init failed"; return false; }
        if (!pl.p->activate(pl.p, rate, frames, frames)) { err = "activate failed"; return false; }
        if (auto *ap = (const clap_plugin_audio_ports *)pl.p->get_extension(pl.p, CLAP_EXT_AUDIO_PORTS)) {
            clap_audio_port_info pi;
            if (ap->count(pl.p, true) && ap->get(pl.p, 0, true, &pi)) pl.in_ch = pi.channel_count > 2 ? 2 : pi.channel_count;
            if (ap->count(pl.p, false) && ap->get(pl.p, 0, false, &pi)) pl.out_ch = pi.channel_count > 2 ? 2 : pi.channel_count;
        }
        auto *params = (const clap_plugin_params *)pl.p->get_extension(pl.p, CLAP_EXT_PARAMS);
        for (auto &ps : s.sets) {
            bool found = false;
            for (uint32_t i = 0; params && i < params->count(pl.p); i++) {
                clap_param_info info;
                if (!params->get_info(pl.p, i, &info)) continue;
                if (!strstr(info.name, ps.name.c_str())) continue;
                clap_event_param_value e{};
                e.header.size = sizeof e;
                e.header.time = 0;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.type = CLAP_EVENT_PARAM_VALUE;
                e.param_id = info.id;
                e.cookie = info.cookie;
                e.note_id = e.port_index = e.channel = e.key = -1;
                e.value = ps.value;
                pl.init_events.ev.push_back(e);
                found = true;
            }
            if (!found) fprintf(stderr, "rack: %s: no param matching '%s'\n", pl.name.c_str(), ps.name.c_str());
        }
        plugins.push_back(std::move(pl));
        return true;
    }

    static void list_params(const Spec &s)
    {
        Rack r(48000, 64);
        std::string err;
        if (!r.add(s, err)) { fprintf(stderr, "%s\n", err.c_str()); return; }
        auto *p = r.plugins[0].p;
        auto *params = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        for (uint32_t i = 0; params && i < params->count(p); i++) {
            clap_param_info info;
            params->get_info(p, i, &info);
            printf("%u\t%s\t%g..%g def %g\n", info.id, info.name, info.min_value, info.max_value, info.default_value);
        }
    }

    // audio thread. in/out: stereo, `frames` long. Returns false on error.
    bool process(const float *inL, const float *inR, float *outL, float *outR, int64_t steady)
    {
        memcpy(buf[0][0].data(), inL, frames * sizeof(float));
        memcpy(buf[0][1].data(), inR, frames * sizeof(float));
        int cur = 0;
        static thread_local EvList empty;
        clap_output_events oev{nullptr, out_push};
        for (auto &pl : plugins) {
            if (!pl.started) { pl.p->start_processing(pl.p); pl.started = true; }
            float *ich[2] = {buf[cur][0].data(), buf[cur][1].data()};
            float *och[2] = {buf[cur ^ 1][0].data(), buf[cur ^ 1][1].data()};
            clap_audio_buffer ib{}, ob{};
            ib.data32 = ich; ib.channel_count = pl.in_ch;
            ob.data32 = och; ob.channel_count = pl.out_ch;
            EvList *el = pl.init_events.ev.empty() ? &empty : &pl.init_events;
            clap_input_events iev{el, EvList::size, EvList::get};
            clap_process pr{};
            pr.steady_time = steady;
            pr.frames_count = frames;
            pr.audio_inputs = &ib; pr.audio_inputs_count = 1;
            pr.audio_outputs = &ob; pr.audio_outputs_count = 1;
            pr.in_events = &iev; pr.out_events = &oev;
            if (pl.p->process(pl.p, &pr) == CLAP_PROCESS_ERROR) return false;
            if (el == &pl.init_events) pl.init_events.ev.clear(); // capacity kept: no free on the audio thread
            if (pl.out_ch == 1) memcpy(och[1], och[0], frames * sizeof(float)); // mono plugin: dual mono onward
            cur ^= 1;
        }
        memcpy(outL, buf[cur][0].data(), frames * sizeof(float));
        memcpy(outR, buf[cur][1].data(), frames * sizeof(float));
        return true;
    }

    ~Rack()
    {
        for (auto &pl : plugins) {
            if (pl.started) pl.p->stop_processing(pl.p);
            pl.p->deactivate(pl.p);
            pl.p->destroy(pl.p);
            // entry->deinit and dlclose skipped on purpose: several racks share a .so
        }
    }
};

} // namespace blo
