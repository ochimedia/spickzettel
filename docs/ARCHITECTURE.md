# Architecture

This document explains how Spickzettel is put together and, above all,
*why*: the decisions that are not obvious from the code, the constraints
that shaped them, and the approaches that were tried and rejected. Commit
messages say what changed; this is where the reasoning lives.

## Module layout and the platform boundary

```
src/
├── platform/   # The abstraction the rest of the app is written against:
│   │           # IPlatformHost, IOverlayWindow and plain data types.
│   └── win32/  # The Windows backend. The only place <windows.h> or
│               # <d3d11.h> is included.
├── core/       # Model, persistence, settings and the session. No OS
│               # headers, no Dear ImGui. Builds and tests on any platform.
├── ui/         # The overlay: the Editor and its input machine
│               # (interaction/), which hold what the hand works on, and
│               # OverlayApp with its surfaces (view/), drawn with Dear
│               # ImGui - a view of the session.
├── app/        # TrayController: owns settings, session and overlay, and
│               # moves the overlay between its five states.
└── app_main/   # The composition root: main_win32.cpp.
```

Three rules keep the layers apart, and the build enforces each:

- `core` links neither an OS SDK nor Dear ImGui. It is what any UI views,
  and a build that cannot reach `imgui.h` is what keeps it that way. The
  `linux-tests` preset builds and tests exactly this part with GCC or
  Clang, which is also what keeps a Linux port possible.
- Only `platform/win32` may include Windows headers. Everything above it
  talks to `IPlatformHost` and `IOverlayWindow`; a second backend
  implements the same two interfaces and nothing above changes.
- `ui` is the only library above the platform interface that links Dear
  ImGui. Dear ImGui itself is split the same way: `imgui_core` is the
  OS-independent part; the Win32 and D3D11 backend sources are compiled
  only into `sz_platform_win32`.

Words: the code says *item* for what the interface calls a *snippet*. The
two are the same thing; "snippet" is what a person sees, "Item" is the
struct.

## Build

Presets live in `CMakePresets.json`. `windows-msvc-release` is what a
release is built with; `windows-msvc-debug` additionally builds the UI
tests; `linux-tests` builds the portable core and its tests on a Linux
host.

The release preset copies the finished exe straight into `dist/` at the
repo root (`SPICKZETTEL_COPY_TO_DIST`), as `Spickzettel.exe`
(`SPICKZETTEL_EXE_NAME`). The name is the linker's output name, not a
rename of the copy, so the PDB is named to match and is the name the exe
records for it - what a debugger looks for when it reads a dump. Debug
is not copied: it is built for its tests, and nobody is handed it.

Until 2026-10-04 there were two more presets: a demo build with a
watermark that wandered over the overlay, and a prerelease build that
asked at every start not to be passed on. Both went with the move to
an open-source license, under which anyone may rebuild without the
watermark and pass on what they were given.

The copy is a target of its own that runs on every build and copies
only when the exe differs, so a copy deleted by hand comes back without
a relink. A copy that is running cannot be overwritten, and fails the
build just as a running build-tree exe fails the link.

`scripts/clean_build.cmd` is the release build: it deletes the debug
and release build trees and `dist/`, then configures, builds and tests
both in turn and stops at the first failure. Nothing an earlier
build left behind - a stale object, a cached option, an exe in `dist/`
from before a rename - can end up in what is handed out. It finds
Visual Studio itself with `vswhere`, so it runs from a double-click.

### Continuous integration

`.github/workflows/build.yml` builds and tests `windows-msvc-debug` and
`windows-msvc-release` on GitHub's hosted Windows runner, and keeps
`Spickzettel.exe` and its PDB as an artifact of the run; run on a
release tag, it also makes a draft GitHub release (below). Added on
2026-10-04, so that a build can come from a clean checkout of one
commit, on a machine that has never seen the working tree, and be
traced back to that commit. The local build stays the same; the
workflow is the same presets, run somewhere else.

- **The toolchain is loaded as `scripts/clean_build.cmd` loads it**:
  `vswhere` finds Visual Studio, `vcvars64.bat` sets it up, and its
  environment is handed to the later steps through `GITHUB_ENV`. That
  puts Visual Studio's own CMake and Ninja first on the path, as on a
  developer's machine, rather than whichever versions the runner image
  installs separately, and needs no third-party action: the only actions
  used are GitHub's own `checkout` and `upload-artifact`.
- **The checkout is the whole history** (`fetch-depth: 0`). The version
  stamp is `git describe --tags`, which a shallow clone cannot answer;
  the About tab of a CI build would show a bare hash.
- **It is started by hand** ("Run workflow" in the Actions tab, for a
  branch picked there), not by every push: changed on 2026-10-04, while
  the details around it are still being worked out, as a run for every
  small push costs minutes for nothing. GitHub offers the button only
  for a workflow that is on the default branch.
- **A newer run on the same branch cancels the one still going**, whose
  result no longer matters. Windows runners count double against a
  private repository's minutes.

**A release** is the same run, started on a tag instead of a branch:

1. Raise `VERSION`, give the version its section in `ABOUT.md`'s
   changelog (`### 0.2.4`), and commit.
2. Tag that commit `v0.2.4`, annotated (`Spickzettel 0.2.4`), and push
   the tag.
3. Run the workflow, picking the tag in "Use workflow from".
4. Read over the draft release it makes, and publish it.

On a tag, the run first checks that the tag is `v` and the version in
`VERSION`, and that the changelog has a section for it, and stops before
building if not. The tag is made by hand rather than by the workflow:
the build then runs on the commit the tag names, so its `git describe`
is the tag itself and the About tab reads "0.2.4", and the tag is the
person's, annotated like the ones before it. The draft release carries
that section of the changelog as its notes, and two files:

- **`Spickzettel.exe`, as it is.** It needs nothing installed beside it,
  and the license and the notices are compiled in, so a zip around it
  would only add a step.
- **`Spickzettel-0.2.4-symbols.zip`**, the PDB, zipped and named so
  that nobody takes it for the download. It is kept with the release
  because a crash dump from that build is read with it and with nothing
  else, and a workflow artifact is deleted after 30 days.

GitHub adds the tagged commit's source to every release by itself, which
is the source the GPL asks to be offered beside the binary. The job may
write to the repository for this (`contents: write`); a run on a branch
writes nothing.

The C++ runtime is linked statically (`CMAKE_MSVC_RUNTIME_LIBRARY`), so
the exe needs nothing beyond what Windows itself ships: no Visual C++
Redistributable to install, and no risk of loading an older copy of
`msvcp140.dll` than the toolset built against, a known cause of crashes
at startup. It costs about 300 KB. googletest is left to pick the static
runtime itself (`gtest_force_shared_crt` off) so the tests link.

Every third-party dependency is fetched with `FetchContent`, as an
archive of one pinned commit checked against its SHA-256 (see below), so a
checkout builds with nothing installed beyond a compiler, CMake and Ninja. Header-only libraries are marked `SYSTEM` so
their warnings do not count against the project's own warning level.

### Crash dumps and symbols

A crash on someone else's machine leaves a minidump in
`%LOCALAPPDATA%\Spickzettel\crashes\`, beside the library, named after the version line and the
time (`win32_crash_dump.h`, installed first thing in `WinMain`). It
covers an unhandled SEH exception - an access violation, a stack
overflow, a C++ exception nothing caught - and `abort()`, which
`std::terminate` ends in, and the CRT's invalid-parameter and pure-call
handlers. The process then ends without Windows' own error dialog. The
dump is written from a thread of its own: a thread cannot reliably walk
its own stack into a dump, and one that overflowed has little left to
do it with. Everything the crash needs - the folder, the name's prefix -
is prepared at startup, so the crash itself allocates nothing; the folder
is created then, with any folder above it that is missing, one level at a
time in the prepared name. Each start keeps the newest ten dumps and
deletes the rest.

What can go wrong while dying is bounded. The writer thread is waited for
20 seconds, not forever: a crashed thread holding the loader lock keeps
a new thread from ever starting, and a process hung in its crash handler
is worse than one with no dump. A second crash waits for the first to end
the process, rather than ending it under the first dump half-written -
unless it is the writer's own, which gives that dump up. The writer is
started suspended, so that it is known for the writer before it can
crash. A dump is written as `.dmp.partial` and renamed once whole, so a
writer cut off leaves no half dump among the ten kept, and pruning
deletes what it leaves. And the main
thread keeps 64 KB of stack for the handler (`SetThreadStackGuarantee`),
which a stack overflow otherwise leaves it without.

A dump is read with the PDB of the very build that wrote it, so release
builds make one: `/Z7` for everything, the fetched code included,
`/DEBUG` with `/OPT:REF` and `/OPT:ICF` turned back on so the exe is
what it was, and `/PDBALTPATH` so the exe names its PDB without the path
of the machine that built it. The dist copy puts the PDB in
`dist/symbols/`, apart from the exes, so `dist/` holds only what is
handed out. Keep the PDB of every build you hand out: a later
build's PDB does not match an earlier build's dump.

### Build-time configuration: version, embedded text

Three things are decided when a binary is *built* rather than when it
runs, and all three land in `build/<preset>/generated/` through
`cmake/BuildInfo.cmake`:

| Header | Holds | Regenerated | Included by |
| --- | --- | --- | --- |
| `build_config.h` | `kVersion` | configure | `build_info.h`, so widely |
| `git_stamp.h` | `kGitDescribe` | **every build** | `build_info.cpp` only |
| `about_text.h`, `notices_text.h`, `license_text.h` | `ABOUT.md`, `THIRD-PARTY-NOTICES.md`, `LICENSE` | configure | `build_info.cpp` only |

The third column is the design: the git stamp changes with every commit
and the embedded texts are by far the largest, so both sit behind
functions in a header that declares but does not contain them. In
`build_config.h` they would rebuild everything that merely wanted the
version number.

The boxes about what the start itself found - a library set aside, or
hotkeys another application owns - are native rather than drawn by the
overlay: on a first run it comes up fullscreen, topmost and in edit
mode, and would cover them. They can only be shown once the controller
has looked. `Initialize` loads the library and registers the hotkeys but
leaves the overlay hidden;
`WinMain` shows the boxes; `TrayController::Start` then brings the
overlay to where a start puts it. They were once shown after it, and a
start on a library set aside is always a first run: the box sat under
the edit overlay, fullscreen and topmost and holding the keyboard, and
that overlay drew no frame until the box was answered, since frames
come from the event loop. A box nobody could see or reach, over a
screen that did not move.

Until `Start`, the hotkeys and the tray icon's show and hide are held
(`TrayController::HoldUntilStart`); the menu's Exit is not. Found in
review on 2026-09-27: a box's modal loop hands on `WM_HOTKEY` and tray
clicks, and the edit hotkey pressed under one brought up the same
undrawn edit overlay, the grab holding the mouse and keyboard until the
box was closed by Enter or the overlay put away again.

The version lives in `VERSION` at the repo root, read by CMake and fed to
both `project()` and the header, so a release script can bump it without
parsing CMake. (A file named `VERSION` can shadow `#include <version>` on
a case-insensitive filesystem. It is safe here only because the repo root
is never an include directory; do not add it to one.) The exe's version
resource is configured from it too (`src/app_main/version.rc.in`), for
Explorer's Details tab and the name Task Manager shows. It carries the
version and the build's kind, not the git description, which would go
stale in a resource configured once per build tree.

The git stamp is regenerated per build from an always-run target and
written through `copy_if_different`, so it costs one `git` call per
build and recompiles one file only when the commit changed. A configure
time `execute_process` would bake whatever commit the build directory
was created on, which this project - configured once, built for days -
would show for weeks.

### Cross-compiling: what is known

An earlier incarnation of this project cross-compiled the Windows build
from Linux with MinGW-w64 as a build-health check. The toolchain file is
not part of this repository, but three things learned then are worth
keeping for whoever adds it back:

- `imgui_impl_dx11.cpp` links `d3dcompiler` through
  `#pragma comment(lib, ...)`, which GCC ignores. The library has to be
  listed explicitly, which is harmless under MSVC.
- `_dupenv_s` and other MSVC "secure CRT" extensions are not reliably
  exported by the `msvcrt.dll` MinGW links against. Plain `std::getenv`
  works everywhere.
- A statically linked MinGW executable was flagged and deleted by Windows
  Defender as a virus. Real malware commonly static-links for the same
  "no DLLs to bring along" reason, and Defender's heuristics key on that
  shape. Copying the three MinGW runtime DLLs next to the executable
  avoids it; an MSVC build needs no such step.

### Licenses: what ships, and where the notices are

`LICENSE` covers Spickzettel itself: the GNU GPL, version 3 or (at the
user's option) any later version, since 2026-10-04, when the app went
from proprietary to open source. The file is the license text exactly
as the FSF publishes it, so that it is recognized as what it is; the
copyright line, "The Spickzettel Authors", is in `README.md`, the
exe's version resource and the About tab.

The About tab carries what GPLv3 asks of an interactive program: the
copyright, the license, that there is no warranty, and a way to read
the license, which is compiled in (`build::LicenseText`) and is one
button away in the footer, beside the third-party licenses. Where the
source is found is the download page's to say, next to the binary, not
the app's.

`THIRD-PARTY-NOTICES.md` covers everything that ends up inside the
binary, and is compiled into it and shown on the About tab, because MIT,
ISC and the OFL all require the notice to reach whoever received the
software; a text file next to the executable is one copy away from not
doing that.

What is in a release binary, and why each is allowed in it:

- **Dear ImGui**, **nlohmann/json**, **QOI**: MIT. Reproduce the notice.
- **SQLite**: public domain. Nothing is required; the notices file names
  it all the same, so that what the binary holds is all in one place.
- **Manrope**: SIL OFL 1.1, which permits bundling and selling a font
  *with* software provided the license travels with it, and forbids only
  selling the font by itself. The font stays under the OFL inside a GPL
  program; neither license asks the other to give way.
- **Icon designs**: ISC (Lucide) and MIT (Feather). The SVGs here are
  drawn from this project's own path data, but both licenses cover the
  designs.

googletest and imgui_test_engine only build or test the app and are not
in a release binary, so they are not in the notices. imgui_test_engine
is under a license of its own, not an open-source one, which is free for
a project released publicly under an open-source license; it is linked
only into the UI tests, which nobody is handed. The MSVC runtime, linked
statically, and the Windows DLLs the app loads are the GPL's System
Libraries.

Every dependency is fetched at a commit hash, with the tag it
corresponds to in a comment beside it. A tag is a mutable reference -
its owner can move it - so a build pinned to one is reproducible only
for as long as nobody does; a hash is a build's exact input.

It is fetched as GitHub's archive of that commit
(`<repo>/archive/<commit>.tar.gz`) with `URL_HASH SHA256=`, not cloned:
a clone brings the repository's whole history, and nlohmann/json's alone
was 300 MB of the 475 MB each preset downloaded - four times over in a
clean build. The archives come to 5.5 MB. The SHA-256 is also the
stronger pin: a clone checks out whatever the commit names on the
server, an archive that differs by a byte is refused. json is an
exception to the URL pattern, fetched as the `json.tar.xz` its releases
publish for this use - the headers and the CMake files, nothing else.
SQLite is the other: its source lives in Fossil, not on GitHub, and it
is fetched as the release's amalgamation zip from sqlite.org - the whole
library as one C file - after checking it against the SHA3-256 the
download page publishes.

Bumping a dependency is therefore: pick the tag, resolve it (`git
ls-remote <repo> refs/tags/<tag>^{}`, or the un-peeled line for a
lightweight tag), download the archive of that commit and take its
SHA-256, and write the commit, the hash and the tag's name side by side.

## The platform interface

`IPlatformHost` owns what exists for the whole process: the tray icon,
global hotkey registration, the event loop, the list of displays and the
per-user paths. `IOverlayWindow` is the one fullscreen window: lazily
created, then shown and hidden without tearing down its GPU resources,
so toggling it by hotkey costs nothing after the first time. Both are
kept deliberately small and grow one method at a time as a feature needs
one, rather than being redesigned per feature.

Everything crossing the boundary is plain data in `platform_types.h`: a
`KeyCombo` is modifiers plus one logical key (letters and digits share
their virtual-key value on every platform; function keys get their own
encoding), a `CaptureResult` is the pixels captured, and a texture is an
opaque `uint64_t` so no platform header ever names an ImGui type. The
texture calls have one caller, `TextureCache` (see "Textures").

`pen_glyph.h` is the one small piece of drawing that lives here: the pen
pointer's outline, which both the software pointer (drawn by the UI) and
the Win32 cursor bitmap are built from, so the two pens are the same pen.

### Command-line options

The executable takes options for the scripts that test and measure it;
a person starting the app has no reason to pass any. They are one table,
`kOptions` in `app/command_line.cpp`: a name, what follows it, a line of
help, and what it sets in `app::CommandLine`. A new option is a row and
a field. A value follows its option as the next argument or after `=`.
Anything not understood - an unknown option, one given twice, one
without its value, an argument that is no option - stops the start with
a message naming it and listing the options: a start that quietly
ignored a misspelled `--data-dir` would be a start on the user's own
library.

- **`--data-dir <folder>`** keeps the settings, the library and the
  crash dumps in that one folder (`CreatePlatformHost`), made absolute.
  The former library place is the same file there, so nothing is moved.
  Added after 0.3.1, when the review of 2026-10-08 found
  `tools/perf_library/measure.ps1` measuring the user's real library:
  it pointed `APPDATA` and `LOCALAPPDATA` at a sandbox, which the app
  stopped reading when it began asking the shell for its folders (see
  `AppDataBase`). The single-instance lock stays per user, since the
  hotkeys are: with a copy running, a start with a data folder is
  refused with a message rather than handed to that copy, which is the
  user's and on the user's library.

## Drawing model

`Stroke` is a polyline with a color, a width and whether its corners
are square (see "Tessellation, and its cache"); `CanvasState` - the
session's live layer, where a stroke is drawn before it is committed to
its snippet - holds a list of finished strokes plus at most one in
progress. Both are dumb on purpose: they record what they are given, so
a shape tool can hand them exact corners and a test exact points.

The live layer is drawn as its snippet's strokes are, at the snippet's
foreground opacity. It was drawn opaque, and a stroke in a snippet set
to a quarter faded to a quarter the moment it was let go.

### From a hand to a mark: input, fitting, tessellation

Three stages, and it is worth knowing which owns what, because the same
visible defect can come from any of them.

**Input hygiene, in `DrawTool`.** A mouse reports far faster than a hand
moves, so consecutive samples of a slow line land a fraction of a pixel
apart, and the direction between two points that close is quantization
noise. `DrawTool` discards a sample that has not traveled 2px from the
last control point *kept* (so a slow hand still draws), low-passes what
survives, and snaps a real line's end to the release point so it does not
fall short of the mark. A press and release without travel stays a single
point: that is the dot.

While the pen is down, the stroke also has a provisional *tail* (see
`CanvasState::SetActiveStrokeTail`): the spans still waiting for their
next control point, fitted as if the pen were lifted, and on to the
pointer. Each move replaces it, and lifting the pen drops it for the
real end, which it draws the same - a dot included: before the second
control point the tail does not reach for the pointer, as the release
does not (found in review on 2026-10-02; it showed a stub of up to 4px
that the lift took back). Without it the ink trailed the
pointer by a control point and the smoothing's lag, and caught up only
when the next control point came - at a turn, after the hand had turned,
so the stroke went on growing the old way while the pointer went back,
and felt like the pen overshooting. Measured on 2026-10-02: a V held at
its bottom was 5px short, and 3px of that was drawn after turning back.

**Curve fitting, in `stroke_smoothing.h`.** The surviving control points
are the curve's frame, not the curve. `AppendFittedSpan` fits a
*centripetal* Catmull-Rom through them and samples it adaptively: a
straight span costs one segment however long, curvature costs points.
Centripetal rather than uniform because it cannot loop or cusp when the
spacing between control points is uneven, which it always is. This is an
input stage, not a rendering one: the fitted polyline is what is stored,
so the eraser cuts what you see, and the shape tools, whose corners must
stay corners, simply never call it.

**Tessellation** is the next section's subject.

### Erasing is clipping, not deleting

`ClipStrokeOutsideCircle` and `ClipStrokeOutsideRect` compute what is
left of a stroke after removing everything inside a region, segment by
segment: the line/circle quadratic or a Liang-Barsky clip finds where each
segment crosses the boundary, points inside are dropped, and the
survivors are regrouped into however many fragments the stroke split
into. Both shapes share one walk (`ClipStrokeOutsideRegion`) and differ
only in an inside test and a crossing finder, so they cannot disagree
about what erasing means.

A drag with the eraser clips against the capsule from its last position
to its new one (`ClipStrokeOutsideCapsule`), the circle swept along the
way, not against a circle at each. Movement comes once a frame (see
"Movement is never posted"), and a quick scrub moves the eraser farther
than its width between two: at 60 frames a second the default 28 px
eraser skips at about 1700 px a second, and a thin line crossed between
two circles was left whole - the pen joins its samples, and the eraser
now does too. The capsule is convex, so the same walk takes it; its
crossings are where the segment's line meets the band along the pass or
either end's circle, the least start to the greatest end. Found in
review on 2026-09-27.

A closed stroke - the rectangle tool's outline, which starts and ends on
the corner its drag began at - is cut open where it is erased, not at its seam:
the run that ends at the seam and the one that begins there are joined
into one. Left as two, each ended there in a round cap, and of a
rectangle cut anywhere that one corner went round while the others
stayed square.

Why not rasterize instead: a bitmap erase gives up resolution
independence and turns undo into pixel diffs, for a problem that a
bounded piece of segment geometry solves while keeping every stroke a
plain, inspectable polyline.

The boundary points only cut a segment into pieces; each piece is then
classified by its midpoint, rather than each boundary point being taken
as a way in or out. A line that merely touches the region - tangent to
the circle, through a rectangle's corner - meets the boundary once
without entering: a walk that toggled its state there thought itself
inside for the rest of the segment, rebuilt the segment from its start
when the far end turned out to be outside, and handed back the first
half twice - darker on a translucent stroke, and longer with every
touch, since the fragments replace the stroke and are saved. Classifying
pieces also covers the older edge case: a crossing within 1e-6 of an
existing vertex is deliberately not reported, to avoid a zero-length
fragment, so a sample point exactly on the boundary has no crossing of
its own, and the pieces either side of it are what split the stroke
there. Before either, the walk bridged the erased middle into one
fragment, which looks exactly like not having erased anything.

### Tessellation, and its cache

`BuildStrokeMesh` turns a centerline into the shape a round pen leaves:
a miter join while the turn is a few degrees, a round join past that,
round caps, a disc for a dot, and a seam join instead of caps for a
closed path (the rectangle tool's). It exists because ImGui's
`AddPolyline` offsets each point along the average of its adjacent
normals and rescales by 1/cos² of half the turn, clamped only at 100x
the half width, so a near-reversal throws a spike most of a hundred
widths out of a wide pen; it also has only flat caps and no round join.

A join is mitered only while the miter's corner stays within a quarter
of a pixel of the round pen's outline. A miter reaches half the width
over the cosine of half the turn, so a limit in half widths - 2, about
120 degrees, which is what there was - lets a wide pen's corners reach
further: a quick flick of a 40px pen grew a point 20px long, measured
on hand-like strokes through `DrawTool`, and fitting does not prevent
it, since the curve passes through every control point and a flick is
a sharp one. A round join is the corner the pen leaves, and nothing
spoke for the miter any more: its shared vertices draw nothing twice,
but neither does the round join, pivoting on the inside of the turn,
and the depth test (see "Drawing strokes") makes overlap harmless
anyway. A fitted curve's joins are mostly a few degrees, so a smooth
stroke has as many vertices as before; wavy and zigzag strokes of a
wide pen have up to a quarter and a half more.

The round join pivots on the inside of the turn at the pen's half width
from the point, along the bisector, and not where the two inner edges
cross - so the inside of a turn is pinched a little: a right angle
between long segments of a 40px pen narrows to 34px. Pivoting at the
crossing was tried on 2026-10-01, after a review found the pinch, and
reverted the same day. The crossing is far up the inside of a sharp
turn, past the end of a short segment, and the strip folds back over
itself there; held to half the shorter segment, the pivot sat closer to
the centerline than the half width between a hand's closely spaced
points, and the inner edge of a wavy stroke was notched with gaps. And
it bought nothing a hand draws: the same scripted strokes, a wave, a V
and a W, came out pixel for pixel the same as with the pinch wherever
there were no gaps, since a hand's turns are between short segments,
where the crossing is out of reach anyway. Only a long straight segment
on either side of a sharp turn shows the pinch.

The rectangle tool's corners stay square: a shape tool's stroke says
`StrokeCorners::Sharp`, and keeps the miter up to 120 degrees. The
stroke says so because the mesh sees only points, and a rectangle cut
open by the eraser is not a closed path any more but should keep its
corners. Strokes saved before there was a choice read as `Sharp` when
every segment is exactly level or plumb - a rectangle, or what is left
of one - and `Round` otherwise.

The mesh is one connected strip whose neighboring quads share vertices,
so no triangle is drawn over another. That is what a *translucent*
stroke needs: every overlap is a place the color lands twice, which at
less than full opacity is a visibly darker patch. `StrokeMeshTest`
measures this as area, since a screenshot cannot tell a double-covered
pixel from a slightly darker one. A stroke that crosses itself still
overlaps where it crosses; the renderer's depth test is what keeps that
to once (see "Drawing strokes"), and the reason the mesh comes body
first, fringes after. A one-pixel anti-aliasing fringe is
carried in screen space so it stays a pixel wide whatever an item is
scaled to.

A stroke's mesh goes into the draw list in one piece, and ImGui is built
with 32-bit indices (`ImDrawIdx`, set in `cmake/FetchImGui.cmake`),
because one piece can pass the 65536 vertices 16 bits reach. The pen
adds a point every 2 px it travels and holding still adds none; measured
on straight lines, circles, zigzags, shading scribbles and loops, a
stroke comes to 1.7-2.0 vertices per pixel whatever its width, so the
ceiling was 32,000-39,000 px drawn without lifting the pen - half a
minute of shading. Past it the indices wrapped and the stroke drew
triangles across the screen. ImGui can split a draw list between two
reservations but not inside one, and 32-bit indices cost a larger index
buffer and nothing else: the DX11 backend takes its index format from
`sizeof(ImDrawIdx)`.

`StrokeMeshCache` keeps each stroke's mesh between frames, keyed by item
and stroke index. Two things make it work. The mesh is built around the
origin and translated as it is written into the draw list, so dragging
an item - the case where a dropped frame shows most - changes nothing
the cache holds; only the scale reaches the tessellator. And each entry
remembers the `CanvasManager` generation it was last checked at: while
that has not moved nothing anywhere has changed, so a canvas at rest
costs one integer compare per stroke. When it has moved the stroke is
fingerprinted (64-bit FNV over its bytes) and compared, which catches
the case a stroke count cannot: the eraser rewriting the middle of a
list without changing its length. A fingerprint rather than a kept copy
because there are two caches - the canvas's and the Overview previews' -
and two copies of every stroke on screen is a lot to hold to compare
against. `EndFrame` drops whatever was not drawn that frame, so
switching canvas or deleting an item releases the memory without either
having to know the cache exists.

### Drawing strokes: once a pixel, one layer a snippet

A snippet's strokes are drawn as one layer. Each stroke reaches a pixel
once, however it crosses or folds over itself; each blends over the
strokes before it by its own ink's alpha, the pen color's, chosen with
the color in its chooser; and the snippet's opacity
(`Item::foregroundOpacity`) fades the finished layer once. So a scribble
fills an area evenly, two translucent strokes mix where they cross, and
opaque strokes on a faded snippet stay flat against each other - the top
one hides what it covers, as if the drawing were a picture laid down at
that opacity. The two opacities are different things: the ink's belongs
to a stroke, the snippet's to the drawing.

**Once a pixel: a depth test.** Each stroke is drawn between two calls
of `IOverlayWindow::StrokeDepthCallback`. The renderer gives it a depth
of its own, nearer than every stroke before it in the frame, under a
strictly-less test that writes depth: a stroke's second fragment on a
pixel meets its own depth and fails, and the next stroke, nearer,
passes. The depth is set by narrowing the viewport's depth range to one
value - ImGui's vertex shader puts every vertex at z = 0.5 - so ImGui's
shaders stay as they are. Depths run 1 - k/2^20 down from the far end,
exact in a float and in the `D32_FLOAT` buffer; past 2^19 strokes in a
frame the buffer is cleared and counted again, since only a stroke's own
fragments have to meet. The buffer is the size of the target, made when
that changes and cleared each frame.

The first fragment on a pixel is the one that stays, so the mesh comes
body first and fringes after (`RibsToMesh`). In the old interleaved
order, where a stroke crossed itself the first pass's anti-aliasing edge
took pixels the second pass's body covered fully, and left a fainter line
either side of every crossing - alpha 61 and 47 where 128 was right.
Where only two edges meet, at a crossing's corners, the first edge wins
over the stronger one: a pixel a shade light, never dark. The order is
the tessellator's, so the mesh cache keeps it; made in `DrawStroke`
every frame instead, it cost 18-21% of a frame's CPU.

**One layer: a scratch target.** Below full opacity, a snippet's strokes
go between the two steps of `IOverlayWindow::StrokeLayerCallback`.
Opening, drawing switches to a scratch layer the size of the frame, with
the frame's depth buffer, cleared within the command's clip rectangle -
the snippet's - by `ClearView` (the whole layer where D3D 11.1 is
missing). Closing, it switches back and lays that rectangle down once,
one triangle scissored to it, times the snippet's opacity. ImGui's blend,
straight alpha into a target cleared to nothing, leaves the layer
premultiplied, so it is laid down with a premultiplied blend; the frame
is premultiplied the same way, which is what DWM composites. At full
opacity no layer is opened: source-over is associative, so drawing
straight onto the frame comes to the same. The stroke being drawn is
drawn with the snippet's own (`DrawItemContent`'s `moreStrokes`), into
the same layer, and looks as it will once let go.

