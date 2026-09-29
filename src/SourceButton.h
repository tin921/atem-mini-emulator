#pragma once
#include <QPushButton>
#include <QImage>

class SourceButton : public QPushButton
{
public:
    enum Mode { ModePgm, ModeFill };
    explicit SourceButton(const QString& text, Mode mode = ModePgm, QWidget* parent = nullptr);
    void setThumb(const QImage& img);
    void setActive(bool a);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage m_thumb;
    QImage m_scaled;          // m_thumb at the button's size, made when either changes
    bool   m_active = false;
    Mode   m_mode;
};
