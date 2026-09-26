#include "app/overlay_states.h"

namespace sz::app {

namespace {

bool IsSession(OverlayState state, const OverlayFacts& facts) {
    return state == OverlayState::Edit || (state == OverlayState::View && facts.viewHasSession);
}

// From `from` to `to`, the way the window goes and whether a session starts
// or ends on the way - everything a cell says once its target is chosen.
OverlayTransition Between(OverlayState from, OverlayState to, bool throughHidden, const OverlayFacts& facts) {
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
    // A View entered in place from a state without a session has none of
    // its own yet: it is one coming up from Hidden, or carrying on Edit's.
    const bool toSession = to == OverlayState::Edit ||
                           (to == OverlayState::View && (from == OverlayState::Hidden || IsSession(from, facts)));
    transition.endsSession = IsSession(from, facts) && !toSession;
    transition.startsSession = !IsSession(from, facts) && toSession;
    return transition;
}

OverlayTransition Stay(OverlayState state) {
    OverlayTransition transition;
    transition.from = state;
    transition.to = state;
    return transition;
}

// Where putting the overlay away leads, and how: section 2's Away.
OverlayTransition Away(OverlayState from, const OverlayFacts& facts) {
    if (!facts.pinnedHere) {
        return Between(from, OverlayState::Hidden, false, facts);
    }
    // Only what is drawn changes from View. From Edit it goes through
    // hidden, as today - C7 makes it in place.
    return Between(from, OverlayState::Pinned, from == OverlayState::Edit, facts);
}

// Edit, from wherever: up, in place from a View with a session, and
// through hidden from anything else that is up, to come up with a profile.
OverlayTransition ToEdit(OverlayState from, const OverlayFacts& facts) {
    const bool inPlace = IsSession(from, facts);
    return Between(from, OverlayState::Edit, !inPlace, facts);
}

}  // namespace

OverlayTransition Next(OverlayState state, OverlayRequest request, const OverlayFacts& facts) {
    switch (request) {
        case OverlayRequest::Edit:
            return state == OverlayState::Edit ? Away(state, facts) : ToEdit(state, facts);
        case OverlayRequest::View:
            if (state == OverlayState::View) {
                return Away(state, facts);
            }
            return Between(state, OverlayState::View, false, facts);
        case OverlayRequest::QuickCapture:
            // Edit again in place from Edit: the frozen screen is taken again.
            return ToEdit(state, facts);
        case OverlayRequest::SilentCapture:
            if (state == OverlayState::Hidden && facts.messagesWhileHidden) {
                return Between(state, OverlayState::Notice, false, facts);
            }
            return Stay(state);
        case OverlayRequest::NoticeFaded:
            // Written as Hidden, though it is Away: a notice comes up only
            // from Hidden, over a canvas the capture has just made.
            if (state == OverlayState::Notice) {
                return Between(state, OverlayState::Hidden, false, facts);
            }
            return Stay(state);
        case OverlayRequest::Restart:
            if (state == OverlayState::Edit) {
                OverlayTransition transition = Between(state, state, true, facts);
                transition.restart = true;
                return transition;
            }
            return Stay(state);
        case OverlayRequest::Start:
            if (state != OverlayState::Hidden) {
                return Stay(state);
            }
            if (facts.firstRun) {
                return Between(state, OverlayState::Edit, false, facts);
            }
            return facts.pinnedHere ? Between(state, OverlayState::Pinned, false, facts) : Stay(state);
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
