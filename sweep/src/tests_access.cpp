#include "tests_access.h"

#include "tests_common.h"

namespace {

template <class I>
I* qi(IUnknown* obj) {
    I* p = nullptr;
    if (obj && SUCCEEDED(obj->QueryInterface(__uuidof(I), reinterpret_cast<void**>(&p)))) return p;
    return nullptr;
}

template <class I>
I* addRef(I* p) {
    if (p) p->AddRef();
    return p;
}

IBMDSwitcherFairlightAudioInput* fairlightInput(Ctx& c, BMDSwitcherAudioInputId id) {
    Com<IBMDSwitcherFairlightAudioMixer> mixer(qi<IBMDSwitcherFairlightAudioMixer>(c.s.sw));
    if (!mixer) return nullptr;
    Com<IBMDSwitcherFairlightAudioInputIterator> it;
    if (FAILED(SDK_CALL(IBMDSwitcherFairlightAudioMixer, mixer.p, CreateIterator,
                        __uuidof(IBMDSwitcherFairlightAudioInputIterator), reinterpret_cast<void**>(it.out()))) || !it)
        return nullptr;
    IBMDSwitcherFairlightAudioInput* in = nullptr;
    SDK_CALL(IBMDSwitcherFairlightAudioInputIterator, it.p, GetById, id, &in);
    return in;
}

// The first source of HDMI 1's Fairlight input.
IBMDSwitcherFairlightAudioSource* fairlightSource(Ctx& c) {
    Com<IBMDSwitcherFairlightAudioInput> in(fairlightInput(c, 1));
    if (!in) return nullptr;
    Com<IBMDSwitcherFairlightAudioSourceIterator> it;
    if (FAILED(SDK_CALL(IBMDSwitcherFairlightAudioInput, in.p, CreateIterator,
                        __uuidof(IBMDSwitcherFairlightAudioSourceIterator), reinterpret_cast<void**>(it.out()))) || !it)
        return nullptr;
    IBMDSwitcherFairlightAudioSource* src = nullptr;
    SDK_CALL(IBMDSwitcherFairlightAudioSourceIterator, it.p, Next, &src);
    return src;
}

template <class I>
I* effect(Ctx& c) {
    Com<IBMDSwitcherFairlightAudioSource> src(fairlightSource(c));
    I* e = nullptr;
    if (src) SDK_CALL(IBMDSwitcherFairlightAudioSource, src.p, GetEffect, __uuidof(I), reinterpret_cast<void**>(&e));
    return e;
}

template <class I>
I* processor(Ctx& c) {
    Com<IBMDSwitcherFairlightAudioDynamicsProcessor> dyn(effect<IBMDSwitcherFairlightAudioDynamicsProcessor>(c));
    I* p = nullptr;
    if (dyn) SDK_CALL(IBMDSwitcherFairlightAudioDynamicsProcessor, dyn.p, GetProcessor, __uuidof(I), reinterpret_cast<void**>(&p));
    return p;
}

IBMDSwitcherKeyFlyKeyFrameParameters* keyFrame(Ctx& c, BMDSwitcherFlyKeyFrame which) {
    IBMDSwitcherKeyFlyKeyFrameParameters* kf = nullptr;
    if (c.s.fly) SDK_CALL(IBMDSwitcherKeyFlyParameters, c.s.fly, GetKeyFrameParameters, which, &kf);
    return kf;
}

IBMDSwitcherFairlightAudioEqualizerBand* firstBand(Ctx& c) {
    Com<IBMDSwitcherFairlightAudioEqualizer> eq(effect<IBMDSwitcherFairlightAudioEqualizer>(c));
    if (!eq) return nullptr;
    Com<IBMDSwitcherFairlightAudioEqualizerBandIterator> it;
    if (FAILED(SDK_CALL(IBMDSwitcherFairlightAudioEqualizer, eq.p, CreateIterator,
                        __uuidof(IBMDSwitcherFairlightAudioEqualizerBandIterator), reinterpret_cast<void**>(it.out()))) || !it)
        return nullptr;
    IBMDSwitcherFairlightAudioEqualizerBand* band = nullptr;
    SDK_CALL(IBMDSwitcherFairlightAudioEqualizerBandIterator, it.p, Next, &band);
    return band;
}

} // namespace

