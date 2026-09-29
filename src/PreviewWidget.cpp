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
// The grid: 32 x 18 cells of one switcher unit (40 px at 1280 x 720), so the
// frame edges and the centre lines are grid lines.
constexpr int kColumns = 32, kRows = 18;
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

    // The grid, the axes, the PiP's outline and corner handles: only in this
    // preview (never in the virtual camera's picture).
    const bool dragging = m_drag != Part::None || m_showGuides;
    if (!m_hover && !dragging) return;
    if (dragging) paintGrid(p);
    paintAxes(p);
    if (!interactive()) return;
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
    m_oneAxis = e->modifiers() & Qt::ShiftModifier;
    m_snapBoth = e->modifiers() & Qt::AltModifier;
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
        m_oneAxis = e->modifiers() & Qt::ShiftModifier;
        m_snapBoth = e->modifiers() & Qt::AltModifier;
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

// Esc during a drag puts the PiP back where it was; Shift and Alt take
// effect at once, without waiting for the mouse to move.
void PreviewWidget::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt) && m_drag != Part::None) {
        (e->key() == Qt::Key_Shift ? m_oneAxis : m_snapBoth) = true;
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
    if ((e->key() != Qt::Key_Shift && e->key() != Qt::Key_Alt) || m_drag == Part::None) {
        QWidget::keyReleaseEvent(e);
        return;
    }
    (e->key() == Qt::Key_Shift ? m_oneAxis : m_snapBoth) = false;
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

    // Shift: along one axis only (the one moved more), snapped on it.
    // Alt: snapped on both axes. (Pixels are square: 40 px per unit.)
    QPointF delta = at - m_pressFrame;
    const bool alongX = std::abs(delta.x()) >= std::abs(delta.y());
    if (m_oneAxis) (alongX ? delta.ry() : delta.rx()) = 0;
    const bool snapX = m_snapBoth || (m_oneAxis && alongX);
    const bool snapY = m_snapBoth || (m_oneAxis && !alongX);

    if (m_drag == Part::Body) {
        // The nearest of the PiP's edges and centre goes onto a grid line
        // (for a turned PiP, its bounding box's).
        QRectF r = rotation(g).map(QPolygonF(g.visible)).boundingRect().translated(delta);
        if (snapX) delta.rx() += snapShift({ r.left(), r.center().x(), r.right() }, Qt::Horizontal);
        if (snapY) delta.ry() += snapShift({ r.top(), r.center().y(), r.bottom() }, Qt::Vertical);
        emitGeometry(g.box.center() + delta, sizeX0, sizeY0, false);
        return;
    }

    // A corner: the opposite corner stays put. Worked out in the PiP's own
    // (unrotated) coordinates, then turned back into the frame.
    const QSizeF frame = frameSize();
    const QTransform turn = rotation(g);
    const bool right = m_drag == Part::TopRight || m_drag == Part::BottomRight;
    const bool bottom = m_drag == Part::BottomLeft || m_drag == Part::BottomRight;
    const QPointF anchor(right ? g.visible.left() : g.visible.right(),
                         bottom ? g.visible.top() : g.visible.bottom());
    const QPointF startCorner(right ? g.visible.right() : g.visible.left(),
                              bottom ? g.visible.bottom() : g.visible.top());
    // The corner follows the mouse (keeping where it was grabbed); snapping
    // puts it on a grid line.
    QPointF corner = turn.map(startCorner) + delta;
    if (snapX) corner.rx() += snapShift({ corner.x() }, Qt::Horizontal);
    if (snapY) corner.ry() += snapShift({ corner.y() }, Qt::Vertical);
    const QPointF local = turn.inverted().map(corner);
    const double fw = 1 - g.ml - g.mr, fh = 1 - g.mt - g.mb;   // the mask keeps these shares

    double sizeX = (right ? local.x() - anchor.x() : anchor.x() - local.x()) / fw / frame.width();
    double sizeY = (bottom ? local.y() - anchor.y() : anchor.y() - local.y()) / fh / frame.height();
    if (m_lockAspect) {
        // The axis the corner moved more along sets the size.
        const QPointF moved = local - startCorner;
        double s = std::abs(moved.x()) / frame.width() >= std::abs(moved.y()) / frame.height()
                 ? sizeX / sizeX0 : sizeY / sizeY0;
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
    const QRectF r = frameRect();
    p.setPen(QPen(QColor(255, 255, 255, m_oneAxis || m_snapBoth ? 60 : 30), 1));
    for (double at : gridLines(Qt::Horizontal)) {
        const double x = toWidget(QPointF(at, 0)).x();
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    for (double at : gridLines(Qt::Vertical)) {
        const double y = toWidget(QPointF(0, at)).y();
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.restore();
}

// The switcher's coordinates: X and Y axes through the origin (the frame's
// centre), a tick per unit, and the edges (+-16, +-9) and halfway points
// (+-8, +-4.5) marked with their values. +Y is up.
void PreviewWidget::paintAxes(QPainter& p) const
{
    p.save();
    const QRectF r = frameRect();
    const QPointF o = r.center();
    const double unit = r.width() / 32;     // widget pixels per switcher unit
    const QColor ink(255, 255, 255, 170), shadow(0, 0, 0, 150);

    auto line = [&](QPointF a, QPointF b) {
        p.setPen(QPen(shadow, 3));
        p.drawLine(a, b);
        p.setPen(QPen(ink, 1));
        p.drawLine(a, b);
    };
    line({ r.left(), o.y() }, { r.right(), o.y() });
    line({ o.x(), r.top() }, { o.x(), r.bottom() });
    for (int i = -16; i <= 16; ++i) {
        const double len = i % 8 == 0 ? 5 : 2;
        line({ o.x() + i * unit, o.y() - len }, { o.x() + i * unit, o.y() + len });
    }
    for (int j = -9; j <= 9; ++j) {
        const double len = j == 9 || j == -9 ? 5 : 2;
        line({ o.x() - len, o.y() - j * unit }, { o.x() + len, o.y() - j * unit });
    }
    for (double j : { -4.5, 4.5 })
        line({ o.x() - 5, o.y() - j * unit }, { o.x() + 5, o.y() - j * unit });

    QFont f = font();
    f.setPointSizeF(7);
    p.setFont(f);
    auto label = [&](const QString& text, QPointF at, Qt::Alignment a) {
        QRectF box(at.x() - 40, at.y() - 10, 80, 20);
        if (a & Qt::AlignLeft) box.moveLeft(at.x());
        if (a & Qt::AlignRight) box.moveRight(at.x());
        if (a & Qt::AlignTop) box.moveTop(at.y());
        if (a & Qt::AlignBottom) box.moveBottom(at.y());
        p.setPen(shadow);
        p.drawText(box.translated(1, 1), a, text);
        p.setPen(ink);
        p.drawText(box, a, text);
    };
    const double below = o.y() + 6, beside = o.x() + 7;
    label("0", { o.x() + 4, o.y() + 4 }, Qt::AlignLeft | Qt::AlignTop);
    label("-16", { r.left() + 3, below }, Qt::AlignLeft | Qt::AlignTop);
    label("16", { r.right() - 3, below }, Qt::AlignRight | Qt::AlignTop);
    label("-8", { o.x() - 8 * unit, below }, Qt::AlignHCenter | Qt::AlignTop);
    label("8", { o.x() + 8 * unit, below }, Qt::AlignHCenter | Qt::AlignTop);
    label("9", { beside, r.top() + 2 }, Qt::AlignLeft | Qt::AlignTop);
    label("-9", { beside, r.bottom() - 2 }, Qt::AlignLeft | Qt::AlignBottom);
    label("4.5", { beside, o.y() - 4.5 * unit }, Qt::AlignLeft | Qt::AlignVCenter);
    label("-4.5", { beside, o.y() + 4.5 * unit }, Qt::AlignLeft | Qt::AlignVCenter);
    p.restore();
}
