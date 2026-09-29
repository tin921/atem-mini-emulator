#include "server.h"
#include "device.h"

#include <QNetworkDatagram>

namespace emu {

namespace {

constexpr quint8 kReliable = 0x08;
constexpr quint8 kSyn = 0x10;
constexpr quint8 kResend = 0x20;
constexpr quint8 kRequestResend = 0x40;
constexpr quint8 kAck = 0x80;

constexpr int kHeaderSize = 12;
constexpr int kMaxPacket = 1420;          // the real switcher fills packets to about this
constexpr qint64 kResendAfterMs = 400;
constexpr int kMaxTries = 15;
constexpr qint64 kSilenceTimeoutMs = 60000;

// Packet ids are 15 bits and wrap: is a newer than b?
bool newer(quint16 a, quint16 b) {
    quint16 diff = (a - b) & 0x7fff;
    return diff != 0 && diff < 0x4000;
}

QByteArray encodeField(const Field& f) {
    QByteArray out(8, '\0');
    setU16(out, 0, static_cast<quint16>(8 + f.data.size()));
    out.replace(4, 4, f.name.left(4));
    return out + f.data;
}

QString fieldSummary(const FieldList& fields) {
    QStringList names;
    for (const auto& f : fields) names << QString::fromLatin1(f.name);
    return names.join(' ');
}

} // namespace

QString Server::Client::name() const {
    return QString("%1:%2").arg(address.toString()).arg(port);
}

Server::Server(Device* device, QObject* parent) : QObject(parent), m_device(device) {
    connect(&m_socket, &QUdpSocket::readyRead, this, &Server::readPending);
    connect(m_device, &Device::fieldsChanged, this, [this](const FieldList& fields) { queue(fields); });
    connect(&m_frame, &QTimer::timeout, this, &Server::flushFrame);
    m_housekeeping.setInterval(50);
    connect(&m_housekeeping, &QTimer::timeout, this, &Server::resendAndExpire);
    m_clock.start();
}

bool Server::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (!m_socket.bind(address, port)) {
        if (error) *error = m_socket.errorString();
        return false;
    }
    m_housekeeping.start();
    m_frame.setTimerType(Qt::PreciseTimer);
    m_frame.start(std::max(1, qRound(m_device->frameIntervalMs())));
    emit log(QString("Listening on %1:%2 as \"%3\"").arg(address.toString()).arg(port).arg(m_device->productName()));
    return true;
}

void Server::close() {
    m_housekeeping.stop();
    m_socket.close();
    bool had = clientCount() > 0;
    m_clients.clear();
    if (had) emit clientCountChanged(0);
}

int Server::clientCount() const {
    int n = 0;
    for (const Client& c : m_clients) n += c.connected ? 1 : 0;
    return n;
}

QByteArray Server::header(quint8 flags, int length, quint16 session, quint16 ack, quint16 id) const {
    QByteArray h(kHeaderSize, '\0');
    setU16(h, 0, static_cast<quint16>((flags << 8) | (length & 0x07ff)));
    setU16(h, 2, session);
    setU16(h, 4, ack);
    setU16(h, 10, id);
    return h;
}

void Server::readPending() {
    while (m_socket.hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket.receiveDatagram();
        handleDatagram(datagram.data(), datagram.senderAddress(), static_cast<quint16>(datagram.senderPort()));
    }
}

