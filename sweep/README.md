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

It **reads everything**, **changes settings and puts each back** (checked
per test: program / preview, keys, PiP, transitions, colour generators, the
HDMI-out source, Fairlight audio, …) and **runs / stops the stored macros**.

**Stored content** — macros and stills — is created, listed, changed and
deleted only by the storage tests (`s.`), and only inside a
**backup-protected run**: a complete backup first (the storage tests are
skipped otherwise), then the tests, which use empty slots and delete what
they make, then the backup is restored and verified (see [Backup](#backup)).
That needs Ethernet (the recording proxy).

It **never** saves or clears the startup state (it can't be read back),
changes the video mode (every output goes black), records or streams. The
full list is [coverage/excluded.txt](coverage/excluded.txt).

What "puts them back" covers is exactly the settings in the snapshot
([src/snapshot.cpp](src/snapshot.cpp)), taken right after connecting. At the
end each one is set back and checked, then everything is read back and
compared; any setting that could not be read at the start, could not be set,
or differs afterwards is listed and the run fails (exit code 5). **Your stored
macros can change settings outside the snapshot** (audio, for example) when
the macro tests run them; those are not restored.

It will not start (exit code 4, nothing changed) while a macro is being
recorded or is running: the sweep's commands would end up in the recording.

It does visibly and audibly change the live output while it runs (inputs
switch, the PiP moves, macros run, a short fade to black, the HDMI output's
source and audio levels change for a moment), so run it when the switcher is
not in use; it asks before starting (`--yes` skips the question).

The few SDK calls it never makes are listed, with reasons, in
[coverage/excluded.txt](coverage/excluded.txt). Camera actions (autofocus)
only run with `--allow-camera`, mic plug-in power only with
`--allow-mic-power`.

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

# Full backup / restore of the switcher (see Backup)
atem-sweep 192.168.0.240 --backup
atem-sweep 192.168.0.240 --restore backups\<folder>
atem-sweep --compare backups\A backups\B

# Make a golden record from a run (drops transferred picture bytes)
python slim_record.py runs\<run> golden\<name>
```

Opt-in actions: `--allow-camera` (autofocus on a Blackmagic camera),
`--allow-mic-power` (plug-in power on the mic inputs).

For other programs: `--groups connect,generated,manual,storage,scenario`
runs only those groups, `--json` also prints the plan, each test's result and
the run's phases as `@@` + one JSON object per line, `--stop-file PATH` stops
after the current test once the file exists (settings and stored content are
put back as usual), and `--protect` backs up the stored content before a
recording and restores and checks it after, even without the storage tests.
With the backup, a real-ATEM run takes three backups: `-before`, `-swept`
(what the sweep left, for troubleshooting) and `-after` (after restoring,
compared with `-before`; the verdict is in `-after\restore-check.json`).

### atem-sweep-gui

`atem-sweep-gui.exe` (next to `atem-sweep.exe`) shows the sweep as a
coverage map: every test is a square, grouped as below, coloured as it runs
(same as the real ATEM, differs, skipped).

- **Emulator** verifies the emulator against a golden record. The GUI starts
  `atem-emu --listen 127.0.0.2` with the matching profile, and only starts
  the sweep once that process owns 127.0.0.2:9910 (otherwise the SDK could
  fall back to the real ATEM on USB). It stops the emulator afterwards.
- **Real ATEM** records a new run: it asks first, then backs up, sweeps, puts
  the settings back, backs up what the sweep left, restores the stored
  content and checks it; the steps show above the map. **Stop** finishes the
  current test and puts everything back.
- A checkbox per group takes it out of the run. The right-hand list drills
  in: interface, then property, then each value, with both values where the
  emulator differs; pointing at a row lights up its squares.
- **Backup ATEM** (real ATEM only) shows the backups grouped by sweep, with
  **Backup now** and a **Restore** per backup (asks first; checks after).
- **Download report** saves every test with its source, values and SDK
  functions as a Markdown file.

`--screenshot FILE` (with `--iface NAME [--prop NAME]` or `--backups`) saves
a view and exits; `--selftest GROUPS --screenshot FILE` runs those groups
against the emulator first.

### Test groups

| Prefix | Tests | What |
|---|---:|---|
| `connect.` `switcher.` `input.` `me.` `trans.` `key.` `fly.` `dve.` `pip` `dsk.` `macro.` `media.` `audio.` `hyperdeck` `camera` `record` `stream` | 273 | the original sweep: everything obs-atem and the SDK samples use |
| `probe.` | 2 | read-only: which interfaces and features the switcher has |
| `g.` | 1,563 | generated from the SDK (`coverage/gen_tests.py`): every setting of 26 interfaces with good and bad values, getters, resets, callbacks; each test puts back every value of the object it touched |
| `m.` | 23 | hand-written: several-argument calls, keyframe store/clear, iterator lookups |
| `s.` | 20 | stored content: macros and stills listed, created, changed, deleted — backup-protected |

Needs ATEM Software Control installed (the SDK's COM library) and Qt's `bin`
folder on `PATH` (or run `windeployqt atem-sweep.exe`).

## Backup

A full backup of the switcher through the SDK, so that tests which create,
change and delete stored content (macros, stills) can later run safely and
everything can be put back. Backups are a safety net and a troubleshooting
aid only; the reverse-engineering data is the sweep's recorded responses.

```powershell
atem-sweep 192.168.0.240 --backup            # read-only; into backups\<time>
atem-sweep --compare backups\A backups\B     # 0 = identical, 1 = differences
atem-sweep 192.168.0.240 --restore backups\A # put macros + stills back, then verify
```

| In the backup folder | What |
|---|---|
| `manifest.json` | target, product, time, every macro and still slot in use (name, description, size, SHA-256; stills also the switcher's own hash, size and pixel format), and any problem |
| `state.txt` | the connect dump: every state field the switcher sends a new client (364 on the ATEM Mini), one `NAME HEX` per line. Needs Ethernet: over USB there is no state |
| `macros/NN.bin` | each stored macro's bytes (`MacroPool::Download`) |
| `stills/NN.raw` | each stored still's frame as the switcher sends it (the ATEM Mini: 10-bit YUVA, 1920×1080) |

```text
┌──────────┐    ┌──────────────────────┐    ┌──────────┐    ┌──────────┐    ┌──────────────┐
│ backup A │───►│ tests: create, list, │───►│ backup B │───►│ restore  │───►│ backup C     │
│          │    │ change, delete, ...  │    │          │    │ from A   │    │ compare C, A │
└──────────┘    └──────────┬───────────┘    └──────────┘    └──────────┘    └──────────────┘
 state, macros,            ▼                 what the tests  macros and      must be
 stills          results.json, wire.jsonl    left behind     stills          identical
                 (the reverse-engineering
                 data)
```

Backups A, B and C are only a safety net and a troubleshooting aid; the tests'
recordings are the reverse-engineering data.

Taking a backup only downloads; it never changes the switcher. A backup that
misses anything says so (`"complete": false`, exit code 3). Backups go to
`sweep/backups/`, which git ignores: they hold your stored macros and
stills.

**Restore** checks every backup file's checksum and that it is the same
product, refuses while a macro runs or records, and asks first (`--yes`
skips). Macros are uploaded when their bytes differ, renamed when only the
name or description does, deleted when the backup's slot is empty; stills
the same, by the switcher's hash. Settings are not written by the restore
(the sweep puts back the settings its tests change). Then it waits 10 s,
takes a new backup and compares it with the one restored: exit 0 only when
identical, 5 when anything differs.

Verified on the real ATEM Mini (2026-09-28): backup A, restore a test backup
(a macro and a still added in empty slots, a macro and a still renamed),
restore A, verification backup identical to A — all 364 state fields, the
4 macros' bytes and the 3 stills' hashes.

Behaviour of the real switcher found on the way:
- Macro names are cut to 20 characters.
- For a few seconds after stored content changes, a new client's connect
  dump still shows some old content (e.g. a deleted macro as used) and the
  switcher corrects it with updates right after the dump; hence the 10 s
  wait before verifying.
- Unused bytes of many state fields hold leftovers (pieces of names); they
  change with the stored content and come back when it does.

## Golden record

Two records of the real ATEM Mini (protocol 2.30, SDK 10.2.1), each
`results.json`, `wire.jsonl` and `coverage.txt`:

| Record | Tests | Use |
|---|---:|---|
| [golden/atem-mini_sdk10.2.1_proto2.30/](golden/atem-mini_sdk10.2.1_proto2.30) (2026-09-26) | 273 | what the emulator passes today; its profile ([../core/profiles](../core/profiles)) is built from it |
| [golden/atem-mini_sdk10.2.1_proto2.30_2026-09-28/](golden/atem-mini_sdk10.2.1_proto2.30_2026-09-28) | 1,881 (1,864 passed, 17 skipped) | the full sweep: every safe SDK function the Mini has, storage included — the target for emulator work |

The 2026-09-28 record was taken in a backup-protected run (backup, sweep,
storage restore verified, final backup identical to the first). Its
`wire.jsonl` has 22,823 packets; file-transfer payloads (the bytes of the
switcher's stills and macros) are cut to their header plus length and
SHA-256 by `slim_record.py`, so the record carries no pictures; the full run
is kept locally under `runs/`.

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

```text
record: the real ATEM (Ethernet)

┌────────────┐  SDK calls  ┌─────────────────┐  UDP 9910  ┌────────────────┐
│ atem-sweep │────────────►│ recording proxy │───────────►│ real ATEM Mini │
│ tests      │◄────────────│ 127.0.0.1:9910  │◄───────────│ 192.168.0.240  │
└─────┬──────┘  responses  └────────┬────────┘            └────────────────┘
      │         and events          │ every packet, both ways
      ▼                             ▼
results.json                    wire.jsonl
per test: return codes,         every packet, split
read-backs, SDK events          into ATEM fields

verify: the emulator

┌────────────┐             ┌─────────────────┐            ┌────────────────┐
│ atem-sweep │────────────►│ recording proxy │───────────►│ atem-emu       │
│ --verify   │◄────────────│ 127.0.0.1:9910  │◄───────────│ 127.0.0.2:9910 │
└─────┬──────┘             └─────────────────┘            └────────────────┘
      │ every test compared with the golden record
      ▼
269 passed, 0 failed, 4 skipped
```

Over Ethernet the SDK is pointed at a small UDP proxy on `127.0.0.1:9910`
which forwards to the ATEM and keeps every packet. Each test therefore shows
which command bytes an API call produced and which state fields the ATEM sent
back — for good input and for bad.

## Coverage

### The six categories

Every callable SDK method (1,264: the current interfaces, without the
callback interfaces a client implements) is in exactly one category.
[coverage/categories.py](coverage/categories.py) sorts them from an emulator
verify run and writes two lists:

| List | Category | Methods (2026-09-29) | Meaning |
|---|---|---:|---|
| [api-supported.tsv](coverage/api-supported.tsv) | 1 emulator | 252 | recorded on the real ATEM, emulated and verified |
| | 2 samples | 1 | used by the SDK samples, recorded, emulator to do |
| | 3 sweep | 391 | every other safe function the Mini has: 390 recorded (emulator to do), 1 not recorded (mic plug-in power, opt-in) |
| | 4 hardware | 74 | safe and on the Mini, but needs hardware this setup lacks: a HyperDeck, a Blackmagic camera ([needs-hardware.txt](coverage/needs-hardware.txt)) |
| [api-unsupported.tsv](coverage/api-unsupported.tsv) | 5 not on mini | 541 | the ATEM Mini doesn't have it — all recorded by the probe ([not-on-mini.txt](coverage/not-on-mini.txt)) |
| | 6 destructive | 5 | can't be undone: the startup state, the video mode, HyperDeck recording ([excluded.txt](coverage/excluded.txt)) |
| | **total** | **1,264** | |

```powershell
python coverage\categories.py --verify <emulator verify run>\results.json --record <real ATEM run>\results.json
```

Category 1 counts a method only when every test that calls it passed in the
emulator verify run (each result lists its SDK calls). How methods move
between categories:

```text
 3 sweep, 2 samples ──add a sweep test, record the real ATEM──► recorded
 recorded ──extend core/src/device.cpp until verify passes──► 1 emulator
 4 hardware ──connect the HyperDeck / Blackmagic camera──► 3 sweep
 not-on-mini.txt entry ──probe.interfaces / probe.features on the real ATEM──► 5, or back to 3
```


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
