#include "device.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QTextStream>
#include <algorithm>
#include <optional>
#include <tuple>

namespace emu {

namespace {

// Offsets are into the command payload and the state field payload. They
// were read off the wire capture of the real ATEM Mini (sweep/golden) and
// agree with every command/echo pair recorded there.

constexpr quint8 kKeyTypeLuma = 0;
constexpr quint8 kKeyTypeDVE = 3;
constexpr quint8 kStyleDVE = 3;

// File transfers
constexpr quint16 kStillStore = 0x0000;
constexpr quint16 kMacroStore = 0xffff;
constexpr int kChunkSize = 1396;     // data bytes per FTDa, as the ATEM Mini sends them
constexpr int kWindow = 10;          // chunks sent ahead of the client's acknowledgements

} // namespace

// ── Setter table ─────────────────────────────────────────────
// Most "set value" commands are [mask][key][values] and change one field the
// same way; setters_table.inc (generated from core/tools/setters_spec.py,
// checked against the golden record) describes them and applySetter() runs
// them.
namespace setters {

enum class Rule : quint8 { None, Clamp, Mod, Allow, Ignore, UnitWrap, NegDec, AllowBits, EqFreq, RefuseIf };
enum class Post : quint8 { None, KeyframeStored, ChromaCursor, TransitionRate };
struct Effect { qint64 value; int field, size; qint64 set; };   // storing value also stores set
struct Prop {
    int bit;                       // mask bit; -1: the command has no mask
    int cmd, field, size;          // offsets in the command and the field
    bool isSigned;
    Rule rule;
    qint64 a, b;                   // rule arguments (see setters_spec.py)
    const qint64* set; int setCount;
    bool echoSame;                 // answered when the value doesn't change
    const char* alsoField; int alsoOffset;   // a second field that holds the same value
    const Effect* effects; int effectCount;
};
struct KeyByte { int cmd, field; };
struct Setter {
    const char* command;
    const char* field;
    int maskSize;
    const KeyByte* key; int keyCount;
    bool echoSame;
    Post post;
    const Prop* props; int propCount;
};
struct EqRange { qint64 range, lo, hi; };

#include "setters_table.inc"

qint64 readValue(const QByteArray& d, int at, int size, bool isSigned) {
    quint64 v = 0;
    for (int i = 0; i < size; ++i) v = v << 8 | u8(d, at + i);
    if (isSigned && size < 8 && (v >> (size * 8 - 1) & 1)) v |= ~0ull << (size * 8);
    return static_cast<qint64>(v);
}

void writeValue(QByteArray& d, int at, int size, qint64 v) {
    for (int i = size - 1; i >= 0; --i, v >>= 8) setU8(d, at + i, static_cast<quint8>(v));
}

bool inSet(const Prop& p, qint64 v) { return std::find(p.set, p.set + p.setCount, v) != p.set + p.setCount; }

// The value the switcher stores, or nothing if it refuses it.
std::optional<qint64> applyRule(const Prop& p, qint64 sent, const QByteArray& field) {
    switch (p.rule) {
    case Rule::None: return sent;
    case Rule::Clamp: return std::clamp(sent, p.a, p.b);
    case Rule::Mod: return (sent % p.a + p.a) % p.a;
    case Rule::Allow: if (inSet(p, sent)) return sent; return std::nullopt;
    case Rule::Ignore: return std::nullopt;
    case Rule::UnitWrap: {                  // whole part kept in 16 bits
        qint64 whole = sent / 1000;
        qint64 v = ((whole + 32768) % 65536 + 65536) % 65536 - 32768;
        v = v * 1000 + (sent - whole * 1000);
        return p.b ? std::max(p.a, v) : v;
    }
    case Rule::NegDec: return sent < 0 && sent % 1000 == 0 ? sent - 1 : sent;
    case Rule::AllowBits:
        if (sent > 0 && (sent & (sent - 1)) == 0 && (sent & u8(field, static_cast<int>(p.a)))) return sent;
        return std::nullopt;
    case Rule::EqFreq:
        for (const EqRange& r : kEqRanges)
            if (r.range == u8(field, static_cast<int>(p.a))) return std::clamp(sent, r.lo, r.hi);
        return sent;
    case Rule::RefuseIf:
        if (inSet(p, u8(field, static_cast<int>(p.a)))) return std::nullopt;
        return sent;
    }
    return sent;
}

// The advanced chroma sample cursor stays inside the frame (X +-16000,
// Y +-9000 = 960 x 540 pixels from the centre). Clamped positions are whole
// pixels; a size change re-reads the position in whole pixels too.
void keepCursorInFrame(QByteArray& f, bool sizeSet) {
    const int size = i16(f, 8);
    // Half the cursor in pixels, measured at these sizes; straight lines between.
    static constexpr int pts[][2] = { { 620, 37 }, { 2500, 140 }, { 5000, 275 }, { 9925, 540 } };
    int half = 540;
    for (int i = 0; i + 1 < 4; ++i) {
        if (size <= pts[i + 1][0]) {
            half = pts[i][1] + (pts[i + 1][1] - pts[i][1]) * (std::max(size, pts[i][0]) - pts[i][0])
                                   / (pts[i + 1][0] - pts[i][0]);
            break;
        }
    }
    for (auto [at, pixels] : { std::pair{ 4, 960 }, std::pair{ 6, 540 } }) {
        int v = i16(f, at);
        if (sizeSet) v = static_cast<int>(static_cast<int>(v * 3 / 50.0) * 50 / 3.0);
        const int limit = static_cast<int>((pixels - half) * 50 / 3.0);
        setU16(f, at, static_cast<quint16>(std::clamp(v, -limit, limit)));
    }
}

} // namespace setters

namespace {

double framesPerSecond(quint8 videoMode) {
    switch (videoMode) {
    case 4: case 12: return 50;
    case 5: case 13: return 59.94;
    case 6: case 10: return 25;
    case 7: case 11: return 29.97;
    case 8: return 23.98;
    case 9: return 24;
    default: return 25;
    }
}

QByteArray text(const char* s, int size) {
    QByteArray b(s);
    b.resize(size, '\0');
    return b;
}

} // namespace

Device::Device(QObject* parent) : QObject(parent) {
    registerHandlers();
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_frameTimer, &QTimer::timeout, this, &Device::tick);
    m_macroTimer.setSingleShot(true);
    m_macroTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_macroTimer, &QTimer::timeout, this, [this]() {
        m_out.clear();
        continueMacro();
        updateTally();
        emitOutput();
    });
}

bool Device::load(const QString& profileDir, QString* error) {
    if (!m_store.load(profileDir + "/dump.txt", error)) return false;

    m_defaultNames.clear();
    for (QByteArray* in : m_store.all("InPr")) m_defaultNames[u16(*in, 0)] = *in;

    const QByteArray* keyProps = m_store.find("KeBP", key(0, 0));
    m_dveOwner = keyProps && u8(*keyProps, 2) == kKeyTypeDVE ? DveOwner::Keyer
               : dveTakenByTransition()                      ? DveOwner::Transition
                                                             : DveOwner::None;

    const QByteArray* videoMode = m_store.find("VidM");
    m_frameTimer.setInterval(qRound(1000.0 / framesPerSecond(videoMode ? u8(*videoMode, 0) : 0)));

    // Stored macros: names from the dump, steps from macros.txt (the fields
    // each macro changed when atem-sweep ran it on the real switcher).
    loadMacroPool();
    QFile macros(profileDir + "/macros.txt");
    if (macros.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&macros);
        while (!in.atEnd()) {
            QStringList parts = in.readLine().trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() != 3 || parts[0].startsWith('#')) continue;
            int index = parts[0].toInt();
            if (index < 0 || index >= m_pool.size()) continue;
            m_pool[index].ops.append({ MacroOp::Patch, parts[1].toLatin1(), QByteArray::fromHex(parts[2].toLatin1()), 0 });
        }
    }
    // The stored macros' bytes (as a download gives them), from a backup of the real switcher.
    QFile bytes(profileDir + "/macro-bytes.txt");
    if (bytes.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&bytes);
        while (!in.atEnd()) {
            QStringList parts = in.readLine().trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() != 2 || parts[0].startsWith('#')) continue;
            int index = parts[0].toInt();
            if (index >= 0 && index < m_pool.size()) m_pool[index].bytes = QByteArray::fromHex(parts[1].toLatin1());
        }
    }
    int used = 0;
    for (const Macro& m : m_pool) used += m.used ? 1 : 0;
    emit log(QString("Profile %1: %2 fields, %3 of %4 macro slots used, %5 ms per frame")
             .arg(profileDir).arg(m_store.size()).arg(used).arg(m_pool.size()).arg(m_frameTimer.interval()));
    return true;
}

QString Device::productName() const {
    const QByteArray* pin = m_store.find("_pin");
    return pin ? QString::fromUtf8(pin->left(44).constData()) : QString();
}

namespace {
// Commands that control macros themselves; never recorded into a macro.
bool isMacroControl(const QByteArray& name) {
    return name == "MAct" || name == "MSRc" || name == "MSlp" || name == "MRCP" || name == "CMPr";
}
} // namespace

