// backline-test-beatclick.vst3: the beat-click test plugin (beatclick.h) as
// VST3, on the SDK's SingleComponentEffect. It reads ProcessContext:
// projectTimeMusic (quarter notes), tempo and kPlaying.
// SPDX-License-Identifier: MIT
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

extern "C" {
#include "beatclick.h"
}

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID kUid(0x42ac11a1, 0x6b6c4e2a, 0x9b1e7c3d, 0x5f0a2b44);

class BeatClick : public SingleComponentEffect {
public:
    static FUnknown *create(void *) { return (IAudioProcessor *)new BeatClick; }

    tresult PLUGIN_API initialize(FUnknown *ctx) override
    {
        tresult r = SingleComponentEffect::initialize(ctx);
        if (r != kResultOk)
            return r;
        addAudioOutput(STR16("out"), SpeakerArr::kStereo);
        return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *, int32 nin, SpeakerArrangement *outs, int32 nout) override
    {
        return nin == 0 && nout == 1 && outs[0] == SpeakerArr::kStereo ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API setupProcessing(ProcessSetup &s) override
    {
        rate = s.sampleRate;
        return SingleComponentEffect::setupProcessing(s);
    }
    tresult PLUGIN_API setActive(TBool on) override
    {
        beatclick_reset(&bc);
        return SingleComponentEffect::setActive(on);
    }
    tresult PLUGIN_API process(ProcessData &d) override
    {
        if (d.numOutputs < 1 || d.outputs[0].numChannels < 2 || d.numSamples <= 0)
            return kResultOk;
        float *click = d.outputs[0].channelBuffers32[0], *beat = d.outputs[0].channelBuffers32[1];
        const ProcessContext *c = d.processContext;
        bool rolling = c && (c->state & ProcessContext::kPlaying) && (c->state & ProcessContext::kTempoValid) &&
                       (c->state & ProcessContext::kProjectTimeMusicValid);
        double b0 = rolling ? c->projectTimeMusic : 0, step = rolling ? c->tempo / 60.0 / rate : 0;
        beatclick_run(&bc, rolling, b0, step, click, beat, (uint32_t)d.numSamples);
        d.outputs[0].silenceFlags = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream *) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream *) override { return kResultOk; }

private:
    double rate = 48000;
    struct beatclick bc{};
};

} // namespace

bool InitModule() { return true; }
bool DeinitModule() { return true; }

BEGIN_FACTORY_DEF("Backline", "https://github.com/manuaudio/backline-audio", "mailto:188948351+manuaudio@users.noreply.github.com")
DEF_CLASS2(INLINE_UID_FROM_FUID(kUid), PClassInfo::kManyInstances, kVstAudioEffectClass, "Backline Test Beat Click",
           Vst::kDistributable, "Instrument|Tools", "0.1.0", kVstVersionString, BeatClick::create)
END_FACTORY
