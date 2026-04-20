#include "MacroEngine.h"

using namespace Atem;

MacroEngine::MacroEngine(ATEMState* state, QObject* parent)
    : QObject(parent), m_state(state)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &MacroEngine::executeNextStep);
}

void MacroEngine::runMacro(int index)
{
    if (index < 0 || index >= m_state->macros.size()) return;
    const MacroDef& def = m_state->macros[index];
    if (!def.isUsed) return;

    m_runningIndex = index;
    m_steps        = def.actions;
    m_stepIndex    = 0;
    m_running      = true;

    emit macroStarted(index);
    executeNextStep();
}

void MacroEngine::stopMacro()
{
    m_timer->stop();
    int idx = m_runningIndex;
    m_running      = false;
    m_runningIndex = -1;
    m_steps.clear();
    m_stepIndex = 0;
    if (idx >= 0) emit macroFinished(idx);
}

void MacroEngine::executeNextStep()
{
    if (!m_running) return;

    if (m_stepIndex >= m_steps.size()) {
        stopMacro();
        return;
    }

    const MacroAction& action = m_steps[m_stepIndex];
    ++m_stepIndex;

    if (action.type == MacroActionType::Delay) {
        m_timer->start(qMax(1, action.param));
    } else {
        emit applyAction(action);
        // Schedule next step after a short inter-step gap
        m_timer->start(50);
    }
}
