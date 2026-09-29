// atem-sweep — exercises the BMDSwitcherAPI against an ATEM (or the emulator),
// good and bad input alike, and records every response.
//
//   record:  atem-sweep 192.168.0.240                 (golden record + wire capture)
//   verify:  atem-sweep 127.0.0.1 --verify golden.json (emulator vs the real device)
//   usb:     atem-sweep usb                           (API only, no wire capture)
//   backup:  atem-sweep 192.168.0.240 --backup [--out DIR]   (read-only: state, macros, stills)
//   compare: atem-sweep --compare DIR1 DIR2                  (two backups)
//
// Safety policy: reads everything, changes settings and puts them back, runs
// and stops macros. Never deletes, uploads, clears, records or streams.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>
#include <cstdio>
#include <iostream>

#include "backup.h"
#include "runner.h"
#include "wireproxy.h"

#include <windows.h>

#ifndef SWEEP_COVERAGE_DIR
#define SWEEP_COVERAGE_DIR "coverage"
#endif
#ifndef SWEEP_BACKUP_DIR
#define SWEEP_BACKUP_DIR "backups"
#endif

static void enableConsole() {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

int main(int argc, char** argv) {
    // COM for the BMD SDK; never uninitialised (see obs-atem: CoUninitialize
    // while the SDK's network threads wind down crashes the process).
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    enableConsole();

    QCoreApplication app(argc, argv);
    app.setApplicationName("atem-sweep");

    QCommandLineParser p;
    p.setApplicationDescription("Sweeps the Blackmagic ATEM switcher API and records every response.");
    p.addHelpOption();
    p.addPositionalArgument("target", "ATEM address (e.g. 192.168.0.240), 127.0.0.1 for the emulator, or \"usb\".");
    QCommandLineOption verify("verify", "Compare against a golden results.json instead of recording.", "golden");
    QCommandLineOption noProxy("no-proxy", "Do not capture wire traffic (the SDK talks to the target directly).");
    QCommandLineOption only("only", "Run only tests whose id starts with this (connect.main always runs).", "prefix");
    QCommandLineOption list("list", "List the tests and exit.");
    QCommandLineOption outDir("out", "Output folder (default: runs\\<time>-<mode>).", "dir");
    QCommandLineOption yes("yes", "Do not ask before changing the switcher's output.");
    QCommandLineOption allowCamera("allow-camera", "Also send camera actions (autofocus) to Blackmagic cameras.");
    QCommandLineOption coverageDir("coverage-dir", "Folder with tier1-plugin.txt, tier2-samples.txt, excluded.txt.", "dir");
    QCommandLineOption capture("capture", "Run no tests: only the recording proxy on 127.0.0.1:9910, for another "
                                          "client (e.g. ATEM Software Control), for this many seconds or until "
                                          "a file named \"stop\" appears in the output folder.", "seconds");
    QCommandLineOption backup("backup", "Run no tests: take a full backup (state, macros, stills) into the output "
                                        "folder. Read-only.");
    QCommandLineOption compare("compare", "Compare two backup folders (given as the two arguments) and exit.");
    p.addOptions({ verify, noProxy, only, list, outDir, yes, allowCamera, coverageDir, capture, backup, compare });
    p.process(app);

    if (p.isSet(compare)) {
        if (p.positionalArguments().size() != 2) {
            QTextStream(stdout) << "--compare needs two backup folders\n";
            return 2;
        }
        QTextStream out(stdout);
        return compareBackups(p.positionalArguments()[0], p.positionalArguments()[1], out);
    }

    registerConnectTests();
    registerInputTests();
    registerMixEffectTests();
    registerKeyTests();
    registerDownstreamKeyTests();
    registerMacroTests();
    registerMediaTests();
    registerDeviceTests();

    Options opt;
    opt.listOnly = p.isSet(list);
    opt.only = p.value(only);
    opt.allowCamera = p.isSet(allowCamera);
    opt.coverageDir = p.isSet(coverageDir) ? p.value(coverageDir) : QString(SWEEP_COVERAGE_DIR);

    Switcher s;
    if (opt.listOnly) return runSweep(s, nullptr, opt);

    if (p.positionalArguments().isEmpty()) {
        p.showHelp(2);
    }
    QString target = p.positionalArguments().first();

    if (p.isSet(capture)) {
        QString dir = p.isSet(outDir) ? p.value(outDir)
                                      : QDir(QCoreApplication::applicationDirPath()).filePath(
                                            "runs/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-capture");
        if ((QFileInfo(dir).exists() && !QFileInfo(dir).isDir()) || !QDir().mkpath(dir)) {
            QTextStream(stdout) << "Cannot use output folder " << QDir::toNativeSeparators(dir) << "\n";
            return 2;
        }
        QFile::remove(dir + "/stop");
        WireProxy proxy(QHostAddress::LocalHost, QHostAddress(target));
        QString error;
        if (!proxy.startProxy(&error)) {
            QTextStream(stdout) << "Cannot start the proxy on 127.0.0.1:9910: " << error << "\n";
            return 2;
        }
        int seconds = p.value(capture).toInt();
        QTextStream(stdout) << "Capturing: connect clients to 127.0.0.1, traffic goes to " << target << " ("
                            << seconds << " s, or create " << QDir::toNativeSeparators(dir + "/stop") << ")\n" << Qt::flush;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < seconds * 1000LL && !QFile::exists(dir + "/stop")) QThread::msleep(200);
        proxy.stopProxy();
        if (!proxy.writeLog(dir + "/wire.jsonl")) {
            QTextStream(stdout) << "Could not write " << QDir::toNativeSeparators(dir + "/wire.jsonl") << "\n";
            return 3;
        }
        QTextStream(stdout) << "Wrote " << QDir::toNativeSeparators(dir + "/wire.jsonl") << "\n";
        return 0;
    }

    if (p.isSet(backup)) {
        QString t = target.compare("usb", Qt::CaseInsensitive) == 0 ? QString() : target;
        bool loopback = t == "127.0.0.1" || t.compare("localhost", Qt::CaseInsensitive) == 0;
        bool proxy = !p.isSet(noProxy) && !t.isEmpty() && !loopback;
        // Backups hold the switcher's stored macros and stills: by default they
        // go to the source tree's backups/ (not the build folder, not in git).
        QString dir = p.isSet(outDir) ? p.value(outDir)
                                      : QString(SWEEP_BACKUP_DIR) + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        WireProxy* wire = nullptr;
        if (proxy) {
            wire = new WireProxy(QHostAddress::LocalHost, QHostAddress(t));
            QString error;
            if (!wire->startProxy(&error)) {
                QTextStream(stdout) << "Cannot start the proxy on 127.0.0.1:9910: " << error << "\n";
                delete wire;
                return 2;
            }
        }
        QTextStream out(stdout);
        out << "atem-sweep backup (read-only)  target: " << (t.isEmpty() ? QString("USB") : t) << "\n" << Qt::flush;
        int rc = takeBackup(s, wire, proxy ? QString("127.0.0.1") : t, t, dir, out);
        s.disconnect();
        if (wire) {
            wire->stopProxy();
            delete wire;
        }
        return rc;
    }

    opt.target = target.compare("usb", Qt::CaseInsensitive) == 0 ? QString() : target;
    opt.verify = p.isSet(verify);
    opt.goldenPath = p.value(verify);
    bool local = opt.target == "127.0.0.1" || opt.target.compare("localhost", Qt::CaseInsensitive) == 0;
    // The proxy listens on 127.0.0.1:9910, so it can't sit in front of an
    // emulator on 127.0.0.1 (same port) or a USB connection (not network
    // traffic). An emulator on 127.0.0.2 (atem-emu --listen 127.0.0.2) is fine.
    opt.useProxy = !p.isSet(noProxy) && !opt.target.isEmpty() && !local;
    opt.connectAddress = opt.useProxy ? QString("127.0.0.1") : opt.target;
    opt.outDir = p.isSet(outDir) ? p.value(outDir)
                                 : QDir(QCoreApplication::applicationDirPath()).filePath(
                                       QString("runs/%1-%2").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"),
                                                                 opt.verify ? "verify" : "record"));

    QTextStream(stdout) << "\x1b[1matem-sweep\x1b[0m  target: " << (opt.target.isEmpty() ? "USB" : opt.target)
                        << (opt.useProxy ? "  (via recording proxy 127.0.0.1:9910)" : "")
                        << "  mode: " << (opt.verify ? "verify against " + opt.goldenPath : QString("record")) << "\n"
                        << "\x1b[33mThis switches inputs, moves the PiP, runs the stored macros and fades to black\n"
                        << "on the live output. The settings it changes are put back and checked at the end;\n"
                        << "your stored macros may change others (e.g. audio), which are not. Nothing is deleted.\x1b[0m\n";
    if (!p.isSet(yes)) {
        QTextStream(stdout) << "Continue? [y/N] " << Qt::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") return 6;   // declined: nothing done
    }

    WireProxy* wire = nullptr;
    if (opt.useProxy) {
        wire = new WireProxy(QHostAddress::LocalHost, QHostAddress(opt.target));
        QString error;
        if (!wire->startProxy(&error)) {
            QTextStream(stdout) << "\x1b[31mCannot start the proxy on 127.0.0.1:9910: " << error
                                << " (is an emulator listening on 127.0.0.1? use --listen 127.0.0.2)\x1b[0m\n";
            return 2;
        }
    }

    int rc = runSweep(s, wire, opt);
    s.disconnect();
    if (wire) {
        wire->stopProxy();
        delete wire;
    }
    return rc;
}
