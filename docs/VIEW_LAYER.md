# The view layer

Status: **built** (2026-09-26), in the phases of section 10: the
reference for how the overlay is drawn, as `docs/INTERACTIONS.md` is for
input, `docs/OVERLAY_STATES.md` for the overlay's states and
`docs/SETTINGS.md` for settings. Every behavior below is either what the
app did before it (unmarked, or said so) or a change (marked **Change**)
that it made. All ten were made; what the building found is noted under
each phase. "Today" means the app as of `73e091c`, before the work. The
questions it was reviewed with, and their answers, are in section 11.

## The principle

The same as for input, the overlay's states and settings: the view is
one structured, well-defined thing, and that is a decision in its own
right, beyond how the code happens to be written:

- **One list of what is on screen.** Every surface the view draws, from
  the canvas layer to the delete confirmation, is one row of section 3,
  which says when it is up, what closing it does, who owns it and where
  it sits. The stack is that list's order, applied in one place.
- **One frame, in named stages.** Every frame runs the stages of
  section 5 in order, and each stage says what it may change. Nothing is
  changed by being drawn: what the frame's own state calls for is done
  before anything is drawn from it, and what a widget asks for is done
  after the draw.
- **One way for a widget to act.** A click or a drop records an action,
  and the actions are done in one stage, the way a command is done
  (section 6).
- **Each surface has one owner**, which holds its state and nothing
  else's (section 7).

A change that fits is a new row, a new action or a changed cell. A change
to the structure has to be well founded and argued the way this document
argues its own shape: the case that does not fit, why no row or cell can
express it, and what it does to the rest. The structure is:

- the stages, and what each may change;
- the columns of the surface list;
- how an action reaches the model;
- the rules for owners.

If the structure changes, this document changes with it, first.

**What this does not change:**

- the input machine and its levels (`docs/INTERACTIONS.md`);
- the overlay's states (`docs/OVERLAY_STATES.md`);
- settings and how they are edited (`docs/SETTINGS.md`);
- how anything looks;
- any ImGui id a test finds a widget by.

## 1. What this is for

**One class for everything.** `OverlayApp` is the whole view. It has 78
data members and is one class across twelve files and about 9,400 lines
(`overlay_app*.{h,cpp}`). Its members belong to at least ten different
things on screen (section 7), and only comments say which belongs to
which. `overlay_app_overview.cpp` (2,854 lines) holds:

- the Canvases tab and the About tab;
- the widgets both of them use;
- the delete confirmation and the toast;
- the whole Settings tab, about 1,500 lines of it.

`docs/ARCHITECTURE.md` says as much: "it is still one class".

**Order by position.** `OnFrame` is about forty steps in 210 lines.
Their order matters, and only where each call sits holds it. Several
carry a comment saying why they are where they are: "Last, so it renders
on top", "After both things that can open it", "Not at the very start of
the frame". Some of the steps that draw also change things:

- `RenderItems` first puts away a note edit that a command ended
  elsewhere.
- `RenderBrushSizePreview` saves the pen's width once its preview has
  faded.
- `RenderColorChooser` saves the pen's color when it sees that the
  chooser has closed.
- `RenderItemPropertiesPopover` ends the style edit when it sees that the
  popover has closed.
- The view-only path reports that a notice has finished.
- `ApplyEffects` runs between the canvas bar and the popovers, and may
  delete a canvas: a delete that Settings > Behavior says not to ask
  about.

**The stack by position too.** Twelve windows and layers and six popups
sit in a stack. Its order is also set by call position: 19 calls to
`BringToFront`, `KeepPopoverInFront`, `KeepChildPopupsInFront` and
`BringWindowToDisplayFront`, in eight files. In ImGui, whichever call runs
last wins, so each call's place in the frame is part of the answer. The
comments record what getting it wrong looked like:

- a dropdown in Settings "that appears to open but cannot be clicked";
- a color picker "that flashed up and vanished, with the clicks meant for
  it landing on the panel".

Each was fixed by moving a call. The order that results is written down
nowhere.

**A popup is kept in four places.** Since `docs/INTERACTIONS.md`, the
machine's Popup level says which popup is up, one at a time. ImGui's
open-popup stack is what is drawn. The view keeps a copy of its own for
each of the six kinds: what the popup is about, and whether it was drawn
on the last frame, each under a different name:

