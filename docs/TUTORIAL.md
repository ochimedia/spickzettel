# The tutorial

Status: **built** (2026-09-28), for 0.2.0, in the five phases of
section 10. It was proposed on 2026-09-27 and agreed on 2026-09-28. Its
questions and their answers are in section 12, and the design below was
changed to match them. **Topics** (section 13), several chains chosen
from a list, were agreed after phase 3 and built as phase 4. The
sections below describe what is built; section 13 keeps the reasoning
for the topics. **The topics after Basics** are listed in 13.2, each
designed and built in a section of its own, 14 to 18. Sections 19 and
20 changed the card after a hands-on try: where it sits, and its end
buttons.

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
- It changes the app only in the few named ways of section 7.3: the
  folders it runs in, made at the start and put in the trash at the
  end; a practice snippet it places when asked; switching back to its
  folder; and the profiles a topic made, deleted at its end.
- The rest of the app knows about the tutorial in a few named places
  (section 7):
  - the view's owners mark where an anchored widget is drawn, one line
    each, and know nothing else of it;
  - one owner draws the card and the spotlight;
  - `OverlayApp` answers the world (`AppWorld`), counts what nothing
    else counts (showings, pinned views, view modes, captures), and
    applies the tutorial's actions;
  - the tray says what its start found of the library, which hotkey
    took a capture, and when a mode was entered;
  - its rows in the settings, and its button in Settings > Interaction.

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
  - Next;
  - Skip tutorial, a quieter button.

  The last step, the end card, has instead a checkbox that keeps what
  the topic made, over Back, Done and More topics (section 20).

**Where it sits.** At the top center to begin with, clear of what the
step is about: the anchor the ring is on, the subject, and the bars
over the selection, whose buttons a hint may name with no ring on them.
If any of these lies under it, the card tries the bottom center, then
the top corners, and takes the first place that covers none of them, or
else the one that covers least, the anchor and the subject counting
double. Over the Overview, the lower right corner comes before all of
them (17.8). Once placed, it stays, across steps too, until it covers
the ring's anchor or a bar; then it slides to the nearest place that
covers least. The Overview opened or closed places it anew (section 19).
Wherever it goes, all of it stays on screen: the list is tall (18.8).
The user can drag the card by any spot that is not a button, and it
stays where it is left until a topic starts or the list opens or
closes. The list is always at the top center, twice as
wide as a step's card with the topics in two columns where the display
has room, and the card placed as at the start once it closes or a topic
starts from it (19.5).

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
- It goes once the step's goal is met. It goes too while the Overview
  or the cheat sheet covers the canvas, unless it is on the Overview's
  own widgets; those it rings only while the Overview is up and nothing
  is over it. A spot on a bar's button rings the snippet until its bar
  is drawn.

**When a step is done.** A do step whose goal is met shows its check,
and the card moves on by itself about a second later (question 2).

**Next** depends on the step (question 3):

- On a read step, Next moves on.
- On a do step that no later step depends on, Next moves on too, done or
  not.
- On a gated step, Next is grayed out until the goal is met. Its tooltip
  says why: "The next steps use what this one makes."

Skip tutorial is on every step but the end card, whose Done is the way
out, so the tutorial still never traps the user.

**Skip.** It goes to a last card, "Tutorial skipped":

