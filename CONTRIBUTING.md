# Contributing

Contributions are welcome. Please read this before opening a pull request.

## What we want

- Bug fixes with a clear reproduction case
- Protocol accuracy improvements (backed by a real-device capture log)
- New ATEM commands handled in `AtemServer.cpp`
- Virtual camera compatibility improvements

## What we don't want (right now)

- Cross-platform ports — this project is intentionally Windows-only
- GUI theme or layout changes without a concrete use-case
- Additional abstraction layers or design pattern rewrites

## Getting started

1. Fork the repository
2. Build the project following [tech.md](tech.md)
3. Make your change on a feature branch
4. Test against ATEM Software Control and/or the `obs-atem` plugin
5. Open a pull request describing what changed and why

## Code style

- C++17, MSVC-compatible
- Qt types preferred over raw Win32 types except in the `vcam/` layer
- Keep each source file focused on one concern
- No comments unless the reason is genuinely non-obvious from the code

## Protocol changes

If your contribution changes emulator behavior, include the relevant bytes from
a `capture.py` or `capture.exe` log in the PR description. See [train.md](train.md)
for how to run the capture tools.

## License

By contributing, you agree your contribution is licensed under the MIT License.
