#include "PreviewWidget.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <cmath>

namespace {
constexpr double kHandle = 8;       // corner handle, widget pixels
constexpr double kGrab = 7;         // how close to a corner grabs it
constexpr double kMinSize = 0.05;   // the window's size range, 5-200%
constexpr double kMaxSize = 2.0;
constexpr double kMaxPos = 200.0;   // the position boxes' range
} // namespace

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);   // Esc cancels a drag
}

void PreviewWidget::setFrame(const QImage& img)
{
    m_frame = img;
    update();
}

void PreviewWidget::setPip(const Atem::KeDVState& pip)
{
    m_pip = pip;
    m_geo = Compositor::pipGeometry(pip, frameSize());
    if (m_drag == Part::None && m_hover)
        updateCursor(partAt(mapFromGlobal(QCursor::pos())));
}

// ── Coordinates ───────────────────────────────────────────────────────────────

QSizeF PreviewWidget::frameSize() const
{
    return m_frame.isNull() ? QSizeF(1280, 720) : QSizeF(m_frame.size());
}

QRectF PreviewWidget::frameRect() const
{
    QSizeF scaled = frameSize().scaled(QSizeF(size()), Qt::KeepAspectRatio);
    return QRectF((width() - scaled.width()) / 2, (height() - scaled.height()) / 2,
                  scaled.width(), scaled.height());
}

QPointF PreviewWidget::toFrame(const QPointF& widgetPos) const
{
    QRectF r = frameRect();
    return (widgetPos - r.topLeft()) * (frameSize().width() / r.width());
}

QPointF PreviewWidget::toWidget(const QPointF& framePos) const
{
    QRectF r = frameRect();
    return r.topLeft() + framePos * (r.width() / frameSize().width());
}

bool PreviewWidget::interactive() const
{
    return !m_frame.isNull() && m_pip.enabled && !m_geo.visible.isEmpty();
}

// The compositor turns the visible rectangle around its centre.
QTransform PreviewWidget::rotation(const PipGeometry& g) const
{
    QPointF c = g.visible.center();
    QTransform t;
    t.translate(c.x(), c.y());
    t.rotate(g.angle);
    t.translate(-c.x(), -c.y());
    return t;
}

QPolygonF PreviewWidget::corners(const PipGeometry& g) const
{
    const QRectF& v = g.visible;
    return rotation(g).map(QPolygonF({ v.topLeft(), v.topRight(), v.bottomRight(), v.bottomLeft() }));
}

PreviewWidget::Part PreviewWidget::partAt(const QPointF& widgetPos) const
{
    if (!interactive()) return Part::None;
    QPolygonF c = corners(m_geo);
    const Part parts[4] = { Part::TopLeft, Part::TopRight, Part::BottomRight, Part::BottomLeft };
    for (int i = 0; i < 4; ++i) {
        QPointF d = toWidget(c[i]) - widgetPos;
        if (std::abs(d.x()) <= kGrab && std::abs(d.y()) <= kGrab) return parts[i];
    }
    QPointF local = rotation(m_geo).inverted().map(toFrame(widgetPos));
    return m_geo.visible.contains(local) ? Part::Body : Part::None;
}

void PreviewWidget::updateCursor(Part part)
{
    switch (part) {
    case Part::Body:        setCursor(m_drag == Part::Body ? Qt::ClosedHandCursor : Qt::OpenHandCursor); break;
    case Part::TopLeft:
    case Part::BottomRight: setCursor(Qt::SizeFDiagCursor); break;
    case Part::TopRight:
    case Part::BottomLeft:  setCursor(Qt::SizeBDiagCursor); break;
    case Part::None:        unsetCursor(); break;
    }
}

// ── Painting ──────────────────────────────────────────────────────────────────

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (m_frame.isNull()) {
        p.setPen(Qt::darkGray);
        p.setFont(QFont("Arial", 12));
        p.drawText(rect(), Qt::AlignCenter, "No Source");
        return;
    }

    QRect dst = frameRect().toRect();
    p.drawImage(dst, m_frame);

    // "PROGRAM" label overlay
    p.setPen(Qt::white);
    p.setFont(QFont("Arial", 9, QFont::Bold));
    p.drawText(dst.adjusted(4, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft, "PROGRAM OUTPUT");

    // The PiP's outline and corner handles, only in this preview (never in
    // the virtual camera's picture).
    if (!interactive() || (!m_hover && m_drag == Part::None)) return;
    QPolygonF c = corners(m_geo);
    for (QPointF& pt : c) pt = toWidget(pt);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 160), 3));
    p.drawPolygon(c);
    p.setPen(QPen(QColor("#3377ff"), 1.5));
    p.drawPolygon(c);
    p.setPen(QPen(QColor("#1a5acc"), 1));
    p.setBrush(Qt::white);
    for (const QPointF& pt : c)
        p.drawRect(QRectF(pt.x() - kHandle / 2, pt.y() - kHandle / 2, kHandle, kHandle));
}

