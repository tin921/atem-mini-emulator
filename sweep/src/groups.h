#pragma once
// The five test groups, shared by atem-sweep (--groups) and atem-sweep-gui.

#include <QString>
#include <QStringList>

// connect: connecting and probing; generated: every setter with good and bad
// values (g.); manual: hand-written SDK calls (m.); storage: stored macros
// and stills, backup-protected on a real switcher (s.); scenario: the rest.
inline QString testGroup(const QString& id) {
    if (id.startsWith("connect.") || id.startsWith("switcher.") || id.startsWith("probe.")) return "connect";
    if (id.startsWith("g.")) return "generated";
    if (id.startsWith("m.")) return "manual";
    if (id.startsWith("s.")) return "storage";
    return "scenario";
}

inline const QStringList& testGroups() {
    static const QStringList groups = { "connect", "generated", "manual", "storage", "scenario" };
    return groups;
}
