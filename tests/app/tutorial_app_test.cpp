// The tutorial in the running app - docs/TUTORIAL.md, section 9: the
// anchors the owners mark as they draw, what the app answers the tutorial,
// the card and the spotlight, and the welcome chain walked through with
// real gestures.
#include <cmath>
#include <string>
#include <vector>

#include "fakes/headless_app.h"
#include "generated/ui_strings.h"
#include "support/view_stack.h"
#include "ui/tutorial/welcome_chain.h"

namespace sz::test {
namespace {

class TutorialAppTest : public HeadlessAppTest {
protected:
    OverlayApp& Overlay() { return controller_->Overlay(); }

    // A screenshot framed on empty canvas, left selected.
    ItemId MakeASnippet(float fromX, float fromY, float toX, float toY) {
        Drag(fromX, fromY, toX, toY);
        return Canvases().CurrentOrNull()->items.back().id;
    }

    static ImVec2 Center(const AnchorRect& rect) {
        return ImVec2((rect.min.x + rect.max.x) * 0.5f, (rect.min.y + rect.max.y) * 0.5f);
    }

    // ===== The tutorial =====

    const tutorial::Tutorial& Runner() const { return App().TutorialRunner(); }
    std::string StepUp() const {
        return Runner().GetState() == tutorial::Tutorial::State::OnStep ? std::string(Runner().CurrentStep().id)
                                                                         : "(" + Runner().Progress() + ")";
    }
    // The hint under the step's text, or empty.
    std::string HintUp() const {
        const std::optional<tutorial::Hint>& hint = Runner().CurrentHint();
        return hint.has_value() ? std::string(hint->text) : std::string();
    }
    std::optional<tutorial::Need> NeedUp() const {
        const std::optional<tutorial::Hint>& hint = Runner().CurrentHint();
        return hint.has_value() ? hint->need : std::nullopt;
    }

    // Edit mode, and the tutorial at its first step in a folder of its own.
    void StartTheTutorial() {
        ShowEditMode();
        StepFrame();
        Overlay().StartTutorial();
        StepFrames(2);
        ASSERT_TRUE(Runner().On());
        ASSERT_EQ(App().TutorialWorld().FolderOf(Canvases().CurrentCanvasId()), Runner().Folder());
    }
    // Long enough for a step whose goal is met to move on by itself.
    void Settle() { StepFrames(75); }
    void Press(TutorialButton button) {
        Overlay().PressTutorial(button);
        StepFrames(2);
    }

    // The subject as the model has it now.
    const Item& Subject() const { return *Canvases().FindItemAnywhere(*Runner().Subject()); }
    ImVec2 SubjectMiddle() const {
        const Rect& r = Subject().rect;
        return ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    }

    // What a hand does for each step of the welcome chain, as the step's
    // text says it: real gestures, keys and hotkeys.
    void DoStep(const std::string& id) {
        if (id == "welcome" || id == "programs" || id == "antiCheat") {
            Press(TutorialButton::Next);
        } else if (id == "screenshot") {
            Drag(300.0f, 360.0f, 600.0f, 560.0f);
        } else if (id == "move") {
            const ImVec2 from = SubjectMiddle();
            Drag(from.x, from.y, from.x + 120.0f, from.y - 60.0f);
        } else if (id == "resize") {
            if (!App().IsSelected(Subject().id)) {
                RawClick(SubjectMiddle().x, SubjectMiddle().y);
            }
            const Rect r = Subject().rect;
            // On the bottom-right corner's handle.
            const float x = std::round(r.x + r.w);
            const float y = std::round(r.y + r.h);
            Drag(x, y, x + 80.0f, y + 60.0f);
        } else if (id == "drawingMode") {
            DoubleClick(SubjectMiddle().x, SubjectMiddle().y);
        } else if (id == "draw") {
            const ImVec2 middle = SubjectMiddle();
            Drag(middle.x - 60.0f, middle.y - 20.0f, middle.x + 60.0f, middle.y + 20.0f);
        } else if (id == "stopDrawing") {
            RawClick(1200.0f, 120.0f);  // outside it
        } else if (id == "delete") {
            RawClick(SubjectMiddle().x, SubjectMiddle().y);
            PressKey(ImGuiKey_Delete);
        } else if (id == "undo") {
            PressCtrlKey(ImGuiKey_Z);
        } else if (id == "away") {
            ShowEditMode();  // away
            StepFrame();
            ShowEditMode();  // and back
            StepFrame();
        } else if (id == "end") {
            Press(TutorialButton::Done);
        } else {
            FAIL() << "no hand for step " << id;
        }
        Settle();
    }
    // The steps done, one after the other, until `id` is up.
    void WalkTo(const std::string& id) {
        StartTheTutorial();
        for (int guard = 0; guard < 20 && StepUp() != id; ++guard) {
            const std::string before = StepUp();
            DoStep(before);
            ASSERT_NE(StepUp(), before) << "the step " << before << " did not move on: " << HintUp();
        }
        ASSERT_EQ(StepUp(), id);
    }
};

// ===== Anchors (section 7.2) =====

TEST_F(TutorialAppTest, TheSelectionBarsCloseButtonIsMarkedWhereItIsDrawn) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    StepFrame();

