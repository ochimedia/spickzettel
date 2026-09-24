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
├── ui/         # OverlayApp: the overlay as drawn with Dear ImGui. A view
│               # of the session.
├── app/        # TrayController: owns settings, session and overlay, and
│               # moves the window between hidden, edit and view-only.
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
tests; `windows-msvc-demo` is the release build with a permanent demo
watermark compiled in; `windows-msvc-prerelease` is the release build
with a not-for-redistribution notice at every start; `linux-tests`
builds the portable core and its tests on a Linux host.

The presets whose build is handed out copy the finished exe straight
into `dist/` at the repo root (`SPICKZETTEL_COPY_TO_DIST`), each under
a name of its own (`SPICKZETTEL_EXE_NAME`): `Spickzettel.exe`,
`Spickzettel Prerelease.exe`, `Spickzettel Demo.exe`. Side by side in
one folder, and named for what they are, rather than each
`Spickzettel.exe` in a folder of its own: the name goes wherever the
file goes, into a download folder or an email, where the folder it came
from does not, and Task Manager shows which kind is running. The name
is the linker's output name, not a rename of the copy, so the PDB is
named to match and is the name the exe records for it - what a debugger
looks for when it reads a dump. Debug is not copied: it is built for
its tests, and nobody is handed it.

The copy is a target of its own that runs on every build and copies
only when the exe differs, so a copy deleted by hand comes back without
a relink. A copy that is running cannot be overwritten, and fails the
build just as a running build-tree exe fails the link.

`scripts/clean_build.cmd` is the release build: it deletes the Windows
presets' build trees and `dist/`, then configures, builds and tests
every preset in turn and stops at the first failure. Nothing an earlier
build left behind - a stale object, a cached option, an exe in `dist/`
from before a rename - can end up in what is handed out. It finds
Visual Studio itself with `vswhere`, so it runs from a double-click.

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
`%APPDATA%\Spickzettel\crashes\`, named after the version line and the
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
20 seconds, not for ever: a crashed thread holding the loader lock keeps
a new thread from ever starting, and a process hung in its crash handler
is worse than one with no dump. A second crash waits for the first to end
the process, rather than ending it under the first dump half-written -
unless it is the writer's own, which gives that dump up. And the main
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

### Build-time configuration: version, flags, embedded text

Three things are decided when a binary is *built* rather than when it
runs, and all three land in `build/<preset>/generated/` through
`cmake/BuildInfo.cmake`:

| Header | Holds | Regenerated | Included by |
| --- | --- | --- | --- |
| `build_config.h` | `kVersion`, `kDemoMode`, `kPrereleaseNotice` | configure | `build_info.h`, so widely |
| `git_stamp.h` | `kGitDescribe` | **every build** | `build_info.cpp` only |
| `about_text.h`, `notices_text.h` | `ABOUT.md`, `THIRD-PARTY-NOTICES.md` | configure | `build_info.cpp` only |

The third column is the design: the git stamp changes with every commit
and the embedded texts are by far the largest, so both sit behind
functions in a header that declares but does not contain them. In
`build_config.h` they would rebuild everything that merely wanted the
version number.

Feature flags are `constexpr bool`, not `#ifdef`. Both branches of an
`if (build::kDemoMode)` are compiled and type-checked in every
configuration, where an `#ifdef`-ed branch nobody builds for months has
quietly stopped compiling; the optimizer removes the dead side either
way. Demo mode is deliberately not a setting: a watermark that can be
switched off in `config.json` is not a watermark.

The prerelease notice follows the same reasoning. A prerelease build
shows a message box at every start saying it is not for redistribution,
and `VersionLine` names it a prerelease, so the About tab says so too.
The box is native rather than drawn by the overlay: on most starts the
overlay is not shown at all, and on a first run it comes up fullscreen,
topmost and in edit mode. So `WinMain` shows it before the tray
controller initializes - before the overlay exists to cover it or take
its input. A second copy started by mistake shows the notice before it
finds the first one running and stops; that is the price of the
ordering.

The version lives in `VERSION` at the repo root, read by CMake and fed to
both `project()` and the header, so a release script can bump it without
parsing CMake. (A file named `VERSION` can shadow `#include <version>` on
a case-insensitive filesystem. It is safe here only because the repo root
is never an include directory; do not add it to one.)

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

`LICENSE` covers Spickzettel itself, which is proprietary.
`THIRD-PARTY-NOTICES.md` covers everything that ends up inside the
binary, and is compiled into it and shown on the About tab, because MIT,
ISC and the OFL all require the notice to reach whoever received the
software; a text file next to the executable is one copy away from not
doing that.

What is in a release binary, and why each is allowed in a paid,
closed-source one:

- **Dear ImGui**, **nlohmann/json**, **QOI**: MIT. Reproduce the notice.
- **stb_image / stb_image_write**: dual MIT or public domain; the notices
  file takes the MIT branch and says so.
- **Manrope**: SIL OFL 1.1, which permits bundling and selling a font
  *with* software provided the license travels with it, and forbids only
  selling the font by itself.
- **Icon designs**: ISC (Lucide) and MIT (Feather). The SVGs here are
  drawn from this project's own path data, but both licenses cover the
  designs.

googletest and imgui_test_engine only build or test the app and are not
in a release binary, so they are not in the notices.

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
server, an archive that differs by a byte is refused. json is the
exception to the URL pattern, fetched as the `json.tar.xz` its releases
publish for this use - the headers and the CMake files, nothing else.
The archives were compared with the clones they replaced: the same
files, apart from the line endings Git's `core.autocrlf` had converted
and the ImPlot submodule the test engine's test suite pulls in, which
nothing here builds.

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
encoding), a `CaptureResult` carries the GPU texture *and* the CPU pixels
so persisting a capture needs no second OS call, and a texture is an
opaque `uint64_t` so no platform header ever names an ImGui type.

`pen_glyph.h` is the one small piece of drawing that lives here: the pen
pointer's outline, which both the software pointer (drawn by the UI) and
the Win32 cursor bitmap are built from, so the two pens are the same pen.

## Drawing model

`Stroke` is a polyline with a color and a width; `CanvasState` holds a
list of finished strokes plus at most one in progress. Both are dumb on
purpose: they record what they are given, so a shape tool can hand them
exact corners and a test exact points.

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

