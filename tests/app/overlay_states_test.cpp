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

#include "app/overlay_states.h"
#include "fakes/headless_app.h"
#include "support/session_test_access.h"

namespace sz::test {
namespace {

// ===== The table itself: Next, cell by cell =====

using app::Next;
using app::OverlayFacts;
using app::OverlayRequest;
using app::OverlayState;
using app::OverlayTransition;
using app::Route;

struct NextCell {
    OverlayState state;
    OverlayRequest request;
    OverlayFacts facts;
    OverlayState to;
    Route route;
    bool startsSession;
    bool endsSession;
};

OverlayFacts Pinned() {
    OverlayFacts facts;
    facts.pinnedHere = true;
    return facts;
}
OverlayFacts Messages() {
    OverlayFacts facts;
    facts.messagesWhileHidden = true;
    return facts;
}
OverlayFacts FirstRun() {
    OverlayFacts facts;
    facts.firstRun = true;
    return facts;
}

const char* Name(Route route) {
    switch (route) {
        case Route::Stay:
            return "Stay";
        case Route::Up:
            return "Up";
        case Route::Down:
            return "Down";
        case Route::InPlace:
            return "InPlace";
        case Route::ThroughHidden:
            return "ThroughHidden";
    }
    return "?";
}

// docs/OVERLAY_STATES.md, section 5, one row per cell - and per fact that
// changes a cell.
TEST(OverlayStatesNextTest, EveryCellOfTheTable) {
    using S = OverlayState;
    using R = OverlayRequest;
    const OverlayFacts none;
    const std::vector<NextCell> cells = {
        {S::Hidden, R::Edit, none, S::Edit, Route::Up, true, false},
        {S::Pinned, R::Edit, Pinned(), S::Edit, Route::ThroughHidden, true, false},
        {S::Notice, R::Edit, none, S::Edit, Route::ThroughHidden, true, false},
        {S::View, R::Edit, none, S::Edit, Route::InPlace, false, false},
        {S::Edit, R::Edit, none, S::Hidden, Route::Down, false, true},
        // C7: in place.
        {S::Edit, R::Edit, Pinned(), S::Pinned, Route::InPlace, false, true},

        {S::Hidden, R::View, none, S::View, Route::Up, true, false},
        // C3: a session starts in place.
        {S::Pinned, R::View, Pinned(), S::View, Route::InPlace, true, false},
        {S::Notice, R::View, none, S::View, Route::InPlace, true, false},
        {S::View, R::View, none, S::Hidden, Route::Down, false, true},
        {S::View, R::View, Pinned(), S::Pinned, Route::InPlace, false, true},
        {S::Edit, R::View, none, S::View, Route::InPlace, false, false},

        {S::Hidden, R::QuickCapture, none, S::Edit, Route::Up, true, false},
        {S::Pinned, R::QuickCapture, none, S::Edit, Route::ThroughHidden, true, false},
        {S::Notice, R::QuickCapture, none, S::Edit, Route::ThroughHidden, true, false},
        {S::View, R::QuickCapture, none, S::Edit, Route::InPlace, false, false},
        {S::Edit, R::QuickCapture, none, S::Edit, Route::InPlace, false, false},
        {S::Edit, R::QuickCapture, Pinned(), S::Edit, Route::InPlace, false, false},

        {S::Hidden, R::SilentCapture, Messages(), S::Notice, Route::Up, false, false},
        {S::Hidden, R::SilentCapture, none, S::Hidden, Route::Stay, false, false},
        {S::Pinned, R::SilentCapture, Messages(), S::Pinned, Route::Stay, false, false},
        {S::Notice, R::SilentCapture, Messages(), S::Notice, Route::Stay, false, false},
        {S::View, R::SilentCapture, Messages(), S::View, Route::Stay, false, false},
        {S::Edit, R::SilentCapture, Messages(), S::Edit, Route::Stay, false, false},

        {S::Notice, R::NoticeFaded, none, S::Hidden, Route::Down, false, false},
        {S::Hidden, R::NoticeFaded, none, S::Hidden, Route::Stay, false, false},
        {S::Pinned, R::NoticeFaded, Pinned(), S::Pinned, Route::Stay, false, false},
        {S::View, R::NoticeFaded, none, S::View, Route::Stay, false, false},
        {S::Edit, R::NoticeFaded, none, S::Edit, Route::Stay, false, false},

        {S::Edit, R::Restart, none, S::Edit, Route::ThroughHidden, false, false},
        {S::Hidden, R::Restart, none, S::Hidden, Route::Stay, false, false},
        {S::Pinned, R::Restart, Pinned(), S::Pinned, Route::Stay, false, false},
        {S::Notice, R::Restart, none, S::Notice, Route::Stay, false, false},
        {S::View, R::Restart, none, S::View, Route::Stay, false, false},

        {S::Hidden, R::Start, FirstRun(), S::Edit, Route::Up, true, false},
        {S::Hidden, R::Start, Pinned(), S::Pinned, Route::Up, false, false},
        {S::Hidden, R::Start, none, S::Hidden, Route::Stay, false, false},
    };
    for (const NextCell& cell : cells) {
        const OverlayTransition transition = Next(cell.state, cell.request, cell.facts);
        SCOPED_TRACE(std::string(app::Name(cell.state)) + " + request " + std::to_string(static_cast<int>(cell.request)));
        EXPECT_EQ(transition.from, cell.state);
        EXPECT_STREQ(app::Name(transition.to), app::Name(cell.to));
        EXPECT_STREQ(Name(transition.route), Name(cell.route));
        EXPECT_EQ(transition.startsSession, cell.startsSession);
        EXPECT_EQ(transition.endsSession, cell.endsSession);
        EXPECT_EQ(transition.restart, cell.request == R::Restart && cell.state == S::Edit);
    }
}

// What holds of every cell whatever the facts: a Stay goes nowhere, a
// route agrees with where it starts and ends, and a session never both
// starts and ends.
TEST(OverlayStatesNextTest, EveryRouteAgreesWithItsEnds) {
    const OverlayState states[] = {OverlayState::Hidden, OverlayState::Pinned, OverlayState::Notice,
                                   OverlayState::View, OverlayState::Edit};
    const OverlayRequest requests[] = {OverlayRequest::Edit,          OverlayRequest::View,
                                       OverlayRequest::QuickCapture,  OverlayRequest::SilentCapture,
                                       OverlayRequest::NoticeFaded,   OverlayRequest::Restart,
                                       OverlayRequest::Start};
    for (int bits = 0; bits < 8; ++bits) {
        OverlayFacts facts;
        facts.pinnedHere = (bits & 1) != 0;
        facts.messagesWhileHidden = (bits & 2) != 0;
        facts.firstRun = (bits & 4) != 0;
        for (const OverlayState state : states) {
            for (const OverlayRequest request : requests) {
                const OverlayTransition t = Next(state, request, facts);
                SCOPED_TRACE(std::string(app::Name(state)) + " + request " +
                             std::to_string(static_cast<int>(request)) + ", facts " + std::to_string(bits));
                EXPECT_EQ(t.from, state);
                if (t.route == Route::Stay) {
                    EXPECT_EQ(t.to, state);
                    EXPECT_FALSE(t.startsSession || t.endsSession || t.restart);
                    continue;
                }
                EXPECT_EQ(t.route == Route::Up, state == OverlayState::Hidden);
                EXPECT_EQ(t.route == Route::Down, t.to == OverlayState::Hidden);
                EXPECT_FALSE(t.startsSession && t.endsSession);
                EXPECT_FALSE(t.to == OverlayState::Pinned && !facts.pinnedHere) << "nothing to pin";
                EXPECT_FALSE(t.to == OverlayState::Notice && state != OverlayState::Hidden)
                    << "a notice only while nothing else is up";
                const bool fromSession = state == OverlayState::View || state == OverlayState::Edit;
                const bool toSession = t.to == OverlayState::View || t.to == OverlayState::Edit;
                EXPECT_EQ(t.startsSession, !fromSession && toSession) << "every View and Edit is a session";
                EXPECT_EQ(t.endsSession, fromSession && !toSession);
                EXPECT_FALSE(t.route == Route::ThroughHidden && fromSession && toSession && !t.restart)
                    << "within a session, nothing goes through hidden but a restart";
            }
        }
    }
}

// ===== The app, cell by cell =====

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
        config.profileable.freezeScreen = true;
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

