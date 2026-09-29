// Stored content: macros and stills — list, create, change, delete — and the
// transfers behind them. These change what the switcher has stored, so they
// only run when that is safe: inside a backup-protected run on a real switcher
// (backup first, restore and verify after; see main.cpp) or against the
// emulator (--verify). They use empty slots and delete what they create.
#include "tests_common.h"
#include "transfer.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

namespace {

void needStorage(Ctx& c) {
    c.needConnection();
    if (!c.opt.storageAllowed) c.skip("changes stored content: only in a backup-protected run (or against the emulator)");
    if (!c.s.pool || !c.s.macros) c.skip("no macro pool");
}

QString sha(const QByteArray& b) {
    return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex().left(16));
}

// ── Macros ───────────────────────────────────────────────────

int firstMacro(Ctx& c, bool valid) {
    unsigned int n = 0;
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetMaxCount, &n);
    for (int i = valid ? 0 : static_cast<int>(n) - 1; valid ? i < static_cast<int>(n) : i >= 0; valid ? ++i : --i) {
        BOOL v = FALSE;
        SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, IsValid, static_cast<unsigned int>(i), &v);
        if ((v != FALSE) == valid) return i;
    }
    return -1;
}

QJsonObject macroSlot(Ctx& c, unsigned int i) {
    BOOL valid = FALSE, unsupported = FALSE;
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, IsValid, i, &valid);
    QJsonObject o{ { "valid", valid != FALSE } };
    if (!valid) return o;
    BSTR b = nullptr;
    if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetName, i, &b))) o["name"] = takeBstr(b);
    b = nullptr;
    if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, GetDescription, i, &b))) o["description"] = takeBstr(b);
    if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, HasUnsupportedOps, i, &unsupported)))
        o["hasUnsupportedOps"] = unsupported != FALSE;
    return o;
}

// Downloads a macro: "done" and its bytes, or why not. Progress is recorded.
QString download(Ctx& c, unsigned int i, QByteArray* bytes, bool cancel = false) {
    TransferWait wait;
    auto* sink = macroTransferSink(wait);
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, AddCallback, sink);
    wait.start(static_cast<int>(i));
    Com<IBMDSwitcherTransferMacro> transfer;
    HRESULT hr = SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, Download, i, transfer.out());
    QString result = "Download " + hrText(hr);
    if (SUCCEEDED(hr) && transfer) {
        double progress = -1;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherTransferMacro, transfer.p, GetProgress, &progress)))
            c.observe("progress (informational)", progress);
        if (cancel) c.hr("cancel", SDK_CALL(IBMDSwitcherTransferMacro, transfer.p, Cancel));
        result = wait.wait();
        Com<IBMDSwitcherMacro> macro;
        void* data = nullptr;
        if (result == "done" && SUCCEEDED(SDK_CALL(IBMDSwitcherTransferMacro, transfer.p, GetMacro, macro.out())) && macro &&
            SUCCEEDED(SDK_CALL(IBMDSwitcherMacro, macro.p, GetBytes, &data)) && data)
            *bytes = QByteArray(static_cast<const char*>(data), SDK_CALL(IBMDSwitcherMacro, macro.p, GetSize));
    }
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, RemoveCallback, sink);
    sink->Release();
    return result;
}

QString upload(Ctx& c, unsigned int i, const QString& name, const QString& description, const QByteArray& bytes) {
    Com<IBMDSwitcherMacro> macro;
    void* data = nullptr;
    HRESULT ch = SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, CreateMacro, static_cast<unsigned int>(bytes.size()), macro.out());
    if (FAILED(ch) || !macro || FAILED(SDK_CALL(IBMDSwitcherMacro, macro.p, GetBytes, &data)) || !data)
        return "CreateMacro " + hrText(ch);
    memcpy(data, bytes.constData(), bytes.size());
    TransferWait wait;
    auto* sink = macroTransferSink(wait);
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, AddCallback, sink);
    wait.start(static_cast<int>(i));
    BSTR n = makeBstr(name), d = makeBstr(description);
    Com<IBMDSwitcherTransferMacro> transfer;
    HRESULT hr = SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, Upload, i, n, d, macro.p, transfer.out());
    SysFreeString(n);
    SysFreeString(d);
    QString result = SUCCEEDED(hr) ? wait.wait() : "Upload " + hrText(hr);
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, RemoveCallback, sink);
    sink->Release();
    c.settle();
    return result;
}

