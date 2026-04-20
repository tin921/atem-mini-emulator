# capture.exe — ATEM capture tool (BMDSwitcherAPI)

Connects to an ATEM Mini via the BMDSwitcherAPI COM SDK and dumps all
state the SDK exposes, plus macro run/stop probes.  Supports **USB** and
**Ethernet** — unlike `capture.py` which requires Ethernet only.

Output goes to `captured-output.log` (same file used by the run.py update
workflow, so the two tools produce a compatible log).

## Table of Contents

- [When to use this vs capture.py](#when-to-use-this-vs-capturepy)
- [Build](#build)
- [Usage](#usage)
- [What it captures](#what-it-captures)
- [What it cannot capture](#what-it-cannot-capture)
- [Console output example](#console-output-example)

## When to use this vs capture.py

| Tool | Transport | What it captures |
|------|-----------|-----------------|
| `capture.py` | UDP/Ethernet only | Raw protocol bytes: `_ver`, `_top`, `_pin`, `_MeC`, `_mpl`, `VidM`, `MPrp`, `MRPr` — everything needed to fully update `run.py` |
| `capture.exe` | USB or Ethernet | Macro names/descriptions, idle/running `MRPr` status values — useful when you only have USB |

**If you have an Ethernet cable**: use `capture.py` — it gets everything.

**If you only have USB**: use `capture.exe` to capture macro names and
`MRPr` status values, and manually keep the other `run.py` fields at their
defaults (they change only on firmware updates).

## Build

Prerequisites:
- Visual Studio 2022 (Desktop C++ workload)
- CMake 3.16+
- ATEM SDK 10.2.1 (`BMDSwitcherAPI.h` in the include folder)
- ATEM Software Control installed (registers `BMDSwitcherAPI64.dll`)

```powershell
# From Developer PowerShell for VS 2022
cd D:\cemc-sr\obs-atem\atem-simulator

cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DATEM_SDK_DIR="D:/cemc-sr/Blackmagic_ATEM_Switchers_SDK_10.2.1/Blackmagic ATEM Switchers SDK 10.2.1/Windows"

cmake --build build --config Release
# Output: build\Release\capture.exe
```

## Usage

```powershell
# USB auto-detect (no IP needed)
.\build\Release\capture.exe

# Ethernet / manual IP
.\build\Release\capture.exe 192.168.10.240
```

Output is printed to the console and written to `captured-output.log` in
the same directory as `capture.exe`.

## What it captures

| Log section | run.py function updated |
|-------------|------------------------|
| `MPrp` entries | default macros list + `macros.tsv` |
| `MRPr` idle runStatus value | `build_macro_run_status()` idle sentinel |
| `MRPr` running runStatus value | `build_macro_run_status()` running value |

## What it cannot capture

The COM SDK does not expose raw protocol bytes.  These fields require
`capture.py` over Ethernet:

| Field | run.py function |
|-------|----------------|
| `_ver` firmware bytes | `build_firmware_version()` |
| `_top` topology bytes | `build_topology()` |
| `_pin` model byte + string length | `build_product_name()` |
| `_MeC` M/E config | `build_mec()` |
| `_mpl` media player config | `build_mpl()` |
| `VidM` video mode byte | `build_video_mode()` |

These fields only change on BMD firmware updates, so the defaults in
`run.py` are usually still correct without a refresh.

## Console output example

```
[14:32:01.010] COM initialized (STA)
[14:32:01.015] IBMDSwitcherDiscovery created OK

# ── STEP 2: Connect ──────────────────────────────────────────
[14:32:01.016] # TX  ConnectTo(nullptr)  [USB auto-detect]
[14:32:01.820] # RX  Connected — IBMDSwitcher obtained

# ── STEP 3: Product / firmware info ──────────────────────────
[14:32:01.821] _pin  ProductName = "ATEM Mini"

# ── STEP 4: IBMDSwitcherMacroPool — enumerate macros ─────────
[14:32:01.822] IBMDSwitcherMacroPool obtained
[14:32:01.823] MaxMacroCount = 100
[14:32:01.830]   MPrp  slot=0  name="Wide Shot"  desc="Camera 1 wide"  unsupported=0
[14:32:01.831]   MPrp  slot=1  name="Close Up"   desc="Camera 2 close" unsupported=0
[14:32:01.850] 2 macros found

# ── STEP 6: IBMDSwitcherMacroControl — run/stop probes ───────
[14:32:01.860]   MRPr  idle  runStatus=0  loop=0  index=0
[14:32:01.861]   NOTE  idle runStatus value = 0  (use this as 'idle' in run.py)
[14:32:01.870] # TX  RunMacro(0)  name="Wide Shot"
[14:32:02.075]   MRPr  running  runStatus=1  loop=0  index=0
[14:32:02.076] # TX  StopMacro
[14:32:02.280]   MRPr  stopped  runStatus=0  loop=0  index=0

# ── SUMMARY — for updating run.py ────────────────────────────
# MPrp  [0]  "Wide Shot"   desc="Camera 1 wide"
# MPrp  [1]  "Close Up"    desc="Camera 2 close"
```