Why not rasterize instead: a bitmap erase gives up resolution
independence and turns undo into pixel diffs, for a problem that a
bounded piece of segment geometry solves while keeping every stroke a
plain, inspectable polyline. (Painted layers, below, are the case where
pixels are the right answer and are treated as such.)

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
a miter join while the turn is shallow, a round join past a limit of 2
half-widths (about 120 degrees), round caps, a disc for a dot, and a
seam join instead of caps for a closed path (the rectangle tool's). It
exists because ImGui's `AddPolyline` offsets each point along the
average of its adjacent normals and rescales by 1/cos² of half the turn,
clamped only at 100x the half width, so a near-reversal throws a spike
most of a hundred widths out of a wide pen; it also has only flat caps
and no round join.

The mesh is one connected strip whose neighboring quads share vertices,
so no triangle is drawn over another. That is what a *translucent*
stroke needs: every overlap is a place the color lands twice, which at
less than full opacity is a visibly darker patch. `StrokeMeshTest`
measures this as area, since a screenshot cannot tell a double-covered
pixel from a slightly darker one. A one-pixel anti-aliasing fringe is
carried in screen space so it stays a pixel wide whatever an item is
scaled to.

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

### Painting: the pixel brush

`PaintedImage` holds a pixel-authoritative layer's pixels and the brush
that writes them. The brush is a capsule per segment - the same figure
the tessellator builds - with coverage taken analytically from each
pixel's distance to the centerline, so round caps, round joins and
anti-aliasing come out of the arithmetic, a zero-length segment is a dab
and therefore a dot, and a line drawn in either mode is the same shape.
Only what can be done to it afterwards differs.

A stroke is a session, not a run of stamps. Compositing each segment as
it arrives would darken every overlap, and a brush moving a pixel at a
time overlaps almost entirely, so a translucent line would go opaque
within a few steps. Instead a stroke accumulates coverage into a mask
(taking the maximum) and recomposites each touched tile from the pixels
it held *before* the stroke began. Those saved pixels are exactly what
undo needs, so nothing is stored twice: `EndStroke` hands them back as
the undo entry's tiles, and `RestoreTiles` puts them back and returns
what it replaced, which is the redo state. Tiles are 64x64 (16 KB); a
stroke across a 640x640 layer touches four of a hundred, where a
whole-image undo entry would be 8 MB a step.

The rectangular eraser is the same session with a different coverage
function (`ExtendRect`), so it shares the mask, the tiles and the undo
entry. Erasing takes alpha and leaves color, so a half-erased edge
fades instead of shifting toward black.

A layer's size is capped at 4096 on a side, and the cap is applied to
the *resolution scale*, not to the bitmap: `FitResolutionScale` lowers
the scale uniformly until the longer side fits, and because every
coordinate on the way in is multiplied by that same scale, a 5120-wide
capture paints where the pen is. Clamping the bitmap's width and height
while still mapping coordinates 1:1 cropped everything past the cap and
stretched the rest, putting a stroke a quarter of the way across the
item from the pen.

A painted layer without pixels gets fresh, blank ones when a brush
first touches it - but only if it has no file, or its file is gone. A
file that is there and could not be read when the canvas came back
(another program holding it) is read again as the brush starts, and
while it still cannot be, the brush paints nothing. Taking the failed
load for an empty layer painted on blank pixels, and the next save wrote
them over the drawing in the file.

## Canvases, items and folders

`Item` is a snippet: freehand strokes over a stack of layers, at a
`rect` on screen. `Canvas` is an independent collection of items whose
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

### Layers

An item's picture is a list of `Layer`s composited bottom-first, with
strokes and the caption always on top. Every item has an Image layer
(its screenshot, or a transparent fill for a drawing), and gets a
Painted layer on top the first time something is painted on it. The rule
that keeps the kinds honest: a layer is either vector-authoritative (its
pixels are a rebuildable cache) or pixel-authoritative (its pixels are
the document, persisted and undone as pixels), never both. Strokes are
the first kind; both layer kinds are the second.

A texture handle has single-owner lifetime even though `Item` is a
freely copyable struct, so a copy detaches its layers: the handle is
reset and reloaded from the file, painted pixels are deep-copied, and
the clone starts *dirty* because it has no file yet and its pixels are
the only copy.

### Strokes live in the item's native space

Strokes are stored in a fixed coordinate space set at creation
(`nativeW/nativeH`), not in screen space, so a stroke drawn at one size
still looks right after the item is resized. `ScreenToNative` is the one
transform for everything that lands a gesture on an item - the pen, the
erasers, the brush - so they cannot disagree about where the pen is.

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

### The floor is a shape

Items cannot shrink below 90x70. Applied as two independent per-axis
clamps that floor reshapes anything that is not 9:7: a 16:9 item reaches
the height floor at 124x70 and then goes on narrowing to 90. Reaching
one floor has to stop the whole resize, so `MinimumSizeForAspectRatio`
turns the two numbers into one floor on the item's own ratio, and the
derived axis is deliberately not re-clamped. The same floor applies in
the display sync and in fullscreen restore, and a free (Shift) resize,
which is meant to reshape, keeps the plain per-axis pair.

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

Whether a handle keeps the shape is `Item::keepAspect`, set from the
defaults when the snippet is made and changed in its popover; Shift does
the other. It used to follow from whether the snippet had text, on the
reasoning that a box of text is a box whose shape is the point of
resizing it. But text is a caption any snippet can carry, so typing one
into a screenshot changed what its handles did, with nothing on screen
to say so. A property says it, and can be set either way on purpose. A
record from before the field reads back as the old rule had it (no text:
kept), so nothing already made starts behaving differently.

### What a new snippet starts with

Settings > Defaults holds the starting values for a new snippet, by kind:
its shape, its two opacities, and a drawing's background color - plus
the style of text typed into it later. They are applied in
`OverlayApp::ApplyCreationDefaults`, which every way of making a snippet
passes through, and they are only ever a starting point: each is the
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
canvas would not bring it back either. It erases without the texture
sync a delete for good ends with: the library is opened before the overlay
window has made its device, so that sync would load nothing and still
record the current canvas as loaded, and its pictures would stay
placeholders until the canvas changed.

Folders and canvases can be given a retention period
(`AppConfig::purgeDeleted`, on by default, and `purgeDeletedAfterDays`, 14):
once the library is opened, `Session::EraseDeletedBefore` deletes for good
whatever carries a mark older than the period, by the same erase and for
the same reason. Only its own mark counts. A canvas that went with its
folder has none and goes when the folder does; one marked on its own
before the folder went can go first, which leaves the folder as a
Delete permanently by hand would. It runs at startup only: an instance
left running for days keeps what is due until it is next started, which
costs nothing but the wait. Being on by default, it says what it does: the
delete confirmation says for how many days a thing can be restored, a
deleted folder or canvas says from which day it goes, and what a start
deleted for good is counted in a message the next time the overlay comes
up, since the start itself happens while nobody is looking.

Two designs preceded this. A reserved Trash folder inside the library
grouped three structurally different things under one "dig through the
bin" model that fit none of them. A trash that was a second library of
the same shape needed a delete to transfer records, directories and
textures into it with scaffolding containers on the far side, ids
reserved across both, a tab that switched which library the whole
overlay viewed, and a staging rule for a capture deleted before its
first save. A stamp in the record replaces all of it without moving
anything.

### Ids and names

Ids are random six-character base36 uids, checked against everything
the library holds. Random rather than counted because every counted
library starts at 1, so two libraries built independently collide on
nearly every id, and moving a directory between them - which the on-disk
tree is meant to allow - would be a guaranteed conflict. Lowercase only,
since the ids become directory names on case-insensitive filesystems.

A folder or canvas nobody has named is called for the moment it was
made, "2026-09-07 22:36:14": a counted "Folder 2, Folder 5" says nothing
about which is which a week later. Items keep numbered names
("Screenshot 3"), which is all an item name is asked to carry. Names are
not identity; the slug a directory is named by carries the id.

## Persistence: the on-disk library

Everything the overlay shows survives a restart, written to
`%APPDATA%\Spickzettel\library\`. There is no save action anywhere in
the UI; the session decides when to write (see "Session"). Loading
happens once, at startup.

### Layout

```
library/
  library.json          - the format version, currentFolderId and
                           currentCanvasId. Nothing else: the tree is
                           the rest.
  pending.json          - what was deleted for good and is not wholly
                           gone from the disk yet, by uid, and where
                           anything moved out of it belongs. Only there
                           while something is.
  folders/order.json    - the folders' uids, in order
  folders/<folder>/
    folder.json         - that folder's id and name
    order.json          - its canvases' uids, in order
    <canvas>/
      canvas.json       - that canvas's id and name
      order.json        - its snippets' uids, back to front
      <snippet>/
        item.json       - rect, strokes, layers, anchor, note text...
        <uid>.qoi       - its captured pixels, if it has any
        <uid>.thumb.qoi - a 256px copy for the Overview's thumbnails
  staging/              - where a capture waits between being taken
                           and the next save moving it into the
                           snippet that names it
  retired/<folder>/...  - what a save found the library no longer
                           holding, set aside whole rather than deleted
  retired/staging/      - staged pictures no snippet named, set aside
                           for the same reason
```

### Format version

`library.json` carries `LibraryStore::kFormatVersion`, stamped by every
save. It goes up whenever a build writes something an older one would
misread or drop. A library stamped higher than the build knows is
refused whole. `TrayController::Initialize` checks before the tray icon
and the app does not start, saying why in a message box of its own. The
store backs that up by itself: once it has seen a newer stamp, `Load`
reads nothing and `Save`, `Remove`, `SaveImage` and `SaveThumbnail` all
fail, so a path that forgot to ask still cannot write. Opening read-only
was the alternative, and was not worth its cost: every write path would
need a read-only state, for a case whose fix is running the newer
build. A `library.json` with no version, or none readable, counts as
not newer.

`config.json` carries a version of its own, which nothing reads yet. A
newer build's settings read by an older one lose only the fields the
older one does not know, the next time it writes the file - a setting,
not the library.

Every directory is `<slug of its current name>-<uid>`. The readable half
is regenerated on every save so it stays true after a rename; the
trailing uid is the identity, so a rename is cosmetic and a failed one
costs a stale label. A *move* - a snippet to another canvas, a canvas to
another folder - is the same rename with a different parent, and one
that fails is not cosmetic: the tree is the index, so a record left under
its old parent is a move the next load undoes. The record is still
written where the directory is, so nothing in it is lost, but the save
reports failure, is retried, and is said on screen, where a failed
relabel is acknowledged. The first version reported both as success, and
a snippet moved while a picture viewer held its capture open was back on
its old canvas at the next start. Every id inside a record is spelled the
same six-character way, so a record and its directory can be matched by
eye.

The readable half is capped at 20 characters - the default timestamp
names fit whole. It was 40, and at 40 a snippet's files sit up to 141
characters of slugs below the library root: long names on a long profile
path, or in the recovery copy beside it (29 characters longer again), ran
past MAX_PATH, where a record is neither written nor read back and the
snippet was gone at the next start. The label is the only thing lost to
a shorter cap; the name itself is in the record.

A tree rather than one file because one file is rewritten whole on every
save: 50 canvases of ordinary drawing is a 42 MB document taking half a
second to serialize, on the render thread, every couple of seconds of
quiet.

**The tree is the index.** Nothing records which canvas is in which
folder; the directory it sits in says so, and where a record disagrees
with where it physically is, the filesystem wins. Rearranging the
library in a file manager while the app is closed is a supported way to
use it, so `Load` reconciles rather than validates: a directory without
a record is not ours and is left alone; an order file naming something
gone skips it, and anything present it does not name goes to the end
(the front, for snippets, since their order is z-order); a directory
whose readable half was renamed by hand keeps its place by its uid; two
directories claiming one id - what copying one produces - is not
corruption, the second gets a fresh id; a current-canvas pointer naming
nothing falls back to a canvas that exists. The same reconciliation is
what makes a half-finished save survivable: a crash mid-write leaves the
same kind of inconsistency a hand edit does.

The tree also invites files that are not ours, so what is read has a
budget checked before anything is allocated for it: a record is refused
unread past 64 MB (some seven hundred thousand stroke points on one
snippet, as the records are written), a picture past 256 MB of file,
16384 pixels on a side or 64 million pixels, with the picture's header
checked before the decoder is handed the bytes - a QOI header claiming
100000x100000 asked for a 40 GB allocation before that. The record
budget binds the writer too. `Save` wrote whatever a snippet had grown
to and counted it saved, and the next `Load` skipped the record whole -
the snippet gone, with nothing to say why. A record past the budget is
now not written: the save fails, the last record that fitted stays on
disk to load, and the warning along the bottom says a snippet is too
large rather than promising a retry that cannot succeed. Every float a record carries is read through one function that
turns a value too large for a float (`1e100` is valid JSON and infinite
as a float) into the field's default and holds the fields with a range
inside it, rather than letting one infinite coordinate poison every
bounding box it meets. Repaired, never refused: the rest of the record
is still the user's, and the next save writes the repaired value back.
The settings file has the same shape of budget: read past 1 MB it is
read as one that said nothing (`ReadConfigFile`), and the one setting
that took any positive number, the stroke width, has a ceiling it is
held to. Two budgets were considered and not added. There is no
aggregate limit on decoded pixels because the GPU textures are per
canvas: only the current canvas's pictures are resident, so the working
set is bounded by one canvas rather than the library, and a library of
a thousand screenshots costs disk, not memory. There is no separate
limit on the number of strokes, points or layers in a record because
the record's byte budget bounds all three at once, and a second limit
would have to be kept in step with the first for no extra safety.

A snippet's pictures live in its own directory, so moving a snippet is
moving one directory with no window where the record has moved and the
picture has not. `Layer::imageFile` names a *file*, not a path, and
`FindImage` turns it into a path through the owning snippet. A
library-wide filename-to-directory map fell behind whenever a directory
moved, and two snippets can legitimately hold files of the same name.

**Links are not part of the tree.** A symlink or junction inside the
library names something that may be anywhere on the disk, so what is
behind one is never the store's: `Load` skips linked directories and
does not look inside one, so nothing behind one enters the index, and
every path the store creates, writes, moves or deletes goes through one
check (`IsOurs`) that walks the whole path from the root down for a
link - at the moment of the write, move or delete, whether the path was
made just now or indexed at load. An indexed directory is trusted only
as far as answering "unchanged?" from the hash: the moment there is
something to write into it, it is checked like any other, so a junction
put in its place between two saves fails that record's write rather
than being written through. The check costs a filesystem call per path
component, and only what is written pays it; a no-op save pays nothing.
That covers the top-level directories too: a junction at
`folders/` fails every save outright, one at `staging/` refuses the
capture's write (the session keeps the pixels and the save keeps
failing, visibly), and one at `retired/` leaves what would have been
retired where it is. The first version checked only the directory a
record would be created in and took the roots on trust, and a junction
at either root had a save writing a whole tree outside the library. A
junction standing where a save would have to create a directory makes
that record unplaceable: the save reports failure and leaves the link
alone rather than writing through it. The library root itself is not
checked: a root that is a junction is how a library is moved to another
drive, and is supported. The checks are by path, not by handle. The
threat is a user's own junction (a snippet directory pointed at a folder
of notes, say) meeting an ordinary sweep, not a process racing the
store's own file operations; the latter would need handle-based
operations with reparse checks and is out of scope for a single-user
tray app writing its own `%APPDATA%`.

### A save is a plan

First everything the library holds is *placed* - its directory found
where the index says, or moved to where its current name says, or
created - and its record written; then whatever the index knows about
that the library no longer holds is retired; then pictures waiting in
staging are moved in with their snippets. Placing everything before
retiring anything is what makes a move a move: a save that swept each
canvas for strays as it went deleted a snippet dragged to a *later*
canvas, picture and all, before reaching the canvas it went to. Retiring
only what the index knows is what makes an unfamiliar directory safe:
unreadable must never become deleted.

Retiring sets aside into `retired/` rather than deleting. With a delete
a mark and a permanent delete an eager `Remove`, the index and the
snapshot agree about everything that went on purpose by the time a save
runs, and the pass finds nothing. It is the net under a snapshot that
lacks something nobody deleted, the kind of disagreement that once
emptied a library.

A permanent delete takes what the store writes and nothing else: the
records and order files, the pictures and thumbnails, an older build's
`.removed` mark, and a temporary a crash left one of those as. A note
someone kept beside a record, or a directory of scans beside a canvas,
stays. The directory stands for it, holding no record, which nothing
reads back and, never having been indexed, nothing sets aside. The
first version deleted the directory whole, with whatever anyone had
put in it, while every other path in the store left foreign files
alone.

It recurses into a directory inside what it deletes when either of
these holds:

- the directory's uid was deleted for good along with it. It may have
  lost its record to an earlier run already.
- the directory holds something only the store writes: a record, an
  order file, a picture named after the directory's own uid, a
  temporary of one of those, or such a directory below it.

The second rule is broader than Load's rule of "holds a record" on
purpose. A save that stopped partway can leave a directory whose own
record never landed: a canvas's snippets and order file with no canvas
record above them, or a snippet's picture moved in from staging and
not its record. Load does not read such a directory, so nothing names
it when its parent is deleted. Asked only about the record, the
removal left it standing, and it kept the parent standing too.

A directory is removed only once it is empty. One that cannot be
listed counts as not emptied: calling it done dropped the removal and
left the record in it to load again.

A permanent delete is recorded before anything is deleted. The store
names the uids in `pending.json`, drops the directories from the index
so that nothing under them is placed or set aside meanwhile, and only
then removes. A removal that stops partway is then still known when
the process starts again. It can stop for two reasons:

- a crash;
- a file that cannot be deleted. Windows refuses to delete a file
  another program holds open without delete sharing, and a picture
  viewer looking at a capture is exactly that.

`Load` reads nothing of a directory the file names, and owes its
removal. Every save takes another run at it. The store removes only
what the file already records, so a `pending.json` that cannot be
written deletes nothing new, and fails the save. The file is gone
again once nothing is owed. A `pending.json` that still names
something counts as owed even when everything it names is gone. A
crash between the last removal and the rewrite leaves it so, and
otherwise no save would come along to rewrite it.

The previous version wrote a `.removed` mark into the directory, but
only once a removal had failed. A crash partway through one left no
mark, and the next start reloaded what was left: a snippet deleted for
good came back without its picture, which had gone first. Found by the
crash tests (see "One way to the disk"). Marks an older build left are
still read as owed removals.

A permanent delete names everything that goes, not only the thing: the
session passes the thing and every folder, canvas and snippet the model
held under it. A snippet moved into a canvas since the last save still
has its directory under the canvas it came from, and only the model
knows it went with this one. Named only by the canvas, it was left
indexed under its old parent, and the next save set it aside in
`retired/` as something the library had lost. Were its picture held
open, the set-aside failed and a restart loaded it back. The store also
takes whatever its index has inside a named directory that the library
no longer holds, named or not.

The session counts an owed removal as
an unsaved change: the flush that recorded it counts, but the autosave
keeps asking - on a clock of its own, every ten seconds, the cadence the
hidden retry timer has - and the hidden retry timer keeps running, until
the directory is gone. The owed path follows every rename and
retirement of a parent, the way the index does, so a folder renamed
before the retry does not leave the pass looking at the old spelling,
finding nothing, and calling the removal done with the directory
sitting under the new name. The clock matters: the save that recorded the
removal counted, so neither the failure backoff nor the quiet period
held the next one back, and the retry ran on every frame, hashing the
library each time, for as long as another program held the file. The
first version forgot the directory before
deleting it and returned true whatever happened, which left the remains
to be retired as something lost, or reloaded as a snippet; the second
kept the removal pending but let the next save count as clean, so
nothing retried until an unrelated edit, and a restart reloaded the
record.

A permanent delete must not take what was moved out of it. A move
changes the model at once and the directories at the next save, so a
snippet moved to another canvas, or a canvas to another folder, is still
inside its old parent on disk until then - and deleting that parent for
good in between took the moved snippet's pictures with it, while the
model kept the snippet and the next save wrote its record afresh,
without them. `Remove` is therefore handed the library as it stands
without the thing. When anything that library still holds is indexed
inside the directory, nothing inside is deleted yet. The removal is
recorded all the same, and so is where each such thing belongs, under
`moves` in `pending.json`: a snippet's canvas, or a canvas's folder. A
save places everything before it runs the owed removals, so it
finishes the removal once nothing held is left inside. The session
makes that save at once, so the ordinary case completes as the delete
is asked for. If the move cannot land, because a picture in the moved
snippet is held open, the removal waits with it.

A restart meanwhile reads the moved thing from where it is, inside the
deleted directory, and puts it where `moves` says; the next save moves
its directory to match. It goes to the first folder or canvas instead
if the one it belongs to is gone too. Only what `moves` names is
rescued. The record is written, moves and all, before anything is
removed, so anything else inside went with the deleted thing. The
previous version wrote nothing for a removal that waited, so that a
restart would still find what was moved. It found it in the old place,
with the deleted canvas loaded back around it. Retention later erased
that canvas again, and took the snippet with it.

A snippet's directory is swept for pictures its layers no longer name
only after the record that stopped naming them is on disk. Until then
the old record is what a restart reloads, and the pictures it names have
to still be there for it: a sweep that ran on a record that failed to
land deleted the only image the surviving record pointed at. The sweep
also takes only pictures (`.qoi`); anything else someone put
beside a record is not the store's to delete.

### A save writes what changed

The store keeps what it last wrote - a content hash per snippet, the
exact text for the small records - and where everything lives, so a save
neither re-reads the tree nor re-serializes what it would write back
unchanged. A twelve-canvas library measured 858 ms per autosave when
every file was rewritten, for one stroke on one snippet; bounded by what
moved it is 1.8 ms when nothing did and 7.5 ms for that stroke. `Load`
establishes the same record as it reads, so the first save of a session
costs only what the load had to repair. The hash and the serializer are
kept adjacent in the source and a test asserts every field moves the
hash, because a field added to one and not the other is an edit that is
silently never saved.

To be precise about what is and is not bounded: the *writes* are
bounded by what changed; the *pass* that finds out what changed is not.
A save reads the library through a `LibraryView` - borrowed references
to the manager's own vectors - rather than a snapshot, since the
snapshot copied every stroke point in the library on every save and was
the largest single cost of one that then wrote nothing. What remains is
the fingerprint: `HashItem` walks every point of every snippet, about a
thousandth of the cost of serializing it, and linear in the library's
size. Canvases are grouped by folder once rather than scanned per
folder. If the fingerprint ever shows in a profile, the next step is a
per-snippet revision counter bumped by every mutation path, so that only
changed snippets are hashed; it has not been needed at the library sizes
measured, and every stroke mutation would have to remember to bump it.

Every file goes through write-to-temp-then-rename, pictures and the
settings file included, and through one writer (`WriteFileAtomically`
in `core/util`): a painted layer is re-encoded over its own previous
file on every save, and truncating in place left a window in which the
only copy on disk of a drawing was the first half of it. The temporary
is created exclusively, under a name nothing was at. A file already at
`<file>.tmp` is never opened: a plain file with no other name is a
temporary of ours that a crash left, and is removed; anything else - a
hard link to a file elsewhere, a symlink, a directory - is passed over
for `<file>.tmp1` and so on. The first version opened `<file>.tmp` with
truncation and trusted whatever was there, and a hard link at that name
to a file outside the library had the library's bytes written into it;
a hard link to the committed file itself would have truncated it before
the rename, which is the opposite of what the rename is for. Hard links
are not links in the reparse sense, so the path check for junctions
(`IsOurs`) does not see them; exclusive creation is the only answer.

**What is and is not promised.** Atomicity per file, not a transaction
across files. The rename means no reader - this process after a crash
included - sees half a record or half a picture. The temporary's
contents are committed to the disk (`_commit`, which is
`FlushFileBuffers`) before it is renamed into place. Without that, a
power cut inside the OS's write-back window could keep the rename,
which NTFS journals, and lose the data, which it does not: the file came
back at its new length full of zeros, and the good version it replaced
was already gone - a whole snippet with weeks of strokes, not a few
seconds of ink. The rename itself is not flushed, so a power cut can
still lose the last save, falling back to the file before it. Nor is there a
transaction spanning a record and the pictures it names: a crash between
the two leaves a picture with no record (kept in staging, then in
`retired/staging/`) or a record naming a picture that was never written
(the layer draws as its placeholder). Both are the same shape of
inconsistency a hand edit leaves and are reconciled the same way. What
"captured" means, precisely: the pixels are encoded to disk before the
capture returns; the *record* naming them is written by the save that
follows - at once when the overlay is hidden, within the debounce
otherwise - and the session holds the pixels until both have landed.
A journal of saves was considered and not done: the library is a
working surface autosaved every few seconds, not a document with a save
button, and with each file's data on the disk before its rename, the
last few seconds of ink are the most a power loss can take. Deleting for
good is different: it is recorded in `pending.json` before it starts
(see "A save is a plan").

### One way to the disk

Every disk operation the store makes goes through one interface,
`FileSystem` in `core/util`, and so do the atomic writer and the QOI
codec's file reads and writes. The store is handed one at
construction. The disk itself (`RealFileSystem`) is the default.

The reason is testing. Most of what has gone wrong in the store went
wrong when the disk refused something: a picture held open by a viewer,
a rename that failed, a crash between two steps. Each of those tests
used to set up its own real failure, a file opened without delete
sharing, say. That only works on Windows, and only for the failures
someone thought to stage. A file system a test can hand the store
reaches every case without staging it: one that fails a chosen
operation, holds a chosen file, or stops changing the disk after the
Nth change, which is all a crash leaves behind.

The interface is primitive on purpose: status, list, read, make one
directory, write a new file, rename, remove. Everything built from
several steps is built on top of those: creating a directory with its
parents, removing a tree, writing a file atomically. That way a test
can stop such an operation between any two of its steps. Nothing in
it throws. A listing is whole or absent, never partial: the standard
iterator's `++` throws on an error partway through a directory, which
a range-for over a library directory turned into a crash.

Listing reports what an entry *is*, not what it points at. So a
symlink that stands where a snippet's picture or a staged capture
would be is now passed over by the picture sweep and the staging pass,
rather than removed or moved. The rule is the same one the tree
already keeps for linked directories: a link is never the store's.

**The disks the tests use** live in `tests/support`:

- `MemoryFileSystem` is a model of the disk in memory. It is fast
  enough to run a scenario hundreds of times, and it behaves the same
  on Linux as on Windows.
- `FaultyFileSystem` wraps any file system and adds three kinds of
  misbehavior:
  - holding a file open, which blocks removing it, renaming it,
    replacing it and renaming any directory above it;
  - failing a chosen operation;
  - crashing: after N changes the disk stops changing, and the write
    the crash interrupts leaves its first half.

A model is only worth what it agrees with. So one conformance suite
runs the same expectations against both the model and the real disk,
a held file included (a stream open the way the MSVC runtime opens
one). Anything the model gets wrong about Windows shows up as the two
disagreeing.

`ForEachCrashPoint` runs a scenario once to count its changes, then
once more for every point it could crash at. Each time it restarts on
what was left and checks the rules a crash must never break:

- nothing loads twice;
- every picture a record names loads;
- each snippet is its version from before the change or from after it,
  whole, in one place or the other;
- the restart saves, and loads back as itself.

Edits, moves of snippets and canvases, renames, captures, rewritten
pictures and deleting for good all pass at every point. A delete also
passes with a picture held open, and with a snippet just moved into
the deleted canvas. It also passes with a snippet or a canvas just
moved out of what is deleted, while a picture in it is held open so
the move cannot land. For a delete, one more rule applies: once the
restart has saved, nothing of what was deleted is left anywhere in the
library, `retired/` included.

### Images: QOI

Pixels are written as QOI. Measured on this app's own screenshots
against stb's PNG:

| | 1920x1080 capture | ~500x400 capture |
|---|---|---|
| PNG decode | 43 ms | 8.8 ms |
| QOI decode | 7 ms | 1.2 ms |
| PNG encode | 296 ms | 49 ms |
| QOI encode | 13 ms | 2.4 ms |

Decoding is what a canvas switch pays; encoding is what every screenshot
pays, synchronously, while the user waits. Both are lossless, and the
files come out ~30% smaller because stb's encoder is a weak one. Raw
pixels were measured too and are a trap: reading 8 MB off disk costs
more than reading 1.6 MB and decoding it. The library holds QOI only;
stb's PNG codec stays in `image_codec.h` for importing and exporting
pictures, which nothing does yet. A 256px thumbnail is written beside every picture so the
Overview never decodes a fullscreen capture to draw a 200px tile.

A capture's pixels are written synchronously at capture time, not with
the debounced record write: a screenshot lost to a crash can never be
recaptured, where a few seconds of strokes can be redrawn. For the same
reason a picture found in staging that no record names is set aside
into `retired/staging/` rather than deleted: from the store's side it is
either the capture of a snippet deleted for good before it was saved,
which nobody wants, or the capture a crash left without its record,
which is exactly what writing it early was for, and the two cannot be
told apart. `retired/` is the user's to empty.

`staging/` is removed by the save that drains it, so it exists only
between a capture and the next save, or while a picture in it could not
be moved. An empty folder left in the library was only something for a
person looking in to wonder about.

A picture waiting in staging whose snippet's directory already holds a
file of the same name is set aside the same way, not moved in over it.
A picture goes to staging only while its snippet has no directory and
to the directory from the moment it has one, so the one at home is
always the newer: the waiting one is what a move refused at the save
that made the directory left behind - a file held open - and the
painting has been saved again at home since. Moving it in once the file
was let go of put the older pixels back under a layer that believed
itself saved, where no later save would notice.

## Configuration

`AppConfig` is every user-editable setting, read from and written to
`config.json` by `ParseConfig`/`SerializeConfig`. Parsing is pure core
logic; only *where* the file lives is platform-specific.

The file is JSON rather than flat `key=value` lines because of
profiles: a profile matches on a list of executable names and window
titles, which are arbitrary strings holding `=`, `#`, commas and
non-ASCII, and any flat encoding of "a list of arbitrary strings" grows
a bespoke escaping scheme with its own bugs. nlohmann/json was already
in the binary for the library.

Three things about the file are deliberate:

- **Groups, not a flat namespace** (`hotkeys`, `drawing`, `appearance`,
  `bars`, `overview`, `display`, `behavior`, `shortcuts`, `diagnostics`).
  `AppConfig`'s fields stay flat and the mapping lives in the
  serializer. The grouping is not cosmetic: `behavior` and `shortcuts` are
  exactly the settings a per-application profile may override, so a
  profile is those two objects again, sparse.
- **Absent means inherit, `null` means explicitly unset.** "Said
  nothing" and "said none" have different spellings, which a shortcut
  that ships bound and is unbound on purpose depends on. The summon
  hotkeys use the same spelling: a hotkey given another's combination
  leaves that other unbound, and an unbound hotkey read back as "said
  nothing" took its default again on the next start - which could now be
  the combination the other had taken, and one combination registered
  twice left the second hotkey dead. The tray also unbinds a later
  duplicate of an earlier hotkey at startup, for a file edited by hand.
  Every setting is written, defaults included, so the file documents
  what can be set.
- **A hotkey another application owns does not stop the start.** It
  used to: a screenshot tool on Ctrl+Alt+C was enough for the app to
  refuse to start, with a message that named nothing, and a hand edit of
  `config.json` as the only way back in. The hotkey is left unregistered
  instead, and a message box names each one with its combination; the
  tray menu reaches the overlay without any, and Settings > Hotkeys can
  pick another.
- **`ordered_json`, and floats rounded to six decimals**, because the
  file is meant to be opened and read: alphabetical keys interleave
  settings by spelling, and `0.22f` promoted to double writes as
  `0.2199999988079071`.

Malformed input is never an error: a value of the wrong type or out of
range leaves that setting at its default, the same contract the library
has. A file that is not settings at all - not JSON, or too big to be -
is not a first run either. Read as defaults, it was written over by the
next settings change, and a stray comma cost every hotkey and profile.
`LoadOrCreateConfig` renames it to `config-unreadable-<stamp>.json`
instead, and one that cannot be opened is left where it is and not
written over for that run. Either way the app starts on the defaults,
says so in a message box, and skips the retention period for that start,
since whether it was on is what could not be read. Skipping it for one
start was not enough: the next start found no file, or the defaults a
settings change had written, and both turn a 14-day retention back on
over a library whose owner may have switched it off. So a file set aside
is replaced at once by the defaults with retention switched off, and it
stays off until switched on again. The file is written
through temp-then-rename, since truncating it in place leaves a window
in which every setting is a half-written file.

`KeyCombo` represents a hotkey as modifiers plus one logical key rather
than an OS virtual-key code, and no modifier is required: a bare
function key is a legitimate hotkey, and refusing plain letters is a
possible later restriction rather than a rule today.

### Tool shortcuts

Every drawing tool, creation tool and clipboard action can carry a key,
pressed while the overlay is up in edit mode. Four ship bound (`S`
screenshot, `D` drawing, `E` eraser, `P` pen) plus the clipboard's usual
`Ctrl+C/X/V`, `Ctrl+D` to duplicate the selection, `Ctrl+Shift+N` for
a new canvas the selection comes along to and `Ctrl+H` for the cheat
sheet; the rest start unset, because
a shortcut that fires a tool you did not want is worse than no shortcut.
The two chords are safe to ship where a letter would not be, since a
chord cannot fire from ordinary typing - `Ctrl+D` sits beside the plain
`D` that makes a drawing, and the exact-modifier match keeps them apart.

Duplicate is Copy and Paste in one step and deliberately does not go
through the clipboard: duplicating something is not a reason to lose what
was copied earlier. `Ctrl+Shift+N` moves the selected snippets to the
canvas it makes, which otherwise costs a new canvas, a switch back, a
cut, a switch forward and a paste; it is a separate action from the plain
new canvas rather than one that reads the selection, so the canvas bar's
own "+" keeps meaning only what its icon says. With nothing selected the
two do the same thing, because an empty selection is no reason to refuse
the canvas. These are not OS hotkeys and
are stored apart from the summon hotkeys: the UI reads them off its own
frame, they only do anything while the overlay takes input, and nothing
about them can fail the way registering a global hotkey can, which is
also why a bare letter is allowed here and questionable there.

`ShortcutAction` is the flat list the config layer persists, by name.
Config sits below the app and cannot see `Tool` or `ClipboardAction`;
the table that ties an action to what it runs lives in the UI, and a
test there stands in for the exhaustiveness check a switch would give.

### Per-application profiles

A profile is a name, match rules and sparse overrides. Two levels, always
the same two: exactly one profile matches at a time, first in list
order, so "which profile am I in" has one answer; and everything it does
not state comes from the defaults, so "where did this value come from"
has two possible answers and no chain to trace.

There was a third level once: a profile could be `basedOn` another, so
one input recipe could serve a dozen games. The resolver was fine and
the idea was not. The rule that made it comprehensible - a recipe may
not itself be based on something - existed only in conversation, so
nothing stopped a chain four deep; a list where every entry names what
it derives from has to be read rather than scanned; and the sharing it
bought is speculative, since typing the same three settings into a
second profile costs seconds, once.

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

The layers above the platform are three libraries, and the build keeps
them apart: `sz_core` holds the model, the persistence, the settings and
the session and does not link Dear ImGui; `sz_ui` is the overlay as it is
drawn, a view of the session; `sz_app` is the tray controller that owns
settings, session and overlay and moves the window between hidden, edit
and view-only.

**`Settings`** is the one copy of every setting. `Stored()` is what
`config.json` holds; `Live()` is the profileable group resolved against
the profile that matched what the overlay came up over. The UI reads
plain fields through `Stored()`, edits them in place through `Mutable()`
and `Commit()`s once an edit is finished; profileable fields go through
setters that say whether the defaults or a profile is meant. A commit
re-resolves and calls the controller back, which applies what changed to
the window and writes the file. The live values are derived, never
assigned, so there is no path by which what runs and what is stored can
disagree.

**`Session`** is what is being worked on, independent of how it is
shown: the library and deleting and restoring in it; the debounced
autosave and the texture sync that keeps the GPU in step with the current
canvas; screen capture (the frozen screen, a snippet's capture, the
picture a copy gets); and the per-canvas undo history with every edit
that goes on it, offered as commands (`DeleteItem`, `ClearDrawing`,
`CommitLiveStroke`, text edits) and as gestures in screen space (paint,
erase, shapes). The session needs two things of the platform, textures
and captures, and takes them from the window it is attached to, which may
be absent: the session tests drive all of this with no window, no store
and no ImGui.

What stays in the UI is what a UI decides: which tool is in hand and its
color and width, where a gesture starts and what it is over, panel and
popover state, toasts, and GPU caches that exist only for drawing.

### Autosave

`Session::Tick` runs every frame and compares the model's generation
counter against what was true last frame and what was last saved. A
write fires once the generation has been unchanged for 2 s, coalescing a
burst of edits into one write, or after 15 s regardless, so a long
uninterrupted session is still persisted. `Flush` is called where
content stops being editable with no frame to follow - hiding the
overlay, restarting it for a setting, exiting - and every one of those
first settles what the hand is in the middle of
(`OverlayApp::SettleForPersistence`): the gesture under a held button
ends, and a note being typed is committed to its item. A note lives in
the editor's buffer until it is committed, and a flush that ran before
the commit wrote the note as it was when the editor opened; typing that
had been on screen for a minute was gone at the next start. A flush at
one of those points, or after a silent capture taken while hidden, that
does not land arranges its own retry: the autosave's clock runs on
frames and there are none while hidden, so the tray asks the platform
host for a background timer (a `WM_TIMER` on the host window) and
tries again every ten seconds until the save lands or the overlay is up
again and frames take over. Before that, a silent capture with notices
off created its canvas and snippet and returned to hidden without a
save; the picture was on disk and the record naming it was not, for as
long as the overlay stayed hidden. A failed write (disk full, a file held open) is retried
on a clock of its own, doubling up to 30 s; falling through to the quiet
check, which a failed save does nothing to reset, retried on every frame
and turned a full disk into a synchronous rewrite per frame. A save is
acknowledged only when *all* of it landed, painted pixels included, so a
layer whose write failed is retried rather than waiting for an unrelated
edit. A capture whose picture could not be written at capture time keeps
its pixels in the session and is written by the next save that can, and
no save counts until it has. A screenshot is the one thing in the
library that cannot be remade; the first version let the pixels go with
the capture result, so a picture that failed to write stayed on screen,
looking captured, and was gone at the next restart.

Painted pixels are written before the texture sync discards anything
non-current, and the release path refuses to drop a layer that is still
dirty. Waiting for the debounced save was not good enough: switching
canvas bumps the generation, which pushes the save *further away* at the
exact moment the pixels are thrown out.

Painted pixels also count as a change on their own. The generation only
follows a brush stroke when it ends (`EndPaintStroke`), and a stroke
whose end never came - drawing mode left with Escape or the view-only
hotkey while the button was held - left its pixels on screen and dirty
with the generation unchanged: nothing looked unsaved, a flush wrote
nothing, and the stroke was gone at the next start. Every pixel change
now bumps a paint revision beside the generation, which `HasUnsavedChanges`
and the autosave's quiet period both read, so pixels on screen are saved
whether or not their gesture ever ends - and a stroke in progress holds
the quiet period off the way a drag does. Leaving drawing mode also ends
a stroke in flight as a release would, so it is its own undo step rather
than one the next stroke files late.

**One writer per library.** Two copies of the app would each save the
library from a stale picture of it, through the same temp-file names.
The tray claims a per-user named mutex before it does anything else,
and a second copy exits with the app's one message box instead of
loading the library. Per user is per `%APPDATA%`, which is per library;
the kernel drops the mutex with the process, so a copy that crashed
holds nothing. A hotkey collision is not a lock: with a hand-edited
config the two copies could have different hotkeys and never notice
each other.

**When the disk says no.** A save that fails is said on screen for as
long as it stays failed: a line along the bottom naming the library,
drawn from `Session::LastSaveFailed`, in edit and view-only mode alike,
rather than a toast that fades while the problem does not. A settings
file that could not be written is reported on the same line by the
tray, which is the only writer of it, and remembered as owed: the
background timer tries it again every ten seconds, whether or not the
overlay is up, and the shutdown flush tries it once more before the app
goes. A settings edit is rare, and a failed write used to stay
unwritten until the next one. Before this, both results were
discarded: a full disk lost every outstanding edit on an ordinary exit
without a word, and a setting that appeared applied was back to its old
value at the next start.

Exit and the OS ending the session (`WM_QUERYENDSESSION`, answered TRUE
after the flush, and `WM_ENDSESSION` again for good measure) are the two
flushes with no retry after them. Those two messages are a broadcast to
every *top-level* window, and Windows leaves message-only
(`HWND_MESSAGE`) windows off that list - which the host window was when
the handling was first written, so no logoff could have reached it. It
is now an ordinary hidden top-level window, and the test finds it with
`FindWindow`, which likewise sees only top-level windows, and sends it
the query. Being top-level, it also receives `WM_CLOSE` - `taskkill`
without `/f` posts it - which `DefWindowProc` answered by destroying the
window and nothing else: the process ran on with no tray icon and no
hotkeys, still holding the single-instance mutex. A close from outside,
and the Restart Manager's `ENDSESSION_CLOSEAPP`, now take the tray menu's
Exit. So does a `WM_CLOSE` sent to the overlay, which is where `taskkill`
sends it while the overlay is up - it closes the windows it can see, and
the host window is hidden. Alt+F4 over the overlay arrives as `SC_CLOSE`
instead, and stays swallowed. Both settle the hand's work, try the
save twice - the first attempt may be what clears the way - and, if the
library still cannot be written, write a **recovery copy** beside it:
`library-recovery-<timestamp>/`, a fresh tree holding every record and
every picture those records name - from the session where it holds the
pixels (a capture whose write never landed, a painted layer), and
re-encoded from the real library otherwise - with a `recovery.txt`
beside the tree saying where it came from and whether it is whole. The
copy has to open on its own: the first version copied only what was in
memory, so its records named screenshot files that were still in the
real library and the copy opened with placeholders where they should
have been. A copy that could not be made whole (a picture the real
library no longer has) is reported as such, in the return and in the
note. Exit still exits, and this is an **accepted outcome**: when the
library cannot be written twice over and the recovery copy beside it
cannot be written either - a full volume, an unwritable parent - what
is in memory is lost when the process goes. Holding the app open
against the user's explicit request was judged worse than a copy they
have to go and find; the OS's session end cannot be held up at all; and
the tray has no window of its own to ask in. A retry loop at exit, an
alternate destination to ask for, or a message box naming the copy were
each considered and not done - a save that has failed at two places
three times is a disk problem, not one more attempt away from working,
and the on-screen warning while the app ran was the time to say so. A
test pins the outcome down (both destinations unwritable, exit still
exits, nothing forced anywhere), so that it stays a decision rather
than drifting into an accident.

### GPU textures are per canvas

`SyncTexturesToCurrentCanvas` uploads a texture for every layer on the
current canvas that has a file or pixels but no texture, and releases
every other canvas's. Only the current canvas is ever drawn from a real
texture, so everything else is pure cost - a library of fifty 4K
captures would otherwise pin ~1.6 GB of VRAM behind a game and pay for
it as a stall on the first hotkey. The gated form runs immediately
before anything draws item content, not merely once a frame: a canvas
switch can happen mid-frame (Alt+wheel is handled from the frame), and
one frame drawn between the switch and the load renders every shot as
the placeholder gradient - the gradient flash the gate removed.

**A release waits for the frame.** Anything may release a texture
mid-frame after it has already been drawn into that frame: clicking an
overview tile drops the tile previews, moving an item to another canvas
drops its texture. ImGui's draw commands hold the raw pointer without a
reference and are only submitted at the end of the frame, so the D3D11
renderer holds releases made between `NewFrame` and `RenderAndPresent`
until the frame has been handed to D3D, which keeps what it uses alive
from there. Doing it in the renderer covers every caller, rather than
asking each of them to order its mutations before its drawing.

**A lost device is replaced in place.** The driver can take the D3D11
device away: an update, or a restart after it stopped responding, which
a game underneath can cause. Before this was handled, every later frame
failed silently, so the overlay was blank for the rest of the process
while its input grab still took the input. And a resize that could not
make its render target left none, which the next frame cleared: an
access violation inside d3d11.dll. `ReadyToRender`, at the start of each
frame, checks the device and replaces a lost one, with the swapchain,
the filter shaders and ImGui's backend. The ImGui context stays, and
ImGui makes its font atlas again by itself. While no device can be made,
the frame is skipped: a short wait stands in for vsync, and the grab,
no longer hearing from the frame loop, lets input through. What the app
holds cannot be carried over: every texture was made on the old device.
So the window's `TextureGeneration` moves on, and at the start of the
next frame, before anything draws, the app lets go of each texture and
makes it again. The session remakes the current canvas's pictures from
memory or the library, a picture not written yet from its pending pixels,
and the frozen screen from the pixels it keeps for cropping. The
Overview's previews and the stroke rasters rebuild as they are next
wanted. A stray old texture handed to `UpdateTextureRegionRGBA` is
refused, not written with the new device's context.

A window nobody can see is skipped the same way. With the screen locked
or the secure desktop up, `Present` returns `DXGI_STATUS_OCCLUDED` at
once instead of waiting for vsync, and a frame loop drawing every frame
used a whole core until unlock. Once a present has said so, each frame
first asks with `DXGI_PRESENT_TEST`, which draws nothing, and waits
while the answer is still occluded.

**A copy owns its pixels.** The clipboard holds ids, not pixels, and a
copy made from them (paste, duplicate, copy to another canvas) must
share neither a file nor a texture with its source. `CanvasManager`
clears the copied layers' filenames and deep-copies whatever painted
pixels are resident; `Session::ClonePicturesForCopy` then gives the
copy the rest, layer by layer: the picture layer from the session's own
pending pixels or the source's file, written under the copy's name at
once, and every painted layer whose pixels were let go of - the state
of every layer on a canvas that is not current, which is what a paste
across canvases copies from - read back from the source's file into
pixels of the copy's own, dirty, for the next save to write. The first
version restored the picture layer alone, so painting copied to another
canvas came out blank, for good; the UI says so when a layer's source
cannot be read, rather than showing a copy that looks whole.

### Undo is per canvas

History is a `deque` per canvas, capped at 50 entries *and* 128 MB of
what they hold (a Clear drawing holds a whole fullscreen layer, 8 MB;
fifty of them was 400 MB on one stack). A canvas is this app's document,
and undo scoped to a document is what every editor does. One global
stack reached across canvases and failed invisibly: draw on A, switch to
B, draw, come back to A, press Ctrl+Z, and the stroke that vanished was
B's, on a canvas you were not looking at.

Eight kinds of entry, and every one is either its own inverse or a mirror
with the direction as the only difference, so undo and redo are one walk
in opposite directions through one dispatch. Each kind is a struct of
its own in one `std::variant` (`core/session/undo_entry.h`), holding
only its own fields and saying itself what it weighs and which snippets
it names; applying it is one `Session::Apply` overload per kind, so a
kind added without all of that does not compile. It was one struct with
every kind's fields side by side, most of them commented "X only", and
adding a kind meant finding the four switches that had to agree about
it. A `StrokeBaked` entry
carries the stroke so redo can push it back, and undo takes off that
stroke, found from the back, rather than whatever is last. An `Erased`
entry is a list of *replacements*: for each original a gesture clipped,
its index in the list as it was, the original itself, and the fragments
now standing in its place. Undo rebuilds the before-list from the
after-list and redo the reverse, both by position. The session follows
the list through the gesture - the manager's erase reports what became
of each stroke, index for index, and the session keeps a parallel
"which original does this stand for" vector - so a fragment clipped
again by a later call in the same drag still traces back to the stroke
that was there before the drag. The first version diffed a snapshot
against the result by value, removed fragments by value and appended
the originals at the end: that changed the draw order, left the next
undo of a stroke popping a different stroke than it was for, could not
tell two equal strokes apart, and was quadratic in the drawing's size.
The painted half of the same gesture rides in the same entry, so one
drag is one undo whichever kinds of ink it touched. `ItemCreated` carries
an id and toggles the mark, and `ItemDeleted` does the same for every
snippet one Delete took: one entry each made deleting a selection that
many undos, and past the history's cap of 50 the earliest could not be
undone at all - for a deleted snippet, which comes back only by undo,
that was deleted for good. `NoteTextChanged` and the painted entries swap
their contents with the item's, so the popped entry is already what the
opposite stack needs. Undo is best-effort about staleness: an entry
naming something gone does nothing and is dropped rather than moved to
the other stack.

A paste or a duplicate is one `ItemsArrived` entry for everything it
brought, filed on the canvas it landed on: a copy is undone into its
deletion mark as a new snippet is, and a snippet a cut moved here goes
back to the canvas it came from, at the place in the stack it left -
undone last first, since each place was taken after the snippets before
it had gone, which is what puts several cut from one stack back in its
order rather than swapped. With
no entry of its own, an undo after a paste reached past it and took back
whatever came before - usually out of sight, under the copy. Unlike the
rest, this entry can find it has nowhere to go: the canvas a cut came
from deleted, or deleted for good, before the undo. Sending the snippets
there would hide them in the trash, or have nowhere to put them at all,
so they stay, and the step is *refused*: dropped, with a toast saying
why, so that the next undo reaches the step before it rather than
finding the same refusal forever. A redo refuses the same way when the
snippets it would bring have been deleted since. The move each way also
leaves the history behind that it would otherwise carry to a canvas it
is not on - see `ForgetHistoryOfItem`.

Deliberately narrow: reorders and renames are not tracked, and deleting
a canvas or folder gets a confirmation and Show deleted instead of an
undo entry. Either confirmation - for a delete that can be restored, and
for one that is for good - can be switched off in Settings > Behavior
(`AppConfig::confirmDelete`, `confirmDeleteForGood`), for everything at
once rather than per profile, since what a delete asks is about the
library. The request is still deferred to where the popover is drawn,
and done there without asking, since a button that deleted on the spot
would change the Overview while it is still being drawn from it. Moving a snippet to another canvas from its menu, or to a
new canvas with Ctrl+Shift+N, is not on the history either.

Moves and resizes are, since one accidental drag of a snippet in a
carefully stacked overlay had no way back. A snippet's placement is
changed in the model directly, event by event, and recorded afterwards
as one `PlacementChanged` entry per gesture: the press takes the
placements of everything the gesture may move, and the release files
the difference, so a drag is one undo however many events it took and a
multi-selection is one undo for all of it. A click that moved nothing
files nothing. A placement is the whole of where a snippet is - its
rect, its fullscreen state and the anchor its rect is recomputed from on
a display change - because a drag takes a fullscreen snippet out of
fullscreen, and an undo that brought back the rect but not the
fullscreen would not be the snippet as it was. Like the painted kinds
the entry is its own inverse, a swap each way. The one-shot changes -
fullscreen from the bar or the menu, Original size - are an entry each.
Steps that come in bursts, wheel notches and arrow-key nudges, fold
into the entry the burst began, keeping its before, when they continue
it within a second and nothing else was filed in between: a spin of the
wheel is taken back in one step, to the size it started at, rather than
a notch at a time. Minimizing is not a placement - the snippet does not
move - and is not recorded.

Delete, undo and redo pressed with a drag still in flight end the drag
where it stands first, filed as its release would file it; the rest of
the drag moves nothing - see "One gesture engine on the raw pipeline".

### Making a snippet is on the history, and an untouched one goes

Snippets are made through `Session::CreateItem`, so a screenshot taken
by mistake can be undone into its deletion mark and redone out of it. A
drawing a press made is watched until the hand moves on, and
`DiscardIfUntouched` erases it for good if nothing was put into it: no
strokes, no paint, no text, no picture. A screenshot is content even when
its capture failed. The watch ends the frame after something first goes
in, not when the hand moves on: a drawing that has held a stroke is a
drawing, and an undo that empties it again must leave an empty drawing
behind rather than erase it - which took the redo of the stroke with it.

### Freezing the screen

`FreezeScreen` captures the whole display through the same call a
snippet's capture uses, which leaves the overlay out of it, so the
picture is the application underneath and none of our content. While a
screen is frozen a region capture is cropped out of it rather than taken
live; without that the user drags a region over a still picture and gets
back whatever the game showed a moment later, which for a moving camera
is a different scene. The pixels are kept alongside the texture for
exactly this. Released on hide: it is a fullscreen texture with no reason
to exist while nothing is shown.

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

- **The ImGui style.** `ApplySpickzettelStyle` builds the style from a
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
pushes, since ImGui would otherwise apply it). The welcome notes are the
exception, made at the interface scale when they are made - they are the
app speaking, and someone who reads at 150% should not need the
text-size slider to read the note that says where it is. Drag thresholds
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
of the session. Its definition is split across `overlay_app_*.cpp` by
section of the UI (items, input, paint, popovers, docks, overview,
deleted, undo) with `overlay_app.cpp` holding the per-frame entry points
and construction; it is still one class. Helpers used by more than one
file live in `overlay_app_internal.h` under `overlay_detail`; anything
used by one file stays a file-local helper. `ui::ContextMenu`
(`context_menu.*`) is the one piece drawn beside the class rather than
inside it: it is a widget, not a view of the session, and depends on
nothing but Dear ImGui and the icon tables.

