# Overview

How the pieces fit, how the emulator is built from the real switcher, and
where an AI coding agent does the work — plus the tech stack and build.

---

## The pieces

```text
 sweep/  atem-sweep + atem-sweep-gui               core/  the emulator core
┌──────────────────────────────┐                  ┌──────────────────────────────┐
│ test client (Blackmagic SDK) │ ─ golden record ►│ server: ATEM UDP protocol    │
│ + recording proxy            │ ─ profile ──────►│ device: state, setter table, │
│ backup / restore             │                  │ handlers, macros, transfers  │
└──────────────┬───────────────┘                  └──────────────┬───────────────┘
               │ verify: the same tests,                         │ a library, used by
               │ answers compared                                ├─► atem-emu.exe (console)
               └────────────────────────────────────────────────►└─► atem-emulator.exe (src/:
                                                                      window, picture, webcam)
```

| Piece | What it is | Guide |
|---|---|---|
| `src/` → `atem-emulator.exe` | The desktop app: window, program picture, virtual camera | [emulator.md](emulator.md) |
| `core/` → `atem-emu.exe` + a library | The switcher: protocol, state, behaviour; the app runs on it | [core.md](core.md) |
| `sweep/` → `atem-sweep.exe`, `atem-sweep-gui.exe` | Records the real ATEM through the SDK and a proxy; checks the emulator against the recordings | [sweep.md](sweep.md), [sdk-functions.md](sdk-functions.md) |

They share no code: what connects the sweep and the core is data — the
golden records and the profiles made from them.

---

## The workflow

Blackmagic publishes the SDK but not the protocol under it. Everything the
emulator knows comes from recordings of the real ATEM Mini:

```text
         ┌───────────────────────────────────────────────────────────────┐
         ▼                                                               │
 1 RECORD the real ATEM          atem-sweep 192.168.0.240 --protect      │
   (settings put back, stored    → results.json  what the SDK saw        │
    content backed up)           → wire.jsonl    every packet, both ways │
         │                                                               │
 2 GOLDEN RECORD                 slim_record.py → sweep/golden/<name>    │
         │                                                               │
 3 PROFILE                       make_profile.py → core/profiles/<name>  │
   the startup state, macros                                             │
         │                                                               │
 4 IMPLEMENT  ◄── the AI agent   setters_spec.py → check_setters.py      │
                                 → gen_setters.py; device.cpp            │
         │                                                               │
 5 VERIFY the emulator           atem-sweep 127.0.0.2 --verify <golden>  │
         │                                                               │
         └── differences ──► back to 4;  a test that can't show the ─────┘
                                          truth ──► fix the test, record again
```

Steps 1–3 and 5 are tools. Step 4 — turning recordings into emulator code —
is where the reasoning happens, and it is done by an AI coding agent (Claude
Code built most of the current core this way), with a person deciding what
runs on the real switcher.

---

## Where AI comes in

### What it reads

The sweep's output is written to be read by an agent as much as by a person:

| Source | What it tells |
|---|---|
| The verify's `diffs` (console, `results.json`, the GUI's **Download report**) | *What* differs, per test: `readback: golden 0.5, got 0.25`, `events missing: ME/tfrC` |
| The golden record's `obs` and `sdk` per test | What the real ATEM answered, and which SDK functions the test called |
| The per-test `wire` (`tx` / `rx`) in both runs' `results.json` | *Why*: the command bytes the SDK sent and the state fields that came back — real ATEM and emulator side by side |
| `wire.jsonl` of both runs | Timing and packet grouping (which answers came in the same frame), the handshake, what arrived after the connect dump |
| The coverage lists (`sweep/coverage/api-*.tsv`) | What is emulated and verified, what is recorded but not emulated yet |

Helpers in [core/tools](../core/tools) for reading records:
`show_command.py` (every recorded use of one command, with its answers),
`infer_layout.py` (which command bytes land where in the state field),
`show_transfer.py` (file transfers in order).

### What it writes

- **A setter-table entry** in `core/tools/setters_spec.py` when a command
  just sets values: the layout, and rules for clamps, refused values and side
  effects. `check_setters.py` replays *every* recorded use of the command
  through the rule and compares with what the real ATEM sent back — the rule
  is proven on the recording before any C++ is built — and `gen_setters.py`
  writes the C++ table.
- **A handler** in `core/src/device.cpp` for actions and anything with its
  own logic (transitions, keyframes, macros, file transfers, the lock).
- **A sweep test change** when the recording can't show the truth: a value
  that is timing noise, garbage the SDK returns for a feature the Mini
  doesn't have, a test that depends on what is plugged in. That needs a new
  recording before it counts.

### How it checks itself

The checker first (seconds), then a targeted run (`--only fly.`,
`--groups storage`: minutes), then the full verify (1,881 tests, about 30
minutes). Each step is committed with its numbers.

### The guardrails

- **The golden record is never edited** to make a test pass, and verify is
  never loosened for a value that comes from the switcher. A value is left
  out of the comparison only when it provably doesn't (timings, the SDK's
  leftovers, a callback the SDK fires or not for the same packets), with the
  evidence in a comment.
- **The real switcher is touched only with the person's go-ahead**, only in
  a protected run (backup, settings put back, stored content restored and
  compared), and never while it is in use.
