// The view as docs/VIEW_LAYER.md describes it: what is on screen and in
// which order (section 3), and what closing each popup does (section 4).
// Written against the app as it was before that document's changes, so
// that each change shows it moved nothing it did not mean to.
#include "fakes/headless_app.h"
#include "support/session_test_access.h"
#include "support/view_stack.h"

namespace sz::test {
namespace {

class ViewLayerTest : public HeadlessAppTest {
protected:
    OverlayApp& Overlay() { return controller_->Overlay(); }

    // A screenshot framed on empty canvas, and the selection it leaves let
    // go of.
    ItemId MakeASnippet(float fromX, float fromY, float toX, float toY) {
        Drag(fromX, fromY, toX, toY);
        PressKey(ImGuiKey_Escape);
        return Canvases().CurrentOrNull()->items.back().id;
    }

    // The pointer at the bottom edge, for long enough that the canvas bar
    // is all the way out.
    void RevealTheBar() {
        MoveTo(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f);
        StepFrames(30);
    }

    // The windows the last frame drew are `expected`, back to front - which
    // has to be a reading of section 3's table in the first place.
    void ExpectStack(const std::vector<std::string>& expected, const std::string& appPopup = "app popup") {
        ASSERT_TRUE(InStackOrder(expected)) << Describe(expected) << " is not in the table's order";
        EXPECT_EQ(Describe(SurfacesBackToFront(appPopup)), Describe(expected));
    }

    // Whether the machine holds no popup, and nothing else above the canvas.
    std::string NothingUp() const { return "Canvas / - / - / - / - / -"; }

    // A drawing, in drawing mode, with the color chooser up and a color
    // picked in it - one that differs from the pen's color as stored. The
    // chooser is left up.
    void PickAColor() {
        MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
        const uint32_t before = AppSettings().Stored().strokeColorRGBA;
        const std::optional<ImVec2> color = App().SelectionBarButtonCenter(ChromeButton::Color);
        ASSERT_TRUE(color.has_value());
        RawClick(color->x, color->y);
        StepFrames(2);
        ASSERT_TRUE(App().IsColorChooserOpen());

        // A drag across the picker's square, which sits at the top left of
        // the chooser - to ImGui alone, as a widget is dragged.
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        ASSERT_FALSE(g.OpenPopupStack.empty());
        const ImRect square = g.OpenPopupStack.back().Window->InnerRect;
        MoveTo(square.Min.x + 30.0f, square.Min.y + 30.0f);
        StepFrame();
        MouseButtonEvent(ImGuiMouseButton_Left, true);
        StepFrame();
        MoveTo(square.Min.x + 120.0f, square.Min.y + 60.0f);
        StepFrames(2);
        MouseButtonEvent(ImGuiMouseButton_Left, false);
        StepFrames(2);
        ASSERT_NE(App().DrawColorRGBA(), before) << "the drag changed nothing, so the test proves nothing";
        ASSERT_TRUE(App().IsColorChooserOpen());
    }

    // A drawing, in drawing mode, with the pen's width just changed by the
    // wheel: its size preview is up, and the width not yet kept.
    float WheelThePen() {
        MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
        const float before = AppSettings().Stored().strokeWidth;
        MoveTo(500.0f, 400.0f);
        Wheel(2.0f);
        EXPECT_EQ(AppSettings().Stored().strokeWidth, before) << "not yet: the preview is still up";
        return before + 2.0f;
    }
};

// ===== The stack (section 3) =====

TEST_F(ViewLayerTest, EverythingOverTheCanvasStacksInTheTablesOrder) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(100.0f, 100.0f, 400.0f, 300.0f);
    const ItemId minimized = MakeASnippet(500.0f, 100.0f, 800.0f, 300.0f);
    controller_->GetSession().SetMinimized({minimized}, true);
    // A note being typed: the Text tool, in a drawing.
    MakeADrawing(300.0f, 350.0f, 700.0f, 600.0f);
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::TextTool}));
    RawClick(400.0f, 450.0f);
    ASSERT_TRUE(App().EditingNote().has_value());
    RevealTheBar();
    ASSERT_GE(App().CanvasBarReveal(), 1.0f);

    ExpectStack({"canvas", "items", "note editor", "dock", "canvas bar", "hud", "chrome"});
}

