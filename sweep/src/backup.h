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

#include <QJsonObject>
#include <QString>
#include <QTextStream>
#include <map>
#include <utility>
#include <vector>

class Switcher;
class WireProxy;

// Connects (through `wire` when given), writes the backup into `dir`.
// Returns 0 on success, 2 if it could not start, 3 if something could not
// be read or written (the backup is then incomplete and says so).
int takeBackup(Switcher& s, WireProxy* wire, const QString& connectAddress, const QString& target,
               const QString& dir, QTextStream& out);

// Puts the switcher's stored content back to a backup: macros (uploaded
// when their bytes differ, renamed when only the name/description does,
// deleted when the backup has none) and stills (the same, by the switcher's
// hash). Checks every backup file's checksum first. Settings are not written
// here (the sweep restores the settings it changes); a verification backup
// compared with this one shows any that differ. 0 = done, 2 = could not
// start, 3 = something failed, 4 = a macro was running or recording.
int restoreBackup(Switcher& s, const QString& connectAddress, const QString& dir, QTextStream& out);

// Compares two backup folders: state fields, macros, stills. Both must be
// complete and pass validateBackup. Returns 0 when they match, 1 when they
// differ, 2 when either can't be read or isn't complete (not comparable).
int compareBackups(const QString& dirA, const QString& dirB, QTextStream& out);

// A backup checked from end to end: the manifest's format, version and
// completeness, every slot entry (index unique and inside the pool, the
// metadata a restore needs), every file against its size and checksum, and
// the state. Missing or wrongly typed information fails; it never reads as
// an empty pool.
struct ValidBackup {
    QJsonObject manifest;
    std::map<int, QByteArray> macroBytes, stillBytes;     // by slot
    std::vector<std::pair<QString, QString>> state;       // name, hex
};
bool validateBackup(const QString& dir, ValidBackup* backup, QString* why);