    const std::optional<AnchorRect> close = App().AnchorAt(Anchor{AnchorId::SelectionBarClose});
    const std::optional<ImVec2> center = App().SelectionBarButtonCenter(ChromeButton::Close);
    ASSERT_TRUE(close.has_value());
    ASSERT_TRUE(center.has_value());
    EXPECT_FLOAT_EQ(Center(*close).x, center->x);
    EXPECT_FLOAT_EQ(Center(*close).y, center->y);
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::DrawingBarPen}).has_value()) << "not the drawing bar";
}

TEST_F(TutorialAppTest, AnAnchorNotDrawnThisFrameIsNotOnTheBoard) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    StepFrame();
    ASSERT_TRUE(App().AnchorAt(Anchor{AnchorId::SelectionBarClose}).has_value());

    PressKey(ImGuiKey_Escape);  // deselected: no bar
    EXPECT_TRUE(App().Selection().empty());
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::SelectionBarClose}).has_value());
}

TEST_F(TutorialAppTest, TheDrawingBarsPenIsMarkedWhereItIsDrawn) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_TRUE(App().DrawingItem().has_value());
    StepFrame();

    const std::optional<AnchorRect> pen = App().AnchorAt(Anchor{AnchorId::DrawingBarPen});
    const std::optional<ImVec2> center = App().SelectionBarButtonCenter(ChromeButton::Pen);
    ASSERT_TRUE(pen.has_value());
    ASSERT_TRUE(center.has_value());
    EXPECT_FLOAT_EQ(Center(*pen).x, center->x);
    EXPECT_FLOAT_EQ(Center(*pen).y, center->y);
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::SelectionBarClose}).has_value()) << "not the selection bar";
}

TEST_F(TutorialAppTest, ADockChipIsMarkedForItsSnippet) {
    ShowEditMode();
    StepFrame();
    const ItemId first = MakeASnippet(100.0f, 100.0f, 400.0f, 300.0f);
    const ItemId second = MakeASnippet(500.0f, 100.0f, 800.0f, 300.0f);
    controller_->GetSession().SetMinimized({first, second}, true);
    StepFrame();

    const std::optional<AnchorRect> firstChip = App().AnchorAt(Anchor{AnchorId::DockChip, first});
    const std::optional<AnchorRect> secondChip = App().AnchorAt(Anchor{AnchorId::DockChip, second});
    ASSERT_TRUE(firstChip.has_value());
    ASSERT_TRUE(secondChip.has_value());
    EXPECT_LT(firstChip->max.x, secondChip->min.x) << "side by side, in the canvas's order";
    EXPECT_GT(firstChip->min.y, kDisplayHeight * 0.5f) << "along the bottom";

    // A click on the chip where it is marked brings the snippet back.
    Click(Center(*firstChip).x, Center(*firstChip).y);
    EXPECT_FALSE(Canvases().FindItemAnywhere(first)->minimized);
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::DockChip, first}).has_value());
}

