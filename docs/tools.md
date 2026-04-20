# tools

Protocol capture tool for the ATEM Mini — used to extract macro metadata and
protocol field values from real hardware to keep the emulator accurate.

See [capture.md](capture.md) for the reverse-engineering background and workflow.

## capture.exe

Connects to an ATEM Mini via USB or Ethernet using the BMDSwitcherAPI COM SDK.
Captures macro names, descriptions, and run-status values and writes them to
`captured-output.log`.

**Build** (Developer PowerShell for VS 2022, run once):

```powershell
cd tools
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DATEM_SDK_DIR="D:/cemc-sr/Blackmagic_ATEM_Switchers_SDK_10.2.1/Blackmagic ATEM Switchers SDK 10.2.1/Windows"
cmake --build build --config Release
```

**Run:**

```powershell
.\build\Release\capture.exe                  # USB auto-detect
.\build\Release\capture.exe 192.168.10.240   # Ethernet
```

Output is written to `captured-output.log` in the current directory.
See [capture.exe.md](capture.exe.md) for full output format details.
