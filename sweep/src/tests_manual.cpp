// Category-3 SDK methods the generator can't handle (several arguments,
// arrays, keyframe actions, iterator lookups). Everything changed is put back.
#include "tests_access.h"
#include "tests_common.h"

#include <QElapsedTimer>
#include <QJsonObject>

namespace {

const BMDSwitcherVideoMode kVideoModes[] = {
    bmdSwitcherVideoMode525i5994NTSC, bmdSwitcherVideoMode625i50PAL, bmdSwitcherVideoMode525i5994Anamorphic,
    bmdSwitcherVideoMode625i50Anamorphic, bmdSwitcherVideoMode720p50, bmdSwitcherVideoMode720p5994,
    bmdSwitcherVideoMode720p60, bmdSwitcherVideoMode1080i50, bmdSwitcherVideoMode1080i5994,
    bmdSwitcherVideoMode1080i60, bmdSwitcherVideoMode1080p2398, bmdSwitcherVideoMode1080p24,
    bmdSwitcherVideoMode1080p25, bmdSwitcherVideoMode1080p2997, bmdSwitcherVideoMode1080p30,
    bmdSwitcherVideoMode1080p50, bmdSwitcherVideoMode1080p5994, bmdSwitcherVideoMode1080p60,
    bmdSwitcherVideoMode4KHDp2398, bmdSwitcherVideoMode4KHDp24, bmdSwitcherVideoMode4KHDp25,
    bmdSwitcherVideoMode4KHDp2997, bmdSwitcherVideoMode4KHDp30, bmdSwitcherVideoMode4KHDp50,
    bmdSwitcherVideoMode4KHDp5994, bmdSwitcherVideoMode4KHDp60, bmdSwitcherVideoMode8KHDp2398,
    bmdSwitcherVideoMode8KHDp24, bmdSwitcherVideoMode8KHDp25, bmdSwitcherVideoMode8KHDp2997,
    bmdSwitcherVideoMode8KHDp30, bmdSwitcherVideoMode8KHDp50, bmdSwitcherVideoMode8KHDp5994,
    bmdSwitcherVideoMode8KHDp60,
};

// Every value of a fly keyframe, so keyframe actions can be undone.
using KfGet = HRESULT (STDMETHODCALLTYPE IBMDSwitcherKeyFlyKeyFrameParameters::*)(double*);
using KfSet = HRESULT (STDMETHODCALLTYPE IBMDSwitcherKeyFlyKeyFrameParameters::*)(double);
struct KfField { const char* name; KfGet get; KfSet set; };
#define KF(n) { #n, &IBMDSwitcherKeyFlyKeyFrameParameters::Get##n, &IBMDSwitcherKeyFlyKeyFrameParameters::Set##n }
const KfField kKeyFrameFields[] = {
    KF(SizeX), KF(SizeY), KF(PositionX), KF(PositionY), KF(Rotation), KF(BorderWidthOut), KF(BorderWidthIn),
    KF(BorderSoftnessOut), KF(BorderSoftnessIn), KF(BorderBevelSoftness), KF(BorderBevelPosition),
    KF(BorderOpacity), KF(BorderHue), KF(BorderSaturation), KF(BorderLuma), KF(BorderLightSourceDirection),
    KF(BorderLightSourceAltitude), KF(MaskTop), KF(MaskBottom), KF(MaskLeft), KF(MaskRight),
};
#undef KF

struct KeyFrameSave {
    BOOL stored = FALSE;
    std::vector<std::pair<HRESULT, double>> values;
};

KeyFrameSave saveKeyFrame(Ctx& c, BMDSwitcherFlyKeyFrame which) {
    KeyFrameSave save;
    SDK_CALL(IBMDSwitcherKeyFlyParameters, c.s.fly, IsKeyFrameStored, which, &save.stored);
    Com<IBMDSwitcherKeyFlyKeyFrameParameters> kf;
    if (SUCCEEDED(SDK_CALL(IBMDSwitcherKeyFlyParameters, c.s.fly, GetKeyFrameParameters, which, kf.out())) && kf)
        for (const KfField& f : kKeyFrameFields) {
            double v = 0;
            HRESULT hr = (kf.p->*f.get)(&v);
            save.values.emplace_back(hr, v);
        }
    return save;
}

void restoreKeyFrame(Ctx& c, BMDSwitcherFlyKeyFrame which, const KeyFrameSave& save) {
    Com<IBMDSwitcherKeyFlyKeyFrameParameters> kf;
    if (FAILED(SDK_CALL(IBMDSwitcherKeyFlyParameters, c.s.fly, GetKeyFrameParameters, which, kf.out())) || !kf) return;
    for (size_t i = 0; i < save.values.size(); ++i)
        if (SUCCEEDED(save.values[i].first)) (kf.p->*kKeyFrameFields[i].set)(save.values[i].second);
    if (!save.stored) SDK_CALL(IBMDSwitcherKeyFlyParameters, c.s.fly, ClearKeyFrame, which);
    c.settle();
    KeyFrameSave now = saveKeyFrame(c, which);
    bool same = now.stored == save.stored;
    for (size_t i = 0; same && i < save.values.size(); ++i)
        if (SUCCEEDED(save.values[i].first)) same = ::same(now.values[i].second, save.values[i].second);
    c.expect(same, QString("keyframe %1 was not put back").arg(which == bmdSwitcherFlyKeyFrameA ? "A" : "B"));
}

QJsonObject keyFrameState(Ctx& c, BMDSwitcherFlyKeyFrame which) {
    KeyFrameSave s = saveKeyFrame(c, which);
    QJsonObject o{ { "stored", s.stored != FALSE } };
    for (size_t i = 0; i < s.values.size(); ++i)
        o[kKeyFrameFields[i].name] = SUCCEEDED(s.values[i].first) ? js(s.values[i].second) : QJsonValue("error " + hrText(s.values[i].first));
    return o;
}

} // namespace

