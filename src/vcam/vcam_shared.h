#pragma once

// Shared between the main exe and the DLL loaded in OBS/Zoom.
//
// One producer (the emulator whose virtual camera is on; it holds
// kProducerMutexName meanwhile) writes frames into the shared memory; any
// number of camera consumers read them. A frame is published with a
// sequence number: odd while the producer writes, even once the frame is
// complete, so a reader that sees the same even number before and after its
// copy has a whole frame. The event is only a hint that a frame is ready:
// readers also look on a short timeout, so several consumers all get frames.
static const char kSharedMemName[]     = "AtemEmulatorVCamFrame";
static const char kSharedEventName[]   = "AtemEmulatorVCamEvent";
static const char kProducerMutexName[] = "AtemEmulatorVCamProducer";

static const int kVCamWidth  = 1280;
static const int kVCamHeight = 720;

static const unsigned int kVCamMagic   = 0x43564541;   // "AEVC"
static const unsigned int kVCamVersion = 2;

#pragma pack(push, 4)
struct VCamSharedFrame {
    unsigned int  magic;                        // kVCamMagic once the producer set it up
    unsigned int  version;                      // kVCamVersion
    unsigned int  size;                         // sizeof(VCamSharedFrame)
    unsigned int  width;                        // kVCamWidth
    unsigned int  height;                       // kVCamHeight
    volatile long sequence;                     // odd: being written; even: complete
    unsigned char bgra[kVCamWidth * kVCamHeight * 4]; // Qt Format_RGB32 = BGRA on x86, top-down
};
#pragma pack(pop)