// ===== The world (section 7.1) =====

TEST_F(TutorialAppTest, TheWorldSaysWhatCoversTheCanvas) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);

    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrame();
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::CheatSheet);
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);

    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrame();
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::Overview);
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);

    RightClick(900.0f, 400.0f);
    ASSERT_TRUE(App().IsEmptyCanvasMenuOpen());
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::Popup);
}

TEST_F(TutorialAppTest, TheWorldSaysWhatIsInTheHand) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const ItemId shot = MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    EXPECT_EQ(world.Selection(), std::vector<ItemId>{shot});
    EXPECT_FALSE(world.DrawingItem().has_value());
    EXPECT_FALSE(world.CreationToolInHand().has_value());

    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::NewScreenshotTool}));
    EXPECT_EQ(world.CreationToolInHand(), ItemCreationKind::Screenshot);
    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(world.CreationToolInHand().has_value());

    DoubleClick(450.0f, 400.0f);
    EXPECT_EQ(world.DrawingItem(), shot);
}

TEST_F(TutorialAppTest, TheWorldReadsTheSnippetsOfAFolderAsFacts) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const ItemId shot = MakeASnippet(100.0f, 100.0f, 400.0f, 300.0f);
    MakeADrawing(500.0f, 100.0f, 800.0f, 300.0f);
    const ItemId drawing = Canvases().CurrentOrNull()->items.back().id;
    PressKey(ImGuiKey_Escape);  // drawing mode ended
    const CanvasId canvas = Canvases().CurrentCanvasId();
    const FolderId folder = world.FolderOf(canvas);
    ASSERT_NE(folder, 0u);
    EXPECT_EQ(world.CurrentCanvas(), canvas);
    EXPECT_EQ(world.CanvasName(canvas), Canvases().CurrentOrNull()->name);

    controller_->GetSession().SetMinimized({drawing}, true);
    RawClick(250.0f, 200.0f);
    PressKey(ImGuiKey_Delete);

    std::vector<tutorial::SnippetFacts> facts = world.SnippetsIn(folder);
    const auto find = [&](ItemId id) -> const tutorial::SnippetFacts* {
        for (const tutorial::SnippetFacts& f : facts) {
            if (f.id == id) {
                return &f;
            }
        }
        return nullptr;
    };
    const tutorial::SnippetFacts* shotFacts = find(shot);
    const tutorial::SnippetFacts* drawingFacts = find(drawing);
    ASSERT_NE(shotFacts, nullptr) << "a deleted snippet is still a fact";
    ASSERT_NE(drawingFacts, nullptr);
    EXPECT_TRUE(shotFacts->picture);
    EXPECT_TRUE(shotFacts->deleted);
    EXPECT_FALSE(shotFacts->minimized);
    EXPECT_EQ(shotFacts->canvas, canvas);
    EXPECT_FLOAT_EQ(shotFacts->rect.w, Canvases().FindItemAnywhere(shot)->rect.w);
    EXPECT_FALSE(drawingFacts->picture);
    EXPECT_FALSE(drawingFacts->deleted);
    EXPECT_TRUE(drawingFacts->minimized);
}

TEST_F(TutorialAppTest, TheWorldCountsTheStrokesAndSeesFullscreen) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    const ItemId drawing = Canvases().CurrentOrNull()->items.back().id;
    const FolderId folder = world.FolderOf(world.CurrentCanvas());
    Drag(350.0f, 350.0f, 600.0f, 500.0f);  // drawn with the pen in hand
    const auto factsOf = [&](ItemId id) {
        for (const tutorial::SnippetFacts& f : world.SnippetsIn(folder)) {
            if (f.id == id) {
                return f;
            }
        }
        return tutorial::SnippetFacts{};
    };
    EXPECT_EQ(factsOf(drawing).strokes, 1u);
    EXPECT_FALSE(factsOf(drawing).fullscreen);

    PressKey(ImGuiKey_Escape);  // out of drawing mode
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::ToggleFullscreen, drawing}));
    StepFrame();
    EXPECT_TRUE(factsOf(drawing).fullscreen);
}