- The card shows the two warnings (Basics' steps 6 and 7, section 4) in
  short form. In Basics it leaves out those the user has already passed
  in this run; from any other topic it shows both, until Basics has been
  finished once (13.6). Someone who skips at once still sees them.
- It offers Back, to the step skipped from, so a misclick costs nothing.
  That is why no confirmation box is needed.
- Its Done ends the topic and puts its folder in the trash, unless
  "Keep the tutorial folder", a checkbox above the buttons, is ticked
  (question 9, section 20).
- Its "More topics" ends the topic as Done does and opens the list, and
  it says the tutorial is also in Settings > Interaction. The end card
  has the same checkbox and buttons: Back, Done and More topics, in one
  row.

**The list.** A state of the card, with a cell per topic, two to a row
where the display has room (19.5): its title, one line on what it
covers, its number of steps, and where the user is with it (New, At
step 4 of 10, Started, Done). A cell starts its topic; the list's
Close takes the card away, or goes back to the topic running where the
list was opened from Settings during one (13.3, section 20).

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
| 1 | `screenshot` | do | **waits** | Drag a box over anything on screen to take a screenshot of it. With a trigger key set: "Hold {trigger} and drag". With the trigger off: "Press {newScreenshot}, then drag", or with that key unbound too, the empty canvas's menu. | - | in the tutorial folder; canvas uncovered; no drawing mode; no tool in hand but the screenshot tool | a screenshot snippet made since the step began, on the current canvas, not fullscreen | a fullscreen one: "That took the whole screen - a double-click does that. Press {key:deleteSelection} to delete it, and drag a box instead." (question 31) A drawing: "That made a drawing rather than a screenshot. Drag a box as above instead." |
| 2 | `move` | do | moves on | Drag it to move it. A press selects it, and a drag takes it along. | the subject | a subject that can move; no drawing mode | the subject moved 16 px or more from where the step found it, its size changed by less than 10%, and not fullscreen | resized instead: "That changed its size. Drag from the middle to move it." |
| 3 | `resize` | do | moves on | Drag a corner to resize it; Shift switches keeping its shape. A right-drag near an edge does it too. | the subject's lower right handle | a subject that can move; no drawing mode; the subject selected | its width or height changed by 10% or more, and not fullscreen | - |
| 4 | `delete` | do | **waits** | Select it and press Delete, or the close button on its bar. | the subject's close button | a subject; no drawing mode | one of the tutorial's snippets, there when the step began, deleted: the subject follows the hand | - |
| 5 | `undo` | do | moves on | Deleted by mistake? {undo} brings it back. It takes back anything you did, a step at a time. | - | in the tutorial folder; canvas uncovered; the subject's canvas; a deleted subject | the subject back on the canvas, after being deleted in this step (or when it began) | - |
| 6 | `programs` | read | moves on | Set it up for your programs. Some games break when the overlay takes focus; others need it to. Look through Settings > Behavior, and make a profile for each program that needs its own. | - | - | Next | - |
| 7 | `antiCheat` | read | moves on | Careful with anti-cheat. Some games watch for tools that draw over them or read their input. If a game might object, quit Spickzettel before you start it. | - | - | Next | - |
| 8 | `away` | do | moves on | Press {editMode} to put the overlay away and go back to your program. Press it again to come back here. With the hotkey unbound, the tray icon instead. | - | - | the overlay has come back since the step began | - |
| 9 | `end` | read | Done | That's the basics. {cheatSheet} shows every key and gesture, and a right-click on anything shows what it can do. More topics has the others. | - | - | Done | - |

The needs of `move`, `resize` and `delete` come after those every step
about a snippet on the canvas has first: the tutorial folder, the canvas
uncovered, a subject, the subject here and on screen (`OnTheCanvas` in
`chains.cpp`).

**The other topics** have their tables in their own sections: Pinning
and view mode in 14.2, Drawing and notes in 15.2, Capturing in 16.2,
Folders and canvases in 17.2 and Profiles in 18.2. Drawing's first
table, of four steps, is in 13.2.

A new folder has no snippet, so a topic whose first step is about one,
such as Drawing and notes, starts with its need for a subject unmet:
its line says to take a screenshot, and its button, Put one here,
places the practice snippet (6.4).

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

- **Off**: no tutorial, and how the last run ended, finished or
  skipped (`Outcome`).
- **On a step**: the step's index, its start record, whether its goal
  has been met, and when. The end card is the chain's last step.
- **Skipped**: the skip card, and the step skipped from, for Back
  (section 3).

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
| Canvas uncovered | a panel or a popup is up | "Close the Overview to go on." (and the same for the cheat sheet); for a popup, "Press Esc to close what is open, and go on." | - |
| No drawing mode | a snippet is in drawing mode | "Click outside the snippet, or press Esc, to stop drawing first." | - |
| No tool in hand | a creation tool is in hand, other than the one the step asks for | "Press Esc to put the tool down first." | - |
| In the tutorial folder | the current canvas is in another folder | "The tutorial is in a folder of its own." | Go back to the tutorial |
| A subject | none of the tutorial's snippets will do, on this canvas or any other of its folder | "Take a screenshot to practice on, or let me put one here." | Put one here |
| The subject here | the subject is on another canvas of the tutorial folder | "It's on the canvas "{canvas}"." | Go there |
| A subject that can move | the only candidate is fullscreen | "It fills the screen. Right-click it and choose Fullscreen to shrink it back." | - |
| The subject on screen | the subject is minimized | "It went to the dock. Click it there to bring it back." The spotlight moves to its chip. | - |
| The subject selected | the subject is not selected | "Click it to select it." | - |
| Drawing mode on the subject | no drawing mode on it | "Double-click it to draw on it again." | - |
| A deleted subject | the subject is on the canvas: brought back before the step began (section 5) | "Delete it again to try this, or go on with Next." | - |
| The subject pinned | the subject is not pinned | "It isn't pinned. Select it, and press Pin on its bar." | - |
| The pen in hand | another tool is in hand | "Press the pen on the bar." | - |
| Something drawn on the subject | the subject has no strokes | "Nothing is drawn on it to erase. Draw something with the pen first." | - |
| The Overview up | it is not up (17.3) | "Open the Overview: right-click an empty spot and choose Overview." | - |
| Its Canvases tab | it is on Settings or About | "Go back to the Canvases tab, at the top left." | - |
| Show deleted on | it is off | "Tick Show deleted, at the top right, to see it." | - |
| Something in the trash | nothing of the tutorial's is deleted | "Nothing of the tutorial's is in the trash. Delete a canvas first, or go on with Next." | - |
| Settings up, its tab, and a section | the Overview is not up, is on another tab, or on another section (18.3) | "Open Settings: right-click an empty spot and choose Settings.", and so on | - |
| A program underneath | the overlay is up over nothing it can name | "The overlay is up over nothing it can name. ..." | - |
| The tutorial's profile, Showing on it, something set in it | none; Showing on another; it states nothing (18.3) | "The tutorial has no profile of its own ...", and so on | - |

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
- On one without, the need's line offers the way back ("Go there"),
  or a practice snippet.
- On a canvas outside the folder, the need "In the tutorial folder" says
  so, and its button switches back to the tutorial's folder, to its
  first canvas not deleted.
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

- Skip is on every step's card but the end card, whose Done ends the
  topic. Next may wait on a gated step; Skip never does.
- The card takes no keys.
- Escape, undo and every hotkey do what they always do while the
  tutorial runs.
- Nothing about the tutorial can keep the user over their program.

### 6.9 Every way, answered

| Way (6.1) | Answer |
|---|---|
| The subject goes | a new subject (6.5); or the need's line with its button (6.4) |
| The canvas changes | the subject is followed; or Go there (6.4, 6.5) |
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
It has no ImGui. `AppWorld` (`ui/view/tutorial_world.{h,cpp}`), a member
of `OverlayApp`, implements it from the editor, the session, the
settings, the Overview and the Settings page, and a test fakes it. It
answers:

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
- **Folders and canvases:** every folder, and every canvas of a folder,
  each with its name and whether it is in the trash (17.3).
- **The Overview and the canvas bar:** whether the Overview shows the
  canvases and what is deleted; whether the canvas bar is on (17.3).
- **Profiles and Settings:** the program the overlay is up over; each
  profile, with what it is matched on, whether it matches that program
  and runs, and how many Behavior settings it states; the Settings tab,
  its section, and whose values Showing shows (18.3).
- **The snippets in a folder** (`SnippetsIn`), each with:
  - its kind (a picture or not);
  - its rectangle;
  - whether it is fullscreen, minimized or deleted;
  - its canvas;
  - its strokes, each with its color, its width and length on screen,
    and whether it is a line or a rectangle (15.3);
  - its note's text;
  - whether it is pinned, and its picture's and strokes' opacity.
- **Words:** the label of the keys that run a command, or of a hotkey,
  as bound now (`KeysFor`, `FormatKeyComboLabel`, as the cheat sheet
  uses them), and none for a hotkey another program holds (16.3); the
  triggers as set.
- **The tutorial's own progress:** what is kept for a topic (7.6), for
  the list and the skip card's warnings.

Answers are values: ids, rectangles, counts. The tutorial never holds a
reference into the model, which is the same rule `docs/VIEW_LAYER.md`
set for actions.

Where things are on screen is not the world's: the runner needs no
pixels. The card reads the anchor board itself (7.2), and works out the
subject's rectangle from the world's answers.

### 7.2 Anchors

`ui/view/anchors.h` holds `AnchorId`, an enum of the places a step may
point at, and a board of where each is this frame. There are two kinds:

- **Marked by owners.** An owner that draws an anchored widget marks it
  as it draws it: `host_.Mark(Anchor{AnchorId::SelectionBarClose}, min, max)`,
  through `ViewHost`. That is one line per anchored widget. The board is
  cleared in Prepare, so an anchor not drawn this frame is not on screen.
  The topics mark:
  - the selection bar's close button and its Pin, and its pen, eraser,
    Text and color;
  - a dock chip, by its snippet;
  - the canvas bar's + and Overview buttons;
  - the Overview's New folder and Show deleted; a folder's row; a
    canvas's tile and the trash button under it; a Restore - by the
    folder's or canvas's id (17.3);
  - Settings' section buttons, Make a profile for this, New profile, a
    profile's trash button, Showing and its entries, the Don't steal
    focus row, and each Behavior row's revert arrow (18.3).
- **Worked out from the model:** the subject, and its handles, from the
  snippet's rectangle and the selection's layout
  (`ui/selection_layout.h`), in the card (`TutorialCard::SpotRect`).

Each owner knows only that there is a board. None of them knows the
tutorial.

### 7.3 What the tutorial may change

Only through view actions (`ui/view_action.h`), done in the Apply stage
like every other:

| Action | Does | Through |
|---|---|---|
| Tutorial next, back, skip, end; More topics, and leaving the list | the runner's own state, and the card's | the runner, the card |
| Switch to a canvas | Go there (6.4) | the existing `action::SwitchCanvas` |
| Go back to the tutorial | the need "In the tutorial folder" (6.4) | `action::BackToTutorial`: the folder's first canvas not deleted, switched to, or the folder made again |
| Make the tutorial folder | a start (7.6); Go back to the tutorial when the folder is gone | `Session::AddFolder` and a canvas in it, switched to, as the Overview's New folder does |
| Practice snippet | Put one here (6.4): a snippet of the drawing kind, with a backing, centered across and a little below the middle | `action::PracticeSnippet`: `Session::CreateItem(prototype, /*undoable=*/false)` |
| Start a topic | a first run, an install from before, a row of the list (7.6) | the Overview closed, the running topic let go of, the tutorial folder, then a runner for the topic's chain |
| Resume a topic | a start after quitting partway (7.6) | its folder, or a new one; then a runner at its step |
| Open the list | the Settings button (7.6); More topics | the Overview closed; the card's own state |
| End with what the topic made | Done or More topics on the end card or the skip card (question 9, section 20) | kept with Keep ticked; else the run's folders under one delete confirmation (`AskToDelete`, `DeleteTarget::alsoFolders`), which puts them in the trash, and the run's profiles deleted (`SettingsPage::RemoveProfiles`) |

The practice snippet is made off the history, the way the first-run
notes were made before the tutorial: undo cannot take it away from under
a step. In every other
way it is an ordinary snippet, and it can be moved, drawn on and deleted
like one.

The tutorial's own progress, and which folder is its own, are
settings, set through `Settings::Set` (7.6).

`OverlayApp` does these actions in `overlay_app_tutorial.cpp`, which
holds the rest of its part in the tutorial too: what the start decided
(7.6), the progress kept, and the counts of 7.1.

### 7.4 The owner, and where it sits

`ui/view/tutorial_card.{h,cpp}` holds one owner in the sense of
`docs/VIEW_LAYER.md`, section 7. It draws two surfaces:

- **the card**, a window named `##tutorial_card`;
- **the spotlight**, an overlay.

It holds the runner, which topic it runs, whether the list is up,
whether a topic has run, the Keep checkbox, and where the card sits or
was left (section 19), and records actions.

**New rows in the surface table** (`docs/VIEW_LAYER.md`, section 3), for
edit mode:

| # | Surface | Kind | Up while | Owner |
|---|---|---|---|---|
| between 10 and 11 | The tutorial card | window `##tutorial_card` | a topic runs, or the list is up | Tutorial card |
| between 13 and 14 | The spotlight | overlay | the step points at an anchor on screen, and its goal is not met | Tutorial card |

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
    bool keepsFolders;                         // folders made while up are the run's (17.3)
    bool keepsProfiles;                        // profiles made while up are the run's (18.3)
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
  of first, keeping its folder (question 13).

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

**The topics** are the table in section 13.2, each designed in this
document before it was built: Pinning and view mode (section 14),
Drawing and notes (15), Capturing (16), Folders and canvases (17) and
Profiles (18). A next topic is designed here the same way.

The design carried what these needed:

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
- **The chains' shape,** for every topic in the table. The needs no
  line can get back (a deleted subject, drawing mode on the subject, the
  subject pinned) are each the goal of an earlier gated step of their
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
  - Done puts it in the trash, and with Keep ticked keeps it.
- **The start:**
  - a first run places no notes and starts Basics;
  - an install from before starts Basics too;
  - the topic running when the app quit is resumed at its step, and no
    other;
  - a library whose tutorial is over starts nothing.
- **Progress:** kept per topic as it goes; another topic started keeps
  the running one's folder, and what it ended as.
- **The list:** More topics on the end card and the skip card, and the
  Settings button, open it; More topics ends the topic as Done does,
  and Close goes back to a topic still running (section 20); a row
  starts its topic at its first step; each topic's status; Drawing's
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
why. The topics it lists are all built, each in a section of its own,
14 to 18.

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
| `folders` | Folders and canvases | the Overview; a new canvas; switching canvases; a new folder; moving a snippet to another canvas; the trash, and restoring from it | built: section 17 |
| `profiles` | Profiles | what a profile is for; making one for a program; what it can change | built: section 18 |

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
| 0 | `drawingMode` | do | **waits** | A snippet can be drawn on. Double-click it, or hold the button down on it, to draw on it. The pen on its bar does the same once it is selected. | the subject | in the tutorial folder; canvas uncovered; a subject | drawing mode on the subject |
| 1 | `draw` | do | moves on | as in section 4 | the selection bar's pen | drawing mode on the subject | one stroke more than when the step began |
| 2 | `stopDrawing` | do | moves on | as in section 4 | - | - | no drawing mode |
| 3 | `end` | read | Done | That's drawing. The list has more topics. | - | - | Done |

A new folder has no snippet, so step 0 starts with its need for a
subject unmet. The need's line says to make one, and its button, Put
one here, places the practice snippet (6.4). This is the case that
button was made for.

The later steps (the eraser, the color, the width, text) were rows
added to this table, as section 8 says; section 15 built them, and 15.2
has the chain as it is.

### 13.3 The list

The list is a state of the card, like the skip card. It shows one row
per topic, with (two topics to a row where the display has room, 19.5):

- the title, and one line on what the topic covers;
- how many steps it has, beside where the user is with it (18.8);
- where the user is with it:
  - *New*: never started;
  - *At step 4 of 10*: the topic running now;
  - *Started*: left partway, or skipped;
  - *Done*: finished at least once.

**Pressing a row** starts that topic at its first step, in a new folder
where it has one (18.3), whatever its status (question 12). A done topic taken again is the
refresher.

**While a topic runs,** the list marks it as the current one:

- Pressing it goes back to its step.
- Pressing another topic ends the running one, keeping its folder, and
  starts the other (question 13). Since section 20 this is only the
  list opened from Settings: the end and skip cards' More topics ends
  the topic itself.
  - on its end card, the running topic counts as finished;
  - on its skip card, it counts as skipped;
  - on any other step, its progress stays at that step, which the list
    shows as *Started*.

  Its folder stays either way.

Only one topic runs at a time.

**Leaving the list.** Its one button is Close (section 20). With no
topic running, it takes the card away; opened from Settings during a
topic, it goes back to that topic's step. (Built first with a Back
while a topic ran, for the end card's More topics, which then kept the
topic running.)

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
  button that opens the list. That is how Basics leads on. (Built first
  with the topic kept until another is chosen, and a Back on the list;
  section 20 ends the topic at the press instead.)
