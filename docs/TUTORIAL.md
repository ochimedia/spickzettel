# The tutorial

Status: **built** (2026-09-28), for 0.2.0, in the five phases of
section 10. It was proposed on 2026-09-27 and agreed on 2026-09-28. Its
questions and their answers are in section 12, and the design below was
changed to match them. **Topics** (section 13), several chains chosen
from a list, were agreed after phase 3 and built as phase 4. The
sections below describe what is built; section 13 keeps the reasoning
for the topics. **The next topics** are listed in 13.2. The first of
them, Pinning and view mode (section 14), is built; the others are to
be designed.

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
  marked where a widget is drawn, one owner in the view, and its rows
  in the settings.

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

**What a first run offered before the tutorial.** It opened edit mode
with three notes, which are gone now (question 4):

- **The welcome.** How to make a screenshot and open a menu, the
  edit-mode hotkey, the cheat sheet's key, and how to delete.
- **Two warnings.** One on setting the app up for each program, one on
  anti-cheat.

The cheat sheet, on its key or from the empty canvas's menu, lists every
key and gesture.

**Why that was not enough.** Both are references. They say what exists,
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

- **Topic**: a subject the tutorial covers, such as Basics or Drawing,
  picked from the tutorial's list (section 13).
- **Chain**: a topic's ordered list of steps. Until phase 4 there was one,
  the *welcome chain*; it was split into Basics and Drawing.
- **The list**: the card's list of topics, to start one from (13.3).
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

- "Step 3 of 10", with a thin progress bar;
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

**Where it sits.** At the top center by default, clear of what the
step is about: the anchor the ring is on, the subject, and the bars
over the selection, whose buttons a hint may name with no ring on them.
If any of these lies under it, the card tries the bottom center, then
the top corners, and takes the first place that covers none of them, or
else the one that covers least, the anchor and the subject counting
double. The user can drag the card by its title, and it stays where it
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

**The spotlight.** A ring around the anchor, in a soft glow:

- It is in the tutorial's own highlight, a bright cyan, and not in the
  accent. The selection frame is in the accent, and a ring the same color
  around a selected snippet was hard to see. The accent is also a
  setting, and can be dark. The card marks out its hint, a warning's
  title, the check and the progress in the same cyan.
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

