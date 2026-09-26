// Builders for the command payloads the SDK sends, so a local UI can drive
// the emulator through exactly the same handlers as a remote client. Values
// are in the SDK's units (position/size: frame units, ±16 x ±9 is the frame).
#pragma once

#include <QByteArray>
#include <QString>

namespace emu::cmd {

QByteArray programInput(int me, quint16 source);            // CPgI
QByteArray previewInput(int me, quint16 source);            // CPvI
QByteArray cut(int me);                                     // DCut
QByteArray autoTransition(int me);                          // DAut
QByteArray fadeToBlack(int me);                             // FtbA
QByteArray keyType(int me, int key, int type);              // CKTp (0 luma .. 3 DVE)
QByteArray keyFill(int me, int key, quint16 source);        // CKeF
QByteArray keyOnAir(int me, int key, bool onAir);           // CKOn
QByteArray macroAction(quint16 index, int action);          // MAct (0 run, 1 stop, 4 continue, 5 delete)
QByteArray macroProperties(quint16 index, const QString& name, const QString& description);  // CMPr

// CKDV: DVE / fly parameters of an upstream key.
struct DveParams {
    double sizeX = 1, sizeY = 1, positionX = 0, positionY = 0;
    bool border = false;
    double borderWidth = 0;                 // outer width, 0..16
    double borderHue = 0;                   // degrees
    double borderSaturation = 0, borderLuma = 1, borderOpacity = 1;   // 0..1
    bool masked = false;
    double maskTop = 0, maskBottom = 0, maskLeft = 0, maskRight = 0;
    int rate = 25;
};
enum DveBit : quint32 {
    SizeX = 1u << 0, SizeY = 1u << 1, PositionX = 1u << 2, PositionY = 1u << 3,
    Border = 1u << 5, BorderWidth = 1u << 8, BorderOpacity = 1u << 14,
    BorderHue = 1u << 15, BorderSaturation = 1u << 16, BorderLuma = 1u << 17,
    Masked = 1u << 20, MaskTop = 1u << 21, MaskBottom = 1u << 22, MaskLeft = 1u << 23, MaskRight = 1u << 24,
    Rate = 1u << 25,
};
QByteArray dve(int me, int key, quint32 mask, const DveParams& p);

} // namespace emu::cmd