FieldList Device::handle(const QByteArray& name, const QByteArray& data) {
    m_out.clear();
    auto it = m_handlers.constFind(name);
    if (it == m_handlers.constEnd()) {
        emit log("unhandled command " + QString::fromLatin1(name) + " " + QString::fromLatin1(data.toHex()));
        return {};
    }
    // While a macro is being recorded the switcher stores what it is told to do.
    if (m_recording.active && !isMacroControl(name))
        m_recording.macro.ops.append({ MacroOp::Command, name, data, 0 });
    it.value()(data);
    updateTally();
    if (!m_out.isEmpty()) emit stateChanged();
    return std::exchange(m_out, {});
}

FieldList Device::endOfPacket() {
    if (m_pendingMacro < 0) return {};
    m_out.clear();
    m_run = { m_pendingMacro, 0, false };
    m_pendingMacro = -1;
    emit macroStarted(m_run.index);
    continueMacro();
    updateTally();
    if (!m_out.isEmpty()) emit stateChanged();
    return std::exchange(m_out, {});
}

void Device::apply(const QByteArray& name, const QByteArray& data) {
    FieldList out = handle(name, data);
    out += endOfPacket();
    if (!out.isEmpty()) emit fieldsChanged(out);
}

void Device::emitOutput() {
    if (m_out.isEmpty()) return;
    emit fieldsChanged(std::exchange(m_out, {}));
    emit stateChanged();
}

void Device::send(const char* name, const QByteArray* data) {
    if (data) m_out.append({ QByteArray(name), *data });
}

void Device::sendIfChanged(const char* name, QByteArray* data, const QByteArray& before) {
    if (data && *data != before) send(name, data);
}

void Device::reply(const char* name, const QByteArray& data) {
    m_out.append({ QByteArray(name), data, true });
}

// Applies a command from the setter table: each value whose mask bit is set
// goes through its rule. A command whose values are all refused changes
// nothing and gets no answer; one that changes nothing is answered only if
// its values are echoed when unchanged (not for sources and audio).
void Device::applySetter(const setters::Setter& s, const QByteArray& d) {
    using namespace setters;
    const quint32 mask = s.maskSize ? static_cast<quint32>(readValue(d, 0, s.maskSize, false)) : ~0u;
    QByteArray* target = nullptr;
    for (QByteArray* f : m_store.all(s.field)) {
        bool match = true;
        for (int i = 0; i < s.keyCount && match; ++i) match = u8(*f, s.key[i].field) == u8(d, s.key[i].cmd);
        if (match) { target = f; break; }
    }
    if (!target) return;

    QByteArray next = *target;
    int count = 0, refused = 0;
    bool echo = false;
    QList<const Prop*> mirrored;
    for (int i = 0; i < s.propCount; ++i) {
        const Prop& p = s.props[i];
        if (p.bit >= 0 && !(mask & (1u << p.bit))) continue;
        ++count;
        std::optional<qint64> value = applyRule(p, readValue(d, p.cmd, p.size, p.isSigned), next);
        if (!value) { ++refused; continue; }
        writeValue(next, p.field, p.size, *value);
        for (int e = 0; e < p.effectCount; ++e)
            if (p.effects[e].value == *value) writeValue(next, p.effects[e].field, p.effects[e].size, p.effects[e].set);
        echo = echo || p.echoSame;
        if (p.alsoField) mirrored.append(&p);
    }
    if (count == 0 || refused == count) return;
    if (s.post == Post::ChromaCursor) keepCursorInFrame(next, mask & 0x10);
    if (next == *target && !(s.echoSame && echo)) return;
    *target = next;

    if (s.post == Post::KeyframeStored) {        // setting a keyframe value stores the keyframe
        const int frame = u8(d, 6);
        QByteArray* stored = m_store.find("KeFS", key(u8(d, 4), u8(d, 5)));
        if (stored && (frame == 1 || frame == 2)) {
            setU8(*stored, 1 + frame, 1);
            send("KeFS", stored);
        }
    }
    send(s.field, target);
    // The rate of the next transition's style is also TrPs's frames remaining.
    const bool rateSet = s.propCount && (s.props[0].bit < 0 || (mask & (1u << s.props[0].bit)));
    if (s.post == Post::TransitionRate && rateSet) {
        const QByteArray* style = m_store.find("TrSS", key(u8(*target, 0)));
        static const char* kParams[] = { "TMxP", "TDpP", "TWpP", "TDvP" };
        const int current = style ? u8(*style, 1) : 0;
        QByteArray* position = m_store.find("TrPs", key(u8(*target, 0)));
        if (position && !u8(*position, 1) && current < 4 && qstrcmp(kParams[current], s.field) == 0) {
            setU8(*position, 2, u8(*target, 1));
            send("TrPs", position);
        }
    }
    // Values shared with another field (the DVE transition's key settings are
    // the stinger's too). Its key is the leading key bytes of this field.
    QByteArray prefix;
    for (int i = 0; i < s.keyCount && s.key[i].field == i; ++i) prefix.append(static_cast<char>(u8(d, s.key[i].cmd)));
    QList<QByteArray> others;
    for (const Prop* p : mirrored) {
        QByteArray* other = m_store.find(p->alsoField, prefix);
        if (!other) continue;
        for (int i = 0; i < p->size; ++i) setU8(*other, p->alsoOffset + i, u8(next, p->field + i));
        if (!others.contains(p->alsoField)) others.append(p->alsoField);
    }
    for (const QByteArray& name : others) send(name.constData(), m_store.find(name.constData(), prefix));
}

// ── Inputs ───────────────────────────────────────────────────

const QByteArray* Device::input(quint16 id) const { return m_store.find("InPr", key16(id)); }

// InPr byte 35: mix effect blocks the input can be used on (bit 0 = M/E 1).
// Program/Preview outputs and "Camera 1 Direct" have 0 and are refused.
bool Device::usableOnMixEffect(quint16 id) const {
    const QByteArray* in = input(id);
    return in && (u8(*in, 35) & 0x01);
}

// InPr byte 34 bit 4: the input can be a key cut (key source).
bool Device::usableAsKeySource(quint16 id) const {
    const QByteArray* in = input(id);
    return in && (u8(*in, 34) & 0x10);
}

// Names are copied like strcpy: up to and including the terminating zero,
// leaving whatever followed in the old name (the real switcher does the same).
void Device::copyName(QByteArray& field, int at, int size, const QByteArray& name) {
    for (int i = 0; i < size; ++i) {
        quint8 c = u8(name, i);
        setU8(field, at + i, c);
        if (!c) break;
    }
}

// "Camera N Direct" (input 11000 + N) follows camera N's long name.
void Device::updateDirectName(quint16 id) {
    QByteArray* direct = m_store.find("InPr", key16(static_cast<quint16>(11000 + id)));
    const QByteArray* source = input(id);
    if (!direct || !source) return;
    QByteArray name = source->mid(2, 20);
    name.truncate(name.indexOf('\0') < 0 ? 20 : name.indexOf('\0'));
    QByteArray full = name.size() + 7 <= 20 ? name + " Direct" : name.left(10) + "... Direct";
    if (full.size() < 20) full.append('\0');
    copyName(*direct, 2, 20, full);
    setU8(*direct, 26, 0);                          // its names are no longer default
    send("InPr", direct);
}

// ── Mix effect / transition ──────────────────────────────────

int Device::transitionRate() const {
    const QByteArray* style = m_store.find("TrSS", key(0));
    const char* field = "TMxP";
    switch (style ? u8(*style, 1) : 0) {
    case 1: field = "TDpP"; break;
    case 2: field = "TWpP"; break;
    case 3: field = "TDvP"; break;
    }
    const QByteArray* params = m_store.find(field, key(0));
    return std::max(1, params ? int(u8(*params, 1)) : 25);
}

// The ATEM Mini has one DVE, shared by the DVE transition and upstream key 1.
bool Device::dveTakenByTransition() const {
    const QByteArray* style = m_store.find("TrSS", key(0));
    return style && (u8(*style, 1) == kStyleDVE || u8(*style, 3) == kStyleDVE);
}

// While the transition holds the DVE the key keeps its type but can neither
// be a DVE key nor fly (KeBP bytes 3 and 4). The switcher warns when the DVE
// changes hands between the keyer and the transition ("Take").
void Device::setDveTaken(bool taken) {
    QByteArray* props = m_store.find("KeBP", key(0, 0));
    if (!props) return;
    bool wasTaken = u8(*props, 3) == 0;
    if (taken == wasTaken) return;
    if (taken) {
        if (m_dveOwner == DveOwner::Keyer) warn("DVE taken from Keyer");
        m_dveOwner = DveOwner::Transition;
    }
    setU8(*props, 3, taken ? 0 : 1);
    setU8(*props, 4, taken ? 0 : 1);
    send("KeBP", props);
}

void Device::warn(const char* message) {
    m_out.append({ "Warn", text(message, 44) });
}

void Device::swapProgramPreview() {
    QByteArray* program = m_store.find("PrgI", key(0));
    QByteArray* preview = m_store.find("PrvI", key(0));
    if (!program || !preview) return;
    quint16 p = u16(*program, 2);
    setU16(*program, 2, u16(*preview, 2));
    setU16(*preview, 2, p);
    send("PrgI", program);
    send("PrvI", preview);
}

