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
// The grid: 16 x 9 cells of 2 x 2 switcher units (80 px at 1280 x 720),
// plus the centre lines. Includes the frame edges.
constexpr int kColumns = 16, kRows = 9;
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

    // The grid, the PiP's outline and corner handles: only in this preview
    // (never in the virtual camera's picture).
    if (!interactive() || (!m_hover && m_drag == Part::None)) return;
    if (m_drag != Part::None) paintGrid(p);
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
    m_lastPos = e->position();
    m_snap = e->modifiers() & Qt::ShiftModifier;
    m_startPip = m_pip;
    m_startGeo = m_geo;
    updateCursor(part);
    update();
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e)
{
    m_hover = true;
    if (m_drag != Part::None) {
        m_lastPos = e->position();
        m_snap = e->modifiers() & Qt::ShiftModifier;
        dragTo(m_lastPos);
        update();
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

// Esc during a drag puts the PiP back where it was; Shift snaps (or stops
// snapping) at once, without waiting for the mouse to move.
void PreviewWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Shift && m_drag != Part::None) {
        m_snap = true;
        dragTo(m_lastPos);
        update();
        return;
    }
    if (e->key() != Qt::Key_Escape || m_drag == Part::None) { QWidget::keyPressEvent(e); return; }
    bool resized = m_drag != Part::Body;
    m_drag = Part::None;
    emitGeometry(m_startGeo.box.center(), m_startPip.sizeX / 1000.0, m_startPip.sizeY / 1000.0, resized);
    emit pipEditFinished(resized);
    update();
}

void PreviewWidget::keyReleaseEvent(QKeyEvent* e)
{
    if (e->key() != Qt::Key_Shift || m_drag == Part::None) { QWidget::keyReleaseEvent(e); return; }
    m_snap = false;
    dragTo(m_lastPos);
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
        QPointF delta = at - m_pressFrame;
        if (m_snap) {
            // The nearest of the PiP's edges and centre goes onto a grid line
            // (for a turned PiP, its bounding box's).
            QRectF r = rotation(g).map(QPolygonF(g.visible)).boundingRect().translated(delta);
            delta += QPointF(snapShift({ r.left(), r.center().x(), r.right() }, Qt::Horizontal),
                             snapShift({ r.top(), r.center().y(), r.bottom() }, Qt::Vertical));
        }
        emitGeometry(g.box.center() + delta, sizeX0, sizeY0, false);
        return;
    }

    // A corner: the opposite corner stays put. Worked out in the PiP's own
    // (unrotated) coordinates, then turned back into the frame.
    const QSizeF frame = frameSize();
    const QTransform turn = rotation(g);
    // Snapping puts the dragged corner on the nearest grid point.
    const QPointF corner = m_snap ? at + QPointF(snapShift({ at.x() }, Qt::Horizontal),
                                                 snapShift({ at.y() }, Qt::Vertical)) : at;
    const QPointF local = turn.inverted().map(corner);
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

// ── Grid ──────────────────────────────────────────────────────────────────────

QList<double> PreviewWidget::gridLines(Qt::Orientation o) const
{
    const double length = o == Qt::Horizontal ? frameSize().width() : frameSize().height();
    const int cells = o == Qt::Horizontal ? kColumns : kRows;
    QList<double> lines;
    for (int i = 0; i <= cells; ++i) lines.append(length * i / cells);
    if (cells % 2) lines.append(length / 2);    // 9 rows: the centre is between lines
    return lines;
}

// How far to shift so that the nearest of these edges lies on a grid line.
double PreviewWidget::snapShift(const QList<double>& edges, Qt::Orientation o) const
{
    double best = 0;
    bool found = false;
    for (double line : gridLines(o))
        for (double edge : edges)
            if (!found || std::abs(line - edge) < std::abs(best)) {
                best = line - edge;
                found = true;
            }
    return best;
}

void PreviewWidget::paintGrid(QPainter& p) const
{
    p.save();
    QRectF r = frameRect();
    const QColor line(255, 255, 255, m_snap ? 70 : 35);
    const QColor centre(255, 255, 255, m_snap ? 130 : 70);
    auto draw = [&](Qt::Orientation o) {
        const double length = o == Qt::Horizontal ? frameSize().width() : frameSize().height();
        for (double at : gridLines(o)) {
            const bool mid = qFuzzyCompare(at, length / 2);
            p.setPen(QPen(mid ? centre : line, 1, mid ? Qt::DashLine : Qt::SolidLine));
            QPointF w = toWidget(o == Qt::Horizontal ? QPointF(at, 0) : QPointF(0, at));
            if (o == Qt::Horizontal) p.drawLine(QPointF(w.x(), r.top()), QPointF(w.x(), r.bottom()));
            else                     p.drawLine(QPointF(r.left(), w.y()), QPointF(r.right(), w.y()));
        }
    };
    draw(Qt::Horizontal);
    draw(Qt::Vertical);
    p.restore();
}
