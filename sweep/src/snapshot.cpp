// Snapshot of everything the sweep may change, taken right after connecting
// and put back at the end, so the switcher is left as it was found.
#include "runner.h"

#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>

struct Snapshot {
    struct Name {
        IBMDSwitcherInput* input;
        QString longName, shortName;
        BOOL isDefault = TRUE;
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

    BOOL fly = FALSE;
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
};

Snapshot* takeSnapshot(Switcher& s) {
    auto* snap = new Snapshot();
    if (auto* m = s.me) {
        m->GetProgramInput(&snap->program);
        m->GetPreviewInput(&snap->preview);
        m->GetPreviewTransition(&snap->previewTransition);
        m->GetFadeToBlackFullyBlack(&snap->fullyBlack);
        m->GetFadeToBlackRate(&snap->ftbRate);
        if (SUCCEEDED(m->QueryInterface(__uuidof(IBMDSwitcherTransitionWipeParameters), reinterpret_cast<void**>(&snap->wipe)))) {
            snap->wipe->GetRate(&snap->wipeRate);
            snap->wipe->GetPattern(&snap->wipePattern);
        }
    }
    if (auto* t = s.trans) {
        t->GetNextTransitionStyle(&snap->nextStyle);
        t->GetNextTransitionSelection(&snap->nextSelection);
    }
    if (auto* k = s.key) {
        k->GetType(&snap->keyType);
        k->GetInputFill(&snap->keyFill);
        k->GetInputCut(&snap->keyCut);
        k->GetOnAir(&snap->keyOnAir);
        k->GetMasked(&snap->keyMasked);
        k->GetMaskTop(&snap->keyMask[0]);
        k->GetMaskBottom(&snap->keyMask[1]);
        k->GetMaskLeft(&snap->keyMask[2]);
        k->GetMaskRight(&snap->keyMask[3]);
    }
    if (auto* f = s.fly) {
        f->GetFly(&snap->fly);
        f->GetRate(&snap->flyRate);
        f->GetPositionX(&snap->posX);
        f->GetPositionY(&snap->posY);
        f->GetSizeX(&snap->sizeX);
        f->GetSizeY(&snap->sizeY);
        f->GetRotation(&snap->rotation);
    }
    if (auto* d = s.dve) {
        d->GetMasked(&snap->dveMasked);
        d->GetMaskTop(&snap->dveMask[0]);
        d->GetMaskBottom(&snap->dveMask[1]);
        d->GetMaskLeft(&snap->dveMask[2]);
        d->GetMaskRight(&snap->dveMask[3]);
        d->GetBorderEnabled(&snap->border);
        d->GetBorderWidthOut(&snap->borderWidthOut);
        d->GetBorderOpacity(&snap->borderOpacity);
        d->GetBorderHue(&snap->borderHue);
        d->GetShadow(&snap->shadow);
        d->GetLightSourceDirection(&snap->lightDirection);
        d->GetLightSourceAltitude(&snap->lightAltitude);
    }
    if (auto* d = s.dsk) {
        d->GetInputFill(&snap->dskFill);
        d->GetInputCut(&snap->dskCut);
        d->GetOnAir(&snap->dskOnAir);
        d->GetTie(&snap->dskTie);
        d->GetRate(&snap->dskRate);
        d->GetPreMultiplied(&snap->dskPreMultiplied);
        d->GetClip(&snap->dskClip);
        d->GetGain(&snap->dskGain);
        d->GetInverse(&snap->dskInverse);
        d->GetMasked(&snap->dskMasked);
        d->GetMaskTop(&snap->dskMask[0]);
        d->GetMaskBottom(&snap->dskMask[1]);
        d->GetMaskLeft(&snap->dskMask[2]);
        d->GetMaskRight(&snap->dskMask[3]);
    }
    for (auto* in : s.inputs) {
        Snapshot::Name n{ in };
        BSTR b = nullptr;
        if (SUCCEEDED(in->GetLongName(&b))) n.longName = takeBstr(b);
        b = nullptr;
        if (SUCCEEDED(in->GetShortName(&b))) n.shortName = takeBstr(b);
        in->AreNamesDefault(&n.isDefault);
        snap->names.push_back(n);
    }
    if (s.macros) s.macros->GetLoop(&snap->macroLoop);

    IBMDSwitcherMediaPlayerIterator* it = nullptr;
    if (s.sw && SUCCEEDED(s.sw->CreateIterator(__uuidof(IBMDSwitcherMediaPlayerIterator), reinterpret_cast<void**>(&it))) && it) {
        if (it->Next(&snap->player) == S_OK && snap->player) {
            snap->player->GetSource(&snap->playerType, &snap->playerIndex);
            snap->player->GetLoop(&snap->playerLoop);
        }
        it->Release();
    }
    return snap;
}

namespace {

void check(QStringList& log, const char* what, HRESULT hr) {
    if (FAILED(hr)) log << QString("could not restore %1: %2").arg(what, hrText(hr));
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

void restoreSnapshot(Switcher& s, Snapshot* snap, QStringList& log) {
    if (!snap || !s.connected()) {
        log << "not connected: nothing restored";
        delete snap;
        return;
    }

    if (s.macros) {
        s.macros->StopRunning();
        check(log, "macro loop", s.macros->SetLoop(snap->macroLoop));
    }
    if (auto* k = s.key) k->SetOnAir(FALSE);   // back on air (if it was) at the end

    if (auto* m = s.me) {
        m->SetTransitionPosition(0.0);
        BOOL black = FALSE;
        m->GetFadeToBlackFullyBlack(&black);
        if (black != snap->fullyBlack) {
            m->PerformFadeToBlack();
            waitUntil([&] { BOOL b = FALSE; m->GetFadeToBlackFullyBlack(&b); BOOL busy = FALSE; m->GetInFadeToBlack(&busy);
                            return b == snap->fullyBlack && !busy; }, 6000);
        }
        check(log, "fade-to-black rate", m->SetFadeToBlackRate(snap->ftbRate));
        check(log, "program input", m->SetProgramInput(snap->program));
        check(log, "preview input", m->SetPreviewInput(snap->preview));
        check(log, "preview transition", m->SetPreviewTransition(snap->previewTransition));
    }
    if (auto* t = s.trans) {
        check(log, "next transition style", t->SetNextTransitionStyle(snap->nextStyle));
        check(log, "next transition selection", t->SetNextTransitionSelection(snap->nextSelection));
        // A DVE transition takes the DVE from the keyer; wait for the switcher
        // to confirm the style change before asking for a DVE key again.
        s.events.settle(200, 2000);
    }
    if (snap->wipe) {
        check(log, "wipe rate", snap->wipe->SetRate(snap->wipeRate));
        check(log, "wipe pattern", snap->wipe->SetPattern(snap->wipePattern));
        snap->wipe->Release();
    }
    if (auto* k = s.key) {
        check(log, "key type", k->SetType(snap->keyType));
        check(log, "key fill", k->SetInputFill(snap->keyFill));
        k->SetInputCut(snap->keyCut);
        check(log, "key mask", k->SetMasked(snap->keyMasked));
        k->SetMaskTop(snap->keyMask[0]);
        k->SetMaskBottom(snap->keyMask[1]);
        k->SetMaskLeft(snap->keyMask[2]);
        k->SetMaskRight(snap->keyMask[3]);
    }
    if (auto* f = s.fly) {
        f->SetFly(snap->fly);
        check(log, "fly rate", f->SetRate(snap->flyRate));
        check(log, "position X", f->SetPositionX(snap->posX));
        check(log, "position Y", f->SetPositionY(snap->posY));
        check(log, "size X", f->SetSizeX(snap->sizeX));
        check(log, "size Y", f->SetSizeY(snap->sizeY));
        f->SetRotation(snap->rotation);
    }
    if (auto* d = s.dve) {
        check(log, "crop on/off", d->SetMasked(snap->dveMasked));
        check(log, "crop top", d->SetMaskTop(snap->dveMask[0]));
        check(log, "crop bottom", d->SetMaskBottom(snap->dveMask[1]));
        check(log, "crop left", d->SetMaskLeft(snap->dveMask[2]));
        check(log, "crop right", d->SetMaskRight(snap->dveMask[3]));
        check(log, "border", d->SetBorderEnabled(snap->border));
        d->SetBorderWidthOut(snap->borderWidthOut);
        d->SetBorderOpacity(snap->borderOpacity);
        d->SetBorderHue(snap->borderHue);
        check(log, "shadow", d->SetShadow(snap->shadow));
        d->SetLightSourceDirection(snap->lightDirection);
        d->SetLightSourceAltitude(snap->lightAltitude);
    }
    if (auto* d = s.dsk) {
        check(log, "DSK fill", d->SetInputFill(snap->dskFill));
        d->SetInputCut(snap->dskCut);
        check(log, "DSK tie", d->SetTie(snap->dskTie));
        check(log, "DSK rate", d->SetRate(snap->dskRate));
        d->SetPreMultiplied(snap->dskPreMultiplied);
        d->SetClip(snap->dskClip);
        d->SetGain(snap->dskGain);
        d->SetInverse(snap->dskInverse);
        d->SetMasked(snap->dskMasked);
        d->SetMaskTop(snap->dskMask[0]);
        d->SetMaskBottom(snap->dskMask[1]);
        d->SetMaskLeft(snap->dskMask[2]);
        d->SetMaskRight(snap->dskMask[3]);
        check(log, "DSK on air", d->SetOnAir(snap->dskOnAir));
    }
    for (const auto& n : snap->names) {
        if (n.isDefault) {
            check(log, "input names (reset)", n.input->ResetNames());
        } else {
            BSTR l = makeBstr(n.longName), sh = makeBstr(n.shortName);
            check(log, "input long name", n.input->SetLongName(l));
            check(log, "input short name", n.input->SetShortName(sh));
            SysFreeString(l);
            SysFreeString(sh);
        }
    }
    if (snap->player) {
        check(log, "media player source", snap->player->SetSource(snap->playerType, snap->playerIndex));
        snap->player->SetLoop(snap->playerLoop);
        snap->player->Release();
    }
    if (auto* k = s.key) check(log, "key on air", k->SetOnAir(snap->keyOnAir));

    s.events.settle(200, 3000);
    // Spot-check the values the PiP panel cares about.
    if (s.me && s.key && s.fly) {
        BMDSwitcherInputId p = 0;
        BOOL onAir = FALSE;
        double x = 0;
        s.me->GetProgramInput(&p);
        s.key->GetOnAir(&onAir);
        s.fly->GetPositionX(&x);
        bool ok = p == snap->program && onAir == snap->keyOnAir && std::abs(x - snap->posX) < 1e-3;
        log << (ok ? QString("restored: program %1, PiP %2, X %3").arg(p).arg(onAir ? "on air" : "off air").arg(x)
                   : QString("restore check FAILED: program %1 (was %2), on air %3 (was %4), X %5 (was %6)")
                         .arg(p).arg(snap->program).arg(onAir).arg(snap->keyOnAir).arg(x).arg(snap->posX));
    }
    delete snap;
}
