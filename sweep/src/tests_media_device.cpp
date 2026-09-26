// Media pool / players, audio, HyperDeck, camera control, recording, streaming.
// Policy: read everything; change only settings that are put back; never
// upload, delete, clear, start recording or start streaming.
#include "tests_common.h"

#include <QElapsedTimer>
#include <QThread>
#include <atomic>

namespace {

// ── Multi-method callback sinks ──────────────────────────────

template <class Iface>
class RefCounted : public Iface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(Iface)) { *ppv = static_cast<Iface*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG c = --m_ref; if (!c) delete this; return c; }
protected:
    RefCounted(EventLog& log, QString source) : m_log(log), m_source(std::move(source)) {}
    virtual ~RefCounted() = default;
    void log(uint32_t type) { m_log.add(m_source, type); }
private:
    EventLog& m_log;
    QString m_source;
    std::atomic<ULONG> m_ref{ 1 };
};

class PlayerSink : public RefCounted<IBMDSwitcherMediaPlayerCallback> {
public:
    PlayerSink(EventLog& l) : RefCounted(l, "Player") {}
    HRESULT STDMETHODCALLTYPE SourceChanged() override { log('srcC'); return S_OK; }
    HRESULT STDMETHODCALLTYPE PlayingChanged() override { log('plyC'); return S_OK; }
    HRESULT STDMETHODCALLTYPE LoopChanged() override { log('lopC'); return S_OK; }
    HRESULT STDMETHODCALLTYPE AtBeginningChanged() override { log('begC'); return S_OK; }
    HRESULT STDMETHODCALLTYPE ClipFrameChanged() override { log('frmC'); return S_OK; }
};

class RecordSink : public RefCounted<IBMDSwitcherRecordAVCallback> {
public:
    RecordSink(EventLog& l) : RefCounted(l, "Record") {}
    HRESULT STDMETHODCALLTYPE Notify(BMDSwitcherRecordAVEventType t) override { log(static_cast<uint32_t>(t)); return S_OK; }
    HRESULT STDMETHODCALLTYPE NotifyWorkingSetChange(unsigned int, BMDSwitcherRecordDiskId) override { log('wset'); return S_OK; }
    HRESULT STDMETHODCALLTYPE NotifyDiskAvailability(BMDSwitcherRecordDiskAvailabilityEventType t, BMDSwitcherRecordDiskId) override { log(static_cast<uint32_t>(t)); return S_OK; }
    HRESULT STDMETHODCALLTYPE NotifyStatus(BMDSwitcherRecordAVState s, BMDSwitcherRecordAVError) override { log(static_cast<uint32_t>(s)); return S_OK; }
};

class StreamSink : public RefCounted<IBMDSwitcherStreamRTMPCallback> {
public:
    StreamSink(EventLog& l) : RefCounted(l, "Stream") {}
    HRESULT STDMETHODCALLTYPE Notify(BMDSwitcherStreamRTMPEventType t) override { log(static_cast<uint32_t>(t)); return S_OK; }
    HRESULT STDMETHODCALLTYPE NotifyStatus(BMDSwitcherStreamRTMPState s, BMDSwitcherStreamRTMPError) override { log(static_cast<uint32_t>(s)); return S_OK; }
};

class LockSink : public RefCounted<IBMDSwitcherLockCallback> {
public:
    LockSink(EventLog& l) : RefCounted(l, "Lock") {}
    HRESULT STDMETHODCALLTYPE Obtained() override { obtained = true; log('lckO'); return S_OK; }
    std::atomic<bool> obtained{ false };
};

template <class Pred>
bool pollUntil(Pred p, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (p()) return true;
        QThread::msleep(20);
    }
    return false;
}

QString bstr(HRESULT hr, BSTR b) { return SUCCEEDED(hr) ? takeBstr(b) : "error " + hrText(hr); }

// Marks interfaces that can't be reached on this device.
void unavailable(std::initializer_list<const char*> ifaces) {
    for (const char* i : ifaces) sweepUnavailable(i);
}

} // namespace