TEST_F(TutorialAppTest, ADeletedCanvasIsInNoFolderAndADeletedFolderHoldsNothing) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    const CanvasId canvas = world.CurrentCanvas();
    const FolderId folder = world.FolderOf(canvas);
    ASSERT_FALSE(world.SnippetsIn(folder).empty());

    Session& session = controller_->GetSession();
    ASSERT_TRUE(session.Delete(folder));
    StepFrame();
    EXPECT_EQ(world.FolderOf(canvas), 0u);
    EXPECT_TRUE(world.SnippetsIn(folder).empty());
    EXPECT_EQ(world.FolderOf(987654321u), 0u) << "no such canvas";
}

TEST_F(TutorialAppTest, TheWorldNamesKeysAsTheyAreBound) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    EXPECT_EQ(world.KeyLabel(CommandId::Undo), std::optional<std::string>("Ctrl+Z"));
    EXPECT_EQ(world.KeyLabel(CommandId::ToggleEditMode),
              std::optional<std::string>(FormatKeyComboLabel(config_.hotkeyEditMode)));
    EXPECT_FALSE(world.KeyLabel(CommandId::Properties).has_value()) << "only a menu reaches it";
    EXPECT_EQ(world.ScreenshotTrigger(), config_.screenshotTrigger);
    EXPECT_EQ(world.DrawingTrigger(), config_.drawingTrigger);
}

TEST_F(TutorialAppTest, TheWorldCountsTheOverlayComingUp) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const uint64_t before = world.Showings();
    ShowEditMode();  // away
    StepFrame();
    EXPECT_EQ(world.Showings(), before);
    ShowEditMode();  // and back
    StepFrame();
    EXPECT_EQ(world.Showings(), before + 1);
}

// ===== The card, the spotlight and the walk-through (section 9) =====

TEST_F(TutorialAppTest, AStartMakesAFolderOfItsOwnAndSwitchesToIt) {
    ShowEditMode();
    StepFrame();
    const CanvasId before = Canvases().CurrentCanvasId();
    const size_t folders = Canvases().Folders().size();
    Overlay().StartTutorial();
    StepFrames(2);

    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(StepUp(), "welcome");
    ASSERT_EQ(Canvases().Folders().size(), folders + 1);
    const Folder& made = Canvases().Folders().back();
    EXPECT_EQ(made.id, Runner().Folder());
    EXPECT_EQ(made.name, strings::kTutorialFolderName);
    EXPECT_NE(Canvases().CurrentCanvasId(), before);
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, made.id);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "an empty canvas to start on";
}

TEST_F(TutorialAppTest, TheWelcomeChainIsWalkedThroughWithRealGestures) {
    StartTheTutorial();
    std::vector<std::string> seen;
    for (int guard = 0; guard < 20 && Runner().On(); ++guard) {
        const std::string step = StepUp();
        seen.push_back(step);
        DoStep(step);
        if (Runner().On()) {
            ASSERT_NE(StepUp(), step) << "the step " << step << " did not move on: " << HintUp();
        }
    }
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Runner().GetOutcome(), tutorial::Tutorial::Outcome::Finished);
    std::vector<std::string> chain;
    for (const tutorial::Step& step : tutorial::WelcomeChain()) {
        chain.emplace_back(step.id);
    }
    EXPECT_EQ(seen, chain) << "every step, in order, none skipped";
}

TEST_F(TutorialAppTest, ADoStepShowsItsCheckAndMovesOnASecondLater) {
    WalkTo("screenshot");
    Drag(300.0f, 360.0f, 600.0f, 560.0f);
    EXPECT_EQ(StepUp(), "screenshot");
    EXPECT_TRUE(Runner().GoalMet());
    StepFrames(30);
    EXPECT_EQ(StepUp(), "screenshot") << "half a second on, still showing its check";
    StepFrames(40);
    EXPECT_EQ(StepUp(), "move");
}