void Server::handleDatagram(const QByteArray& data, const QHostAddress& from, quint16 port) {
    if (data.size() < kHeaderSize) return;
    quint8 flags = u8(data, 0) & 0xf8;
    quint16 session = u16(data, 2);
    quint16 ackId = u16(data, 4);
    quint16 resendFrom = u16(data, 6);
    quint16 id = u16(data, 10);

    if (flags & kSyn) {
        handleSyn(data, from, port, session);
        return;
    }

    QString key = QString("%1:%2").arg(from.toString()).arg(port);
    auto it = m_clients.find(key);
    if (it == m_clients.end()) return;
    Client& client = it.value();
    client.lastHeard = m_clock.elapsed();

    if (!client.connected) {
        if (flags & kAck) {             // handshake complete
            client.connected = true;
            emit log(QString("%1 connected (session %2)").arg(client.name()).arg(client.session, 4, 16, QChar('0')));
            sendDump(client);
            emit clientCountChanged(clientCount());
        }
        return;
    }

    if (flags & kAck) {
        // Acknowledgements are cumulative.
        for (auto s = client.unacked.begin(); s != client.unacked.end();) {
            if (s.key() == ackId || newer(ackId, s.key())) s = client.unacked.erase(s);
            else ++s;
        }
    }
    if (flags & kRequestResend) {
        for (auto s = client.unacked.begin(); s != client.unacked.end(); ++s)
            if (s.key() == resendFrom || newer(s.key(), resendFrom)) {
                m_socket.writeDatagram(s->packet, client.address, client.port);
                s->sentAt = m_clock.elapsed();
            }
    }
    if (!(flags & kReliable)) return;

    bool fresh = !client.haveRemoteId || newer(id, client.lastRemoteId);
    if (!fresh) {                       // a resend of something already handled
        sendAck(client, id);
        return;
    }
    client.haveRemoteId = true;
    client.lastRemoteId = id;

    // Commands: 16-bit length, 2 unused bytes, 4-character name, payload.
    QList<FieldList> perCommand;
    QStringList names;
    QByteArray payload = data.mid(kHeaderSize);
    for (int at = 0; at + 8 <= payload.size();) {
        int length = u16(payload, at);
        if (length < 8 || at + length > payload.size()) break;
        QByteArray name = payload.mid(at + 4, 4);
        QByteArray body = payload.mid(at + 8, length - 8);
        if (m_verbose) emit log(QString("%1 > %2 %3").arg(client.name(), QString::fromLatin1(name), QString::fromLatin1(body.toHex())));
        names << QString::fromLatin1(name);
        perCommand.append(m_device->handle(name, body));
        at += length;
    }
    perCommand.append(m_device->endOfPacket());
    if (!names.isEmpty()) emit commandsReceived(client.name(), names);
    // Answers for this client alone (file transfers, the lock) go at once, as
    // the switcher sends them; state answers wait for the next frame.
    FieldList now;
    for (FieldList& answers : perCommand) {
        FieldList state;
        for (const Field& f : answers) (f.toSender ? now : state).append(f);
        queue(state);
    }
    if (now.isEmpty()) {
        sendAck(client, id);
    } else {
        if (m_verbose) emit log(QString("%1 < %2").arg(client.name(), fieldSummary(now)));
        sendFields(client, now, id);
    }
}

void Server::queue(const FieldList& fields) {
    if (fields.isEmpty()) return;
    ++m_batch;
    for (const Field& f : fields) m_pending.append({ f, m_batch });
}

// The switcher answers at the next frame with its state at that moment: a
// field that several commands changed during the frame goes out once, with
// what it holds now (so a change undone within the frame is never seen).
// Answers of one command that really come in steps (a new macro's
// properties) keep their steps.
void Server::flushFrame() {
    if (m_pending.isEmpty()) return;
    FieldList out;
    for (int i = 0; i < m_pending.size(); ++i) {
        const Pending& p = m_pending[i];
        const QByteArray* now = m_device->current(p.field);
        if (!now) {                      // not state: sent as it was made
            out.append(p.field);
            continue;
        }
        const QByteArray instance = FieldStore::instanceKey(p.field);
        bool laterBatch = false, laterSame = false;
        for (int j = i + 1; j < m_pending.size(); ++j) {
            const Field& g = m_pending[j].field;
            if (g.name != p.field.name || FieldStore::instanceKey(g) != instance) continue;
            (m_pending[j].batch == p.batch ? laterSame : laterBatch) = true;
        }
        if (laterBatch) continue;
        Field f = p.field;
        if (!laterSame) f.data = *now;
        out.append(f);
    }
    m_pending.clear();
    if (out.isEmpty()) return;
    // Every state packet starts with the time code, as on the switcher.
    if (out.first().name != "Time") out.prepend(m_device->timeCode());
    if (m_verbose) emit log(QString("frame < %1").arg(fieldSummary(out)));
    broadcast(out);
}

