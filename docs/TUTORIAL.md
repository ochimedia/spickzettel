# The tutorial

Status: **agreed** (2026-09-28), for 0.2.0, and to be built in the
phases of section 10. It was proposed on 2026-09-27. Its questions and
their answers are in section 12, and the design below was changed to
match them.

It fits into the designs that are built: `docs/INTERACTIONS.md` for
input, `docs/OVERLAY_STATES.md` for the overlay's states,
`docs/VIEW_LAYER.md` for what is drawn and `docs/SETTINGS.md` for
settings. It changes the structure of none of them. What it adds to each
is what each already allows: a new row, a new action, a new setting and
a new owner.

## The principle

**The tutorial reads the app; it is not part of it.** It watches what
the app shows, points at it, and says what to do next:

- It decides nothing about what any input does.
- It changes the library only in the few named ways of section 7.3: the
  folder it runs in, a practice snippet it places when asked, and
  switching back to its folder.
- The app knows about the tutorial in three places only: an anchor
  marked where a widget is drawn, one owner in the view, and one
  setting.

**It guides; it does not guard.** Nothing the user does is held back
while the tutorial runs. Instead, every step is written so that nothing
can derail it:

- It is done when a result is there, however that came about.
- It names what it needs, and says how to get that back when it is gone.
- It has a line ready for the mistakes one can see coming.

Section 6 argues this, against the alternatives.

**Changes that fit, and changes that don't.** A step is a row in a
table. A change that fits is a new row, a new need, a new anchor or a
new hint. A change to the structure has to be argued the way the other
design documents argue theirs. The structure is:

- the runner's states;
- what the tutorial may read (the world, section 7.1);
- what it may change (section 7.3);
- that it holds nothing back.

## 1. What this is for

The first external testers found the app hard to discover.

**What a first run offers today.** It opens edit mode with three notes
(`OverlayApp::PlaceWelcomeNotes`):

- **The welcome.** How to make a screenshot and open a menu, the
  edit-mode hotkey, the cheat sheet's key, and how to delete.
- **Two warnings.** One on setting the app up for each program, one on
  anti-cheat.

The cheat sheet, on its key or from the empty canvas's menu, lists every
key and gesture.

**Why that is not enough.** Both are references. They say what exists,
to someone who already knows what they want. Neither leads. A first-time
user does not know:

- that a click selects a snippet, and a drag moves it;
- that the handles show only on a selected snippet;
- that drawing on a snippet is a mode, entered by a double-click or a
  hold;
- that Delete does nothing in that mode;
- that the edit hotkey is how you get back to your program.

These are sequences, and a sequence is learned by doing it once.

**What is wanted:**

- **A chain of steps.** Shown one at a time on a card with Back, Next
  and Skip. Each has text and, where it helps, a highlight on the part of
  the screen it talks about.
- **When it runs.** It starts on the first start, and can be started
  again from Settings.
- **A first chain:** make a screenshot, move and resize it, draw on it,
  delete it. Extended as we go (section 8).
- **Light coupling.** The tutorial knows the app through one narrow
  seam that can only read, and the app knows the tutorial hardly at all.
- **A decision on derailing:** how the user is kept from derailing the
  step they are on (section 6).

## 2. Vocabulary

- **Chain**: an ordered list of steps. The first is the *welcome chain*.
  More may follow (section 8).
- **Step**: what one card shows. There are two kinds:
  - a *read step* is done when Next is pressed;
  - a *do step* is done when its goal is met.
- **Gated step**: a do step whose result a later step needs. Next waits
  for its goal (section 5).
- **Tutorial folder**: the folder of the user's library that the
  tutorial makes and runs in. It is a lightweight sandbox (section 6.5).
- **Card**: the small window that shows the current step, with Back,
  Next and Skip.
- **Start record**: what a step notes as it begins, for its goal to
  compare with. Examples: the subject's place and size, how many strokes
  it has, which snippets exist.
- **Goal**: what says a do step is done. It checks the app's state
  against the start record.
- **Need**: something a step must have before its goal can be met, such
  as a subject, or the canvas not covered (section 6.4).
- **Subject**: the snippet a step is about (section 6.5).
- **Hint**: a line the card adds under the text. It is either a need not
  met, or a near miss.
- **Anchor**: a named place on screen that a step may point at, such as
  a button on the selection bar, the subject, or one of its handles.
- **Spotlight**: the ring drawn around an anchor.
- **World**: what the tutorial can read of the app (section 7.1).

## 3. What the user sees

**The card.** A small window in the app's own theme, about 360 px wide
at 100%. It holds:

- "Step 3 of 11", with a thin progress bar;
- the title and the text.
  - Keys are named as they are bound now: "Press Ctrl+Z".
  - A step whose key is unbound says what to do instead, the way the
    welcome note has a second text for a cheat sheet with no key.
  - Every text is in `assets/ui_strings.json`, under `tutorial.*`.
