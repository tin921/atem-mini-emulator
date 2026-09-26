# atem-emu — emulator core

A headless ATEM Mini emulator, rebuilt from recordings of the real switcher.
It answers the Blackmagic switcher SDK the way the device does. The recordings
come from the [atem-sweep](../sweep) golden record, and the emulator passes
every test in it:

| Check | Result |
|---|---|
| atem-sweep verify, current golden record | 269 passed, 0 failed, 4 skipped (empty macro slots) |
| same, second run on the same emulator | 269 passed |
| original golden record (different start state) | 268 passed; the 1 difference is a test whose output format changed since |

It has no UI and no video: it is the protocol and the switcher's behaviour.
The GUI emulator in [../src](../src) can adopt it later.

## Run

```powershell
atem-emu                          # all addresses, UDP 9910
atem-emu --listen 127.0.0.2       # a loopback address of its own (see below)
atem-emu --verbose                # log every command and reply
atem-emu --profile <folder>       # another recorded switcher
```

Connect anything that speaks to an ATEM (the obs-atem plugin, `atem-cli
--ip`, ATEM Software Control, atem-sweep) to this PC's address.

### Checking it against the real switcher

```powershell
atem-emu --listen 127.0.0.2
atem-sweep 127.0.0.2 --verify ..\sweep\golden\atem-mini_sdk10.2.1_proto2.30\results.json
```

On 127.0.0.2 the sweep can keep its recording proxy on 127.0.0.1:9910, so
the run also compares the connect dump field by field and captures the
emulator's traffic (`wire.jsonl`) for comparison with the real device's.

## How it works

| File | Role |
|---|---|
| `src/server.*` | UDP transport: handshake, sessions, reliable packets, acknowledgements, resends, several clients |
| `src/device.*` | The switcher: state fields plus one handler per command |
| `src/fields.*` | Field store: the state as raw field payloads, in dump order |
| `make_profile.py` | Builds a profile from a sweep golden record |
| `profiles/atem-mini_proto2.30/` | `dump.txt` (the connect dump, 364 fields) and `macros.txt` (what each stored macro changed) |

**The state is the connect dump.** The emulator starts from the exact bytes
the real switcher sent when the sweep connected: all 75 field types. That is
what the old emulator lacked: its ~10 hand-built fields made the SDK reject
the connection as corrupt data (`cfcd`). Commands change those fields in
place, and new clients get the current state.

**Commands** follow the byte layouts read off the wire capture. Each handler
was checked against the recorded command/reply pairs. Some behaviour the real
device shows, and the emulator copies:

- A command naming an input the M/E can't use (Program, Preview,
  Camera 1 Direct) is ignored; nothing is sent back.
- Negative DVE sizes are stored as 0. Hue and light direction wrap at 360°.
  Fade-to-black, DSK and wipe rates of 0 are stored as 1.
- Names are copied like `strcpy`, so bytes after the terminator keep the old
  name. "Camera 1 Direct" follows camera 1's long name, shortened to
  "xxxxxxxxxx... Direct" when too long.
- The key remembers a fill per key type (`KBfT`), so changing the type
  restores that type's fill.
- There is one DVE. A DVE transition takes it from the key: the key keeps its
  type but can't be DVE or fly. A transition selection that includes a DVE
  key is refused ("DVE unavailable").
- Auto transitions, T-bar, fade to black and DSK auto run frame by frame
  (1080p24: 42 ms). Full black drops the program tally.
- A macro runs after the rest of its packet, so run + stop in one packet
  stops it before it does anything. Macros replay the fields the real macro
  changed, as recorded.

Handled commands: CInL, RInL, CPgI, CPvI, CTPr, DCut, DAut, CTPs, CTTp, CTWp,
FtbA, FtbC, CKTp, CKeF, CKeC, CKOn, CKMs, CKDV, RFlK, CDsF, CDsC, CDsL, CDsT,
CDsR, CDsG, CDsM, DDsA, MAct, MRCP, LOCK, MPSS, SCPS, CCmd. Anything else is
acknowledged and logged as unhandled.

## Limits

- It emulates what the sweep exercises. Other commands are acknowledged but
  change nothing: audio mixer, camera control, media upload, recording and
  streaming (none of these exist on an ATEM Mini or are recorded yet).
- Macros replay recorded results; they don't execute macro steps. A macro
  that was never recorded only reports running and stopped.
- There is no video.

To support more, record the real switcher with atem-sweep (after adding
tests), rebuild the profile with `make_profile.py`, and extend `device.cpp`
until verify passes.

## Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DQt6_DIR="D:/ProgramFiles/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6"
cmake --build build --config Release
```

Needs Qt 6 Core and Network. The executable finds its profile next to itself
(`profiles/`, via `cmake --install`) or in this source folder.
