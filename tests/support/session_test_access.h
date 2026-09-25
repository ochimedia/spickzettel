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
        return session.placement_.has_value() || session.eraseItemId_.has_value() ||
               session.shapeItemId_.has_value() || session.textEditItemId_.has_value();
    }
};

}  // namespace sz::core

namespace sz::test {

inline core::CanvasManager& Model(core::Session& session) { return core::SessionTestAccess::Model(session); }
inline bool HandGestureOpen(const core::Session& session) { return core::SessionTestAccess::HandGestureOpen(session); }

}  // namespace sz::test
