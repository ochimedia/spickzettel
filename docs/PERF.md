# Measuring the overlay

Two instruments, because they answer different questions, and one of them is
much less trustworthy than it looks.

## The scenarios

`tools/perf_library` writes a synthetic library through `CanvasManager` and
`LibraryStore` - the app's own model and serializer, so a generated library
is by construction what the app itself would have written. The strokes are
long smooth scribbles sampled every few pixels, which is the shape a hand
actually leaves; a hundred short straight lines with the same total ink cost
a fraction as much, so a generator that produced those would flatter the app.

    cmake --build build/windows-release --target perf_library
    build/windows-release/tools/perf_library/perf_library.exe --out %TEMP%/libs/medium.db \
        --canvases 1 --items 11 --strokes 13 --points 130 --width 1920 --height 1080

`--opacity P` gives every snippet that opacity (default 1). Below 1 each
snippet's strokes are drawn through a layer of their own (see
ARCHITECTURE.md, "Drawing strokes"), which is the costlier case; the
stroke-layer results below use 0.8.

The scenarios used below, all laid out for 1920x1080:

| name      | canvases | items | strokes | points  | what it is for |
| --------- | -------- | ----- | ------- | ------- | -------------- |
| `empty`   | 1        | 0     | 0       | 0       | the floor: an overlay on screen with nothing on it |
| `light`   | 1        | 3     | 39      | 5,070   | a few snippets, the ordinary case |
| `medium`  | 1        | 11    | 143     | 18,590  | a working canvas |
| `heavy`   | 1        | 40    | 520     | 67,600  | a canvas someone has really used |
| `extreme` | 1        | 40    | 1,600   | 320,000 | past anything reasonable, to find where it breaks |
| `gallery` | 12       | 6/ea  | 936     | 121,680 | many canvases, for the Overview's own mesh cache |

`medium` was sized to match a real library that prompted this work: 143
strokes and 18,590 points against its 145 and 18,638. The app reports 91,328
vertices per frame for `medium` and reported 90,292 for the real one, which
is the check that the generator produces a comparable load rather than merely
a similar-sounding one.

## Instrument 1: the headless frame benchmark (use this one)

`PerfBench.OneFrameAgainstAGeneratedLibrary` in `tests/app/perf_bench_test.cpp`.
Opt-in - it skips unless `SZ_PERF_LIBRARY` names a library file:

    set SZ_PERF_LIBRARY=%TEMP%\libs\medium.db
    build\windows-release\tests\sz_core_tests.exe --gtest_filter=PerfBench.*

It runs the identical per-frame path the app runs - `NewFrame`, `OnFrame`,
`Render`, so the whole app plus all of ImGui's own work - with no vsync and no
GPU in the way, and reports the median of 240 frames after 30 warm-up frames.
It is deterministic and repeatable to a few percent.

What it does **not** measure is what the GPU then does with the draw lists.
That is deliberate: the CPU side is where the app's own decisions show up.
One consequence worth knowing - the strokes' depth test and layers are
callbacks the headless window only names, so what they cost the driver and
the GPU is not in this number. Measure that with instruments 2 and 3.

## Instrument 2: the real app