- **Settings > Interaction:** "Take the tutorial again" becomes "Open
  the tutorial". It closes the Overview and opens the list.

### 13.5 The folder of a run

- **Each run of a topic has a folder of its own**, named for the topic:
  "Tutorial: Drawing" (question 14). A refresher starts in a new
  folder, a clean place to practice.
- **At the end,** Done and More topics put the folder in the trash,
  unless the card's "Keep the tutorial folder" is ticked (question 9,
  section 20).
- **Runs of one topic** leave folders of one name, found by hand. Done
  puts each in the trash, so they pile up only for someone who keeps
  every run. A number after the name is the fix, if that turns out to
  matter.
- **A topic that makes folders** (Folders and canvases):
  - The folders made during the run count as part of the run's space.
    A step in one of them counts as in the tutorial folder.
  - Done puts them in the trash with the run's folder. Deleting several
    folders under one confirmation is new.

  Section 17.3 settles which folders these are: those made while the
  step that asks for one is up.
- **A topic with nothing on a canvas** (Profiles) has no folder
  (18.3).

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

  Section 17 argues them.

  As built, `Topic` also has `folder`, false for a topic with nothing on
  a canvas (18.3), and `chains.cpp` holds all six chains.

### 13.9 Tests

Section 9's tests, per topic:

- the chain's shape, for every topic in the table;
- a walk-through by the real gestures, for every topic;
- the derail matrix, for every do step of every topic.

And for the list:

- a first run, and an install from before, start Basics without the
  list;
- "More topics" on an end card and on the skip card opens the list;
- a new, a done, a skipped and a topic left partway start at their
  first step, in a new folder;
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
| 0 | `pin` | do | **waits** | A pinned snippet stays on screen when you put the overlay away, so it can sit over your game. Select it, and press Pin on the bar above it. | the selection bar's Pin, or the subject while its bar is not shown | in the tutorial folder; canvas uncovered; a subject; the subject here and on screen; the subject selected | one of the tutorial's snippets pinned | - |
| 1 | `pinnedAway` | do | moves on | Press {editMode} to put the overlay away. The pinned snippet stays over your program, and your clicks go through to it. Press {editMode} again to come back here. With the hotkey unbound, the tray icon, as in Basics' `away`. | - | in the tutorial folder; a subject; the subject here and on screen; the subject pinned | the pinned view has come up since the step began | view mode instead: "That was view mode, which keeps everything on screen - it comes next. Put the overlay away as above, and only the pinned snippet stays." |
| 2 | `opacity` | do | moves on | Make it see-through, so it hides less of what's underneath: hold Ctrl and turn the wheel. Shift and the wheel fade what's drawn on it instead, and the wheel alone resizes it. More, on its bar, has the same as sliders. | the subject | in the tutorial folder; canvas uncovered; a subject; the subject here and on screen; the subject selected | a change that shows: the subject's picture opacity changed by 0.10 or more (two notches) from the step's start, or the opacity of what is drawn on it, while something is | resized instead: "That changed its size. Hold Ctrl as you turn the wheel." Shift with nothing drawn: "Shift fades what's drawn on it, and nothing is yet. Hold Ctrl instead." |
| 3 | `viewMode` | do | moves on | View mode keeps everything on this canvas on screen, pinned or not, and your clicks go through to your program. Press {viewMode} to switch to it, and {editMode} to come back here. With {viewMode} unbound: "It has no key yet. Set one in Settings > Hotkeys to use it, and go on with Next." With {editMode} unbound, the tray icon comes back. | - | - | view mode has come up since the step began | put away and back instead, while {viewMode} is bound: "That put the overlay away, and only what's pinned stayed. Press {viewMode} for view mode." |
| 4 | `unpin` | do | moves on | A pinned snippet stays on screen until you unpin it, even after the tutorial. Press Pin on its bar again. | the selection bar's Pin, or the subject | as `pinnedAway`, and canvas uncovered and the subject selected | a snippet of the tutorial's seen pinned during the step is not pinned now | - |
| 5 | `end` | read | Done | That's pinning and view mode. Pin what you need while you play, and make it see-through so it doesn't hide the game. View mode keeps the whole canvas in sight. The bar's Fullscreen fills the screen with a snippet, and its More has the opacity, the background color and the text. | - | - | Done | - |

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
  close together, are a double-click. (Since 2026-09-30 there is one
  bar, with Pin in drawing mode too, and the need is gone - see 15.8.)
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
(13.2). The topic grows into the rest of the bar's drawing tools, which a
user otherwise finds only by hovering its buttons:

- **The color,** the swatch on the bar, which opens a chooser.
- **The width,** which is the wheel while drawing. Nothing on screen
  says so: there is no width slider anywhere.
- **The pen's shapes:** the pen's menu, a right click or a hold on it,
  has straight lines and rectangles. Its icon shows which. Shift and
  Ctrl held do the same for one stroke.
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
| 1 | `draw` | do | moves on | Drag across it to draw. | the selection bar's pen | as built | as built | - |
| 2 | `color` | do | moves on | Press the color on the bar, and pick another. Click anywhere to close the chooser, then draw with it. | the selection bar's color | drawing mode on the subject; the pen in hand | a stroke on the subject in a color that differs from the pen's color when the step began, more of them than at the start | the color changed, nothing drawn with it yet: "Give it a try and draw something." |
| 3 | `width` | do | moves on | While you draw on a snippet, the wheel sets the pen's width. Turn it a few notches, then draw. | the subject | as `color` | a stroke on the subject 2 px or more wider or thinner than the pen when the step began, more of them than at the start | the width changed, nothing drawn yet: "Give it a try and draw something." Ctrl or Shift held: "That changed its opacity. Turn the wheel without a key held." |
| 4 | `line` | do | moves on | Right-click the pen on the bar, or hold the button down on it, and choose Line: its icon turns into a line, and a drag draws a straight one. Holding Shift does the same for one stroke. | the selection bar's pen | as `color` | a straight line on the subject, more than at the start | a freehand stroke instead: "That was the pen. Right-click it on the bar and choose Line." |
| 5 | `rectangle` | do | moves on | The same menu has Rectangle: choose it, and drag one. Holding Ctrl does the same for one stroke. Pen, in the menu, brings the pen back. | the selection bar's pen | as `color` | a rectangle on the subject, more than at the start | a line instead: "That was a line. Right-click the pen on the bar and choose Rectangle." |
| 6 | `erase` | do | moves on | Press the eraser on the bar, and drag over what you drew. It cuts through strokes. | the selection bar's eraser | drawing mode on the subject; something drawn on the subject | the strokes on the subject 16 px shorter, or more, than at the start | - |
| 7 | `eraseRect` | do | moves on | Right-click the eraser, or hold the button down on it, and choose Rectangle eraser. Drag one across what's left. Holding Ctrl does the same for one drag. | the selection bar's eraser | as `erase` | 16 px of ink or more gone from the subject during the step while the rectangle eraser was in hand | ink gone with the round eraser: "That was the round eraser. Right-click it on the bar and choose Rectangle eraser." |
| 8 | `eraseRight` | do | moves on | The right button erases with any tool. Press the pen, then hold the right button and drag across what's left. | the selection bar's pen | as `erase` | 16 px of ink or more gone from the subject during the step while another tool than the eraser was in hand | ink gone with the eraser: "That was the eraser. Press the pen, then drag with the right button." |
| 9 | `note` | do | moves on | Press Text on the bar, click the snippet, and type a note. How to finish shows as the line under it once typing begins. | the selection bar's Text | drawing mode on the subject | the subject's note is not empty, differs from the start, and is not being typed | the note being typed: "Press Esc or click outside it when you're done." |
| 10 | `stopDrawing` | do | moves on | as built, and: The tool lit on the bar, pressed again, does it too. | - | - | as built | - |
| 11 | `end` | read | Done | That's drawing and notes. Double-click any snippet, or press the pen on its bar, to draw on it. Clear drawing, in its right-click menu, takes every stroke away; More, on its bar, has the note's color and size. | - | - | Done | - |

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
    Draw something with the pen first." No button.
