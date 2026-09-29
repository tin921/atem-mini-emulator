#include "backup.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

#include "sdk.h"
#include "transfer.h"
#include "wireproxy.h"

namespace {


QString sha256(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString& path, const QByteArray& data) {
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}

// Releases a COM pointer when it goes out of scope.
template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** out() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};


// The connect dump this connection received: every field up to and
// including InCm, each packet once (resends dropped). Only this connection's
// packets: the client that sent the first SYN after the mark. A connection
// closed just before (e.g. the restore's) may still be receiving packets.
std::vector<AtemField> connectDump(const WireProxy& wire, size_t mark, bool* complete) {
    std::vector<AtemField> out;
    std::set<quint16> seen;
    *complete = false;
    std::vector<WirePacket> packets = wire.since(mark);
    int client = -1;
    for (const auto& p : packets) {
        if (p.toDevice && (p.flags() & 0x10)) {   // SYN
            client = p.client;
            break;
        }
    }
    for (const auto& p : packets) {
        if (p.toDevice || p.client != client) continue;
        if (p.packetId() && !seen.insert(p.packetId()).second) continue;
        for (const auto& f : p.fields()) {
            out.push_back(f);
            if (f.name == "InCm") {
                *complete = true;
                return out;
            }
        }
    }
    return out;
}

QString slotFile(const QString& folder, int index, const char* ext) {
    return QString("%1/%2.%3").arg(folder).arg(index, 2, 10, QChar('0')).arg(ext);
}

} // namespace

