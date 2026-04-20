#pragma once
#include "AtemState.h"
#include "InputSource.h"
#include <QtGui>

// Composites the program source with an optional DVE/PiP overlay.
// Pure QPainter — no OpenGL required.

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
};