void removeMacro(Ctx& c, int i) {
    if (i < 0) return;
    BOOL v = FALSE;
    SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, IsValid, static_cast<unsigned int>(i), &v);
    if (v) SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, Delete, static_cast<unsigned int>(i));
    c.settle();
}

QByteArray sampleMacro(Ctx& c) {
    int src = firstMacro(c, true);
    QByteArray bytes;
    if (src >= 0) download(c, static_cast<unsigned int>(src), &bytes);
    return bytes;
}

// ── Stills ───────────────────────────────────────────────────

struct Stills {
    Com<IBMDSwitcherMediaPool> pool;
    Com<IBMDSwitcherStills> st;
    unsigned int count = 0;
};

bool stills(Ctx& c, Stills* s) {
    if (FAILED(c.s.sw->QueryInterface(__uuidof(IBMDSwitcherMediaPool), reinterpret_cast<void**>(s->pool.out()))) || !s->pool) return false;
    if (FAILED(SDK_CALL(IBMDSwitcherMediaPool, s->pool.p, GetStills, s->st.out())) || !s->st) return false;
    return SUCCEEDED(SDK_CALL(IBMDSwitcherStills, s->st.p, GetCount, &s->count));
}

int firstStill(Stills& s, bool valid) {
    for (int i = valid ? 0 : static_cast<int>(s.count) - 1; valid ? i < static_cast<int>(s.count) : i >= 0; valid ? ++i : --i) {
        BOOL v = FALSE;
        SDK_CALL(IBMDSwitcherStills, s.st.p, IsValid, static_cast<unsigned int>(i), &v);
        if ((v != FALSE) == valid) return i;
    }
    return -1;
}

QString hashOf(Stills& s, unsigned int i) {
    BMDSwitcherHash h{};
    HRESULT hr = SDK_CALL(IBMDSwitcherStills, s.st.p, GetHash, i, &h);
    return SUCCEEDED(hr) ? QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(h.data), 16).toHex()) : "error " + hrText(hr);
}

QJsonObject stillSlot(Stills& s, unsigned int i) {
    BOOL valid = FALSE;
    HRESULT hr = SDK_CALL(IBMDSwitcherStills, s.st.p, IsValid, i, &valid);
    if (FAILED(hr)) return QJsonObject{ { "isValid", "error " + hrText(hr) } };
    QJsonObject o{ { "valid", valid != FALSE } };
    if (!valid) return o;
    BSTR b = nullptr;
    if (SUCCEEDED(SDK_CALL(IBMDSwitcherStills, s.st.p, GetName, i, &b))) o["name"] = takeBstr(b);
    o["hash"] = hashOf(s, i);
    return o;
}

// Runs `f` with the stills locked and a transfer sink attached.
QString withLock(Ctx& c, Stills& s, TransferWait& wait, const std::function<QString()>& f) {
    auto* sink = stillTransferSink(wait);
    SDK_CALL(IBMDSwitcherStills, s.st.p, AddCallback, sink);
    auto* lock = new LockSink;
    bool locked = SUCCEEDED(SDK_CALL(IBMDSwitcherStills, s.st.p, Lock, lock)) && pollUntil([&] { return lock->obtained.load(); }, kLockTimeoutMs);
    QString result = locked ? f() : QString("could not lock the stills");
    if (locked) SDK_CALL(IBMDSwitcherStills, s.st.p, Unlock, lock);
    lock->Release();
    SDK_CALL(IBMDSwitcherStills, s.st.p, RemoveCallback, sink);
    sink->Release();
    c.settle();
    return result;
}