int takeBackup(Switcher& s, WireProxy* wire, const QString& connectAddress, const QString& target,
               const QString& dir, QTextStream& out) {
    QDir d(dir);
    if (d.exists() && !d.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()) {
        out << "Backup folder is not empty (a backup is never overwritten): " << QDir::toNativeSeparators(dir) << "\n";
        return 2;
    }
    if (!QDir().mkpath(dir + "/macros") || !QDir().mkpath(dir + "/stills")) {
        out << "Cannot create " << QDir::toNativeSeparators(dir) << "\n";
        return 2;
    }

    QStringList problems;
    size_t mark = wire ? wire->mark() : 0;
    BMDSwitcherConnectToFailure fail = bmdSwitcherConnectToFailureNoResponse;
    HRESULT hr = s.connect(connectAddress, &fail);
    if (FAILED(hr)) {
        out << "Cannot connect: " << hrText(hr) << " " << fourcc(static_cast<uint32_t>(fail)) << "\n";
        return 2;
    }
    BSTR bname = nullptr;
    QString product = SUCCEEDED(SDK_CALL(IBMDSwitcher, s.sw, GetProductName, &bname)) ? takeBstr(bname) : QString();
    out << "Connected: " << product << "\n" << Qt::flush;

    QJsonObject manifest;
    manifest["format"] = "atem-sweep-backup";
    manifest["version"] = 1;
    manifest["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    manifest["target"] = target.isEmpty() ? QString("usb") : target;
    manifest["product"] = product;

    // ── State: the connect dump ──
    QJsonObject state;
    if (wire) {
        QThread::msleep(300);   // let the proxy log the last packets
        bool complete = false;
        std::vector<AtemField> dump = connectDump(*wire, mark, &complete);
        if (dump.empty()) {
            // The SDK falls back to a USB ATEM when the address doesn't answer.
            problems << "no state: nothing came through the proxy (did the SDK fall back to USB?)";
        } else {
            QByteArray text = QString("# connect dump: %1 fields, %2\n").arg(dump.size())
                                  .arg(complete ? "complete (ends with InCm)" : "INCOMPLETE (no InCm)").toUtf8();
            for (const auto& f : dump) text += f.name.toLatin1() + ' ' + f.data.toHex() + '\n';
            if (!writeFile(dir + "/state.txt", text)) problems << "cannot write state.txt";
            if (!complete) problems << "connect dump incomplete (no InCm)";
            state["fields"] = static_cast<int>(dump.size());
            state["complete"] = complete;
        }
    } else {
        problems << "no state: backups over USB can't capture the connect dump (use the ATEM's IP address)";
    }
    state["included"] = state.contains("fields");
    manifest["state"] = state;
    out << "State: " << (state["included"].toBool() ? QString::number(state["fields"].toInt()) + " fields" : QString("not included"))
        << "\n" << Qt::flush;

    // ── Macros ──
    QJsonArray macros;
    unsigned int maxMacros = 0;
    if (!s.pool || FAILED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, GetMaxCount, &maxMacros))) {
        problems << "macro pool not available";
    } else {
        TransferWait wait;
        auto* sink = new Sink<IBMDSwitcherMacroPoolCallback, BMDSwitcherMacroPoolEventType, unsigned int, IBMDSwitcherTransferMacro*>(
            [&wait](BMDSwitcherMacroPoolEventType t, unsigned int i, IBMDSwitcherTransferMacro*) {
                if (t == bmdSwitcherMacroPoolEventTypeTransferCompleted) wait.finish(static_cast<int>(i), "done");
                else if (t == bmdSwitcherMacroPoolEventTypeTransferFailed) wait.finish(static_cast<int>(i), "failed");
                else if (t == bmdSwitcherMacroPoolEventTypeTransferCancelled) wait.finish(static_cast<int>(i), "cancelled");
            });
        SDK_CALL(IBMDSwitcherMacroPool, s.pool, AddCallback, sink);
        for (unsigned int i = 0; i < maxMacros; ++i) {
            // A slot that can't be read is not an empty slot: a restore from
            // this backup would delete whatever is there.
            BOOL valid = FALSE;
            if (FAILED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, IsValid, i, &valid))) {
                problems << QString("macro %1: could not read whether the slot is in use").arg(i);
                continue;
            }
            if (!valid) continue;
            QJsonObject m;
            m["index"] = static_cast<int>(i);
            BSTR b = nullptr;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, GetName, i, &b))) m["name"] = takeBstr(b);
            else problems << QString("macro %1: could not read its name").arg(i);
            b = nullptr;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, GetDescription, i, &b))) m["description"] = takeBstr(b);
            else problems << QString("macro %1: could not read its description").arg(i);
            BOOL unsupported = FALSE;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, HasUnsupportedOps, i, &unsupported)))
                m["hasUnsupportedOps"] = unsupported == TRUE;

            wait.start(static_cast<int>(i));
            Ref<IBMDSwitcherTransferMacro> transfer;
            HRESULT dh = SDK_CALL(IBMDSwitcherMacroPool, s.pool, Download, i, transfer.out());
            QString result = SUCCEEDED(dh) && transfer ? wait.wait() : "Download " + hrText(dh);
            Ref<IBMDSwitcherMacro> macro;
            void* bytes = nullptr;
            if (result == "done" && SUCCEEDED(SDK_CALL(IBMDSwitcherTransferMacro, transfer.p, GetMacro, macro.out())) && macro &&
                SUCCEEDED(SDK_CALL(IBMDSwitcherMacro, macro.p, GetBytes, &bytes)) && bytes) {
                QByteArray data(static_cast<const char*>(bytes), SDK_CALL(IBMDSwitcherMacro, macro.p, GetSize));
                m["size"] = data.size();
                m["sha256"] = sha256(data);
                if (!writeFile(slotFile(dir + "/macros", i, "bin"), data))
                    problems << QString("macro %1: cannot write its file").arg(i);
            } else {
                problems << QString("macro %1: download %2").arg(i).arg(result == "done" ? "gave no bytes" : result);
            }
            macros.append(m);
            out << "  macro " << i << " \"" << m["name"].toString() << "\" " << m["size"].toInt() << " bytes\n" << Qt::flush;
        }
        SDK_CALL(IBMDSwitcherMacroPool, s.pool, RemoveCallback, sink);
        sink->Release();
    }
    manifest["macroSlots"] = static_cast<int>(maxMacros);
    manifest["macros"] = macros;

    // ── Stills ──
    QJsonArray stills;
    unsigned int stillCount = 0;
    Ref<IBMDSwitcherMediaPool> mediaPool;
    Ref<IBMDSwitcherStills> st;
    if (FAILED(s.sw->QueryInterface(__uuidof(IBMDSwitcherMediaPool), reinterpret_cast<void**>(mediaPool.out()))) || !mediaPool ||
        FAILED(SDK_CALL(IBMDSwitcherMediaPool, mediaPool.p, GetStills, st.out())) || !st ||
        FAILED(SDK_CALL(IBMDSwitcherStills, st.p, GetCount, &stillCount))) {
        problems << "stills not available";
    } else {
        TransferWait wait;
        auto* sink = new Sink<IBMDSwitcherStillsCallback, BMDSwitcherMediaPoolEventType, IBMDSwitcherFrame*, int>(
            [&wait](BMDSwitcherMediaPoolEventType t, IBMDSwitcherFrame* frame, int i) {
                if (t == bmdSwitcherMediaPoolEventTypeTransferCompleted) {
                    // The frame is only lent for the call: copy it now.
                    void* bytes = nullptr;
                    if (frame && SUCCEEDED(frame->GetBytes(&bytes)) && bytes) {
                        std::lock_guard<std::mutex> lock(wait.m);
                        if (i == wait.index) {
                            wait.width = frame->GetWidth();
                            wait.height = frame->GetHeight();
                            wait.rowBytes = frame->GetRowBytes();
                            wait.pixelFormat = static_cast<uint32_t>(frame->GetPixelFormat());
                            wait.bytes = QByteArray(static_cast<const char*>(bytes), wait.rowBytes * wait.height);
                        }
                    }
                    wait.finish(i, "done");
                } else if (t == bmdSwitcherMediaPoolEventTypeTransferFailed) {
                    wait.finish(i, "failed");
                } else if (t == bmdSwitcherMediaPoolEventTypeTransferCancelled) {
                    wait.finish(i, "cancelled");
                }
            });
        SDK_CALL(IBMDSwitcherStills, st.p, AddCallback, sink);
        auto* lock = new LockSink;
        bool locked = SUCCEEDED(SDK_CALL(IBMDSwitcherStills, st.p, Lock, lock)) &&
                      pollUntil([&] { return lock->obtained.load(); }, kLockTimeoutMs);
        if (!locked) problems << "could not lock the stills for downloading";
        for (unsigned int i = 0; i < stillCount; ++i) {
            BOOL valid = FALSE;
            if (FAILED(SDK_CALL(IBMDSwitcherStills, st.p, IsValid, i, &valid))) {
                problems << QString("still %1: could not read whether the slot is in use").arg(i);
                continue;
            }
            if (!valid) continue;
            QJsonObject m;
            m["index"] = static_cast<int>(i);
            BSTR b = nullptr;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherStills, st.p, GetName, i, &b))) m["name"] = takeBstr(b);
            else problems << QString("still %1: could not read its name").arg(i);
            BMDSwitcherHash hash{};
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherStills, st.p, GetHash, i, &hash)))
                m["hash"] = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(hash.data), 16).toHex());
            else problems << QString("still %1: could not read its hash").arg(i);
            if (locked) {
                wait.start(static_cast<int>(i));
                HRESULT dh = SDK_CALL(IBMDSwitcherStills, st.p, Download, i);
                QString result = SUCCEEDED(dh) ? wait.wait() : "Download " + hrText(dh);
                QByteArray data;
                {
                    std::lock_guard<std::mutex> g(wait.m);
                    data = wait.bytes;
                    m["width"] = wait.width;
                    m["height"] = wait.height;
                    m["rowBytes"] = wait.rowBytes;
                    m["pixelFormat"] = fourcc(wait.pixelFormat);
                }
                if (result == "done" && !data.isEmpty()) {
                    m["size"] = data.size();
                    m["sha256"] = sha256(data);
                    if (!writeFile(slotFile(dir + "/stills", i, "raw"), data))
                        problems << QString("still %1: cannot write its file").arg(i);
                    if (wait.pixelFormat == bmdSwitcherPixelFormat8BitARGB) {
                        QImage img(reinterpret_cast<const uchar*>(data.constData()), m["width"].toInt(), m["height"].toInt(),
                                   m["rowBytes"].toInt(), QImage::Format_ARGB32);
                        img.copy().save(slotFile(dir + "/stills", i, "png"));
                    }
                } else {
                    problems << QString("still %1: download %2").arg(i).arg(result == "done" ? "gave no bytes" : result);
                }
            }
            stills.append(m);
            out << "  still " << i << " \"" << m["name"].toString() << "\" " << m["size"].toInt() << " bytes\n" << Qt::flush;
        }
        if (locked) SDK_CALL(IBMDSwitcherStills, st.p, Unlock, lock);
        lock->Release();
        SDK_CALL(IBMDSwitcherStills, st.p, RemoveCallback, sink);
        sink->Release();
    }
    manifest["stillSlots"] = static_cast<int>(stillCount);
    manifest["stills"] = stills;

    manifest["complete"] = problems.isEmpty();
    manifest["problems"] = QJsonArray::fromStringList(problems);
    if (!writeFile(dir + "/manifest.json", QJsonDocument(manifest).toJson(QJsonDocument::Indented))) {
        out << "Cannot write manifest.json\n";
        return 3;
    }
    out << "Backup " << (problems.isEmpty() ? "complete" : "INCOMPLETE") << ": " << QDir::toNativeSeparators(dir) << "\n"
        << "  state " << (state["included"].toBool() ? "yes" : "no") << ", " << macros.size() << " of " << maxMacros
        << " macro slots in use, " << stills.size() << " of " << stillCount << " stills in use\n";
    for (const QString& p : problems) out << "  problem: " << p << "\n";
    return problems.isEmpty() ? 0 : 3;
}