- **Emulator runs are isolated**: the emulator must own `127.0.0.2:9910`
  before a sweep starts, because the SDK falls back to a real ATEM on USB when
  nothing answers.
- **Generated tests are diffed before they run**: a regeneration once tried
  to add two tests that would have deleted stored content outside the
  protected storage tests.

### What it found this way

Examples from the current core, each read off the recordings:

- **A table instead of code.** 22 "set value" commands turned out to share
  one shape; describing them as data (clamps, refusals, side effects) took
  the emulator from 874 to 1,425 matching tests in one step.
- **The switcher answers once per video frame**, with the state *at that
  moment*, so a keyframe stored and cleared within one frame is never seen by
  the client — found from 19 identical answers to 21 commands.
- **Every state packet starts with a time code** (`Time`).
- **The camera model the SDK reports without a Blackmagic camera is
  garbage** — 4294967295 on the device, 0 and then 2146862410 against the
  emulator — so it is recorded, not compared.
- **The SDK falls back to USB**: two "unreachable address" tests had been
  connecting to the real ATEM on USB even in emulator runs.

### What still needs a person

Recording the real switcher (and saying when nothing is live), connecting
hardware the sweep can't fake (a HyperDeck, a Blackmagic camera — this
project has neither, see [sdk-functions.md](sdk-functions.md#hardware)), and
deciding what is safe to exercise (the opt-ins, the excluded functions).

---

## Tech stack

| Layer | Technology |
| --- | --- |
| Language | C++17; tools in Python 3 |
| UI | Qt 6 Widgets (the app, the sweep GUI), Qt Multimedia (video sources) |
| Network | Qt `QUdpSocket` — the ATEM UDP protocol on port 9910 |
| Switcher SDK | Blackmagic BMDSwitcherAPI 10.2.1 (COM) — atem-sweep only |
| Virtual camera | Win32 COM / DirectShow push-source DLL |
| Build | CMake 3.28+, Visual Studio 2022 (MSVC, x64) |

## The app's architecture

```text
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

- **One source of truth:** the core's field store — the connect dump
  recorded from the real ATEM, changed in place by commands.
- **One path for changes:** a button builds the same command the SDK would
  send (`emu::cmd`) and runs it through `Device::apply`, the handlers the
  network uses; like the switcher, the core answers clients once per frame.
- **The window follows the switcher:** `Device::stateChanged` (any client,
  the window, a running transition or macro) triggers one refresh from
  `Device::view()`.
- **Macros** are the switcher's pool; the window adds per-slot extras the
  ATEM has no place for (camera pictures, size lock, rotation, opacity) and
  applies them when the macro starts.
- **Virtual camera:** each frame goes into named shared memory
  (`AtemEmulatorVCamFrame`) with an event (`AtemEmulatorVCamEvent`);
  `AtemVirtualCam.dll` waits on the event, flips the frame (Qt top-down →
  DirectShow bottom-up) and delivers it. It registers itself under HKCU (no
  admin).

| `src/` file | Role |
|---|---|
| `MainWindow.*` | The window: sends commands to the core, shows its state |
| `Compositor.*` | QPainter program picture (program + DVE PiP) |
| `PreviewWidget.*` | 30 fps display |
| `InputSource.*` | Solid colour, still image, looping video |
| `SourceButton.*` | Source button with thumbnail and active bar |
| `AtemState.h` | Input ids, PiP drawing state, camera input types |
| `Logger.*`, `main.cpp` | File log; options (`--profile`, `--listen`, `--reference`) |
| `vcam/` | The DirectShow filter DLL (`vcam.cpp`, `vcam.def`, `vcam_shared.h`) |

## Build

Requirements (Windows 10/11, 64-bit): Visual Studio 2022 or its Build Tools
with the Desktop C++ workload; Qt 6.11.1 MSVC 2022 64-bit with **Qt
Multimedia**. atem-sweep also needs the Blackmagic ATEM SDK 10.2.1 headers
and ATEM Software Control installed (the SDK's COM library).

```powershell
# the app, the core and atem-emu (Developer PowerShell for VS 2022)
cmake -B build-gui -G "Visual Studio 17 2022" -A x64 `
    -DQt6_DIR="D:/ProgramFiles/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6"
cmake --build build-gui --config Release

# atem-sweep and atem-sweep-gui
cd sweep
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DQt6_DIR="D:/ProgramFiles/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6"
cmake --build build --config Release
```

Outputs: `build-gui\Release\` — `atem-emulator.exe`, `AtemVirtualCam.dll`,
the Qt runtime (`windeployqt6`) and `profiles\`;
`build-gui\core\Release\atem-emu.exe`; `sweep\build\Release\` —
`atem-sweep.exe`, `atem-sweep-gui.exe`.

CMake targets: **`atem-emu-core`** (static library, `core/`, also builds
`atem-emu`), **`AtemVirtualCam`** (the DirectShow DLL), **`atem-emulator`**
(the app; `/utf-8`, `NOMINMAX`; post-build `windeployqt6` and a copy of
`core/profiles`); in `sweep/`, **`atem-sweep`** and **`atem-sweep-gui`**.

The virtual camera registers itself when you click **Virtual Camera ON**; by
hand: `regsvr32 build-gui\Release\AtemVirtualCam.dll` (HKCU, no admin).
