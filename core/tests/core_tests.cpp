// Offline regression tests for the emulator core: malformed and hostile
// input, transfers, field identity, profiles, macro bytes, and the transport
// over real UDP on 127.0.0.3 (no SDK, no switcher). Run with ctest or
// directly; exit code 0 when every check passes.

#include "commands.h"
#include "device.h"
#include "server.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <cstdio>

#ifndef ATEM_EMU_PROFILES
#define ATEM_EMU_PROFILES "profiles"
#endif

using namespace emu;

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

const QString kProfile = QString(ATEM_EMU_PROFILES) + "/atem-mini_proto2.30_2026-09-28";

FieldList run(Device& d, const char* name, const QByteArray& body) {
    FieldList out = d.handle(QByteArray(name), body);
    out += d.endOfPacket();
    return out;
}

bool has(const FieldList& fields, const char* name) {
    for (const Field& f : fields)
        if (f.name == name) return true;
    return false;
}

QByteArray field(const Device& d, const char* name, const QByteArray& key) {
    for (const Field& f : d.connectDump())
        if (f.name == name && f.data.startsWith(key)) return f.data;
    return {};
}

QByteArray bytes(std::initializer_list<int> list) {
    QByteArray b;
    for (int v : list) b.append(static_cast<char>(v));
    return b;
}

// ── Device ────────────────────────────────────────────────────

void transfers() {
    Device d;
    QString error;
    check(d.load(kProfile, &error), "profile loads");

    // NET-1: slot numbers of 0x80000000 and up were negative indexes.
    QByteArray down(12, '\0');
    setU16(down, 0, 7); setU16(down, 2, 0xffff); setU32(down, 4, 0xffffffff);
    check(has(run(d, "FTSU", down), "FTDE"), "download of macro slot 0xffffffff: not found, no crash");
    QByteArray up(16, '\0');
    setU16(up, 0, 8); setU16(up, 2, 0xffff); setU32(up, 4, 0xffffffff); setU32(up, 8, 0);
    check(has(run(d, "FTSD", up), "FTDE"), "upload to macro slot 0xffffffff: refused");
    QByteArray fd(212, '\0');
    setU16(fd, 0, 8);
    check(!has(run(d, "FTFD", fd), "FTDC"), "its description finishes nothing (no transfer)");

    // A macro upload keeps its exact bytes.
    QList<MacroOp> ops{ { MacroOp::Wait, {}, {}, 10 }, { MacroOp::Command, "CPvI", bytes({ 0, 0, 0, 3 }), 0 } };
    const QByteArray macro = Macro::encode(ops);
    setU16(up, 0, 9); setU32(up, 4, 50); setU32(up, 8, static_cast<quint32>(macro.size()));
    check(has(run(d, "FTSD", up), "FTCD"), "macro upload to slot 51 accepted");
    setU16(fd, 0, 9);
    fd.replace(2, 4, "Test");
    run(d, "FTFD", fd);
    QByteArray data(4, '\0');
    setU16(data, 0, 9); setU16(data, 2, static_cast<quint16>(macro.size()));
    check(has(run(d, "FTDa", data + macro), "FTDC"), "macro upload completes");
    check(d.macro(50).used && d.macro(50).bytes == macro && d.macro(50).name == "Test", "the slot holds the exact bytes");

    // Still uploads: counted as the chunks arrive; an absurd run is refused.
    setU16(up, 0, 10); setU16(up, 2, 0); setU32(up, 4, 3); setU32(up, 8, 64);
    check(has(run(d, "FTSD", up), "FTCD"), "still upload to slot 4 accepted");
    setU16(fd, 0, 10);
    run(d, "FTFD", fd);
    setU16(data, 0, 10); setU16(data, 2, 64);
    check(has(run(d, "FTDa", data + QByteArray(64, '\x11')), "FTDC"), "a 64-byte still completes");
    setU16(up, 0, 11); setU32(up, 4, 4); setU32(up, 8, 64);
    run(d, "FTSD", up);
    setU16(fd, 0, 11);
    run(d, "FTFD", fd);
    QByteArray rle(8, '\xfe');
    QByteArray count(8, '\0');
    setU32(count, 0, 0x00100000);   // 2^52 words
    setU16(data, 0, 11); setU16(data, 2, 24);
    check(has(run(d, "FTDa", data + rle + count + QByteArray(8, '\x22')), "FTDE"), "a run of 2^52 words is refused");
}

