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

    // Apply crop
    QImage pipSrc = pip;
    if (dve.cropLeft || dve.cropRight || dve.cropTop || dve.cropBottom) {
        int cw = pip.width(), ch = pip.height();
        int cl = qMin((int)(cw * dve.cropLeft   / 100), cw / 2 - 1);
        int cr = qMin((int)(cw * dve.cropRight  / 100), cw / 2 - 1);
        int ct = qMin((int)(ch * dve.cropTop    / 100), ch / 2 - 1);
        int cb = qMin((int)(ch * dve.cropBottom / 100), ch / 2 - 1);
        pipSrc = pip.copy(cl, ct, qMax(1, cw - cl - cr), qMax(1, ch - ct - cb));
    }

    double scaleX = dve.sizeX / 1000.0;
    double scaleY = dve.sizeY / 1000.0;
    int pipW = qMax(2, (int)(W * scaleX));
    int pipH = qMax(2, (int)(H * scaleY));

    double cx = W / 2.0 + (dve.posX / 16000.0) * W - pipW / 2.0;
    double cy = H / 2.0 - (dve.posY /  9000.0) * H - pipH / 2.0;
    int x = (int)cx, y = (int)cy;

    QImage scaled = pipSrc.scaled(pipW, pipH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    double deg = dve.rotation / 100.0;
    if (deg != 0.0) {
        double pcx = x + pipW / 2.0, pcy = y + pipH / 2.0;
        p.translate(pcx, pcy);
        p.rotate(deg);
        p.translate(-pcx, -pcy);
    }

    p.setOpacity(dve.opacity / 100.0);
    p.drawImage(x, y, scaled);

    if (dve.border > 0) {
        p.setOpacity(1.0);
        int bw = (int)dve.border;
        p.setPen(QPen(QColor::fromRgba(dve.borderArgb), bw));
        p.setBrush(Qt::NoBrush);
        p.drawRect(x - bw/2, y - bw/2, pipW + bw - 1, pipH + bw - 1);
    }
    p.end();
    return out;
}
