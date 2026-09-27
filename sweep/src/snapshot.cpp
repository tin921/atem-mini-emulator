// Snapshot of the settings the sweep changes, taken right after connecting
// and put back at the end.
//
// What this covers is exactly the fields of Snapshot. Running the switcher's
// own macros (the macro tests) can change other settings too — audio, for
// example — and those are not restored. Restoring reports every setting that
// could not be read at the start or set at the end, then reads everything
// back and reports whatever differs; any of that fails the run.
#include "runner.h"

#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <functional>

struct Snapshot {
    struct Name {
        IBMDSwitcherInput* input;
        QString longName, shortName;
        BOOL isDefault = TRUE;
        bool read = true;
    };

    BMDSwitcherInputId program = 0, preview = 0;
    BOOL previewTransition = FALSE, fullyBlack = FALSE;
    unsigned int ftbRate = 0;

    BMDSwitcherTransitionStyle nextStyle{};
    BMDSwitcherTransitionSelection nextSelection{};
    IBMDSwitcherTransitionWipeParameters* wipe = nullptr;
    unsigned int wipeRate = 0;
    BMDSwitcherPatternStyle wipePattern{};

    BMDSwitcherKeyType keyType{};
    BMDSwitcherInputId keyFill = 0, keyCut = 0;
    BOOL keyOnAir = FALSE, keyMasked = FALSE;
    double keyMask[4] = {};

    BOOL fly = FALSE, canRotate = FALSE;
    unsigned int flyRate = 0;
    double posX = 0, posY = 0, sizeX = 1, sizeY = 1, rotation = 0;

    BOOL dveMasked = FALSE, border = FALSE, shadow = FALSE;
    double dveMask[4] = {};
    double borderWidthOut = 0, borderOpacity = 0, borderHue = 0, lightDirection = 0, lightAltitude = 0;

    BMDSwitcherInputId dskFill = 0, dskCut = 0;
    BOOL dskOnAir = FALSE, dskTie = FALSE, dskPreMultiplied = FALSE, dskInverse = FALSE, dskMasked = FALSE;
    unsigned int dskRate = 0;
    double dskClip = 0, dskGain = 0, dskMask[4] = {};

    std::vector<Name> names;
    BOOL macroLoop = FALSE;

    IBMDSwitcherMediaPlayer* player = nullptr;
    BMDSwitcherMediaPlayerSourceType playerType{};
    unsigned int playerIndex = 0;
    BOOL playerLoop = FALSE;

    QStringList unread;   // settings that could not be read at the start

    ~Snapshot() {
        if (wipe) wipe->Release();
        if (player) player->Release();
    }
};

namespace {

void rd(Snapshot* s, const char* what, HRESULT hr) {
    if (FAILED(hr)) s->unread << what;
}

template <class Pred>
bool waitUntil(Pred p, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (p()) return true;
        QThread::msleep(20);
    }
    return false;
}

} // namespace

