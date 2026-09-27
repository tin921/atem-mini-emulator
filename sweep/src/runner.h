#pragma once
// Test registry, per-test context and the run loop for atem-sweep.

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <functional>
#include <vector>

#include "sdk.h"

class WireProxy;

struct Options {
    QString target;          // device address as given ("" = USB)
    QString connectAddress;  // what the SDK connects to (proxy or target)
    bool useProxy = false;
    bool verify = false;     // compare against a golden record
    QString goldenPath;
    QString outDir;
    QString only;            // run tests whose id starts with this
    bool listOnly = false;
    bool allowCamera = false; // send camera actions (autofocus) to BMD cameras
    QString coverageDir;     // tier1-plugin.txt, tier2-samples.txt, excluded.txt
};

struct SkipTest {
    QString why;
};

// Handed to every test. Everything a test observes goes into `obs`; in
// verify mode `obs` and the event set are compared with the golden record.
struct Ctx {
    Switcher& s;
    WireProxy* wire;
    const Options& opt;
    size_t eventMark = 0;
    QJsonObject obs;
    QStringList problems;   // failed expectations (always reported)

    void observe(const QString& key, const QJsonValue& value) { obs.insert(key, value); }
    HRESULT hr(const QString& key, HRESULT h) {
        obs.insert(key, hrText(h));
        return h;
    }
    void expect(bool ok, const QString& what) {
        if (!ok) problems << what;
    }
    // Waits for an SDK event fired since the test started.
    bool waitEvent(const QString& source, uint32_t type, int timeoutMs = 800) {
        return s.events.waitFor(source, type, eventMark, timeoutMs);
    }
    // Lets echoes from the switcher arrive: returns after quietMs without events.
    void settle(int quietMs = 150, int maxMs = 1500) { s.events.settle(quietMs, maxMs); }
    void needConnection() {
        if (!s.connected()) throw SkipTest{ "not connected" };
    }
    [[noreturn]] void skip(const QString& why) { throw SkipTest{ why }; }
};

struct Test {
    QString id;       // stable, dotted: "me.program.cam1"
    QString title;
    std::function<void(Ctx&)> run;
};

std::vector<Test>& registry();
void addTest(const QString& id, const QString& title, std::function<void(Ctx&)> run);

// Test groups (tests_*.cpp).
void registerConnectTests();
void registerInputTests();
void registerMixEffectTests();
void registerKeyTests();
void registerDownstreamKeyTests();
void registerMacroTests();
void registerMediaTests();
void registerDeviceTests();

// Restores what the sweep changed (snapshot.cpp): the settings listed in
// Snapshot. Returns false if anything could not be read at the start, could
// not be set, or reads back differently afterwards (all listed in log).
struct Snapshot;
Snapshot* takeSnapshot(Switcher& s);
bool restoreSnapshot(Switcher& s, Snapshot* snap, QStringList& log);

int runSweep(Switcher& s, WireProxy* wire, const Options& opt);
