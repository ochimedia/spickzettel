#pragma once

namespace sz::app {

// How the overlay comes and goes, as one machine: docs/OVERLAY_STATES.md.
// The states and the requests are its sections 3 and 4; Next is its table,
// section 5, and TrayController::Apply is section 6. Nothing here knows of
// a window or a host, so the table is tested cell by cell.

// Section 3. Every state but Hidden is up; View and Edit are sessions.
enum class OverlayState { Hidden, Pinned, Notice, View, Edit };

// Section 4: what asks for a transition. The four that change no state -
// a setting, the displays, the session ending, exit - are not here: the
// controller handles them without one.
enum class OverlayRequest {
    Edit,
    View,
    // After the capture, which the controller takes first.
    QuickCapture,
    SilentCapture,
    NoticeFaded,
    // The Behavior panel's, for a row only read on the way up.
    Restart,
    // Once, from Initialize.
    Start,
};

// Section 2: how the window gets from one state to the next.
enum class Route {
    // No transition: the request does nothing in this state.
    Stay,
    Up,
    Down,
    InPlace,
    ThroughHidden,
};

// What a cell needs to know besides the state and the request.
struct OverlayFacts {
    // The current canvas has a pinned snippet: Away is Pinned, not Hidden.
    bool pinnedHere = false;
    // "Say so when the overlay is hidden".
    bool messagesWhileHidden = false;
    bool firstRun = false;
};

struct OverlayTransition {
    OverlayState from = OverlayState::Hidden;
    OverlayState to = OverlayState::Hidden;
    Route route = Route::Stay;
    // A session ends when View or Edit is left for a state without one, and
    // starts when one is entered from such a state.
    bool endsSession = false;
    bool startsSession = false;
    // Through hidden in the same session - the restart, which keeps what
    // the session came up over.
    bool restart = false;
};

OverlayTransition Next(OverlayState state, OverlayRequest request, const OverlayFacts& facts);

const char* Name(OverlayState state);

}  // namespace sz::app