Snapshot* takeSnapshot(Switcher& s) {
    auto* snap = new Snapshot();
    if (auto* m = s.me) {
        rd(snap, "program input", m->GetProgramInput(&snap->program));
        rd(snap, "preview input", m->GetPreviewInput(&snap->preview));
        rd(snap, "preview transition", m->GetPreviewTransition(&snap->previewTransition));
        rd(snap, "fully black", m->GetFadeToBlackFullyBlack(&snap->fullyBlack));
        rd(snap, "fade-to-black rate", m->GetFadeToBlackRate(&snap->ftbRate));
        if (SUCCEEDED(m->QueryInterface(__uuidof(IBMDSwitcherTransitionWipeParameters), reinterpret_cast<void**>(&snap->wipe)))) {
            rd(snap, "wipe rate", snap->wipe->GetRate(&snap->wipeRate));
            rd(snap, "wipe pattern", snap->wipe->GetPattern(&snap->wipePattern));
        }
    }
    if (auto* t = s.trans) {
        rd(snap, "next transition style", t->GetNextTransitionStyle(&snap->nextStyle));
        rd(snap, "next transition selection", t->GetNextTransitionSelection(&snap->nextSelection));
    }
    if (auto* k = s.key) {
        rd(snap, "key type", k->GetType(&snap->keyType));
        rd(snap, "key fill", k->GetInputFill(&snap->keyFill));
        rd(snap, "key cut", k->GetInputCut(&snap->keyCut));
        rd(snap, "key on air", k->GetOnAir(&snap->keyOnAir));
        rd(snap, "key mask", k->GetMasked(&snap->keyMasked));
        rd(snap, "key mask top", k->GetMaskTop(&snap->keyMask[0]));
        rd(snap, "key mask bottom", k->GetMaskBottom(&snap->keyMask[1]));
        rd(snap, "key mask left", k->GetMaskLeft(&snap->keyMask[2]));
        rd(snap, "key mask right", k->GetMaskRight(&snap->keyMask[3]));
    }
    if (auto* f = s.fly) {
        rd(snap, "fly", f->GetFly(&snap->fly));
        rd(snap, "fly rate", f->GetRate(&snap->flyRate));
        rd(snap, "position X", f->GetPositionX(&snap->posX));
        rd(snap, "position Y", f->GetPositionY(&snap->posY));
        rd(snap, "size X", f->GetSizeX(&snap->sizeX));
        rd(snap, "size Y", f->GetSizeY(&snap->sizeY));
        f->GetCanRotate(&snap->canRotate);
        if (snap->canRotate) rd(snap, "rotation", f->GetRotation(&snap->rotation));
    }
    if (auto* d = s.dve) {
        rd(snap, "crop on/off", d->GetMasked(&snap->dveMasked));
        rd(snap, "crop top", d->GetMaskTop(&snap->dveMask[0]));
        rd(snap, "crop bottom", d->GetMaskBottom(&snap->dveMask[1]));
        rd(snap, "crop left", d->GetMaskLeft(&snap->dveMask[2]));
        rd(snap, "crop right", d->GetMaskRight(&snap->dveMask[3]));
        rd(snap, "border", d->GetBorderEnabled(&snap->border));
        rd(snap, "border width", d->GetBorderWidthOut(&snap->borderWidthOut));
        rd(snap, "border opacity", d->GetBorderOpacity(&snap->borderOpacity));
        rd(snap, "border hue", d->GetBorderHue(&snap->borderHue));
        rd(snap, "shadow", d->GetShadow(&snap->shadow));
        rd(snap, "light direction", d->GetLightSourceDirection(&snap->lightDirection));
        rd(snap, "light altitude", d->GetLightSourceAltitude(&snap->lightAltitude));
    }
    if (auto* d = s.dsk) {
        rd(snap, "DSK fill", d->GetInputFill(&snap->dskFill));
        rd(snap, "DSK cut", d->GetInputCut(&snap->dskCut));
        rd(snap, "DSK on air", d->GetOnAir(&snap->dskOnAir));
        rd(snap, "DSK tie", d->GetTie(&snap->dskTie));
        rd(snap, "DSK rate", d->GetRate(&snap->dskRate));
        rd(snap, "DSK pre-multiplied", d->GetPreMultiplied(&snap->dskPreMultiplied));
        rd(snap, "DSK clip", d->GetClip(&snap->dskClip));
        rd(snap, "DSK gain", d->GetGain(&snap->dskGain));
        rd(snap, "DSK inverse", d->GetInverse(&snap->dskInverse));
        rd(snap, "DSK mask", d->GetMasked(&snap->dskMasked));
        rd(snap, "DSK mask top", d->GetMaskTop(&snap->dskMask[0]));
        rd(snap, "DSK mask bottom", d->GetMaskBottom(&snap->dskMask[1]));
        rd(snap, "DSK mask left", d->GetMaskLeft(&snap->dskMask[2]));
        rd(snap, "DSK mask right", d->GetMaskRight(&snap->dskMask[3]));
    }
    for (auto* in : s.inputs) {
        Snapshot::Name n{ in };
        BSTR b = nullptr;
        n.read = SUCCEEDED(in->GetLongName(&b));
        if (n.read) n.longName = takeBstr(b);
        b = nullptr;
        n.read = SUCCEEDED(in->GetShortName(&b)) && n.read;
        if (b) n.shortName = takeBstr(b);
        n.read = SUCCEEDED(in->AreNamesDefault(&n.isDefault)) && n.read;
        snap->names.push_back(n);
    }
    if (s.macros) rd(snap, "macro loop", s.macros->GetLoop(&snap->macroLoop));

    IBMDSwitcherMediaPlayerIterator* it = nullptr;
    if (s.sw && SUCCEEDED(s.sw->CreateIterator(__uuidof(IBMDSwitcherMediaPlayerIterator), reinterpret_cast<void**>(&it))) && it) {
        if (it->Next(&snap->player) == S_OK && snap->player) {
            rd(snap, "media player source", snap->player->GetSource(&snap->playerType, &snap->playerIndex));
            rd(snap, "media player loop", snap->player->GetLoop(&snap->playerLoop));
        }
        it->Release();
    }
    return snap;
}