// ── Dragging ──────────────────────────────────────────────────────────────────

void PreviewWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    Part part = partAt(e->position());
    if (part == Part::None) return;
    m_drag = part;
    m_pressFrame = toFrame(e->position());
    m_startPip = m_pip;
    m_startGeo = m_geo;
    updateCursor(part);
    update();
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e)
{
    m_hover = true;
    if (m_drag != Part::None) {
        dragTo(e->position());
        return;
    }
    updateCursor(partAt(e->position()));
    update();
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || m_drag == Part::None) return;
    bool resized = m_drag != Part::Body;
    m_drag = Part::None;
    updateCursor(partAt(e->position()));
    update();
    emit pipEditFinished(resized);
}

// Esc during a drag puts the PiP back where it was.
void PreviewWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->key() != Qt::Key_Escape || m_drag == Part::None) { QWidget::keyPressEvent(e); return; }
    bool resized = m_drag != Part::Body;
    m_drag = Part::None;
    emitGeometry(m_startGeo.box.center(), m_startPip.sizeX / 1000.0, m_startPip.sizeY / 1000.0, resized);
    emit pipEditFinished(resized);
    update();
}

void PreviewWidget::leaveEvent(QEvent*)
{
    m_hover = false;
    update();
}

void PreviewWidget::dragTo(const QPointF& widgetPos)
{
    const QPointF at = toFrame(widgetPos);
    const PipGeometry& g = m_startGeo;
    const double sizeX0 = m_startPip.sizeX / 1000.0, sizeY0 = m_startPip.sizeY / 1000.0;

    if (m_drag == Part::Body) {
        emitGeometry(g.box.center() + (at - m_pressFrame), sizeX0, sizeY0, false);
        return;
    }

    // A corner: the opposite corner stays put. Worked out in the PiP's own
    // (unrotated) coordinates, then turned back into the frame.
    const QSizeF frame = frameSize();
    const QTransform turn = rotation(g);
    const QPointF local = turn.inverted().map(at);
    const bool right = m_drag == Part::TopRight || m_drag == Part::BottomRight;
    const bool bottom = m_drag == Part::BottomLeft || m_drag == Part::BottomRight;
    const QPointF anchor(right ? g.visible.left() : g.visible.right(),
                         bottom ? g.visible.top() : g.visible.bottom());
    const double fw = 1 - g.ml - g.mr, fh = 1 - g.mt - g.mb;   // the mask keeps these shares

    double sizeX = (right ? local.x() - anchor.x() : anchor.x() - local.x()) / fw / frame.width();
    double sizeY = (bottom ? local.y() - anchor.y() : anchor.y() - local.y()) / fh / frame.height();
    if (m_lockAspect) {
        double s = qMax(sizeX / sizeX0, sizeY / sizeY0);
        s = qBound(kMinSize / qMin(sizeX0, sizeY0), s, kMaxSize / qMax(sizeX0, sizeY0));
        sizeX = sizeX0 * s;
        sizeY = sizeY0 * s;
    } else {
        sizeX = qBound(kMinSize, sizeX, kMaxSize);
        sizeY = qBound(kMinSize, sizeY, kMaxSize);
    }

    const double vw = sizeX * frame.width() * fw, vh = sizeY * frame.height() * fh;
    const QRectF visible(right ? anchor.x() : anchor.x() - vw,
                         bottom ? anchor.y() : anchor.y() - vh, vw, vh);
    // The visible centre sits off the box centre by the mask's imbalance.
    const double bw = sizeX * frame.width(), bh = sizeY * frame.height();
    const QPointF centre = turn.map(visible.center())
                         - QPointF(bw * (g.ml - g.mr) / 2, bh * (g.mt - g.mb) / 2);
    emitGeometry(centre, sizeX, sizeY, true);
}

void PreviewWidget::emitGeometry(const QPointF& boxCentre, double sizeX, double sizeY, bool resized)
{
    const QSizeF frame = frameSize();
    double posX = (boxCentre.x() / frame.width() - 0.5) * 32;
    double posY = (0.5 - boxCentre.y() / frame.height()) * 18;
    posX = qBound(-kMaxPos, posX, kMaxPos);
    posY = qBound(-kMaxPos, posY, kMaxPos);
    emit pipEdited(posX, posY, sizeX, sizeY, resized);
}
