# ATEM Mini Emulator

A Windows app that behaves like a Blackmagic **ATEM Mini** video switcher on
the network. Software that talks to an ATEM through Blackmagic's switcher SDK
(BMDSwitcherAPI) connects to it as if it were the real device — so you can
develop, test and demonstrate ATEM software, such as the
[obs-atem](https://github.com/tin921/obs-atem) OBS plugin, without a switcher
on the desk.

And because Blackmagic publishes the SDK but not the protocol under it, the
repository also holds the tool that worked the protocol out: **atem-sweep**,
which records how a real ATEM Mini answers every safe SDK function and checks
the emulator against those recordings.

![The emulator: program output with the PiP being placed, the switcher's controls and macro pool](docs/emulator.png)

```text
 TEST WITHOUT THE DEVICE
   your software ──── Blackmagic SDK, UDP 9910 ────► ATEM Mini Emulator
   (obs-atem, SDK apps)                              window · program picture · virtual webcam
                                                     core: the switcher's protocol, state, rules
                                                                      ▲
 MAKE SURE IT ANSWERS LIKE THE DEVICE                                 │ rules written from the
   record:  atem-sweep ─► SDK ─► recording proxy ─► real ATEM Mini    │ recordings; every answer
   verify:  atem-sweep ─► SDK ─► recording proxy ─► emulator ─────────┘ compared with the device's
                                 (every packet, both ways)
```

---

## Test ATEM software without an ATEM

The emulator starts with the exact state a real ATEM Mini sends a new client
(364 fields), handles about 90 commands with the device's own checks, clamps
and quirks, and answers once per video frame like the device. In the window:

- **Program bus** — Black, Camera 1–4, Color Bars; cut, auto transition,
  T-bar and fade to black, frame by frame.
- **Picture-in-picture** — upstream key 1 as a DVE key, like the Mini: drag
  and resize it on the program picture (with the switcher's axes, a grid and
  snapping), or type size, position, border and crop.
- **Macros** — the switcher's 100 slots: record them in ATEM Software
  Control, through the SDK, or with **Save Output**; run, loop, rename,
  upload and download.
- **Cameras** — each a colour, a still image or a looping video.
- **Virtual webcam** — the program picture as "ATEM Mini Emulator" in OBS,
  Zoom or the Camera app.
- **Several clients** at once, every change sent to all of them, and a log
  of every command received.

Build and run (Windows, Visual Studio 2022, Qt 6 with Multimedia):

```powershell
cmake -B build-gui -G "Visual Studio 17 2022" -A x64 -DQt6_DIR="<Qt>/msvc2022_64/lib/cmake/Qt6"
cmake --build build-gui --config Release
build-gui\Release\atem-emulator.exe
```

Then connect your software to `127.0.0.1` (the obs-atem plugin: ⚙ → Manual
IP; the SDK: `ConnectTo("127.0.0.1")`). One thing to know: when nothing
answers at an address, the SDK quietly falls back to an ATEM on the PC's USB.

**More:** [docs/emulator.md](docs/emulator.md) — options, connecting
clients, every control in the window, macros, the virtual camera.

---

## Make sure it answers like the real ATEM

The emulator is only useful if it answers like the device. atem-sweep drives
a switcher through the Blackmagic SDK — every safe function the ATEM Mini
has, with good and bad input — through a **recording proxy** that captures
every packet both ways:

- against the **real ATEM** it makes a *golden record*: what the SDK
  reported and the switcher's exact bytes;
- against the **emulator** it runs the same tests and compares every answer
  with the golden record.

![atem-sweep in a console, through its recording proxy](docs/sweep-cli.png)

The same sweep runs in a window, every test a square coloured as it runs.
Against the real ATEM it backs up the switcher first, puts every setting
back, restores the stored macros and stills and checks they're identical.

![atem-sweep-gui: the coverage map, grouped by test group, and the drill-down list](docs/sweep-gui.png)

Where it stands (2026-09-29): 1,881 tests; the emulator matches the real
ATEM Mini in 1,858, and the rest are tests changed since the recording. Of
the SDK's 1,264 methods, 630 are recorded, emulated and verified, 534 don't
exist on the Mini, 5 are never called (they can't be undone), and 70 need a
HyperDeck or a Blackmagic camera — which this project doesn't have.

**More:**

- [docs/sweep.md](docs/sweep.md) — how the sweep and the proxy work, safety,
  running it (console and window), capturing another client, backups.
- [docs/sdk-functions.md](docs/sdk-functions.md) — every SDK function's
  coverage, the test groups, the hardware and excluded functions, the golden
  records, what a verification compares.
- [docs/core.md](docs/core.md) — the emulator core: protocol, state, the
  rules the recordings showed, and its limits.
- [docs/overview.md](docs/overview.md) — how the pieces fit, the workflow
  from recording to emulator code (written with an AI coding agent), the
  app's architecture and the build.

---

## Repository

| Folder | What |
| --- | --- |
| [src/](src) | The emulator app: window, picture, virtual camera |
| [core/](core) | The switcher itself, shared by the app and `atem-emu.exe` (the same switcher with a console) |
| [sweep/](sweep) | atem-sweep and atem-sweep-gui, the golden records and the coverage lists |
| [docs/](docs) | The guides above |

## Related projects

| Project | Role |
| --- | --- |
| [obs-atem](https://github.com/tin921/obs-atem) | OBS Studio plugin — macro, PiP and Views panels; connects to a real ATEM or this emulator |
| ATEM Software Control | Blackmagic's app; against this emulator not verified yet |
| BMDSwitcherAPI SDK | Blackmagic's COM SDK, used by obs-atem and atem-sweep |

## License

MIT — see [LICENSE](LICENSE).