// Everything in `after` that differs from `before` (settings read at the start only).
static QStringList differences(const Snapshot& before, const Snapshot& after) {
    QStringList out;
    auto skip = [&](const char* what) { return before.unread.contains(what); };
    auto num = [&](const char* what, double a, double b) {
        if (!skip(what) && std::abs(a - b) > 1e-3) out << QString("%1: now %2, was %3").arg(what).arg(b).arg(a);
    };
    auto val = [&](const char* what, long long a, long long b) {
        if (!skip(what) && a != b) out << QString("%1: now %2, was %3").arg(what).arg(b).arg(a);
    };
    val("program input", before.program, after.program);
    val("preview input", before.preview, after.preview);
    val("preview transition", before.previewTransition, after.previewTransition);
    val("fully black", before.fullyBlack, after.fullyBlack);
    val("fade-to-black rate", before.ftbRate, after.ftbRate);
    val("next transition style", before.nextStyle, after.nextStyle);
    val("next transition selection", before.nextSelection, after.nextSelection);
    if (before.wipe) {
        val("wipe rate", before.wipeRate, after.wipeRate);
        val("wipe pattern", before.wipePattern, after.wipePattern);
    }
    val("key type", before.keyType, after.keyType);
    val("key fill", before.keyFill, after.keyFill);
    val("key cut", before.keyCut, after.keyCut);
    val("key on air", before.keyOnAir, after.keyOnAir);
    val("key mask", before.keyMasked, after.keyMasked);
    num("key mask top", before.keyMask[0], after.keyMask[0]);
    num("key mask bottom", before.keyMask[1], after.keyMask[1]);
    num("key mask left", before.keyMask[2], after.keyMask[2]);
    num("key mask right", before.keyMask[3], after.keyMask[3]);
    val("fly", before.fly, after.fly);
    val("fly rate", before.flyRate, after.flyRate);
    num("position X", before.posX, after.posX);
    num("position Y", before.posY, after.posY);
    num("size X", before.sizeX, after.sizeX);
    num("size Y", before.sizeY, after.sizeY);
    if (before.canRotate) num("rotation", before.rotation, after.rotation);
    val("crop on/off", before.dveMasked, after.dveMasked);
    num("crop top", before.dveMask[0], after.dveMask[0]);
    num("crop bottom", before.dveMask[1], after.dveMask[1]);
    num("crop left", before.dveMask[2], after.dveMask[2]);
    num("crop right", before.dveMask[3], after.dveMask[3]);
    val("border", before.border, after.border);
    num("border width", before.borderWidthOut, after.borderWidthOut);
    num("border opacity", before.borderOpacity, after.borderOpacity);
    num("border hue", before.borderHue, after.borderHue);
    val("shadow", before.shadow, after.shadow);
    num("light direction", before.lightDirection, after.lightDirection);
    num("light altitude", before.lightAltitude, after.lightAltitude);
    val("DSK fill", before.dskFill, after.dskFill);
    val("DSK cut", before.dskCut, after.dskCut);
    val("DSK on air", before.dskOnAir, after.dskOnAir);
    val("DSK tie", before.dskTie, after.dskTie);
    val("DSK rate", before.dskRate, after.dskRate);
    val("DSK pre-multiplied", before.dskPreMultiplied, after.dskPreMultiplied);
    num("DSK clip", before.dskClip, after.dskClip);
    num("DSK gain", before.dskGain, after.dskGain);
    val("DSK inverse", before.dskInverse, after.dskInverse);
    val("DSK mask", before.dskMasked, after.dskMasked);
    num("DSK mask top", before.dskMask[0], after.dskMask[0]);
    num("DSK mask bottom", before.dskMask[1], after.dskMask[1]);
    num("DSK mask left", before.dskMask[2], after.dskMask[2]);
    num("DSK mask right", before.dskMask[3], after.dskMask[3]);
    for (size_t i = 0; i < before.names.size() && i < after.names.size(); ++i) {
        const auto& a = before.names[i];
        const auto& b = after.names[i];
        if (!a.read) continue;
        if (a.isDefault != b.isDefault || (!a.isDefault && (a.longName != b.longName || a.shortName != b.shortName)))
            out << QString("input names: now \"%1\"/\"%2\", was \"%3\"/\"%4\"").arg(b.longName, b.shortName, a.longName, a.shortName);
    }
    val("macro loop", before.macroLoop, after.macroLoop);
    if (before.player && after.player) {
        val("media player source", before.playerType, after.playerType);
        val("media player source", before.playerIndex, after.playerIndex);
        val("media player loop", before.playerLoop, after.playerLoop);
    }
    return out;
}