void registerMediaTests() {
    addTest("media.pool", "Media pool: stills and clips (read, lock/unlock only)", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherMediaPool> pool(query<IBMDSwitcherMediaPool>(c, c.s.sw, "IBMDSwitcherMediaPool"));
        if (!pool) {
            unavailable({ "IBMDSwitcherStills", "IBMDSwitcherClip", "IBMDSwitcherFrame" });
            return;
        }
        Com<IBMDSwitcherStills> stills;
        c.hr("getStills", SDK_CALL(IBMDSwitcherMediaPool, pool.p, GetStills, stills.out()));
        if (stills) {
            auto* sink = new Sink<IBMDSwitcherStillsCallback, BMDSwitcherMediaPoolEventType, IBMDSwitcherFrame*, int>(
                [&c](BMDSwitcherMediaPoolEventType t, IBMDSwitcherFrame*, int) { c.s.events.add("Stills", static_cast<uint32_t>(t)); });
            SDK_CALL(IBMDSwitcherStills, stills.p, AddCallback, sink);
            unsigned int count = 0;
            SDK_CALL(IBMDSwitcherStills, stills.p, GetCount, &count);
            c.observe("stillCount", static_cast<double>(count));
            QJsonObject list;
            for (unsigned int i = 0; i < count; ++i) {
                BOOL valid = FALSE;
                SDK_CALL(IBMDSwitcherStills, stills.p, IsValid, i, &valid);
                if (!valid) continue;
                BSTR name = nullptr;
                HRESULT hr = SDK_CALL(IBMDSwitcherStills, stills.p, GetName, i, &name);
                list[QString::number(i)] = bstr(hr, name);
            }
            c.observe("validStills", list);
            BSTR first = nullptr;
            HRESULT fhr = SDK_CALL(IBMDSwitcherStills, stills.p, GetName, 0, &first);
            c.hr("getName0.hr", fhr);
            c.observe("getName0", bstr(fhr, first));
            BOOL bad = FALSE;
            c.hr("isValid.outOfRange", SDK_CALL(IBMDSwitcherStills, stills.p, IsValid, count + 5, &bad));

            auto* lock = new LockSink(c.s.events);
            c.hr("lock", SDK_CALL(IBMDSwitcherStills, stills.p, Lock, lock));
            c.observe("lockObtained", pollUntil([&] { return lock->obtained.load(); }, 3000));
            c.hr("unlock", SDK_CALL(IBMDSwitcherStills, stills.p, Unlock, lock));
            lock->Release();

            SDK_CALL(IBMDSwitcherStills, stills.p, RemoveCallback, sink);
            sink->Release();
        }

        unsigned int clips = 0;
        c.hr("getClipCount", SDK_CALL(IBMDSwitcherMediaPool, pool.p, GetClipCount, &clips));
        c.observe("clipCount", static_cast<double>(clips));
        QJsonObject clipList;
        for (unsigned int i = 0; i < clips; ++i) {
            Com<IBMDSwitcherClip> clip;
            if (FAILED(SDK_CALL(IBMDSwitcherMediaPool, pool.p, GetClip, i, clip.out())) || !clip) continue;
            auto* sink = new Sink<IBMDSwitcherClipCallback, BMDSwitcherMediaPoolEventType, IBMDSwitcherFrame*, int, IBMDSwitcherAudio*, int>(
                [&c](BMDSwitcherMediaPoolEventType t, IBMDSwitcherFrame*, int, IBMDSwitcherAudio*, int) { c.s.events.add("Clip", static_cast<uint32_t>(t)); });
            SDK_CALL(IBMDSwitcherClip, clip.p, AddCallback, sink);
            unsigned int index = 0, frames = 0, maxFrames = 0;
            BOOL valid = FALSE;
            BSTR name = nullptr;
            SDK_CALL(IBMDSwitcherClip, clip.p, GetIndex, &index);
            SDK_CALL(IBMDSwitcherClip, clip.p, IsValid, &valid);
            HRESULT nhr = SDK_CALL(IBMDSwitcherClip, clip.p, GetName, &name);
            SDK_CALL(IBMDSwitcherClip, clip.p, GetFrameCount, &frames);
            SDK_CALL(IBMDSwitcherClip, clip.p, GetMaxFrameCount, &maxFrames);
            c.hr(QString("clip%1.cancelTransferIdle").arg(i), SDK_CALL(IBMDSwitcherClip, clip.p, CancelTransfer));
            auto* lock = new LockSink(c.s.events);
            HRESULT lhr = SDK_CALL(IBMDSwitcherClip, clip.p, Lock, lock);
            bool got = SUCCEEDED(lhr) && pollUntil([&] { return lock->obtained.load(); }, 3000);
            SDK_CALL(IBMDSwitcherClip, clip.p, Unlock, lock);
            lock->Release();
            clipList[QString::number(i)] = QJsonObject{ { "index", static_cast<double>(index) }, { "valid", valid != FALSE },
                                                        { "name", bstr(nhr, name) }, { "frames", static_cast<double>(frames) },
                                                        { "maxFrames", static_cast<double>(maxFrames) }, { "lock", got } };
            SDK_CALL(IBMDSwitcherClip, clip.p, RemoveCallback, sink);
            sink->Release();
        }
        c.observe("clips", clipList);
        if (clips == 0) unavailable({ "IBMDSwitcherClip" });
        Com<IBMDSwitcherClip> none;
        c.hr("getClip.outOfRange", SDK_CALL(IBMDSwitcherMediaPool, pool.p, GetClip, clips + 5, none.out()));
    });

    addTest("media.frame", "Create a frame buffer (local only, nothing uploaded)", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherMediaPool> pool(query<IBMDSwitcherMediaPool>(c, c.s.sw, "IBMDSwitcherMediaPool"));
        if (!pool) c.skip("no media pool");
        Com<IBMDSwitcherFrame> frame;
        c.hr("create", SDK_CALL(IBMDSwitcherMediaPool, pool.p, CreateFrame, bmdSwitcherPixelFormat8BitARGB, 1920, 1080, frame.out()));
        if (!frame) return;
        void* bytes = nullptr;
        c.hr("getBytes", SDK_CALL(IBMDSwitcherFrame, frame.p, GetBytes, &bytes));
        c.observe("width", frame->GetWidth());
        c.observe("height", frame->GetHeight());
        c.observe("rowBytes", frame->GetRowBytes());
        Com<IBMDSwitcherFrame> bad;
        c.hr("create.bad", SDK_CALL(IBMDSwitcherMediaPool, pool.p, CreateFrame, bmdSwitcherPixelFormat8BitARGB, 0, 0, bad.out()));
    });

    addTest("media.player", "Media player 1: source, loop, play, position (restored)", [](Ctx& c) {
        c.needConnection();
        IBMDSwitcherMediaPlayerIterator* it = nullptr;
        HRESULT hr = SDK_CALL(IBMDSwitcher, c.s.sw, CreateIterator, __uuidof(IBMDSwitcherMediaPlayerIterator), reinterpret_cast<void**>(&it));
        c.hr("iterator", hr);
        Com<IBMDSwitcherMediaPlayer> player;
        if (it) {
            SDK_CALL(IBMDSwitcherMediaPlayerIterator, it, Next, player.out());
            it->Release();
        }
        if (!player) {
            unavailable({ "IBMDSwitcherMediaPlayer", "IBMDSwitcherMediaPlayerIterator" });
            c.skip("no media player");
        }
        auto* sink = new PlayerSink(c.s.events);
        SDK_CALL(IBMDSwitcherMediaPlayer, player.p, AddCallback, sink);

        auto state = [&](const QString& p) {
            BMDSwitcherMediaPlayerSourceType type{};
            unsigned int index = 0, frame = 0;
            BOOL playing = FALSE, loop = FALSE, begin = FALSE;
            SDK_CALL(IBMDSwitcherMediaPlayer, player.p, GetSource, &type, &index);
            SDK_CALL(IBMDSwitcherMediaPlayer, player.p, GetPlaying, &playing);
            SDK_CALL(IBMDSwitcherMediaPlayer, player.p, GetLoop, &loop);
            SDK_CALL(IBMDSwitcherMediaPlayer, player.p, GetAtBeginning, &begin);
            HRESULT fhr = SDK_CALL(IBMDSwitcherMediaPlayer, player.p, GetClipFrame, &frame);
            c.observe(p, QJsonObject{ { "sourceType", fourcc(static_cast<uint32_t>(type)) }, { "sourceIndex", static_cast<double>(index) },
                                      { "playing", playing != FALSE }, { "loop", loop != FALSE }, { "atBeginning", begin != FALSE },
                                      { "clipFrame", SUCCEEDED(fhr) ? QJsonValue(static_cast<double>(frame)) : QJsonValue(hrText(fhr)) } });
        };
        state("initial");
        c.hr("setSource.still0", SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetSource, bmdSwitcherMediaPlayerSourceTypeStill, 0));
        c.settle();
        state("afterStill0");
        c.hr("setSource.still999", SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetSource, bmdSwitcherMediaPlayerSourceTypeStill, 999));
        c.hr("setSource.clip0", SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetSource, bmdSwitcherMediaPlayerSourceTypeClip, 0));
        c.settle();
        state("afterClip0");
        for (BOOL v : { TRUE, FALSE }) c.hr(QString("setLoop.%1").arg(v), SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetLoop, v));
        for (BOOL v : { TRUE, FALSE }) c.hr(QString("setPlaying.%1").arg(v), SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetPlaying, v));
        c.hr("setAtBeginning", SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetAtBeginning));
        c.hr("setClipFrame.0", SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetClipFrame, 0));
        c.hr("setClipFrame.9999", SDK_CALL(IBMDSwitcherMediaPlayer, player.p, SetClipFrame, 9999));
        c.settle();
        state("final");
        SDK_CALL(IBMDSwitcherMediaPlayer, player.p, RemoveCallback, sink);
        sink->Release();
    });
}