- the hint line, when one applies, and its button if it has one ("Put
  one here");
- a check mark, once a do step's goal is met;
- the buttons:
  - Back, except on the first step;
  - Next, which says Done on the last step;
  - Skip tutorial, a quieter button.

**Where it sits.** At the top center by default, clear of the anchor
and the subject. If either lies under it, the card moves to the bottom
center. The user can drag the card by its title, and it stays where it
is left for the rest of the run.

**Keys.** The card takes none:

- Escape stays the canvas's: it puts the hand down one stage
  (`docs/INTERACTIONS.md`, section 5).
- Enter is bound to nothing.

A card that took keys would be a level in the input machine. No step
needs that (section 6.8).

**Pressing it.** A press on the card is rule 1 of the recognizer ("over
an ImGui window"), like a press on the canvas bar. The machine needs
nothing new. Two consequences:

- While a popup is up, the first press on the card closes the popup, as
  any press outside a popup does. The second press reaches the button.
- A screenshot never has the card in it. The capture comes from the
  frozen screen, or from `CaptureRegion`, which leaves the overlay out.

**When it shows.** In edit mode only:

- When the overlay is put away or turned view-only, the card goes with
  it.
- When edit mode comes back, the card comes back where it was.
- View, Pinned and Notice are click-through, and a card whose buttons
  cannot be pressed would teach the wrong thing.

**Over the panels.** The card sits above the Overview and the cheat
sheet, so a later step can talk about them. It sits below the delete
confirmation (section 7.4).

**The spotlight.** A ring in the accent color around the anchor:

- It pulses slowly, is drawn above everything but the pointer, and takes
  no input.
- Nothing else on screen is dimmed: a do step needs the screen as it is,
  and the screenshot step is about what is on it.
- An anchor not on screen in this frame draws no ring. An example is
  the canvas bar, until the pointer reaches the bottom edge. For such
  an anchor, the step's text says where to look.

**When a step is done.** A do step whose goal is met shows its check,
and the card moves on by itself about a second later (question 2).

**Next** depends on the step (question 3):

- On a read step, Next moves on.
- On a do step that no later step depends on, Next moves on too, done or
  not.
- On a gated step, Next is grayed out until the goal is met. Its tooltip
  says why: "The next steps use what this one makes."

Skip tutorial is always there, so the tutorial still never traps the
user.

**Skip.** It goes to a last card, "Tutorial skipped":

- The card shows the two warnings (section 4, steps 9 and 10) in short
  form, unless the user has already passed them. Someone who skips at
  once still sees them.
- It says where to take the tutorial again (Settings > Interaction).
- It offers Back, to the step skipped from, so a misclick costs nothing.
  That is why no confirmation box is needed.
- Its Done ends the tutorial and puts the tutorial folder in the trash.
  Its "Done, keep the folder" keeps the folder (question 9). The end
  card has the same two buttons.

## 4. The welcome chain

It runs in the tutorial folder (section 6.5), and "the tutorial's
snippets" means the snippets in it. The keys in braces are named as they
are bound, and written for the triggers as set: on a first run the
screenshot trigger is plain, a drag with no key held. In the Next
column, "waits" marks a gated step (section 3).

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when | Near misses (6.6) |
|---|---|---|---|---|---|---|---|---|
| 0 | `welcome` | read | moves on | Spickzettel keeps snippets (screenshots, drawings and notes) over whatever program is underneath, and everything saves itself. This tutorial runs in a folder of its own. | - | - | Next | - |
| 1 | `screenshot` | do | **waits** | Drag a box over anything on screen to take a screenshot of it. With a trigger key set: "Hold {trigger} and drag". With the trigger off: "Press {newScreenshot}, then drag", or with that key unbound too, the empty canvas's menu. | - | in the tutorial folder; canvas uncovered; no drawing mode; no tool in hand but the screenshot tool | a screenshot snippet made since the step began, on the current canvas, not fullscreen | a fullscreen one: "That took the whole screen; a double-click does that. Drag a box instead." A drawing: "That made a drawing; {drawingTrigger}+drag does that." |
| 2 | `move` | do | moves on | Drag it to move it. A press selects it, and a drag takes it along. | the subject | a subject that can move; no drawing mode | the subject moved 16 px or more from where the step found it, its size changed by less than 10% | resized instead: "That changed its size. Drag from the middle to move it." |
| 3 | `resize` | do | moves on | Drag a corner to resize it; Shift switches keeping its shape. A right-drag near an edge does it too. | the subject's lower right handle | a subject that can move; no drawing mode; the subject selected | its width or height changed by 10% or more | - |
| 4 | `drawingMode` | do | **waits** | Double-click it, or hold the button down on it, to draw on it. | the subject | a subject | drawing mode on the subject | - |
| 5 | `draw` | do | moves on | Drag across it to draw. The bar above it has the pen, the eraser, text and the color. | the drawing bar's pen | drawing mode on the subject | the subject has more strokes than when the step began | - |
| 6 | `stopDrawing` | do | moves on | Click outside it, or press Esc, to stop drawing. | - | - | no drawing mode | - |
| 7 | `delete` | do | **waits** | Select it and press Delete, or the close button on its bar. | the subject's close button | a subject; no drawing mode | the subject deleted | - |
| 8 | `undo` | do | moves on | Deleted by mistake? {undo} brings it back. It takes back anything you did, a step at a time. | - | a deleted subject | the subject back on the canvas, after being deleted in this step (or when it began) | - |
| 9 | `programs` | read | moves on | Set it up for your programs. Some games break when the overlay takes focus; others need it to. Look through Settings > Behavior, and make a profile for each program that needs its own. | - | - | Next | - |
| 10 | `antiCheat` | read | moves on | Careful with anti-cheat. Some games watch for tools that draw over them or read their input. If a game might object, quit Spickzettel before you start it. | - | - | Next | - |
| 11 | `away` | do | moves on | Press {editMode} to put the overlay away and go back to your program. Press it again to come back here. | - | - | the overlay has come back since the step began | with the hotkey unbound, the text names the tray icon instead |
| 12 | `end` | read | Done | That's the basics. {cheatSheet} shows every key and gesture, and a right-click on anything shows what it can do. You can take this tutorial again from Settings > Interaction. | - | - | Done | - |

*Found while building phase 1:* the welcome text first said that the
screen behind is frozen while you edit. That holds only with "Freeze
screen while editing" on, which is off by default, so the card no
longer says it. The move and resize steps also need drawing mode off,
since a drag on a snippet in drawing mode draws.

**Which steps wait.** A step waits when a later step needs what its goal
makes, and nothing else can make it:

- the screenshot is the subject of every step after it;
- drawing mode is what step 5 draws in;
- the deleted subject is what step 8 brings back.

The other do steps move on, because no later step depends on them. A
need they leave unmet has its own way back: step 6's "no drawing mode"
is a need of step 7, with its own line. A test checks this for the
whole chain. Every need of every step is either the goal of an earlier
gated step, or has a line that says how to meet it (section 9).

**Steps beyond the first list.** The first list asked for screenshot,
move and resize, draw, and delete. Four more steps came out of the
review:

- **Undo** is the other half of delete. A first delete is less
  frightening when the next card takes it back.
- **The two warnings** were first-run notes. They are read steps now
  (question 4). They come before the step that sends the user back to
  their program, because that is when they matter: the anti-cheat
  warning is about starting a game.
- **Away** is the one thing a first-time user must know to get back to
  their program (question 8).

## 5. How a step runs

**The runner's states:**

- **Off**: no tutorial.
- **On a step**: the step's index, its start record, whether its goal
  has been met, and when.
- **On the end card**: the chain finished, or skipped (section 3).

Whether the card is drawn is not a state. The overlay's mode decides it
(section 3).

**Every frame of edit mode.** In the Prepare stage
(`docs/VIEW_LAYER.md`, section 5), `Tutorial::Update(world, now)` does
this, in order:

1. **A step just begun takes its start record.** This covers:
   - which snippets there are;
   - the subject, with its place, its size and its strokes;
   - how many times the overlay has come up.
2. **It chooses the subject** (section 6.5).
3. **It checks the goal.** Once met, a goal stays met (it is latched),
   even if what met it is undone.
4. **It checks the step's needs, in order**, while the goal is not met.
   The first need not met gives the hint line.
5. **It checks the near misses**, while the goal is not met and every
   need is. The first that holds gives the hint line.
6. **A step done for a second moves on.** In that second nothing is
   checked, and no hint is shown.

*Found while building phase 1:* the needs were first checked before the
goal, and the goal was not checked while one was not met. But what meets
a goal can itself leave a need unmet. Deleting the only snippet leaves
the delete step with no subject, so the step waited for a snippet
instead of moving on. A result is a result (6.3), so the goal comes
first, and the needs only guide while it is not met.

**Next and Back:**

- **Next** moves on, except on a gated step whose goal is not yet met
  (section 3).
- **Back** goes to the step before. That step shows its check if it was
  done, and takes its start record again, so it can be done again. A
  gated step that was done stays done, so going back never locks Next.
- **The goal was met before the step began.** Undo can land there, for
  example when Ctrl+Z is pressed during the second after a delete. The
  step's need says so ("Delete it again to try this"), and on a step
  that does not wait, Next moves on.

**What is kept on disk.** Each step change is kept, by the step's id, in
the setting `tutorial.welcome` (section 7.6).

## 6. Staying on track

### 6.1 What derails a tutorial

These are the app's actual ways off the path, with the welcome chain's
examples:

| Way | Examples |
|---|---|
| The subject goes | deleted; taken back by undo past its making; minimized to the dock; sent to another canvas; made fullscreen, which has no size of its own to move or resize |
| The canvas changes | a canvas bar tile; Alt+wheel; New canvas; the capture hotkey, which makes a canvas and switches to it; the Overview |
| The folder changes | a folder picked in the Overview; the tutorial folder deleted |
| The canvas is covered | the Overview; the cheat sheet; a context menu; Properties |
| A mode is in the way | drawing mode, where Delete and the arrows do nothing; a creation tool in hand, where the next press makes a snippet; a note being typed |
| The overlay leaves edit mode | the edit or view hotkey; the tray |
| A near miss | a double-click instead of a drag, which makes a fullscreen shot; Ctrl+drag, which makes a drawing; a resize instead of a move |
| The settings change | the trigger set to Ctrl, a key rebound: the card's words would go wrong |
| Out of order | drawing before moving; deleting before drawing |
| Wanting out | the user has seen enough |

### 6.2 Three ways to answer

**A. Rails: hold back everything but what the step teaches.** Not
proposed. It needs a gate at every entrance, and every entrance is a
different mechanism:

- the command table (about fifty commands, through `Available`);
- the recognizer's fifteen rules (a move, a stroke and a resize are not
  commands);
- every widget of every owner: the canvas bar's tiles, the dock's chips,
  the whole Overview;
- the global hotkeys, which are the tray's.

Each step would carry an allow-list over all of these, and every feature
added later would have to be taught to every list. A gate in front of
the machine's routing is a change to its structure, which
`docs/INTERACTIONS.md` asks to be argued "with the case that does not
fit". A feature each user runs once is not such a case.

The overlay also covers the user's program, often a game in full screen:

- Holding back the hotkey that puts the overlay away would trap them.
- Holding back anything else would make a first-time user's first
  impression "the app ignores me". The testers' complaint was already
  that it does not explain itself, and this would make it worse.

**B. A spotlight that blocks: dim the screen, take input only inside the
anchor.** Not proposed:

- A snippet being moved, or a stroke being drawn, leaves the anchor's
  rectangle at once.
- The screenshot step is about the whole screen.

It only fits read steps, and a read step has nothing to do.

**C. Guide: hold nothing back, make the steps unbreakable, and say how
to get back.** Agreed as a trial (question 1). Sections 6.3 to 6.8 say how, and
section 6.9 checks every way of 6.1 against them.

### 6.3 Results, not paths

**What a goal compares.** A goal compares the state with the step's
start record. It never looks at what was pressed. A snippet can be
deleted with Delete, with Backspace, or with the close button on its
bar; all three count, and so does any way added later. The tutorial
needs nothing from the gesture code, the commands or the menus: no
"moved" signal, no hook in `Dispatch`.

**Undo.** Undo is handled for free:

- A goal met and then undone stays met.
- A need that is undone away (the subject taken back past its making)
  shows its line, like any other need not met.

### 6.4 Needs

A step lists what it needs, from a closed list. Each need has its line,
and where it can, a way back in one click:

| Need | Not met when | The card says | Button |
|---|---|---|---|
| Canvas uncovered | a panel or a popup is up | "Close the Overview to go on." (and the same for the cheat sheet or a menu) | - |
| No drawing mode | a snippet is in drawing mode | "Click outside the snippet, or press Esc, to stop drawing first." | - |
| No tool in hand | a creation tool is in hand, other than the one the step asks for | "Press Esc to put the tool down first." | - |
| In the tutorial folder | the current canvas is in another folder | "The tutorial is in its own folder." | Go back to the tutorial |
| A subject | none of the tutorial's snippets will do, on this canvas or any other of its folder | "Take a screenshot to practice on, or let me put one here." | Put one here |
| The subject here | the subject is on another canvas of the tutorial folder | "It's on the canvas "{name}"." | Go back there |
| A subject that can move | the only candidate is fullscreen | "It fills the screen. Right-click it and choose Fullscreen to shrink it back." | - |
| The subject on screen | the subject is minimized | "It went to the dock. Click it there to bring it back." The spotlight moves to its chip. | - |
| The subject selected | the subject is not selected | "Click it to select it." | - |
| Drawing mode on the subject | no drawing mode on it | "Double-click it to draw on it again." | - |
| A deleted subject | the subject is on the canvas: brought back before the step began (section 5) | "Delete it again to try this, or go on with Next." | - |

The needs are checked in the step's order, every frame the goal is not
met (section 5). So a card never shows a text that is wrong for what is
on screen. Where the
situation is not the one the text assumes, the line under it says what
to do.

### 6.5 The subject

**The tutorial folder: a lightweight sandbox** (question 6). Every run
of the tutorial makes a folder of its own, named "Tutorial", with one
canvas, and switches to it:

- The user's other folders are left as they were, and the tutorial
  starts from a clean canvas every time.
- The tutorial's snippets are the snippets in this folder, so the
  user's library is never what the tutorial asks them to move, draw on
  or delete.
- It stays a sandbox by itself. A capture hotkey pressed during the
  tutorial makes its canvas in the folder the current canvas is in
  (`Editor::QuickCapture`), so the capture lands in the tutorial folder.
- It is an ordinary folder of the library. It shows in the Overview,
  its snippets are kept like any others, and the tutorial's steps are on
  the undo history like any others. A throwaway library was weighed
  against this and not proposed (section 11).
- When the tutorial ends, the user chooses whether the folder goes to
  the trash or is kept (question 9).

A snippet the user sends into the folder from elsewhere, through the
Overview's picker, counts as the tutorial's. Doing that is deliberate,
and the delete it may then meet is undoable.

**Only the tutorial's snippets.** A subject is always one of the
snippets in the tutorial folder.

**Choosing it.** A snippet *will do* when it still exists, is on the
current canvas, is not minimized, and is not fullscreen where the step
needs to move it. In order:

1. **It follows the hand.** The snippet being drawn on, or else the one
   selected last, when it will do. So working on "the wrong one" is
   working on the right one.
2. **It stays.** The subject there was, for as long as it will do.
3. **Otherwise the newest** of the tutorial's snippets that will do.
4. **Otherwise the subject there was**, while it still exists, or else
   the one that comes closest, so that the step's needs can say what is
   wrong with it.
5. **Otherwise none,** and the step's need for a subject says so.

*Found while building phase 1:* the hand came second, after the subject
staying, as first written. Then the hand never took over while the old
subject would still do, and the rule meant to let the user work on any
of the tutorial's snippets did nothing.

**Following the canvas.** The subject is looked for on the current
canvas. Switching canvases is therefore no derailment:

- On another canvas of the tutorial folder that has a subject, the step
  goes on there.
- On one without, the need's line offers the way back ("Go back there"),
  or a practice snippet.
- On a canvas outside the folder, the need "In the tutorial folder" says
  so, and its button switches back to the canvas of the folder the
  tutorial was last on.
- If the folder has been deleted, that button makes a new one.

### 6.6 Near misses

A step may list a few lines for mistakes that can be seen coming, each
a check on the state (the table in section 4). A near miss is checked
while the goal is not met, and only says something. It changes nothing,
not even undoing the miss: the user decides what to keep.

### 6.7 Out of order

A goal compares with its own step's start record. A stroke drawn during
step 2 therefore does not complete step 5: step 5 asks for one more,
and its text reads right either way. Skipping a step the user has
already done was considered and not proposed. Doing it once more, while
reading about it, is what the step is for.

### 6.8 Always a way out

- Skip is on every card. Next may wait on a gated step; Skip never
  does.
- The card takes no keys.
- Escape, undo and every hotkey do what they always do while the
  tutorial runs.
- Nothing about the tutorial can keep the user over their program.

### 6.9 Every way, answered

| Way (6.1) | Answer |
|---|---|
| The subject goes | a new subject (6.5); or the need's line with its button (6.4) |
| The canvas changes | the subject is followed; or Go back there (6.4, 6.5) |
| The folder changes | Go back to the tutorial, which makes the folder again if it is gone (6.4, 6.5) |
| The canvas is covered | the need's line (6.4) |
| A mode is in the way | the need's line (6.4) |
| The overlay leaves edit mode | the card goes and comes back with it (3); step 11 is built on it |
| A near miss | the hint (6.6) |
| The settings change | the text is resolved in every frame from the bindings and triggers as they are (7.1) |
| Out of order | the start record (6.7) |
| Wanting out | Skip; nothing is held (6.8) |

**If a step ever needs a hold.** Nothing in the welcome chain does. The
one place a hold would go is `Editor::Available`, with a reason: it is
what grays a menu row out and what a key checks. That covers commands
only, not gestures or widgets, and it is a change to
`docs/INTERACTIONS.md`, section 7, to be argued there with the step that
needs it.

## 7. How it fits

### 7.1 The world: what the tutorial reads

The world is an interface with only `const` queries, in `ui/tutorial/`.
It has no ImGui. `OverlayApp` implements it from the editor, the session,
the settings and the anchors, and a test fakes it. It answers:

- **The overlay:** its mode; how many times it has come up (counted in
  `OverlayApp::OnOverlayShown`).
- **What covers the canvas:** the panel or popup that is up, if any.
- **The hand:** the selection; the snippet in drawing mode; the tool in
  hand.
- **The canvas:** the current canvas's id, name and folder; the
  snippets on it; whether a folder exists and is not deleted, and its
  canvases.
- **A snippet, by id:**
  - its kind (a picture or not);
  - its rectangle;
  - whether it is fullscreen, minimized or deleted;
  - its canvas;
  - how many strokes it has.
- **Anchors:** where an anchor is this frame, if it is on screen (7.2).
- **Words:** the label of the keys that run a command, or of a hotkey,
  as bound now (`KeysFor`, `FormatKeyComboLabel`, as the cheat sheet
  uses them); the triggers as set.

Answers are values: ids, rectangles, counts. The tutorial never holds a
reference into the model, which is the same rule `docs/VIEW_LAYER.md`
set for actions.

### 7.2 Anchors

`ui/view/anchors.h` holds `AnchorId`, an enum of the places a step may
point at, and a board of where each is this frame. There are two kinds:

- **Marked by owners.** An owner that draws an anchored widget marks it
  as it draws it: `host_.Mark(AnchorId::SelectionBarClose, rect)`,
  through `ViewHost`. That is one line per anchored widget. The board is
  cleared in Prepare, so an anchor not drawn this frame is not on screen.
  The welcome chain marks:
  - the selection bar's close button;
  - the drawing bar's pen;
  - a dock chip, by its snippet.
- **Worked out from the model:** the subject, and its handles, from the
  snippet's rectangle and the selection's layout
  (`ui/selection_layout.h`), in the world's implementation.

Each owner knows only that there is a board. None of them knows the
tutorial.

### 7.3 What the tutorial may change

Only through view actions (`ui/view_action.h`), done in the Apply stage
like every other:

| Action | Does | Through |
|---|---|---|
| Tutorial next, back, skip, end | the runner's own state | the runner |
| Switch to a canvas | Go back there; Go back to the tutorial (6.4) | the existing `action::SwitchCanvas` |
| Make the tutorial folder | a start (7.6); Go back to the tutorial when the folder is gone | `Session::AddFolder` and `AddCanvas`, as the Overview's New folder does, then a switch |
| Practice snippet | Put one here (6.4): a snippet of the drawing kind, with a backing, at the middle of the canvas and clear of the card | `Session::CreateItem(prototype, /*undoable=*/false)` |
| Start the tutorial | the Settings button, the offer card (7.6) | the tutorial folder, then the runner |
| End with the folder | Done on the end card or the skip card (question 9) | kept, or the existing `action::Delete`, which puts it in the trash |

The practice snippet is made off the history, the way the first-run
notes have been made until now: undo cannot take it away from under a step. In every other
way it is an ordinary snippet, and it can be moved, drawn on and deleted
like one.

The tutorial's own progress, and which folder is its own, are
settings, set through `Settings::Set` (7.6).

### 7.4 The owner, and where it sits

`ui/view/tutorial_card.{h,cpp}` holds one owner in the sense of
`docs/VIEW_LAYER.md`, section 7. It draws two surfaces:

- **the card**, a window named `##tutorial_card`;
- **the spotlight**, an overlay.

It holds where the card was left, and records actions.

**New rows in the surface table** (`docs/VIEW_LAYER.md`, section 3), for
edit mode:

| # | Surface | Kind | Up while | Owner |
|---|---|---|---|---|
| between 10 and 11 | The tutorial card | window `##tutorial_card` | the tutorial is on | Tutorial card |
| between 13 and 14 | The spotlight | overlay | the step points at an anchor on screen | Tutorial card |

The positions have reasons:

- **The card is above the panels**, so that a step can talk about the
  Overview.
- **The card is below the delete confirmation**, which must stay
  reachable.
- **The spotlight is above everything but the pointer**, so that it can
  ring something inside a panel.

**Stages:**

- The runner updates in stage 1, Prepare. It changes nothing but its
  own state.
- The card is drawn in stage 6, Panels, after the cheat sheet and before
  the delete confirmation.
- The spotlight is drawn in stage 7, with the messages. Like them, it
  changes nothing.

### 7.5 The runner

`ui/tutorial/`, with no ImGui, tested without a frame:

- `world.h`: the world (7.1).
- `step.h`: a step, its needs, and the functions for its goal, its
  subject rule and its near misses.
- `welcome_chain.{h,cpp}`: the table of section 4.
- `tutorial.{h,cpp}`: the runner (section 5).

```cpp
// The sketch, not the final shape.
struct Step {
    std::string_view id;               // stable: kept on disk (7.6)
    StepKind kind;                     // Read, Do
    bool gated;                        // Next waits for the goal (section 3)
    std::string_view title, text;      // ui_strings keys, with {placeholders}
    std::optional<Pointing> pointsAt;  // an AnchorId, the subject, or a handle of it
    std::vector<Need> needs;           // checked in order (6.4)
    SubjectRule subject;               // none, any, can move (6.5)
    Goal goal;                         // bool(const World&, const StartRecord&)
    std::vector<NearMiss> nearMisses;  // {check, ui_strings key}
};
```

Goals and near misses are plain functions in the table. A closed set of
goal kinds would be more declarative, but every new step would need a
new kind. A function is as readable in the table, and each is tested
against the fake world. Needs are a closed list, because they are shared
across steps and each carries a text and a button.

### 7.6 Start, resume and start again

**The settings.** Two catalog rows (`docs/SETTINGS.md`, section 3):

| Key | Field | Default | Rule | Edited from | Effect |
|---|---|---|---|---|---|
| `tutorial.welcome` | `tutorialWelcome` | `""` | `""` (never shown), `"offered"`, a step id, `"finished"` or `"skipped"` | the tutorial; Settings > Interaction | Use |
| `tutorial.folder` | `tutorialFolder` | 0 | the tutorial folder's id; 0 for none | the tutorial | Use |

Both are Global, and in no profile. Adding keys needs no version bump
(section 8 there).

The folder's id is a library id kept in the settings file. The two
files can disagree: the library may be set aside, or the folder deleted.
Either way the id finds no live folder, and a start or a resume makes a
new one. A row in the library's `meta` table would keep the two
together, at the cost of a change to the store. It is not needed for
this.

**When the tutorial starts:**

- **A first run** (`TrayController`'s `firstRun_`, which
  `RequestWelcomeNote` answers today) starts it at its first step. It
  makes the tutorial folder beside the folder and canvas a first run
  makes. That one stays empty, for the user's own work.
- **A start after quitting partway** comes back to the step it was on,
  by its id, the next time edit mode comes up. It switches to the
  tutorial folder, or makes a new one if that folder is gone. An id no
  longer in the chain starts the chain again.
- **An install from before 0.2.0** (a library, and `tutorial.welcome`
  empty) is offered the tutorial once, the first time edit mode comes up
  (question 5). A card of its own reads "New: a short tutorial, in a
  folder of its own. [Start] [No thanks]". Either answer is kept
  (`"offered"`), so the offer is made once. Start begins the chain as
  below.

**Take the tutorial again** is a button in Settings > Interaction
(question 7). It:

1. closes the Overview;
2. makes a new tutorial folder, and switches to it (question 6). A
   folder from an earlier run is left as it is;
3. starts at the first step.

**The first-run notes go** (question 4). A first run places no notes.
The welcome note's content is on the first and last cards, and the two
warnings are steps 9 and 10, and the skip card (section 3).
`OverlayApp::PlaceWelcomeNotes`, `RequestWelcomeNote` and the
`welcome.*` strings go with them.

### 7.7 What does not change

- **The input machine:** its levels, the routing, the recognizer's
  rules, the command table and `Available`.
- **The session and the store.** The folder and the practice snippet
  are made with calls that exist.
- **The overlay's states and their table.** The tray only asks for the
  tutorial where it asks for the welcome notes today.
- **The surface table's existing rows**, and every ImGui id a test finds
  a widget by.

## 8. Extending it

**A step** is:

- a row in the chain's table;
- its strings;
- sometimes an anchor marked in the owner that draws it;
- rarely a new need.

**A chain** is:

- a table;
- a catalog row for its progress;
- a place it is offered from.

**What later steps or chains could cover:**

- the context menu and Properties;
- the wheel, to scale and change opacity;
- canvases and the canvas bar;
- the Overview, the trash and restoring;
- pinning, and view mode;
- the capture hotkeys;
- profiles.

The design already carries what these need:

- steps over a panel (the card above the panels);
- anchors inside the Overview (the owner marks them);
- steps about hotkeys (the world grows a count, as it has for showings).

**Every new step brings its tests** (section 9): its goal, needs and
near misses against the fake world, and a walk-through by the real
gesture.

## 9. Tests

- **The runner**, against a fake world:
  - Next, Back and Skip, and the end card;
  - goals latched;
  - the move on after a second;
  - progress kept by id; an unknown id starts the chain again;
  - Next grayed out on a gated step until its goal is met, and not after
    Back.
- **The chain's shape.** Every need of every step is either the goal of
  an earlier gated step, or has a line that says how to meet it. Step ids
  are unique.
- **Each step:** its goal, each of its needs and each near miss, with
  the fake world set up for each.
- **A walk-through.** In the headless harness (`HeadlessAppTest`), the
  whole chain is done with real gestures: a drag, a double-click, Delete,
  Ctrl+Z, the edit hotkey twice. The test checks that each card moves
  on.
- **The derail matrix.** For each do step, crossed with each way of 6.1
  that applies (subject deleted, undo run several times, canvas
  switched, Overview opened, cheat sheet opened, overlay hidden and
  shown, view mode, minimized, fullscreen, the capture hotkey), the test
  checks:
  - the card shows a line that applies;
  - Next and Skip are there;
  - following the line and then doing the step completes it.
- **The folder:**
  - a start makes it and switches to it;
  - a capture hotkey during the tutorial lands in it;
  - a canvas outside it shows "Go back to the tutorial", and the button
    switches back;
  - the folder deleted, the button makes a new one, and so does a resume;
  - starting again makes a new folder and leaves the old one;
  - Done puts it in the trash, and Done, keep the folder keeps it.
- **The start:**
  - a first run places no notes and starts the chain;
  - an install from before gets the offer once, whatever it answers.
- **The stack.** The table in `tests/support/view_stack.h` gains the
  card's row.
- **By hand.** A scratch instance with its own APPDATA, and a screenshot
  of each card.

## 10. Getting there

Each phase is a set of reviewable commits, and the app is whole after
each.

1. **The runner and the welcome chain**, against the fake world. No UI
   yet.
2. **On screen:**
   - the world's implementation;
   - the anchor board and the three marks;
   - the card and spotlight owner;
   - the actions and the stack rows;
   - the walk-through test and the derail matrix.
   The tutorial can be started only by a test at this point.
3. **Start, resume and start again:**
   - the two settings;
   - the tutorial folder, and Go back to the tutorial;
   - the first run, with the first-run notes gone;
   - the Settings button;
   - the offer to installs from before;
   - the end with the folder: trash, or keep (question 9).
4. **By hand, and the docs:**
   - the hand check;
   - `docs/ARCHITECTURE.md` gains a "The tutorial" paragraph;
   - the rows in `docs/VIEW_LAYER.md` and `docs/SETTINGS.md`;
   - this document marked built.

## 11. Considered and not proposed

- **Rails, and a spotlight that blocks** (6.2).
- **A throwaway library** (in memory, or a temp file), swapped in for
  the tutorial's run. It would keep the user's library out of reach, and
  would let later chains teach destructive things safely: deleting a
  canvas, emptying the trash. The swap itself is easy: the session's
  store is a pointer, and a session runs without one. What makes it
  costly:
  - The session would need a call to put its model and undo history
    aside and bring them back. `ImportLibrary` cannot do it, since it
    erases every snippet marked deleted.
  - `OverlayApp`, `Editor`, the gestures and every owner hold a fixed
    `Session&`. Much of the view's state refers to the library that is
    up: the selection, the clipboard, the popup record, the Overview's
    picker and renames, the canvas bar's last canvas, the stroke
    rasters. Every owner would need an answer to "the library was
    replaced", a new rule in `docs/VIEW_LAYER.md` that is easy to forget.
  - A capture hotkey pressed in step 11, back in the user's program,
    would land in the throwaway library and be lost, unless it were held
    back or both libraries ran at once.
  - Keeping what was made there would need snippets and pictures copied
    between two stores.

  The tutorial folder (6.5) is the lightweight sandbox chosen instead.
  The tutorial reads only through the world and changes things only
  through actions, so a throwaway library can still be added under it
  later. It would be argued as its own change, when a chain needs it,
  and no step would change.
- **A modal "Take the tour?" box before anything.** A first run already
  puts the app in front of the user. A card they can ignore is kinder,
  and Skip is one click.
- **More first-run notes.** A note cannot see what the user does, point
  at anything, or move on.
- **A demo that moves a pointer by itself.** It teaches watching, not
  doing.
- **Keys on the card**, such as Enter for Next. A card that takes keys
  is a level in the machine, and Enter would be taken from a note being
  typed.
- **Goals from events**, such as a "moved" signal from the gesture code.
  That would tie the gestures to the tutorial. Comparing the state needs
  nothing from them (6.3).

## 12. Questions for review, and the answers

1. **Guide, don't guard** (section 6). Recommended: hold nothing back;
   steps that cannot be derailed, needs with their way back, near
   misses. The alternative is holds on a few commands through
   `Available`. *Answer (2026-09-28):* as recommended, as a trial.
2. **Moving on by itself.** Recommended: a do step whose goal is met
   shows its check and moves on about a second later. The alternative is
   waiting for Next. *Answer:* as recommended.
3. **Next on a do step not yet done.** Recommended: it moves on. The
   alternative is Next disabled until the goal is met. *Answer:* it
   depends on the step. Next moves on for a read step and for a do step
   nothing later depends on. For a step that makes what a later step
   needs, it waits for the goal: the gated steps of section 4. Skip
   tutorial is always there.
4. **The first-run notes.** Recommended: the welcome note goes, and the
   two warnings stay as notes beside the card. *Answer:* the warnings
   become part of the tutorial, as steps 9 and 10. Someone who skips
   sees them on the skip card, with where to take the tutorial again. So
   no notes are placed at all.
5. **Installs from before 0.2.0.** Recommended: offer it once, the first
   time edit mode comes up. *Answer:* as recommended.
6. **Where it starts again.** Recommended: a canvas of its own named
   "Tutorial". *Answer:* a whole folder of its own, as a lightweight
   sandbox and a clean starting point, and for every run, including the
   first (6.5). A throwaway library was also weighed (section 11).
7. **Where the button goes.** Recommended: Settings > Interaction.
   *Answer:* as recommended.
8. **Step 11, the overlay put away and back.** Recommended: in the
   welcome chain. *Answer:* as recommended.

Asked after the first answers:

9. **What becomes of the tutorial folder at the end.** Recommended: the
   end card and the skip card each have two buttons:
   - "Done", the primary one, which puts the folder in the trash;
   - "Done, keep the folder".

   The trash can give it back, like any folder, until the retention
   period deletes it for good. It is a sandbox, and runs of it should
   not pile up in the Overview, so putting it away is the default. Keep
   is one click, for someone who took a screenshot they want. The
   alternatives are always keeping it, or always putting it in the
   trash. *Answer:* as recommended.