TEST_F(TutorialAppTest, TheCardIsDrawnWhileTheTutorialIsOnInEditModeOnly) {
    ShowEditMode();
    StepFrame();
    EXPECT_EQ(ImGui::FindWindowByName("##tutorial_card"), nullptr) << "no tutorial, no card";
    Overlay().StartTutorial();
    StepFrames(2);
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->Active);

    ShowViewMode();
    StepFrames(2);
    EXPECT_FALSE(card->Active) << "view only is click-through";
    ShowEditMode();
    StepFrames(2);
    EXPECT_TRUE(card->Active);
    EXPECT_EQ(StepUp(), "welcome") << "back where it was";
}

TEST_F(TutorialAppTest, TheCardSitsAboveThePanelsAndBelowTheDeleteConfirmation) {
    StartTheTutorial();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrames(2);
    const std::vector<std::string> overSheet = {"canvas", "items", "hud", "chrome", "cheat sheet backdrop",
                                                "cheat sheet", "tutorial card"};
    ASSERT_TRUE(InStackOrder(overSheet));
    EXPECT_EQ(Describe(SurfacesBackToFront()), Describe(overSheet));
    PressKey(ImGuiKey_Escape);

    // The tutorial's own canvas deleted: asked first. Which comes out
    // from under the canvas bar for a moment - in the table's order all
    // the same.
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::DeleteCanvas, 0, Canvases().CurrentCanvasId()}));
    StepFrames(3);
    const std::vector<std::string> stack = SurfacesBackToFront("delete confirmation");
    EXPECT_TRUE(InStackOrder(stack)) << Describe(stack);
    ASSERT_GE(stack.size(), 2u);
    EXPECT_EQ(stack[stack.size() - 2], "tutorial card") << Describe(stack);
    EXPECT_EQ(stack.back(), "delete confirmation") << Describe(stack);
}

TEST_F(TutorialAppTest, TheSpotlightRingsTheSubjectAndTheCardStaysClearOfIt) {
    WalkTo("move");
    const std::optional<AnchorRect> spot = App().TutorialSpot();
    ASSERT_TRUE(spot.has_value());
    const Rect& r = Subject().rect;
    EXPECT_FLOAT_EQ(spot->min.x, r.x);
    EXPECT_FLOAT_EQ(spot->max.y, r.y + r.h);

    // Moved up under the card, the card goes to the bottom.
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_LT(card->Pos.y, kDisplayHeight * 0.5f) << "at the top to begin with";
    const ImVec2 from = SubjectMiddle();
    const ImVec2 to(card->Pos.x + card->Size.x * 0.5f, card->Pos.y + card->Size.y * 0.5f);
    Drag(from.x, from.y, to.x, to.y);
    StepFrames(2);
    EXPECT_GT(card->Pos.y, kDisplayHeight * 0.5f);
}

TEST_F(TutorialAppTest, TheSpotlightRingsTheBarsCloseOnTheDeleteStep) {
    WalkTo("delete");
    RawClick(SubjectMiddle().x, SubjectMiddle().y);
    StepFrame();
    const std::optional<AnchorRect> spot = App().TutorialSpot();
    const std::optional<AnchorRect> close = App().AnchorAt(Anchor{AnchorId::SelectionBarClose});
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(close.has_value());
    EXPECT_FLOAT_EQ(spot->min.x, close->min.x);
    EXPECT_FLOAT_EQ(spot->min.y, close->min.y);

    // Pressed where the ring is, it is the step done.
    RawClick(Center(*spot).x, Center(*spot).y);
    Settle();
    EXPECT_EQ(StepUp(), "undo");
}

TEST_F(TutorialAppTest, TheSpotlightRingsTheDrawingBarsPen) {
    WalkTo("draw");
    const std::optional<AnchorRect> spot = App().TutorialSpot();
    const std::optional<AnchorRect> pen = App().AnchorAt(Anchor{AnchorId::DrawingBarPen});
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(pen.has_value());
    EXPECT_FLOAT_EQ(spot->min.x, pen->min.x);
}