// End of an auto transition, a completed T-bar or a cut: the layers in the
// next-transition selection change over.
void Device::finishTransition() {
    QByteArray* style = m_store.find("TrSS", key(0));
    quint8 selection = style ? u8(*style, 4) : 1;
    if (selection & 0x01) swapProgramPreview();
    if (selection & 0x02) {
        if (QByteArray* onAir = m_store.find("KeOn", key(0, 0))) {
            setU8(*onAir, 2, u8(*onAir, 2) ? 0 : 1);
            send("KeOn", onAir);
        }
    }
    if (QByteArray* position = m_store.find("TrPs", key(0)); position && u8(*position, 1)) {
        setU8(*position, 1, 0);
        setU8(*position, 2, static_cast<quint8>(transitionRate()));
        setU16(*position, 4, 0);
        send("TrPs", position);
    }
    if (QByteArray* preview = m_store.find("PrvI", key(0)); preview && u8(*preview, 4)) {
        setU8(*preview, 4, 0);
        send("PrvI", preview);
    }
}

// Tally: bit 0 program, bit 1 preview.
void Device::updateTally() {
    QByteArray* byInput = m_store.find("TlIn");
    QByteArray* bySource = m_store.find("TlSr");
    if (!byInput || !bySource) return;

    QHash<quint16, int> flags;
    auto mark = [&](const QByteArray* field, int at, int bit) { if (field) flags[u16(*field, at)] |= bit; };
    const QByteArray* program = m_store.find("PrgI", key(0));
    const QByteArray* preview = m_store.find("PrvI", key(0));
    const QByteArray* position = m_store.find("TrPs", key(0));
    const QByteArray* style = m_store.find("TrSS", key(0));
    const QByteArray* keyOn = m_store.find("KeOn", key(0, 0));
    const QByteArray* keyProps = m_store.find("KeBP", key(0, 0));
    const QByteArray* dskState = m_store.find("DskS", key(0));
    const QByteArray* dskProps = m_store.find("DskP", key(0));
    const QByteArray* dskSources = m_store.find("DskB", key(0));
    // Fully faded to black: nothing is on program.
    const QByteArray* fade = m_store.find("FtbS", key(0));
    bool black = fade && u8(*fade, 1);
    if (!black) mark(program, 2, 1);
    mark(preview, 2, 2);
    if (position && u8(*position, 1) && !black) mark(preview, 2, 1);
    if (black) {
        // keys and DSK are faded out too
    } else if (keyOn && u8(*keyOn, 2)) {
        mark(keyProps, 6, 1);
        if (keyProps && u8(*keyProps, 2) == kKeyTypeLuma) mark(keyProps, 8, 1);
    } else if (style && (u8(*style, 4) & 0x02)) {
        mark(keyProps, 6, 2);
    }
    if (dskState && u8(*dskState, 1) && !black) {
        mark(dskSources, 2, 1);
        mark(dskSources, 4, 1);
    } else if (dskProps && u8(*dskProps, 1)) {
        mark(dskSources, 2, 2);
    }

    QByteArray before = *bySource;
    for (int i = 0, n = u16(*bySource, 0); i < n; ++i)
        setU8(*bySource, 2 + i * 3 + 2, static_cast<quint8>(flags.value(u16(*bySource, 2 + i * 3))));
    sendIfChanged("TlSr", bySource, before);

    before = *byInput;
    for (int i = 0, n = u16(*byInput, 0); i < n; ++i)
        setU8(*byInput, 2 + i, static_cast<quint8>(flags.value(static_cast<quint16>(i + 1))));
    sendIfChanged("TlIn", byInput, before);
}

// ── Animations (one step per video frame) ────────────────────

void Device::startAnimation(const QString& id, int frames, std::function<void(int)> step,
                            std::function<void()> done) {
    if (frames <= 0) {
        done();
        return;
    }
    m_animations[id] = { 0, frames, std::move(step), std::move(done) };
    if (!m_frameTimer.isActive()) m_frameTimer.start();
}

void Device::tick() {
    m_out.clear();
    for (const QString& id : m_animations.keys()) {
        Animation& a = m_animations[id];
        if (++a.frame >= a.frames) {
            auto done = std::move(a.done);
            m_animations.remove(id);
            done();
        } else {
            a.step(a.frame);
        }
    }
    if (m_animations.isEmpty()) m_frameTimer.stop();
    updateTally();
    emitOutput();
}

// ── Macros ───────────────────────────────────────────────────

QJsonArray Macro::opsToJson() const {
    static const char* kinds[] = { "command", "patch", "wait", "userWait" };
    QJsonArray out;
    for (const MacroOp& op : ops) {
        QJsonObject o{ { "kind", kinds[op.kind] } };
        if (op.kind == MacroOp::Command || op.kind == MacroOp::Patch) {
            o["name"] = QString::fromLatin1(op.name);
            o["data"] = QString::fromLatin1(op.data.toHex());
        }
        if (op.kind == MacroOp::Wait) o["frames"] = op.frames;
        out.append(o);
    }
    return out;
}

QList<MacroOp> Macro::opsFromJson(const QJsonArray& json) {
    QList<MacroOp> ops;
    for (const QJsonValue& v : json) {
        QJsonObject o = v.toObject();
        QString kind = o["kind"].toString();
        MacroOp op;
        op.kind = kind == "patch" ? MacroOp::Patch : kind == "wait" ? MacroOp::Wait
                : kind == "userWait" ? MacroOp::UserWait : MacroOp::Command;
        op.name = o["name"].toString().toLatin1();
        op.data = QByteArray::fromHex(o["data"].toString().toLatin1());
        op.frames = o["frames"].toInt();
        ops.append(op);
    }
    return ops;
}

namespace {
// Macro op ids, as recorded from the ATEM Mini (little-endian, see Macro::encode).
constexpr quint16 kOpPreviewInput = 0x0003;   // me, _, input
constexpr quint16 kOpUserWait = 0x0006;
constexpr quint16 kOpWait = 0x0007;           // frames (32 bits)

void putLE16(QByteArray& b, quint16 v) { b.append(static_cast<char>(v & 0xff)).append(static_cast<char>(v >> 8)); }
quint16 le16(const QByteArray& b, int at) { return static_cast<quint16>(u8(b, at) | u8(b, at + 1) << 8); }
} // namespace

QByteArray Macro::encode(const QList<MacroOp>& ops) {
    QByteArray out;
    for (const MacroOp& op : ops) {
        if (op.kind == MacroOp::Wait) {
            putLE16(out, 8); putLE16(out, kOpWait);
            putLE16(out, static_cast<quint16>(op.frames)); putLE16(out, static_cast<quint16>(op.frames >> 16));
        } else if (op.kind == MacroOp::UserWait) {
            putLE16(out, 4); putLE16(out, kOpUserWait);
        } else if (op.kind == MacroOp::Command && op.name == "CPvI") {
            putLE16(out, 8); putLE16(out, kOpPreviewInput);
            out.append(static_cast<char>(u8(op.data, 0))).append('\0');
            putLE16(out, u16(op.data, 2));
        }
    }
    return out;
}

bool Macro::decode(const QByteArray& bytes, QList<MacroOp>* ops) {
    QList<MacroOp> out;
    for (int at = 0; at < bytes.size();) {
        const int length = le16(bytes, at);
        if (length < 4 || at + length > bytes.size()) return false;
        const quint16 op = le16(bytes, at + 2);
        if (op == kOpWait && length >= 8) {
            out.append({ MacroOp::Wait, {}, {}, le16(bytes, at + 4) | le16(bytes, at + 6) << 16 });
        } else if (op == kOpUserWait) {
            out.append({ MacroOp::UserWait, {}, {}, 0 });
        } else if (op == kOpPreviewInput && length >= 8) {
            QByteArray d(4, '\0');
            setU8(d, 0, u8(bytes, at + 4));
            setU16(d, 2, le16(bytes, at + 6));
            out.append({ MacroOp::Command, "CPvI", d, 0 });
        }
        at += length;
    }
    if (ops) *ops = out;
    return true;
}

// Slot count from _MAC, names and descriptions from the MPrp fields.
void Device::loadMacroPool() {
    const QByteArray* count = m_store.find("_MAC");
    m_pool = QVector<Macro>(count ? u8(*count, 0) : 0);
    for (QByteArray* props : m_store.all("MPrp")) {
        int index = u16(*props, 0);
        if (index >= m_pool.size()) continue;
        Macro& m = m_pool[index];
        m.used = u8(*props, 2);
        int nameLength = u16(*props, 4), descLength = u16(*props, 6);
        m.name = QString::fromUtf8(props->mid(8, nameLength));
        m.description = QString::fromUtf8(props->mid(8 + nameLength, descLength));
    }
}

// MPrp: index, used, has-unsupported-ops, name length, description length,
// name, description, padded to 4 bytes.
void Device::publishMacro(int index) {
    QByteArray* stored = m_store.find("MPrp", key16(static_cast<quint16>(index)));
    if (!stored) return;
    const Macro& m = m_pool[index];
    QByteArray props(8, '\0');
    setU16(props, 0, static_cast<quint16>(index));
    if (m.used) {
        QByteArray name = m.name.toUtf8(), desc = m.description.toUtf8();
        setU8(props, 2, 1);
        setU16(props, 4, static_cast<quint16>(name.size()));
        setU16(props, 6, static_cast<quint16>(desc.size()));
        props += name + desc;
        while (props.size() % 4) props.append('\0');
    }
    *stored = props;
    send("MPrp", stored);
    emit macroPoolChanged(index);
}

void Device::setMacro(int index, const Macro& macro) {
    if (index < 0 || index >= m_pool.size()) return;
    m_out.clear();
    m_pool[index] = macro;
    publishMacro(index);
    emitOutput();
}

