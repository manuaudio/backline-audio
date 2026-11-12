// The worker's one internal plugin interface. Each format (CLAP, VST3, LV2)
// is one adapter behind it; the worker (PipeWire node, D-Bus, OSC, state
// files, linking) never sees a format.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct pw_loop;

namespace bl {

constexpr uint32_t kMaxFrames = 8192; // PipeWire's largest quantum
constexpr uint32_t kMaxEvents = 2048; // per cycle

struct ParamInfo {
    uint32_t id = 0;            // format id: CLAP param id, VST3 ParamID, LV2 port index
    std::string name, module;   // module: CLAP module path, VST3 unit name, "" for LV2
    double min = 0, max = 1, def = 0;
    bool readonly = false, stepped = false;
    void *cookie = nullptr;     // adapter-private (CLAP param cookie)
};

struct BusInfo {
    std::string name;
    uint32_t channels = 0;
};

struct PresetInfo {
    std::string id;             // what LoadPreset takes
    std::string name;
    std::string location;       // a file, or where inside the plugin it lives
};

// Events for one cycle, in the order the worker collected them: parameter
// changes first (time 0), then MIDI in timeline order.
struct MidiEv {
    uint32_t time;
    uint8_t size;
    uint8_t data[3];
};
struct ParamEv {
    uint32_t index;             // into the adapter's params
    uint32_t id;
    void *cookie;
    double value;               // adapter units (see PluginInstance::prepare_set)
};

struct ProcessArgs {
    uint32_t frames;
    int64_t steady;             // frames processed since start
    float *const *const *ins;   // ins[bus][channel], never null (zeros when unlinked)
    float *const *const *outs;  // outs[bus][channel], never null
    const ParamEv *params;
    uint32_t n_params;
    const MidiEv *midi;
    uint32_t n_midi;
};

// What the worker offers an adapter.
class Host {
public:
    virtual ~Host() = default;
    virtual void request_restart() = 0;          // any thread: deactivate + activate on main
    virtual void request_callback() = 0;         // any thread: PluginInstance::on_main_thread on main
    virtual void params_changed() = 0;           // main: the parameter list or values changed
    virtual void mark_dirty() = 0;               // main: the plugin state changed
    virtual void param_from_plugin(uint32_t id, double value) = 0; // audio (or main): the plugin moved a parameter
    virtual int set_param(uint32_t id, double value) = 0;          // main: as SetParam (public units)
    virtual void count_dropped() = 0;            // audio: an event did not fit
    virtual bool is_main_thread() = 0;
    virtual bool is_audio_thread() = 0;
    virtual pw_loop *main_loop() = 0;            // for plugin timers
    virtual void log(const char *fmt, ...) __attribute__((format(printf, 2, 3))) = 0;
};

class PluginInstance {
public:
    virtual ~PluginInstance() = default;

    // ---- identity (valid after load)
    virtual const char *format() const = 0;     // "clap", "vst3", "lv2"
    std::string id;                             // CLAP id, VST3 class id (hex), LV2 URI
    std::string name, vendor;
    std::string path;                           // .clap file, .vst3 bundle, .lv2 bundle
    std::string node_suffix;                    // default node name: backline.<suffix>
    bool instrument = false;
    bool midi_input = false;                    // wants the midi_in port
    std::string midi_dialect;                   // for the log: "midi", "clap", "vst3 events", "atom"
    std::vector<BusInfo> inputs, outputs;
    std::vector<ParamInfo> params;

    // ---- main thread
    virtual void rescan_params() = 0;           // refills params
    // Current value in public units; false: not readable (use the cache).
    virtual bool read_param(const ParamInfo &p, double &value) = 0;
    // True if read_param is current while the audio thread runs (VST3: the
    // controller; LV2: the port values). CLAP: false, the worker's cache of
    // what process() reported is.
    virtual bool live_values() const { return false; }
    // A set from D-Bus/OSC, already clamped. Returns the value to send to the
    // audio thread (VST3: normalized; others: as is).
    virtual double prepare_set(const ParamInfo &p, double value) { return value; }
    // A set while not active (no audio thread to deliver it).
    virtual bool set_param_inactive(const ParamInfo &p, double value) = 0;
    // A value the plugin reported (audio thread units), back on main; returns
    // the public value.
    virtual double plugin_value_to_public(uint32_t id, double v) { return v; }

    virtual bool activate(uint32_t rate, uint32_t max_frames) = 0;
    virtual void deactivate() = 0;
    virtual void on_main_thread() {}            // after Host::request_callback

    virtual bool save_state(std::vector<uint8_t> &out, std::string *err) = 0;
    virtual bool load_state(const std::vector<uint8_t> &in, std::string *err) = 0;
    // True if load_state/load_preset must not run while process() may run
    // (LV2 restore is in the instantiation class); the worker pauses audio.
    virtual bool restore_needs_pause() const { return false; }

    virtual std::vector<PresetInfo> list_presets() { return {}; }
    virtual bool load_preset(const PresetInfo &p, std::string *err)
    {
        if (err) *err = "this plugin has no presets";
        return false;
    }
    // Saves the current state as a preset file; returns its path.
    virtual bool save_preset(const std::string &name_or_path, std::string &path_out, std::string *err)
    {
        if (err) *err = std::string("saving presets is not supported for ") + format();
        return false;
    }
    std::vector<std::string> extra_preset_dirs; // --preset-dir

    // ---- audio thread
    virtual bool start_processing() = 0;
    virtual void stop_processing() = 0;
    virtual bool process(const ProcessArgs &a) = 0; // false: error, the worker outputs silence
};

// Loaders. path: as given (resolved by the worker), plugin_id: "" = first
// instrument, else first plugin. list_only: print the plugins as JSON, exit.
std::unique_ptr<PluginInstance> load_clap(Host *h, const std::string &path, const std::string &plugin_id, bool list_only);
#ifdef BL_HAVE_VST3
std::unique_ptr<PluginInstance> load_vst3(Host *h, const std::string &path, const std::string &plugin_id, bool list_only);
#endif
#ifdef BL_HAVE_LV2
// uri_or_bundle: an LV2 URI, or a .lv2 bundle directory (then plugin_id may
// name the URI).
std::unique_ptr<PluginInstance> load_lv2(Host *h, const std::string &uri_or_bundle, const std::string &plugin_id, bool list_only);
#endif

// Shared helpers
std::string slug(const std::string &s);         // "Surge XT" -> "surge-xt"
std::string json_str(const std::string &v);
[[noreturn]] void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace bl