- The card shows the two warnings (Basics' steps 6 and 7, section 4) in
  short form. In Basics it leaves out those the user has already passed
  in this run; from any other topic it shows both, until Basics has been
  finished once (13.6). Someone who skips at once still sees them.
- It offers Back, to the step skipped from, so a misclick costs nothing.
  That is why no confirmation box is needed.
- Its Done ends the topic and puts its folder in the trash. Its "Done,
  keep the folder" keeps the folder (question 9). The end card has the
  same two buttons.
- Its "More topics" opens the list, and it says the tutorial is also in
  Settings > Interaction. The end card has "More topics" too.

**The list.** A state of the card, with a row per topic: its title, one
line on what it covers, its number of steps, and where the user is with
it (New, At step 4 of 10, Started, Done). A row starts its topic; the
list's Back goes back to the topic running, and its Close, with none
running, takes the card away (13.3).

## 4. The chains

Each topic's chain runs in its tutorial folder (section 6.5), and "the
tutorial's snippets" means the snippets in it. The keys in braces are
named as they are bound, and written for the triggers as set: on a first
run the screenshot trigger is plain, a drag with no key held. In the Next
column, "waits" marks a gated step (section 3).

**Basics:**

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when | Near misses (6.6) |
|---|---|---|---|---|---|---|---|---|
| 0 | `welcome` | read | moves on | Spickzettel keeps snippets (screenshots, drawings and notes) over whatever program is underneath, and everything saves itself. This tutorial runs in a folder of its own. | - | - | Next | - |
| 1 | `screenshot` | do | **waits** | Drag a box over anything on screen to take a screenshot of it. With a trigger key set: "Hold {trigger} and drag". With the trigger off: "Press {newScreenshot}, then drag", or with that key unbound too, the empty canvas's menu. | - | in the tutorial folder; canvas uncovered; no drawing mode; no tool in hand but the screenshot tool | a screenshot snippet made since the step began, on the current canvas, not fullscreen | a fullscreen one: "That took the whole screen - a double-click does that. Press {key:deleteSelection} to delete it, and drag a box instead." (question 31) A drawing: "That made a drawing; {drawingTrigger}+drag does that." |
| 2 | `move` | do | moves on | Drag it to move it. A press selects it, and a drag takes it along. | the subject | a subject that can move; no drawing mode | the subject moved 16 px or more from where the step found it, its size changed by less than 10%, and not fullscreen | resized instead: "That changed its size. Drag from the middle to move it." |
| 3 | `resize` | do | moves on | Drag a corner to resize it; Shift switches keeping its shape. A right-drag near an edge does it too. | the subject's lower right handle | a subject that can move; no drawing mode; the subject selected | its width or height changed by 10% or more, and not fullscreen | - |
| 4 | `delete` | do | **waits** | Select it and press Delete, or the close button on its bar. | the subject's close button | a subject; no drawing mode | the subject deleted | - |
| 5 | `undo` | do | moves on | Deleted by mistake? {undo} brings it back. It takes back anything you did, a step at a time. | - | in the tutorial folder; canvas uncovered; the subject's canvas; a deleted subject | the subject back on the canvas, after being deleted in this step (or when it began) | - |
| 6 | `programs` | read | moves on | Set it up for your programs. Some games break when the overlay takes focus; others need it to. Look through Settings > Behavior, and make a profile for each program that needs its own. | - | - | Next | - |
| 7 | `antiCheat` | read | moves on | Careful with anti-cheat. Some games watch for tools that draw over them or read their input. If a game might object, quit Spickzettel before you start it. | - | - | Next | - |
| 8 | `away` | do | moves on | Press {editMode} to put the overlay away and go back to your program. Press it again to come back here. | - | - | the overlay has come back since the step began | with the hotkey unbound, the text names the tray icon instead |
| 9 | `end` | read | Done | That's the basics. {cheatSheet} shows every key and gesture, and a right-click on anything shows what it can do. More topics has the others. Done puts the folder in the trash; Done, keep the folder keeps it. | - | - | Done | - |

**Drawing:**

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when |
|---|---|---|---|---|---|---|---|
| 0 | `drawingMode` | do | **waits** | Any snippet can be drawn on, a screenshot as well as a drawing. Double-click it, or hold the button down on it, to draw on it. | the subject | in the tutorial folder; canvas uncovered; a subject; the subject here and on screen | drawing mode on the subject |
| 1 | `draw` | do | moves on | Drag across it to draw. The bar above it has the pen, the eraser, text and the color. | the drawing bar's pen | the same, and drawing mode on the subject | the subject has more strokes than when the step began |
| 2 | `stopDrawing` | do | moves on | Click outside it, or press Esc, to stop drawing. | - | - | no drawing mode |
| 3 | `end` | read | Done | That's drawing. Double-click any snippet to draw on it. Done puts the folder in the trash; Done, keep the folder keeps it. | - | - | Done |

A new folder has no snippet, so Drawing starts with its need for a
subject unmet: its line says to take a screenshot, and its button, Put
one here, places the practice snippet (6.4).

**Pinning and view mode** has its table in section 14.2.

*Found while building phase 1:* the welcome text first said that the
screen behind is frozen while you edit. That holds only with "Freeze
screen while editing" on, which is off by default, so the card no
longer says it. The move and resize steps also need drawing mode off,
since a drag on a snippet in drawing mode draws.

*Found while building phase 2,* by the derail matrix (section 9):

- Fullscreen changes a snippet's rectangle, and so counted as a resize,
  and made the resize step done. The move and resize goals now ask for
  a subject that is not fullscreen, and a fullscreen one gets the "It
  fills the screen" line.
- The undo step had no needs but a deleted subject. With the Overview
  or the cheat sheet up, Ctrl+Z does nothing (a panel has the keys), and
  on another canvas it takes back that canvas's steps, since the history
  is kept per canvas. Either way the card said nothing. The step now
  needs the tutorial folder, an uncovered canvas and the subject's
  canvas, each with its line.

**Which steps wait.** A step waits when a later step needs what its goal
makes, and nothing else can make it:

- the screenshot is the subject of every Basics step after it;
- the deleted subject is what Basics' `undo` brings back;
- drawing mode is what Drawing's `draw` draws in.

The other do steps move on, because no later step depends on them. A
need they leave unmet has its own way back, with its own line: Basics'
`delete` needs drawing mode off, and says how. A test checks this for
every topic: every need of every step is either the goal of an earlier
gated step of its chain, or has a line, or a button, that says how to
meet it (section 9).

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

**What is kept on disk.** Each step change is kept, by the step's id,
under the topic's id in the setting `tutorial.progress` (section 7.6).

## 6. Staying on track

### 6.1 What derails a tutorial

These are the app's actual ways off the path, with the topics'
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
| The subject pinned | the subject is not pinned | "It isn't pinned. Select it, and press Pin on its bar." | - |
| The pen in hand | another tool is in hand | "Press the pen on the bar." | - |
| Something drawn on the subject | the subject has no strokes | "Nothing is drawn on it to erase. Draw something with the pen first." | - |

The needs are checked in the step's order, every frame the goal is not
met (section 5). So a card never shows a text that is wrong for what is
on screen. Where the
situation is not the one the text assumes, the line under it says what
to do.

### 6.5 The subject

**The tutorial folder: a lightweight sandbox** (question 6). Every run
of a topic makes a folder of its own, named for the topic ("Tutorial:
Basics"), with one canvas, and switches to it:

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
| The overlay leaves edit mode | the card goes and comes back with it (3); Basics' `away` step is built on it |
| A near miss | the hint (6.6) |
| The settings change | the text is resolved in every frame from the bindings and triggers as they are (7.1) |
| Out of order | the start record (6.7) |
| Wanting out | Skip; nothing is held (6.8) |

**If a step ever needs a hold.** Nothing in the topics built does. The
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
  `OverlayApp::OnOverlayShown`), and how many times the pinned view and
  view mode have (14.3); how many screenshots each capture hotkey has
  taken (16.3).
- **What covers the canvas:** the panel or popup that is up, if any,
  but a snippet's own popups, the color chooser and Properties (15.3).
- **The hand:** the selection; the snippet in drawing mode; the tool in
  hand, and the eraser's shape; the pen's color and width; the snippet
  whose note is being typed (15.3).
- **The canvas:** the current canvas's id, name and folder; the
  snippets on it; whether a folder exists and is not deleted, and its
  canvases.
- **A snippet, by id:**
  - its kind (a picture or not);
  - its rectangle;
  - whether it is fullscreen, minimized or deleted;
  - its canvas;
  - its strokes, each with its color, its width and length on screen,
    and whether it is a line or a rectangle (15.3);
  - its note's text;
  - whether it is pinned, and its picture's and strokes' opacity.
- **Anchors:** where an anchor is this frame, if it is on screen (7.2).
- **Words:** the label of the keys that run a command, or of a hotkey,
  as bound now (`KeysFor`, `FormatKeyComboLabel`, as the cheat sheet
  uses them), and none for a hotkey another program holds (16.3); the
  triggers as set.
- **The tutorial's own progress:** what is kept for a topic (7.6), for
  the list and the skip card's warnings.

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
  The topics mark:
  - the selection bar's close button and its Pin;
  - the drawing bar's pen, eraser, Text and color;
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
| Tutorial next, back, skip, end; More topics, and leaving the list | the runner's own state, and the card's | the runner, the card |
| Switch to a canvas | Go back there; Go back to the tutorial (6.4) | the existing `action::SwitchCanvas` |
| Make the tutorial folder | a start (7.6); Go back to the tutorial when the folder is gone | `Session::AddFolder` and `AddCanvas`, as the Overview's New folder does, then a switch |
| Practice snippet | Put one here (6.4): a snippet of the drawing kind, with a backing, at the middle of the canvas and clear of the card | `Session::CreateItem(prototype, /*undoable=*/false)` |
| Start a topic | a first run, an install from before, a row of the list (7.6) | the running topic let go of, the tutorial folder, then a runner for the topic's chain |
| Resume a topic | a start after quitting partway (7.6) | its folder, or a new one; then a runner at its step |
| Open the list | the Settings button (7.6) | the Overview closed; the card's own state |
| End with the folder | Done on the end card or the skip card (question 9) | kept, or the existing `action::Delete`, which puts it in the trash |

The practice snippet is made off the history, the way the first-run
notes were made before the tutorial: undo cannot take it away from under
a step. In every other
way it is an ordinary snippet, and it can be moved, drawn on and deleted
like one.

The tutorial's own progress, and which folder is its own, are
settings, set through `Settings::Set` (7.6).

### 7.4 The owner, and where it sits

`ui/view/tutorial_card.{h,cpp}` holds one owner in the sense of
`docs/VIEW_LAYER.md`, section 7. It draws two surfaces:

- **the card**, a window named `##tutorial_card`;
- **the spotlight**, an overlay.

It holds the runner, which topic it runs, whether the list is up and
where the card was left, and records actions.

**New rows in the surface table** (`docs/VIEW_LAYER.md`, section 3), for
edit mode:

| # | Surface | Kind | Up while | Owner |
|---|---|---|---|---|
| between 10 and 11 | The tutorial card | window `##tutorial_card` | a topic runs, or the list is up | Tutorial card |
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
- `chains.{h,cpp}`: the tables of section 4, one function per topic's
  chain, sharing their goals, needs and near misses.
- `topics.{h,cpp}`: the table of topics, in the list's order (13.2).
- `tutorial.{h,cpp}`: the runner (section 5). A runner runs one chain;
  the card makes another for another topic, and lets go of the one
  running with `Leave` (13.3).

```cpp
struct Step {
    std::string_view id;                       // stable: kept on disk (7.6)
    StepKind kind;                             // Read, Do
    bool gated;                                // Next waits for the goal (section 3)
    bool warning;                              // repeated on the skip card (13.6)
    const char* title;                         // from ui_strings
    const char* (*text)(const World&);         // chosen for the keys and triggers
    Spot spot;                                 // an anchor, the subject, or a handle of it
    std::vector<Need> needs;                   // checked in order (6.4)
    SubjectRule subject;                       // none, any, can move, last deleted, pinned (6.5)
    Check goal;                                // bool(const Look&)
    std::vector<NearMiss> nearMisses;          // {check, ui_strings text}
};
```

Goals and near misses are plain functions in the table. A closed set of
goal kinds would be more declarative, but every new step would need a
new kind. A function is as readable in the table, and each is tested
against the fake world. Needs are a closed list, because they are shared
across steps and each carries a text and a button.

### 7.6 Start, resume and start again

**The settings.** Three catalog rows (`docs/SETTINGS.md`, section 3):

| Key | Field | Default | Rule | Edited from | Effect |
|---|---|---|---|---|---|
| `tutorial.progress` | `tutorialProgress` | `{}` | each topic's id, mapped to a step id, `"finished"` or `"skipped"`; no entry for a topic never started | the tutorial | Use |
| `tutorial.current` | `tutorialCurrent` | `""` | the topic running, or empty | the tutorial | Use |
| `tutorial.folder` | `tutorialFolder` | `"0"` | the running topic's folder id, as digits; 0 for none | the tutorial | Use |

All three are Global, and in no profile. Adding keys needs no version
bump (section 8 there). Phase 3 built a single `tutorial.welcome` row,
which phase 4 replaced before any release had it.

The folder's id is a library id kept in the settings file. The two
files can disagree: the library may be set aside, or the folder deleted.
Either way the id finds no live folder, and a resume makes a new one. A
row in the library's `meta` table would keep the two together, at the
cost of a change to the store. It is not needed for this.

**When a topic starts** (`OverlayApp::WelcomeAtStart`, told by
`TrayController` what its start found of the library; done the first
time edit mode comes up):

- **A first run** starts Basics at its first step. It makes the
  tutorial folder beside the folder and canvas a first run makes. That
  one stays empty, for the user's own work.
- **A start after quitting partway** goes on with the topic
  `tutorial.current` names, at the step kept for it. It switches to that
  topic's folder, or makes a new one if the folder is gone. A step id no
  longer in the chain starts the topic again.
- **An install from before 0.2.0** (a library, and nothing in
  `tutorial.progress`) starts Basics, as a first run does (question 11).
  Phase 3 built an offer card for it (question 5), which phase 4 took
  out.
- **A row of the list** starts its topic at its first step, in a new
  folder, whatever its status (question 12). A topic running is let go
  of first, as "Done, keep the folder" would (question 13).

**Open the tutorial** is a button in Settings > Interaction (question
7). It closes the Overview and opens the list.

**The first-run notes are gone** (question 4). A first run places no
notes. The welcome note's content is on Basics' first and last cards,
and the two warnings are Basics' steps 6 and 7, and on the skip card
(section 3).

### 7.7 What does not change

- **The input machine:** its levels, the routing, the recognizer's
  rules, the command table and `Available`.
- **The session and the store.** The folder and the practice snippet
  are made with calls that exist.
- **The overlay's states and their table.** The tray tells the overlay
  what its start found of the library, where it asked for the welcome
  notes before.
- **The surface table's existing rows**, and every ImGui id a test finds
  a widget by.

## 8. Extending it

**A step** is:

- a row in the chain's table;
- its strings;
- sometimes an anchor marked in the owner that draws it;
- rarely a new need.

**A topic** is:

- a chain in `ui/tutorial/chains.cpp`, or a file of its own;
- a row in the topic table, with its title and one line on what it
  covers;
- its strings.

Its progress is kept under its id with no new setting, and the list
offers it with no new code.

**What the next topics are to cover** is the table in section 13.2, in
the order they are to be built: Pinning and view mode (section 14),
Drawing and notes, Capturing, Folders and canvases, and Profiles. Each
is designed in this document first.

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
- **The chains' shape,** for every topic in the table. Every need a
  step cannot meet by itself is the goal of an earlier gated step of its
  chain. Step ids are unique within a chain, and topic ids within the
  table. Each chain ends on a read step.
- **Each step:** its goal, each of its needs and each near miss, with
  the fake world set up for each.
- **A walk-through,** for every topic. In the headless harness
  (`HeadlessAppTest`), the whole chain is done with real gestures: a
  drag, a double-click, Delete, Ctrl+Z, the edit hotkey twice, Put one
  here. The test checks that each card moves on.
- **The derail matrix.** For each do step of each topic, crossed with each way of 6.1
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
  - a first run places no notes and starts Basics;
  - an install from before starts Basics too;
  - the topic running when the app quit is resumed at its step, and no
    other;
  - a library whose tutorial is over starts nothing.
- **Progress:** kept per topic as it goes; another topic started keeps
  the running one's folder, and what it ended as.
- **The list:** More topics on the end card and the skip card, and the
  Settings button, open it; Back returns to the card it came from; a
  row starts its topic at its first step; each topic's status; Drawing's
  skip card shows the two warnings until Basics is finished.
- **The stack.** The table in `tests/support/view_stack.h` gains the
  card's row.
- **By hand.** A scratch instance with its own APPDATA, and a screenshot
  of each card, both topics walked through by real input (section 10,
  phase 5).

## 10. Getting there

Each phase was a set of reviewable commits, and the app was whole after
each. All five are built.

1. **The runner and the welcome chain**, against the fake world. No UI
   yet.
2. **On screen:**
   - the world's implementation;
   - the anchor board and the three marks;
   - the card and spotlight owner;
   - the actions and the stack rows;
   - the walk-through test and the derail matrix.
   The tutorial could be started only by a test at this point.
3. **Start, resume and start again:**
   - the settings;
   - the tutorial folder, and Go back to the tutorial;
   - the first run, with the first-run notes gone;
   - the Settings button;
   - the offer to installs from before (taken out in phase 4);
   - the end with the folder: trash, or keep (question 9).
4. **Topics** (section 13):
   - Basics and Drawing split out of the welcome chain, and the topic
     table;
   - the progress kept per topic;
   - the list, and More topics;
   - Basics started on a first run and for installs from before;
   - Open the tutorial in Settings;
   - the skip card's warnings from any topic.
5. **By hand, and the docs:**
   - the hand check: both topics walked through in a scratch instance
     by real input, with a screenshot of each card, the list, the
     delete confirmation, and the card over the Overview;
   - `docs/ARCHITECTURE.md` gains "The tutorial";
   - the rows in `docs/VIEW_LAYER.md` and `docs/SETTINGS.md`;
   - this document marked built.

*Found by hand:* nothing broke. The spotlight was hard to see in the
accent around a selected snippet, and has a color of its own (section
3). Runs of one topic leave folders of one name, "Tutorial: Basics" and
"Tutorial: Basics" (13.5).

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
  - A capture hotkey pressed in Basics' `away` step, back in the user's program,
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

Asked with the topics (section 13), after phase 3 was built:

10. **Topics and a list.** Asked for by the review: several chains,
    chosen from a list on the card. Recommended, with four decisions:
    - progress kept per topic, and only the running topic resumed after
      a restart (13.7);
    - one folder per run of a topic, named for it, with the folders
      made during a run counted as its space (13.5);
    - the two warnings stay in Basics, and the skip card of any topic
      shows them until Basics is finished (13.6);
    - the derail matrix, the walk-through and the chain's shape test
      run per topic (13.9).

    *Answer (2026-09-28):* as recommended.
11. **Installs from before 0.2.0, with topics.** Recommended: the offer
    card's Start opens the list, since those users know the basics.
    *Answer:* treat them as new users. Basics starts, as on a first run,
    and the offer card goes (13.4). This revises the answer to question
    5.
12. **A topic left partway, chosen again from the list.** Recommended:
    it goes on at its step, in a new folder, where its needs say what
    is missing. The alternatives are:
    - starting it over;
    - going back to its old folder, which needs a folder kept per topic.

    *Answer (2026-09-28):* it starts over, at its first step. A step in
    the middle may rest on what earlier steps made, and in a new folder
    none of it is there. Only the topic running when the app quit goes
    on at its step, at the next start (decision 1 of question 10).
13. **Another topic chosen while one runs.** Recommended: the running
    one ends as "Done, keep the folder" would, with its progress kept at
    its step. The alternative is asking first, which adds a box for
    something the user can undo by choosing the first topic again.
    *Answer:* as recommended.
14. **The folder's name.** Recommended: "Tutorial: {topic}", such as
    "Tutorial: Drawing". The alternative is "Tutorial" for every topic,
    which leaves several folders of one name in the Overview. *Answer:*
    as recommended.
15. **How much of Drawing in the first build.** Recommended: the three
    steps that exist, so topics can ship without new steps to design.
    The pen, eraser, color, width and text steps are the next piece of
    work. The alternative is designing and building them now, in phase
    4. *Answer:* as recommended. A drawing topic in more depth is
    designed later.

## 13. Topics

Status: **built** (2026-09-28) as phase 4, after phases 1 to 3. Its
questions are 10 to 15 of section 12, and this section matches their
answers. Sections 3 to 10 describe what is built; this section keeps
why, and what is still to come.

### 13.1 What changes, and why

One chain becomes several **topics**. Each topic is a chain of its own,
and the user chooses one from a list on the card:

- **Each chain stays short.** The welcome chain already has 13 steps,
  and every step about the Overview or a profile would add to it.
- **Each topic can go into more detail** than one long chain could
  afford. The drawing topic can show the eraser, the colors and text,
  where the welcome chain had room for one stroke.
- **A refresher.** A user who comes back later picks the one topic they
  need, rather than walking the whole chain again.

Most of what is needed is built already, and stays as it is:

- the runner, which already takes whichever chain it is given (section
  7.5);
- the needs and the hints;
- the spotlight and the card;
- the folder;
- the first run and the resume.

The new parts are:

- the table of topics;
- the list, a state of the card;
- the progress, kept per topic;
- the steps of section 4, split between two topics;
- each later topic's own steps, as it is written.

### 13.2 The topics

| Id | Title | What it covers | Status |
|---|---|---|---|
| `basics` | Basics | a screenshot; moving and resizing it; deleting it and undoing that; the two warnings; putting the overlay away and back | built: section 4's steps, but three |
| `pinning` | Pinning and view mode | pinning a snippet, and the pinned view; making it see-through with the wheel or Properties; view mode; unpinning | built: section 14 |
| `drawing` | Drawing and notes | drawing mode, a stroke, stopping; the color; the width; a line and a rectangle; the eraser, its rectangle and the right button; a note | built: section 15 |
| `capturing` | Capturing | a blank drawing; a full-screen screenshot; the quick and silent capture hotkeys, which work while the overlay is away | built: section 16 |
| `folders` | Folders and canvases | the Overview; a new canvas; switching canvases; a new folder; moving a snippet to another canvas; the trash, and restoring from it | to design (13.8) |
| `profiles` | Profiles | what a profile is for; making one for a program; what it can change | to design |

The first build had Basics, Drawing, Folders and canvases, and Profiles
in the table. After it, the list was reordered and grew by two topics
(2026-09-28):

- **Pinning and view mode is new, and comes second.** The app is mostly
  used to keep a reference in sight over another program, often a game.
  Basics ends with putting the overlay away, and does not say that a
  pinned snippet stays, or that view mode lets clicks through. It is
  also the cheapest topic to build: every step is on the canvas, and
  Basics' `away` step already runs across the overlay going and coming
  back.
- **The selection bar and Properties** are taught inside it, since Pin
  is on the bar, and Properties, the bar's More, sets the opacity. They
  are not a topic of their own.
- **Drawing becomes Drawing and notes** once its later steps are built,
  since the text tool is used in drawing mode, like the pen. Its id
  stays `drawing`, so its progress is kept.
- **Capturing is new.** Its hotkeys are the fastest way to get a
  reference in, and they work while the overlay is away, which the
  other topics never show.
- **The trash moves into Folders and canvases**, which is where the
  Overview shows it.
- **Profiles comes last.** A user needs to know the app before they can
  judge what a profile is for, and its steps happen inside the Settings
  panel. That makes it the most work to guide, with the Overview's
  topic next.
- **The cheat sheet stays a line**, on Basics' end card, where it
  already is.

The list shows the topics in this order. The first three reuse what is
built. Folders and canvases and Profiles need steps inside a panel: the
spotlight shown in it, and anchors marked by its owner (13.8).

**Basics** is section 4's chain without steps 4 to 6 (`drawingMode`,
`draw`, `stopDrawing`), in the same order: `welcome`, `screenshot`,
`move`, `resize`, `delete`, `undo`, `programs`, `antiCheat`, `away`,
`end`. Nothing in it depends on the steps that leave. `delete` needs
drawing mode off, and already has its line for when it is on. The end
card names the other topics.

