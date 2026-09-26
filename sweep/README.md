# atem-sweep

Exercises the Blackmagic ATEM switcher API (BMDSwitcherAPI, SDK 10.2.1)
against a switcher — good input and bad — and records exactly how it
responds. Run it against the **real ATEM** to produce a golden record (and a
capture of every network packet); run it against the **emulator** to check
the emulator answers the same way.

```text
[ 19/272] ✓ me.program.1          Program -> Camera 1           280 ms
[ 28/272] ✓ me.program.bad.5      Program -> 5 (bad)            251 ms
[ 92/272] ✗ fly.x.8p59            PiP position X = 8.59          ...
        diff: readback: golden 8.59, got 8.6
```

## Safety policy

It **reads everything**, **changes settings and puts them back** (program /
preview, key, PiP position / size / crop, transitions, input names, …) and
**runs / stops the stored macros**. It **never deletes, uploads, clears,
records or streams**. The switcher's state is snapshotted right after
connecting and restored at the end.

It does visibly change the live output while it runs (inputs switch, the
PiP moves, macros run, a short fade to black), so it asks before starting
(`--yes` skips the question).

The few SDK calls it never makes are listed, with reasons, in
[coverage/excluded.txt](coverage/excluded.txt). Camera actions (autofocus)
only run with `--allow-camera`.

## Usage

```powershell
# Record the real ATEM (over Ethernet) — golden record + wire capture
atem-sweep 192.168.0.240

# Verify the emulator against that record (emulator on this PC, see ../core)
atem-emu --listen 127.0.0.2
atem-sweep 127.0.0.2 --verify runs\<record-run>\results.json

# Over USB (API behaviour only: USB traffic can't be captured)
atem-sweep usb

atem-sweep --list                     # all tests
atem-sweep 192.168.0.240 --only fly.  # just the PiP position/size tests
```

Needs ATEM Software Control installed (the SDK's COM library) and Qt's `bin`
folder on `PATH` (or run `windeployqt atem-sweep.exe`).

## Golden record

[golden/atem-mini_sdk10.2.1_proto2.30/](golden/atem-mini_sdk10.2.1_proto2.30) is the
reference recorded from the real ATEM Mini (protocol 2.30, SDK 10.2.1) on
2026-09-26: `results.json`, `wire.jsonl` (1,018 packets) and `coverage.txt`.
The emulator's profile ([../core/profiles](../core/profiles)) is built from it.

| Target | Result |
|---|---|
| Real ATEM Mini (record) | 269 passed, 0 failed, 4 skipped (empty macro slots) |
| Real ATEM Mini vs its own golden record | all match (tally/time-code events excluded, see runner.cpp) |
| Emulator core ([../core](../core)) vs golden | 269 passed, 0 failed |
| Old GUI emulator ([../src](../src)) vs golden | fails at connect: the SDK rejects the state dump ("corrupt data", `cfcd`) |

The real device sends 75 kinds of field (364 fields) when a client connects;
`connect.main` in `results.json` lists them all.

```powershell
atem-emu --listen 127.0.0.2
atem-sweep 127.0.0.2 --verify golden\atem-mini_sdk10.2.1_proto2.30\results.json
```

The emulator listens on its own loopback address so the recording proxy can
stay on 127.0.0.1:9910. A target of 127.0.0.1 itself runs without the proxy
(no wire capture, and `connect.main` can't count the dump fields).

Some values are left out of the comparison because they don't come from the
switcher: keys marked `(informational)` (timings, and the media player clip
state, which an ATEM Mini never sends so the SDK returns whatever its memory
held) and the tally/time-code events.

## Output (`runs\<time>-<mode>\`)

| File | Contents |
|---|---|
| `results.json` | Per test: status, every observation (return codes, read-backs), the SDK events fired, and — when recorded through the proxy — the ATEM fields sent (`tx`) and received (`rx`) during that test. Verify runs add `diffs`. |
| `wire.jsonl` | Every UDP packet between the SDK and the ATEM, both directions, timestamped and split into fields. The raw material for the emulator. |
| `coverage.txt` | Coverage of the SDK calls, see below. |

### How recording works

Over Ethernet the SDK is pointed at a small UDP proxy on `127.0.0.1:9910`
which forwards to the ATEM and keeps every packet. Each test therefore shows
which command bytes an API call produced and which state fields the ATEM sent
back — for good input and for bad.

## Coverage

Every SDK call goes through `SDK_CALL(...)`, which records it as it executes,
so coverage reflects calls that really ran. The report checks three lists in
[coverage/](coverage):

| List | Source | Target |
|---|---|---|
| `tier1-plugin.txt` | every SDK call obs-atem makes | 100% |
| `tier2-samples.txt` | every SDK call in the SDK 10.2.1 Windows samples | 100% |
| `sdk-api.json` | every interface/method in `BMDSwitcherAPI.h` | reference |

A method counts as accounted for when it was **called**, when its interface
is **not on this model** (e.g. recording/streaming on a base ATEM Mini — the
SDK refuses the interface, which is itself recorded), or when it is
**excluded by policy**. The lists are regenerated with
`coverage/scan_usage.py` (see the comment at its top).

## Layout

```text
sweep/
├── CMakeLists.txt
├── coverage/            coverage targets + scanner
└── src/
    ├── main.cpp          options, safety prompt, proxy
    ├── runner.*          run loop, console, golden comparison, coverage report
    ├── snapshot.cpp      snapshot/restore of everything the sweep changes
    ├── sdk.*             connection, event log, SDK_CALL coverage
    ├── wireproxy.*       UDP recording proxy + ATEM field parser
    └── tests_*.cpp       test groups (connect, inputs, M/E, key/PiP, DSK,
                          macros, media, audio/devices)
```

## Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DQt6_DIR="D:/ProgramFiles/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6"
cmake --build build --config Release
```
