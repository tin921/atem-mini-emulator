#include "sdk.h"

#include <QThread>
#include <set>

// ── Coverage ─────────────────────────────────────────────────

namespace {
std::mutex g_coverageMutex;
std::set<QString> g_coverage;
} // namespace

void sweepCalled(const QString& ifaceMethod) {
    std::lock_guard<std::mutex> lock(g_coverageMutex);
    g_coverage.insert(ifaceMethod);
}

QStringList sweepCoverage() {
    std::lock_guard<std::mutex> lock(g_coverageMutex);
    return QStringList(g_coverage.begin(), g_coverage.end());
}

namespace {
std::set<QString> g_unavailable;
} // namespace

void sweepUnavailable(const QString& iface) {
    std::lock_guard<std::mutex> lock(g_coverageMutex);
    g_unavailable.insert(iface);
}

QStringList sweepUnavailableInterfaces() {
    std::lock_guard<std::mutex> lock(g_coverageMutex);
    return QStringList(g_unavailable.begin(), g_unavailable.end());
}

// ── Helpers ──────────────────────────────────────────────────

QString fourcc(uint32_t v) {
    char c[5] = { char((v >> 24) & 0xFF), char((v >> 16) & 0xFF), char((v >> 8) & 0xFF), char(v & 0xFF), 0 };
    for (int i = 0; i < 4; ++i)
        if (c[i] < 32 || c[i] > 126) return QString("0x%1").arg(v, 8, 16, QLatin1Char('0'));
    return QString::fromLatin1(c);
}

QString hrText(HRESULT hr) {
    switch (hr) {
    case S_OK: return "S_OK";
    case S_FALSE: return "S_FALSE";
    case E_FAIL: return "E_FAIL";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_POINTER: return "E_POINTER";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_ACCESSDENIED: return "E_ACCESSDENIED";
    case E_UNEXPECTED: return "E_UNEXPECTED";
    default: return QString("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
    }
}

QString takeBstr(BSTR b) {
    if (!b) return QString();
    QString s = QString::fromWCharArray(b, static_cast<int>(SysStringLen(b)));
    SysFreeString(b);
    return s;
}

BSTR makeBstr(const QString& s) {
    std::wstring w = s.toStdWString();
    return SysAllocStringLen(w.data(), static_cast<UINT>(w.size()));
}

QString inputName(BMDSwitcherInputId id) {
    switch (id) {
    case 0: return "Black";
    case 1: case 2: case 3: case 4: return QString("Camera %1").arg(id);
    case 1000: return "Color Bars";
    case 2001: return "Color 1";
    case 2002: return "Color 2";
    case 3010: return "Media Player 1";
    case 3011: return "Media Player 1 Key";
    default: return QString("Input %1").arg(id);
    }
}

// ── EventLog ─────────────────────────────────────────────────

EventLog::EventLog() { m_clock.start(); }

void EventLog::add(const QString& source, uint32_t type) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back({ m_clock.elapsed(), source, fourcc(type) });
}

size_t EventLog::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_events.size();
}

std::vector<SdkEvent> EventLog::since(size_t from) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (from >= m_events.size()) return {};
    return std::vector<SdkEvent>(m_events.begin() + static_cast<ptrdiff_t>(from), m_events.end());
}

bool EventLog::waitFor(const QString& source, uint32_t type, size_t from, int timeoutMs) const {
    QString t = fourcc(type);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (size_t i = from; i < m_events.size(); ++i)
                if (m_events[i].source == source && m_events[i].type == t) return true;
        }
        QThread::msleep(5);
    }
    return false;
}

void EventLog::settle(int quietMs, int maxMs) const {
    QElapsedTimer timer;
    timer.start();
    size_t last = size();
    QElapsedTimer quiet;
    quiet.start();
    while (timer.elapsed() < maxMs) {
        QThread::msleep(10);
        size_t now = size();
        if (now != last) {
            last = now;
            quiet.restart();
        } else if (quiet.elapsed() >= quietMs) {
            return;
        }
    }
}

// ── Switcher ─────────────────────────────────────────────────

namespace {

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

template <class Iterator, class Item, class Owner>
Item* first(Owner* owner, const char* ownerName, const char* iteratorName) {
    Iterator* it = nullptr;
    Item* item = nullptr;
    if (!owner) return nullptr;
    sweepCalled(QString("%1::CreateIterator").arg(ownerName));
    if (SUCCEEDED(owner->CreateIterator(__uuidof(Iterator), reinterpret_cast<void**>(&it)))) {
        sweepCalled(QString("%1::Next").arg(iteratorName));
        it->Next(&item);
        it->Release();
    }
    return item;
}

// Adds a callback that logs every Notify under `source`; the remover undoes it.
template <class Obj, class CbIface, class... Args>
void watch(Obj* obj, const char* ifaceName, EventLog& log, const QString& source,
           std::vector<std::function<void()>>& removers) {
    if (!obj) return;
    auto* sink = new Sink<CbIface, Args...>([&log, source](auto type, auto...) {
        log.add(source, static_cast<uint32_t>(type));
    });
    sweepCalled(QString("%1::AddCallback").arg(ifaceName));
    obj->AddCallback(sink);
    QString remove = QString("%1::RemoveCallback").arg(ifaceName);
    removers.push_back([obj, sink, remove]() {
        sweepCalled(remove);
        obj->RemoveCallback(sink);
        sink->Release();
    });
}

} // namespace

Switcher::Switcher() {
    CoCreateInstance(__uuidof(CBMDSwitcherDiscovery), nullptr, CLSCTX_ALL,
                     __uuidof(IBMDSwitcherDiscovery), reinterpret_cast<void**>(&discovery));
}