QString uploadStill(Ctx& c, Stills& s, unsigned int i, const QString& name, BMDSwitcherPixelFormat format) {
    Com<IBMDSwitcherFrame> frame;
    void* data = nullptr;
    HRESULT ch = SDK_CALL(IBMDSwitcherMediaPool, s.pool.p, CreateFrame, format, 1920, 1080, frame.out());
    if (FAILED(ch) || !frame || FAILED(SDK_CALL(IBMDSwitcherFrame, frame.p, GetBytes, &data)) || !data) return "CreateFrame " + hrText(ch);
    // A plain pattern: the rows count up, so any mix-up of rows shows in the hash.
    auto* bytes = static_cast<unsigned char*>(data);
    int rowBytes = frame->GetRowBytes();
    for (int y = 0; y < frame->GetHeight(); ++y)
        memset(bytes + y * rowBytes, (y * 7) & 0xFF, rowBytes);
    TransferWait wait;
    return withLock(c, s, wait, [&] {
        wait.start(static_cast<int>(i));
        BSTR n = makeBstr(name);
        HRESULT hr = SDK_CALL(IBMDSwitcherStills, s.st.p, Upload, i, n, frame.p);
        SysFreeString(n);
        return SUCCEEDED(hr) ? wait.wait() : "Upload " + hrText(hr);
    });
}

void removeStill(Ctx& c, Stills& s, int i) {
    if (i < 0) return;
    BOOL v = FALSE;
    SDK_CALL(IBMDSwitcherStills, s.st.p, IsValid, static_cast<unsigned int>(i), &v);
    if (v) SDK_CALL(IBMDSwitcherStills, s.st.p, SetInvalid, static_cast<unsigned int>(i));
    c.settle();
}

} // namespace

