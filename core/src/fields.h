// ATEM state fields: the switcher's state is the list of fields it sends in
// the connect dump. The emulator keeps them as raw bytes, changes them in
// place, and sends a field again whenever a command changes it.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <vector>

namespace emu {

struct Field {
    QByteArray name;   // four characters, e.g. "PrgI"
    QByteArray data;   // payload, without the 8-byte field header
};
using FieldList = QList<Field>;

// Big-endian accessors on a field payload. Out-of-range reads return 0 and
// out-of-range writes are ignored, so a short field never crashes the device.
inline quint8 u8(const QByteArray& d, int at) {
    return at < d.size() ? static_cast<quint8>(d[at]) : 0;
}
inline quint16 u16(const QByteArray& d, int at) {
    return static_cast<quint16>(u8(d, at) << 8 | u8(d, at + 1));
}
inline qint16 i16(const QByteArray& d, int at) { return static_cast<qint16>(u16(d, at)); }
inline quint32 u32(const QByteArray& d, int at) {
    return static_cast<quint32>(u16(d, at)) << 16 | u16(d, at + 2);
}
inline qint32 i32(const QByteArray& d, int at) { return static_cast<qint32>(u32(d, at)); }

inline void setU8(QByteArray& d, int at, quint8 v) {
    if (at < d.size()) d[at] = static_cast<char>(v);
}
inline void setU16(QByteArray& d, int at, quint16 v) {
    setU8(d, at, static_cast<quint8>(v >> 8));
    setU8(d, at + 1, static_cast<quint8>(v));
}
inline void setU32(QByteArray& d, int at, quint32 v) {
    setU16(d, at, static_cast<quint16>(v >> 16));
    setU16(d, at + 2, static_cast<quint16>(v));
}
// Copies [from, from+size) of src into dst at the same offset.
inline void copyBytes(QByteArray& dst, const QByteArray& src, int from, int size) {
    for (int i = from; i < from + size; ++i) setU8(dst, i, u8(src, i));
}

// The fields in dump order. Entries are never added or removed after
// loading, so pointers returned by find() stay valid.
class FieldStore {
public:
    bool load(const QString& path, QString* error);
    void clear() { m_fields.clear(); }

    // Instances of a field type are told apart by their leading bytes
    // (mix effect, key, input id, ...): find() returns the first field with
    // this name whose payload starts with key.
    QByteArray* find(const char* name, const QByteArray& key = {});
    const QByteArray* find(const char* name, const QByteArray& key = {}) const;
    QList<QByteArray*> all(const char* name);

    // How many leading bytes identify an instance of this field type.
    static int keyLength(const QByteArray& name);
    // Replaces the stored instance with the same name and key; false if none.
    bool replace(const Field& field);

    FieldList dump() const;
    int size() const { return static_cast<int>(m_fields.size()); }

private:
    std::vector<Field> m_fields;
};

// Short text key for find(): key(0) is "\x00", key(0, 1) is "\x00\x01".
QByteArray key(std::initializer_list<int> bytes);
inline QByteArray key(int a) { return key({ a }); }
inline QByteArray key(int a, int b) { return key({ a, b }); }
inline QByteArray key16(quint16 v) { return key({ v >> 8, v & 0xff }); }

} // namespace emu
