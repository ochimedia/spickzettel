// The overlay's states and transitions, cell by cell - docs/OVERLAY_STATES.md,
// sections 3 and 5 - with the window's calls in the order they are made
// (section 6). Each cell of the table is a row here, and each row says
// which state the request leads to and what the window was told on the way.

#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fakes/headless_app.h"
#include "support/session_test_access.h"

namespace sz::test {
namespace {

// Long enough for a message to have faded - see ShowActionToast.
constexpr int kFramesPastAToast = 200;

enum class State { Hidden, Pinned, Notice, View, Edit };

const char* Name(State state) {
    switch (state) {
        case State::Hidden:
            return "Hidden";
        case State::Pinned:
            return "Pinned";
        case State::Notice:
            return "Notice";
        case State::View:
            return "View";
        case State::Edit:
            return "Edit";
    }
    return "?";
}

// Where a row starts. The state alone is not enough: which way the overlay
// is put away depends on whether the current canvas has a pinned snippet,
// and what a View does next on whether it has a session.
enum class From {
    Hidden,
    Pinned,
    Notice,
    View,
    ViewWithAPinnedSnippet,
    // View entered from the pinned view, in place: no session, no profile.
    ViewFromPinned,
    Edit,
    EditWithAPinnedSnippet,
};

enum class Request { Edit, View, QuickCapture, SilentCapture, NoticeFaded };

const char* Name(Request request) {
    switch (request) {
        case Request::Edit:
            return "Edit";
        case Request::View:
            return "View";
        case Request::QuickCapture:
            return "QuickCapture";
        case Request::SilentCapture:
            return "SilentCapture";
        case Request::NoticeFaded:
            return "NoticeFaded";
    }
    return "?";
}

class OverlayStatesTest : public HeadlessAppTest {
protected:
    // Freezing on, and a capture that returns pixels: the freeze is a step
    // of its own in section 6, and the window is where it shows.
    static AppConfig Config() {
        AppConfig config = DefaultConfig();
        config.freezeScreenInEditMode = true;
        return config;
    }
    void SetUp() override { Restart(Config()); }
    // A fresh app on a fresh window: a row starts from nothing the row
    // before left behind.
    void Restart(AppConfig config) {
        Shutdown();
        host_.overlayWindow = FakeOverlayWindow();
        host_.overlayWindow.captureReturnsWidth = 2;
        host_.overlayWindow.captureReturnsHeight = 1;
        host_.overlayWindow.captureReturnsPixelsRGBA = {1, 2, 3, 255, 4, 5, 6, 255};
        StartWith(std::move(config));
    }

    // The state, as today's flags say it.
    State Observed() const {
        if (!host_.overlayWindow.visible) {
            return State::Hidden;
        }
        if (App().IsNoticeOnly()) {
            return State::Notice;
        }
        if (App().IsPinnedOnly()) {
            return State::Pinned;
        }
        return App().IsViewOnly() ? State::View : State::Edit;
    }

    // Section 3's invariants, as far as they can be seen from outside.
    void ExpectInvariants() {
        const State state = Observed();
        SCOPED_TRACE(Name(state));
        if (state != State::Hidden) {
            EXPECT_EQ(host_.overlayWindow.inputPassthrough, state != State::Edit)
                << "click-through exactly in the pinned view, a notice and view mode";
        }
        EXPECT_FALSE(App().IsNoticeOnly() && App().IsPinnedOnly());
        EXPECT_TRUE(!App().IsNoticeOnly() || App().IsViewOnly());
        EXPECT_TRUE(!App().IsPinnedOnly() || App().IsViewOnly());
        if (HoldsFrozenScreen(controller_->GetSession())) {
            EXPECT_EQ(state, State::Edit) << "a frozen screen only in edit mode";
        }
    }