`tools/perf_library/measure.ps1` runs a real build against a generated
library in a data folder of its own (`--data-dir`, ARCHITECTURE.md,
"Command-line options"), so it can neither touch nor be perturbed by the
real one. It refuses to start while a copy of Spickzettel runs - quit it
first - and stops only the copy it started; it warns if the real config or
library changed meanwhile.

    .\tools\perf_library\measure.ps1 -Exe <path-to-exe> -LibrarySource %TEMP%\libs\heavy.db `
        -Seconds 5 -Repeat 3 -ShowFps -Screenshot heavy.png

`-ShowFps` turns on the **debug overlay** (`showDebugOverlay`), whose
third line in edit mode is the frame rate and frame time - the input
readout, which was the input options HUD's first line until the Behavior
panel replaced the HUD. `-ShowFpsHud` still works. For frame times over
time, and what made one late, see instrument 4.

**Read the fps line, not the CPU percentage.** Process CPU sampling of a
vsync-locked GUI app is a poor instrument: on this machine, repeated
identical runs of one scenario ranged from 15% to 33% of a core. The CPU
figure is still worth having as a rough load signal, but the fps/ms line is
ImGui's own smoothed frame time and is the number to quote.

Two traps this harness had to be taught about, both of which silently produce
plausible-looking numbers:

- **The frame budget is not 16.7 ms.** Read the refresh rate; this machine
  runs at 120 Hz, so a held frame is 8.33 ms and "60 fps" means it is
  dropping half of them.
- **The app shows itself on some starts and not others** (a first run starts
  the tutorial), so "sleep, then press the hotkey" is a coin flip
  between showing the overlay and hiding it again. That produced samples of
  0.0% - the app sitting in the tray with the loop parked in `GetMessage`,
  drawing nothing. The script now polls for visibility and checks it either
  side of every sample.

## Instrument 3: the GPU, offscreen

`Win32Dx11RendererTest.StrokesGpuTime` in `sz_win32_tests`, with the same
`SZ_PERF_LIBRARY` opt-in, draws the library's current canvas into a
1920x1080 target with the real renderer, stroke by stroke and layered,
and times each frame with D3D timestamp queries - the GPU's own time, no
window, no vsync, no input. Two lessons are built in. The ways take turns
frame by frame: run one after the other, the same one varied up to 4x as
the GPU's clocks moved, and the minimum is the GPU at full speed. And it
reports submission plus GPU as well, from `RenderTo`'s start to the GPU
done, because a timestamp pair also counts a GPU waiting on commands the
CPU has not sent yet.

`Win32Dx11RendererTest.StrokesSideBySide`, with `SZ_STROKE_COMPARE_OUT`
naming a `.bmp`, writes a picture of the same snippet drawn both ways -
what a person looks at to judge the drawing, not a number.

## Instrument 4: the frame graph, in the real app

Settings > Debug > **Show frame graph** (`diagnostics.showFrameGraph`)
draws the last ten seconds in the top right corner, in edit mode, view
mode and the pinned view. It is for the question the others cannot
answer: *what was that stutter?* A hitch seen once, on someone else's
machine, is gone before any benchmark runs.

- **The frames**, one bar each for the time since the frame before -
  green within a refresh at 60 Hz or so, amber within two, red past that,
  cut off at 50 ms with the time written over it - and over it, pale, the
  time the overlay spent building the frame (`OverlayApp::OnFrame`, the
  checkpoint at its start included, presenting not). Frames paced idle -
  view mode, four a second - show only the build, over a gray line along
  the bottom: the gap is the pacing's. Time hidden is shaded.
- **The work**, a lane each: library commits, pictures encoded, checkpoints,
  screen captures, pictures read back, `config.json` written. Each is
  marked as long as it took, with its time beside it from 2 ms, and a line
  up through the frames from 1 ms, so the frame it made late is the one
  under the line.
- **The header**: the last frame's time, the worst frame and the worst
  build in the ten seconds, and the WAL's size after the last commit or
  checkpoint.

All of it runs on the app thread, so work that makes a frame late is in
that frame's interval: a commit made while input is handled lands before
the frame is built, a checkpoint at the start of a view-mode frame lands
in its build. The record is `core::Timeline`, one for the process,
recording only while the setting is on; turned off, it is let go of. It
does not change view mode's pace, unlike `showDebugOverlay`, whose
readout follows the pointer: an idle frame made late shows as such.

First reading, on 2026-09-30, on this machine's idle disk, a scratch
library: a full-screen screenshot snippet in edit mode was a **106 ms
frame** - the capture 36 ms, then its commit 59 ms, of which encoding
the picture was 28 ms. The checkpoint after going to view mode took
49-89 ms, in the build of view mode's second frame, where it shows
least (ARCHITECTURE.md, "Commits wait for nobody: the WAL").

## Results: strokes drawn layered

Measured 2026-09-29 while the render modes still stood side by side:
Tessellated is stroke by stroke, Depth-tested is the depth test alone,
Layered is what the app now does. RTX 2080 SUPER, 1920x1080 at 120 Hz.

Headless, median ms of CPU per frame, snippets at full opacity (two rounds):

| scenario  | tessellated   | depth-tested  |
| --------- | ------------- | ------------- |
| `medium`  | 0.857-0.905   | 0.869-0.905   |
| `heavy`   | 3.289-3.332   | 3.358-3.469   |
| `extreme` | 14.854-14.881 | 14.799-15.070 |

Within noise - once the body-first order moved into the tessellator,
whose meshes are cached. Reordered in `DrawStroke` every frame it had
cost 18-21%.

The GPU offscreen (instrument 3), snippets at 80%, fastest frame: `medium`
0.260 ms tessellated, 0.262 depth-tested, 0.287 layered; `heavy` 0.93,
0.96, 1.06. `heavy`'s median was higher for layered in every round (3.5
ms against 1.96), with submission plus GPU the same for all three (12.0
against 11.9) - the GPU waiting on commands after the layers' target
switches, most likely, rather than drawing more.

The real app (instrument 2), snippets at 80%, three samples of 5 s:

| scenario  | tessellated               | depth-tested    | layered                   | rasterized |
| --------- | ------------------------- | --------------- | ------------------------- | ---------- |
| `heavy`   | 120 fps, 4.7-5.1 ms CPU   | 120 fps, 4.9 ms | 120 fps, 5.8-6.2 ms CPU   | 120 fps, 0.23 ms CPU |
| `extreme` | 21.3 / 22.2 ms            | 22.4 / 22.6 ms  | 23.0 / 22.9 ms            | 120 fps, 0.47 ms CPU |

A layer costs about 25 microseconds of CPU a frame - 1.1 ms for `heavy`'s
forty - most likely the driver's work for switching targets. Rasterized
drew each snippet as one finished bitmap, which is why its frames cost
nothing; it paid at every change, in memory, and in sharpness. Keeping
each layer between frames would bring a still canvas down to that too, at
a texture per snippet in video memory; not built (ARCHITECTURE.md,
"Drawing strokes").

## Results: what the stroke-mesh cache bought

Headless, median ms of CPU per frame, tessellated:

| scenario  | before (`1c529d5`) | after (`02e8bfb`) | ratio |
| --------- | ------------------ | ----------------- | ----- |
| `light`   | 0.649              | 0.284             | 2.3x  |
| `medium`  | 2.446              | 1.076             | 2.3x  |
| `heavy`   | 8.929              | 4.203             | 2.1x  |
| `extreme` | 39.231             | 18.477            | 2.1x  |
| `gallery` | 1.337              | 0.586             | 2.3x  |

The real app, RTX 2080 SUPER, 1920x1080 at 120 Hz (8.33 ms budget):

| scenario  | before          | after           |
| --------- | --------------- | --------------- |
| `empty`   | 120 fps 8.33 ms | 120 fps 8.33 ms |
| `medium`  | 120 fps 8.33 ms | 120 fps 8.33 ms |
| `heavy`   | **93 fps 10.73 ms** | **120 fps 8.33 ms** |
| `extreme` | 23 fps 42.91 ms | 41 fps 24.50 ms |

`heavy` is the case that matters: before the cache the app misses vsync on a
canvas that is well within what someone might actually draw; after it, the
frame is inside budget with room to spare. `extreme` is past saving by this
route - at 320,000 points the frame is 24 ms whatever the tessellator does,
and the remaining cost is submitting 2.1M triangles.

The headless numbers run about 4-6 ms under the real app's at the top end
(`extreme`: 18.5 vs 24.5), which is the GPU submission and present that the
headless path does not do. They track well enough to develop against and are
far more repeatable, which is the trade.

## Results: what incremental saving bought

These numbers are from the library as a directory tree, before it moved into
a database; see the next section for what the database costs.

`PerfBench.SavingTheWholeLibrary` (since replaced by `WritingACommand`, see
below) measured one autosave. Before, a save cost
the whole library however little had changed, and it runs on the render
thread:

| scenario  | before | after, nothing changed | after, one snippet changed |
| --------- | ------ | ---------------------- | -------------------------- |
| `light`   | 52.6 ms   | 1.20 ms | 6.54 ms  |
| `medium`  | 135.3 ms  | 1.29 ms | 6.85 ms  |
| `heavy`   | 442.7 ms  | 1.50 ms | 7.77 ms  |
| `extreme` | 1310.4 ms | 2.38 ms | 18.76 ms |
| `gallery` | 857.6 ms  | 1.83 ms | 7.49 ms  |

And the first save after a load, once `Load` began establishing the same
record of what is on disk (see below): `medium` 1.31 ms, `gallery` 2.43 ms -
the nothing-changed cost, where it had been the "before" column once per
start.

The shape is what matters more than the numbers: the cost tracks what
changed rather than what exists. `gallery` is twelve canvases, and editing one
snippet in one of them costs one file rather than all hundred.

The three things a save was spending its time on, measured separately before
any of it was fixed (`gallery`, 1040 ms):

| | cost | share |
| --- | --- | --- |
| reading and parsing every file to find which directory held which id | ~335 ms | 32% |
| JSON serialization | ~560 ms | 54% |
| writing and renaming | ~144 ms | 14% |

All three had to go, since fixing any one alone leaves most of the second.
`ExportSnapshot`, which looked like an obvious target - it deep-copies every
stroke - turned out to be 0.02-1.08 ms, under 0.1%, and was left alone.

Seen from the running app, on `medium`: with every file rewritten, the frame
the save landed in took the frame-rate readout from 120 fps / 8.33 ms down to
103 fps / 9.67 ms. Bounded by what changed it reads 120 fps / 8.33 ms - no
measurable dip.

**The first save of a session costs what the load repaired.** For a while it
was deliberately a full one, on the grounds that `Load` reconciles what it
reads and memory may not match disk afterwards - which was true of the
repaired records and of nothing else, and on the review's 50-canvas fixture
cost 3.9 seconds on the render thread for the sake of them. `Load` now notes
each record that came back exactly as a save would write it, and the first
save writes the rest: a copied directory's fresh id, a corrected `folderId`,
an order file that disagreed with its directories, a record in an older
shape. The bench reports it as "first after load"; on a clean tree it is the
nothing-changed cost.

## Results: saving into the database

The same bench after the library moved into one SQLite file (see
ARCHITECTURE.md, "Persistence"), release build, same machine:

| scenario  | file    | nothing changed | one snippet changed |
| --------- | ------- | --------------- | ------------------- |
| `light`   | 100 KB  | 0.52 ms | 9.89 ms  |
| `medium`  | 228 KB  | 0.98 ms | 10.22 ms |
| `heavy`   | 692 KB  | 2.06 ms | 12.12 ms |
| `extreme` | 2668 KB | 4.61 ms | 16.86 ms |
| `gallery` | 1204 KB | 3.45 ms | 13.25 ms |

"Nothing changed" is now serializing every snippet to compare it with what
was written - a record and a stroke blob each, hashed - where the tree hashed
fields directly; it grows with the library and is a few milliseconds at the
top. "One snippet changed" has a floor of about 9 ms whatever the library
holds: that is the commit making itself durable, the journal and the file
each flushed to the disk (`synchronous=FULL`). The tree's writes were not
flushed at all, which is also why a power cut could leave half of one. The
first save after a load costs what a nothing-changed one does.

## Results: writing each command

Since every command is written as it is made (see ARCHITECTURE.md, "Every
command is written as it is made"), there is no save to measure; what a
command costs to write is. `PerfBench.WritingACommand` measures it end to end
through the session - the checkpoint before the command, the change, working
out what changed, and the write - on the render thread, release build, same
machine:

| scenario  | file    | a stroke | a canvas switch | the whole library |
| --------- | ------- | -------- | --------------- | ----------------- |
| `light`   | 100 KB  | 10.21 ms | 8.27 ms | 36.07 ms |
| `medium`  | 228 KB  | 10.17 ms | 8.15 ms | 48.33 ms |
| `heavy`   | 692 KB  | 10.30 ms | 8.71 ms | 42.05 ms |
| `extreme` | 2668 KB | 15.07 ms | 8.66 ms | 67.81 ms |
| `gallery` | 1204 KB | 10.15 ms | 8.75 ms | 42.98 ms |

A stroke is the heaviest ordinary command - the snippet's record and all of
its strokes are written, with its canvas's order - and a canvas switch the
lightest, one meta row. Both sit on the floor the durable commit sets
(`synchronous=FULL`, the journal and the file each flushed): about 8 ms,
whatever the library holds, and the rest is what the snippet weighs - 5 ms
more for an `extreme` snippet of forty strokes of two hundred points. The
autosave's "one snippet changed" cost the same, once per two seconds of
quiet rather than once per command; its "nothing changed" pass, a few
milliseconds of serializing and hashing every snippet, is gone. Writing the
whole library is what a first run does, once.

## Results: the journal, on a busy disk

A tester saw a hitch "sometimes, after making some snippets or drawing", on
an almost empty library, and `library.db-journal` beside the file while it
lasted. The floor above - every commit flushing the journal and then the
file - is what a quiet disk costs; a flush waits for everything the disk
has queued, so it costs what everyone else is writing too.

`tools/journal_bench`, 2026-09-30, this machine's NVMe drive:
Python's `sqlite3` (SQLite 3.50) making the app's commit - one snippet row
of a 560-byte record and a 2 KB stroke blob, upserted in its own
transaction, one every 50 ms - into a database of its own per setting, the
settings taking turns in rounds of 20 so that a busy spell of the disk hits
them alike; 120 commits per setting. *Moderate* is another process writing
a file at about 50 MB/s, *heavy* one writing flat out. Milliseconds, and
how many of the 120 commits took longer than a frame at 120 Hz, 30 Hz and
10 Hz:

| disk | setting | median | p90 | p99 | max | >8.3 | >33 | >100 |
| ---- | ------- | -----: | --: | --: | --: | ---: | --: | ---: |
| idle | rollback, `FULL` (was) | 9.32 | 12.0 | 14.0 | 14.3 | 115 | 0 | 0 |
| idle | WAL, `FULL` | 2.61 | 3.0 | 5.4 | 5.8 | 0 | 0 | 0 |
| idle | WAL, `FULL`, exclusive | 2.52 | 3.0 | 5.3 | 5.4 | 0 | 0 | 0 |
| idle | WAL, `NORMAL` | 0.10 | 0.16 | 2.5 | 2.5 | 0 | 0 | 0 |
| idle | WAL, `NORMAL`, exclusive (now) | 0.08 | 0.13 | 2.5 | 2.7 | 0 | 0 | 0 |
| moderate | rollback, `FULL` (was) | 9.65 | 16.3 | 117 | 118 | 115 | 9 | 3 |
| moderate | WAL, `FULL` | 2.61 | 49.7 | 112 | 117 | 15 | 13 | 8 |
| moderate | WAL, `FULL`, exclusive | 2.66 | 4.0 | 71 | 76 | 5 | 5 | 0 |
| moderate | WAL, `NORMAL` | 0.10 | 0.16 | 2.5 | 3.0 | 0 | 0 | 0 |
| moderate | WAL, `NORMAL`, exclusive (now) | 0.08 | 0.14 | 2.5 | 6.1 | 0 | 0 | 0 |
| heavy | rollback, `FULL` (was) | 160 | 420 | 1686 | 3085 | 120 | 120 | 110 |
| heavy | WAL, `FULL` | 109 | 176 | 441 | 1203 | 120 | 117 | 82 |
| heavy | WAL, `FULL`, exclusive | 108 | 178 | 360 | 514 | 120 | 117 | 87 |
| heavy | WAL, `NORMAL` | 0.20 | 70 | 618 | 1771 | 35 | 19 | 11 |
| heavy | WAL, `NORMAL`, exclusive (now) | 0.14 | 86 | 681 | 988 | 27 | 16 | 10 |

And the checkpoint that moves the WAL into the file, after each round of 20
commits - median and max of the 6, in milliseconds:

| setting | idle | moderate | heavy |
| ------- | ---- | -------- | ----- |
| WAL, `FULL` | 3.4 / 8.4 | 3.4 / 52 | 112 / 153 |
| WAL, `FULL`, exclusive | 3.5 / 4.0 | 5.9 / 6.6 | 125 / 273 |
| WAL, `NORMAL` | 7.3 / 7.8 | 70 / 138 | 1767 / 3746 |
| WAL, `NORMAL`, exclusive | 8.0 / 17 | 65 / 126 | 1525 / 1900 |

- **A commit costs the flushes it makes.** Rollback with `FULL` flushes
  twice (the journal, then the file), WAL with `FULL` once, WAL with
  `NORMAL` not at all - 9, 2.6 and 0.1 ms on an idle disk.
- **The hold (`locking_mode=EXCLUSIVE`) costs nothing, and buys nothing,
  in time**: hundredths of a millisecond a commit. The two differ in the
  tails of the busy runs by which of them a busy spell happened to land
  on - 120 commits give a noisy p99. It is there for the network drive
  and the second computer (ARCHITECTURE.md, "Persistence").
- **On a moderately busy disk only `NORMAL` stays unaffected**, 6 ms at
  worst; everything that flushes at commit had commits of 70-120 ms. That
  is the tester's hitch.
- **On a disk written flat out nothing is safe.** `NORMAL`'s median stays
  at 0.2 ms, but one commit in seven took longer than 33 ms and the worst
  up to 1.8 s: once Windows' cache is full, a write waits even unflushed.
  The rollback journal's median was 160 ms there.
- **`NORMAL` moves the flush to the checkpoint rather than doing without
  it**: 8 ms on an idle disk, 65-140 ms on a moderate one, and 1.5-3.7 s
  on a disk written flat out. Hence when the app makes one: out of edit
  mode, and in it while the hand has rested five seconds - where nothing
  on screen waits for it (ARCHITECTURE.md, "Commits wait for nobody: the
  WAL") - and never inside a commit on its own at every thousand pages -
  a single capture could be that. The 3.7 s is past the input grab's two
  seconds without a frame, after which its hooks let everything through
  (ARCHITECTURE.md, "The hooks stand down when the app thread stops").

The six checkpoints per setting are few, and the tails of 120 commits vary
from run to run; the medians and the order of the settings are what to
read. One machine and one drive: a slower SSD or a hard disk moves every
number up.

### Writing each command, in the WAL

`PerfBench.WritingACommand` again, release build, same machine, after the
switch (commits into the WAL, `synchronous=NORMAL`, the file held); the
median of two runs agreed within a few percent:

| scenario  | file    | a stroke | a canvas switch | the whole library |
| --------- | ------- | -------- | --------------- | ----------------- |
| `light`   | 88 KB   | 0.22 ms | 0.07 ms | 48.9 ms |
| `medium`  | 216 KB  | 0.25 ms | 0.07 ms | 39.8 ms |
| `heavy`   | 680 KB  | 0.49 ms | 0.09 ms | 42.6 ms |
| `extreme` | 2600 KB | 3.94 ms | 0.12 ms | 59.7 ms |
| `gallery` | 1192 KB | 0.29 ms | 0.10 ms | 44.8 ms |

The 8 ms floor is gone; what is left is the snippet's weight - an
`extreme` snippet's forty strokes of two hundred points serialized and
written. The whole library is still a first run's once: it is timed from
the open, which makes the schema in the rollback journal, flushed, before
the file goes into the WAL.

## Where the floor is

`empty` costs 0.005 ms per frame headless but 3-5% of a core in the real app.
Almost nothing the app does per frame is what an idle overlay costs; it is
presentation. That is worth remembering before optimizing frame *content* to
make an idle overlay cheaper - the lever there is not drawing the frame at
all.
