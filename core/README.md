# atem-emu — emulator core

A headless ATEM Mini emulator, rebuilt from recordings of the real switcher.
It answers the Blackmagic switcher SDK the way the device does. The recordings
come from the [atem-sweep](../sweep) golden record, and the emulator passes
every test in it:

| Check | Result |
|---|---|
| atem-sweep verify, current golden record (2026-09-28) | 269 passed, 0 failed, 4 skipped (empty macro slots) |
| same, second run on the same emulator | 269 passed |
| original golden record (different start state) | 268 passed; the 1 difference is a test whose output format changed since |

It is the protocol and the switcher's behaviour, no video. The emulator app
([../src](../src), `atem-emulator.exe`) runs on it and adds the window,
picture and webcam; `atem-emu.exe` is the same switcher without a window.
The app passes the same check (`atem-emulator --reference --listen 127.0.0.2`).

## Run

```powershell
atem-emu                          # all addresses, UDP 9910
atem-emu --listen 127.0.0.2       # a loopback address of its own (see below)
atem-emu --verbose                # log every command and reply
atem-emu --profile <folder>       # another recorded switcher
```

Connect anything that speaks to an ATEM through the SDK (the obs-atem plugin,
`atem-cli --ip`, atem-sweep) to this PC's address. ATEM Software Control
against the emulator is not yet verified.

### Checking it against the real switcher

```powershell
atem-emu --listen 127.0.0.2
atem-sweep 127.0.0.2 --verify ..\sweep\golden\atem-mini_sdk10.2.1_proto2.30\results.json
```

On 127.0.0.2 the sweep can keep its recording proxy on 127.0.0.1:9910, so
the run also compares the connect dump field by field and captures the
emulator's traffic (`wire.jsonl`) for comparison with the real device's.

## How it works

```text
SDK clients (obs-atem, atem-sweep, ...)      emulator window (src/)
      │ UDP 9910                                   │ emu::cmd builds the same
      ▼                                            │ commands a client sends
┌──────────────────────────────┐                   │
│ Server (server.cpp)          │                   │
│ handshake, acks, resends,    │                   │
│ one session per client       │                   │
└──────────────┬───────────────┘                   │
               │ commands                          │
               ▼                                   │
┌──────────────────────────────┐                   │
│ Device::apply (device.cpp)   │◄──────────────────┘
│ one handler per command,     │
│ the macro pool               │
└──────────────┬───────────────┘
               │ changes fields in place
               ▼
┌──────────────────────────────┐
│ field store (fields.cpp)     │  starts as the connect dump
│ the switcher's whole state   │  recorded from the real ATEM
└──────────────┬───────────────┘  (profiles/)
               │
               ▼
changed fields go back through the Server to every client;
the window redraws from Device::view()
```

| File | Role |
|---|---|
| `src/server.*` | UDP transport: handshake, sessions, reliable packets, acknowledgements, resends, several clients |
| `src/device.*` | The switcher: state fields, one handler per command, the macro pool, a typed view for UIs |
| `src/commands.*` | Builders for command payloads, so a local UI uses the same handlers as a client |
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
  stops it before it does anything.

**Macros** are the switcher's 100-slot pool. Recording (`MSRc`) stores every
command the switcher receives until "stop recording", with pauses (`MSlp`) and
user waits; running replays those commands through the same handlers, frame
timed, with loop and continue. Names and descriptions change with `CMPr`,
slots are deleted with `MAct` 5. The recording, rename and delete layouts were
confirmed with the SDK's own `Record` / `RecordPause` / `StopRecording` /
`SetName` / `Delete` calls. While a macro waits for the user, `MRPr` byte 0 is
2 (waiting) rather than 3 (running + waiting): with both bits the SDK reports
"running" and never "waiting for user". That is the SDK's reading; the real
switcher's byte has not been recorded yet.

The profile's four macros (recorded from the real ATEM by atem-sweep) are an
approximation: they replay the whole fields the real macro changed when run
from that recording's starting state, all at once. A replayed field also
overwrites its other values (e.g. a `KeDV` echo carries crop and border too),
and a step that changed nothing at the time is missing.

Handled commands: CInL, RInL, CPgI, CPvI, CTPr, DCut, DAut, CTPs, CTTp, CTWp,
FtbA, FtbC, CKTp, CKeF, CKeC, CKOn, CKMs, CKDV, RFlK, CDsF, CDsC, CDsL, CDsT,
CDsR, CDsG, CDsM, DDsA, MAct, MRCP, MSRc, MSlp, CMPr, LOCK, MPSS, SCPS, CCmd.
Anything else is
acknowledged and logged as unhandled.

## Limits

- It emulates what the sweep exercises. Other commands are acknowledged but
  change nothing (logged as unhandled). That includes things an ATEM Mini
  does have and ATEM Software Control uses: the Fairlight audio mixer (the
  recording shows it), camera control, still upload/download, colour
  generators and several transition parameters. Recording and streaming do
  not exist on the base ATEM Mini.
- The profile's own macros are replayed approximations (above); macros
  recorded on the emulator run their real steps. The SDK can download a
  stored macro's bytes (`IBMDSwitcherMacroPool::Download` →
  `IBMDSwitcherTransferMacro::GetMacro` → `IBMDSwitcherMacro::GetBytes`), a
  better source for real macro steps than replayed fields — not used yet.
- The transport assumes a well-behaved client on a clean network: packets
  arriving out of order can lose a command, and the session id is not
  checked after the handshake. Malformed commands are not validated, and some
  handlers ignore the M/E index (the Mini has one). Media locks have no
  owner, and every reply goes to every client.
- There is no video (the emulator app draws the picture).

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