Panels - popovers, the canvas bar, the dock, the note editor, the
Overview - are ordinary ImGui windows and widgets. Items and the
selection's furniture are painted into full-screen `NoInputs` layers and
hit-tested by the app itself. Keyboard input reaches the UI through
ImGui's own input state, which the platform backends feed; a UI not
built on ImGui would need a key callback on `IOverlayWindow` in its
place.

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
up the creation tool, the fullscreen ones make it at once), Paste, the
Overview and Settings - so that a kind set to no press at all, or
forgotten, is one click away, and the canvas has the menu a right click
everywhere else has taught. It opens on release, like a snippet's; a
right drag there does nothing, and a canvas switch or a capture during
the press drops it rather than releasing it into a menu over a canvas
nobody clicked on.

Two things make it cheap to hit by accident. A drawing a press made is
watched until the hand moves on or something goes into it, and discarded
for good if nothing was put into it. And making one is on the history:
undo marks the snippet deleted, where a screenshot taken by mistake can
still be found.

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
Text are in hand only while a snippet is in *drawing mode* and act on
that snippet alone; for them a shape is a modifier held as the press
starts (Shift for a line, Ctrl for a rectangle, Ctrl with the eraser for
a rectangle eraser), read once on the press so letting go mid-drag
changes nothing. Drawing and Screenshot place a new snippet with the
next press, anywhere, then hand over: a drawing to Draw, a screenshot to
the tool that was in hand before.

