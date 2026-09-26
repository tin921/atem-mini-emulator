#pragma once
// UDP recording proxy: the SDK connects to listen:9910, the proxy forwards to
// device:9910 and keeps every datagram (both directions, timestamped).
// Each packet's payload is split into ATEM fields (len, pad, 4CC, data), so a
// test can report which commands it sent and which state fields came back.

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <atomic>
#include <mutex>
#include <vector>

struct AtemField {
    QString name;       // four-character code, e.g. "CPgI", "PrgI"
    QByteArray data;    // field data after the 8-byte field header
};

struct WirePacket {
    qint64 ms;          // since the proxy started
    bool toDevice;      // true: SDK -> ATEM
    quint16 client;     // SDK-side UDP port (tells connections apart)
    QByteArray data;    // whole datagram

    quint8 flags() const;       // top 5 bits of byte 0
    quint16 session() const;
    quint16 ackId() const;
    quint16 packetId() const;
    std::vector<AtemField> fields() const;
};

class WireProxy : public QThread {
public:
    WireProxy(const QHostAddress& listen, const QHostAddress& device, quint16 port = 9910);
    ~WireProxy() override;

    bool startProxy(QString* error);   // returns once the socket is bound
    void stopProxy();

    size_t mark() const;
    std::vector<WirePacket> since(size_t from) const;
    // {"tx":[{"field":"CPgI","hex":"..."}], "rx":[...], "packets":n}
    QJsonObject fieldsSince(size_t from) const;
    bool writeLog(const QString& path) const;

protected:
    void run() override;

private:
    void record(bool toDevice, quint16 client, const QByteArray& data);

    QHostAddress m_listen;
    QHostAddress m_device;
    quint16 m_port;

    mutable std::mutex m_mutex;
    std::vector<WirePacket> m_packets;
    qint64 m_startMs = 0;

    std::mutex m_startMutex;
    bool m_started = false;
    bool m_ok = false;
    QString m_error;
};