    // The state, as what is on screen says it: the window, and the
    // overlay's mode.
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
        EXPECT_STREQ(app::Name(controller_->State()), Name(state)) << "the controller's state is what is on screen";
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

// What the way up from Hidden into a session does before the window is
// presented: the window is made and placed, and the profile is resolved
// for what is underneath and handed to the window.
const std::vector<std::string> kUp = {"EnsureCreated", "MoveToDisplay", "UnderlyingApplication", "NoActivate(on)",
                                      "EditModeInput"};

std::vector<std::string> Then(std::vector<std::string> calls, const std::vector<std::string>& more) {
    calls.insert(calls.end(), more.begin(), more.end());
    return calls;
}

// The table, as the app does it. The cells a Change of
// docs/OVERLAY_STATES.md touches say which; they change here when their
// phase lands, and nowhere else. What the window does for each Present is
// PresentationSteps' (tests/platform/presentation_test.cpp).
const std::vector<Cell>& Table() {
    static const std::vector<Cell> cells = {
        // From Hidden: up.
        {From::Hidden, State::Hidden, Request::Edit, State::Edit, Then(kUp, {"Present(Interactive)", "Capture"})},
        // C1: up click-through, where it was shown as edit mode and made
        // click-through afterwards.
        {From::Hidden, State::Hidden, Request::View, State::View, Then(kUp, {"Present(ClickThrough)"})},
        {From::Hidden, State::Hidden, Request::QuickCapture, State::Edit,
         Then({"EnsureCreated", "MoveToDisplay", "Capture"}, Then(kUp, {"Present(Interactive)", "Capture"}))},
        {From::Hidden, State::Hidden, Request::SilentCapture, State::Notice,
         {"EnsureCreated", "MoveToDisplay", "Capture", "EnsureCreated", "MoveToDisplay", "Present(ClickThrough)"}},

        // From the pinned view: edit mode through hidden, for its profile.
        {From::Pinned, State::Pinned, Request::Edit, State::Edit,
         Then({"Present(Hidden)"}, Then(kUp, {"Present(Interactive)", "Capture"}))},
        // C3: in place, with a session - the profile resolved for what is
        // underneath now. C2: the window stays as it is, where it used to
        // hand focus to whatever had it when the pinned view came up.
        {From::Pinned, State::Pinned, Request::View, State::View,
         {"UnderlyingApplication", "NoActivate(on)", "EditModeInput", "Present(ClickThrough)"}},
        {From::Pinned, State::Pinned, Request::QuickCapture, State::Edit,
         Then({"EnsureCreated", "MoveToDisplay", "Capture", "Present(Hidden)"},
              Then(kUp, {"Present(Interactive)", "Capture"}))},
        {From::Pinned, State::Pinned, Request::SilentCapture, State::Pinned,
         {"EnsureCreated", "MoveToDisplay", "Capture"}},

        // From a notice: the same as from the pinned view.
        {From::Notice, State::Notice, Request::Edit, State::Edit,
         Then({"Present(Hidden)"}, Then(kUp, {"Present(Interactive)", "Capture"}))},
        {From::Notice, State::Notice, Request::View, State::View,
         {"UnderlyingApplication", "NoActivate(on)", "EditModeInput", "Present(ClickThrough)"}},
        {From::Notice, State::Notice, Request::QuickCapture, State::Edit,
         Then({"EnsureCreated", "MoveToDisplay", "Capture", "Present(Hidden)"},
              Then(kUp, {"Present(Interactive)", "Capture"}))},
        {From::Notice, State::Notice, Request::SilentCapture, State::Notice,
         {"EnsureCreated", "MoveToDisplay", "Capture"}},
        {From::Notice, State::Notice, Request::NoticeFaded, State::Hidden, {"Present(Hidden)"}},

        // From view mode with a session: edit mode in place - C4: with the
        // keys forgotten and the pointer placed, as coming up does.
        {From::View, State::View, Request::Edit, State::Edit, {"Present(Interactive)", "Capture"}},
        {From::View, State::View, Request::View, State::Hidden, {"Present(Hidden)"}},
        {From::View, State::View, Request::QuickCapture, State::Edit,
         {"EnsureCreated", "MoveToDisplay", "Capture", "Present(Interactive)", "Capture"}},
        {From::View, State::View, Request::SilentCapture, State::View, {"EnsureCreated", "MoveToDisplay", "Capture"}},
        // Put away into the pinned view in place: only what is drawn changes.
        {From::ViewWithAPinnedSnippet, State::View, Request::View, State::Pinned, {"Present(ClickThrough)"}},
        // C3: from a view mode entered from the pinned view, too - it is a
        // session - where it went through hidden to get a profile.
        {From::ViewFromPinned, State::View, Request::Edit, State::Edit, {"Present(Interactive)", "Capture"}},
        {From::ViewFromPinned, State::View, Request::View, State::Pinned, {"Present(ClickThrough)"}},

        // From edit mode.
        {From::Edit, State::Edit, Request::Edit, State::Hidden, {"Present(Hidden)"}},
        {From::Edit, State::Edit, Request::View, State::View, {"Present(ClickThrough)"}},
        // The shot is cut from the frozen screen, and the screen frozen again.
        {From::Edit, State::Edit, Request::QuickCapture, State::Edit,
         {"EnsureCreated", "MoveToDisplay", "Present(Interactive)", "Capture"}},
        {From::Edit, State::Edit, Request::SilentCapture, State::Edit, {"EnsureCreated", "MoveToDisplay"}},
        // C7: put away into the pinned view in place, where it went through
        // hidden and the pinned snippets blinked.
        {From::EditWithAPinnedSnippet, State::Edit, Request::Edit, State::Pinned, {"Present(ClickThrough)"}},
    };
    return cells;
}

TEST_F(OverlayStatesTest, EveryCellLeadsWhereTheTableSaysTheWayItSays) {
    for (const Cell& cell : Table()) {
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
              (std::vector<std::string>{"NoActivate(off)", "Present(Hidden)", "EnsureCreated", "MoveToDisplay",
                                        "NoActivate(off)", "EditModeInput", "Present(Interactive)", "Capture"}));
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
        host_.RunPostedTasks();
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

// The mode is set before the window comes up and the overlay is told it
// was shown, where it used to be set after both (section 6). The two
// settles involved end in the same place in either order: coming up into
// view mode ends everything edit mode left up, and coming up into edit
// mode keeps it.
TEST_F(OverlayStatesTest, TheModeBeforeTheShowingEndsWhereTheShowingBeforeTheModeDid) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    Drag(350.0f, 400.0f, 650.0f, 400.0f);  // something in it, so it stays
    ASSERT_TRUE(controller_->Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrame();
    ASSERT_EQ(App().InputStack(), "Canvas / DrawingMode / CheatSheet / - / - / -");
    ShowEditMode();  // put away
    StepFrame();
    ASSERT_EQ(Observed(), State::Hidden);

    ShowViewMode();  // up into view mode
    StepFrame();

    EXPECT_EQ(App().InputStack(), "Canvas / - / - / - / - / -");
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_FALSE(App().IsCheatSheetOpen());

    ShowViewMode();  // put away, and up into edit mode: nothing is left to end
    StepFrame();
    ShowEditMode();
    StepFrame();
    EXPECT_EQ(App().InputStack(), "Canvas / - / - / - / - / -");
}

// ===== Between frames (C5) =====

// A request that arrives in a frame is carried out after it: the frame in
// which the notice's message fades ends with the window still up, and the
// window goes before the next one.
TEST_F(OverlayStatesTest, ANoticeFadedInAFrameGoesAfterIt) {
    TriggerHotkey(config_.hotkeySilentCapture);
    ASSERT_EQ(Observed(), State::Notice);
    bool visibleAtTheEndOfEveryFrame = true;
    platform::FrameCallback frame = host_.overlayWindow.frameCallback;
    host_.overlayWindow.frameCallback = [&](float dt) {
        frame(dt);
        visibleAtTheEndOfEveryFrame = visibleAtTheEndOfEveryFrame && host_.overlayWindow.visible;
    };
    int frames = 0;
    while (Observed() == State::Notice && frames < kFramesPastAToast) {
        StepFrame();
        ++frames;
    }

    EXPECT_EQ(Observed(), State::Hidden);
    EXPECT_TRUE(visibleAtTheEndOfEveryFrame) << "never hidden inside a frame";
}

// The same for a restart, and for the window's half of a setting.
TEST_F(OverlayStatesTest, ARestartAndASettingAskedForInAFrameHappenAfterIt) {
    AppConfig config = Config();
    config.showInputOptionsHud = true;
    Restart(config);
    ShowEditMode();
    StepFrame();
    std::vector<std::vector<std::string>> callsInFrames;
    platform::FrameCallback frame = host_.overlayWindow.frameCallback;
    host_.overlayWindow.frameCallback = [&](float dt) {
        host_.overlayWindow.calls.clear();
        frame(dt);
        callsInFrames.push_back(host_.overlayWindow.calls);
    };
    host_.overlayWindow.calls.clear();

    PressKey(ImGuiKey_1);  // "Don't steal focus": a setting, and then a restart
    StepFrames(4);

    ASSERT_EQ(Observed(), State::Edit);
    for (const std::vector<std::string>& calls : callsInFrames) {
        EXPECT_TRUE(calls.empty()) << "the window is told nothing from inside a frame, and here got " << calls.front();
    }
}

// ===== Away =====

// Put away into the pinned view, what edit mode left up is ended, as view
// mode ends it - where put away into hidden, it is there again when the
// overlay comes back (docs/INTERACTIONS.md, decision 3). The pinned view
// draws frames, and ImGui does not keep a popup across a frame that does
// not draw it; so everything ends together (docs/OVERLAY_STATES.md,
// section 10).
TEST_F(OverlayStatesTest, ThePinnedViewEndsWhatEditModeLeftUp) {
    ShowEditMode();
    StepFrame();
    PinASnippet();  // at the top left, clear of what follows
    MakeADrawing(500.0f, 350.0f, 900.0f, 600.0f);
    Drag(550.0f, 450.0f, 850.0f, 450.0f);  // something in it, so it stays
    ASSERT_TRUE(controller_->Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrame();
    ASSERT_EQ(App().InputStack(), "Canvas / DrawingMode / CheatSheet / - / - / -");

    ShowEditMode();  // put away
    ASSERT_EQ(Observed(), State::Pinned);
    StepFrames(3);
    ShowEditMode();
    StepFrame();

    EXPECT_EQ(App().InputStack(), "Canvas / - / - / - / - / -");
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_FALSE(App().IsCheatSheetOpen());
}

// Why: a popup left up in the pinned view is gone after its first frames,
// closed by ImGui itself. Hidden draws no frames, and keeps it.
TEST_F(OverlayStatesTest, APopupSurvivesHiddenButNotFramesThatDoNotDrawIt) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressKey(ImGuiKey_Escape);
    RightClick(640.0f, 400.0f);
    ASSERT_TRUE(App().IsItemContextMenuOpen());

    ShowEditMode();  // put away, into hidden
    ShowEditMode();
    StepFrames(2);
    EXPECT_TRUE(App().IsItemContextMenuOpen()) << "kept while hidden";

    // The frames the pinned view would draw, with no popup in them - the
    // way edit mode would be if the menu's own frame never came.
    ImGui::NewFrame();
    ImGui::Render();
    ImGui::NewFrame();
    ImGui::Render();
    EXPECT_FALSE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) << "closed by ImGui";
}

// ===== Sessions (C3) =====

// View mode from the pinned view is a session like any other: edit mode
// from it is in place, with no blink of the pinned snippets, and runs the
// profile resolved for what was underneath when view mode began.
TEST_F(OverlayStatesTest, EditModeFromAViewEnteredFromThePinnedViewIsInPlace) {
    AppConfig config = Config();
    Profile profile;
    profile.name = "The Game";
    profile.match.executables.push_back("game.exe");
    profile.overrides.counterRawMouseInput = true;
    config.profiles.push_back(profile);
    Restart(config);
    Reach(From::Pinned);
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "The Game"};
    const int hides = host_.overlayWindow.hideCallCount;

    ShowViewMode();
    StepFrame();
    ASSERT_TRUE(AppSettings().ActiveProfile().has_value()) << "resolved as view mode began";
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{"other.exe", "Other"};
    ShowEditMode();
    StepFrame();

    EXPECT_EQ(Observed(), State::Edit);
    EXPECT_EQ(host_.overlayWindow.hideCallCount, hides) << "never hidden on the way";
    EXPECT_TRUE(host_.overlayWindow.editModeInput.counterRawMouseInput) << "the game's profile, still";
}

// A message waiting for the next showing is said when such a session
// starts, rather than at the next edit mode that comes up from hidden.
TEST_F(OverlayStatesTest, AMessageForTheNextShowingIsSaidWhenViewModeBeginsInThePinnedView) {
    Reach(From::Pinned);
    controller_->Overlay().SayDeletedForGoodAtStart(3, 30);
    ASSERT_TRUE(App().ActionToastText().empty());

    ShowViewMode();
    StepFrame();

    EXPECT_FALSE(App().ActionToastText().empty());
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

