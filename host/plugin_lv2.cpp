// LV2 adapter of the plugin worker, on lilv.
// SPDX-License-Identifier: MIT
//
// Threads, as LV2 defines them: instantiate, activate, state save/restore
// (restore is in the instantiation class: the worker keeps the audio thread
// out while it runs) and the worker extension's work() on the worker's main
// loop; run() on PipeWire's data loop. Control ports are plain floats the
// audio thread writes before run(); MIDI goes in as an atom sequence with
// frame offsets. Worker requests and responses cross threads through SPSC
// rings of fixed slots, so run() never locks or allocates.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/port-props/port-props.h>
#include <lv2/presets/presets.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "plugin.h"
#include "spsc.h"

namespace bl {
namespace {

constexpr uint32_t kAtomBuf = 65536;   // default atom port buffer
constexpr uint32_t kWorkSlot = 2048;   // largest worker message

struct WorkMsg {
    uint32_t size;
    uint8_t data[kWorkSlot];
};

std::string trim_slash(std::string s)
{
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

class Lv2Instance;

struct Port {
    enum Kind { Audio, Control, Atom, Cv, Other } kind = Other;
    bool input = false;
    uint32_t index = 0;
    std::string symbol;
    float value = 0;                    // control: what the plugin reads (audio thread writes)
    std::atomic<float> mirror{0};       // control input: main thread's copy (state save)
    bool midi = false;                  // atom input that takes MIDI
    bool freewheel = false;             // control input designated lv2:freeWheeling
    std::vector<uint64_t> atom;         // atom buffer (8-byte aligned)
    uint32_t atom_size = 0;             // bytes
    std::vector<float> cv;              // cv buffer
};

class Lv2Instance final : public PluginInstance {
public:
    Host *H;
    LilvWorld *world = nullptr;
    const LilvPlugin *plugin = nullptr;
    LilvInstance *instance = nullptr;
    uint32_t inst_rate = 0;
    bool active = false;

    // URIDs
    std::mutex map_lock;
    std::unordered_map<std::string, LV2_URID> uri_to_id;
    std::vector<std::string> id_to_uri;
    LV2_URID_Map map{this, map_uri};
    LV2_URID_Unmap unmap{this, unmap_uri};
    LV2_URID u_atom_Sequence, u_atom_Chunk, u_atom_Float, u_atom_Double, u_atom_Int, u_atom_Long, u_atom_Bool,
        u_midi_Event, u_min_block, u_max_block, u_nominal_block, u_seq_size, u_sample_rate, u_frame_time;

    // features
    LV2_Log_Log log_feature{this, log_printf, log_vprintf};
    LV2_Worker_Schedule schedule{this, schedule_work};
    int32_t min_block = 1, max_block = (int32_t)kMaxFrames, nominal_block = 1024, seq_size = (int32_t)kAtomBuf;
    float sample_rate = 48000;
    std::vector<LV2_Options_Option> options;
    LV2_Feature f_map{LV2_URID__map, &map}, f_unmap{LV2_URID__unmap, &unmap}, f_options{LV2_OPTIONS__options, nullptr},
        f_bounded{LV2_BUF_SIZE__boundedBlockLength, nullptr}, f_schedule{LV2_WORKER__schedule, &schedule},
        f_log{LV2_LOG__log, &log_feature}, f_default_state{LV2_STATE__loadDefaultState, nullptr};
    std::vector<const LV2_Feature *> features;

    // ports
    std::vector<std::unique_ptr<Port>> ports;
    std::vector<std::vector<uint32_t>> audio_in, audio_out;   // port indices, by bus (port group)
    std::vector<float> zeros = std::vector<float>(kMaxFrames, 0.f), trash = std::vector<float>(kMaxFrames, 0.f);
    Port *midi_port = nullptr;

    // worker extension
    const LV2_Worker_Interface *worker_iface = nullptr;
    Spsc<WorkMsg> work_req{64}, work_resp{64};
    std::atomic<uint64_t> work_dropped{0};

    // presets
    std::vector<std::pair<std::string, LilvNode *>> preset_nodes;

    explicit Lv2Instance(Host *h) : H(h) {}

    ~Lv2Instance() override
    {
        if (instance) {
            if (active)
                lilv_instance_deactivate(instance);
            lilv_instance_free(instance);
        }
        for (auto &p : preset_nodes)
            lilv_node_free(p.second);
        if (world)
            lilv_world_free(world);
    }

    const char *format() const override { return "lv2"; }

    // ------------------------------------------------------------ URIDs
    static LV2_URID map_uri(LV2_URID_Map_Handle h, const char *uri)
    {
        auto *self = static_cast<Lv2Instance *>(h);
        std::lock_guard<std::mutex> g(self->map_lock);
        auto it = self->uri_to_id.find(uri);
        if (it != self->uri_to_id.end())
            return it->second;
        self->id_to_uri.push_back(uri);
        LV2_URID id = (LV2_URID)self->id_to_uri.size();
        self->uri_to_id[uri] = id;
        return id;
    }
    static const char *unmap_uri(LV2_URID_Unmap_Handle h, LV2_URID id)
    {
        auto *self = static_cast<Lv2Instance *>(h);
        std::lock_guard<std::mutex> g(self->map_lock);
        return id && id <= self->id_to_uri.size() ? self->id_to_uri[id - 1].c_str() : nullptr;
    }
    LV2_URID m(const char *uri) { return map_uri(this, uri); }

    static int log_vprintf(LV2_Log_Handle h, LV2_URID type, const char *fmt, va_list ap)
    {
        auto *self = static_cast<Lv2Instance *>(h);
        if (self->H->is_audio_thread())
            return 0; // no I/O on the audio thread
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        const char *t = unmap_uri(h, type);
        self->H->log("plugin %s: %s%s", t && strrchr(t, '#') ? strrchr(t, '#') + 1 : "log", buf,
                     buf[0] && buf[strlen(buf) - 1] == '\n' ? "" : "\n");
        return 0;
    }
    static int log_printf(LV2_Log_Handle h, LV2_URID type, const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        int r = log_vprintf(h, type, fmt, ap);
        va_end(ap);
        return r;
    }

    // ----------------------------------------------------------- worker
    static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size, const void *data)
    {
        auto *self = static_cast<Lv2Instance *>(h);
        if (size > kWorkSlot)
            return LV2_WORKER_ERR_NO_SPACE;
        WorkMsg msg;
        msg.size = size;
        memcpy(msg.data, data, size);
        return self->work_resp.push(msg) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
    }
    static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle h, uint32_t size, const void *data)
    {
        auto *self = static_cast<Lv2Instance *>(h);
        if (!self->worker_iface || size > kWorkSlot)
            return LV2_WORKER_ERR_NO_SPACE;
        if (!self->H->is_audio_thread()) {
            // main thread (instantiate, restore): do it now; the response is
            // delivered with the next run()
            return self->worker_iface->work(lilv_instance_get_handle(self->instance), respond, self, size, data);
        }
        static thread_local WorkMsg msg; // no stack monster on the RT thread
        msg.size = size;
        memcpy(msg.data, data, size);
        if (!self->work_req.push(msg)) {
            self->work_dropped++;
            return LV2_WORKER_ERR_NO_SPACE;
        }
        self->H->request_callback();
        return LV2_WORKER_SUCCESS;
    }

