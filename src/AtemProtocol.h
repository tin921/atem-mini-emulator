#pragma once
#include <QtCore>

// All ATEM Mini UDP protocol constants, packet building, and parsing.
// Field byte layouts are exact captures from a real ATEM Mini (firmware 8.1.1).

namespace Atem {

// ── Flags ──────────────────────────────────────────────────────────────────
constexpr quint8  FLAG_RELIABLE    = 0x08;
constexpr quint8  FLAG_SYN         = 0x10;
constexpr quint8  FLAG_RETRANSMIT  = 0x20;
constexpr quint8  FLAG_REQ_RETRANS = 0x40;
constexpr quint8  FLAG_ACK         = 0x80;

constexpr int     HEADER_SIZE      = 12;
constexpr quint16 ATEM_PORT        = 9910;
constexpr quint16 CONTROL_SESSION  = 0x8000; // secondary SYN from SDK

// ── Well-known source IDs ──────────────────────────────────────────────────
constexpr quint16 SRC_BLACK = 0;
constexpr quint16 SRC_CAM1  = 1;
constexpr quint16 SRC_CAM2  = 2;
constexpr quint16 SRC_CAM3  = 3;
constexpr quint16 SRC_CAM4  = 4;
constexpr quint16 SRC_BARS  = 1000;

// ── Parsed header ─────────────────────────────────────────────────────────
struct Header {
    quint8  flags     = 0;
    quint16 length    = 0;
    quint16 session   = 0;
    quint16 ackId     = 0;
    quint16 unknown   = 0;
    quint16 remoteSeq = 0;
    quint16 localSeq  = 0;
};

// ── Parsed command/field ───────────────────────────────────────────────────
struct Command {
    QString    name;
    QByteArray data;
};

// ── Helpers ────────────────────────────────────────────────────────────────

inline QByteArray buildHeader(quint8 flags, quint16 length, quint16 session,
                               quint16 ackId = 0, quint16 remoteSeq = 0,
                               quint16 localSeq = 0)
{
    QByteArray out(HEADER_SIZE, '\0');
    quint8* d = reinterpret_cast<quint8*>(out.data());
    d[0]  = (flags & 0xF8) | ((length >> 8) & 0x07);
    d[1]  = length & 0xFF;
    d[2]  = (session >> 8) & 0xFF;
    d[3]  = session & 0xFF;
    d[4]  = (ackId >> 8) & 0xFF;
    d[5]  = ackId & 0xFF;
    d[6]  = 0; d[7] = 0;
    d[8]  = (remoteSeq >> 8) & 0xFF;
    d[9]  = remoteSeq & 0xFF;
    d[10] = (localSeq >> 8) & 0xFF;
    d[11] = localSeq & 0xFF;
    return out;
}

inline bool parseHeader(const QByteArray& raw, Header& hdr)
{
    if (raw.size() < HEADER_SIZE) return false;
    const quint8* d = reinterpret_cast<const quint8*>(raw.constData());
    hdr.flags     = d[0] & 0xF8;
    hdr.length    = ((quint16)(d[0] & 0x07) << 8) | d[1];
    hdr.session   = ((quint16)d[2] << 8) | d[3];
    hdr.ackId     = ((quint16)d[4] << 8) | d[5];
    hdr.unknown   = ((quint16)d[6] << 8) | d[7];
    hdr.remoteSeq = ((quint16)d[8] << 8) | d[9];
    hdr.localSeq  = ((quint16)d[10] << 8) | d[11];
    return true;
}

// Build a single ATEM field: length(2) + pad(2) + name(4) + data
inline QByteArray buildField(const char* name, const QByteArray& data)
{
    quint16 total = 8 + (quint16)data.size();
    QByteArray out;
    out.resize(8);
    quint8* d = reinterpret_cast<quint8*>(out.data());
    d[0] = (total >> 8) & 0xFF;
    d[1] = total & 0xFF;
    d[2] = 0; d[3] = 0;
    d[4] = (quint8)name[0]; d[5] = (quint8)name[1];
    d[6] = (quint8)name[2]; d[7] = (quint8)name[3];
    out += data;
    return out;
}

inline QByteArray buildFieldHex(const char* name, const char* hex)
{
    return buildField(name, QByteArray::fromHex(hex));
}

inline QVector<Command> parseCommands(const QByteArray& payload)
{
    QVector<Command> cmds;
    int offset = 0;
    while (offset + 8 <= payload.size()) {
        const quint8* d = reinterpret_cast<const quint8*>(payload.constData()) + offset;
        int len = ((int)d[0] << 8) | d[1];
        if (len < 8 || offset + len > payload.size()) break;
        Command cmd;
        cmd.name = QString::fromLatin1(payload.mid(offset + 4, 4));
        cmd.data = payload.mid(offset + 8, len - 8);
        cmds.append(cmd);
        offset += len;
    }
    return cmds;
}

// Big-endian write helpers
inline void writeU16BE(quint8* buf, quint16 v) {
    buf[0] = (v >> 8) & 0xFF; buf[1] = v & 0xFF;
}
inline void writeU32BE(quint8* buf, quint32 v) {
    buf[0] = (v >> 24) & 0xFF; buf[1] = (v >> 16) & 0xFF;
    buf[2] = (v >> 8)  & 0xFF; buf[3] =  v        & 0xFF;
}
inline void writeI32BE(quint8* buf, qint32 v) {
    writeU32BE(buf, (quint32)v);
}
inline quint16 readU16BE(const quint8* buf) {
    return ((quint16)buf[0] << 8) | buf[1];
}
inline qint16 readI16BE(const quint8* buf) {
    return (qint16)(((quint16)buf[0] << 8) | buf[1]);
}

} // namespace Atem
