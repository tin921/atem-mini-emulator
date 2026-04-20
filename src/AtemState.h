#pragma once
#include "AtemProtocol.h"
#include <QtCore>

namespace Atem {

// ── DVE / Keyer state ──────────────────────────────────────────────────────
struct KeDVState {
    bool    enabled = false;
    quint16 fillSrc = SRC_CAM2;
    qint32  posX    = 0;       // units: 1/1000 of frame width  (range ~-16000..16000)
    qint32  posY    = 0;       // units: 1/1000 of frame height (range ~-9000..9000)
    quint32 sizeX   = 1000;    // 0–2000, 1000 = 100%
    quint32 sizeY   = 1000;
    quint32 border      = 0;      // border width 0–50 px at 1280 reference
    quint32 opacity     = 100;    // 0–100 percent
    qint32  rotation    = 0;      // degrees × 100 (0–35999)
    quint32 borderArgb  = 0xFFFFFFFF; // border colour ARGB, default white
    quint32 cropLeft    = 0;      // 0–50 percent per edge
    quint32 cropRight   = 0;
    quint32 cropTop     = 0;
    quint32 cropBottom  = 0;
};

// ── Macro ─────────────────────────────────────────────────────────────────
enum class MacroActionType { SwitchProgram, SwitchPreview, KeyerEnable, Delay };

struct MacroAction {
    MacroActionType type   = MacroActionType::Delay;
    int             param  = 500;  // source ID, 0/1, or ms
};

enum class InputMode { SolidColor, Photo, Video };

struct InputSnap {
    InputMode mode = InputMode::SolidColor;
    quint32   argb = 0xFF000000;
    QString   path;
};

struct MacroSnapshot {
    bool      captured      = false;
    quint16   programSource = SRC_BARS;
    KeDVState dve;
    bool      lockSize      = true;   // UI checkbox — not in KeDVState
    InputSnap inputs[4];
};

struct MacroDef {
    QString              name;
    QString              description;
    bool                 isUsed   = false;
    QVector<MacroAction> actions;
    MacroSnapshot        snapshot;
};

struct MacroRunStatus {
    bool    running = false;
    bool    waiting = false;
    quint16 index   = 0xFFFF;
};

// ── Full device state ──────────────────────────────────────────────────────
struct ATEMState {
    // Dynamic — change at runtime, must broadcast updates
    quint16  programSource = SRC_BARS;
    quint16  previewSource = SRC_CAM1;
    bool     keyerOn       = false;
    KeDVState dve;
    QVector<MacroDef> macros = QVector<MacroDef>(20);
    MacroRunStatus    macroRun;

    // ── Field serialisers (each returns a complete built field) ──
    QByteArray fieldPrgI()       const;
    QByteArray fieldPrvI()       const;
    QByteArray fieldKeOn()       const;
    QByteArray fieldKeDV()       const;
    QByteArray fieldMPrp(int i)  const;
    QByteArray fieldMRPr()       const;
    QByteArray fieldTlIn()       const; // tally by index
    QByteArray fieldTlSr()       const; // tally by source

    // Build entire state dump as a list of packet payloads, packed ≤900 bytes each.
    // Each payload is preceded by a header by the caller.
    QVector<QByteArray> buildStateDump() const;
};

} // namespace Atem
