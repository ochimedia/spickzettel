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

- Click a snippet to select it, drag it to move it, drag a handle to
  resize it. Shift adds to the selection; Shift-drag on open canvas draws
  a box to select by.
- Double-click a snippet, or hold a press on it, to draw on it. Its bar
  then shows Pen, Eraser, Text and the color. Click Pen or Eraser again
  for a line or a rectangle; right-click or hold it to pick one.
  Right-drag on the snippet erases.
  Click anywhere else, or press Escape, to stop.
- Delete removes the selection; Ctrl+Z brings it back. Ctrl+C, Ctrl+X and
  Ctrl+V copy, cut and paste snippets, between canvases too.
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
  good. After 14 days it is deleted permanently the next time the app
  starts; Settings > Behavior changes the period or turns it off. A
  deleted snippet comes back with undo, until the app restarts.
- Screenshots are captured with the overlay hidden, so nothing the
  overlay draws - including the demo watermark - ends up in them.
- Settings > Appearance > Picture scaling decides how a screenshot looks
  at another size. Bicubic or Lanczos keep text readable in a snippet
  shrunk to a third; Nearest keeps pixel art blocky.


## Changelog

### 0.2.1

#### Features

- Frame graph for diagnosing stutters (see Settings > Debug > Show frame graph)

#### Changes

- Moved the library to local AppData and optimized its performance. An existing library in roaming AppData is moved automatically.
- Opening the app while an instance is already running brings up the running instance.

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