Without the callbacks - a backend that draws nothing - strokes are drawn
untested and each at the snippet's opacity, which is how every stroke was
drawn before these.

**Drawn every frame, not kept.** A snippet's layer could be kept as a
texture and drawn again only when its strokes, its size or its place
within a pixel changed; a still canvas would then cost one textured
rectangle a snippet. The layers cost about 25 microseconds of CPU each
per frame - 1.1 ms for the forty of `heavy`, which holds 120 fps either
way - and keeping them would cost a texture per snippet on screen at its
size there: 0.9 MB for a third of a 1080p screen, 8.3 MB for all of it,
four times that at 4K. That is video memory a game behind the overlay
is using, spent for canvases far busier than annotations get. See
`docs/PERF.md` for the measurements.

**Before this, three render modes** and a setting to choose: the
tessellator stroke by stroke, ImGui's polyline to judge it against, and
Rasterized - the strokes drawn into a bitmap at the snippet's native size,
coverage taken per stroke and the color applied once, composited at the
snippet's opacity. Rasterized had this look, as pixels: soft when a
snippet was enlarged, capped at 4096 a side, up to 64 MB a snippet plus
a copy of its strokes, rebuilt on the CPU at every erase, and the stroke
being drawn shown tessellated beside it until let go. The layered drawing
gives its look at the tessellator's sharpness, and the modes and the
setting went (see "Dead ends").

## Canvases, items and folders

`Item` is a snippet: freehand strokes over a picture, at a `rect` on
screen. `Canvas` is an independent collection of items whose
order is paint order; `Folder` is a flat, non-nesting group of canvases
- one per game, or per set of levels. `CanvasManager` owns all three,
plus which folder is browsed and which canvas is current.

Two pieces of "current" are deliberately decoupled: the current canvas
is what is on screen and drawn on; the browsed folder is what the
Overview shows and where its own "New canvas" lands. Browsing a folder
never switches away from the canvas being edited. Code that wants one of
them has to say which: "where am I working" is the current canvas's
folder, "what am I looking at" is the browsed folder. A canvas made from
the canvas itself - the shortcuts, the canvas bar, a capture - lands
beside the current one, in the working folder: the two differ only after
the Overview has been browsed elsewhere and closed without switching, and
a canvas that then landed where the user was last *looking* rather than
working was a surprise.

There is no "always at least one canvas" invariant. An empty folder is a
legal state the moment one is created, so refusing to let the library
reach the same state would be an inconsistency dressed up as a
safeguard. Everything treats "nothing at all" as ordinary: the only
accessor for the current canvas is `CurrentOrNull()`, every item
operation no-ops without one, `AddCanvas` mints a folder if none is
left, and the library saves and loads empty.

### The picture

Every item has one `Picture`, with strokes and the caption on top: its
screenshot, or a fill for a drawing, transparent until given a color. A
picture's pixels are stored once, when they are captured, and never
changed after; everything drawn over them is strokes.

A picture is content only. The texture it is drawn with is kept by
`TextureCache` under the snippet's id (see "Textures"), so copying an
`Item` never copies a handle; a copy is given stored pixels of its own.

A capture that comes back with nothing - the screen could not be read,
or it is past what a picture may be - still makes its snippet, with the
placeholder it shows for a picture that cannot be read: something to
delete or to draw on, and undoable like any other. It says so
(`Session::FailedCaptures`, `Editor::CreateSnippet`), and the tutorial
does not count it. Until the review of 2026-10-08 it said "Captured
screenshot".

### Strokes live in the item's native space

Strokes are stored in a fixed coordinate space set at creation
(`nativeW/nativeH`), not in screen space, so a stroke drawn at one size
still looks right after the item is resized. `ScreenToNative` is the one
transform for everything that lands a gesture on an item - the pen and
the erasers - so they cannot disagree about where the pen is.

A width has no axis of its own, so on a snippet stretched unevenly it
travels by one factor made of both: their geometric mean
(`LengthScale`), into the snippet's space and back out when drawn. The
average of the two was used before, and one way times the other came to
(a+b)²/4ab rather than 1, so a stroke let go of on a snippet stretched
to twice its width grew to 1.125 times what it was while drawn - 1.33 at
three times. The geometric mean is the one that undoes itself exactly.
Strokes kept from before on a stretched snippet draw a little thinner
than they used to, by the same factor. Found in review on
2026-09-27.

### Resolution-relative item sizing

`rect` is always in absolute pixels for every runtime consumer, but it
is a *derived* value: recomputed every frame from a per-item anchor (the
last deliberate placement and the display size it was made against) by
`SyncItemsToDisplaySize`. Position scales per axis so an item near a
corner stays near it; size scales by one uniform factor so its shape is
never distorted, at the cost of not filling an ultrawide. Rescaling in
place instead is unstable - the uniform factor is asymmetric between
shrinking and growing, so shrinking a display and widening it back
ratchets items smaller every cycle - which is why the anchor is fixed
and every deliberate move or resize re-anchors through
`CommitItemLayout`.

A fullscreen item bypasses the anchor entirely and is recomputed from
the viewport each call, stretched or fitted to its own aspect ratio as
it was entered (`isFullscreenStretch` remembers which, since `rect`
alone cannot tell). Exiting fullscreen recomputes the restore rect from
the untouched anchor against the *current* viewport, so a display change
while fullscreen lands it in the right place.

A copy of a fullscreen snippet, duplicated beside its source, is
fullscreen too, and what is offset is its anchor - the place it goes
back to - not its rect (`Session::OffsetCopy`). Offset like any other
copy, the fullscreen rect was committed as its anchor, so taken out of
fullscreen it stayed the size of the screen, and was saved so. Found in
review on 2026-09-27.

### The floor is a shape

Items cannot shrink below 16x16. Applied as two independent per-axis
clamps that floor reshapes anything that is not square: a 16:9 item
reaches the height floor at 28x16 and then goes on narrowing to 16.
Reaching one floor has to stop the whole resize, so
`MinimumSizeForAspectRatio` turns the two numbers into one floor on the
item's own ratio, and the derived axis is deliberately not re-clamped.
The same floor applies in the display sync and in fullscreen restore,
and a free (Shift) resize, which is meant to reshape, keeps the plain
per-axis pair.

**Changed on 2026-10-01: 16x16, where it was 90x70.** A tester scaling
flat snippets down, and to one width, was stopped by the 70 px height.
Nothing said why the floor was what it was; the bar has floated above
the snippet for as long as this rebuild has had one. What a floor has to
protect is what is drawn on and around a snippet. Its handles would
have met on a small snippet, and covered it, so they went for a resize
band outside it (`docs/INTERACTIONS.md`, 6.5), which needs no room of
the snippet's own. What is left is the snippet's border: 16 px is a line
of small text - a line from a game's chat or status bar - with a
hovered border (3 px, drawn inside) on either side of it, and still
something to aim at.

A snippet is made at least that size too. A framing drag is kept when
it reaches 24 px corner to corner (`kRegionMinSize`), however thin,
where it had to reach 24 px on both sides, and a side under the floor
grows to it about its middle, so what was framed stays centered and the
picture is captured at the size it is shown at. Grown past the screen's
edge, it is moved back on: the capture is of the screen, and of a frame
at the very top 4 px tall it kept the 10 rows on screen and showed them
16 px tall (found in review on 2026-10-01). Before, a frame between
24 px and the floor was made at its own size and then shown at the
floor's by the display sync, which floors every rect every frame - with
a picture captured smaller than it was shown.

### An aspect-locked corner follows the diagonal

One scale drives both axes while the shape is kept, and a corner takes it
by projecting the corner the pointer asks for onto the item's own
diagonal. The obvious rule - whichever axis moved proportionally further
drives, the other is derived - is discontinuous wherever the two axes
disagree about which way they are going. At the crossover one answer says
a tenth bigger and the other a tenth smaller, so moving the pointer
*across* the diagonal rather than along it made the size jump: measured on
a 400x300 snippet, 440x330 to 360x270 from a ten-thousandth of a pixel of
movement, and up to the snippet's whole width in the worst case. Along the
diagonal, where both axes agree, the two rules are identical, which is why
it only ever happened in two of the four directions.

The projection has no crossover to jump at and costs one thing worth
knowing: a corner dragged straight sideways grows by less than the pointer
moved (`w^2 / (w^2 + h^2)` of it), because the corner tracks the pointer's
foot on the diagonal rather than the pointer itself. `std::max` of the two
per-axis scales is the other continuous rule - it tracks the pointer
exactly on whichever axis wants the item bigger, at the price of growing
the item when a corner is dragged inward on one axis and outward on the
other.

### Keeping the shape is the snippet's own setting

Whether a resize keeps the shape is `Item::keepAspect`, set from the
defaults when the snippet is made and changed in its popover; Shift does
the other. Not whether the snippet has text, which it once followed:
text is a caption any snippet can carry, so typing one into a
screenshot changed what resizing it did, with nothing on screen to say
so. A property says it, and can be set either way on purpose.

### What a new snippet starts with

Settings > Defaults holds the starting values for a new snippet, by kind:
its shape, its two opacities, and a drawing's background color - plus
the style of text typed into it later. They are applied in
`Editor::PrototypeForKind`, which every way of making a snippet passes
through, and they are only ever a starting point: each is the
snippet's own property from then on, and changing a default touches no
snippet already made. There is no background color for a screenshot,
where it would only tint the capture.

The text size is written into the settings on the first frame the app
ever draws: 20px at Windows' scale for that display. A number rather
than "20 at whatever the scale is", so it does not change when the
overlay moves to a display with another scale, and so the slider shows
what a new note will actually get.

### Z-order steps past what actually overlaps

Bring forward and send backward move an item past the nearest item that
*overlaps* it, not the immediate neighbor in the list. A canvas holds
snippets all over the screen, and a step over one that shares no pixels
with this one changes the order without changing anything anybody can
see, which reads as a button that does nothing.

### Deletion is a mark

A deleted folder, canvas or snippet stays exactly where it is, with
`deletedAt` stamped, and is hidden by every walk over the library. A
thing counts as deleted when it or anything holding it is marked, so
deleting a canvas stamps only the canvas, restoring it brings back
exactly what went with it, and a snippet deleted earlier keeps its own
mark. "Delete permanently" is the erasure the `Delete*` methods perform.

Restoring is aimed at what the Overview shows (see Show deleted). A
folder's restore clears its own mark and those of every canvas in it,
so it brings back everything deleted there, whether the folder went
whole or only some canvases in it did. A canvas restored out of a
deleted folder needs the folder back to be seen, but clearing the
folder's mark would bring back every canvas that went with it; so those
are marked instead, each with the folder's stamp, and exactly one canvas
comes back while the rest stay deleted as of when they went.

A snippet's mark is only ever cleared by undo, and no history outlives
its session, so `Session::ImportLibrary` erases every snippet that comes
in marked. That includes one deleted before its canvas: restoring the
canvas would not bring it back either.

Folders and canvases can be given a retention period
(`AppConfig::purgeDeleted`, off by default, and `purgeDeletedAfterDays`, 14):
once the library is opened, `Session::EraseDeletedBefore` deletes for good
whatever carries a mark older than the period, by the same erase and for
the same reason. Only its own mark counts. A canvas that went with its
folder has none and goes when the folder does; one marked on its own
before the folder went can go first, which leaves the folder as a
Delete permanently by hand would. It runs at startup only: an instance
left running for days keeps what is due until it is next started, which
costs nothing but the wait. Switched on, it says what it does: the
delete confirmation says for how many days a thing can be restored, a
deleted folder or canvas says from which day it goes, and what a start
deleted for good is counted in a message the next time the overlay comes
up, since the start itself happens while nobody is looking.

It was on by default until 0.2.3, and reviews kept finding one more way
a start could fall back to the defaults - no file, a file set aside, a
stand-in that could not be written, a key that could not be read - and
with it erase what someone who had switched retention off meant to
keep. Off by default, every one of those keeps things instead, and what
is deleted can only be lost by a choice: Empty trash, or retention
switched on. A file that has it on with no period that can be read is
read with it off and written back so, since the default period could be
far shorter than the one meant. A file written before keeps what it
says: every file lists every setting, so one written while retention
was on by default goes on purging, as its owner was shown.

With nothing emptying it by default, the trash only grows, so a start
measures the library once retention has had its turn and, past a size
(`AppConfig::librarySizeReminder`, on, at 2000 MB), keeps a reminder for
the first frame in edit mode: a box like the delete confirmation that
says how big the library is, with buttons to the trash (the Overview
with Show deleted on, where Empty trash is) and to Settings > Behavior.
View-only cannot answer a box, so it waits for edit mode, and it comes
up once a run, so that a long session is not reminded again. The size
is what the library holds - its pages less the free ones
(`LibraryStore::HeldBytes`) - rather than the file's, which keeps the
pages of what was just deleted until the next start gives them back: a
trash emptied is no longer counted.

A trash folder and then a trash library came before (see "Dead ends"):
a delete that moves things somewhere else has to move them back whole,
and a stamp in the record moves nothing.

### Ids and names

Ids are random numbers below 36^6, checked against everything the
library holds (`MakeUid`). Random rather than counted because every
counted library starts at 1, so two libraries built independently
collide on nearly every id, and anything that ever moves things from one
into the other would be a guaranteed conflict. They cost nothing and
keep that door open.

A folder or canvas nobody has named is called for the moment it was
made, "2026-09-07 22:36:14": a counted "Folder 2, Folder 5" says nothing
about which is which a week later. Snippets are not named: the numbered
names they had ("Region 3") could not be changed and told nobody
anything, so nothing shows them any more and a new snippet has none.
`Item::name` stays in the record for what libraries already hold. Names
are not identity, and two may be alike; the id is what tells them apart.

## Persistence: the library file

Everything the overlay shows survives a restart, in one SQLite file:
`%LOCALAPPDATA%\Spickzettel\library.db`. There is no save action anywhere
in the UI; the session decides when to write (see "Session"). Loading
happens once, at startup.

### Where the file is

The library is kept on this computer, in `%LOCALAPPDATA%`, and the
settings with the user, in `%APPDATA%` (`config.json`) - since
2026-09-30; builds up to 0.2.0 kept the library beside the settings.

- **A roaming profile copies `%APPDATA%`** to a server at every sign-out
  and back at every sign-in, and Microsoft's guidance is to keep what is
  large out of it. A library of screenshots is large, and a copy of it
  that roams is also one that two computers can each change and one of
  them overwrite at sign-out.
- **Folder Redirection can put `%APPDATA%` on a server share** for good,
  which is how a library would end up on a network drive at all - where
  every commit crosses the network, and where SQLite trusts the file
  system's locks and flushes more than it should. `%LOCALAPPDATA%` cannot
  be redirected.
- **The library belongs to this computer anyway**: snippets are placed on
  its displays. Settings - hotkeys, profiles, the look - are what is
  worth having on another.

Both folders are asked of the shell (`SHGetKnownFolderPath`) for the
account the process runs as, not read from the `APPDATA` and
`LOCALAPPDATA` environment variables, as they were until 0.3.0. The
variables are only what the parent process handed down, and some
sandboxes appear to provide unreliable ones: another account's folders,
which the sandboxed process may not open, so that the settings and the
library both failed to load at a start. The shell's answer comes from
the process's own account and still follows Folder Redirection. The
cost is that a deliberately changed `%APPDATA%` is no longer followed.

A library where 0.2.0 kept it is moved at the first start that finds
none in `%LOCALAPPDATA%` (`LibraryStore::MoveHereFrom`, called by
`TrayController::Initialize` after the instance check, so that no copy of
the app has it open): opened and closed first, which plays a journal a
crash left back into it, so that it is one file to move; copied, beside
the new place as `library.db.moving`, and made the library only once the
copy is whole and flushed to the disk.
The flush is explicit: the original is removed right after, and a copy
still in the OS's cache would have gone with it at a power cut. Found
after 0.3.1 that the copy never ran on Windows: `std::filesystem::rename`
is `MoveFileExW` with `MOVEFILE_COPY_ALLOWED` there, which crosses drives
by copying straight to `library.db`, unflushed, and removes the original
when it returns. A power cut during that copy left a partial library
under the real name, which the next start set aside as unreadable while
the whole one stayed behind, never tried again. So it is copied always,
on the same drive too: a rename that refuses another drive is an OS call
(`MoveFileExW` without the flag), and `sz_core` makes none. One that
cannot be moved is opened where it is, and the move is tried
again at the next start. One in `%LOCALAPPDATA%` already wins, and the
old one is left alone. The crash dumps went along
(`%LOCALAPPDATA%\Spickzettel\crashes`); old ones stay where they were.

### Why a database

The library was a directory tree once, meant to be rearranged by hand in
a file manager, and every change to it was several filesystem steps - a
record written, a directory renamed, a picture moved - any of which a
crash, a full disk or another program holding a file could stop between
two others. Most of that store, and most of its bugs, were about
surviving it (see "Dead ends"), and two review rounds in a row found
real data loss in it.

A transaction removes the "between two steps" state altogether: a save
lands whole or not at all. SQLite also brings, already done, what the
tree did by hand: retrying through the antivirus scanners that hold a
file for a moment on Windows, rolling back what a crash interrupted, and
a version number in the file header. Its atomicity is tested by the
SQLite project far beyond anything this app could; the tests here are
about what the app writes. The tree's hand-editability went with it,
deliberately, and old libraries are not migrated: nothing released
wrote one worth keeping.

### Schema

```
meta      key, value              - which folder and which canvas are current
folders   id, position, name, created_at, deleted_at
canvases  id, folder_id, position, name, created_at, deleted_at
items     id, canvas_id, position, record (JSON), strokes (blob)
pictures  item_id, width, height, pixels (QOI), thumbnail (QOI)
```

A folder's and a canvas's fields are columns. A snippet's are a JSON
record, because there are twenty of them and more come: a new field is a
new key read with a default, and needs no change to the schema. Its
strokes are one packed blob rather than rows or JSON - a point as JSON
is an object of two keys, and an ordinary canvas holds tens of thousands
of them. Folders, canvases and snippets keep their random uids as row
ids (see "Ids and names").

`created_at` - and `createdAt` in a snippet's record - is when the thing
was made, a copy included: a copy is a new thing. Found in review on
2026-09-27: it was stored and documented from the start and never set,
so every row said 0, and when something was made could not be told
later. Nothing reads it yet.

A snippet's picture is keyed by the snippet and lives in the same file,
so the two cannot disagree about where either is. Foreign keys keep a
canvas in a folder and a snippet on a canvas, with a cascade, and a
trigger deletes a snippet's picture with the snippet, however it goes.
`pictures` itself has no foreign key, because a capture is stored before
the save that first writes its snippet.

The file's `application_id` ("Sztl") tells a library of ours from any
other SQLite file, and `user_version` is `LibraryStore::kFormatVersion`.
It goes up once per release that changes what is written, not once per
change. `auto_vacuum` is incremental, and every load gives back the
pages deleted rows left - to the file system at the next checkpoint (see
below) - so the file does not stay the size of the largest library it
ever held.

### Commits wait for nobody: the WAL

The file is in WAL mode with `synchronous=NORMAL`: a commit appends its
pages to `library.db-wal` and returns, without waiting for the disk. What
it waits for instead is a checkpoint (`LibraryStore::Checkpoint`), which
moves the WAL into the file and flushes both. It is made where a flush
that waits for a busy disk shows least (`TrayController::CheckpointLibrary`):

- **Hidden**, at once: when the overlay goes away, and after a silent
  capture that leaves it hidden. Nothing is on screen to wait for it
  (OVERLAY_STATES.md, section 6, step 10).
- **In view mode and the pinned view**, at the start of a frame, once one
  frame of the state is on screen: the first would leave edit mode's
  bars up for as long as the disk took, and after it a late frame looks
  the same as the one before, since nothing there moves by itself. View
  mode draws a frame every 250 ms when nothing happens, so leaving edit
  mode for it is checkpointed a quarter second later, and anything
  written in it - a silent capture - at the frame after.
- **In edit mode, only while the hand is still**
  (`TrayController::CheckpointWhenStill`): once what is unflushed has
  waited a minute, at the start of a frame with no input for five
  seconds and no mouse button held. Anywhere else in edit mode it would
  be the hitch the WAL takes out; there, a frame it makes late is one
  nobody is waiting on. Added after 0.3.1, from a review: an hour in edit
  mode had been an hour of changes a power cut could take. On a disk
  written flat out a checkpoint can take longer than the input grab
  waits for a frame (PERF.md: up to 3.7 s; the grab, two seconds), and a
  hand that moves again meanwhile reaches the game - see "The hooks stand
  down when the app thread stops". And by a
  write once the WAL holds 64 MB, so that the WAL does not grow without
  end however busy the hand.

A checkpoint with nothing written since does nothing, so this is a
flush per edit, however often the overlay changes mode. SQLite's own
checkpoint, every thousand pages, is off: it would come inside a commit,
on the render thread. Closing the file checkpoints it too, and takes the
WAL away. Leaving edit mode for view mode was added on 2026-09-30, at the
user's suggestion: someone who plays for hours with the snippets up in
view mode had nothing made durable since the edits before it.