// MRPr: byte 0 bit 0 running, bit 1 waiting for the user; byte 1 loop; index.
// While waiting only bit 1 is set: with both bits the SDK reports "running"
// and never "waiting for user" (the SDK's reading; not yet recorded from the
// real switcher).
void Device::setRunStatus(bool running, bool waiting, int index) {
    QByteArray* status = m_store.find("MRPr");
    if (!status) return;
    setU8(*status, 0, static_cast<quint8>(waiting ? 2 : running ? 1 : 0));
    setU16(*status, 2, running ? static_cast<quint16>(index) : 0xffff);
    send("MRPr", status);
}

// MRcS: byte 0 recording, index.
void Device::setRecordingStatus(bool recording, int index) {
    QByteArray* status = m_store.find("MRcS");
    if (!status) return;
    setU8(*status, 0, recording ? 1 : 0);
    setU16(*status, 2, static_cast<quint16>(index));
    send("MRcS", status);
}

// Runs the macro's steps until a wait or the end. Output goes to m_out.
void Device::continueMacro() {
    if (m_run.index < 0 || m_run.waitingForUser) return;
    const Macro& m = m_pool[m_run.index];
    while (m_run.step < m.ops.size()) {
        const MacroOp& op = m.ops[m_run.step++];
        switch (op.kind) {
        case MacroOp::Command:
            if (auto h = m_handlers.constFind(op.name); h != m_handlers.constEnd() && !isMacroControl(op.name))
                h.value()(op.data);
            break;
        case MacroOp::Patch:
            if (m_store.replace({ op.name, op.data })) m_out.append({ op.name, op.data });
            break;
        case MacroOp::Wait:
            m_macroTimer.start(std::max(1, qRound(op.frames * frameIntervalMs())));
            return;
        case MacroOp::UserWait:
            m_run.waitingForUser = true;
            setRunStatus(true, true, m_run.index);
            return;
        }
    }
    const QByteArray* status = m_store.find("MRPr");
    if (status && u8(*status, 1) && !m.ops.isEmpty()) {   // loop: again, one frame later
        m_run.step = 0;
        m_macroTimer.start(std::max(1, qRound(frameIntervalMs())));
        return;
    }
    finishMacro();
}

void Device::finishMacro() {
    int index = m_run.index;
    m_run = {};
    m_macroTimer.stop();
    setRunStatus(false, false, -1);
    if (index >= 0) emit macroFinished(index);
}

// ── Reading the state ────────────────────────────────────────

SwitcherView Device::view() const {
    SwitcherView v;
    if (const QByteArray* f = m_store.find("PrgI", key(0))) v.program = u16(*f, 2);
    if (const QByteArray* f = m_store.find("PrvI", key(0))) {
        v.preview = u16(*f, 2);
        v.previewLive = u8(*f, 4);
    }
    if (const QByteArray* f = m_store.find("TrPs", key(0))) {
        v.inTransition = u8(*f, 1);
        v.transitionPosition = u16(*f, 4) / 10000.0;
    }
    if (const QByteArray* f = m_store.find("TrSS", key(0))) {
        v.transitionStyle = u8(*f, 1);
        v.nextSelection = u8(*f, 4);
    }
    if (const QByteArray* f = m_store.find("KeOn", key(0, 0))) v.keyOnAir = u8(*f, 2);
    if (const QByteArray* f = m_store.find("KeBP", key(0, 0))) {
        v.keyType = u8(*f, 2);
        v.keyCanUseDve = u8(*f, 3);
        v.keyFill = u16(*f, 6);
        v.keyCut = u16(*f, 8);
    }
    if (const QByteArray* f = m_store.find("KeDV", key(0, 0))) {
        v.sizeX = u32(*f, 4) / 1000.0;
        v.sizeY = u32(*f, 8) / 1000.0;
        v.positionX = i32(*f, 12) / 1000.0;
        v.positionY = i32(*f, 16) / 1000.0;
        v.borderEnabled = u8(*f, 24);
        v.borderWidth = u16(*f, 28) / 100.0;
        v.borderOpacity = u8(*f, 36) / 100.0;
        v.borderHue = u16(*f, 38) / 10.0;
        v.borderSaturation = u16(*f, 40) / 1000.0;
        v.borderLuma = u16(*f, 42) / 1000.0;
        v.masked = u8(*f, 47);
        v.maskTop = u16(*f, 48) / 1000.0;
        v.maskBottom = u16(*f, 50) / 1000.0;
        v.maskLeft = u16(*f, 52) / 1000.0;
        v.maskRight = u16(*f, 54) / 1000.0;
    }
    if (const QByteArray* f = m_store.find("FtbS", key(0))) {
        v.fadeFullyBlack = u8(*f, 1);
        v.fadeInTransition = u8(*f, 2);
        v.fadeFramesRemaining = u8(*f, 3);
    }
    if (const QByteArray* f = m_store.find("FtbP", key(0))) v.fadeRate = u8(*f, 1);
    if (const QByteArray* f = m_store.find("DskS", key(0))) {
        v.dskOnAir = u8(*f, 1);
        v.dskInTransition = u8(*f, 2);
        v.dskFramesRemaining = u8(*f, 5);
    }
    if (const QByteArray* f = m_store.find("DskP", key(0))) v.dskRate = u8(*f, 2);
    if (const QByteArray* f = m_store.find("DskB", key(0))) {
        v.dskFill = u16(*f, 2);
        v.dskCut = u16(*f, 4);
    }
    if (const QByteArray* f = m_store.find("MRPr")) {
        v.macroRunning = u8(*f, 0) & 0x01;
        v.macroWaiting = u8(*f, 0) & 0x02;
        v.macroLoop = u8(*f, 1);
        v.macroIndex = v.macroRunning ? u16(*f, 2) : -1;
    }
    v.macroRecording = m_recording.active;
    v.macroRecordingIndex = m_recording.active ? m_recording.index : -1;
    return v;
}

QList<InputInfo> Device::inputs() const {
    QList<InputInfo> out;
    for (const Field& f : m_store.dump()) {
        if (f.name != "InPr") continue;
        auto text = [&](int at, int size) {
            QByteArray b = f.data.mid(at, size);
            int end = b.indexOf('\0');
            return QString::fromUtf8(end < 0 ? b : b.left(end));
        };
        out.append({ u16(f.data, 0), text(2, 20), text(22, 4), u8(f.data, 26) != 0 });
    }
    return out;
}

// ColV: generator, hue x10, saturation x1000, luma x1000.
bool Device::colorGenerator(int index, double* hue, double* saturation, double* luma) const {
    const QByteArray* f = m_store.find("ColV", key(index - 1));
    if (!f) return false;
    *hue = u16(*f, 2) / 10.0;
    *saturation = u16(*f, 4) / 1000.0;
    *luma = u16(*f, 6) / 1000.0;
    return true;
}

// ── Command handlers ─────────────────────────────────────────

