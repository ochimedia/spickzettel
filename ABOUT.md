# Spickzettel

A translucent, always-on-top drawing surface for annotating whatever is on
screen - built for keeping notes over a game without alt-tabbing away from
it.

This text is compiled into the binary and shown in the overlay's own About
tab, so it travels with whatever build you were handed. Edit `ABOUT.md` at
the repo root and rebuild to change it.


## Getting around

- Ctrl+Alt+O - show or hide the overlay in edit mode, where you capture,
  draw, place and arrange.
- Ctrl+Alt+V - show or hide it in view-only mode, where your clicks pass
  straight through to whatever is underneath.
- Ctrl+Alt+C - capture the screen onto a new canvas and open the overlay
  on it.
- Ctrl+Alt+S - the same capture, without opening anything.

Making a snippet is a press on empty canvas: left-drag to capture a
screenshot of a region, right-drag to frame a drawing, double-click (or
hold) for a fullscreen one. A plain click makes nothing.

- Click a snippet to select it, drag it to move it, drag a handle to
  resize it. Shift adds to the selection; Shift-drag on open canvas draws
  a box to select by.
- Double-click a snippet, or hold a press on it, to draw on it. Its bar
  then shows Pen, Eraser, Text and the colour. Right-drag on it erases.
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


## Notes for testers

- Everything autosaves on its own. There is no save button, and closing
  the overlay is not "discarding" anything.
- Deleting a folder or canvas hides it where it is. "Show deleted", in
  the Overview, shows it there again in red, to restore or delete for
  good. A deleted snippet comes back with undo, until the app restarts.
- Screenshots are captured with the overlay hidden, so nothing the
  overlay draws - including the demo watermark - ends up in them.


## Changelog

### 0.1.0

- Snippets are objects: select, move, resize by handles, multi-select,
  clipboard, pin, minimize, fullscreen.
- Drawing mode on a snippet, with pen, line and rectangle shapes, two
  erasers, text, and a colour chooser.
- Per-canvas undo history, and per-canvas GPU textures so only the canvas
  you are looking at holds video memory.
- Folders and canvases, an Overview to move and reorder them, and
  Show deleted to get them back.
- Per-application profiles for the input options, and a Settings tab for
  everything else.