- **Three spots, with their anchors:** the selection bar's color, eraser
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
  a click in the chooser and a drag; the wheel and a drag; a shape
  picked from the pen's menu and a drag, twice; the eraser and a drag across the
  strokes; the eraser again and a drag; the pen and a right-drag; Text,
  a click, typed keys and Esc.
- **The derail matrix,** a row per way:
  - drawing mode left, in every new step;
  - the eraser in hand for `color`, `width`, `line` and `rectangle`;
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
  Minimize was not on the drawing bar then, so a user could hardly get
  there.
- **Properties holds the wheel.** With Properties up, the wheel over the
  canvas does nothing, so in `opacity` the sliders stand in for it. The
  card now says nothing while Properties is up, where it asked to close
  it before.
- **One bar, since 2026-09-30** (`docs/INTERACTIONS.md`, section 6.5).
  The drawing tools are on the selection bar at rest too, and the lit
  tool pressed again leaves drawing mode, where it used to cycle the
  tool's shapes. So `line`, `rectangle` and `eraseRect` teach the shape
  menu, a right click or a hold on the button, and their near misses
  say so; `drawingMode`, `stopDrawing` and the end card name the bar as
  a way in and out. Pinning's `pin` and `unpin` no longer need drawing
  mode off: Pin is on the bar in it. The walk-through picks a shape from
  the menu, and lets the card come to rest first, as a hand waits for
  it: a line just shown under the card can send it sliding across the
  bar.

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
| 2 | `quickCapture` | do | moves on | The capture hotkeys work while the overlay is away, so you can grab your game without opening the overlay first. Press {key:toggleEditMode} to put the overlay away, then press {key:quickCapture}. It takes a screenshot of the whole screen and brings the overlay back with it, on a new canvas of its own. | - | in the tutorial folder | the quick capture has taken a screenshot since the step began | the silent capture instead, while {key:quickCapture} is bound: "That was the background capture, which comes next. Press {key:quickCapture} for this one." |
| 3 | `silentCapture` | do | moves on | {key:silentCapture} captures the same way, but the overlay stays away, so your game keeps the focus. Put the overlay away, press {key:silentCapture}, then press {key:toggleEditMode} to come back and see the capture. | - | in the tutorial folder | the silent capture has taken a screenshot since the step began | the quick capture instead, while {key:silentCapture} is bound: "That was the quick capture, which brings the overlay up. Put it away, and press {key:silentCapture} instead." |
| 4 | `end` | read | Done | That's capturing. Each capture is on a canvas of its own, so they're easy to tell apart: the Overview, in the right-click menu, shows them side by side. A right-click on an empty spot also lists every way to make a snippet, with the keys beside them. | - | - | Done | - |

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
  canvas for it could not be written), which `Editor::QuickCapture` now
  returns.
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
- **The hotkey steps need no uncovered canvas.** A capture hotkey works
  whatever is up, and the goal reads the capture. (First written as
  "leaving edit mode closes any panel": only view mode and the pinned
  view do, by the All scope. Put away with nothing pinned, the overlay
  is hidden, which keeps the Overview up - 17.8.)

## 17. Folders and canvases

Status: **built** (2026-09-28). Its questions and their answers are in
17.7, and the design below matches them. What building it found is in
17.8.

### 17.1 What it teaches, and why

The other topics stay on one canvas. This one is about where snippets
live, and how to find them again:

- **Canvases.** A canvas is a page of snippets, and a folder holds
  canvases. The canvas bar shows the current folder's canvases, with +
  for a new one and a button for the Overview. It stays out of sight
  until the pointer reaches the bottom edge of the screen, which is why
  it gets a step.
- **Switching.** A tile on the bar, or Alt and the wheel anywhere on the
  canvas, which steps through the folder's canvases. Alt and the wheel
  is on no button and in no menu; only the cheat sheet has it.
- **Moving a snippet.** Cut it, then paste it on the other canvas: the
  snippet itself moves, not a copy (`Editor::PasteFromClipboard`,
  question 27).
- **The Overview.** Every folder and canvas: the folders listed on the
  left, and the canvases of the folder picked there as tiles. A folder's
  row only shows its canvases. A tile goes to its canvas and closes the
  Overview.
- **Folders.** New folder makes one, with a canvas in it, and goes
  there.
- **Two gestures the Overview shows no sign of:** a double-click on a
  name renames the folder or canvas, and a tile dragged onto a folder's
  row moves the canvas into that folder.
- **The trash.** A deleted canvas or folder is only marked. Show deleted
  shows what is marked, in red, where it was, and Restore brings it back
  with everything on it. A deleted snippet is not in the trash: it comes
  back by undo, as Basics shows.

**The order** goes from the canvas outward. The bar and the canvases
come first, since they need nothing open. The Overview and its folders
follow. The trash comes last, since its steps leave Show deleted on and
the rest of the grid dimmed.

**Left out:**

- **Move to canvas and Move to new canvas,** in a snippet's right-click
  menu: other ways to the move the step teaches. The end card names
  them (question 33).
- **Copy and Duplicate:** understood without a step.
- **Reordering,** by dragging a tile between tiles or a row between
  rows: plain once a tile has been dragged onto a folder.
- **Delete permanently and the retention period.** The tooltip on
  anything deleted says when it goes for good, and Settings > Behavior
  sets how long that is.
- **Deleting a canvas from the canvas bar,** by a right-click on its
  tile: the end card names it.
- **The rest of the Overview:** the Vector and Bitmap previews, and the
  Settings and About tabs.

### 17.2 The chain

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when | Near misses (6.6) |
|---|---|---|---|---|---|---|---|---|
| 0 | `newCanvas` | do | **waits** | A folder holds canvases, and each canvas its own snippets. Move the pointer to the bottom edge of the screen: the canvas bar comes out with this folder's canvases. Press + on it for a new canvas. | the bar's + | in the tutorial folder | the current canvas is one of the tutorial's that was not there when the step began | - |
| 1 | `moveSnippet` | do | moves on | To take a snippet to another canvas, select it and press {key:cut}. Go to the other canvas - hold Alt and turn the wheel, or click its tile on the bar - and press {key:paste} there. | the subject | in the tutorial folder; canvas uncovered; a subject | a snippet of the tutorial's is on another canvas than when the step first saw it | a copy pasted: "That pasted a copy - the first one is still on the other canvas. Cut it to move it instead." |
| 2 | `overview` | do | moves on | The Overview has all your folders and canvases. Open it with the grid button at the right end of the canvas bar. | the bar's Overview button | in the tutorial folder | the Overview up | - |
| 3 | `newFolder` | do | **waits** | Folders keep canvases apart - one for each game, say. Press New folder, at the bottom left. It comes with a canvas, and takes you there. | New folder | in the tutorial folder; the Overview up; its Canvases tab | a folder that was not there when the step began | New canvas pressed instead: "That made a canvas. New folder is the button to its left." |
| 4 | `rename` | do | moves on | It's named for the time it was made. Double-click the name, type a better one, and press Enter. A canvas is renamed the same way, by the name under its tile. | the new folder's row | the Overview up; its Canvases tab | a folder or canvas of the tutorial's has another name than when the step began | - |
| 5 | `switchFolder` | do | moves on | A folder's row shows its canvases, and a tile takes you there. Click the tutorial's folder in the list, then one of its canvases. | the tutorial folder's row | the Overview up; its Canvases tab | the current canvas is another than when the step began, in the tutorial's own folder | - |
| 6 | `moveCanvas` | do | moves on | To move a canvas to another folder, drag its tile onto the folder in the list. Drag one of these onto your folder. | the new folder's row | in the tutorial folder; the Overview up; its Canvases tab | a canvas of the tutorial's is in another folder than when the step began | - |
| 7 | `deleteCanvas` | do | moves on | The trash button under a tile deletes that canvas, with its snippets. Delete the one your snippet is on. | that tile's trash button | the Overview up; its Canvases tab | a canvas or folder of the tutorial's deleted since the step began | - |
| 8 | `showDeleted` | do | moves on | Nothing deleted is gone yet. Tick Show deleted, at the top right: what's in the trash shows in red, where it was. | Show deleted | the Overview up; its Canvases tab | Show deleted on | - |
| 9 | `restore` | do | moves on | Press Restore, the arrow under its tile: the canvas comes back, with its snippet. | that Restore | the Overview up; its Canvases tab; Show deleted on; something in the trash | a canvas or folder of the tutorial's that was in the trash during the step is back | - |
| 10 | `end` | read | Done | That's folders and canvases. A snippet's right-click menu has two more ways to move it: Move to canvas, which picks one in the Overview, and Move to new canvas. A right-click on a tile of the canvas bar deletes that canvas. | - | - | Done | - |

**"The tutorial's"** means the run's folders (17.3): the tutorial's own
folder and the one made in `newFolder`. "In the tutorial folder" is met
in either, so the steps after `newFolder` go on in the new folder as
well. `switchFolder` is the one step that asks for the tutorial's own
folder.

