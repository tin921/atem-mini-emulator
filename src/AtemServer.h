#pragma once
#include "AtemProtocol.h"
#include "AtemState.h"
#include <QtCore>
#include <QtNetwork>

namespace Atem {

class AtemServer : public QObject
{
    Q_OBJECT
public:
    explicit AtemServer(ATEMState* state, quint16 port = ATEM_PORT,
                        QObject* parent = nullptr);

    bool start();
    void stop();
    int  clientCount() const { return m_clients.size(); }

    // Broadcast a single updated field to all connected clients.
    // Call after modifying state to notify SDK / ATEM Software Control.
    void broadcastField(const QByteArray& field);

    // Convenience helpers that build the field then broadcast
    void broadcastPrgI();
    void broadcastPrvI();
    void broadcastKeOn();
    void broadcastKeDV();
    void broadcastMRPr();
    void broadcastMPrp(int index);
    void broadcastTally();

signals:
    void clientConnected(int total);
    void clientDisconnected(int total);
    void logMessage(const QString& msg);

    // Emitted when the SDK sends a recognised command
    void cmdProgramInput(quint16 source);
    void cmdPreviewInput(quint16 source);
    void cmdCut();
    void cmdAuto();
    void cmdKeyerOn(bool on);
    void cmdKeyerDVE(quint16 fillSrc, quint32 sizeX, quint32 sizeY,
                     qint32 posX, qint32 posY);
    void cmdMacroRun(quint16 index);
    void cmdMacroStop();

private slots:
    void onReadyRead();
    void onKeepalive();

private:
    enum ClientState { Handshake, Handshake2, Connected };

    struct Client {
        quint16       session    = 0;
        QHostAddress  addr;
        quint16       port       = 0;
        ClientState   state      = Handshake;
        quint16       localSeq   = 0;   // server→client sequence counter
        qint64        lastContact = 0;  // msecsSinceEpoch
    };

    void handlePacket(const QByteArray& data, const QHostAddress& addr, quint16 port);
    void handleSyn(const Header& hdr, const QByteArray& payload,
                   const QHostAddress& addr, quint16 port);
    void handleAck(Client& c, const Header& hdr);
    void handleReliable(Client& c, const Header& hdr, const QByteArray& payload);
    void sendStateDump(Client& c);
    void sendPacket(Client& c, quint8 flags, const QByteArray& payload = {});
    void sendAck(Client& c, quint16 ackNum);
    void dispatchCommand(const QString& name, const QByteArray& data);

    QUdpSocket*          m_socket      = nullptr;
    QTimer*              m_keepalive   = nullptr;
    ATEMState*           m_state       = nullptr;
    quint16              m_port;
    quint16              m_sessionCounter = 0x0001;
    QMap<quint16, Client> m_clients;
};

} // namespace Atem
