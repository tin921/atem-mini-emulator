#pragma once
#include "AtemState.h"
#include <QObject>
#include <QTimer>

// Executes macro step sequences with inter-step delays.
// Emits signals that MainWindow connects to state-mutating slots.

class MacroEngine : public QObject
{
    Q_OBJECT
public:
    explicit MacroEngine(Atem::ATEMState* state, QObject* parent = nullptr);

    void runMacro(int index);
    void stopMacro();
    bool isRunning() const { return m_running; }
    int  runningIndex() const { return m_runningIndex; }

signals:
    void macroStarted(int index);
    void macroFinished(int index);
    void applyAction(const Atem::MacroAction& action);

private slots:
    void executeNextStep();

private:
    Atem::ATEMState*            m_state        = nullptr;
    int                          m_runningIndex = -1;
    int                          m_stepIndex    = 0;
    bool                         m_running      = false;
    QTimer*                      m_timer        = nullptr;
    QVector<Atem::MacroAction>   m_steps;
};
