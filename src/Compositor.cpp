#include "Compositor.h"
#include <QPainter>

QImage Compositor::compose(const QImage& pgm,
                           const QImage& pip,
                           const Atem::KeDVState& dve) const
{
    constexpr int W = 1280, H = 720;

    QImage out = pgm.isNull()
        ? QImage(W, H, QImage::Format_RGB32)
        : pgm.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
             .convertToFormat(QImage::Format_RGB32);
    if (pgm.isNull()) out.fill(Qt::black);
    if (!dve.enabled || pip.isNull()) return out;

    // The box: size 1000 = full frame; the frame is 32 x 18 units, so the
    // centre moves W/32 per unit (position 16000 puts it on the right edge).
    double boxW = W * dve.sizeX / 1000.0;
    double boxH = H * dve.sizeY / 1000.0;
    if (boxW < 1 || boxH < 1) return out;
    double left = W / 2.0 + dve.posX / 32000.0 * W - boxW / 2.0;
    double top  = H / 2.0 - dve.posY / 18000.0 * H - boxH / 2.0;

    // The DVE mask hides edges of the key without rescaling it: left/right
    // are in 32nds of the width, top/bottom in 18ths of the height.
    double ml = qBound(0.0, dve.cropLeft   / 32000.0, 1.0);
    double mr = qBound(0.0, dve.cropRight  / 32000.0, 1.0);
    double mt = qBound(0.0, dve.cropTop    / 18000.0, 1.0);
    double mb = qBound(0.0, dve.cropBottom / 18000.0, 1.0);
    if (ml + mr >= 1.0 || mt + mb >= 1.0) return out;
    QRectF target(left + boxW * ml, top + boxH * mt, boxW * (1 - ml - mr), boxH * (1 - mt - mb));
    QRectF source(pip.width() * ml, pip.height() * mt,
                  pip.width() * (1 - ml - mr), pip.height() * (1 - mt - mb));

    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);

    double deg = dve.rotation / 100.0;
    if (deg != 0.0) {
        QPointF c = target.center();
        p.translate(c);
        p.rotate(deg);
        p.translate(-c);
    }

    p.setOpacity(dve.opacity / 100.0);
    p.drawImage(target, pip, source);

    if (dve.border > 0) {
        p.setOpacity(1.0);
        double bw = dve.border;
        p.setPen(QPen(QColor::fromRgba(dve.borderArgb), bw, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRect(target.adjusted(-bw / 2, -bw / 2, bw / 2, bw / 2));
    }
    p.end();
    return out;
}