    void PinASnippet() {
        CanvasManager& manager = Model(controller_->GetSession());
        manager.FindItemAnywhere(manager.CreateItem(false, Rect{100, 100, 300, 200}, "Pinned"))->pinned = true;
    }

    void Reach(From from) {
        switch (from) {
            case From::Hidden:
                break;
            case From::Pinned:
                ShowEditMode();
                StepFrame();
                PinASnippet();
                ShowEditMode();
                break;
            case From::Notice:
                TriggerHotkey(config_.hotkeySilentCapture);
                break;
            case From::View:
                ShowViewMode();
                break;
            case From::ViewWithAPinnedSnippet:
                ShowViewMode();
                PinASnippet();
                break;
            case From::ViewFromPinned:
                Reach(From::Pinned);
                StepFrame();
                ShowViewMode();
                break;
            case From::Edit:
                ShowEditMode();
                break;
            case From::EditWithAPinnedSnippet:
                ShowEditMode();
                PinASnippet();
                break;
        }
        StepFrame();
    }

    void Ask(Request request) {
        switch (request) {
            case Request::Edit:
                ShowEditMode();
                break;
            case Request::View:
                ShowViewMode();
                break;
            case Request::QuickCapture:
                TriggerHotkey(config_.hotkeyQuickCapture);
                break;
            case Request::SilentCapture:
                TriggerHotkey(config_.hotkeySilentCapture);
                break;
            case Request::NoticeFaded:
                StepFrames(kFramesPastAToast);
                break;
        }
    }
};

struct Cell {
    From from;
    State fromState;
    Request request;
    State to;
    std::vector<std::string> calls;
};

// What the way up from Hidden into a session does: the window is made and
// placed, the profile is resolved for what is underneath and handed to the
// window, and then it is shown.
const std::vector<std::string> kUp = {"EnsureCreated",  "MoveToDisplay", "UnderlyingApplication",
                                      "NoActivate(on)", "EditModeInput", "Show"};

std::vector<std::string> Then(std::vector<std::string> calls, const std::vector<std::string>& more) {
    calls.insert(calls.end(), more.begin(), more.end());
    return calls;
}

// Today's table, as the app does it at fa49b05. The cells marked Change in
// docs/OVERLAY_STATES.md change here when their phase lands, and nowhere
// else.
const std::vector<Cell>& TodaysTable() {
    static const std::vector<Cell> cells = {
        // From Hidden: up.
        {From::Hidden, State::Hidden, Request::Edit, State::Edit, Then(kUp, {"Passthrough(off)", "Capture"})},
        // Shown, and made click-through afterwards: edit mode in between (C1).
        {From::Hidden, State::Hidden, Request::View, State::View, Then(kUp, {"Passthrough(on)"})},
        {From::Hidden, State::Hidden, Request::QuickCapture, State::Edit,
         Then({"EnsureCreated", "MoveToDisplay", "Capture"}, Then(kUp, {"Passthrough(off)", "Capture"}))},
        {From::Hidden, State::Hidden, Request::SilentCapture, State::Notice,
         {"EnsureCreated", "MoveToDisplay", "Capture", "EnsureCreated", "MoveToDisplay", "ShowClickThrough"}},

        // From the pinned view: edit mode through hidden, for its profile.
        {From::Pinned, State::Pinned, Request::Edit, State::Edit,
         Then({"Hide"}, Then(kUp, {"Passthrough(off)", "Capture"}))},
        // In place, with no profile (C3).
        {From::Pinned, State::Pinned, Request::View, State::View, {"Passthrough(on)"}},
        {From::Pinned, State::Pinned, Request::QuickCapture, State::Edit,
         Then({"EnsureCreated", "MoveToDisplay", "Capture", "Hide"}, Then(kUp, {"Passthrough(off)", "Capture"}))},
        {From::Pinned, State::Pinned, Request::SilentCapture, State::Pinned,
         {"EnsureCreated", "MoveToDisplay", "Capture"}},

        // From a notice: the same as from the pinned view.
        {From::Notice, State::Notice, Request::Edit, State::Edit,
         Then({"Hide"}, Then(kUp, {"Passthrough(off)", "Capture"}))},
        {From::Notice, State::Notice, Request::View, State::View, {"Passthrough(on)"}},
        {From::Notice, State::Notice, Request::QuickCapture, State::Edit,
         Then({"EnsureCreated", "MoveToDisplay", "Capture", "Hide"}, Then(kUp, {"Passthrough(off)", "Capture"}))},
        {From::Notice, State::Notice, Request::SilentCapture, State::Notice,
         {"EnsureCreated", "MoveToDisplay", "Capture"}},
        {From::Notice, State::Notice, Request::NoticeFaded, State::Hidden, {"Hide"}},

        // From view mode with a session: edit mode in place.
        {From::View, State::View, Request::Edit, State::Edit, {"Passthrough(off)", "Capture"}},
        {From::View, State::View, Request::View, State::Hidden, {"Hide"}},
        {From::View, State::View, Request::QuickCapture, State::Edit,
         {"EnsureCreated", "MoveToDisplay", "Capture", "Passthrough(off)", "Capture"}},
        {From::View, State::View, Request::SilentCapture, State::View, {"EnsureCreated", "MoveToDisplay", "Capture"}},
        // Put away into the pinned view in place: only what is drawn changes.
        {From::ViewWithAPinnedSnippet, State::View, Request::View, State::Pinned, {}},
        // From view mode without one: edit mode through hidden (C3).
        {From::ViewFromPinned, State::View, Request::Edit, State::Edit,
         Then({"Hide"}, Then(kUp, {"Passthrough(off)", "Capture"}))},
        {From::ViewFromPinned, State::View, Request::View, State::Pinned, {}},

        // From edit mode.
        {From::Edit, State::Edit, Request::Edit, State::Hidden, {"Hide"}},
        {From::Edit, State::Edit, Request::View, State::View, {"Passthrough(on)"}},
        // The shot is cut from the frozen screen, and the screen frozen again.
        {From::Edit, State::Edit, Request::QuickCapture, State::Edit,
         {"EnsureCreated", "MoveToDisplay", "Passthrough(off)", "Capture"}},
        {From::Edit, State::Edit, Request::SilentCapture, State::Edit, {"EnsureCreated", "MoveToDisplay"}},
        // Put away into the pinned view through hidden (C7).
        {From::EditWithAPinnedSnippet, State::Edit, Request::Edit, State::Pinned,
         {"Hide", "EnsureCreated", "MoveToDisplay", "ShowClickThrough"}},
    };
    return cells;
}

TEST_F(OverlayStatesTest, EveryCellLeadsWhereTheTableSaysTheWayItSays) {
    for (const Cell& cell : TodaysTable()) {
        SCOPED_TRACE(std::string(Name(cell.fromState)) + " + " + Name(cell.request) + " -> " + Name(cell.to) +
                     " (row from " + std::to_string(static_cast<int>(cell.from)) + ")");
        Restart(Config());
        Reach(cell.from);
        ASSERT_EQ(Name(Observed()), std::string(Name(cell.fromState)));
        host_.overlayWindow.calls.clear();

        Ask(cell.request);

        EXPECT_EQ(Name(Observed()), std::string(Name(cell.to)));
        EXPECT_EQ(host_.overlayWindow.calls, cell.calls);
        ExpectInvariants();
    }
}

// With "Say so when the overlay is hidden" off, a silent capture leaves the
// overlay where it was, and drops the message.
TEST_F(OverlayStatesTest, ASilentCaptureWhileHiddenStaysHiddenWithMessagesOff) {
    AppConfig config = Config();
    config.showToastsWhileHidden = false;
    Restart(config);
    host_.overlayWindow.calls.clear();

    TriggerHotkey(config_.hotkeySilentCapture);

    EXPECT_EQ(Observed(), State::Hidden);
    EXPECT_EQ(host_.overlayWindow.calls, (std::vector<std::string>{"EnsureCreated", "MoveToDisplay", "Capture"}));
    EXPECT_TRUE(App().ActionToastText().empty());
}

// The restart the input options HUD asks for, once the key that asked for
// it is up: through hidden, in the same session - so the profile is not
// resolved again - and frozen again.
TEST_F(OverlayStatesTest, ARestartGoesThroughHiddenInTheSameSession) {
    AppConfig config = Config();
    config.showInputOptionsHud = true;
    Restart(config);
    ShowEditMode();
    StepFrame();
    host_.overlayWindow.calls.clear();

    PressKey(ImGuiKey_1);  // "Don't steal focus", which is read only on the way up
    StepFrames(4);

    EXPECT_EQ(Observed(), State::Edit);
    EXPECT_EQ(host_.overlayWindow.calls,
              (std::vector<std::string>{"NoActivate(off)", "Hide", "EnsureCreated", "MoveToDisplay",
                                        "NoActivate(off)", "EditModeInput", "Show", "Passthrough(off)",
                                        "Capture"}));
    ExpectInvariants();
}

// The requests that change no state, in every state they can meet.
TEST_F(OverlayStatesTest, SettingsDisplaysAndTheSessionEndingChangeNoState) {
    for (const From from : {From::Hidden, From::Pinned, From::Notice, From::View, From::Edit}) {
        Restart(Config());
        Reach(from);
        const State before = Observed();
        SCOPED_TRACE(Name(before));
        const bool frozen = HoldsFrozenScreen(controller_->GetSession());
        host_.overlayWindow.calls.clear();

        controller_->GetSettings().Mutable().showItemBorders = !AppSettings().Stored().showItemBorders;
        controller_->GetSettings().Commit();
        host_.overlayWindow.displaysChangedCallback();
        host_.TriggerSessionEnd();

        EXPECT_EQ(Observed(), before);
        EXPECT_EQ(HoldsFrozenScreen(controller_->GetSession()), frozen) << "the frozen screen stays";
        for (const std::string& call : host_.overlayWindow.calls) {
            EXPECT_EQ(call, "MoveToDisplay") << "nothing to tell the window but where it is";
        }
        ExpectInvariants();
    }
}

// Any order of requests keeps section 3's invariants - the way the input
// machine's randomized test holds its own. Pinned snippets come and go
// too, since they decide where the overlay is put away to.
TEST_F(OverlayStatesTest, RandomRequestsKeepTheInvariants) {
    std::mt19937 random(20260926);
    std::set<State> visited;
    for (int step = 0; step < 300; ++step) {
        const int pick = std::uniform_int_distribution<int>(0, 7)(random);
        SCOPED_TRACE("step " + std::to_string(step) + " in " + Name(Observed()) + ", pick " + std::to_string(pick));
        switch (pick) {
            case 0:
            case 1:
                Ask(Request::Edit);
                break;
            case 2:
            case 3:
                Ask(Request::View);
                break;
            case 4:
                Ask(Request::QuickCapture);
                break;
            case 5:
                Ask(Request::SilentCapture);
                break;
            case 6:
                StepFrames(kFramesPastAToast);
                break;
            case 7: {
                // Whatever canvas is current: a capture moves to a new one.
                CanvasManager& manager = Model(controller_->GetSession());
                if (manager.CurrentCanvasHasPinnedItems()) {
                    for (Item& item : manager.CurrentOrNull()->items) {
                        item.pinned = false;
                    }
                } else {
                    PinASnippet();
                }
                break;
            }
        }
        StepFrame();
        ExpectInvariants();
        if (HasFailure()) {
            return;
        }
        visited.insert(Observed());
    }
    EXPECT_EQ(visited.size(), 5u) << "every state met along the way";
}

}  // namespace
}  // namespace sz::test