// ── Checking a backup ────────────────────────────────────────

namespace {

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool isInt(const QJsonValue& v) { return v.isDouble() && v.toDouble() == static_cast<double>(v.toInteger()); }
bool isHex(const QJsonValue& v, int length) {
    const QString t = v.toString();
    return v.isString() && t.size() == length && QByteArray::fromHex(t.toLatin1()).toHex() == t.toLatin1();
}

} // namespace

bool validateBackup(const QString& dir, ValidBackup* b, QString* why) {
    auto fail = [&](const QString& reason) { *why = reason; return false; };
    QFile mf(dir + "/manifest.json");
    if (!mf.open(QIODevice::ReadOnly)) return fail("cannot read manifest.json");
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(mf.readAll(), &err);
    if (!doc.isObject()) return fail("manifest.json is not a JSON object: " + err.errorString());
    const QJsonObject m = doc.object();
    if (m["format"].toString() != "atem-sweep-backup") return fail("not an atem-sweep backup");
    if (!isInt(m["version"]) || m["version"].toInt() != 1) return fail("unsupported backup version");
    if (!m["complete"].isBool() || !m["complete"].toBool()) return fail("the backup is not complete");
    if (!m["product"].isString() || m["product"].toString().isEmpty()) return fail("no product name");
    if (!isInt(m["macroSlots"]) || m["macroSlots"].toInt() < 0 || !isInt(m["stillSlots"]) || m["stillSlots"].toInt() < 0)
        return fail("no slot counts");
    if (!m["macros"].isArray() || !m["stills"].isArray()) return fail("no macro or still list");
    const QJsonObject state = m["state"].toObject();
    if (!state["included"].toBool() || !state["complete"].toBool() || !isInt(state["fields"]) || state["fields"].toInt() <= 0)
        return fail("no complete switcher state");

    b->manifest = m;
    b->macroBytes.clear();
    b->stillBytes.clear();
    b->state.clear();
    auto checkSlots = [&](const char* key, int count, const char* dirName, const char* ext,
                     std::map<int, QByteArray>* bytes, const std::function<QString(const QJsonObject&)>& check) -> QString {
        for (const QJsonValue& v : m[key].toArray()) {
            if (!v.isObject()) return QString("%1: an entry is not an object").arg(key);
            const QJsonObject e = v.toObject();
            if (!isInt(e["index"]) || e["index"].toInt() < 0 || e["index"].toInt() >= count)
                return QString("%1: slot index missing or outside the pool").arg(key);
            const int i = e["index"].toInt();
            if (bytes->count(i)) return QString("%1: slot %2 listed twice").arg(key).arg(i);
            if (!e["name"].isString()) return QString("%1 %2: no name").arg(key).arg(i);
            if (!isHex(e["sha256"], 64) || !isInt(e["size"])) return QString("%1 %2: no checksum or size").arg(key).arg(i);
            if (QString problem = check(e); !problem.isEmpty()) return QString("%1 %2: %3").arg(key).arg(i).arg(problem);
            QByteArray data = readFile(slotFile(dir + "/" + dirName, i, ext));
            if (data.isEmpty() || data.size() != e["size"].toInt() || sha256(data) != e["sha256"].toString())
                return QString("%1 %2: its file is missing or damaged").arg(key).arg(i);
            (*bytes)[i] = data;
        }
        return {};
    };
    QString problem = checkSlots("macros", m["macroSlots"].toInt(), "macros", "bin", &b->macroBytes, [](const QJsonObject& e) {
        return e["description"].isString() ? QString() : QString("no description");
    });
    if (problem.isEmpty())
        problem = checkSlots("stills", m["stillSlots"].toInt(), "stills", "raw", &b->stillBytes, [](const QJsonObject& e) {
            if (!isHex(e["hash"], 32)) return QString("no switcher hash");
            if (!isInt(e["width"]) || !isInt(e["height"]) || !isInt(e["rowBytes"]) || e["width"].toInt() <= 0 ||
                e["height"].toInt() <= 0 || e["rowBytes"].toInt() <= 0 || e["pixelFormat"].toString().size() != 4)
                return QString("no frame format");
            if (e["size"].toInt() != e["rowBytes"].toInt() * e["height"].toInt()) return QString("size doesn't match the frame");
            return QString();
        });
    if (!problem.isEmpty()) return fail(problem);

    QFile sf(dir + "/state.txt");
    if (!sf.open(QIODevice::ReadOnly)) return fail("cannot read state.txt");
    for (const QByteArray& line : sf.readAll().split('\n')) {
        if (line.isEmpty() || line.startsWith('#')) continue;
        int sp = line.indexOf(' ');
        if (sp != 4) return fail("state.txt: a line is not NAME HEX");
        b->state.emplace_back(QString::fromLatin1(line.left(sp)), QString::fromLatin1(line.mid(sp + 1)));
    }
    if (static_cast<int>(b->state.size()) != state["fields"].toInt()) return fail("state.txt doesn't hold the fields the manifest lists");
    return true;
}

