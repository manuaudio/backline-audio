// backline-test-slotfx.vst3: the slot test effect (slotfx.h) as VST3, on the
// SDK's SingleComponentEffect: four RangeParameters and getLatencySamples.
// SPDX-License-Identifier: MIT
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

extern "C" {
#include "slotfx.h"
}

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID kUid0(0x42ac11a1, 0x6b6c4e2a, 0x9b1e7c3d, 0x5f0a2b50);
const FUID kUid37(0x42ac11a1, 0x6b6c4e2a, 0x9b1e7c3d, 0x5f0a2b51);
const FUID kUid300(0x42ac11a1, 0x6b6c4e2a, 0x9b1e7c3d, 0x5f0a2b52);

template <uint32_t Latency>
class SlotFx : public SingleComponentEffect {
public:
    static FUnknown *create(void *) { return (IAudioProcessor *)new SlotFx; }

    SlotFx() { slotfx_init(&fx, Latency); }

    tresult PLUGIN_API initialize(FUnknown *ctx) override
    {
        tresult r = SingleComponentEffect::initialize(ctx);
        if (r != kResultOk)
            return r;
        addAudioInput(STR16("in"), SpeakerArr::kStereo);
        addAudioOutput(STR16("out"), SpeakerArr::kStereo);
        for (uint32_t i = 0; i < SLOTFX_N_PARAMS; i++) {
            double mn, mx, def;
            slotfx_param_range(i, &mn, &mx, &def);
            String128 name;
            const char *n = slotfx_param_name(i);
            size_t k = 0;
            for (; n[k] && k < 127; k++)
                name[k] = (char16)n[k];
            name[k] = 0;
            parameters.addParameter(new RangeParameter(name, i, nullptr, mn, mx, def, (i == 1 || i == 3) ? 1 : 0,
                                                       ParameterInfo::kCanAutomate));
        }
        return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *ins, int32 nin, SpeakerArrangement *outs, int32 nout) override
    {
        return nin == 1 && nout == 1 && ins[0] == SpeakerArr::kStereo && outs[0] == SpeakerArr::kStereo ? kResultTrue
                                                                                                         : kResultFalse;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return Latency; }
    tresult PLUGIN_API setActive(TBool on) override
    {
        slotfx_reset(&fx);
        return SingleComponentEffect::setActive(on);
    }
    tresult PLUGIN_API process(ProcessData &d) override
    {
        if (IParameterChanges *pc = d.inputParameterChanges) {
            for (int32 q = 0; q < pc->getParameterCount(); q++) {
                IParamValueQueue *vq = pc->getParameterData(q);
                int32 off;
                ParamValue v;
                if (!vq || vq->getPointCount() < 1 || vq->getPoint(vq->getPointCount() - 1, off, v) != kResultOk)
                    continue;
                ParamID id = vq->getParameterId();
                if (id < SLOTFX_N_PARAMS) {
                    double mn, mx, def;
                    slotfx_param_range(id, &mn, &mx, &def);
                    *slotfx_param(&fx, id) = mn + v * (mx - mn);
                }
            }
        }
        if (d.numSamples <= 0 || d.numInputs < 1 || d.numOutputs < 1)
            return kResultOk;
        AudioBusBuffers &i = d.inputs[0], &o = d.outputs[0];
        slotfx_run(&fx, i.numChannels > 0 ? i.channelBuffers32[0] : nullptr, i.numChannels > 1 ? i.channelBuffers32[1] : nullptr,
                   o.numChannels > 0 ? o.channelBuffers32[0] : nullptr, o.numChannels > 1 ? o.channelBuffers32[1] : nullptr,
                   (uint32_t)d.numSamples);
        o.silenceFlags = 0;
        return kResultOk;
    }
    // state: the gain only (the misbehaving switches never persist)
    tresult PLUGIN_API setState(IBStream *s) override
    {
        double g = 0;
        int32 n = 0;
        if (!s || s->read(&g, sizeof(g), &n) != kResultOk || n != (int32)sizeof(g))
            return kResultFalse;
        fx.gain_db = g;
        double mn, mx, def;
        slotfx_param_range(0, &mn, &mx, &def);
        setParamNormalized(0, (g - mn) / (mx - mn));
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream *s) override
    {
        double g = fx.gain_db;
        int32 n = 0;
        return s && s->write(&g, sizeof(g), &n) == kResultOk ? kResultOk : kResultFalse;
    }

private:
    struct slotfx fx;
};

} // namespace

bool InitModule() { return true; }
bool DeinitModule() { return true; }

BEGIN_FACTORY_DEF("Backline", "https://github.com/manuaudio/backline-audio", "mailto:188948351+manuaudio@users.noreply.github.com")
DEF_CLASS2(INLINE_UID_FROM_FUID(kUid0), PClassInfo::kManyInstances, kVstAudioEffectClass, "Backline Test Slot FX",
           Vst::kDistributable, "Fx|Tools", "0.1.0", kVstVersionString, SlotFx<0>::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(kUid37), PClassInfo::kManyInstances, kVstAudioEffectClass, "Backline Test Slot FX (37 frames)",
           Vst::kDistributable, "Fx|Tools", "0.1.0", kVstVersionString, SlotFx<37>::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(kUid300), PClassInfo::kManyInstances, kVstAudioEffectClass, "Backline Test Slot FX (300 frames)",
           Vst::kDistributable, "Fx|Tools", "0.1.0", kVstVersionString, SlotFx<300>::create)
END_FACTORY
