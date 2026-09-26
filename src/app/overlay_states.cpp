#include "app/overlay_states.h"

namespace sz::app {

namespace {

bool IsSession(OverlayState state) { return state == OverlayState::View || state == OverlayState::Edit; }

// From `from` to `to`, the way the window goes and whether a session starts
// or ends on the way - everything a cell says once its target is chosen.
OverlayTransition Between(OverlayState from, OverlayState to, bool throughHidden) {
    OverlayTransition transition;
    transition.from = from;
    transition.to = to;
    if (from == OverlayState::Hidden) {
        transition.route = Route::Up;
    } else if (to == OverlayState::Hidden) {
        transition.route = Route::Down;
    } else {
        transition.route = throughHidden ? Route::ThroughHidden : Route::InPlace;
    }
    // Every View is a session (C3): one entered in place from the pinned
    // view or a notice starts its own there.
    transition.endsSession = IsSession(from) && !IsSession(to);
    transition.startsSession = !IsSession(from) && IsSession(to);
    return transition;
}

OverlayTransition Stay(OverlayState state) {
    OverlayTransition transition;
    transition.from = state;
    transition.to = state;
    return transition;
}

// Where putting the overlay away leads, and how: section 2's Away. The
// pinned view is in place from either mode - only what is drawn changes,
// and from Edit the window goes click-through (C7).
OverlayTransition Away(OverlayState from, const OverlayFacts& facts) {
    return Between(from, facts.pinnedHere ? OverlayState::Pinned : OverlayState::Hidden, false);
}

// Edit, from wherever: up, in place within a session, and through hidden
// from the pinned view or a notice.
OverlayTransition ToEdit(OverlayState from) { return Between(from, OverlayState::Edit, !IsSession(from)); }

}  // namespace

OverlayTransition Next(OverlayState state, OverlayRequest request, const OverlayFacts& facts) {
    switch (request) {
        case OverlayRequest::Edit:
            return state == OverlayState::Edit ? Away(state, facts) : ToEdit(state);
        case OverlayRequest::View:
            if (state == OverlayState::View) {
                return Away(state, facts);
            }
            return Between(state, OverlayState::View, false);
        case OverlayRequest::QuickCapture:
            // Edit again in place from Edit: the frozen screen is taken again.
            return ToEdit(state);
        case OverlayRequest::SilentCapture:
            if (state == OverlayState::Hidden && facts.messagesWhileHidden) {
                return Between(state, OverlayState::Notice, false);
            }
            return Stay(state);
        case OverlayRequest::NoticeFaded:
            // Written as Hidden, though it is Away: a notice comes up only
            // from Hidden, over a canvas the capture has just made.
            if (state == OverlayState::Notice) {
                return Between(state, OverlayState::Hidden, false);
            }
            return Stay(state);
        case OverlayRequest::Restart:
            if (state == OverlayState::Edit) {
                OverlayTransition transition = Between(state, state, true);
                transition.restart = true;
                return transition;
            }
            return Stay(state);
        case OverlayRequest::Start:
            if (state != OverlayState::Hidden) {
                return Stay(state);
            }
            if (facts.firstRun) {
                return Between(state, OverlayState::Edit, false);
            }
            return facts.pinnedHere ? Between(state, OverlayState::Pinned, false) : Stay(state);
    }
    return Stay(state);
}

const char* Name(OverlayState state) {
    switch (state) {
        case OverlayState::Hidden:
            return "Hidden";
        case OverlayState::Pinned:
            return "Pinned";
        case OverlayState::Notice:
            return "Notice";
        case OverlayState::View:
            return "View";
        case OverlayState::Edit:
            return "Edit";
    }
    return "?";
}

}  // namespace sz::app
