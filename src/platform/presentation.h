#pragma once

#include <vector>

namespace sz::platform {

// What the overlay window is told to be - see IOverlayWindow::Present and
// docs/OVERLAY_STATES.md, section 7.
enum class Presentation {
    Hidden,
    // Up, every click going to whatever is underneath, and never focused:
    // the pinned view, a notice, view mode.
    ClickThrough,
    // Up, and the overlay's input: edit mode.
    Interactive,
};

// One thing the window does on the way from one presentation to another.
// Planned here, carried out by the backend, in the order planned.
enum class PresentationStep {
    // Puts the game's camera back and gives it time to show that, while the
    // window still covers the game - before anything reveals it.
    SettleCamera,
    // A text field that took WS_EX_NOACTIVATE off to borrow focus gives the
    // bit back, since it will never close to do so itself.
    RestoreBorrowedNoActivate,
    // Whether the window counts as click-through, which is what the input
    // grab and hit-testing go by. Set before a show, so the show starts the
    // grab only for a window that is to take input.
    CountAsClickThrough,
    CountAsInteractive,
    // The window styles that make it click-through at the OS level. On only
    // once the window is shown: a window with WS_EX_LAYERED at its first
    // show draws nothing at all.
    ClickThroughStylesOn,
    ClickThroughStylesOff,
    // SW_SHOWNOACTIVATE. Focus is only ever taken by TakeFocus, which notes
    // where from; SW_SHOW would take it by itself, unnoted.
    Show,
    Hide,
    // Takes focus, noting the window it is taken from - unless this window
    // holds it already, when the note it has stands.
    TakeFocus,
    // Gives focus back to the window it was taken from, if this window held
    // it when the change began; the note is dropped either way.
    HandFocusBack,
    // The front of the topmost band, without taking activation.
    ClaimFront,
    // No key goes down unseen while away that goes up seen.
    ForgetKeys,
    // ImGui's pointer is put where the cursor is on the next frame: a window
    // without focus hears of the cursor only once it moves.
    PlacePointer,
    // The input grab on or off, for whether the window is now visible and
    // not click-through.
    RefreshGrab,
};

// The steps from `from` to `to`, in order - docs/OVERLAY_STATES.md,
// section 7. `noActivate` is "Don't steal focus" as the window has it: with
// it, focus is never taken. Nothing for a presentation to itself.
inline std::vector<PresentationStep> PresentationSteps(Presentation from, Presentation to, bool noActivate) {
    using S = PresentationStep;
    std::vector<S> steps;
    if (from == to) {
        return steps;
    }
    if (to == Presentation::Hidden) {
        return {S::SettleCamera, S::RestoreBorrowedNoActivate, S::Hide, S::RefreshGrab, S::HandFocusBack};
    }
    if (from == Presentation::Hidden) {
        if (to == Presentation::ClickThrough) {
            return {S::CountAsClickThrough, S::Show, S::ClickThroughStylesOn, S::ClaimFront, S::ForgetKeys,
                    S::RefreshGrab};
        }
        steps = {S::CountAsInteractive, S::ClickThroughStylesOff, S::Show};
        if (!noActivate) {
            steps.push_back(S::TakeFocus);
        }
        steps.insert(steps.end(), {S::ClaimFront, S::ForgetKeys, S::PlacePointer, S::RefreshGrab});
        return steps;
    }
    if (to == Presentation::ClickThrough) {
        return {S::SettleCamera, S::CountAsClickThrough, S::ClickThroughStylesOn, S::RefreshGrab, S::HandFocusBack};
    }
    // Click-through to interactive, in place.
    steps = {S::CountAsInteractive, S::ClickThroughStylesOff};
    if (!noActivate) {
        steps.push_back(S::TakeFocus);
    }
    steps.insert(steps.end(), {S::ForgetKeys, S::PlacePointer, S::RefreshGrab});
    return steps;
}

}  // namespace sz::platform