TEST_F(TutorialAppTest, SkipShowsTheWarningsNotReachedAndDoneLetsGo) {
    WalkTo("move");
    Press(TutorialButton::Skip);
    EXPECT_EQ(Runner().GetState(), tutorial::Tutorial::State::Skipped);
    EXPECT_EQ(Runner().WarningsNotReached().size(), 2u);
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->Active) << "the skip card";

    Press(TutorialButton::Back);
    EXPECT_EQ(StepUp(), "move") << "a misclick costs nothing";
    Press(TutorialButton::Skip);
    Press(TutorialButton::Done);
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Runner().GetOutcome(), tutorial::Tutorial::Outcome::Skipped);
    EXPECT_FALSE(card->Active);
}

// ===== Progress, kept (section 7.6) =====

TEST_F(TutorialAppTest, TheProgressIsKeptInTheSettingsAsItGoes) {
    const auto welcome = [this] { return AppSettings().Stored().tutorialWelcome; };
    EXPECT_EQ(welcome(), "") << "never shown";
    StartTheTutorial();
    EXPECT_EQ(welcome(), "welcome");
    EXPECT_EQ(AppSettings().Stored().tutorialFolder, Runner().Folder());
    EXPECT_NE(Runner().Folder(), 0u);

    Press(TutorialButton::Next);
    EXPECT_EQ(welcome(), "screenshot");
    DoStep("screenshot");
    Settle();
    EXPECT_EQ(welcome(), "move") << "a step moved on by itself is kept too";

    Press(TutorialButton::Skip);
    EXPECT_EQ(welcome(), "skipped");
    Press(TutorialButton::Back);
    EXPECT_EQ(welcome(), "move");
    Press(TutorialButton::Skip);
    Press(TutorialButton::Done);
    EXPECT_EQ(welcome(), "skipped");
}

TEST_F(TutorialAppTest, AFinishedTutorialIsKeptAsFinished) {
    WalkTo("end");
    Press(TutorialButton::Done);
    EXPECT_EQ(AppSettings().Stored().tutorialWelcome, "finished");
}

// ===== The derail matrix (sections 6.1 and 9) =====
//
// Each do step, crossed with each way off the path that applies to it: the
// card says a line that applies, and doing what the lines say - one after
// the other, as a user would - and then the step completes it.

// A way off the path, as a user takes it.
enum class Way {
    Overview,
    CheatSheet,
    OtherFolder,
    OtherCanvasHere,
    HiddenAndShown,
    ViewOnly,
    Deleted,
    UndoneSeveralTimes,
    Minimized,
    Fullscreen,
    CaptureHotkey,
    LeftDrawingMode,
    EnteredDrawingMode,
};

const char* WayName(Way way) {
    switch (way) {
        case Way::Overview: return "Overview";
        case Way::CheatSheet: return "CheatSheet";
        case Way::OtherFolder: return "OtherFolder";
        case Way::OtherCanvasHere: return "OtherCanvasHere";
        case Way::HiddenAndShown: return "HiddenAndShown";
        case Way::ViewOnly: return "ViewOnly";
        case Way::Deleted: return "Deleted";
        case Way::UndoneSeveralTimes: return "UndoneSeveralTimes";
        case Way::Minimized: return "Minimized";
        case Way::Fullscreen: return "Fullscreen";
        case Way::CaptureHotkey: return "CaptureHotkey";
        case Way::LeftDrawingMode: return "LeftDrawingMode";
        case Way::EnteredDrawingMode: return "EnteredDrawingMode";
    }
    return "?";
}

struct Derail {
    const char* step;
    Way way;
    // The need the card says a line for - none where the way breaks
    // nothing the step needs.
    std::optional<tutorial::Need> says;
};

