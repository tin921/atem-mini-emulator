// atem-sweep-gui: the sweep as a coverage map. Every test is a square; runs
// atem-sweep (next to this program) against the emulator or the real ATEM,
// with the real ATEM backed up first and restored after.

#include <QApplication>
#include <QTimer>

#include "window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("atem-sweep-gui");
    QFont font("Segoe UI");
    font.setPixelSize(13);
    app.setFont(font);
    Window w;
    w.show();
    // --screenshot FILE [--iface NAME [--prop NAME]] [--backups]: save the
    // window (that view) as it first appears and exit (for docs).
    const QStringList args = app.arguments();
    if (int i = args.indexOf("--iface"); i > 0 && i + 1 < args.size()) {
        int p = args.indexOf("--prop");
        w.showInterface(args[i + 1], p > 0 && p + 1 < args.size() ? args[p + 1] : QString());
    }
    if (args.contains("--backups")) w.showBackups();
    int shot = args.indexOf("--screenshot");
    // --selftest GROUPS --screenshot FILE: run those groups against the
    // emulator (never the real ATEM), save the window when done, exit.
    if (int i = args.indexOf("--selftest"); i > 0 && i + 1 < args.size() && shot > 0 && shot + 1 < args.size()) {
        QString file = args[shot + 1];
        QTimer::singleShot(500, &w, [&w, groups = args[i + 1].split(','), file]() {
            w.selfTest(groups, [&w, file]() {
                QTimer::singleShot(500, &w, [&w, file]() {   // let the rebuilt list lay out
                    w.grab().save(file);
                    QApplication::quit();
                });
            });
        });
        return app.exec();
    }
    if (shot > 0 && shot + 1 < args.size()) {
        QString file = args[shot + 1];
        QTimer::singleShot(1500, &w, [&w, file]() {
            w.grab().save(file);
            QApplication::quit();
        });
    }
    return app.exec();
}
