#pragma once
#include <QtCore>

// Types shared by the window and the compositor. The switcher state itself
// (program, key, DVE, macros, ...) lives in the emulator core (emu::Device),
// which answers the SDK and ATEM Software Control like the real ATEM Mini.

namespace Atem {

// ── ATEM Mini input ids ────────────────────────────────────────────────────
constexpr quint16 SRC_BLACK  = 0;
constexpr quint16 SRC_CAM1   = 1;
constexpr quint16 SRC_CAM2   = 2;
constexpr quint16 SRC_CAM3   = 3;
constexpr quint16 SRC_CAM4   = 4;
constexpr quint16 SRC_BARS   = 1000;
constexpr quint16 SRC_COLOR1 = 2001;
constexpr quint16 SRC_COLOR2 = 2002;

// ── What the compositor draws for the PiP ──────────────────────────────────
// Derived each frame from the switcher's upstream key 1 (a DVE key).
// Position/size/crop use the ATEM's units x1000: the frame is 32 x 18, so
// position +-16000 / +-9000 is the frame edge and size 1000 is full frame.
struct KeDVState {
    bool    enabled    = false;
    quint16 fillSrc    = SRC_CAM2;
    qint32  posX       = 0;
    qint32  posY       = 0;
    quint32 sizeX      = 1000;
    quint32 sizeY      = 1000;
    quint32 border     = 0;             // border width in px at 1280 wide
    quint32 borderArgb = 0xFFFFFFFF;
    quint32 cropLeft   = 0;             // DVE mask: left/right of 32000, top/bottom of 18000
    quint32 cropRight  = 0;
    quint32 cropTop    = 0;
    quint32 cropBottom = 0;
    // Emulator-only picture settings (an ATEM Mini can't rotate or fade a key)
    qint32  rotation   = 0;             // degrees x 100
    quint32 opacity    = 100;           // percent
};

// ── Camera inputs (what cameras 1-4 show in the emulator) ──────────────────
enum class InputMode { SolidColor, Photo, Video };

struct InputSnap {
    InputMode mode = InputMode::SolidColor;
    quint32   argb = 0xFF000000;
    QString   path;
};

} // namespace Atem
