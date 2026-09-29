# atem-sweep

Exercises the Blackmagic ATEM switcher API (BMDSwitcherAPI, SDK 10.2.1)
against a switcher — good input and bad — and records exactly how it
responds. Run it against the **real ATEM** to produce a golden record (and a
capture of every network packet); run it against the **emulator** to check
the emulator answers the same way.

Its purpose is reverse engineering the ATEM Mini for the emulator. The goal
is every SDK function the ATEM Mini has and that is safe to call, recorded
well enough that adding it to the emulator core just works — whether or not
the emulator supports it yet. Priority: what the emulator and obs-atem use,
then the SDK samples, then everything else (see [Coverage](#coverage)).

```text
[ 19/273] ✓ me.program.1          Program -> Camera 1           280 ms
[ 28/273] ✓ me.program.bad.5      Program -> 5 (bad)            251 ms
[ 92/273] ✗ fly.x.8p59            PiP position X = 8.59          ...
        diff: readback: golden 8.59, got 8.6
```

## Safety policy

It **reads everything**, **changes settings and puts them back** (program /
preview, key, PiP position / size / crop, transitions, input names, …) and
**runs / stops the stored macros**. It **never deletes, uploads, clears,
records or streams** — no macro recording or deleting, no still capture,
no saving the startup state, no video mode change. The full list is
[coverage/excluded.txt](coverage/excluded.txt).

What "puts them back" covers is exactly the settings in the snapshot
([src/snapshot.cpp](src/snapshot.cpp)), taken right after connecting. At the
end each one is set back and checked, then everything is read back and
compared; any setting that could not be read at the start, could not be set,
or differs afterwards is listed and the run fails (exit code 5). **Your stored
macros can change settings outside the snapshot** (audio, for example) when
the macro tests run them; those are not restored.

It will not start (exit code 4, nothing changed) while a macro is being
recorded or is running: the sweep's commands would end up in the recording.

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

# Capture another client's traffic (no tests): a recording proxy on
# 127.0.0.1:9910 for 10 minutes, or until a file "stop" appears in the folder
atem-sweep 192.168.0.240 --capture 600 --out runs\asc

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

Emulator results last verified 2026-09-28 on the current code.

| Target | Result |
|---|---|
| Real ATEM Mini (record) | 269 passed, 0 failed, 4 skipped (empty macro slots) |
| Real ATEM Mini vs its own golden record | all match (tally/time-code events excluded, see runner.cpp) |
| Emulator core ([../core](../core)) vs golden | 269 passed, 0 failed |
| Emulator app ([../src](../src), `--reference --listen 127.0.0.2`) vs golden | 269 passed, 0 failed (it runs on the same core) |

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

What a verification does and doesn't prove: it compares, per test, the SDK's
return codes and read-backs and *which kinds* of SDK event fired (a set: not
their order, count or arguments), plus the connect dump's field counts. It
does not compare wire bytes. Many SDK getters read the SDK's own cached state
and some bad input is refused inside the SDK, so a match shows the emulator
answers this test sequence like the device — not that it behaves the same in
every situation.

Checks on the run itself:

- The golden record must be a passing recording: every test "pass" or
  "skip", no duplicate ids — otherwise it is refused (exit code 2).
- A full verification (no `--only`) fails if a test of the golden record is
  missing from the current sweep.
- The output folder is checked before connecting (exit code 2), and a
  results file that could not be written fails the run (exit code 3).

| Exit code | Meaning |
|---|---|
| 0 | all passed |
| 1 | a test failed or differs from the golden record |
| 2 | could not start: bad golden record, output folder, proxy |
| 3 | results could not be written |
| 4 | a macro was recording / running: nothing was changed |
| 5 | the switcher was not completely restored |
| 6 | declined at the "Continue?" question: nothing was done |

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

### The five categories

Every callable SDK method (1,264: the current interfaces, without the
callback interfaces a client implements) is in exactly one category.
[coverage/categories.py](coverage/categories.py) sorts them from an emulator
verify run and writes two lists:

| List | Category | Methods (2026-09-28) | Meaning |
|---|---|---:|---|
| [api-supported.tsv](coverage/api-supported.tsv) | 1 emulator | 252 | recorded on the real ATEM, emulated and verified |
| | 2 samples | 1 | used by the SDK samples, not yet recorded (camera autofocus, opt-in) |
| | 3 sweep | 471 | every other safe function the ATEM Mini has: still to record, then emulate |
| [api-unsupported.tsv](coverage/api-unsupported.tsv) | 4 not on mini | 525 | the ATEM Mini doesn't have it ([not-on-mini.txt](coverage/not-on-mini.txt)) |
| | 5 destructive | 15 | deletes, overwrites stored content, records ([excluded.txt](coverage/excluded.txt)) |
| | **total** | **1,264** | |

```powershell
python coverage\categories.py <verify run>\results.json   # also writes coverage\api-categories.txt
```

"Not on mini" is *recorded* when the real ATEM refused the interface in a
sweep run, and *expected* (400 of the 525) until a read-only probe on the
real ATEM confirms it. Work moves methods from 3 (and 2) to 1: add sweep
tests, record the real ATEM, extend the emulator core until verify passes.

### Call tracking

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