std::vector<Derail> Matrix() {
    using enum Way;
    using tutorial::Need;
    const std::optional<Need> nothing;
    std::vector<Derail> cases = {
        {"screenshot", Overview, Need::CanvasUncovered},
        {"screenshot", CheatSheet, Need::CanvasUncovered},
        {"screenshot", OtherFolder, Need::InTutorialFolder},
        {"screenshot", HiddenAndShown, nothing},
        {"screenshot", ViewOnly, nothing},
        {"screenshot", CaptureHotkey, nothing},
    };
    for (const char* step : {"move", "resize"}) {
        cases.push_back({step, Overview, Need::CanvasUncovered});
        cases.push_back({step, CheatSheet, Need::CanvasUncovered});
        cases.push_back({step, OtherFolder, Need::InTutorialFolder});
        cases.push_back({step, OtherCanvasHere, Need::SubjectHere});
        cases.push_back({step, HiddenAndShown, nothing});
        cases.push_back({step, ViewOnly, nothing});
        cases.push_back({step, Deleted, Need::ASubject});
        cases.push_back({step, UndoneSeveralTimes, Need::ASubject});
        cases.push_back({step, Minimized, Need::SubjectOnScreen});
        cases.push_back({step, Fullscreen, Need::SubjectCanMove});
        cases.push_back({step, CaptureHotkey, Need::SubjectHere});
        cases.push_back({step, EnteredDrawingMode, Need::NoDrawingMode});
    }
    const std::vector<Derail> more = {
        {"drawingMode", Overview, Need::CanvasUncovered},
        {"drawingMode", CheatSheet, Need::CanvasUncovered},
        {"drawingMode", OtherFolder, Need::InTutorialFolder},
        {"drawingMode", OtherCanvasHere, Need::SubjectHere},
        {"drawingMode", HiddenAndShown, nothing},
        {"drawingMode", ViewOnly, nothing},
        {"drawingMode", Deleted, Need::ASubject},
        {"drawingMode", Minimized, Need::SubjectOnScreen},
        {"drawingMode", Fullscreen, nothing},
        {"draw", Overview, Need::CanvasUncovered},
        {"draw", CheatSheet, Need::CanvasUncovered},
        {"draw", HiddenAndShown, nothing},
        {"draw", ViewOnly, Need::DrawingOnSubject},
        {"draw", LeftDrawingMode, Need::DrawingOnSubject},
        {"delete", Overview, Need::CanvasUncovered},
        {"delete", CheatSheet, Need::CanvasUncovered},
        {"delete", OtherFolder, Need::InTutorialFolder},
        {"delete", OtherCanvasHere, Need::SubjectHere},
        {"delete", HiddenAndShown, nothing},
        {"delete", ViewOnly, nothing},
        {"delete", Minimized, Need::SubjectOnScreen},
        {"delete", EnteredDrawingMode, Need::NoDrawingMode},
        {"undo", Overview, Need::CanvasUncovered},
        {"undo", CheatSheet, Need::CanvasUncovered},
        {"undo", OtherFolder, Need::InTutorialFolder},
        {"undo", OtherCanvasHere, Need::SubjectHere},
        {"undo", HiddenAndShown, nothing},
        {"undo", ViewOnly, nothing},
        {"away", Overview, nothing},
        {"away", CheatSheet, nothing},
    };
    cases.insert(cases.end(), more.begin(), more.end());
    return cases;
}

