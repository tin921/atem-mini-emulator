#include "SourceButton.h"
#include <QPainter>
#include <QPen>
#include <QFontMetrics>

SourceButton::SourceButton(const QString& text, Mode mode, QWidget* parent)
    : QPushButton(text, parent), m_mode(mode)
{
    setFlat(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::NoFocus);
}

void SourceButton::setThumb(const QImage& img) { m_thumb = img; update(); }
void SourceButton::setActive(bool a)            { m_active = a;  update(); }

void SourceButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    QRect r = rect();

    // Thumbnail or solid fallback
    if (!m_thumb.isNull())
        p.drawImage(r, m_thumb.scaled(r.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    else
        p.fillRect(r, QColor(20, 20, 20));

    // Dim inactive buttons
    if (!m_active)
        p.fillRect(r, QColor(0, 0, 0, 155));

    // ── Selection bar at top (4 px) ───────────────────────────────────
    p.fillRect(QRect(r.left(), r.top(), r.width(), 6),
               m_active ? QColor(255, 255, 255) : QColor(10, 10, 10));

    // 1px border
    p.setPen(QPen(QColor(45, 45, 45), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0, 0, -1, -1));

    // Button label — centred
    QFont f = font();
    f.setBold(m_active);
    p.setFont(f);
    p.setPen(m_active ? Qt::white : QColor(150, 150, 150));
    p.drawText(r, Qt::AlignCenter, text());
}
