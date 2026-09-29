# Tech Stack, Architecture & Build Guide

## Tech stack

| Layer | Technology |
| --- | --- |
| Language | C++17 |
| UI framework | Qt 6.x (Widgets, Network, Multimedia) |
| Build system | CMake 3.28+ with Visual Studio 17 2022 generator |
| Compiler | MSVC 2022 (cl.exe) — x64 |
| Switcher | The emulator core ([core/](../core)): Qt Core + Network, no UI |
| Virtual camera | Win32 COM / DirectShow push-source DLL |
| Network | Qt `QUdpSocket` — ATEM UDP protocol on port 9910 |
| Video playback | Qt Multimedia `QMediaPlayer` + `QVideoSink` |
| Compositing | `QPainter` (software, no GPU) |

---

## Source layout

```
atem-emulator/
├── CMakeLists.txt              Build config — app, core, AtemVirtualCam DLL
├── README.md                   User-facing overview
├── core/                       The switcher: protocol, state, commands, macros
│   ├── src/server.*            UDP transport (handshake, reliable packets, resends)
│   ├── src/device.*            State fields + command handlers + macro pool + view
│   ├── src/fields.*            Field store (the connect dump, changed in place)
│   ├── src/commands.*          Command payload builders for a local UI
│   ├── src/main.cpp            atem-emu: the switcher without a window
│   ├── profiles/               Recorded switchers (dump.txt, macros.txt)
│   └── make_profile.py         Profile from an atem-sweep golden record
├── sweep/                      atem-sweep: records the real ATEM, verifies the emulator
├── docs/                       This file, sweep.md (real switcher → emulator)
└── src/                        The emulator app (window, picture, webcam)
    ├── AtemState.h             Input ids, PiP drawing state, camera input types
    ├── InputSource.h/cpp       SolidColorSource, StaticImageSource, VideoFileSource
    ├── Compositor.h/cpp        QPainter compositor (program + DVE PiP)
    ├── PreviewWidget.h/cpp     30 fps program output display widget
    ├── SourceButton.h/cpp      Custom QPushButton with thumbnail + active bar
    ├── MainWindow.h/cpp        Window: sends commands to the core, shows its state
    ├── Logger.h/cpp            File log
    ├── main.cpp                Entry point, command-line options
    └── vcam/
        ├── vcam_shared.h       Shared memory layout between exe and DLL
        ├── vcam.cpp            DirectShow push-source COM filter (self-contained)
        └── vcam.def            DLL exports (DllGetClassObject, Register/Unregister)
```

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│  atem-emulator.exe                                                 │
│                                                                    │
│  MainWindow ──commands──► emu::Device ◄──commands── emu::Server ◄──┼── UDP 9910
│      ▲                    (the switcher)            (transport)    │   clients
│      └──stateChanged / view()──┘   └──fieldsChanged──► all clients │
│                                                                    │
│  MainWindow ──► Compositor ──► PreviewWidget (30 fps display)      │
│                     └──► Named shared memory ──► AtemVirtualCam.dll│
│                          (VCamSharedFrame)        DirectShow device│
│  InputSource[4]: SolidColorSource | StaticImageSource | VideoFileSource
└────────────────────────────────────────────────────────────────────┘
```

- **One source of truth.** The switcher state is the core's field store: the
  connect dump recorded from a real ATEM Mini, changed in place by commands.
- **One path for changes.** A button in the window builds the same command
  the SDK would send (`emu::cmd`) and runs it through `Device::apply`, the
  handlers the network uses. The resulting fields go to every client.
- **The window follows the switcher.** `Device::stateChanged` (from any
  client, the window, a running transition or macro) triggers one refresh
  per event-loop pass from `Device::view()`.
- **Macros** are the switcher's pool. The window adds per-slot extras the ATEM
  has no place for (camera pictures, size lock, rotation, opacity) and applies
  them when the macro starts (`Device::macroStarted`), whoever started it.

### Virtual camera IPC

`MainWindow` writes each composited frame into a Win32 named shared memory
segment (`AtemEmulatorVCamFrame`) and signals a named event
(`AtemEmulatorVCamEvent`). `AtemVirtualCam.dll` runs a streaming thread that
waits on the event, reads the frame, flips it vertically (Qt top-down →
DirectShow bottom-up), and delivers it via `IMemInputPin::Receive()`.

The DLL self-registers under `HKCU\Software\Classes\CLSID\...` and the
DirectShow `VideoInputDeviceCategory` devenum key — no admin rights required.

---

## ATEM protocol

See [core/README.md](../core/README.md): the transport as captured from the
real switcher, the state as its connect dump, the command handlers and the
quirks they copy, and how it is checked with atem-sweep.

---

## Build requirements (Windows only)

| Requirement | Tested version |
| --- | --- |
| Windows | 10 or 11 (64-bit) |
| Visual Studio | 2022 (Community or Build Tools) — Desktop C++ workload |
| CMake | 3.28+ (bundled with VS) |
| Qt | 6.11.1 MSVC 2022 64-bit, with the **Qt Multimedia** module |

Qt is the only external dependency. No BMD SDK, no DirectShow SDK, no strmbase —
the virtual camera DLL is self-contained using only Win32 headers shipped with
the MSVC SDK.

---

## Build steps

From a Developer PowerShell for VS 2022 (or after `vcvars64.bat`):

```powershell
cd D:\cemc-sr\atem-emulator
cmake -B build-gui -G "Visual Studio 17 2022" -A x64 `
    -DQt6_DIR="D:/ProgramFiles/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6"
cmake --build build-gui --config Release
```

Outputs in `build-gui\Release\`: `atem-emulator.exe`, `AtemVirtualCam.dll`,
the Qt runtime (`windeployqt6`, post-build) and `profiles\` (copied from
`core\profiles`, post-build). `build-gui\core\Release\atem-emu.exe` is the
switcher without a window.

### First-run (virtual camera registration)

The virtual camera DLL is registered automatically when you click
**Virtual Camera ON** in the GUI. To register manually from PowerShell:

```powershell
regsvr32 build-gui\Release\AtemVirtualCam.dll
```

No admin required — writes to HKCU only.

---

## CMakeLists overview

1. **`atem-emu-core`** (static library, `core/`) — the switcher; also builds
   `atem-emu`.
2. **`AtemVirtualCam`** (shared library) — `src/vcam/vcam.cpp`, `vcam.def`;
   links `strmiids`, `ole32`.
3. **`atem-emulator`** (WIN32 executable) — `src/*.cpp`; links
   `atem-emu-core`, Qt Widgets/Network/Multimedia; `/utf-8`, `NOMINMAX`;
   post-build `windeployqt6` and a copy of `core/profiles`.