| Popup | What it is about | Drawn on the last frame | What closing does |
|---|---|---|---|
| Snippet menu | `itemContextMenuItemId_` | `itemContextMenu_.IsOpen()` | forgets the snippet |
| Canvas tile menu | `canvasContextMenuCanvasId_` | `canvasContextMenu_.IsOpen()` | forgets the canvas |
| Empty canvas menu | | `emptyCanvasMenu_.IsOpen()` | |
| Properties | `itemPropertiesPopoverItemId_`, `itemPropertiesPopoverAnchor_` | `itemPropertiesPopoverItemId_` | ends the style edit; forgets the snippet |
| Color chooser | `colorChooserAnchor_` | `colorChooserOpen_` | saves the pen's color |
| Delete confirmation | `confirmDeleteTarget_` | `confirmDeleteShown_` | forgets the target, on a button only |

The fourth place is the effect queue, where a popup that was asked for
waits for the next frame. The machine's Popup asks `PopupShowing` on
every tick, and it reads the queue and then six members. What closing
does runs only when the draw notices that the popup is gone. A popup
ended while nothing draws it does its closing late, or not at all
(section 9, finding 1).

**Four ways for a widget to act.** What a click asks for is done in one
of four ways:

1. **At once, in the middle of the draw:**
   - a folder's or a canvas's rename, inside the loop that draws the
     folders or the canvases;
   - New folder and New canvas in the Overview's footer;
   - a picked snippet sent to a canvas;
   - a context menu's command, right after the menu is drawn.
2. **At the end of its own draw function:**
   - the Overview's `OverviewActions`, after the sidebar and the grid;
   - the canvas bar's click, reorder and "+";
   - the dock's restore.
3. **At the next frame, through the effect queue:** every popup opened,
   and a delete that does not ask.
4. **As a command, through `Dispatch`:** the context menus' rows.

Each deferral gives the same reason in its own comment: the draw reads a
`const&` into a list the change would reallocate. Each is still its own
mechanism. And the same act takes different paths. A canvas tile clicked
in the Overview calls `Session::SwitchToCanvas`. One clicked on the
canvas bar calls `Editor::SwitchCanvas`, which first ends the canvas
scope, as a command does. The bar's "+" does exactly what the NewCanvas
command does, but without being that command, so it is not asked
`Available` and not counted by `CommandsRun`. Nothing goes wrong today,
because while a panel is up there is nothing in the hand to end. But why
it works is written down nowhere.

**Flags that carry something to a later frame.** About twenty members do
this. They are of five kinds, and only two kinds are a problem:

| Kind | Members | |
|---|---|---|
| What was last applied, compared with what is wanted | `appliedUiScalePercent_`, `appliedAccentRGBA_`, `appliedFramePacing_`, `appliedInputOptionsHudDigits_`, `appliedPointerShape_` and the cursor history | sound: the same reconcile the tray does for the window |
| Something only a later frame can do | `effects_`, `welcomePending_`, `edgePanelsFlashPending_`, `styleApplied_`, `pendingOverlayRestart_`, `noticeFinishedReported_` | sound; each belongs to a stage (section 5) |
| A request to one widget, used where it is drawn | `overviewBodyScrollToTop_`, `renameJustFocused_`, `overviewScrollToCanvasId_`, `overviewScrollToFolderId_`, `canvasBarScrollToCurrent_` | sound; each belongs to the owner of that widget (section 7) |
| A copy of whether a popup was drawn | `confirmDeleteShown_`, `colorChooserOpen_`, `ContextMenu::open_` | goes (C2) |
| A save waiting for a draw | `drawWidthDirty_`, and `colorChooserOpen_` again | goes (C3, C4) |

**Leftovers.** Three drag-and-drop payload names still carry the
Hoverboard prefix: `HB_FOLDER_REORDER`, `HB_CANVAS_REORDER` and
`HB_BAR_<row>`.

**Little of this is a fault.** The overlay states and settings surveys
each found faults a person could run into. This one found one (section
9, finding 1), by reading the code. The case here is that the view is
hard to change safely. Every change has to find its place in the frame,
its place in the stack and a way to act that fits, and all three exist
only as the positions of calls and the comments beside them.

## 2. Vocabulary