**Drawing** is:

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when |
|---|---|---|---|---|---|---|---|
| 0 | `drawingMode` | do | **waits** | A snippet can be drawn on. Double-click it, or hold the button down on it, to draw on it. | the subject | in the tutorial folder; canvas uncovered; a subject | drawing mode on the subject |
| 1 | `draw` | do | moves on | as in section 4 | the drawing bar's pen | drawing mode on the subject | one stroke more than when the step began |
| 2 | `stopDrawing` | do | moves on | as in section 4 | - | - | no drawing mode |
| 3 | `end` | read | Done | That's drawing. The list has more topics. | - | - | Done |

A new folder has no snippet, so step 0 starts with its need for a
subject unmet. The need's line says to make one, and its button, Put
one here, places the practice snippet (6.4). This is the case that
button was made for.

The later steps (the eraser, the color, the width, text) are rows added
to this table, as section 8 says, each with its tests.

### 13.3 The list

The list is a state of the card, like the skip card. It shows one row
per topic, with:

- the title, and one line on what the topic covers;
- how many steps it has;
- where the user is with it:
  - *New*: never started;
  - *At step 4 of 10*: the topic running now;
  - *Started*: left partway, or skipped;
  - *Done*: finished at least once.

**Pressing a row** starts that topic at its first step, in a new folder,
whatever its status (question 12). A done topic taken again is the
refresher.

