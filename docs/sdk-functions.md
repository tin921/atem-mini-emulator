# SDK functions: coverage, tests and records

What atem-sweep covers of the Blackmagic switcher SDK (BMDSwitcherAPI 10.2.1)
on the ATEM Mini, how the tests are grouped, what a verification compares,
and the recorded results. How to run the sweep: [sweep.md](sweep.md).

The goal is every SDK function the Mini has and that is safe to call,
recorded well enough that adding it to the emulator core just works.
Priority: what obs-atem and the emulator use, then the SDK samples, then
everything else.

## Coverage

Every callable SDK method (1,264: the current interfaces, without the
callback interfaces a client implements) is in exactly one of six
categories. [categories.py](../sweep/coverage/categories.py) sorts them from
an emulator verify run and real-ATEM record runs, into
[api-categories.txt](../sweep/coverage/api-categories.txt) and two lists:

| List | Category | Methods (2026-09-29) | Meaning |
|---|---|---:|---|
| [api-supported.tsv](../sweep/coverage/api-supported.tsv) | 1 emulator | 630 | recorded on the real ATEM, emulated and verified |
| | 2 samples | 15 | used by the SDK samples, recorded, emulator to do |
| | 3 sweep | 10 | other safe functions the Mini has, emulator to do (9 recorded, 1 not yet) |
| | 4 hardware | 70 | on the Mini and safe, but need a HyperDeck or a Blackmagic camera, which this project doesn't have ([Hardware](#hardware)) |
| [api-unsupported.tsv](../sweep/coverage/api-unsupported.tsv) | 5 not on mini | 534 | the ATEM Mini doesn't have it — each recorded by the probe ([not-on-mini.txt](../sweep/coverage/not-on-mini.txt)) |
| | 6 destructive | 5 | can't be undone ([Excluded functions](#excluded-functions)) |
| | **total** | **1,264** | |

```powershell
python coverage\categories.py --verify <emulator verify run>\results.json --record <real ATEM run>\results.json
```

Category 1 counts a method only when every test that calls it passed in the
emulator verify run (each result lists its SDK calls). How methods move:

```text
 3 sweep, 2 samples ──add a sweep test, record the real ATEM──► recorded
 recorded ──emulate in the core until verify passes──► 1 emulator
 4 hardware ──someone connects a HyperDeck / Blackmagic camera──► 3 sweep
 not-on-mini.txt entry ──probe.interfaces / probe.features on the real ATEM──► 5, or back to 3
```

### Call tracking

