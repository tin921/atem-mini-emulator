#include "Logger.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QMutexLocker>

Logger& Logger::instance()
{
    static Logger inst;
    return inst;
}

void Logger::open()
{
    QString path = QCoreApplication::applicationDirPath() + "/atem-emulator.log";
    m_file.setFileName(path);
    m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    m_stream.setDevice(&m_file);
    write("APP", "Logger.cpp", __LINE__,
          QString("=== Session started %1 ===")
          .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
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
    m_stream.flush();
}