**While a topic runs,** the list marks it as the current one:

- Pressing it goes back to its step.
- Pressing another topic ends the running one the way "Done, keep the
  folder" does, and starts the other (question 13):
  - on its end card, the running topic counts as finished;
  - on its skip card, it counts as skipped;
  - on any other step, its progress stays at that step, which the list
    shows as *Started*.

  Its folder stays either way.

Only one topic runs at a time.

**Leaving the list.** Its bottom button depends on whether a topic
runs:

- **Back**, while one does. It goes back to that topic's step, so
  opening the list from an end card costs nothing.
- **Close**, while none does. It takes the card away.

Opening the list keeps nothing.

### 13.4 Where the list shows, and where a topic starts

- **A first run starts Basics** at its first step, without the list. A
  user who has never seen the app cannot yet judge what "Profiles"
  means.
- **An install from before 0.2.0 starts Basics the same way** (question
  11). Such an install is a library with nothing in
  `tutorial.progress`. The offer card built in phase 3 goes: a user from
  before is treated as a new one.
- **Every topic's end card, and the skip card,** gain a "More topics"
  button that opens the list. The topic stays where it is until another
  is chosen, so Back on the list returns to the card it came from. That
  is how Basics leads on.
- **Settings > Interaction:** "Take the tutorial again" becomes "Open
  the tutorial". It closes the Overview and opens the list.

### 13.5 The folder of a run

- **Each run of a topic has a folder of its own**, named for the topic:
  "Tutorial: Drawing" (question 14). A refresher starts in a new
  folder, a clean place to practice.
- **At the end,** Done puts the folder in the trash, and "Done, keep
  the folder" keeps it, as now (question 9).
- **Runs of one topic** leave folders of one name, found by hand. Done
  puts each in the trash, so they pile up only for someone who keeps
  every run. A number after the name is the fix, if that turns out to
  matter.
- **A topic that makes folders** (Folders and canvases):
  - The folders made during the run count as part of the run's space.
    A step in one of them counts as in the tutorial folder.
  - Done puts them in the trash with the run's folder. Deleting several
    folders under one confirmation is new, and is settled when that
    topic is built.

### 13.6 The warnings on the skip card

The two warnings are steps of Basics. The skip card of **any** topic
shows them until Basics has been finished once, so a user who skips
straight to Drawing on the first day still sees them. Within Basics,
the rule of section 3 stays: warnings already passed in this run are
left out.

### 13.7 What is kept

`tutorial.welcome` is replaced. No release has shipped with it, so no
migration and no version bump are needed. The rows are:

| Key | Field | Default | Rule |
|---|---|---|---|
| `tutorial.progress` | `tutorialProgress` | `{}` | each topic's id, mapped to a step id, `"finished"` or `"skipped"`; a topic never started has no entry |
| `tutorial.current` | `tutorialCurrent` | `""` | the topic running, or empty |
| `tutorial.folder` | `tutorialFolder` | `"0"` | as in section 7.6: the running topic's folder |

The progress is a new kind of row, an object mapping text to text, and
needs a new rule in `core/config/setting.h`. The tutorial writes it as
it goes, as it wrote `tutorial.welcome`.

**At a start:**

- **A first run** starts Basics (13.4).
- **`tutorial.current` names a topic whose progress is a step id.** That
  topic goes on at that step the next time edit mode comes up, as a
  resume does now (7.6). A step id no longer in that topic starts the
  topic again.
- **A library, and `tutorial.progress` empty:** Basics starts (13.4).
- **Anything else:** nothing starts.

### 13.8 How it fits

- **`ui/tutorial/topics.{h,cpp}`** holds the table, `struct Topic {
  id, title, gist, chain }`, and `Topics()`. `welcome_chain.{h,cpp}`
  becomes `chains.{h,cpp}`, holding the Basics and Drawing chains. The
  two share their goals, needs and near misses, which are functions in
  that file. A later topic whose steps share nothing with these can
  have a file of its own.
- **The runner** is unchanged. The card makes one for the chosen
  topic's chain, and holds which topic it is.
- **The card** gains the list state and loses the offer. The Offer
  state, `TutorialButton::NoThanks`, `"offered"` and the
  `tutorial.offer.*` strings go.
- **Actions:** `StartTutorial` names its topic, and `OpenTutorialList`
  is new.
- **The world** needs nothing new for Basics and Drawing. Folders and
  canvases will need:
  - more of the world: the canvases of a folder, the folders made since
    the run began, and the Overview's tab;
  - the Overview's widgets marked on the anchor board as the Overview
    draws them;
  - a need of the opposite kind, "the Overview open";
  - the spotlight shown inside the Overview for such a step.

  Those are argued with that topic.

### 13.9 Tests

Section 9's tests, per topic:

- the chain's shape, for every topic in the table;
- a walk-through by the real gestures, for every topic;
- the derail matrix, for every do step of every topic.

And for the list:

- a first run, and an install from before, start Basics without the
  list;
- "More topics" on an end card and on the skip card opens the list;
- a new, a done and a skipped topic start at their first step, and a
  topic left partway starts at its step, in a new folder;
- choosing another topic while one runs keeps the old one's folder and
  progress;
- Drawing's skip card shows the two warnings until Basics has been
  finished;
- the progress is kept per topic, and the running topic resumes.

### 13.10 Getting there

Phase 4 of section 10 becomes phase 5, and a new phase 4 comes before
it:

4. **Topics:**
   - the topic table, with Basics and Drawing split out of the welcome
     chain;
   - the progress rows, replacing `tutorial.welcome`;
   - the list on the card, and "More topics";
   - Basics started on a first run and for installs from before, with
     the offer gone;
   - "Open the tutorial" in Settings;
   - the tests of 13.9.

The later topics are each a piece of work of their own after phase 5,
in the order of 13.2, each with its table in this document first.
Section 14 is the first.

## 14. Pinning and view mode

Status: **built** (2026-09-28). Its questions and their answers are in
14.7, and the design below matches them. What building it found is in
14.8.

### 14.1 What it teaches, and why

A snippet is mostly there to be looked at while the user is in another
program. The topic teaches the two ways to keep it in sight, and how to
keep it from hiding what is underneath:

- **Pinning.** A pinned snippet stays on screen when the overlay is put
  away. The overlay is then in its pinned view: click-through, showing
  the current canvas's pinned snippets and nothing else
  (`docs/OVERLAY_STATES.md`, section 3).
- **See-through.** Ctrl and the wheel change the selection's picture
  opacity; Shift and the wheel change the opacity of what is drawn on
  it. Properties, the bar's More, has the same as sliders.
- **View mode.** Everything on the canvas stays on screen, and clicks go
  through to the program underneath.
- **Unpinning.** A pinned snippet stays until it is unpinned, even after
  the tutorial, so the topic shows how.

Along the way it shows the selection bar: its Pin, and its More, which
opens Properties. The right-click menu has neither, so the topic does
not send the user there.