There were seven tools once - Pen, Rectangle, Line, Eraser, RectEraser,
Text and Move - with three favorite slots of them on a right-click ring
menu, so the tool wanted was usually not on a slot and the slots were
rebound all the time. The ring and the flat tool strip that mirrored it
went: with the selection bar carrying every action on a snippet and the
canvas bar reaching the Overview, they added a gesture to learn and
nothing to reach.

### Drawing is a mode, entered on one snippet

At rest a snippet is an object that a click selects and a drag moves.
Double-clicking it, or holding a press still on it, enters drawing mode:
a stronger outline, the bar shows Pen, Eraser, Text and the color, the
pen is in hand, a press on it draws, a right-drag on it erases whatever
tool is in hand, and a click anywhere else, a right click on the snippet
or Escape leaves. The Pen and Eraser buttons pressed again cycle their
tool through its shapes, so a hand with no keyboard can draw a line with
a plain drag. Holding Alt picks the snippet up instead of drawing on it.

The mode exists so a plain press on a snippet can mean one thing: with
drawing the default, every click on a snippet made a mark, and with a
Select tool the hand had to be switched to move anything and switched
back to draw.

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
would take a press there (a bar button, a handle, an item's body, or
nothing) and the frontmost item whose content holds the point. Furniture
first, in the order it is painted, then items front to back. Drawing and
resolving share one definition of the geometry, so what is drawn and
what is hit cannot differ. Panels that are always above every item stay
ImGui's.