- **Surface**: something the view draws that has its own place in the
  stack. There are five kinds:
  - a *layer*: a full-screen window that takes no input
    (`BeginScreenLayer`), such as the canvas, the items, the HUD and the
    screen chrome;
  - a *window*: an ImGui window that takes input, such as the canvas bar,
    the dock and the note editor;
  - a *panel*: a window over a dimming backdrop that covers the canvas,
    such as the Overview and the cheat sheet (the machine's Panel level);
  - a *popup*: one of the six the app opens (the machine's Popup level),
    or one of ImGui's own opened inside a panel, such as a dropdown, a
    help popover or a color picker;
  - an *overlay*: drawn on ImGui's foreground list, above every window,
    such as the drag previews, the size preview, the modifier badge, the
    toast, the persistence warning and the software pointer.
- **Up**: drawn this frame. For a panel or a popup, the machine says
  whether it should be up, and the view draws it. ImGui closes a popup
  that a frame does not draw (`docs/OVERLAY_STATES.md`, section 10).
- **Stage**: one step of the frame (section 5).
- **Action**: what a widget asks for: a command, or a view action
  (section 6).
- **Request**: something asked of a later frame, of the kinds in the
  table above.
- **Owner**: the object that holds a surface's state (section 7).

## 3. What is on screen

Every surface, bottom to top. The order is the stack, and it is today's
order, including where that is surprising (section 9, finding 3).

**Edit mode:**

| # | Surface | Kind | Up while | Owner (section 7) |
|---|---|---|---|---|
| 1 | The canvas: the frozen screen, the debug readout | layer `##spickzettel_canvas` | always | Canvas view |
| 2 | The items: every snippet; the selection's border and bar; a box being dragged | layer `##sz_items_layer` | a canvas is current | Canvas view |
| 3 | The note editor | window `##noteedit<id>` | a note is being typed | Canvas view |
| 4 | The dock | window `##dock` | a snippet on the canvas is minimized | Canvas view |
| 5 | The canvas bar | window `##canvas_bar` | the pointer is at the bottom edge or on the bar, or its menu is up; not under a panel | Canvas bar |
| 6 | The snippet menu, the canvas tile menu, the empty canvas menu, Properties, the color chooser, the shape menu | popups | the Popup level holds it | Popups |
| 7 | The input options HUD | layer `##sz_input_hud_layer` | its setting is on | Screen chrome |
| 8 | The edit-mode border, the frame graph, the demo mark | layer `##sz_chrome_layer` | always, each as its setting says | Screen chrome |
| 9 | The Overview: its backdrop, its panel, and ImGui's popups inside it | panel | the Panel level holds the Overview | Overview; Settings page |
| 10 | The cheat sheet: its backdrop and its panel | panel | the Panel level holds the cheat sheet | Cheat sheet |
| 10a | The tutorial card | window `##tutorial_card` | a tutorial topic runs, or its list is up | Tutorial card |
| 11 | The delete confirmation | popup | the Popup level holds it | Popups |
| 12 | The drag previews (a region, the rectangle eraser), the size preview, the modifier badge | overlays | their gesture or preview is on; not under a panel | Pointer |
| 13 | The toast, the persistence warning | overlays | a message is up; a write has failed | Messages |
| 13a | The tutorial's spotlight | overlay | the step points at something on screen, and its goal is not met | Tutorial card |
| 14 | The software pointer | overlay | its setting draws one | Pointer |

Rows 10a and 13a came with the tutorial (`docs/TUTORIAL.md`, section
7.4), numbered so that no other row's number changed. The card sits
above the panels, so that a step can talk about them, and below the
delete confirmation, which must stay reachable. The spotlight sits above
everything but the pointer, so that it can ring something inside a
panel.

**View, Pinned and Notice:**

| # | Surface | Kind | Up while | Owner |
|---|---|---|---|---|
| 1 | The current canvas's snippets (in Pinned only the pinned ones; in Notice none), the debug readout, the frame graph, the demo mark | layer `##spickzettel_view_only` | always | Canvas view |
| 2 | The toast, the persistence warning | overlays | as above | Messages |

What closing does is in section 4 for the popups. For the rest:

- **The note editor:** the text is kept (the Text level's `TypingNote`).
- **The Overview:** a name being edited or a key being captured ends, and
  so does the picker.
- **Every other surface:** nothing.

**Change (C1)**: the stack is this table. One list holds the windows in
this order. One pass, after everything is drawn, brings each to the
front in turn, and finds each popup's window through ImGui's open-popup
stack. The 19 calls go. The order stays today's. The phase starts with a
test that records today's order (section 10, phase 1), and that test
does not change when the pass replaces the calls.

## 4. Popups

The machine's Popup level stays as it is. It says which popup is up, one
at a time, and opening one ends the one that was there. What changes is
the view's side.

**Change (C2)**: one record of the popup that is up. The machine allows
one at a time, so the view keeps one record, holding:

- its kind;
- what it is about: a snippet, a canvas, a delete target, or nothing;
- where it opens;
- whether it was drawn on the last frame.

The record is set when the popup is asked for, so `PopupShowing` asks it
alone. The six subjects and anchors go, and so do the five copies of
"drawn". `ContextMenu` keeps drawing its rows and handing back the one
chosen; whether a menu is up, and where it opens, is the record's to say.

**Change (C3)**: what closing a popup does is done once, however it
closes. Today the draw that notices ImGui has closed the popup does it.
Instead, each kind has one place for it, run from one of two points:

- by that draw, when the popup closed by itself: a row chosen, a click
  outside, Escape;
- at once, when the machine ends the popup from outside
  (`Popup::Interrupt`, through `ClosePopup`). Today that only queues
  ImGui's half of the close for the next frame.

So a popup ended while nothing draws it, as when edit mode is left or the
app exits, does its closing then.

A third point: by `ApplyEffects`, when a popup it opened is closed again
in the same pass - Escape in the same gap between two frames as the
click that asked for it. The draw after it finds the popup not open, and
a popup not yet drawn is taken there as not drawn *yet*, not closed, so
nothing closed it: the record stayed, the machine's Popup level with it,
and every key and click was claimed until a canvas switch. *Found in
review on 2026-09-27.*

| Popup | Closing does |
|---|---|
| Snippet menu, canvas tile menu | forget what it was about |
| Empty canvas menu, shape menu | nothing |
| Properties | end the style edit; forget the snippet |
| Color chooser | keep the pen's color (C4) |
| Delete confirmation | forget the target |

**Change (C4)**: the pen's width and color are kept when the overlay
settles. Today:

- **The width** is saved once the wheel's size preview has faded, from
  the draw of that preview.
- **The color** is saved when the color chooser closes, from the
  chooser's draw.

A frame that does not draw them never saves them:

- Turn the wheel with the pen, put the overlay away within the second
  the preview is up, and quit from the tray: the new width is gone at the
  next start.
- Put the overlay away with the color chooser up (a popup stays up for
  the next showing), and quit from the tray: the picked color is gone at
  the next start.
- Leave for view mode with the chooser up, and the color is saved only at
  the next frame of edit mode.

Settling (put away, left for a read-only mode, the app exiting) keeps
both, as it commits a settings preview (`docs/SETTINGS.md`, C7). The
preview's fade still saves the width, so a burst of notches is still one
write. This was found by reading the code; its phase starts with tests
that show the loss.

## 5. The frame

Every frame of edit mode runs these stages, in order:

| Stage | What it does | What it may change | Today |
|---|---|---|---|
| 1. Prepare | tells the editor the display size; the anchor board cleared; the interface scale, the style and the accent; a note's text size, decided once; the frame pacing; the textures' frame begun and the current canvas's asked for; snippets fitted to a changed display; the selection pruned; a note edit ended elsewhere put away (C5); where the canvas bar is; what the start decided for the tutorial, asked for once as an action; the tutorial's runner brought up to date, its own state only | the library and the editor, for what the frame's own state calls for; the one setting decided here; ImGui's style | the top of `OnFrame`, down to `UpdateEdgePanels` |
| 2. Canvas | surfaces 1 to 5 | a widget's own value (section 6); records actions | `RenderCanvasLayer`, `RenderItems`, `RenderCanvasBar` |
| 3. Open | the effect queue: popups opened and closed, ImGui's active widget let go | ImGui's popups and focus | `ApplyEffects` |
| 4. Popups | surface 6 | as stage 2 | the five popups' `Render...` functions |
| 5. Over the canvas | surfaces 7, 8 and 12 | nothing | the drag previews, `RenderBrushSizePreview`, `RenderToolModifierBadge`, `RenderScreenChrome` |
| 6. Panels | surfaces 9 to 11 | as stage 2 | `RenderOverview`, `RenderCheatSheet`, `RenderConfirmDeletePopover` |
| 7. Messages | surfaces 13 and 13a | nothing | `RenderActionToast`, `RenderPersistenceWarning` |
| 8. Stack | the pass of C1 | the windows' order | the 19 calls, spread over stages 2 to 6 |
| 9. Pointer | the pointer's shape; surface 14 | the window's cursor | `ApplyPointerShape`, `DrawSoftwareCursor` |
| 10. Apply | the actions recorded in stages 2 to 6, in order; the tutorial's progress set, where it changed; the pen's width, once its preview has faded; the HUD's restart, once its key is up; a notice's end | the library, the editor, settings, what is up | new (C5, C6); today spread over the draws |

The view-only modes run part of Prepare, the view-only layer, Messages
and Apply.

Before any stage, every frame tells `TrayController` it is starting
(`OverlayApp::SetFrameStartCallback`): the frame before it is on screen
by then, which makes it the moment for work that makes a frame late -
in view mode and the pinned view, the library's checkpoint (ARCHITECTURE.md,
"Commits wait for nobody: the WAL"). It changes nothing the frame draws.

Why this order:

- **Prepare comes first, and may change the library.** Nothing has been
  drawn from the library yet, so a change here cannot leave half a frame
  drawn from the old state. Everything in it is today's, in today's
  order.
- **Open sits between the canvas and the popups**, where the effect
  queue runs today. Opened at the very start of the frame, the canvas
  bar's menu was closed again before it was drawn. ImGui wants a popup
  opened after the frame's other windows and shortly before the popup is
  begun.
- **Stack comes after everything is drawn**, because a window's order is
  read only when the frame is rendered and when the next frame
  hit-tests. It is not read in between.
- **Pointer comes before Apply.** The pointer is drawn from what this
  frame drew. After Apply, a changed library would put a pointer meant
  for the next frame on this one.
- **Apply comes last**, inside the frame, because a change may say
  something (a toast needs ImGui's clock) and may ask for a popup (which
  opens in the next frame's Open stage). Input arrives between frames and
  reads the library as Apply left it, as it does today.

So a frame draws one library: the one Prepare left. Today a click on the
canvas bar switches the canvas halfway through a frame, and what is drawn
after it (the popovers, the Overview) is drawn from the other canvas.

**Change (C5)**: the frame is these stages, one function each, called
in order by `OnFrame`. The changes made in draws today move out:

- the note edit put away moves to Prepare;
- the pen's width moves to Apply;
- a notice's end moves to Apply;
- the HUD's restart moves to Apply (it already comes after the draw).

A delete that does not ask moves out of the Open stage and becomes an
action (C6).

## 6. Actions

**Change (C6)**: what a widget asks for is an action. It is recorded as
the widget is drawn and done in the Apply stage, in the order recorded.
The Overview's `OverviewActions` and `ApplyOverviewActions` become the
general case, and the canvas bar's and the dock's local copies and the
in-place changes of section 1 go through it too.

| Action | Asked for by | Done through |
|---|---|---|
| A command | a context menu's row; the canvas bar's "+" and Overview buttons (C7) | `Dispatch` |
| Switch to a canvas | a tile in the Overview or on the canvas bar | `Editor::SwitchCanvas`, so the Overview's tile ends the canvas scope as the bar's does |
| Switch to a folder; show a deleted folder | a folder row | the session; the Overview |
| Reorder a folder or a canvas; move a canvas to a folder | a drop | the session |
| Rename a folder or a canvas | a name field let go of | the session |
| New folder; new canvas | the Overview's footer | the editor |
| Send the picked snippet | a tile, or New canvas, while picking | the session |
| Restore | a Restore button | the session |
| Delete; delete for good | the confirmation's Delete, or a delete button where Settings says not to ask | the session, once the canvas scope has ended |
| Restore a minimized snippet | a dock chip | the session |
| Close the Overview or the cheat sheet | the backdrop; the picker's Cancel, a tile clicked | the machine |
| Finish a note edit | the note editor let go of | the editor |
| The tutorial card's buttons: Next, Back, Skip, Done, Keep, More topics, the list's Close | the tutorial card | the card's runner; Done and More topics also delete what the topic made unless Keep is ticked: its folders, asked first, and its profiles |
| Start a tutorial topic; open the list | a row of the list; the start (Prepare); the Settings button | `OverlayApp`: the Overview closed, the running topic let go of, the topic's folder made, then the card |
| Go back to the tutorial; put a practice snippet here | a hint's button on the card | the editor's switch, or the topic's folder made again; the session, off the history |

An action is a value: an id, an index, a name. It never holds a
reference into the model, which is the failure that every deferral today
exists to prevent. Closing the Overview from its own header then needs no
early return: today that return stops the rest of the panel from drawing
a closed Overview. Instead, the whole panel draws this frame, and is gone
the next.

**What stays in the draw:** a widget's own value, changed in place as it
is changed. That is:

- **A setting.** Every bound widget edits as it changes
  (`docs/SETTINGS.md`, section 9). A row drawn from a list edits a copy
  and sets it after its loop, as the bar rows and the profiles list do
  today.
- **A snippet's style while a slider or a swatch is held, and a note's
  text as it is typed.** These are the session's previews: they change
  one snippet's values and file nothing.
- **The pen's color while the chooser's square is dragged.**

None of these adds, removes, reorders or switches anything that a draw is
walking.

**Change (C7)**: the canvas bar's "+" and Overview buttons dispatch the
NewCanvas and Overview commands. Today they repeat those commands' work.

**Considered and not proposed:**

- **Every action a command** (question 2). The command table is what
  keys, menu rows, the selection bar and gestures reach. It carries the
  bindings, the cheat sheet's rows and availability. An Overview action
  has none of these, and each would add an argument to `Command`: a
  folder, an index, a name.
- **Settings edits as actions.** `docs/SETTINGS.md` settled that an edit
  is made in the frame, and that the window's side of it is reconciled
  after the frame. A setting is a value, and changing it moves nothing a
  draw walks.
- **Actions as closures** (question 1).

## 7. Who owns what

**Change (C8)**: `OverlayApp` is split by surface. Each owner is a class
of its own. It holds the state of its surfaces, privately, draws from
what it is handed, and records actions:

| Owner | Surfaces (section 3) | Its state, from today's members | From |
|---|---|---|---|
| Canvas view | 1 to 4; the view-only layer | both mesh caches; the preview budgets; the debug handle readout | `overlay_app_items.cpp`, `overlay_app_rasters.cpp`, part of `overlay_app.cpp` |
| Canvas bar | 5 | its reveal, place, scroll, last canvas and scroll request; the flash request; the top of the bottom edge | `overlay_app_docks.cpp` |
| Popups | 6, 11 | the record (C2); the effect queue | `overlay_app_popovers.cpp`; `ContextMenu` stays a widget |
| Overview | 9, but for the Settings tab | the tab; the picker; Show deleted and the deleted folder shown; the renames; the scroll requests; About's page | `overlay_app_overview.cpp` (Canvases, About), `overlay_app_deleted.cpp` |
| Settings page | the Settings tab | the section; the edit target; a refused profile name; the display list | `overlay_app_overview.cpp` (Settings), `settings_widgets.*` |
| Cheat sheet | 10 | none | `overlay_app_cheatsheet.cpp` |
| Screen chrome | 7, 8 | the HUD's applied digits, restart request and last-key record; the demo mark's place | `overlay_app.cpp` |
| Messages | 13 | the message and when it expires; the message for the next showing; the settings file that failed; whether a notice's end was reported | `overlay_app.cpp`, `overlay_app_overview.cpp` |
| Pointer | 12, 14; the pointer's shape | the applied shape and the cursor history; when the size preview expires; the pen's width owed | `overlay_app.cpp`, `overlay_app_popovers.cpp` |
| Tutorial card | 10a, 13a | the runner and the topic it runs; whether the list is up; whether a topic has run; whether the card was dragged | new with the tutorial (`docs/TUTORIAL.md`) |
| `OverlayApp` | none of its own | the mode; the stages; the actions; the scale, accent and pacing applied; the style; what the start decided for the tutorial; the tutorial's world and the anchor board; the tray's callbacks | `overlay_app.cpp`, `overlay_app_input.cpp`, `overlay_app_commands.cpp` |

The rules:

- **An owner draws only its own surfaces and changes only its own
  state.** Anything else it wants is an action, or a request made through
  `OverlayApp`.
- **No owner calls another.** `OverlayApp` is the only object that knows
  them all. It routes the `EditorViews` calls the editor makes, such as
  opening a popup or the Overview, to the owner concerned.
- **An owner reads the session, the settings and the editor.** It changes
  them only through actions, or through its widgets' own values
  (section 6).
- **The tests keep what they use today.** `OverlayApp`'s public
  accessors stay and forward to the owners, and every ImGui id stays.

**Change (C9)**: what is shared goes into files of its own, out of
`overlay_app.cpp` and the anonymous namespaces of
`overlay_app_overview.cpp`:

- **`ui/theme.{h,cpp}`:** the palette, the accent and the style.
- **`ui/widgets.{h,cpp}`:**
  - the buttons (`IconButton`, `PillIconButton`, `DangerIconButton`,
    `PrimaryButton`, `TabButton` and the rest);
  - `Labeled` and `PressLandsThisFrame`;
  - the screen layers;
  - the key names (`FormatKeyComboLabel` and the ImGui key mappings).
- **`ui/item_painting.{h,cpp}`:** `DrawStroke`, `DrawPicture`,
  `DrawItemContent`, `DrawCanvasPreview` and the rest.

`overlay_app_internal.h` goes.

**Change (C10)**: the drag payloads are named for this app:
`SZ_FOLDER`, `SZ_CANVAS` and `SZ_BAR_<row>`. They are never stored, so
there is nothing to migrate.

## 8. What changes

| | Change | What it fixes |
|---|---|---|
| C1 | The stack is section 3's table, applied in one pass after the draw | an order set by which call runs last, written down nowhere; two past faults fixed by moving a call |
| C2 | One record of the popup that is up | six kinds each kept in four places; `PopupShowing` reading six members |
| C3 | What closing a popup does is done once, however it closes | closing done only when a draw notices, so late or never for a popup not drawn |
| C4 | The pen's width and color are kept when the overlay settles | both lost when the overlay is put away before they are saved and the app exits (found by reading) |
| C5 | The frame is ten named stages, one function each | an order held by call position; changes made inside draws |
| C6 | A widget's action is recorded in the draw and done after it | four ways to act, each deferral its own mechanism; one act taking two paths |
| C7 | The canvas bar's two buttons dispatch their commands | work repeated outside the command table |
| C8 | `OverlayApp` is split into owners | one class of 78 members across twelve files |
| C9 | Widgets, theme and item painting in files of their own | helpers spread over `overlay_app.cpp`, the Overview's file and a shared internal header |
| C10 | The drag payloads are named for this app | Hoverboard's prefix |

Everything else stays as it is today.

## 9. Found while writing this

1. **The pen's width and color can be lost** (C4). Found by reading
   `RenderBrushSizePreview`, `RenderColorChooser` and
   `OverlayApp::SettleForPersistence`, which commits a settings preview
   and neither of these.
2. **The Overview switches canvases around the editor** (C6). No fault
   follows today, because a panel leaves nothing in the hand to end. But
   the canvas bar goes through the editor for the same act.
3. **The border and the demo mark sit above every popup over the
   canvas**, and below the Overview. `docs/ARCHITECTURE.md` says the
   demo mark is "above every snippet and below only the Overview". That
   is true of the Overview, but says nothing of the popups. The border's
   reason for being high up, that a fullscreen snippet would hide it,
   says nothing of popups either. Recorded here as it is (question 4).
4. **`docs/ARCHITECTURE.md` has drifted:**
   - "The overlay UI" lists the files as "items, input, rasters,
     popovers, docks, overview, deleted", leaving out the cheat sheet and
     the commands.
   - It says the gestures "stay in the view" until the Gesture level
     holds them. It has held them since `docs/INTERACTIONS.md`, phase 3.
   - "The Overview" says the panel is "drawn last so ordinary insertion
     order puts them above everything". In fact it is brought to the
     front on every frame, and the cheat sheet, the delete confirmation
     and the messages come after it.
   - "Cursors" says the shape "is re-asserted every frame". It is
     re-asserted when the pointer moves, or when ImGui's wanted cursor
     has changed in the last two frames. Pushing it every frame was
     measured at a quarter of an idle frame.

   Corrected in the last phase.
5. **`ShowActionToast`'s comment is out of date.** It says every other
   call site runs from inside `RenderOverview`. In fact any command
   reaches it through `Say`, between frames too. Corrected when it moves
   (C8).
6. **A spin of Ctrl or Shift and the wheel was several undo steps**
   (found building C3). Properties' draw ended the style edit on every
   frame it was not up. That is the edit a spin of the wheel's opacity
   holds open, so it was ended after every notch: one undo took back one
   notch, and Escape found nothing to call off. Fixed by C3, since the
   closing runs once, when Properties closes.

## 10. Where the code goes, and getting there

As built:

- **`ui/overlay_app.{h,cpp}`:** the frame and its stages, the mode, the
  actions, and the routing to the owners; `overlay_app_tutorial.cpp`,
  its part in the tutorial (`docs/TUTORIAL.md`, section 7.3).
- **`ui/view_action.h`:** the action type (section 6), and `DeleteTarget`.
- **One pair of files per owner of section 7**, in `ui/view/`:
  `canvas_view`, `canvas_bar`, `popups`, `overview_panel`,
  `settings_page`, `cheat_sheet`, `screen_chrome`, `messages`,
  `pointer`, `tutorial_card`; `view_host.h`, what an owner may ask of
  `OverlayApp`; `anchors.h`, the board an owner marks anchored widgets
  on; and `tutorial_world`, the app's answers to the tutorial
  (`docs/TUTORIAL.md`, section 7). The tutorial's runner, which draws
  nothing and has no ImGui, is in `ui/tutorial/`.
- **`ui/theme`, `ui/widgets` and `ui/item_painting`** (C9). A Settings
  row's widgets stay in `ui/settings_widgets`, where `docs/SETTINGS.md`
  put them.

**Phases.** Each phase is its own set of commits, and the tests stay
green throughout.

1. **Tests of today.** No behavior change.
   - **The stack:** a headless test that draws frames with each surface
     of section 3 up, and checks ImGui's window order against the table.
   - **The actions:** each action of section 6, asked for through the UI,
     does what it does today.
   - **Closing:** each popup's closing does what the table of section 4
     says.

   *Built in* `b108f9c`: `tests/support/view_stack.h` names every window
   a frame drew by its surface, and `tests/app/view_layer_test.cpp` and
   `tests/ui/view_layer_ui_test.cpp` hold the rest. *Found while building
   it:* the order of section 3 was right as written.
2. **Popups (C2, C3, C4).** One record, closing done once, and the pen
   kept. It starts with tests that show the pen's width and color lost,
   seen failing before the change.

   *Built in* `3cd1f68`. All four pen tests failed first, as section 9
   said they would. *Found while building it:* finding 6. And the record
   is set as a popup is asked for, so `PopupShowing` asks it alone,
   without the effect queue.
3. **Stages and actions (C5, C6, C7).** The stage functions and the
   actions. `OverviewActions`, the local copies and the Overview's early
   return go. New tests check that an action's change shows in the frame
   after the one it was asked in, using the frame hook of
   `docs/SETTINGS.md`, phase 5.

   *Built in* `8f2c7f8`. The test that a frame draws one library was seen
   failing with the canvas bar's switch put back in the draw. *Found
   while building it:* the cheat sheet's backdrop closed its panel in the
   draw too, so the action closes either panel. A delete where Settings
   says not to ask is an action at once rather than a popup that deletes
   as it opens.
4. **The stack (C1).** One table and one pass. Phase 1's stack test does
   not change.

   *Built in* `418712c`. The pass finds the app's popups by their ids,
   hashed at the top level of the frame as they are opened, and ImGui's
   own by the window they were opened in. Tooltips need nothing: ImGui
   draws them in a layer of their own, above every window.
5. **Owners (C8, C9).** Code moves, one owner per commit: the shared
   files first, then the Settings page, the Overview, the popups, the
   canvas bar, the screen chrome, the messages and the pointer, and the
   canvas view last. No behavior changes, and every id stays.

   *Built in* `2f4d2d0` to `23fa1be`, the cheat sheet after the Overview.
   What an owner asks of `OverlayApp` is an interface, `ViewHost`: to act,
   to say something, the window, the displays, a hotkey offered to the
   OS, a delete asked, the overlay restarted, a tile menu opened, and what
   a preview is drawn with. The tutorial added one more: an anchor marked
   where a widget is drawn, on a board cleared each frame, which the
   tutorial's spotlight reads (`docs/TUTORIAL.md`, section 7.2). The Overview draws the Settings page's body
   through a call `OverlayApp` hands it, and the view-only layer the demo
   mark the same way. `overlay_app_internal.h` went last, its tool tables
   to the Settings page.
6. **Leftovers and docs (C10).** The payload names, and finding 4.
   `docs/ARCHITECTURE.md`'s "The overlay UI" is rewritten around this
   document, and this document is marked built.

   *Built in* the commit that marks this document built, and in
   `23fa1be` for "The overlay UI". *Found while building it:* the demo
   mark's paragraph in `docs/ARCHITECTURE.md` said "below only the
   Overview" (finding 3); it says where the mark sits now.

**Risks, and what guards them:**

- **ImGui closes a popup that a frame does not draw.** A phase that moves
  a draw must not skip one. The tests of phase 1 open each popup and
  draw frames with it up.
- **The stack pass reaches into `imgui_internal.h`**, as the calls it
  replaces already do. ImGui is pinned to one commit
  (`cmake/FetchImGui.cmake`).
- **Two actions in one frame** are done in the order recorded. One click
  makes at most one, but a drop and a field let go of can land in the
  same frame. That is the order today's in-place changes run in.

## 11. Questions for review, and the answers

1. **Actions as values, or as closures?** Recommended: values, a
   `std::variant` of small structs. A closure is shorter to write, but it
   can capture a reference into the list being drawn, which is exactly
   what the deferral is there to prevent. A value can also be read by a
   test. *Answer:* as recommended.
2. **The Overview's actions as commands?** Recommended: no (section 6,
   "Considered and not proposed"). They go through the editor instead,
   which ends the canvas scope for a switch as a command does. Where a
   command already does the same work, the command is used (C7).
   *Answer:* as recommended.
3. **How far the split goes.** Recommended: the ten owners of section 7.
   The alternative is three owners (the canvas, the panels, the chrome),
   which would leave the Overview and the Settings page together at about
   2,300 lines. *Answer:* as recommended.
4. **The border and the demo mark above the popups** (finding 3).
   Recommended: keep today's order. The stack's table records it, and
   changing it would be a decision of its own. *Answer:* as recommended.