### 14.2 The chain

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when | Near misses (6.6) |
|---|---|---|---|---|---|---|---|---|
| 0 | `pin` | do | **waits** | A pinned snippet stays on screen when you put the overlay away, so it can sit over your game. Select it, and press Pin on the bar above it. | the selection bar's Pin, or the subject while its bar is not shown | in the tutorial folder; canvas uncovered; a subject; the subject here and on screen; no drawing mode, whose bar has no Pin; the subject selected | one of the tutorial's snippets pinned | - |
| 1 | `pinnedAway` | do | moves on | Press {editMode} to put the overlay away. The pinned snippet stays over your program, and your clicks go through to it. Press {editMode} again to come back here. With the hotkey unbound, the tray icon, as in Basics' `away`. | - | in the tutorial folder; a subject; the subject here and on screen; the subject pinned | the pinned view has come up since the step began | view mode instead: "That was view mode, which keeps everything on screen; it comes next. Press {editMode} to put the overlay away." |
| 2 | `opacity` | do | moves on | Make it see-through, so it hides less of what's underneath: hold Ctrl and turn the wheel. Shift and the wheel fade what's drawn on it instead, and the wheel alone resizes it. More, on its bar, has the same as sliders. | the subject | in the tutorial folder; canvas uncovered; a subject; the subject here and on screen; the subject selected | a change that shows: the subject's picture opacity changed by 0.10 or more (two notches) from the step's start, or the opacity of what is drawn on it, while something is | resized instead: "That changed its size. Hold Ctrl as you turn the wheel." Shift with nothing drawn: "Shift fades what's drawn on it, and nothing is yet. Hold Ctrl instead." |
| 3 | `viewMode` | do | moves on | View mode keeps everything on this canvas on screen, pinned or not, and your clicks go through to your program. Press {viewMode} to switch to it, and {editMode} to come back here. With {viewMode} unbound: "View mode has no key yet. Set one in Settings > Hotkeys to use it." With {editMode} unbound, the tray icon comes back. | - | - | view mode has come up since the step began | the pinned view instead: "That put the overlay away, and only pinned snippets stayed. Press {viewMode} for view mode." |
| 4 | `unpin` | do | moves on | A pinned snippet stays on screen until you unpin it, even after the tutorial. Press Pin on its bar again. | the selection bar's Pin, or the subject | as `pinnedAway`, and canvas uncovered, no drawing mode and the subject selected | a snippet of the tutorial's seen pinned during the step is not pinned now | - |
| 5 | `end` | read | Done | That's pinning and view mode. Pin what you need while you play, and make it see-through so it doesn't hide the game. The bar's Fullscreen fills the screen with a snippet, and its More has the opacity, the background color and the text. Done puts the folder in the trash; Done, keep the folder keeps it. | - | - | Done | - |

**The subject.** Steps 0 and 2 take any of the tutorial's snippets
(`SubjectRule::Any`), and step 0 is done by any of them pinned, since
the bar pins the whole selection. Steps 1 and 4 prefer a pinned one (14.3), so that
selecting another snippet does not make the step's subject one that
cannot be put away pinned, or unpinned. A new folder has no snippet, so
step 0 starts with its need for a subject unmet, and Put one here
places the practice snippet, as in Drawing.

**Which step waits.** Only `pin`: `pinnedAway` and `unpin` are about a
pinned snippet, and nothing else in the chain makes one. The need "the
subject pinned" has its own line too, for a snippet unpinned since.

**The overlay leaving edit mode is the lesson** in steps 1 and 3, as in
Basics' `away`. The card goes with edit mode (section 3), so the step's
text says, before the user leaves, how to come back. Its goal is
checked when edit mode is back, and the step then shows its check and
moves on.

**Opacity, only what shows** (question 18). A change counts only when
it can be seen:

- **The picture's opacity** always shows: it is a screenshot's image, or
  a drawing's backing in its tint.
- **The opacity of what is drawn** shows only while the snippet has
  strokes or text. The practice snippet has neither, so Shift and the
  wheel on it change a value and nothing on screen. That gets the near
  miss's line instead of a check.

The practice snippet's backing starts at 50%, and a screenshot at the
default in Settings > Defaults, so the text asks for "see-through", and
the goal accepts a change either way. Properties' sliders count too,
since the goal reads the snippet, not the wheel (6.3). The drawing's opacity is the strokes' only: a note's
text has no opacity of its own, and fades with its color
(`Item::noteTextColorRGBA`).

**Left out:** full screen and the order of snippets (Bring forward,
Send backward). Both are understood without a demonstration (question
19); full screen gets a line on the end card.

### 14.3 What it needs that is new

Each of these is one of section 8's changes that fit: rows, a need, a
subject rule, an anchor.

- **The world** (7.1):
  - three facts per snippet: pinned, the picture's opacity and the
    drawing's opacity (`Item::pinned`, `Picture::opacity`,
    `Item::foregroundOpacity`). "While something is drawn" (14.2) is the
    strokes it already counts;
  - two counts beside the showings: how many times the pinned view has
    come up, and how many times view mode has. `TrayController::Apply`
    tells `OverlayApp` when a transition enters a state it was not in,
    beside giving it its mode (step 5 of `docs/OVERLAY_STATES.md`,
    section 6). `SetMode` alone cannot count them: Hidden keeps the last
    mode, so View, then Hidden, then View sets View twice, and a notice
    going down sets View with nothing on screen. The overlay's states
    and their table do not change.
- **The start record** keeps both counts, as it keeps the showings.
- **The look** gains the snippets seen pinned during the step, kept by
  the runner as it keeps the snippets deleted during the step
  (`deletedThisStep`). `unpin` is done when one of those is not pinned
  now. A step that began with nothing pinned asks for a pin first, with
  the need below, and is then done by the unpin that follows.
- **A need, "the subject pinned":** "It isn't pinned. Select it, and
  press Pin on its bar." It has no button: pinning
  the user's snippet for them is not among what the tutorial may change
  (7.3).
- **A subject rule, `Pinned`:** section 6.5's order, with a pinned
  snippet chosen before one that is not at each rung. The hand's
  snippet, if pinned; else the subject there was, if pinned; else the
  newest pinned. With none pinned, the rule falls back to `Any`, so the
  need above can say what is wrong with the one there is.
- **A spot and its anchor, the selection bar's Pin:** marked in
  `canvas_view.cpp` beside the Close button (7.2). While no bar is
  drawn, the spotlight rings the subject instead, as the handle's spot
  does before the handles show.
- **The strings:** `tutorial.topics.pinning.*`, each step's title and
  text, their unbound-key texts, the near misses, the new need's line
  and the end card (`tutorial.pinningEnd.*`, as Drawing's
  `drawingEnd`).
- **A row in the topic table**, second, after Basics.

### 14.4 What does not change

- The runner's states, Next, Back and Skip; the card, the list and the
  settings.
- The input machine: pinning, the wheel and the hotkeys work as they
  do.
- The overlay's states and their table. The tutorial only counts two of
  the transitions, as it counts the showings.
- The skip card's warnings (13.6).

### 14.5 Tests

Section 9's, for the new topic:

- **`chains_test`,** against the fake world: each step's goal, needs
  and near misses; the `Pinned` rule's choices; the chain's shape, which
  `TopicsTest` already checks for every topic. The rule "a need is met
  by an earlier gated step, or has a line" covers the new need, with
  `pin` gated.
- **The walk-through** picks the topic up from the table. Its hands:
  - `pin`, a click on the bar's Pin;
  - `pinnedAway`, the edit hotkey twice, and the pinned view seen in
    between;
  - `opacity`, Ctrl and the wheel;
  - `viewMode`, the view hotkey, then the edit hotkey;
  - `unpin`, the bar's Pin again.
- **The derail matrix,** a row per way:
  - `pin`: minimized, deleted, another canvas, another folder, the
    Overview up;
  - `pinnedAway`: unpinned first; minimized first; view mode instead
    (the near miss);
  - `opacity`: not selected; the wheel alone, and Shift on the
    practice snippet (the near misses, no check);
  - `viewMode`: the pinned view instead (the near miss); the view
    hotkey unbound (the text, and Next);
  - `unpin`: nothing pinned at the step's start; another snippet
    selected (the rule).
- **By hand:** the topic's cards, the pinned view over a program, and
  view mode, with real input.

### 14.6 Getting there

One piece of work: the world's facts and counts, the look's pinned
snippets, the need, the subject rule, the spot with its anchor, the
chain, its strings and its row in the table, with the tests of 14.5 and
the hand check.

### 14.7 Questions for review, and the answers

16. **The title and id:** "Keeping it on screen", `onscreen`. *Answer:*
    "Pinning and view mode", `pinning`: the list names the features as
    the app does (the Pin button, the View mode hotkey), and reads like
    the other titles. See-through is part of making a pinned snippet
    usable, and needs no word in the title.
17. **Unpin as a step,** rather than only a line on the end card. The
    reason for a step: after "Done, keep the folder", a snippet still
    pinned would come up over every program whenever that canvas is
    current. *Answer:* a small step of its own.