TEST_F(ViewLayerTest, TheSnippetMenuSitsOverTheBarAndUnderTheChrome) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    RightClick(450.0f, 400.0f);
    ASSERT_TRUE(App().IsItemContextMenuOpen());
    RevealTheBar();
    ASSERT_TRUE(App().IsItemContextMenuOpen());

    ExpectStack({"canvas", "items", "canvas bar", "snippet menu", "hud", "chrome"}, "snippet menu");
}

TEST_F(ViewLayerTest, TheEmptyCanvasMenuSitsOverTheBarAndUnderTheChrome) {
    ShowEditMode();
    StepFrame();
    RightClick(900.0f, 400.0f);
    ASSERT_TRUE(App().IsEmptyCanvasMenuOpen());
    RevealTheBar();
    ASSERT_TRUE(App().IsEmptyCanvasMenuOpen());

    ExpectStack({"canvas", "items", "canvas bar", "empty canvas menu", "hud", "chrome"}, "empty canvas menu");
}

TEST_F(ViewLayerTest, TheCanvasTileMenuSitsOverTheBarItCameFrom) {
    ShowEditMode();
    StepFrame();
    RevealTheBar();
    const ImGuiWindow* bar = ImGui::FindWindowByName("##canvas_bar");
    ASSERT_NE(bar, nullptr);
    // The first tile, just inside the bar's padding.
    MoveTo(bar->Pos.x + 20.0f, bar->Pos.y + 20.0f);
    StepFrame();
    MouseButtonEvent(ImGuiMouseButton_Right, true);
    StepFrame();
    MouseButtonEvent(ImGuiMouseButton_Right, false);
    StepFrames(3);
    ASSERT_TRUE(App().IsCanvasContextMenuOpen());

    ExpectStack({"canvas", "items", "canvas bar", "canvas tile menu", "hud", "chrome"}, "canvas tile menu");
}

TEST_F(ViewLayerTest, PropertiesSitOverTheSnippetsAndUnderTheChrome) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 700.0f, 550.0f);  // a screenshot, selected as made
    const std::optional<ImVec2> more = App().SelectionBarButtonCenter(ChromeButton::More);
    ASSERT_TRUE(more.has_value());
    RawClick(more->x, more->y);
    RevealTheBar();
    ASSERT_EQ(App().InputStack(), "Canvas / - / - / ItemProperties / - / -");

    ExpectStack({"canvas", "items", "canvas bar", "properties", "hud", "chrome"}, "properties");
}

TEST_F(ViewLayerTest, TheColorChooserSitsOverTheSnippetsAndUnderTheChrome) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    const std::optional<ImVec2> color = App().SelectionBarButtonCenter(ChromeButton::Color);
    ASSERT_TRUE(color.has_value());
    RawClick(color->x, color->y);
    RevealTheBar();
    ASSERT_TRUE(App().IsColorChooserOpen());

    ExpectStack({"canvas", "items", "canvas bar", "color chooser", "hud", "chrome"}, "color chooser");
}

TEST_F(ViewLayerTest, TheOverviewCoversTheChromeAndTheBarGoesUnderIt) {
    ShowEditMode();
    StepFrame();
    RevealTheBar();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrames(2);
    ASSERT_TRUE(App().IsOverviewOpen());

    ExpectStack({"canvas", "items", "hud", "chrome", "overview backdrop", "overview"});
}

TEST_F(ViewLayerTest, TheCheatSheetCoversTheChrome) {
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrames(2);
    ASSERT_TRUE(App().IsCheatSheetOpen());

    ExpectStack({"canvas", "items", "hud", "chrome", "cheat sheet backdrop", "cheat sheet"});
}

TEST_F(ViewLayerTest, TheDeleteConfirmationIsOverEverythingElse) {
    ShowEditMode();
    StepFrame();
    Command deleteCanvas{CommandId::DeleteCanvas};
    deleteCanvas.canvas = Canvases().CurrentCanvasId();
    ASSERT_TRUE(Overlay().Dispatch(deleteCanvas));
    RevealTheBar();
    ExpectStack({"canvas", "items", "canvas bar", "hud", "chrome", "delete confirmation"}, "delete confirmation");

    // And over the Overview, whose own delete buttons ask it.
    PressKey(ImGuiKey_Escape);
    ASSERT_EQ(App().InputStack(), NothingUp());
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrame();
    ASSERT_TRUE(Overlay().Dispatch(deleteCanvas));
    StepFrames(2);
    ExpectStack({"canvas", "items", "hud", "chrome", "overview backdrop", "overview", "delete confirmation"},
                "delete confirmation");
}

