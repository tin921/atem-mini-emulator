# atem-sweep

Records how a real ATEM Mini answers every safe function of Blackmagic's
switcher SDK — through a recording proxy that captures every packet — and
checks the emulator against those recordings. `atem-sweep.exe` runs in a
console, `atem-sweep-gui.exe` shows the run as a coverage map.

- **[docs/sweep.md](../docs/sweep.md)** — how it works, safety, running it,
  the GUI, capturing another client, backups, layout and build.
- **[docs/sdk-functions.md](../docs/sdk-functions.md)** — coverage of every
  SDK function, the test groups, hardware and excluded functions, the golden
  records in [golden/](golden), what a verification compares.
