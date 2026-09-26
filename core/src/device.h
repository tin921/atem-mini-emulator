// The emulated switcher: state fields loaded from a profile (the connect
// dump recorded from the real device) plus a handler for each command the
// SDK sends. Handlers change fields the way the real ATEM Mini does, as
// recorded by atem-sweep, and return the fields that changed.
#pragma once

#include "fields.h"

#include <QHash>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <functional>

namespace emu {

class Device : public QObject {
    Q_OBJECT
public:
    explicit Device(QObject* parent = nullptr);

    // Loads dump.txt (required) and macros.txt (optional) from profileDir.
    bool load(const QString& profileDir, QString* error);

    QString productName() const;
    // The full state, in dump order, for a newly connected client.
    FieldList connectDump() const { return m_store.dump(); }

    // Applies one command. Returns the fields to send to every client; the
    // switcher sends nothing back for commands it rejects.
    FieldList handle(const QByteArray& name, const QByteArray& data);
    bool knows(const QByteArray& name) const { return m_handlers.contains(name); }
    // Called after all commands of a packet: a macro started in the packet
    // runs now, so a stop in the same packet cancels it (as on the device).
    FieldList endOfPacket();

signals:
    // Fields changed outside a command (transitions running frame by frame).
    void fieldsChanged(const emu::FieldList& fields);
    void log(const QString& message);

private:
    using Handler = std::function<void(const QByteArray&)>;
    void registerHandlers();

    // Output of the handler that is running (or of the current frame).
    void send(const char* name, const QByteArray* data);
    void sendIfChanged(const char* name, QByteArray* data, const QByteArray& before);

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

    // Frame-by-frame animations
    void startAnimation(const QString& id, int frames, std::function<void(int frame)> step,
                        std::function<void()> done);
    void tick();

    FieldStore m_store;
    QHash<QByteArray, Handler> m_handlers;
    QMap<int, FieldList> m_macros;        // slot -> recorded effect fields
    int m_pendingMacro = -1;              // started in the current packet
    // Who last used the one DVE; decides the switcher's warnings.
    enum class DveOwner { None, Keyer, Transition };
    DveOwner m_dveOwner = DveOwner::None;
    FieldList m_out;                      // collects fields while handling
    QHash<quint16, QByteArray> m_defaultNames;   // input id -> InPr as loaded

    struct Animation {
        int frame = 0;
        int frames = 0;
        std::function<void(int)> step;
        std::function<void()> done;
    };
    QMap<QString, Animation> m_animations;
    QTimer m_frameTimer;
};

} // namespace emu
