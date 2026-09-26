// Downstream key 1 and macros (run/stop only — never recorded, renamed or deleted).
#include "tests_common.h"

#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace {

IBMDSwitcherDownstreamKey* dsk(Ctx& c) {
    c.needConnection();
    if (!c.s.dsk) c.skip("no downstream key");
    return c.s.dsk;
}

IBMDSwitcherMacroControl* macros(Ctx& c) {
    c.needConnection();
    if (!c.s.macros || !c.s.pool) c.skip("no macro interfaces");
    return c.s.macros;
}

QJsonObject runStatus(IBMDSwitcherMacroControl* m) {
    BMDSwitcherMacroRunStatus status{};
    BOOL loop = FALSE;
    unsigned int index = 0;
    HRESULT hr = SDK_CALL(IBMDSwitcherMacroControl, m, GetRunStatus, &status, &loop, &index);
    return QJsonObject{ { "hr", hrText(hr) }, { "status", static_cast<double>(status) },
                        { "loop", loop != FALSE }, { "index", static_cast<double>(index) } };
}

bool idle(IBMDSwitcherMacroControl* m) {
    BMDSwitcherMacroRunStatus status{};
    BOOL loop = FALSE;
    unsigned int index = 0;
    SDK_CALL(IBMDSwitcherMacroControl, m, GetRunStatus, &status, &loop, &index);
    return status == bmdSwitcherMacroRunStatusIdle;
}

bool waitIdle(IBMDSwitcherMacroControl* m, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (idle(m)) return true;
        QThread::msleep(30);
    }
    return false;
}

std::vector<unsigned int> validMacros(Ctx& c) {
    std::vector<unsigned int> out;
    unsigned int max = 0;
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetMaxCount, &max);
    for (unsigned int i = 0; i < max; ++i) {
        BOOL valid = FALSE;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, IsValid, i, &valid)) && valid) out.push_back(i);
    }
    return out;
}

} // namespace

