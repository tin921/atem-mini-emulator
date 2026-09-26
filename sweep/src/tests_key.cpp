// Upstream key 1: type, sources, on air, mask; fly (position/size) and DVE.
// Everything the obs-atem PiP panel uses is here.
#include "tests_common.h"

namespace {

IBMDSwitcherKey* key(Ctx& c) {
    c.needConnection();
    if (!c.s.key) c.skip("no upstream key");
    return c.s.key;
}
IBMDSwitcherKeyFlyParameters* fly(Ctx& c) {
    c.needConnection();
    if (!c.s.fly) c.skip("no fly parameters");
    return c.s.fly;
}
IBMDSwitcherKeyDVEParameters* dve(Ctx& c) {
    c.needConnection();
    if (!c.s.dve) c.skip("no DVE parameters");
    return c.s.dve;
}

// An ATEM Mini has one DVE: while the next transition style is DVE the key
// can't be a DVE key (SetType returns E_FAIL). Free it with a mix transition.
void freeDve(Ctx& c) {
    if (c.s.trans) SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, SetNextTransitionStyle, bmdSwitcherTransitionStyleMix);
    c.settle();
}

using DoubleSet = HRESULT (*)(void*, double);

// Registers one probe per value: good values must read back exactly.
template <class Obj, class Setter, class Getter>
void doubleSeries(const QString& idBase, const QString& what, Obj* (*get)(Ctx&),
                  Setter set, Getter read, std::initializer_list<double> good, std::initializer_list<double> bad) {
    for (double v : good)
        addTest(QString("%1.%2").arg(idBase, idNum(v)), QString("%1 = %2").arg(what).arg(v), [=](Ctx& c) {
            Obj* o = get(c);
            probe<double>(c, v, [&](double x) { return set(o, x); }, [&](double* x) { return read(o, x); }, true);
        });
    for (double v : bad)
        addTest(QString("%1.bad.%2").arg(idBase, idNum(v)), QString("%1 = %2 (out of range)").arg(what).arg(v), [=](Ctx& c) {
            Obj* o = get(c);
            probe<double>(c, v, [&](double x) { return set(o, x); }, [&](double* x) { return read(o, x); }, false);
        });
}

template <class Obj, class Setter, class Getter>
void boolPair(const QString& idBase, const QString& what, Obj* (*get)(Ctx&), Setter set, Getter read) {
    for (BOOL v : { TRUE, FALSE })
        addTest(QString("%1.%2").arg(idBase, v ? "on" : "off"), QString("%1 %2").arg(what, v ? "on" : "off"), [=](Ctx& c) {
            Obj* o = get(c);
            probe<BOOL>(c, v, [&](BOOL x) { return set(o, x); }, [&](BOOL* x) { return read(o, x); }, true);
        });
}

} // namespace

