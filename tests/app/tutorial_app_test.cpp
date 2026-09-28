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

}  // namespace
}  // namespace sz::test
