#include "commands.h"
#include "fields.h"

#include <cmath>

namespace emu::cmd {

namespace {

QByteArray sized(int size) { return QByteArray(size, '\0'); }
qint32 thousandths(double v) { return static_cast<qint32>(std::lround(v * 1000)); }

// Name + description commands: 16-bit lengths, then the UTF-8 bytes, padded
// to a multiple of 4 like every ATEM command.
QByteArray withText(QByteArray head, const QString& name, const QString& description, int lengthsAt) {
    QByteArray n = name.toUtf8(), d = description.toUtf8();
    setU16(head, lengthsAt, static_cast<quint16>(n.size()));
    setU16(head, lengthsAt + 2, static_cast<quint16>(d.size()));
    head += n + d;
    while (head.size() % 4) head.append('\0');
    return head;
}

} // namespace

QByteArray programInput(int me, quint16 source) {
    QByteArray d = sized(4);
    setU8(d, 0, static_cast<quint8>(me));
    setU16(d, 2, source);
    return d;
}

QByteArray previewInput(int me, quint16 source) { return programInput(me, source); }

QByteArray cut(int me) {
    QByteArray d = sized(4);
    setU8(d, 0, static_cast<quint8>(me));
    return d;
}

QByteArray autoTransition(int me) { return cut(me); }
QByteArray fadeToBlack(int me) { return cut(me); }

QByteArray keyType(int me, int key, int type) {
    QByteArray d = sized(8);
    setU8(d, 0, 0x01);
    setU8(d, 1, static_cast<quint8>(me));
    setU8(d, 2, static_cast<quint8>(key));
    setU8(d, 3, static_cast<quint8>(type));
    return d;
}

QByteArray keyFill(int me, int key, quint16 source) {
    QByteArray d = sized(4);
    setU8(d, 0, static_cast<quint8>(me));
    setU8(d, 1, static_cast<quint8>(key));
    setU16(d, 2, source);
    return d;
}

QByteArray keyOnAir(int me, int key, bool onAir) {
    QByteArray d = sized(4);
    setU8(d, 0, static_cast<quint8>(me));
    setU8(d, 1, static_cast<quint8>(key));
    setU8(d, 2, onAir ? 1 : 0);
    return d;
}

QByteArray macroAction(quint16 index, int action) {
    QByteArray d = sized(4);
    setU16(d, 0, index);
    setU8(d, 2, static_cast<quint8>(action));
    return d;
}

QByteArray macroProperties(quint16 index, const QString& name, const QString& description) {
    QByteArray head = sized(8);
    setU8(head, 0, 0x03);                    // name and description
    setU16(head, 2, index);
    return withText(head, name, description, 4);
}

QByteArray dve(int me, int key, quint32 mask, const DveParams& p) {
    QByteArray d = sized(64);
    setU32(d, 0, mask);
    setU8(d, 4, static_cast<quint8>(me));
    setU8(d, 5, static_cast<quint8>(key));
    setU32(d, 8, static_cast<quint32>(thousandths(p.sizeX)));
    setU32(d, 12, static_cast<quint32>(thousandths(p.sizeY)));
    setU32(d, 16, static_cast<quint32>(thousandths(p.positionX)));
    setU32(d, 20, static_cast<quint32>(thousandths(p.positionY)));
    setU8(d, 28, p.border ? 1 : 0);
    setU16(d, 32, static_cast<quint16>(std::lround(p.borderWidth * 100)));
    setU8(d, 40, static_cast<quint8>(std::lround(p.borderOpacity * 100)));
    setU16(d, 42, static_cast<quint16>(std::lround(p.borderHue * 10)));
    setU16(d, 44, static_cast<quint16>(std::lround(p.borderSaturation * 1000)));
    setU16(d, 46, static_cast<quint16>(std::lround(p.borderLuma * 1000)));
    setU8(d, 51, p.masked ? 1 : 0);
    setU16(d, 52, static_cast<quint16>(thousandths(p.maskTop)));
    setU16(d, 54, static_cast<quint16>(thousandths(p.maskBottom)));
    setU16(d, 56, static_cast<quint16>(thousandths(p.maskLeft)));
    setU16(d, 58, static_cast<quint16>(thousandths(p.maskRight)));
    setU8(d, 60, static_cast<quint8>(p.rate));
    return d;
}

} // namespace emu::cmd