Switcher::~Switcher() {
    disconnect();
    release(discovery);
}

IBMDSwitcherInput* Switcher::input(BMDSwitcherInputId id) const {
    for (auto* in : inputs) {
        BMDSwitcherInputId got = -1;
        if (SUCCEEDED(in->GetInputId(&got)) && got == id) return in;
    }
    return nullptr;
}

HRESULT Switcher::connect(const QString& address, BMDSwitcherConnectToFailure* fail) {
    disconnect();
    if (!discovery) return E_NOINTERFACE;

    BSTR addr = makeBstr(address);
    sweepCalled("IBMDSwitcherDiscovery::ConnectTo");
    HRESULT hr = discovery->ConnectTo(addr, &sw, fail);
    SysFreeString(addr);
    if (FAILED(hr) || !sw) {
        sw = nullptr;
        return FAILED(hr) ? hr : E_FAIL;
    }

    me = first<IBMDSwitcherMixEffectBlockIterator, IBMDSwitcherMixEffectBlock>(sw, "IBMDSwitcher", "IBMDSwitcherMixEffectBlockIterator");
    if (me) {
        me->QueryInterface(__uuidof(IBMDSwitcherTransitionParameters), reinterpret_cast<void**>(&trans));
        key = first<IBMDSwitcherKeyIterator, IBMDSwitcherKey>(me, "IBMDSwitcherMixEffectBlock", "IBMDSwitcherKeyIterator");
    }
    if (key) {
        key->QueryInterface(__uuidof(IBMDSwitcherKeyFlyParameters), reinterpret_cast<void**>(&fly));
        key->QueryInterface(__uuidof(IBMDSwitcherKeyDVEParameters), reinterpret_cast<void**>(&dve));
    }
    dsk = first<IBMDSwitcherDownstreamKeyIterator, IBMDSwitcherDownstreamKey>(sw, "IBMDSwitcher", "IBMDSwitcherDownstreamKeyIterator");
    sw->QueryInterface(__uuidof(IBMDSwitcherMacroPool), reinterpret_cast<void**>(&pool));
    sw->QueryInterface(__uuidof(IBMDSwitcherMacroControl), reinterpret_cast<void**>(&macros));

    IBMDSwitcherInputIterator* it = nullptr;
    sweepCalled("IBMDSwitcher::CreateIterator");
    if (SUCCEEDED(sw->CreateIterator(__uuidof(IBMDSwitcherInputIterator), reinterpret_cast<void**>(&it)))) {
        IBMDSwitcherInput* in = nullptr;
        sweepCalled("IBMDSwitcherInputIterator::Next");
        while (it->Next(&in) == S_OK && in) {
            inputs.push_back(in);
            in = nullptr;
        }
        it->Release();
    }

    watch<IBMDSwitcher, IBMDSwitcherCallback, BMDSwitcherEventType, BMDSwitcherVideoMode>(sw, "IBMDSwitcher", events, "Switcher", m_removers);
    watch<IBMDSwitcherMixEffectBlock, IBMDSwitcherMixEffectBlockCallback, BMDSwitcherMixEffectBlockEventType>(me, "IBMDSwitcherMixEffectBlock", events, "ME", m_removers);
    watch<IBMDSwitcherTransitionParameters, IBMDSwitcherTransitionParametersCallback, BMDSwitcherTransitionParametersEventType>(trans, "IBMDSwitcherTransitionParameters", events, "Trans", m_removers);
    watch<IBMDSwitcherKey, IBMDSwitcherKeyCallback, BMDSwitcherKeyEventType>(key, "IBMDSwitcherKey", events, "Key", m_removers);
    watch<IBMDSwitcherKeyFlyParameters, IBMDSwitcherKeyFlyParametersCallback, BMDSwitcherKeyFlyParametersEventType, BMDSwitcherFlyKeyFrame>(fly, "IBMDSwitcherKeyFlyParameters", events, "Fly", m_removers);
    watch<IBMDSwitcherKeyDVEParameters, IBMDSwitcherKeyDVEParametersCallback, BMDSwitcherKeyDVEParametersEventType>(dve, "IBMDSwitcherKeyDVEParameters", events, "DVE", m_removers);
    watch<IBMDSwitcherDownstreamKey, IBMDSwitcherDownstreamKeyCallback, BMDSwitcherDownstreamKeyEventType>(dsk, "IBMDSwitcherDownstreamKey", events, "DSK", m_removers);
    watch<IBMDSwitcherMacroPool, IBMDSwitcherMacroPoolCallback, BMDSwitcherMacroPoolEventType, unsigned int, IBMDSwitcherTransferMacro*>(pool, "IBMDSwitcherMacroPool", events, "Pool", m_removers);
    watch<IBMDSwitcherMacroControl, IBMDSwitcherMacroControlCallback, BMDSwitcherMacroControlEventType>(macros, "IBMDSwitcherMacroControl", events, "Macro", m_removers);
    for (auto* in : inputs) {
        BMDSwitcherInputId id = -1;
        in->GetInputId(&id);
        watch<IBMDSwitcherInput, IBMDSwitcherInputCallback, BMDSwitcherInputEventType>(in, "IBMDSwitcherInput", events, QString("Input:%1").arg(id), m_removers);
    }
    return S_OK;
}

void Switcher::disconnect() {
    for (auto it = m_removers.rbegin(); it != m_removers.rend(); ++it) (*it)();
    m_removers.clear();
    for (auto*& in : inputs) release(in);
    inputs.clear();
    release(macros);
    release(pool);
    release(dsk);
    release(dve);
    release(fly);
    release(key);
    release(trans);
    release(me);
    release(sw);
}