### One gesture engine on the raw pipeline

Every way a snippet is selected, moved or resized is one gesture on the
left button, in `HandleItemGesture`, fed by the platform mouse callback
rather than by frame-time polling. Starting a gesture is gated on
`!io.WantCaptureMouse`, so a click on a panel is the panel's; a gesture
in flight is consumed regardless, so straying over a panel mid-drag
cannot hand the event to it. The engine is a start snapshot plus the
*full* delta from there, recomputed on every move - not an incremental
delta, which drifts under event coalescing, and not ImGui's drag delta,
which loses the grab offset against a screen edge.

A press is a click until the pointer has traveled 4px. A resize started
on one of several selected snippets scales all of them about the fixed
corner; the smallest is the floor for the group. Shift-drag on open
canvas draws a box that adds every snippet it touches to the selection.
One button at a time: the first to press owns the pointer until it lets
go. Windows' press-and-hold on a touch screen injects a right press into
a held finger's left press, and the app does not depend on the OS being
asked not to.

What the pointer is doing is one field, `OverlayApp::gesture_`: a
`std::variant` of the seven things a held button can be in the middle of -
moving or resizing snippets, holding a bar button, dragging a selection
box, framing a snippet, a stroke, a right-drag erase, a right click on
empty canvas - or none. It was a
field per kind, which excluded each other only by the order `OnMouse`
asked in, and ending "whatever is in flight" meant knowing every field
it might be; each place that forgot one was a bug. A drag went on moving
a snippet a Delete had hidden and filed that move after the delete, so
the first undo did nothing to be seen; an undo mid-drag restored a
placement the drag then wrote over, losing that step; a stroke outlived
the Escape that left its drawing mode; and a canvas switch settled the
left button's gestures only, so a right-drag resize went on across it.
There are two ways to end one early. `ReleaseGesture` is a release where
the pointer is, through `OnMouse` - what a canvas switch, a capture
hotkey and hiding the overlay do, so a region being framed is made and a
held bar button fires. `EndGesture` keeps what is done and makes nothing
new - what delete, undo, redo and leaving drawing mode do: a move is
filed, a stroke kept, and a region not yet made is dropped. An undo
pressed mid-drag or mid-stroke therefore takes back what the hand has
done so far, the most recent thing done. Both also disarm a press held
still, whose hold would otherwise enter drawing mode half a second after
the press it came with had been ended. Either way the rest of the held
button's drag finds nothing in flight and does nothing.

