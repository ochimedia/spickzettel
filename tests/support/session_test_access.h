#pragma once

#include "core/session/session.h"

namespace sz::core {

// How a test reaches the model a session holds, to set a library up
// directly - a canvas with snippets on it, a picture marked stored - rather
// than command by command. What is changed through it is not a command:
// nothing is filed on the history, which is what setting up is.
struct SessionTestAccess {
    static CanvasManager& Model(Session& session) { return session.Model(); }
};

}  // namespace sz::core

namespace sz::test {

inline core::CanvasManager& Model(core::Session& session) { return core::SessionTestAccess::Model(session); }

}  // namespace sz::test
