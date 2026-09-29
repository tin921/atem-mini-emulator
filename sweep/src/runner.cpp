#include "runner.h"
#include "groups.h"
#include "wireproxy.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

// ── Console ──────────────────────────────────────────────────

namespace {

void out(const QString& s) {
    QByteArray b = s.toUtf8();
    fwrite(b.constData(), 1, static_cast<size_t>(b.size()), stdout);
    fflush(stdout);
}

QString paint(const QString& s, const char* ansi) {
    return QString("\x1b[%1m%2\x1b[0m").arg(ansi, s);
}

// For atem-sweep-gui: one JSON object per line, marked so it can be told
// from the text around it.
void jsonLine(const Options& opt, const QJsonObject& o) {
    if (opt.json) out("@@" + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) + "\n");
}

QString mark(const QString& status) {
    if (status == "pass") return paint(QString::fromUtf8("✓"), "32");
    if (status == "fail") return paint(QString::fromUtf8("✗"), "31");
    if (status == "skip") return paint(QString::fromUtf8("○"), "33");
    return paint("!", "35");
}

// ── Comparison with the golden record ────────────────────────

bool sameValue(const QJsonValue& a, const QJsonValue& b) {
    if (a.isDouble() && b.isDouble()) {
        double x = a.toDouble(), y = b.toDouble();
        return std::abs(x - y) <= 1e-3 + 1e-4 * std::max(std::abs(x), std::abs(y));
    }
    if (a.isArray() && b.isArray()) {
        QJsonArray x = a.toArray(), y = b.toArray();
        if (x.size() != y.size()) return false;
        for (int i = 0; i < x.size(); ++i)
            if (!sameValue(x[i], y[i])) return false;
        return true;
    }
    if (a.isObject() && b.isObject()) {
        QJsonObject x = a.toObject(), y = b.toObject();
        if (x.keys() != y.keys()) return false;
        for (const QString& k : x.keys())
            if (!sameValue(x[k], y[k])) return false;
        return true;
    }
    return a == b;
}

QString brief(const QJsonValue& v) {
    QString s = v.isString() ? v.toString()
              : QString::fromUtf8(QJsonDocument(QJsonArray{ v }).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
    return s.size() > 60 ? s.left(57) + "..." : s;
}

// Events left out of the comparison because they depend on history, not on
// what the test did:
//  - Switcher/setc: the time code ticks every second.
//  - Input:*/iprt, ipvt: program/preview tally changes. Which input's tally
//    drops depends on sources used earlier (the key remembers fills across
//    type changes, even from a previous run). Tally itself is still compared
//    through input.properties (programTallied / previewTallied).
//  - Stills/lbsy: "lock busy" after locking the media pool. The real ATEM sent
//    the same packets in the same order in two recordings (2026-09-26 and
//    09-28), and the SDK fired it in one and not the other.
bool timingEvent(const QString& e) {
    return e == "Switcher/setc" || e == "Stills/lbsy" ||
           (e.startsWith("Input:") && (e.endsWith("/iprt") || e.endsWith("/ipvt")));
}

std::set<QString> eventSet(const QJsonObject& result) {
    std::set<QString> s;
    for (const auto& e : result["events"].toArray()) {
        QJsonObject o = e.toObject();
        QString key = o["source"].toString() + "/" + o["type"].toString();
        if (!timingEvent(key)) s.insert(key);
    }
    return s;
}

QStringList compareWithGolden(const QJsonObject& gold, const QJsonObject& got) {
    QStringList diffs;
    if (gold["status"].toString() == "skip" || got["status"].toString() == "skip") {
        if (gold["status"] != got["status"])
            diffs << QString("status: golden %1, got %2").arg(gold["status"].toString(), got["status"].toString());
        return diffs;
    }
    QJsonObject g = gold["obs"].toObject(), o = got["obs"].toObject();
    for (const QString& k : g.keys()) {
        if (k.contains("(informational)")) continue;   // timings etc.
        if (!o.contains(k)) diffs << QString("%1: missing (golden %2)").arg(k, brief(g[k]));
        else if (!sameValue(g[k], o[k])) diffs << QString("%1: golden %2, got %3").arg(k, brief(g[k]), brief(o[k]));
    }
    std::set<QString> ge = eventSet(gold), oe = eventSet(got);
    QStringList missing, extra;
    for (const auto& e : ge) if (!oe.count(e)) missing << e;
    for (const auto& e : oe) if (!ge.count(e)) extra << e;
    if (!missing.isEmpty()) diffs << "events missing: " + missing.join(", ");
    if (!extra.isEmpty()) diffs << "events extra: " + extra.join(", ");
    return diffs;
}

// ── Coverage report ──────────────────────────────────────────

std::map<QString, QString> loadList(const QString& path) {
    std::map<QString, QString> m;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.startsWith('#') || line.trimmed().isEmpty()) continue;
        QStringList parts = line.split('\t');
        m[parts[0].trimmed()] = parts.size() > 1 ? parts[1].trimmed() : QString();
    }
    return m;
}

