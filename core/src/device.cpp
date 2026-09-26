#include "device.h"

#include <QFile>
#include <QTextStream>
#include <algorithm>

namespace emu {

namespace {

// Offsets are into the command payload and the state field payload. They
// were read off the wire capture of the real ATEM Mini (sweep/golden) and
// agree with every command/echo pair recorded there.

constexpr quint8 kKeyTypeLuma = 0;
constexpr quint8 kKeyTypeDVE = 3;
constexpr quint8 kStyleDVE = 3;

// CKDV bit -> (command offset, KeDV offset, size)
struct Span { int bit, cmd, field, size; };
constexpr Span kDveSpans[] = {
    { 0, 8, 4, 4 },   { 1, 12, 8, 4 },  { 2, 16, 12, 4 }, { 3, 20, 16, 4 },  // size x/y, position x/y
    { 4, 24, 20, 4 },                                                       // rotation
    { 5, 28, 24, 1 }, { 6, 29, 25, 1 }, { 7, 30, 26, 1 },                   // border, shadow, bevel
    { 8, 32, 28, 2 }, { 9, 34, 30, 2 },                                     // border width out/in
    { 10, 36, 32, 1 }, { 11, 37, 33, 1 }, { 12, 38, 34, 1 }, { 13, 39, 35, 1 },  // softness, bevel
    { 14, 40, 36, 1 },                                                      // border opacity
    { 15, 42, 38, 2 }, { 16, 44, 40, 2 }, { 17, 46, 42, 2 },                // border hue/sat/luma
    { 18, 48, 44, 2 }, { 19, 50, 46, 1 },                                   // light direction/altitude
    { 20, 51, 47, 1 }, { 21, 52, 48, 2 }, { 22, 54, 50, 2 }, { 23, 56, 52, 2 }, { 24, 58, 54, 2 },  // mask
    { 25, 60, 56, 1 },                                                      // fly rate
};

// CTWp bit -> (command offset, TWpP offset, size)
constexpr Span kWipeSpans[] = {
    { 0, 3, 1, 1 },   { 1, 4, 2, 1 },   { 2, 6, 4, 2 },   { 3, 8, 6, 2 },    // rate, pattern, width, fill
    { 4, 10, 8, 2 },  { 5, 12, 10, 2 }, { 6, 14, 12, 2 }, { 7, 16, 14, 2 },  // symmetry, softness, x, y
    { 8, 18, 16, 1 }, { 9, 19, 17, 1 },                                     // reverse, flip-flop
};

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

    m_macros.clear();
    QFile macros(profileDir + "/macros.txt");
    if (macros.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&macros);
        while (!in.atEnd()) {
            QStringList parts = in.readLine().trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() != 3 || parts[0].startsWith('#')) continue;
            m_macros[parts[0].toInt()].append({ parts[1].toLatin1(), QByteArray::fromHex(parts[2].toLatin1()) });
        }
    }
    emit log(QString("Profile %1: %2 fields, %3 recorded macros, %4 ms per frame")
             .arg(profileDir).arg(m_store.size()).arg(m_macros.size()).arg(m_frameTimer.interval()));
    return true;
}

QString Device::productName() const {
    const QByteArray* pin = m_store.find("_pin");
    return pin ? QString::fromUtf8(pin->left(44).constData()) : QString();
}

FieldList Device::handle(const QByteArray& name, const QByteArray& data) {
    m_out.clear();
    auto it = m_handlers.constFind(name);
    if (it == m_handlers.constEnd()) {
        emit log("unhandled command " + QString::fromLatin1(name) + " " + QString::fromLatin1(data.toHex()));
        return {};
    }
    it.value()(data);
    updateTally();
    return std::exchange(m_out, {});
}

// Stored macros replay the fields the real macro changed, all at once.
FieldList Device::endOfPacket() {
    if (m_pendingMacro < 0) return {};
    m_out.clear();
    for (const Field& f : m_macros.value(m_pendingMacro))
        if (m_store.replace(f)) m_out.append(f);
    m_pendingMacro = -1;
    if (QByteArray* status = m_store.find("MRPr")) {
        setU8(*status, 0, 0);
        setU16(*status, 2, 0xffff);
        send("MRPr", status);
    }
    updateTally();
    return std::exchange(m_out, {});
}