void registerManualTests() {
    // ── Switcher: capabilities with two video modes, time code ──
    for (bool multiView : { false, true }) {
        addTest(multiView ? "m.sw.supportsMultiViewMode" : "m.sw.supportsDownConvertedHDMode",
                multiView ? "DoesSupportMultiViewVideoMode for every mode" : "DoesSupportDownConvertedHDVideoMode for every mode",
                [multiView](Ctx& c) {
                    c.needConnection();
                    BMDSwitcherVideoMode core{};
                    SDK_CALL(IBMDSwitcher, c.s.sw, GetVideoMode, &core);
                    QJsonObject result;
                    for (BMDSwitcherVideoMode m : kVideoModes) {
                        BOOL ok = FALSE;
                        HRESULT hr = multiView ? SDK_CALL(IBMDSwitcher, c.s.sw, DoesSupportMultiViewVideoMode, core, m, &ok)
                                               : SDK_CALL(IBMDSwitcher, c.s.sw, DoesSupportDownConvertedHDVideoMode, core, m, &ok);
                        result[fourcc(static_cast<uint32_t>(m))] = SUCCEEDED(hr) ? QJsonValue(ok != FALSE) : QJsonValue("error " + hrText(hr));
                    }
                    c.observe("supported", result);
                });
    }

    struct TimeCode { unsigned char h, m, s, f; const char* id; };
    for (TimeCode tc : { TimeCode{ 1, 2, 3, 4, "01020304" }, TimeCode{ 23, 59, 59, 23, "23595923" },
                         TimeCode{ 25, 61, 61, 99, "bad" } }) {
        addTest(QString("m.sw.timeCode.%1").arg(tc.id), QString("SetTimeCode %1:%2:%3:%4").arg(tc.h).arg(tc.m).arg(tc.s).arg(tc.f),
                [tc](Ctx& c) {
                    c.needConnection();
                    // Time code runs on, so "put back" is the old value plus the time taken.
                    unsigned char h = 0, m = 0, s = 0, f = 0;
                    BOOL drop = FALSE;
                    HRESULT h0 = SDK_CALL(IBMDSwitcher, c.s.sw, GetTimeCode, &h, &m, &s, &f, &drop);
                    QElapsedTimer t;
                    t.start();
                    c.hr("set", SDK_CALL(IBMDSwitcher, c.s.sw, SetTimeCode, tc.h, tc.m, tc.s, tc.f));
                    c.hr("request", SDK_CALL(IBMDSwitcher, c.s.sw, RequestTimeCode));
                    c.settle();
                    unsigned char h2 = 0, m2 = 0, s2 = 0, f2 = 0;
                    BOOL d2 = FALSE;
                    if (SUCCEEDED(SDK_CALL(IBMDSwitcher, c.s.sw, GetTimeCode, &h2, &m2, &s2, &f2, &d2)))
                        c.observe("readback (informational)", QString("%1:%2:%3").arg(h2).arg(m2).arg(s2));
                    if (SUCCEEDED(h0)) {
                        int secs = h * 3600 + m * 60 + s + static_cast<int>(t.elapsed() / 1000);
                        SDK_CALL(IBMDSwitcher, c.s.sw, SetTimeCode, static_cast<unsigned char>((secs / 3600) % 24),
                                 static_cast<unsigned char>((secs / 60) % 60), static_cast<unsigned char>(secs % 60), f);
                    }
                });
    }

    // ── Advanced chroma: sampled colour (three values at once) ──
    struct Ycc { double y, cb, cr; const char* id; };
    for (Ycc v : { Ycc{ 0.5, 0.1, -0.1, "mid" }, Ycc{ 0, 0, 0, "zero" }, Ycc{ 2, -2, 2, "bad" } }) {
        addTest(QString("m.achroma.sampledColor.%1").arg(v.id), QString("SetSampledColor %1 %2 %3").arg(v.y).arg(v.cb).arg(v.cr),
                [v](Ctx& c) {
                    Com<IBMDSwitcherKeyAdvancedChromaParameters> o(
                        static_cast<IBMDSwitcherKeyAdvancedChromaParameters*>(accessObject(c, "IBMDSwitcherKeyAdvancedChromaParameters")));
                    if (!o) c.skip("no advanced chroma parameters");
                    double y0, cb0, cr0;
                    if (FAILED(SDK_CALL(IBMDSwitcherKeyAdvancedChromaParameters, o.p, GetSampledColor, &y0, &cb0, &cr0)))
                        c.skip("the current colour can't be read, so it could not be put back: not set");
                    c.hr("set", SDK_CALL(IBMDSwitcherKeyAdvancedChromaParameters, o.p, SetSampledColor, v.y, v.cb, v.cr));
                    c.settle();
                    double y, cb, cr;
                    if (SUCCEEDED(SDK_CALL(IBMDSwitcherKeyAdvancedChromaParameters, o.p, GetSampledColor, &y, &cb, &cr)))
                        c.observe("readback", QJsonObject{ { "y", y }, { "cb", cb }, { "cr", cr } });
                    SDK_CALL(IBMDSwitcherKeyAdvancedChromaParameters, o.p, SetSampledColor, y0, cb0, cr0);
                    c.settle();
                    SDK_CALL(IBMDSwitcherKeyAdvancedChromaParameters, o.p, GetSampledColor, &y, &cb, &cr);
                    c.expect(same(y, y0) && same(cb, cb0) && same(cr, cr0), "the sampled colour was not put back");
                });
    }

    // ── Fly keyframes: store and clear (A, B, both, and a bad one) ──
    struct Kf { BMDSwitcherFlyKeyFrame which; const char* id; };
    for (bool store : { true, false }) {
        for (Kf k : { Kf{ bmdSwitcherFlyKeyFrameA, "a" }, Kf{ bmdSwitcherFlyKeyFrameB, "b" },
                      Kf{ static_cast<BMDSwitcherFlyKeyFrame>(bmdSwitcherFlyKeyFrameA | bmdSwitcherFlyKeyFrameB), "ab" },
                      Kf{ bmdSwitcherFlyKeyFrameFull, "bad" } }) {
            addTest(QString("m.fly.%1KeyFrame.%2").arg(store ? "store" : "clear", k.id),
                    QString("%1KeyFrame(%2) (keyframes put back after)").arg(store ? "StoreAs" : "Clear", k.id),
                    [store, k](Ctx& c) {
                        c.needConnection();
                        if (!c.s.fly) c.skip("no fly parameters");
                        KeyFrameSave a = saveKeyFrame(c, bmdSwitcherFlyKeyFrameA), b = saveKeyFrame(c, bmdSwitcherFlyKeyFrameB);
                        c.hr("call", store ? SDK_CALL(IBMDSwitcherKeyFlyParameters, c.s.fly, StoreAsKeyFrame, k.which)
                                           : SDK_CALL(IBMDSwitcherKeyFlyParameters, c.s.fly, ClearKeyFrame, k.which));
                        c.settle();
                        c.observe("a", keyFrameState(c, bmdSwitcherFlyKeyFrameA));
                        c.observe("b", keyFrameState(c, bmdSwitcherFlyKeyFrameB));
                        restoreKeyFrame(c, bmdSwitcherFlyKeyFrameA, a);
                        restoreKeyFrame(c, bmdSwitcherFlyKeyFrameB, b);
                    });
        }
    }

    // ── Iterators: lookups by id ──
    addTest("m.iter.inputById", "IBMDSwitcherInputIterator::GetById (good and bad ids)", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherInputIterator> it;
        c.hr("create", SDK_CALL(IBMDSwitcher, c.s.sw, CreateIterator, __uuidof(IBMDSwitcherInputIterator), reinterpret_cast<void**>(it.out())));
        if (!it) return;
        QJsonObject r;
        for (BMDSwitcherInputId id : { 0LL, 1LL, 4LL, 1000LL, 2001LL, 8001LL, 10010LL, 11001LL, 5LL, 9999LL, -1LL }) {
            Com<IBMDSwitcherInput> in;
            r[QString::number(id)] = hrText(SDK_CALL(IBMDSwitcherInputIterator, it.p, GetById, id, in.out()));
        }
        c.observe("getById", r);
    });
    addTest("m.iter.fairlightInputById", "IBMDSwitcherFairlightAudioInputIterator::GetById", [](Ctx& c) {
        Com<IBMDSwitcherFairlightAudioMixer> mixer(static_cast<IBMDSwitcherFairlightAudioMixer*>(accessObject(c, "IBMDSwitcherFairlightAudioMixer")));
        if (!mixer) c.skip("no Fairlight mixer");
        Com<IBMDSwitcherFairlightAudioInputIterator> it;
        c.hr("create", SDK_CALL(IBMDSwitcherFairlightAudioMixer, mixer.p, CreateIterator,
                                __uuidof(IBMDSwitcherFairlightAudioInputIterator), reinterpret_cast<void**>(it.out())));
        if (!it) return;
        QJsonObject r;
        for (BMDSwitcherAudioInputId id : { 1LL, 4LL, 1301LL, 1302LL, 0LL, 5LL, 2001LL, 9999LL }) {
            Com<IBMDSwitcherFairlightAudioInput> in;
            r[QString::number(id)] = hrText(SDK_CALL(IBMDSwitcherFairlightAudioInputIterator, it.p, GetById, id, in.out()));
        }
        c.observe("getById", r);
    });
    addTest("m.iter.fairlightSourceById", "IBMDSwitcherFairlightAudioSourceIterator: Next, GetById", [](Ctx& c) {
        Com<IBMDSwitcherFairlightAudioInput> in(static_cast<IBMDSwitcherFairlightAudioInput*>(accessObject(c, "IBMDSwitcherFairlightAudioInput")));
        if (!in) c.skip("no Fairlight input");
        Com<IBMDSwitcherFairlightAudioSourceIterator> it;
        c.hr("create", SDK_CALL(IBMDSwitcherFairlightAudioInput, in.p, CreateIterator,
                                __uuidof(IBMDSwitcherFairlightAudioSourceIterator), reinterpret_cast<void**>(it.out())));
        if (!it) return;
        QJsonArray ids;
        IBMDSwitcherFairlightAudioSource* src = nullptr;
        while (SDK_CALL(IBMDSwitcherFairlightAudioSourceIterator, it.p, Next, &src) == S_OK && src) {
            BMDSwitcherFairlightAudioSourceId id = 0;
            if (SUCCEEDED(src->GetId(&id))) ids.append(QString::number(id));
            src->Release();
            src = nullptr;
        }
        c.observe("sources", ids);
        QJsonObject r;
        QList<BMDSwitcherFairlightAudioSourceId> probeIds{ 0, 1, -1, 9999 };
        if (!ids.isEmpty()) probeIds.prepend(ids[0].toString().toLongLong());
        for (BMDSwitcherFairlightAudioSourceId id : probeIds) {
            Com<IBMDSwitcherFairlightAudioSource> s;
            r[QString::number(id)] = hrText(SDK_CALL(IBMDSwitcherFairlightAudioSourceIterator, it.p, GetById, id, s.out()));
        }
        c.observe("getById", r);
    });
    addTest("m.aux.createIterator", "IBMDSwitcherInputAux::CreateIterator (what it can list)", [](Ctx& c) {
        Com<IBMDSwitcherInputAux> aux(static_cast<IBMDSwitcherInputAux*>(accessObject(c, "IBMDSwitcherInputAux")));
        if (!aux) c.skip("no aux output");
        QJsonObject r;
        struct I { const char* name; IID iid; };
        for (I i : { I{ "InputIterator", __uuidof(IBMDSwitcherInputIterator) },
                     I{ "InputAuxShadowIterator", __uuidof(IBMDSwitcherInputAuxShadowIterator) } }) {
            Com<IUnknown> it;
            r[i.name] = hrText(SDK_CALL(IBMDSwitcherInputAux, aux.p, CreateIterator, i.iid, reinterpret_cast<void**>(it.out())));
        }
        c.observe("createIterator", r);
    });

    // ── DVE transition: the supported style list ──
    addTest("m.trdve.supportedStyles", "IBMDSwitcherTransitionDVEParameters::GetSupportedStyles", [](Ctx& c) {
        Com<IBMDSwitcherTransitionDVEParameters> o(static_cast<IBMDSwitcherTransitionDVEParameters*>(accessObject(c, "IBMDSwitcherTransitionDVEParameters")));
        if (!o) c.skip("no DVE transition parameters");
        unsigned int n = 0;
        c.hr("count", SDK_CALL(IBMDSwitcherTransitionDVEParameters, o.p, GetNumSupportedStyles, &n));
        std::vector<BMDSwitcherDVETransitionStyle> styles(n + 1);
        c.hr("styles", SDK_CALL(IBMDSwitcherTransitionDVEParameters, o.p, GetSupportedStyles, styles.data(), n));
        QJsonArray list;
        for (unsigned int i = 0; i < n; ++i) list.append(js(styles[i]));
        c.observe("supported", list);
        c.hr("styles.zeroMax", SDK_CALL(IBMDSwitcherTransitionDVEParameters, o.p, GetSupportedStyles, styles.data(), 0));
    });

    // ── Fairlight mixer: level notifications, talkback gain ──
    addTest("m.fl.mixer.levelNotifications", "SetAllLevelNotificationsEnabled on, then off", [](Ctx& c) {
        Com<IBMDSwitcherFairlightAudioMixer> o(static_cast<IBMDSwitcherFairlightAudioMixer*>(accessObject(c, "IBMDSwitcherFairlightAudioMixer")));
        if (!o) c.skip("no Fairlight mixer");
        c.hr("on", SDK_CALL(IBMDSwitcherFairlightAudioMixer, o.p, SetAllLevelNotificationsEnabled, TRUE));
        c.settle();
        c.hr("off", SDK_CALL(IBMDSwitcherFairlightAudioMixer, o.p, SetAllLevelNotificationsEnabled, FALSE));
    });
    addTest("m.fl.mixer.micTalkbackGain", "Mic talkback gain (only where it is not supported)", [](Ctx& c) {
        Com<IBMDSwitcherFairlightAudioMixer> o(static_cast<IBMDSwitcherFairlightAudioMixer*>(accessObject(c, "IBMDSwitcherFairlightAudioMixer")));
        if (!o) c.skip("no Fairlight mixer");
        BOOL supported = FALSE;
        c.hr("doesSupport", SDK_CALL(IBMDSwitcherFairlightAudioMixer, o.p, DoesSupportMicTalkbackGain, &supported));
        c.observe("supported", supported != FALSE);
        if (supported) c.skip("supported: its value can't be read here, so it is not changed");
        c.hr("set", SDK_CALL(IBMDSwitcherFairlightAudioMixer, o.p, SetMicTalkbackGain, 0.0));
    });
}