void registerDownstreamKeyTests() {
    addTest("dsk.state", "Downstream key 1: all getters", [](Ctx& c) {
        auto* d = dsk(c);
        read<BMDSwitcherInputId>(c, "fill", [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetInputFill, v); });
        read<BMDSwitcherInputId>(c, "cut", [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetInputCut, v); });
        BMDSwitcherInputAvailability m{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherDownstreamKey, d, GetFillInputAvailabilityMask, &m))) c.observe("fillAvailabilityMask", static_cast<double>(m));
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherDownstreamKey, d, GetCutInputAvailabilityMask, &m))) c.observe("cutAvailabilityMask", static_cast<double>(m));
        read<BOOL>(c, "tie", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetTie, v); });
        read<unsigned int>(c, "rate", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetRate, v); });
        read<BOOL>(c, "onAir", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetOnAir, v); });
        read<BOOL>(c, "transitioning", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, IsTransitioning, v); });
        read<BOOL>(c, "autoTransitioning", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, IsAutoTransitioning, v); });
        read<BOOL>(c, "towardsOnAir", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, IsTransitionTowardsOnAir, v); });
        read<unsigned int>(c, "framesRemaining", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetFramesRemaining, v); });
        read<BOOL>(c, "preMultiplied", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetPreMultiplied, v); });
        read<double>(c, "clip", [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetClip, v); });
        read<double>(c, "gain", [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetGain, v); });
        read<BOOL>(c, "inverse", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetInverse, v); });
        read<BOOL>(c, "masked", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMasked, v); });
        read<double>(c, "maskTop", [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskTop, v); });
        read<double>(c, "maskBottom", [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskBottom, v); });
        read<double>(c, "maskLeft", [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskLeft, v); });
        read<double>(c, "maskRight", [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskRight, v); });
    });

    for (BMDSwitcherInputId id : { 1, 3010, 9999 })
        addTest(QString("dsk.fill.%1").arg(id), "DSK fill -> " + inputName(id), [id](Ctx& c) {
            auto* d = dsk(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetInputFill, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetInputFill, v); }, id != 9999);
        });
    for (BMDSwitcherInputId id : { 3011, 1, 9999 })
        addTest(QString("dsk.cut.%1").arg(id), "DSK cut -> " + inputName(id), [id](Ctx& c) {
            auto* d = dsk(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetInputCut, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetInputCut, v); }, false);
        });
    for (BOOL v : { TRUE, FALSE })
        addTest(QString("dsk.onair.%1").arg(v ? "on" : "off"), QString("DSK on air %1").arg(v ? "on" : "off"), [v](Ctx& c) {
            auto* d = dsk(c);
            probe<BOOL>(c, v, [&](BOOL x) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetOnAir, x); },
                              [&](BOOL* x) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetOnAir, x); }, true);
        });
    for (BOOL v : { TRUE, FALSE })
        addTest(QString("dsk.tie.%1").arg(v ? "on" : "off"), QString("DSK tie %1").arg(v ? "on" : "off"), [v](Ctx& c) {
            auto* d = dsk(c);
            probe<BOOL>(c, v, [&](BOOL x) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetTie, x); },
                              [&](BOOL* x) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetTie, x); }, true);
        });
    for (unsigned int v : { 25u, 0u, 10000u })
        addTest(QString("dsk.rate.%1").arg(v), QString("DSK rate = %1").arg(v), [v](Ctx& c) {
            auto* d = dsk(c);
            probe<unsigned int>(c, v, [&](unsigned int x) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetRate, x); },
                                      [&](unsigned int* x) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetRate, x); }, v == 25);
        });
    addTest("dsk.key-settings", "DSK pre-multiplied, clip, gain, inverse, mask (good and bad)", [](Ctx& c) {
        auto* d = dsk(c);
        QJsonObject r;
        auto dbl = [&](const QString& name, auto set, auto get, std::initializer_list<double> values) {
            QJsonObject o;
            for (double v : values) {
                HRESULT hr = set(v);
                c.settle();
                double back = 0;
                get(&back);
                o[QString::number(v)] = QJsonObject{ { "set", hrText(hr) }, { "readback", back } };
            }
            r[name] = o;
        };
        auto bol = [&](const QString& name, auto set, auto get) {
            QJsonObject o;
            for (BOOL v : { TRUE, FALSE }) {
                HRESULT hr = set(v);
                c.settle();
                BOOL back = FALSE;
                get(&back);
                o[v ? "on" : "off"] = QJsonObject{ { "set", hrText(hr) }, { "readback", back != FALSE } };
            }
            r[name] = o;
        };
        bol("preMultiplied", [&](BOOL v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetPreMultiplied, v); },
                             [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetPreMultiplied, v); });
        dbl("clip", [&](double v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetClip, v); },
                    [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetClip, v); }, { 0, 50, 100, -1, 1000 });
        dbl("gain", [&](double v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetGain, v); },
                    [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetGain, v); }, { 0, 50, 100, -1, 1000 });
        bol("inverse", [&](BOOL v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetInverse, v); },
                       [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetInverse, v); });
        bol("masked", [&](BOOL v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetMasked, v); },
                      [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMasked, v); });
        dbl("maskTop", [&](double v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetMaskTop, v); },
                       [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskTop, v); }, { 9, 0, 20 });
        dbl("maskBottom", [&](double v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetMaskBottom, v); },
                          [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskBottom, v); }, { -9, 0, -20 });
        dbl("maskLeft", [&](double v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetMaskLeft, v); },
                        [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskLeft, v); }, { -16, 0, -40 });
        dbl("maskRight", [&](double v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, SetMaskRight, v); },
                         [&](double* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetMaskRight, v); }, { 16, 0, 40 });
        c.hr("resetMask", SDK_CALL(IBMDSwitcherDownstreamKey, d, ResetMask));
        c.observe("results", r);
    });
    addTest("dsk.auto", "DSK auto transition on, then off", [](Ctx& c) {
        auto* d = dsk(c);
        for (int pass = 0; pass < 2; ++pass) {
            QString k = pass == 0 ? "toggle1" : "toggle2";
            c.hr(k + ".hr", SDK_CALL(IBMDSwitcherDownstreamKey, d, PerformAutoTransition));
            c.settle(300, 5000);
            read<BOOL>(c, k + ".onAir", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetOnAir, v); });
        }
        c.hr("towardsOnAir.hr", SDK_CALL(IBMDSwitcherDownstreamKey, d, PerformAutoTransitionInDirection, FALSE));
        c.settle(300, 5000);
        read<BOOL>(c, "afterDirection.onAir", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherDownstreamKey, d, GetOnAir, v); });
    });
}

