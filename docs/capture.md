# Protocol capture & reverse engineering

The ATEM Mini's UDP protocol is undocumented. Blackmagic publishes the
BMDSwitcherAPI COM SDK but not the wire format underneath it, and no
emulator. The emulator's knowledge comes from recording the real switcher.

---

## How the emulator is built from the real switcher

1. **Record.** [atem-sweep](../sweep) drives the real ATEM through the SDK
   (good and bad input), with a UDP proxy on `127.0.0.1:9910` recording every
   packet both ways. Result: a golden record (`results.json`, `wire.jsonl`,
   `coverage.txt`), see [sweep/golden](../sweep/golden).
2. **Profile.** [core/make_profile.py](../core/make_profile.py) takes the
   connect dump (every field the switcher sends a new client, up to `InCm`)
   and what each stored macro changed, into
   [core/profiles](../core/profiles).
3. **Behaviour.** [core/src/device.cpp](../core/src/device.cpp) has one
   handler per command, written from the recorded command/reply pairs (byte
   layouts, checks, clamps, quirks — see [core/README.md](../core/README.md)).
4. **Verify.** atem-sweep runs the same tests against the emulator and
   compares with the golden record:

   ```powershell
   atem-emu --listen 127.0.0.2
   atem-sweep 127.0.0.2 --verify sweep\golden\atem-mini_sdk10.2.1_proto2.30\results.json
   ```

What verification compares: the SDK's return codes and read-backs, and which
kinds of SDK event fired, per test (as a set, not in order), plus the connect
dump's field counts and protocol version. It does not compare the wire bytes
or event arguments; `wire.jsonl` is kept for looking them up.

---

## Capturing another client

To see what a client such as ATEM Software Control sends, run only the
recording proxy and point the client at `127.0.0.1`:

```powershell
atem-sweep 192.168.0.240 --capture 600 --out runs\asc   # to the real ATEM
atem-sweep 127.0.0.2 --capture 600 --out runs\asc-emu    # to the emulator
```

It stops after the given seconds, or when a file named `stop` appears in the
output folder, and writes `wire.jsonl`.

---

## Updating after a firmware or SDK change

1. Record a new golden record on the updated device (`atem-sweep <ip>`),
   in its own folder under `sweep/golden`.
2. Build a profile from it with `make_profile.py`.
3. Verify the emulator against it and extend `device.cpp` until it passes.

Keep the old record: the differences are the change.

---

## Other tools

- `tools/capture-bmd.cpp` (`capture.exe`): lists what the SDK exposes over
  USB or Ethernet (macro names, run status). SDK-level only, no packet bytes;
  see [capture.exe.md](capture.exe.md).

## Protocol reference

- **BMDSwitcherAPI.h / .idl** (SDK) — the interfaces the fields belong to.
- **[LibAtem / AtemUtils](https://github.com/LibAtem/AtemUtils)** and
  atem-connection — community documentation of the wire protocol
  (third-party; the recordings win where they disagree).
