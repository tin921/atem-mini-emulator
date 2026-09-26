#include "wireproxy.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QUdpSocket>
#include <map>
#include <memory>

// ── Packet parsing ───────────────────────────────────────────

namespace {
quint16 be16(const QByteArray& d, int at) {
    return static_cast<quint16>((static_cast<quint8>(d[at]) << 8) | static_cast<quint8>(d[at + 1]));
}
constexpr int kHeader = 12;
constexpr quint8 kFlagSyn = 0x10;
} // namespace

quint8 WirePacket::flags() const { return data.size() ? static_cast<quint8>(data[0]) & 0xF8 : 0; }
quint16 WirePacket::session() const { return data.size() >= 4 ? be16(data, 2) : 0; }
quint16 WirePacket::ackId() const { return data.size() >= 6 ? be16(data, 4) : 0; }
quint16 WirePacket::packetId() const { return data.size() >= 12 ? be16(data, 10) : 0; }

std::vector<AtemField> WirePacket::fields() const {
    std::vector<AtemField> out;
    if (data.size() <= kHeader || (flags() & kFlagSyn)) return out;
    int at = kHeader;
    while (at + 8 <= data.size()) {
        int len = be16(data, at);
        if (len < 8 || at + len > data.size()) break;
        out.push_back({ QString::fromLatin1(data.mid(at + 4, 4)), data.mid(at + 8, len - 8) });
        at += len;
    }
    return out;
}

// ── Proxy ────────────────────────────────────────────────────

WireProxy::WireProxy(const QHostAddress& listen, const QHostAddress& device, quint16 port)
    : m_listen(listen), m_device(device), m_port(port) {}

WireProxy::~WireProxy() { stopProxy(); }

bool WireProxy::startProxy(QString* error) {
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    start();
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_startMutex);
            if (m_started) break;
        }
        QThread::msleep(5);
    }
    if (!m_ok && error) *error = m_error;
    return m_ok;
}

void WireProxy::stopProxy() {
    if (isRunning()) {
        quit();
        wait(2000);
    }
}

void WireProxy::run() {
    QUdpSocket listener;
    bool ok = listener.bind(m_listen, m_port);
    {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_ok = ok;
        m_error = ok ? QString() : listener.errorString();
        m_started = true;
    }
    if (!ok) return;

    // One upstream socket per SDK-side port, so parallel connections stay apart.
    struct Client {
        QHostAddress addr;
        quint16 port;
        std::unique_ptr<QUdpSocket> up;
    };
    std::map<quint16, Client> clients;

    QObject::connect(&listener, &QUdpSocket::readyRead, &listener, [&]() {
        while (listener.hasPendingDatagrams()) {
            QByteArray data(static_cast<int>(listener.pendingDatagramSize()), '\0');
            QHostAddress from;
            quint16 fromPort = 0;
            listener.readDatagram(data.data(), data.size(), &from, &fromPort);

            auto it = clients.find(fromPort);
            if (it == clients.end()) {
                Client c{ from, fromPort, std::make_unique<QUdpSocket>() };
                c.up->bind(QHostAddress::AnyIPv4, 0);
                QUdpSocket* up = c.up.get();
                QObject::connect(up, &QUdpSocket::readyRead, up, [this, up, &listener, from, fromPort]() {
                    while (up->hasPendingDatagrams()) {
                        QByteArray reply(static_cast<int>(up->pendingDatagramSize()), '\0');
                        up->readDatagram(reply.data(), reply.size());
                        record(false, fromPort, reply);
                        listener.writeDatagram(reply, from, fromPort);
                    }
                });
                it = clients.emplace(fromPort, std::move(c)).first;
            }
            record(true, fromPort, data);
            it->second.up->writeDatagram(data, m_device, m_port);
        }
    });

    exec();
}

void WireProxy::record(bool toDevice, quint16 client, const QByteArray& data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_packets.push_back({ QDateTime::currentMSecsSinceEpoch() - m_startMs, toDevice, client, data });
}

size_t WireProxy::mark() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_packets.size();
}

std::vector<WirePacket> WireProxy::since(size_t from) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (from >= m_packets.size()) return {};
    return std::vector<WirePacket>(m_packets.begin() + static_cast<ptrdiff_t>(from), m_packets.end());
}

QJsonObject WireProxy::fieldsSince(size_t from) const {
    QJsonArray tx, rx;
    auto packets = since(from);
    for (const auto& p : packets) {
        for (const auto& f : p.fields()) {
            QJsonObject o{ { "field", f.name }, { "hex", QString::fromLatin1(f.data.toHex()) } };
            (p.toDevice ? tx : rx).append(o);
        }
    }
    return QJsonObject{ { "tx", tx }, { "rx", rx }, { "packets", static_cast<int>(packets.size()) } };
}

bool WireProxy::writeLog(const QString& path) const {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    for (const auto& p : since(0)) {
        QJsonArray fields;
        for (const auto& fl : p.fields())
            fields.append(QJsonObject{ { "field", fl.name }, { "hex", QString::fromLatin1(fl.data.toHex()) } });
        QJsonObject o{
            { "ms", p.ms },
            { "dir", p.toDevice ? "tx" : "rx" },
            { "client", p.client },
            { "flags", QString("0x%1").arg(p.flags(), 2, 16, QLatin1Char('0')) },
            { "session", QString("0x%1").arg(p.session(), 4, 16, QLatin1Char('0')) },
            { "ack", p.ackId() },
            { "id", p.packetId() },
            { "hex", QString::fromLatin1(p.data.toHex()) },
            { "fields", fields },
        };
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.write("\n");
    }
    return true;
}