// ── Compare ──────────────────────────────────────────────────

namespace {

std::map<int, QJsonObject> bySlot(const QJsonArray& a) {
    std::map<int, QJsonObject> m;
    for (const auto& v : a) m[v.toObject()["index"].toInt()] = v.toObject();
    return m;
}

QString shortHex(const QString& hex) { return hex.size() > 48 ? hex.left(48) + "…" : hex; }

} // namespace

int compareBackups(const QString& dirA, const QString& dirB, QTextStream& out) {
    // Only complete, checked backups are compared: missing information is
    // "not comparable", never "identical".
    ValidBackup a, b;
    for (auto [dir, backup] : { std::pair{ &dirA, &a }, std::pair{ &dirB, &b } }) {
        QString why;
        if (!validateBackup(*dir, backup, &why)) {
            out << "Not comparable: " << QDir::toNativeSeparators(*dir) << ": " << why << "\n";
            return 2;
        }
    }
    int differences = 0;
    auto diff = [&](const QString& text) { out << "  " << text << "\n"; ++differences; };

    // State: fields compared by name and occurrence (the 3rd "KeBP" with the 3rd "KeBP").
    {
        auto keyed = [](const std::vector<std::pair<QString, QString>>& state) {
            std::map<QString, QString> m;
            std::map<QString, int> seen;
            for (const auto& [name, hex] : state) m[QString("%1#%2").arg(name).arg(seen[name]++)] = hex;
            return m;
        };
        auto ka = keyed(a.state), kb = keyed(b.state);
        out << "State (" << a.state.size() << " / " << b.state.size() << " fields):\n";
        for (const auto& [k, v] : ka) {
            auto it = kb.find(k);
            if (it == kb.end()) diff(k + " only in the first");
            else if (it->second != v) diff(k + ": " + shortHex(v) + " -> " + shortHex(it->second));
        }
        for (const auto& [k, v] : kb)
            if (!ka.count(k)) diff(k + " only in the second");
    }

    auto compareSlots = [&](const char* what, const char* key) {
        auto ma = bySlot(a.manifest[key].toArray()), mb = bySlot(b.manifest[key].toArray());
        out << what << " (" << ma.size() << " / " << mb.size() << " in use):\n";
        std::set<int> indices;
        for (const auto& [i, v] : ma) indices.insert(i);
        for (const auto& [i, v] : mb) indices.insert(i);
        for (int i : indices) {
            QString label = QString("%1 %2").arg(what).arg(i);
            if (!ma.count(i)) { diff(label + ": only in the second (\"" + mb[i]["name"].toString() + "\")"); continue; }
            if (!mb.count(i)) { diff(label + ": only in the first (\"" + ma[i]["name"].toString() + "\")"); continue; }
            for (const char* field : { "name", "description", "hash", "sha256", "size" }) {
                if (ma[i][field] != mb[i][field])
                    diff(QString("%1 %2: %3 -> %4").arg(label, field, ma[i][field].toVariant().toString(),
                                                        mb[i][field].toVariant().toString()));
            }
        }
    };
    compareSlots("macro", "macros");
    compareSlots("still", "stills");

    out << (differences ? QString("%1 difference(s)").arg(differences) : QString("identical")) << "\n";
    return differences ? 1 : 0;
}

