#include "Logger.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMutexLocker>
#include <QStandardPaths>

namespace {
constexpr qint64 kMaxLogBytes = 20LL << 20;   // then it starts again (the previous one kept as .1)
constexpr qint64 kFlushMs = 1000;             // written out at least this often
}

Logger& Logger::instance()
{
    static Logger inst;
    return inst;
}

// The log goes to the user's data folder (next to macros.json): the program's
// own folder may not be writable (e.g. under Program Files).
void Logger::open()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    m_path = dir + "/atem-emulator.log";
    rotate();
    write("APP", "Logger.cpp", __LINE__,
          QString("=== Session started %1 ===")
          .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
}

void Logger::rotate()
{
    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.close();
    }
    QFile::remove(m_path + ".1");
    QFile::rename(m_path, m_path + ".1");
    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        qWarning("atem-emulator: cannot write the log %s: %s", qPrintable(QDir::toNativeSeparators(m_path)),
                 qPrintable(m_file.errorString()));
    m_stream.setDevice(&m_file);
    m_sinceFlush.start();
}

void Logger::write(const char* category, const char* file, int line, const QString& msg)
{
    QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen()) return;
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    m_stream << QString("[%1] [%2] %3:%4  %5\n")
                .arg(ts, -27)
                .arg(QString(category), -5)
                .arg(file)
                .arg(line)
                .arg(msg);
    // Not a disk flush per line (the network log has a line per packet):
    // errors at once, the rest at least once a second.
    if (qstrcmp(category, "ERROR") == 0 || m_sinceFlush.elapsed() >= kFlushMs) {
        m_stream.flush();
        m_sinceFlush.restart();
    }
    if (m_file.size() + m_stream.device()->bytesToWrite() > kMaxLogBytes) rotate();
}
