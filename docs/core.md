# Emulator core (atem-emu)

The switcher itself: the ATEM Mini's network protocol, its state and its
behaviour, rebuilt from recordings of the real device made with
[atem-sweep](../sweep/README.md). No video — the emulator app ([../src](../src),
`atem-emulator.exe`) runs on this core and adds the window, picture and
virtual camera; `atem-emu.exe` is the same core with a console instead.

| Check against the real ATEM's golden records | Result |
|---|---|
| full sweep (2026-09-28), 1,881 tests | 1,858 same, 6 differ, 17 skipped (2026-09-29); none of the six is left in the emulator, see [sweep/README.md](../sweep/README.md#golden-records) |
| original record (2026-09-26), 273 tests | 267 same; the other 6 are sweep tests changed since |

## Run

```powershell
atem-emu                               # all addresses, UDP 9910
atem-emu --listen 127.0.0.2            # one address (leaves 127.0.0.1 to atem-sweep's proxy)
atem-emu --profile <folder>            # another recorded switcher (default: atem-mini_proto2.30)
atem-emu --port 9911                   # another port
atem-emu --verbose                     # log every command and answer
```

Connect anything that talks to an ATEM through the SDK (the obs-atem plugin,
`atem-cli --ip`, atem-sweep) to this PC's address. Check it against the real
switcher (from `core/`; more in [sweep/README.md](../sweep/README.md)):

```powershell
atem-emu --listen 127.0.0.2 --profile profiles\atem-mini_proto2.30_2026-09-28
..\sweep\build\Release\atem-sweep 127.0.0.2 --verify ..\sweep\golden\atem-mini_sdk10.2.1_proto2.30_2026-09-28\results.json
```

## How it works

```text
SDK clients (obs-atem, atem-sweep, ...)      emulator window (../src)
      │ UDP 9910                                   │ emu::cmd builds the same
      ▼                                            │ commands a client sends
┌──────────────────────────────┐                   │
│ Server (server.cpp)          │                   │
│ handshake, acks, resends,    │                   │
│ one session per client;      │                   │
│ state answers once per frame │                   │
└──────────────┬───────────────┘                   │
               │ commands                          │
               ▼                                   │
┌──────────────────────────────┐                   │
│ Device (device.cpp)          │◄──────────────────┘
│ setter table + handlers,     │
│ macros, transfers            │
└──────────────┬───────────────┘
               │ changes fields in place
               ▼
┌──────────────────────────────┐
│ field store (fields.cpp)     │  starts as the connect dump
│ the switcher's whole state   │  recorded from the real ATEM
└──────────────┬───────────────┘  (profiles/)
               │
               ▼
at the next video frame the changed fields go to every client;
the window redraws from Device::view()
```

| File | Role |
|---|---|
| `core/src/server.*` | UDP transport: handshake, sessions, reliable packets, acknowledgements, resends, several clients, the per-frame answers |
| `core/src/device.*` | The switcher: state, the setter table, command handlers, macros, file transfers, a typed view for UIs |
| `core/src/setters_table.inc` | Generated from `tools/setters_spec.py` |
| `core/src/fields.*` | Field store: the state as raw field payloads, in dump order |
| `core/src/commands.*` | Builders for command payloads, so a local UI uses the same handlers as a client |
| `core/src/main.cpp` | atem-emu: options, console log |
| `core/tools/` | `setters_spec.py` (the setter table's source), `check_setters.py`, `gen_setters.py`; `infer_layout.py`, `show_command.py`, `show_transfer.py` for reading golden records |
| `core/make_profile.py` | A profile from a golden record (`--backup` adds the stored macros' bytes) |
| `core/profiles/atem-mini_proto2.30/` | From the 2026-09-26 record: `dump.txt` (the connect dump) and `macros.txt` (what each stored macro changed). The default |
| `core/profiles/atem-mini_proto2.30_2026-09-28/` | From the full sweep, plus `macro-bytes.txt` |

### The state is the connect dump

The emulator starts from the exact bytes the real switcher sends a new
client: 364 fields of 75 kinds, in the same order and packets. Commands
change those fields in place, and new clients get the current state. (An
earlier emulator with ~10 hand-built fields was rejected by the SDK as corrupt
data, `cfcd`.)

### Transport

Handshake, session ids, reliable packets, acknowledgements and resends as
captured from the real switcher. Two timing rules matter to the SDK:

- **Once per frame.** The switcher applies each command at once but answers
  at the next video frame (1080p24: 42 ms) with each changed field's content
  *at that moment*: a change undone within the frame is never seen. The
  emulator queues state answers and sends them the same way; answers for the
  asking client alone (file transfers, the lock) go at once, as on the
  device.
- **Time code.** Every state packet starts with a `Time` field (the time
  code), and a time code request (`TiRq`) is answered with it. Without it the
  SDK reported a media pool lock as busy.

### Commands and the rules the recordings showed

**Setters from a table.** Most "set value" commands are `[mask][key][values]`
and change one field the same way. Their layouts and rules are data:
[core/tools/setters_spec.py](../core/tools/setters_spec.py) describes 22 commands (DVE key
and keyframes, luma / pattern / advanced chroma key, colour generators, aux,
mix / dip / wipe / DVE transitions, time code, the Fairlight source,
compressor, limiter, expander, EQ bands, master, mic level, input
configuration). [check_setters.py](../core/tools/check_setters.py) replays
every such command in a golden record through the table and compares with
what the switcher sent back — before any C++ is built;
[gen_setters.py](../core/tools/gen_setters.py) writes `src/setters_table.inc`,
which `Device::applySetter` runs. What the recordings showed:

- clamps (a transition rate of 0 is 1, compressor ratio 1.2–20, EQ frequency
  inside the band's range, …); hue and light direction wrap at 360°; keyframe
  sizes and positions keep their whole part in 16 bits; negative whole-number
  crop values are stored one lower (-1.000 → -1.001)
- refused values change nothing and get no answer (sources a transition or
  aux can't use, EQ shapes and ranges a band doesn't support, bevel on the DVE)
- an accepted value that doesn't change is answered again — except sources,
  transition rates and all audio values
- side effects: a pattern sets its symmetry, a keyframe value marks the
  keyframe stored, the next transition's rate is also its frames remaining,
  the DVE transition's key settings are the stinger's too, the chroma sample
  cursor stays inside the frame for its size

**Handlers** for everything else: program / preview / cut / auto / T-bar,
fade to black (and cut to black), key type / fill / cut / on air / mask,
run to keyframe, store / clear keyframe, DSK, input names, macros, the media
player, locks, audio resets, file transfers. Behaviour they copy:

- A command naming an input the M/E can't use (Program, Preview, Camera 1
  Direct) is ignored; nothing is sent back.
- Names are copied like `strcpy`, so bytes after the terminator keep the old
  name. "Camera 1 Direct" follows camera 1's long name, shortened to
  "xxxxxxxxxx... Direct" when too long.
- The key remembers a fill per key type, so changing the type restores that
  type's fill.
- There is one DVE. A DVE transition takes it from the key: the key keeps its
  type but can't be DVE or fly. A selection or style that would need a DVE
  key in the next transition is refused ("DVE unavailable").
- Changing the transition style sets the frames remaining to that style's
  rate.
- Auto transitions, T-bar, fade to black and DSK auto run frame by frame.
  Full black drops the program tally.
- Storing, clearing and running to a keyframe update which keyframes the key
  is at.
- A macro runs after the rest of its packet, so run + stop in one packet
  stops it before it does anything.

**File transfers** (`FTSU` / `FTSD` / `FTDa` / `FTUA` / `FTFD` / `FTAD` →
`FTCD` / `FTDa` / `FTDC` / `FTDE`): macros and stills download in 1,396-byte
chunks, each acknowledged; uploads end with the new macro or still
properties and `FTDC`. Stills are run-length encoded as on the device;
stills from the profile have no recorded picture and download as one flat
colour, uploaded stills download as uploaded. Capture, rename, clear and
clear-all work on the media pool.

**Macros** are the switcher's 100-slot pool. Recording stores every command
the switcher receives until "stop recording", with pauses and user waits;
running replays them through the same handlers, frame timed, with loop and
continue. Macro bytes are little-endian ops `[length][op][values]`: recorded
macros are written as bytes and uploaded bytes run, for the ops known so far
(preview input, pause, user wait). The profile's own macros are replayed
approximations: the whole fields the real macro changed when atem-sweep ran
it, all at once — their bytes (from a backup) download exactly, but ops
0x26, 0x47, 0x48, 0x4a, 0x4b, 0x54 and 0x56 aren't decoded yet.

Unknown commands are acknowledged and logged as unhandled.

## Limits

- It emulates what atem-sweep records. Not yet: audio level meters
  (`SFLN` → `FMLv` / `FDLv`), camera control and HyperDecks (need the hardware
  to record, see [sweep/README.md](../sweep/README.md#hardware)). Recording and
  streaming don't exist on the base ATEM Mini.
- ATEM Software Control against the emulator is not verified yet.
- The transport assumes a well-behaved client on a clean network: packets
  arriving out of order can lose a command, and the session id is not checked
  after the handshake. Malformed commands are not validated, some handlers
  ignore the M/E index (the Mini has one), and media locks have no owner.
- There is no video (the emulator app draws the picture).

## Build

```powershell
cd core
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DQt6_DIR="D:/ProgramFiles/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6"
cmake --build build --config Release
```

Needs Qt 6 Core and Network. The executable finds its profile next to itself
(`profiles/`, via `cmake --install`) or in `core/profiles`. The top-level
build (`build-gui`) builds it too, as `build-gui\core\Release\atem-emu.exe`.