It was SQLite's default rollback journal with `synchronous=FULL` until
2026-09-30, when a tester found a hitch "sometimes, after making some
snippets or drawing", on an almost empty library, with
`library.db-journal` beside the file for as long as it lasted. Every
command is written as it is made, on the render thread (see "Every
command is written as it is made"), and that journal flushed the journal
and then the file at every commit - and a flush waits for everything the
disk has queued, not only what this commit wrote. On an idle disk that
was 9 ms a commit, a frame at 120 Hz; with another program writing, a
median of 130-170 ms and up to seconds (PERF.md, "Results: the journal,
on a busy disk"). A commit into the WAL took 0.1 ms on an idle disk and
on a moderately busy one alike. On a disk written to flat out, one in
seven still waited - Windows makes a write wait once its cache is full -
where every one had before; only moving the writes off the render thread
would remove that, and a write that fails could then no longer take its
command back as it is made.

What `NORMAL` gives up is durability, not consistency. Every commit is
in the WAL whole, each of its frames checksummed, and the WAL is flushed
before a checkpoint writes any of it into the file: after a crash of the
app, of Windows, or a power cut, the next open finds a whole library,
reading from the WAL what it holds and dropping a torn tail. A crash of
the app loses nothing - what it committed is in Windows' cache already.
A power cut or a crash of Windows may lose what was committed since the
last checkpoint, which is since edit mode was last left or the hand last
paused in it: the library opens as it was a moment earlier. That holds as long as the disk does
what a flush asks, as the rollback journal needed too.

The file is held for the store alone, from the open to the close
(`locking_mode=EXCLUSIVE`, set before the first read). Two reasons:

- Without it, WAL keeps its index in a `-shm` file that every connection
  maps as shared memory, which does not work on a network drive, where
  a redirected folder can put the library. Held, SQLite keeps the index
  in this process's memory and makes no `-shm` file. Tried over SMB
  (the loopback share), the held WAL wrote and read back whole, and
  without the hold a `-shm` file was made.
- Another copy of the app on another computer, sharing the folder, is
  refused at its start (**Unreadable**, below) rather than writing the
  same file one transaction after the other, each over the other's
  changes. The single-instance mutex covers one computer only.

What it costs: no other program reads the library through SQLite while
the app runs, and one that copies `library.db` alone meanwhile copies
the library as of the last checkpoint.

### Opening

`LibraryStore::Open` says what it found, and `TrayController::Initialize`
asks before the tray icon, so a refusal is a message box and no start:

- **Opened** - including a file that was not there yet, which is made,
  directory and all. `Load` of a library this `Open` made returns
  nothing, which is what a first run is; a library someone emptied loads
  as an empty one. So does a file that holds nothing at all - no folder,
  no canvas, and not the `meta` rows every whole write leaves and nothing
  takes out: the schema is committed by the `Open` that makes it, so a
  first run whose first write failed leaves just that. It was loaded as
  an emptied library, and every start after it had no folder, no canvas
  and no welcome.
- **Written by a newer version** - `user_version` above this build's.
  Every row it saved back would lose what the newer build put there, so
  the store reads and writes nothing at all, and the app does not start.
  An older version's library, `user_version` below, is read as it is -
  every version reads what the ones before it wrote - and its
  `user_version` raised at once: what this build writes, the older one
  would lose, so from then on it is refused there. Version 2 is a
  stroke's corners (see "Tessellation, and its cache"): 0.2.1 would read
  the new stroke blobs as damaged, and write them back empty.
- **Unreadable** - the file is there and cannot be opened or read:
  another program holding it - another copy of the app, on another
  computer sharing the folder, among them - or not ours to read. A start
  over it would save an empty library where it was, so the app does not
  start, and says that trying again later may work.

A file that is not a library this store can read - not a SQLite
database, a damaged one, or someone else's - is set aside beside it as
`library-unreadable-<time>.db`, with its journal or WAL, and a new library
starts in its place. The app says so once, naming the file kept. A
journal or WAL that cannot go along, held open by another program say,
stops the new start: one left beside the new file would be played into
it. What was moved is put back, and the file counts as unreadable, as
one that cannot be moved at all does (after 0.3.1; the failure used to
be ignored). A load
that finds damage partway does the same, and one that fails partway for
any other reason - the file held past the wait below, a read error - is
an unreadable file, refused before the tray icon. It used to leave a
store that would write nothing, which the start took for a first run: the
welcome over an empty library, and every change refused.

Statements wait 250 ms for a lock another program holds - short,
because writes run on the render thread, and a command whose write gives
up is simply not made (see "Every command is written as it is made").
Since the store holds the file from its open, that is only ever the
open's wait.

### A write

`LibraryStore::Write(view, changes, pictures)` writes one command's
change in one transaction - the rows `changes` names (see
`LibraryChanges`, which the session works out from a checkpoint), read
from the model's view:

1. every folder and canvas row, when any of them changed - a few hundred
   rows at most, and rare;
2. the snippets taken out;
3. every snippet written whole - its record and its stroke blob - and
   the canvas and place of every snippet on each canvas one of them is
   on, or whose order changed;
4. the folders and canvases the library no longer holds, with what is
   still on them - after the snippets, so that one moved off a canvas
   deleted in the same write goes with the move;
5. the pictures the command made: a capture's pixels, a copy's picture
   copied from its source's as stored;
6. the current folder and canvas.

The store remembers nothing of what it wrote: which rows a command
touched is the command's to say, through its checkpoint, and nothing is
compared at write time. `Save(view)` is the same write with everything
named - every row, and every one the model does not hold taken out - for
a library made rather than changed: a first run's, a test's.

Every statement a write runs is checked, and one that fails fails the
write. Two were not. A statement that failed to prepare - a table the
file no longer has, another program's doing - was run all the same, and
SQLite built without `SQLITE_ENABLE_API_ARMOR` answers that with a
crash. And the read of the ids already in a table, which the rows the
view no longer holds are found among, could stop short unseen; the write
then committed with the rows past that point left in.

The transaction is an object that rolls back unless it is committed
(`WriteTransaction`). An exception partway through a write went past the
rollback, which ran only on a failure returned, and left the transaction
open: every write after it failed to begin its own. Nothing in the app
catches an exception today - the process ends, and the journal takes the
write back at the next open - so this is for whatever will. Its test
threw with a snippet named in bytes that are not UTF-8; that record is
written with U+FFFD since (see "Text is UTF-8, and so is the code
page"), no other write is known to throw, and the rollback on an
exception has no test now.

### Reading what cannot be used

A value a row carries that cannot be used is repaired rather than
refused, and the repaired row is written back by the load itself, as it
now reads. A float
that is not finite reads as its default - JSON has no infinity, but
1e100 becomes one the moment it is read as a float, and one infinite
coordinate poisons every bounding box it meets - and one with a range
is held inside it. A snippet's rectangles have one too, the range a
screen could have (`ReadRect`): finite is not enough, since 1e30 places
a snippet nowhere. A snippet whose size is not above zero gets the
smallest a snippet can have, as it could not be seen or picked; an
anchor of no size is kept, since that means not yet anchored. A value of the
wrong type is its default. A stroke
blob cut short keeps the strokes before the cut. A current canvas or
folder naming nothing - no row, or an id the library does not hold -
opens on the first one that exists and is not deleted.

A deletion stamp that is no date - negative, or past the year 3000 -
stays deleted and reads as the time of the load (`DeletionStamp`).
Found in review on 2026-09-27: such a stamp was read as it was, and
hovering the folder or canvas with Show deleted on ended the app - the
tooltip turned it into a date, which the C runtime cannot do before 1970
or past the year 3000, and the formatting then failed on a zeroed date,
which the invalid-parameter handler takes for a crash. The tooltip
copes with such a time on its own as well (see "Show deleted").

Any stamp but 0 means deleted, whatever its time, and one ahead of the
clock is kept as it is. The first fix for the above read every stamp
more than a day past the clock as not deleted, and wrote that back.
Found in the next review on 2026-09-27: a PC that starts with its clock
days behind - a dead CMOS battery, a restored VM snapshot, a clock wound
back for a game - makes every recent delete such a stamp, so folders and
canvases came back out of the trash and snippets onto their canvases,
for good. Kept, a stamp ahead is purged by retention that much later,
which is the side that loses nothing. A snippet whose stamp is no date
is still deleted, so ImportLibrary erases it at start as it does every
deleted snippet; nothing tells a damaged stamp from a real delete.

None current is not a pointer naming nothing, and a load keeps it.
Deleting a folder's last canvas leaves no canvas on screen and that
folder browsed, and the model never falls back to another folder's
canvas (see `CanvasManager::DeleteCanvas`). The load once took the 0 for
a dangling pointer, so a restart opened on the library's first canvas -
another folder's, or the one just deleted - and wrote that back. The
randomized persistence test did not catch it: it made a new canvas
whenever none was current, so the file never had to keep that state. It
now makes one only when no live canvas is left, and deletes the canvas
on screen as often as any other.

### Pictures: QOI, in the file

Pixels are stored as QOI. Measured on this app's own screenshots against
stb's PNG:

| | 1920x1080 capture | ~500x400 capture |
|---|---|---|
| PNG decode | 43 ms | 8.8 ms |
| QOI decode | 7 ms | 1.2 ms |
| PNG encode | 296 ms | 49 ms |
| QOI encode | 13 ms | 2.4 ms |

Decoding is what a canvas switch pays; encoding is what every screenshot
pays, synchronously, while the user waits. Both are lossless, and QOI
comes out ~30% smaller because stb's encoder is a weak one. Raw pixels
were measured too and are a trap: reading 8 MB costs more than reading
1.6 MB and decoding it. A 256px thumbnail is stored with every picture,
so the Overview never decodes a fullscreen capture to draw a 200px tile.

In the file rather than beside it, because nearly every bug of the tree
that was hard to find was a record and a picture file disagreeing. A
blob is a few milliseconds slower to read than a file, once per canvas
switch.

A capture's pixels are written in the same transaction as the snippet
they belong to, so neither is ever in the file without the other. A
picture is never changed after that. A copy of a snippet gets a copy of
the row as stored, in the copy's own write, without decoding it.

### Testing

`library_store_test.cpp` runs against real files. Tests share the file
(`tests/support/shared_library.cpp` turns the store's hold off for every
test executable), because they read a library back, and make its writes
fail, through a second SQLite connection while a store has it open; the
hold itself has its own test, which turns it back on
(`TheAppsStoreHoldsItsFileForItself`). What would fail a write is
another program holding the file, done with that second connection
(`tests/support/held_library.h`): a write lock stops writes and leaves
reads, and a hold of the file itself stops both - but only before a
store has opened it, since a store that shares a WAL reads through its
`-shm` index past any hold. A write that fails partway is a trigger the
test adds that aborts on one row, which shows the rows before it rolled
back with it. The randomized test in `history_test.cpp` fails writes the
same way, with triggers on every table that abort while a flag row
exists - instant, where a held lock costs the busy timeout each time.

## Configuration

`AppConfig` is every user-editable setting, read from and written to
`config.json` by `TryParseConfig`/`SerializeConfig`. Parsing is pure core
logic; only *where* the file lives is platform-specific.

**Every setting is one row of a catalog** (`settings_catalog.h`, and
`docs/SETTINGS.md` for the whole plan). A row says where the setting is
in the file, what values it may hold (its rule), when a change to it
takes effect, and where its value lives, and reading and writing the file
are loops over the rows. Written out by hand, as it was, one setting was
named in up to seven places, and nothing failed when a copy was missed.
A rule's `Hold` is the one answer to what a setting keeps given a value -
held to a band, or rejected - and the parser asks it, as every edit does.

The overridable settings are held once, as `AppConfig::profileable`, a
`ProfileableSettings` named as the file names them - not also as copies
under names of `AppConfig`'s own, copied back and forth: the rows say
where each is in the file.

C++ cannot list a struct's fields, so a field without a row would be a
setting nothing reads or writes, and no test of the rows could see it.
Two things can: the number of initializers a struct takes, compared at
compile time with the number of rows for the profile's two structs and
with a constant for `AppConfig`; and a test that takes the structs apart
with structured bindings and checks that changing every row changes every
field.

The file is JSON rather than flat `key=value` lines because of
profiles: a profile matches on a list of executable names and window
titles, which are arbitrary strings holding `=`, `#`, commas and
non-ASCII, and any flat encoding of "a list of arbitrary strings" grows
a bespoke escaping scheme with its own bugs. nlohmann/json was already
in the binary for the library.

Four things about the file are deliberate:

- **Groups, not a flat namespace** (`hotkeys`, `drawing`, `appearance`,
  `bars`, `overview`, `defaults`, `deleted`, `display`, `behavior`,
  `shortcuts`, `diagnostics`).
  Where each setting is in them is said by its row in the catalog. The
  grouping is not cosmetic: `behavior` and `shortcuts` are
  exactly the settings a per-application profile may override, so a
  profile is those two objects again, sparse.
- **Absent means inherit, `null` means explicitly unset.** "Said
  nothing" and "said none" have different spellings, which a shortcut
  that ships bound and is unbound on purpose depends on. The summon
  hotkeys use the same spelling: a hotkey given another's combination
  leaves that other unbound, and an unbound hotkey read back as "said
  nothing" took its default again on the next start - which could now be
  the combination the other had taken, and one combination registered
  twice left the second hotkey dead. A file edited by hand to give two
  hotkeys one combination has the later one unbound as it is read.
  Every setting is written, defaults included, so the file documents
  what can be set.
- **A hotkey another application owns does not stop the start.** A
  screenshot tool on Ctrl+Alt+C would otherwise cost the whole app, with
  a hand edit of `config.json` as the only way back in. The hotkey is
  left unregistered, and a message box names each one with its
  combination; the tray menu reaches the overlay without any, and
  Settings > Hotkeys can pick another.
- **`ordered_json`, and floats rounded to six decimals**, because the
  file is meant to be opened and read: alphabetical keys interleave
  settings by spelling, and `0.22f` promoted to double writes as
  `0.2199999988079071`.

Malformed input is never an error: a value of the wrong type or out of
range leaves that setting at its default, the same contract the library
has. A file that is not settings at all - not JSON, or too big to be -
is not a first run either: read as defaults, it would be written over by
the next settings change, and a stray comma would cost every hotkey and
profile. `LoadOrCreateConfig` renames it to
`config-unreadable-<stamp>.json`, and one that cannot be opened is left
where it is and not written over for that run. Either way the app starts
on the defaults, says so in a message box, and skips the retention
period, since whether it was on is what could not be read. Skipping one
start was not enough while retention was on by default - the next found
no file, or the defaults, and both turned a 14-day retention back on
over a library whose owner may have switched it off - so the stand-in
written in its place has retention off until it is switched on again,
even when the file could not be moved aside. The defaults have it off
now too (see "Deletion is a mark"), and the stand-in still says so itself. The tray writes the stand-in again as it starts, in case that
write failed, and a write that fails is owed and retried from the
background timer, like any settings write. The file is written through
temp-then-rename, since truncating it in place leaves a window in which
every setting is a half-written file.

What a file can say that no one setting's rule rules out, and the app
cannot run with, is repaired as it is read, in one place
(`RepairOnLoad`): both creation triggers on one press go back to their
defaults, a later duplicate of an earlier summon hotkey is unbound, and
profile names are made non-empty and unique. A file that needed any of
these is written back as the app starts, so that the file says what runs
rather than one thing while the app does another. A value held to
its rule is not a repair: out of range or missing, it reads the same at
every start, and the next settings change writes it anyway.

The file's version is read (`config_migrations.h`). An older file is
brought up to this build's version before anything in it is read, by a
chain of steps that each take version n to n+1, and is then written back
at start like a repair. A chain rather than a conversion from each old
version straight to the current one: those would all be rewritten at
every new version, against shapes nobody had looked at in a while, where
a step is written once, between two shapes that are both fresh. A step
works on the JSON with its keys spelled out and calls nothing of the
current build's, which changes under it; and once a build writing its
target version is out, it is never edited. Adding or dropping a key is
not a version, since an absent key reads as its default and an unknown
one is ignored - which costs a downgrade the keys added since (see
`docs/SETTINGS.md`, section 8). A file a newer build wrote is read as well as this build
can, and not written over for the run, which would lose whatever the
newer build had stored. It is the
arrangement a file that cannot be read already had, message and skipped
retention period included - a newer build may have moved the retention
keys, and read as defaults they turn a 14-day purge back on. Refusing to
start, as for a newer library, would have cost the whole app over
settings, which unlike a library can be read in part.

A file kept for the run - one a newer build wrote, one that cannot be
opened, one that is not settings and could not be moved aside - leaves
every settings change applied and unsaved. The message box says so, at
the start; Settings says so too, above every section, for as long as it
lasts (`SettingsPage::SetFileKept`). The app runs in the tray for days,
and a change that looked saved and was gone at the next start was not
connected to a box dismissed long before. Not the line along the bottom
that a failed write gets: nothing has failed, and it would stand over
the canvas for the whole run.

The files a release wrote are kept as test fixtures
(`tests/core/config_files/`): `v0.1.0`'s with the defaults and with every
setting it had changed, produced by that release's own serializer. Each
must go on reading as the config it was written from, because once a
build is out, a renamed key is a setting that goes back to its default
for everyone who upgrades, and nothing else would notice. What this build
writes is kept beside them and must come out byte for byte, so a change
to the file is a diff to review. `docs/SETTINGS.md` is the design this
section summarizes, with what each change was for.

`KeyCombo` represents a hotkey as modifiers plus one logical key rather
than an OS virtual-key code, and no modifier is required: a bare
function key is a legitimate hotkey, and refusing plain letters is a
possible later restriction rather than a rule today.

### Tool shortcuts

Every drawing tool, creation tool and clipboard action can carry a key,
pressed while the overlay is up in edit mode. Every one ships bound
(`DefaultShortcuts`). The tools sit on the letters around W, A, S and D,
where the left hand already rests over a game: `Q` the plain pointer,
`W` pen, `E` eraser, `A` text, `S` screenshot, `D` drawing. The
clipboard has its usual `Ctrl+C/X/V`, with `Ctrl+Shift+V` to paste in
place, `Ctrl+A` to select all and `Ctrl+D` to duplicate; `Ctrl+N` makes
a new canvas, `Ctrl+Shift+N` one the selection comes along to, and
`Ctrl+H` opens the cheat sheet. A chord cannot fire from ordinary
typing - `Ctrl+D` sits beside the plain `D` that makes a drawing, and the
exact-modifier match keeps them apart - and a bare letter types into a
note while one is being typed, as every key does (`TypingNote`).

Until 0.2.3 only four letters shipped (`S`, `D`, `E` and `P` for the
pen) and Text, Select and New canvas started unset, on the grounds that
a shortcut firing a tool you did not want is worse than none. Used over
games, the tools wanted to be together under one hand more than they
wanted to be safe from a stray key, and in edit mode no key is anything
else's. A settings file lists every shortcut it was written with, so the
new defaults reach a new file only; an older one keeps its keys, and is
not migrated, since a key still at the old default cannot be told from
one chosen to be there (user decision, 2026-10-03). Set by hand in
Settings > Hotkeys, or left out of the file, a key takes the new default.

A shortcut can be a mouse button instead of a key: the middle one or a
side one, with modifiers or without, pressed at the row in Settings like
a key. Config spells them `Mouse3`, `Mouse4` and `Mouse5`, the way games
number them. Not the left or the right, which are what every gesture is
made with; and not as a global hotkey, which Windows registers for keys
only - one written into the file by hand is read as nothing said. A
bound button reaches its command as a key does, over a panel too - none
of them does anything with these buttons, and taken for the panel's, the
button that opened the cheat sheet could not close it. Only while a
gesture is in flight does it wait, as the wheel does (see "The hand").

Duplicate is Copy and Paste in one step and deliberately does not go
through the clipboard: duplicating something is not a reason to lose what
was copied earlier. It is also the one that puts a copy beside its
source.

Paste puts what it pastes at a point: the pointer for its key, where the
right click was for a menu's row (`Command::at`), not where the pointer
went to choose the row. The middle of what is pasted goes there, and
the snippets are moved as one, so where they stand to each other is
kept, then onto the screen as a whole - against its edge, or with the
top left corner on it for a group bigger than the screen
(`Session::PlaceAround`). A fullscreen snippet keeps its place, the
screen. A key has no position of its own, so the editor is told where
the pointer is at every event, as ImGui last saw it, the overlay's own
pointer while input is grabbed. Paste in place puts everything where it
was: a copy exactly on its source when that is on the canvas, which is
what the name says and what the programs that have it do. A cut pasted
at the pointer onto its own canvas is a move there, undone as one; onto
another canvas, the move between canvases and the new place are one
change (`history::Moved::placement`), since a step holds one change per
snippet.

Select all (Ctrl+A) takes every snippet on the canvas that can be
selected, not one deleted or minimized, and leaves drawing mode first,
since there the selection is the snippet being drawn on. While a note is
typed it is the field's, as every key is.

Until 0.2.3 there was one Paste, and it offset every copy 24 px when any
of them had its source on the canvas - so a copy from another canvas was
moved too, which the code's own comment said it would not be. Found in
review on 2026-10-02; the two pastes left no case for the offset. `Ctrl+Shift+N` moves the selected snippets to the
canvas it makes, which otherwise costs a new canvas, a switch back, a
cut, a switch forward and a paste; it is a separate action from the plain
new canvas rather than one that reads the selection, so the canvas bar's
own "+" keeps meaning only what its icon says. With nothing selected the
two do the same thing, because an empty selection is no reason to refuse
the canvas. These are not OS hotkeys and
are stored apart from the summon hotkeys: the overlay reads them from
its own input, they only do anything while it takes input, and nothing
about them can fail the way registering a global hotkey can, which is
also why a bare letter is allowed here and questionable there.

`ShortcutAction` is the flat list the config layer persists, by name.
An action added since a file was written - Paste in place and Select
all, in 0.2.3 - has its default, unless the file gives that combination
to another action, which keeps it; the new one starts unbound. Taken,
the key went to whichever of the two comes first in the command table,
which could be the new one.
Config sits below the app and cannot see what a key does; the command
table ties each action to its command (see "Commands" under the overlay
UI), and a test checks that every action names exactly one.

### Per-application profiles

A profile is a name, match rules and sparse overrides. Two levels, always
the same two: exactly one profile matches at a time, first in list
order, so "which profile am I in" has one answer; and everything it does
not state comes from the defaults, so "where did this value come from"
has two possible answers and no chain to trace.

A third level - a profile `basedOn` another - was tried and dropped: a
list where every entry names what it derives from has to be read rather
than scanned, nothing stopped a chain four deep, and typing the same
three settings into a second profile costs seconds, once.

`ProfileOverrides` is `std::optional` throughout because "says nothing"
must be distinct from "says false". For a shortcut that is three states:
nullopt inherits, a default `KeyCombo` is explicitly unbound, anything
else is a binding.

**An override is what you touched, not what happens to differ.**
Inferring overrides from values unlike the inherited ones cannot tell "I
never touched this here" from "I set this here and it happens to match",
and only the second should survive a later change to the defaults. So
overrides are written directly, and the UI makes the state visible and
reversible with a marker and a revert arrow per row.

What is overridable is exactly the `behavior` and `shortcuts` groups: the
settings about the machine in front of you rather than about you.
Colors and the rest are deliberately not.

**The summon hotkeys are not overridable, and the obstacle is the OS.**
`RegisterHotKey` is exclusive and system-wide, so per-application
hotkeys would mean the registration following the foreground: a
foreground-change hook while the overlay is down, a rule freezing the
swap while it is up (edit mode taking focus makes *this* the foreground
application), somewhere to report a registration that fails per
application, and an unavoidable race on alt-tab. The alternative -
matching combinations in a permanently installed low-level keyboard hook
- is an always-on system-wide hook in every keystroke's path, in an app
whose purpose is to sit over games with anti-cheat. That is a posture
decision rather than an implementation one, and it has been decided
against.

Matching is on lowercased executable name (several per profile, since a
launcher and the game it starts arrive under different names), with
case-insensitive title substrings as the fallback for a process whose
image path cannot be read. Resolution happens once, on the way up, and
is deliberately not re-run when the foreground changes while the overlay
is showing: that would tear the input hooks down mid-session, and "which
profile am I in" would stop having one answer while the panel is open.

A profile always has a name, since the name is what the list and the
picker show. The name field never stores an empty one: cleared to be
retyped, the profile keeps its old name until something is typed. A file
edited by hand can still hold a nameless profile, and it is read back
under the name a new profile would get rather than dropped, which lost
its match and every override at the next start.

### Which display

`ChooseDisplay` turns the remembered display id and name into an
attached display, and always answers: the display with that id; else
the only display with that name, which is the same monitor on another
port; else the primary. Two monitors sharing a name are not guessed
between. A chosen display that is not attached is not forgotten; the
primary stands in until it returns.

## Session and settings

The layers above the platform are three libraries, `sz_core`, `sz_ui`
and `sz_app`, the `core`, `ui` and `app` of the module layout above.

**`Settings`** is the one copy of every setting. `Stored()` is what
`config.json` holds; `Live()` is the profileable group resolved against
the profile that matched what the overlay came up over. Every change to
the stored settings is an edit through it: `Set` names the setting's row
in the catalog (and, for an overridable one, whether the defaults or a
profile is meant), holds the value to the row's rule, applies the edit
repair of any invariant it touches, and commits. A slider or a color
being dragged is a `Preview` - stored, so everything drawn shows it, but
committed only when the drag ends. ImGui says a drag ended only in a
frame that draws the widget, so a drag cut short by leaving edit mode or
putting the overlay away was shown and never written; the overlay commits
the previews wherever it settles. A commit re-resolves and calls the
controller back, which applies what changed to the window and writes the
file. The live values are derived, never assigned, so there is no path
by which what runs and what is stored can disagree.

There is one way in. There were three - fields written in place and
committed, overridable ones through setters, hotkeys by the tray - and
nothing held what the first wrote to a rule, so the Settings panel
restated each band beside its widget. One edit path means the rule is
asked in one place, the file is written by one commit, and a new setting
needs no code of its own to be edited:
the Settings panel's widgets take a row (`ui/settings_widgets.h`). A
summon hotkey is still registered with the OS first, since a combination
another application owns must not be stored; the tray then makes the
edit, and `HotkeySetting` is the one table from a hotkey to its row.
A profile's rename is an edit of its own because it can be refused: a
name another profile has is not taken, and the field says so while it is
typed. Numbering it on the spot would change the text under the cursor,
and taking it as typed left two profiles of one name, numbered behind
the user's back at the next start.

**`Session`** is what is being worked on, independent of how it is
shown: the library and deleting and restoring in it; every command
written to the library as it is made; the texture cache the app draws
from; screen capture (the frozen screen, a snippet's capture, the
picture a copy gets); and the per-canvas undo history with every edit
that goes on it, offered as commands (`DeleteItem`, `ClearDrawing`,
`CommitLiveStroke`, text edits) and as gestures in screen space (erase,
shapes). The session needs two things of the platform, textures
and captures, and takes them from the window it is attached to, which may
be absent: the session tests drive all of this with no window, no store
and no ImGui.

What stays in the UI is what a UI decides: which tool is in hand and its
color and width, where a gesture starts and what it is over, panel and
popover state, toasts, and GPU caches that exist only for drawing.

### Only the session changes the library

The UI holds the model as `const CanvasManager&`. Every change it makes -
a pin, a rename, a slider, a drag, a paste - is a session command, so a
change reaches the history, the disk and the GPU in one place, and the
compiler refuses one that tries to go around them. When the UI wrote
fields in place, some forty sites each had to remember, separately, to
file an undo entry, to forget history when a snippet left its canvas, or
to sync textures - which is where the history's gaps came from.

A gesture that changes something continuously is previewed through the
session and ends as one command: a drag opens a placement
(`BeginPlacement`, `PreviewRect`, `EndPlacement`), a popover slider or
the color picker a style edit (`PreviewStyle`, ended when the hand lets
go of the widget), the eraser and the shapes their own gestures. A
style edit, like a placement, can hold several snippets as one step
(`PreviewStyles`): the opacity wheel changes the whole selection. A new
snippet is made whole, from a prototype the UI fills with Settings >
Defaults, instead of being made and then adjusted field by field; a paste,
a duplicate and a send to another canvas are one call each. The stroke
being drawn lives on the session's live layer rather than on the canvas:
scratch, not content, and dropped on a canvas switch.

The tests set a library up directly through `SessionTestAccess`, which is
a friend of the session; nothing in `src` can reach it.

### Every command is written as it is made

Each command the session runs is written to the library before it
returns, in one transaction: the file holds what the model holds at
every moment but the middle of a gesture, and there is nothing to save.
The debounced autosave it replaced (see "Dead ends") needed flushes,
retries, a recovery copy and a store that diffed every row, all to carry
changes that were in memory and not yet on disk; there are none.

**What a command writes** is worked out, not said: before it runs, the
session takes a checkpoint (`CanvasManager::TakeCheckpoint`) - every
folder, every canvas with the ids of its snippets in order, which folder
and canvas are current, and copies of the snippets the command names,
the only ones whose content it may change - and afterwards
`ChangesSince` compares: snippets that came, went or moved, canvases
whose order changed, folder and canvas rows that differ, and the named
snippets whose content did. The checkpoint is ids and a few snippets,
cheap to take on every command; a command that forgot to name a snippet
it changed would leave that change out of the file, which is what
`HistoryTest.TheFileHoldsWhatTheModelHoldsWhateverFailsToBeWritten`
exists to catch - random commands against a real file, with the file
read back and compared with the model throughout. A snippet written is
written with its canvas's whole order, so that a place never lands
beside a stale one; a screenshot's pixels and a copy's picture are
written in the same transaction as the snippet they belong to.

**A command whose write fails is not made.** The model goes back to the
checkpoint (`CanvasManager::RollBack`) - a capture gone again, a
moved snippet back where it was, a snippet deleted for good back with
its history - and a line along the bottom of the screen says the
library could not be written and the last change was not made, drawn
from `Session::LastWriteFailed` until a write lands - which a command
with nothing to write, a click that selected, does not - and for eight
seconds at least from the frame it is first drawn in, whatever lands
(`Messages::PersistenceWarning`): a write landing a moment after the
failure took the line down with it, before anyone could read it.
Nothing is filed on the history for it, and an undo or redo whose write
fails puts its step back on its stack as it was. A disk that is full or
a write the disk refuses loses the change being made, visibly, and
nothing else: a screenshot that cannot be written is not taken, rather
than kept in memory looking captured. Under the autosave, the same
failure kept every change since in memory, for an exit to lose.

**Nor is what comes after it in the same command.** Every command first
ends what is open - `Editor::Settle` for the hand, and
`Session::EndOpenGesture` under it - and that write can fail too. What
it ended then goes back as it was, and the command does nothing more:
Ctrl+Z mid-drag with the drag's write failing took back the step before
the drag as well, a second thing gone for one key, and the undo's own
write, landing, took the line down before the drag's failure had been
drawn. `Session::FailedWrites` counts the failures, which is how a
caller tells across a call whether anything it wrote failed.

A gesture - a drag, a slider, a note being typed, the eraser - is
previewed in the model and written once, as the command it ends in; its
checkpoint is taken when it begins. A crash in the middle of one loses
that gesture and nothing before it. A first run writes the library it
begins with (`Session::WriteWholeLibrary`) before the first command,
which writes only what it changes and would find what holds it missing.
Should that first write fail, the store writes the whole library with
whatever write comes next, until one lands: a snippet written alone
onto a canvas the file does not have is refused by the foreign key, and
so was every write after a failed first one, until one happened to
write the folders and canvases.

What each costs is in docs/PERF.md: a stroke, the heaviest ordinary
command, is one transaction of the snippet's record and strokes; with
`synchronous=FULL` it is bound by the disk's flush, around ten
milliseconds on a local SSD, paid on the frame the command lands in.

A command that ends a stroke in flight - the view-only hotkey while the
button is held, say - keeps it as a release would: its own undo step,
written like any other. Escape cancels it instead (see "The hand").
Hiding the overlay, restarting it for a setting, exiting and the OS
ending the session all settle what the hand is in the middle of first
(`OverlayApp::Settle`), which writes it.

**One writer per library.** Two copies of the app would each write the
library from a stale picture of it.
The tray claims a per-user named mutex before it does anything else,
and a second copy exits instead of loading the library. Per user is per profile, which is per library;
the kernel drops the mutex with the process, so a copy that crashed
holds nothing. A hotkey collision is not a lock: with a hand-edited
config the two copies could have different hotkeys and never notice
each other. The mutex is one computer's: another copy of the app on
another computer, where a shared folder puts the same file in front of
both, is kept out by the file's hold instead (see "Commits wait for
nobody: the WAL").

**Starting the app again brings up the copy running.** Someone who
starts an app that is already running wants that app, not a message
that it is. The second copy finds the first by its host window, which
is titled for the instance - the mutex's name, and so the user's: not
another account's copy in the same session - posts it a message
registered for the purpose (`PassOpeningToRunningCopy`), and exits. It
first lets the first copy take the foreground (`AllowSetForegroundWindow`),
which only a program the user just started may give, and the message is
let up to an elevated first copy as `TaskbarCreated` is. The running copy
takes it as the Edit request, but one that never puts the overlay away
(`TrayController::OnOpenedAgain`, and OVERLAY_STATES.md, section 4). The
app's message box that another copy may be running is left for when no
copy answers - another account's, say. Added on 2026-09-30, asked for by
the user.

The mutex is in the session's namespace (`Local\`) and named for the
user's SID as well, because the session is not the user: "Run as
administrator" from a standard account runs the app as the
administrator account, in the same session, with a library of its own,
and the session's name alone would turn it away. From an administrator
account the same command runs the same user elevated, over the same
library, and that copy's mutex is made for the Administrators group at
high integrity. The user unelevated may not open it, and `CreateMutex`,
which asks every right of a mutex that is already there, fails with
`ERROR_ACCESS_DENIED` rather than answering `ERROR_ALREADY_EXISTS`.
So that answer is a copy running too. It was once taken for "could not
ask" and let the second copy start, and two writers on one library
delete each other's canvases: a layout write removes the rows its own
picture does not have. Any other failure still starts, as one that says
nothing about a copy of the app. The test stands in for the elevated
copy with a mutex whose DACL grants no one anything; that is the answer
the documentation gives, not one measured against an elevated copy.

**The settings file** is written by the tray, which is the only writer
of it; one that could not be written is said on the same line along the
bottom, and remembered as owed: the background timer (a `WM_TIMER` on
the host window, the one clock the app has while hidden) tries it again
every ten seconds, whether or not the overlay is up, and exit tries it
once more before the app goes.

The timer's callback is called on a copy, as a hotkey's is. Found in the
next review on 2026-09-27: the retry that saves the file sets the timer
again from inside the callback, which destroyed the running closure;
harmless only because nothing it captured was touched afterwards.

Exit and the OS ending the session (`WM_QUERYENDSESSION`, answered TRUE
after settling, and `WM_ENDSESSION` again for good measure) reach the
app as a broadcast to every *top-level* window, and Windows leaves
message-only (`HWND_MESSAGE`) windows off that list. So the host window
is an ordinary hidden top-level window - as a message-only one, no
logoff reached it - and the test finds it with `FindWindow`, which
likewise sees only top-level windows, and sends it the query. Being
top-level, it also receives `WM_CLOSE` - `taskkill` without `/f` posts
it - which `DefWindowProc` would answer by destroying the window and
nothing else, leaving a process with no tray icon and no hotkeys, still
holding the single-instance mutex. A close from outside, and the Restart
Manager's `ENDSESSION_CLOSEAPP`, take the tray menu's Exit, and a
close-app runs no session-end settling before the exit's own. So does a `WM_CLOSE` sent to the overlay take the
Exit, which is where `taskkill` sends it while the overlay is up - it
closes the windows it can see, and the host window is hidden. Alt+F4
over the overlay arrives as `SC_CLOSE` instead, and stays swallowed.

The exit then has to end the loop. Found in review on 2026-09-27: `Quit`
only set a flag, and hidden, the loop waits in `GetMessage`, which
handles a *sent* message inside itself and returns only for a posted
one. The Restart Manager's close and `WM_CLOSE` both come sent, so with
the app in the tray - where it usually is - the exit settled and the
process stayed until something unrelated was posted, past the Restart
Manager's wait. `Quit` now posts a `WM_NULL` to wake the loop - not
`WM_QUIT`, which would also end whatever message box is up. And the loop
no longer sets itself running as it starts, which wiped a `Quit` that
came first: a close while a startup message box was up was forgotten.

### Textures

Every GPU texture the app draws with is held by one object,
`TextureCache`, and by nothing else. A texture is asked for by what it
shows - a `TextureKey`: a snippet's picture, its thumbnail, its stroke
raster, the frozen screen - at the moment it is drawn, and made from its
pixels when there is none: a picture read from the library, a thumbnail
read or scaled down, a raster uploaded from its bitmap, the frozen
screen from the pixels the session keeps. A handle is good for the frame
it was asked for in, and nothing keeps one past it. With four owners
of their own, each releasing on occasions of its own, a replaced device
had to be answered by each of them, from a list a fifth owner would not
have been on (see "Dead ends"); and the model carried the GPU through
every checkpoint, rollback and copy.

**What is not drawn goes.** A texture no one asked for through a whole
frame is released at the start of the next (`TextureCache::BeginFrame`).
That one rule is the lifetime of everything: a canvas switched away
from, a snippet deleted or sent elsewhere, a panel closed, a capture
whose write failed - each gives its textures back without anyone saying
so. What must stay while it is not drawn is asked for all the same:
each frame begins by asking for the textures of every snippet on the
current canvas (`CanvasView::KeepCurrentCanvasTextures`), minimized ones
and those the pinned view leaves out included: only the current canvas
is on the GPU, since a library of fifty 4K captures would otherwise pin
~1.6 GB of VRAM behind a game. The frame of grace covers a canvas
switched away from after something of it was drawn.

**A failure is remembered.** A texture that could not be made - no
pixels, or an upload that failed - is kept as 0, so that a picture that
cannot be read is not read on every frame. It is tried again once it has
gone unasked for a frame, or the device is replaced.

**A capture is not read back.** The session puts a capture's texture
into the cache from the pixels it has just taken, so the frame that
shows the new snippet does not decode the picture just written.

**A release waits for the frame.** A texture may be released mid-frame
after it has already been drawn into that frame: the frozen screen
dropped as the overlay goes, a texture made again in place of another.
ImGui's draw commands hold the raw pointer without a
reference and are only submitted at the end of the frame, so the D3D11
renderer holds releases made between `NewFrame` and `RenderAndPresent`
until the frame has been handed to D3D, which keeps what it uses alive
from there. Doing it in the renderer covers every caller, rather than
asking each of them to order its mutations before its drawing.

**A lost device is replaced in place.** The driver can take the D3D11
device away: an update, or a restart after it stopped responding, which
a game underneath can cause. Unanswered, every later frame fails
silently - the overlay blank for the rest of the process while its
input grab still takes the input - and a resize that cannot make its
render target leaves none for the next frame to clear: an access
violation inside d3d11.dll. `ReadyToRender`, at the start of each
frame, checks the device and replaces a lost one, with the swapchain,
the filter shaders and ImGui's backend. The ImGui context stays, and
ImGui makes its font atlas again by itself. While no device can be made,
the frame is skipped: a short wait stands in for vsync, and the grab,
no longer hearing from the frame loop, lets input through. What the app
holds cannot be carried over: every texture was made on the old device.
So the window's `TextureGeneration` moves on, and the cache, which looks
at it before every answer, lets go of every texture before it hands out
another; each is made again as it is next asked for - the pictures from
the library, the frozen screen from its pixels, the rasters from their
bitmaps. No handle from a device that is gone is handed out, whatever
order the calls come in. A stray old texture handed to
`UpdateTextureRegionRGBA` is refused, not written with the new device's
context.

A capture is pixels only (`IOverlayWindow::CaptureRegion`); uploading
them is the cache's. Only a frame notices a lost device, and no frame
runs while the overlay is hidden, so a capture made from the tray after
a driver reset cannot be uploaded. Its pixels are stored all the same -
they are the one thing that cannot be taken again - and its texture is
read from the library once the device is replaced. A freeze keeps its
pixels the same way: shots are cut from what was frozen, and the frozen
screen shows once the device is back.

The generation moves once the new device is made, not as the old one is
let go of. A capture taken while the driver is still on its way back -
the capture hotkey works while frames are skipped - cannot be uploaded,
and is kept as a failure until the generation moves. Moved already, it
never did: the new device came up under the same generation, and the
snippet showed its placeholder for as long as its canvas was on screen.

`HeadlessSaveTest.EveryTextureDrawnIsLiveWhateverHappens` holds all of
this to account: random commands, undo and redo, canvases switched,
deleted and erased, the renderer switched, the canvas bar's previews
shown, the overlay put away, the device replaced and uploads failing -
with every frame's draw lists checked against the textures the fake
window has live on its current device, none released twice, and the
window holding exactly the textures the cache does.

**A copy owns its pixels.** The clipboard holds ids, not pixels, and a
copy made from them (paste, duplicate, copy to another canvas) must not
share a picture with its source. `CanvasManager` starts the copy without
one; `Session::ClonePicturesForCopy` then gives
it its picture - the source's row copied as stored, in the write that
makes the copy. The UI says so when the source's picture cannot be
found, rather than showing a copy that looks whole.

### Undo is per canvas

History is two stacks per canvas, undo and redo, each capped at 50 steps
*and* 128 MB of what they hold (a Clear drawing holds every stroke it
took). A canvas is this app's document, and undo scoped to a document is
what every editor does. One global stack reached across canvases and
failed invisibly: draw on A, switch to B, draw, come back to A, press
Ctrl+Z, and the stroke that vanished was B's, on a canvas you were not
looking at. `core/session/history.h` is the whole of it.

**A step is a list of changes, each about exactly one snippet.** Seven
kinds, and each is its own inverse or a mirror, so undo and redo are one
walk in opposite directions:

- `StrokeAdded` carries the stroke, so redo can push it back; undo takes
  it off the end, where it went.
- `StrokesErased` is a list of *replacements*: for each original a
  gesture clipped, its index in the list as it was, the original, and the
  fragments now standing in its place. Undo rebuilds the before-list from
  the after-list and redo the reverse, both by position. The session
  follows the list through the gesture - the manager's erase reports what
  became of each stroke, index for index - so a fragment clipped again
  later in the same drag still traces back to the stroke that was there
  before it. The first version diffed by value and appended the
  originals at the end: that changed the draw order, left the next undo
  of a stroke popping a different one, and could not tell equal strokes
  apart. A gesture that loses track of its fragments - which only a bug
  could make it do - is filed as one replacement of the whole list, exact
  if coarse, rather than not at all.
- `TextChanged`, `PlacementChanged`, `StyleChanged` and `DeletionChanged`
  hold the other side's value and swap it with the snippet's. The
  deletion mark is what a new snippet, a copy and a delete all are:
  undone, a new snippet is marked deleted, where a screenshot taken by
  mistake can still be found.
- `Moved` is a snippet going from one canvas to another - a cut's paste,
  a move from the picker, the selection taken to a new canvas - with the
  place it stood in the stack at each end, taken as it leaves, so that an
  undo and a redo put it back exactly where the other found it.

A step about several snippets - a Delete of a selection, a group drag, a
paste - is one undo for all of them. One entry per snippet made deleting
a selection that many undos, and past the cap of 50 the earliest could
not be undone at all: for a deleted snippet, deleted for good.

**Every step on a stack applies when it is reached**, whatever happened
in the library in between; nothing is ever refused at Ctrl+Z time. The
rules that keep it so are applied eagerly, the moment they become true:

- *A snippet's undo changes are on the stack of the canvas holding it.*
  When it moves - pasted, sent, or by an undo or redo of either - they
  move with it, merged in by when each was done (`History::Migrate`); a
  step about several snippets is split between their canvases, and a part
  that meets its step again rejoins it. This is what makes a move
  undoable at all: the paste is the newest change about the snippet, on
  the canvas it is now on, and its older changes are beneath it there.
  Before, a move forgot the snippet's history outright. A part rejoins in
  its own place among the rest, which each change keeps from when its
  step was filed. It was appended, and order is what a group move's
  changes are made of: each snippet goes back to the index it left at, in
  the reverse of the order they left in, and undone out of turn two of
  them came back in each other's places in the stack.
- *A new change to a snippet drops its changes from every redo stack*,
  besides the usual rule that a new step clears its own canvas's redo
  stack: the future they were for is gone. A paste undone and the snippet
  then drawn on where it went back to is not pasted again by a redo.
- *An undo or redo that changes a snippet drops its changes from the redo
  stacks of every other canvas*, which were for a state it has just left.
- *A snippet deleted for good takes every change about it with it; a
  canvas deleted for good takes its stacks, every move from or to it
  anywhere, and every move its snippets made before one of those.* A
  paste whose source canvas is gone for good is no longer undoable, and
  the snippet is its new canvas's for good; the moves before it go with
  it, since each is from a place no undo can take the snippet back to
  now, and everything else about it is still undoable. The earlier moves
  were kept until 2026-10-02: a snippet sent from A to B, on to C and to
  D, with C deleted for good, was asked to go back from B to A while it
  was on D. Running the random sessions below past their usual seeds
  found it.

With those, the one thing a step checks is that it is being applied to
the state it was made against, and it asks that of every change before
applying any (`history::CanApply`), so that a step is never applied
halfway. A step that fails it is a bug in the rules: dropped, and an
assert in a debug build. `HistoryTest.EveryStepAppliesWhateverHappensInBetween`
drives a session through random commands - gestures left open, pastes
and sends between canvases, canvases and folders deleted, restored and
deleted for good, undos and redos - and checks after every one that each
undo change is on its snippet's canvas, and every few that undoing
everything on the current canvas and redoing it gives back exactly the
library it started from. It ran clean over three thousand seeds before
being cut to the hundred and twenty that run every time.

That round trip cannot see an undo that goes wrong in a way its redo
carries back out, and the group move undone out of turn was one: redone
from the wrong order, it ended where it started all the same.
`HistoryTest.EveryUndoGivesBackTheSnippetsAsTheyWereBeforeItsStep` checks
each undo against the snippets as they were before its step, which takes
an undo with one right answer: only commands the history takes back,
every gesture ended as it is made, and every undo the newest step on any
canvas. It found the swap in 3 of 200 seeds, and nothing else; 40 run
every time, the swap among them.

A paste undone while the canvas it came from is deleted - not for good -
sends the snippet back into it all the same, where restoring the canvas
finds it, and the toast says so. The alternative, refusing, left a step
on the stack that could not be taken, and had to be dropped by hand.

What is not on the history is what is not in a snippet: pins and
minimizing, the stacking order (raising a snippet on every press would
file a step per click), renames, reorders and switches, and folders and
canvases, which are deleted with a confirmation and restored from Show
deleted. Either confirmation - for a delete that can be restored, and for
one that is for good - can be switched off in Settings > Behavior
(`AppConfig::confirmDelete`, `confirmDeleteForGood`), for everything at
once rather than per profile, since what a delete asks is about the
library. A delete that does not ask is an action, done once the frame is
drawn (see "The overlay UI"), since a button that deleted on the spot
would change the Overview while it is still being drawn from it. None of
these changes anything a step reads, so none can make one stale.
`Session::Delete` and `Restore` refuse a snippet for the same reason: its
mark is the history's.

**One gesture at a time.** A drag, a slider, the color picker, a note
being typed, the eraser and a shape are gestures: previewed in the model
through the session, and filed as one step when they end. Every other
command ends the one open first (`Session::EndOpenGesture`), so that no
step is ever filed in the middle of another's changes, where undoing it
would undo part of them. An undo pressed mid-gesture takes back the
gesture. A note's text is the note's as it is typed (`PreviewText`), so
whatever ends its edit keeps what was typed; the editor sees its edit
ended from elsewhere and closes.

A placement is the whole of where a snippet is - its rect, its
fullscreen state and the anchor its rect is recomputed from on a display
change - because a drag takes a fullscreen snippet out of fullscreen,
and an undo that brought back the rect but not the fullscreen would not
be the snippet as it was. A style is everything the properties popover
and the opacity wheel change. Steps that come in bursts - wheel notches,
arrow-key nudges, opacity steps - are one step: the burst holds a
placement or a style edit open on the session while it goes on, and it
is filed when the burst ends (see "Bursts"). A spin of the wheel is
taken back in one step, to where it started.

### Making a snippet is on the history, and an empty one stays

Snippets are made through `Session::CreateItem`, so a snippet made by
mistake - a screenshot, a drawing - can be undone into its deletion mark
and redone out of it.

An empty drawing stays, as any snippet does, until it is deleted or its
making undone. It used to go: a drawing a press on empty canvas made was
watched, and erased for good once the hand moved on - a press elsewhere,
another canvas, the overlay put away - with nothing put into it. That
kept drawings made by accident from piling up. In use it took drawings
made on purpose, ready ahead of the moment they were for: in a game, the
drawing was gone by the time it was wanted, and a dot drawn into it to
keep it was the workaround. Making one takes the drawing trigger and a
drag, a double-click or a hold, and one made by accident is an undo or a
Delete away, while one lost was not noticed until it was needed. The
watch also had corners of its own: a move or a burst canceled with
Escape had already counted as placing the drawing, and kept it where a
click elsewhere would not have.

### Freezing the screen

`FreezeScreen` captures the whole display through the same call a
snippet's capture uses, which leaves the overlay out of it, so the
picture is the application underneath and none of our content. While a
screen is frozen a region capture is cropped out of it rather than taken
live; without that the user drags a region over a still picture and gets
back whatever the game showed a moment later, which for a moving camera
is a different scene. The pixels are kept for exactly this, and its
texture is made from them - again, after a lost device. Both go on hide:
a full screen's worth, with no reason to exist while nothing is shown.

## Visual theme

The look is a graphite scale with a teal accent, translucent panel
backgrounds with a thin border in place of real backdrop blur (ImGui has
no blur, and a multi-pass blur target does not compose with a single
full-viewport swapchain), pill-shaped containers, the accent for every
active state with dark ink on top of it, a bundled UI font, and real
vector icons on every button.

### Interface scale

The process is per-monitor DPI aware, so the overlay covers its display
pixel for pixel, and nothing Windows does scales what it draws. The app
scales its interface itself: by Windows' scale for the display the window
is on (`IOverlayWindow::ScalePercent`, asked every frame, so a change
made in Windows while the overlay is up lands on the next frame), or by
`AppConfig::uiScalePercent` when that is set.

It is one number for the whole frame, `UiScale()`, set at the start of
`OnFrame` and nowhere else. Two things carry it:

- **The ImGui style.** `theme::ApplyStyle` builds the style from a
  fresh `ImGuiStyle` on every change and then calls `ScaleAllSizes`, and
  sets `FontScaleDpi`, which ImGui 1.92 multiplies into every font size.
  From a fresh one because `ScaleAllSizes` multiplies what is there:
  scaling a scaled style would compound. The font is rasterized at the
  size it is drawn at, so 150% text is as sharp as 100% text, not
  magnified.
- **`Px`.** Every size the UI code writes itself - a button's size, a
  bar's padding, a column's x - is written in pixels at 100% and wrapped
  in `Px`, where it is drawn and where it is hit-tested alike, so what is
  drawn larger is also where clicks land. A named constant stays at 100%
  and is wrapped at its use rather than scaled at its definition, which
  keeps it `constexpr` and makes an unwrapped one easy to grep for. Line
  widths that have to stay on the pixel grid go through `PxWhole`.

What is not interface is not scaled: snippets, strokes, the eraser and
brush sizes, and a note's text size, which is content with a size of its
own (the note editor divides the scale back out of the font size it
pushes, since ImGui would otherwise apply it). Drag thresholds
stay in pixels too: they are about how far a hand moves, not about how
large anything looks.

The pointers follow Windows' scale rather than the setting, because they
stand in for the system pointer, which Windows draws larger on a scaled
display whatever this app says: the software pointer scales its
outlines, and the OS pen cursor is rasterized once per scale.

### Icons: vector shapes, not an icon font

The conventional ImGui way to get icons is an icon font merged into the
atlas and drawn as text. This app instead compiles `assets/icons/*.svg`
with `scripts/gen_icons.py` into `src/ui/icons_generated.h`, a table of
drawing commands (move, line, cubic, circular arc, circle, rounded rect
in a fixed 24x24 space) that `DrawIcon` replays against an `ImDrawList`
every frame, scaled to whatever size the icon is drawn at. Nothing is
baked at one size; ImGui tessellates the curves fresh like any other
shape. The script needs `svgpathtools` and is a dev-machine tool, not a
build dependency: the generated header is committed, so building needs no
Python.

**A point may not appear twice in a row in a path.** ImGui's stroker
offsets each point along the average of its two adjacent normals and
divides by that average's squared length, the standard mitre. A
zero-length segment has no direction, so its normal is zero, averaging
halves the vector, and the mitre reads a half-length vector as a very
sharp corner and doubles the stroke: a lump twice the line's width,
sticking out where the path does not turn. Icons produce such points
honestly - `DrawIcon` emits the subpath's start and `PathArcTo` emits the
arc's start at the same place - so `DropRepeatedPathPoints` runs over
every subpath before it is stroked. `icon_draw_test` asserts on the
triangles ImGui actually produces: no vertex may land outside its icon's
own outline by more than the stroke plus a mitre's worth, which is how
seven affected icons were found.

Icon choices are hand-picked per button rather than a one-to-one Lucide
mapping; the set is styled after Lucide and Feather, drawn from this
project's own path data, and both licenses cover the designs.

### Every word in one file

All user-facing text lives in `assets/ui_strings.json` and reaches the
code as `sz::strings::k...` constants. `cmake/UiStrings.cmake` compiles it
into `generated/ui_strings.h` at configure time, parsed by CMake's own
`string(JSON)` so that editing what the app says needs no Python and no
remembered step. Keys are dotted paths that become PascalCase
identifiers; a key starting with `_` is a note to the reader, JSON having
no comments; two keys that would mangle to one identifier are a
configure-time error. Values become raw string literals, so nothing has
to be escaped. A handful of strings carry `%s` placeholders and the file
says so; that is the one edit that can crash rather than read oddly.

The other half, and the reason it was worth doing: **text is not an
identifier.** A widget's ImGui id must not be its label, or rewording a
checkbox silently renames it and breaks every test that reaches for it.
`Labeled(text, id)` returns `"text###id"`, and the row-building helpers
all take an id of their own. Two traps: `##` hides the id but hashes the
whole string, only `###` restarts the hash; and a widget that pushes its
own label as an id scope (`ColorEdit3/4`) needs an id-only label with the
caption written beside it by hand.

## The overlay UI

`ui::OverlayApp` is the overlay as it is drawn, with Dear ImGui: a view
of the session, laid out in `docs/VIEW_LAYER.md`. `overlay_app.*` holds
the frame and its stages, the mode, the actions and the routing; every
surface has an owner of its own in `ui/view/`, which holds that surface's
state, draws from what it is handed, and asks for anything beyond its own
through `ViewHost`, which `OverlayApp` implements. No owner knows another,
and `OverlayApp` is the one object that knows them all:

- `CanvasView`: the canvas and items layers, the note editor, the dock and
  the view-only layer, with both mesh caches and the
  previews' pictures, which the Overview and the canvas bar reach through
  `ViewHost::Previews`.
- `CanvasBar`: the bar along the bottom edge, whose tile menu it asks for
  through `ViewHost`.
- `Popups`: the app's six popups, the record of the one that is up, and
  the effect queue.
- `OverviewPanel` and `SettingsPage`: the Overview, and its Settings tab,
  which the Overview draws inside its body through a call `OverlayApp`
  hands it.
- `CheatSheet`, `BehaviorPanel`, `ScreenChrome` (the border, the frame
  graph, the input readout),
  `Messages` (the toast, the persistence warning) and `Pointer` (its
  shape, the software pointer, the drag previews, the size preview, the
  badge).

What every surface shares is in files of its own: the palette, the accent
and the style (`theme.*`), the buttons, the panel backdrop, the screen
layers and the key names (`widgets.*`), how a snippet is painted, on the
canvas or in a preview (`item_painting.*`), and a Settings row's widgets
(`settings_widgets.*`). Anything used by one file stays a file-local
helper. `ui::ContextMenu` (`context_menu.*`) is a widget, not a view of
the session, and depends on nothing but Dear ImGui and the icon tables.

What the hand works on is not the view's: `ui::Editor` (`editor.*`,
`editor_commands.cpp`) holds the selection, the tool in hand and its
shapes, drawing mode, the clipboard, the note being typed, and every
command, and makes snippets and canvases -
with no ImGui in it. It is the Editor of `docs/INTERACTIONS.md`, section
10: the state the interactions work on, which is why it can have no
frame behind it. The view tells it the display size (each frame
and each event) and the time and modifiers of the event being handled,
and it asks the view, through `EditorViews`, for what only a view can
do: a message, a panel or a popup opened. The hit test
(`Editor::ResolvePointerTarget`) is the editor's too, over the same
rects the view paints the bar to, and the resize band is laid out by
(`selection_layout.*`), so what is hit is what is drawn.

Panels - popovers, the canvas bar, the dock, the note editor, the
Overview - are ordinary ImGui windows and widgets. Items and the
selection's furniture are painted into full-screen `NoInputs` layers and
hit-tested by the app itself. Input reaches the editor as the window's
own stream of events (see "Input, in order"); ImGui is fed the same
input by its backend, for its widgets.

A frame is the stages of `docs/VIEW_LAYER.md`, section 5, one function
each, called in order by `OnFrame`: Prepare, the canvas, the effect
queue, the popups, what sits over the canvas, the panels, the messages,
the stack, the pointer, and Apply. Nothing is changed by being drawn:
what the frame's own state calls for is done in Prepare, before anything
is drawn from it, and what a widget asks for in Apply, after. What a
widget asks for - a tile clicked, a drop, a name let go of, a menu's row,
a delete - is a value (`ui/view_action.h`) recorded as it is drawn and
done in Apply, in the order recorded (`OverlayApp::Act`). A widget that
acted in the middle of the draw - a canvas bar tile switching the canvas
halfway through the frame - left everything drawn after it drawn from
the other canvas. A frame draws the library Prepare left, and the change
shows in the next frame; and one act takes one path, wherever it is
asked (a tile in the Overview and on the bar both go through
`Editor::SwitchCanvas`, the bar's "+" is the NewCanvas command). What
stays in the draw is a widget's own value: a setting
(`docs/SETTINGS.md`), a snippet's style or a note's text as the session
previews it, the pen's color while the chooser is dragged. None of these
adds, removes, reorders or switches anything a draw is walking.

The stack is set once a frame, after the draw (`StackSurfaces`): each
surface brought to the front in the order of `docs/VIEW_LAYER.md`,
section 3, and each popup ImGui opened inside one just above it. With a
call beside each window's draw, the last call in a frame won, so the
order was that of the calls, written down nowhere: a Settings dropdown
that opened behind the panel and a color picker that flashed up and
vanished were each fixed by moving a call.

### Making a snippet

Making a snippet is the thing done most often, so it is a press on empty
canvas rather than a tool: dragged past a threshold it is the dragged
rectangle, and a double-click - or a press held still for half a second,
since a finger or a pen cannot double-click reliably - makes it
fullscreen. A plain click makes nothing: a fullscreen snippet is too
much to make by accident. "Empty canvas" means nothing under the pointer
and nothing open that a click outside of is meant to close, or every
dismissal would leave a drawing behind.

Only the left button makes one, and the modifier held as it presses
picks the kind: by default a plain press makes a screenshot and a Ctrl
press a drawing. Which press makes which is a setting - plain, Ctrl, Alt,
or none, for each kind - because which kind someone makes most is theirs
to say. The two never share one: the file reader puts the defaults back
if they do, and Settings swaps them rather than graying out the choice
wanted. Shift is not offered, being the selection box's. A modified press
is otherwise never half of a double-click (Shift-clicking a snippet twice
adds and removes it); the modifier a kind is set to is let through, both
presses needing it, so its fullscreen double-click works like the plain
one's. In drawing mode a press on empty canvas only leaves the mode,
modifier or not: Ctrl also draws a rectangle there, and one begun just
outside the snippet must not make another.

The right button once made drawings the same way. It opens a menu on
empty canvas now - every way to make either kind (the "New" rows pick
up the creation tool, the fullscreen ones make it at once), Paste,
Paste in place and Select all, the Overview and Settings - so that a
kind set to no press at all, or forgotten, is one click away, and the
canvas has the menu a right click everywhere else has taught. It opens on release, like a snippet's; a
right drag there does nothing, and a canvas switch or a capture during
the press drops it rather than releasing it into a menu over a canvas
nobody clicked on.

One made by accident is cheap to take back: making one is on the
history, and undo marks the snippet deleted, where a screenshot taken by
mistake can still be found. An empty drawing is not taken away on its
own (see "Making a snippet is on the history, and an empty one stays").

While one is being made the others fade to a fifth of themselves - with
a creation tool in hand, and while a region is dragged out - so what is
being framed shows through what sits on it. Faint rather than hidden, so
where things are stays in view; not on a press that has not moved yet,
which may still be a click and would only flicker. The capture never saw
the overlay anyway (see Screen capture), so this is only about what the
hand can see. It is done to the finished vertices of the snippets'
layer, because a picture is drawn in its own colors and the style's
alpha would not reach it.

### Tools, and what a modifier does

Six tools, exactly one in hand at a time, so the one lit on the bar is
always what a press will do. Select is the hand at rest. Draw, Erase and
Text are in hand only while snippets are in *drawing mode* - every one
selected when the tool was picked - and a press acts on the one it lands
on. Their shapes are picked from the pen's and the eraser's menus and
stay picked until another is; a modifier held as the press starts
(Shift for a line, Ctrl for a rectangle, Ctrl with the eraser for a
rectangle eraser) draws one for that drag only, read once on the press
so letting go mid-drag changes nothing. Drawing and Screenshot place a new snippet with the
next press, anywhere, then hand over: a drawing to Draw, a screenshot to
the tool that was in hand before.

A ring menu of favorite tool slots came before (see "Dead ends"): with
the selection bar carrying every action on a snippet and the canvas bar
reaching the Overview, it added a gesture to learn and nothing to reach.

### Drawing is a mode, entered on snippets

At rest a snippet is an object that a click selects and a drag moves.
Double-clicking it, holding a press still on it, or pressing a drawing
tool on its bar enters drawing mode: a stronger outline, the tool lit on
the bar, the pen in hand (or the tool pressed), a press on it draws, a
right-drag on it erases whatever tool is in hand, and a click anywhere
else, a right click on the snippet, Escape or the lit tool pressed again
leaves. A tool pressed on the bar over several selected snippets, or
picked by key, enters the mode on every one of them, and a press on any
draws: the bar is over the whole selection, so what is pressed there is
for all of it. A mark is on every snippet in drawing mode it reaches -
a stroke, a line or a rectangle kept whole by each its ink reaches, as
one undo step, and the eraser erasing on all of them - since each draws
the stroke in progress, and a stroke kept by the first alone vanished
from the rest as it was let go (`docs/INTERACTIONS.md`, 6.5;
`Session::CommitLiveStroke`). Holding Alt picks the snippet up instead
of drawing on it.

A right click or a hold on the Pen or the Eraser lists its shapes, the
current one marked and the modifier that draws each beside it, so a hand
with no keyboard can draw a line with a plain drag. The shape picked is
kept until another is - through the other tool in hand, and drawing mode
left and entered again - and the button wears its icon throughout. It
used to go back to the plain pen and eraser at either, which had a hand
drawing boxes pick Rectangle again after every correction with the
eraser. It is kept for the session only, like the tool in hand: it is
how the hand is drawing now, not a setting.

There is one bar, whatever the mode: the drawing tools, a divider, then
what is done to the snippet (Pin, More, Minimize, Fullscreen, Close).
There were two, and drawing mode swapped one for the other. That hid the
way in - a double-click, or a hold, which nothing on the screen shows -
and moved every button a hand had learned the place of each time the
mode changed. With both groups always there, a drawing tool is visibly a
way in, the lit tool is visibly the mode, and Pin or More is one click
away while drawing. The lit tool pressed again puts it down, as its key
always did; before, the click cycled the tool's shapes, which the menu
above now offers directly, so the click was free to mean what a toggle
means. Drawing mode itself is unchanged: it is still what lets a plain
press on a snippet mean select-and-move (below), and its keys - Delete,
the arrows - stay claimed while it is on. See `docs/INTERACTIONS.md`,
section 6.5.

The mode exists so a plain press on a snippet can mean one thing: with
drawing the default, every click on a snippet made a mark, and with a
Select tool the hand had to be switched to move anything and switched
back to draw.

Drawing mode is an interaction on the machine's Mode level
(`DrawingMode`), and so is a creation tool in hand (`CreationTool`); the
tool in hand is read from there (`Editor::ActiveTool`), Select when the
level is empty, rather than kept beside it. Each answers Escape - drawing
mode is left, the tool put down - so Escape's stages fall out of it
passing down the stack: a gesture canceled, then a popup, a note or a
panel closed, then drawing mode or the creation tool, and only then, at
the Canvas level, a cut called off and the selection cleared. Drawing mode also claims Delete and the
arrow keys, which act on a snippet from outside and not on the one being
worked in.

### Nothing over the canvas is hit-tested by ImGui

This is the one rule the selection rests on. An item's chrome was once an
ImGui window full of `InvisibleButton`s (before that, one window per
handle), and every bug in the family - "the pointer flickers", "I can
grab the handles of the window behind", "my stroke didn't start" - was
the same bug: two hit-testers on two clocks. ImGui resolves the hovered
window inside `NewFrame` against each window's rect as it stood at the
end of the *previous* frame, with no idea that item B's body should
occlude item A's chrome. At the pixels where one item's chrome lay under
a fronter item's body the two disagreed for one frame, and everything
that consumed ImGui's answer inherited it. Each was fixed with another
correction until the chrome was a model fighting its framework.

So there is one resolver, `ResolvePointerTarget`: a walk over the current
canvas, pure in position and model, returning the frontmost thing that
would take a press there (a bar button, a selected snippet's resize
band, an item's body, or
nothing) and the frontmost item whose content holds the point. Furniture
first, in the order it is painted, then items front to back. Drawing and
resolving share one definition of the geometry, so what is drawn and
what is hit cannot differ. Panels that are always above every item stay
ImGui's.

### One gesture engine on the raw pipeline

What a press on the canvas means is decided in one place,
`RecognizePress` (`ui/interaction/recognizer.*`): the rules of
`docs/INTERACTIONS.md`, section 6.5, tried in order, the first that
matches deciding what the press does at once and which interaction it
starts on the Gesture level of the machine (see "Input, in order"),
rather than in several handlers whose answers depended on the order
they were asked in. A press over one of ImGui's windows is the
window's (rule 1, `io.WantCaptureMouse`); a gesture in flight takes its
moves and its release wherever they land, so straying over a panel
mid-drag cannot hand the event to it. A move or resize is a snapshot
plus the *full* delta from the press, recomputed on every move - not an
incremental delta, which drifts under event coalescing, and not ImGui's
drag delta, which loses the grab offset against a screen edge.

A press whose meaning is still open is a `Pending` interaction, and the
rest of a press whose gesture has ended is `Spent`
(`docs/INTERACTIONS.md`, sections 4 and 6): the machine puts one there
whenever the Gesture level is left empty with a button held, so "the
rest of the drag does nothing" is a state, not the consequence of fields
being empty. A pending press does at once only what every meaning
shares - the first click on a snippet selects it - so no click waits the
double-click time for its answer. The gesture's outcomes that make or
open something - a snippet framed, drawing mode entered, a menu opened -
are commands of their own in the table (`FrameSnippet`, `DrawingMode`,
`ItemMenu`, ...), run after the gesture is off the stack, so none of
them changes the stack under the machine's routing.

A press is a click until the pointer has traveled 4px (6px for framing
a snippet, and for a hold to still be a hold). A resize started on one
of several selected snippets scales all of them about the fixed corner;
the smallest is the floor for the group. Shift-drag on open canvas draws
a box that adds every snippet it touches to the selection.

What the pointer is doing is one interaction on the Gesture level -
`Pending`, `Spent`, `Placement` (a move or a resize), `BarPress`,
`BoxSelect`, `Framing`, `Marking` (a stroke, a shape, the eraser's path
or its rectangle, and the right button's erase) or `Widget` (a press on
one of ImGui's windows, held: a slider, a tile dragged) - or nothing
(`ui/interaction/gestures.*`). As a field per kind, the gestures
excluded each other only by the order a handler asked in, and each place
that forgot one was a bug: a drag moving a snippet a Delete had hidden,
a stroke outliving the Escape that left its drawing mode. Each gesture
answers every event kind in one switch (`Gesture::Offer`), so there is
no event a gesture has no answer for. How one is ended early is the next
section's.

The selection bar floats over the selection's bounding box, or below it
when there is no room, or inside its top edge for a fullscreen snippet.
Its buttons fire on release over the same button, ImGui's own rule. The
Properties popover opens through the effect queue rather than
`ImGui::OpenPopup` from the release: the input stream runs during the
message pump, before that frame's `NewFrame`, where `OpenPopup` has no
current window and dereferences an empty id stack.

A right click on a snippet opens its context menu. Right-drag already
resized from the nearest edge, so the two are told apart by the one thing
the gesture engine was already tracking: whether the press traveled its
4px. On the snippet being drawn on the right button belongs to the
eraser, and the click that gets past it is Alt's, which leaves drawing
mode and opens nothing.

The menu itself knows nothing about the app - rows in, the chosen row's
action out, every color read from the current ImGui style rather than
the palette - so the dock's canvases and empty canvas can have their
own, and anything after them, without it growing a second personality. Each row carries
the shortcut of the action it runs, read from the live binding, which
makes the menu the place the keys are learned as well as pressed; a row
whose action has no binding shows nothing rather than the key editor's
"(none)". Four rows act on the selection and not only the snippet -
Copy, Cut, Duplicate and Move to new canvas - because the shortcut
printed beside each is the selection's, and a row that names Ctrl+D has
to do what Ctrl+D does. Paste and Paste in place are there too, as on
empty canvas's menu: they once were not, having nothing to do with the
snippet, but Paste puts things where the right click was, and over a
snippet is as good a place for them as beside it - a hand should not
have to find a gap first. A row that cannot be chosen right now is grayed rather than
dropped, so the menu is the same shape over every snippet and a hand can
learn where a row is. While it is up ImGui claims the mouse, so the press
that dismisses it does nothing else - which is what a context menu does
everywhere, and is why a right-drag after a right-click resizes nothing.

It opens at the point it was asked for, with the anchoring corner flipped
where the menu would run off the right or the bottom, so a menu opened in
a corner does not cover what it was opened on. Then it is held on the
screen: at a large interface scale the snippet menu is taller than the
room either side of a point halfway down, and flipping alone moved it
off the top instead of the bottom.

The menu is the one place a snippet's actions live. The Properties
popover (the bar's More button) holds what describes a snippet rather
than what is done to it - the two opacities, the background color and the text's
size and color. Its colors are a picker each, with no preset swatches
beside them: the picker does the whole job, and a row of presets was a
second way to do part of it. The background keeps one swatch, white,
because white is the one color with a meaning there - the no-op tint
that gives a capture back as it was - and hitting it exactly in a picker
takes aim.

Which buttons each of the bar's two groups carries is a setting - two
lists, `bars.drawing` and `bars.snippet`, kept from when they were two
bars, which is also why no config version was needed for one - and
anything the file gets wrong is made sense of rather than obeyed: a name
from the other group is dropped, a duplicate kept once, and a button the file never mentioned is
appended shown, since a new button arriving invisible is a feature that
silently isn't there.

### The hand

Something often wants to act while the hand is in the middle of
something: a key, a global hotkey, a menu row, the overlay going away.
There are two answers - end what the hand is doing and act, or leave the
hand be and refuse the command - and the app takes the first, for
everything but the pointer's own device.

- Refusing depends on knowing a gesture is in flight, and that is the
  one thing that goes stale: a release lost to a focus change or a hide
  leaves a gesture that never ends. Refused for it, the hotkey that hides
  the overlay would be refused for good. Settling instead cures a stale
  gesture as a side effect of any command.
- Some commands cannot be refused anyway - the session ending, the
  window closing - so settling has to exist and be right regardless; a
  second, refusing, policy would be a second set of cases.
- A hotkey is deliberate. Pressed mid-stroke it usually means "the game,
  now", and a press that does nothing is pressed again, and the second
  one is the surprise.
- Putting the command off until the release is worse than either: the
  release may never come, and a queued command fires at a moment nobody
  chose.

So every command ends what its scope covers first (`Machine::EndFor`),
the Gesture level among it: the gesture there is *interrupted* - what is
done is kept and filed, a stroke committed as one undo step, a move or
resize filed where it got to, and nothing a release would newly do
happens, so no snippet being framed is made, no held bar button fires, no
right click opens a menu, and a rectangle erase erases nothing. A note
being typed is committed. The rest of the held button's press is Spent,
and its release ends that. An undo pressed mid-stroke therefore takes
back the stroke so far, the most recent thing done. Ending a gesture as
a release would have - which is how it was once done - makes a region
being framed and fires a held bar button: things nobody asked for, over
a canvas they had not clicked on.

What ends is said in one place, the scope (`Editor::Settle`), for the
commands and for the moments no command follows. The overlay put away -
hidden, restarted - ends what a command's Hand scope does, and drawing
mode, a panel and a popup are still up at the next showing; the overlay
coming up ends it again, for whatever went some other way, and forgets
which buttons were down (`Machine::Forget`); view-only mode, and the app
exiting, end everything above the canvas. All of these go through one
sequence, `OverlayApp::Settle`: the scope, then what only a frame of
edit mode would otherwise keep, a slider's preview and the pen.

Escape is not such a command while a gesture is in flight: the gesture
sees it first and is *canceled*, leaving things as the press found them
(`docs/INTERACTIONS.md`, section 5, has every gesture's answer). The
session rolls the gesture back to the checkpoint it took when it began
(`Session::CancelPlacement`, `CancelErase`, `CancelShape`,
`CancelStyleEdit`), which is exact and costs no write, since previews
change the model in memory and nothing is written until a step is filed.
A canceled Widget gesture - a slider, a tile dragged - also has the view
let go of ImGui's active widget, an effect, since only a frame may touch
it.

The pointer's own device is the exception: input from the mouse holding
the gesture is ignored until it ends rather than ending it - a second
button, since a touch screen's press-and-hold injects one into a finger
held still; the wheel and a mouse button bound to a command, since a
notch mid-stroke would switch the canvas under it. The gesture's own
button pressed again means its release went missing: the gesture ends
as interrupted, and the press is taken afresh. An arrow key is a command
like any other: mid-drag it ends the drag where it is and nudges after
it, two undo steps.

### Bursts

A key held down repeats, and a wheel spun turns several notches: steps
that come one after another, which a person means as one - one undo,
back to where they began. Each is an interaction on the Gesture level
(`ui/interaction/bursts.*`): an arrow begins a `NudgeBurst`, and the
wheel over a selection, outside drawing mode, a `WheelBurst` of its
kind - the selection's size, or its opacity. The burst holds a placement
or a style edit open on the session, so every step is a preview, and
files it as one step when a second passes without another. Anything
else ends it first, filed: a press, a command (its scope ends the Gesture
level), a notch of another kind. Escape takes it back to where it began,
through the same cancel a drag has - for the arrows only while one is
held, since a run of presses already let go of is not what Escape is
pressed about; with none held, Escape ends the burst and goes on to put
the hand down. So does a burst that holds nothing open, its steps having
had nothing to change: Escape is not spent on it.

The arrows stay commands. A burst runs each as its step
(`Editor::Step`), which ends nothing first - the burst is the hand - and
has the nudge preview into the burst's placement rather than file a step
of its own (`Editor::Filing`). The wheel's size and opacity are not
commands, and only a burst reaches them.

As an interaction, what comes in between ends the burst, and nothing is
filed until it is over; merged into the step on top after the fact, as
bursts once were, a step could be neither kept apart from a drag in
between nor canceled (see "Dead ends"). The second stays, as the end of
an interaction the wheel has no other end for: a run of arrow presses
within it is one undo.

All of this follows `docs/INTERACTIONS.md`, the reference for how input
behaves: every input through one state machine - a stack of
interactions, each offered every event first - in which every pair of
state and event has a written answer. That
structure is a decision in its own right. A new input behavior is added
through its tables, commands and interactions; a change to the structure
itself needs a well-founded reason, argued there first.

`HeadlessAppTest.EveryCommandSettlesTheHandWhateverItInterrupts` holds
this: random presses with either button, moves, releases (a quarter of
them lost), modifiers, the wheel and holds, with drawing mode entered and
strokes left in flight, interrupted by every command in the table (next
section) - by its key, its hotkey, or dispatched as its menu row or bar
button would. After each command that ran (`OverlayApp::CommandsRun`
says which did), nothing is in flight in the app - at most the rest of a
press, Spent - or open on the session, and a stroke it interrupted is on
its snippet (or, after an undo, taken back). It counts the strokes it checked that way, and fails for a command
that never ran, so neither check can quietly stop running.

`InteractionRandomTest.AnythingAnywhereEscapeIncluded`
(`tests/ui/interaction_cases_test.cpp`) runs the same kind of input
against the machine and `Editor` alone - no `OverlayApp`, no ImGui
frame - which makes it fast enough for 2000 seeds of 150 events, and
adds Escape anywhere, the wheel, and keys held as well as let go of.
Whenever Escape cancels a move, a resize, a mark or a burst, every
snippet is exactly as it found them and the live layer is empty; it
counts those checks too, the bursts' on their own. A gesture that took over
from another in the same event (a press that ended a stroke and began
the next) is not checked that way: it found the library with the
other's work in it, which the test has no snapshot of. Beside it, each
row of the cases table in `docs/INTERACTIONS.md` is a scripted test in
the same file.

### Commands

Everything the app can be told to do in one step is a `Command`, and all
of them are in one table, `ui/interaction/command.h`: its id, its scope
(what it ends first), and what reaches it - keys of its own that nobody
rebinds (Escape, Delete, the arrows), the key a person chooses in
Settings (by the `ShortcutAction` name config stores it under), or a
global hotkey. The table is checked at compile time to hold
one row per id, in order. No ImGui in it: what a command is and what
reaches it are the app's words, not its widgets'.

Every way in ends at `Editor::Dispatch`: the Canvas level of the input
machine for the keys and the mouse buttons a shortcut may be (the command
a key runs is `Editor::CommandForKey`'s answer), the three context menus (each row names its command), the selection
bar (`CommandForBarButton`), and the tray, whose hotkeys and "Show" menu
entry dispatch through the overlay and are handed back to it to run
(`SetAppCommandCallback`) - the tray alone knows the window and the modes,
but the hand is the overlay's to settle. A hotkey reaches it as an event
of the input machine (`OverlayApp::OnHotkey`), which every level passes
on to the command - hidden or not, with no frame needed. `Dispatch` asks `Available`,
ends what the command's scope covers, and runs it - unless what it ended
could not be written (see "Every command is written as it is made"). So settling
first is not something each command has to remember: nothing runs
a command any other way, and `Run` is one exhaustive switch.

`Available` is the model's answer - Delete with nothing selected does
nothing, Paste with nothing on the clipboard does nothing - and the same
answer grays a menu row out, so a row and its key cannot disagree about
whether something would happen. It is asked before settling, so it never
depends on what settling would file: undo is always available, because a
stroke in flight is on the history only once it has been settled. A
command that is not available does nothing at all, including not
settling: a key that does nothing is no command.

Whether a key *reaches* its command from where it is pressed is a
separate question, and the stack's answer: a key reaches the Canvas level
only if every level above passed it. A note being typed, a name being
edited or a row waiting for a key claims every key; a popup every key
but the global hotkeys, so no undo, tool or clipboard key changes the
canvas under a menu left up over it; a panel every key but the global
hotkeys and its own, so the cheat sheet's key closes the cheat sheet.
Delete and the arrows do nothing in drawing
mode, where the snippet is being worked in rather than on - the Mode
level's to say, once drawing mode is on the stack.

A key belongs to one command: the first row it is bound to, which puts
the fixed keys ahead of chosen ones, and the table's order ahead of a
profile that bound one key twice. The fixed keys match as `HeldWith`
says: Escape and the arrows with any modifiers (a nudge reads Shift
itself, for ten pixels), Delete and Backspace bare or with Shift
(docs/INTERACTIONS.md, section 7). The cheat sheet's
rows and the menus' key labels are read from the same table, so they
show what is bound, not what a string says is; and a menu row runs the
very command its key does, so the two cannot differ ("New screenshot"
with the tool in hand puts it down, as `S` does).

`KeyCombo` grew a range of named keys for the fixed bindings (Escape,
Delete, Backspace, the arrows), outside what a global hotkey or a key
editor accepts - Escape, Backspace and Delete are what unbind a row
there.

Undo and redo are chosen keys, Ctrl+Z and Ctrl+Y as shipped, since
2026-10-06. They were fixed keys, and from 2026-10-04 no row or hotkey
could be given theirs - a row given one never ran, as undo matched
first. That made them the one exception among keys a person might want
elsewhere, while Copy, Cut and Paste, as common, could be moved. Now
they all follow one rule: a key given to another row is taken from the
row that had it. Redo has one key, as every row does; Ctrl+Shift+Z,
its other usual key, is free to choose for it.

A text field is the other side of that rule. ImGui's `InputText` reads
Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z, Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+A itself,
with no way to rebind them, and a note being typed or a name being
edited claims every key (`TypingNote`, `NameEdit`), so no chosen key
reaches the canvas from there. Handing a field the chosen keys instead
would mean rewriting key events before ImGui sees them, and a key that
types a letter could not be one there anyway. The fields keep the keys
every program has, whatever is chosen; the Shortcuts help says so.

### Input, in order

The window hands the app one stream of input
(`IOverlayWindow::SetInputCallback`): presses, moves and releases of all
five buttons, the wheel, keys and modifier changes, each with its time
and the modifiers held, in the order they happened - section 3 of
`docs/INTERACTIONS.md` - and each is handled as it arrives. Read from
ImGui at the next frame instead, as keys once were, a key pressed
between two moves of a drag is seen after both.

- The modifiers are the key state or'd with the input grab's record,
  the two sources the frame already gave ImGui (see RenderFrame), and a
  change no key message carried - no focus, no keyboard grab - is told
  once a frame. The handlers read the stream's (`Editor::Held`), not
  ImGui's, which are last frame's.
- A key's repeat is the window's to count, since the grab posts every
  repeat as a fresh press. Undo and the arrows repeat at the system's
  keyboard rate.
- A hidden window hands on nothing. Messages still arrive: the grab
  passes on the key of the hotkey that hid the overlay, after the hide.
- Moves under the grab are still sampled once a frame, never one per
  mouse report - what countering needs (see "Taking input back from the
  game" and `docs/INTERACTIONS.md`, "Phase 2 and the input grab").
- Once a frame, after that sample, the window says the time: a `Tick`,
  on the stream's own clock, which is what a press held still is judged
  by. ImGui's clock is another one, and a hold measured from an event's
  time to ImGui's would be measuring between two clocks.

Every event goes through the machine of `docs/INTERACTIONS.md`, section
4 (`ui/interaction/machine.*`): a stack of interactions, one per level,
offered each event from the top down, each answering Claim, Pass,
Finish, Cancel or Start. The machine is the editor's (`Editor::Input`),
since the interactions work on the editor; nothing in it knows ImGui.
Its routing is tested alone, with interactions that do nothing but
answer (`tests/ui/machine_test.cpp`). A command's scope is ended through
it before the command runs (`Machine::EndFor`). The Canvas level
(`CanvasLevel`, `ui/interaction/canvas.*`) hands a press to the
recognizer (see "One gesture engine"), a key or a bound mouse button to
its command, and the wheel to `Editor::Wheel` - an arrow, and the
wheel on the selection, beginning a burst (below). The whole machine - every level, the
recognizer, the commands - is the editor's, with nothing of the view in
it: what it asks of ImGui goes through `EditorViews`, which a test of the
machine alone leaves out. The machine also keeps which buttons are down, from
the presses and releases it is offered: that is what leaves the rest of
a press Spent, and what the overlay coming up forgets
(`Machine::Forget`).

What only a frame can do - opening a popup, closing the top one on
Escape - is queued as an effect (`Popups::Effect`) and done in the
next frame, just before the popups are drawn, where every popup's id is
hashed at the top level. Done at the very start of the frame instead,
the canvas bar's menu was closed again before it was drawn. Of two
popups asked for before a frame, the one asked for last comes up.

Every popup the app opens is an interaction on the machine's Popup
level (`Popup`, `ui/interaction/levels.*`), put there as it is asked for
(`Popups::Open`): the machine's record of what ImGui draws. It finishes
on the first frame's tick that finds the view no longer showing it,
since ImGui closes a popup by itself - a row chosen, a click outside -
and the machine has to follow. Ended from outside - another popup
opened, a command whose scope covers popups - it asks the view to close
it (`Effect::Kind::ClosePopup`), queued before the new one opens so that
the one asked for last is up. Escape closes the innermost popup open,
which may be one of ImGui's own inside it (a color picker in the
Properties popover), and the popup finishes when it is itself gone.

The view keeps one record of the popup that is up
(`Popups::PopupRecord`, `ui/view/popups.*`): its kind, what it is about, where it opens,
and whether a frame has drawn it - set as it is asked for, let go of as
it closes (`docs/VIEW_LAYER.md`, section 4). The machine allows one popup
at a time, so one record is enough. What closing a popup does
(`Popups::Closed`) is done once, however it closes: by the draw that
finds ImGui without a popup the record says was drawn, or at once when
the machine ends it from outside. Done only by a draw noticing, a popup
ended while nothing drew it closed late or never - the color chooser,
put away and the app then exited from the tray, lost the color picked
in it. The pen's width and color are also kept when the overlay settles
(`KeepPen`), as a settings preview is committed, since the wheel's size
preview keeps the width only once it has faded, drawn.

The Text level holds a note being typed (`TypingNote`), a name being
edited in the Overview (`NameEdit`) and a Settings row waiting for a key
(`KeyCapture`); the Panel level the Overview and the cheat sheet
(`Panel`). A press outside a note being typed is passed on, with the
note still open - which is what keeps that press from making a snippet.
A row takes its key from the input stream rather than from ImGui, and a
global hotkey that fires while a hotkey's row waits is the key it takes.
A row stops waiting when it goes out of sight - its Settings section
left, the Overview's tab, or the profile it edits - as it does when the
Overview closes: left waiting, Escape pressed to close the Overview went
to the row no one could see and unbound its shortcut, and a letter bound
that letter. Found in review on 2026-09-27.

### Item text

A caption is a plain string on any item, not a separate note kind - a
dedicated text-only kind could not combine with a drawing or a
screenshot, and once text stopped being exclusive the flag that gated it
had nothing left to do. Color and size are per item, since a caption
over a dark screenshot and one over a pale drawing want different
answers. Size is in screen pixels at the item's current size, not scaled
with the item like strokes: a caption that shrinks to illegibility is
worse than one that wraps sooner. A new snippet takes its text style from
Settings > Defaults. Text is never erased by either eraser.

Editing is a second, genuinely interactive window over the content rect,
not a flag on the items layer, which is unconditionally `NoInputs`. The
editor's buffer is a `std::string` the widget grows through ImGui's
resize callback, not a fixed array: a note is whatever its record says
it is, and the 8 KB array it once was silently cut a longer note off the
moment it was opened for editing.
Escape ends editing without discarding what was typed: ImGui reverts its
own buffer on Escape in the same call that reports deactivation, so the
commit reads a snapshot taken before the widget ran. "Stop editing" and
"undo my typing" are different actions, and the second is Ctrl+Z.

### The canvas bar and the wheel

The canvas bar along the bottom edge shows the canvases of the folder
being worked in, with the current one outlined and buttons for a new
canvas and the Overview. It hides below the edge and slides out when the
pointer reaches it, for a moment when the canvas changes, and for a
moment when the overlay comes up; it stays in while a gesture is in
flight, so a stroke run into the bottom of the screen cannot pull a bar
out from under the pointer. A press on the bar itself is not such a
gesture: counting it had the bar slide away half a second into a tile
being dragged to a new place. Found in review on 2026-09-27; the
test-engine tests of the bar never went through the input machine,
where that press - a `Widget` - is pushed. The first fix left out every
`Widget`, and so a press on any other window - the color chooser's
square, a slider in the Properties popover - dragged into the bottom
edge slid the bar out around it. Found in the next review on
2026-09-27: the bar now leaves out only a `Widget` whose press went down
on it (`Editor::WidgetPressedAt`).

A tile's own context menu is a right click on it, which does not also
switch to that canvas: a menu is opened to act on something, not to go
to it. It opens on the release, over the tile the press went down on,
as every other context menu does; until 2026-10-03 it opened on the
press, the one menu that did. The bar is held out for as long as the menu is up, since the
pointer has left the bar for the menu and a menu hanging over a panel
that slid away would be a puzzle. Delete goes through the same
confirmation the Overview's delete button asks for - a canvas takes
every snippet on it along, and unlike a snippet's own delete there is no
undo entry to take it back with.

A tile dragged onto another takes that one's place, the others shifting
along, which is the order Alt+wheel walks. The move is made by
`ReorderCanvas`, which counts places among all of a folder's canvases,
deleted ones included; the bar leaves the deleted ones out, so each
tile's place is looked up in the whole folder rather than read off its
position on the bar - otherwise a deleted canvas between two tiles would
make a drop land one short.

Without a modifier the wheel sets a size, and the mode says which: in
drawing mode the size of Draw or Erase, and outside it the selection's,
scaled as a group about its middle the way a corner of its band scales it,
kept between the smallest snippet's floor and the screen. The mode
decides rather than whether anything is selected, because in drawing
mode something always is - the snippet under the pen, which a size
meant for the brush must not start scaling. Ctrl and Shift with the
wheel set the selection's background and foreground opacity, in either
mode, five percent a notch within the Properties sliders' ranges, and a
toast says the value reached.

With Alt the wheel steps through the canvases of the folder the
*current canvas* lives in (not the browsed folder), without wrapping.
None of it acts while a gesture is in flight - see "The hand". All of these honor how far the wheel actually turned,
keeping a remainder across frames, so a fast spin is not truncated to
one step and a precision touchpad's fractions are not rounded to
nothing. For the brush, a transient size preview at the cursor is the
feedback; a permanent brush cursor is what made an earlier design feel
busy over a game.

### The Overview

A translucent backdrop and a centered panel, above every surface of the
canvas and below the cheat sheet, the delete confirmation and the
messages (`docs/VIEW_LAYER.md`, section 3). Tabs: Canvases (a folder
sidebar and a tile grid with live thumbnails, drag to reorder, drag a
tile onto a folder to move it), Settings, About. What either pane asks
for is an action, done once the frame is drawn (see "The overlay UI"),
since the draw reads a `const&` into the live canvas vector that a
mutation would reallocate. "New canvas" and "New folder" switch to what they made
and leave the panel up; closing it the instant a button is pressed was
disorienting, and stopping at "it exists" left half the job to a click
on a tile that had just appeared. The move/copy picker deliberately does
not follow the item to its destination. It comes up as the Overview
always does, on the Canvases tab with nothing deleted shown. Found in
the next review on 2026-09-27: it kept the tab a previous visit left, so
a pick after a visit to Settings came up on Settings.

A thumbnail is what its canvas shows: a snippet deleted on its own is
left out, and so is a minimized one, which the canvas shows only as a
chip on the dock - decided on 2026-09-27, after the next review asked;
it had been drawn.

Thumbnails draw strokes by default (nearly free at tile size) and
bitmaps only when asked, because a bitmap means decoding a picture for a
canvas whose pixels are deliberately not in memory. The 256px thumbnail
stored with every picture makes the ordinary case a sub-millisecond
decode; the full decode is a budgeted fallback for a picture stored
without one. A picture waiting its turn draws nothing rather than the
placeholder
gradient, which would read as thumbnails being wrong and then correcting
themselves.

Settings is a list of sections down the left, not one long scroll.
Appearance, Interaction, Defaults and Debug are about *you* and always
global; Behavior and Hotkeys are about *whatever is underneath* and are
what a profile may override, which makes the section boundary the rule.
Hotkeys holds the four global summon keys above its profile picker and
the rebindable tool keys below it, since a control that governs what is
below it must have nothing above it that it does not govern. Behavior
does the same with
deleted-item retention. In both sections each half sits in its own box,
badged Global or Per profile, and the per-profile box carries the accent
down its edge. A heading alone read as one more group of the list,
rather than as the line past which a profile changes things. The layout
is written by hand, but each row's widget takes its row in the catalog
(`ui/settings_widgets.h`) and edits through `Settings`, so the band a
slider offers is the rule's and no row restates it. A slider or a color
swatch previews while it is dragged and commits when the drag ends,
rather than on every frame of it, which wrote the file sixty times a
second.

Hotkeys are the one setting that cannot just be written: an OS
registration can fail, so the editor asks the controller and commits only
if it accepts. A combo one of the app's own other hotkeys has is taken
from it rather than refused. While a row waits for a key, a press of one
of the app's own combos never reaches it as a key - Windows hands it to
its hotkey - so the hotkey event is what the waiting row takes (see
"Input, in order").

### The cheat sheet

Every key and gesture on one panel, `Ctrl+H` by default and in the
empty canvas's menu. It exists mostly for what nothing else shows: a
tool key is on its button's tooltip, but Alt-drag in drawing mode, a
right-drag that resizes or the modifier that makes a press a drawing are
nowhere on screen. So the tutorial teaches a few gestures and names the
sheet's key, and the sheet has the rest, where they can be kept current.

Rows are built from the bindings as they are (`BuildCheatSheet`): the
summon hotkeys, the shortcuts the active profile resolves to and the
creation triggers. A rebound key reads as rebound, and an unbound one or
a trigger set to Off drops its row rather than showing "(none)". The
text of each row is fixed, while the keys come from the settings. A
hand-written page could not do this: it would be out of date the first
time someone rebound a key. The tutorial's cards fill in their keys the
same way. When a key is unbound, a card says what to do instead.

The sheet is a panel over a dimmed canvas, like the Overview, and shares
its backdrop, and it is on the machine's Panel level as the Overview is:
while it is up its own key is the only shortcut that reaches a command,
and Escape closes it and does nothing else. A tool picked up under a
panel that hides the canvas would be a change nobody saw happen. Its own key is a `ShortcutAction`
like the tools, so it is rebound in Settings > Hotkeys and a profile can
override it. The panel sizes itself to its text and fits up to three
columns within the Overview's margins. Its six groups are split across
the columns so that the tallest column is as short as it can be.

### The Behavior panel

The options of Settings > Behavior that decide how edit mode sits over a
game - Don't steal focus and the rows under it, the software pointer,
freezing the screen - in a window of their own, a number key each,
behind a global hotkey (Ctrl+Alt+B). They interact in ways that are
found by trying combinations in front of the game, and a combination
being tried can leave the mouse hard to use: clicks reaching the overlay
and the game at once, a pointer that is not where the hand is. Settings
is then several clicks away through exactly that mouse. A global hotkey
is the one key that reaches the overlay whatever the options are, so the
panel hangs off one, and its rows switch from the keyboard.

It replaced the input options HUD, a debugging aid in the corner that
listed the same rows with a number key each, behind a switch in
Settings > Debug. The HUD took the digits from the game for as long as
it was switched on; the panel takes them only while it is up. The HUD's
readouts - the frame rate, the pointer's steps, the countering's lag,
what is in front - are the debug overlay's now, under its own lines, in
edit mode.

- **A window, not a panel.** It sits top left, over the snippets and
  under the popups, dims nothing, and can be dragged anywhere by its
  background. The game and the snippets stay in view and in reach while
  it is up, since seeing them behave, and working the snippets, is how a
  combination is judged. It was built first as a panel over a dimmed
  screen, on the machine's Panel level like the cheat sheet, and that
  hid exactly what was being tried. So it is not on the machine at all:
  it keeps whether it is up itself, a click on it is a click on an ImGui
  window like the canvas bar's, and it closes by its key or its button,
  never by Escape, which stays the canvas's.
- **Its digits, and no other key.** While it is up the keyboard grab
  takes the bare digits 1 to 7 from the game and hands them to the
  overlay (`IOverlayWindow::SetPanelDigits`) - bare of the Windows keys
  too, since Win+1 is the taskbar's (found in the review of 2026-10-08) -
  and every other key goes
  where it would without the panel: to the game with keystrokes
  forwarded, so that forwarding can be judged with the panel up. The
  Canvas level turns a digit into a command, `SwitchBehaviorRow`, ahead
  of any chosen key bound to it, and claims its repeats
  (docs/INTERACTIONS.md, section 7). The HUD's digits were claimed there
  and switched at once, so a digit mid-stroke changed a setting under the
  stroke; as a command of the Hand's scope it ends the stroke first,
  kept. Taking focus instead would change the
  very thing being tried. With Don't steal focus off the overlay has
  focus already, and the digits come the ordinary way. Arrow keys and
  Space, which a first version used to pick a row and switch it, are
  game keys and canvas keys too, so the panel has none: a row is a
  digit, or a click.
- **Into the profile that runs.** A switch goes into the profile that
  matched the application in front, or into the defaults when none did,
  and the panel says which. That is the configuration being looked at;
  the Settings panel's edit target can be another profile entirely.
- **Rows read on the way up restart the overlay.** Don't steal focus,
  Take focus from elevated applications and Freeze screen decide how the
  window comes up, so a switch on one hides and shows the overlay again
  (`OverlayRequest::Restart`) once the key or button that made it is up,
  and every other mouse button too: a digit pressed during a right-drag
  ends the drag but not the press. A restart with the key still down
  loses its up, and the next press of that key was no press at all. The
  panel stays up through the restart.
- **Drawn as in Settings.** A row is Settings' own checkbox
  (`SettingCheckbox`), for the profile that runs: a row the profile
  states for itself is in the accent, with the arrow that hands it back
  to the defaults. A row whose prerequisite is off is grayed and its key
  does nothing, from the same predicates Settings grays its checkboxes
  on.
- **Explained in Settings.** The panel says where the rows are explained
  rather than explaining them: Settings has each row's help beside its
  checkbox. A first version showed the help of the row under the
  pointer, which only someone already reaching for a row would read.
- **Bound from the start.** A way out nobody has set up is no way out.
  Like every hotkey, one another application holds is left unregistered
  and named in the message at the start, and the app runs without it
  (`TrayController::UnregisteredHotkeys`). Each hotkey added makes such
  a clash more likely, which is the cost of binding one by default; the
  message, once per start, is the price of that clash, and Settings >
  Hotkeys or `null` in the file ends it.

### The tutorial

A card that leads through the app one step at a time, in topics picked
from a list: Basics (a screenshot, moving, resizing, deleting and
undoing, two warnings, getting back to your program), Pinning and view
mode, Drawing and notes, Capturing, Folders and canvases, and Profiles. A
first run starts Basics; so does the first start of an install from
before it. `docs/TUTORIAL.md` is the design, and says why.

**It reads the app, and is not part of it.** The runner
(`ui/tutorial/`, no ImGui) sees the app only through `tutorial::World`,
an interface of `const` queries that `OverlayApp` answers from the
editor, the session, the settings, the Overview and the Settings page
(`AppWorld`), and that the tests fake. It changes nothing but its own
state. What the card's buttons want - a topic started, the folder made
again, a practice snippet, what the topic made deleted at its end - is a
view action, done in Apply like every other. The rest of the app knows
the tutorial in a few named places: a widget marks where it was drawn
on an anchor board (`ViewHost::Mark`, one line per anchored widget); one
owner draws the card and the spotlight (`TutorialCard`); `OverlayApp`
answers the world, counts what nothing else counts, such as showings
and captures, and applies the tutorial's actions; the tray says what its
start found; and three settings rows keep its progress.

**It guides, and does not guard.** Nothing is held back while it runs:
no command, gesture or key is blocked, and the card takes no keys, so
the input machine is unchanged. Instead a step is done when its result
is there, however that came about - a goal compares the app's state with
a record the step took as it began - and names what it needs first, with
a line and often a button to get it back: close the Overview, go back to
the tutorial's folder, put a practice snippet here. Rails were weighed
and rejected: they would need holds at the commands, the gestures and
every widget, and could trap someone over their game.

**Each run has a folder of its own**, named for its topic, as a
lightweight sandbox: the user's own canvases are never what a step asks
to move or delete, and a capture hotkey pressed meanwhile lands there by
the ordinary current-folder rule. At the end Done, or More topics, puts
the folder in the trash, asked first like any folder's delete, unless
the card's "Keep the tutorial folder" is ticked. A throwaway library was
considered instead, and set aside: every owner holds the session, and much of the view's state would
need an answer to "the library was replaced".

**A step is a row of a table** (`chains.cpp`): its text, what it points
at, its needs, its goal as a plain function, and the mistakes it has a
line for. A topic is a chain and a row in `topics.cpp`. The derail
matrix crosses every do step of every topic with every way off the path
- the Overview opened, the canvas switched, the snippet deleted,
minimized or made fullscreen, the overlay hidden - and checks that the
card says a line, and that following it gets the step done.

### Show deleted

A checkbox on the Canvases row adds what is deleted to the same sidebar
and grid, where it was: deleted folders in the sidebar and deleted
canvases in the grid, marked out in red with a Restore and a Delete
permanently each, and everything else dimmed. A folder is marked out
when it is deleted or holds a deleted canvas, and both its buttons act on
what is deleted in it - all of it back, or all of it gone for good, the
folder too only if the folder is what was deleted. Putting the buttons
on the thing, where it would come back to, says where a restore lands
without a line of text to explain it.

The folder's color tells the two cases apart: red for a folder deleted
whole, yellow for a live one holding a deleted canvas. One red for both
read as "this folder is in the trash" for a folder that is in use and
only has something deleted in it. A legend on the tab row, before the
checkbox, says which color is which, so neither has to be learned from
a tooltip. It shows each as a small copy of a marked row - the fill and
the ink, nothing else: a square swatch beside the words read as one
more checkbox on a row of them. It and Empty trash go before the
checkbox rather than after it, so the checkbox does not move when it is
ticked; on a row too narrow for both, the legend is left out.

Empty trash: every folder and canvas with a mark
of its own goes for good (`Session::EmptyTrash`, through
`CanvasManager::Marked`, the retention period's list with no cutoff).
It is always confirmed, whatever Settings > Behavior says about deleting
permanently: that setting is about one thing at a time, pressed on the
thing, and this is everything at once from a button away from all of
it. The confirmation gives the same count as the Show deleted checkbox.

A deleted folder is looked into rather than browsed: the browsed folder
is where a new canvas lands, and the manager never lets that be a
deleted one, so the UI keeps the deleted folder it is showing
(`deletedFolderShown_`) and New canvas is off while it does. Restored,
the folder becomes the browsed one. A deleted canvas cannot be opened -
restoring it is how it is opened - so nothing needs a read-only check;
the live folders and canvases around it stay fully usable, only dimmed.
Snippets are left out: a deleted snippet comes back by undo or not at
all (see Deletion is a mark).

A mode that showed deleted things in place but read-only, and then a
list of everything deleted, newest first, came before (see "Dead ends"):
the first needed a read-only check at every edit, and the second could
not show where a restore would put a thing.

The hover tooltip tells when a thing was deleted and, with retention on,
when it goes (`DeletedWhen`, `GoesOn`). A time the C runtime has no date
for - before 1970, or past the year 3000 with Microsoft's - is told as
unknown. Found in review on 2026-09-27: `std::localtime` gives nothing
for one, the zeroed date put in its place has day 0, and `strftime`
refuses that as an invalid parameter - which the crash handler dumps and
ends the process on (see platform/win32/win32_crash_dump.cpp). The load
no longer lets such a stamp in (see "Reading what cannot be used"), so
what is left to reach it is a clock set near the year 3000.

### Cursors

Every cursor is ImGui's except the crosshair and the pen, which ImGui's
set does not have and every OS does, so those go through
`IOverlayWindow::SetCursorShape`. `WantedPointerShape` is the one place
that says what the pointer means; both the OS cursor and the drawn
software pointer read it, so they cannot disagree. The shape is pushed
again when the pointer moves, or when ImGui's wanted cursor has changed
in either of the last two frames - the backend's own cursor push lands
one frame late - and not otherwise: pushing it every frame was measured
at a quarter of an idle frame (see `Pointer::ApplyPointerShape`).

Over a panel the pointer is ImGui's, whatever tool is in hand: the
crosshair of a creation tool is asked for only over the canvas, where
the click places the snippet. Found in the next review on 2026-09-27:
it was asked for first, and a menu or the Overview wore it.

### A snippet's border: one line, color by state, weight by pointer

Every snippet wears one border, and it says two things, each in a
channel of its own:

- **Its color says what the snippet is.** In order:
  - selected, in the accent, or a color of its own
    (`snippetColors.borderSelected`);
  - pinned;
  - in front;
  - the rest.

  The colors are settings (`appearance.snippetColors`).
- **Its weight says where the pointer is.** It is heavier on the
  snippet under the pointer, or the one being dragged, resized or drawn
  into, whatever its color.

Drawing mode adds a faint halo a few pixels out, rather than a heavier
line, since weight already means the pointer.

**Changed on 2026-10-01: the selection's outline replaces the border.**
It used to be drawn over the border. A tester reported the colors
"mixed" on a selected snippet under the pointer. Hover had thickened the
border underneath to 3 px, while the selection's outline stayed at 2 px.
The border's color (white at about 43%) then showed as a band inside the
accent, and both lines were half a pixel off, so the two blended into a
pixel of neither color. Now the selected snippet's border is the
selection's, drawn with the selection over every snippet, and its weight
follows the same rule as any border's (`BorderThickness`).

What is lost is a pinned snippet's color while it is selected. The bar
says it instead: its Pin button is lit for a pinned selection.

Every border is drawn inside the snippet's rect on whole pixels
(`AddInnerOutline`); see the next section.

### Tooltips

Every tooltip goes through `HelpTooltip` or `InfoTooltip` (`ui/widgets.h`),
never `ImGui::SetTooltip` directly. Both wait until the pointer has rested
on the same tooltip for half a second, so crossing the interface on the way
somewhere no longer flashes a text at every button it passes. ImGui's own
`ImGuiHoveredFlags_ForTooltip` delay was not used: it goes by the last
item, and half the tooltips here are for things ImGui does not see as
items (the bar's tiles and the canvas bar are hit-tested by hand), so the
wait is kept by the tooltip's words instead. Another tooltip, or a frame
with none, starts it again. A text that changes while it is up, such as
"deleted 2 minutes ago", starts it again too, which is rare enough not to
matter.

The two differ in the Appearance setting `showHelpTooltips`. Help, which
says what a control does, can be switched off by someone who knows the
app. Information shown nowhere else always appears: a canvas tile's name
and place, when something was deleted and what a deleted folder holds,
and the default that a "set here" mark resets to. ImGui's own tooltips,
like a color swatch's values, are left as ImGui has them.

### ImGui gotchas worth knowing before touching this code

- `##` hides an id from the display; only `###` detaches it from the
  label. The id of `"Play##1"` is a hash of the whole string.
- Two visible widgets with one id is an ImGui error, detected only while
  they are hovered, so it never shows in a screenshot of an idle panel.
- `CalcTextSize` measures the line box, not the ink; centering a glyph on
  it sits the glyph low. `FindGlyph` gives the ink's own corners.
- `DC.CurrLineSize.y` is the row height something joining a row after
  `SameLine` should center on; `GetFrameHeight()` is only right if a
  framed widget started the row.
- `IsMouseHoveringRect(..., clip=true)` intersects with the *current*
  window's clip rect, which between windows is whatever the stack left.
- An ordinary window sets `WantCaptureMouse` just from being hovered,
  and a press ImGui owns keeps it true for the whole drag. Every layer
  the app paints into is `NoInputs`; ImGui's hit-test does not stop at
  an excluded window but at the next ordinary one behind it.
- `NoBringToFrontOnFocus` also changes where a *new* window is first
  inserted (the back). The app sets the order of every window it draws
  itself, every frame (`OverlayApp::StackSurfaces`).
- Chrome that has to sit at a stated height gets a screen layer of its
  own; the background and foreground draw lists are fixed at the very
  bottom and the very top, and a border drawn at the bottom was invisible
  under a fullscreen snippet, exactly where the cue mattered most.
- A live stroke has to be drawn inside the item's own draw block, after
  its fill; a layer below is painted over by the fill.
- An `AlwaysAutoResize` window's real width is not knowable from its
  content's logical bounds; ask `FindWindowByName`.
- Overlapping widgets resolve first-submitted-wins: `ItemHoverable` sets
  the hovered id once per frame and every later widget over the same
  point returns false.
- `AddRect`'s order is `(min, max, col, rounding, thickness, flags)`, and
  `AddPolyline`'s and `PathStroke`'s put thickness before flags too; a
  flag in the thickness slot compiles and draws garbage. 1.92.8 swapped
  them and kept the old order working through obsolete overloads, so
  the old order compiled silently. Since 2026-10-02 the build defines
  `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, which deletes those overloads, so
  the old order no longer compiles; that took the test engine at the
  ImGui version it runs against, which picks the order by that version,
  and four obsolete names out of the app. An ImGui bump that obsoletes
  more breaks the build rather than going on with the old meaning.
- `AddLine` and `AddRect` move their points half a pixel: `AddLine` to
  the center of the pixel at the coordinates it is given, `AddRect` half
  a pixel in from each edge. That is right for a one-pixel line on whole
  coordinates and wrong for anything else. A line placed by its own
  center lands half a pixel off, across two columns; that was the
  selection bar's divider until 2026-10-01. A wider rect outline hangs
  out over the rect, blurs over a pixel more on either side, and is cut
  where a clip ends at the rect; that was the dock's last tile.
  `AddInnerOutline` draws an outline of any whole width inside its rect,
  and a line of a stated width is a filled rect.
- `WindowRounding` feeds a window's *minimum height*; a pill value of
  999 is safe only for frame and grab rounding.
- A popup's id is scoped to the window current at `OpenPopup`/
  `BeginPopup`; both must run inside the same `Begin`/`End` block or the
  popup silently never opens.
- `BringWindowToDisplayFront` wins by running *last*, so the app calls it
  in one place, once everything is drawn: section 3 of
  `docs/VIEW_LAYER.md`, bottom to top, with every popup ImGui has open
  inside a window brought up right after that window - found through the
  open-popup stack, which also reaches a widget's own internal popup
  (`ColorEdit3`'s picker) that has no `Begin` of ours to hook. Tooltips
  are drawn in a layer of their own above every window, whatever the
  order.
- A popup's default placement is anchored to the mouse and can overlap
  its opener, stealing clicks; pin it with `SetNextWindowPos`.
- `TextWrapped` wraps to a width that is not settled on a new auto-resize
  popup's first frame; use an explicit wrap position.
- A hand-rolled wrapping grid needs a trailing `Dummy` after the loop or
  ImGui asserts about extending the parent's bounds.
- `InvisibleButton`'s return value, not `IsItemClicked()`, means
  "clicked": the latter fires on press, and breaks click-to-select next
  to a drag source.
- The input queue and the key state both outlive a gap in frames: an
  event queued while nothing is rendering waits for the next `NewFrame`,
  whenever that is, and what ImGui believes is held is whatever the last
  frame saw. A window that stops drawing while its messages keep arriving
  has to say so - see the hotkey letter stranded by hiding, under the
  input grab.

## The tray controller: the overlay's five states

`app::TrayController` owns the settings, the session and the overlay,
loads the library, and is the only place that knows what a hotkey does.
The overlay is in one of five states:

- **hidden**;
- **the pinned view**: the current canvas's pinned snippets,
  click-through;
- **a notice**: a message and nothing else, click-through;
- **view**: the current canvas, click-through;
- **edit**: everything, interactive.

Two hotkeys ask for edit and view. Each puts its own mode away, and
switches straight to its mode otherwise, including directly between edit
and view with no hide and reshow. Away is the pinned view when the
current canvas has a pinned snippet, and hidden otherwise. Every request
in every state, and what the window is told on the way, is
`docs/OVERLAY_STATES.md`, section 5, pinned by `overlay_states_test.cpp`.

The controller holds the state, and changes it in one place:
`Apply(Next(state, request, facts))`, where `Next` (`app/overlay_states`)
is the table and `Apply` carries a transition out in the one order the
document gives. Reading the state back from the window and the overlay's
flags instead, so that no member could go stale, held for three states;
with five, the flags were what went stale, and every caller combined
them its own way. The overlay's mode is one `OverlayMode`, which the
controller sets, and a debug build checks after every transition that
it, the window and the session agree with the state.

**Pinned snippets.** Whenever the current canvas has a pinned snippet,
"hidden" is the pinned view instead: view-only with every other snippet
left out, click-through, never focused, put up the way a notice is. It
stands in for hidden throughout, and there is deliberately no hotkey that
hides it - unpinning is how pinned snippets go.

**Notices.** A state the hotkeys never ask for: view-only with the
canvas left out, so the only thing on screen is a message. It exists for
the silent capture hotkey, which acts while the overlay is hidden and
still has to say what it did. Three things about it were found by
watching it fail on the real thing: showing a window activates it
(`SW_SHOW` takes focus by itself, hence `SW_SHOWNOACTIVATE`); a
window given `WS_EX_LAYERED` before its first show draws nothing, so the
order is show first, click-through styles second - with the window
already counted click-through for the input grab, which a show in edit
mode would start. Those are the window's own rules for every change it
is told to make (see "The window is told what to be", below); and
ImGui's clock is not the app's clock, so the first frame after an hour
in the tray carries an hour's delta and puts every expiry set while
hidden in the past - the renderer caps the delta at 0.1 s.

**The window is told what to be.** `IOverlayWindow::Present` takes
hidden, click-through or interactive, and the window gets there in the
order only it knows: a plan of steps from `PresentationSteps`
(`platform/presentation.h`), pure and tested on every pair, which the
Win32 window carries out. With the order each caller's to choose, view
mode came up as edit mode made click-through a moment later, which
started the input grab and, with "Don't steal focus" off, took focus
from the game only to hand it back. Focus is taken by one step, which
notes the window it was taken from, and handed back only while the
overlay holds it: noted at the show instead, the note could be hours
old (`docs/OVERLAY_STATES.md`, section 7, has the faults and how each
was reproduced).

**Transitions happen between frames.** Two requests arrive inside a
frame - a notice's message has faded, and the Behavior panel asks for a
restart - and a committed setting usually does too. Each is
posted (`IPlatformHost::Post`) and carried out after the frame, before
the next: Win32 queues it and posts the host window a message, which the
loop dispatches before it draws. Hiding or restarting the window from
inside the frame callback relied on the renderer ending the frame
whatever happened in it - true, but something every change to the
renderer would have to keep true. The settings file is still written in
the frame, where a failure is said.

**What a setting does to the window is decided after the frame, in one
place.** `ApplySettingsToWindow` compares what the settings running want
with what the window was last given: no-activate, the input options,
whether a frozen screen is held, and the display - rather than the
Settings panel doing it from inside its draw. Switching
the frozen screen on still waits for the next entry into edit mode
(`docs/SETTINGS.md`, section 7).

**View-only draws only now and then.** Its picture does not change by
itself, and with pinned snippets it can sit over a game for hours, where
drawing at the refresh rate cost 2% of a core. The overlay tells the
window which frame pacing it wants; idle, the Win32 loop draws a frame
every 250 ms plus one after any dispatched message, and sleeps in
`MsgWaitForMultipleObjectsEx` in between. Measured: 31 ms of CPU per 10 s
in the pinned view, against 203 ms before.

**Profiles are resolved once per session**, as view or edit mode begins
and before the window is presented, which is what makes the application
underneath the answer rather than this window. Every view mode is a
session, the one entered from the pinned view or a notice too, so edit
mode from any of them switches in place; the pinned view and a notice
are not, and resolve nothing. A restart the overlay does to itself (for a setting only read on
the way in) keeps the answer it already had: asked again in the middle
of hiding itself, the overlay may have taken the foreground on the way
out, the profile stops matching, and a toggle that went into it reads as
if it had switched itself back.

**Which display** is decided when the overlay comes up from hidden and
kept while it is up, so switching modes or a capture hotkey happens where
the overlay already is. It moves while up only when the displays change
or a different monitor is picked in Settings, and a move retakes the
frozen screen, which was a picture of the display just left.

**Hotkeys are the one setting that cannot just be written**: a
registration can fail, so the editor asks and the controller commits only
if the OS accepts. A set combination that cannot be registered - another
application owns it - is left unregistered and named at the start, for
any of the four: the tray menu reaches the overlay without one (see "A
hotkey another application owns does not stop the start", under
Configuration). A first run - nothing on disk at all - shows the overlay in edit
mode and starts the tutorial (`docs/TUTORIAL.md`), since an app that
installs a tray icon and then waits for a chord it never mentioned is
indistinguishable from one that did not start. Two of its steps are
warnings: set up Behavior and profiles per program, because no one set
of input defaults suits every game, and beware of anti-cheat systems,
which may object to an input hook drawing over a game. Skipping the
tutorial still shows both.

## The Windows backend

### Process and window lifecycle

Single process, single executable: a hidden `HWND` with no render loop
costs essentially nothing, so a tray-stub-plus-spawned-overlay split
would add IPC and a second failure surface for no benefit. At launch only
the hidden host window that receives tray, hotkey and session-end
messages exists;
the first hotkey creates the overlay window and the D3D11 device, once
per session; every toggle after that is `ShowWindow`, with no swapchain
teardown, so fast repeated toggling has no re-creation latency. While
hidden the event loop blocks in `GetMessage` and nothing is rendered,
which is what delivers near-zero idle CPU. `Destroy` happens once, on
exit.

The tray icon is Explorer's to keep, and Explorer restarting - a crash,
or ended in Task Manager - drops every icon it had. Once the new taskbar
is up it says so to every top-level window with the registered message
`TaskbarCreated`, and the host adds its icon again. It did not listen:
the icon was gone until the app was restarted, and the tray menu's Exit
with it. An elevated copy is above Explorer, where UIPI drops the
broadcast unless the window lets it through (`ChangeWindowMessageFilterEx`).

The same message puts up an icon the shell refused in the first place.
Started at log-on, the app can be ahead of Explorer's notification area,
and the add fails; the icon is still wanted, and goes up with the
taskbar's `TaskbarCreated`, or at the next try of a timer every five
seconds, for an Explorer that was only slow to answer. Each add takes
out any icon of ours first, since one that timed out may have landed
anyway. Found in review on 2026-09-27: the failed add ended the start,
with the message for a second copy already running, which there was
not; and a failed add after an Explorer restart was never tried again.

A window nobody can see is not drawn either. With the screen locked or
the secure desktop up, `Present` returns `DXGI_STATUS_OCCLUDED` at once
instead of waiting for vsync, and a frame loop drawing every frame used
a whole core until unlock. Once a present has said so, each frame first
asks with `DXGI_PRESENT_TEST`, which draws nothing, and waits while the
answer is still occluded.

DXGI's own Alt+Enter is turned off (`DXGI_MWA_NO_ALT_ENTER`, asked of the
factory that made the swap chain, at each device made). Found in review
on 2026-09-27: with the overlay holding focus, Alt+Enter would have
taken the swap chain to exclusive fullscreen - opaque over the game,
and nothing here handles it. No test: nothing reads the association
back, and trying the key on the old build would take the screen.

### Text is UTF-8, and so is the code page

Every string in the app is UTF-8, but on Windows the narrow side of a
`std::filesystem::path` is the process's ANSI code page, and MSVC's
`path::string()` throws on a character that page cannot spell - a user
name in Japanese on an English Windows puts one in every path under
`%APPDATA%` and `%LOCALAPPDATA%`. When the library was a directory tree, a canvas directory
renamed by hand was enough to crash every load. Rather than convert at
each place a path becomes a string, every
executable, the tests included, carries a manifest
(`src/platform/win32/resources/utf8.manifest`) that makes UTF-8 the
process's code page (Windows 10 1903 and later): `path::string()` and
`path(std::string)` then round-trip exactly, and the `-A` Windows calls
take the same UTF-8 the UI strings are in.

The sources are UTF-8 too, without a BOM, and compiled with `/utf-8`.
Found in the next review on 2026-09-27: MSVC read them in the build
machine's code page, so on an English Windows the test's `u8"ü 日本"`
became other characters, all inside that page, and the test of a
name outside every code page tested none.

A string stays UTF-8 only while nothing cuts it. A text field over a
fixed array is handed a copy made to fit the array, and `snprintf` fits
it by bytes, through the middle of a character; an edit then saves the
cut. The profile fields did that. A new profile was named after the
window's title then, and a title in characters of three bytes each overran
the name field's 128 bytes at the forty-third. `json::dump` throws on a
string that is not UTF-8, and nothing between a settings edit and
`SerializeConfig` catches it, so renaming that profile ended the app. A
field that edits a string made elsewhere is now `InputString` over the
string itself, grown through the resize callback as the note editor's
is. The Overview's rename fields keep their array: the names they edit
are only ever made there or from a timestamp, and ImGui cuts what is
typed or pasted into one at a character's edge. `SerializeConfig` also
writes a broken character as U+FFFD instead of throwing, since a
setting saved slightly wrong is not worth the app.

So does a snippet's record in the library (`ItemRecord`). Found in
review on 2026-09-27: it was dumped strictly, and nothing around a write
catches - a name or note that was not UTF-8 would have ended the app at
the save. Nothing known makes one today; a snippet written slightly
wrong is still not worth the app.

### In front of the taskbar

`WS_EX_TOPMOST` puts the overlay in the topmost band but not at its front,
and with the taskbar as the foreground window as the overlay comes up,
the taskbar lands in front of it anyway. Measured: the first frame is
clear, the taskbar is in front from the second (8-16 ms after the show),
and without a correction it stays there. Claiming the front in `Show` is
too early to help, so each frame checks whether a topmost window is
above the overlay and covering it, and only then moves it back. Once put
back, the taskbar stayed back in every run, so this checks every frame
only for the first half second after a show, four times a second after
that. Checking at that rate from the start left the taskbar in front for
up to 250 ms, long enough to see the canvas bar's first peek pop out
from under it; checking every frame leaves it there for one frame.

The Start menu, Search and the notification center stay in front, and
that is known and left so. They are drawn in z-order bands of their
own, above the one every application's windows share, topmost ones
included: measured on Windows 11 (build 26200) with the undocumented
`GetWindowBand`, the overlay and the taskbar are in band 1, Start and
Search in band 6, notifications in band 4. No window of an ordinary
exe gets above them; that takes UIAccess, a signed exe installed under
Program Files. What would make them go is taking focus, since Start
closes when it loses it - and the overlay does not close the user's
shell UI for them.

### Translucency

The overlay window is *not* `WS_EX_LAYERED`. It is an ordinary topmost
tool window made transparent through `DwmEnableBlurBehindWindow` with an
effectively infinite blur region (ImGui's own `EnableAlphaCompositing`
helper), which makes the DWM composite the window using its rendered
per-pixel alpha: the render target is cleared to alpha 0 each frame, so
untouched regions are see-through and strokes are opaque.

Two layered-window approaches were rejected because layered windows tie
hit-testing to pixel transparency, which is fatal for an overlay whose
premise is that the whole screen is clickable while shown. Color-keying
(`LWA_COLORKEY`) composites correctly but `DefWindowProc` answers
`WM_NCHITTEST` with `HTTRANSPARENT` over keyed pixels, so clicks on
"empty" parts fell through, and answering `HTCLIENT` explicitly did not
fix it. Constant alpha (`LWA_ALPHA`) does not consult the backbuffer's
alpha at all, so the whole window went uniformly opaque.

### Click-through for view-only

Answering `WM_NCHITTEST` with `HTTRANSPARENT` does nothing on real
hardware against a window in another process. What works is toggling
`WS_EX_TRANSPARENT` on the whole window, paired with `WS_EX_LAYERED`,
which is required alongside it to take effect reliably. `WS_EX_LAYERED`'s
bit is borrowed only for what it does to hit-testing; neither
`SetLayeredWindowAttributes` nor `UpdateLayeredWindow` is ever called,
since those are how the rejected techniques fed a window's alpha and
would fight blur-behind. Going click-through also hands keyboard focus
back to what had it (the `HandFocusBack` step of `PresentationSteps`),
since click-through alone only stops mouse routing and the overlay would
otherwise keep eating the game's keys.

Focus is handed back, going click-through or hidden, only if this window
held it when the change began: once view-only has handed focus off, the
user may have clicked into other windows through the click-through
overlay, and forcing a stale memory over what `GetForegroundWindow`
already points at can only make things worse.
Alt+F4 is swallowed, and `WM_CLOSE` is the app's Exit (see "Every
command is written as it is made"): `DefWindowProc` would destroy the
window and nothing resets the handle, so `EnsureCreated` would report
success against a dead window forever.

### Not stealing focus in edit mode

Some games detect losing focus and pause or throttle. `WS_EX_NOACTIVATE`
baked into the creation style stops showing or clicking the window from
activating it, while mouse routing works as usual. It defaults on, and
has a real measured cost: keeping the game foreground is exactly what
most games use to gate raw mouse input for camera-look, so a drag draws
on the overlay *and* spins the camera. The input options below exist to
claw that back, which is why they default on together.

Foreground is not focus, and `WS_EX_NOACTIVATE` blocks the half that
matters: a text field opened under it looked ready and every keystroke
went elsewhere with the system beep, two renames in eight. So a field
*borrows* focus by clearing the bit for the duration - or, better, needs
no focus at all while the keyboard is grabbed (below). Either way what
was borrowed is handed back when the field closes, and a field can close
two ways: ImGui deactivates it, or the Overview around it is closed from
outside the frame by a mode switch, in which case the field is never
rendered again and its own release never runs. `OverviewPanel::Close`
ends the name edit, whose end releases for it; `ReleaseTextInput` is idempotent, so both routes running is
harmless.

### Taking input back from the game: the input grab

`WH_MOUSE_LL` and `WH_KEYBOARD_LL` are the only user-mode mechanism that
can discard an input event before another process sees it. What that
reaches was measured rather than assumed, and the answer is not the
intuitive one: a foreground application registered for raw input stops
receiving legacy messages, `GetAsyncKeyState`, cursor movement and the
whole raw *keyboard* stream, but keeps receiving the raw *mouse* stream.
Raw mouse input branches off ahead of the hooks and is delivered only to
the foreground window, so the sole way to stop it is to take the
foreground - the one thing this mode exists to avoid.

Consequences that shape `Win32InputGrab`:

- **The grab feeds the overlay.** Movement, buttons and wheel are read
  from the raw stream (one stream, so a click can never land at the
  position of the previous report) and re-posted as ordinary messages.
  Button state is tracked in the grab because Windows no longer knows it.
  Raw input reports the button pressed, before Windows swaps left and
  right for a mouse whose primary button is set to the right
  (`SM_SWAPBUTTON`), so the grab swaps them itself (`ButtonSwap`), asking
  the setting at every press. Taken as they came - confirmed by hand -
  the overlay under the grab read that primary button as its secondary: a
  click opened the menu, while the same overlay without the grab, fed by
  Windows' own messages, had it right. A button comes up as the one it
  went down as, should the setting change while it is held.
- **The pointer is driven by raw device counts, not screen positions.**
  Differencing integer cursor positions discards any movement too small
  to cross a pixel with no remainder kept: measured, 22% of events came
  out as zero and 5% of motion vanished, and slow steady movement moved
  the pointer not at all. Raw counts are exact; the sub-pixel part is
  accumulated in a float.
- **The pointer obeys the desktop's own ballistics**, read from the
  registry: the speed slider and, when "enhance pointer precision" is
  on, the acceleration curve. The slider means two different things and
  both were measured: with the curve off Windows applies the documented
  multiplier table, with it on a linear `slider/10`. Applying the table
  in curve mode ran the pointer at 2.25x where the OS runs 1.5x, which
  lifted slow-speed gain past a pixel per count and made single reports
  step two pixels. Fractional pointer drawing was tried twice to hide the
  two-pixel steps and retired once the real cause was fixed.
- **The curve is applied per mouse report, as Windows applies it.** The
  first version read it at hand speed - counts per millisecond, timed on
  arrival - with one constant calibrated on a 1000 Hz mouse, and a
  125 Hz mouse then felt slow. Measured against the real cursor
  (`pointer_ballistics.h`): the same counts travel the same distance in
  1-count reports at 1000, 125 or 50 Hz, and further in larger reports;
  time plays no part, and neither does the monitor's refresh rate (60 and
  120 Hz identical, contrary to what older reverse-engineering reports).
  A report's size is its larger axis plus half the smaller, the curve is
  read at size / 3.5 and its output scaled by 0.8 - fitted to 26 report
  sizes, RMS error 0.0003 px per count - and the slider multiplies that.
  Windows' background rate cap merges a fast mouse's reports into one
  raw message, so the size of each is not in the message; the low-level
  hook, called once per report regardless, counts them between messages
  (exact in every message logged: 16 counts over 8 reports) and the
  curve is read at the average. The grab now travels what the desktop
  pointer does to within a pixel or two at 125 and 1000 Hz, slider 6 to
  14, curve on and off. The cap has one visible cost left: the last
  reports of a movement reach the sink about 55 ms after the mouse
  stops, so a fast mouse's pointer settles a few pixels late.
- **The overlay draws its own pointer** because a game holding the mouse
  for mouse-look typically sets the cursor back to screen center every
  frame, and `SetCursorPos` is not an input event: 120 such calls
  produced zero hook invocations. Sharing one cursor with such a game is
  unwinnable. The grab accumulates its own virtual cursor and submits it
  to ImGui between the backend's frame setup and `ImGui::NewFrame`.
  Hiding the OS cursor takes `io.MouseDrawCursor`, not just
  `WM_SETCURSOR`: the backend also installs a cursor from its own
  `NewFrame` whenever ImGui's wanted shape changes, and under a grab the
  cursor stops moving over the window so `WM_SETCURSOR` barely arrives.
  Drawing and driving the pointer are separate options: with the drawn
  pointer off the grab writes its position to the real cursor
  *absolutely*, which does not go through the ballistics curve (a
  relative `SendInput` did, and was the reason the pair looked
  inseparable).
- **The handover runs both ways from one place.** The real cursor is
  parked where the grab began and the drawn one is far away by the end;
  starting to drive seeds from the real cursor and stopping puts the real
  cursor where the drawn one was, both in `Refresh`, or switching raw
  input on mid-session leaves the pointer jumping back to where edit mode
  opened.
- **The hooks live on their own thread, and this is not a nicety.** A
  low-level hook runs on the thread that installed it and the input stack
  blocks every mouse event system-wide until that thread services it.
  Installed on the render thread, which sits in `Present` most of a
  frame, that throttled the whole machine's mouse to the frame rate:
  300 injected reports took 10 ms ungrabbed and 5,318 ms grabbed, and a
  drag laid down 2 stroke points ungrabbed and 282 grabbed. On a thread
  that does nothing but pump, 18 ms and 4 points, the ungrabbed baseline.
- **One hook thread, told what is wanted.** The thread is started the
  first time anything is wanted and ended only by `Shutdown`; hidden, it
  holds nothing - no hook, no raw-input registration, no timer, not the
  desktop-switch hook - and sleeps in `GetMessage`, a stack's few pages
  and no CPU. Starting it is a handshake: a thread has no message queue
  until it first asks for one, and `PostThreadMessage` to a thread
  without one fails, so the thread signals an event once its queue exists
  and the start waits for that, half a second at most; one slower than
  that keeps the event and reconciles by itself when it gets there.

  `Refresh` decides once what is wanted - the pointer grab, the keyboard -
  makes its handovers for that decision, and hands it to the thread as
  one snapshot with a generation number (`Wanted`); the thread installs
  from the snapshot alone, publishes what is in - `Off`, `Installed` or
  `Failed`, for the pointer and the keyboard - and answers the
  generation. `Refresh` waits for the answer, half a second at most, so
  a grab is in, or known not to be, when the call that asked for it
  returns. Everything the app thread decides by reads what is published,
  never what was asked for: no thread, one that could not start, and
  one that has not answered are nothing installed, so every fallback -
  the real cursor, typing by focus - holds without a word from the
  thread. A thread that misses its answer is not waited for again until
  it has caught up: one wait per hang, not one per call.

  Until after 0.3.1 the thread came and went with every grab, which made
  every show and hide a race between two threads: over a queue not up
  yet, a quit posted to one, a thread that outlived its stop and the
  sink window that died with it. Its states were told apart by which
  handles were null, it published only failures - whose default, none,
  read as working whenever no reconcile had run, a thread that never
  started included (found in the review of 2026-10-08) - and the app
  thread and the hook thread each worked out what was wanted from the
  live state, so that what was installed and what the handovers were
  made for could be two decisions. Every review since September found
  one more race among those; the classes are gone with the per-grab
  thread and the second decision. `Shutdown` still waits two seconds for
  the thread and keeps the handle of one that does not end, so that no
  second thread is started over its hooks.
- **The mouse hook and the raw-input sink are one grab, set up whole or
  not at all** (`ReconcileHooks`). The hook alone swallows the mouse with
  nothing left to read it: the overlay's pointer stands still and its
  clicks never arrive. The sink alone posts every click to the overlay
  on top of the one Windows delivers. Until 0.3.1 neither the sink's
  window nor its `RegisterRawInputDevices` was checked, and a sink whose
  registration failed was kept and never registered again. Now a failed
  try takes down whatever it set up, `VirtualCursorActive` turns false so
  that the overlay goes by the real cursor as it does without the grab,
  and the hook thread tries again every second for as long as the grab is
  wanted (a keyboard hook that could not be installed as well). With
  the sink up and the hook refused, the sink goes too
  (`FailMouseHookForTesting`). A grab got
  on a later try seeds the drawn pointer again from the real one
  (`SeedVirtualCursor`). The seed happens on the hook thread, in the same
  reconcile that installs the hook and the sink, and before the grab is
  published. So no report is integrated, and no click is stamped with a
  position, until the seed is in place. The app thread is told only to take
  its frame baseline, the frame's last point, from the new position at its
  next frame (`SampleFrameStep`), since that point is its own. Two earlier
  versions were wrong: seeding the baseline from the hook thread was a data
  race, and leaving the whole seed to the app thread's next frame let a click
  through at the old position first and could lose the request (both found
  in follow-up reviews). A grab that ends while it is failing hands no
  cursor back (`Refresh`): the real cursor was the pointer all along, and
  the drawn one was still where the grab began, so the hand-back put the
  cursor there - the very jump it exists to prevent (found in the review
  of 2026-10-08).

  A keyboard hook that cannot be installed swallows nothing, so the keys
  reach whatever has focus. Its failure is published too:
  `CanDeliverTyping` and `DeliversTypingToOverlay` say no, so a text field
  opened meanwhile takes focus instead of waiting on a hook that is not
  there. A field that chose the hook anyway, when no hook was needed until
  it opened, learns as it opens: `SetTextFieldOpen` returns whether typing
  is delivered, the hook thread's answer waited for, and the field takes
  focus at once (`TakeTextInputFocus`). It used to be told afterwards, by
  a message the hook thread posted at the first failure, which a field
  opening just as that failure was published never got (found in the
  review of 2026-10-08). The modifier record is cleared on failure, leaving
  the system's state alone to say what is held, and it is seeded from the
  system just before every install, on the hook thread. With no hook in
  place nothing has been swallowed, so the system is right; a modifier let
  go of between `Refresh`'s seed and the install, a retry later, was
  otherwise left held. A grab that ended between the hook thread's look
  at what is wanted and its seed is cleared again after it: the end had
  cleared the record before the seed refilled it with the modifiers of
  the very hotkey that ended it, and once the grab is over no hook
  records. `VirtualCursorActive` says what is installed: the ordinary
  start has no frame on the real cursor all the same, since the call
  that shows the overlay waits for the thread's answer.
  `FailPointerGrabForTesting` makes the registration fail,
  `FailHookThreadStartForTesting` the thread's start, and
  `StallHookThreadForTesting` holds its next answer up.
- **The pointer's integration state has a lock of its own.** The
  raw-input sink integrates reports into it on the hook thread, and the
  app thread seeds it whenever the virtual pointer starts driving. The
  two were assumed never to overlap, but they can: the hook thread
  outlives a raw-mouse toggle while the keyboard grab keeps it up, and
  the sink is only taken down once that thread reconciles, so switching
  raw input off and on again can seed the position under a sink still
  moving it. A dedicated mutex, taken per report and at seeding, never
  across `SetCursorPos`, is uncontended the rest of the time; the
  diagnostics read their gain under it too.
- **Movement is never posted.** Windows coalesces `WM_MOUSEMOVE` to about
  one per frame; re-posting every swallowed report made a 1000 Hz mouse a
  message flood. The render thread emits one Move per frame while a
  button is held, if the pointer moved, which is the OS's own behavior
  by construction. A button's release is posted, with its own position,
  and can come before the frame's move to where it happened - so a
  gesture ends where its release says, not where the last move left it:
  a placement is applied at the release's position first, and the eraser
  passes on to it (`Placement::Released`, `Marking::Released`), as the
  pen and the shapes always did. Ended at the last move, a quick drag
  landed a frame short, and a handle let go of with no move between
  resized nothing. Found in review on 2026-09-27. The pen went further
  than its last move only to snap the end of a line already drawn, so a
  stroke pressed, moved and released between two frames, with no move at
  all, stayed a dot. After 0.3.1 a release with no second point yet is a
  line when it is at least twice the point spacing from the press, which is
  as far as a single move must go to earn a point through the smoothing
  (`DrawTool::OnMouseEvent`); a twitch is still the dot. The first version
  made the release stand in only when no move had come, and one short first
  move, taken by the frame before, left the stroke a dot all the same
  (follow-up review). The path between
  two frames is still one straight step: a queue of every report for the
  pen was left out until strokes are seen to need it.
- **Modifiers are fed to ImGui by hand**, from `GetAsyncKeyState` OR'd
  with the grab's own record, since the backend learns them from key
  messages and key messages need focus. A keyboard chord has one frame
  to work in and a stale modifier event cannot be corrected after the
  fact (ImGui refuses a second change to a key in one frame), so for the
  duration of one key message the grabbed modifiers are made visible to
  `GetKeyState` with `SetKeyboardState`, which is safe to lie in
  precisely because no real key message reaches that thread.
- **Hotkeys are dispatched by the grab.** A swallowing keyboard hook
  suppresses `RegisterHotKey` too (the hook ate 18 events, `WM_HOTKEY`
  never fired), so without this the grab would disable the hotkey that
  turns it off. The grab matches every registered combo itself and posts
  an identical `WM_HOTKEY` back. The key is then handed to the overlay as
  well - a global chord is not a reason for the focused surface to go
  deaf - which strands the letter of the hotkey that *hides* the overlay:
  it is posted after the last frame of that showing, no frame is drawn
  while hidden, and ImGui's queue holds it until the overlay comes back,
  where it reads as a fresh press. Ctrl+Alt+S for edit mode came back as
  a bare S and put the screenshot tool in hand, intermittently - only
  when no frame had recorded Ctrl and Alt as held, since a binding fires
  on exactly the modifiers it names. `OverlayApp::OnOverlayShown` clears
  ImGui's event queue and key state, so a showing starts from no input.
- **A key-up is swallowed only if its key-down was.** The hotkey that
  turns edit mode on is pressed before any hook exists; its key-ups then
  arrived under the hook and were swallowed, so Windows never learned
  Ctrl and Alt came up and ImGui saw Alt held for the rest of the session
  - and the game underneath got the same stuck state. The grab records
  which downs it took, seeds its modifier record from `GetAsyncKeyState`
  when it starts mid-chord, and injects still-held modifiers back to the
  OS when it ends mid-chord (only modifiers: handing back every swallowed
  key would type its letters into whatever has focus). Those are handed
  back as the very keys that were held, left or right, and only the ones
  whose down it took: the generic keys it used to send are the left ones
  to Windows, so AltGr+O - Ctrl+Alt+O on a German keyboard - left the
  left Alt down on the whole desktop. The rule has a second half: a
  key-down for a key Windows already holds, whose down the grab did not
  take, is the repeat of a key held since before it began, and is
  swallowed without being recorded. Recorded, it made the key's up the
  grab's to swallow, and a W held to walk kept walking after the overlay
  was gone. Only for a key the grab takes, though: while the input
  options HUD took the digits alone, every other key and a digit held
  from before were the game's, repeats and all. Found in the next review
  on 2026-09-27: the rule was asked ahead of the HUD's, and a Backspace
  or an arrow held in the game acted once. The Behavior panel brought the
  digits-only case back (`SetPanelDigits`), and asks the same: a key held
  from before is the game's. A hotkey fires on a press, never on its repeat, as
  `RegisterHotKey`'s `MOD_NOREPEAT` does; held a moment too long, the
  edit hotkey opened the overlay and closed it again. Mouse buttons follow
  the same rule, so a drag in the application underneath ends there when
  its button comes up. Measured with injected input against the build
  before these: left Alt stayed down after AltGr+O, a repeated key stayed
  down, and the held hotkey left the overlay closed.
  The hook comes down after the grab ends, on its own thread, and in
  between it takes nothing and records nothing (`keyboardGrabbed_`); a
  modifier let go of in that gap had its up reach a Windows not yet handed
  the down, which then stayed down, so an up the hook lets by there is
  handed back after the down. Found in review on 2026-09-27 (the gap) and
  in the next one (what it left recorded); both are microseconds wide,
  and tested through seams, not by hand.
- **Held while it hides the overlay, a hotkey brings it back once** -
  known, and left so. The grab took the letter's down, and hands back
  only the modifiers when it ends, so Windows has not seen the letter go
  down: its first auto-repeat is a fresh press of the chord to
  `RegisterHotKey`, which `MOD_NOREPEAT` cannot tell apart. The grab that
  starts then counts the letter as held from before it, so its later
  repeats are swallowed without firing and its up reaches Windows: the
  overlay stays up, and nothing is left held. It costs one extra showing
  (and screenshot, with Freeze screen on). Keeping the hook until the key
  comes up, or ignoring a second hotkey within a second of the first,
  would stop it, at more cost than it has.
- **Taking the keyboard takes the Windows key too** - known, and left so
  until use gives a reason to change it. While the grab takes every key -
  keystroke holding on, the default while the game keeps focus, or a
  text field open under the grab - the Windows key goes to the overlay
  with the rest, and so does Alt: Start, Win+E, Win+Shift+S and Alt+Tab
  do nothing until edit mode ends. Ctrl+Alt+Del never reaches a hook.
  Letting the Windows key through would hand the shell's shortcuts to it
  mid-edit, where the window one opens takes focus from the game (see
  "What is a Windows limitation, not a bug"). The key never reaches
  Windows' key state either, and the grab keeps no record of it, so
  `HeldModifiers` reads it as up while the keyboard is grabbed: a press on
  empty canvas with it held makes a snippet, where with focus taken it
  makes none (`CreationTriggerFor`).
- **Keys that go up on another desktop are taken as released.** The
  Ctrl+Alt+Del screen, the lock screen and a UAC prompt are desktops of
  their own, and a low-level hook of this one is not called there. Found
  in review on 2026-09-27 and confirmed by hand: Ctrl and Alt held until
  the Ctrl+Alt+Del screen came up had their downs swallowed and their ups
  go unheard, so after Cancel the grab's record still held them. A bare S
  matched Ctrl+Alt+S and hid the overlay, and the downs handed back as it
  hid stayed down system-wide - from then on a bare S showed it again,
  in the game as well, until Ctrl and Alt were pressed once more. The
  hook thread now listens for `EVENT_SYSTEM_DESKTOPSWITCH` while it
  holds a hook, and forgets every key it swallowed the down of: the overlay is told each came up,
  and nothing is left to hand back (`InputLeftOnAnotherDesktop`). A key
  still held on the way back comes up later through the hook, as an up
  whose down it has no record of, which it passes on and Windows
  ignores. `GetAsyncKeyState` cannot be asked instead: Windows never saw
  a swallowed key go down, so it reads every one as up. Checked by hand
  with the fix: the same steps, and a bare S was a bare S. Mouse buttons
  the same: the raw input the overlay hears them from is not delivered
  there either. Found in the next review on 2026-09-27 and confirmed by
  hand: a snippet dragged into the Ctrl+Alt+Del screen, the button let
  go there, followed the pointer back on the desktop until the next
  click. The switch now tells the overlay every button it holds came up.
  Without the grab the same comes of the mouse capture taken away while
  a button is held - Alt+Tab mid-drag - whose up then goes to the other
  window: the overlay window takes the loss as the button going up,
  where the pointer was last (`WM_CAPTURECHANGED`), and tells each up
  once, however many say so.
- **The hooks stand down when the app thread stops.** They swallow the
  machine's mouse and, with forwarding off, its keyboard, whatever the app
  thread is doing, and the way out - the hotkey - is posted to that same
  thread. Hung there, the machine had no input short of Ctrl+Alt+Del. The
  window stamps a heartbeat every frame it renders (at least four a
  second while it is up), and with none for two seconds both hooks pass
  everything through until one comes. Checked by suspending the app
  thread with the overlay up: input stayed with the overlay for half a
  second, reached Windows after three, and was taken back once the thread
  resumed. A key or button that passes through is the system's from then
  on. The hook thread notes that as each one goes past: the grab's
  record of Ctrl, Alt and Shift follows it, the up of a key whose down
  was swallowed before the stall is no longer swallowed, and the overlay
  is told that key went up. A checkpoint in edit mode on a disk written
  flat out is such a stall too (see "In edit mode, only while the hand is
  still"): made after five seconds of rest, it can outlast two, and what
  the hand does in the rest of it - a click, a key, a note's typing - goes
  to the game behind the overlay. Left so on purpose (review of
  2026-10-08): the watchdog cannot tell a slow disk from a hung thread,
  and should not; the only way out is a checkpoint off the app thread,
  which would put a lock into a store that is single-threaded by design,
  and the first edit during it would wait all the same. It takes a
  saturated disk, a rest and a move inside the checkpoint's last part.
  The first version worked this out after the
  stall from what the system said was held. The system never saw a
  swallowed key go down, though, so a key held through the stall looked
  let go, and one pressed again during it still looked ours: its up was
  swallowed, and the key stayed down system-wide.
- **Typing needs the keyboard, not focus.** ImGui implements text editing
  from key events; what it cannot do is turn a virtual key into a
  character, which is the layout's job and arrives as `WM_CHAR` only for
  a focused window. The grab closes that gap with `ToUnicodeEx` against
  its own keyboard state (the thread's reports nothing held, so every
  letter would come out unshifted), the overlay window is registered
  wide so a posted `WM_CHAR` carries a UTF-16 unit, and
  `TranslateMessage` is skipped for the key-downs the grab posts or every
  letter arrives twice. A chord with Ctrl alone, Alt alone or a Win key
  makes no character; only Ctrl and Alt together do, which is AltGr.
  Found in the next review on 2026-09-27, and seen by hand: only Ctrl
  was left out, and Alt+E or Win+E typed an "e" - Windows makes a
  `WM_SYSCHAR` of Alt+E, which no text field takes. IME composition genuinely needs a focused window
  and still borrows one. The event loop takes and dispatches its
  messages with the wide calls too. Found in the next review on
  2026-09-27: through the ANSI ones every `WM_CHAR` went to a code-page
  byte and back, one UTF-16 unit at a time, and an emoji's two halves,
  no character of any code page each, arrived as two question marks.
  An emoji still does not reach a name, though, typed or pasted: ImGui
  is built with 16-bit characters (no `IMGUI_USE_WCHAR32`), and takes
  anything past U+FFFF as U+FFFD. Nor could it draw one - Manrope has
  no emoji, nor Japanese or Chinese, and ImGui draws `?` for a glyph
  the font lacks. Both are left for now.
- **Counter raw mouse input** banks the exact negation of every
  physical movement, against the raw device deltas read through an
  `RIDEV_INPUTSINK` registration: negating hook-derived screen
  coordinates removed ~27% of the motion, negating device deltas ~98%.
  Corrections are stamped in `dwExtraInfo` so the hook recognizes and
  swallows them (they still reach the game), since passing them through
  corrupted the next movement's delta and jittered the pointer. Anything
  with anti-cheat discards injected input outright. Kept, labeled
  experimental, and off by default: it injects input into whatever is
  underneath, which is for a game's profile to ask for.
- **The bank is settled in steps**: whenever it passes a threshold on
  either axis, and once more as countering ends. The threshold is a
  Behavior setting, per profile, in device counts (default 100), because
  how many degrees a count turns the camera is up to each game's
  sensitivity. Settled per frame, as it first was, the camera visibly
  shook, and the pointer paid for it too. Windows 11 caps raw input to a
  background listener at about 125 messages a second and merges the
  rest; each correction reaches our own sink as one of those messages.
  Measured with a steady 125 Hz source: 200 reports arrived as 200
  messages alone, as ~120 once per-frame corrections were added - two
  or three reports merged and up to 16 ms late, so the pointer stood
  still and then jumped. A zero-length injection did the same, and so
  did injecting from the sink's own thread: it is the message count. A
  1000 Hz mouse is over the cap regardless and arrives as evenly spaced
  merged messages, which is why it never showed there. Settling only at
  the end was tried next and fails differently: a camera that reaches
  its pitch limit drops part of the movement, and the correction then
  overshoots by that part. A threshold bounds how far the camera can
  wander from where the overlay found it. In between it does wander,
  which a frozen screen hides.
- **The last correction goes out while the game is still covered.**
  Sent as the window went away, it reached the game in time but showed a
  frame or two later, so hiding the overlay revealed the wandered camera
  for a moment before it snapped back. Hiding and switching to view-only
  (which drops the frozen screen) now settle the bank first and wait
  80 ms, two to three refreshes of a 60 fps game, before uncovering it.
  Measured: the correction reaches a raw-input listener about 145 ms
  before the window is gone.
- **Against the shake of per-frame countering**, four things were tried
  before settling in steps and are gone: injecting per report instead of
  per frame (cut the window as intended, changed nothing in a real
  game); a dedicated high-priority sink thread
  (measured 15 ms median lateness from the render loop, tried in a game,
  worse, reverted); an integral term aiming at the accumulated total (a
  feedback loop with dead time and no damping; oscillated wildly - do not
  reintroduce it); and `BlockInput`, which settles the question: elevated
  it stops the game's camera dead and blinds the overlay at the same
  moment, both channels, so the raw stream can be taken from a game but
  never selectively. What remains is take focus, counter-inject, or a
  kernel mouse-class filter driver, which is exactly the shape anti-cheat
  is built to notice and has not been undertaken.

### What an elevated application does to all of this

None of it reaches an application running at a higher integrity level than
the overlay, which in practice means anything started as administrator.
Windows cuts a lower-integrity process out of such an application's input
completely: the low-level hooks stop being called and the raw-input sink
stops receiving reports. Measured over Task Manager on an account with
admin rights, with every input option on, not one number in the input
debug overlay moved, and the overlay's own shortcuts were dead with it.

The one thing that survives is `RegisterHotKey`, because with the keyboard
hook never called there is nothing left to swallow `WM_HOTKEY` - so the
hotkey that puts the overlay away still works, which is the difference
between a limitation and a trap.

Task Manager is the everyday example and also a reminder of why this is a
comparison rather than a test: it asks Windows for the highest level it
can have, so it comes up elevated for an administrator and ordinary for
everyone else. On the second kind of account it is level with the overlay,
nothing is blocked, and there is correctly nothing to do. The same holds
when the overlay itself was started as administrator.

Turning raw mouse input off recovers the pointer and nothing else: without
the grab the cursor arrives as ordinary messages, but the keyboard hook is
what carries shortcuts to a window that deliberately has no focus, and it
is blocked just the same. Taking focus is the only thing that restores
both, because it leaves no higher-integrity foreground window to be shut
out of.

So the overlay asks where the foreground application sits relative to it -
`ForegroundIntegrity`, read in `UnderlyingApplication`, where the process
handle is open anyway. The measurement that shaped it: of every process on
one machine, seen from a medium-integrity process, 91 refused the handle
outright and 36 granted it, of which 35 answered the integrity query and
one refused the token. The refusals are processes owned by another
account, not processes above us - the limited query right is granted
across integrity levels for the same user, which is why an elevated Task
Manager reports its executable name like anything else.

Taking focus is decided once per showing, beside the profile, and not per
mode. View-only wants none of that input and would rather leave the
foreground alone - but it and edit mode switch in place, without coming
up from hidden and without asking again, so the answer view-only settled
on is the one edit mode inherits. Deciding it per mode would leave edit
mode deaf two keypresses into the very case this is here to fix.

That asymmetry is the whole design. A process that cannot be read is
`Unknown`, and `Unknown` is acted on as "leave it alone" rather than as
either answer. Treating it as above us would take focus from exactly the
application that must never lose it: a game behind an anti-cheat driver
refuses that query in precisely the same way an elevated tool would. Only
a positive reading forces anything.

The option dependencies are enforced, not documented: keystroke holding,
raw input and countering need the game to keep focus (with focus taken
the ordinary way the game has already stopped receiving input, and the
hooks would install a system-wide chokepoint for nothing), countering
needs raw input, and the software pointer is independent of all of them.
An option whose precondition fails keeps its stored value and has no
effect, and every reader asks availability, not storage - the stored
value of an option that cannot take effect is not evidence of anything.
Settings and the Behavior panel gray such a row.

### Freezing the screen instead of out-arguing the game

`freezeScreen` sidesteps all of the above: on entering edit mode the
whole display is captured and drawn beneath everything else. The game
keeps running and turning its camera; it stops being what you look at.
This is what ZoomIt does, minus the part where ZoomIt takes the
foreground. Captured fresh on every entry and released on hide. The
known failure is a game in exclusive fullscreen, where a GDI capture can
come back blank; ZoomIt keeps a Windows.Graphics.Capture path for the
same reason, which is the fallback to reach for if it turns up.

### Screen capture

`CaptureRegion` excludes the overlay from capture
(`WDA_EXCLUDEFROMCAPTURE`, for the moment of the capture only),
`DwmFlush`es so the next composition pass has happened, `BitBlt`s with
`CAPTUREBLT` so other applications' layered windows are included,
deselects the bitmap and reads it with `GetDIBits` (which documents that
the bitmap must not be selected into a DC while it is read, and whose row
count is checked rather than taken as nonzero), and converts GDI's BGRA to
RGBA once, setting every alpha byte to opaque - the fourth byte of a 32bpp
DIB is not an alpha channel, GDI leaves it undefined. It used to hide the
overlay instead, which flickered and, worse, handed activation to the game
underneath and took it back: a focus gained and lost that the game had no
part in. `BitBlt` honors the exclusion (measured, and a test captures a
window of known color with and without it). Before Windows 10 2004 the
overlay is still hidden for the moment. Those builds are told apart by
their build number, not by the call failing: they accept the flag and
treat it as `WDA_MONITOR`, which captures the overlay as black.
The exclusion is not left on: a screenshot or a stream the user takes of
their own screen should show the overlay. The rectangle goes through `ClientToScreen`, so a capture
comes from the overlay's display rather than from wherever its
coordinates land on the primary. Textures are `D3D11_USAGE_DEFAULT`
rather than immutable so one can be updated in place (`TextureCache::Get`).

### Picture scaling

Every picture in a snippet is drawn through `DrawPicture`, resampled the way
Settings > Appearance says (`AppConfig::imageFilter`). Bilinear is the
default and what every picture had before there was a choice; drawn
below about half size it lands on one texel in two or three and text
breaks up. Nearest is for pixel art and small captures blown up.

**One callback, a shader swap.** The UI stays out of D3D:
`IOverlayWindow::ImageFilterCallback` hands it a draw callback, which
`DrawPicture` puts in front of the picture with the filter as user data,
and ImGui's own `DrawCallback_ResetRenderState` after it. Bilinear adds
nothing to the draw list. Nearest is ImGui's own nearest sampler. Bicubic
and Lanczos replace ImGui's pixel shader for that one draw and leave its
vertex shader, blend and viewport alone, so the swap is all there is to
undo. The callback runs inside `RenderTo` and nowhere else, which is how
a static function with nothing but the draw command finds its renderer.

**Shrinking is the hard part.** A kernel the textbook size (4x4 texels
for Catmull-Rom, 6x6 for Lanczos-3) aliases just as bilinear does when
shrinking: it has to widen by the reduction so every texel under a pixel
counts, and a 4K screenshot shown at a tenth of its size would be 60x60
taps a pixel. So a texture has a full mip chain while one of the two is
chosen (made only for them; see below), the shader reads
the level just above the target size and widens the kernel by what is
left - at most 2x, so 12x12 taps at worst for Lanczos and far fewer for
a picture shown near its own size. The mips are box-filtered, which is
where some quality goes; this keeps most of it for a fraction of the
cost. Other routes were weighed: a pre-scaled copy per snippet, redone
whenever its size changes, is the best quality but a cache to invalidate
on every resize and stroke; a separable two-pass filter needs an
intermediate target per picture per frame.

**The mips are built by hand.** `GenerateMips` averages what it is
given, and the pictures are straight alpha: one with transparent parts
is (0,0,0,0) around what it shows, so a plain average darkens every edge
toward black as the picture shrinks. `BuildMips` averages premultiplied instead,
one full-target triangle per level, and the resampling shader sums
premultiplied too, then clamps - both kernels have negative lobes that
ring past 0 and 1 at a hard edge. A texture's chain is rebuilt once at
the start of the next frame after it is created or updated, however many
updates there were in between. Each texel of a level averages the part
of the level above that it covers: two texels along an even side, and
parts of three along an odd one, weighted by how much of each falls
inside it. A plain 2x2 box, which this was until 0.3.1, left the last
row or column of every odd level out of the chain.

**Only Bicubic and Lanczos get a chain.** It costs a third more memory
per picture, and Bilinear and Nearest never read it: ImGui's samplers
clamp to the top level. Each frame the overlay tells the window which
the setting asks for (`IOverlayWindow::SetMipmapsWanted`), and a change
moves `TextureGeneration`, so every texture is made again, with the
chain or without it, the way it is after a lost device: pictures from
the library, the frozen screen from the pixels kept. Switching between
Bicubic and Lanczos keeps the textures as they are. Until 0.3.1 every
picture had a chain whatever the filter, so that the default paid for
what only the other two use.

### Displays

`EnumDisplayMonitors` gives rectangles, the primary flag and (with
`GetDpiForMonitor`) the scale; `QueryDisplayConfig` gives the EDID name,
refresh rate and the device path, joined by GDI device name. The device
path is the id because it survives a restart, while `\\.\DISPLAY2` is
renumbered by Windows. The window is created and moved to its display's
rectangle, and `WM_DPICHANGED`'s suggested size is declined: the window
is exactly its display's size in physical pixels. The UI does not scale
with a display's scale factor, and there is no way to show the overlay on
several displays at once; that would be a window per display.

### What is a Windows limitation, not a bug

Win+E, Alt-Tab and other shell shortcuts reach their targets outside
normal focus routing, and once they open a window focus follows it.
While the input grab takes the keyboard - the default in edit mode
while the game keeps focus - they do not get there at all: the Windows
key and Alt go to the overlay with every other key (see "Taking the
keyboard takes the Windows key too"). Without it - keystroke holding
off, or edit mode taking focus - they work as they do anywhere, and only
a hook swallowing them could stop them. Exclusive-fullscreen games
sidestep all of this by not sharing the desktop, which an always-on-top
overlay deliberately does.

### Cursors

Windows has no stock pen, so the window builds one from `pen_glyph.h`: a
24x24 top-down 32-bit DIB section, an empty mask (alpha is the mask), and
`CreateIconIndirect` with a hotspot at the nib, sampling the glyph 4x4
per pixel. Built once; a failure falls back to the crosshair. The push
of a wanted shape is guarded on owning the cursor (`WindowFromPoint`),
not on being foreground, which in edit mode the overlay never is.

## Testing

Automated UI testing of a transparent, always-on-top Windows window is
impractical, so the strategy focuses on what can be verified without a
live compositor, in four tiers:

- **Core tests** (`tests/core/`, portable) cover the drawing model, the
  canvas model, persistence against a real library file in a temporary
  directory, the config, the session and the settings. They are where a test of new
  behavior belongs before any UI reaches it.
- **Headless app tests** (`tests/app/`) run the whole app - a real
  `OverlayApp` driven through a real ImGui frame - over the in-memory fake
  platform, with nothing drawn anywhere. ImGui needs a context and a font
  atlas, not a window or a GPU, so every path the app has is reachable
  from an ordinary test. Input arrives the same two ways it does in the
  real app: ImGui's own event queue, which widgets see, and the platform's
  input stream, which the canvas, the commands and the wheel run on. A
  key, a modifier or the wheel goes to both at once, as the real window
  sends it.
- **UI tests** (`tests/ui/`, debug preset only) add Dear ImGui's test
  engine, which drives widgets by name - "click the thing labeled
  Settings" - and fails when a widget is present but unreachable, the
  shape of every z-order bug this UI has had. Text is never an identifier
  (see "Every word in one file"), so rewording a label breaks no test.
- **Win32 tests** (`tests/platform/`) run the input grab against the
  real registration API and list whatever displays the machine has. CTest
  runs them one at a time, with a timeout: the hooks and the raw-input
  registration are process-wide state two copies would fight over, and a
  hook thread that never stops should fail the suite, not hang it. The
  rest of the backend needs manual verification: tray icon, hotkeys,
  focus returned on hide, idle CPU in Task Manager, a real capture that
  survives resize and disappears from GPU memory when its item is
  deleted.

A test's files go under its program's own folder in the temporary one,
`spickzettel-tests-<process id>` (`tests/support/temp_dir.h`), removed
as the program ends; a death test's child is handed the same folder.
Found in the next review on 2026-09-27: every test had a fixed name of
its own under the temporary folder itself, and two runs at once - two
worktrees - wrote each other's files. The one real hotkey a test
registers is skipped, not failed, when another run holds it.

`linux-tests` builds the portable core and its tests with GCC or Clang.
Worth running now and then even when working on Windows: MSVC is the
more forgiving reader, and core can drift for weeks into a shape only it
accepts. Two examples that happened: a braced default argument for a
nested aggregate (a hard error on GCC), and constructor initializers out
of declaration order (`-Wreorder`, which MSVC leaves off even at `/W4`).
Configuring a scratch MSVC build with `/permissive- /W4 /w45038` catches
most of this class without a Linux machine.

`windows-msvc-asan` is the debug build under AddressSanitizer, everything
in it compiled with `/fsanitize=address`, and is worth a run after
anything that touches what a failed write rolls back. A test that reads
or writes freed memory can pass in a debug build. The session's style
preview once held pointers into canvases a rollback had just replaced,
found by reading; its test, run against that code under this preset,
stops with a heap-use-after-free report. The crash dump's death test is
skipped there: the sanitizer takes the crash before the dump writer can.

## Dead ends, for the record

Things that were built, used and removed. Each is described where it
matters above; this is the index, so nobody spends an afternoon proving
one twice.

- **A drawing bar in place of the selection bar in drawing mode**, and
  the lit tool's button cycling its shapes. The way in was hidden and
  the buttons moved with the mode; replaced by one bar with both groups,
  whose lit tool puts itself down.
- **A right-click ring menu with favorite tool slots**, and the flat
  tool strip that mirrored it. Seven tools on three slots meant the slot
  wanted was usually not there. Replaced by six tools, a selection bar
  and drawing mode.
- **Per-item ImGui windows for chrome** (titlebar bands, handle margins,
  `InvisibleButton`s). Two hit-testers on two clocks; replaced by one
  resolver of the app's own over `NoInputs` layers.
- **A dedicated text-note item kind.** Could not combine with a drawing
  or a screenshot; replaced by a caption any item can carry.
- **A trash folder, then a trash library.** Replaced by a deletion mark
  in place and a Recently deleted list.
- **Deleted things shown in a read-only mode**, then **a Recently
  deleted list**, newest first, snippets and all. The first needed a
  read-only check at every edit; the second could not show where a
  restore puts a thing. Replaced by Show deleted, in place.
- **Profiles based on other profiles.** A third level nobody could see;
  replaced by exactly two levels.
- **One library file of JSON.** 42 MB rewritten every two seconds at
  fifty canvases; replaced by a directory tree that is its own index,
  and then by a save whose writes are bounded by what changed.
- **The library as a directory tree** - a directory per folder, canvas
  and snippet, records and pictures in them, rearrangeable in a file
  manager. Every change was several filesystem steps a crash or a held
  file could stop between, and the store grew a pending-removals file, a
  staging directory, a retired directory, a reconciling load and a
  fault-injecting test file system to survive that. Replaced by one
  SQLite file, whose transactions have no "between" (see "Why a
  database"). It compared snippets to what it had written by a hash of
  their fields, which had to be kept in step with the serializer by
  hand; each command now says which rows it changed (see "A write").
- **A debounced autosave** (2 s of quiet, 15 s at most), with a flush
  wherever no frame followed, a retry clock and a background timer of its
  own, pixels kept in memory for a capture not yet written, a recovery
  copy beside the library at exit, and a store that diffed every row
  against what it had last written. All of it carried changes that were
  in memory and not on disk; replaced by writing every command as it is
  made.
- **Texture handles in the model**, kept by four owners - the pictures,
  the frozen screen, the Overview's thumbnails, the stroke rasters -
  each releasing on occasions of its own and each to be told of a
  replaced device, with checkpoints, rollbacks and copies carrying them
  along. Replaced by one cache that lets go of whatever went a frame
  undrawn.
- **PNG for captures.** Six to twenty times slower than QOI on this
  app's own screenshots. The PNG codec (stb) was kept for importing and
  exporting pictures that nothing ever imported or exported, and went
  with the move to a database.
- **Painting pixels** (`AppConfig::paintPixelsInsteadOfStrokes`): the pen
  and the erasers writing into a pixel layer of each snippet's own
  instead of making strokes. It made pictures mutable, and so needed a
  re-encode of every changed layer on every save, tiles of pixels on the
  undo stack (capped at 128 MB), a texture release that had to wait for
  the write, a revision counter beside the generation, and a copy that
  deep-copied pixels and read back released ones. Removed so that a
  picture is written once and never changed, ahead of moving the library
  into a database.
- **Loading every canvas's textures at startup.** 1.6 GB of VRAM behind a
  game for fifty 4K captures; replaced by per-canvas residency.
- **A global undo stack.** Undid strokes on canvases not on screen;
  replaced by a stack per canvas.
- **Bursts merged after the fact**: a step filed within a second of the
  last of its kind, with the history's revision unchanged, merged into
  the step on top. It needed the revision to keep a drag in between from
  being merged into, and could not be canceled. Replaced by a burst as an
  interaction, filed once it is over.
- **Gestures as a field per kind**, excluding each other only by the
  order the handlers asked in, then as a variant in `OverlayApp::Hand`,
  ended early by a synthesized release that made what a release makes.
  Replaced by interactions on the input machine's Gesture level,
  interrupted or canceled.
- **Where a key reaches, one switch of conditions per command**
  (`KeyReaches`, three key handlers before it), and a request flag per
  popup. Replaced by the stack of levels, and by the effect queue.
- **Lifecycle events** offered to the input machine as the overlay went
  away, came up or turned view-only. No level answered them; the scope
  did the work, and does it alone (`OverlayApp::Settle`).
- **The stack set by the draw**: nineteen calls in eight files each
  bringing a window to the front as it was drawn, the order being that
  of the calls. Replaced by one pass after the draw, in the order of
  `docs/VIEW_LAYER.md`.
- **Settings written in place** (`Mutable()`), beside the overridable
  ones' setters and the tray's own writes of the hotkeys. Replaced by
  one edit path through the catalog's rows.
- **`SetCursorPos` to drive the real cursor under a grab**, fractional
  pointer drawing, an integral term for counter-injection, a dedicated
  sink thread, `BlockInput`, and a null-device-handle fallback for
  recognizing injected input. All in the input grab section.
- **Three stroke render modes**, a setting to choose between them, and a
  stroke bitmap per snippet for the one that composited strokes right.
  Replaced by one way of drawing, a depth test and a layer per snippet,
  in "Drawing strokes".
- **A Linux dev harness** (GLFW/OpenGL, an ordinary window showing the
  same UI) and a **MinGW cross-compile preset**. Useful once for
  iterating without a Windows machine; not carried into this repository.
  Core stays portable and the `linux-tests` preset keeps that honest;
  what the cross build taught is under "Cross-compiling".