void Device::registerHandlers() {
    auto& h = m_handlers;

    // Setter table (setters_table.inc) ----------------------------------
    for (const setters::Setter& s : setters::kSetters)
        h[s.command] = [this, &s](const QByteArray& d) { applySetter(s, d); };

    // Inputs ------------------------------------------------------------
    h["CInL"] = [this](const QByteArray& d) {       // set input names
        quint8 mask = u8(d, 0);
        quint16 id = u16(d, 2);
        QByteArray* in = m_store.find("InPr", key16(id));
        if (!in) return;
        if (mask & 0x01) copyName(*in, 2, 20, d.mid(4, 20));
        if (mask & 0x02) copyName(*in, 22, 4, d.mid(24, 4));
        if (mask & 0x03) setU8(*in, 26, 0);         // names no longer default
        send("InPr", in);
        if (mask & 0x01) updateDirectName(id);
    };
    h["RInL"] = [this](const QByteArray& d) {       // reset input names
        quint16 id = u16(d, 0);
        QByteArray* in = m_store.find("InPr", key16(id));
        if (!in || !m_defaultNames.contains(id)) return;
        const QByteArray& original = m_defaultNames[id];
        copyName(*in, 2, 20, original.mid(2, 20));
        copyName(*in, 22, 4, original.mid(22, 4));
        setU8(*in, 26, 1);
        send("InPr", in);
    };

    // Program / preview / transition ---------------------------------------
    auto setSource = [this](const char* name, const QByteArray& d) {
        quint16 source = u16(d, 2);
        if (!usableOnMixEffect(source)) return;
        QByteArray* field = m_store.find(name, key(u8(d, 0)));
        if (!field) return;
        setU16(*field, 2, source);
        send(name, field);
    };
    h["CPgI"] = [setSource](const QByteArray& d) { setSource("PrgI", d); };
    h["CPvI"] = [setSource](const QByteArray& d) { setSource("PrvI", d); };

    h["CTPr"] = [this](const QByteArray& d) {       // preview transition
        if (QByteArray* f = m_store.find("TrPr", key(u8(d, 0)))) {
            setU8(*f, 1, u8(d, 1));
            send("TrPr", f);
        }
    };
    h["DCut"] = [this](const QByteArray&) {
        const QByteArray* position = m_store.find("TrPs", key(0));
        if (position && u8(*position, 1)) return;   // not during a transition
        finishTransition();
    };
    h["DAut"] = [this](const QByteArray&) {
        QByteArray* position = m_store.find("TrPs", key(0));
        if (!position || u8(*position, 1) || m_animations.contains("me")) return;
        int rate = transitionRate();
        if (QByteArray* preview = m_store.find("PrvI", key(0))) {
            setU8(*preview, 4, 1);                   // preview is now live
            send("PrvI", preview);
        }
        startAnimation("me", rate,
            [this, rate](int frame) {
                QByteArray* p = m_store.find("TrPs", key(0));
                setU8(*p, 1, 1);
                setU8(*p, 2, static_cast<quint8>(rate - 1 - frame));
                setU16(*p, 4, static_cast<quint16>(10000 * frame / std::max(1, rate - 1)));
                send("TrPs", p);
            },
            [this]() { finishTransition(); });
    };
    h["CTPs"] = [this](const QByteArray& d) {       // T-bar position, 0..10000
        QByteArray* position = m_store.find("TrPs", key(u8(d, 0)));
        QByteArray* preview = m_store.find("PrvI", key(u8(d, 0)));
        if (!position || !preview || m_animations.contains("me")) return;
        int pos = std::clamp(int(u16(d, 2)), 0, 10000);
        bool inTransition = u8(*position, 1);
        int rate = transitionRate();
        if (pos == 0) {
            if (!inTransition) return;
            setU8(*position, 1, 0);                  // T-bar pulled back: abandon
            setU8(*position, 2, static_cast<quint8>(rate));
            setU16(*position, 4, 0);
            setU8(*preview, 4, 0);
            send("PrvI", preview);
            send("TrPs", position);
            return;
        }
        if (!inTransition && pos < 10000) {
            setU8(*preview, 4, 1);
            send("PrvI", preview);
        }
        setU8(*position, 1, 1);
        setU8(*position, 2, static_cast<quint8>(rate * (10000 - pos) / 10000));
        setU16(*position, 4, static_cast<quint16>(pos));
        send("TrPs", position);
        if (pos >= 10000) finishTransition();
    };
    h["CTTp"] = [this](const QByteArray& d) {       // next transition style / selection
        quint8 mask = u8(d, 0);
        QByteArray* style = m_store.find("TrSS", key(u8(d, 1)));
        const QByteArray* position = m_store.find("TrPs", key(u8(d, 1)));
        if (!style) return;
        bool inTransition = position && u8(*position, 1);
        if ((mask & 0x01) && u8(d, 2) == kStyleDVE) {
            // A DVE key in the next transition keeps the DVE: the style is
            // refused (recorded: three warnings). Without the key in the
            // selection the transition takes the DVE (below).
            const QByteArray* props = m_store.find("KeBP", key(0, 0));
            if (props && u8(*props, 2) == kKeyTypeDVE && (u8(*style, 4) & 0x02)) {
                for (int i = 0; i < 3; ++i) warn("DVE unavailable");
                return;
            }
        }
        if ((mask & 0x01) && u8(d, 2) <= 4) {
            setU8(*style, 3, u8(d, 2));
            if (!inTransition) setU8(*style, 1, u8(d, 2));
        }
        if ((mask & 0x02) && u8(d, 3) != 0 && u8(d, 3) < 4) {
            // A DVE key can't take part in a DVE transition: there is one DVE.
            const QByteArray* props = m_store.find("KeBP", key(0, 0));
            bool dveKey = props && u8(*props, 2) == kKeyTypeDVE;
            if ((u8(d, 3) & 0x02) && dveKey && dveTakenByTransition()) {
                warn("DVE unavailable");
                return;
            }
            setU8(*style, 4, u8(d, 3));
            if (!inTransition) setU8(*style, 2, u8(d, 3));
        }
        send("TrSS", style);
        setDveTaken(dveTakenByTransition());
    };
    // Fade to black ------------------------------------------------------
    h["FtbA"] = [this](const QByteArray&) {
        QByteArray* state = m_store.find("FtbS", key(0));
        const QByteArray* params = m_store.find("FtbP", key(0));
        if (!state || !params || m_animations.contains("ftb")) return;
        bool toBlack = !u8(*state, 1);
        int rate = std::max(1, int(u8(*params, 1)));
        startAnimation("ftb", rate - 1,
            [this, rate](int frame) {
                QByteArray* s = m_store.find("FtbS", key(0));
                setU8(*s, 1, 0);
                setU8(*s, 2, 1);
                setU8(*s, 3, static_cast<quint8>(rate - 1 - frame));
                send("FtbS", s);
            },
            [this, rate, toBlack]() {
                QByteArray* s = m_store.find("FtbS", key(0));
                setU8(*s, 1, toBlack ? 1 : 0);
                setU8(*s, 2, 0);
                setU8(*s, 3, static_cast<quint8>(rate));
                send("FtbS", s);
            });
    };
    h["FtbC"] = [this](const QByteArray& d) {       // fade to black rate
        if (!(u8(d, 0) & 0x01)) return;
        QByteArray* params = m_store.find("FtbP", key(u8(d, 1)));
        QByteArray* state = m_store.find("FtbS", key(u8(d, 1)));
        if (!params || !state) return;
        quint8 rate = std::max<quint8>(1, u8(d, 2));
        setU8(*params, 1, rate);
        send("FtbP", params);
        if (!u8(*state, 2)) {
            QByteArray before = *state;
            setU8(*state, 3, rate);
            sendIfChanged("FtbS", state, before);
        }
    };

    // Upstream key ---------------------------------------------------------
    h["CKTp"] = [this](const QByteArray& d) {       // key type, fly enabled
        quint8 mask = u8(d, 0);
        QByteArray* props = m_store.find("KeBP", key(u8(d, 1), u8(d, 2)));
        if (!props) return;
        if (mask & 0x01) {
            quint8 type = u8(d, 3);
            if (type > kKeyTypeDVE || (type == kKeyTypeDVE && dveTakenByTransition())) return;
            if (type == kKeyTypeDVE && m_dveOwner != DveOwner::Keyer) {
                if (m_dveOwner == DveOwner::Transition) warn("DVE taken from Take");
                m_dveOwner = DveOwner::Keyer;
            }
            setU8(*props, 2, type);
            // Each key type remembers its own fill source.
            if (const QByteArray* memory = m_store.find("KBfT", key({ u8(d, 1), u8(d, 2), type })))
                setU16(*props, 6, u16(*memory, 4));
        }
        if (mask & 0x02) setU8(*props, 5, u8(d, 4) ? 1 : 0);
        send("KeBP", props);
    };
    h["CKeF"] = [this](const QByteArray& d) {       // key fill
        quint16 source = u16(d, 2);
        QByteArray* props = m_store.find("KeBP", key(u8(d, 0), u8(d, 1)));
        if (!props || !usableOnMixEffect(source)) return;
        setU16(*props, 6, source);
        if (QByteArray* memory = m_store.find("KBfT", key({ u8(d, 0), u8(d, 1), u8(*props, 2) }))) {
            setU16(*memory, 4, source);
            send("KBfT", memory);
        }
        send("KeBP", props);
    };
    h["CKeC"] = [this](const QByteArray& d) {       // key cut
        quint16 source = u16(d, 2);
        QByteArray* props = m_store.find("KeBP", key(u8(d, 0), u8(d, 1)));
        if (!props || !usableAsKeySource(source)) return;
        setU16(*props, 8, source);
        send("KeBP", props);
    };
    h["CKOn"] = [this](const QByteArray& d) {       // key on air
        if (QByteArray* onAir = m_store.find("KeOn", key(u8(d, 0), u8(d, 1)))) {
            setU8(*onAir, 2, u8(d, 2) ? 1 : 0);
            send("KeOn", onAir);
        }
    };
    h["CKMs"] = [this](const QByteArray& d) {       // key mask
        quint8 mask = u8(d, 0);
        QByteArray* props = m_store.find("KeBP", key(u8(d, 1), u8(d, 2)));
        if (!props) return;
        if (mask & 0x01) setU8(*props, 10, u8(d, 3));
        if (mask & 0x02) setU16(*props, 12, u16(d, 4));
        if (mask & 0x04) setU16(*props, 14, u16(d, 6));
        if (mask & 0x08) setU16(*props, 16, u16(d, 8));
        if (mask & 0x10) setU16(*props, 18, u16(d, 10));
        send("KeBP", props);
    };
    h["RFlK"] = [this](const QByteArray& d) {       // run flying key to A / B / full
        QByteArray* dve = m_store.find("KeDV", key(u8(d, 1), u8(d, 2)));
        if (!dve) return;
        quint8 frame = u8(d, 4);
        if (frame == 3) {                           // full: size 1, centred, no crop
            setU32(*dve, 4, 1000);
            setU32(*dve, 8, 1000);
            setU32(*dve, 12, 0);
            setU32(*dve, 16, 0);
            for (int at = 48; at < 56; ++at) setU8(*dve, at, 0);
        } else if (frame == 1 || frame == 2) {
            const QByteArray* stored = m_store.find("KKFP", key({ u8(d, 1), u8(d, 2), frame }));
            if (!stored) return;
            copyBytes(*dve, *stored, 4, 16);
            for (int i = 0; i < 8; ++i) setU8(*dve, 48 + i, u8(*stored, 44 + i));   // crop
        } else {
            return;
        }
        send("KeDV", dve);
        updateAtKeyFrames(u8(d, 1), u8(d, 2));
    };

    h["SFKF"] = [this](const QByteArray& d) {       // store the fly key as keyframe A / B / both
        QByteArray* stored = m_store.find("KeFS", key(u8(d, 0), u8(d, 1)));
        const QByteArray* dve = m_store.find("KeDV", key(u8(d, 0), u8(d, 1)));
        const int which = u8(d, 2);
        if (!stored || !dve || which < 1 || which > 3) return;
        for (int frame = 1; frame <= 2; ++frame)
            if (which & frame) setU8(*stored, 1 + frame, 1);
        QList<QByteArray*> changed;
        for (int frame = 1; frame <= 2; ++frame) {
            QByteArray* keyFrame = m_store.find("KKFP", key({ u8(d, 0), u8(d, 1), frame }));
            if (!(which & frame) || !keyFrame) continue;
            // KeDV -> KKFP: size/position/rotation, border widths..opacity,
            // border colour and light, mask edges (KKFP has no on/off flags).
            for (auto [from, to, size] : { std::tuple{ 4, 4, 20 }, std::tuple{ 28, 24, 9 },
                                           std::tuple{ 38, 34, 9 }, std::tuple{ 48, 44, 8 } })
                for (int i = 0; i < size; ++i) setU8(*keyFrame, to + i, u8(*dve, from + i));
            changed.append(keyFrame);
        }
        updateAtKeyFrames(u8(d, 0), u8(d, 1));
        for (QByteArray* keyFrame : changed) send("KKFP", keyFrame);
    };
    h["RFKF"] = [this](const QByteArray& d) {       // clear keyframe A / B / both: back to full size
        QByteArray* stored = m_store.find("KeFS", key(u8(d, 0), u8(d, 1)));
        const int which = u8(d, 2);
        if (!stored || which < 1 || which > 3) return;
        QList<QByteArray*> cleared;
        for (int frame = 1; frame <= 2; ++frame) {
            QByteArray* keyFrame = m_store.find("KKFP", key({ u8(d, 0), u8(d, 1), frame }));
            if (!(which & frame) || !keyFrame) continue;
            setU8(*stored, 1 + frame, 0);
            // The recorded cleared keyframe: size 1, centred, no border or
            // crop, light at 36 degrees, altitude 25 (bytes 3 and 33 keep leftovers).
            for (int i = 4; i < keyFrame->size(); ++i)
                if (i != 33) setU8(*keyFrame, i, 0);
            setU32(*keyFrame, 4, 1000);
            setU32(*keyFrame, 8, 1000);
            setU16(*keyFrame, 40, 360);
            setU8(*keyFrame, 42, 25);
            cleared.append(keyFrame);
        }
        updateAtKeyFrames(u8(d, 0), u8(d, 1));
        for (QByteArray* keyFrame : cleared) send("KKFP", keyFrame);
    };
    h["RACK"] = [this](const QByteArray& d) {       // reset advanced chroma key settings
        QByteArray* chroma = m_store.find("KACk", key(u8(d, 0), u8(d, 1)));
        if (!chroma) return;
        const QByteArray before = *chroma;
        const quint8 mask = u8(d, 3);
        if (mask & 0x01) { setU16(*chroma, 2, 0); setU16(*chroma, 4, 0); setU16(*chroma, 6, 500); }   // key adjustments
        if (mask & 0x02) { setU16(*chroma, 8, 0); setU16(*chroma, 10, 0); }                        // chroma correction
        if (mask & 0x04) {                                                                          // colour adjustments
            for (int at : { 12, 14, 18, 20, 22 }) setU16(*chroma, at, 0);
            setU16(*chroma, 16, 1000);
        }
        sendIfChanged("KACk", chroma, before);
    };

    // Downstream key -------------------------------------------------------
    h["CDsF"] = [this](const QByteArray& d) {       // DSK fill
        QByteArray* sources = m_store.find("DskB", key(u8(d, 0)));
        if (!sources || !usableOnMixEffect(u16(d, 2))) return;
        setU16(*sources, 2, u16(d, 2));
        send("DskB", sources);
    };
    h["CDsC"] = [this](const QByteArray& d) {       // DSK cut
        QByteArray* sources = m_store.find("DskB", key(u8(d, 0)));
        if (!sources || !usableAsKeySource(u16(d, 2))) return;
        setU16(*sources, 4, u16(d, 2));
        send("DskB", sources);
    };
    h["CDsL"] = [this](const QByteArray& d) {       // DSK on air
        QByteArray* state = m_store.find("DskS", key(u8(d, 0)));
        if (!state) return;
        bool on = u8(d, 1);
        setU8(*state, 1, on ? 1 : 0);
        setU8(*state, 4, on ? 0 : 1);               // next auto goes towards on air
        send("DskP", m_store.find("DskP", key(u8(d, 0))));
        send("DskS", state);
    };
    h["CDsT"] = [this](const QByteArray& d) {       // DSK tie
        if (QByteArray* props = m_store.find("DskP", key(u8(d, 0)))) {
            setU8(*props, 1, u8(d, 1) ? 1 : 0);
            send("DskP", props);
        }
    };
    h["CDsR"] = [this](const QByteArray& d) {       // DSK rate (0 is stored as 1)
        QByteArray* props = m_store.find("DskP", key(u8(d, 0)));
        QByteArray* state = m_store.find("DskS", key(u8(d, 0)));
        if (!props || !state) return;
        quint8 rate = std::max<quint8>(1, u8(d, 1));
        setU8(*props, 2, rate);
        send("DskP", props);
        if (!u8(*state, 2)) {
            QByteArray before = *state;
            setU8(*state, 5, rate);
            sendIfChanged("DskS", state, before);
        }
    };
    h["CDsG"] = [this](const QByteArray& d) {       // DSK pre-multiplied, clip, gain, invert
        quint8 mask = u8(d, 0);
        QByteArray* props = m_store.find("DskP", key(u8(d, 1)));
        if (!props) return;
        if (mask & 0x01) setU8(*props, 3, u8(d, 2));
        if (mask & 0x02) setU16(*props, 4, u16(d, 4));
        if (mask & 0x04) setU16(*props, 6, u16(d, 6));
        if (mask & 0x08) setU8(*props, 8, u8(d, 8));
        send("DskP", props);
    };
    h["CDsM"] = [this](const QByteArray& d) {       // DSK mask
        quint8 mask = u8(d, 0);
        QByteArray* props = m_store.find("DskP", key(u8(d, 1)));
        if (!props) return;
        if (mask & 0x01) setU8(*props, 9, u8(d, 2));
        if (mask & 0x02) setU16(*props, 10, u16(d, 4));
        if (mask & 0x04) setU16(*props, 12, u16(d, 6));
        if (mask & 0x08) setU16(*props, 14, u16(d, 8));
        if (mask & 0x10) setU16(*props, 16, u16(d, 10));
        send("DskP", props);
    };
    h["DDsA"] = [this](const QByteArray& d) {       // DSK auto transition
        quint8 dsk = u8(d, 1);
        QByteArray* state = m_store.find("DskS", key(dsk));
        const QByteArray* props = m_store.find("DskP", key(dsk));
        QString id = QString("dsk%1").arg(dsk);
        if (!state || !props || u8(*state, 2) || m_animations.contains(id)) return;
        bool on = u8(*state, 1);
        if ((u8(d, 0) & 0x01) && bool(u8(d, 2)) == on) return;   // already there
        bool target = !on;
        int rate = std::max(1, int(u8(*props, 2)));
        startAnimation(id, rate - 1,
            [this, dsk, rate, target](int frame) {
                QByteArray* s = m_store.find("DskS", key(dsk));
                if (target) setU8(*s, 1, 1);
                setU8(*s, 2, 1);
                setU8(*s, 3, 1);
                setU8(*s, 5, static_cast<quint8>(rate - frame));
                send("DskS", s);
            },
            [this, dsk, rate, target]() {
                QByteArray* s = m_store.find("DskS", key(dsk));
                setU8(*s, 1, target ? 1 : 0);
                setU8(*s, 2, 0);
                setU8(*s, 3, 0);
                setU8(*s, 4, target ? 0 : 1);
                setU8(*s, 5, static_cast<quint8>(rate));
                send("DskP", m_store.find("DskP", key(dsk)));
                send("DskS", s);
            });
    };

    // Macros ---------------------------------------------------------------
    h["MAct"] = [this](const QByteArray& d) {
        quint16 index = u16(d, 0);
        switch (u8(d, 2)) {
        case 0:                                     // run (starts after the rest of the packet)
            if (index >= m_pool.size() || !m_pool[index].used || m_recording.active) return;
            if (m_run.index >= 0) finishMacro();
            setRunStatus(true, false, index);
            m_pendingMacro = index;
            return;
        case 1:                                     // stop
            if (m_pendingMacro >= 0) {
                m_pendingMacro = -1;
                setRunStatus(false, false, -1);
            } else if (m_run.index >= 0) {
                finishMacro();
            }
            return;
        case 2:                                     // stop recording: store the macro
            if (!m_recording.active) return;
            {
                int slot = m_recording.index;
                m_pool[slot] = m_recording.macro;
                m_pool[slot].used = true;
                m_pool[slot].bytes = Macro::encode(m_pool[slot].ops);
                m_recording = {};
                publishMacro(slot);
                setRecordingStatus(false, slot);
                emit log(QString("Macro %1 \"%2\" recorded: %3 steps")
                         .arg(slot + 1).arg(m_pool[slot].name).arg(m_pool[slot].ops.size()));
            }
            return;
        case 3:                                     // recording: wait for the user here
            if (m_recording.active) m_recording.macro.ops.append({ MacroOp::UserWait, {}, {}, 0 });
            return;
        case 4:                                     // continue after a user wait
            if (m_run.index >= 0 && m_run.waitingForUser) {
                m_run.waitingForUser = false;
                setRunStatus(true, false, m_run.index);
                continueMacro();
            }
            return;
        case 5:                                     // delete
            if (index >= m_pool.size() || index == m_run.index) return;
            m_pool[index] = Macro();
            publishMacro(index);
            return;
        }
    };
    h["MRCP"] = [this](const QByteArray& d) {       // macro loop
        QByteArray* status = m_store.find("MRPr");
        if (!status || !(u8(d, 0) & 0x01)) return;
        setU8(*status, 1, u8(d, 1) ? 1 : 0);
        send("MRPr", status);
    };
    h["MSRc"] = [this](const QByteArray& d) {       // start recording: index, name, description
        quint16 index = u16(d, 0);
        int nameLength = u16(d, 2), descLength = u16(d, 4);
        if (index >= m_pool.size() || m_recording.active || m_run.index >= 0) return;
        m_recording.active = true;
        m_recording.index = index;
        m_recording.macro = Macro();
        m_recording.macro.name = QString::fromUtf8(d.mid(6, nameLength));
        m_recording.macro.description = QString::fromUtf8(d.mid(6 + nameLength, descLength));
        setRecordingStatus(true, index);
    };
    h["MSlp"] = [this](const QByteArray& d) {       // recording: pause for N frames
        if (m_recording.active) m_recording.macro.ops.append({ MacroOp::Wait, {}, {}, u16(d, 2) });
    };
    h["CMPr"] = [this](const QByteArray& d) {       // rename / describe a macro
        quint8 mask = u8(d, 0);
        quint16 index = u16(d, 2);
        int nameLength = u16(d, 4), descLength = u16(d, 6);
        if (index >= m_pool.size()) return;
        if (mask & 0x01) m_pool[index].name = QString::fromUtf8(d.mid(8, nameLength));
        if (mask & 0x02) m_pool[index].description = QString::fromUtf8(d.mid(8 + nameLength, descLength));
        publishMacro(index);
    };

    // Media ----------------------------------------------------------------
    h["LOCK"] = [this](const QByteArray& d) {       // media pool lock
        quint16 store = u16(d, 0);
        QByteArray* lock = m_store.find("LKST", key16(store));
        if (!lock) return;
        bool locked = u8(d, 2);
        if (locked) {
            QByteArray obtained(4, '\0');
            setU16(obtained, 0, store);
            reply("LKOB", obtained);
        }
        setU8(*lock, 2, locked ? 1 : 0);
        send("LKST", lock);
    };
    h["MPSS"] = [this](const QByteArray& d) {       // media player source
        quint8 mask = u8(d, 0);
        QByteArray* source = m_store.find("MPCE", key(u8(d, 1)));
        const QByteArray* pool = m_store.find("_mpl");     // stills, clips
        if (!source) return;
        if (mask & 0x01) setU8(*source, 1, u8(d, 2));
        if ((mask & 0x02) && pool && u8(d, 3) < u8(*pool, 0)) setU8(*source, 2, u8(d, 3));
        if ((mask & 0x04) && pool && u8(d, 4) < u8(*pool, 1)) setU8(*source, 3, u8(d, 4));
        send("MPCE", source);
    };
    // Accepted without a reply, as the real switcher does for these.
    h["SCPS"] = [](const QByteArray&) {};           // media player play state (stills: nothing to do)
    h["CCmd"] = [](const QByteArray&) {};           // camera control (no camera attached)

    // Fade to black: cut straight to / from black -------------------------
    h["FCut"] = [this](const QByteArray& d) {
        QByteArray* state = m_store.find("FtbS", key(u8(d, 0)));
        if (!state || m_animations.contains("ftb")) return;
        const QByteArray before = *state;
        setU8(*state, 1, u8(d, 1) ? 1 : 0);
        setU8(*state, 2, 0);
        sendIfChanged("FtbS", state, before);
    };

    // Fairlight audio resets ----------------------------------------------
    // Defaults as the ATEM Mini's resets recorded them.
    h["RICD"] = [this](const QByteArray& d) {       // reset dynamics: 1 makeup, 2 expander, 4 compressor, 8 limiter
        const quint8 mask = u8(d, 17);
        auto reset = [&](const char* name, std::initializer_list<std::tuple<int, int, qint32>> values) {
            QByteArray* f = findAudioSource(name, d, 0);
            if (!f) return;
            const QByteArray before = *f;
            for (auto [at, size, v] : values) size == 4 ? setU32(*f, at, static_cast<quint32>(v)) : setU16(*f, at, static_cast<quint16>(v));
            sendIfChanged(name, f, before);
        };
        if (mask & 0x01) reset("FASP", { { 36, 4, 0 } });
        if (mask & 0x02) reset("AIXP", { { 20, 4, -4500 }, { 24, 2, 1800 }, { 26, 2, 110 }, { 28, 4, 140 }, { 32, 4, 0 }, { 36, 4, 9300 } });
        if (mask & 0x04) reset("AICP", { { 20, 4, -3500 }, { 24, 2, 200 }, { 28, 4, 140 }, { 32, 4, 0 }, { 36, 4, 9300 } });
        if (mask & 0x08) reset("AILP", { { 20, 4, -1200 }, { 24, 4, 71 }, { 28, 4, 0 }, { 32, 4, 9300 } });
    };
    h["RICE"] = [this](const QByteArray& d) {       // reset the EQ (1) or one band (2, band at 17)
        // band: enabled, shape, range, frequency, Q (gain resets to 0)
        static constexpr int bands[][5] = { { 0, 16, 1, 46, 71 },   { 1, 1, 1, 49, 80 },  { 1, 4, 2, 171, 230 },
                                            { 1, 4, 4, 798, 230 }, { 1, 32, 8, 7260, 80 }, { 0, 2, 8, 12900, 71 } };
        const quint8 mask = u8(d, 0);
        for (int band = 0; band < 6; ++band) {
            if (!(mask & 0x01) && !((mask & 0x02) && u8(d, 17) == band)) continue;
            QByteArray* f = findAudioSource("AEBP", d, 2, band);
            if (!f) continue;
            const QByteArray before = *f;
            setU8(*f, 17, static_cast<quint8>(bands[band][0]));
            setU8(*f, 19, static_cast<quint8>(bands[band][1]));
            setU8(*f, 21, static_cast<quint8>(bands[band][2]));
            setU32(*f, 24, static_cast<quint32>(bands[band][3]));
            setU32(*f, 28, 0);
            setU16(*f, 32, static_cast<quint16>(bands[band][4]));
            sendIfChanged("AEBP", f, before);
        }
    };
    // File transfers --------------------------------------------------------
    // Download: FTSU -> FTDa chunks, each acknowledged with FTUA -> FTDC.
    // Upload: FTSD -> FTCD; FTDa chunks and FTFD (name, description, hash) ->
    // the new MPrp / MPfe, FTDC. Errors: FTDE. Answers go to the asking client.
    h["FTSU"] = [this](const QByteArray& d) {
        const quint16 id = u16(d, 0);
        Transfer t;
        t.store = u16(d, 2);
        t.index = static_cast<int>(u32(d, 4));
        if (t.store == kMacroStore && t.index < m_pool.size() && m_pool[t.index].used) {
            const Macro& m = m_pool[t.index];
            t.data = m.bytes.isEmpty() ? Macro::encode(m.ops) : m.bytes;
        } else if (t.store == kStillStore && stillValid(t.index)) {
            t.data = stillBytes(t.index);
            noteStillTransfer(t.index);
        } else {
            QByteArray error(4, '\0');
            setU16(error, 0, id);
            setU8(error, 2, 2);                        // not found
            reply("FTDE", error);
            return;
        }
        m_transfers[id] = t;
        sendChunks(id);
    };
    h["FTUA"] = [this](const QByteArray& d) {       // chunk received
        const quint16 id = u16(d, 0);
        auto it = m_transfers.find(id);
        if (it == m_transfers.end() || it->upload) return;
        ++it->acked;
        if (it->acked >= it->sent && it->sent * kChunkSize >= it->data.size()) {
            QByteArray done(4, '\0');
            setU16(done, 0, id);
            setU8(done, 2, 1);
            reply("FTDC", done);
            m_transfers.erase(it);
            return;
        }
        sendChunks(id);
    };
    h["FTAD"] = [this](const QByteArray& d) { m_transfers.remove(u16(d, 0)); };   // abort
    h["FTSD"] = [this](const QByteArray& d) {
        const quint16 id = u16(d, 0);
        Transfer t;
        t.upload = true;
        t.store = u16(d, 2);
        t.index = static_cast<int>(u32(d, 4));
        t.size = u32(d, 8);
        const QByteArray* pool = m_store.find("_mpl");
        const bool ok = t.store == kMacroStore ? t.index < m_pool.size()
                      : t.store == kStillStore && pool && t.index < u8(*pool, 0);
        if (!ok) {
            QByteArray error(4, '\0');
            setU16(error, 0, id);
            setU8(error, 2, 2);
            reply("FTDE", error);
            return;
        }
        m_transfers[id] = t;
        if (t.store == kStillStore) noteStillTransfer(t.index);
        QByteArray go(12, '\0');                     // id, 2, _, chunk size, chunk count (as recorded)
        setU16(go, 0, id);
        setU16(go, 2, 2);
        setU16(go, 6, 0x0574);
        setU16(go, 8, 800);
        reply("FTCD", go);
    };
    h["FTDa"] = [this](const QByteArray& d) {       // upload data
        const quint16 id = u16(d, 0);
        auto it = m_transfers.find(id);
        if (it == m_transfers.end() || !it->upload) return;
        it->data += d.mid(4, u16(d, 2));
        finishUpload(id);
    };
    h["FTFD"] = [this](const QByteArray& d) {       // upload: name, description, hash
        const quint16 id = u16(d, 0);
        auto it = m_transfers.find(id);
        if (it == m_transfers.end() || !it->upload) return;
        auto text = [&](int at, int size) { QByteArray b = d.mid(at, size); return b.left(b.indexOf('\0') < 0 ? size : b.indexOf('\0')); };
        it->name = text(2, 64);
        it->description = text(66, 128);
        it->hash = d.mid(194, 16);
        it->described = true;
        finishUpload(id);
    };

    // Media pool stills: rename, clear, clear all, capture the program.
    h["SMPS"] = [this](const QByteArray& d) {
        const int index = u8(d, 0);
        QByteArray* f = stillField(index);
        if (!f) return;
        QByteArray name = d.mid(1, 63);
        if (name.indexOf('\0') >= 0) name.truncate(name.indexOf('\0'));
        publishStill(index, u8(*f, 4), f->mid(5, 16), name);
    };
    h["CSTL"] = [this](const QByteArray& d) {
        if (stillField(u8(d, 0))) publishStill(u8(d, 0), false, {}, {});
    };
    h["CLMP"] = [this](const QByteArray&) {
        noteStillTransfer(0xff);
        const QByteArray* pool = m_store.find("_mpl");
        for (int i = 0; pool && i < u8(*pool, 0); ++i) publishStill(i, false, {}, {});
    };
    h["Capt"] = [this](const QByteArray&) {
        const QByteArray* pool = m_store.find("_mpl");
        for (int i = 0; pool && i < u8(*pool, 0); ++i) {
            if (stillValid(i)) continue;
            QByteArray name = QString("Capture %1").arg(++m_captures).toUtf8();
            QByteArray hash = QCryptographicHash::hash(name + QByteArray::number(i), QCryptographicHash::Md5);
            publishStill(i, true, hash, name);
            return;
        }
    };

    // Accepted, nothing to answer: peak level resets, time code request.
    for (const char* name : { "RFIP", "RFLP", "TiRq" }) h[name] = [](const QByteArray&) {};
    // Refused on the ATEM Mini (recorded: nothing changes, no answer).
    for (const char* name : { "TlMe" }) h[name] = [](const QByteArray&) {};
}

