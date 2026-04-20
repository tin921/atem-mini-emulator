#include "AtemServer.h"
#include <QtCore>

namespace Atem {

AtemServer::AtemServer(ATEMState* state, quint16 port, QObject* parent)
    : QObject(parent), m_state(state), m_port(port)
{}

bool AtemServer::start()
{
    m_socket = new QUdpSocket(this);
    m_socket->setSocketOption(QAbstractSocket::SocketOption::LowDelayOption, 1);
    if (!m_socket->bind(QHostAddress::AnyIPv4, m_port,
                        QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        emit logMessage(QString("Failed to bind UDP port %1: %2")
                        .arg(m_port).arg(m_socket->errorString()));
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &AtemServer::onReadyRead);

    m_keepalive = new QTimer(this);
    m_keepalive->setInterval(1000);
    connect(m_keepalive, &QTimer::timeout, this, &AtemServer::onKeepalive);
    m_keepalive->start();

    emit logMessage(QString("Listening on UDP 0.0.0.0:%1").arg(m_port));
    return true;
}

void AtemServer::stop()
{
    if (m_keepalive) m_keepalive->stop();
    if (m_socket)    m_socket->close();
    m_clients.clear();
}

// ── Broadcast helpers ──────────────────────────────────────────────────────

void AtemServer::broadcastField(const QByteArray& field)
{
    for (auto& c : m_clients) {
        if (c.state == Connected)
            sendPacket(c, FLAG_RELIABLE, field);
    }
}

void AtemServer::broadcastPrgI()  { broadcastField(m_state->fieldPrgI()); }
void AtemServer::broadcastPrvI()  { broadcastField(m_state->fieldPrvI()); }
void AtemServer::broadcastKeOn()  { broadcastField(m_state->fieldKeOn()); }
void AtemServer::broadcastKeDV()  { broadcastField(m_state->fieldKeDV()); }
void AtemServer::broadcastMRPr()  { broadcastField(m_state->fieldMRPr()); }
void AtemServer::broadcastTally() {
    broadcastField(m_state->fieldTlIn());
    broadcastField(m_state->fieldTlSr());
}
void AtemServer::broadcastMPrp(int i) { broadcastField(m_state->fieldMPrp(i)); }

// ── Packet receive ─────────────────────────────────────────────────────────

void AtemServer::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray data;
        QHostAddress addr;
        quint16 port;
        data.resize((int)m_socket->pendingDatagramSize());
        m_socket->readDatagram(data.data(), data.size(), &addr, &port);
        handlePacket(data, addr, port);
    }
}

void AtemServer::handlePacket(const QByteArray& data, const QHostAddress& addr, quint16 port)
{
    Header hdr;
    if (!parseHeader(data, hdr)) return;
    QByteArray payload = data.mid(HEADER_SIZE);

    emit logMessage(QString("<< %1:%2 [%3b] flags=0x%4 sess=0x%5")
                    .arg(addr.toString()).arg(port).arg(data.size())
                    .arg(hdr.flags, 2, 16, QLatin1Char('0'))
                    .arg(hdr.session, 4, 16, QLatin1Char('0')));

    if (hdr.flags & FLAG_SYN) {
        handleSyn(hdr, payload, addr, port);
        return;
    }

    auto it = m_clients.find(hdr.session);
    if (it == m_clients.end()) return;
    Client& c = it.value();
    c.lastContact = QDateTime::currentMSecsSinceEpoch();

    if ((hdr.flags & FLAG_ACK) && c.state == Handshake2) {
        c.state = Connected;
        emit logMessage(QString("Secondary channel ready (sess 0x%1)")
                        .arg(c.session, 4, 16, QLatin1Char('0')));
        return;
    }

    if ((hdr.flags & FLAG_ACK) && c.state == Handshake) {
        c.state = Connected;
        emit logMessage(QString("Handshake complete, sending state dump to %1:%2")
                        .arg(c.addr.toString()).arg(c.port));
        sendStateDump(c);
        return;
    }

    if ((hdr.flags & FLAG_ACK) && c.state == Connected) {
        // keepalive ACK or dump ACK — nothing to do
        return;
    }

    if ((hdr.flags & FLAG_RELIABLE) && c.state == Connected) {
        handleReliable(c, hdr, payload);
    }
}

