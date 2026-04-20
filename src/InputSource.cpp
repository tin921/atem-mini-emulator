#include "InputSource.h"
#include <QtMultimedia>

// ── VideoFileSource ────────────────────────────────────────────────────────

VideoFileSource::VideoFileSource(QObject* parent)
    : InputSource(parent)
{
    m_sink   = new QVideoSink(this);
    m_player = new QMediaPlayer(this);
    m_player->setVideoSink(m_sink);
    m_player->setLoops(QMediaPlayer::Infinite);

    connect(m_sink, &QVideoSink::videoFrameChanged,
            this,   &VideoFileSource::onVideoFrame);
}

VideoFileSource::~VideoFileSource() = default;

bool VideoFileSource::loadFile(const QString& path)
{
    m_path = path;
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
    return true;
}

QString VideoFileSource::label() const
{
    if (m_path.isEmpty()) return "(no file)";
    return QFileInfo(m_path).fileName();
}

QImage VideoFileSource::currentFrame() const
{
    QMutexLocker lock(&m_mutex);
    if (m_lastFrame.isNull()) {
        QImage placeholder(1280, 720, QImage::Format_RGB32);
        placeholder.fill(Qt::darkGray);
        return placeholder;
    }
    return m_lastFrame;
}

void VideoFileSource::onVideoFrame(const QVideoFrame& frame)
{
    QImage img = frame.toImage().convertToFormat(QImage::Format_RGB32);
    if (!img.isNull()) {
        QMutexLocker lock(&m_mutex);
        m_lastFrame = img;
    }
    emit frameReady();
}
