#pragma once
#include "AtemState.h"
#include "Compositor.h"
#include <QWidget>
#include <QImage>

// Displays the composited program output at 30 fps.
// Accepts a new frame via setFrame() and repaints.
//
// The PiP on it can be dragged to move it, and resized by its corner handles
// (shown while the mouse is over the picture). The widget only reports the
// new geometry; the window sends it to the switcher like any other change.

class PreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {640, 360}; }
    bool hasHeightForWidth() const override { return true; }
    int  heightForWidth(int w) const override { return w * 9 / 16; }

    // The PiP as drawn in the current frame.
    void setPip(const Atem::KeDVState& pip);
    // Corner drags keep the width : height ratio (the window's size Lock).
    void setLockAspect(bool lock) { m_lockAspect = lock; }

public slots:
    void setFrame(const QImage& img);

signals:
    // In the switcher's units: position (frame edges at +-16 / +-9, +Y up)
    // and size (1.0 = full frame). resized is false for a plain move.
    void pipEdited(double posX, double posY, double sizeX, double sizeY, bool resized);
    void pipEditFinished(bool resized);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    enum class Part { None, Body, TopLeft, TopRight, BottomLeft, BottomRight };

    QSizeF frameSize() const;
    QRectF frameRect() const;                 // where the frame is drawn in the widget
    QPointF toFrame(const QPointF& widgetPos) const;
    QPointF toWidget(const QPointF& framePos) const;
    bool interactive() const;
    QTransform rotation(const PipGeometry& g) const;   // box-local → frame
    QPolygonF corners(const PipGeometry& g) const;     // TL, TR, BR, BL in the frame
    Part partAt(const QPointF& widgetPos) const;
    void updateCursor(Part part);
    void dragTo(const QPointF& widgetPos);
    void emitGeometry(const QPointF& boxCentre, double sizeX, double sizeY, bool resized);

    QImage m_frame;
    Atem::KeDVState m_pip;
    PipGeometry m_geo;
    bool m_lockAspect = true;
    bool m_hover = false;

    Part m_drag = Part::None;
    QPointF m_pressFrame;                     // where the drag started, frame pixels
    Atem::KeDVState m_startPip;
    PipGeometry m_startGeo;
};