void registerStorageTests() {
    // ── Macros ──
    addTest("s.macro.download", "Download a stored macro (its bytes)", [](Ctx& c) {
        needStorage(c);
        int i = firstMacro(c, true);
        if (i < 0) c.skip("no stored macro");
        QByteArray bytes;
        c.observe("result", download(c, static_cast<unsigned int>(i), &bytes));
        c.observe("size", static_cast<double>(bytes.size()));
        c.observe("sha256", sha(bytes));
    });
    addTest("s.macro.download.bad", "Download an empty slot and an index out of range", [](Ctx& c) {
        needStorage(c);
        QByteArray bytes;
        int empty = firstMacro(c, false);
        if (empty >= 0) c.observe("emptySlot", download(c, static_cast<unsigned int>(empty), &bytes));
        c.observe("index1000", download(c, 1000, &bytes));
    });
    addTest("s.macro.download.cancel", "Download, cancelled straight away", [](Ctx& c) {
        needStorage(c);
        int i = firstMacro(c, true);
        if (i < 0) c.skip("no stored macro");
        QByteArray bytes;
        c.observe("result (informational)", download(c, static_cast<unsigned int>(i), &bytes, true));
    });
    addTest("s.macro.create", "CreateMacro: a local macro object (nothing sent)", [](Ctx& c) {
        needStorage(c);
        for (unsigned int size : { 16u, 0u, 1u << 20 }) {
            Com<IBMDSwitcherMacro> m;
            HRESULT hr = SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, CreateMacro, size, m.out());
            c.observe(QString("create.%1").arg(size), hrText(hr));
            if (m) c.observe(QString("size.%1").arg(size), static_cast<double>(SDK_CALL(IBMDSwitcherMacro, m.p, GetSize)));
        }
    });
    addTest("s.macro.upload", "Upload a copy of a macro into an empty slot, list it, download it, delete it", [](Ctx& c) {
        needStorage(c);
        QByteArray bytes = sampleMacro(c);
        int slot = firstMacro(c, false);
        if (bytes.isEmpty() || slot < 0) c.skip("needs a stored macro and an empty slot");
        c.observe("before", macroSlot(c, slot));
        c.observe("upload", upload(c, slot, "sweep upload", "atem-sweep test", bytes));
        c.observe("after", macroSlot(c, slot));
        QByteArray back;
        c.observe("download", download(c, slot, &back));
        c.observe("sameBytes", back == bytes);
        c.hr("delete", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, Delete, static_cast<unsigned int>(slot)));
        c.settle();
        c.observe("deleted", macroSlot(c, slot));
        removeMacro(c, slot);
    });
    addTest("s.macro.upload.bad", "Upload out of range and nonsense bytes", [](Ctx& c) {
        needStorage(c);
        QByteArray bytes = sampleMacro(c);
        int slot = firstMacro(c, false);
        if (bytes.isEmpty() || slot < 0) c.skip("needs a stored macro and an empty slot");
        c.observe("index1000", upload(c, 1000, "sweep bad", "", bytes));
        c.observe("nonsense", upload(c, slot, "sweep nonsense", "", QByteArray(16, char(0xFF))));
        c.observe("nonsense.after", macroSlot(c, slot));
        removeMacro(c, slot);
    });
    addTest("s.macro.record", "Record a macro (a command, a pause, a user wait), list it, download it, delete it", [](Ctx& c) {
        needStorage(c);
        int slot = firstMacro(c, false);
        if (slot < 0) c.skip("no empty slot");
        BMDSwitcherInputId preview = 0;
        SDK_CALL(IBMDSwitcherMixEffectBlock, c.s.me, GetPreviewInput, &preview);
        BSTR n = makeBstr("sweep record"), d = makeBstr("atem-sweep test");
        c.hr("record", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, Record, static_cast<unsigned int>(slot), n, d));
        SysFreeString(n);
        SysFreeString(d);
        c.settle();
        BMDSwitcherMacroRecordStatus status{};
        unsigned int index = 0;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, GetRecordStatus, &status, &index)))
            c.observe("recording", QJsonObject{ { "status", js(status) }, { "index", static_cast<double>(index) } });
        c.hr("command", SDK_CALL(IBMDSwitcherMixEffectBlock, c.s.me, SetPreviewInput, preview));   // changes nothing
        c.hr("pause", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, RecordPause, 25));
        c.hr("userWait", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, RecordUserWait));
        c.hr("stop", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, StopRecording));
        c.settle();
        c.observe("after", macroSlot(c, slot));
        QByteArray bytes;
        c.observe("download", download(c, slot, &bytes));
        c.observe("bytes", QString::fromLatin1(bytes.toHex()));
        removeMacro(c, slot);
        c.observe("deleted", macroSlot(c, slot));
    });
    addTest("s.macro.record.bad", "Record out of range; pause, user wait and stop while not recording", [](Ctx& c) {
        needStorage(c);
        BSTR n = makeBstr("sweep bad");
        c.hr("record.index1000", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, Record, 1000, n, n));
        SysFreeString(n);
        c.settle();
        SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, StopRecording);   // in case it did start
        c.settle();
        c.hr("pause.notRecording", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, RecordPause, 25));
        c.hr("userWait.notRecording", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, RecordUserWait));
        c.hr("stop.notRecording", SDK_CALL(IBMDSwitcherMacroControl, c.s.macros, StopRecording));
    });
    addTest("s.macro.rename", "Rename and re-describe a macro (long and empty text too)", [](Ctx& c) {
        needStorage(c);
        QByteArray bytes = sampleMacro(c);
        int slot = firstMacro(c, false);
        if (bytes.isEmpty() || slot < 0) c.skip("needs a stored macro and an empty slot");
        upload(c, slot, "sweep rename", "atem-sweep test", bytes);
        struct T { const char* id; QString name; };
        for (T t : { T{ "name", "renamed" }, T{ "name.long", QString(40, 'N') }, T{ "name.empty", "" } }) {
            BSTR b = makeBstr(t.name);
            c.hr(QString("set.%1").arg(t.id), SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, SetName, static_cast<unsigned int>(slot), b));
            SysFreeString(b);
            c.settle();
            c.observe(QString("after.%1").arg(t.id), macroSlot(c, slot)["name"]);
        }
        for (T t : { T{ "description", "described" }, T{ "description.long", QString(200, 'D') } }) {
            BSTR b = makeBstr(t.name);
            c.hr(QString("set.%1").arg(t.id), SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, SetDescription, static_cast<unsigned int>(slot), b));
            SysFreeString(b);
            c.settle();
            c.observe(QString("after.%1").arg(t.id), macroSlot(c, slot)["description"]);
        }
        BSTR b = makeBstr("x");
        int empty = firstMacro(c, false);
        if (empty >= 0) c.hr("set.emptySlot", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, SetName, static_cast<unsigned int>(empty), b));
        c.hr("set.index1000", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, SetName, 1000, b));
        SysFreeString(b);
        removeMacro(c, slot);
    });
    addTest("s.macro.delete.bad", "Delete an empty slot and an index out of range", [](Ctx& c) {
        needStorage(c);
        int empty = firstMacro(c, false);
        if (empty >= 0) c.hr("emptySlot", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, Delete, static_cast<unsigned int>(empty)));
        c.hr("index1000", SDK_CALL(IBMDSwitcherMacroPool, c.s.pool, Delete, 1000));
    });

    // ── Stills ──
    addTest("s.still.list", "Every still slot: valid, name, hash", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        QJsonArray slotList;
        for (unsigned int i = 0; i < s.count; ++i) slotList.append(stillSlot(s, i));
        c.observe("stills", slotList);
        c.observe("hash.index1000", hashOf(s, 1000));
    });
    addTest("s.still.download", "Download a stored still (its frame)", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        int i = firstStill(s, true);
        if (i < 0) c.skip("no stored still");
        TransferWait wait;
        QString result = withLock(c, s, wait, [&] {
            wait.start(i);
            HRESULT hr = SDK_CALL(IBMDSwitcherStills, s.st.p, Download, static_cast<unsigned int>(i));
            double progress = -1;
            if (SUCCEEDED(SDK_CALL(IBMDSwitcherStills, s.st.p, GetProgress, &progress))) c.observe("progress (informational)", progress);
            return SUCCEEDED(hr) ? wait.wait() : "Download " + hrText(hr);
        });
        c.observe("result", result);
        c.observe("frame", QJsonObject{ { "width", wait.width }, { "height", wait.height }, { "rowBytes", wait.rowBytes },
                                        { "pixelFormat", fourcc(wait.pixelFormat) }, { "size", static_cast<double>(wait.bytes.size()) } });
    });
    addTest("s.still.download.bad", "Download an empty slot, out of range, and a cancelled download", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        TransferWait wait;
        int empty = firstStill(s, false), valid = firstStill(s, true);
        c.observe("results", withLock(c, s, wait, [&] {
            QJsonObject r;
            if (empty >= 0) {
                wait.start(empty);
                HRESULT hr = SDK_CALL(IBMDSwitcherStills, s.st.p, Download, static_cast<unsigned int>(empty));
                r["emptySlot"] = SUCCEEDED(hr) ? wait.wait(5000) : "Download " + hrText(hr);
            }
            r["index1000"] = hrText(SDK_CALL(IBMDSwitcherStills, s.st.p, Download, 1000));
            if (valid >= 0) {
                wait.start(valid);
                SDK_CALL(IBMDSwitcherStills, s.st.p, Download, static_cast<unsigned int>(valid));
                r["cancel"] = hrText(SDK_CALL(IBMDSwitcherStills, s.st.p, CancelTransfer));
                r["cancelled (informational)"] = wait.wait(5000);
            }
            return QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
        }));
    });
    for (bool yuva : { false, true }) {
        addTest(QString("s.still.upload.%1").arg(yuva ? "yuva" : "argb"),
                QString("Upload a %1 still into an empty slot, list it, remove it").arg(yuva ? "10-bit YUVA" : "8-bit ARGB"), [yuva](Ctx& c) {
                    needStorage(c);
                    Stills s;
                    if (!stills(c, &s)) c.skip("no stills");
                    int slot = firstStill(s, false);
                    if (slot < 0) c.skip("no empty still slot");
                    c.observe("upload", uploadStill(c, s, slot, "sweep still",
                                                    yuva ? bmdSwitcherPixelFormat10BitYUVA : bmdSwitcherPixelFormat8BitARGB));
                    c.observe("after", stillSlot(s, slot));
                    c.hr("setInvalid", SDK_CALL(IBMDSwitcherStills, s.st.p, SetInvalid, static_cast<unsigned int>(slot)));
                    c.settle();
                    c.observe("removed", stillSlot(s, slot));
                    removeStill(c, s, slot);
                });
    }
    addTest("s.still.upload.bad", "Upload out of range, and without the lock", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        c.observe("index1000", uploadStill(c, s, 1000, "sweep bad", bmdSwitcherPixelFormat8BitARGB));
        int slot = firstStill(s, false);
        if (slot < 0) return;
        Com<IBMDSwitcherFrame> frame;
        if (SUCCEEDED(SDK_CALL(IBMDSwitcherMediaPool, s.pool.p, CreateFrame, bmdSwitcherPixelFormat8BitARGB, 1920, 1080, frame.out()))) {
            BSTR n = makeBstr("sweep unlocked");
            c.hr("unlocked", SDK_CALL(IBMDSwitcherStills, s.st.p, Upload, static_cast<unsigned int>(slot), n, frame.p));
            SysFreeString(n);
            c.settle();
            c.settle();
        }
        c.observe("unlocked.after (informational)", stillSlot(s, slot));
        removeStill(c, s, slot);
    });
    addTest("s.still.rename", "Rename a still (long and empty names too)", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        int slot = firstStill(s, false);
        if (slot < 0) c.skip("no empty still slot");
        uploadStill(c, s, slot, "sweep rename", bmdSwitcherPixelFormat8BitARGB);
        struct T { const char* id; QString name; };
        for (T t : { T{ "name", "renamed" }, T{ "long", QString(80, 'N') }, T{ "empty", "" } }) {
            BSTR b = makeBstr(t.name);
            c.hr(QString("set.%1").arg(t.id), SDK_CALL(IBMDSwitcherStills, s.st.p, SetName, static_cast<unsigned int>(slot), b));
            SysFreeString(b);
            c.settle();
            c.observe(QString("after.%1").arg(t.id), stillSlot(s, slot)["name"]);
        }
        BSTR b = makeBstr("x");
        c.hr("set.index1000", SDK_CALL(IBMDSwitcherStills, s.st.p, SetName, 1000, b));
        SysFreeString(b);
        removeStill(c, s, slot);
    });
    addTest("s.still.setInvalid.bad", "SetInvalid on an empty slot and out of range", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        int empty = firstStill(s, false);
        if (empty >= 0) c.hr("emptySlot", SDK_CALL(IBMDSwitcherStills, s.st.p, SetInvalid, static_cast<unsigned int>(empty)));
        c.hr("index1000", SDK_CALL(IBMDSwitcherStills, s.st.p, SetInvalid, 1000));
    });
    addTest("s.still.capture", "Capture the program as a still; see where it goes; remove it", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        Com<IBMDSwitcherStillCapture> cap;
        if (FAILED(c.s.sw->QueryInterface(__uuidof(IBMDSwitcherStillCapture), reinterpret_cast<void**>(cap.out()))) || !cap)
            c.skip("no still capture");
        BOOL available = FALSE;
        c.hr("isAvailable", SDK_CALL(IBMDSwitcherStillCapture, cap.p, IsAvailable, &available));
        c.observe("available", available != FALSE);
        std::vector<bool> before;
        for (unsigned int i = 0; i < s.count; ++i) {
            BOOL v = FALSE;
            SDK_CALL(IBMDSwitcherStills, s.st.p, IsValid, i, &v);
            before.push_back(v != FALSE);
        }
        c.hr("capture", SDK_CALL(IBMDSwitcherStillCapture, cap.p, CaptureStill));
        c.settle();
        QThread::msleep(1000);
        QJsonArray added;
        for (unsigned int i = 0; i < s.count; ++i) {
            BOOL v = FALSE;
            SDK_CALL(IBMDSwitcherStills, s.st.p, IsValid, i, &v);
            if (v && !before[i]) {
                QJsonObject o = stillSlot(s, i);
                o["index"] = static_cast<double>(i);
                o.remove("hash");   // the live picture: differs every time
                added.append(o);
                removeStill(c, s, static_cast<int>(i));
            }
        }
        c.observe("added", added);
    });
    addTest("s.media.clear", "Clear the media pool (every still), list the stills", [](Ctx& c) {
        needStorage(c);
        Stills s;
        if (!stills(c, &s)) c.skip("no stills");
        c.hr("clear", SDK_CALL(IBMDSwitcherMediaPool, s.pool.p, Clear));
        c.settle();
        QThread::msleep(500);
        int valid = 0;
        for (unsigned int i = 0; i < s.count; ++i) {
            BOOL v = FALSE;
            SDK_CALL(IBMDSwitcherStills, s.st.p, IsValid, i, &v);
            valid += v ? 1 : 0;
        }
        c.observe("validAfter", valid);
    });
}
