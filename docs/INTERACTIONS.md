# Interactions: a design

Status: **agreed** (phase 0, reviewed 2026-09-26); built in the phases of
section 11. Every behavior below is either what the app does today (said
so) or a change (marked **Change**). As the phases land, what each settles
is described in `docs/ARCHITECTURE.md`; this file stays the design they
follow.

## The principle

Input is handled by one structured, well-defined machine, and that is a
decision in its own right, not an implementation detail. Every pair of
state and event has a written answer; what is a matter of taste is an
entry in a table; and the rules in sections 4 to 7 are how new input
behavior is added.

A change that fits them is an edit to a table, a new command or a new
interaction. A change to the structure itself - the levels, the routing
answers, the order of events, the rule for ambiguous presses - has to be
well founded, and argued as carefully as this document argues its own
shape: the case that does not fit, why no table entry, command or
interaction can express it, and what the change does to every case in
section 9. Convenience, or a deadline, is not such a reason. If the
structure does change, this document changes with it, first.

## 1. What this is for

Most input bugs in the last two reviews had one shape: something was in
the middle of something - a stroke, a drag, a hold, a menu - and another
input arrived that nobody had decided about. The `settle-hand` branch made
one rule structural (every command settles the hand first). This design
goes the rest of the way: every input goes through one state machine in
which **every pair of state and input has a written answer**, and the
choices that are matters of taste - does Escape cancel a stroke? - are
entries in a table, not branches in code.

It covers the canvas's own gestures, keys, the wheel, global hotkeys, the
menus and popovers, the panels, text being typed, and ImGui's own widgets
where they hold the pointer (a slider, a tile being dragged). It does not
cover what is below the app: the input grab, the raw-input sink and the
virtual cursor stay in the platform layer and deliver events as they do
now.

## 2. Vocabulary

- **Event**: one thing that happened, with its time and the modifiers
  held at that moment (section 3).
- **Interaction**: anything that lasts from a beginning to an end and
  wants a say in the events that arrive meanwhile - a stroke, a drag, a
  press not yet understood, a held arrow key, an open menu, a note being
  typed, drawing mode, the Overview.
- **Command**: an interaction whose beginning is its end - Undo, Copy, a
  tool picked. It needs no state of its own.
- **Stack**: the interactions in progress, innermost on top (section 4).
  This is the machine's whole state.
- **Recognizer**: what decides what a press means - click, drag, hold,
  double-click - and which interaction it starts (section 6).
- **Effect**: something the machine asks of the UI that can only happen
  inside a frame, like opening a popup.

## 3. Events: one stream, in order

| Event | Carries | Notes |
|---|---|---|
| `PointerDown` | button, position | Left, Right, Middle, X1, X2 |
| `PointerMove` | position, buttons held | also with no button held (hover) |
| `PointerUp` | button, position | |
| `Wheel` | notches (fractional), position | |
| `KeyDown` | key, repeat flag | |
| `KeyUp` | key | |
| `Modifiers` | Ctrl, Shift, Alt | only when they change |
| `Tick` | - | once per frame: what timeouts run on |
| `Hotkey` | which | global; may arrive while hidden, with no frames |
| `Lifecycle` | Shown, Hidden, ViewOnly, EditMode, SessionEnding | from the tray |

Every event carries its time and the modifiers as they were when it
happened.

**Change: one ordered stream.** Today the pointer and the keys arrive on
two timelines: raw pointer events call `OnMouse` from the message pump,
between frames, while keys, the wheel and the modifiers are read from
ImGui at frame time. The order between them is lost - an Escape pressed
between two moves of a drag is seen after both. A state machine is only
as well-defined as the order of its input, so the platform window gets
one input callback that delivers every event above in arrival order, with
its time and modifiers. ImGui is still fed by its backend as now, for its
widgets.

Events are handled as they arrive, not queued for the frame: a hotkey has
to act while the overlay is hidden and no frames run. What only a frame
can do - opening a popup, clearing ImGui's drag - is queued as an effect
and applied in the next frame, just before the popups are drawn (at the
very start of the frame, ImGui closed a menu again before it was drawn).
That one queue replaces the
request flags the popups use today (`itemPropertiesPopoverRequested_`,
`colorChooserRequested_`, `ContextMenu::RequestOpenAt`, ...), which exist
because `OpenPopup` cannot be called between frames.

