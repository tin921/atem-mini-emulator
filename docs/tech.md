# Tech Stack, Architecture & Build Guide

## Tech stack

| Layer | Technology |
| --- | --- |
| Language | C++17 |
| UI framework | Qt 6.x (Widgets, Network, Multimedia) |
| Build system | CMake 3.28+ with Visual Studio 17 2022 generator |
| Compiler | MSVC 2022 (cl.exe) — x64 |
| Virtual camera | Win32 COM / DirectShow push-source DLL |
| Network | Qt `QUdpSocket` — ATEM Mini UDP protocol on port 9910 |
| Video playback | Qt Multimedia `QMediaPlayer` + `QVideoSink` |
| Compositing | `QPainter` (software, no GPU) |

---

## Source layout

```
atem-emulator/
├── CMakeLists.txt              Build config — exe + AtemVirtualCam DLL
├── README.md                   User-facing overview
├── docs/
│   ├── tech.md                 This file
│   ├── capture.md              Protocol capture & reverse engineering
│   ├── capture.exe.md          capture.exe output format details
│   └── tools.md                tools/ directory guide
├── tools/                      Protocol capture tools (capture.exe)
└── src/
    ├── AtemProtocol.h          Header constants, source IDs, packet helpers
    ├── AtemState.h/cpp         Device state struct + UDP field serialisers
    ├── AtemServer.h/cpp        UDP server — handshake FSM, command dispatch, keepalive
    ├── InputSource.h/cpp       SolidColorSource, StaticImageSource, VideoFileSource
    ├── Compositor.h/cpp        QPainter DVE compositor (PiP overlay)
    ├── MacroEngine.h/cpp       QTimer-driven step sequencer
    ├── PreviewWidget.h/cpp     30 fps program output display widget
    ├── SourceButton.h/cpp      Custom QPushButton with thumbnail + active bar
    ├── MainWindow.h/cpp        Full application window and GUI logic
    ├── main.cpp                Entry point
    └── vcam/
        ├── vcam_shared.h       Shared memory layout between exe and DLL
        ├── vcam.cpp            DirectShow push-source COM filter (self-contained)
        └── vcam.def            DLL exports (DllGetClassObject, Register/Unregister)
```

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  atem-emulator.exe                                               │
│                                                                  │
│  MainWindow ──► Compositor ──► PreviewWidget (30 fps display)   │
│      │              │                                            │
│      │              └──► Named shared memory ──► AtemVirtualCam.dll
│      │                   (VCamSharedFrame)        DirectShow device
│      │
│      ├──► AtemServer (QUdpSocket, port 9910)
│      │        handles: CPgI, CPvI, DCut, DAut, CKeO, CDvP, MSRc
│      │        broadcasts: PrgI, PrvI, KeOn, KeDV, MPrp, MRPr, TlIn, TlSr
│      │
│      ├──► MacroEngine (QTimer step sequencer)
│      │        actions: SwitchProgram, KeyerEnable, Delay
│      │
│      └──► InputSource[4]
│               SolidColorSource | StaticImageSource | VideoFileSource
│               (QMediaPlayer + QVideoSink for video)
│
└──────────────────────────────────────────────────────────────────┘

Connected clients (any number simultaneously):
  • ATEM Software Control (official Blackmagic app)
  • BMDSwitcherAPI COM SDK (obs-atem plugin, capture.exe, custom tools)
