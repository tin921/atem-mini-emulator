#pragma once
// The GUI's view of a sweep: one TestItem per test, filled from the test
// plan (atem-sweep --list --json), the golden record and the run's progress.

#include <QColor>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

enum class St { Queued, Running, Pass, Fail, Skip, Off };

struct TestItem {
    QString id, title, group;
    QString iface, prop, value;       // for navigation: g.fl.comp / ratio / 50
    St status = St::Queued;
    QJsonObject obs, goldenObs;       // this run's observations, the golden record's
    QStringList diffs, notes, problems, sdk, goldenSdk;
    int ms = 0;
    bool ran = false;
};

// One value that differs from the real switcher.
struct Diff {
    QString key, golden, got;
};

struct Hover {
    enum Kind { None, Iface, Prop, Test } kind = None;
    QString value;   // interface or property name
    int test = -1;
};

struct Model {
    QVector<TestItem> tests;
    bool showPass = true, showFail = true, showSkip = true;
    int selected = -1;
    Hover hover;
    QString iface;   // the interface being looked at (for property hovers)
    QString gotLabel = "Emulator";   // who "this run" is: Emulator or This run

    bool shown(St s) const;
    bool lit(const TestItem& t) const;   // not dimmed by the current hover
};

// A backup folder (sweep/backups/<name>/manifest.json).
struct BackupInfo {
    QString dir, name, target, product;
    QString role;                 // before, swept, after (a sweep's), restored, or "" (Backup now)
    QDateTime created;
    bool complete = false;
    int fields = 0, macros = 0, stills = 0, problems = 0;
};

// The backups of one sweep (<time>-sweep-before / -swept / -after), or one
// backup on its own.
struct BackupGroup {
    QString key, title;
    QDateTime when;
    QList<BackupInfo> members;
    int verdict = -1;             // restore checked: 1 identical, 0 not, -1 unknown
};

namespace ui {
QColor color(St s);
QString groupLabel(const QString& group);
QString statusWord(St s);
void classify(TestItem& t);
QList<Diff> differences(const TestItem& t);
QString brief(const QJsonValue& v);
QString ago(const QDateTime& when);
QList<BackupInfo> backups(const QString& root);
QList<BackupGroup> backupGroups(const QString& root);
QString roleLabel(const QString& role);
}
