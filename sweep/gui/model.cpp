#include "model.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>

bool Model::shown(St s) const {
    if (s == St::Pass) return showPass;
    if (s == St::Fail) return showFail;
    if (s == St::Skip) return showSkip;
    return true;
}

bool Model::lit(const TestItem& t) const {
    switch (hover.kind) {
    case Hover::None: return true;
    case Hover::Iface: return t.iface == hover.value;
    case Hover::Prop: return t.iface == iface && t.prop == hover.value;
    case Hover::Test: return &t == &tests[hover.test];
    }
    return true;
}

namespace ui {

QColor color(St s) {
    switch (s) {
    case St::Pass: return QColor("#2f8f7c");
    case St::Fail: return QColor("#e4572e");
    case St::Skip: return QColor("#b9b4a8");
    case St::Running: return QColor("#f0a830");
    case St::Queued: return QColor("#e2ded5");
    case St::Off: return QColor("#ebe8e1");
    }
    return QColor("#e2ded5");
}

QString groupLabel(const QString& group) {
    if (group == "connect") return "Connect & probe";
    if (group == "generated") return "Every setter, good and bad values";
    if (group == "manual") return "Hand-written SDK calls";
    if (group == "storage") return "Stored macros & stills (backed up first on a real ATEM)";
    return "Scenarios: PiP, transitions, keys, macros";
}

QString statusWord(St s) {
    switch (s) {
    case St::Pass: return "same";
    case St::Fail: return "differs";
    case St::Skip: return "skipped";
    case St::Running: return "running";
    case St::Queued: return "not run";
    case St::Off: return "not in this run";
    }
    return {};
}

// g.<interface...>.<property>.<value> for setters, whose title reads
// "IBMDSwitcherXxx Property = value"; g.<interface>.state / .reset / ...;
// m.<area>.* and s.<area>.*; everything else by its first part.
void classify(TestItem& t) {
    const QStringList p = t.id.split('.');
    if (p.value(0) == "g") {
        int eq = t.title.indexOf(" = ");
        if (eq > 0) {
            QString prop = t.title.left(eq).section(' ', -1);
            for (int i = p.size() - 2; i >= 1; --i) {
                if (p[i].compare(prop, Qt::CaseInsensitive) == 0) {
                    t.iface = p.mid(0, i).join('.');
                    t.prop = p[i];
                    t.value = t.title.mid(eq + 3);
                    return;
                }
            }
        }
        static const QStringList tails = { "state", "reset", "callbacks", "bad" };
        if (p.size() <= 3 || tails.contains(p.last())) {
            t.iface = p.mid(0, p.size() - 1).join('.');
            t.prop = p.last();
        } else {
            t.iface = p.mid(0, p.size() - 2).join('.');
            t.prop = p[p.size() - 2];
            t.value = p.last();
        }
        return;
    }
    if ((p.value(0) == "m" || p.value(0) == "s") && p.size() >= 3) {
        t.iface = p.mid(0, 2).join('.');
        return;
    }
    t.iface = p.value(0);
}

// The verify diffs ("key: golden X, got Y", "events missing: A, B", ...) as
// value pairs: real switcher, this run.
QList<Diff> differences(const TestItem& t) {
    QList<Diff> out;
    static const QRegularExpression pair("^(.*): golden (.*), got (.*)$");
    static const QRegularExpression missing("^(.*): missing \\(golden (.*)\\)$");
    for (const QString& d : t.diffs) {
        if (auto m = pair.match(d); m.hasMatch()) out << Diff{ m.captured(1), m.captured(2), m.captured(3) };
        else if (auto k = missing.match(d); k.hasMatch()) out << Diff{ k.captured(1), k.captured(2), "missing" };
        else if (d.startsWith("events missing: "))
            for (const QString& e : d.mid(16).split(", ")) out << Diff{ "callback " + e, "fired", "not fired" };
        else if (d.startsWith("events extra: "))
            for (const QString& e : d.mid(14).split(", ")) out << Diff{ "callback " + e, "not fired", "fired" };
        else out << Diff{ d, "", "" };
    }
    return out;
}

QString brief(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isBool()) return v.toBool() ? "true" : "false";
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 10);
    if (v.isNull() || v.isUndefined()) return "null";
    if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
}

QString ago(const QDateTime& when) {
    qint64 s = when.secsTo(QDateTime::currentDateTime());
    if (s < 60) return "just now";
    if (s < 3600) return QString("%1 min ago").arg(s / 60);
    if (s < 86400) return QString("%1 h ago").arg(s / 3600);
    qint64 d = s / 86400;
    return d == 1 ? QString("1 day ago") : QString("%1 days ago").arg(d);
}

QList<BackupInfo> backups(const QString& root) {
    QList<BackupInfo> out;
    for (const QFileInfo& fi : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile f(fi.filePath() + "/manifest.json");
        if (!f.open(QIODevice::ReadOnly)) continue;
        QJsonObject m = QJsonDocument::fromJson(f.readAll()).object();
        if (m["format"].toString() != "atem-sweep-backup") continue;
        BackupInfo b;
        b.dir = fi.filePath();
        b.name = fi.fileName();
        b.target = m["target"].toString();
        b.product = m["product"].toString();
        b.created = QDateTime::fromString(m["created"].toString(), Qt::ISODate);
        if (!b.created.isValid()) b.created = fi.lastModified();
        b.complete = m["complete"].toBool();
        b.fields = m["state"].toObject()["fields"].toInt();
        b.macros = m["macros"].isArray() ? m["macros"].toArray().size() : m["macros"].toInt();
        b.stills = m["stills"].isArray() ? m["stills"].toArray().size() : m["stills"].toInt();
        b.problems = m["problems"].isArray() ? m["problems"].toArray().size() : m["problems"].toInt();
        static const QRegularExpression role("-(before|swept|after|restored)$");
        if (auto r = role.match(b.name); r.hasMatch()) b.role = r.captured(1);
        out << b;
    }
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.created > b.created; });
    return out;
}

QString roleLabel(const QString& role) {
    if (role == "before") return "Before the sweep";
    if (role == "swept") return "What the sweep left";
    if (role == "after") return "After restoring";
    if (role == "restored") return "After a restore";
    return "Backup";
}

QList<BackupGroup> backupGroups(const QString& root) {
    QList<BackupGroup> groups;
    QMap<QString, int> at;
    static const QRegularExpression sweep("^(.*-sweep)-(before|swept|after)$");
    for (const BackupInfo& b : backups(root)) {
        auto m = sweep.match(b.name);
        QString key = m.hasMatch() ? m.captured(1) : b.name;
        if (!at.contains(key)) {
            BackupGroup g;
            g.key = key;
            g.title = m.hasMatch() ? "Sweep" : b.role == "restored" ? "Restore" : "Backup";
            g.when = b.created;
            at[key] = groups.size();
            groups << g;
        }
        BackupGroup& g = groups[at[key]];
        g.members << b;
        g.when = std::min(g.when, b.created);
        QFile check(b.dir + "/restore-check.json");
        if (check.open(QIODevice::ReadOnly))
            g.verdict = QJsonDocument::fromJson(check.readAll()).object()["identical"].toBool() ? 1 : 0;
    }
    // Members in the order they were taken; groups newest first.
    for (BackupGroup& g : groups)
        std::sort(g.members.begin(), g.members.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.created < b.created; });
    std::sort(groups.begin(), groups.end(), [](const BackupGroup& a, const BackupGroup& b) { return a.when > b.when; });
    return groups;
}

} // namespace ui
