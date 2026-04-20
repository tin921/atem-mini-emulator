# Protocol Training & Reverse Engineering

The ATEM Mini uses an undocumented UDP protocol. This document covers how the
emulator's protocol knowledge was obtained — through live capture from real
hardware — and how to update it if the protocol ever changes.

---

## Overview

Blackmagic publishes the BMDSwitcherAPI COM SDK for Windows and macOS but does
not document the underlying UDP wire protocol. To build a faithful emulator,
the protocol was reverse-engineered by:

1. Connecting a real ATEM Mini to a PC running `capture.exe`
2. Recording the full session at the field level via the COM SDK
3. Annotating each field against the known SDK types and interface definitions
4. Hard-coding the initial state dump into the emulator to match exactly

---

## Capture tool

`capture.exe` (built from `tools/capture-bmd.cpp`) connects to the ATEM via
the BMDSwitcherAPI COM SDK and records macro names, descriptions, and protocol
run-status values. It works over USB (virtual ethernet) or direct Ethernet —
no separate Ethernet cable required.

See [tools.md](tools.md) for build and usage instructions.

```powershell
.\tools\build\Release\capture.exe                  # USB auto-detect
.\tools\build\Release\capture.exe 192.168.10.240   # Ethernet
# → writes captured-output.log
```

---

## What was captured and where it lives

| Protocol element | Source | Where used in emulator |
| --- | --- | --- |
| `_ver` field bytes | BMD SDK version info | `AtemState.cpp` — `buildStateDump()` |
| `_pin` product name | `IBMDSwitcher::GetProductName()` | `AtemState.cpp` — `fieldPin()` |
| `_top` topology bytes | `IBMDSwitcherMixEffectBlock` counts | `AtemState.cpp` — `buildStateDump()` |
| `InPr` (×14) input properties | `IBMDSwitcherInput` enumeration | `AtemState.cpp` |
| `MPrp` macro properties | `IBMDSwitcherMacroPool::GetName/Desc` | `AtemState.cpp` — `fieldMPrp()` |
| `MRPr` run-state field | `IBMDSwitcherMacroControl::GetRunStatus` | `AtemState.cpp` — `fieldMRPr()` |
| Handshake flags & timing | Observed from SDK connection timing | `AtemServer.cpp` |
| Command IDs (`CPgI`, `CKeO`, etc.) | Matched against SDK method calls | `AtemServer.cpp` — `dispatchCommand()` |

---

## Annotating a capture log

`capture.exe` writes a structured log (`captured-output.log`) with one entry
per SDK field:

```text
[macro 0] name="Opening Titles" desc="Fade in from black"
[macro 1] name="Camera 1 Wide" desc=""
[run-status] running=0 loop=0 index=65535
[product] "ATEM Mini"
[inputs] count=14
```

These values are then reflected in the byte arrays in `AtemState.cpp`. The
fixed protocol fields (`_ver`, `_top`, `InPr`) were captured once against
firmware 8.1.1 and are unlikely to change unless Blackmagic releases a major
protocol revision.

---

## Updating after a firmware change

If Blackmagic releases new ATEM firmware:

1. Re-run `capture.exe` against the updated device
2. Compare the new log against the previous one
3. Update the changed field values in `AtemState.cpp` — specifically
   `buildStateDump()` for `_ver`, `_top`, and the `InPr` array

---

## Protocol reference

The following sources supplement the captured data:

- **BMDSwitcherAPI.h** — COM interface definitions; field names like `PrgI`,
  `KeDV`, `MPrp` correspond to SDK properties on the switcher interfaces
- **[LibAtem / AtemUtils](https://github.com/LibAtem/AtemUtils)** — community
  documentation of the ATEM wire protocol (third-party, use as secondary reference)