The selection bar floats over the selection's bounding box, or below it
when there is no room, or inside its top edge for a fullscreen snippet.
Its buttons fire on release over the same button, ImGui's own rule. The
Properties popover opens through a request flag rather than
`ImGui::OpenPopup` from the release: the raw callback runs during the
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
to do what Ctrl+D does. Paste is not among them: it has nothing to do
with the snippet the menu is over, and is on empty canvas's menu. A row that cannot be chosen right now is grayed rather than
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
popover (the bar's More button) had a row of the same actions as icon
buttons, and kept it for a while after the menu arrived; it went, and
the popover is left with what describes a snippet rather than what is
done to it - the two opacities, the background color and the text's
size and color. Its colors are a picker each, with no preset swatches
beside them: the picker does the whole job, and a row of presets was a
second way to do part of it. The background keeps one swatch, white,
because white is the one color with a meaning there - the no-op tint
that gives a capture back as it was - and hitting it exactly in a picker
takes aim.

Which buttons either bar carries is a setting, and anything the file gets
wrong is made sense of rather than obeyed: a name from the other bar is
dropped, a duplicate kept once, and a button the file never mentioned is
appended shown, since a new button arriving invisible is a feature that
silently isn't there.

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
out from under the pointer.

A tile's own context menu is a right click on it, which does not also
switch to that canvas: a menu is opened to act on something, not to go
to it. The bar is held out for as long as the menu is up, since the
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
scaled as a group about its middle the way a corner handle scales it,
kept between the smallest snippet's floor and the screen. The mode
decides rather than whether anything is selected, because in drawing
mode something always is - the snippet under the pen, which a size
meant for the brush must not start scaling. Ctrl and Shift with the
wheel set the selection's background and foreground opacity, in either
mode, five percent a notch within the Properties sliders' ranges, and a
toast says the value reached.

With Alt the wheel steps through the canvases of the folder the
*current canvas* lives in (not the browsed folder), without wrapping,
and ends any gesture in flight first by feeding the release the handler
is waiting for. All of these honor how far the wheel actually turned,
keeping a remainder across frames, so a fast spin is not truncated to
one step and a precision touchpad's fractions are not rounded to
nothing. For the brush, a transient size preview at the cursor is the
feedback; a permanent brush cursor is what made an earlier design feel
busy over a game.

### The Overview

A translucent backdrop and a centered panel, drawn last so ordinary
insertion order puts them above everything. Tabs: Canvases (a folder
sidebar and a tile grid with live thumbnails, drag to reorder, drag a
tile onto a folder to move it), Settings, About. What either pane asks
for is collected and applied after both have been drawn, since the
handlers read a `const&` into the live canvas vector that a mutation
would reallocate. "New canvas" and "New folder" switch to what they made
and leave the panel up; closing it the instant a button is pressed was
disorienting, and stopping at "it exists" left half the job to a click
on a tile that had just appeared. The move/copy picker deliberately does
not follow the item to its destination.

Thumbnails draw strokes by default (nearly free at tile size) and
bitmaps only when asked, because a bitmap means decoding a file for a
canvas whose pixels are deliberately not in memory. A 256px sidecar
thumbnail beside every picture makes the ordinary case a sub-millisecond
decode; the full decode is a budgeted fallback that writes the sidecar
on its way out, so an old library acquires them one visit at a time. A
layer waiting its turn draws nothing rather than the placeholder
gradient, which would read as thumbnails being wrong and then correcting
themselves.

Settings is a list of sections down the left, not one long scroll.
Appearance, Drawing and Debug are about *you* and always global; Behavior
and Hotkeys are about *whatever is underneath* and are what a profile may
override, which makes the section boundary the rule. Hotkeys holds the
three global summon keys above its profile picker and the rebindable tool
keys below it, since a control that governs what is below it must have
nothing above it that it does not govern. Behavior does the same with
deleted-item retention. In both sections each half sits in its own box,
badged Global or Per profile, and the per-profile box carries the accent
down its edge. A heading alone read as one more group of the list,
rather than as the line past which a profile changes things. Most rows bind ImGui widgets
straight to the settings' fields and commit on a finished edit; color
swatches commit on deactivation rather than on every frame of a drag,
which wrote the file sixty times a second.

Hotkeys are the one setting that cannot just be written: an OS
registration can fail, so the editor asks the controller and commits only
if it accepts. A combo one of the app's own other hotkeys has is taken
from it rather than refused. While a row waits for a key, a press of one
of the app's own combos never reaches the capture loop as a key - Windows
hands it to its hotkey - so the hotkey handlers ask first, and while a
row is armed the hotkey firing *is* the press.

### The cheat sheet

Every key and gesture on one panel, `Ctrl+H` by default and in the
empty canvas's menu. It exists mostly for what nothing else shows: a
tool key is on its button's tooltip, but Alt-drag in drawing mode, a
right-drag that resizes or the modifier that makes a press a drawing are
nowhere on screen. The welcome note shrank to match: one gesture, the
right-click menus, the key that brings the overlay back and the sheet's
own key. The other hotkeys moved to the sheet, where they can be kept
current.

Rows are built from the bindings as they are (`BuildCheatSheet`): the
summon hotkeys, the shortcuts the active profile resolves to and the
creation triggers. A rebound key reads as rebound, and an unbound one or
a trigger set to Off drops its row rather than showing "(none)". The
text of each row is fixed, while the keys come from the settings. A
hand-written page could not do this: it would be out of date the first
time someone rebound a key. The welcome note fills in its two keys the
same way. When the sheet's key is unbound, the note sends you to the
menu instead.

The sheet is a panel over a dimmed canvas, like the Overview, and shares
its backdrop. `PanelOpen()` is the one condition the canvas's keys,
wheel and pointer defer to for both panels. While the sheet is up its
own key is the only shortcut that runs, and Escape closes it and does
nothing else. A tool picked up under a panel that hides the canvas
would be a change nobody saw happen. Its own key is a `ShortcutAction`
like the tools, so it is rebound in Settings > Hotkeys and a profile can
override it. The panel sizes itself to its text and fits up to three
columns within the Overview's margins. Its six groups are split across
the columns so that the tallest column is as short as it can be.

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

A deleted folder is looked into rather than browsed: the browsed folder
is where a new canvas lands, and the manager never lets that be a
deleted one, so the UI keeps the deleted folder it is showing
(`deletedFolderShown_`) and New canvas is off while it does. Restored,
the folder becomes the browsed one. A deleted canvas cannot be opened -
restoring it is how it is opened - so nothing needs a read-only check;
the live folders and canvases around it stay fully usable, only dimmed.
Snippets are left out: a deleted snippet comes back by undo or not at
all (see Deletion is a mark).

Two designs came before. The first showed deleted things in place in a
mode that let them be looked at but not changed, which needed a
read-only check at every edit and a banner for a deleted canvas. The
second was a list of everything with a stamp of its own, newest first,
with a preview, what it held and when it went. It answered "what did I
delete half an hour ago", but not where a restore would put a thing, and
snippets - which undo already covers - crowded out the folders and
canvases it was for.

### Cursors and the demo mark

Every cursor is ImGui's except the crosshair and the pen, which ImGui's
set does not have and every OS does, so those go through
`IOverlayWindow::SetCursorShape`. `WantedPointerShape` is the one place
that says what the pointer means; both the OS cursor and the drawn
software pointer read it, so they cannot disagree. It is re-asserted
every frame because the backend's own cursor push lands one frame late.

The demo watermark is drawn wherever the overlay is visible, above every
snippet and below only the Overview: a mark a snippet can be parked on
top of is not one. It wanders every ten seconds across a 3x3 grid, never
landing where it was, so it cannot be hidden permanently under a snippet
that is never moved again. It needs no special handling for capture:
the platform hides the whole overlay before grabbing pixels.

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
  inserted (the back). The app re-asserts its layers to the front every
  frame, in the order it draws them.
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
- `AddRect`'s order is `(min, max, col, rounding, thickness, flags)`; a
  flag in the thickness slot compiles and draws garbage.
- `WindowRounding` feeds a window's *minimum height*; a pill value of
  999 is safe only for frame and grab rounding.
- A popup's id is scoped to the window current at `OpenPopup`/
  `BeginPopup`; both must run inside the same `Begin`/`End` block or the
  popup silently never opens.
- `BringWindowToDisplayFront` wins by running *last*; a window that
  re-asserts itself must do so before any popover it opens renders, and
  a widget's own internal popup (`ColorEdit3`'s picker) has no `Begin` to
  hook, so `KeepChildPopupsInFront` walks the open-popup stack for it.
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

