# Spickzettel

A translucent, always-on-top drawing surface for annotating whatever is on
screen - built for keeping notes over a game without alt-tabbing away from
it.

This text is compiled into the binary and shown in the overlay's own About
tab, so it travels with whatever build you were handed. Edit `ABOUT.md` at
the repo root and rebuild to change it.


## Getting around

- Ctrl+Alt+S - show or hide the overlay in edit mode, where you capture,
  draw, place and arrange.
- Ctrl+Alt+V - show or hide it in view-only mode, where your clicks pass
  straight through to whatever is underneath.
- Ctrl+Alt+C - capture the screen onto a new canvas and open the overlay
  on it.
- Ctrl+Alt+X - the same capture, without opening anything.
- Ctrl+H, with the overlay up - the cheat sheet: every key and gesture,
  as they are bound right now.

Making a snippet is a press on empty canvas: drag to capture a
screenshot of a region, Ctrl-drag to frame a drawing, double-click (or
hold) for a fullscreen one. A plain click makes nothing. Which press
makes which is up to you, in Settings. Right-click empty canvas for a
menu with every way to make one, Paste, the cheat sheet, Settings and
the Overview.

- Click a snippet to select it, drag it to move it, drag just outside
  its edges or corners to resize it. Shift adds to the selection;
  Shift-drag on open canvas draws a box to select by.
- Double-click a snippet, or hold a press on it, to draw on it - or
  pick Pen, Eraser or Text on its bar, which draws on every selected
  snippet. Right-click or hold Pen or Eraser to pick a line or a
  rectangle; it stays picked until you pick another. Right-drag on the
  snippet erases. Click the highlighted tool again, click anywhere else,
  or press Escape, to stop.
- Delete removes the selection; Ctrl+Z brings it back. Ctrl+C, Ctrl+X and
  Ctrl+V copy, cut and paste snippets, between canvases too: Ctrl+V puts
  them at the pointer, Ctrl+Shift+V where they were.
- Ctrl+Z - undo. Ctrl+Y or Ctrl+Shift+Z - redo.
- Mouse wheel - the selected snippets' size; while drawing, the pen or
  eraser size. Ctrl+wheel - background opacity, Shift+wheel - foreground
  opacity. Alt+wheel - step between the canvases of the folder you are in.
- The bar along the bottom edge shows the canvases of the folder; it
  slides out when the pointer reaches the edge. Its buttons make a new
  canvas and open the Overview, where folders, canvases and Settings
  live.


## Before using it over a game

- Every game takes an overlay differently. Some break when it takes
  focus; others need it to. Look through Settings > Behavior, and make a
  profile (Settings > Profiles) for each program that needs its own.
- Some games watch for tools that draw over them or read their input,
  and may treat this one as a cheat. If a game might object, quit
  Spickzettel before you start it.


## Notes for testers

- Everything autosaves on its own. There is no save button, and closing
  the overlay is not "discarding" anything.
- Deleting a folder or canvas hides it where it is. "Show deleted", in
  the Overview, shows it there again in red, to restore or delete for
  good - a folder that only holds deleted canvases in yellow - and
  "Empty trash" deletes all of it for good at once. After 14 days it is deleted permanently the next time the app
  starts; Settings > Behavior changes the period or turns it off. A
  deleted snippet comes back with undo, until the app restarts.
- Screenshots are captured with the overlay hidden, so nothing the
  overlay draws - including the demo watermark - ends up in them.
- Settings > Appearance > Picture scaling decides how a screenshot looks
  at another size. Bicubic or Lanczos keep text readable in a snippet
  shrunk to a third; Nearest keeps pixel art blocky.


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

### 0.2.3

#### Features

- A button for emptying the trash, with Show deleted on in the Overview
- Paste puts snippets at the pointer, or where the menu was opened; Paste in place (Ctrl+Shift+V) puts them where they were. Both are in a snippet's right-click menu too

#### Changes

- With Show deleted on, deleted folders are told apart from folders with deleted canvases by color - red and yellow - with a legend for the two
- The tutorial's rectangle eraser step accepts a Ctrl-drag with the round eraser, as its text says
- The Folders and canvases tutorial keeps the Overview open on the way back to its folder, and opens a canvas at the end instead
- The Capturing tutorial covers both capture hotkeys in one step
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
- Pen and eraser shapes to pick from: right-click or hold the button

#### Changes

- Breaking change: the 0.1.0 library is not carried over
- Removed pixel-based drawing
- Reworked and unified stroke rendering

#### Internals

- Lots of stability and correctness fixes
- Storage, interaction and UI overhauls

### 0.1.0

- First release.
