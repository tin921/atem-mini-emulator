// Capability probe: which SDK interfaces and features this switcher has.
// Read-only: it only asks for interfaces, lists iterators and calls getters.
// Its results decide the "not on the ATEM Mini" category (coverage/).
#include "tests_common.h"

#include <QJsonObject>
#include <cstdio>
#include <map>
#include <set>

namespace {

struct Iface {
    const char* name;
    IID iid;
    bool iterator;
};

const Iface kIfaces[] = {
#define IFACE(n) { #n, __uuidof(n), false },
#define ITER(n) { #n, __uuidof(n), true },
#include "probe_ifaces.inc"
#undef IFACE
#undef ITER
};

// Every SDK iterator's first method is Next([out] T**), ending with S_FALSE.
struct GenericIterator : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Next(IUnknown** out) = 0;
};

// CreateIterator on whichever parent interface `obj` has (they share the
// signature but sit at different places in each interface).
HRESULT createIterator(IUnknown* obj, REFIID iid, IUnknown** out, QString* parent) {
#define TRY_PARENT(P)                                                                        \
    {                                                                                        \
        P* p = nullptr;                                                                      \
        if (SUCCEEDED(obj->QueryInterface(__uuidof(P), reinterpret_cast<void**>(&p))) && p) { \
            sweepCalled(QStringLiteral(#P "::CreateIterator"));                              \
            HRESULT hr = p->CreateIterator(iid, reinterpret_cast<void**>(out));              \
            p->Release();                                                                    \
            if (SUCCEEDED(hr) && *out) {                                                     \
                *parent = QStringLiteral(#P);                                                \
                return hr;                                                                   \
            }                                                                                \
        }                                                                                    \
    }
    TRY_PARENT(IBMDSwitcher)
    TRY_PARENT(IBMDSwitcherAudioMixer)
    TRY_PARENT(IBMDSwitcherCameraControl)
    TRY_PARENT(IBMDSwitcherFairlightAudioAuxOutput)
    TRY_PARENT(IBMDSwitcherFairlightAudioEqualizer)
    TRY_PARENT(IBMDSwitcherFairlightAudioInput)
    TRY_PARENT(IBMDSwitcherFairlightAudioMixer)
    TRY_PARENT(IBMDSwitcherHyperDeck)
    TRY_PARENT(IBMDSwitcherInputAux)
    TRY_PARENT(IBMDSwitcherInputSuperSource)
    TRY_PARENT(IBMDSwitcherMixEffectBlock)
    TRY_PARENT(IBMDSwitcherRecordAV)
    TRY_PARENT(IBMDSwitcherRemoteSourceConfiguration)
    TRY_PARENT(IBMDSwitcherVisca)
#undef TRY_PARENT
    return E_NOINTERFACE;
}

IUnknown* identity(IUnknown* obj) {
    IUnknown* id = nullptr;
    obj->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&id));
    return id;
}

} // namespace