    void on_main_thread() override
    {
        static WorkMsg msg;
        while (worker_iface && work_req.pop(msg))
            worker_iface->work(lilv_instance_get_handle(instance), respond, this, msg.size, msg.data);
    }

    // -------------------------------------------------------- instantiate
    bool instantiate(uint32_t rate, std::string *err)
    {
        sample_rate = (float)rate;
        options = {
            {LV2_OPTIONS_INSTANCE, 0, u_min_block, sizeof(int32_t), u_atom_Int, &min_block},
            {LV2_OPTIONS_INSTANCE, 0, u_max_block, sizeof(int32_t), u_atom_Int, &max_block},
            {LV2_OPTIONS_INSTANCE, 0, u_nominal_block, sizeof(int32_t), u_atom_Int, &nominal_block},
            {LV2_OPTIONS_INSTANCE, 0, u_seq_size, sizeof(int32_t), u_atom_Int, &seq_size},
            {LV2_OPTIONS_INSTANCE, 0, u_sample_rate, sizeof(float), u_atom_Float, &sample_rate},
            {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
        };
        f_options.data = options.data();
        features = {&f_map, &f_unmap, &f_options, &f_bounded, &f_schedule, &f_log, &f_default_state, nullptr};
        instance = lilv_plugin_instantiate(plugin, rate, features.data());
        if (!instance) {
            if (err) *err = "lilv_plugin_instantiate failed";
            return false;
        }
        inst_rate = rate;
        worker_iface = (const LV2_Worker_Interface *)lilv_instance_get_extension_data(instance, LV2_WORKER__interface);
        for (auto &p : ports) {
            if (p->kind == Port::Control)
                lilv_instance_connect_port(instance, p->index, &p->value);
            else if (p->kind == Port::Atom)
                lilv_instance_connect_port(instance, p->index, p->atom.data());
            else if (p->kind == Port::Cv)
                lilv_instance_connect_port(instance, p->index, p->cv.data());
            else if (p->kind == Port::Audio)
                lilv_instance_connect_port(instance, p->index, p->input ? zeros.data() : trash.data());
            else
                lilv_instance_connect_port(instance, p->index, nullptr);
        }
        return true;
    }

    // ------------------------------------------------------------ params
    void rescan_params() override
    {
        params.clear();
        for (auto &p : ports) {
            if (p->kind != Port::Control || !p->input)
                continue;
            const LilvPort *lp = lilv_plugin_get_port_by_index(plugin, p->index);
            if (p->freewheel)
                continue; // host-owned: the worker never runs freewheeling
            LilvNode *def = nullptr, *min = nullptr, *max = nullptr;
            lilv_port_get_range(plugin, lp, &def, &min, &max);
            ParamInfo pi;
            pi.id = p->index;
            LilvNode *nm = lilv_port_get_name(plugin, lp);
            pi.name = nm ? lilv_node_as_string(nm) : p->symbol;
            lilv_node_free(nm);
            pi.min = min ? lilv_node_as_float(min) : 0;
            pi.max = max ? lilv_node_as_float(max) : 1;
            pi.def = def ? lilv_node_as_float(def) : pi.min;
            auto has = [&](const char *uri) {
                LilvNode *n = lilv_new_uri(world, uri);
                bool r = lilv_port_has_property(plugin, lp, n);
                lilv_node_free(n);
                return r;
            };
            if (has(LV2_CORE__sampleRate)) {
                pi.min *= sample_rate;
                pi.max *= sample_rate;
                pi.def *= sample_rate;
            }
            if (has(LV2_CORE__toggled)) {
                pi.stepped = true;
                pi.min = 0;
                pi.max = 1;
            }
            if (has(LV2_CORE__integer) || has(LV2_CORE__enumeration))
                pi.stepped = true;
            if (pi.min > pi.max)
                std::swap(pi.min, pi.max);
            lilv_node_free(def);
            lilv_node_free(min);
            lilv_node_free(max);
            params.push_back(std::move(pi));
        }
    }

    bool live_values() const override { return true; }

    bool read_param(const ParamInfo &p, double &v) override
    {
        v = ports[p.id]->mirror.load();
        return true;
    }

    double prepare_set(const ParamInfo &p, double v) override
    {
        ports[p.id]->mirror.store((float)v);
        return v;
    }

    bool set_param_inactive(const ParamInfo &p, double v) override
    {
        ports[p.id]->mirror.store((float)v);
        ports[p.id]->value = (float)v; // the audio thread isn't running the plugin
        return true;
    }

    bool activate(uint32_t rate, uint32_t) override
    {
        if (rate != inst_rate) {
            // LV2 fixes the rate at instantiation: carry the state over to a
            // new instance
            std::vector<uint8_t> st;
            std::string err;
            bool have = save_state(st, &err);
            lilv_instance_free(instance);
            instance = nullptr;
            if (!instantiate(rate, &err))
                return false;
            rescan_params(); // sampleRate-relative ranges
            if (have)
                load_state(st, &err);
        }
        lilv_instance_activate(instance);
        active = true;
        return true;
    }

    void deactivate() override
    {
        if (active)
            lilv_instance_deactivate(instance);
        active = false;
    }

    // ------------------------------------------------------------- state
    static const void *get_port_value(const char *symbol, void *user, uint32_t *size, uint32_t *type)
    {
        auto *self = static_cast<Lv2Instance *>(user);
        static thread_local float v;
        for (auto &p : self->ports)
            if (p->kind == Port::Control && p->input && p->symbol == symbol) {
                v = p->mirror.load();
                *size = sizeof(float);
                *type = self->u_atom_Float;
                return &v;
            }
        *size = *type = 0;
        return nullptr;
    }

    static void set_port_value(const char *symbol, void *user, const void *value, uint32_t size, uint32_t type)
    {
        auto *self = static_cast<Lv2Instance *>(user);
        double v;
        if (type == self->u_atom_Float && size == sizeof(float)) v = *(const float *)value;
        else if (type == self->u_atom_Double && size == sizeof(double)) v = *(const double *)value;
        else if ((type == self->u_atom_Int || type == self->u_atom_Bool) && size == sizeof(int32_t)) v = *(const int32_t *)value;
        else if (type == self->u_atom_Long && size == sizeof(int64_t)) v = (double)*(const int64_t *)value;
        else return;
        for (auto &p : self->ports)
            if (p->kind == Port::Control && p->input && p->symbol == symbol) {
                p->value = (float)v; // restore runs with the audio thread kept out
                p->mirror.store((float)v);
            }
    }

    bool save_state(std::vector<uint8_t> &out, std::string *err) override
    {
        LilvState *st = lilv_state_new_from_instance(plugin, instance, &map, nullptr, nullptr, nullptr, nullptr, get_port_value,
                                                     this, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features.data());
        if (!st) {
            if (err) *err = "plugin failed to save state";
            return false;
        }
        char *s = lilv_state_to_string(world, &map, &unmap, st, "urn:backline:state", nullptr);
        lilv_state_free(st);
        if (!s) {
            if (err) *err = "could not serialise the state";
            return false;
        }
        out.assign(s, s + strlen(s));
        lilv_free(s);
        return true;
    }

    bool restore(LilvState *st, std::string *err)
    {
        const LilvNode *puri = lilv_state_get_plugin_uri(st);
        if (!puri || !lilv_node_equals(puri, lilv_plugin_get_uri(plugin))) {
            if (err) *err = std::string("the state is for another plugin (") + (puri ? lilv_node_as_uri(puri) : "?") + ")";
            return false;
        }
        lilv_state_restore(st, instance, set_port_value, this, 0, features.data());
        return true;
    }

    bool load_state(const std::vector<uint8_t> &in, std::string *err) override
    {
        std::string s(in.begin(), in.end());
        LilvState *st = lilv_state_new_from_string(world, &map, s.c_str());
        if (!st) {
            if (err) *err = "not an LV2 state file";
            return false;
        }
        bool ok = restore(st, err);
        lilv_state_free(st);
        return ok;
    }

    bool restore_needs_pause() const override { return true; }

    // ----------------------------------------------------------- presets
    std::vector<PresetInfo> list_presets() override
    {
        std::vector<PresetInfo> out;
        for (auto &p : preset_nodes)
            lilv_node_free(p.second);
        preset_nodes.clear();
        LilvNode *pset = lilv_new_uri(world, LV2_PRESETS__Preset);
        LilvNode *label = lilv_new_uri(world, LILV_NS_RDFS "label");
        LilvNode *see_also = lilv_new_uri(world, LILV_NS_RDFS "seeAlso");
        LilvNodes *ps = lilv_plugin_get_related(plugin, pset);
        LILV_FOREACH (nodes, i, ps) {
            const LilvNode *n = lilv_nodes_get(ps, i);
            lilv_world_load_resource(world, n);
            LilvNodes *labels = lilv_world_find_nodes(world, n, label, nullptr);
            std::string name = labels && lilv_nodes_size(labels) ? lilv_node_as_string(lilv_nodes_get_first(labels)) : "";
            lilv_nodes_free(labels);
            std::string where;
            LilvNodes *files = lilv_world_find_nodes(world, n, see_also, nullptr);
            if (files && lilv_nodes_size(files)) {
                char *f = lilv_file_uri_parse(lilv_node_as_uri(lilv_nodes_get_first(files)), nullptr);
                if (f) where = f;
                lilv_free(f);
            }
            lilv_nodes_free(files);
            std::string uri = lilv_node_as_uri(n);
            if (name.empty())
                name = uri.substr(uri.find_last_of("#/") + 1);
            out.push_back({uri, name, where.empty() ? uri : where});
            preset_nodes.push_back({uri, lilv_node_duplicate(n)});
        }
        lilv_nodes_free(ps);
        lilv_node_free(pset);
        lilv_node_free(label);
        lilv_node_free(see_also);
        return out;
    }

    bool load_preset(const PresetInfo &p, std::string *err) override
    {
        LilvNode *node = nullptr;
        for (auto &pn : preset_nodes)
            if (pn.first == p.id)
                node = pn.second;
        if (!node) {
            if (err) *err = "unknown preset";
            return false;
        }
        LilvState *st = lilv_state_new_from_world(world, &map, node);
        if (!st) {
            if (err) *err = "could not read the preset";
            return false;
        }
        bool ok = restore(st, err);
        lilv_state_free(st);
        if (ok)
            H->mark_dirty();
        return ok;
    }

    // ------------------------------------------------------------- audio
    bool start_processing() override { return true; }
    void stop_processing() override {}

    void reset_atom_in(Port &p)
    {
        auto *seq = reinterpret_cast<LV2_Atom_Sequence *>(p.atom.data());
        seq->atom.type = u_atom_Sequence;
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->body.unit = u_frame_time;
        seq->body.pad = 0;
    }

    bool process(const ProcessArgs &a) override
    {
        for (uint32_t i = 0; i < a.n_params; i++)
            ports[a.params[i].id]->value = (float)a.params[i].value;
        for (auto &pp : ports) {
            Port &p = *pp;
            if (p.kind != Port::Atom)
                continue;
            if (p.input) {
                reset_atom_in(p);
            } else {
                auto *atom = reinterpret_cast<LV2_Atom *>(p.atom.data());
                atom->type = u_atom_Chunk;
                atom->size = p.atom_size - sizeof(LV2_Atom);
            }
        }
        if (midi_port) {
            auto *seq = reinterpret_cast<LV2_Atom_Sequence *>(midi_port->atom.data());
            for (uint32_t i = 0; i < a.n_midi; i++) {
                const MidiEv &e = a.midi[i];
                uint32_t need = sizeof(LV2_Atom_Event) + lv2_atom_pad_size(e.size);
                if (sizeof(LV2_Atom) + seq->atom.size + need > midi_port->atom_size) {
                    H->count_dropped();
                    break;
                }
                auto *ev = reinterpret_cast<LV2_Atom_Event *>((uint8_t *)LV2_ATOM_BODY(&seq->atom) + seq->atom.size);
                ev->time.frames = e.time;
                ev->body.type = u_midi_Event;
                ev->body.size = e.size;
                memcpy(LV2_ATOM_BODY(&ev->body), e.data, e.size);
                seq->atom.size += need;
            }
        }
        for (size_t b = 0; b < audio_in.size(); b++)
            for (size_t k = 0; k < audio_in[b].size(); k++)
                lilv_instance_connect_port(instance, audio_in[b][k], a.ins[b][k]);
        for (size_t b = 0; b < audio_out.size(); b++)
            for (size_t k = 0; k < audio_out[b].size(); k++)
                lilv_instance_connect_port(instance, audio_out[b][k], a.outs[b][k]);
        if (worker_iface) {
            WorkMsg *msg = &resp_slot;
            while (work_resp.pop(*msg))
                worker_iface->work_response(lilv_instance_get_handle(instance), msg->size, msg->data);
        }
        lilv_instance_run(instance, a.frames);
        if (worker_iface && worker_iface->end_run)
            worker_iface->end_run(lilv_instance_get_handle(instance));
        return true;
    }
    WorkMsg resp_slot; // audio thread only

    // ------------------------------------------------------------- setup
    bool setup_ports(std::string *err)
    {
        LilvNode *n_audio = lilv_new_uri(world, LV2_CORE__AudioPort), *n_control = lilv_new_uri(world, LV2_CORE__ControlPort),
                 *n_input = lilv_new_uri(world, LV2_CORE__InputPort), *n_atom = lilv_new_uri(world, LV2_ATOM__AtomPort),
                 *n_cv = lilv_new_uri(world, LV2_CORE__CVPort), *n_midi = lilv_new_uri(world, LV2_MIDI__MidiEvent),
                 *n_min_size = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize),
                 *n_optional = lilv_new_uri(world, LV2_CORE__connectionOptional),
                 *n_group = lilv_new_uri(world, "http://lv2plug.in/ns/ext/port-groups#group"),
                 *n_designation = lilv_new_uri(world, LV2_CORE__designation);
        // audio ports go into buses by port group (pg:group), in order of
        // first appearance; ungrouped ports share one bus per direction
        std::vector<std::string> in_keys, out_keys;
        auto bus_of = [&](const LilvPort *lp, bool input) -> std::vector<uint32_t> & {
            LilvNode *g = lilv_port_get(plugin, lp, n_group);
            std::string key = g ? lilv_node_as_string(g) : "";
            lilv_node_free(g);
            auto &keys = input ? in_keys : out_keys;
            auto &buses = input ? audio_in : audio_out;
            for (size_t i = 0; i < keys.size(); i++)
                if (keys[i] == key)
                    return buses[i];
            keys.push_back(key);
            buses.emplace_back();
            return buses.back();
        };
        uint32_t n = lilv_plugin_get_num_ports(plugin);
        std::vector<float> defs(n, NAN);
        lilv_plugin_get_port_ranges_float(plugin, nullptr, nullptr, defs.data());
        bool ok = true;
        for (uint32_t i = 0; i < n; i++) {
            const LilvPort *lp = lilv_plugin_get_port_by_index(plugin, i);
            auto p = std::make_unique<Port>();
            p->index = i;
            p->symbol = lilv_node_as_string(lilv_port_get_symbol(plugin, lp));
            p->input = lilv_port_is_a(plugin, lp, n_input);
            if (lilv_port_is_a(plugin, lp, n_audio)) {
                p->kind = Port::Audio;
                bus_of(lp, p->input).push_back(i);
            } else if (lilv_port_is_a(plugin, lp, n_control)) {
                p->kind = Port::Control;
                p->value = std::isnan(defs[i]) ? 0.f : defs[i];
                if (LilvNode *d = lilv_port_get(plugin, lp, n_designation)) {
                    p->freewheel = !strcmp(lilv_node_as_uri(d), LV2_CORE__freeWheeling);
                    lilv_node_free(d);
                }
                p->mirror.store(p->value);
            } else if (lilv_port_is_a(plugin, lp, n_atom)) {
                p->kind = Port::Atom;
                uint32_t sz = kAtomBuf;
                if (LilvNodes *ms = lilv_port_get_value(plugin, lp, n_min_size)) {
                    if (lilv_nodes_size(ms))
                        sz = std::max<uint32_t>(sz, (uint32_t)lilv_node_as_int(lilv_nodes_get_first(ms)));
                    lilv_nodes_free(ms);
                }
                p->atom_size = sz;
                p->atom.assign((sz + 7) / 8, 0);
                if (p->input && lilv_port_supports_event(plugin, lp, n_midi) && !midi_port) {
                    p->midi = true;
                    midi_port = p.get();
                }
                if (p->input)
                    reset_atom_in(*p);
            } else if (lilv_port_is_a(plugin, lp, n_cv)) {
                p->kind = Port::Cv;
                p->cv.assign(kMaxFrames, 0.f);
            } else if (!lilv_port_has_property(plugin, lp, n_optional)) {
                if (err) *err = "port '" + p->symbol + "' has a type this host does not support";
                ok = false;
            }
            ports.push_back(std::move(p));
        }
        for (LilvNode *x : {n_audio, n_control, n_input, n_atom, n_cv, n_midi, n_min_size, n_optional, n_group, n_designation})
            lilv_node_free(x);
        for (size_t b = 0; b < audio_in.size(); b++)
            inputs.push_back({in_keys[b], (uint32_t)audio_in[b].size()});
        for (size_t b = 0; b < audio_out.size(); b++)
            outputs.push_back({out_keys[b], (uint32_t)audio_out[b].size()});
        midi_input = midi_port != nullptr;
        midi_dialect = "atom";
        return ok;
    }
};

bool is_instrument(LilvWorld *world, const LilvPlugin *p)
{
    LilvNode *inst = lilv_new_uri(world, LV2_CORE__InstrumentPlugin);
    const LilvPluginClasses *all = lilv_world_get_plugin_classes(world);
    const LilvPluginClass *c = lilv_plugin_get_class(p);
    bool r = false;
    for (int depth = 0; c && depth < 8 && !r; depth++) {
        r = lilv_node_equals(lilv_plugin_class_get_uri(c), inst);
        const LilvNode *parent = lilv_plugin_class_get_parent_uri(c);
        c = parent ? lilv_plugin_classes_get_by_uri(all, parent) : nullptr;
    }
    lilv_node_free(inst);
    return r;
}

// One pass over the manifests on LV2_PATH: the specification bundles (for
// the class hierarchy), and the bundles that name the URI (the plugin's own,
// and preset bundles that apply to it). Much faster than parsing every
// installed TTL, which lilv_world_load_all would do.
void scan_bundles(const std::string &uri, std::vector<std::string> &specs, std::vector<std::string> &naming)
{
    std::vector<std::string> dirs;
    const char *lp = getenv("LV2_PATH");
    const char *h = getenv("HOME");
    std::string path = lp ? lp : std::string(h ? h : "") + "/.lv2:/usr/local/lib/lv2:/usr/lib/lv2";
    std::stringstream ss(path);
    std::string d;
    while (std::getline(ss, d, ':'))
        if (!d.empty())
            dirs.push_back(d);
    std::string needle = "<" + uri + ">";
    for (auto &dir : dirs) {
        DIR *dp = opendir(dir.c_str());
        if (!dp)
            continue;
        while (dirent *de = readdir(dp)) {
            if (de->d_name[0] == '.')
                continue;
            std::string b = dir + "/" + de->d_name;
            std::ifstream f(b + "/manifest.ttl");
            if (!f)
                continue;
            std::string text((std::istreambuf_iterator<char>(f)), {});
            if (text.find("Specification") != std::string::npos)
                specs.push_back(b);
            if (!uri.empty() && text.find(needle) != std::string::npos)
                naming.push_back(b);
        }
        closedir(dp);
    }
}

void load_bundle(LilvWorld *w, const std::string &dir)
{
    std::string uri = "file://" + trim_slash(dir) + "/";
    LilvNode *b = lilv_new_uri(w, uri.c_str());
    lilv_world_load_bundle(w, b);
    lilv_node_free(b);
}

} // namespace