class TutorialDerailTest : public TutorialAppTest, public ::testing::WithParamInterface<Derail> {
protected:
    void TakeTheWay(Way way) {
        const std::optional<ItemId> subject = Runner().Subject();
        switch (way) {
            case Way::Overview:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
                break;
            case Way::CheatSheet:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::CheatSheet}));
                break;
            case Way::OtherFolder:
                // The canvas the app started on, outside the tutorial's folder.
                for (const Canvas& canvas : Canvases().Canvases()) {
                    if (canvas.folderId != Runner().Folder()) {
                        controller_->GetSession().SwitchToCanvas(canvas.id);
                        break;
                    }
                }
                ASSERT_NE(Canvases().CurrentOrNull()->folderId, Runner().Folder());
                break;
            case Way::OtherCanvasHere:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::NewCanvas}));
                break;
            case Way::HiddenAndShown:
                ShowEditMode();
                StepFrame();
                ShowEditMode();
                break;
            case Way::ViewOnly:
                ShowViewMode();
                StepFrame();
                ShowEditMode();
                break;
            case Way::Deleted:
                ASSERT_TRUE(subject.has_value());
                ASSERT_TRUE(controller_->GetSession().DeleteItem(*subject));
                break;
            case Way::UndoneSeveralTimes:
                for (int i = 0; i < 4; ++i) {
                    PressCtrlKey(ImGuiKey_Z);
                }
                break;
            case Way::Minimized:
                ASSERT_TRUE(subject.has_value());
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Minimize, *subject}));
                break;
            case Way::Fullscreen:
                ASSERT_TRUE(subject.has_value());
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::ToggleFullscreen, *subject}));
                break;
            case Way::CaptureHotkey:
                TriggerHotkey(config_.hotkeyQuickCapture);
                break;
            case Way::LeftDrawingMode:
                RawClick(1200.0f, 120.0f);
                break;
            case Way::EnteredDrawingMode:
                DoubleClick(SubjectMiddle().x, SubjectMiddle().y);
                break;
        }
        StepFrames(3);
    }

    // What the lines say, done one after the other until there is none:
    // the card's button where the line has one, and otherwise what its
    // words say to do.
    void FollowTheLines() {
        for (int guard = 0; guard < 6; ++guard) {
            const std::optional<tutorial::Hint>& hint = Runner().CurrentHint();
            if (!hint.has_value() || !hint->need.has_value()) {
                return;
            }
            if (hint->button != tutorial::HintButton::None) {
                Overlay().PressTutorialHint();
                StepFrames(3);
                continue;
            }
            switch (*hint->need) {
                case tutorial::Need::CanvasUncovered:
                case tutorial::Need::NoOtherTool:
                    PressKey(ImGuiKey_Escape);
                    break;
                case tutorial::Need::NoDrawingMode:
                    // "Click outside the snippet, or press Esc."
                    PressKey(ImGuiKey_Escape);
                    break;
                case tutorial::Need::SubjectOnScreen: {
                    const std::optional<AnchorRect> chip = App().TutorialSpot();
                    ASSERT_TRUE(chip.has_value()) << "the ring on the chip";
                    Click(Center(*chip).x, Center(*chip).y);
                    break;
                }
                case tutorial::Need::SubjectCanMove:
                    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::ToggleFullscreen, *Runner().Subject()}));
                    break;
                case tutorial::Need::SubjectSelected:
                    RawClick(SubjectMiddle().x, SubjectMiddle().y);
                    break;
                case tutorial::Need::DrawingOnSubject:
                    DoubleClick(SubjectMiddle().x, SubjectMiddle().y);
                    break;
                default:
                    FAIL() << "a line with nothing to do: " << hint->text;
            }
            StepFrames(3);
        }
        FAIL() << "the lines never ran out: " << HintUp();
    }

};

TEST_P(TutorialDerailTest, TheCardSaysALineAndFollowingItGetsTheStepDone) {
    const Derail& derail = GetParam();
    WalkTo(derail.step);
    const bool gated = Runner().CurrentStep().gated;

    TakeTheWay(derail.way);
    ASSERT_EQ(StepUp(), derail.step) << "the way off the path did not do the step";
    EXPECT_EQ(NeedUp(), derail.says) << "the line: " << HintUp();
    // Never trapped: Skip is on every step, and Next where it always is.
    EXPECT_EQ(Runner().GetState(), tutorial::Tutorial::State::OnStep);
    if (!gated) {
        EXPECT_TRUE(Runner().NextEnabled());
    }

    FollowTheLines();
    EXPECT_FALSE(NeedUp().has_value()) << HintUp();
    DoStep(derail.step);
    EXPECT_NE(StepUp(), derail.step) << "not done after following the lines: " << HintUp();
}

INSTANTIATE_TEST_SUITE_P(Matrix, TutorialDerailTest, ::testing::ValuesIn(Matrix()),
                         [](const ::testing::TestParamInfo<Derail>& info) {
                             return std::string(info.param.step) + "_" + WayName(info.param.way);
                         });

}  // namespace
}  // namespace sz::test