// ── File transfers ──────────────────────────────────────────

// Sends download chunks while fewer than kWindow are unacknowledged.
void Device::sendChunks(quint16 id) {
    Transfer& t = m_transfers[id];
    const int chunks = std::max(1, static_cast<int>((t.data.size() + kChunkSize - 1) / kChunkSize));
    while (t.sent < chunks && t.sent - t.acked < kWindow) {
        const QByteArray part = t.data.mid(t.sent * kChunkSize, kChunkSize);
        QByteArray chunk(4, '\0');
        setU16(chunk, 0, id);
        setU16(chunk, 2, static_cast<quint16>(part.size()));
        reply("FTDa", chunk + part);
        ++t.sent;
    }
}

namespace {
// Size of a still once its run-length encoding is undone: 8-byte words;
// FE FE FE FE FE FE FE FE, count, word repeats word count times.
qint64 unpackedSize(const QByteArray& rle) {
    static const QByteArray kRun(8, '\xfe');
    qint64 size = 0;
    for (int at = 0; at + 8 <= rle.size();) {
        if (rle.mid(at, 8) == kRun && at + 24 <= rle.size()) {
            size += 8 * static_cast<qint64>(static_cast<quint64>(u32(rle, at + 8)) << 32 | u32(rle, at + 12));
            at += 24;
        } else {
            size += 8;
            at += 8;
        }
    }
    return size;
}
} // namespace

