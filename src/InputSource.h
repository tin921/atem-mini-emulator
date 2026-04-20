#pragma once
#include <QtCore>
#include <QtGui>
#include <QtMultimedia>

// An input source delivers a QImage frame on demand.
// Subclasses: SolidColorSource (instant) and VideoFileSource (QMediaPlayer).

class InputSource : public QObject
{
    Q_OBJECT
public:
    explicit InputSource(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~InputSource() = default;

    virtual QImage currentFrame() const = 0;
    virtual QString label() const = 0;

signals:
    void frameReady(); // emitted when a new frame is available
};

// ── Solid colour source ────────────────────────────────────────────────────
class SolidColorSource : public InputSource
{
    Q_OBJECT
public:
    explicit SolidColorSource(const QColor& c = Qt::black, QObject* parent = nullptr)
        : InputSource(parent), m_color(c) {}

    QImage currentFrame() const override {
        QImage img(1280, 720, QImage::Format_RGB32);
        img.fill(m_color);
        return img;
    }
    QString label() const override { return m_color.name(); }

    void setColor(const QColor& c) { m_color = c; emit frameReady(); }
    QColor color() const { return m_color; }

private:
    QColor m_color;
};

// ── Static image source ────────────────────────────────────────────────────
class StaticImageSource : public InputSource
{
    Q_OBJECT
public:
    explicit StaticImageSource(QObject* parent = nullptr) : InputSource(parent) {}
    bool loadFile(const QString& path) {
        QImage img(path);
        if (img.isNull()) return false;
        m_path = path;
        m_image = img.scaled(1280, 720, Qt::KeepAspectRatioByExpanding,
                             Qt::SmoothTransformation).copy(0, 0, 1280, 720);
        emit frameReady();
        return true;
    }
    QImage currentFrame() const override {
        if (m_image.isNull()) { QImage b(1280,720,QImage::Format_RGB32); b.fill(Qt::black); return b; }
        return m_image;
    }
    QString label() const override { return m_path.section('/', -1).section('\\', -1); }
    QString path()  const { return m_path; }
private:
    QImage   m_image;
    QString  m_path;
};

// ── Video file source ──────────────────────────────────────────────────────
class VideoFileSource : public InputSource
{
    Q_OBJECT
public:
    explicit VideoFileSource(QObject* parent = nullptr);
    ~VideoFileSource() override;

    bool loadFile(const QString& path);
    QString filePath() const { return m_path; }

    QImage currentFrame() const override;
    QString label() const override;

private slots:
    void onVideoFrame(const QVideoFrame& frame);

private:
    QMediaPlayer*  m_player = nullptr;
    QVideoSink*    m_sink   = nullptr;
    QString        m_path;
    mutable QMutex m_mutex;
    QImage         m_lastFrame;
};
