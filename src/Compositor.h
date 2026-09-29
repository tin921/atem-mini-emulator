#pragma once
#include "AtemState.h"
#include <QtGui>

// Composites the program source with an optional DVE/PiP overlay.
// Pure QPainter — no OpenGL required.

// Where the PiP lands in a frame (pixels, y down). Shared by the picture and
// the preview's drag handles so both agree to the pixel.
struct PipGeometry {
    QRectF box;                         // the key at its size and position
    QRectF visible;                     // what the mask leaves of it; empty if nothing
    double ml = 0, mr = 0, mt = 0, mb = 0;  // the mask, as fractions of the box
    double angle = 0;                   // degrees, clockwise around visible.center()
};

class Compositor
{
public:
    // Returns a 1280×720 composite image.
    // pgm    = the program source frame
    // pip    = the PiP fill source frame (ignored if !dve.enabled)
    // dve    = DVE/keyer state from ATEMState
    QImage compose(const QImage& pgm,
                   const QImage& pip,
                   const Atem::KeDVState& dve) const;

    static PipGeometry pipGeometry(const Atem::KeDVState& dve, const QSizeF& frame);
};
