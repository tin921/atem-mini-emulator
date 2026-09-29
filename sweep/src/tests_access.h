#pragma once
// Finding the SDK objects the generated tests (tests_generated.cpp) work on.

#include "runner.h"

// An AddRef'd pointer to the object named by `key` — an interface name,
// optionally with "#which" (e.g. "IBMDSwitcherInputColor#2") — as that
// interface, or nullptr when the switcher doesn't have it. The caller
// releases it.
void* accessObject(Ctx& c, const char* key);

#include <functional>
#include <vector>

// Saved values of an object (and its children), so a test can put back every
// value it changed, side effects included. Built by the generated snap_*.
struct SavedValue {
    QString name;
    std::function<bool()> unchanged;   // reads it again and compares
    std::function<HRESULT()> put;      // sets the saved value
};
struct Saved {
    std::vector<SavedValue> values;
    std::vector<IUnknown*> refs;       // objects the lambdas use
    Saved() = default;
    Saved(const Saved&) = delete;
    Saved& operator=(const Saved&) = delete;
    ~Saved() { for (IUnknown* r : refs) r->Release(); }
};

// Puts back every value that differs (two passes, for values that depend on
// each other), then checks them all: a test fails if one isn't back.
void restoreAll(Ctx& c, Saved& saved);

// For keyframe tests ("...KeyFrameParameters#A" / "#B"): writing a keyframe's
// values marks it as stored; this puts the stored flag back (last).
void saveKeyFrameStored(Ctx& c, Saved& saved, const char* key);

// Would setting this switch the mic input's plug-in power on? (Only that
// needs --allow-mic-power: it powers whatever is plugged in.)
inline bool micPowerWouldSwitchOn(IBMDSwitcherFairlightAnalogAudioInput* in,
                                  BMDSwitcherFairlightAudioAnalogInputMicPowerMode value) {
    BMDSwitcherFairlightAudioAnalogInputMicPowerMode now{};
    if (FAILED(in->GetMicPowerMode(&now))) return true;
    return value == bmdSwitcherFairlightAudioAnalogInputMicPowerModePlugInPower &&
           now != bmdSwitcherFairlightAudioAnalogInputMicPowerModePlugInPower;
}