QString coverageReport(const Options& opt, QJsonObject& json) {
    QStringList called = sweepCoverage();
    std::set<QString> calledSet(called.begin(), called.end());
    QStringList unavailable = sweepUnavailableInterfaces();
    std::set<QString> unavailableSet(unavailable.begin(), unavailable.end());
    auto excluded = loadList(opt.coverageDir + "/excluded.txt");

    QString text;
    QTextStream ts(&text);
    for (const auto& [tier, file] : { std::pair<QString, QString>{ "Tier 1 (obs-atem plugin)", "tier1-plugin.txt" },
                                      std::pair<QString, QString>{ "Tier 2 (SDK samples)", "tier2-samples.txt" } }) {
        auto list = loadList(opt.coverageDir + "/" + file);
        int done = 0, na = 0, ex = 0;
        QStringList missing;
        QJsonArray missingJson;
        for (const auto& [method, where] : list) {
            QString iface = method.section("::", 0, 0);
            if (calledSet.count(method)) ++done;
            else if (unavailableSet.count(iface)) ++na;
            else if (excluded.count(method)) ++ex;
            else {
                missing << method;
                missingJson.append(method);
            }
        }
        int total = static_cast<int>(list.size());
        int accounted = done + na + ex;
        ts << QString("%1: %2/%3 accounted for — %4 called, %5 not on this model, %6 excluded by policy")
                  .arg(tier).arg(accounted).arg(total).arg(done).arg(na).arg(ex);
        if (total == 0) ts << "  (list not found in " << opt.coverageDir << ")";
        ts << "\n";
        for (const QString& m : missing) ts << "    MISSING " << m << "\n";
        json[file] = QJsonObject{ { "total", total }, { "called", done }, { "notOnModel", na },
                                  { "excluded", ex }, { "missing", missingJson } };
    }
    ts << QString("SDK methods called in total: %1; interfaces not on this model: %2\n")
              .arg(called.size()).arg(unavailable.isEmpty() ? "none" : unavailable.join(", "));
    json["called"] = QJsonArray::fromStringList(called);
    json["notOnModel"] = QJsonArray::fromStringList(unavailable);
    return text;
}

} // namespace

// ── Registry ─────────────────────────────────────────────────

std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

void addTest(const QString& id, const QString& title, std::function<void(Ctx&)> run) {
    registry().push_back({ id, title, std::move(run) });
}

// ── Run ──────────────────────────────────────────────────────

