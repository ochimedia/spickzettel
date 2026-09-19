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

All third-party code is fetched by CMake on the first configure; nothing
has to be installed or vendored by hand.

## Layout

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the module layout,
the platform boundary, and the reasoning behind the design.

## Licence

Spickzettel is proprietary; see [`LICENSE`](LICENSE). The third-party
components it is built on carry their own licences, reproduced in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
