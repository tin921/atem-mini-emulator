// atem-emu — headless ATEM switcher emulator.
//
//   atem-emu [--profile DIR] [--listen ADDRESS] [--port 9910] [--verbose]
//
// Answers the BMD switcher SDK (and anything else speaking the ATEM
// protocol) like the switcher recorded in the profile. Point ATEM Software
// Control, atem-sweep or the obs-atem plugin at this PC's address.

#include "device.h"
#include "server.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#ifndef ATEM_EMU_PROFILES
#define ATEM_EMU_PROFILES ""
#endif

namespace {

void print(const QString& message) {
    static QTextStream out(stdout);
    out << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << "  " << message << Qt::endl;
}

// profiles/ next to the executable (installed), else the source tree.
QString defaultProfile() {
    const QString name = "atem-mini_proto2.30";
    QString beside = QCoreApplication::applicationDirPath() + "/profiles/" + name;
    if (QFileInfo::exists(beside + "/dump.txt")) return beside;
    return QString(ATEM_EMU_PROFILES) + "/" + name;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("atem-emu");

    QCommandLineParser parser;
    parser.setApplicationDescription("Emulates an ATEM switcher on UDP port 9910.");
    parser.addHelpOption();
    QCommandLineOption profile("profile", "Profile folder (dump.txt, macros.txt).", "dir", defaultProfile());
    QCommandLineOption listen("listen", "Address to listen on (default: all).", "address", "0.0.0.0");
    QCommandLineOption port("port", "UDP port (default 9910).", "port", "9910");
    QCommandLineOption verbose("verbose", "Log every command and reply.");
    parser.addOptions({ profile, listen, port, verbose });
    parser.process(app);

    emu::Device device;
    QObject::connect(&device, &emu::Device::log, &print);
    QString error;
    if (!device.load(QDir::cleanPath(parser.value(profile)), &error)) {
        print("error: " + error);
        return 1;
    }

    emu::Server server(&device);
    server.setVerbose(parser.isSet(verbose));
    QObject::connect(&server, &emu::Server::log, &print);
    if (!server.listen(QHostAddress(parser.value(listen)), parser.value(port).toUShort(), &error)) {
        print("error: cannot listen: " + error);
        return 1;
    }
    return app.exec();
}
