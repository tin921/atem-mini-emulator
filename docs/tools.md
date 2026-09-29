# tools

`capture.exe` (`tools/capture-bmd.cpp`) is the first capture tool, from before
[atem-sweep](../sweep). It connects to an ATEM Mini through the BMDSwitcherAPI
COM SDK (USB or Ethernet) and logs what the SDK exposes: product name, macro
names and descriptions, and macro run-status values. SDK-level only — no
packet bytes.

**It changes the switcher:** to read the run-status values it runs and stops
up to five of the stored macros, so the live output changes while it runs.

Recording the switcher for the emulator is now done with atem-sweep (see
[capture.md](capture.md)); this tool is only a quick listing.

**Build** (Developer PowerShell for VS 2022):

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

Output goes to the console and to `captured-output.log` next to `capture.exe`.