void shortCommands() {
    Device d;
    QString error;
    d.load(kProfile, &error);
    const QByteArray before = field(d, "PrgI", bytes({ 0 }));
    run(d, "CPgI", QByteArray());
    run(d, "CPgI", bytes({ 0, 0, 0 }));
    check(field(d, "PrgI", bytes({ 0 })) == before, "CPgI with no or 3 bytes changes nothing (NET-4)");
    run(d, "CPgI", cmd::programInput(0, 2));
    check(u16(field(d, "PrgI", bytes({ 0 })), 2) == 2, "a whole CPgI still switches");

    // What the window builds is as long as what the SDK sends.
    int shortOnes = 0;
    QObject::connect(&d, &Device::log, [&](const QString& m) { if (m.startsWith("short command")) ++shortOnes; });
    cmd::DveParams p;
    run(d, "CPvI", cmd::previewInput(0, 3));
    run(d, "CKTp", cmd::keyType(0, 0, 3));
    run(d, "CKeF", cmd::keyFill(0, 0, 2));
    run(d, "CKOn", cmd::keyOnAir(0, 0, true));
    run(d, "CKDV", cmd::dve(0, 0, cmd::SizeX, p));
    run(d, "MAct", cmd::macroAction(0xffff, 1));
    run(d, "CMPr", cmd::macroProperties(0, "A", "B"));
    check(shortOnes == 0, "the window's own commands are never too short");
}

void fieldIdentity() {
    // CORE-4: a replacement lands on its own instance (MPfe: store, index).
    QTemporaryDir dir;
    QFile f(dir.filePath("dump.txt"));
    (void)f.open(QIODevice::WriteOnly);
    f.write("MPfe 0000000001aa\nMPfe 0000000101bb\nInCm 01\n");
    f.close();
    FieldStore store;
    QString error;
    check(store.load(f.fileName(), &error), "a small dump loads");
    Field patch{ "MPfe", QByteArray::fromHex("0000000101cc") };
    check(store.replace(patch), "replace finds still 2");
    FieldList dump = store.dump();
    check(dump[0].data == QByteArray::fromHex("0000000001aa") && dump[1].data == QByteArray::fromHex("0000000101cc"),
          "still 1 is untouched, still 2 changed");

    // CORE-5: damaged dumps are refused whole, and the old state stays.
    QFile bad(dir.filePath("bad.txt"));
    (void)bad.open(QIODevice::WriteOnly);
    bad.write("MPfe 00zz\nInCm 01\n");
    bad.close();
    check(!store.load(bad.fileName(), &error), "a dump with bad hex is refused");
    check(store.size() == 3, "and the loaded state is kept");
    Device d;
    QDir().mkpath(dir.filePath("profile"));
    QFile::copy(f.fileName(), dir.filePath("profile/dump.txt"));
    check(!d.load(dir.filePath("profile"), &error), "a profile without the Mini's fields is refused");
}

void macroBytes() {
    QList<MacroOp> ops{ { MacroOp::Wait, {}, {}, 70000 }, { MacroOp::UserWait, {}, {}, 0 },
                        { MacroOp::Command, "CPvI", bytes({ 0, 0, 0, 4 }), 0 } };
    QList<MacroOp> back;
    check(Macro::decode(Macro::encode(ops), &back), "encoded macro decodes");
    check(back.size() == 3 && back[0].frames == 70000 && back[1].kind == MacroOp::UserWait &&
              back[2].name == "CPvI" && back[2].data == ops[2].data,
          "wait (70000 frames), user wait and preview input survive the round trip");
    check(!Macro::decode(bytes({ 2, 0, 7, 0 }), &back), "a length below 4 is not a macro");
}

void locksAndOwners() {
    Device d;
    QString error;
    d.load(kProfile, &error);
    QByteArray lock(4, '\0');
    setU16(lock, 0, 0); setU8(lock, 2, 1);
    d.setCaller(5);
    check(has(run(d, "LOCK", lock), "LKOB"), "client 5 locks the stills");
    check(u8(field(d, "LKST", bytes({ 0, 0 })), 2) == 1, "LKST: locked");
    d.setCaller(0);
    d.clientGone(5);
    check(u8(field(d, "LKST", bytes({ 0, 0 })), 2) == 0, "client 5 gone: the lock is released (NET-5)");

    // Transfers belong to their connection.
    int used = -1;
    for (int i = 0; i < d.macroCount() && used < 0; ++i) if (d.macro(i).used) used = i;
    QByteArray down(12, '\0');
    setU16(down, 0, 7); setU16(down, 2, 0xffff); setU32(down, 4, static_cast<quint32>(used));
    d.setCaller(1);
    check(has(run(d, "FTSU", down), "FTDa"), "client 1 downloads a macro");
    QByteArray id(4, '\0');
    setU16(id, 0, 7);
    d.setCaller(2);
    run(d, "FTAD", id);
    check(!has(run(d, "FTUA", id), "FTDC"), "client 2 can't acknowledge client 1's transfer");
    d.setCaller(1);
    check(has(run(d, "FTUA", id), "FTDC"), "client 2's abort didn't touch it: client 1 finishes");
    d.setCaller(0);
}

