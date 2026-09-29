#include "fields.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace emu {

QByteArray key(std::initializer_list<int> bytes) {
    QByteArray k;
    for (int b : bytes) k.append(static_cast<char>(b));
    return k;
}

bool FieldStore::load(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    // Read into a new list: the current state is only replaced by a dump that
    // is whole and valid.
    static const QRegularExpression hex("^(?:[0-9a-fA-F]{2})*$");
    std::vector<Field> fields;
    QTextStream in(&file);
    int lineNo = 0;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        ++lineNo;
        if (line.isEmpty() || line.startsWith('#')) continue;
        QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() != 2 || parts[0].size() != 4) {
            if (error) *error = QString("%1:%2: expected NAME HEX").arg(path).arg(lineNo);
            return false;
        }
        if (!hex.match(parts[1]).hasMatch()) {
            if (error) *error = QString("%1:%2: the data is not whole bytes of hex").arg(path).arg(lineNo);
            return false;
        }
        fields.push_back({ parts[0].toLatin1(), QByteArray::fromHex(parts[1].toLatin1()) });
    }
    if (fields.empty() || fields.back().name != "InCm") {
        if (error) *error = path + ": the dump must end with InCm";
        return false;
    }
    m_fields = std::move(fields);
    return true;
}

QByteArray* FieldStore::find(const char* name, const QByteArray& k) {
    for (auto& f : m_fields)
        if (f.name == name && f.data.startsWith(k)) return &f.data;
    return nullptr;
}

const QByteArray* FieldStore::find(const char* name, const QByteArray& k) const {
    return const_cast<FieldStore*>(this)->find(name, k);
}

QList<QByteArray*> FieldStore::all(const char* name) {
    QList<QByteArray*> out;
    for (auto& f : m_fields)
        if (f.name == name) out.append(&f.data);
    return out;
}

int FieldStore::keyLength(const QByteArray& name) {
    static const QHash<QByteArray, int> lengths = {
        // mix effect + key
        { "KeBP", 2 }, { "KeDV", 2 }, { "KeOn", 2 }, { "KeFS", 2 }, { "KeLm", 2 },
        { "KePt", 2 }, { "KACk", 2 }, { "KACC", 2 },
        // mix effect + key + key type / key frame
        { "KBfT", 3 }, { "KKFP", 3 },
        // 16-bit index
        { "InPr", 2 }, { "MPrp", 2 },
        // camera: destination + category + parameter
        { "CCdP", 3 },
    };
    return lengths.value(name, 1);
}

QByteArray FieldStore::instanceKey(const Field& field) {
    const QByteArray& d = field.data;
    if (field.name == "MPfe") return d.left(1) + d.mid(2, 2);
    static const QSet<QByteArray> audio = { "FASP", "AICP", "AILP", "AIXP", "AEBP", "FASD" };
    if (audio.contains(field.name)) return d.left(2) + d.mid(8, field.name == "AEBP" ? 9 : 8);
    return d.left(keyLength(field.name));
}

const QByteArray* FieldStore::findInstance(const Field& field) const {
    const QByteArray key = instanceKey(field);
    for (const auto& f : m_fields)
        if (f.name == field.name && instanceKey(f) == key) return &f.data;
    return nullptr;
}

// The same identity as findInstance(): for media slots and Fairlight sources
// the leading bytes alone would pick the wrong instance.
bool FieldStore::replace(const Field& field) {
    const QByteArray key = instanceKey(field);
    for (auto& f : m_fields) {
        if (f.name == field.name && instanceKey(f) == key) {
            f.data = field.data;
            return true;
        }
    }
    return false;
}

FieldList FieldStore::dump() const {
    FieldList out;
    for (const auto& f : m_fields) out.append(f);
    return out;
}

} // namespace emu