void Server::handleSyn(const QByteArray& data, const QHostAddress& from, quint16 port, quint16 session) {
    QString key = QString("%1:%2").arg(from.toString()).arg(port);
    quint8 opcode = u8(data, kHeaderSize);

    if (opcode == 0x04) {               // disconnect
        QByteArray reply = header(kSyn, kHeaderSize + 8, session, 0, 0) + QByteArray(8, '\0');
        setU8(reply, kHeaderSize, 0x05);
        m_socket.writeDatagram(reply, from, port);
        if (m_clients.remove(key)) {
            emit log(key + " disconnected");
            emit clientCountChanged(clientCount());
        }
        return;
    }
    if (opcode != 0x01) return;

    auto it = m_clients.find(key);
    if (it == m_clients.end() || it->connected || it->clientSession != session) {
        Client c;
        c.address = from;
        c.port = port;
        c.clientSession = session;
        c.session = static_cast<quint16>(0x8000 | m_nextSession);
        m_nextSession = static_cast<quint16>((m_nextSession + 1) & 0x7fff);
        c.lastHeard = m_clock.elapsed();
        it = m_clients.insert(key, c);
    }
    QByteArray reply = header(kSyn, kHeaderSize + 8, session, 0, 0) + QByteArray(8, '\0');
    setU8(reply, kHeaderSize, 0x02);
    setU16(reply, kHeaderSize + 2, it->session & 0x7fff);
    m_socket.writeDatagram(reply, from, port);
}

void Server::sendDump(Client& client) {
    sendFields(client, m_device->connectDump());
}

void Server::sendFields(Client& client, const FieldList& fields, int ackId) {
    QByteArray payload;
    for (const Field& f : fields) {
        QByteArray encoded = encodeField(f);
        if (!payload.isEmpty() && kHeaderSize + payload.size() + encoded.size() > kMaxPacket) {
            sendReliable(client, payload, ackId);
            ackId = -1;
            payload.clear();
        }
        payload += encoded;
    }
    if (!payload.isEmpty()) sendReliable(client, payload, ackId);
}

void Server::sendReliable(Client& client, const QByteArray& payload, int ackId) {
    quint16 id = client.nextId;
    client.nextId = static_cast<quint16>((client.nextId + 1) & 0x7fff);
    quint8 flags = kReliable | (ackId >= 0 ? kAck : 0);
    QByteArray packet = header(flags, kHeaderSize + payload.size(), client.session,
                               static_cast<quint16>(ackId >= 0 ? ackId : 0), id) + payload;
    m_socket.writeDatagram(packet, client.address, client.port);
    client.unacked[id] = { packet, m_clock.elapsed(), 1 };
}

void Server::sendAck(Client& client, quint16 id) {
    m_socket.writeDatagram(header(kAck, kHeaderSize, client.session, id, 0), client.address, client.port);
}

void Server::broadcast(const FieldList& fields, Client* origin, int originAck) {
    for (Client& c : m_clients) {
        if (!c.connected) continue;
        FieldList mine;
        for (const Field& f : fields)
            if (!f.toSender || &c == origin) mine.append(f);
        int ack = &c == origin ? originAck : -1;
        // Answers for the asking client that come first (LKOB) go in a packet
        // of their own, ahead of the state they change, as on the switcher.
        int lead = 0;
        while (lead < mine.size() && mine[lead].toSender) ++lead;
        if (lead > 0 && lead < mine.size()) {
            sendFields(c, mine.mid(0, lead), ack);
            ack = -1;
            mine = mine.mid(lead);
        }
        if (!mine.isEmpty()) sendFields(c, mine, ack);
        else if (ack >= 0) sendAck(c, static_cast<quint16>(ack));
    }
}

void Server::resendAndExpire() {
    qint64 now = m_clock.elapsed();
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        Client& c = it.value();
        bool drop = now - c.lastHeard > kSilenceTimeoutMs;
        for (auto s = c.unacked.begin(); s != c.unacked.end() && !drop; ++s) {
            if (now - s->sentAt < kResendAfterMs) continue;
            if (s->tries >= kMaxTries) {
                drop = true;
                break;
            }
            QByteArray again = s->packet;
            setU8(again, 0, u8(again, 0) | kResend);
            m_socket.writeDatagram(again, c.address, c.port);
            s->sentAt = now;
            ++s->tries;
        }
        if (drop) {
            emit log(c.name() + " timed out");
            it = m_clients.erase(it);
            emit clientCountChanged(clientCount());
        } else {
            ++it;
        }
    }
}

} // namespace emu