// ── Restore ──────────────────────────────────────────────────

namespace {

uint32_t fromFourcc(const QString& s) {
    QByteArray b = s.toLatin1();
    if (b.size() != 4) return 0;
    return (uint32_t(uchar(b[0])) << 24) | (uint32_t(uchar(b[1])) << 16) | (uint32_t(uchar(b[2])) << 8) | uchar(b[3]);
}

// Downloads one stored macro's bytes (the pool callback must feed `wait`).
QString downloadMacro(IBMDSwitcherMacroPool* pool, unsigned int i, TransferWait& wait, QByteArray* data) {
    wait.start(static_cast<int>(i));
    Ref<IBMDSwitcherTransferMacro> transfer;
    HRESULT hr = SDK_CALL(IBMDSwitcherMacroPool, pool, Download, i, transfer.out());
    if (FAILED(hr) || !transfer) return "Download " + hrText(hr);
    QString result = wait.wait();
    if (result != "done") return result;
    Ref<IBMDSwitcherMacro> macro;
    void* bytes = nullptr;
    if (FAILED(SDK_CALL(IBMDSwitcherTransferMacro, transfer.p, GetMacro, macro.out())) || !macro ||
        FAILED(SDK_CALL(IBMDSwitcherMacro, macro.p, GetBytes, &bytes)) || !bytes)
        return "no bytes";
    *data = QByteArray(static_cast<const char*>(bytes), SDK_CALL(IBMDSwitcherMacro, macro.p, GetSize));
    return "done";
}

QString hashHex(const BMDSwitcherHash& hash) {
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(hash.data), 16).toHex());
}

} // namespace

