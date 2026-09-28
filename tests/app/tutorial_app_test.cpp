// The tutorial in the running app - docs/TUTORIAL.md, section 9: the
// anchors the owners mark as they draw, what the app answers the tutorial,
// the card and the spotlight, and the welcome chain walked through with
// real gestures.
#include "fakes/headless_app.h"

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

}  // namespace
}  // namespace sz::test
