// The emulated switcher: state fields loaded from a profile (the connect
// dump recorded from the real device) plus a handler for each command the
// SDK sends. Handlers change fields the way the real ATEM Mini does, as
// recorded by atem-sweep, and return the fields that changed.
#pragma once

#include "fields.h"

#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
#include <functional>

namespace emu {

namespace setters { struct Setter; }

// One step of a stored macro.
struct MacroOp {
    enum Kind {
        Command,    // a command, run through its handler (recorded macros)
        Patch,      // a state field to set (macros recorded from a real switcher by atem-sweep)
        Wait,       // pause for `frames` video frames
        UserWait,   // pause until "continue"
    };
    Kind kind = Command;
    QByteArray name;   // command / field name
    QByteArray data;
    int frames = 0;
};

struct Macro {
    bool used = false;
    QString name;
    QString description;
    QList<MacroOp> ops;
    QByteArray bytes;   // the macro as the switcher stores and transfers it (empty: made from ops)

    QJsonArray opsToJson() const;
    static QList<MacroOp> opsFromJson(const QJsonArray& json);
    // The switcher's macro format: little-endian ops [length][op id][values].
    // Only ops recorded from the real switcher are known (preview input,
    // pause, user wait); other commands and state patches are left out.
    static QByteArray encode(const QList<MacroOp>& ops);
    // False if the bytes are not a well-formed op list. Unknown ops are kept
    // out of *ops (they do nothing when the macro runs).
    static bool decode(const QByteArray& bytes, QList<MacroOp>* ops);
};

// The switcher state in SDK units, for a UI to display.
struct SwitcherView {
    quint16 program = 0, preview = 0;
    bool previewLive = false;
    bool inTransition = false;
    double transitionPosition = 0;          // 0..1
    int transitionStyle = 0;                // 0 mix, 1 dip, 2 wipe, 3 DVE, 4 stinger
    int nextSelection = 1;                  // bit 0 background, bit 1 key 1

    bool keyOnAir = false;
    int keyType = 0;                        // 0 luma, 1 chroma, 2 pattern, 3 DVE
    bool keyCanUseDve = true;               // false while the DVE transition holds the DVE
    quint16 keyFill = 0, keyCut = 0;
    double sizeX = 1, sizeY = 1, positionX = 0, positionY = 0;
    bool borderEnabled = false;
    double borderWidth = 0, borderHue = 0, borderSaturation = 0, borderLuma = 0, borderOpacity = 1;
    bool masked = false;
    double maskTop = 0, maskBottom = 0, maskLeft = 0, maskRight = 0;

    bool fadeFullyBlack = false, fadeInTransition = false;
    int fadeFramesRemaining = 0, fadeRate = 25;

    bool dskOnAir = false, dskInTransition = false;
    int dskFramesRemaining = 0, dskRate = 25;
    quint16 dskFill = 0, dskCut = 0;

    bool macroRunning = false, macroWaiting = false, macroLoop = false;
    int macroIndex = -1;
    bool macroRecording = false;
    int macroRecordingIndex = -1;
};

struct InputInfo {
    quint16 id = 0;
    QString longName, shortName;
    bool namesDefault = true;
};

class Device : public QObject {
    Q_OBJECT
public:
    explicit Device(QObject* parent = nullptr);

    // Loads dump.txt (required) and macros.txt (optional) from profileDir.
    bool load(const QString& profileDir, QString* error);

    QString productName() const;
    // The full state, in dump order, for a newly connected client.
    FieldList connectDump() const { return m_store.dump(); }
    // The state field's content now (for answers sent at the next frame);
    // nullptr for fields that aren't state (warnings, transfers).
    const QByteArray* current(const Field& field) const { return m_store.findInstance(field); }
    // Time: the switcher's time code (hours, minutes, seconds, frames, then
    // 00 00 03 e8 as recorded), sent at the start of every state packet.
    Field timeCode() const;

    // Applies one command. Returns the fields to send to every client; the
    // switcher sends nothing back for commands it rejects.
    FieldList handle(const QByteArray& name, const QByteArray& data);
    bool knows(const QByteArray& name) const { return m_handlers.contains(name); }
    // Called after all commands of a packet: a macro started in the packet
    // runs now, so a stop in the same packet cancels it (as on the device).
    FieldList endOfPacket();

    // For a local UI: runs a command exactly as if a client had sent it, and
    // sends the resulting changes to every client.
    void apply(const QByteArray& name, const QByteArray& data);

