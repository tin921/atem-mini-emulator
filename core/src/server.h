// The ATEM UDP transport (port 9910), as captured from the real ATEM Mini:
//
//   client  SYN  (flag 0x10, payload 01)          session = client's pick
//   switch  SYN  (flag 0x10, payload 02 00 SS SS)  SS SS = new session & 0x7fff
//   client  ACK  (flag 0x80)
//   switch  connect dump, reliable packets 1..n (session 0x8000 | SS SS), ends with InCm
//
// Afterwards both sides send reliable packets (flag 0x08, 15-bit packet id in
// bytes 10-11) and acknowledge them (flag 0x80, acked id in bytes 4-5); an
// acknowledgement can ride on a reliable packet (0x88). A SYN with payload
// 04 disconnects and is answered with 05. Unacknowledged packets are resent
// with flag 0x20.
#pragma once

#include "fields.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

namespace emu {

class Device;

class Server : public QObject {
    Q_OBJECT
public:
    Server(Device* device, QObject* parent = nullptr);

    bool listen(const QHostAddress& address, quint16 port, QString* error);
    void close();                       // drops every client and stops listening
    bool isListening() const { return m_socket.state() == QAbstractSocket::BoundState; }
    int clientCount() const;            // connected clients
    void setVerbose(bool verbose) { m_verbose = verbose; }

signals:
    void log(const QString& message);
    void clientCountChanged(int connected);
    void commandsReceived(const QString& client, const QStringList& commands);   // one packet

private:
    struct Sent {
        QByteArray packet;
        qint64 sentAt = 0;
        int tries = 0;
    };
    struct Client {
        QHostAddress address;
        quint16 port = 0;
        quint16 clientSession = 0;   // the session id from the client's SYN
        quint16 session = 0;         // ours: 0x8000 | n
        bool connected = false;      // handshake done, dump sent
        quint16 nextId = 1;          // our next reliable packet id
        bool haveRemoteId = false;
        quint16 lastRemoteId = 0;    // last reliable packet id we processed
        QMap<quint16, Sent> unacked;
        qint64 lastHeard = 0;
        QString name() const;
    };

    void readPending();
    void handleDatagram(const QByteArray& data, const QHostAddress& from, quint16 port);
    void handleSyn(const QByteArray& data, const QHostAddress& from, quint16 port, quint16 session);
    void sendDump(Client& client);
    // Sends fields as reliable packets. ackId >= 0 piggybacks an acknowledgement.
    void sendFields(Client& client, const FieldList& fields, int ackId = -1);
    void sendReliable(Client& client, const QByteArray& payload, int ackId);
    void sendAck(Client& client, quint16 id);
    void broadcast(const FieldList& fields, Client* origin = nullptr, int originAck = -1);
    void resendAndExpire();
    // State answers go out once per video frame, as on the switcher.
    void queue(const FieldList& fields);
    void flushFrame();
    QByteArray header(quint8 flags, int length, quint16 session, quint16 ack, quint16 id) const;

    Device* m_device;
    QUdpSocket m_socket;
    QTimer m_housekeeping;
    QTimer m_frame;
    struct Pending {
        Field field;
        quint64 batch;   // one command's answers (or one device update)
    };
    QList<Pending> m_pending;
    quint64 m_batch = 0;
    QElapsedTimer m_clock;
    QMap<QString, Client> m_clients;   // "address:port"
    quint16 m_nextSession = 0x28;
    bool m_verbose = false;
};

} // namespace emu
