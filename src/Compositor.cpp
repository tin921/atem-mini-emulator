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

    PipGeometry g = pipGeometry(dve, QSizeF(W, H));
    if (g.visible.isEmpty()) return out;
    QRectF target = g.visible;
    QRectF source(pip.width() * g.ml, pip.height() * g.mt,
                  pip.width() * (1 - g.ml - g.mr), pip.height() * (1 - g.mt - g.mb));

    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);

    if (g.angle != 0.0) {
        QPointF c = target.center();
        p.translate(c);
        p.rotate(g.angle);
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

PipGeometry Compositor::pipGeometry(const Atem::KeDVState& dve, const QSizeF& frame)
{
    const double W = frame.width(), H = frame.height();
    PipGeometry g;
    g.angle = dve.rotation / 100.0;

    // The box: size 1000 = full frame; the frame is 32 x 18 units, so the
    // centre moves W/32 per unit (position 16000 puts it on the right edge).
    double boxW = W * dve.sizeX / 1000.0;
    double boxH = H * dve.sizeY / 1000.0;
    g.box = QRectF(W / 2.0 + dve.posX / 32000.0 * W - boxW / 2.0,
                   H / 2.0 - dve.posY / 18000.0 * H - boxH / 2.0, boxW, boxH);
    if (boxW < 1 || boxH < 1) return g;

    // The DVE mask hides edges of the key without rescaling it: left/right
    // are in 32nds of the width, top/bottom in 18ths of the height.
    g.ml = qBound(0.0, dve.cropLeft   / 32000.0, 1.0);
    g.mr = qBound(0.0, dve.cropRight  / 32000.0, 1.0);
    g.mt = qBound(0.0, dve.cropTop    / 18000.0, 1.0);
    g.mb = qBound(0.0, dve.cropBottom / 18000.0, 1.0);
    if (g.ml + g.mr >= 1.0 || g.mt + g.mb >= 1.0) return g;
    g.visible = QRectF(g.box.left() + boxW * g.ml, g.box.top() + boxH * g.mt,
                       boxW * (1 - g.ml - g.mr), boxH * (1 - g.mt - g.mb));
    return g;
}
