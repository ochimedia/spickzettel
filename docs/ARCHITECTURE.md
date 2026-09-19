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
watermark compiled in; `linux-tests` builds the portable core and its
tests on a Linux host.

Every third-party dependency is fetched with `FetchContent` and pinned to
a tag or commit, so a checkout builds with nothing installed beyond a
compiler, CMake and Ninja. Header-only libraries are marked `SYSTEM` so
their warnings do not count against the project's own warning level.

### Build-time configuration: version, flags, embedded text

Three things are decided when a binary is *built* rather than when it
runs, and all three land in `build/<preset>/generated/` through
`cmake/BuildInfo.cmake`:

| Header | Holds | Regenerated | Included by |
| --- | --- | --- | --- |
| `build_config.h` | `kVersion`, `kDemoMode` | configure | `build_info.h`, so widely |
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
quietly stopped compiling; the optimiser removes the dead side either
way. Demo mode is deliberately not a setting: a watermark that can be
switched off in `config.json` is not a watermark.

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

### Licences: what ships, and where the notices are

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
  *with* software provided the licence travels with it, and forbids only
  selling the font by itself.
- **Icon designs**: ISC (Lucide) and MIT (Feather). The SVGs here are
  drawn from this project's own path data, but both licences cover the
  designs.

googletest and imgui_test_engine only build or test the app and are not
in a release binary, so they are not in the notices.

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

`Stroke` is a polyline with a colour and a width; `CanvasState` holds a
list of finished strokes plus at most one in progress. Both are dumb on
purpose: they record what they are given, so a shape tool can hand them
exact corners and a test exact points.

### From a hand to a mark: input, fitting, tessellation

Three stages, and it is worth knowing which owns what, because the same
visible defect can come from any of them.

**Input hygiene, in `DrawTool`.** A mouse reports far faster than a hand
moves, so consecutive samples of a slow line land a fraction of a pixel
apart, and the direction between two points that close is quantisation
noise. `DrawTool` discards a sample that has not travelled 2px from the
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

Why not rasterise instead: a bitmap erase gives up resolution
independence and turns undo into pixel diffs, for a problem that a
bounded piece of segment geometry solves while keeping every stroke a
plain, inspectable polyline. (Painted layers, below, are the case where
pixels are the right answer and are treated as such.)

One edge case earned its own test: a crossing that lands within 1e-6 of
an existing vertex is deliberately not reported, to avoid a zero-length
fragment, and when a sample point sits exactly on the boundary that
leaves the walk's inside/outside state disagreeing with the pointwise
test. The walk compares the two after each segment and splits there;
without that it silently bridged the erased middle into one fragment,
which looks exactly like not having erased anything.

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

The mesh is one connected strip whose neighbouring quads share vertices,
so no triangle is drawn over another. That is what a *translucent*
stroke needs: every overlap is a place the colour lands twice, which at
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
entry. Erasing takes alpha and leaves colour, so a half-erased edge
fades instead of shifting toward black.

A layer's size is capped at 4096 on a side, and the cap is applied to
the *resolution scale*, not to the bitmap: `FitResolutionScale` lowers
the scale uniformly until the longer side fits, and because every
coordinate on the way in is multiplied by that same scale, a 5120-wide
capture paints where the pen is. Clamping the bitmap's width and height
while still mapping coordinates 1:1 cropped everything past the cap and
stretched the rest, putting a stroke a quarter of the way across the
item from the pen.

## Canvases, items and folders

`Item` is a snippet: freehand strokes over a stack of layers, at a
`rect` on screen. `Canvas` is an independent collection of items whose
order is paint order; `Folder` is a flat, non-nesting group of canvases
- one per game, or per set of levels. `CanvasManager` owns all three,
plus which folder is browsed and which canvas is current.

Two pieces of "current" are deliberately decoupled: the current canvas
is what is on screen and drawn on; the browsed folder is what the
Overview shows and where a new canvas lands. Browsing a folder never
switches away from the canvas being edited. Code that wants one of them
has to say which: "where am I working" is the current canvas's folder,
"what am I looking at" is the browsed folder.

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

### Z-order steps past what actually overlaps

Bring forward and send backward move an item past the nearest item that
*overlaps* it, not the immediate neighbour in the list. A canvas holds
snippets all over the screen, and a step over one that shares no pixels
with this one changes the order without changing anything anybody can
see, which reads as a button that does nothing.

### Deletion is a mark

A deleted folder, canvas or snippet stays exactly where it is, with
`deletedAt` stamped, and is hidden by every walk over the library. A
thing counts as deleted when it or anything holding it is marked, so
deleting a canvas stamps only the canvas, restoring it brings back
exactly what went with it, and a snippet deleted earlier keeps its own
mark. Restoring a snippet inside a deleted canvas restores the canvas
too. "Delete permanently" is the erasure the `Delete*` methods perform.

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
  library.json          - currentFolderId and currentCanvasId. Nothing
                           else: the tree is the rest.
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
  images/               - staging: where a capture waits between being
                           taken and the next save moving it into the
                           snippet that names it
  retired/<folder>/...  - what a save found the library no longer
                           holding, set aside whole rather than deleted
```

Every directory is `<slug of its current name>-<uid>`. The readable half
is regenerated on every save so it stays true after a rename; the
trailing uid is the identity, so a rename is cosmetic and a failed one
costs a stale label. Every id inside a record is spelled the same
six-character way, so a record and its directory can be matched by eye.

A tree rather than one file because one file is rewritten whole on every
save: 50 canvases of ordinary drawing is a 42 MB document taking half a
second to serialise, on the render thread, every couple of seconds of
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
same kind of inconsistency a hand edit does. The reader also tolerates a
few pre-release record shapes (numeric ids, an item's single layer
written as five fields, order files listing directory names); they are
cheap and the tree invites old files.

A snippet's pictures live in its own directory, so moving a snippet is
moving one directory with no window where the record has moved and the
picture has not. `Layer::imageFile` names a *file*, not a path, and
`FindImage` turns it into a path through the owning snippet. A
library-wide filename-to-directory map fell behind whenever a directory
moved, and two snippets can legitimately hold files of the same name.

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

### A save costs what changed

The store keeps what it last wrote - a content hash per snippet, the
exact text for the small records - and where everything lives, so a save
neither re-reads the tree nor re-serialises what it would write back
unchanged. A twelve-canvas library measured 858 ms per autosave when
every file was rewritten, for one stroke on one snippet; bounded by what
moved it is 1.8 ms when nothing did and 7.5 ms for that stroke. `Load`
establishes the same record as it reads, so the first save of a session
costs only what the load had to repair. The hash and the serialiser are
kept adjacent in the source and a test asserts every field moves the
hash, because a field added to one and not the other is an edit that is
silently never saved.

Every file goes through write-to-temp-then-rename, pictures included: a
painted layer is re-encoded over its own previous file on every save,
and truncating in place left a window in which the only copy on disk of
a drawing was the first half of it.

### Images: QOI, decoded by magic bytes

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
more than reading 1.6 MB and decoding it. `DecodeImageFromFile`
dispatches on the file's leading bytes, not its extension, so a `.png`
still loads. A 256px thumbnail is written beside every picture so the
Overview never decodes a fullscreen capture to draw a 200px tile.

A capture's pixels are written synchronously at capture time, not with
the debounced record write: a screenshot lost to a crash can never be
recaptured, where a few seconds of strokes can be redrawn.