void* accessObject(Ctx& c, const char* keyText) {
    c.needConnection();
    const QString key(keyText);
    if (key == "IBMDSwitcher") return addRef(c.s.sw);
    if (key == "IBMDSwitcherMixEffectBlock") return addRef(c.s.me);
    if (key == "IBMDSwitcherInput") return addRef(c.s.input(1));
    if (key == "IBMDSwitcherInputColor#1") return qi<IBMDSwitcherInputColor>(c.s.input(2001));
    if (key == "IBMDSwitcherInputColor#2") return qi<IBMDSwitcherInputColor>(c.s.input(2002));
    if (key == "IBMDSwitcherInputAux") return qi<IBMDSwitcherInputAux>(c.s.input(8001));
    if (key == "IBMDSwitcherKeyDVEParameters") return addRef(c.s.dve);
    if (key == "IBMDSwitcherKeyLumaParameters") return qi<IBMDSwitcherKeyLumaParameters>(c.s.key);
    if (key == "IBMDSwitcherKeyAdvancedChromaParameters") return qi<IBMDSwitcherKeyAdvancedChromaParameters>(c.s.key);
    if (key == "IBMDSwitcherKeyPatternParameters") return qi<IBMDSwitcherKeyPatternParameters>(c.s.key);
    if (key == "IBMDSwitcherKeyFlyKeyFrameParameters#A") return keyFrame(c, bmdSwitcherFlyKeyFrameA);
    if (key == "IBMDSwitcherKeyFlyKeyFrameParameters#B") return keyFrame(c, bmdSwitcherFlyKeyFrameB);
    if (key == "IBMDSwitcherTransitionMixParameters") return qi<IBMDSwitcherTransitionMixParameters>(c.s.me);
    if (key == "IBMDSwitcherTransitionDipParameters") return qi<IBMDSwitcherTransitionDipParameters>(c.s.me);
    if (key == "IBMDSwitcherTransitionWipeParameters") return qi<IBMDSwitcherTransitionWipeParameters>(c.s.me);
    if (key == "IBMDSwitcherTransitionDVEParameters") return qi<IBMDSwitcherTransitionDVEParameters>(c.s.me);
    if (key == "IBMDSwitcherFairlightAudioMixer") return qi<IBMDSwitcherFairlightAudioMixer>(c.s.sw);
    if (key == "IBMDSwitcherFairlightAudioInput") return fairlightInput(c, 1);
    if (key == "IBMDSwitcherFairlightAnalogAudioInput") {
        Com<IBMDSwitcherFairlightAudioInput> mic(fairlightInput(c, 1301));
        return qi<IBMDSwitcherFairlightAnalogAudioInput>(mic.p);
    }
    if (key == "IBMDSwitcherFairlightAudioSource") return fairlightSource(c);
    if (key == "IBMDSwitcherFairlightAudioEqualizer") return effect<IBMDSwitcherFairlightAudioEqualizer>(c);
    if (key == "IBMDSwitcherFairlightAudioEqualizerBand") return firstBand(c);
    if (key == "IBMDSwitcherFairlightAudioDynamicsProcessor") return effect<IBMDSwitcherFairlightAudioDynamicsProcessor>(c);
    if (key == "IBMDSwitcherFairlightAudioCompressor") return processor<IBMDSwitcherFairlightAudioCompressor>(c);
    if (key == "IBMDSwitcherFairlightAudioLimiter") return processor<IBMDSwitcherFairlightAudioLimiter>(c);
    if (key == "IBMDSwitcherFairlightAudioExpander") return processor<IBMDSwitcherFairlightAudioExpander>(c);
    if (key == "IBMDSwitcherStillCapture") {
        // Offered by one of these, depending on the model.
        if (auto* p = qi<IBMDSwitcherStillCapture>(c.s.sw)) return p;
        Com<IBMDSwitcherMediaPool> pool(qi<IBMDSwitcherMediaPool>(c.s.sw));
        if (auto* p = qi<IBMDSwitcherStillCapture>(pool.p)) return p;
        if (pool) {
            Com<IBMDSwitcherStills> stills;
            SDK_CALL(IBMDSwitcherMediaPool, pool.p, GetStills, stills.out());
            if (auto* p = qi<IBMDSwitcherStillCapture>(stills.p)) return p;
        }
        return qi<IBMDSwitcherStillCapture>(c.s.me);
    }
    if (key == "IBMDSwitcherMediaPool") return qi<IBMDSwitcherMediaPool>(c.s.sw);
    return nullptr;
}

void restoreAll(Ctx& c, Saved& saved) {
    for (int pass = 0; pass < 2; ++pass) {
        bool any = false;
        for (SavedValue& v : saved.values) {
            if (!v.unchanged()) {
                v.put();
                any = true;
            }
        }
        if (!any) break;
        c.settle();
    }
    for (SavedValue& v : saved.values)
        c.expect(v.unchanged(), v.name + " was not put back");
}

void saveKeyFrameStored(Ctx& c, Saved& saved, const char* key) {
    if (!c.s.fly) return;
    BMDSwitcherFlyKeyFrame which = QByteArray(key).endsWith("#B") ? bmdSwitcherFlyKeyFrameB : bmdSwitcherFlyKeyFrameA;
    IBMDSwitcherKeyFlyParameters* fly = c.s.fly;
    BOOL stored = FALSE;
    if (FAILED(fly->IsKeyFrameStored(which, &stored))) return;
    fly->AddRef();
    saved.refs.push_back(fly);
    saved.values.push_back({ QString("keyframe %1 stored").arg(which == bmdSwitcherFlyKeyFrameB ? "B" : "A"),
                             [fly, which, stored] { BOOL now = FALSE; return SUCCEEDED(fly->IsKeyFrameStored(which, &now)) && (now != FALSE) == (stored != FALSE); },
                             [fly, which, stored] { return stored ? fly->StoreAsKeyFrame(which) : fly->ClearKeyFrame(which); } });
}