**Which steps wait** (question 37). `newCanvas` does, since `moveSnippet`
needs another canvas to move to, and `newFolder` does, since `rename`
and `moveCanvas` are about the folder it makes. `deleteCanvas` does not:
`restore` has its need for something in the trash, with a line, as
Basics' `undo` has for a deleted subject.

**What each goal reads.** Each is a change between the step's start and
now, as in 6.3, and none looks at what was pressed:

- **A canvas made:** the quick capture, the Overview's New canvas and a
  key bound to New canvas count for `newCanvas` too.
- **A snippet moved:** a move keeps the snippet's id (`Session::Paste`,
  `Session::SendItemsTo`), so the goal asks whether a snippet the step
  has seen is now on another canvas. Move to canvas and Move to new
  canvas count.
- **A copy:** a copy pasted onto another canvas keeps its place exactly
  (`Editor::PasteFromClipboard`). So a snippet made during the step, of
  the kind and in the place of one of the tutorial's on another canvas,
  is a copy, and gets the near miss.
- **A name:** any folder or canvas of the tutorial's, renamed to
  something else. The same name typed again is no change.
- **A switch:** a canvas of the tutorial's own folder other than the one
  the step began on. From the new folder, any of its tiles does it.
- **Deleted and restored:** any canvas or folder of the tutorial's,
  including the tutorial's own folder. Deleted, that one takes the
  current canvas with it, to another folder
  (`CanvasManager::SettleOffDeleted`). So `deleteCanvas`, `showDeleted`
  and `restore` do not need the tutorial folder, and `restore` then
  brings it back.

**Deleted for good by mistake.** Restore's neighbor is Delete
permanently, and it asks first. If the canvas goes for good anyway, the
need for something in the trash has its line: "Nothing of the tutorial's
is in the trash. Delete a canvas first, or go on with Next."

**The text follows the settings:**

- **With the canvas bar off** (Settings > Appearance): `newCanvas` says
  "Press {key:newCanvas} for a new canvas", or, with that unbound,
  "Right-click an empty spot, choose Overview, and press New canvas at
  the bottom". `moveSnippet` names only Alt and the wheel, and
  `overview` the right-click menu. The goal of `newCanvas` is met in the
  Overview too, which is why it does not need the canvas uncovered.
- **With Cut or Paste unbound:** "Right-click it and choose Cut. Go to
  the other canvas - ... - then right-click an empty spot and choose
  Paste."

### 17.3 What it needs that is new

- **The run's folders** (13.5, settled here; question 36). They are the
  tutorial's own folder and those made while `newFolder` is up:
  - They are the tutorial's space. "In the tutorial folder" is met in
    any of them, and the subject and the snippets are looked for in all
    of them.
  - A flag on the step, `keepsFolders`, marks the step whose folders are
    the run's. The runner notes each folder that appears while that step
    is up.
  - Only there. A folder made at another time, perhaps for the user's
    own work while a topic is left partway, stays the user's.
  - They are not kept across a restart. After one, a folder made before
    it is the user's own.