void Device::send(const char* name, const QByteArray* data) {
    if (data) m_out.append({ QByteArray(name), *data });
}

void Device::sendIfChanged(const char* name, QByteArray* data, const QByteArray& before) {
    if (data && *data != before) send(name, data);
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
    if (!m_out.isEmpty()) emit fieldsChanged(std::exchange(m_out, {}));
}

// ── Command handlers ─────────────────────────────────────────

void Device::registerHandlers() {
    auto& h = m_handlers;

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
    h["CTWp"] = [this](const QByteArray& d) {       // wipe parameters
        quint16 mask = u16(d, 0);
        QByteArray* wipe = m_store.find("TWpP", key(u8(d, 2)));
        if (!wipe) return;
        for (const Span& s : kWipeSpans)
            if (mask & (1u << s.bit)) {
                QByteArray value = d.mid(s.cmd, s.size);
                value.resize(s.size, '\0');
                wipe->replace(s.field, s.size, value);
            }
        if (u8(*wipe, 1) == 0) setU8(*wipe, 1, 1);  // rate 0 is stored as 1
        send("TWpP", wipe);
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
    h["CKDV"] = [this](const QByteArray& d) {       // DVE: fly position/size, crop, border, ...
        quint32 mask = u32(d, 0);
        QByteArray* dve = m_store.find("KeDV", key(u8(d, 4), u8(d, 5)));
        if (!dve) return;
        for (const Span& s : kDveSpans) {
            if (!(mask & (1u << s.bit))) continue;
            QByteArray value = d.mid(s.cmd, s.size);
            value.resize(s.size, '\0');
            if (s.bit <= 1 && i32(value, 0) < 0) value.fill('\0');   // negative size -> 0
            if (s.bit == 15 || s.bit == 18) {                        // hue, light direction wrap at 360.0
                QByteArray wrapped(2, '\0');
                setU16(wrapped, 0, static_cast<quint16>(u16(value, 0) % 3600));
                value = wrapped;
            }
            dve->replace(s.field, s.size, value);
        }
        send("KeDV", dve);
    };
    h["RFlK"] = [this](const QByteArray& d) {       // run flying key to A / B / full
        QByteArray* dve = m_store.find("KeDV", key(u8(d, 1), u8(d, 2)));
        if (!dve) return;
        quint8 frame = u8(d, 4);
        if (frame == 3) {
            setU32(*dve, 4, 1000);
            setU32(*dve, 8, 1000);
            setU32(*dve, 12, 0);
            setU32(*dve, 16, 0);
        } else if (frame == 1 || frame == 2) {
            const QByteArray* stored = m_store.find("KKFP", key({ u8(d, 1), u8(d, 2), frame }));
            if (!stored) return;
            copyBytes(*dve, *stored, 4, 16);
        } else {
            return;
        }
        send("KeDV", dve);
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
        quint8 action = u8(d, 2);
        QByteArray* status = m_store.find("MRPr");
        if (!status) return;
        if (action == 1) {                          // stop: only a macro not yet run
            if (m_pendingMacro < 0) return;
            m_pendingMacro = -1;
            setU8(*status, 0, 0);
            setU16(*status, 2, 0xffff);
            send("MRPr", status);
            return;
        }
        if (action != 0) return;                    // continue etc.: nothing waits here
        const QByteArray* props = m_store.find("MPrp", key16(index));
        if (!props || !u8(*props, 2)) return;       // empty slot: nothing happens
        setU8(*status, 0, 1);
        setU16(*status, 2, index);
        send("MRPr", status);
        m_pendingMacro = index;
    };
    h["MRCP"] = [this](const QByteArray& d) {       // macro loop
        QByteArray* status = m_store.find("MRPr");
        if (!status || !(u8(d, 0) & 0x01)) return;
        setU8(*status, 1, u8(d, 1) ? 1 : 0);
        send("MRPr", status);
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
            m_out.append({ "LKOB", obtained });
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
}

} // namespace emu