    // Reading the state
    SwitcherView view() const;
    QList<InputInfo> inputs() const;
    // Colour generator N (1 or 2) as hue (degrees), saturation, luma (0..1).
    bool colorGenerator(int index, double* hue, double* saturation, double* luma) const;
    double frameIntervalMs() const { return m_frameTimer.interval(); }

    // Macro pool (the switcher's stored macros)
    int macroCount() const { return m_pool.size(); }
    const Macro& macro(int index) const { return m_pool[index]; }
    // Stores a macro from the local UI and tells every client.
    void setMacro(int index, const Macro& macro);

signals:
    // Fields changed outside a command (transitions, macros); for the server.
    void fieldsChanged(const emu::FieldList& fields);
    // Anything changed (commands, frames, macros); for a UI to refresh.
    void stateChanged();
    void macroStarted(int index);
    void macroFinished(int index);
    void macroPoolChanged(int index);   // a slot was stored, renamed or deleted
    void log(const QString& message);

private:
    using Handler = std::function<void(const QByteArray&)>;
    void registerHandlers();

    // Output of the handler that is running (or of the current frame).
    void send(const char* name, const QByteArray* data);
    void sendIfChanged(const char* name, QByteArray* data, const QByteArray& before);
    void emitOutput();   // output produced outside handle(): broadcast it
    void applySetter(const setters::Setter& setter, const QByteArray& data);
    QByteArray* findAudioSource(const char* name, const QByteArray& data, int inputAt, int band = -1);
    void reply(const char* name, const QByteArray& data);   // to the client that sent the command

    // File transfers (macros, stills) and the media pool
    void sendChunks(quint16 id);
    void finishUpload(quint16 id);
    QByteArray* stillField(int index);
    bool stillValid(int index) const;
    QByteArray stillBytes(int index) const;
    void publishStill(int index, bool valid, const QByteArray& hash, const QByteArray& name);
    void publishMacroSteps(int index);
    void updateAtKeyFrames(int me, int key);   // KeFS byte 6, then send KeFS
    void noteStillTransfer(int index);          // LKST byte 3

    // Inputs
    const QByteArray* input(quint16 id) const;
    bool usableOnMixEffect(quint16 id) const;
    bool usableAsKeySource(quint16 id) const;
    static void copyName(QByteArray& field, int at, int size, const QByteArray& name);
    void updateDirectName(quint16 id);

    // Mix effect / transition
    int transitionRate() const;
    void setDveTaken(bool taken);
    void warn(const char* message);
    bool dveTakenByTransition() const;
    void swapProgramPreview();
    void finishTransition();
    void updateTally();

    // Macros
    void loadMacroPool();
    void publishMacro(int index);          // MPrp for the slot
    void setRunStatus(bool running, bool waiting, int index);
    void startMacro(int index);
    void continueMacro();
    void finishMacro();
    void setRecordingStatus(bool recording, int index);

    // Frame-by-frame animations
    void startAnimation(const QString& id, int frames, std::function<void(int frame)> step,
                        std::function<void()> done);
    void tick();

    FieldStore m_store;
    QHash<QByteArray, Handler> m_handlers;
    // Who last used the one DVE; decides the switcher's warnings.
    enum class DveOwner { None, Keyer, Transition };
    DveOwner m_dveOwner = DveOwner::None;
    FieldList m_out;                      // collects fields while handling
    QHash<quint16, QByteArray> m_defaultNames;   // input id -> InPr as loaded

    QVector<Macro> m_pool;
    struct Transfer {
        bool upload = false;
        quint16 store = 0;          // 0 stills, 0xffff macros
        int index = 0;
        quint32 size = 0;           // upload: the size once unpacked
        QByteArray data;
        int sent = 0, acked = 0;    // download: chunks
        bool described = false;     // upload: FTFD received
        QByteArray name, description, hash;
    };
    QHash<quint16, Transfer> m_transfers;
    QHash<int, QByteArray> m_stillData;   // uploaded stills (RLE, as transferred)
    int m_captures = 0;
    int m_pendingMacro = -1;              // started in the current packet
    struct Run { int index = -1; int step = 0; bool waitingForUser = false; } m_run;
    QTimer m_macroTimer;                  // a macro's Wait step
    struct Recording { bool active = false; int index = -1; Macro macro; } m_recording;

    struct Animation {
        int frame = 0;
        int frames = 0;
        std::function<void(int)> step;
        std::function<void()> done;
    };
    QMap<QString, Animation> m_animations;
    QTimer m_frameTimer;
    QElapsedTimer m_uptime;   // the time code counts from start-up
};

} // namespace emu
