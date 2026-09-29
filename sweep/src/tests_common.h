#pragma once
// Shared helpers for the test groups.

#include "runner.h"
#include "wireproxy.h"

#include <QJsonArray>
#include <cmath>
#include <vector>

// Every source an ATEM Mini offers, and ids it must reject.
inline const std::vector<BMDSwitcherInputId> kMiniSources = { 0, 1, 2, 3, 4, 1000, 2001, 2002, 3010, 3011 };
inline const std::vector<BMDSwitcherInputId> kBadSources = { 5, 9999, -1, 10010, 6000 };

// Test-id friendly number: -16 -> "m16", 0.25 -> "0p25".
inline QString idNum(double v) {
    QString s = QString::number(v, 'g', 6);
    s.replace('-', 'm').replace('.', 'p').replace('+', "");
    return s;
}

inline QJsonValue js(double v) { return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QString::number(v)); }
inline QJsonValue js(BOOL v) { return QJsonValue(v != FALSE); }
inline QJsonValue js(unsigned int v) { return QJsonValue(static_cast<double>(v)); }
inline QJsonValue js(BMDSwitcherInputId v) { return QJsonValue(static_cast<double>(v)); }
inline QJsonValue js(unsigned short v) { return QJsonValue(static_cast<double>(v)); }
inline QJsonValue js(short v) { return QJsonValue(static_cast<double>(v)); }
// A number of any integer type (int is BOOL on Windows, so js(int) records a bool).
template <class T> QJsonValue jsNum(T v) { return QJsonValue(static_cast<double>(v)); }
template <class E, class = std::enable_if_t<std::is_enum_v<E>>>
inline QJsonValue js(E v) { return QJsonValue(fourcc(static_cast<uint32_t>(v))); }

inline bool same(double a, double b) { return std::abs(a - b) <= 1e-3 + 1e-4 * std::abs(b); }
template <class T> bool same(const T& a, const T& b) { return a == b; }

// Sets `value`, lets the switcher answer, reads it back.
// good = true: the value is in range, so set must succeed and read back equal.
// good = false: out-of-range probe; whatever happens is only recorded.
template <class T, class Set, class Get>
void probe(Ctx& c, T value, Set set, Get get, bool good) {
    c.needConnection();
    c.observe("value", js(value));
    HRESULT hs = set(value);
    c.hr("set", hs);
    c.settle();
    T back{};
    HRESULT hg = get(&back);
    c.hr("get", hg);
    if (SUCCEEDED(hg)) c.observe("readback", js(back));
    if (good) {
        c.expect(SUCCEEDED(hs), "set returned " + hrText(hs));
        c.expect(SUCCEEDED(hg) && same(back, value),
                 QString("read back %1, expected %2")
                     .arg(js(back).toVariant().toString(), js(value).toVariant().toString()));
    }
}

// Like probe(), for reverse engineering: the original value is read first,
// `value` is set and read back (recorded whatever it is, good or bad), then
// the original is put back and checked. Nothing is set when the original
// can't be read (it could not be put back).
template <class T, class Set, class Get>
void probeRestore(Ctx& c, T value, Set set, Get get) {
    c.needConnection();
    T original{};
    HRESULT h0 = get(&original);
    if (FAILED(h0)) {
        c.hr("get", h0);
        // Not implemented for this switcher (e.g. talkback on the ATEM Mini):
        // the set is refused by the SDK too, and the proxy shows that nothing
        // reached the switcher, so there is nothing to put back.
        if (h0 != E_NOTIMPL || !c.wire)
            c.skip("the current value can't be read, so it could not be put back: not set");
        size_t mark = c.wire->mark();
        c.observe("value", js(value));
        c.hr("set", set(value));
        c.settle();
        bool sent = !c.wire->fieldsSince(mark)["tx"].toArray().isEmpty();
        c.observe("sentToSwitcher", sent);
        c.expect(!sent, "a command reached the switcher for a value that can't be read back (not put back)");
        return;
    }
    probe<T>(c, value, set, get, false);
    // Put back only if it changed: a refused set (e.g. E_NOTIMPL) left it alone.
    T now{};
    if (SUCCEEDED(get(&now)) && same(now, original)) return;
    HRESULT hr = set(original);
    c.settle();
    T back{};
    HRESULT hb = get(&back);
    c.expect(SUCCEEDED(hb) && same(back, original),
             QString("the original value was not put back (set %1, now %2, was %3)")
                 .arg(hrText(hr), js(back).toVariant().toString(), js(original).toVariant().toString()));
}

// Records a getter's HRESULT and value under `name`.
template <class T, class Get>
T read(Ctx& c, const QString& name, Get get) {
    T v{};
    HRESULT hr = get(&v);
    if (SUCCEEDED(hr)) c.observe(name, js(v));
    else c.observe(name, "error " + hrText(hr));
    return v;
}

// Gets an interface from `obj`; records "not on this model" when refused.
template <class I, class Obj>
I* query(Ctx& c, Obj* obj, const char* name) {
    if (!obj) return nullptr;
    I* out = nullptr;
    HRESULT hr = obj->QueryInterface(__uuidof(I), reinterpret_cast<void**>(&out));
    c.observe(QString("%1.available").arg(name), SUCCEEDED(hr) && out);
    if (FAILED(hr) || !out) {
        sweepUnavailable(name);
        return nullptr;
    }
    return out;
}

// Releases a COM pointer when it goes out of scope.
template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    explicit Com(T* x) : p(x) {}
    ~Com() { if (p) p->Release(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    T** out() { return &p; }
};
