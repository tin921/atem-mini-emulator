#include "MainWindow.h"
#include "Logger.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSurfaceFormat>

#ifndef ATEM_EMU_PROFILES
#define ATEM_EMU_PROFILES ""
#endif

// profiles/ next to the executable, else the source tree's core/profiles.
static QString defaultProfile()
{
    const QString name = "atem-mini_proto2.30";
    QString beside = QCoreApplication::applicationDirPath() + "/profiles/" + name;
    if (QFileInfo::exists(beside + "/dump.txt")) return beside;
    return QString(ATEM_EMU_PROFILES) + "/" + name;
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ATEM Emulator");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("CEMC");

    QCommandLineParser parser;
    parser.setApplicationDescription("Emulates a Blackmagic ATEM Mini on UDP port 9910.");
    parser.addHelpOption();
    QCommandLineOption profile("profile", "Recorded switcher to emulate (folder with dump.txt).", "dir", defaultProfile());
    QCommandLineOption listen("listen", "Address to listen on (default: all).", "address", "0.0.0.0");
    QCommandLineOption reference("reference", "Start exactly as recorded: no saved macros are loaded or saved "
                                              "(for checking the emulator with atem-sweep).");
    parser.addOptions({ profile, listen, reference });
    parser.process(app);

    Logger::instance().open();

    EmulatorOptions options;
    options.profileDir = QDir::cleanPath(parser.value(profile));
    options.listenAddress = parser.value(listen);
    options.reference = parser.isSet(reference);

    MainWindow w(options);
    if (!w.isReady()) return 1;
    w.show();

    return app.exec();
}
