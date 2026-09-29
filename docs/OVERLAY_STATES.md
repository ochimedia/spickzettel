# Overlay states

Status: **built** (2026-09-26), in the phases of section 11: the
reference for how the overlay comes and goes, as `docs/INTERACTIONS.md`
is for input. Every behavior below is either what the app did before it
(unmarked, or said so) or a change (marked **Change**) that it made. All
were made but C6, which a condition of its own ruled out (section 10,
finding 1). "Today" means the app as of `fa49b05`, before the work. The
questions it was reviewed with, and their answers, are in section 12.

## The principle

The same as for input. How the overlay comes and goes is one structured,
well-defined machine, and that is a decision in its own right, not an
implementation detail: every pair of state and request has a written
answer (section 5), every transition does its work in one fixed order
(section 6), and the window is told what to be rather than which calls
to make, and applies it in the order only it knows (section 7).

A change that fits is an edit to a cell of the table. A change to the
structure - the states, the requests, the order of section 6, the
window's steps - has to be well founded and argued as this document
argues its own shape: the case that does not fit, why no cell can
express it, and what it does to the other rows. If the structure
changes, this document changes with it, first.

## 1. What this is for

The overlay has five states, and the code and `docs/ARCHITECTURE.md` name
three ("Two hotkeys drive three states"). The pinned view and the notice
were added later, each as an exception to the checks the others already
made. What state the overlay is in is spread over seven flags in three
objects:

- the window: visible, click-through (`inputPassthrough_`);
- `OverlayApp`: `viewOnly_`, `noticeOnly_`, `pinnedOnly_`;
- `TrayController`: `profileAppliedThisShowing_`, `sessionApp_`.

The controller decides by combining them - `ToggleMode` asks "visible,
and not a notice, and not the pinned view, and view-only is what was
asked" - and says in its header that it is "deliberately stateless", so
that no `mode_` can go stale. That argument held for three states. With
five, the flags are the state that goes stale: `profileAppliedThisShowing_`
exists because visible-and-view-only could not say whether a profile had
been applied, and `sessionApp_` is never cleared.

The work a transition does is written out again at each place that does
it, and the copies have drifted:

- The way down (settle the hand, release the frozen screen, hide) is in
  `PutAway`, `RestartOverlay` and `EnsureMode`.
- The click-through way up is in both `ShowNotice` and `ShowPinnedView`.
- The condition for freezing is in both `EnsureMode` and `MoveOverlayTo`.

The window's calls also have an order that only the window knows, and
callers get it wrong. `EnsureMode` shows the window and makes it
click-through afterwards. On the way up into view mode, that means it is
briefly edit mode. The show starts the input grab, and the passthrough
right after takes it down again. With "Don't steal focus" off, or over an
elevated application, the show also takes focus from the game and the
passthrough hands it back: exactly the focus-loss event games pause on.
`ShowClickThrough` was made to fix this for notices
(`docs/ARCHITECTURE.md`, "Notices"), but view mode still goes the old
way.

Two transitions run inside a frame: a notice hiding, and a restart
hiding and showing again. Both are called from within
`OverlayApp::OnFrame`. That works because the renderer finishes the
frame whatever happened during it - something to rely on, rather than to
design around.

The tests count the window's calls but do not record their order, and
the order is what went wrong.

## 2. Vocabulary

- **State**: Hidden, Pinned, Notice, View or Edit (section 3). This is
  the controller's whole state as far as coming and going is concerned.
- **Up**: every state but Hidden - the window is shown.
- **Away**: where putting the overlay away leads. It is Pinned when the
  current canvas has a pinned snippet, and Hidden otherwise, decided at
  the moment it is asked.
- **Session**: View and Edit. A session starts when one of them is
  entered from a state that is neither, and ends when it is left for
  one. A session has the application the overlay came up over, and so a
  profile. Hidden, Pinned and Notice have none.
- **Request**: what asks for a transition (section 4).
- **Route**: how the window gets from one state to the next:
  - *up*: from Hidden;
  - *down*: to Hidden;
  - *in place*: it stays up;
  - *through hidden*: down, then up again.
- **Presentation**: what the window is told to be - hidden, up
  click-through, or up interactive (section 7).

## 3. The states

