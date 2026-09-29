#pragma once
// Full backup of a switcher through the SDK, so the sweep can later change
// stored content (macros, stills) and put everything back. Backups are a
// safety net and a troubleshooting aid, not reverse-engineering data: that
// comes only from the sweep's recorded responses.
//
// A backup folder holds:
//   manifest.json     target, product, time, what is included, per-slot info
//   state.txt         the connect dump: every state field the switcher sent a
//                     new client (NAME HEX per line, in order). Needs the
//                     recording proxy (Ethernet); a USB backup has no state.
//   macros/NN.bin     the bytes of each stored macro (MacroPool::Download)
//   stills/NN.raw     each stored still's frame bytes (Stills::Download), as
//                     the switcher sends them (the ATEM Mini: 10-bit YUVA);
//   stills/NN.png     a preview, only when the frame is 8-bit ARGB
//
// Taking a backup is read-only: it downloads, it never uploads or changes.

#include <QString>
#include <QTextStream>

class Switcher;
class WireProxy;

// Connects (through `wire` when given), writes the backup into `dir`.
// Returns 0 on success, 2 if it could not start, 3 if something could not
// be read or written (the backup is then incomplete and says so).
int takeBackup(Switcher& s, WireProxy* wire, const QString& connectAddress, const QString& target,
               const QString& dir, QTextStream& out);

// Compares two backup folders: state fields, macros, stills. Returns 0 when
// they match, 1 when they differ, 2 when a folder can't be read.
int compareBackups(const QString& dirA, const QString& dirB, QTextStream& out);