int runSweep(Switcher& s, WireProxy* wire, const Options& opt) {
    std::vector<const Test*> selected;
    for (const auto& t : registry()) {
        bool wanted = (opt.only.isEmpty() || t.id.startsWith(opt.only)) &&
                      (opt.groups.isEmpty() || opt.groups.contains(testGroup(t.id)));
        if (wanted || t.id.startsWith("connect.main")) selected.push_back(&t);
    }

    QJsonArray plan;
    for (const auto* t : selected) plan.append(QJsonObject{ { "id", t->id }, { "title", t->title } });
    jsonLine(opt, QJsonObject{ { "plan", plan } });
    if (opt.listOnly && opt.json) return 0;
    if (opt.listOnly) {
        for (const auto* t : selected) out(QString("%1  %2\n").arg(t->id, -34).arg(t->title));
        out(QString("%1 tests\n").arg(selected.size()));
        return 0;
    }

    std::map<QString, QJsonObject> golden;
    if (opt.verify) {
        QFile f(opt.goldenPath);
        if (!f.open(QIODevice::ReadOnly)) {
            out(paint("Cannot read golden record " + opt.goldenPath + "\n", "31"));
            return 2;
        }
        // A reference must be a recording that passed: every test "pass" or
        // "skip" (e.g. an empty macro slot), each id once.
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
        QStringList invalid;
        if (doc.isNull()) invalid << "not JSON: " + parseError.errorString();
        else if (!doc.object()["tests"].isArray()) invalid << "no \"tests\" array";
        for (const auto& v : doc.object()["tests"].toArray()) {
            QJsonObject t = v.toObject();
            QString id = t["id"].toString();
            QString status = t["status"].toString();
            if (id.isEmpty()) invalid << "a test without an id";
            else if (golden.count(id)) invalid << "duplicate test id " + id;
            else if (status != "pass" && status != "skip")
                invalid << QString("%1 has status \"%2\" (a reference may only pass or skip)").arg(id, status);
            golden[id] = t;
        }
        if (golden.empty() && invalid.isEmpty()) invalid << "no tests";
        if (!invalid.isEmpty()) {
            out(paint("Not a usable golden record: " + opt.goldenPath + "\n", "31"));
            for (const QString& why : invalid) out("  " + why + "\n");
            return 2;
        }
    }

    // Check the output folder before touching the switcher.
    QFileInfo outInfo(opt.outDir);
    if ((outInfo.exists() && !outInfo.isDir()) || !QDir().mkpath(opt.outDir)) {
        out(paint("Cannot use output folder " + QDir::toNativeSeparators(opt.outDir) + "\n", "31"));
        return 2;
    }
    {
        QFile probe(opt.outDir + "/.write-test");
        bool writable = probe.open(QIODevice::WriteOnly) && probe.write("x") == 1;
        probe.close();
        probe.remove();
        if (!writable) {
            out(paint("Cannot write in output folder " + QDir::toNativeSeparators(opt.outDir) + "\n", "31"));
            return 2;
        }
    }
    QJsonArray results;
    Snapshot* snapshot = nullptr;
    int pass = 0, fail = 0, skip = 0, error = 0;
    int n = static_cast<int>(selected.size());
    QString productName;

    bool stopped = false;
    for (int i = 0; i < n; ++i) {
        if (!opt.stopFile.isEmpty() && QFile::exists(opt.stopFile)) {
            out(paint(QString("\nStopped after %1 of %2 tests.\n").arg(i).arg(n), "33;1"));
            jsonLine(opt, QJsonObject{ { "stopped", i } });
            stopped = true;
            break;
        }
        const Test& t = *selected[i];
        jsonLine(opt, QJsonObject{ { "start", t.id }, { "i", i + 1 }, { "n", n } });
        Ctx c{ s, wire, opt };
        c.eventMark = s.events.size();
        size_t wireMark = wire ? wire->mark() : 0;
        qint64 start = s.events.elapsed();
        QElapsedTimer timer;
        timer.start();

        QString status = "pass";
        QStringList notes;
        sweepTestStarted();
        try {
            t.run(c);
            if (!c.problems.isEmpty()) status = "fail";
        } catch (const SkipTest& k) {
            status = "skip";
            notes << k.why;
        } catch (const std::exception& e) {
            status = "error";
            notes << e.what();
        }
        // Let late echoes land in this test, not the next one.
        s.events.settle(80, 600);
        qint64 ms = timer.elapsed();

        QJsonArray events;
        for (const auto& e : s.events.since(c.eventMark))
            events.append(QJsonObject{ { "ms", e.ms - start }, { "source", e.source }, { "type", e.type } });

        QJsonObject r{
            { "id", t.id }, { "title", t.title }, { "status", status },
            { "problems", QJsonArray::fromStringList(c.problems) },
            { "notes", QJsonArray::fromStringList(notes) },
            { "obs", c.obs }, { "events", events }, { "ms", ms },
            { "sdk", QJsonArray::fromStringList(sweepTestCalls()) },
        };
        if (wire) r["wire"] = wire->fieldsSince(wireMark);

        QStringList diffs;
        if (opt.verify && status != "error") {
            auto g = golden.find(t.id);
            if (g == golden.end()) diffs << "not in golden record";
            else diffs = compareWithGolden(g->second, r);
            // Against the golden record, a test the device passed that can't
            // run here (skip) is a failure too.
            if (!diffs.isEmpty() && (status == "pass" || status == "skip")) status = "fail";
            r["status"] = status;
            r["diffs"] = QJsonArray::fromStringList(diffs);
        }
        results.append(r);
        if (opt.json) {
            QJsonObject line = r;
            line.remove("wire");
            line["i"] = i + 1;
            line["n"] = n;
            if (opt.verify) {
                auto g = golden.find(t.id);
                if (g != golden.end()) {
                    line["goldenObs"] = g->second["obs"];
                    line["goldenSdk"] = g->second["sdk"];
                }
            }
            jsonLine(opt, QJsonObject{ { "result", line } });
        }

        if (status == "pass") ++pass;
        else if (status == "fail") ++fail;
        else if (status == "skip") ++skip;
        else ++error;

        out(QString("[%1/%2] %3 %4 %5 %6\n")
                .arg(i + 1, 3).arg(n, -3)
                .arg(mark(status))
                .arg(t.id, -36)
                .arg(t.title, -44)
                .arg(paint(QString("%1 ms").arg(ms, 5), "90")));
        for (const QString& p : c.problems) out("        " + paint("problem: ", "31") + p + "\n");
        for (const QString& d : diffs) out("        " + paint("diff: ", "31") + d + "\n");
        for (const QString& x : notes) out("        " + paint("note: ", "33") + x + "\n");

        if (t.id == "connect.main" && s.connected() && !snapshot) {
            // Nothing has changed yet. Don't start while a macro is being
            // recorded (the sweep's commands would end up in it) or runs.
            BMDSwitcherMacroRecordStatus rec = bmdSwitcherMacroRecordStatusIdle;
            BMDSwitcherMacroRunStatus run = bmdSwitcherMacroRunStatusIdle;
            unsigned int recIndex = 0, runIndex = 0;
            BOOL loop = FALSE;
            if (s.macros) {
                s.macros->GetRecordStatus(&rec, &recIndex);
                s.macros->GetRunStatus(&run, &loop, &runIndex);
            }
            bool recording = rec != bmdSwitcherMacroRecordStatusIdle;
            if (recording || run != bmdSwitcherMacroRunStatusIdle) {
                out(paint(QString("\nThe switcher is %1 macro %2: stopping before changing anything.\n")
                              .arg(recording ? "recording" : "running").arg((recording ? recIndex : runIndex) + 1), "31;1"));
                return 4;
            }
            snapshot = takeSnapshot(s);
            productName = c.obs["productName"].toString();
        }
    }

    // A full verification also needs every test of the reference to exist.
    if (opt.verify && opt.only.isEmpty() && opt.groups.isEmpty() && !stopped) {
        std::set<QString> ran;
        for (const auto* t : selected) ran.insert(t->id);
        for (const auto& [id, g] : golden) {
            if (ran.count(id)) continue;
            out("        " + paint("missing: ", "31") + id + " is in the golden record but not in this sweep\n");
            ++fail;
        }
    }

    QStringList restoreLog;
    bool restored = true;
    if (snapshot) {
        out(paint("\nRestoring the switcher to its state before the sweep...\n", "36"));
        jsonLine(opt, QJsonObject{ { "phase", "settings" }, { "state", "start" } });
        restored = restoreSnapshot(s, snapshot, restoreLog);
        jsonLine(opt, QJsonObject{ { "phase", "settings" }, { "state", restored ? "ok" : "fail" } });
        for (const QString& l : restoreLog) out("  " + l + "\n");
        if (!restored) out(paint("  RESTORE INCOMPLETE: see above\n", "31;1"));
    }

    QJsonObject coverage;
    QString coverageText = coverageReport(opt, coverage);
    out("\n" + paint("Coverage", "1") + "\n" + coverageText);

    QString summary = QString("%1 passed, %2 failed, %3 skipped, %4 errors (%5 tests)")
                          .arg(pass).arg(fail).arg(skip).arg(error).arg(n);
    out("\n" + paint(summary, fail || error ? "31;1" : "32;1") + "\n");
    jsonLine(opt, QJsonObject{ { "summary", QJsonObject{ { "pass", pass }, { "fail", fail }, { "skip", skip }, { "error", error },
                                                         { "restored", restored }, { "stopped", stopped }, { "out", opt.outDir } } } });

    QJsonObject doc{
        { "meta", QJsonObject{
              { "tool", "atem-sweep" },
              { "mode", opt.verify ? "verify" : "record" },
              { "target", opt.target.isEmpty() ? "usb" : opt.target },
              { "viaProxy", opt.useProxy },
              { "productName", productName },
              { "date", QDateTime::currentDateTime().toString(Qt::ISODate) },
              { "golden", opt.goldenPath },
          } },
        { "summary", QJsonObject{ { "pass", pass }, { "fail", fail }, { "skip", skip }, { "error", error } } },
        { "coverage", coverage },
        { "restore", QJsonArray::fromStringList(restoreLog) },
        { "restored", restored },
        { "tests", results },
    };
    auto writeFile = [](const QString& path, const QByteArray& data) {
        QSaveFile f(path);   // written completely or not at all
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
    };
    QStringList unwritten;
    if (!writeFile(opt.outDir + "/results.json", QJsonDocument(doc).toJson())) unwritten << "results.json";
    if (wire && !wire->writeLog(opt.outDir + "/wire.jsonl")) unwritten << "wire.jsonl";
    if (!writeFile(opt.outDir + "/coverage.txt", coverageText.toUtf8())) unwritten << "coverage.txt";
    if (!unwritten.isEmpty()) {
        out(paint("Could not write " + unwritten.join(", ") + " in " + QDir::toNativeSeparators(opt.outDir) + "\n", "31;1"));
        return 3;
    }
    out(paint("Results: " + QDir::toNativeSeparators(opt.outDir) + "\n", "90"));

    if (!restored) return 5;
    return (fail || error) ? 1 : 0;
}
