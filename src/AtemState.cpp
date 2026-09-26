#include "AtemState.h"
#include <QtCore>

namespace Atem {

using namespace Atem;

// ── Dynamic field builders ─────────────────────────────────────────────────

QByteArray ATEMState::fieldPrgI() const
{
    // PrgI: 4 bytes — ME(1) + pad(1) + source(2)
    QByteArray d(4, '\0');
    writeU16BE(reinterpret_cast<quint8*>(d.data()) + 2, programSource);
    return buildField("PrgI", d);
}

QByteArray ATEMState::fieldPrvI() const
{
    // PrvI: 8 bytes — ME(1) + pad(1) + source(2) + 4 zeros (real device: 0000000100000000)
    QByteArray d(8, '\0');
    writeU16BE(reinterpret_cast<quint8*>(d.data()) + 2, previewSource);
    return buildField("PrvI", d);
}

QByteArray ATEMState::fieldKeOn() const
{
    // KeOn: 4 bytes — ME(1) + keyer(1) + on_air(1) + pad(1)
    QByteArray d(4, '\0');
    d[2] = keyerOn ? 1 : 0;
    return buildField("KeOn", d);
}

QByteArray ATEMState::fieldKeDV() const
{
    // KeDV: 60 bytes. Real device base from capture; patch sizeX/Y, posX/Y, fillSrc.
    // Layout (0-indexed in field data, after 8-byte field header):
    //   [0-1]  ME + keyer index  = 0x0000
    //   [2-3]  fill source       (uint16)
    //   [4-7]  sizeX             (uint32, 1000 = 100%)
    //   [8-11] sizeY             (uint32)
    //   [12-15] posX             (int32)
    //   [16-19] posY             (int32)
    //   [20-59] fixed bytes from real device (border, shadow, rotation...)
    QByteArray d = QByteArray::fromHex(
        "0000"                              // ME + keyer
        "0002"                              // fill source placeholder
        "000003e8"                          // sizeX placeholder
        "000003e8"                          // sizeY placeholder
        "00000000"                          // posX placeholder
        "00000000"                          // posY placeholder
        // remaining 40 bytes from real device capture
        "000000000000000000000000000000000000000000"
        "740000000000000168190000000000000000001900"
        "0100"
    );
    quint8* p = reinterpret_cast<quint8*>(d.data());
    writeU16BE(p + 2,  dve.fillSrc);
    writeU32BE(p + 4,  dve.sizeX);
    writeU32BE(p + 8,  dve.sizeY);
    writeI32BE(p + 12, dve.posX);
    writeI32BE(p + 16, dve.posY);
    return buildField("KeDV", d);
}

QByteArray ATEMState::fieldMPrp(int i) const
{
    const MacroDef& m = macros.at(i);
    // MPrp: index(2) + isUsed(1) + hasUnsupported(1) + nameLen(2) + descLen(2) + name + desc + pad4
    QByteArray name = m.name.toUtf8().left(63);
    QByteArray desc = m.description.toUtf8().left(255);
    QByteArray d;
    d.resize(8);
    quint8* p = reinterpret_cast<quint8*>(d.data());
    writeU16BE(p,     (quint16)i);
    p[2] = m.isUsed ? 1 : 0;
    p[3] = 0;
    writeU16BE(p + 4, (quint16)name.size());
    writeU16BE(p + 6, (quint16)desc.size());
    d += name;
    d += desc;
    while (d.size() % 4 != 0) d += '\0';
    return buildField("MPrp", d);
}

QByteArray ATEMState::fieldMRPr() const
{
    // MRPr: 8 bytes — running(1) + waiting(1) + loop(1) + pad(1) + index(2) + pad(2)
    QByteArray d(8, '\0');
    d[0] = macroRun.running ? 1 : 0;
    d[1] = macroRun.waiting ? 1 : 0;
    d[2] = 0; // loop
    d[3] = 0;
    writeU16BE(reinterpret_cast<quint8*>(d.data()) + 4, macroRun.index);
    return buildField("MRPr", d);
}

QByteArray ATEMState::fieldTlIn() const
{
    // TlIn: count(2) + per-source tally(1) per source in topology order
    // Sources in order: 0(Black) 1 2 3 4 1000 2001 2002 3010 3011 10010 10011 11001 8001
    static const quint16 srcOrder[] = {0,1,2,3,4,1000,2001,2002,3010,3011,10010,10011,11001,8001};
    constexpr int N = 14;
    QByteArray d;
    d.resize(2 + N);
    quint8* p = reinterpret_cast<quint8*>(d.data());
    writeU16BE(p, N);
    for (int i = 0; i < N; ++i) {
        quint8 flags = 0;
        if (srcOrder[i] == programSource) flags |= 0x01; // on program
        if (srcOrder[i] == previewSource) flags |= 0x02; // on preview
        p[2 + i] = flags;
    }
    // pad to 4-byte boundary
    while (d.size() % 4 != 0) d += '\0';
    return buildField("TlIn", d);
}

QByteArray ATEMState::fieldTlSr() const
{
    // TlSr: count(2) + per-entry: source(2) + tally(1) + pad(1)
    // Real device format from capture
    static const quint16 srcOrder[] = {0,1,2,3,4,1000,2001,2002,3010,3011,10010,10011,11001,8001};
    constexpr int N = 14;
    QByteArray d;
    d.resize(2);
    quint8 cnt[2]; writeU16BE(cnt, N);
    d[0] = cnt[0]; d[1] = cnt[1];
    for (int i = 0; i < N; ++i) {
        quint8 entry[4] = {0,0,0,0};
        writeU16BE(entry, srcOrder[i]);
        quint8 flags = 0;
        if (srcOrder[i] == programSource) flags |= 0x01;
        if (srcOrder[i] == previewSource) flags |= 0x02;
        entry[2] = flags;
        d += QByteArray(reinterpret_cast<char*>(entry), 4);
    }
    return buildField("TlSr", d);
}

// ── State dump ─────────────────────────────────────────────────────────────

QVector<QByteArray> ATEMState::buildStateDump() const
{
    QByteArray all;

    // Minimal state dump matching run.py — only fields the BMD SDK requires.
    // Field order and content must match exactly; extra fields cause StateSync failures.

    // Protocol v2.28 (firmware 8.0) — SDK rejects v2.30+ from a Mini
    all += buildFieldHex("_ver", "0002001c");

    {
        // _pin: 44-byte null-padded name + model byte 0x01 (ATEM Mini base) + 3 pad
        QByteArray name = QByteArray("ATEM Mini").leftJustified(44, '\0');
        all += buildField("_pin", name + QByteArray::fromHex("01000000"));
    }

    {
        // _top: 32 bytes (4-byte aligned) — topology matching run.py
        static const quint8 topBytes[32] = {
            1, 8, 2, 1, 0, 1, 1, 0,  // M/E, sources, DSK, AUX, MixMinus, MPs, MVs, rs485
            0, 1, 1, 0, 0, 0, 0, 1,  // HyperDecks, DVE, stingers, SS, unk×3, scalers
            0, 0, 1, 0, 0, 0, 1, 1,  // unk×2, camera ctrl, unk×3, adv chroma, cfg outputs
            1, 0x20, 3, 0xe8, 0, 0, 0, 0  // unk, ATEM Mini bytes, extra padding
        };
        all += buildField("_top", QByteArray(reinterpret_cast<const char*>(topBytes), 32));
    }

    all += buildFieldHex("_MeC", "00010000");  // M/E 0, 1 keyer
    all += buildFieldHex("_mpl", "14000000");  // 20 stills, 0 clips
    all += buildFieldHex("VidM", "31307073");  // bmdSwitcherVideoMode1080p60
    all += fieldPrgI();
    all += fieldPrvI();
    all += buildFieldHex("_MAC", "64000000");  // 100 macro slots

    // -- Macro pool (100 slots) --
    for (int i = 0; i < 100; ++i)
        all += fieldMPrp(i);
    all += fieldMRPr();

    // -- InCm: end of dump --
    all += buildFieldHex("InCm", "00000000");

    // Pack into ≤900-byte payload chunks
    QVector<QByteArray> packets;
    QByteArray cur;
    for (int offset = 0; offset < all.size(); ) {
        // Find next field boundary
        const quint8* p = reinterpret_cast<const quint8*>(all.constData()) + offset;
        int flen = ((int)p[0] << 8) | p[1];
        if (flen < 8) break;
        if (!cur.isEmpty() && cur.size() + flen > 900) {
            packets.append(cur);
            cur.clear();
        }
        cur += all.mid(offset, flen);
        offset += flen;
    }
    if (!cur.isEmpty()) packets.append(cur);
    return packets;
}

} // namespace Atem
