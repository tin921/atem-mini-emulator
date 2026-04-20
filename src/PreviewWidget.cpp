#include "PreviewWidget.h"
#include <QPainter>
#include <QPaintEvent>

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Maintain 16:9
    setMinimumWidth(320);
    setMinimumHeight(180);
}

void PreviewWidget::setFrame(const QImage& img)
{
    m_frame = img;
    update();
}

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (!m_frame.isNull()) {
        QSize scaled = m_frame.size().scaled(size(), Qt::KeepAspectRatio);
        QRect dst((width() - scaled.width()) / 2,
                  (height() - scaled.height()) / 2,
                  scaled.width(), scaled.height());
        p.drawImage(dst, m_frame);

        // "PROGRAM" label overlay
        p.setPen(Qt::white);
        p.setFont(QFont("Arial", 9, QFont::Bold));
        p.drawText(dst.adjusted(4, 4, 0, 0), Qt::AlignTop | Qt::AlignLeft,
                   "PROGRAM OUTPUT");
    } else {
        p.setPen(Qt::darkGray);
        p.setFont(QFont("Arial", 12));
        p.drawText(rect(), Qt::AlignCenter, "No Source");
    }
}
