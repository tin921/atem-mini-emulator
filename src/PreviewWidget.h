#pragma once
#include <QWidget>
#include <QImage>

// Displays the composited program output at 30 fps.
// Accepts a new frame via setFrame() and repaints.

class PreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {640, 360}; }
    bool hasHeightForWidth() const override { return true; }
    int  heightForWidth(int w) const override { return w * 9 / 16; }

public slots:
    void setFrame(const QImage& img);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage m_frame;
};