void registerKeyTests() {
    // ── Key ──
    addTest("key.state", "Upstream key 1: all getters", [](Ctx& c) {
        auto* k = key(c);
        read<BMDSwitcherKeyType>(c, "type", [&](BMDSwitcherKeyType* v) { return SDK_CALL(IBMDSwitcherKey, k, GetType, v); });
        read<BMDSwitcherInputId>(c, "fill", [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherKey, k, GetInputFill, v); });
        read<BMDSwitcherInputId>(c, "cut", [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherKey, k, GetInputCut, v); });
        read<BOOL>(c, "onAir", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKey, k, GetOnAir, v); });
        read<BOOL>(c, "canBeDVE", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKey, k, CanBeDVEKey, v); });
        read<BOOL>(c, "supportsAdvancedChroma", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKey, k, DoesSupportAdvancedChroma, v); });
        BMDSwitcherInputAvailability m{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherKey, k, GetFillInputAvailabilityMask, &m))) c.observe("fillAvailabilityMask", static_cast<double>(m));
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherKey, k, GetCutInputAvailabilityMask, &m))) c.observe("cutAvailabilityMask", static_cast<double>(m));
        read<BOOL>(c, "masked", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMasked, v); });
        read<double>(c, "maskTop", [&](double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskTop, v); });
        read<double>(c, "maskBottom", [&](double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskBottom, v); });
        read<double>(c, "maskLeft", [&](double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskLeft, v); });
        read<double>(c, "maskRight", [&](double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskRight, v); });
        BMDSwitcherTransitionSelection sel{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherKey, k, GetTransitionSelectionMask, &sel))) c.observe("transitionSelectionMask", static_cast<double>(sel));
    });

    const std::vector<std::pair<BMDSwitcherKeyType, const char*>> types = {
        { bmdSwitcherKeyTypeLuma, "luma" }, { bmdSwitcherKeyTypeChroma, "chroma" },
        { bmdSwitcherKeyTypePattern, "pattern" }, { bmdSwitcherKeyTypeDVE, "dve" },
        { static_cast<BMDSwitcherKeyType>(0x12345678), "bad" },
    };
    for (const auto& [type, label] : types) {
        bool good = QString(label) != "bad";
        addTest(QString("key.type.%1").arg(label), QString("Key type -> %1").arg(label), [type, good](Ctx& c) {
            auto* k = key(c);
            freeDve(c);
            probe<BMDSwitcherKeyType>(c, type,
                [&](BMDSwitcherKeyType v) { return SDK_CALL(IBMDSwitcherKey, k, SetType, v); },
                [&](BMDSwitcherKeyType* v) { return SDK_CALL(IBMDSwitcherKey, k, GetType, v); }, good);
        });
    }

    addTest("key.type.dve.while-dve-transition", "Key type -> DVE while the next transition is DVE (conflict)", [](Ctx& c) {
        auto* k = key(c);
        if (!c.s.trans) c.skip("no transition parameters");
        SDK_CALL(IBMDSwitcherKey, k, SetType, bmdSwitcherKeyTypeLuma);
        c.hr("setTransitionDve", SDK_CALL(IBMDSwitcherTransitionParameters, c.s.trans, SetNextTransitionStyle, bmdSwitcherTransitionStyleDVE));
        c.settle();
        probe<BMDSwitcherKeyType>(c, bmdSwitcherKeyTypeDVE,
            [&](BMDSwitcherKeyType v) { return SDK_CALL(IBMDSwitcherKey, k, SetType, v); },
            [&](BMDSwitcherKeyType* v) { return SDK_CALL(IBMDSwitcherKey, k, GetType, v); }, false);
        freeDve(c);
        SDK_CALL(IBMDSwitcherKey, k, SetType, bmdSwitcherKeyTypeDVE);
        c.settle();
    });

    for (BMDSwitcherInputId id : kMiniSources)
        addTest(QString("key.fill.%1").arg(id), "Key fill -> " + inputName(id), [id](Ctx& c) {
            auto* k = key(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherKey, k, SetInputFill, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherKey, k, GetInputFill, v); }, id != 3011);
        });
    for (BMDSwitcherInputId id : kBadSources)
        addTest(QString("key.fill.bad.%1").arg(idNum(static_cast<double>(id))), QString("Key fill -> %1 (bad)").arg(id), [id](Ctx& c) {
            auto* k = key(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherKey, k, SetInputFill, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherKey, k, GetInputFill, v); }, false);
        });
    for (BMDSwitcherInputId id : { 1, 3011, 9999 })
        addTest(QString("key.cut.%1").arg(id), QString("Key cut -> %1").arg(inputName(id)), [id](Ctx& c) {
            auto* k = key(c);
            probe<BMDSwitcherInputId>(c, id,
                [&](BMDSwitcherInputId v) { return SDK_CALL(IBMDSwitcherKey, k, SetInputCut, v); },
                [&](BMDSwitcherInputId* v) { return SDK_CALL(IBMDSwitcherKey, k, GetInputCut, v); }, false);
        });

    boolPair("key.onair", "Key on air", &key,
             [](IBMDSwitcherKey* k, BOOL v) { return SDK_CALL(IBMDSwitcherKey, k, SetOnAir, v); },
             [](IBMDSwitcherKey* k, BOOL* v) { return SDK_CALL(IBMDSwitcherKey, k, GetOnAir, v); });
    boolPair("key.masked", "Key mask", &key,
             [](IBMDSwitcherKey* k, BOOL v) { return SDK_CALL(IBMDSwitcherKey, k, SetMasked, v); },
             [](IBMDSwitcherKey* k, BOOL* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMasked, v); });
    doubleSeries("key.mask.top", "Key mask top", &key,
                 [](IBMDSwitcherKey* k, double v) { return SDK_CALL(IBMDSwitcherKey, k, SetMaskTop, v); },
                 [](IBMDSwitcherKey* k, double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskTop, v); },
                 { 9, 0, -9 }, { 20, -20 });
    doubleSeries("key.mask.bottom", "Key mask bottom", &key,
                 [](IBMDSwitcherKey* k, double v) { return SDK_CALL(IBMDSwitcherKey, k, SetMaskBottom, v); },
                 [](IBMDSwitcherKey* k, double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskBottom, v); },
                 { -9, 0, 9 }, { 20, -20 });
    doubleSeries("key.mask.left", "Key mask left", &key,
                 [](IBMDSwitcherKey* k, double v) { return SDK_CALL(IBMDSwitcherKey, k, SetMaskLeft, v); },
                 [](IBMDSwitcherKey* k, double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskLeft, v); },
                 { -16, 0, 16 }, { 40, -40 });
    doubleSeries("key.mask.right", "Key mask right", &key,
                 [](IBMDSwitcherKey* k, double v) { return SDK_CALL(IBMDSwitcherKey, k, SetMaskRight, v); },
                 [](IBMDSwitcherKey* k, double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskRight, v); },
                 { 16, 0, -16 }, { 40, -40 });
    addTest("key.mask.reset", "Key mask reset", [](Ctx& c) {
        auto* k = key(c);
        c.hr("reset", SDK_CALL(IBMDSwitcherKey, k, ResetMask));
        c.settle();
        read<double>(c, "maskTop", [&](double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskTop, v); });
        read<double>(c, "maskLeft", [&](double* v) { return SDK_CALL(IBMDSwitcherKey, k, GetMaskLeft, v); });
    });

    // ── Fly (position / size) ── the PiP panel's X, Y and Size.
    addTest("fly.state", "Fly parameters: all getters", [](Ctx& c) {
        auto* f = fly(c);
        freeDve(c);
        c.hr("setTypeDve", SDK_CALL(IBMDSwitcherKey, c.s.key, SetType, bmdSwitcherKeyTypeDVE));   // DVE key for the fly tests
        c.settle();
        read<BOOL>(c, "fly", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetFly, v); });
        read<BOOL>(c, "canFly", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetCanFly, v); });
        read<unsigned int>(c, "rate", [&](unsigned int* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetRate, v); });
        read<double>(c, "sizeX", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeX, v); });
        read<double>(c, "sizeY", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeY, v); });
        read<BOOL>(c, "canScaleUp", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetCanScaleUp, v); });
        read<double>(c, "positionX", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionX, v); });
        read<double>(c, "positionY", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionY, v); });
        read<double>(c, "rotation", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetRotation, v); });
        read<BOOL>(c, "canRotate", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetCanRotate, v); });
        BMDSwitcherFlyKeyFrame at{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherKeyFlyParameters, f, IsAtKeyFrames, &at))) c.observe("atKeyFrames", static_cast<double>(at));
        BOOL running = FALSE;
        BMDSwitcherFlyKeyFrame dest{};
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherKeyFlyParameters, f, IsRunning, &running, &dest))) c.observe("running", running != FALSE);
        for (auto [kf, label] : { std::pair{ bmdSwitcherFlyKeyFrameA, "A" }, std::pair{ bmdSwitcherFlyKeyFrameB, "B" } }) {
            BOOL stored = FALSE;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherKeyFlyParameters, f, IsKeyFrameStored, kf, &stored)))
                c.observe(QString("keyFrame%1Stored").arg(label), stored != FALSE);
        }
    });

    doubleSeries("fly.x", "PiP position X", &fly,
                 [](IBMDSwitcherKeyFlyParameters* f, double v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetPositionX, v); },
                 [](IBMDSwitcherKeyFlyParameters* f, double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionX, v); },
                 { -16, -8, 0, 0.1, 8.59, 16 }, { -32, 32, -100, 100 });
    doubleSeries("fly.y", "PiP position Y", &fly,
                 [](IBMDSwitcherKeyFlyParameters* f, double v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetPositionY, v); },
                 [](IBMDSwitcherKeyFlyParameters* f, double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionY, v); },
                 { -9, -4.83, 0, 0.1, 4.5, 9 }, { -18, 18, -100, 100 });
    doubleSeries("fly.sizex", "PiP size X", &fly,
                 [](IBMDSwitcherKeyFlyParameters* f, double v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetSizeX, v); },
                 [](IBMDSwitcherKeyFlyParameters* f, double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeX, v); },
                 { 0.1, 0.25, 0.363, 0.5, 1.0 }, { 0, 2, -1, 100 });
    doubleSeries("fly.sizey", "PiP size Y", &fly,
                 [](IBMDSwitcherKeyFlyParameters* f, double v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetSizeY, v); },
                 [](IBMDSwitcherKeyFlyParameters* f, double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeY, v); },
                 { 0.1, 0.25, 0.363, 0.5, 1.0 }, { 0, 2, -1, 100 });
    doubleSeries("fly.rotation", "Key rotation (degrees)", &fly,
                 [](IBMDSwitcherKeyFlyParameters* f, double v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetRotation, v); },
                 [](IBMDSwitcherKeyFlyParameters* f, double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetRotation, v); },
                 {}, { 0, 45, 359, -10, 720 });
    for (unsigned int v : { 1u, 25u, 250u, 0u, 10000u })
        addTest(QString("fly.rate.%1").arg(v), QString("Fly rate = %1 frames").arg(v), [v](Ctx& c) {
            auto* f = fly(c);
            probe<unsigned int>(c, v,
                [&](unsigned int x) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetRate, x); },
                [&](unsigned int* x) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetRate, x); }, v == 25);
        });
    addTest("fly.flag", "Fly flag on/off (luma key)", [](Ctx& c) {
        auto* f = fly(c);
        SDK_CALL(IBMDSwitcherKey, c.s.key, SetType, bmdSwitcherKeyTypeLuma);
        c.settle();
        for (BOOL v : { TRUE, FALSE }) {
            QString k = v ? "on" : "off";
            c.hr(k + ".set", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetFly, v));
            c.settle();
            read<BOOL>(c, k + ".readback", [&](BOOL* x) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetFly, x); });
        }
        SDK_CALL(IBMDSwitcherKey, c.s.key, SetType, bmdSwitcherKeyTypeDVE);
        c.settle();
    });
    addTest("fly.reset-dve", "ResetDVE (position/size/rotation defaults)", [](Ctx& c) {
        auto* f = fly(c);
        c.hr("reset", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, ResetDVE));
        c.settle();
        read<double>(c, "positionX", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionX, v); });
        read<double>(c, "positionY", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionY, v); });
        read<double>(c, "sizeX", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeX, v); });
        read<double>(c, "sizeY", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeY, v); });
    });
    addTest("fly.reset-dve-full", "ResetDVEFull (full screen)", [](Ctx& c) {
        auto* f = fly(c);
        c.hr("reset", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, ResetDVEFull));
        c.settle();
        read<double>(c, "sizeX", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeX, v); });
        read<double>(c, "positionX", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionX, v); });
    });
    addTest("fly.reset-rotation", "ResetRotation", [](Ctx& c) {
        auto* f = fly(c);
        c.hr("reset", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, ResetRotation));
        c.settle();
        read<double>(c, "rotation", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetRotation, v); });
    });
    addTest("fly.run-to-keyframe", "Run to stored key frames (no storing)", [](Ctx& c) {
        auto* f = fly(c);
        for (auto [kf, label] : { std::pair{ bmdSwitcherFlyKeyFrameFull, "full" }, std::pair{ bmdSwitcherFlyKeyFrameA, "A" } }) {
            BOOL stored = TRUE;
            if (kf == bmdSwitcherFlyKeyFrameA) SDK_CALL(IBMDSwitcherKeyFlyParameters, f, IsKeyFrameStored, kf, &stored);
            if (!stored) {
                c.observe(QString("%1.skipped").arg(label), "not stored");
                continue;
            }
            c.hr(QString("%1.run").arg(label), SDK_CALL(IBMDSwitcherKeyFlyParameters, f, RunToKeyFrame, kf));
            c.settle(300, 4000);
            read<double>(c, QString("%1.sizeX").arg(label), [&](double* v) { return SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeX, v); });
        }
    });

    // ── DVE (crop = mask, border, shadow) ──
    addTest("dve.state", "DVE parameters: all getters", [](Ctx& c) {
        auto* d = dve(c);
        read<BOOL>(c, "shadow", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetShadow, v); });
        read<double>(c, "lightDirection", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetLightSourceDirection, v); });
        read<double>(c, "lightAltitude", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetLightSourceAltitude, v); });
        read<BOOL>(c, "borderEnabled", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderEnabled, v); });
        read<BMDSwitcherBorderBevelOption>(c, "borderBevel", [&](BMDSwitcherBorderBevelOption* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderBevel, v); });
        read<double>(c, "borderWidthIn", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderWidthIn, v); });
        read<double>(c, "borderWidthOut", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderWidthOut, v); });
        read<double>(c, "borderSoftnessIn", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderSoftnessIn, v); });
        read<double>(c, "borderSoftnessOut", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderSoftnessOut, v); });
        read<double>(c, "borderBevelSoftness", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderBevelSoftness, v); });
        read<double>(c, "borderBevelPosition", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderBevelPosition, v); });
        read<double>(c, "borderOpacity", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderOpacity, v); });
        read<double>(c, "borderHue", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderHue, v); });
        read<double>(c, "borderSaturation", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderSaturation, v); });
        read<double>(c, "borderLuma", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderLuma, v); });
        read<BOOL>(c, "masked", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMasked, v); });
        read<double>(c, "maskTop", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskTop, v); });
        read<double>(c, "maskBottom", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskBottom, v); });
        read<double>(c, "maskLeft", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskLeft, v); });
        read<double>(c, "maskRight", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskRight, v); });
    });

    boolPair("dve.masked", "PiP crop (DVE mask)", &dve,
             [](IBMDSwitcherKeyDVEParameters* d, BOOL v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMasked, v); },
             [](IBMDSwitcherKeyDVEParameters* d, BOOL* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMasked, v); });
    doubleSeries("dve.crop.top", "PiP crop top", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMaskTop, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskTop, v); },
                 { 0, 0.1, 4.5, 9, 18 }, { 38, -1, 100 });
    doubleSeries("dve.crop.bottom", "PiP crop bottom", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMaskBottom, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskBottom, v); },
                 { 0, 0.1, 4.5, 9, 18 }, { 38, -1, 100 });
    doubleSeries("dve.crop.left", "PiP crop left", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMaskLeft, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskLeft, v); },
                 { 0, 0.1, 8, 16, 32 }, { 52, -1, 100 });
    doubleSeries("dve.crop.right", "PiP crop right", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMaskRight, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskRight, v); },
                 { 0, 0.1, 8, 16, 32 }, { 52, -1, 100 });
    addTest("dve.crop.reset", "PiP crop reset (ResetMask)", [](Ctx& c) {
        auto* d = dve(c);
        c.hr("reset", SDK_CALL(IBMDSwitcherKeyDVEParameters, d, ResetMask));
        c.settle();
        read<double>(c, "maskTop", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskTop, v); });
        read<double>(c, "maskLeft", [&](double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskLeft, v); });
        read<BOOL>(c, "masked", [&](BOOL* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMasked, v); });
    });
    boolPair("dve.border", "PiP border", &dve,
             [](IBMDSwitcherKeyDVEParameters* d, BOOL v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetBorderEnabled, v); },
             [](IBMDSwitcherKeyDVEParameters* d, BOOL* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderEnabled, v); });
    doubleSeries("dve.border.width-out", "Border width out", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetBorderWidthOut, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderWidthOut, v); },
                 {}, { 0, 1, 5, 16, -1, 100 });
    doubleSeries("dve.border.opacity", "Border opacity", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetBorderOpacity, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderOpacity, v); },
                 {}, { 0, 50, 100, -1, 200 });
    doubleSeries("dve.border.hue", "Border hue", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetBorderHue, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetBorderHue, v); },
                 {}, { 0, 180, 359.9, -1, 720 });
    boolPair("dve.shadow", "PiP shadow", &dve,
             [](IBMDSwitcherKeyDVEParameters* d, BOOL v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetShadow, v); },
             [](IBMDSwitcherKeyDVEParameters* d, BOOL* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetShadow, v); });
    doubleSeries("dve.light.direction", "Light source direction", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetLightSourceDirection, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetLightSourceDirection, v); },
                 {}, { 0, 90, 359.9, -1, 720 });
    doubleSeries("dve.light.altitude", "Light source altitude", &dve,
                 [](IBMDSwitcherKeyDVEParameters* d, double v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetLightSourceAltitude, v); },
                 [](IBMDSwitcherKeyDVEParameters* d, double* v) { return SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetLightSourceAltitude, v); },
                 {}, { 10, 50, 100, 0, 1000 });

    // ── The PiP panel's exact sequence ──
    addTest("pip.scenario", "PiP panel flow: DVE key, Camera 2 fill, on air, move, size, crop", [](Ctx& c) {
        auto* k = key(c);
        auto* f = fly(c);
        auto* d = dve(c);
        auto* m = c.s.me;
        freeDve(c);
        c.hr("setType", SDK_CALL(IBMDSwitcherKey, k, SetType, bmdSwitcherKeyTypeDVE));
        c.hr("setProgram", SDK_CALL(IBMDSwitcherMixEffectBlock, m, SetProgramInput, 1));
        c.hr("setFill", SDK_CALL(IBMDSwitcherKey, k, SetInputFill, 2));
        c.hr("setPositionX", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetPositionX, 9.6));
        c.hr("setPositionY", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetPositionY, 5.1));
        c.hr("setSizeX", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetSizeX, 0.3));
        c.hr("setSizeY", SDK_CALL(IBMDSwitcherKeyFlyParameters, f, SetSizeY, 0.3));
        c.hr("setMasked", SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMasked, TRUE));
        c.hr("setMaskLeft", SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMaskLeft, 4.0));
        c.hr("setMaskRight", SDK_CALL(IBMDSwitcherKeyDVEParameters, d, SetMaskRight, 4.0));
        c.hr("setOnAir", SDK_CALL(IBMDSwitcherKey, k, SetOnAir, TRUE));
        c.settle(200, 2500);
        double x = 0, y = 0, sx = 0, sy = 0, ml = 0, mr = 0;
        BOOL onAir = FALSE, masked = FALSE;
        BMDSwitcherInputId fill = 0, program = 0;
        SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionX, &x);
        SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetPositionY, &y);
        SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeX, &sx);
        SDK_CALL(IBMDSwitcherKeyFlyParameters, f, GetSizeY, &sy);
        SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskLeft, &ml);
        SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMaskRight, &mr);
        SDK_CALL(IBMDSwitcherKeyDVEParameters, d, GetMasked, &masked);
        SDK_CALL(IBMDSwitcherKey, k, GetOnAir, &onAir);
        SDK_CALL(IBMDSwitcherKey, k, GetInputFill, &fill);
        SDK_CALL(IBMDSwitcherMixEffectBlock, m, GetProgramInput, &program);
        c.observe("readback", QJsonObject{ { "positionX", x }, { "positionY", y }, { "sizeX", sx }, { "sizeY", sy },
                                           { "maskLeft", ml }, { "maskRight", mr }, { "masked", masked != FALSE },
                                           { "onAir", onAir != FALSE }, { "fill", static_cast<double>(fill) },
                                           { "program", static_cast<double>(program) } });
        c.expect(same(x, 9.6) && same(y, 5.1) && same(sx, 0.3) && same(sy, 0.3), "position/size did not read back");
        c.expect(onAir && fill == 2 && program == 1 && masked && same(ml, 4.0), "sources/on-air/crop did not read back");
        c.hr("offAir", SDK_CALL(IBMDSwitcherKey, k, SetOnAir, FALSE));
        c.settle();
    });
}
