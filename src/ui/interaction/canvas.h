#pragma once

// The Canvas level of docs/INTERACTIONS.md: the root of the stack, always
// there, and what an event comes to once every level above has passed it.
// A left or right press is what the recognizer says it is (see
// RecognizePress); a key, or a mouse button a shortcut may be, starts the
// command it is bound to (section 7 - reaching it is every level above
// having passed it); the wheel is the canvas's (see Editor::Wheel); a
// global hotkey starts its command. Everything else ends here, dropped: a
// hover, the rest of a press nothing took, a key bound to nothing.

#include "ui/interaction/machine.h"

namespace sz::ui {

class CanvasLevel final : public Interaction {
public:
    Level level() const override { return Level::Canvas; }
    const char* Name() const override { return "Canvas"; }
    Answer Offer(const Event& event, Editor& editor) override;
    // The root is never ended.
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}
};

}  // namespace sz::ui