What stays one frame stale is ImGui's own answer to "is the pointer over
one of my windows" (`WantCaptureMouse`) and "is a text field focused"
(`WantTextInput`): both are last frame's, as they are today. Both change
only on a click or a focus change, which is itself an event this machine
sees, so the staleness never decides anything a frame too early.

## 4. The machine: a stack of interactions

A flat state machine over everything - which mode, which panel, which
popup, what the pointer is doing, what is being typed - is the product
of all of them, and its transition table would be unreadable. It is
also not what the app has: those things nest. A slider is dragged inside
a popover, over a snippet in drawing mode, on the canvas. So the state is
a **stack of interactions**, one per level, innermost on top - a
pushdown automaton, or in statechart terms (Harel, 1987) a hierarchy of
states where the innermost active one sees an event first.

| Level (bottom to top) | What lives there | At most |
|---|---|---|
| Canvas | the canvas and its selection - always there | 1 |
| Mode | drawing mode on a snippet; a creation tool in hand | 1 |
| Panel | the Overview; the cheat sheet | 1 |
| Popup | a context menu; the Properties popover; the color chooser; a delete confirmation | 1 |
| Text | a note being typed; a name being edited; a key being captured in Settings | 1 |
| Gesture | what a held button or key is doing: a press not yet understood, a stroke, a drag, a widget drag, a spent button, a nudge or wheel burst | 1 |

The order of levels is fixed, and each level holds at most one
interaction. Checked after every event in debug builds.

### 4.1 Routing

Every event is offered to the stack from the top down. Each interaction
answers with one of:

- **Claim**: it is mine; handled; stop.
- **Pass**: not mine; offer it to the level below.
- **Finish**: I am done, as I was meant to end; pop me. The answer also
  says whether the event is used up (a click outside a menu closes it and
  does nothing else) or goes on down.
- **Cancel**: I am done and leave no trace; pop me; the event is used up.
- **Start**: this event begins something - a command, or a new
  interaction to push. See 4.2.

