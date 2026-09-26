#pragma once
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>

class Logger {
public:
    static Logger& instance();
    void open();
    void write(const char* category, const char* file, int line, const QString& msg);

private:
    Logger() = default;
    QFile       m_file;
    QTextStream m_stream;
    QMutex      m_mutex;
};

// Strip to filename only (works for both / and \ paths)
#define _LOG_FILE (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 \
                 : strrchr(__FILE__, '/')  ? strrchr(__FILE__, '/')  + 1 \
                 : __FILE__)

// Auto-promote to ERROR if message contains "error" or "fail"
#define _LOG_CAT(base, msg) \
    (QString(msg).contains("error", Qt::CaseInsensitive) || \
     QString(msg).contains("fail",  Qt::CaseInsensitive) ? "ERROR" : (base))

#define LOG_NET(msg)   Logger::instance().write(_LOG_CAT("NET",   msg), _LOG_FILE, __LINE__, (msg))
#define LOG_APP(msg)   Logger::instance().write(_LOG_CAT("APP",   msg), _LOG_FILE, __LINE__, (msg))
#define LOG_ERROR(msg) Logger::instance().write("ERROR",               _LOG_FILE, __LINE__, (msg))