void registerProbeTests() {
    addTest("probe.interfaces", "Which SDK interfaces this switcher has (read-only walk)", [](Ctx& c) {
        c.needConnection();
        // Breadth-first over every object reachable from the switcher: ask each
        // for every interface, and list every iterator it can create.
        constexpr size_t kMaxObjects = 400;
        constexpr int kMaxPerIterator = 64;
        std::map<QString, int> found;              // interface -> objects that have it
        std::map<QString, QString> iteratorFrom;   // iterator -> parent interface
        std::set<IUnknown*> seen;                  // identities (AddRef'd)
        std::vector<IUnknown*> queue;
        if (IUnknown* id = identity(c.s.sw)) {
            seen.insert(id);
            queue.push_back(id);
        }
        auto enqueue = [&](IUnknown* child) {
            if (!child) return;
            IUnknown* id = identity(child);
            child->Release();
            if (!id) return;
            if (seen.size() < kMaxObjects && seen.insert(id).second) queue.push_back(id);
            else id->Release();
        };
        // Objects some interfaces hand out through getters (all read-only).
        auto followGetters = [&](IUnknown* obj) {
            Com<IBMDSwitcherFairlightAudioSource> source;
            if (SUCCEEDED(obj->QueryInterface(__uuidof(IBMDSwitcherFairlightAudioSource), reinterpret_cast<void**>(source.out()))) && source) {
                for (const Iface& f : kIfaces) {
                    IUnknown* e = nullptr;
                    if (!f.iterator && SUCCEEDED(SDK_CALL(IBMDSwitcherFairlightAudioSource, source.p, GetEffect, f.iid, reinterpret_cast<void**>(&e))) && e)
                        enqueue(e);
                }
            }
            Com<IBMDSwitcherFairlightAudioDynamicsProcessor> dynamics;
            if (SUCCEEDED(obj->QueryInterface(__uuidof(IBMDSwitcherFairlightAudioDynamicsProcessor), reinterpret_cast<void**>(dynamics.out()))) && dynamics) {
                for (const Iface& f : kIfaces) {
                    IUnknown* e = nullptr;
                    if (!f.iterator && SUCCEEDED(SDK_CALL(IBMDSwitcherFairlightAudioDynamicsProcessor, dynamics.p, GetProcessor, f.iid, reinterpret_cast<void**>(&e))) && e)
                        enqueue(e);
                }
            }
            Com<IBMDSwitcherFairlightAudioMixer> mixer;
            if (SUCCEEDED(obj->QueryInterface(__uuidof(IBMDSwitcherFairlightAudioMixer), reinterpret_cast<void**>(mixer.out()))) && mixer) {
                for (const Iface& f : kIfaces) {
                    IUnknown* e = nullptr;
                    if (!f.iterator && SUCCEEDED(SDK_CALL(IBMDSwitcherFairlightAudioMixer, mixer.p, GetMasterOutEffect, f.iid, reinterpret_cast<void**>(&e))) && e)
                        enqueue(e);
                }
            }
            Com<IBMDSwitcherKeyFlyParameters> fly;
            if (SUCCEEDED(obj->QueryInterface(__uuidof(IBMDSwitcherKeyFlyParameters), reinterpret_cast<void**>(fly.out()))) && fly) {
                for (BMDSwitcherFlyKeyFrame k : { bmdSwitcherFlyKeyFrameA, bmdSwitcherFlyKeyFrameB }) {
                    IBMDSwitcherKeyFlyKeyFrameParameters* kf = nullptr;
                    if (SUCCEEDED(SDK_CALL(IBMDSwitcherKeyFlyParameters, fly.p, GetKeyFrameParameters, k, &kf)) && kf) enqueue(kf);
                }
            }
            Com<IBMDSwitcherMediaPool> media;
            if (SUCCEEDED(obj->QueryInterface(__uuidof(IBMDSwitcherMediaPool), reinterpret_cast<void**>(media.out()))) && media) {
                IBMDSwitcherStills* stills = nullptr;
                if (SUCCEEDED(SDK_CALL(IBMDSwitcherMediaPool, media.p, GetStills, &stills)) && stills) enqueue(stills);
                IBMDSwitcherClip* clip = nullptr;
                if (SUCCEEDED(SDK_CALL(IBMDSwitcherMediaPool, media.p, GetClip, 0, &clip)) && clip) enqueue(clip);
            }
        };
        for (size_t at = 0; at < queue.size(); ++at) {
            IUnknown* obj = queue[at];
            followGetters(obj);
            for (const Iface& f : kIfaces) {
                if (f.iterator) continue;
                if (getenv("SWEEP_PROBE_TRACE")) { fprintf(stderr, "obj %zu QI %s\n", at, f.name); fflush(stderr); }
                IUnknown* p = nullptr;
                if (SUCCEEDED(obj->QueryInterface(f.iid, reinterpret_cast<void**>(&p))) && p) {
                    ++found[f.name];
                    p->Release();
                }
            }
            for (const Iface& f : kIfaces) {
                if (!f.iterator) continue;
                if (getenv("SWEEP_PROBE_TRACE")) { fprintf(stderr, "obj %zu iterator %s\n", at, f.name); fflush(stderr); }
                IUnknown* it = nullptr;
                QString parent;
                if (FAILED(createIterator(obj, f.iid, &it, &parent)) || !it) continue;
                ++found[f.name];
                iteratorFrom[f.name] = parent;
                sweepCalled(QString("%1::Next").arg(f.name));
                if (QByteArray(f.name) == "IBMDSwitcherCameraControlParameterIterator") {
                    // The one iterator whose Next returns numbers, not objects.
                    auto* params = reinterpret_cast<IBMDSwitcherCameraControlParameterIterator*>(it);
                    unsigned int device = 0, category = 0, parameter = 0;
                    int n = 0;
                    while (n < 1024 && params->Next(&device, &category, &parameter) == S_OK) ++n;
                    c.observe("cameraControlParameters", static_cast<double>(n));
                    it->Release();
                    continue;
                }
                auto* gen = reinterpret_cast<GenericIterator*>(it);
                IUnknown* child = nullptr;
                for (int n = 0; n < kMaxPerIterator && gen->Next(&child) == S_OK && child; ++n) {
                    IUnknown* id = identity(child);
                    child->Release();
                    child = nullptr;
                    if (!id) continue;
                    if (seen.size() < kMaxObjects && seen.insert(id).second) queue.push_back(id);
                    else id->Release();
                }
                it->Release();
            }
        }
        for (IUnknown* id : seen) id->Release();

        // Objects that exist only on this side: the discovery object (it made
        // the connection), frames, macros and audio created locally for an
        // upload, and a macro transfer (a download, read-only).
        if (c.s.discovery) found["IBMDSwitcherDiscovery"] = 1;
        Com<IBMDSwitcherMediaPool> media;
        if (SUCCEEDED(c.s.sw->QueryInterface(__uuidof(IBMDSwitcherMediaPool), reinterpret_cast<void**>(media.out()))) && media) {
            Com<IBMDSwitcherFrame> frame;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherMediaPool, media.p, CreateFrame, bmdSwitcherPixelFormat8BitARGB, 16, 16, frame.out())) && frame)
                found["IBMDSwitcherFrame"] = 1;
            Com<IBMDSwitcherAudio> audio;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherMediaPool, media.p, CreateAudio, 16, audio.out())) && audio)
                found["IBMDSwitcherAudio"] = 1;
        }
        if (c.s.pool) {
            Com<IBMDSwitcherMacro> macro;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, CreateMacro, 16, macro.out())) && macro)
                found["IBMDSwitcherMacro"] = 1;
            unsigned int count = 0;
            SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetMaxCount, &count);
            for (unsigned int i = 0; i < count; ++i) {
                BOOL valid = FALSE;
                if (FAILED(SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, IsValid, i, &valid)) || !valid) continue;
                Com<IBMDSwitcherTransferMacro> transfer;
                if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, Download, i, transfer.out())) && transfer) {
                    found["IBMDSwitcherTransferMacro"] = 1;
                    SDK_CALL(IBMDSwitcherTransferMacro, transfer.p, Cancel);   // only the object was needed
                }
                break;
            }
        }

        QJsonObject available;
        QJsonArray unavailable;
        for (const Iface& f : kIfaces) {
            auto it = found.find(f.name);
            if (it == found.end()) {
                unavailable.append(f.name);
                sweepUnavailable(f.name);
            } else {
                available[f.name] = f.iterator ? QJsonValue(QString("from %1").arg(iteratorFrom[f.name])) : QJsonValue(it->second);
            }
        }
        c.observe("objects", static_cast<double>(seen.size()));
        c.observe("available", available);
        c.observe("unavailable", unavailable);
    });

    addTest("probe.features", "Switcher features for hardware the ATEM Mini may lack (getters only)", [](Ctx& c) {
        c.needConnection();
        IBMDSwitcher* sw = c.s.sw;
        BMDSwitcherVideoMode mode{};
        SDK_CALL(IBMDSwitcher, sw, GetVideoMode, &mode);
        read<BMDSwitcherDownConversionMethod>(c, "GetMethodForDownConvertedSD",
            [&](BMDSwitcherDownConversionMethod* v) { return SDK_CALL(IBMDSwitcher, sw, GetMethodForDownConvertedSD, v); });
        read<BMDSwitcherVideoMode>(c, "GetDownConvertedHDVideoMode",
            [&](BMDSwitcherVideoMode* v) { return SDK_CALL(IBMDSwitcher, sw, GetDownConvertedHDVideoMode, mode, v); });
        read<BMDSwitcherVideoMode>(c, "GetMultiViewVideoMode",
            [&](BMDSwitcherVideoMode* v) { return SDK_CALL(IBMDSwitcher, sw, GetMultiViewVideoMode, mode, v); });
        read<BMDSwitcher3GSDIOutputLevel>(c, "Get3GSDIOutputLevel",
            [&](BMDSwitcher3GSDIOutputLevel* v) { return SDK_CALL(IBMDSwitcher, sw, Get3GSDIOutputLevel, v); });
        read<BOOL>(c, "GetSuperSourceCascade", [&](BOOL* v) { return SDK_CALL(IBMDSwitcher, sw, GetSuperSourceCascade, v); });
        read<BOOL>(c, "GetTimecodeSdiOutputEnabled",
                   [&](BOOL* v) { return SDK_CALL(IBMDSwitcher, sw, GetTimecodeSdiOutputEnabled, v); });

        if (IBMDSwitcherInput* in = c.s.input(1)) {
            read<BOOL>(c, "input1.DoesSupportCameraModel",
                       [&](BOOL* v) { return SDK_CALL(IBMDSwitcherInput, in, DoesSupportCameraModel, v); });
            read<unsigned int>(c, "input1.GetCameraModel",
                               [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherInput, in, GetCameraModel, v); });
            read<BMDSwitcherViscaDeviceId>(c, "input1.GetViscaDeviceId",
                [&](BMDSwitcherViscaDeviceId* v) { return SDK_CALL(IBMDSwitcherInput, in, GetViscaDeviceId, v); });
        }
        Com<IBMDSwitcherMediaPool> pool(query<IBMDSwitcherMediaPool>(c, sw, "IBMDSwitcherMediaPool"));
        if (pool) {
            read<unsigned int>(c, "mediaPool.GetFrameTotalForClips",
                               [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherMediaPool, pool.p, GetFrameTotalForClips, v); });
            unsigned int counts[8] = {};
            HRESULT hr = SDK_CALL(IBMDSwitcherMediaPool, pool.p, GetClipMaxFrameCounts, 8, counts);
            c.observe("mediaPool.GetClipMaxFrameCounts", SUCCEEDED(hr) ? QString("S_OK") : "error " + hrText(hr));
        }
    });
}