TEST_F(ViewLayerTest, ViewModeDrawsOneLayer) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    ShowViewMode();
    StepFrames(2);
    ASSERT_TRUE(App().IsViewOnly());

    // Not a row of the edit-mode table: view mode has a table of its own,
    // of one window.
    EXPECT_EQ(Describe(SurfacesBackToFront()), "view only");
}

// ===== What closing a popup does (section 4) =====

// Escape, and a click outside: the menu is gone, and so is what it was
// about, and the machine holds no popup.
TEST_F(ViewLayerTest, TheSnippetMenuForgetsItsSnippetHoweverItCloses) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);

    RightClick(450.0f, 400.0f);
    ASSERT_TRUE(App().ItemContextMenuItem().has_value());
    PressKey(ImGuiKey_Escape);
    StepFrame();
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_FALSE(App().ItemContextMenuItem().has_value());

    RightClick(450.0f, 400.0f);
    ASSERT_TRUE(App().ItemContextMenuItem().has_value());
    Click(1100.0f, 100.0f);
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_FALSE(App().ItemContextMenuItem().has_value());
    EXPECT_EQ(App().InputStack().find("ItemMenu"), std::string::npos) << App().InputStack();
}

TEST_F(ViewLayerTest, TheEmptyCanvasMenuClosesByEscapeOrAClickOutside) {
    ShowEditMode();
    StepFrame();
    RightClick(640.0f, 400.0f);
    ASSERT_TRUE(App().IsEmptyCanvasMenuOpen());
    PressKey(ImGuiKey_Escape);
    StepFrame();
    EXPECT_FALSE(App().IsEmptyCanvasMenuOpen());
    EXPECT_EQ(App().InputStack(), NothingUp());

    RightClick(640.0f, 400.0f);
    ASSERT_TRUE(App().IsEmptyCanvasMenuOpen());
    Click(100.0f, 100.0f);
    EXPECT_FALSE(App().IsEmptyCanvasMenuOpen());
    EXPECT_EQ(App().InputStack(), NothingUp());
}

// Properties, closed by Escape or a click outside, leaves no style edit
// open and no popup on the machine.
TEST_F(ViewLayerTest, ClosingPropertiesEndsItsStyleEdit) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 700.0f, 550.0f);
    const auto openProperties = [this] {
        const std::optional<ImVec2> more = App().SelectionBarButtonCenter(ChromeButton::More);
        ASSERT_TRUE(more.has_value());
        RawClick(more->x, more->y);
        StepFrame();
        ASSERT_EQ(App().InputStack(), "Canvas / - / - / ItemProperties / - / -");
    };

    openProperties();
    PressKey(ImGuiKey_Escape);
    StepFrame();
    EXPECT_EQ(App().InputStack(), NothingUp());
    EXPECT_FALSE(AppSession().StyleEditOpen());

    openProperties();
    Click(1200.0f, 80.0f);
    EXPECT_EQ(App().InputStack(), NothingUp());
    EXPECT_FALSE(AppSession().StyleEditOpen());
}

// What the chooser was left on is the pen's color from then on, and the
// next time the app starts: kept when it closes.
TEST_F(ViewLayerTest, TheColorChooserKeepsThePensColorWhenItCloses) {
    ShowEditMode();
    StepFrame();
    const uint32_t before = AppSettings().Stored().strokeColorRGBA;
    PickAColor();
    EXPECT_EQ(AppSettings().Stored().strokeColorRGBA, before) << "kept as it closes, not as it is dragged";

    PressKey(ImGuiKey_Escape);
    StepFrame();
    EXPECT_FALSE(App().IsColorChooserOpen());
    EXPECT_EQ(AppSettings().Stored().strokeColorRGBA, App().DrawColorRGBA());
}