int restoreBackup(Switcher& s, const QString& connectAddress, const QString& dir, QTextStream& out) {
    // The whole backup is checked before connecting: a manifest that is
    // incomplete, malformed or doesn't match its files changes nothing (a
    // missing list must never read as "delete everything").
    ValidBackup backup;
    QString why;
    if (!validateBackup(dir, &backup, &why)) {
        out << "Not a usable backup: " << QDir::toNativeSeparators(dir) << ": " << why << ": nothing restored\n";
        return 2;
    }
    const QJsonObject& manifest = backup.manifest;
    std::map<int, QJsonObject> macrosIn, stillsIn;
    for (const auto& v : manifest["macros"].toArray()) macrosIn[v.toObject()["index"].toInt()] = v.toObject();
    for (const auto& v : manifest["stills"].toArray()) stillsIn[v.toObject()["index"].toInt()] = v.toObject();
    std::map<int, QByteArray>& macroBytes = backup.macroBytes;
    std::map<int, QByteArray>& stillBytes = backup.stillBytes;

    BMDSwitcherConnectToFailure fail = bmdSwitcherConnectToFailureNoResponse;
    HRESULT hr = s.connect(connectAddress, &fail);
    if (FAILED(hr)) {
        out << "Cannot connect: " << hrText(hr) << " " << fourcc(static_cast<uint32_t>(fail)) << "\n";
        return 2;
    }
    BSTR bname = nullptr;
    QString product = SUCCEEDED(SDK_CALL(IBMDSwitcher, s.sw, GetProductName, &bname)) ? takeBstr(bname) : QString();
    if (product != manifest["product"].toString()) {
        out << "The backup is of a " << manifest["product"].toString() << ", this is a " << product << ": nothing restored\n";
        return 2;
    }
    // The pools must be the ones the backup was taken from.
    {
        unsigned int macroSlots = 0, stillSlots = 0;
        Ref<IBMDSwitcherMediaPool> mp;
        Ref<IBMDSwitcherStills> sp;
        bool read = s.pool && SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, GetMaxCount, &macroSlots)) &&
                    SUCCEEDED(s.sw->QueryInterface(__uuidof(IBMDSwitcherMediaPool), reinterpret_cast<void**>(mp.out()))) && mp &&
                    SUCCEEDED(SDK_CALL(IBMDSwitcherMediaPool, mp.p, GetStills, sp.out())) && sp &&
                    SUCCEEDED(SDK_CALL(IBMDSwitcherStills, sp.p, GetCount, &stillSlots));
        if (!read || static_cast<int>(macroSlots) != manifest["macroSlots"].toInt() ||
            static_cast<int>(stillSlots) != manifest["stillSlots"].toInt()) {
            out << "The switcher's macro / still pools don't match the backup's (" << manifest["macroSlots"].toInt() << " / "
                << manifest["stillSlots"].toInt() << "): nothing restored\n";
            return 2;
        }
    }
    if (s.macros) {
        BMDSwitcherMacroRunStatus run = bmdSwitcherMacroRunStatusIdle;
        BOOL loop = FALSE;
        unsigned int index = 0;
        BMDSwitcherMacroRecordStatus rec = bmdSwitcherMacroRecordStatusIdle;
        SDK_CALL(IBMDSwitcherMacroControl, s.macros, GetRunStatus, &run, &loop, &index);
        SDK_CALL(IBMDSwitcherMacroControl, s.macros, GetRecordStatus, &rec, &index);
        if (run != bmdSwitcherMacroRunStatusIdle || rec != bmdSwitcherMacroRecordStatusIdle) {
            out << "A macro is running or being recorded: nothing restored\n";
            return 4;
        }
    }

    QStringList problems;
    int changes = 0;
    auto report = [&](const QString& what, const QString& result, bool ok) {
        out << "  " << what << " (" << result << ")\n" << Qt::flush;
        if (!ok) problems << what + ": " + result;
        ++changes;
    };

    // ── Macros ──
    unsigned int maxMacros = 0;
    if (!s.pool || FAILED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, GetMaxCount, &maxMacros))) {
        problems << "macro pool not available";
    } else {
        TransferWait wait;
        auto* sink = new Sink<IBMDSwitcherMacroPoolCallback, BMDSwitcherMacroPoolEventType, unsigned int, IBMDSwitcherTransferMacro*>(
            [&wait](BMDSwitcherMacroPoolEventType t, unsigned int i, IBMDSwitcherTransferMacro*) {
                if (t == bmdSwitcherMacroPoolEventTypeTransferCompleted) wait.finish(static_cast<int>(i), "done");
                else if (t == bmdSwitcherMacroPoolEventTypeTransferFailed) wait.finish(static_cast<int>(i), "failed");
                else if (t == bmdSwitcherMacroPoolEventTypeTransferCancelled) wait.finish(static_cast<int>(i), "cancelled");
            });
        SDK_CALL(IBMDSwitcherMacroPool, s.pool, AddCallback, sink);
        for (unsigned int i = 0; i < maxMacros; ++i) {
            BOOL valid = FALSE;
            if (FAILED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, IsValid, i, &valid))) {
                problems << QString("macro %1: could not read whether the slot is in use: left as it is").arg(i);
                continue;
            }
            auto want = macrosIn.find(static_cast<int>(i));
            if (want == macrosIn.end()) {
                if (valid) {
                    HRESULT dh = SDK_CALL(IBMDSwitcherMacroPool, s.pool, Delete, i);
                    report(QString("macro %1: deleted").arg(i), hrText(dh), SUCCEEDED(dh));
                }
                continue;
            }
            const QJsonObject& m = want->second;
            QString name = m["name"].toString(), description = m["description"].toString();
            bool sameBytes = false;
            if (valid) {
                QByteArray current;
                sameBytes = downloadMacro(s.pool, i, wait, &current) == "done" && current == macroBytes[static_cast<int>(i)];
            }
            if (sameBytes) {
                BSTR b = nullptr;
                QString curName = SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, GetName, i, &b)) ? takeBstr(b) : QString();
                b = nullptr;
                QString curDesc = SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, GetDescription, i, &b)) ? takeBstr(b) : QString();
                if (curName != name) {
                    BSTR nb = makeBstr(name);
                    HRESULT nh = SDK_CALL(IBMDSwitcherMacroPool, s.pool, SetName, i, nb);
                    SysFreeString(nb);
                    report(QString("macro %1: name set back to \"%2\"").arg(i).arg(name), hrText(nh), SUCCEEDED(nh));
                }
                if (curDesc != description) {
                    BSTR db = makeBstr(description);
                    HRESULT dh = SDK_CALL(IBMDSwitcherMacroPool, s.pool, SetDescription, i, db);
                    SysFreeString(db);
                    report(QString("macro %1: description set back").arg(i), hrText(dh), SUCCEEDED(dh));
                }
                continue;
            }
            const QByteArray& data = macroBytes[static_cast<int>(i)];
            Ref<IBMDSwitcherMacro> macro;
            void* buffer = nullptr;
            QString result;
            if (FAILED(SDK_CALL(IBMDSwitcherMacroPool, s.pool, CreateMacro, static_cast<unsigned int>(data.size()), macro.out())) ||
                !macro || FAILED(SDK_CALL(IBMDSwitcherMacro, macro.p, GetBytes, &buffer)) || !buffer) {
                result = "CreateMacro failed";
            } else {
                memcpy(buffer, data.constData(), data.size());
                BSTR nb = makeBstr(name), db = makeBstr(description);
                wait.start(static_cast<int>(i));
                Ref<IBMDSwitcherTransferMacro> transfer;
                HRESULT uh = SDK_CALL(IBMDSwitcherMacroPool, s.pool, Upload, i, nb, db, macro.p, transfer.out());
                SysFreeString(nb);
                SysFreeString(db);
                result = SUCCEEDED(uh) ? wait.wait() : "Upload " + hrText(uh);
            }
            report(QString("macro %1: uploaded \"%2\"").arg(i).arg(name), result, result == "done");
        }
        SDK_CALL(IBMDSwitcherMacroPool, s.pool, RemoveCallback, sink);
        sink->Release();
    }

    // ── Stills ──
    unsigned int stillCount = 0;
    Ref<IBMDSwitcherMediaPool> mediaPool;
    Ref<IBMDSwitcherStills> st;
    if (FAILED(s.sw->QueryInterface(__uuidof(IBMDSwitcherMediaPool), reinterpret_cast<void**>(mediaPool.out()))) || !mediaPool ||
        FAILED(SDK_CALL(IBMDSwitcherMediaPool, mediaPool.p, GetStills, st.out())) || !st ||
        FAILED(SDK_CALL(IBMDSwitcherStills, st.p, GetCount, &stillCount))) {
        problems << "stills not available";
    } else {
        TransferWait wait;
        auto* sink = new Sink<IBMDSwitcherStillsCallback, BMDSwitcherMediaPoolEventType, IBMDSwitcherFrame*, int>(
            [&wait](BMDSwitcherMediaPoolEventType t, IBMDSwitcherFrame*, int i) {
                if (t == bmdSwitcherMediaPoolEventTypeTransferCompleted) wait.finish(i, "done");
                else if (t == bmdSwitcherMediaPoolEventTypeTransferFailed) wait.finish(i, "failed");
                else if (t == bmdSwitcherMediaPoolEventTypeTransferCancelled) wait.finish(i, "cancelled");
            });
        SDK_CALL(IBMDSwitcherStills, st.p, AddCallback, sink);
        auto* lock = new LockSink;
        bool locked = false;
        for (unsigned int i = 0; i < stillCount; ++i) {
            BOOL valid = FALSE;
            if (FAILED(SDK_CALL(IBMDSwitcherStills, st.p, IsValid, i, &valid))) {
                problems << QString("still %1: could not read whether the slot is in use: left as it is").arg(i);
                continue;
            }
            auto want = stillsIn.find(static_cast<int>(i));
            if (want == stillsIn.end()) {
                if (valid) {
                    HRESULT ih = SDK_CALL(IBMDSwitcherStills, st.p, SetInvalid, i);
                    report(QString("still %1: removed").arg(i), hrText(ih), SUCCEEDED(ih));
                }
                continue;
            }
            const QJsonObject& m = want->second;
            QString name = m["name"].toString();
            BMDSwitcherHash hash{};
            bool sameFrame = valid && SUCCEEDED(SDK_CALL(IBMDSwitcherStills, st.p, GetHash, i, &hash)) &&
                             hashHex(hash) == m["hash"].toString();
            if (sameFrame) {
                BSTR b = nullptr;
                QString curName = SUCCEEDED(SDK_CALL(IBMDSwitcherStills, st.p, GetName, i, &b)) ? takeBstr(b) : QString();
                if (curName != name) {
                    BSTR nb = makeBstr(name);
                    HRESULT nh = SDK_CALL(IBMDSwitcherStills, st.p, SetName, i, nb);
                    SysFreeString(nb);
                    report(QString("still %1: name set back to \"%2\"").arg(i).arg(name), hrText(nh), SUCCEEDED(nh));
                }
                continue;
            }
            if (!locked) {
                locked = SUCCEEDED(SDK_CALL(IBMDSwitcherStills, st.p, Lock, lock)) &&
                         pollUntil([&] { return lock->obtained.load(); }, kLockTimeoutMs);
                if (!locked) {
                    problems << "could not lock the stills for uploading";
                    break;
                }
            }
            const QByteArray& data = stillBytes[static_cast<int>(i)];
            Ref<IBMDSwitcherFrame> frame;
            void* buffer = nullptr;
            QString result;
            HRESULT ch = SDK_CALL(IBMDSwitcherMediaPool, mediaPool.p, CreateFrame,
                                  static_cast<BMDSwitcherPixelFormat>(fromFourcc(m["pixelFormat"].toString())),
                                  static_cast<unsigned int>(m["width"].toInt()), static_cast<unsigned int>(m["height"].toInt()),
                                  frame.out());
            if (FAILED(ch) || !frame || FAILED(SDK_CALL(IBMDSwitcherFrame, frame.p, GetBytes, &buffer)) || !buffer ||
                frame->GetRowBytes() * frame->GetHeight() != data.size()) {
                result = "CreateFrame " + hrText(ch);
            } else {
                memcpy(buffer, data.constData(), data.size());
                BSTR nb = makeBstr(name);
                wait.start(static_cast<int>(i));
                HRESULT uh = SDK_CALL(IBMDSwitcherStills, st.p, Upload, i, nb, frame.p);
                SysFreeString(nb);
                result = SUCCEEDED(uh) ? wait.wait() : "Upload " + hrText(uh);
            }
            report(QString("still %1: uploaded \"%2\"").arg(i).arg(name), result, result == "done");
        }
        if (locked) SDK_CALL(IBMDSwitcherStills, st.p, Unlock, lock);
        lock->Release();
        SDK_CALL(IBMDSwitcherStills, st.p, RemoveCallback, sink);
        sink->Release();
    }

    out << "Restore: " << changes << " change(s)" << (problems.isEmpty() ? "" : ", PROBLEMS:") << "\n";
    for (const QString& p : problems) out << "  problem: " << p << "\n";
    return problems.isEmpty() ? 0 : 3;
}
