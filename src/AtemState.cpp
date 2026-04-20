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
    // MRPr: 4 bytes — is_running(1) + is_waiting(1) + index(2)
    QByteArray d(4, '\0');
    d[0] = macroRun.running ? 1 : 0;
    d[1] = macroRun.waiting ? 1 : 0;
    writeU16BE(reinterpret_cast<quint8*>(d.data()) + 2, macroRun.index);
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

    // -- Group 1: version, product, topology, pool config --
    all += buildFieldHex("_ver", "0002001e");
    {
        // _pin: 44 bytes — "ATEM Mini" padded to 40 + model 0x0d + 3 zeros
        QByteArray name = QByteArray("ATEM Mini").leftJustified(40, '\0');
        QByteArray pin = name + QByteArray::fromHex("0d000000");
        all += buildField("_pin", pin);
    }
    all += buildFieldHex("_top", "010e0101000100000401000000000001000001000000010101000000");
    all += buildFieldHex("_MeC", "00010000");
    all += buildFieldHex("_mpl", "14000100");

    // -- Group 2: capability tables --
    all += buildFieldHex("_FAC", "06000000");
    all += buildFieldHex("_FEC", "00040000010000000000001e0000018b0200000000000064000005c804000000000001c200001ee60800000000000578000054c4");
    all += buildFieldHex("_VMC", "0008000008000000000000000000000000090000000000000000000000000a0000000000000000000000000b0000000000000000000000001a0000000000000000000000000c0000000000000000000000000d0000000000000000000000001b000000000000000000000000");
    all += buildFieldHex("_MAC", "64000000");
    all += buildFieldHex("_DVE", "00010011101112131415161718191a1b1c1d1e1f22000000");
    all += buildFieldHex("Powr", "01000000");
    all += buildFieldHex("VidM", "09000000");
    all += buildFieldHex("AiVM", "00000000");
    all += buildFieldHex("TcLk", "00000000");
    all += buildFieldHex("TCCc", "01000000");

    // -- Group 3: input properties (14 sources, exact real device bytes) --
    static const char* inprHex[] = {
        "0000426c61636b000000000000000000000000000000424c4b0001000100010001001001",
        "000143616d657261203100000000000000000000000043414d3101000002000200001101",
        "000243616d657261203200000000000000000000000043414d3201000002000200001101",
        "000343616d657261203300000000000000000000000043414d3301000002000200001101",
        "000443616d657261203400000000000000000000000043414d3401000002000200001101",
        "03e8436f6c6f722042617273000000000000000000004241525301000100010002001001",
        "07d1436f6c6f72203100000000000000000000000000434f4c3101000100010003000001",
        "07d2436f6c6f72203200000000000000000000000000434f4c3201000100010003000001",
        "0bc24d6564696120506c6179657220310000000000004d50310001000100010004001001",
        "0bc34d6564696120506c617965722031204b657900004d50314b01000100010005001001",
        "271a50726f6772616d0000000000000000000000000050474d0001000100010080000100",
        "271b50726576696577000000000000000000000000005056570001000100010080000100",
        "2af943616d65726120312044697265637400000000004449520001000002000207000100",
        "1f414f757470757400000000000000000000000000000000000001000100010081000000",
    };
    for (auto h : inprHex)
        all += buildFieldHex("InPr", h);

    // -- Group 4: M/E state --
    all += fieldPrgI();
    all += fieldPrvI();
    all += buildFieldHex("TrSS", "0000020002000000");
    all += buildFieldHex("TrPr", "00000000");
    all += buildFieldHex("TrPs", "0000190000000000");
    all += buildFieldHex("TMxP", "00190000");
    all += buildFieldHex("TDpP", "001907d1");
    all += buildFieldHex("TWpP", "00190600000007d11388206c1388138800000000");
    all += buildFieldHex("TDvP", "0019191c0bc20bc3010101f402bc000000000000");
    all += buildFieldHex("TStP", "0001010001f402bc000000020049002200050000");

    // -- Group 5: keyer --
    all += fieldKeOn();
    all += buildFieldHex("KeBP", "00000301010000010bc300002328dcd8c1803e80");
    all += buildFieldHex("KBfT", "000000000bc20bc3");
    all += buildFieldHex("KBfT", "0000010000040000");
    all += buildFieldHex("KBfT", "0000020000010000");
    all += buildFieldHex("KBfT", "0000030000010000");
    all += buildFieldHex("KeLm", "0000010001f402bc00000000");
    all += buildFieldHex("KACk", "00000000000001f4000000000000000003e8000000000000");
    all += buildFieldHex("KACC", "00000000c40a209e026c1a5c065503f7");

    // -- Group 6: FTB, DSK, color, AUX, media --
    all += buildFieldHex("FtbP", "0019001b");
    all += buildFieldHex("FtbS", "00000019");
    all += buildFieldHex("DskB", "00000bc20bc30000");
    all += buildFieldHex("DskP", "0000190101f402bc00002328dcd8c1803e800000");
    all += buildFieldHex("DskS", "0000000001190d00");
    all += buildFieldHex("ColV", "0111010e03e801f4");
    all += buildFieldHex("AuxS", "0000271a");
    all += buildFieldHex("MPCE", "00010000");
    all += buildFieldHex("MPfe", "002c00130000000000000000000000000000000000000000");

    // -- Group 6b: extra keyer/DVE --
    all += buildFieldHex("KKFP", "000002c2000003e8000003e800000000000000000000000000000000000000000000000000000000016819000000000000000000");
    all += fieldKeDV();
    all += buildFieldHex("KeFS", "00000000004d0743");
    all += buildFieldHex("KePt", "0000061e13881388206c13881388004d");

    // -- Group 6c: monitor, camera control, RX --
    all += buildFieldHex("MOCP", "00020002fffff25400c801000000008c0000000000002454");
    all += buildFieldHex("CCdP", "040b00800000000200000000005072700000000000000000");
    all += buildFieldHex("CCst", "00001388");
    all += buildFieldHex("CapA", "010c0000");
    all += buildFieldHex("RXCC", "00030000");
    all += buildFieldHex("RXCP", "000301000088000000000000000000000000696e");
    all += buildFieldHex("RXMS", "0003000000000000000000000000009e0002005c");
    all += buildFieldHex("RXSS", "000303e800000000ffffffff0000000000000000000000000000000000000000");

    // -- Group 6d: Fairlight audio --
    all += buildFieldHex("FAIP", "05160250000102000206020301ffffff");
    all += buildFieldHex("FAMP", "0601362500000000000000000000000000ffffff");
    all += buildFieldHex("FASP", "05160000002c0000ffffffffffff0100010800430000000000ff000006012d010000000000000000000000000000000003010000");
    all += buildFieldHex("FIEP", "05160302");
    all += buildFieldHex("FMPP", "00000100");
    all += buildFieldHex("FMTl", "0006425041454250ffffffffffff0100000100ffffffffffff0100000200ffffffffffff0100000300ffffffffffff0100000400ffffffffffff0100051500ffffffffffff01000516001c5c");
    all += buildFieldHex("AEBP", "0516000041494c50ffffffffffff01000500330208080100000032640000000000470047");
    all += buildFieldHex("AICP", "05166b42ffffffffffffffffffff010000000000fffff25400c82d010000008c0000000000002454");
    all += buildFieldHex("AILP", "0516425041454250ffffffffffff010000fffffffffffb50000000470000000000002454");
    all += buildFieldHex("AIXP", "051603e846414950ffffffffffff010000000200ffffee6c0708006e0000008c0000000000002454");
    all += buildFieldHex("AMBP", "050033020808008c000032640000000000470000");
    all += buildFieldHex("AMLP", "00002454fffffb50000000470000000000002454");

    // -- Group 7: macro pool (100 slots) --
    for (int i = 0; i < 100; ++i)
        all += fieldMPrp(i);
    all += fieldMRPr();
    all += buildFieldHex("MRcS", "00ff0000");

    // -- Group 8: tally + lock --
    all += buildFieldHex("NIfT", "00000001000000010000000100000000");
    all += buildFieldHex("_TlC", "0001000004002454");
    all += buildFieldHex("TlFc", "00040001000002000003000004000000");
    all += fieldTlIn();
    all += fieldTlSr();
    all += buildFieldHex("LKST", "00000065");

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
