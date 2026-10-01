#pragma once

#include "core/session/session.h"

namespace sz::core {

// How a test reaches the model a session holds, to set a library up
// directly - a canvas with snippets on it, a picture marked stored - rather
// than command by command. What is changed through it is not a command:
// nothing is filed on the history, which is what setting up is.
struct SessionTestAccess {
    static CanvasManager& Model(Session& session) { return session.Model(); }
    // Whether the hand has left a gesture open on the session - a move or
    // resize, an erase, a shape or a note being typed - that nothing has
    // ended yet. A style edit is the Properties popover's, not the hand's.
    static bool HandGestureOpen(const Session& session) {
        return session.placement_.has_value() || session.erasing_ || !session.shapeItems_.empty() ||
               session.textEditItemId_.has_value();
    }
    // Whether a frozen screen is held - see Session::FreezeScreen. Only
    // ever in edit mode (docs/OVERLAY_STATES.md, section 3).
    static bool HoldsFrozenScreen(const Session& session) { return !session.frozenScreenPixels_.empty(); }
};

}  // namespace sz::core

namespace sz::test {

inline core::CanvasManager& Model(core::Session& session) { return core::SessionTestAccess::Model(session); }
inline bool HandGestureOpen(const core::Session& session) { return core::SessionTestAccess::HandGestureOpen(session); }
inline bool HoldsFrozenScreen(const core::Session& session) {
    return core::SessionTestAccess::HoldsFrozenScreen(session);
}

}  // namespace sz::test
