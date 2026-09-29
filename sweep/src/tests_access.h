#pragma once
// Finding the SDK objects the generated tests (tests_generated.cpp) work on.

#include "runner.h"

// An AddRef'd pointer to the object named by `key` — an interface name,
// optionally with "#which" (e.g. "IBMDSwitcherInputColor#2") — as that
// interface, or nullptr when the switcher doesn't have it. The caller
// releases it.
void* accessObject(Ctx& c, const char* key);