void AtemServer::handleSyn(const Header& hdr, const QByteArray& /*payload*/,
                           const QHostAddress& addr, quint16 port)
{
    // Secondary control-channel SYN (session=0x8000) — no state dump
    if (hdr.session == CONTROL_SESSION) {
        emit logMessage("Secondary SYN (0x8000) — no dump");
        QByteArray synPayload(8, '\0');
        synPayload[0] = 0x02;
        quint16 pktLen = HEADER_SIZE + synPayload.size();
        QByteArray pkt = buildHeader(FLAG_SYN, pktLen, CONTROL_SESSION, hdr.remoteSeq)
                       + synPayload;
        m_socket->writeDatagram(pkt, addr, port);

        Client c2;
        c2.session     = CONTROL_SESSION;
        c2.addr        = addr;
        c2.port        = port;
        c2.state       = Handshake2;
        c2.lastContact = QDateTime::currentMSecsSinceEpoch();
        m_clients[CONTROL_SESSION] = c2;
        return;
    }

    // SYN retransmit — re-send same SYN-ACK if we already have a pending entry for this addr
    if (hdr.flags & FLAG_RETRANSMIT) {
        for (auto& c : m_clients) {
            if (c.addr == addr && c.port == port && c.state == Handshake) {
                QByteArray synPayload(8, '\0'); synPayload[0] = 0x02;
                quint16 pktLen = HEADER_SIZE + synPayload.size();
                QByteArray pkt = buildHeader(FLAG_SYN, pktLen, c.session, hdr.remoteSeq)
                               + synPayload;
                m_socket->writeDatagram(pkt, addr, port);
                emit logMessage(QString("SYN retransmit -> re-sent SYN-ACK sess=0x%1")
                                .arg(c.session, 4, 16, QLatin1Char('0')));
                return;
            }
        }
    }

    // New connection — assign server session with high bit set
    quint16 serverSession = 0x8000 | (m_sessionCounter & 0x7FFF);
    m_sessionCounter = (m_sessionCounter + 1) & 0x7FFF;
    if (m_sessionCounter == 0) m_sessionCounter = 1;

    emit logMessage(QString("SYN from %1:%2 (client 0x%3) -> server sess 0x%4")
                    .arg(addr.toString()).arg(port)
                    .arg(hdr.session, 4, 16, QLatin1Char('0'))
                    .arg(serverSession, 4, 16, QLatin1Char('0')));

    QByteArray synPayload(8, '\0');
    synPayload[0] = 0x02;
    quint16 pktLen = HEADER_SIZE + synPayload.size();
    // Echo client's remoteSeq as ackId so SDK accepts the SYN-ACK immediately
    QByteArray pkt = buildHeader(FLAG_SYN, pktLen, serverSession, hdr.remoteSeq)
                   + synPayload;
    m_socket->writeDatagram(pkt, addr, port);

    Client c;
    c.session     = serverSession;
    c.addr        = addr;
    c.port        = port;
    c.state       = Handshake;
    c.localSeq    = 0;
    c.lastContact = QDateTime::currentMSecsSinceEpoch();
    m_clients[serverSession] = c;
}

void AtemServer::handleReliable(Client& c, const Header& hdr, const QByteArray& payload)
{
    // ACK the client's packet
    sendAck(c, hdr.localSeq);

    if (payload.isEmpty()) return;

    for (const auto& cmd : parseCommands(payload)) {
        emit logMessage(QString("  CMD '%1' (%2 bytes)").arg(cmd.name).arg(cmd.data.size()));
        dispatchCommand(cmd.name, cmd.data);
    }
}

