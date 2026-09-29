# Using the emulator

`atem-emulator.exe` is an ATEM Mini on UDP port 9910 with a window: program
bus, PiP, macros, a live program picture and a virtual webcam. Software that
uses Blackmagic's switcher SDK connects to it as it would to the real device.
`atem-emu.exe` is the same switcher without the window ([core.md](core.md)).

## What it does

| Capability | Details |
| --- | --- |
| ATEM Mini protocol | The real device's handshake, its full startup state (364 fields), acknowledgements and resends, answers once per video frame; about 90 commands with the device's own checks, clamps and quirks, including macro and still transfers |
| Source switching | Program bus: Black, Camera 1–4, Color Bars (plus the colour generators and media player inputs from clients) |
| Picture-in-Picture | Upstream key 1 as a DVE key, like the ATEM Mini: size, position, border, border colour, crop |
| Transitions | Cut, auto transition, T-bar and fade to black run frame by frame (1080p24) |
| Input sources | Per camera: solid colour, still image or looping video file |
| Macro pool | The switcher's 100 slots: record in ATEM Software Control or through the SDK, or **Save Output** here; run with pauses, user waits and loop; rename; delete; upload and download |
| Live preview | 640×360 program picture at 30 fps (QPainter, no GPU) |
| Virtual camera | A DirectShow webcam — "ATEM Mini Emulator" in OBS, Zoom, the Camera app |
| Protocol log | Connections and every command received, in the window |
| Several clients | Every change reaches every connected client |

How close it is to the real device, and what it doesn't do yet:
[core.md](core.md#limits) and [sdk-functions.md](sdk-functions.md).

## Start

```powershell
atem-emulator.exe
```

It listens on **UDP 0.0.0.0:9910** at start. Options:

| Option | Meaning |
| --- | --- |
| `--listen ADDRESS` | Listen on one address only, e.g. `127.0.0.2` |
| `--profile DIR` | Emulate another recorded switcher (default `profiles/atem-mini_proto2.30`; also `atem-mini_proto2.30_2026-09-28`) |
| `--reference` | Start exactly as recorded: saved macros are not loaded or saved (for checks against the recordings) |
| `--screenshot FILE [--macro N]` | For documentation: run macro N, show the preview's axes and grid, save the window to FILE and exit |

Building it: [overview.md](overview.md#build).

## Connecting clients

### obs-atem plugin

In the panel settings (⚙), connect to **Manual IP** → `127.0.0.1`.

### BMDSwitcherAPI (C++ / COM SDK)

```cpp
IBMDSwitcherDiscovery* disc;
CoCreateInstance(__uuidof(CBMDSwitcherDiscovery), nullptr, CLSCTX_ALL,
                 __uuidof(IBMDSwitcherDiscovery), (void**)&disc);

BMDSwitcherConnectToFailure fail;
IBMDSwitcher* sw;
disc->ConnectTo(_bstr_t(L"127.0.0.1"), &sw, &fail);
```

If nothing answers at the address, the SDK falls back to an ATEM on this PC's
USB: with a real ATEM plugged in, a wrong address connects to that instead.

### ATEM Software Control (official app)

**File → Connection → Manual IP Address** `127.0.0.1` → **Connect**. Not yet
verified end to end: the next step is to record what it sends (see
[Capturing another client](sweep.md#capturing-another-client)).

## Using the window

Everything in the window goes through the same command handlers as the
SDK's commands, so the window, SDK clients such as the OBS plugin and every
other connected client show the same state.

### Program bus

Six source buttons across the top; click one to switch Program. The selected
one has a white bar along its top edge. Labels follow the switcher's input
names when they are renamed (e.g. in ATEM Software Control).

### Picture-in-Picture (DVE)

Click a **Fill** button to show that source as the PiP (the key becomes a DVE
key and goes on air); click the lit one again to take the PiP off air.

On the program picture, drag the PiP to move it, or drag one of its corner
handles to resize it; the opposite corner stays put, and **Lock** keeps the
proportions (the axis you drag along more sets the size). While the mouse is
over the picture it shows the switcher's axes: the origin 0 in the centre,
+Y up, a tick per unit, the edges (±16, ±9) and halfway points (±8, ±4.5)
labelled. While you drag, a grid of one unit (32 × 18 cells) is drawn:

| Hold | Moving the PiP | Dragging a corner |
| --- | --- | --- |
| **Shift** | along one axis only (the one you move more), its nearest edge or centre snapped to a grid line | along one axis only, snapped to a grid line |
| **Alt** | freely, snapped on both axes | to the nearest grid point |

Shift and Alt apply the moment you press or release them. Esc during a drag
puts the PiP back.

The mouse wheel changes a number box while it has the focus (click it
first); Ctrl + wheel steps ten times as far.

| Control | Range | Sent to the switcher as |
| --- | --- | --- |
| Size X / Size Y | 5–200% | DVE size 0.05–2.0 (Lock keeps X and Y equal) |
| Position X / Y | ±200.00, steps of 0.1 | DVE position in the switcher's units, as ATEM Software Control shows it: ±16 / ±9 are the frame edges, +Y up; beyond that the PiP is (partly) off screen |
| Border width | 0–50 px | DVE border outer width 0–16, border on when > 0 |
| Border color | picker | DVE border hue / saturation / luma |
| Crop L/R/T/B | 0–50% | DVE mask (left/right of 32, top/bottom of 18), on while any edge > 0 |
| Rotation, Opacity | 0–359°, 0–100% | Not sent: an ATEM Mini can't rotate or fade a key; they only change the emulator's picture |

### Input sources

For each of the four cameras: **Color** (a solid fill; click the swatch),
**Photo** (a still image) or **Video** (a looping `.mp4` / `.mov` / `.mkv` …).

### Macros

The list is the switcher's macro pool (100 slots). Macros recorded in ATEM
Software Control or through the SDK appear here, and macros made here appear
there.

- **Save Output** stores the current picture in the selected slot as switcher
  commands (program, PiP source, DVE settings, PiP on/off), plus this
  emulator's camera pictures, size lock, rotation and opacity.
- **▶ Play** runs the selected macro (**■ Stop** while one runs).
- **Update** renames / re-describes the slot.
- **Saved State** lists the macro's steps.

Macros are saved in `%LOCALAPPDATA%\CEMC\ATEM Emulator\macros.json` (a file
from the earlier version is converted on first start and kept as
`macros-v1.json`). Until something is saved there, the pool holds the macros
recorded from the real ATEM Mini with the profile.

### Virtual camera

**Virtual Camera ON** registers `AtemVirtualCam.dll` (under HKCU, no admin)
and streams the program picture; it appears as **"ATEM Mini Emulator"** in
OBS, Zoom, the Windows Camera app and any DirectShow application.

### Network

**Network ON/OFF** starts or stops the UDP server. It starts at launch, on
`0.0.0.0:9910` unless `--listen` says otherwise.
