# Contributing

Contributions are welcome. Please read this before opening a pull request.

## What we want

- Bug fixes with a clear reproduction case
- Protocol accuracy improvements (backed by a real-device recording)
- HyperDeck or Blackmagic camera support, recorded on the real hardware
  (the project has neither; see [docs/sdk-functions.md](docs/sdk-functions.md#hardware))
- New ATEM commands in the core: a setter-table entry
  (`core/tools/setters_spec.py`) or a handler in `core/src/device.cpp`
- Virtual camera compatibility improvements

## What we don't want (right now)

- Cross-platform ports — this project is intentionally Windows-only
- GUI theme or layout changes without a concrete use-case
- Additional abstraction layers or design pattern rewrites

## Getting started

1. Fork the repository
2. Build the project following [docs/overview.md](docs/overview.md#build)
3. Make your change on a feature branch
4. Run the conformance check (`atem-sweep 127.0.0.2 --verify ...`, see
   [docs/sweep.md](docs/sweep.md)) and test with the `obs-atem` plugin
   and/or ATEM Software Control
5. Open a pull request describing what changed and why

## Code style

- C++17, MSVC-compatible
- Qt types preferred over raw Win32 types except in the `vcam/` layer
- Keep each source file focused on one concern
- No comments unless the reason is genuinely non-obvious from the code

## Protocol changes

If your contribution changes emulator behavior, include the evidence from the
real switcher: the atem-sweep test and the recorded packets (`wire.jsonl`)
that show it. Never change a golden record to make a test pass. See
[docs/sweep.md](docs/sweep.md) for recording and the workflow.

## License

By contributing, you agree your contribution is licensed under the MIT License.