// An upload is complete once it is described (FTFD) and all its data is in.
void Device::finishUpload(quint16 id) {
    auto it = m_transfers.find(id);
    if (it == m_transfers.end() || !it->described) return;
    const Transfer& t = *it;
    const qint64 have = t.store == kStillStore ? unpackedSize(t.data) : t.data.size();
    if (have < t.size) return;

    if (t.store == kMacroStore) {
        QList<MacroOp> ops;
        // Bytes that aren't a macro leave the slot as it was (recorded: still "done").
        if (Macro::decode(t.data.left(static_cast<int>(t.size)), &ops)) {
            Macro& m = m_pool[t.index];
            m = Macro();
            m.used = true;
            m.name = QString::fromUtf8(t.name);
            m.description = QString::fromUtf8(t.description);
            m.ops = ops;
            m.bytes = t.data.left(static_cast<int>(t.size));
            publishMacroSteps(t.index);
        }
    } else {
        m_stillData[t.index] = t.data;
        publishStill(t.index, true, t.hash, t.name);
    }
    QByteArray done(4, '\0');
    setU16(done, 0, id);
    reply("FTDC", done);
    m_transfers.erase(it);
}

// A new macro's properties arrive in three steps on the real switcher: used,
// then the name, then name and description.
void Device::publishMacroSteps(int index) {
    Macro m = m_pool[index];
    const QString name = m.name, description = m.description;
    m_pool[index].name.clear();
    m_pool[index].description.clear();
    publishMacro(index);
    m_pool[index].name = name;
    publishMacro(index);
    m_pool[index].description = description;
    publishMacro(index);
}

