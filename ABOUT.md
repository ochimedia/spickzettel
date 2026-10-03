# Spickzettel

A translucent overlay for screenshotting and annotating whatever is on your screen.
Originally built for keeping notes over a game without alt-tabbing away from it.


## Getting around

- The cheat sheet lists every key and gesture, as they are bound right
  now. Open it with the button below, its shortcut, or the context menu
  on empty canvas.
- The tutorial walks through the app topic by topic. You can open it
  from Settings > Interaction > Open the tutorial, or the button below,
  whenever you want a refresher.

<!-- about:open-buttons -->


## Important notes before using it over a game

- Applications and games in particular behave differently with an overlay
  on top. Look through Settings > Behavior for various options, and make
  a profile (Settings > Profiles) for each program that needs special treatment.
- Some games watch for tools that draw over them or read their input,
  and may treat this one as a cheat. If a game might object, quit
  Spickzettel before you start it.


## Questions

### Was this app developed with the help of AI?

Short answer: yes. Long answer: I'm a developer who spends most of my
day programming and consulting for a living. I'm also a gamer and
streamer, and the idea for this app came from that hobby. AI models have
reached a point where they can realize ideas you would otherwise never
find the time for. Most of the code in this app was written by an AI
model, and changes were directed and reviewed by me. I deliberately
chose a programming language and libraries I know well, so I can
understand and judge what the machine writes. I've invested a lot of
time and thought (and also quite some money) in iterating on and
refining the ideas behind it, as well as testing it extensively by hand
in addition to an automated test suite. I hope that shows, and that you
find it useful.

The app itself contains no AI, and your screenshots never leave your
computer.

### Does this app track me, or send telemetry or other data?

No. The app contains no networking code and never connects to the
internet. Your snippets and screenshots stay in
%LOCALAPPDATA%\Spickzettel and your settings in %APPDATA%\Spickzettel.
If the app crashes, it writes a crash report to
%LOCALAPPDATA%\Spickzettel\crashes, on your computer.


## Changelog

Keys named below are the defaults; the cheat sheet shows them as they are
currently bound.

### 0.2.3

#### Features

- A button for emptying the trash, with Show deleted on in the Overview
- Paste puts snippets at the pointer, or where the context menu was opened; Paste in place (Ctrl+Shift+V) puts them where they were. Both are in a snippet's context menu too
- Select all (Ctrl+A) selects every snippet on the canvas
- A reminder when the library grows past a size you choose (2000 MB by default), pointing to the trash and to Settings > Behavior

#### Changes

- Every tool has a key out of the box, in the area around WASD: Q select, W pen, E eraser, A text, S screenshot, D drawing - and Ctrl+N makes a new canvas. A settings file from an earlier version keeps the keys it has; set them in Settings > Hotkeys
- Folders and canvases in the trash are no longer deleted permanently after 14 days by default; switch it on in Settings > Behavior. A settings file from an earlier version keeps what it says
- With Show deleted on, deleted folders are told apart from folders with deleted canvases by color - red and yellow - with a legend for the two
- Snippets are no longer named ("Region 1", "Drawing 2"): the names could not be changed and said nothing, and the dock and Move to another canvas no longer show them
- Refined the tutorial's rectangle eraser step, the Folders and canvases tutorial, and the Capturing tutorial
- Refined various help and tutorial texts
- A tutorial topic quit partway no longer goes on after a restart; Basics starts over from its welcome until it is finished or skipped

### 0.2.2

#### Features

- Drawing on several snippets at once: pick a tool on the bar with them selected
- Optional separate color for the border of a selected snippet (see Settings > Appearance > Snippet colors)
- A Questions section in About

#### Changes

- Breaking change: a library opened once with 0.2.2 cannot be used with earlier versions
- Unified the drawing and snippet bars into one selection bar
- The pen's and eraser's shapes stay picked until another is picked; clicking the highlighted tool stops drawing
- Optimized pen and rectangle stroke appearance
- Optimized appearance of borders in multiple places
- Lowered minimum size of snippets to 16x16 pixels, with a resize band around a selected snippet
- Stacked the canvas bar's two buttons, giving its tiles more room
- The tutorial no longer starts over when the library is new but the settings say it was done
- The tutorial makes its folder only once its first task comes up, so skipping it at the welcome leaves nothing to delete

### 0.2.1

#### Features

- Frame graph for diagnosing stutters (see Settings > Debug > Show frame graph)

#### Changes

- Moved the library to local AppData and optimized its performance; an existing library in roaming AppData is moved automatically
- Opening the app while an instance is already running brings up the running instance

### 0.2.0

#### Features

- Interactive tutorials (see Settings > Interaction > Open the tutorial)
- Interface scaling
- Configurable defaults for snippets
- Semi-transparent pen colors
- Mouse buttons as shortcuts (middle and side buttons)
- Per-snippet aspect ratio setting
- Pen and eraser shapes to pick from: the button's context menu, or hold it

#### Changes

- Breaking change: the 0.1.0 library is not carried over
- Removed pixel-based drawing
- Reworked and unified stroke rendering

#### Internals

- Lots of stability and correctness fixes
- Storage, interaction and UI overhauls

### 0.1.0

- First release.
