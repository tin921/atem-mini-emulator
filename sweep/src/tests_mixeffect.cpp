// Mix effect block: program/preview, cut, auto, T-bar, fade to black, transitions.
#include "tests_common.h"

#include <QElapsedTimer>
#include <QThread>

namespace {

IBMDSwitcherMixEffectBlock* me(Ctx& c) {
    c.needConnection();
    if (!c.s.me) c.skip("no mix effect block");
    return c.s.me;
}

template <class Get>
bool pollUntil(Get cond, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cond()) return true;
        QThread::msleep(20);
    }
    return false;
}

void observeState(Ctx& c, IBMDSwitcherMixEffectBlock* m, const QString& prefix) {
    read<BMDSwitcherInputId>(c, prefix + "program", [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetProgramInput, v); });
    read<BMDSwitcherInputId>(c, prefix + "preview", [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetPreviewInput, v); });
    read<BOOL>(c, prefix + "inTransition", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetInTransition, v); });
    read<double>(c, prefix + "transitionPosition", [&](double* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetTransitionPosition, v); });
}

} // namespace

void registerMixEffectTests() {
    addTest("me.state", "M/E state (all getters)", [](Ctx& c) {
        auto* m = me(c);
        observeState(c, m, "");
        read<BOOL>(c, "previewLive", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetPreviewLive, v); });
        read<BOOL>(c, "previewTransition", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetPreviewTransition, v); });
        read<unsigned int>(c, "transitionFramesRemaining", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetTransitionFramesRemaining, v); });
        read<unsigned int>(c, "fadeToBlackRate", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetFadeToBlackRate, v); });
        read<unsigned int>(c, "fadeToBlackFramesRemaining", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetFadeToBlackFramesRemaining, v); });
        read<BOOL>(c, "fadeToBlackFullyBlack", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetFadeToBlackFullyBlack, v); });
        read<BOOL>(c, "inFadeToBlack", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetInFadeToBlack, v); });
        read<BOOL>(c, "fadeToBlackInTransition", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetFadeToBlackInTransition, v); });
        BMDSwitcherInputAvailability mask{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetInputAvailabilityMask, &mask)))
            c.observe("inputAvailabilityMask", static_cast<double>(mask));
        BMDSwitcherMixEffectBlockTallyEnabledFlags tally{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetTallyConfig, &tally)))
            c.observe("tallyConfig", static_cast<double>(tally));
    });

    for (BMDSwitcherInputId id : kMiniSources) {
        addTest(QString("me.program.%1").arg(id), "Program -> " + inputName(id), [id](Ctx& c) {
            auto* m = me(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetProgramInput, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetProgramInput, v); }, true);
        });
    }
    for (BMDSwitcherInputId id : kBadSources) {
        addTest(QString("me.program.bad.%1").arg(idNum(static_cast<double>(id))), QString("Program -> %1 (bad)").arg(id), [id](Ctx& c) {
            auto* m = me(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetProgramInput, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetProgramInput, v); }, false);
        });
    }
    for (BMDSwitcherInputId id : { 1, 2, 3, 4, 1000 }) {
        addTest(QString("me.preview.%1").arg(id), "Preview -> " + inputName(id), [id](Ctx& c) {
            auto* m = me(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetPreviewInput, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetPreviewInput, v); }, true);
        });
    }
    addTest("me.preview.bad.9999", "Preview -> 9999 (bad)", [](Ctx& c) {
        auto* m = me(c);
        probe<BMDSwitcherInputId>(c, 9999,
            [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetPreviewInput, v); },
            [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetPreviewInput, v); }, false);
    });

    addTest("me.preview-transition", "Preview transition on, then off", [](Ctx& c) {
        auto* m = me(c);
        for (BOOL on : { TRUE, FALSE }) {
            QString k = on ? "on" : "off";
            c.hr(k + ".set", SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetPreviewTransition, on));
            c.settle();
            read<BOOL>(c, k + ".readback", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetPreviewTransition, v); });
        }
    });

    addTest("me.cut", "Cut: Camera 1 on program, Camera 2 on preview", [](Ctx& c) {
        auto* m = me(c);
        SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetProgramInput, 1);
        SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetPreviewInput, 2);
        c.settle();
        size_t mark = c.s.events.size();
        c.hr("cut", SDK_CALL(IBMDSwitcherMixEffectBlock, m, PerformCut));
        c.s.events.waitFor("ME", bmdSwitcherMixEffectBlockEventTypeProgramInputChanged, mark, 1000);
        c.settle();
        observeState(c, m, "after.");
        BMDSwitcherInputId p = 0, v = 0;
        SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetProgramInput, &p);
        SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetPreviewInput, &v);
        c.expect(p == 2 && v == 1, QString("after cut program=%1 preview=%2, expected 2/1").arg(p).arg(v));
    });

    addTest("me.auto", "Auto transition (mix) runs and completes", [](Ctx& c) {
        auto* m = me(c);
        if (c.s.trans) SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, SetNextTransitionStyle, bmdSwitcherTransitionStyleMix);
        SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetProgramInput, 1);
        SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetPreviewInput, 3);
        c.settle();
        c.hr("auto", SDK_CALL(IBMDSwitcherMixEffectBlock, m, PerformAutoTransition));
        bool started = pollUntil([&] { BOOL b = FALSE; SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetInTransition, &b); return b != FALSE; }, 1500);
        c.observe("started", started);
        bool finished = pollUntil([&] { BOOL b = TRUE; SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetInTransition, &b); return b == FALSE; }, 6000);
        c.observe("finished", finished);
        c.settle();
        observeState(c, m, "after.");
        c.expect(started && finished, "transition did not run to completion");
    });

    addTest("me.tbar", "T-bar to 0.5, then back to 0 (cancel)", [](Ctx& c) {
        auto* m = me(c);
        c.hr("half.set", SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetTransitionPosition, 0.5));
        c.settle();
        observeState(c, m, "half.");
        c.hr("zero.set", SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetTransitionPosition, 0.0));
        c.settle();
        observeState(c, m, "zero.");
    });
    for (double v : { -0.5, 1.5 }) {
        addTest(QString("me.tbar.bad.%1").arg(idNum(v)), QString("T-bar to %1 (bad)").arg(v), [v](Ctx& c) {
            auto* m = me(c);
            c.hr("set", SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetTransitionPosition, v));
            c.settle();
            observeState(c, m, "");
            SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetTransitionPosition, 0.0);
            c.settle();
        });
    }

    addTest("me.ftb", "Fade to black, then back", [](Ctx& c) {
        auto* m = me(c);
        auto fully = [&] { BOOL b = FALSE; SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetFadeToBlackFullyBlack, &b); return b != FALSE; };
        auto busy = [&] { BOOL b = FALSE; SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetInFadeToBlack, &b); return b != FALSE; };
        c.hr("fade.hr", SDK_CALL(IBMDSwitcherMixEffectBlock, m, PerformFadeToBlack));
        c.observe("wentBlack", pollUntil(fully, 6000));
        read<unsigned int>(c, "framesRemainingWhenBlack", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetFadeToBlackFramesRemaining, v); });
        c.hr("back.hr", SDK_CALL(IBMDSwitcherMixEffectBlock, m, PerformFadeToBlack));
        c.observe("cameBack", pollUntil([&] { return !fully() && !busy(); }, 6000));
        c.settle();
        c.expect(!fully(), "still fully black after the second fade");
    });

    addTest("me.ftb.rate", "Fade-to-black rate: 25 frames, then 0 and 1000 (bad)", [](Ctx& c) {
        auto* m = me(c);
        QJsonObject r;
        for (unsigned int v : { 25u, 0u, 1000u }) {
            HRESULT hr = SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetFadeToBlackRate, v);
            c.settle();
            unsigned int back = 0;
            SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetFadeToBlackRate, &back);
            r[QString::number(v)] = QJsonObject{ { "set", hrText(hr) }, { "readback", static_cast<double>(back) } };
        }
        c.observe("rates", r);
    });

    // ── Transition parameters ──
    const std::vector<std::pair<BMDSwitcherTransitionStyle, const char*>> styles = {
        { bmdSwitcherTransitionStyleMix, "mix" }, { bmdSwitcherTransitionStyleDip, "dip" },
        { bmdSwitcherTransitionStyleWipe, "wipe" }, { bmdSwitcherTransitionStyleDVE, "dve" },
        { bmdSwitcherTransitionStyleStinger, "stinger" },
        { static_cast<BMDSwitcherTransitionStyle>(0x12345678), "bad" },
    };
    for (const auto& [style, label] : styles) {
        bool good = QString(label) != "bad" && QString(label) != "stinger";
        addTest(QString("trans.style.%1").arg(label), QString("Next transition style: %1").arg(label), [style, good](Ctx& c) {
            c.needConnection();
            if (!c.s.trans) c.skip("no transition parameters");
            read<BMDSwitcherTransitionStyle>(c, "current", [&](BMDSwitcherTransitionStyle* v) {
                return SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, GetTransitionStyle, v);
            });
            probe<BMDSwitcherTransitionStyle>(c, style,
                [&](BMDSwitcherTransitionStyle v) { return SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, SetNextTransitionStyle, v); },
                [&](BMDSwitcherTransitionStyle* v) { return SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, GetNextTransitionStyle, v); }, good);
        });
    }
    const std::vector<std::pair<BMDSwitcherTransitionSelection, const char*>> selections = {
        { bmdSwitcherTransitionSelectionBackground, "background" },
        { static_cast<BMDSwitcherTransitionSelection>(bmdSwitcherTransitionSelectionBackground | bmdSwitcherTransitionSelectionKey1), "background+key1" },
        { bmdSwitcherTransitionSelectionKey1, "key1" },
        { static_cast<BMDSwitcherTransitionSelection>(0), "none" },
        { static_cast<BMDSwitcherTransitionSelection>(0x1F), "all-keys" },
    };
    for (const auto& [sel, label] : selections) {
        bool good = sel == bmdSwitcherTransitionSelectionBackground;
        addTest(QString("trans.selection.%1").arg(label), QString("Next transition selection: %1").arg(label), [sel, good](Ctx& c) {
            c.needConnection();
            if (!c.s.trans) c.skip("no transition parameters");
            read<BMDSwitcherTransitionSelection>(c, "current", [&](BMDSwitcherTransitionSelection* v) {
                return SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, GetTransitionSelection, v);
            });
            c.observe("value", static_cast<double>(sel));
            HRESULT hr = SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, SetNextTransitionSelection, sel);
            c.hr("set", hr);
            c.settle();
            BMDSwitcherTransitionSelection back{};
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, GetNextTransitionSelection, &back)))
                c.observe("readback", static_cast<double>(back));
            if (good) c.expect(back == sel, "selection did not read back");
        });
    }

    addTest("trans.wipe", "Wipe transition: pattern and rate (good and bad)", [](Ctx& c) {
        auto* m = me(c);
        Com<IBMDSwitcherTransitionWipeParameters> wipe(query<IBMDSwitcherTransitionWipeParameters>(c, m, "IBMDSwitcherTransitionWipeParameters"));
        if (!wipe) c.skip("no wipe parameters");
        read<unsigned int>(c, "rate", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherTransitionWipeParameters, wipe.p, GetRate, v); });
        read<BMDSwitcherPatternStyle>(c, "pattern", [&](BMDSwitcherPatternStyle* v) { return SDK_CALL(IBMDSwitcherTransitionWipeParameters, wipe.p, GetPattern, v); });
        QJsonObject rates;
        for (unsigned int v : { 25u, 1u, 250u, 0u, 1000u }) {
            HRESULT hr = SDK_CALL(IBMDSwitcherTransitionWipeParameters, wipe.p, SetRate, v);
            c.settle();
            unsigned int back = 0;
            SDK_CALL(IBMDSwitcherTransitionWipeParameters, wipe.p, GetRate, &back);
            rates[QString::number(v)] = QJsonObject{ { "set", hrText(hr) }, { "readback", static_cast<double>(back) } };
        }
        c.observe("rates", rates);
        QJsonObject patterns;
        for (auto p : { bmdSwitcherPatternStyleLeftToRightBar, bmdSwitcherPatternStyleCircleIris,
                        static_cast<BMDSwitcherPatternStyle>(0x12345678) }) {
            HRESULT hr = SDK_CALL(IBMDSwitcherTransitionWipeParameters, wipe.p, SetPattern, p);
            c.settle();
            BMDSwitcherPatternStyle back{};
            SDK_CALL(IBMDSwitcherTransitionWipeParameters, wipe.p, GetPattern, &back);
            patterns[fourcc(static_cast<uint32_t>(p))] = QJsonObject{ { "set", hrText(hr) }, { "readback", fourcc(static_cast<uint32_t>(back)) } };
        }
        c.observe("patterns", patterns);
    });
}