## The tray controller: hidden, edit and view-only

`app::TrayController` owns the settings, the session and the overlay,
loads the library, and is the only place that knows what a hotkey does.
Two hotkeys drive three states:

```
hidden --edit hotkey--> edit        edit --edit hotkey--> hidden
hidden --view hotkey--> view        view --view hotkey--> hidden
edit   --view hotkey--> view        view --edit hotkey--> edit
```

Each hotkey toggles its own mode off and switches straight to its mode
otherwise, including directly between edit and view with no hide and
reshow. The controller is deliberately stateless about which state it is
in: "hidden vs visible" is the window's `IsVisible()`, "edit vs view" is
the overlay's `IsViewOnly()`, both ground truth something else maintains,
so no `mode_` member can go stale.

**Pinned snippets.** Whenever the current canvas has a pinned snippet,
"hidden" is the pinned view instead: view-only with every other snippet
left out, click-through, never focused, put up the way a notice is. It
stands in for hidden throughout, and there is deliberately no hotkey that
hides it - unpinning is how pinned snippets go.

**Notices.** A fourth state the hotkeys never ask for: view-only with the
canvas left out, so the only thing on screen is a message. It exists for
the silent capture hotkey, which acts while the overlay is hidden and
still has to say what it did. Three things about it were found by
watching it fail on the real thing: showing a window activates it
(`SW_SHOW` takes focus by itself, hence `ShowClickThrough`); a
window given `WS_EX_LAYERED` before its first show draws nothing, so the
order is show first, click-through styles second - with the window
already counted click-through for the input grab, which a show in edit
mode starts and the passthrough a moment later took down again; and ImGui's clock is not the
app's clock, so the first frame after an hour in the tray carries an
hour's delta and puts every expiry set while hidden in the past - the
renderer caps the delta at 0.1 s.