// Properties' closing is done when it closes, and not on every frame it is
// not up: done every frame, it ended the style edit a spin of Ctrl and the
// wheel holds open, after every notch - a spin of three notches was three
// undo steps, and Escape found nothing to call off.
TEST_F(ViewLayerTest, ASpinOfTheWheelsOpacityIsOneStepAndEscapeCallsItOff) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 700.0f, 550.0f);  // a screenshot, selected as made
    const ItemId id = Canvases().CurrentOrNull()->items[0].id;
    RawClick(500.0f, 400.0f);
    MoveTo(500.0f, 400.0f);
    const auto spin = [this] {
        KeyEvent(ImGuiMod_Ctrl, true);
        StepFrame();
        Wheel(-1.0f);
        Wheel(-1.0f);
        Wheel(-1.0f);
        KeyEvent(ImGuiMod_Ctrl, false);
        StepFrame();
    };
    const auto opacity = [&] { return Canvases().FindItemAnywhere(id)->picture.opacity; };

    spin();
    ASSERT_FLOAT_EQ(opacity(), 0.85f);
    StepFrames(90);  // the burst lapses, and is filed
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(opacity(), 1.0f) << "one undo, not three";

    spin();
    ASSERT_FLOAT_EQ(opacity(), 0.85f);
    PressKey(ImGuiKey_Escape);
    EXPECT_FLOAT_EQ(opacity(), 1.0f) << "the whole spin called off";
}

// ===== The pen kept when the overlay settles (section 4, C4) =====

// The width the wheel set is kept once its preview fades - and, put away
// before it has, when the overlay settles: otherwise an exit before edit
// mode is drawn again loses it.
TEST_F(ViewLayerTest, APenWidthPutAwayWithItsPreviewUpIsKeptOnExit) {
    ShowEditMode();
    StepFrame();
    const float wheeled = WheelThePen();
    ShowEditMode();  // the edit hotkey again: put away
    ASSERT_FALSE(host_.overlayWindow.IsVisible());
    host_.TriggerTrayCommand(platform::TrayCommand::Exit);
    ASSERT_TRUE(host_.quitCalled);
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, wheeled);
}

TEST_F(ViewLayerTest, APenWidthLeftForViewModeWithItsPreviewUpIsKept) {
    ShowEditMode();
    StepFrame();
    const float wheeled = WheelThePen();
    ShowViewMode();
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, wheeled);
}

// The color picked is kept when the chooser closes - and, put away with the
// chooser up (a popup stays up for the next showing), when the overlay
// settles.
TEST_F(ViewLayerTest, APenColorPutAwayWithTheChooserUpIsKeptOnExit) {
    ShowEditMode();
    StepFrame();
    PickAColor();
    const uint32_t picked = App().DrawColorRGBA();
    ShowEditMode();  // put away, the chooser still up
    ASSERT_FALSE(host_.overlayWindow.IsVisible());
    host_.TriggerTrayCommand(platform::TrayCommand::Exit);
    ASSERT_TRUE(host_.quitCalled);
    EXPECT_EQ(AppSettings().Stored().strokeColorRGBA, picked);
}

// Left for view mode, the chooser is ended from outside, and nothing draws
// it: its closing is done then, not at the next frame of edit mode.
TEST_F(ViewLayerTest, APenColorLeftForViewModeWithTheChooserUpIsKept) {
    ShowEditMode();
    StepFrame();
    PickAColor();
    const uint32_t picked = App().DrawColorRGBA();
    ShowViewMode();
    EXPECT_EQ(AppSettings().Stored().strokeColorRGBA, picked);
    EXPECT_FALSE(App().IsColorChooserOpen());
}

// Escape on the delete confirmation deletes nothing.
TEST_F(ViewLayerTest, EscapeOnTheDeleteConfirmationDeletesNothing) {
    ShowEditMode();
    StepFrame();
    const CanvasId canvas = Canvases().CurrentCanvasId();
    Command deleteCanvas{CommandId::DeleteCanvas};
    deleteCanvas.canvas = canvas;
    ASSERT_TRUE(Overlay().Dispatch(deleteCanvas));
    StepFrames(2);
    ASSERT_EQ(App().InputStack(), "Canvas / - / - / ConfirmDelete / - / -");

    PressKey(ImGuiKey_Escape);
    StepFrames(2);
    EXPECT_EQ(App().InputStack(), NothingUp());
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindCanvas(canvas)));
    EXPECT_EQ(Canvases().CurrentCanvasId(), canvas);
}

}  // namespace
}  // namespace sz::test