// ── Transport over UDP ─────────────────────────────────────────

struct Client {
    QUdpSocket socket;
    quint16 session = 0x1234;
    quint16 server = 0;
    QHostAddress host{ "127.0.0.3" };
    quint16 port = 19911;

    static QByteArray header(quint8 flags, int length, quint16 session, quint16 ack, quint16 id) {
        QByteArray h(12, '\0');
        setU16(h, 0, static_cast<quint16>(flags << 8 | (length & 0x7ff)));
        setU16(h, 2, session);
        setU16(h, 4, ack);
        setU16(h, 10, id);
        return h;
    }
    static QByteArray command(const char* name, const QByteArray& body) {
        QByteArray c(8, '\0');
        setU16(c, 0, static_cast<quint16>(8 + body.size()));
        c.replace(4, 4, name);
        return c + body;
    }
    void pump(int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            while (socket.hasPendingDatagrams()) {
                QNetworkDatagram d = socket.receiveDatagram();
                const QByteArray p = d.data();
                if (p.size() >= 12 && (u8(p, 0) & 0x10)) server = static_cast<quint16>(0x8000 | u16(p, 14));
                else if (p.size() >= 12 && (u8(p, 0) & 0x08))    // acknowledge reliable packets
                    socket.writeDatagram(header(0x80, 12, server, u16(p, 10), 0), host, port);
            }
        }
    }
    bool connect() {
        socket.bind(host, 0);
        socket.writeDatagram(header(0x10, 20, session, 0, 0) + bytes({ 1, 0, 0, 0, 0, 0, 0, 0 }), host, port);
        pump(200);
        socket.writeDatagram(header(0x80, 12, session, 0, 0), host, port);
        pump(300);
        return server != 0;
    }
    void send(quint16 id, const QByteArray& commands, quint16 sessionId = 0, int lengthOverride = -1) {
        const int length = lengthOverride >= 0 ? lengthOverride : 12 + commands.size();
        socket.writeDatagram(header(0x08, length, sessionId ? sessionId : server, 0, id) + commands, host, port);
        pump(80);
    }
};

void transport() {
    Device d;
    QString error;
    d.load(kProfile, &error);
    Server server(&d);
    if (!server.listen(QHostAddress("127.0.0.3"), 19911, &error)) {
        std::printf("skip  transport tests: cannot listen on 127.0.0.3:19911 (%s)\n", qPrintable(error));
        return;
    }
    Client c;
    check(c.connect(), "client connects (handshake, dump)");
    auto program = [&] { return d.view().program; };
    auto pgm = [](int input) { return Client::command("CPgI", cmd::programInput(0, static_cast<quint16>(input))); };

    c.send(1, pgm(2));
    check(program() == 2, "packet 1 applied");
    c.send(3, pgm(4));
    check(program() == 2, "packet 3 after a gap: not applied yet (NET-2)");
    c.send(2, pgm(3));
    check(program() == 3, "packet 2 applied");
    c.send(3, pgm(4));
    check(program() == 4, "packet 3 resent: applied in order");

    c.send(4, pgm(1), 0x7777);
    check(program() == 4, "a packet with another session changes nothing (NET-3)");
    c.send(4, pgm(1), 0, 12);
    check(program() == 4, "a header whose length isn't the datagram's changes nothing (NET-4)");
    c.send(4, pgm(1) + bytes({ 1, 2, 3, 4, 5 }));
    check(program() == 4, "a damaged packet (garbage after a command) applies nothing (NET-4)");
    c.send(5, pgm(1));
    check(program() == 1, "the next packet still works");
    server.close();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    transfers();
    shortCommands();
    fieldIdentity();
    macroBytes();
    locksAndOwners();
    transport();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
