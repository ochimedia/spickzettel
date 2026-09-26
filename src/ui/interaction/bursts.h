#pragma once

// The bursts of docs/INTERACTIONS.md, on the Gesture level: steps that
// come one after another - the arrow keys' nudges, the wheel's notches -
// held open on the session as one placement or one style edit, and filed
// as one step when a second passes without another (section 5). What
// else comes ends them: a press, a command, the wheel of another kind.
// Escape takes one back to where it began.

#include "ui/editor.h"
#include "ui/interaction/machine.h"

namespace sz::ui {

// The arrow keys' nudges, from the first arrow pressed until a second
// passes without one - a key held down and repeating, or pressed again and
// again. Each is the arrow's command, run as a step of the burst
// (Editor::Step). Escape cancels it while an arrow is held; once they are
// all up, it ends the burst and goes on, putting the hand down a stage as
// it did before - presses already let go of are not taken back.
class NudgeBurst final : public Interaction {
public:
    Level level() const override { return Level::Gesture; }
    const char* Name() const override { return "NudgeBurst"; }
    void Begin(const Event& event, Editor& editor) override;
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override;

private:
    // A nudge, if `event` is an arrow's: run, and the key noted as held.
    bool Nudge(const Event& event, Editor& editor);

    double lastStep_ = 0.0;
    // The arrows held down, which Escape cancels the burst under.
    uint8_t held_ = 0;
};

// The wheel's notches of one kind - the selection's size or its opacity
// (Editor::WheelKind) - until a second passes without one. A notch of
// another kind ends it, and begins its own.
class WheelBurst final : public Interaction {
public:
    explicit WheelBurst(Editor::WheelKind kind) : kind_(kind) {}
    Level level() const override { return Level::Gesture; }
    const char* Name() const override { return "WheelBurst"; }
    Editor::WheelKind Kind() const { return kind_; }
    void Begin(const Event& event, Editor& editor) override;
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override;

private:
    Editor::WheelKind kind_;
    double lastStep_ = 0.0;
};

}  // namespace sz::ui
