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
    build/windows-release/tools/perf_library/perf_library.exe --out %TEMP%/libs/medium \
        --canvases 1 --items 11 --strokes 13 --points 130 --width 1920 --height 1080

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

`PerfBench.OneFrameAgainstAGeneratedLibrary` in `tests/core/perf_bench_test.cpp`.
Opt-in - it skips unless `SZ_PERF_LIBRARY` names a library directory:

    set SZ_PERF_LIBRARY=%TEMP%\libs\medium
    set SZ_PERF_MODE=tessellated          # or polyline, rasterized
    build\windows-release\tests\sz_core_tests.exe --gtest_filter=PerfBench.*

It runs the identical per-frame path the app runs - `NewFrame`, `OnFrame`,
`Render`, so the whole app plus all of ImGui's own work - with no vsync and no
GPU in the way, and reports the median of 240 frames after 30 warm-up frames.
It is deterministic and repeatable to a few percent.

What it does **not** measure is what the GPU then does with the draw lists.
That is deliberate: the CPU side is where the app's own decisions show up.
One consequence worth knowing - `SZ_PERF_MODE=rasterized` is **not
meaningful** here. Rasterized needs GPU textures, the headless harness has no
window, so it falls back to tessellated and reports identical vertex counts.
Measure that mode with instrument 2.

## Instrument 2: the real app

`tools/perf_library/measure.ps1` runs a real build against a generated
library in an isolated `APPDATA`, so it can neither touch nor be perturbed by
the real one.

    .\tools\perf_library\measure.ps1 -Exe <path-to-exe> -LibrarySource %TEMP%\libs\heavy `
        -Mode tessellated -Seconds 5 -Repeat 3 -ShowFpsHud -Screenshot heavy.png

`-ShowFpsHud` turns on the **input-options HUD**, whose first line is the
frame rate and frame time. That is `showInputOptionsHud`, *not*
`showDebugOverlay` - the latter draws the cyan border and the canvas/mouse
readout and carries no timing at all. The HUD also claims the number keys
while it is up, so don't send digits during a measurement.

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
- **The app shows itself on some starts and not others** (a first run puts
  the welcome note up), so "sleep, then press the hotkey" is a coin flip
  between showing the overlay and hiding it again. That produced samples of
  0.0% - the app sitting in the tray with the loop parked in `GetMessage`,
  drawing nothing. The script now polls for visibility and checks it either
  side of every sample.

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

`PerfBench.SavingTheWholeLibrary` measures one autosave. Before, a save cost
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
| JSON serialisation | ~560 ms | 54% |
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

## Where the floor is

`empty` costs 0.005 ms per frame headless but 3-5% of a core in the real app.
Almost nothing the app does per frame is what an idle overlay costs; it is
presentation. That is worth remembering before optimising frame *content* to
make an idle overlay cheaper - the lever there is not drawing the frame at
all.