```

### Virtual camera IPC

`MainWindow` writes each composited frame into a Win32 named shared memory
segment (`AtemEmulatorVCamFrame`) and signals a named event
(`AtemEmulatorVCamEvent`). `AtemVirtualCam.dll` runs a streaming thread that
waits on the event, reads the frame, flips it vertically (Qt top-down →
DirectShow bottom-up), and delivers it via `IMemInputPin::Receive()`.

The DLL self-registers under `HKCU\Software\Classes\CLSID\...` and the
DirectShow `VideoInputDeviceCategory` devenum key — no admin rights required.

---

## ATEM Mini UDP protocol

The emulator implements the full ATEM Mini protocol as captured from real
hardware (firmware 8.1.1). All values are big-endian.

### Packet structure

```
Offset  Size  Field
0       2     Flags + length  (flags in high 5 bits; length in low 11 bits)
2       2     Session ID      (server assigns 0x8000 | counter)
4       2     Remote seq      (client's sequence number)
6       2     Local ack       (acknowledgement of received packets)
8       2     Unknown         (flags field, typically 0)
10      2     Local seq       (sender's sequence number)
12      N     Payload         (zero or more ATEM fields)
```

### Handshake

1. Client sends SYN (`flags=0x10`, `local_seq=0x0001`)
2. Server assigns session ID and replies SYN-ACK (`flags=0x10 0x02`)
3. Client sends secondary SYN (`session_id=0x8000`)
4. Server sends state dump (~8–12 packets), ends with `InCm` + empty terminal
5. Both sides send periodic ACK-only keepalives every ~1 s

### State dump fields (exact order)

`_ver`, `_pin`, `_top`, `Warn`, `InPr` (×14), `MeConf`, `MvIn`, `PrgI`, `PrvI`,
`TrSS`, `TMxP`, `TDpP`, `TWpP`, `TDvP`, `TSti`, `KeOn`, `KeDV`, `KeLm`,
`MPrp` (×20), `MRPr`, `TlIn`, `TlSr`, `InCm`

### Commands handled

| Command | Action |
| --- | --- |
| `CPgI` | Set program source |
| `CPvI` | Set preview source |
| `DCut` | Cut (swap preview to program) |
| `DAut` | Auto (treated as cut in emulator) |
| `CKeO` | Keyer on/off |
| `CDvP` | DVE parameters (fill source, size, position) |
| `MSRc` | Run macro by index |
| `MSt` | Stop running macro |

---

## Build requirements (Windows only)

| Requirement | Tested version |
| --- | --- |
| Windows | 10 or 11 (64-bit) |
| Visual Studio | 2022 Community 17.x — Desktop C++ workload |
| CMake | 3.28+ |
| Qt | 6.x MSVC 2022 64-bit (tested with 6.11.0) |
| Qt Multimedia | Included with Qt; requires Media Foundation on Windows |

Qt is the only external dependency. No BMD SDK, no DirectShow SDK, no strmbase —
the virtual camera DLL is self-contained using only Win32 headers shipped with
the MSVC SDK.

---

## Build steps

Open **Developer PowerShell for VS 2022** (or load VS tools manually):

```powershell
Import-Module "D:\Program Filesx\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "D:\Program Filesx\Microsoft Visual Studio\2022\Community" `
    -DevCmdArguments "-arch=amd64" -SkipAutomaticLocation
```

### Configure

```powershell
cd D:\cemc-sr\atem-emulator

cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DQt6_DIR="D:/ProgramFiles/Qt/6.11.0/msvc2022_64/lib/cmake/Qt6"
```

### Build

```powershell
cmake --build build --config Release
```

Outputs:

- `build\Release\atem-emulator.exe`
- `build\Release\AtemVirtualCam.dll`

Qt runtime DLLs are deployed automatically by `windeployqt6` as a CMake
post-build step.

### First-run (virtual camera registration)

The virtual camera DLL is registered automatically when you click
**Virtual Camera ON** in the GUI. To register manually from PowerShell:

```powershell
regsvr32 build\Release\AtemVirtualCam.dll
```

No admin required — writes to HKCU only.

---

## CMakeLists overview

The build file defines two targets:

1. **`AtemVirtualCam`** (SHARED library, built first)
   - Sources: `src/vcam/vcam.cpp`, `src/vcam/vcam.def`
   - Links: `strmiids`, `ole32` (both in Windows SDK)
   - Output: placed alongside the exe

2. **`atem-emulator`** (WIN32 executable)
   - Sources: all `src/*.cpp` files
   - Links: `Qt6::Widgets`, `Qt6::Network`, `Qt6::Multimedia`, `ws2_32`
   - Depends on: `AtemVirtualCam`
   - Post-build: `windeployqt6` and copy of `AtemVirtualCam.dll`