bool restoreSnapshot(Switcher& s, Snapshot* snap, QStringList& log) {
    if (!snap || !s.connected()) {
        log << "not connected: nothing restored";
        delete snap;
        return false;
    }
    bool ok = true;
    // Sets one setting back, unless it could not be read at the start.
    auto put = [&](const char* what, const std::function<HRESULT()>& call) {
        if (snap->unread.contains(what)) {
            log << QString("not restored (could not be read at the start): %1").arg(what);
            ok = false;
            return;
        }
        HRESULT hr = call();
        if (FAILED(hr)) {
            log << QString("could not restore %1: %2").arg(what, hrText(hr));
            ok = false;
        }
    };

    if (s.macros) {
        s.macros->StopRunning();
        put("macro loop", [&] { return s.macros->SetLoop(snap->macroLoop); });
    }
    if (auto* k = s.key) k->SetOnAir(FALSE);   // back on air (if it was) at the end

    if (auto* m = s.me) {
        m->SetTransitionPosition(0.0);
        BOOL black = FALSE;
        m->GetFadeToBlackFullyBlack(&black);
        if (!snap->unread.contains("fully black") && black != snap->fullyBlack) {
            m->PerformFadeToBlack();
            waitUntil([&] { BOOL b = FALSE; m->GetFadeToBlackFullyBlack(&b); BOOL busy = FALSE; m->GetInFadeToBlack(&busy);
                            return b == snap->fullyBlack && !busy; }, 6000);
        }
        put("fade-to-black rate", [&] { return m->SetFadeToBlackRate(snap->ftbRate); });
        put("program input", [&] { return m->SetProgramInput(snap->program); });
        put("preview input", [&] { return m->SetPreviewInput(snap->preview); });
        put("preview transition", [&] { return m->SetPreviewTransition(snap->previewTransition); });
    }
    if (auto* t = s.trans) {
        put("next transition style", [&] { return t->SetNextTransitionStyle(snap->nextStyle); });
        put("next transition selection", [&] { return t->SetNextTransitionSelection(snap->nextSelection); });
        // A DVE transition takes the DVE from the keyer; wait for the switcher
        // to confirm the style change before asking for a DVE key again.
        s.events.settle(200, 2000);
    }
    if (auto* w = snap->wipe) {
        put("wipe rate", [&] { return w->SetRate(snap->wipeRate); });
        put("wipe pattern", [&] { return w->SetPattern(snap->wipePattern); });
    }
    if (auto* k = s.key) {
        put("key type", [&] { return k->SetType(snap->keyType); });
        put("key fill", [&] { return k->SetInputFill(snap->keyFill); });
        put("key cut", [&] { return k->SetInputCut(snap->keyCut); });
        put("key mask", [&] { return k->SetMasked(snap->keyMasked); });
        put("key mask top", [&] { return k->SetMaskTop(snap->keyMask[0]); });
        put("key mask bottom", [&] { return k->SetMaskBottom(snap->keyMask[1]); });
        put("key mask left", [&] { return k->SetMaskLeft(snap->keyMask[2]); });
        put("key mask right", [&] { return k->SetMaskRight(snap->keyMask[3]); });
    }
    if (auto* f = s.fly) {
        put("fly", [&] { return f->SetFly(snap->fly); });
        put("fly rate", [&] { return f->SetRate(snap->flyRate); });
        put("position X", [&] { return f->SetPositionX(snap->posX); });
        put("position Y", [&] { return f->SetPositionY(snap->posY); });
        put("size X", [&] { return f->SetSizeX(snap->sizeX); });
        put("size Y", [&] { return f->SetSizeY(snap->sizeY); });
        if (snap->canRotate) put("rotation", [&] { return f->SetRotation(snap->rotation); });
    }
    if (auto* d = s.dve) {
        put("crop on/off", [&] { return d->SetMasked(snap->dveMasked); });
        put("crop top", [&] { return d->SetMaskTop(snap->dveMask[0]); });
        put("crop bottom", [&] { return d->SetMaskBottom(snap->dveMask[1]); });
        put("crop left", [&] { return d->SetMaskLeft(snap->dveMask[2]); });
        put("crop right", [&] { return d->SetMaskRight(snap->dveMask[3]); });
        put("border", [&] { return d->SetBorderEnabled(snap->border); });
        put("border width", [&] { return d->SetBorderWidthOut(snap->borderWidthOut); });
        put("border opacity", [&] { return d->SetBorderOpacity(snap->borderOpacity); });
        put("border hue", [&] { return d->SetBorderHue(snap->borderHue); });
        put("shadow", [&] { return d->SetShadow(snap->shadow); });
        put("light direction", [&] { return d->SetLightSourceDirection(snap->lightDirection); });
        put("light altitude", [&] { return d->SetLightSourceAltitude(snap->lightAltitude); });
    }
    if (auto* d = s.dsk) {
        put("DSK fill", [&] { return d->SetInputFill(snap->dskFill); });
        put("DSK cut", [&] { return d->SetInputCut(snap->dskCut); });
        put("DSK tie", [&] { return d->SetTie(snap->dskTie); });
        put("DSK rate", [&] { return d->SetRate(snap->dskRate); });
        put("DSK pre-multiplied", [&] { return d->SetPreMultiplied(snap->dskPreMultiplied); });
        put("DSK clip", [&] { return d->SetClip(snap->dskClip); });
        put("DSK gain", [&] { return d->SetGain(snap->dskGain); });
        put("DSK inverse", [&] { return d->SetInverse(snap->dskInverse); });
        put("DSK mask", [&] { return d->SetMasked(snap->dskMasked); });
        put("DSK mask top", [&] { return d->SetMaskTop(snap->dskMask[0]); });
        put("DSK mask bottom", [&] { return d->SetMaskBottom(snap->dskMask[1]); });
        put("DSK mask left", [&] { return d->SetMaskLeft(snap->dskMask[2]); });
        put("DSK mask right", [&] { return d->SetMaskRight(snap->dskMask[3]); });
        put("DSK on air", [&] { return d->SetOnAir(snap->dskOnAir); });
    }
    for (const auto& n : snap->names) {
        if (!n.read) {
            log << "not restored (could not be read at the start): input names";
            ok = false;
        } else if (n.isDefault) {
            put("input names (reset)", [&] { return n.input->ResetNames(); });
        } else {
            BSTR l = makeBstr(n.longName), sh = makeBstr(n.shortName);
            put("input long name", [&] { return n.input->SetLongName(l); });
            put("input short name", [&] { return n.input->SetShortName(sh); });
            SysFreeString(l);
            SysFreeString(sh);
        }
    }
    if (auto* p = snap->player) {
        put("media player source", [&] { return p->SetSource(snap->playerType, snap->playerIndex); });
        put("media player loop", [&] { return p->SetLoop(snap->playerLoop); });
    }
    if (auto* k = s.key) put("key on air", [&] { return k->SetOnAir(snap->keyOnAir); });

    // Read everything back and compare.
    s.events.settle(200, 3000);
    Snapshot* now = takeSnapshot(s);
    QStringList diffs = differences(*snap, *now);
    delete now;
    for (const QString& d : diffs) log << "differs after restore: " + d;
    if (!diffs.isEmpty()) ok = false;
    if (ok) log << QString("restored and read back: every snapshot setting matches (program %1, PiP %2, X %3)")
                       .arg(snap->program).arg(snap->keyOnAir ? "on air" : "off air").arg(snap->posX);
    delete snap;
    return ok;
}
