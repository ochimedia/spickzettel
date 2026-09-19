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
  `bars`, `overview`, `display`, `input`, `shortcuts`, `diagnostics`).
  `AppConfig`'s fields stay flat and the mapping lives in the
  serializer. The grouping is not cosmetic: `input` and `shortcuts` are
  exactly the settings a per-application profile may override, so a
  profile is those two objects again, sparse.
- **Absent means inherit, `null` means explicitly unset.** "Said
  nothing" and "said none" have different spellings, which a shortcut
  that ships bound and is unbound on purpose depends on. Every setting
  is written, defaults included, so the file documents what can be set.
- **`ordered_json`, and floats rounded to six decimals**, because the
  file is meant to be opened and read: alphabetical keys interleave
  settings by spelling, and `0.22f` promoted to double writes as
  `0.2199999988079071`.

Malformed input is never an error: a value of the wrong type, out of
range, or a file that is not JSON at all leaves every setting at its
default, the same contract the library has. A hand-edited config is
treated as absent, not as a reason to crash on startup. The file is
written through temp-then-rename, since truncating it in place leaves a
window in which every setting is a half-written file.

`KeyCombo` represents a hotkey as modifiers plus one logical key rather
than an OS virtual-key code, and no modifier is required: a bare
function key is a legitimate hotkey, and refusing plain letters is a
possible later restriction rather than a rule today.

### Tool shortcuts

Every drawing tool, creation tool and clipboard action can carry a key,
pressed while the overlay is up in edit mode. Four ship bound (`S`
screenshot, `D` drawing, `E` eraser, `P` pen) plus the clipboard's usual
`Ctrl+C/X/V`; the rest start unset, because a shortcut that fires a tool
you did not want is worse than no shortcut. These are not OS hotkeys and
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

What is overridable is exactly the `input` and `shortcuts` groups: the
settings about the machine in front of you rather than about you.
Colours and the rest are deliberately not.

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
colour and width, where a gesture starts and what it is over, panel and
popover state, toasts, and GPU caches that exist only for drawing.

### Autosave

`Session::Tick` runs every frame and compares the model's generation
counter against what was true last frame and what was last saved. A
write fires once the generation has been unchanged for 2 s, coalescing a
burst of edits into one write, or after 15 s regardless, so a long
uninterrupted session is still persisted. `Flush` is called at the two
places content stops being editable: before hiding the overlay and
before exiting. A failed write (disk full, a file held open) is retried
on a clock of its own, doubling up to 30 s; falling through to the quiet
check, which a failed save does nothing to reset, retried on every frame
and turned a full disk into a synchronous rewrite per frame. A save is
acknowledged only when *all* of it landed, painted pixels included, so a
layer whose write failed is retried rather than waiting for an unrelated
edit.

Painted pixels are written before the texture sync discards anything
non-current, and the release path refuses to drop a layer that is still
dirty. Waiting for the debounced save was not good enough: switching
canvas bumps the generation, which pushes the save *further away* at the
exact moment the pixels are thrown out.

Windows session shutdown (`WM_QUERYENDSESSION`) is not hooked, so a
change inside the debounce window at that moment could be lost; the
tray's Exit is the only quit path that flushes.

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

### Undo is per canvas

History is a `deque` per canvas, capped at 50 entries *and* 128 MB of
what they hold (a Clear drawing holds a whole fullscreen layer, 8 MB;
fifty of them was 400 MB on one stack). A canvas is this app's document,
and undo scoped to a document is what every editor does. One global
stack reached across canvases and failed invisibly: draw on A, switch to
B, draw, come back to A, press Ctrl+Z, and the stroke that vanished was
B's, on a canvas you were not looking at.

Six kinds of entry, and every one is either its own inverse or a mirror
with the direction as the only difference, so undo and redo are one walk
in opposite directions through one dispatch. A `StrokeBaked` entry
carries the stroke so redo can push it back; an `Erased` entry carries
the whole originals a gesture touched and the fragments that replaced
them, built by a snapshot/diff at the gesture's ends rather than by
accumulating per-call results, since a stroke clipped once mid-gesture
can be clipped again by a later call in the same drag; the painted half
of the same gesture rides in the same entry, so one drag is one undo
whichever kinds of ink it touched. `ItemDeleted` and `ItemCreated` carry
an id and toggle the mark. `NoteTextChanged` and the painted entries swap
their contents with the item's, so the popped entry is already what the
opposite stack needs. Undo is best-effort about staleness: an entry
naming something gone does nothing and is dropped rather than moved to
the other stack.

Deliberately narrow: moves, resizes, reorders and renames are not
tracked, and deleting a canvas or folder gets a confirmation and Recently
deleted instead of an undo entry.

### Making a snippet is on the history, and an untouched one goes

Snippets are made through `Session::CreateItem`, so a screenshot taken
by mistake can be undone into Recently deleted and redone out of it. A
drawing a press made is watched until the hand moves on, and
`DiscardIfUntouched` erases it for good if nothing was put into it: no
strokes, no paint, no text, no picture. A screenshot is content even when
its capture failed.

### Freezing the screen

`FreezeScreen` captures the whole display through the same call a
snippet's capture uses, which hides the overlay for the duration, so the
picture is the application underneath and none of our content. While a
screen is frozen a region capture is cropped out of it rather than taken
live; without that the user drags a region over a still picture and gets
back whatever the game showed a moment later, which for a moving camera
is a different scene. The pixels are kept alongside the texture for
exactly this. Released on hide: it is a fullscreen texture with no reason
to exist while nothing is shown.
