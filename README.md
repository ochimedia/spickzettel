# Spickzettel

A translucent, always-on-top drawing overlay for Windows. Press a global
hotkey and it appears over whatever you are doing, ready to capture a
screenshot, sketch on it, or keep a note; press it again and it is gone,
with nothing left on screen and nothing underneath touched. Built for
keeping notes over a game without alt-tabbing away from it.

**Windows only for now**, for windowed and borderless-windowed
applications. The code is split so that a Linux backend can be added
without touching the drawing and application logic; see
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Building

Requires CMake 3.21 or newer, Ninja, and the MSVC toolchain of a recent
Visual Studio with the Desktop C++ workload. Both CMake and Ninja ship
with Visual Studio. Build from a Developer Command Prompt, or open the
folder in Visual Studio, which activates the toolchain itself.

```sh
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

`Spickzettel.exe` lands in `build/windows-release/src/app_main/`, and is
copied to `dist/release/` at the repo root; each Windows preset has its
own folder there, and its PDB goes to `dist/symbols/`. Keep the PDB of
any build you hand out - it is what reads a crash dump from it (see
`docs/ARCHITECTURE.md`). All third-party code is fetched by CMake on the first
configure; nothing has to be installed or vendored by hand.
`windows-msvc-debug` also builds the UI tests; `windows-msvc-demo` is the
release build with a permanent demo watermark; `windows-msvc-prerelease`
is the release build with a not-for-redistribution notice at every
start; `linux-tests` builds and tests the portable core on Linux.

## Configuration

On first run Spickzettel writes `%APPDATA%\Spickzettel\config.json` with
defaults and stores the library beside it under `library\`. Everything
in the config file can be changed from the overlay's own Settings tab,
which writes it back at once; editing the file by hand works too, and is
read at startup.

Default hotkeys: `Ctrl+Alt+S` shows or hides the overlay in edit mode,
`Ctrl+Alt+V` in view-only mode (read-only and click-through),
`Ctrl+Alt+C` captures the screen onto a new canvas and opens the overlay
on it, and `Ctrl+Alt+X` captures without opening anything. `ABOUT.md`,
shown on the overlay's About tab, has the rest of the controls.

## Layout

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the module layout,
the platform boundary, and the reasoning behind the design.

## License

Spickzettel is proprietary; see [`LICENSE`](LICENSE). The third-party
components it is built on carry their own licenses, reproduced in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