| State | On screen | Input | Focus | Input grab | Session | Frozen screen | Frames |
|---|---|---|---|---|---|---|---|
| Hidden | nothing | - | left alone | off | none | none | none |
| Pinned | the current canvas's pinned snippets, and a message | click-through | never taken | off | none | none | idle, unless a message is up |
| Notice | a message, nothing else | click-through | never taken | off | none | none | every refresh, until it fades |
| View | the current canvas, and a message | click-through | handed back | off | yes | none | idle, unless a message is up |
| Edit | everything | the overlay's | taken, unless "Don't steal focus" (see `MustTakeFocusFrom`) | as the input options say | yes | when "Freeze screen while editing" is on | every refresh |

These hold after every transition and are checked in debug builds:

- the window is visible exactly when the state is not Hidden;
- it is click-through exactly in Pinned, Notice and View;
- there is a session exactly in View and Edit;
- a screen is frozen only in Edit;
- `OverlayApp`'s mode is the state.

## 4. Requests

| Request | From | Arrives | Notes |
|---|---|---|---|
| Edit | edit hotkey; tray "Toggle overlay" | as a message | through the input machine, which settles its scope first (`docs/INTERACTIONS.md`, section 7) |
| View | view hotkey | as a message | the same |
| Quick capture | its hotkey | as a message | captures, then asks for Edit |
| Silent capture | its hotkey | as a message | captures; says so |
| Notice faded | `OverlayApp` | in a frame | the notice's message has gone |
| Restart | the input options HUD | in a frame | a row read only on entry (Don't steal focus, Freeze screen while editing) was toggled, and its key is up |
| Settings changed | a setting committed | in a frame | |
| Displays changed | the OS | as a message | |
| Session ending | `WM_QUERYENDSESSION`; `WM_ENDSESSION` for a logoff | as a message | |
| Exit | tray Exit; `WM_CLOSE`; the Restart Manager | as a message | |
| Start | `TrayController::Start`, after `WinMain` has said what `Initialize` found | once | a first run, or not |

Hotkeys the input grab takes from the keyboard are posted to the host
window by the grab, so they arrive as messages too.

## 5. The table

Each cell gives the state the request leads to, and the route. A dash
means the request does nothing in that state.

| Request | Hidden | Pinned | Notice | View | Edit |
|---|---|---|---|---|---|
| Edit | Edit, up | Edit, through hidden | Edit, through hidden | Edit, in place (1) | Away (2) |
| View | View, up (3) | View, in place (4) | View, in place (4) | Away (2) | View, in place |
| Quick capture | capture; Edit, up | capture; Edit, through hidden | capture; Edit, through hidden | capture; Edit, in place | capture; Edit again, in place (5) |
| Silent capture | capture; Notice, up (6) | capture; stays (7) | capture; stays (8) | capture; stays | capture; stays |
| Notice faded | - | - | Hidden, down (9) | - | - |
| Restart | - | - | - | - (10) | Edit, through hidden, same session |

1. **Change** (C3). Today this is in place only when the View came up
   from Hidden. A View entered from Pinned or a Notice has no profile, so
   Edit from it goes through hidden to get one. With C3, every View has
   one.
2. Edit to Hidden is down. View to Pinned is in place: only what is
   drawn changes. View to Hidden is down. Edit to Pinned is in place too,
   a **Change** (C7): today it goes through hidden, and the pinned
   snippets blink.
3. **Change** (C1): the window comes up click-through. Today it comes up
   as edit mode and is made click-through afterwards (section 1).
4. **Change** (C3): a session starts in place, with the profile resolved
   for what is underneath now. Today the switch happens with no profile.
   Also **Change** (C2): focus is not moved. Today the switch hands focus
   to whatever had it when the pinned view or the notice came up,
   however long ago that was.
5. The frozen screen is taken again, as today: `EnsureMode` releases it
   and freezes again on every call (decided, section 12).
6. Only when "Show a message for a background capture while the overlay
   is hidden" is on. Otherwise the overlay stays Hidden and the message
   is dropped, never to be shown later.
7. The capture gets a canvas of its own, but the pinned view stays on the
   canvas it was showing. The message shows in the pinned view's frames.
8. The new message replaces the one showing, and the notice lasts until
   the new one fades.
9. Written as Hidden, though it is Away. A notice comes up only from
   Hidden, over the new capture's canvas, which has nothing pinned.
10. Only the HUD asks for a restart, and it is up only in Edit. This was
    unreachable before, so ignoring it changes nothing.

Four requests change no state, and do the same in every state:

- **Settings changed**:
  - The window gets the live values: "Don't steal focus" (with the
    elevated-application rule) and the input options. Each is compared
    with the value it last got, not with what is stored, which a profile
    may override.
  - While the overlay is up, it moves to the display the settings now
    choose. In Edit, a move takes the frozen screen again.
  - The settings file is written.
- **Displays changed**: the display is chosen again, and the window is
  moved there, whether up or hidden. A hidden window is moved too, so
  that a capture taken without showing it comes from the right display.
  In Edit, a move takes the frozen screen again.
- **Session ending**:
  - What the hand holds is ended, along with everything above the canvas
    (the All scope), and a settings file still owed is written.
  - The state does not change: the window stays up, and the frozen screen
    stays.
  - If the logoff is called off, the overlay carries on in its state,
    with everything settled.
- **Exit**: as Session ending, and then the host quits. The window is
  destroyed along with the host, and `Destroy` takes the input grab down
  first.

**Start**, from Hidden, once: on a first run, Edit, up, with the
tutorial starting (`docs/TUTORIAL.md`, section 7.6); otherwise, Away.

## 6. What a transition does, in order

One procedure for every cell. Where today does the same work, it is in
the same order unless said otherwise.

1. **Settle.**
   - Leaving View or Edit for Away, or for a restart: the Hand scope
     (`OverlayApp::Settle`).
   - Leaving Edit for View, Pinned or a Notice: this is done by the mode
     in step 5 (the All scope), and not here as well. For Pinned and a
     Notice that is a rule, not a leftover - section 10, finding 1.
2. **Release the frozen screen**, when leaving Edit - for any state,
   including a restart and a quick capture's re-entry.
3. **End the session**, when leaving View or Edit for a state without
   one: the application the session came up over is forgotten.
   **Change**, in bookkeeping only: today `sessionApp_` is kept until the
   next show replaces it, and nothing reads it in between.
4. **Choose the display, and make sure the window exists**, when coming
   up from Hidden. Through hidden, the window goes down first, and comes
   up from here as from Hidden. If the window cannot be made, the
   transition stops here and the state is Hidden. Only coming up can
   fail, since a window that is up already exists.
5. **Tell the overlay its mode** (`OverlayApp::SetMode`): what it draws,
   and, on leaving Edit for any other mode, the All scope settled. Down,
   the mode stays as it was, except that a
   notice going down leaves plain View, as today: a notice's mode is what
   reports its fade. A state entered from another is also said
   (`OverlayApp::OnModeEntered`), which the tutorial counts
   (`docs/TUTORIAL.md`, section 14.3).
6. **Start the session**, when entering View or Edit from a state without
   one:
   - ask what is underneath, and match a profile;
   - give the window the live "Don't steal focus" and the input options.

   A restart keeps its session, and resolves the profile again against
   the same application (`sessionApp_`'s reason, unchanged).
7. **Present the window** (section 7): up, down, in place or through
   hidden.
8. **Tell the overlay a session started** (`OnOverlayShown`). This
   happens whenever one does: from Hidden, in place from Pinned or a
   Notice (C3), and at a restart's showing. It does not happen for Pinned
   or a Notice coming up, as today.
9. **Freeze**, when entering Edit with the setting on. This comes after
   the window is presented, so the capture leaves out the overlay as it
   is going to be.

Step 6 comes before step 7 because "Don't steal focus" decides how the
window is shown, and because what is underneath has to be asked before
the window can be the answer.

Today, on the way up into View or Edit, the mode is set after the show
and after `OnOverlayShown`. On the way up into Pinned or a Notice, it is
set before the show. No frame runs in between. The two settles involved,
Shown with Hand and ViewOnly with All, end in the same place in either
order, so one order is used: the mode first. Phase 2's tests check this
rather than assume it.

## 7. The window: a presentation, applied in order

**Change** to the platform interface: `IOverlayWindow::Present(Presentation)`,
with `Presentation` one of Hidden, ClickThrough or Interactive, replaces
`Show`, `ShowClickThrough`, `Hide` and `SetInputPassthrough`. These stay
as they are:

- `SetEditModeNoActivate` and `SetEditModeInput`, which are settings and
  safe to call at any time;
- `IsVisible`, for the host's loop.

The window applies each change in an order that respects what it knows.
Each of these rules was learned by watching something fail on the real
thing:

- A window given `WS_EX_LAYERED` before its first show draws nothing. The
  click-through styles go on after the show.
- The grab starts for a window that is visible and not click-through. A
  window coming up click-through is counted as click-through before it
  is shown.
- `SW_SHOW` activates the window by itself, and nothing notes where
  focus came from. Every show is `SW_SHOWNOACTIVATE`, and focus is taken
  by one step of its own, which notes it.
- The camera correction is settled while the window still covers the
  game, before anything reveals the game: a hide, or a switch to
  click-through.
- Each time the window comes up, it claims the front of the topmost band,
  and the check in `RenderFrame` follows. Each time, the keys held are
  forgotten and ImGui's pointer is placed where the cursor is.

The steps for each pair:

| From → to | Steps |
|---|---|
| Hidden → Interactive | count as interactive, styles off; show; take focus unless no-activate; claim the front; forget keys, place the pointer; grab on; claim the cursor |
| Hidden → ClickThrough | count as click-through; show; click-through styles; claim the front; forget keys; grab off |
| Interactive → ClickThrough | settle the camera; count as click-through, styles on; grab off; hand focus back if held |
| ClickThrough → Interactive | count as interactive, styles off; take focus unless no-activate; forget keys, place the pointer; grab on; claim the cursor |
| up → Hidden | settle the camera; put back a borrowed no-activate bit; hide; grab off; hand focus back if held |
| same → same | nothing |

**Change** (C2), the focus rule: the window hands focus back only while
it holds focus, and only to the window it took focus from. That window
is recorded at the moment focus is taken - on a show, a switch to
interactive, no-activate turned off, or a text field borrowing focus -
not at the show. Today `Hide` already checks that it holds focus, but
the switch to click-through does not, and the record dates from the
show. That leads to two faults found by reading:

- Pinned or Notice to View hands focus to whatever was in front when
  the pinned view came up, possibly hours earlier.
- View to Edit in place, after the user has clicked into another window
  through view mode, gives focus back on hiding to the window from
  before, not to the one the user moved to.

Both were confirmed on the real desktop before phase 3, and are gone
after it, by a scripted check: a scratch instance with its own
`%APPDATA%`, two plain windows, and the hotkeys sent as key presses. The
same check confirmed C1: with "Don't steal focus" off, the view hotkey
from hidden moved the foreground to the overlay and straight back, and
no longer does.

**Change** (C4): ClickThrough → Interactive forgets the keys held and
places the pointer, as a show does. Today, switching from view to edit
in place does neither. A window with no focus hears of the cursor only
once it moves (the reason `RenderFrame` seeds the pointer after a show),
so a click before any movement hovered nothing. Found by reading, and
not confirmed by hand: what ImGui hovers is not visible from outside.

Found after phase 3, by hand: with the software pointer, switching from
view to edit mode in place left the Windows arrow on screen beside the
drawn pointer until the first click. The OS cursor is hidden by the
window answering `WM_SETCURSOR`, which Windows sends on a mouse message
over the window; a window shown under the cursor gets one, a window that
stops being click-through does not, and the grab lets no movement
through to cause one. The last step into interactive claims the cursor:
it is set to where it already is, which moves nothing and sends that
message.

The steps are planned by a pure function in `platform/presentation.h`,
with no OS headers: `PresentationSteps(from, to, noActivate)`. The Win32
window carries them out, one `PresentationStep` at a time. The function
is tested on every pair, with and without no-activate, from a window
holding focus and one not, with the rules above as properties
(`tests/platform/presentation_test.cpp`):

- no plan starts the grab for a click-through target;
- the styles never go on before a first show;
- the camera is settled before every step that reveals the game;
- focus is only handed back by a step that checks it is held.

## 8. Between frames

The controller changes state only between frames. Requests that arrive
as messages already come between frames. The three that arrive in a
frame are posted instead:

- notice faded;
- restart;
- the window's half of settings changed.

**Change** (C5): `IPlatformHost::Post(task)` runs a task on the app
thread after the current frame or message, and before the next frame.
- **Win32**: a queue, and a `PostMessage` to the host window for each
  task, which the loop dispatches before it draws. Each message runs the
  oldest task, so one a task posts runs after those already waiting.
- **The fake host**: holds posted tasks, and the headless app runs them
  after each frame it steps.

As a result, the notice hides, the restart happens, and a changed setting
reaches the window one frame later than today. This removes:

- the "safe mid-frame, since the renderer still ends the frame" reasoning
  in `HideNoticeIfDone` and at the HUD's restart;
- `OnOverlayShown` running inside a frame whose rest is skipped.

The settings file is still written in the frame. `ChangeHotkey` stays
synchronous: its caller needs the answer.

## 9. What changes

| | Change | What it fixes | Seen where |
|---|---|---|---|
| C1 | Up into View comes up click-through | the grab started and stopped; focus taken and handed back with "Don't steal focus" off, or over an elevated application | the view hotkey from Hidden |
| C2 | Focus is handed back only while held, to the window it was taken from | a stale foreground brought back; focus returned to the wrong window | the view hotkey in the pinned view or a notice; hiding after View to Edit |
| C3 | Every View has a session: from Pinned or a Notice, the profile is resolved in place | Edit from such a View hides and reshows (the pinned snippets blink); `profileAppliedThisShowing_` goes | the view hotkey in the pinned view, then the edit hotkey |
| C4 | ClickThrough to Interactive places the pointer and forgets keys | a first click that hovers nothing | View to Edit, clicking before moving |
| C5 | Requests from a frame are applied after it | transitions inside a frame | a notice fading; a HUD restart |
| C6 | *Not made* (section 10, finding 1): Pinned and Notice as away, entering them settling the hand, not everything | - | - |
| C7 | Edit to Pinned is in place | the pinned snippets blink off and on | the edit hotkey in edit mode, with a snippet pinned |

C3 relies on C2. After Pinned → View → Edit in place, the focus taken in
Edit has to be given back to the window it was taken from, not to the
one recorded when the pinned view came up. View mode itself does not
change under C3. The profileable settings (focus, the input options, the
freeze, the shortcuts) are all edit mode's, and nothing in view mode
reads them. With C3, `OnOverlayShown` runs when such a session starts
(section 6, step 8): a message waiting for the next showing appears
then, rather than at the next edit mode that comes up from Hidden.

## 10. Found while writing this

**Each decided in section 12:**

1. Putting the overlay away into the pinned view from Edit ends what
   edit mode left up - drawing mode, a panel, a popup. So does a notice
   that comes up after edit mode was put away. Both enter view-only,
   whose settle ends everything above the canvas. Putting it
   away into Hidden keeps them (`docs/INTERACTIONS.md`, decision 3). So
   whether drawing mode survives being put away depends on whether the
   canvas has a pinned snippet, or whether a silent capture happened
   since. **Kept, as a rule** - C6 was agreed on the condition that ImGui
   keeps a popup and a panel across frames that do not draw them, and it
   does not keep a popup. At the start of a frame, a focused window that
   was not active in the last one loses focus, and losing it closes the
   popups over it. The pinned view and a notice draw frames; edit mode's
   windows are not in them. Hidden draws none, and so keeps a popup until
   the next showing draws it. Settling the hand alone, the input machine
   would have gone on holding a popup that ImGui had closed behind its
   back. So entering the pinned view or a notice from edit mode ends
   everything above the canvas, as view mode does, and the tests
   `ThePinnedViewEndsWhatEditModeLeftUp` and
   `APopupSurvivesHiddenButNotFramesThatDoNotDrawIt` hold both halves of
   the reason. A panel does survive; ending only the popup was possible,
   and not what was agreed.
2. Edit to Pinned goes through hidden, so the pinned snippets blink off
   and on. It could be in place, like Edit to View. Changed: C7.
3. A quick capture in Edit takes the frozen screen again, so the
   background under the user jumps to what the game shows now. Kept.
4. The restart exists because "Don't steal focus" and "Freeze screen
   while editing" are read on entry. With the reconciliation of section
   5, both could be applied in place: the freeze taken or released, and focus handed
   back when no-activate turns on while the window holds it. The restart,
   and the HUD's wait for its key to come up, would then go. This is not
   proposed now: releasing a frozen screen in place reveals the game, and
   that needs the camera settled first, which is the input grab's timing.
   It would be a later change, measured as the grab's changes are. Left
   for later.
5. Found while building phase 4: with C3, the pinned view or a notice
   to Edit could go in place too, resolving the profile there as the
   view hotkey does. It still goes through hidden, as this document
   proposed nothing else; it would be a cell change.

**Documentation that is wrong today, corrected in phase 1:**

- `docs/ARCHITECTURE.md` says an unregistrable hotkey is fatal for the
  three that bring the overlay up. It is not: `Initialize` notes it and
  goes on (test `HotkeysAnotherApplicationOwnsAreNamedNotFatal`).
- "Two hotkeys drive three states" - there are five.

## 11. Where the code goes, and getting there

- **`app/overlay_states.*`**: the states, the requests, and the pure
  `Next(state, request, facts)`, which returns the target, the route, and
  whether a session starts or ends. `facts` covers: pinned snippets here,
  messages while hidden, a first run. No host and no window, so it is
  tested cell by cell.
- **`TrayController`**:
  - holds the state, the session (the application) and the display;
  - `Apply(Transition)` is section 6;
  - `ToggleMode`, `EnsureMode`, `PutAway`, `ShowNotice`, `ShowPinnedView`,
    `HideNoticeIfDone` and `RestartOverlay` go, and with them
    `profileAppliedThisShowing_`.
- **`OverlayApp`**: `SetMode(OverlayMode)`, with Edit, View, Pinned and
  Notice, replaces `SetViewOnly`, `SetNoticeOnly` and `SetPinnedOnly`.
  `IsViewOnly` and the others stay, derived from the mode. Hidden is not
  a mode: the overlay draws nothing then, and keeps the last mode.
- **`platform/presentation.h`**: the presentation and the step plan.
  `IOverlayWindow::Present` replaces the four calls. The Win32 window
  carries the plan out, and the fake window logs every call, in order.
- **`IPlatformHost::Post`**.

Each phase is a set of reviewable commits, and the app is whole after
each:

1. **Tests and documents first.** The call-logging fake window, and tests
   that pin today's order for each cell. `docs/ARCHITECTURE.md`'s tray
   section describes five states, and the hotkey paragraph is corrected.
   No behavior changes.
2. **The states.** `Next`, the controller's state, `Apply`, and
   `OverlayApp::SetMode`. Every path becomes `Apply(Next(...))`. No
   behavior changes. Until phase 4, the View state carries whether it
   has a session, which is what `profileAppliedThisShowing_` says today.
3. **The window presents** (C1, C2, C4). This is the platform phase, and
   gets the care that input phase 2 got: the grab's own tests unchanged,
   then checked by hand in a game with the grab and countering on, and
   the focus faults of C2 and C4 reproduced before and after.
4. **Sessions and away** (C3, C7; C6 was not made, see section 10,
   finding 1).
5. **Between frames** (C5).

Tests grow with it:

- `Next` on every cell of section 5;
- the step plan on every pair, with the properties of section 7;
- the controller's order (section 6) for each cell, from the call log;
- a randomized test that drives random requests through random states
  and checks the invariants of section 3 after each one, as the input
  machine's does;
- the 71 tray tests and the headless tests, unchanged except where a
  Change says otherwise.

## 12. Questions for review, and the answers

1. **Are Pinned and Notice "away"?** Recommended yes: entering them
   would offer what hiding offers (the Hand scope), not what view mode
   offers (All), so drawing mode, a panel or a popup survive being put
   away whether or not something is pinned, as decision 3 of
   `docs/INTERACTIONS.md` says. The machine gets no events in either
   state (`OverlayApp::OnInput` drops everything while view-only), just
   as in Hidden. First a headless test has to show that ImGui keeps a
   popup or panel that frames stop drawing; if it does not, today's
   behavior stays and is written down as a rule.
   **Answer: yes, as recommended** - C6, in phase 4. **Result:** the
   condition failed, so today's behavior stays, as a rule (section 10,
   finding 1).
2. **Edit to Pinned in place** (finding 2)? Recommended yes: the same
   window steps as Edit to View, and no blink.
   **Answer: in place** - C7, in phase 4.
3. **Quick capture in Edit**: take the frozen screen again (today), or
   leave it? A capture while the screen is frozen is cut from the frozen
   picture, so taking it again gives the next quick capture a fresh
   picture, and leaving it keeps the background steady.
   **Answer: take it again**, as today.
4. **Restart** (finding 4): leave it for a later, measured change?
   **Answer: yes, later.**
