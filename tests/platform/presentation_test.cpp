#include "platform/presentation.h"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace sz::platform {
namespace {

using S = PresentationStep;

// A window, as far as the steps can move it: whether it is shown, whether
// it counts as click-through, and its styles, focus and grab - with every
// rule of docs/OVERLAY_STATES.md, section 7, checked as the steps are
// carried out.
struct Model {
    bool visible = false;
    bool countsClickThrough = false;
    bool styles = false;
    bool everShown = false;
    bool holdsFocus = false;
    bool heldFocusAtStart = false;
    bool grab = false;
    bool cameraSettled = false;
    std::vector<std::string> broken;

    void Carry(S step) {
        switch (step) {
            case S::SettleCamera:
                cameraSettled = true;
                break;
            case S::RestoreBorrowedNoActivate:
                break;
            case S::CountAsClickThrough:
                countsClickThrough = true;
                break;
            case S::CountAsInteractive:
                countsClickThrough = false;
                break;
            case S::ClickThroughStylesOn:
                if (!everShown) {
                    broken.push_back("click-through styles on a window never shown");
                }
                if (grab && !cameraSettled) {
                    broken.push_back("the game revealed with the camera unsettled");
                }
                styles = true;
                break;
            case S::ClickThroughStylesOff:
                styles = false;
                break;
            case S::Show:
                visible = true;
                everShown = true;
                if (styles && !countsClickThrough) {
                    broken.push_back("shown interactive with the click-through styles still on");
                }
                break;
            case S::Hide:
                if (grab && !cameraSettled) {
                    broken.push_back("the game revealed with the camera unsettled");
                }
                visible = false;
                break;
            case S::TakeFocus:
                if (!visible) {
                    broken.push_back("focus taken by a hidden window");
                }
                holdsFocus = true;
                break;
            case S::HandFocusBack:
                if (heldFocusAtStart || holdsFocus) {
                    holdsFocus = false;
                }
                break;
            case S::ClaimCursor:
                if (!visible || countsClickThrough || !grab) {
                    broken.push_back("the cursor claimed before the window takes input");
                }
                break;
            case S::ClaimFront:
            case S::ForgetKeys:
            case S::PlacePointer:
                break;
            case S::RefreshGrab:
                grab = visible && !countsClickThrough;
                break;
        }
    }
};

// `wasClickThrough`, for a hidden window: whether it went down from
// click-through, keeping the styles - or was never shown at all.
Model From(Presentation presentation, bool holdsFocus, bool wasClickThrough) {
    Model model;
    model.visible = presentation != Presentation::Hidden;
    model.everShown = model.visible || wasClickThrough;
    model.countsClickThrough =
        presentation == Presentation::ClickThrough || (presentation == Presentation::Hidden && wasClickThrough);
    model.styles = model.countsClickThrough;
    model.grab = presentation == Presentation::Interactive;
    model.holdsFocus = holdsFocus && presentation == Presentation::Interactive;
    model.heldFocusAtStart = model.holdsFocus;
    return model;
}

const char* Name(Presentation presentation) {
    switch (presentation) {
        case Presentation::Hidden:
            return "Hidden";
        case Presentation::ClickThrough:
            return "ClickThrough";
        case Presentation::Interactive:
            return "Interactive";
    }
    return "?";
}

bool Has(const std::vector<S>& steps, S step) { return std::find(steps.begin(), steps.end(), step) != steps.end(); }

size_t IndexOf(const std::vector<S>& steps, S step) {
    return static_cast<size_t>(std::find(steps.begin(), steps.end(), step) - steps.begin());
}

const Presentation kAll[] = {Presentation::Hidden, Presentation::ClickThrough, Presentation::Interactive};

// Every pair, with and without "Don't steal focus", from a window holding
// focus and from one that is not: each plan ends where it says, and breaks
// none of the rules on the way.
TEST(PresentationStepsTest, EveryPlanEndsWhereItSaysAndBreaksNoRule) {
    for (const Presentation from : kAll) {
        for (const Presentation to : kAll) {
            for (const bool noActivate : {false, true}) {
                for (const int variant : {0, 1, 2, 3}) {
                    const bool heldFocus = (variant & 1) != 0;
                    const bool wasClickThrough = (variant & 2) != 0;
                    SCOPED_TRACE(std::string(Name(from)) + " -> " + Name(to) + (noActivate ? ", no-activate" : "") +
                                 (heldFocus ? ", holding focus" : "") + (wasClickThrough ? ", was click-through" : ""));
                    const std::vector<S> steps = PresentationSteps(from, to, noActivate);
                    Model model = From(from, heldFocus && !noActivate, wasClickThrough);
                    for (const S step : steps) {
                        model.Carry(step);
                    }
                    EXPECT_TRUE(model.broken.empty()) << (model.broken.empty() ? "" : model.broken.front());
                    EXPECT_EQ(model.visible, to != Presentation::Hidden);
                    if (to != Presentation::Hidden) {
                        EXPECT_EQ(model.countsClickThrough, to == Presentation::ClickThrough);
                        EXPECT_EQ(model.styles, to == Presentation::ClickThrough);
                    }
                    EXPECT_EQ(model.grab, to == Presentation::Interactive) << "the grab runs exactly while interactive";
                    if (to != Presentation::Interactive) {
                        EXPECT_FALSE(model.holdsFocus) << "focus held is handed back on leaving interactive";
                    }
                }
            }
        }
    }
}

// Nothing to do for what the window already is: a view-mode switch to the
// pinned view, or the pinned view to view mode, moves no focus at all (C2).
TEST(PresentationStepsTest, APresentationToItselfIsNoStepAtAll) {
    for (const Presentation presentation : kAll) {
        EXPECT_TRUE(PresentationSteps(presentation, presentation, false).empty()) << Name(presentation);
        EXPECT_TRUE(PresentationSteps(presentation, presentation, true).empty()) << Name(presentation);
    }
}

// The grab starts for a window that is visible and not click-through, so
// a window coming up click-through counts as such before it is shown: it
// used to come up as edit mode for a moment, starting the grab and taking
// focus (C1).
TEST(PresentationStepsTest, NoPlanStartsTheGrabOrTakesFocusForAClickThroughWindow) {
    for (const Presentation from : kAll) {
        for (const bool noActivate : {false, true}) {
            const std::vector<S> steps = PresentationSteps(from, Presentation::ClickThrough, noActivate);
            EXPECT_FALSE(Has(steps, S::TakeFocus)) << Name(from);
            if (Has(steps, S::Show)) {
                EXPECT_LT(IndexOf(steps, S::CountAsClickThrough), IndexOf(steps, S::Show)) << Name(from);
            }
        }
    }
}

// Focus is taken only interactive, and never under no-activate.
TEST(PresentationStepsTest, FocusIsTakenOnlyInteractiveAndNeverUnderNoActivate) {
    for (const Presentation from : kAll) {
        for (const Presentation to : kAll) {
            EXPECT_FALSE(Has(PresentationSteps(from, to, true), S::TakeFocus));
            EXPECT_EQ(Has(PresentationSteps(from, to, false), S::TakeFocus),
                      to == Presentation::Interactive && from != to);
        }
    }
}

// Every way into interactive starts from no keys held, the pointer where
// the cursor is, and the cursor the window's - switching from view mode in
// place included, where a first click used to hover nothing (C4). Every
// way up claims the front.
TEST(PresentationStepsTest, EveryWayIntoInteractiveForgetsKeysAndPlacesThePointer) {
    for (const Presentation from : {Presentation::Hidden, Presentation::ClickThrough}) {
        for (const bool noActivate : {false, true}) {
            const std::vector<S> steps = PresentationSteps(from, Presentation::Interactive, noActivate);
            EXPECT_TRUE(Has(steps, S::ForgetKeys)) << Name(from);
            EXPECT_TRUE(Has(steps, S::PlacePointer)) << Name(from);
            // And the OS cursor is the window's, to hide under the software
            // pointer: switching in place from view mode left it showing.
            EXPECT_TRUE(Has(steps, S::ClaimCursor)) << Name(from);
        }
    }
    for (const Presentation to : {Presentation::ClickThrough, Presentation::Interactive}) {
        EXPECT_TRUE(Has(PresentationSteps(Presentation::Hidden, to, false), S::ClaimFront)) << Name(to);
        EXPECT_TRUE(Has(PresentationSteps(Presentation::Hidden, to, false), S::ForgetKeys)) << Name(to);
    }
}

// Focus is handed back by the one step that checks it was held, and a
// borrowed no-activate bit is put back before the window goes.
TEST(PresentationStepsTest, LeavingInteractiveHandsFocusBackAndHidingPutsTheBitBack) {
    for (const Presentation to : {Presentation::Hidden, Presentation::ClickThrough}) {
        EXPECT_TRUE(Has(PresentationSteps(Presentation::Interactive, to, false), S::HandFocusBack)) << Name(to);
    }
    const std::vector<S> hide = PresentationSteps(Presentation::Interactive, Presentation::Hidden, true);
    EXPECT_LT(IndexOf(hide, S::RestoreBorrowedNoActivate), IndexOf(hide, S::Hide));
    EXPECT_LT(IndexOf(hide, S::SettleCamera), IndexOf(hide, S::Hide));
}

}  // namespace
}  // namespace sz::platform