std::unique_ptr<PluginInstance> load_lv2(Host *h, const std::string &arg, const std::string &plugin_id, bool list_only)
{
    auto I = std::make_unique<Lv2Instance>(h);
    I->world = lilv_world_new();
    LilvWorld *w = I->world;
    struct stat st;
    bool bundle = stat(arg.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    std::string uri = bundle ? plugin_id : arg;
    std::string bundle_uri = bundle ? "file://" + trim_slash(arg) + "/" : "";
    std::vector<std::string> specs, naming;
    scan_bundles(uri, specs, naming);
    for (auto &b : specs)
        load_bundle(w, b);
    lilv_world_load_specifications(w);
    lilv_world_load_plugin_classes(w);
    if (bundle)
        load_bundle(w, arg);
    for (auto &b : naming)
        if (!bundle || trim_slash(b) != trim_slash(arg))
            load_bundle(w, b);
    if (!bundle && naming.empty())
        lilv_world_load_all(w);
    const LilvPlugins *all = lilv_world_get_all_plugins(w);
    std::vector<const LilvPlugin *> cands;
    LILV_FOREACH (plugins, i, all) {
        const LilvPlugin *p = lilv_plugins_get(all, i);
        if (bundle && strcmp(lilv_node_as_uri(lilv_plugin_get_bundle_uri(p)), bundle_uri.c_str()))
            continue;
        cands.push_back(p);
    }
    if (list_only) {
        fprintf(out, "[\n");
        for (size_t i = 0; i < cands.size(); i++) {
            LilvNode *n = lilv_plugin_get_name(cands[i]), *a = lilv_plugin_get_author_name(cands[i]);
            const LilvPluginClass *c = lilv_plugin_get_class(cands[i]);
            fprintf(out, "  {\"format\": \"lv2\", \"id\": %s, \"name\": %s, \"vendor\": %s, \"features\": [%s]}%s\n",
                   json_str(lilv_node_as_uri(lilv_plugin_get_uri(cands[i]))).c_str(), json_str(n ? lilv_node_as_string(n) : "").c_str(),
                   json_str(a ? lilv_node_as_string(a) : "").c_str(),
                   c ? json_str(lilv_node_as_string(lilv_plugin_class_get_label(c))).c_str() : "",
                   i + 1 < cands.size() ? "," : "");
            lilv_node_free(n);
            lilv_node_free(a);
        }
        fprintf(out, "]\n");
        fflush(out);
        exit(0);
    }
    for (auto *p : cands) {
        bool match = !uri.empty() ? uri == lilv_node_as_uri(lilv_plugin_get_uri(p)) : is_instrument(w, p);
        if (match) {
            I->plugin = p;
            break;
        }
    }
    if (!I->plugin && uri.empty() && !cands.empty())
        I->plugin = cands[0];
    if (!I->plugin)
        die("LV2 plugin %s not found%s", uri.empty() ? "(any)" : uri.c_str(), bundle ? (" in " + arg).c_str() : "");
    const LilvPlugin *p = I->plugin;

    // features we can't offer
    LilvNodes *req = lilv_plugin_get_required_features(p);
    static const char *const ok[] = {LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_BUF_SIZE__boundedBlockLength,
                                     LV2_WORKER__schedule, LV2_LOG__log, LV2_STATE__loadDefaultState,
                                     LV2_CORE_PREFIX "isLive", LV2_CORE__hardRTCapable, LV2_CORE__inPlaceBroken, nullptr};
    LILV_FOREACH (nodes, i, req) {
        const char *f = lilv_node_as_uri(lilv_nodes_get(req, i));
        bool have = false;
        for (auto *x = ok; *x && !have; x++)
            have = !strcmp(*x, f);
        if (!have)
            die("%s needs the LV2 feature %s, which this host does not provide", lilv_node_as_uri(lilv_plugin_get_uri(p)), f);
    }
    lilv_nodes_free(req);

    I->id = lilv_node_as_uri(lilv_plugin_get_uri(p));
    LilvNode *nm = lilv_plugin_get_name(p), *au = lilv_plugin_get_author_name(p);
    I->name = nm ? lilv_node_as_string(nm) : I->id;
    I->vendor = au ? lilv_node_as_string(au) : "";
    lilv_node_free(nm);
    lilv_node_free(au);
    char *bp = lilv_file_uri_parse(lilv_node_as_uri(lilv_plugin_get_bundle_uri(p)), nullptr);
    I->path = trim_slash(bp ? bp : "");
    lilv_free(bp);
    I->node_suffix = slug(I->name);
    I->instrument = is_instrument(w, p);

    I->u_atom_Sequence = I->m(LV2_ATOM__Sequence);
    I->u_atom_Chunk = I->m(LV2_ATOM__Chunk);
    I->u_atom_Float = I->m(LV2_ATOM__Float);
    I->u_atom_Double = I->m(LV2_ATOM__Double);
    I->u_atom_Int = I->m(LV2_ATOM__Int);
    I->u_atom_Long = I->m(LV2_ATOM__Long);
    I->u_atom_Bool = I->m(LV2_ATOM__Bool);
    I->u_midi_Event = I->m(LV2_MIDI__MidiEvent);
    I->u_min_block = I->m(LV2_BUF_SIZE__minBlockLength);
    I->u_max_block = I->m(LV2_BUF_SIZE__maxBlockLength);
    I->u_nominal_block = I->m(LV2_BUF_SIZE__nominalBlockLength);
    I->u_seq_size = I->m(LV2_BUF_SIZE__sequenceSize);
    I->u_sample_rate = I->m(LV2_PARAMETERS__sampleRate);
    I->u_frame_time = I->m(LV2_ATOM__frameTime);

    std::string err;
    if (!I->setup_ports(&err))
        die("%s: %s", I->id.c_str(), err.c_str());
    // Instantiated now at the usual rate, so a saved state can be restored
    // before the graph runs; activate() re-instantiates if the graph's rate
    // differs.
    if (!I->instantiate(48000, &err))
        die("%s: %s", I->id.c_str(), err.c_str());
    // state:loadDefaultState: the plugin's own default state, if it has one
    if (LilvState *def = lilv_state_new_from_world(w, &I->map, lilv_plugin_get_uri(p))) {
        I->restore(def, &err);
        lilv_state_free(def);
    }
    I->rescan_params();
    for (auto &pi : I->params)
        I->ports[pi.id]->mirror.store(I->ports[pi.id]->value);
    return I;
}

} // namespace bl
