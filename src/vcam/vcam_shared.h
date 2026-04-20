#pragma once

// Shared between the main exe and the DLL loaded in OBS/Zoom
static const char kSharedMemName[]   = "AtemEmulatorVCamFrame";
static const char kSharedEventName[] = "AtemEmulatorVCamEvent";

static const int kVCamWidth  = 1280;
static const int kVCamHeight = 720;

#pragma pack(push, 1)
struct VCamSharedFrame {
    volatile long frameId;                      // incremented each frame by exe
    unsigned int  width;                        // kVCamWidth
    unsigned int  height;                       // kVCamHeight
    unsigned char bgra[kVCamWidth * kVCamHeight * 4]; // Qt Format_RGB32 = BGRA on x86
};
#pragma pack(pop)