void registerDeviceTests() {
    addTest("audio.classic", "Classic audio mixer inputs (read-only)", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherAudioMixer> mixer(query<IBMDSwitcherAudioMixer>(c, c.s.sw, "IBMDSwitcherAudioMixer"));
        if (!mixer) {
            unavailable({ "IBMDSwitcherAudioInput", "IBMDSwitcherAudioInputIterator" });
            return;
        }
        IBMDSwitcherAudioInputIterator* it = nullptr;
        c.hr("iterator", SDK_CALL(IBMDSwitcherAudioMixer, mixer.p, CreateIterator, __uuidof(IBMDSwitcherAudioInputIterator), reinterpret_cast<void**>(&it)));
        QJsonArray inputs;
        if (it) {
            IBMDSwitcherAudioInput* in = nullptr;
            while (SDK_CALL(IBMDSwitcherAudioInputIterator, it, Next, &in) == S_OK && in) {
                BMDSwitcherAudioInputId id = 0;
                BMDSwitcherAudioInputType type{};
                BMDSwitcherExternalPortType port{};
                SDK_CALL(IBMDSwitcherAudioInput, in, GetAudioInputId, &id);
                SDK_CALL(IBMDSwitcherAudioInput, in, GetType, &type);
                HRESULT phr = SDK_CALL(IBMDSwitcherAudioInput, in, GetCurrentExternalPortType, &port);
                inputs.append(QJsonObject{ { "id", static_cast<double>(id) }, { "type", fourcc(static_cast<uint32_t>(type)) },
                                           { "port", SUCCEEDED(phr) ? QJsonValue(static_cast<double>(port)) : QJsonValue(hrText(phr)) } });
                in->Release();
                in = nullptr;
            }
            it->Release();
        }
        c.observe("inputs", inputs);
    });

    addTest("audio.fairlight", "Fairlight audio mixer inputs (read-only)", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherFairlightAudioMixer> mixer(query<IBMDSwitcherFairlightAudioMixer>(c, c.s.sw, "IBMDSwitcherFairlightAudioMixer"));
        if (!mixer) {
            unavailable({ "IBMDSwitcherFairlightAudioInput", "IBMDSwitcherFairlightAudioInputIterator" });
            return;
        }
        IBMDSwitcherFairlightAudioInputIterator* it = nullptr;
        c.hr("iterator", SDK_CALL(IBMDSwitcherFairlightAudioMixer, mixer.p, CreateIterator, __uuidof(IBMDSwitcherFairlightAudioInputIterator), reinterpret_cast<void**>(&it)));
        QJsonArray inputs;
        if (it) {
            IBMDSwitcherFairlightAudioInput* in = nullptr;
            while (SDK_CALL(IBMDSwitcherFairlightAudioInputIterator, it, Next, &in) == S_OK && in) {
                BMDSwitcherAudioInputId id = 0;
                BMDSwitcherFairlightAudioInputType type{};
                BMDSwitcherExternalPortType port{};
                SDK_CALL(IBMDSwitcherFairlightAudioInput, in, GetId, &id);
                SDK_CALL(IBMDSwitcherFairlightAudioInput, in, GetType, &type);
                HRESULT phr = SDK_CALL(IBMDSwitcherFairlightAudioInput, in, GetCurrentExternalPortType, &port);
                inputs.append(QJsonObject{ { "id", static_cast<double>(id) }, { "type", fourcc(static_cast<uint32_t>(type)) },
                                           { "port", SUCCEEDED(phr) ? QJsonValue(static_cast<double>(port)) : QJsonValue(hrText(phr)) } });
                in->Release();
                in = nullptr;
            }
            it->Release();
        }
        c.observe("inputs", inputs);
    });

    addTest("hyperdeck", "HyperDeck connections (read-only)", [](Ctx& c) {
        c.needConnection();
        IBMDSwitcherHyperDeckIterator* it = nullptr;
        HRESULT hr = SDK_CALL(IBMDSwitcher, c.s.sw, CreateIterator, __uuidof(IBMDSwitcherHyperDeckIterator), reinterpret_cast<void**>(&it));
        c.hr("iterator", hr);
        if (!it) {
            unavailable({ "IBMDSwitcherHyperDeckIterator", "IBMDSwitcherHyperDeck" });
            return;
        }
        QJsonArray decks;
        IBMDSwitcherHyperDeck* deck = nullptr;
        while (SDK_CALL(IBMDSwitcherHyperDeckIterator, it, Next, &deck) == S_OK && deck) {
            BMDSwitcherHyperDeckConnectionStatus status{};
            unsigned int clips = 0, address = 0;
            BSTR model = nullptr;
            SDK_CALL(IBMDSwitcherHyperDeck, deck, GetConnectionStatus, &status);
            SDK_CALL(IBMDSwitcherHyperDeck, deck, GetClipCount, &clips);
            SDK_CALL(IBMDSwitcherHyperDeck, deck, GetNetworkAddress, &address);
            HRESULT mhr = SDK_CALL(IBMDSwitcherHyperDeck, deck, GetModelName, &model);
            decks.append(QJsonObject{ { "status", fourcc(static_cast<uint32_t>(status)) }, { "clips", static_cast<double>(clips) },
                                      { "address", static_cast<double>(address) }, { "model", bstr(mhr, model) } });
            deck->Release();
            deck = nullptr;
        }
        it->Release();
        c.observe("hyperDecks", decks);
        if (decks.isEmpty()) unavailable({ "IBMDSwitcherHyperDeck" });
    });

    addTest("camera", "Camera control: read focus, write it back, zero offset", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherCameraControl> cam(query<IBMDSwitcherCameraControl>(c, c.s.sw, "IBMDSwitcherCameraControl"));
        if (!cam) return;
        unsigned int flush = 0;
        c.hr("flushInterval.hr", SDK_CALL(IBMDSwitcherCameraControl, cam.p, GetPeriodicFlushInterval, &flush));
        c.observe("flushInterval", static_cast<double>(flush));
        // Camera 1, lens (0), focus (0) — as in the CameraFocus sample.
        double focus[4] = {};
        unsigned int count = 4;
        HRESULT hr = SDK_CALL(IBMDSwitcherCameraControl, cam.p, GetFloats, 1, 0, 0, &count, focus);
        c.hr("getFocus", hr);
        if (SUCCEEDED(hr) && count > 0) {
            c.observe("focus", focus[0]);
            c.hr("setFocusSame", SDK_CALL(IBMDSwitcherCameraControl, cam.p, SetFloats, 1, 0, 0, 1, focus));
        } else {
            c.observe("focus", "unknown (no Blackmagic camera answering)");
        }
        double zero = 0.0;
        c.hr("offsetFocusZero", SDK_CALL(IBMDSwitcherCameraControl, cam.p, OffsetFloats, 1, 0, 0, 1, &zero));
        if (c.opt.allowCamera) {
            c.hr("autofocus", SDK_CALL(IBMDSwitcherCameraControl, cam.p, SetFlags, 1, 0, 1, 0, nullptr));
        } else {
            c.observe("autofocus", "not sent (use --allow-camera)");
        }
    });

    addTest("record", "Recording: status and settings (never starts)", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherRecordAV> rec(query<IBMDSwitcherRecordAV>(c, c.s.sw, "IBMDSwitcherRecordAV"));
        if (!rec) {
            unavailable({ "IBMDSwitcherRecordDisk", "IBMDSwitcherRecordDiskIterator" });
            return;
        }
        auto* sink = new RecordSink(c.s.events);
        SDK_CALL(IBMDSwitcherRecordAV, rec.p, AddCallback, sink);
        BOOL b = FALSE;
        read<BOOL>(c, "isRecording", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherRecordAV, rec.p, IsRecording, v); });
        BMDSwitcherRecordAVState st{};
        BMDSwitcherRecordAVError err{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherRecordAV, rec.p, GetStatus, &st, &err))) c.observe("status", fourcc(static_cast<uint32_t>(st)));
        BSTR name = nullptr;
        HRESULT fhr = SDK_CALL(IBMDSwitcherRecordAV, rec.p, GetFilename, &name);
        QString filename = bstr(fhr, name);
        c.observe("filename", filename);
        if (SUCCEEDED(fhr)) {
            BSTR same = makeBstr(filename);
            c.hr("setFilenameSame", SDK_CALL(IBMDSwitcherRecordAV, rec.p, SetFilename, same));
            SysFreeString(same);
        }
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherRecordAV, rec.p, GetRecordInAllCameras, &b)))
            c.hr("setRecordInAllCamerasSame", SDK_CALL(IBMDSwitcherRecordAV, rec.p, SetRecordInAllCameras, b));
        read<BOOL>(c, "supportsISO", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherRecordAV, rec.p, DoesSupportISORecording, v); });
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherRecordAV, rec.p, GetRecordAllISOInputs, &b)))
            c.hr("setRecordAllISOSame", SDK_CALL(IBMDSwitcherRecordAV, rec.p, SetRecordAllISOInputs, b));
        BMDSwitcherRecordDiskId disk = 0;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherRecordAV, rec.p, GetWorkingSetDisk, 0, &disk)))
            c.hr("setWorkingSetDiskSame", SDK_CALL(IBMDSwitcherRecordAV, rec.p, SetWorkingSetDisk, 0, disk));
        read<unsigned int>(c, "activeDiskIndex", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherRecordAV, rec.p, GetActiveDiskIndex, v); });
        read<unsigned int>(c, "totalTimeAvailable", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherRecordAV, rec.p, GetTotalRecordingTimeAvailable, v); });
        c.hr("requestDuration", SDK_CALL(IBMDSwitcherRecordAV, rec.p, RequestDuration));

        IBMDSwitcherRecordDiskIterator* it = nullptr;
        SDK_CALL(IBMDSwitcherRecordAV, rec.p, CreateIterator, __uuidof(IBMDSwitcherRecordDiskIterator), reinterpret_cast<void**>(&it));
        QJsonArray disks;
        if (it) {
            IBMDSwitcherRecordDisk* d = nullptr;
            while (SDK_CALL(IBMDSwitcherRecordDiskIterator, it, Next, &d) == S_OK && d) {
                auto* dsink = new Sink<IBMDSwitcherRecordDiskCallback, BMDSwitcherRecordDiskEventType, BMDSwitcherRecordDiskId>(
                    [&c](BMDSwitcherRecordDiskEventType t, BMDSwitcherRecordDiskId) { c.s.events.add("Disk", static_cast<uint32_t>(t)); });
                SDK_CALL(IBMDSwitcherRecordDisk, d, AddCallback, dsink);
                BMDSwitcherRecordDiskId id = 0;
                unsigned int time = 0;
                BMDSwitcherRecordDiskStatus ds{};
                BSTR vol = nullptr;
                SDK_CALL(IBMDSwitcherRecordDisk, d, GetId, &id);
                SDK_CALL(IBMDSwitcherRecordDisk, d, GetRecordingTimeAvailable, &time);
                SDK_CALL(IBMDSwitcherRecordDisk, d, GetStatus, &ds);
                HRESULT vhr = SDK_CALL(IBMDSwitcherRecordDisk, d, GetVolumeName, &vol);
                disks.append(QJsonObject{ { "id", static_cast<double>(id) }, { "volume", bstr(vhr, vol) },
                                          { "status", static_cast<double>(ds) } });
                IBMDSwitcherRecordDisk* again = nullptr;
                if (SUCCEEDED(SDK_CALL(IBMDSwitcherRecordDiskIterator, it, GetById, id, &again)) && again) again->Release();
                SDK_CALL(IBMDSwitcherRecordDisk, d, RemoveCallback, dsink);
                dsink->Release();
                d->Release();
                d = nullptr;
            }
            it->Release();
        }
        c.observe("disks", disks);
        if (disks.isEmpty()) unavailable({ "IBMDSwitcherRecordDisk" });
        SDK_CALL(IBMDSwitcherRecordAV, rec.p, RemoveCallback, sink);
        sink->Release();
    });

    addTest("stream", "Streaming: status and settings (never starts)", [](Ctx& c) {
        c.needConnection();
        Com<IBMDSwitcherStreamRTMP> st(query<IBMDSwitcherStreamRTMP>(c, c.s.sw, "IBMDSwitcherStreamRTMP"));
        if (!st) return;
        auto* sink = new StreamSink(c.s.events);
        SDK_CALL(IBMDSwitcherStreamRTMP, st.p, AddCallback, sink);
        read<BOOL>(c, "isStreaming", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, IsStreaming, v); });
        BMDSwitcherStreamRTMPState state{};
        BMDSwitcherStreamRTMPError err{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetStatus, &state, &err))) c.observe("status", fourcc(static_cast<uint32_t>(state)));
        auto rewrite = [&](const char* label, auto get, auto set) {
            BSTR b = nullptr;
            HRESULT hr = get(&b);
            QString v = bstr(hr, b);
            c.observe(label, QString(label) == "key" ? QString(v.isEmpty() ? "" : "(set)") : v);
            if (SUCCEEDED(hr)) {
                BSTR same = makeBstr(v);
                c.hr(QString("%1.setSame").arg(label), set(same));
                SysFreeString(same);
            }
        };
        rewrite("serviceName", [&](BSTR* b) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetServiceName, b); },
                               [&](BSTR b) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, SetServiceName, b); });
        rewrite("url", [&](BSTR* b) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetUrl, b); },
                       [&](BSTR b) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, SetUrl, b); });
        rewrite("key", [&](BSTR* b) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetKey, b); },
                       [&](BSTR b) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, SetKey, b); });
        unsigned int lo = 0, hi = 0;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetVideoBitrates, &lo, &hi))) {
            c.observe("videoBitrates", QJsonArray{ static_cast<double>(lo), static_cast<double>(hi) });
            c.hr("setVideoBitratesSame", SDK_CALL(IBMDSwitcherStreamRTMP, st.p, SetVideoBitrates, lo, hi));
        }
        BOOL low = FALSE;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetLowLatency, &low)))
            c.hr("setLowLatencySame", SDK_CALL(IBMDSwitcherStreamRTMP, st.p, SetLowLatency, low));
        read<unsigned int>(c, "encodingBitrate", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetEncodingBitrate, v); });
        read<double>(c, "cacheUsed", [&](double* v) { return SDK_CALL(IBMDSwitcherStreamRTMP, st.p, GetCacheUsed, v); });
        c.hr("requestDuration", SDK_CALL(IBMDSwitcherStreamRTMP, st.p, RequestDuration));
        SDK_CALL(IBMDSwitcherStreamRTMP, st.p, RemoveCallback, sink);
        sink->Release();
    });
}