void registerMacroTests() {
    addTest("macro.pool", "Macro pool: every slot", [](Ctx& c) {
        c.needConnection();
        if (!c.s.pool) c.skip("no macro pool");
        unsigned int max = 0;
        c.hr("maxCount.hr", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetMaxCount, &max));
        c.observe("maxCount", static_cast<double>(max));
        QJsonObject slotList;
        for (unsigned int i = 0; i < max; ++i) {
            BOOL valid = FALSE;
            SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, IsValid, i, &valid);
            if (!valid) continue;
            BSTR name = nullptr, desc = nullptr;
            SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetName, i, &name);
            SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetDescription, i, &desc);
            BOOL unsupported = FALSE;
            SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, HasUnsupportedOps, i, &unsupported);
            slotList[QString::number(i)] = QJsonObject{ { "name", takeBstr(name) }, { "description", takeBstr(desc) },
                                                     { "unsupportedOps", unsupported != FALSE } };
        }
        c.observe("validSlots", slotList);
        BSTR bad = nullptr;
        c.hr("getName.outOfRange", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetName, max + 5, &bad));
        takeBstr(bad);
        BOOL v = FALSE;
        c.hr("isValid.outOfRange", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, IsValid, max + 5, &v));
    });

    addTest("macro.status", "Macro run / record status (idle)", [](Ctx& c) {
        auto* m = macros(c);
        c.observe("run", runStatus(m));
        BMDSwitcherMacroRecordStatus rs{};
        unsigned int idx = 0;
        c.hr("record.hr", SDK_CALL(IBMDSwitcherMacroControl, m, GetRecordStatus, &rs, &idx));
        c.observe("recordStatus", static_cast<double>(rs));
        BOOL loop = FALSE;
        c.hr("loop.hr", SDK_CALL(IBMDSwitcherMacroControl, m, GetLoop, &loop));
        c.observe("loop", loop != FALSE);
    });

    // One test per stored macro slot (slots are read at run time).
    for (unsigned int slot = 0; slot < 8; ++slot)
        addTest(QString("macro.run.%1").arg(slot), QString("Run macro slot %1 (if stored) to completion").arg(slot), [slot](Ctx& c) {
            auto* m = macros(c);
            auto valid = validMacros(c);
            if (std::find(valid.begin(), valid.end(), slot) == valid.end()) c.skip("slot empty");
            BSTR name = nullptr;
            SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetName, slot, &name);
            c.observe("name", takeBstr(name));
            c.hr("run", SDK_CALL(IBMDSwitcherMacroControl, m, Run, slot));
            bool started = c.waitEvent("Macro", bmdSwitcherMacroControlEventTypeRunStatusChanged, 1500);
            c.observe("runStatusEvent", started);
            c.observe("whileRunning", runStatus(m));
            bool done = waitIdle(m, 20000);
            c.observe("finished", done);
            if (!done) {
                SDK_CALL(IBMDSwitcherMacroControl, m, StopRunning);
                c.expect(false, "macro still running after 20 s (stopped)");
            }
            c.settle(200, 2000);
            c.observe("after", runStatus(m));
        });

    addTest("macro.run.empty-slot", "Run an empty macro slot (bad)", [](Ctx& c) {
        auto* m = macros(c);
        unsigned int max = 0;
        SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetMaxCount, &max);
        auto valid = validMacros(c);
        unsigned int empty = max;
        for (unsigned int i = 0; i < max; ++i)
            if (std::find(valid.begin(), valid.end(), i) == valid.end()) { empty = i; break; }
        if (empty == max) c.skip("no empty slot");
        c.observe("slot", static_cast<double>(empty));
        c.hr("run", SDK_CALL(IBMDSwitcherMacroControl, m, Run, empty));
        c.settle(200, 1500);
        c.observe("after", runStatus(m));
    });
    for (unsigned int idx : { 100u, 65535u, 0xFFFFFFFFu })
        addTest(QString("macro.run.bad.%1").arg(idx), QString("Run macro index %1 (bad)").arg(idx), [idx](Ctx& c) {
            auto* m = macros(c);
            c.hr("run", SDK_CALL(IBMDSwitcherMacroControl, m, Run, idx));
            c.settle(200, 1500);
            c.observe("after", runStatus(m));
        });

    addTest("macro.stop.idle", "Stop when nothing is running", [](Ctx& c) {
        auto* m = macros(c);
        c.hr("stop", SDK_CALL(IBMDSwitcherMacroControl, m, StopRunning));
        c.settle();
        c.observe("after", runStatus(m));
    });
    addTest("macro.stop.running", "Run the first macro, stop it at once", [](Ctx& c) {
        auto* m = macros(c);
        auto valid = validMacros(c);
        if (valid.empty()) c.skip("no stored macros");
        c.hr("run", SDK_CALL(IBMDSwitcherMacroControl, m, Run, valid.front()));
        c.hr("stop", SDK_CALL(IBMDSwitcherMacroControl, m, StopRunning));
        c.observe("stopped", waitIdle(m, 5000));
        c.settle();
        c.observe("after", runStatus(m));
    });
    addTest("macro.resume.idle", "Resume when nothing is waiting", [](Ctx& c) {
        auto* m = macros(c);
        c.hr("resume", SDK_CALL(IBMDSwitcherMacroControl, m, ResumeRunning));
        c.settle();
        c.observe("after", runStatus(m));
    });
    for (BOOL v : { TRUE, FALSE })
        addTest(QString("macro.loop.%1").arg(v ? "on" : "off"), QString("Macro loop %1").arg(v ? "on" : "off"), [v](Ctx& c) {
            auto* m = macros(c);
            probe<BOOL>(c, v, [&](BOOL x) { return SDK_CALL(IBMDSwitcherMacroControl, m, SetLoop, x); },
                              [&](BOOL* x) { return SDK_CALL(IBMDSwitcherMacroControl, m, GetLoop, x); }, true);
        });
}
