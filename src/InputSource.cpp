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
    // A file that can't be played shows the placeholder, not its last frame.
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& message) {
        m_error = message;
        {
            QMutexLocker lock(&m_mutex);
            m_lastFrame = QImage();
        }
        emit frameReady();
    });
}

VideoFileSource::~VideoFileSource() = default;

bool VideoFileSource::loadFile(const QString& path)
{
    if (!QFileInfo::exists(path)) return false;
    m_path = path;
    m_error.clear();
    {
        QMutexLocker lock(&m_mutex);
        m_lastFrame = QImage();   // not the previous file's last frame
    }
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
    return true;
}

void VideoFileSource::stop()
{
    m_player->stop();
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
        static const QImage placeholder = [] {
            QImage p(1280, 720, QImage::Format_RGB32);
            p.fill(Qt::darkGray);
            return p;
        }();
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
