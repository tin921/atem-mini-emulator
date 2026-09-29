#pragma once
// Thin BMDSwitcherAPI wrapper for atem-sweep: one connection, every interface
// the sweep touches, and a timestamped log of every SDK event they fire.

#include <windows.h>
#include <oleauto.h>

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "BMDSwitcherAPI.h"

// ── Small helpers ────────────────────────────────────────────

QString fourcc(uint32_t v);            // 0x70676943 -> "pgiC"
QString hrText(HRESULT hr);            // "S_OK", "E_FAIL", "0x8000FFFF"
QString takeBstr(BSTR b);              // UTF-16 BSTR -> QString, frees it
BSTR makeBstr(const QString& s);       // caller frees (SysFreeString)
QString inputName(BMDSwitcherInputId id);

// ── Coverage ─────────────────────────────────────────────────
// Every SDK call the sweep makes goes through SDK_CALL (or is recorded by
// the Switcher itself), so the coverage report reflects calls that really
// executed, not ones a test merely claims.
void sweepCalled(const QString& ifaceMethod);
QStringList sweepCoverage();
// The SDK calls of one test: cleared when a test starts, read when it ends
// (each result lists them, so a method counts as verified only when every
// test that calls it passes).
void sweepTestStarted();
QStringList sweepTestCalls();
// The device does not expose this interface (e.g. RecordAV on a base
// ATEM Mini): all its methods count as "not available on this model".
void sweepUnavailable(const QString& iface);
QStringList sweepUnavailableInterfaces();

#define SDK_CALL(iface, obj, method, ...)     (sweepCalled(QStringLiteral(#iface "::" #method)), (obj)->method(__VA_ARGS__))

// ── COM callback sink ────────────────────────────────────────
// Every BMD *Callback interface is IUnknown + Notify(eventType, ...).
template <class Iface, class... Args>
class Sink : public Iface {
public:
    explicit Sink(std::function<void(Args...)> fn) : m_fn(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(Iface)) {
            *ppv = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = --m_ref;
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE Notify(Args... a) override {
        m_fn(a...);
        return S_OK;
    }

private:
    virtual ~Sink() = default;
    std::function<void(Args...)> m_fn;
    std::atomic<ULONG> m_ref{1};
};

// ── Event log ────────────────────────────────────────────────

struct SdkEvent {
    qint64 ms;        // since the sweep started
    QString source;   // "ME", "Key", "Fly", "DVE", "DSK", "Trans", "Macro", "Pool", "Switcher", "Input:3"
    QString type;     // four-character event code, e.g. "pgiC"
};

class EventLog {
public:
    EventLog();
    void add(const QString& source, uint32_t type);
    size_t size() const;
    std::vector<SdkEvent> since(size_t from) const;
    // Waits until an event (source, type) arrives at index >= from.
    bool waitFor(const QString& source, uint32_t type, size_t from, int timeoutMs) const;
    // Waits until no new event has arrived for quietMs (at most maxMs).
    void settle(int quietMs, int maxMs) const;
    qint64 elapsed() const { return m_clock.elapsed(); }

private:
    mutable std::mutex m_mutex;
    std::vector<SdkEvent> m_events;
    QElapsedTimer m_clock;
};

// ── Connection ───────────────────────────────────────────────

class Switcher {
public:
    Switcher();
    ~Switcher();
    Switcher(const Switcher&) = delete;
    Switcher& operator=(const Switcher&) = delete;

    // address "" = USB auto-detect. Attaches every interface and callback.
    HRESULT connect(const QString& address, BMDSwitcherConnectToFailure* fail);
    void disconnect();
    bool connected() const { return sw != nullptr; }

    IBMDSwitcherDiscovery* discovery = nullptr;
    IBMDSwitcher* sw = nullptr;
    IBMDSwitcherMixEffectBlock* me = nullptr;
    IBMDSwitcherTransitionParameters* trans = nullptr;
    IBMDSwitcherKey* key = nullptr;
    IBMDSwitcherKeyFlyParameters* fly = nullptr;
    IBMDSwitcherKeyDVEParameters* dve = nullptr;
    IBMDSwitcherDownstreamKey* dsk = nullptr;
    IBMDSwitcherMacroPool* pool = nullptr;
    IBMDSwitcherMacroControl* macros = nullptr;
    std::vector<IBMDSwitcherInput*> inputs;

    EventLog events;

    IBMDSwitcherInput* input(BMDSwitcherInputId id) const;

private:
    std::vector<std::function<void()>> m_removers;  // undo AddCallback + Release sinks
};