18. **Both opacities count** for `opacity`, and the text leads with
    Ctrl (the picture), since the practice snippet has nothing drawn on
    it for Shift to fade. *Answer:* only a change that can be seen
    counts; a check for a change that shows nothing is irritating
    (14.2).
19. **Full screen and the order of snippets are left out** (14.2), full
    screen with a line on the end card. *Answer:* as proposed; both are
    understood without a demonstration.
20. **View mode with no key:** the step says where to set one and moves
    on with Next. It does not wait, since nothing later needs it.
    *Answer:* as proposed, to be judged in the hand check.

### 14.8 Found while building

- **The right-click menu has neither Pin nor Properties.** Both are on
  the selection bar only: Pin, and More, which opens Properties. The
  cards were first written to send the user to the menu; they name the
  bar instead.
- **In drawing mode the bar is the drawing bar,** which has no Pin. So
  `pin` and `unpin` need drawing mode off, with the line every step
  has for it. The derail matrix found it: two clicks on the snippet,
  close together, are a double-click.
- **The card covered the selection bar.** It kept clear of the subject
  and the ring only, and `pinnedAway`, which rings nothing, has a hint
  that names the bar's Pin. The card now keeps clear of the bars too
  (section 3). On a small display (the tests' 1280 by 768), a card of
  two paragraphs, the practice snippet and its bar leave room at
  neither the top nor the bottom, so the card also tries the top
  corners. Moving it to the bottom to clear the bar put it over the
  snippet, and the wheel turned over the card did nothing.
- **Hidden keeps the last mode,** so `SetMode` could not count the
  pinned view and view mode as they are entered: the tray says so as a
  transition enters a state (14.3).
- **Only the strokes fade with the drawing's opacity.** A note's text
  has no opacity of its own, so "while something is drawn" is the
  strokes alone, and the world needs no fact about text.

## 15. Drawing and notes

Status: **built** (2026-09-28). Its questions and their answers are in
15.7, and the design below matches them. What building it found is in
15.8.

### 15.1 What it teaches, and why

Drawing is built with three steps: drawing mode, a stroke, and stopping
(13.2). The topic grows into the rest of the drawing bar, which a user
otherwise finds only by hovering its buttons:

- **The color,** the swatch on the bar, which opens a chooser.
- **The width,** which is the wheel while drawing. Nothing on screen
  says so: there is no width slider anywhere.
- **The pen's shapes:** the pen pressed again draws straight lines, and
  again, rectangles. Its icon shows which. Shift and Ctrl held do the
  same for one stroke.
- **The eraser,** which cuts what it passes over rather than taking
  whole strokes. It too has a second shape, a rectangle, and the right
  button erases with any tool.
- **A note:** Text, then a click on the snippet, and typing. A note is
  one caption per snippet, drawn over its strokes, and editable at any
  time (`Item::noteText`).

Each of these is its own step, and so is each shape and each way of
erasing: they are slightly hidden, and worth knowing (question 23). Each
counts only once it shows on the snippet, as question 18 settled for
opacity: a color or a width counts once something is drawn with it, a
shape once one is drawn, an eraser once something is gone with it, and
a note once it is written.

**Left out:** Clear drawing (the right-click menu), the note's color and
size (Properties, the bar's More). The end card names both. The
modifier keys get a sentence each, in the step of the shape they make.

The title becomes "Drawing and notes". The id stays `drawing`, so
progress is kept, and a run's folder is "Tutorial: Drawing and notes".

### 15.2 The chain

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when | Near misses (6.6) |
|---|---|---|---|---|---|---|---|---|
| 0 | `drawingMode` | do | **waits** | as built | the subject | as built | as built | - |
| 1 | `draw` | do | moves on | Drag across it to draw. | the drawing bar's pen | as built | as built | - |
| 2 | `color` | do | moves on | Press the color on the bar, and pick another. Click anywhere to close the chooser, then draw with it. | the drawing bar's color | drawing mode on the subject; the pen in hand | a stroke on the subject in a color that differs from the pen's color when the step began, more of them than at the start | the color changed, nothing drawn with it yet: "Give it a try and draw something." |
| 3 | `width` | do | moves on | While you draw on a snippet, the wheel sets the pen's width. Turn it a few notches, then draw. | the subject | as `color` | a stroke on the subject 2 px or more wider or thinner than the pen when the step began, more of them than at the start | the width changed, nothing drawn yet: "Give it a try and draw something." Ctrl or Shift held: "That changed its opacity. Turn the wheel without a key held." |
| 4 | `line` | do | moves on | Press the pen on the bar again: its icon turns into a line, and a drag draws a straight one. Holding Shift does the same for one stroke. | the drawing bar's pen | as `color` | a straight line on the subject, more than at the start | a freehand stroke instead: "That was the pen. Press it again on the bar for a line." |
| 5 | `rectangle` | do | moves on | Press the pen once more for rectangles, and drag one. Holding Ctrl does the same for one stroke. A third press brings the pen back. | the drawing bar's pen | as `color` | a rectangle on the subject, more than at the start | a line instead: "That was a line. Press the pen once more for a rectangle." |
| 6 | `erase` | do | moves on | Press the eraser on the bar, and drag over what you drew. It cuts through strokes. | the drawing bar's eraser | drawing mode on the subject; something drawn on the subject | the strokes on the subject 16 px shorter, or more, than at the start | - |
| 7 | `eraseRect` | do | moves on | Press the eraser again: it now erases a rectangle. Drag one across what's left. Holding Ctrl does the same for one drag. | the drawing bar's eraser | as `erase` | 16 px of ink or more gone from the subject during the step while the rectangle eraser was in hand | ink gone with the round eraser: "That was the round eraser. Press the eraser on the bar again for the rectangle." |
| 8 | `eraseRight` | do | moves on | The right button erases with any tool. Press the pen, then hold the right button and drag across what's left. | the drawing bar's pen | as `erase` | 16 px of ink or more gone from the subject during the step while another tool than the eraser was in hand | ink gone with the eraser: "That was the eraser. Press the pen, then drag with the right button." |
| 9 | `note` | do | moves on | Press Text on the bar, click the snippet, and type a note. How to finish shows as the line under it once typing begins. | the drawing bar's Text | drawing mode on the subject | the subject's note is not empty, differs from the start, and is not being typed | the note being typed: "Press Esc or click outside it when you're done." |
| 10 | `stopDrawing` | do | moves on | as built | - | - | as built | - |
| 11 | `end` | read | Done | That's drawing and notes. Double-click any snippet to draw on it. Clear drawing, in its right-click menu, takes every stroke away; More, on its bar, has the note's color and size. Done puts the folder in the trash; Done, keep the folder keeps it. | - | - | Done | - |

**Which step waits.** Only `drawingMode`, as built: every later step
needs drawing mode on the subject, and its line says how to get it back.
The three erasing steps need something drawn, which `draw` makes; the
need has a line too, for a snippet erased bare, or strokes cleared or
undone since.

**Ink gone, by the hand that held it.** `erase` compares the ink with
the step's start, as a goal does. `eraseRect` and `eraseRight` teach the
same result by two other ways, so their goals ask what was in hand when
the ink went. The runner keeps it as it keeps the snippets deleted
during the step: each frame, ink gone from the subject since the last
frame is added to the step's tally for the hand in that frame, the
rectangle eraser, the round one or another tool. The hand is state the
world reads, not a gesture: a rectangle erased is committed on the
release, while the rectangle eraser is still in hand, and a right-drag
erases while the pen stays in hand. What undo takes back with the pen
in hand counts for `eraseRight` too (question 26).

**"More of them than at the start."** A goal about a stroke counts the
subject's strokes that meet the condition, now and at the step's start,
and is done when there are more. So a stroke drawn in a new color in an
earlier step does not finish `color` at once, and undoing the new
stroke takes the step back to waiting, as 6.3 has it for any goal
before it is met.

**Only what shows.** Each goal is a change on the snippet:

- **Color:** a channel 64 or more (of 255) away from the old color. The
  chooser changes the color as it is dragged, so a click next to the old
  color is no new color.
- **Width:** 2 px, two notches of the wheel, like opacity's two notches.
  The width is compared on screen, the stroke's stored width scaled by
  the snippet's size, since a stroke is stored at the snippet's own
  scale.
- **Shape:** a stroke of two points (a line), or of five that close an
  upright rectangle, which is how a shape is stored
  (`session_shapes.cpp`). A freehand stroke flicked in two frames has
  two points too; that counts, and is rare.
- **Erase:** the strokes' total length on screen, 16 px shorter. The
  eraser cuts strokes into pieces, so their count can grow as ink goes.
  Undo of a stroke, and Clear drawing, count for `erase` too: the ink is
  gone, whatever took it (6.3).
- **Note:** the text. The snippet shows it as it is typed, but the step
  waits for the typing to end, and says so while it goes on (question
  25): moved on at the first letter, the next card would talk over the
  typing.

**A shape is the tool's while it is in hand.** Picking another tool
puts the pen and the eraser back to their first shapes
(`DrawingMode::SetTool`), so `line` and `rectangle` come together
before `erase`, and `eraseRect` right after `erase`. The pen steps need
the pen in hand (15.3). `eraseRight` does not: the eraser comes to it
in hand from `eraseRect`, and a drag with it gets the near miss's line,
which a need's line would hide.

**The color is the user's from then on.** The chooser's color is kept
for the next time the app starts. The step does not put it back: the
user picked it.

### 15.3 What it needs that is new

- **The world** (7.1):
  - per snippet: its strokes, each with its color, its width on screen
    and whether it is a line or a rectangle, in place of their count;
    their total length on screen; and its note's text;
  - the pen's color and width, the tool in hand and the eraser's shape
    (`Editor::DrawColorRGBA`, `DrawWidth`, `ActiveTool`,
    `EraserShape`);
  - the snippet whose note is being typed, if one is
    (`Editor::EditingNote`).
- **The start record** keeps the pen's color and width, as it keeps the
  counts.
- **The look** gains the ink gone from the subject during the step, by
  the hand that held it (15.2), kept by the runner beside
  `deletedThisStep` and `pinnedThisStep`.
- **Two needs:**
  - "the pen in hand": "Press the pen on the bar." With the eraser or
    Text in hand, a drag erases or opens the note instead. No button.
    Any of the pen's shapes will do, so `line` and `rectangle` keep it
    met.
  - "something drawn on the subject": "Nothing is drawn on it to erase.
    Draw something first." No button.
- **Three spots, with their anchors:** the drawing bar's color, eraser
  and Text, marked in `canvas_view.cpp` beside the pen (7.2). While the
  bar is not drawn, each falls back to the subject, as Pin does, and so
  does the pen's now. The card keeps clear of all four, as of the other
  bar buttons (section 3).
- **The color chooser and Properties do not cover the canvas** (question
  24). Both are a snippet's own popups, opened from its bar, and today
  the need "canvas uncovered" tells the user to close them: in `color`
  while they pick, and in Pinning's `opacity` while they use the
  sliders the card itself names. The world tells the popups apart, and
  only the menus and the confirmation cover. A press outside either
  still closes it and does nothing else, as ImGui has it, and the
  `color` text says so.
- **The strings:** each step's title and text, the near misses, the two
  needs, the end card; `tutorial.topics.drawing.*` gets the new title
  and gist ("Draw on a snippet: the color, the width, shapes, the
  eraser, and a note.").
- **The settings help** names "drawing and notes".

### 15.4 What does not change

- The runner, the card, the list and the settings rows.
- The input machine and the drawing tools: the steps use them as they
  are.
- Steps 0, 1 and 10, but `draw`'s text, which no longer lists the bar
  since the steps after it go through it.

### 15.5 Tests

- **`chains_test`,** against the fake world: each goal with the strokes
  it counts and those it does not (an old color, 1 px of width, a
  freehand stroke, 8 px erased, an empty note); each near miss; the two
  needs. `TopicsTest` covers the shape.
- **`AppWorld`,** by real gestures: a stroke's color, screen width and
  shape as the world reads them, the ink shorter after an erase, the
  note after typing, and the popups told apart.
- **The walk-through,** with hands for the new steps: the bar's color,
  a click in the chooser and a drag; the wheel and a drag; the pen
  pressed again and a drag, twice; the eraser and a drag across the
  strokes; the eraser again and a drag; the pen and a right-drag; Text,
  a click, typed keys and Esc.
- **The derail matrix,** a row per way:
  - drawing mode left, in every new step;
  - the eraser in hand for `color`, `width`, `line`, `rectangle` and
    `eraseRight`;
  - the chooser up in `color` (no line), and Properties up in `opacity`
    (no line);
  - Ctrl and the wheel in `width` (the near miss);
  - a freehand stroke in `line`, and a line in `rectangle` (the near
    misses);
  - the round eraser in `eraseRect`, and the eraser in `eraseRight`
    (the near misses, no check);
  - the note left typing in `note` (the near miss).

  An erasing step begun with nothing drawn shows its need in
  `chains_test`: in the matrix, a way taken during the step that clears
  the strokes is ink gone, which is the goal.
- **By hand:** every card with real input, and typing a note.

### 15.6 Getting there

One piece of work, as section 14 was: the world's facts, the look's ink
by hand, the two needs, the popups told apart, the three anchors, the
chain, its strings and the new title, with the tests of 15.5 and the
hand check.

### 15.7 Questions for review, and the answers

21. **Five new steps** (`color`, `width`, `shape`, `erase`, `text`,
    which was built as `note`),
    for nine cards in all. The alternative is fewer, longer cards:
    color and width as one "your pen" step, and shapes as a line on the
    end card. *Answer:* small steps, as recommended; question 23 made
    them eight, for twelve cards in all.
22. **A color or a width counts once something is drawn with it,** not
    when the swatch or the size preview changes. *Answer:* drawn with,
    and the line in between says "Give it a try and draw something."
23. **One step for the pen's shapes,** done by a line or a rectangle,
    and the eraser's rectangle and the right button as a sentence in
    `erase`. *Answer:* demonstrate all of them concretely: they are
    slightly hidden, and good to know. So `shape` became `line` and
    `rectangle`, and `eraseRect` and `eraseRight` are new, with the ink
    counted by the hand that held it (15.2).
24. **The color chooser and Properties stop counting as a cover** for
    every step, not only for the steps that use them, rather than a
    list, per step, of the popups it allows. A press outside either
    closes it without doing anything else, so it is never in the way
    for more than a click. It also fixes Pinning's `opacity`, whose
    card names Properties and then asks to close it. *Answer:* as
    recommended.
25. **The note step waits for the typing to end,** with a line while it
    goes on, rather than counting the first letter typed. The note
    reaches the snippet only then. *Answer:* as recommended.
26. **Undo and Clear drawing count for `erase`,** since the goal reads
    the ink, not the eraser (6.3). *Answer:* as recommended.

### 15.8 Found while building

- **The note is on the snippet as it is typed** (`Session::PreviewText`),
  not only once the typing ends, as the design first had it. So the
  goal asks that the typing has ended too, which is what question 25
  settled, and the step is called `note` after its strings.
- **`eraseRight` needs no pen in hand.** The eraser comes to it in hand
  from `eraseRect`, and a need's line is shown before a near miss's: with
  the need, a drag with the eraser would get "Press the pen on the bar",
  and never the line that says why it did not count.
- **Drawing mode outlived a cleared selection.** `Editor::PruneSelection`
  returned early on an empty selection, so Minimize, which clears it,
  left drawing mode on with nothing selected, and the snippet, back from
  the dock, had no bar. The derail matrix found it; fixed in the editor.
  Minimize is not on the drawing bar, so a user could hardly get there.
- **Properties holds the wheel.** With Properties up, the wheel over the
  canvas does nothing, so in `opacity` the sliders stand in for it. The
  card now says nothing while Properties is up, where it asked to close
  it before.

## 16. Capturing

Status: **built** (2026-09-28). Its questions and their answers are in
16.7, and the design below matches them. What building it found is in
16.8.

### 16.1 What it teaches, and why

Basics makes one kind of snippet: a screenshot of a box dragged on the
canvas. There are three more ways in, each with a use of its own:

- **A drawing:** a blank snippet to draw or write on, made by dragging
  with the drawing trigger held (Ctrl by default). It comes ready to
  draw on, in drawing mode with the pen.
- **A screenshot of the whole screen:** a double-click on an empty
  spot, or the button held down on one.
- **The quick capture hotkey:** a screenshot of the whole screen, taken
  from any program with the overlay away, which brings the overlay up
  with it. The fastest way to get a reference in, and the only one that
  does not need the overlay up first.
- **The silent capture hotkey:** the same capture, but the overlay stays
  away, so the program underneath keeps the focus. The capture waits
  for the next time the overlay comes up.

Each hotkey's capture goes onto a new canvas of its own, beside the
current one, and the overlay goes there (`Editor::QuickCapture`). The
card says so, since the snippets made before seem to vanish.

**The order** keeps the canvas steps first. A full-screen screenshot
covers the canvas, so it comes after the drawing, which needs an empty
spot; the hotkeys come last, since each moves the overlay to a new
canvas.

**Left out:**

- **Pasting an image.** Section 13.2 listed it, but the app has no such
  thing: Paste pastes snippets copied or cut inside the app
  (`Editor::PasteFromClipboard`), not an image from another program's
  clipboard. Copy, cut and paste of snippets belong to Folders and
  canvases, as a way to move a snippet (question 27).
- **The creation tools** (S and D by default) and **the right-click
  menu's** New and Fullscreen rows. They are other ways to the same
  results, and the end card names the menu (question 30).
- **A full-screen drawing,** a double-click with the drawing trigger
  held: a blank tint over the whole screen, understood from the two
  steps before it.

### 16.2 The chain

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when | Near misses (6.6) |
|---|---|---|---|---|---|---|---|---|
| 0 | `newDrawing` | do | moves on | A drawing is a blank snippet to draw or write on. Hold {trigger:drawing} and drag a box on an empty spot to make one. | - | in the tutorial folder; canvas uncovered; no drawing mode | a drawing made here since the step began, not full screen | a screenshot instead: "That made a screenshot rather than a drawing. Make one as above." A full-screen drawing: "That took the whole screen - a double-click does that. Press {key:deleteSelection} to delete it, and drag a box instead." |
| 1 | `fullscreen` | do | moves on | Double-click an empty spot, or hold the button down on it, to take a screenshot of the whole screen. | - | in the tutorial folder; canvas uncovered; no creation tool but the screenshot tool | a screenshot made here since the step began, full screen | a drawing of the whole screen, in drawing mode: "That made a drawing of the whole screen. Press Esc to stop drawing on it." Then, out of it: "... Press {key:deleteSelection} to delete it, and take a screenshot as above." A box instead: "That took a part of the screen. Double-click instead of dragging." A drawing: "That made a drawing rather than a screenshot. Take one as above." |
| 2 | `quickCapture` | do | moves on | The capture hotkeys work while the overlay is away, so you can grab your game without opening the overlay first. Press {key:toggleEditMode} to put the overlay away, then press {key:quickCapture}. It takes a screenshot of the whole screen and brings the overlay back with it, on a new canvas of its own. | - | in the tutorial folder | the quick capture has taken a screenshot since the step began | the silent capture instead: "That was the silent capture, which comes next. Press {key:quickCapture} for this one." |
| 3 | `silentCapture` | do | moves on | {key:silentCapture} captures the same way, but the overlay stays away, so your game keeps the focus. Put the overlay away, press {key:silentCapture}, then press {key:toggleEditMode} to come back and see the capture. | - | in the tutorial folder | the silent capture has taken a screenshot since the step began | the quick capture instead: "That was the quick capture, which brings the overlay up. Put it away, and press {key:silentCapture} instead." |
| 4 | `end` | read | Done | That's capturing. Each capture is on a canvas of its own, so they're easy to tell apart: the Overview, in the right-click menu, shows them side by side. A right-click on an empty spot also has every way to make a snippet. Done puts the folder in the trash; Done, keep the folder keeps it. | - | - | Done | - |

**No step waits.** Nothing later needs what a step makes: the hotkeys
make their own canvas, and a step can always be left with Next.

**The text follows the settings,** as Basics' `screenshot` does:

- `newDrawing`, by the drawing trigger: with it plain, "Drag a box on an
  empty spot"; with it off, "Press {key:newDrawingTool}, then drag a
  box", or, with that unbound too, "Right-click an empty spot and choose
  New drawing, then drag a box".
- `fullscreen`, by the screenshot trigger: with a key, "Hold
  {trigger:screenshot} and double-click an empty spot"; with it off,
  "Press {key:newScreenshotTool}, then click anywhere", or "Right-click
  an empty spot and choose Fullscreen screenshot".
- `quickCapture` and `silentCapture`, by the hotkeys: with the edit
  hotkey unbound, the tray icon puts the overlay away and brings it
  back, as in Basics' `away`. With the capture's own hotkey unbound:
  what it does, then "It has no working key yet. Set one in Settings >
  Hotkeys to use it, and go on with Next", as Pinning's `viewMode`.

**The hotkeys count wherever they are pressed** (question 28). Pressed
with the overlay up, a capture leaves the overlay out and is the same
screenshot of what is underneath; the text asks for the overlay away,
since that is the point, but the goal reads the capture (6.3). The
silent capture's check shows once the overlay is back, as the away
steps' do.

**The near misses between the two hotkeys** are the one place where the
result alone cannot tell them apart: both make a new canvas with a
screenshot on it. What differs is whether the overlay came up, so the
world counts each hotkey's captures (16.3).

### 16.3 What it needs that is new

- **The world** (7.1): two counts beside the showings, how many
  captures each hotkey has taken since the app started. The tray asks
  the overlay for a capture as it does today, and says which hotkey
  asked; `OverlayApp` counts it when the snippet is made (not when the
  canvas for it could not be written).
- **The start record** keeps both counts.
- **A hotkey another program holds reads as unbound** (question 29).
  Its combination stays in the settings, but it does nothing, and a
  card that says to press it would wait for a key that never arrives.
  The tray tells the overlay which of its hotkeys registered, and
  `KeyLabel` gives nothing for one that did not. That changes the
  away, pinned-view and view-mode cards as well, for the better.
- **No new need, subject rule, spot or anchor.** The steps are about
  empty canvas and the hotkeys, not a snippet; every need they use
  exists.
- **The strings:** each step's title, texts and near misses, the end
  card (`tutorial.capturingEnd.*`), and `tutorial.topics.capturing.*`
  ("Capturing": "Make a drawing, screenshot the whole screen, and
  capture from your program with a hotkey.").
- **A row in the topic table,** fourth, after Drawing and notes.

### 16.4 What does not change

- The runner, the card, the list and the settings rows.
- The input machine, the capture hotkeys and the overlay's states: the
  tutorial counts captures as it counts the pinned view.
- Where a capture lands: on a new canvas in the current folder, which
  during the topic is the tutorial's, so Done takes the captures with
  it.

### 16.5 Tests

- **`chains_test`,** against the fake world: each goal and near miss,
  the texts for each trigger and each unbound key, the two counts in
  the start record. `TopicsTest` covers the shape.
- **`AppWorld`:** the counts after each hotkey, pressed with the
  overlay up and away; nothing counted when the capture's canvas could
  not be written; `KeyLabel` empty for a hotkey that did not register.
- **The walk-through,** with hands: a drag with the drawing trigger; a
  double-click on an empty spot; the edit hotkey, then the quick
  capture; the edit hotkey, the silent capture, and the edit hotkey
  again.
- **The derail matrix,** a row per way:
  - `newDrawing`: a screenshot framed instead (the near miss); another
    folder; the Overview or the cheat sheet up; put away and back;
  - `fullscreen`: the same, and the drawing tool in hand (the need);
  - `quickCapture`: the silent capture (the near miss); another folder
    (the need and its button); the Overview up; put away and back;
  - `silentCapture`: the quick capture (the near miss); another folder;
    the Overview up.

  Beside the matrix, in `tutorial_app_test`: a snippet made full screen
  by mistake in Basics' `screenshot` and in `newDrawing`, and a drawing
  of the whole screen in `fullscreen`, each taken away as the lines say
  and the step then done; each hotkey pressed in the other's step; both
  pressed with the overlay up. The texts for unbound keys are in
  `chains_test`.
- **By hand:** every card with real input, and both hotkeys over
  another program.

### 16.6 Getting there

One piece of work: the counts, the hotkeys' registration told to the
overlay, the chain, its strings and its row in the table, with the tests
of 16.5 and the hand check.

### 16.7 Questions for review, and the answers

27. **Pasting an image is left out,** since the app has none: Paste is
    of snippets copied inside it. Copy, cut and paste go to Folders and
    canvases, as a way to move a snippet. Pasting an image from another
    program would be a feature of its own, not part of this topic.
    *Answer:* out of scope here. Copy and paste through the Windows
    clipboard goes on the to-do list.
28. **The capture hotkeys count wherever they are pressed,** the overlay
    up or away, though the text asks for it away (16.2).
    *Answer:* as recommended.
29. **A hotkey another program holds reads as unbound** on every card,
    so the card says to set one rather than to press one that does
    nothing (16.3). *Answer:* as recommended.
30. **The creation tools and the right-click menu's rows** get a line
    on the end card, not steps: they reach the same results as the
    steps. *Answer:* as recommended.
31. **Basics' full-screen near miss** says "Drag a box instead", but the
    screenshot it made covers the canvas, and a drag on it does not
    frame. Its line, like `drawing`'s here, should say to delete it
    first. *Answer:* as recommended.

### 16.8 Found while building

- **The first step is `newDrawing`,** not `drawing`: its strings would
  have been `tutorial.drawing.*`, beside the Drawing and notes topic's
  `tutorial.topics.drawing.*` and its steps.
- **A drawing comes in drawing mode, where Delete does nothing** (Basics'
  `delete` has its line for it). So a drawing of the whole screen made by
  mistake takes two lines to clear. In `newDrawing` the first is the need
  for no drawing mode, whose line every step has; in `fullscreen`, which
  has no such need since a double-click leaves drawing mode by itself,
  it is a near miss of its own, "Press Esc to stop drawing on it", and
  the line to delete it follows once drawing mode is off. A drawing made
  in a box needs neither: an empty spot is still there to double-click.
- **The hotkey steps need no uncovered canvas.** Their cards ask for the
  overlay away first, and leaving edit mode closes any panel or popup
  (the All scope).