Every SDK call goes through `SDK_CALL(...)`, which records it as it runs, so
coverage reflects calls that really happened. The report also checks
[tier1-plugin.txt](../sweep/coverage/tier1-plugin.txt) (every SDK call
obs-atem makes) and [tier2-samples.txt](../sweep/coverage/tier2-samples.txt)
(every call in the SDK's Windows samples) against
[sdk-api.json](../sweep/coverage/sdk-api.json) (every interface and method in
`BMDSwitcherAPI.h`); a method counts when it was called, when its interface
is not on this model (the SDK refuses it, which is recorded), or when it is
excluded by policy. The lists are regenerated with
[scan_usage.py](../sweep/coverage/scan_usage.py).

## Test groups

| Group (GUI) | Prefix | Tests | What |
|---|---|---:|---|
| Connect & probe | `connect.` `switcher.` `probe.` | 8 | connecting (good, bad and unreachable addresses, a second client), which interfaces and features the switcher has |
| Every setter | `g.` | 1,563 | generated from the SDK ([gen_tests.py](../sweep/coverage/gen_tests.py)): every setting of 26 interfaces with good and bad values, getters, resets, callbacks; each puts back every value of the object it touched |
| Hand-written SDK calls | `m.` | 23 | several-argument calls, keyframe store/clear, iterator lookups |
| Stored macros & stills | `s.` | 20 | listed, created, changed, deleted, uploaded, downloaded, a still captured — backup-protected |
| Scenarios | `input.` `me.` `trans.` `key.` `fly.` `dve.` `pip` `dsk.` `macro.` `media.` `audio.` `hyperdeck` `camera` `record` `stream` | 267 | what obs-atem and the SDK samples do, as sequences |

`--groups` takes the group names `connect`, `generated`, `manual`,
`storage`, `scenario`.

The generator ([gen_tests.py](../sweep/coverage/gen_tests.py)) has three
lists worth knowing when a regeneration changes the tests (diff them before
running):

- `NEVER` — methods that change stored content (`MediaPool::Clear`,
  `StillCapture::CaptureStill`): never generated; only the storage tests
  call them, inside a protected run.
- `OPT_IN` — setters that need a flag: switching the mic plug-in power *on*
  needs `--allow-mic-power`.
- `UNDEFINED_UNLESS` — getters that return garbage unless another getter
  says the feature exists: `GetCameraModel` without a camera (4294967295 on
  the real ATEM, 0 or 2146862410 against the emulator) is recorded as
  informational.

## Hardware

The ATEM Mini alone covers everything except the functions that need a
device connected to it: 74, of which 70 are not covered (the other 4, the
HyperDeck and camera lists, answer the same as the real Mini with nothing
connected). Without the device the switcher only answers "nothing
connected", so a recording wouldn't show real behaviour
([needs-hardware.txt](../sweep/coverage/needs-hardware.txt)).

| Needs | Functions | What they cover |
|---|---:|---|
| a HyperDeck on the network | 49 | connection, clips, play, cue, loop, shuttle, remote (`IBMDSwitcherHyperDeck`, `…Clip`, iterators) |
| a Blackmagic camera on an HDMI input | 25 | camera control: focus, iris, gain, white balance, shutter, colour correction (`IBMDSwitcherCameraControl`, parameter iterator) |

**This project has neither a HyperDeck nor a Blackmagic camera**, so these
functions are not recorded and the emulator has no HyperDeck or camera
control. The `hyperdeck` and `camera` tests still run: they record what the
Mini answers with nothing connected, and report the setup below as
`setup (informational)` (in the console, the GUI and the report).

Anyone who has the hardware can add them:

- **HyperDeck.** Any HyperDeck with Ethernet (e.g. HyperDeck Studio HD Mini)
  on the ATEM's network with a fixed IP and **Remote** on; media with 2–3
  short clips (the clip tests list, cue and play them). In ATEM Software
  Control > Settings > HyperDeck, put its IP in slot 1 and wait for
  "Connected". The ATEM Mini has 4 HyperDeck slots. `HyperDeck::Record`
  stays excluded (it writes to the disk).
- **Blackmagic camera.** A Pocket Cinema Camera 4K/6K, Micro Studio 4K or
  Studio Camera on an HDMI input; in its setup menu **Camera ID** = that
  input's number; HDMI out at the ATEM's video format; a lens with electronic
  focus and iris (a manual lens only answers "no lens"). Actions that move
  the lens (autofocus) only run with `--allow-camera`.

Then remove its lines from
[needs-hardware.txt](../sweep/coverage/needs-hardware.txt), regenerate the
tests (`python coverage/gen_tests.py <BMDSwitcherAPI.idl>`), record, and
emulate the device in the core.

## Excluded functions

Never called ([excluded.txt](../sweep/coverage/excluded.txt)):

| Function | Why |
|---|---|
| `SaveRecall::Save`, `SaveRecall::Clear` | overwrite / clear the saved startup state, which can't be read, so it can't be put back |
| `Switcher::SetVideoMode`, `SetAutoVideoMode` | every output goes black for a while, and it can clear the media pool |
| `HyperDeck::Record` | writes to the HyperDeck's disk |
| `CameraControl::SetFlags` | moves the lens (autofocus); runs only with `--allow-camera` |
| `RecordAV::Start/StopRecording`, `SwitchDisk`, `StreamRTMP::Start/StopStreaming`, `Clip::UploadFrame/SetValid/SetInvalid` | not on the ATEM Mini anyway |

## The USB fallback

When an address doesn't answer, the SDK falls back to an ATEM on this PC's
USB. `connect.bad-address` and `connect.unreachable` therefore connect
(read-only, then disconnect) to the real ATEM if it is on this PC's USB, even
in a run against the emulator; their outcome is recorded as informational.
Unplug USB for a fully isolated emulator run.

## Golden records

Records of the real ATEM Mini (protocol 2.30, SDK 10.2.1) in
[sweep/golden/](../sweep/golden), each `results.json`, `wire.jsonl` and
`coverage.txt`:

| Record | Tests | Real ATEM | Emulator (last check) |
|---|---:|---|---|
| [atem-mini_sdk10.2.1_proto2.30](../sweep/golden/atem-mini_sdk10.2.1_proto2.30) (2026-09-26) | 273 | 269 passed, 4 skipped (empty macro slots) | 267 same; the other 6 are tests changed since (2026-09-29) |
| [atem-mini_sdk10.2.1_proto2.30_2026-09-28](../sweep/golden/atem-mini_sdk10.2.1_proto2.30_2026-09-28) | 1,881 | 1,864 passed, 17 skipped | 1,858 same, 6 differ, 17 skipped (2026-09-29) |

The 2026-09-28 record is the full sweep, taken in a backup-protected run
(final backup identical to the first); its `wire.jsonl` has 22,823 packets.
File-transfer payloads (the bytes of the switcher's stills and macros) are
cut to their header plus length and SHA-256 by
[slim_record.py](../sweep/slim_record.py), so the record carries no pictures.

Of the emulator's six differences, none is left in the emulator: the media
pool's "lock busy" callback turned out to fire or not for the same packets
on the real ATEM (so it is no longer compared), and the others and the 17
skipped tests were fixed in the tests themselves (camera model undefined
without a camera, keyframe put-back independent of frame timing, the
USB-fallback connections informational, empty macro slots and the talkback
/ mic power tests now run). Those test changes need a **new recording** of
the real ATEM before they count; until then both golden records show them as
differences.

### What a verification compares

Per test: the SDK's return codes and read-backs (`obs`), and *which kinds*
of SDK callback fired (a set: not their order, count or arguments); plus the
connect dump's field counts. Not compared: the wire bytes (kept in
`wire.jsonl` for looking them up), keys marked `(informational)` — values
that don't come from the switcher (timings, the SDK's leftovers for things
the Mini doesn't have, the USB fallback, the hardware notes) — the tally /
time-code events, and the media pool's "lock busy" (fired or not by the SDK
for the same packets). Many getters read the SDK's own cached state and some
bad input is refused inside the SDK, so a match shows the emulator answers
this sequence of tests like the device — not that it behaves the same in
every situation.

Checks on the run itself: the golden record must be a passing recording
(every test pass or skip, no duplicate ids) or it is refused; a full
verification fails if a test of the golden record is missing; a test the
device passed that is skipped here counts as a difference.

## Output

Each run writes `sweep\runs\<time>-<mode>\` (git-ignored):

| File | Contents |
|---|---|
| `results.json` | Per test: status, every observation (return codes, read-backs), the SDK callbacks fired, the SDK functions called (`sdk`), and — through the proxy — the ATEM fields sent (`tx`) and received (`rx`) during that test. Verify runs add `diffs`. |
| `wire.jsonl` | Every UDP packet, both directions, timestamped and split into fields: the raw material for the emulator. |
| `coverage.txt` | The coverage report ([Coverage](#coverage)). |