**View-only draws only now and then.** Its picture does not change by
itself, and with pinned snippets it can sit over a game for hours, where
drawing at the refresh rate cost 2% of a core. The overlay tells the
window which frame pacing it wants; idle, the Win32 loop draws a frame
every 250 ms plus one after any dispatched message, and sleeps in
`MsgWaitForMultipleObjectsEx` in between. Measured: 31 ms of CPU per 10 s
in the pinned view, against 203 ms before.

**Profiles are resolved once, on the way up**, before `Show`, which is
what makes the application underneath the answer rather than this
window. A restart the overlay does to itself (for a setting only read on
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
if the OS accepts. A set combination that cannot be registered is fatal
for the three that bring the overlay up; the silent capture's is an
extra, and a machine where another app owns it still gets an app that
runs. A first run - nothing on disk at all - shows the overlay in edit
mode with a welcome note, since an app that installs a tray icon and then
waits for a chord it never mentioned is indistinguishable from one that
did not start.

Two notes sit beside the welcome, in larger, light-red text. One says to
set up Behavior and profiles per program, because no one set of input
defaults suits every game. The other warns that anti-cheat systems may
object to an input hook drawing over a game. They are notes rather than
a dialog for the reason the welcome is: they can be moved or deleted
like anything else, and they show how the app works. Being deletable is
also why the About text repeats both warnings. They sit in a row with
the welcome, or in a column on a screen too narrow for that, so that
none of them covers another.

`Flush` is called before hiding and before exiting, the two places
content stops being editable and no frame will come soon enough to catch
the debounce.

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

### Text is UTF-8, and so is the code page

Every string in the app is UTF-8, but on Windows the narrow side of a
`std::filesystem::path` is the process's ANSI code page, and MSVC's
`path::string()` throws on a character that page cannot spell. A canvas
directory renamed by hand to Japanese on an English Windows, or a file
named with an emoji dropped beside a snippet, crashed every load or every
save that listed it. Rather than convert at each of those places, every
executable, the tests included, carries a manifest
(`src/platform/win32/resources/utf8.manifest`) that makes UTF-8 the
process's code page (Windows 10 1903 and later): `path::string()` and
`path(std::string)` then round-trip exactly, and the `-A` Windows calls
take the same UTF-8 the UI strings are in.

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
would fight blur-behind. Toggling passthrough also hands keyboard focus
back to what had it, since click-through alone only stops mouse routing
and the overlay would otherwise keep eating the game's keys.

`Hide` restores focus only if this window still holds it: once view-only
has handed focus off, the user may have clicked into other windows
through the click-through overlay, and forcing a stale memory over what
`GetForegroundWindow` already points at can only make things worse.
Alt+F4 and `WM_CLOSE` are swallowed: `DefWindowProc` would destroy the
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
rendered again and its own release never runs. `CloseOverview` releases
for it; `ReleaseTextInput` is idempotent, so both routes running is
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
  The app thread owns the thread from `CreateThread` to the `CloseHandle`
  after it has been seen to exit. Starting it is a handshake: a thread
  has no message queue until it first asks for one, `PostThreadMessage`
  to a thread without one fails, and the first reconcile request is
  posted the moment the start returns - so the thread signals an event
  once its queue exists and the start waits for that. The thread is in
  one of three states the app thread can tell apart - starting (the
  event still open), running, stopping (a quit posted and not yet seen
  to land) - and a reconcile request is posted only to a running one:
  a post to a thread still on its way up is lost, and a thread on its
  way out would run it behind its quit, or not at all. A start whose
  wait times out keeps the event for the thread to set when it gets
  there; a stop whose two-second wait times out keeps the handle rather
  than forgetting a live thread, so that the next start finds it (waits
  for it once more, or finds it finished and closes it) instead of
  starting a second thread over the same hooks and sink; a stop whose
  quit could not be posted because the queue is not up yet waits for
  the queue and posts again. The first version ignored the wait's
  result and closed the event whatever it said, and a start after a
  timed-out stop posted to a thread that was about to quit. These paths
  are established from the source: the class offers no fault injection,
  and the rapid start/stop test exercises ordinary timing.
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
  by construction.
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
  was gone. A hotkey fires on a press, never on its repeat, as
  `RegisterHotKey`'s `MOD_NOREPEAT` does; held a moment too long, the
  edit hotkey opened the overlay and closed it again. Mouse buttons follow
  the same rule, so a drag in the application underneath ends there when
  its button comes up. Measured with injected input against the build
  before these: left Alt stayed down after AltGr+O, a repeated key stayed
  down, and the held hotkey left the overlay closed.
- **The hooks stand down when the app thread stops.** They swallow the
  machine's mouse and, with forwarding off, its keyboard, whatever the app
  thread is doing, and the way out - the hotkey - is posted to that same
  thread. Hung there, the machine had no input short of Ctrl+Alt+Del. The
  window stamps a heartbeat every frame it renders (at least four a
  second while it is up), and with none for two seconds both hooks pass
  everything through until one comes. Checked by suspending the app
  thread with the overlay up: input stayed with the overlay for half a
  second, reached Windows after three, and was taken back once the thread
  resumed.
- **Typing needs the keyboard, not focus.** ImGui implements text editing
  from key events; what it cannot do is turn a virtual key into a
  character, which is the layout's job and arrives as `WM_CHAR` only for
  a focused window. The grab closes that gap with `ToUnicodeEx` against
  its own keyboard state (the thread's reports nothing held, so every
  letter would come out unshifted), the overlay window is registered
  wide so a posted `WM_CHAR` carries a UTF-16 unit, and
  `TranslateMessage` is skipped for the key-downs the grab posts or every
  letter arrives twice. IME composition genuinely needs a focused window
  and still borrows one.
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
The HUD shows such a row as `--` rather than `ON`. The HUD itself is off
by default because its number keys can only reach a focus-less overlay
through the keyboard hook, so an always-on HUD ate digits even with
keystroke forwarding on.

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

`CaptureRegionAsTexture` excludes the overlay from capture
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
window of known color with and without it); before Windows 10 2004, where
the affinity cannot be set, the overlay is still hidden for the moment.
The exclusion is not left on: a screenshot or a stream the user takes of
their own screen should show the overlay. The rectangle goes through `ClientToScreen`, so a capture
comes from the overlay's display rather than from wherever its
coordinates land on the primary. Textures are `D3D11_USAGE_DEFAULT`
rather than immutable so a painted layer can be updated in place.

### Picture scaling

Every picture in a snippet - a screenshot, a painted layer, the
Rasterized strokes - is drawn through `DrawPicture`, resampled the way
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
taps a pixel. So every texture has a full mip chain, the shader reads
the level just above the target size and widens the kernel by what is
left - at most 2x, so 12x12 taps at worst for Lanczos and far fewer for
a picture shown near its own size. The mips are box-filtered, which is
where some quality goes; this keeps most of it for a fraction of the
cost. Other routes were weighed: a pre-scaled copy per snippet, redone
whenever its size changes, is the best quality but a cache to invalidate
on every resize and brush stroke; a separable two-pass filter needs an
intermediate target per picture per frame.

**The mips are built by hand.** `GenerateMips` averages what it is
given, and the pictures are straight alpha: a painted layer is mostly
(0,0,0,0) around its ink, so a plain average darkens every edge toward
black as the picture shrinks. `BuildMips` averages premultiplied instead,
one full-target triangle per level, and the resampling shader sums
premultiplied too, then clamps - both kernels have negative lobes that
ring past 0 and 1 at a hard edge. A texture's chain is rebuilt once at
the start of the next frame after it is created or updated, however many
brush moves there were in between. The chain costs a third more memory
per picture whatever the filter, and Bilinear and Nearest never read it:
ImGui's samplers clamp to the top level.

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
normal focus routing, and once they open a window focus follows it. Only
a global low-level keyboard hook swallowing the Windows key could
prevent that, which is far more invasive than an overlay should be by
default. Exclusive-fullscreen games sidestep all of this by not sharing
the desktop, which an always-on-top overlay deliberately does.

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
  canvas model, persistence against a real temporary directory, the
  config, the session and the settings. They are where a test of new
  behavior belongs before any UI reaches it.
- **Headless app tests** (`tests/app/`) run the whole app - a real
  `OverlayApp` driven through a real ImGui frame - over the in-memory fake
  platform, with nothing drawn anywhere. ImGui needs a context and a font
  atlas, not a window or a GPU, so every path the app has is reachable
  from an ordinary test. Input arrives the same two ways it does in the
  real app: ImGui's own event queue, which widgets see, and the platform
  mouse callback, which the raw drawing pipeline runs on.
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

`linux-tests` builds the portable core and its tests with GCC or Clang.
Worth running now and then even when working on Windows: MSVC is the
more forgiving reader, and core can drift for weeks into a shape only it
accepts. Two examples that happened: a braced default argument for a
nested aggregate (a hard error on GCC), and constructor initializers out
of declaration order (`-Wreorder`, which MSVC leaves off even at `/W4`).
Configuring a scratch MSVC build with `/permissive- /W4 /w45038` catches
most of this class without a Linux machine.

## Dead ends, for the record

Things that were built, used and removed. Each is described where it
matters above; this is the index, so nobody spends an afternoon proving
one twice.

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
- **A Recently deleted list.** Newest first, snippets and all; could not
  show where a restore puts a thing. Replaced by Show deleted, in place.
- **Profiles based on other profiles.** A third level nobody could see;
  replaced by exactly two levels.
- **One library file.** 42 MB rewritten every two seconds at fifty
  canvases; replaced by a directory tree that is its own index, and then
  by a save whose writes are bounded by what changed.
- **PNG for captures.** Six to twenty times slower than QOI on this
  app's own screenshots.
- **Loading every canvas's textures at startup.** 1.6 GB of VRAM behind a
  game for fifty 4K captures; replaced by per-canvas residency.
- **A global undo stack.** Undid strokes on canvases not on screen;
  replaced by a stack per canvas.
- **`SetCursorPos` to drive the real cursor under a grab**, fractional
  pointer drawing, an integral term for counter-injection, a dedicated
  sink thread, `BlockInput`, and a null-device-handle fallback for
  recognizing injected input. All in the input grab section.
- **A Linux dev harness** (GLFW/OpenGL, an ordinary window showing the
  same UI) and a **MinGW cross-compile preset**. Useful once for
  iterating without a Windows machine; not carried into this repository.
  Core stays portable and the `linux-tests` preset keeps that honest;
  what the cross build taught is under "Cross-compiling".