- **Done with several folders.** The confirmation names each ("Delete
  the folders "Tutorial: Folders and canvases" and "Games"? This also
  deletes every canvas inside them."), and its Delete puts them all in
  the trash. `DeleteTarget` carries the other folders. "Keep the
  tutorial folders", ticked, keeps them all (section 20). With the confirmation turned off in Settings, they go
  without asking, as one folder does now.
- **The world** (7.1):
  - every folder, with its name and whether it is deleted; every canvas
    of a folder, with its name and whether it is deleted, deleted ones
    included (`FolderFacts`, `CanvasFacts`);
  - the Overview: whether it is up, which tab it is on, whether Show
    deleted is on;
  - whether the canvas bar is on.
- **The start record** keeps the current canvas, and the folders and the
  run's canvases as they were. The runner keeps what of the run's was
  seen in the trash during the step, beside `deletedThisStep`.
- **Four needs:**

  | Need | Not met when | The card says | Button |
  |---|---|---|---|
  | The Overview up | it is not up | "Open the Overview: right-click an empty spot and choose Overview." | - |
  | Its Canvases tab | it is on Settings or About | "Go back to the Canvases tab, at the top left." | - |
  | Show deleted on | it is off | "Tick Show deleted, at the top right, to see it." | - |
  | Something in the trash | nothing of the run's is deleted, and nothing was during the step | "Nothing of the tutorial's is in the trash. Delete a canvas first, or go on with Next." | - |

  The first is the opposite of "canvas uncovered", as 13.8 foresaw. It
  has no button: opening the Overview is what the topic teaches.
- **Anchors, marked by their owners** (7.2):
  - the canvas bar's + and its Overview button (`canvas_bar.cpp`);
  - the Overview's New folder, Show deleted, a folder's row, a canvas's
    tile, the trash button under a tile, and a Restore
    (`overview_panel.cpp`). The last four are marked by their folder's
    or canvas's id, so `Anchor::item` becomes `Anchor::of` (17.8).
- **Spots:**
  - `CanvasBarNew`, `CanvasBarOverview`, `NewFolder`, `ShowDeleted`;
  - `MadeFolder`: the row of the folder made in `newFolder`;
  - `TutorialFolder`: the row of the tutorial's own folder;
  - `DeleteCanvas`: the trash button under a tile of the tutorial's,
    one with a snippet on it first;
  - `Restore`: the Restore of what of the tutorial's is in the trash:
    under its tile, or on its folder's row while the grid shows another
    folder.
- **The spotlight inside the Overview.** Today it is drawn through no
  panel (7.4). It keeps that rule for the canvas's own spots, and draws
  the Overview's while the Overview is up. Their anchors are only marked
  then anyway.
- **The card over the Overview** takes the lower right corner first,
  and keeps clear of the ring as it does on the canvas (question 38,
  17.8).
- **The strings:** each step's title, texts and near misses, the four
  needs, the end card (`tutorial.foldersEnd.*`), the confirmation for
  several folders, and `tutorial.topics.folders.*` ("Folders and
  canvases": "Make and switch canvases, move a snippet, sort canvases
  into folders, and restore from the trash.").
- **A row in the topic table,** fifth, after Capturing.

### 17.4 What does not change

- The runner's loop, the card, the list and the settings rows.
- The input machine, the Overview and the canvas bar: the steps use them
  as they are, and their owners only mark anchors.
- The session and the store. Done's folders are deleted with the call
  one folder uses.

### 17.5 Tests

- **`chains_test`,** against the fake world:
  - each goal and near miss, and the four needs;
  - the run's folders: a folder made while `newFolder` is up is the
    tutorial's, and one made during another step is not;
  - the texts for the canvas bar off and for Cut or Paste unbound.

  `TopicsTest` covers the shape.
- **`AppWorld`:** the folders and canvases, with their names and deleted
  marks; the Overview's tab and Show deleted; each new anchor marked
  where its widget is drawn.
- **Done with a folder made:** one confirmation naming both; its Delete
  puts both in the trash; with Keep ticked, both are kept; with the
  confirmation off, both go.
- **The walk-through,** with hands:
  - the pointer at the bottom edge, and a press on +;
  - a screenshot, then Ctrl+X, Alt and the wheel, and Ctrl+V;
  - the bar's grid button;
  - New folder;
  - a double-click on its row, a name typed, and Enter;
  - the tutorial folder's row, then a tile;
  - the Overview from the right-click menu, and a tile dragged onto the
    new folder's row;
  - the trash button under a tile, and the confirmation's Delete;
  - Show deleted, and Restore;
  - Done, and the confirmation.
- **The derail matrix,** a row per way:
  - `newCanvas`: another folder; put away and back;
  - `moveSnippet`: the Overview or the cheat sheet up; another folder;
    put away and back; a copy pasted (the near miss);
  - `overview`: another folder; put away and back;
  - each step from `newFolder` to `restore`: the Overview closed with
    Esc (the need for it up); put away and back (nothing: hidden keeps
    it up); the Settings tab (its need); another folder picked in the
    list (nothing);
  - `newFolder`: New canvas pressed (the near miss);
  - `restore`: Show deleted off (its need).
- **By hand:** every card with real input, and where the card lands
  over the Overview.

### 17.6 Getting there

Two pieces of work:

1. **What the chain stands on:** the run's folders, and Done with
   several; the world's folders, canvases and Overview; the four needs;
   the anchors and spots; the spotlight inside the Overview. Each with
   its tests, before any step uses it.
2. **The chain:** its strings and its row, with the walk-through, the
   matrix and the hand check.

### 17.7 Questions for review, and the answers

32. **Ten steps and the end card, the trash last** (17.2). The
    alternative is the trash before the folders, which leaves Show
    deleted on under `newFolder`, with the new folder dimmed.
    *Answer:* as recommended.
33. **Cut and paste is the move,** with Alt and the wheel as the way to
    the other canvas, since both are hidden from sight (question 27).
    Move to canvas and Move to new canvas get a line on the end card.
    *Answer:* as recommended.
34. **Renaming, and dragging a tile onto a folder, are steps,** since
    the Overview shows no sign of either. The alternative is a line each
    on the end card, for nine cards in all. *Answer:* as recommended.
35. **The trash steps use a canvas,** the one the snippet is on, so the
    restore brings a snippet back into sight. A folder is deleted and
    restored the same way, and would do as well. *Answer:* the canvas
    with the snippet on it.
36. **The run's folders are those made while `newFolder` is up,** with
    the tutorial's own. They count as the tutorial folder for every
    step, and Done puts them all in the trash under one confirmation
    that names each. They are not kept across a restart. The
    alternative, every folder made while the topic runs, would put a
    folder made for real work during a topic left partway in the trash
    with the tutorial's. *Answer:* as recommended.
37. **`newCanvas` and `newFolder` wait for their goal,** since later
    steps use what they make. `deleteCanvas` does not: `restore` has a
    need, with its line, for something in the trash. *Answer:* as
    recommended.
38. **Inside the Overview,** the spotlight rings the Overview's own
    widgets, and the card keeps its places. If the hand check finds it
    covering what a step needs, the fix is a place of its own over the
    Overview, the lower right corner, where the grid is usually empty.
    *Answer:* as recommended.

### 17.8 Found while building

- **The card covered the grid.** At the test display's 1280 by 768, top
  center lies over the Overview's first row of tiles, which
  `switchFolder` and `moveCanvas` press and drag: the walk-through's
  drag landed on the card. So the fallback of question 38 is built.
  While the Overview is up, the card tries its lower right corner
  first. At 1920 by 1080 it sits there clear of the grid, the sidebar,
  the footer and Show deleted.
- **Put away and back keeps the Overview up.** With nothing pinned the
  overlay is hidden, and hidden keeps what edit mode left up
  (`docs/OVERLAY_STATES.md`); only view mode and the pinned view end
  the panels. The matrix rows for it say nothing, and 16.8's bullet,
  which said otherwise, is corrected.
- **`moveCanvas` begins with the Overview closed:** the tile that
  `switchFolder` presses closes it. The step's need says how to open it
  again, as 17.2 meant, and the matrix follows that line before it
  takes its way.
- **`Anchor::item` became `Anchor::of`,** the id of the snippet, folder
  or canvas the anchor is for.
- **The spotlight's rule is one function,** `TutorialCard::SpotlightRect`,
  which the draw and the tests share: the step waiting, and the spot not
  covered, or the Overview's own while it is up and nothing is over it.
- **By hand,** at 1920 by 1080 with real input: the bar's + and grid
  button with the rings on them; the line for a subject; Ctrl+X, Alt and
  the wheel, Ctrl+V; New canvas pressed for New folder (the near miss);
  a folder renamed by a double-click; the tutorial's folder and a tile;
  the Overview opened from the right-click menu, as the line says; a
  tile dragged onto the new folder; the trash and its confirmation, with
  no ring through it; Show deleted and Restore; Done, whose confirmation
  names both folders and puts both in the trash.

## 18. Profiles

Status: **built** (2026-09-28). Its questions and their answers are in
18.7, and the design below matches them. What building it found is in
18.8.

### 18.1 What it teaches, and why

Basics' `programs` card says to look through Settings > Behavior and
make a profile for each program that needs its own. This topic shows
how, and what a profile does:

- **What a profile is for.** The overlay runs over other programs, and
  they differ: some games break when the overlay takes focus, others
  need it to. A profile keeps the Behavior settings and the shortcuts
  for one program. It runs whenever the overlay comes up over that
  program, and the defaults run everywhere else.
- **Making one.** Settings > Profiles has Make a profile for this, for
  the program the overlay is up over. It is matched on the program's
  file (`ProfileMatch`), and it runs at once: its row says "currently
  active".
- **What it can change:** what is in the boxes marked Per profile, the
  settings in Behavior and the shortcuts in Hotkeys. Showing (labeled Profile), at the top
  of each box, says whose values are below: the defaults, or a
  profile's.
- **Three things the panel shows no sign of,** which the steps make
  concrete:
  - A profile only runs over its program. Brought up over another one,
    the same rows show the defaults. `change` says so in a sentence;
    no step goes there any more (question 39).
  - A profile states only what it changes. The rest comes from the
    defaults.
  - A row set in a profile stays the profile's own, even set back to the
    default's value. Only the arrow beside it hands it back.

**The setting the steps change is Don't steal focus** (question 40). It
is the reason the `programs` card gives for profiles, and unticking it
shows at once: the row turns to the accent color with its arrow, and
the three rows under it gray out, since they only matter while focus
stays with the program. Over the program, the overlay then takes focus.
The revert at the end puts it back.

**Left out:**

- **New profile,** for a program that is not in front. The end card
  names it.
- **The match rules,** the Applications and Title contains lists inside
  a profile's row, and renaming a profile. The row opens with a click;
  the end card names the lists.
- **The order of profiles:** the first that matches runs. The end card
  says so.
- **The shortcuts in a profile:** the same marks and arrow as in
  Behavior, so no step of their own. The `behavior` card names them.
- **What each Behavior setting does.** Each row has its "?".

### 18.2 The chain

| # | Id | Kind | Next | The card says (the gist) | Points at | Needs (6.4) | Done when | Near misses (6.6) |
|---|---|---|---|---|---|---|---|---|
| 0 | `openProfiles` | do | moves on | Programs differ: some games break when the overlay takes focus, others need it to. A profile keeps settings for one program. Right-click an empty spot, choose Settings, and pick Profiles on the left. | Profiles, in the section list | - | Settings up on its Profiles section | - |
| 1 | `makeProfile` | do | **waits** | The overlay is up over {underneath}. Press Make a profile for this: it runs whenever the overlay comes up over {underneath}, and says "currently active" while it does. | Make a profile for this | Settings up; its Settings tab; the Profiles section; a program underneath | the tutorial's profile matches the program underneath (18.8) | a blank profile made: "That made a blank profile, which matches no program until you name one. Make a profile for this is the button to its left." |
| 2 | `behavior` | do | moves on | A profile can change what is in the boxes marked Per profile: the settings in Behavior, and the shortcuts in Hotkeys. Pick Behavior on the left. | Behavior, in the section list | Settings up; its Settings tab; the tutorial's profile | Settings up on its Behavior section | - |
| 3 | `change` | do | **waits** | Profile, at the top of the box, says whose settings are below: {profile}'s. Untick Don't steal focus. Over {program}, the overlay will now take focus, and the rows under it gray out: they only matter while it doesn't. Over any other program the defaults apply, and Profile shows them. | the Don't steal focus row | Settings up; its Settings tab; the Behavior section; the tutorial's profile; Showing on it | the tutorial's profile states more Behavior settings than when the step began | - |
| 4 | `revert` | do | moves on | To hand a setting back to the defaults, press the arrow beside it. Ticking it again would keep it {profile}'s own, in the accent color. | the first arrow in the box | Settings up; its Settings tab; the Behavior section; the tutorial's profile; Showing on it; something set in it | the tutorial's profile states fewer Behavior settings than when the step began | ticked back by hand: "That keeps it {profile}'s own - the row is still in the accent color. The arrow beside it hands it back to the defaults." |
| 5 | `end` | read | Done | That's profiles. New empty profile makes one for a program that isn't in front. Open a profile's row to add more programs to it, or part of a window's title. The first profile in the list that matches is the one that runs. | - | - | Done | - |

**The words in braces** are filled by `Expand()` from the world (18.3):
`{underneath}` is the program the overlay is up over now, `{profile}`
the tutorial's profile's name, `{program}` what it is matched on, and
`{showing}` the name Showing shows ("Defaults" for the defaults).

**"The tutorial's profile"** is the one made while `makeProfile` is up
(18.3), as the run's folders are those made while `newFolder` is up.

**Which steps wait** (question 37's rule). `makeProfile` does, since
every later step is about the profile it makes. `change` does, since
`revert` takes back what it set.

**What each goal reads.** Each is a change between the step's start and
now, as in 6.3, and none looks at what was pressed:

- **A profile made:** the tutorial's profile matches the program
  underneath. New profile, with the program's file typed into its
  Applications, counts as well. A profile made that matches nothing is
  the near miss.
- **A setting stated:** any Behavior row of the tutorial's profile, not
  only Don't steal focus. What counts is the profile stating it (the
  row's mark), not its value: a row ticked and unticked again is still
  stated.
- **A setting handed back:** the tutorial's profile states fewer
  Behavior settings. The near miss is a stated row whose value is the
  defaults' again.

**The text follows the world:**

- **`openProfiles` with the Overview already up** on its Canvases tab:
  "Press Settings, at the top of the Overview, and pick Profiles on the
  left."
- **`makeProfile` when a profile of the user's already matches the
  program** (question 44): a line under the text, "{running} is already
  the profile for {underneath}, and the first in the list that matches
  is the one that runs. Make another anyway, for practice: it won't run,
  but the steps work the same." (18.8)

### 18.3 What it needs that is new

- **A topic without a folder** (question 43). Nothing in this topic is
  on a canvas, so it makes none: `Topic` gains a flag, and for such a
  topic `StartTutorial` and a resume go to no folder, `tutorial.folder`
  stays `"0"`, and no step needs the tutorial folder. The canvas that was
  up stays up. The checkbox on this topic's end card and skip card
  says "Keep the tutorial profile" (section 20).
- **The tutorial's profile.** The profiles that appear while a step
  flagged `keepsProfiles` is up, `openProfiles` and `makeProfile`, as
  `keepsFolders` does for folders. Of several, it is the newest still
  there that matches a program, so a blank one made first by mistake is
  not it; with none matching, the newest blank one:
  - Known by its id (`core::ProfileId`), which each profile is given
    when it is read or made and keeps through a rename, and which is
    never written to the file. It was known by name at first, names
    being unique (`docs/SETTINGS.md`, section 5). Found in review on
    2026-09-29: a profile of the user's renamed while `makeProfile` was
    up was a name not there when the step began, so it counted as made,
    could become the tutorial's, and went at Done without a question.
    And the tutorial's own, renamed, was lost to it.
  - Not kept across a restart, as the run's folders are not. After one,
    the need's line sends the user Back to make it again.
- **Done**, and More topics, delete the profiles made in the run, the
  tutorial's and a blank one made on the way, without asking, as a
  row's trash button does; with Keep ticked, they are kept (section
  20). The deletion goes through the Settings page
  (`SettingsPage::RemoveProfiles`), so that Showing, which holds an
  index, follows the list as it does when a row is deleted there.
- **Showing follows a profile just made** (question 42). Both makers
  point Showing at the profile they made. Today it stays where it was,
  on the defaults when no profile was running, and the first change a
  user makes after making a profile lands in the defaults, for every
  program. The overlay coming up already points it at the profile that
  runs (`SettingsPage::OnOverlayShown`); making one is as strong a sign.
- **The world** (7.1):
  - the program underneath, as one name: its file, else its title, and
    none when it is not known (`Underneath()`, from
    `Settings::UnderlyingApplication`);
  - the profiles: each one's name, whether its rules match the program
    underneath, whether it is running, and how many Behavior settings it
    states, with how many of those hold the defaults' value
    (`ProfileFacts`);
  - the Settings panel: whether the Overview is on its Settings tab, the
    section picked, and whose values Showing shows.
- **The start record** keeps the profiles, by id, and the tutorial's
  profile's stated count.
- **Eight needs:**

  | Need | Not met when | The card says | Button |
  |---|---|---|---|
  | Settings up | the Overview is not up | "Open Settings: right-click an empty spot and choose Settings." | - |
  | Its Settings tab | the Overview is on Canvases or About | "Press Settings, at the top of the Overview." | - |
  | The Profiles section | another section is picked | "Pick Profiles in the list on the left." | - |
  | The Behavior section | another section is picked | "Pick Behavior in the list on the left." | - |
  | A program underneath | the program underneath is not known | "The overlay is up over nothing it can name. Press {key:toggleEditMode} to put it away, click the program you want a profile for, and press it again." | - |
  | The tutorial's profile | there is none, or it is gone | "The tutorial has no profile of its own: it was deleted, or the app has restarted since. Go Back to make one, or go on with Next." | - |
  | Showing on it | Showing shows the defaults or another profile | "Profile is on {showing}. Pick {profile} there: a change is made to whose settings it shows." | - |
  | Something set in it | the tutorial's profile states no Behavior setting | "{profile} sets nothing of its own yet. Untick Don't steal focus in it first, or go on with Next." | - |

  "Settings up" opens the Overview by the menu's Settings item, where
  17's "the Overview up" says to choose Overview. Four of the needs are
  for where the user is in the panel, as 17's were for the Overview.
- **Anchors, marked by the Settings page** (7.2):
  - a section's button in the list, by its section;
  - Make a profile for this, and New profile;
  - a profile's trash button, by its row;
  - Showing, and each of its entries;
  - the Don't steal focus row;
  - the revert arrow of each Behavior row the target states, by its
    row. The arrow is drawn by `SettingCheckbox`, a free function; the
    page marks the last item after it, when the row is stated.
- **Spots:** `SectionProfiles`, `SectionBehavior`, `MakeProfile`,
  `Showing`, `DontStealFocus`, and `Revert`, the first arrow marked.
  While the line for Showing is up, the ring is on Showing, as it is on
  the dock chip while that line is (18.8).
  They are the Overview's own (`TutorialCard::InOverview`), so they are
  drawn while the Overview is up and nothing is over it, and the card
  takes the lower right corner first (17.8).
- **The strings:** each step's title, texts and near misses, the needs,
  the end card (`tutorial.profilesEnd.*`), the checkbox's profile
  labels, and `tutorial.topics.profiles.*` ("Profiles": "Make a profile
  for a program, change a setting in it, and see it run only there.").
- **A row in the topic table,** sixth and last.

### 18.4 What does not change

- The runner's loop, the card, the list and the settings rows.
- The Settings panel's layout, and how profiles are matched, resolved
  and stored. The page only marks anchors, points Showing at a profile
  it made, and removes the tutorial's profile for Done.
- Settings are edited through `Settings` as always; the tutorial writes
  none of them.

### 18.5 Tests

- **`chains_test`,** against the fake world:
  - each goal and near miss, and the nine needs;
  - the tutorial's profile: one made while `makeProfile` is up is the
    tutorial's, and one made during another step is not;
  - the texts for the Overview already up, the hotkey unbound, the
    overlay that never left, and a program that has a profile already.

  `TopicsTest` covers the shape; the app tests, the flag for no folder.
- **`AppWorld`:** the program underneath; the profiles, with their
  matches, stated counts and which runs; the Settings tab, its section
  and Showing; each new anchor marked where its widget is drawn.
- **The page:** Showing follows a profile made by either button; the
  removal for Done moves Showing as a row's trash button does.
- **Done:** deletes the profiles made in the run, and no other; with
  Keep ticked, keeps them; a topic without a folder trashes none.
- **The walk-through,** with hands, the headless app's program
  underneath set before each showing
  (`host_.overlayWindow.underlyingApp`):
  - the right-click menu's Settings, and Profiles;
  - Make a profile for this;
  - Behavior;
  - the Don't steal focus checkbox;
  - Showing's list, and the profile in it (the UI engine, where the
    headless app has no handle on a combo's list);
  - the arrow;
  - Done.
- **The derail matrix,** a row per way:
  - each step from `makeProfile` to `revert`: the Overview closed (the
    need for Settings up); the Canvases tab (its need); another section
    (the section's need); the tutorial's profile deleted (its need);
  - `makeProfile`: nothing underneath (its need); New profile (the near
    miss);
  - `change` and `revert`: Showing on the defaults (its need);
  - `revert`: the row ticked back (the near miss).
- **By hand:** every card with real input, over a real program and the
  desktop, and where the card lands over Settings.

### 18.6 Getting there

Two pieces of work, as in 17.6:

1. **What the chain stands on:** the topic without a folder; the
   tutorial's profile and Done with it; Showing following a made
   profile; the world's program, profiles and Settings; the needs; the
   anchors and spots. Each with its tests, before any step uses it.
2. **The chain:** its strings and its row, with the walk-through, the
   matrix and the hand check.

### 18.7 Questions for review, and the answers

39. **Seven steps and the end card** (18.2), with a trip to another
    program (`otherProgram`, `elsewhere`) as the demonstration that a
    profile runs only over its program. The alternative, for five and
    the end card, is to pick Defaults in Showing and see the row ticked
    there, which shows the layers but not that the profile follows the
    program. *Answer:* as recommended (the trip). *Revised on
    2026-09-29, after a test run:* the trip is gone, and `change` says
    in a sentence that over any other program the defaults apply. A
    first run starts over the desktop, so the profile was usually for
    explorer.exe, and the desktop, the easiest other place to click,
    is that same program, as every File Explorer window is. The card
    had to leave it out there and explain why, the icon in the
    notification area likely raises the taskbar, explorer.exe again,
    and naming a program to go to (Notepad, Calculator) has
    exceptions of its own: a profile of the user's for it, a Store app
    known only by its host, a program that is not installed. What the
    trip showed, Settings already says: "currently active" or "not
    active" beside Profile, and Profile on the defaults over any other
    program. With it went `otherProgram`, `elsewhere` and the need
    for another program: five steps and the end card.
40. **Don't steal focus is the setting the card names,** for the reason
    the `programs` card gives and for the rows that gray out under it.
    The goal counts any Behavior row. *Answer:* as recommended.
41. **The revert is a step,** with the tick-back as its near miss, since
    a row set back by hand stays the profile's own and nothing on screen
    says so but its color. The alternative is a line on the end card.
    *Answer:* as recommended (the step).
42. **Showing follows a profile just made,** by either button: a change
    to the Settings page outside the tutorial. Without it, `change`
    needs a step of its own to pick the profile in Showing, and a user
    who makes a profile without the tutorial changes the defaults next.
    *Answer:* as recommended (the change).
43. **The topic makes no folder,** and Done deletes the tutorial's
    profile, with "Done, keep the profile" to keep it. The alternative
    is a folder as every topic has, which stays empty, and a keep button
    that keeps both. *Answer:* as recommended (no folder).
44. **A program with a profile of the user's already:** the step makes a
    second one anyway, for practice, and the card says it won't run
    while the user's comes first. The alternative, the steps working on
    the user's own profile, would have `change` and `revert` edit it,
    and Done could not delete what it did not make. *Answer:* as
    recommended (a second one).

### 18.8 Found while building

- **The list outgrew a small display.** Six topics, each with its steps
  on a line of their own, made the list taller than the test display's
  768 pixels, and its Back went off the bottom. The steps now share the
  status's line ("New, 8 steps"; the running one says "At step 3 of 8"
  alone), and the card is kept wholly on screen wherever it is placed
  (section 3).
- **A profile made in the second before the card moves on** was nobody's:
  `openProfiles` is met as Profiles opens, and a Make pressed within the
  second came before `makeProfile` began, so it was not the tutorial's
  and the step then said a profile of the user's was there. So
  `openProfiles` keeps profiles too, and `makeProfile`'s goal is a
  result, the tutorial's profile matching the program, not a profile
  made during the step. The same second lies between the Overview's
  steps in 17; no one has pressed New folder that fast.
- **Text variants became lines.** The step text is chosen from the world
  alone, and "a profile of the user's" and "the overlay never left" are
  about the tutorial's profile, which only the runner knows. The first
  is a line under `makeProfile`'s text, the second was a need of
  `elsewhere`'s, over another program (both gone since; question 39).
  `change` says "over {program},
  the overlay will now take focus" either way: of a practice profile
  that does not run, `makeProfile`'s line has said so already.
- **A read step may have needs.** `elsewhere` was one, until it went
  (question 39); its lines guided and did not hold Next. The shape test said read steps had none, and no
  longer does.
- **`Expand()` takes the tutorial's profile's name,** for {profile} and
  {program}, from the card. {running} is the profile that runs.
- **The ring on Showing** is the whole box: marked after it, the last
  item is its preview's text. While the line for Showing is up, the
  ring is on Showing instead of the step's spot.
- **The desktop is identifiable.** Over the desktop, the program
  underneath is explorer.exe, and Make a profile for this makes one for
  it, which File Explorer's windows match too. So the need for a program
  no longer says "such as the desktop". Settings' own line for nothing
  identifiable still named it; since 2026-09-30 it says "no window in
  front" instead.
  `otherProgram`'s "(the desktop will do)" held unless the profile was
  for explorer.exe, where its near miss said to click another - the
  usual case, as it turned out (question 39).
- **The ring shows the way to a spot in Settings** not drawn yet: the
  Overview's Settings tab while it is on another tab, then the row of
  the section the spot is in while another section is picked. Found in
  a test run on 2026-09-29: over the Overview's Canvases tab,
  `openProfiles` said to press Settings and ringed nothing, since its
  spot, the Profiles row, is only drawn on the Settings tab.
- **Hand check,** at 1920 by 1080 with real input, over Notepad and the
  desktop, while the chain still had its trip (question 39): the ring on Profiles, the card in the lower right; New
  profile's line; Make a profile for this, "running now", Showing on it
  in Behavior; Don't steal focus unticked, the arrow and the gray rows
  under it, "sets 1 setting of its own"; put away, the desktop clicked,
  and back: Underneath explorer.exe, Showing on Defaults, the row
  ticked; the line and the ring on Showing; the profile picked; the row
  ticked back and its line; the arrow; Done, which deleted both the
  blank profile and the tutorial's, and Done, keep the profile, which
  kept it with nothing set. `tutorial.folder` stayed "0".

## 19. Where the card sits, steadier

Status: **built** (2026-09-28). Its questions and their answers are in
19.4, and the design below matches them. What building it found is in
19.5.

### 19.1 Why

A hand test at 150 % interface scale found the card restless: on the
right, then on the left after the practice snippet was moved a little,
then at the bottom when the step moved on. Section 3 places it anew on
every frame, from scratch, and four things make it jump:

- **It goes back to the first free place.** As soon as top center is
  clear, the card returns there, however well it sat elsewhere.
- **One pixel covered counts.** A snippet moved a little under the
  card's edge sends it away.
- **It moves in the middle of a gesture,** while the user still drags
  what it made room for.
- **Its height changes** with the step's text and its lines, so the
  same place can come to cover something with nothing else moved. At a
  large scale all of this happens more often.

This design takes on the first, and makes a move easy to follow. The
middle two are left for a second look after trying it (19.3).

### 19.2 The design

- **The card stays unless it is in the way.** It keeps its place, across
  steps as well, as long as it covers none of what the user is asked to
  click: the anchor the ring is on, and the bars over the selection,
  whose buttons a line may name. Covering the subject alone, such as the
  practice snippet, is not a reason to leave.
- **Where it goes when it must,** the places and their weights are
  section 3's: of the places that cover least, the nearest to where it
  is, not the first in the list. So a card in the top right that must
  leave goes to the top left or center before the bottom.
- **Its place is one of the places, not a point.** It keeps "bottom
  center", not a y, so a card whose text grows stays at the bottom,
  growing upward, and a changed display size keeps its place as well.
- **Where it starts:** as section 3 has it, when a run begins or the list
  opens, with nothing to keep.
- **A move slides.** From where it is drawn to its new place, over about
  150 ms, easing out, so the eye follows it. A move while it slides
  starts from where it is drawn then. A card the user dragged does not
  move again, as before.

### 19.3 Left for later

If the card still feels restless, two more steps, from the same hand
test:

- **No move while a button is held:** it reconsiders when the button is
  let go, so it does not slide away under the hand.
- **Only a real overlap counts:** a few pixels over a snippet's edge are
  not in the way, and the edge it leaves by is wider than the one it
  comes back by, so it does not flicker there.

### 19.4 Questions for review, and the answers

45. **What makes it leave** is what the user must click: the ring's
    anchor and the bars. The alternative also counts the subject, which
    keeps the snippet clear but brings back most of the moves the hand
    test found. *Answer:* as recommended.
46. **It stays across steps,** and is not placed anew when a step
    begins. The alternative, placing it anew at each step and keeping it
    only within one, is what made it jump "when the step moved on".
    *Answer:* as recommended.
47. **The slide:** 150 ms, easing out, from where it is drawn. The
    alternative is no slide, which is less code but keeps the "where did
    it go" moment. *Answer:* as recommended.

### 19.5 Found while building

- **The Overview opened places the card anew.** Kept where it was, the
  card left the Settings section buttons for the nearest place, bottom
  center, and sat there over the Behavior rows and Showing, which a user
  off the path clicks, as two tests did. The lower right is first over
  the Overview because its grid and pages fill the middle (question 38);
  the nearest place does not know that. Opening or closing the Overview
  changes all that is under the card, so the card is placed as at the
  start, and slides there.
- **Where it goes when it must** is ranked by what is in the way first,
  then by what it covers, then by distance. Ranked by the weights alone,
  a place over the subject weighs as much as its own over the anchor,
  and the nearer, its own, would win.
- **A card that grows at the bottom grows upward,** since its place is
  "bottom center" and not a y: `resize`'s text is taller than `move`'s.
- **The list forgets where the card was dragged** (asked for after
  trying it). It shows at the top center, where it first did, always:
  it points at nothing, and a list that turns up wherever the last
  card was is hard to find. A card dragged during a topic is placed as
  at the start again when the list opens, and stays so for the card
  the list closes to and for the topic started from it.
- **The list in two columns** (asked for after trying it at 150 %). One
  column of six topics was long, and at a step's card's width each
  topic's line took three lines. The list is now twice as wide, with the
  topics in two columns read row by row, the two in a row as tall as
  each other, each pressed as a whole as before: three rows of two
  lines instead of six of three. Only the list is wider; a step's card
  stays narrow beside what the user works on. Where the display has no
  room for twice the width, at a large scale on a small display, the
  list stays one column at a step's card's width. Checked at 150 % on
  1920 by 1080.
- **Tests:** the card stays over the subject alone; stays at the bottom
  when the step moves on and the top is clear again; and slides, part
  of the way on the frame after and all of it once the slide is over.
  All three fail with section 3's placing anew.

## 20. The end card's buttons

Status: **built** (2026-09-29), asked for after trying the tutorial at
150 %.

### 20.1 Why

The end card and the skip card had four buttons in two rows: Back, Done,
"Done, keep the folder", and More topics alone on a row below, quiet and
right-aligned. Trying them found three things:

- **More topics was overlooked,** quiet beside an accent Done.
- **Two rows of buttons** looked crowded for a card that ends a topic.
- **Keeping the folder was Done's alone.** More topics kept the topic
  running until another was chosen, which then kept its folder: a second
  way out, with the other choice and nothing on screen to say so.

### 20.2 The design

- **One row: Back, Done, More topics,** Done and More topics both accent
  buttons. Back stays: on the skip card it is the way back to the step,
  so a misclick costs nothing.
- **A checkbox above them keeps what the topic made:** "Keep the
  tutorial folder", "... folders" where the topic made more than one
  (Folders and canvases), and "Keep the tutorial profile", or
  "... profiles", for Profiles.
  Unticked to begin with, on every end and skip card: Back and Skip
  untick it. With nothing left to keep, such as a folder the user
  already deleted, it is left out. It is above the buttons because it
  changes what two of them do, and is read before either is pressed.
- **More topics ends the topic as Done does,** at the press: the folders
  in the trash, asked first where Settings says to, or kept with the box
  ticked; the profiles deleted or kept. Then the list opens. The list's
  Back, which went back to the card, goes: returning to a card whose
  folder was just trashed has nothing to return to, and it was seldom
  wanted.
- **The list's one button is Close.** With no topic running, it takes
  the card away. Opened from Settings during a topic, it goes back to
  that topic, as pressing the topic's own row does; a topic left that
  way, by starting another from the list, keeps its folder, as before
  (13.3): there was no card and no box to choose with.
- **The end texts lose their last paragraph,** "Done puts the tutorial's
  folder in the trash. To keep what you made, choose Done, keep the
  folder.": the checkbox says it.

### 20.3 Tests

- More topics ends the topic as Done does and opens the list: with the
  box ticked, the folder kept and no question; without, the question,
  with the list behind it.
- The list opened during a topic closes back to it.
- The box starts unticked on each card.
- Done and Keep on the end card and the skip card, for a folder, the
  Folders topic's two, and Profiles' profile; by name in the UI tests,
  the checkbox's label for Profiles among them.