void AtemServer::dispatchCommand(const QString& name, const QByteArray& d)
{
    if (name == "CPgI" && d.size() >= 4) {
        quint16 src = readU16BE(reinterpret_cast<const quint8*>(d.constData()) + 2);
        emit cmdProgramInput(src);
    } else if (name == "CPvI" && d.size() >= 4) {
        quint16 src = readU16BE(reinterpret_cast<const quint8*>(d.constData()) + 2);
        emit cmdPreviewInput(src);
    } else if (name == "DCut") {
        emit cmdCut();
    } else if (name == "DAut") {
        emit cmdAuto();
    } else if (name == "CKeO" && d.size() >= 4) {
        // CKeO: ME(1) keyer(1) on_air(1) pad(1)
        bool on = (quint8)d[2] != 0;
        emit cmdKeyerOn(on);
    } else if (name == "CDvP" && d.size() >= 20) {
        // CDvP: ME(1) keyer(1) flags(2) sizeX(4) sizeY(4) posX(4) posY(4) ...
        const quint8* p = reinterpret_cast<const quint8*>(d.constData());
        quint16 fillSrc = readU16BE(p + 2);
        quint32 sX = (((quint32)p[4]<<24)|((quint32)p[5]<<16)|((quint32)p[6]<<8)|p[7]);
        quint32 sY = (((quint32)p[8]<<24)|((quint32)p[9]<<16)|((quint32)p[10]<<8)|p[11]);
        qint32  pX = (qint32)(((quint32)p[12]<<24)|((quint32)p[13]<<16)|((quint32)p[14]<<8)|p[15]);
        qint32  pY = (qint32)(((quint32)p[16]<<24)|((quint32)p[17]<<16)|((quint32)p[18]<<8)|p[19]);
        emit cmdKeyerDVE(fillSrc, sX, sY, pX, pY);
    } else if (name == "MSRc" && d.size() >= 4) {
        quint16 idx = readU16BE(reinterpret_cast<const quint8*>(d.constData()));
        quint8  act = (quint8)d[2];
        if (act == 0) emit cmdMacroRun(idx);
        else if (act == 1) emit cmdMacroStop();
    }
}

// ── Sending ────────────────────────────────────────────────────────────────

void AtemServer::sendStateDump(Client& c)
{
    auto packets = m_state->buildStateDump();
    quint16 seq = 1;
    for (const auto& payload : packets) {
        quint16 pktLen = HEADER_SIZE + payload.size();
        QByteArray pkt = buildHeader(FLAG_RELIABLE | FLAG_ACK, pktLen,
                                     c.session, 0, 0, seq)
                       + payload;
        m_socket->writeDatagram(pkt, c.addr, c.port);
        ++seq;
    }
    // Empty terminal packet
    quint16 pktLen = HEADER_SIZE;
    QByteArray term = buildHeader(FLAG_RELIABLE | FLAG_ACK, pktLen,
                                  c.session, 0, 0, seq);
    m_socket->writeDatagram(term, c.addr, c.port);
    c.localSeq = seq;
    emit logMessage(QString("State dump sent (%1 packets) to %2:%3")
                    .arg(packets.size()).arg(c.addr.toString()).arg(c.port));
    emit clientConnected(m_clients.size());
}

void AtemServer::sendPacket(Client& c, quint8 flags, const QByteArray& payload)
{
    ++c.localSeq;
    quint16 pktLen = HEADER_SIZE + payload.size();
    QByteArray pkt = buildHeader(flags, pktLen, c.session, 0, 0, c.localSeq)
                   + payload;
    m_socket->writeDatagram(pkt, c.addr, c.port);
}

void AtemServer::sendAck(Client& c, quint16 ackNum)
{
    QByteArray pkt = buildHeader(FLAG_ACK, HEADER_SIZE, c.session, ackNum);
    m_socket->writeDatagram(pkt, c.addr, c.port);
}

// ── Keepalive ──────────────────────────────────────────────────────────────

void AtemServer::onKeepalive()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<quint16> timedOut;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        Client& c = it.value();
        if (c.state != Connected) continue;
        if (now - c.lastContact > 10000) {
            emit logMessage(QString("Client %1:%2 timed out")
                            .arg(c.addr.toString()).arg(c.port));
            timedOut.append(c.session);
            continue;
        }
        sendPacket(c, FLAG_RELIABLE);
    }
    for (quint16 s : timedOut) {
        m_clients.remove(s);
        emit clientDisconnected(m_clients.size());
    }
}

} // namespace Atem