An event nobody claims is dropped. A hover move, a key that is bound to
nothing, a wheel notch over nothing: dropping them is an answer too, and
it is written down (the Canvas level's table ends with "anything else:
drop").

This is the rule from the last conversation, made general: the
interaction in flight sees every event first, and decides whether it is
its own business. An Escape during a drag reaches the drag before it
reaches anything that would clear the selection.

### 4.2 Starting something ends what is above it

When a level starts a command or pushes an interaction, the interactions
above what it starts are ended first, top down: for a new interaction,
whatever is on its own level and above; for a command, what its scope
covers (below). The levels between the one that started it and the new
interaction passed the event, and stay - a stroke started by the Canvas
level on the snippet in drawing mode leaves drawing mode where it is.
(Found while building phase 3: read as "above the level that starts it",
that stroke ended drawing mode.) How each ends is its own answer:
**interrupted** - kept as far as it got - unless it says otherwise. This
is `SettleHand`, generalized: Ctrl+Z claimed at the Canvas level ends the
stroke above it (kept), then undoes it.

Not every command should end everything above the level it is bound at,
and some reach further, so each command states its **scope**:

| Scope | Ends | For |
|---|---|---|
| Hand | the Gesture and Text levels | most commands: undo, copy, a tool, a nudge |
| Canvas | Gesture, Text, Popup | anything that switches or empties the canvas: a canvas switch, a capture, moving the selection to a new canvas |
| All | everything above Canvas | view-only mode; the app exiting, the session ending |

The Hand scope ends a note being typed as well as the gesture: every
command commits one today, and one can still run while a note is open -
a key cannot reach it, but a press on the selection bar outside the field
can. (Found while building phase 3; the table first had the Gesture level
alone.)

The overlay put away ends what the Hand scope does: drawing mode, a panel
and a popup are still up at the next showing, as they are today. (Also
found while building phase 3: the table first had a Showing scope for it,
everything above Mode, which would have closed a panel or a popup on the
way out.)

Today these scopes exist as four hand-written functions (`SettleHand`,
`SwitchToCanvasSettled`, `SettleForPersistence`, `SetViewOnly`); here they
are one column of the command table.

### 4.3 Every transition is written

Each interaction's answer is a `switch` over the event kinds with no
`default`: an event kind added later is a compile error in every
interaction until someone decides what it means there. The routing loop
itself is a dozen lines. So "every pair of state and event has an answer"
is not a convention; the compiler holds it.

```cpp
// The sketch, not the final shape.
struct Answer {
    enum Kind { Claim, Pass, Finish, Cancel, Start } kind;
    bool usedUp = true;                          // for Finish
    std::optional<Command> command;              // for Start, and what a Finish produced
    std::unique_ptr<Interaction> push;           // for Start
};

class Interaction {
public:
    virtual Level level() const = 0;
    virtual Answer Offer(const Event& event, Editor& editor) = 0;
    // Ended from outside: keep what is done, make nothing new.
    virtual void Interrupt(Editor& editor) = 0;
    // Ended by the user: leave no trace.
    virtual void Cancel(Editor& editor) = 0;
};
```

`Editor` is the state the interactions work on: the session, the
selection, the tool, drawing mode, the clipboard - what `OverlayApp` holds
today besides its views (section 10).

## 5. The rules, as tables

The choices that are matters of taste are entries, per interaction, in
one table. A behavior change is an edit to a row. Proposed defaults:

| Interaction | Escape | Other button pressed | Wheel | Modifier change | Own button pressed again |
|---|---|---|---|---|---|
| Pending press | cancel | ignored until its release | ignored | re-read at recognition | lost release: interrupt, re-route |
| Stroke | **cancel** | ignored | ignored | nothing (fixed at press) | lost release: interrupt, re-route |
| Shape | **cancel** | ignored | ignored | line/rectangle switch | as above |
| Erase | **cancel** | ignored | ignored | nothing | as above |
| Rectangle erase | cancel | ignored | ignored | nothing | as above |
| Move / resize | **cancel** | ignored | ignored | Shift: keep aspect or not | as above |
| Box select | cancel | ignored | ignored | nothing | as above |
| Framing a snippet | cancel | ignored | ignored | nothing | as above |
| Bar button held | cancel (disarm) | ignored | ignored | nothing | as above |
| Widget drag (ImGui) | **cancel** | ignored | ImGui's | ImGui's | as above |
| Slider (ImGui) | **cancel: value restored** | ignored | ImGui's | ImGui's | as above |
| Spent button | pass | ignored | ignored | nothing | the release came after all |
| Nudge burst | cancel: back to where it began | interrupt | interrupt | nothing | - |
| Wheel burst | cancel: back to where it began | interrupt | continue (same kind) | kind changes: finish, start anew | - |
| Text (note) | **finish: text kept** (today's decision) | press outside: passed on, the field lets go and keeps the text | ImGui's | ImGui's | - |
| Popup | close | press outside: close, used up | ImGui's | - | - |
| Panel | close | its own | its own | - | - |
| Mode: drawing | leave drawing mode | - | - | - | - |
| Mode: creation tool | put it down | - | - | - | - |

Three rows answer the questions from the last conversation:

- **Escape cancels a stroke** (and every other gesture), restoring things
  as they were when it began.
- **Escape during a slider drag restores the value.**
- **A second button during a gesture is ignored, not a cancel.** This is
  the one I would reverse from my earlier suggestion, found by writing it
  down: Windows' touch press-and-hold injects a right press about 650 ms
  into a finger held still (measured; see `kTabletGestureFlags`), and the
  platform cannot tell it from a real one - none of it is tagged as touch.
  A finger resting mid-stroke would cancel its own stroke. As a table
  entry it is one word to change if the platform ever can tell.

Keys, beyond Escape: a note being typed and a name being edited take
every key, as a text field does; a popup takes every key but the global
hotkeys (**Change**, section 12); a panel takes every key but the global
hotkeys and its own.

Escape passing down the stack when the top has nothing to cancel gives
today's staged Escape for free: a spent button passes; a popup closes;
then drawing mode ends or the creation tool goes down; then the Canvas
level calls off a cut, then clears the selection. Today that order is an
`if` chain in `PutDown` plus `CloseTopmostPopover`.

## 6. Recognizing a press

### 6.1 The rule for ambiguity

A press can mean several things until the pointer has moved or time has
passed: a click, a drag, a hold, the first half of a double-click. The
rule:

> A press that can still mean several things does at once only what all
> of them share (a prefix), or something invisible that can be dropped.
> It never delays its feedback, and never visibly takes something back.

So **the first click on a snippet selects it at once**, and a
double-click then enters drawing mode on it: the double-click extends the
click, the way Windows and macOS guidelines ask (select, then open).
Waiting to see whether a second click comes would put the double-click
time - 350 ms here - between every click and its answer.

### 6.2 The Pending interaction

A press whose meaning is open pushes a **Pending** interaction on the
Gesture level. It holds the press, the recognition the rules gave it
(what a click, a drag, a hold and a double-click would each do), and
nothing else happens until it resolves:

| While Pending | Resolves to |
|---|---|
| the pointer travels past the drag threshold | the drag interaction, begun from the press point |
| its button is released | the click's command, if any; then remembered for a double-click |
| a Tick finds it held still for 0.5 s | the hold's command; then **Spent** |
| Escape | nothing (cancel) |
| an outside command | nothing (interrupt: there is nothing done to keep) |

**Change:** today a press on a snippet begins the move at once
(`BeginPlacement`) and a hold drops it again (`MatureHeldPress`). Here the
move begins only when it is a drag - with the snapshot taken as of the
press, so nothing about the drag changes - and a hold has nothing to
drop.

A press whose meaning is not open - a stroke on the snippet in drawing
mode, a handle - starts its interaction at once, with no Pending.

### 6.3 Spent: the rest of a press that has had its say

When a gesture ends while its button is still down - cancelled,
interrupted, a hold matured, a double-click acted on its second press -
it is replaced by **Spent(button)**, which swallows that button's moves
and release, and any other button's press and release, until its own
release comes. "The rest of the drag does nothing" is a state here, not a
consequence of fields being empty. It is also what makes a touch hold
safe: the injected right press lands on Spent.

### 6.4 Double-click

The recognizer remembers the last click: button, position, time, the
creation trigger held, and what it landed on. A press is a double-click's
second half when it is the same button, within 350 ms and 6 px, with the
same trigger. It acts on that press and becomes Spent.

**Change:** on empty canvas the fullscreen snippet is made on the second
*press* rather than its release, as on a snippet. A second press that
then drags no longer frames a region instead - a case nobody means.

### 6.5 The rules, in order

The recognizer is one function, `RecognizePress(press, context)`, whose
rules are tried in order; the first that matches decides. Context is the
level stack, what `ResolvePointerTarget` finds under the press, the tool,
drawing mode and the modifiers. Today the same decisions are spread over
`OnMouse`, `HandleItemGesture`, `HandleCreationGesture` and
`HandleStrokeEvent`, and depend on the order they are asked in.

| # | Press | At once (prefix) | Click | Drag | Hold | Double |
|---|---|---|---|---|---|---|
| 1 | any, over an ImGui window | - | - | Widget (ImGui's) | - | - |
| 2 | left, on a bar button | - | - | Bar button held | - | - |
| 3 | left, on a selected snippet's handle | - | - | Resize | - | - |
| 4 | left, on the drawing snippet, drawing mode, no Alt | - | - | Stroke / Shape / Erase / Rectangle erase / Text, by tool and modifiers | - | - |
| 5 | left, elsewhere, drawing mode, no Alt | leave drawing mode | - | - | drawing mode there, or fullscreen of the trigger's kind on empty canvas | - |
| 6 | left, a creation tool in hand | - | fullscreen | Frame | - | - |
| 7 | left, on a snippet, Shift | add to or take from the selection | - | - | - | - |
| 8 | left, on a snippet | select (and raise, if set) | - | Move | drawing mode | drawing mode |
| 9 | left, on empty canvas, Shift | - | - | Box select | - | - |
| 10 | left, on empty canvas, a trigger held | clear selection | - | Frame (that kind) | fullscreen (that kind) | fullscreen (that kind) |
| 11 | left, on empty canvas | clear selection | - | - | - | - |
| 12 | right, on the drawing snippet, drawing mode, no Alt | - | leave drawing mode | Erase | - | - |
| 13 | right, on a snippet | select (and raise, if set) | context menu | Resize from the nearest edge | - | - |
| 14 | right, on empty canvas | - | empty canvas menu | - | - | - |
| 15 | middle, X1, X2 | - | - | - | - | - |

Rows 1 and 15 hand the press to ImGui and to the bindings (section 7)
respectively. Every row carries its cheat sheet line, so the cheat sheet's
gesture rows are generated from the rules rather than kept in step with
them by hand, as they are today.

## 7. Commands and bindings

One `Command` type covers what is now `ShortcutAction`, `ClipboardAction`,
`CreateAction`, the three menu action enums, `ChromeButton` and the keys
handled by hand (undo, redo, Escape's stages, Delete, the arrows).
`ChromeButton` stays, since the bar layout setting stores bar buttons by
its names; each maps to a command. Each command has one row:

| Column | Meaning |
|---|---|
| name | for the menu row and the cheat sheet |
| scope | what it ends first (4.2) |
| available | whether it can act now: something selected, something to undo - what grays a menu row out and what a key checks |
| run | what it does |

Triggers are bound to commands in one table - a key combo, a mouse button
with modifiers, the wheel with modifiers - with the level the binding
belongs to (most are Canvas; the cheat sheet's key is Panel as well, so
it closes the sheet). Rebinding works as it does for keys today
(`AppConfig::toolShortcuts`), and takes mouse buttons too: the middle
button and X1/X2, with modifiers, can be bound to any command, and the
Settings row asks for "the key or button you want".

**Available** and **reachable** are different things, and both are
needed. Reachable is the stack's answer: a key reaches the Canvas's
bindings only if every level above passed it (a note being typed claims
every key; the Overview claims every key but its own). Available is the
model's answer: Delete with nothing selected does nothing. Today both are
one tangle of conditions per key (`historyKeysFree`, `keysFree`,
`!io.WantTextInput && !PanelOpen()`, the cheat sheet exception).

Global hotkeys are commands too, bound at the root: every level passes a
`Hotkey` event except the one interaction that wants it - a hotkey being
captured in Settings, which today is `CompletesAHotkeyCapture` in the
tray.

## 8. The session side

An interaction that changes the library opens a gesture on the session
when it begins and ends it one of three ways:

| Interaction ends by | Session |
|---|---|
| Finish | the step is filed, and written (`EndPlacement`, `EndErase`, ...) |
| Interrupt | the same, with what is there |
| Cancel | **new:** rolled back to the checkpoint the gesture took when it began; nothing filed, nothing written |

Previews change the model in memory and nothing is written until the step
is filed, so a rollback is exact and costs no write. The session already
takes a checkpoint per gesture (`PlacementGesture::checkpoint`,
`eraseCheckpoint_`, `StyleEdit::checkpoint`) and can roll back to one
(`RollBack`, used today when a write fails). What is new is
`CancelPlacement`, `CancelErase` and `CancelStyleEdit`; `CancelShape`
exists. A freehand stroke lives in the live layer until it is committed,
so cancelling one is clearing it.

`Session::EndOpenGesture` stays as the safety net it is: with the machine
right, no command ever finds a gesture open.

## 9. The cases, worked through

Each case as the machine sees it. "Kept" is Interrupt; "Esc" is Cancel.

| Case | Begins | Meanwhile | Ends | Kept | Esc |
|---|---|---|---|---|---|
| Freehand stroke | rule 4, at once: pen, color, width, snippet fixed | a point per move | release: one step | filed as far as it got | cleared |
| Shape | rule 4 with Ctrl/Shift or the bar's shape | Ctrl/Shift switch line and rectangle | release: one step, or nothing if too short | filed as it stands | `CancelShape` |
| Erase | rule 4 (eraser) at once; rule 12 once it drags | clips along the path | release: one step | filed | rolled back |
| Rectangle erase | rule 4 with Ctrl, or the bar's shape | the rectangle follows | release: erases, if 24 px or more | **Change: nothing** | nothing |
| Move | rule 8, once it drags | full delta from the press | release: one step | filed where it got to | rolled back |
| Resize | rule 3 at once; rule 13 once it drags | Shift flips keeping the aspect | release: one step | filed | rolled back |
| Box select | rule 9 once it drags | the box follows | release: adds what it touches | nothing | nothing |
| Framing | rules 6, 10 once it drags | the frame follows | release: a snippet, if 24 px or more | nothing | nothing |
| Hold | rules 5, 8, 10: Pending, 0.5 s still | - | the hold's command; Spent | - | - |
| Double-click | second press matches the remembered click | - | its command at the press; Spent | - | - |
| Right click | rules 13, 14: Pending | - | release: a menu (an effect), pushed on the Popup level | - | - |
| Bar button | rule 2 | lit only over its own button | release over it: its command | nothing | nothing |
| Tile dragged (canvas bar, Overview) | rule 1: Widget | ImGui draws the drag | dropped on a tile: reorder command | nothing (nothing done yet) | ImGui's drag cleared; Spent |
| Slider (Properties) | rule 1: Widget over the Popup level | the value previews | release: one step | filed | rolled back; ImGui's active item cleared; Spent |
| Typing a note | the Text tool's press, pushed on the Text level | keys are the field's | press outside: kept, and the press goes on (it makes no snippet) | kept (committed) | kept (today's choice) |
| Held arrow key | KeyDown at Canvas: a nudge burst | a nudge per repeat | key up: one step | filed | rolled back |
| Wheel spin | the first notch: a burst of its kind | a step per notch | a second without one: one step | filed | rolled back |
| Hotkey mid-anything | passed to the root | - | its command, after its scope ended what it covers | - | - |
| Lost release | own button pressed again | - | interrupted; the press routed afresh | - | - |
| Touch hold's injected right press | lands on Spent | - | swallowed | - | - |
| Put away | Lifecycle: Hidden | - | the Hand scope ends the gesture and the text; the rest stays for the next showing | - | - |
| Shown | Lifecycle: Shown | - | the Gesture level cleared; the recognizer forgets | - | - |
| View-only | Lifecycle: ViewOnly | - | All scope | - | - |

Found while building phase 3: the Text row first said a press outside a
note is used up by closing it. Today that press also does what it does -
leaves drawing mode, selects a snippet - and only making a snippet is
held back, and the design changes nothing it does not mark as a change;
so the note passes the press on, still open, which is what keeps it from
making a snippet.

Two findings from writing the table:

- **Rectangle erase, interrupted, erases today.** `EndGesture` ends it
  through its release handler, which performs the erase - the one gesture
  where "keep what is done" makes something that was not there. Proposed:
  interrupted, it erases nothing, like framing a snippet.
- **The wheel's and the arrows' undo bursts need no clock.** Today a burst
  is recognized by time (`kBurstSeconds`) and by the history not having
  moved (`lastBurstRevision_`). As interactions, a held arrow key is one
  step from key down to key up, exactly. The wheel has no "up", so it
  keeps a timeout - but as the end of an interaction, which an outside
  command interrupts (filing it) rather than merging into.

Every case fits the five answers and the six levels without an escape
hatch. The ones I expected to need one - the hold, the touch injection,
the staged Escape, the capture of a hotkey in Settings - each came out as
a state (Pending, Spent) or a level.

## 10. Where the code goes

- `ui/interaction/`: the machine - the stack, the routing loop, the
  recognizer and its rules, the interactions, the command and binding
  tables. **No ImGui**: events in; session calls, editor changes and
  effects out. Tested without a frame.
- `Editor`: the state the interactions work on, moved out of `OverlayApp`
  - selection, tool, drawing mode, clipboard, the untouched drawing, the
  stack. `OverlayApp` keeps the views: it draws from `Editor`, applies the
  effects, and tells the machine what ImGui knows (`WantCaptureMouse`,
  `WantTextInput`, whether an ImGui drag is active).
- The platform window: one input callback (section 3), in the Win32
  window and the fake one.

## 11. Getting there

Each phase is a set of reviewable commits, and the app is whole after
each.

1. **Commands.** The `Command` type and its table, bindings, `Available`,
   scopes; keys, menus, the bar and hotkeys go through one `Dispatch`; the
   cheat sheet's key rows come from the table. The randomized test from
   `settle-hand` runs over every command instead of its own list. No
   behavior changes - then, as its own commits, the one new feature:
   the middle button and X1/X2 in the bindings and in Settings.
2. **One event stream.** The platform's input callback with time and
   modifiers; `OverlayApp` consumes it in place of `OnMouse` and the
   per-frame key reads, still with today's handlers behind it. The effect
   queue replaces the request flags. No behavior changes. The panels' own
   keys (the Overview's Escape, a key being captured, the HUD's digits)
   stay ImGui's until phase 3 makes them interactions.
3. **The stack.** Levels, routing, Pending, Spent, the recognizer's rules,
   the interactions one kind at a time, cancel on the session side,
   `Editor` split out. This is where the changes marked **Change** land,
   and where Escape starts cancelling. `Hand`, `PutDown`'s
   chain, `CloseTopmostPopover`, `noteOpenAtPress`, the gates in 7 and the
   gesture half of `OnMouse` go.
4. **Bursts.** The nudge and the wheel as interactions; `kBurstSeconds`,
   `lastBurst_` and friends go.

Tests grow with it: each row of section 9 as a scripted test against the
machine alone; the randomized test gains Escape anywhere (checking that a
cancel leaves the library exactly as the gesture found it) and, from
phase 3, runs against the machine directly, fast enough for thousands of
seeds.

### Phase 2 and the input grab

The event stream starts where the grab's work ends, and the grab has to
come out of phase 2 unchanged - countering above all, which took weeks of
measurement to get where it is (`docs/ARCHITECTURE.md`, "Taking input
back from the game"). Countering itself never passes through the app's
input: the raw-input sink, the bank and its injected corrections all
live on the hook thread, and what reaches the overlay is what the grab
posts or the window samples. Three places touch it all the same, and
phase 2 keeps each as it is:

- **Movement stays sampled once per frame.** Under the grab, the window
  emits one Move per frame from the virtual pointer, and moves are never
  posted: posting one per report flooded the queue at 1000 Hz. "Every
  event in arrival order" means those frame-time moves take their place
  in the stream when the frame emits them - not a move per raw report,
  which is also the shape that disturbed the raw-input rate countering
  depends on.
- **An event's modifiers come from the same two sources the frame reads
  now:** `GetAsyncKeyState` OR'd with the grab's own record of the keys
  it swallowed. Either alone is wrong under the grab - the backend's key
  messages need focus, and Alt-drag stopped working once before for
  exactly that.
- **Nothing new on the hook thread, and nothing that holds the frame
  back.** The heartbeat that lets the hooks stand down, the per-frame
  step sampling, and the order the window is shown and hidden in (the
  last correction reaches a raw-input listener about 145 ms before the
  window is gone) are timing the grab relies on. The stream is fed and
  drained on the app thread, where the messages already arrive.

Checked by the grab's own tests, unchanged, and then by hand in a game
with countering on, as its settings were tuned: if phase 2 turns out to
need anything inside the grab after all, that is a structural change in
the sense of the principle above, measured before and after with the
method its numbers came from.

### Phase 3, in steps

The order is chosen so that code that stays is moved once and code that
goes is never moved: what survives of `OverlayApp`'s input side goes into
`Editor` first, and the gesture code is replaced where it stands.

1. **Cancel on the session side.** `CancelPlacement`, `CancelErase`,
   `CancelStyleEdit`, `CancelShape` made public, and a freehand stroke
   cleared from the live layer (section 8). Nothing calls them yet.
2. **`Editor` split out.** The selection, the tool and its shapes, drawing
   mode, the clipboard, the untouched drawing, making snippets, the hit
   test and the commands move out of `OverlayApp` into `ui/editor`, with
   no ImGui: the display size and the clock are handed in, and what only
   a view can do - open a panel, show a message, ask for the keyboard -
   goes through a small interface `OverlayApp` implements. No behavior
   changes.
3. **The machine.** Levels, the event kinds of section 3, the five
   answers, routing, the scopes of 4.2 and the debug check, in
   `ui/interaction/`, tested with toy interactions. The window emits a
   `Tick` per frame on the stream's own clock. `OverlayApp` offers every
   event to the machine; until the levels above take over, the Canvas
   level hands them to today's handlers. No behavior changes.
4. **The Gesture level.** The recognizer's rules, Pending, Spent and each
   gesture of section 9 as an interaction. `Hand`, the gesture half of
   `OnMouse`, `MatureHeldPress` and `noteOpenAtPress` go - the last
   because the recognizer asks whether a note is open before anything at
   the press can close it. Escape starts cancelling, and the **Change**s
   of sections 6 and 9 land here.
5. **The Popup level.** Every popup is an interaction, closed through the
   effect queue; `CloseTopmostPopover` goes, and a popup claims every key
   but the global hotkeys (decision 2).
6. **The Text level and hotkeys.** A note being typed; global hotkeys
   arrive as `Hotkey` events.
7. **The Panel level.** The Overview and the cheat sheet, with their own
   keys, and the Text interactions inside the Overview - a name being
   edited, a key being captured - with it: while the Overview's Escape is
   ImGui's, a capture on the Text level would unbind a key with the same
   Escape that closes the panel. `KeyReaches` goes, since reaching is now
   the stack's answer, and `CompletesAHotkeyCapture` with the capture.
8. **The Mode level.** Drawing mode and a creation tool in hand; Escape's
   stages come from passing down, and `PutDown`'s chain goes. After the
   levels above it rather than before (as first planned): once drawing
   mode answers Escape, a popup, a note or a panel has to be there above
   it to claim Escape first, or Escape in a menu would leave drawing mode.
9. **Lifecycle.** Shown, Hidden, ViewOnly and SessionEnding as events,
   with the All scope; `SettleHand`, `SwitchToCanvasSettled` and
   `SetViewOnly`'s settling go - one `Editor::Settle(scope)` in their place.
10. **Widgets.** An ImGui drag as a Widget gesture: Escape clears it, and
    restores a slider's value.
11. **The cases as tests.** Each row of section 9 as a scripted test
    against the machine and `Editor` alone, and the randomized test run
    there with Escape anywhere, for thousands of seeds.

Phase 3 was done on 2026-09-26, in these steps; what building it found
wrong in the tables above is recorded where it was corrected ("Found
while building phase 3").

## 12. Decisions

Settled on review (2026-09-26):

1. **Escape while typing a note keeps the text.** It leaves the editing,
   the way it leaves any other mode; Undo is the way to take typing back.
   The Text row of section 5 says *finish*, not *cancel*.
2. **No undo or redo while a popup is up.** **Change:** today they run
   under a context menu, the Properties popover or the color chooser, and
   the popup stays up over a canvas that has changed under it. The Popup
   level claims them. The same reasoning covers the tool, clipboard and
   create keys, which also act on the canvas under the popup; proposed
   rule: a popup passes only global hotkeys down, and claims every other
   key (Escape closes it). Delete and the arrows are refused under a
   popup already today.
3. **Drawing mode survives being put away**, as today, and ends in
   view-only mode. That is what the Hand and All scopes say.
4. **Mouse buttons are bindable.** The binding table takes the middle
   button and X1/X2, with modifiers, from phase 1, and Settings > Hotkeys
   offers them alongside keys - "press the key or button you want".

5. **Phase 2 is the one with a platform change.** The fallback kept in
   reserve - stamping events and ordering them within a frame - was not
   needed: the single stream went in without touching the input grab
   (settled with phase 2).