// MPfe: type (0 still), _, index, valid, MD5 hash, _, name length, name.
QByteArray* Device::stillField(int index) {
    for (QByteArray* f : m_store.all("MPfe"))
        if (u8(*f, 0) == 0 && u16(*f, 2) == index) return f;
    return nullptr;
}

bool Device::stillValid(int index) const {
    const QByteArray* f = const_cast<Device*>(this)->stillField(index);   // FieldStore::all() isn't const
    return f && u8(*f, 4);
}

// The still's data as the switcher sends it: uploaded bytes, or for stills
// from the profile (their pictures aren't recorded) one flat colour.
QByteArray Device::stillBytes(int index) const {
    if (m_stillData.contains(index)) return m_stillData[index];
    QByteArray rle(8, '\xfe');
    QByteArray count(8, '\0');
    setU32(count, 4, 1920 * 1080 * 4 / 8);
    return rle + count + QByteArray::fromHex("0408004004080040");
}

void Device::publishStill(int index, bool valid, const QByteArray& hash, const QByteArray& name) {
    QByteArray* f = stillField(index);
    if (!f) return;
    QByteArray still(24, '\0');
    setU16(still, 2, static_cast<quint16>(index));
    if (valid) {
        setU8(still, 4, 1);
        for (int i = 0; i < 16; ++i) setU8(still, 5 + i, u8(hash, i));
        setU8(still, 23, static_cast<quint8>(name.size()));
        still += name;
        while (still.size() % 4) still.append('\0');
    } else {
        m_stillData.remove(index);
    }
    *f = still;
    send("MPfe", f);
}

// LKST byte 3 holds the still last transferred (0xff after clearing the
// pool) and goes out with the next lock change. With 0xff there the SDK
// reports no "lock busy" on locking (recorded); with 0 it does.
void Device::noteStillTransfer(int index) {
    if (QByteArray* lock = m_store.find("LKST", key16(kStillStore))) setU8(*lock, 3, static_cast<quint8>(index));
}

// KeFS byte 6: the keyframes (1 A, 2 B, 4 full) the DVE key is at now, by
// size, position and crop. Recorded after storing, clearing and running to a
// keyframe (not after plain DVE changes).
void Device::updateAtKeyFrames(int me, int keyIndex) {
    QByteArray* stored = m_store.find("KeFS", key(me, keyIndex));
    const QByteArray* dve = m_store.find("KeDV", key(me, keyIndex));
    if (!stored || !dve) return;
    auto same = [&](const QByteArray& frame, int crop) {
        for (int i = 4; i < 20; ++i) if (u8(*dve, i) != u8(frame, i)) return false;
        for (int i = 0; i < 8; ++i) if (u8(*dve, 48 + i) != u8(frame, crop + i)) return false;
        return true;
    };
    QByteArray full(56, '\0');
    setU32(full, 4, 1000);
    setU32(full, 8, 1000);
    int at = same(full, 48) ? 4 : 0;
    for (int frame = 1; frame <= 2; ++frame)
        if (const QByteArray* kf = m_store.find("KKFP", key({ me, keyIndex, frame })); kf && same(*kf, 44)) at |= frame;
    setU8(*stored, 6, static_cast<quint8>(at));
    send("KeFS", stored);
}

// Fairlight fields are keyed by input (2 bytes at 0) and source (8 bytes at
// 8); an EQ band also by its band number (at 16). inputAt: where the command
// has the input (0, or 2 after a mask).
QByteArray* Device::findAudioSource(const char* name, const QByteArray& d, int inputAt, int band) {
    for (QByteArray* f : m_store.all(name)) {
        bool match = u16(*f, 0) == u16(d, inputAt) && (band < 0 || u8(*f, 16) == band);
        for (int i = 8; i < 16 && match; ++i) match = u8(*f, i) == u8(d, i);
        if (match) return f;
    }
    return nullptr;
}

} // namespace emu
