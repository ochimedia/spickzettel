// The overlay driven through real frames, with no window and no GPU - see
// tests/fakes/headless_app.h for what that costs (a context and a font
// atlas) and why it is worth having (every render path becomes reachable
// from a test, and behavior can be asserted instead of screenshotted).
#include "fakes/headless_app.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>

#include "core/canvas/item_geometry.h"  // kItemMinWidth/kItemMinHeight, for the group-resize floor
#include "core/persistence/library_store.h"
#include "core/util/uid.h"
#include "support/held_library.h"
#include "support/session_test_access.h"
#include "ui/theme.h"
#include "ui/view/canvas_view.h"
#include "ui/widgets.h"
#include "generated/ui_strings.h"

#include <imgui_internal.h>

namespace sz::test {
namespace {

using sz::test::HeadlessAppTest;

TEST_F(HeadlessAppTest, EditModeRendersWithNothingBehindIt) {
    ShowEditMode();
    ASSERT_TRUE(host_.overlayWindow.visible);
    StepFrames(3);
    SUCCEED();  // getting here is the assertion: no path tripped on the way
}

TEST_F(HeadlessAppTest, ViewOnlyModeRendersToo) {
    ShowViewMode();
    ASSERT_TRUE(host_.overlayWindow.visible);
    EXPECT_TRUE(App().IsViewOnly());
    StepFrames(2);
    SUCCEED();
}

// The empty library is a real, reachable state (see CanvasManager's class
// comment) and the one most likely to be missing a null check.
// View-only mode shows a picture that doesn't change by itself, so it asks
// the window for frames only now and then - but for every one while a
// message is fading on it. Edit mode always wants them all.
TEST_F(HeadlessAppTest, ViewOnlyModeAsksForFramesOnlyWhileSomethingMoves) {
    ShowEditMode();
    StepFrame();
    EXPECT_EQ(host_.overlayWindow.framePacing, platform::FramePacing::EveryFrame);

    ShowViewMode();
    StepFrame();
    EXPECT_EQ(host_.overlayWindow.framePacing, platform::FramePacing::Idle);

    // A capture from the view-only overlay says so, and the message fades.
    TriggerHotkey(config_.hotkeySilentCapture);
    StepFrame();
    EXPECT_EQ(host_.overlayWindow.framePacing, platform::FramePacing::EveryFrame);
    StepFrames(400);
    EXPECT_EQ(host_.overlayWindow.framePacing, platform::FramePacing::Idle) << "idle again once it has faded";

    ShowEditMode();
    StepFrame();
    EXPECT_EQ(host_.overlayWindow.framePacing, platform::FramePacing::EveryFrame);
}

// The accent is a setting: whatever it is set to is what the theme and the
// ImGui style draw with from the next frame, and what is drawn on it turns
// light when the accent is dark and dark when it is bright.
TEST_F(HeadlessAppTest, TheAccentColorRecolorsTheThemeAndTheStyle) {
    namespace theme = ui::theme;
    ShowEditMode();
    StepFrame();
    EXPECT_FLOAT_EQ(theme::Accent().y, 0x6C / 255.0f) << "teal by default";
    EXPECT_GT(theme::AccentInk().x, 0.5f) << "light ink on the dark default";

    controller_->GetSettings().Set(setting::kAccentColor, 0xFF6A3DFFu);
    StepFrame();

    EXPECT_FLOAT_EQ(theme::Accent().x, 1.0f);
    EXPECT_FLOAT_EQ(theme::Accent().z, 0x3D / 255.0f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().Colors[ImGuiCol_CheckMark].z, 0x3D / 255.0f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().Colors[ImGuiCol_SliderGrab].x, 1.0f);
    EXPECT_EQ(theme::AccentU32(), IM_COL32(0xFF, 0x6A, 0x3D, 0xFF));
    EXPECT_LT(theme::AccentInk().x, 0.5f) << "dark ink on a bright accent";
}

// The interface is drawn at Windows' scale for the display the overlay is
// on, from the frame after it changes - or at the one set in Settings,
// whatever Windows says. The style's sizes and its text scale together, and
// always from the same unscaled base, so going back leaves nothing behind.
TEST_F(HeadlessAppTest, TheInterfaceScaleFollowsWindowsUnlessOneIsSet) {
    ShowEditMode();
    StepFrame();
    EXPECT_FLOAT_EQ(UiScale(), 1.0f);
    const float padding = ImGui::GetStyle().WindowPadding.x;

    host_.overlayWindow.scalePercent = 150;
    StepFrame();
    EXPECT_FLOAT_EQ(UiScale(), 1.5f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().FontScaleDpi, 1.5f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, padding * 1.5f);

    controller_->GetSettings().Set(setting::kUiScale, 200);
    StepFrame();
    EXPECT_FLOAT_EQ(UiScale(), 2.0f) << "the setting, over Windows' 150";
    EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, padding * 2.0f) << "scaled from the base, not from 150%";

    controller_->GetSettings().Set(setting::kUiScale, 0);
    host_.overlayWindow.scalePercent = 100;
    StepFrame();
    EXPECT_FLOAT_EQ(UiScale(), 1.0f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().FontScaleDpi, 1.0f);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, padding);
}

TEST_F(HeadlessAppTest, RendersWithNoCanvasAtAll) {
    StartWithEmptyLibrary();
    ASSERT_FALSE(Canvases().HasCurrentCanvas());
    ShowEditMode();
    StepFrames(2);
    SUCCEED();
}

// ...and a press on it is what gets out of it: a screenshot needs a canvas
// to go on, so one is made.
TEST_F(HeadlessAppTest, ADoubleClickOnAnEmptyLibraryMakesACanvasForTheScreenshot) {
    StartWithEmptyLibrary();
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);
    StepFrames(2);
    ASSERT_TRUE(Canvases().HasCurrentCanvas());
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[0].hasBackground);
}

// ===== What the app did, rather than what it looked like =====

// A marking tool's key picks it for the selected snippet - drawing mode on
// it - and does nothing with no snippet to draw on.
TEST_F(HeadlessAppTest, AShortcutKeyPicksItsTool) {
    ShowEditMode();
    StepFrame();
    ASSERT_EQ(App().ActiveTool(), Tool::Select);  // the default, the hand at rest

    PressKey(ImGuiKey_E);  // "E" ships bound to the eraser
    EXPECT_EQ(App().ActiveTool(), Tool::Select) << "nothing to draw on";

    DoubleClick(640.0f, 400.0f);  // a screenshot, fullscreen
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    RawClick(640.0f, 400.0f);  // selected
    ASSERT_EQ(App().Selection().size(), 1u);
    PressKey(ImGuiKey_E);
    EXPECT_EQ(App().ActiveTool(), Tool::Erase);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(Canvases().CurrentOrNull()->items[0].id));
}

// A shortcut can be a mouse button: here the pen on the first side button,
// and Select on the middle one.
AppConfig WithMouseButtonShortcuts() {
    AppConfig config = DefaultConfig();
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] =
        platform::KeyCombo{false, false, false, platform::KeyCombo::kX1Button};
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Select)] =
        platform::KeyCombo{false, false, false, platform::KeyCombo::kMiddleButton};
    return config;
}

TEST_F(HeadlessAppTest, AMouseButtonRunsTheCommandItIsBoundTo) {
    // A click of ImGui's button `button`, where the pointer is - the side
    // buttons are 3 and 4, which ImGui names no constant for.
    const auto click = [this](int button) {
        MouseButtonEvent(button, true);
        StepFrame();
        MouseButtonEvent(button, false);
        StepFrame();
    };
    StartWith(WithMouseButtonShortcuts());
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 500.0f, 400.0f);  // a screenshot, selected as it is made
    ASSERT_EQ(App().Selection().size(), 1u);
    MoveTo(900.0f, 650.0f);
    StepFrame();

    click(3);
    EXPECT_EQ(App().ActiveTool(), Tool::Draw) << "the pen, for the selected snippet";
    ASSERT_TRUE(App().DrawingItem().has_value());
    click(ImGuiMouseButton_Middle);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_FALSE(App().DrawingItem().has_value());
}

// The side button is on the mouse that is drawing the stroke: pressed
// mid-stroke it waits for the stroke to end rather than ending it, as the
// other button and the wheel do - and works again once it has.
TEST_F(HeadlessAppTest, AMouseButtonWaitsForTheGestureInFlight) {
    // A click of ImGui's button `button`, where the pointer is - the side
    // buttons are 3 and 4, which ImGui names no constant for.
    const auto click = [this](int button) {
        MouseButtonEvent(button, true);
        StepFrame();
        MouseButtonEvent(button, false);
        StepFrame();
    };
    StartWith(WithMouseButtonShortcuts());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 500.0f);
    ASSERT_TRUE(App().DrawingItem().has_value());

    MouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(350.0f, 350.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(450.0f, 400.0f, platform::MouseEventKind::Move);
    StepFrame();
    click(ImGuiMouseButton_Middle);
    EXPECT_TRUE(AppSession().LiveLayer().ActiveStroke().has_value()) << "still drawing";
    EXPECT_EQ(App().ActiveTool(), Tool::Draw);
    RawMouse(500.0f, 420.0f, platform::MouseEventKind::Move);
    RawMouse(500.0f, 420.0f, platform::MouseEventKind::Up);
    MouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrame();
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 1u);

    click(ImGuiMouseButton_Middle);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
}

// A bound button reaches its command as its key would, over a panel too:
// the cheat sheet on the side button opens with it and closes with it
// again, as its header says. Taken for the panel's, as a press on a panel
// is, the button that opened the sheet could not close it.
TEST_F(HeadlessAppTest, AMouseButtonClosesThePanelItOpened) {
    AppConfig config = DefaultConfig();
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::CheatSheet)] =
        platform::KeyCombo{false, false, false, platform::KeyCombo::kX1Button};
    StartWith(config);
    ShowEditMode();
    StepFrame();
    MoveTo(640.0f, 400.0f);
    StepFrame();
    const auto click = [this](int button) {
        MouseButtonEvent(button, true);
        StepFrame();
        MouseButtonEvent(button, false);
        StepFrames(2);
    };
    click(3);
    ASSERT_TRUE(App().IsCheatSheetOpen());
    click(3);
    EXPECT_FALSE(App().IsCheatSheetOpen());
}

TEST_F(HeadlessAppTest, AnUnboundKeyPicksNothing) {
    ShowEditMode();
    StepFrame();
    PressKey(ImGuiKey_T);  // Text ships unbound
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
}

TEST_F(HeadlessAppTest, ACreationToolsKeyPicksTheToolRatherThanPlacing) {
    ShowEditMode();
    StepFrame();
    ASSERT_FALSE(App().ArmedCreation().has_value());

    PressKey(ImGuiKey_D);  // "D" ships bound to the new-drawing tool
    EXPECT_EQ(App().ActiveTool(), Tool::NewDrawing);
    ASSERT_TRUE(App().ArmedCreation().has_value());
    EXPECT_EQ(*App().ArmedCreation(), ItemCreationKind::Drawing);
    // A tool, not a placing: nothing exists until the canvas is pressed.
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
}

// One tool in hand at a time: picking a creation tool puts the others down,
// and picking another puts the creation tool down - never two lit at once.
TEST_F(HeadlessAppTest, TheCreationToolsAreOneChoiceWithTheOthers) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 600.0f, 500.0f);  // in drawing mode, pen in hand
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);
    PressKey(ImGuiKey_S);  // the screenshot tool
    ASSERT_EQ(App().ActiveTool(), Tool::NewScreenshot);
    EXPECT_FALSE(App().DrawingItem().has_value()) << "a creation tool leaves drawing mode";
    PressKey(ImGuiKey_E);  // for the drawing, which is still selected
    EXPECT_EQ(App().ActiveTool(), Tool::Erase);
    EXPECT_FALSE(App().ArmedCreation().has_value()) << "no creation left armed under the eraser";

    PressKey(ImGuiKey_D);
    ASSERT_EQ(App().ActiveTool(), Tool::NewDrawing);
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_FALSE(App().ArmedCreation().has_value());
}

TEST_F(HeadlessAppTest, PlacingADrawingHandsOverToDraw) {
    ShowEditMode();
    StepFrame();
    PressKey(ImGuiKey_D);
    Drag(300.0f, 300.0f, 600.0f, 500.0f);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    // Placed once, and then the drawing is to be drawn in: drawing mode on
    // it, with the pen - so a second drag draws rather than placing
    // another one.
    EXPECT_EQ(App().ActiveTool(), Tool::Draw);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(Canvases().CurrentOrNull()->items[0].id));
    EXPECT_FALSE(App().ArmedCreation().has_value());
}

TEST_F(HeadlessAppTest, PlacingAScreenshotHandsBackTheToolBeforeIt) {
    ShowEditMode();
    StepFrame();
    ASSERT_EQ(App().ActiveTool(), Tool::Select);
    PressKey(ImGuiKey_S);
    Drag(300.0f, 300.0f, 600.0f, 500.0f);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[0].hasBackground);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
}

// With a creation tool in hand the press need not be on empty canvas: over a
// snippet it places the new one there, rather than drawing into the old.
TEST_F(HeadlessAppTest, ACreationToolPlacesOverASnippetToo) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 700.0f, 600.0f);  // a drawing
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    PressKey(ImGuiKey_S);
    Drag(200.0f, 200.0f, 400.0f, 350.0f);

    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u);
}

TEST_F(HeadlessAppTest, DraggingOverAnItemDrawsAStroke) {
    ShowEditMode();
    StepFrame();
    PressKey(ImGuiKey_D);
    Drag(200.0f, 200.0f, 800.0f, 600.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const size_t before = StrokeCountOnCurrentCanvas();

    Drag(300.0f, 300.0f, 500.0f, 450.0f);
    EXPECT_GT(StrokeCountOnCurrentCanvas(), before);
}

// ===== Making a snippet: a press on empty canvas =====
//
// Whatever is in hand: the left button makes a screenshot, the right a
// drawing; a drag frames it, a double-click makes it fullscreen, and a
// plain click makes nothing - a fullscreen snippet is too much to make by
// accident.

TEST_F(HeadlessAppTest, AClickOnEmptyCanvasMakesNothing) {
    ShowEditMode();
    StepFrame();
    RawClick(640.0f, 400.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
    StepFrames(30);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
}

// A new snippet starts with what Settings > Defaults says for its kind -
// its shape, its opacities, a drawing's background color - and with the
// text style set there, for when text is typed into it.
TEST_F(HeadlessAppTest, ANewSnippetStartsWithTheDefaultsForItsKind) {
    AppConfig config = DefaultConfig();
    config.screenshotDefaults = SnippetDefaults{false, 0.5f, 0.75f};
    config.drawingDefaults = SnippetDefaults{false, 0.25f, 0.6f};
    config.drawingBackgroundColorRGBA = 0x112233FFu;
    config.noteTextSizePx = 30.0f;
    config.noteTextColorRGBA = 0xFF000080u;
    StartWith(config);
    ShowEditMode();
    StepFrame();

    DoubleClick(640.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    {
        const Item& shot = Canvases().CurrentOrNull()->items[0];
        ASSERT_TRUE(shot.hasBackground);
        EXPECT_FALSE(shot.keepAspect);
        EXPECT_FLOAT_EQ(shot.foregroundOpacity, 0.5f);
        EXPECT_FLOAT_EQ(shot.picture.opacity, 0.75f);
        EXPECT_EQ(shot.picture.tintColorRGBA, 0xFFFFFFFFu) << "a capture is not tinted by the drawing's color";
        EXPECT_FLOAT_EQ(shot.noteTextSizePx, 30.0f);
        EXPECT_EQ(shot.noteTextColorRGBA, 0xFF000080u);
    }

    PressKey(ImGuiKey_D);
    Drag(200.0f, 200.0f, 800.0f, 600.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    const Item& drawing = Canvases().CurrentOrNull()->items[1];
    ASSERT_FALSE(drawing.hasBackground);
    EXPECT_FALSE(drawing.keepAspect);
    EXPECT_FLOAT_EQ(drawing.foregroundOpacity, 0.25f);
    EXPECT_FLOAT_EQ(drawing.picture.opacity, 0.6f);
    EXPECT_EQ(drawing.picture.tintColorRGBA, 0x112233FFu);
    EXPECT_FLOAT_EQ(drawing.noteTextSizePx, 30.0f);
}

// The default text size is decided on the first frame, at Windows' scale
// then, and saved - not decided again when the scale changes later.
TEST_F(HeadlessAppTest, TheDefaultTextSizeIsDecidedOnceAtTheScaleFirstSeen) {
    host_.overlayWindow.scalePercent = 150;
    ShowEditMode();
    StepFrame();
    EXPECT_FLOAT_EQ(AppSettings().Stored().noteTextSizePx, kDefaultNoteTextSizePx * 1.5f);

    host_.overlayWindow.scalePercent = 100;
    StepFrame();
    EXPECT_FLOAT_EQ(AppSettings().Stored().noteTextSizePx, kDefaultNoteTextSizePx * 1.5f);
}

TEST_F(HeadlessAppTest, ADoubleClickOnEmptyCanvasMakesAFullscreenScreenshot) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_TRUE(item.isFullscreen);
    EXPECT_TRUE(item.hasBackground);
    EXPECT_FALSE(App().ArmedCreation().has_value()) << "spent on release";
    EXPECT_FALSE(App().DrawingItem().has_value()) << "a screenshot is not made to be drawn in";
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{item.id}) << "selected as made, so its bar is there";
}

// The snippet a double-click makes is made on its second press, not its
// release, as a double-click on a snippet enters drawing mode on its
// second press - and what the rest of that press does is nothing: a drag
// from there frames no second snippet (docs/INTERACTIONS.md, 6.4).
TEST_F(HeadlessAppTest, ADoubleClickOnEmptyCanvasMakesTheSnippetOnItsSecondPress) {
    ShowEditMode();
    StepFrame();
    MoveTo(640.0f, 400.0f);
    StepFrame();
    RawMouse(640.0f, 400.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(640.0f, 400.0f, platform::MouseEventKind::Up);
    StepFrame();
    RawMouse(640.0f, 400.0f, platform::MouseEventKind::Down);
    StepFrame();
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u) << "made as the second press lands";
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[0].isFullscreen);
    for (int i = 1; i <= 4; ++i) {
        RawMouse(640.0f + 60.0f * static_cast<float>(i), 400.0f + 40.0f * static_cast<float>(i),
                 platform::MouseEventKind::Move);
        StepFrame();
    }
    RawMouse(880.0f, 560.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "and the drag after it framed nothing";
}

TEST_F(HeadlessAppTest, ADragOnEmptyCanvasFramesAScreenshot) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_TRUE(item.hasBackground);
    EXPECT_FALSE(item.isFullscreen);
    EXPECT_NEAR(item.rect.x, 100.0f, 1.0f);
    EXPECT_NEAR(item.rect.w, 300.0f, 1.0f);
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{item.id}) << "selected as made, so its bar is there";
}

TEST_F(HeadlessAppTest, ACtrlDoubleClickOnEmptyCanvasMakesAFullscreenDrawing) {
    ShowEditMode();
    StepFrame();
    With(ImGuiMod_Ctrl, [&] { DoubleClick(640.0f, 400.0f); });
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_FALSE(item.hasBackground);
    EXPECT_TRUE(item.isFullscreen);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(item.id)) << "a drawing is made to be drawn in";
}

TEST_F(HeadlessAppTest, ACtrlDragOnEmptyCanvasFramesADrawingToDrawIn) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 600.0f, 500.0f);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_FALSE(item.isFullscreen);
    EXPECT_FALSE(item.hasBackground);
    EXPECT_NEAR(item.rect.x, 300.0f, 1.0f);
    EXPECT_NEAR(item.rect.w, 300.0f, 1.0f);
    EXPECT_EQ(App().ActiveTool(), Tool::Draw) << "a drawing is made to be drawn in";
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(item.id));

    Drag(350.0f, 350.0f, 500.0f, 450.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "a press on it draws";
    EXPECT_GT(StrokeCountOnCurrentCanvas(), 0u);
}

// The right button makes nothing on empty canvas: a click opens its menu,
// and a drag, a double-click or a hold does nothing at all.
TEST_F(HeadlessAppTest, ARightClickOnEmptyCanvasOpensItsMenuAndMakesNothing) {
    ShowEditMode();
    StepFrame();
    RightClick(640.0f, 400.0f);
    EXPECT_TRUE(App().IsEmptyCanvasMenuOpen());
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().IsEmptyCanvasMenuOpen());

    Drag(300.0f, 300.0f, 600.0f, 500.0f, 4, platform::MouseButton::Right);
    EXPECT_FALSE(App().IsEmptyCanvasMenuOpen()) << "a drag is no click";
    Hold(640.0f, 400.0f, platform::MouseButton::Right);
    PressKey(ImGuiKey_Escape);
    DoubleClick(640.0f, 400.0f, platform::MouseButton::Right);
    PressKey(ImGuiKey_Escape);
    StepFrames(30);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
}

// Right-clicking empty canvas is moving on from the snippet being drawn
// on, as a left press there is.
TEST_F(HeadlessAppTest, ARightClickOnEmptyCanvasLeavesDrawingModeAndOpensTheMenu) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 600.0f, 500.0f);
    Drag(350.0f, 350.0f, 500.0f, 450.0f);  // something in it, so it stays
    ASSERT_TRUE(App().DrawingItem().has_value());

    RightClick(900.0f, 650.0f);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_TRUE(App().IsEmptyCanvasMenuOpen());
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// Two menus asked for before a frame could open either: the one asked for
// last is the one that comes up, whatever order the frame draws them in -
// see OverlayApp::Effect.
TEST_F(HeadlessAppTest, OfTwoMenusAskedForBetweenFramesTheLastIsTheOneUp) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 600.0f, 500.0f);  // a snippet to open a menu over
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressKey(ImGuiKey_Escape);

    Command emptyCanvas{CommandId::EmptyCanvasMenu};
    emptyCanvas.at = platform::Vec2{900.0f, 650.0f};
    Command item{CommandId::ItemMenu};
    item.item = Canvases().CurrentOrNull()->items[0].id;
    item.at = platform::Vec2{450.0f, 400.0f};
    ASSERT_TRUE(controller_->Overlay().Dispatch(emptyCanvas));
    ASSERT_TRUE(controller_->Overlay().Dispatch(item));
    StepFrames(2);
    EXPECT_TRUE(App().IsItemContextMenuOpen());
    EXPECT_FALSE(App().IsEmptyCanvasMenuOpen());
}

// A popup takes every key but the global hotkeys: undo, Delete, a tool
// would act on the canvas under it, and it would stay up over one that had
// changed (docs/INTERACTIONS.md, decision 2). Escape closes it, and only
// it - the selection it was opened on stays.
TEST_F(HeadlessAppTest, APopupTakesEveryKeyAndEscapeClosesOnlyIt) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 600.0f, 500.0f);  // a screenshot, selected as made
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    RightClick(450.0f, 400.0f);
    ASSERT_TRUE(App().IsItemContextMenuOpen());

    PressCtrlKey(ImGuiKey_Z);
    PressKey(ImGuiKey_Delete);
    PressKey(ImGuiKey_P);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "neither undone nor deleted under the menu";
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_TRUE(App().IsItemContextMenuOpen());

    PressKey(ImGuiKey_Escape);
    StepFrame();
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_EQ(App().Selection().size(), 1u) << "Escape closed the menu and did nothing else";
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "with the menu gone, undo is undo";
}

// A press while a menu is up is the menu's: a right click elsewhere closes
// it and opens nothing else (docs/INTERACTIONS.md, section 5).
TEST_F(HeadlessAppTest, ARightClickWhileAMenuIsUpOnlyClosesIt) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 600.0f, 500.0f);  // a snippet to right-click on
    PressKey(ImGuiKey_Escape);
    RightClick(900.0f, 650.0f);
    ASSERT_TRUE(App().IsEmptyCanvasMenuOpen());

    MoveTo(450.0f, 400.0f);
    StepFrame();
    MouseButtonEvent(ImGuiMouseButton_Right, true);
    RawMouse(450.0f, 400.0f, platform::MouseEventKind::Down, platform::MouseButton::Right);
    StepFrame();
    MouseButtonEvent(ImGuiMouseButton_Right, false);
    RawMouse(450.0f, 400.0f, platform::MouseEventKind::Up, platform::MouseButton::Right);
    StepFrames(3);
    EXPECT_FALSE(App().IsEmptyCanvasMenuOpen());
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_TRUE(App().Selection().empty()) << "and selected nothing";
    EXPECT_EQ(App().InputStack(), "Canvas / - / - / - / - / -");
}

// A canvas switch in the middle of a right click on empty canvas drops it:
// the release that follows opens no menu over the canvas switched to.
TEST_F(HeadlessAppTest, ACaptureMidRightClickOpensNoMenu) {
    ShowEditMode();
    StepFrame();
    MoveTo(640.0f, 400.0f);
    StepFrame();
    RawMouse(640.0f, 400.0f, platform::MouseEventKind::Down, platform::MouseButton::Right);
    StepFrame();
    TriggerHotkey(config_.hotkeyQuickCapture);
    StepFrame();
    RawMouse(640.0f, 400.0f, platform::MouseEventKind::Up, platform::MouseButton::Right);
    StepFrames(2);
    EXPECT_FALSE(App().IsEmptyCanvasMenuOpen());
}

// Which press makes which kind is a setting: here a plain press makes a
// drawing, and screenshots come from the menu or their key alone.
TEST_F(HeadlessAppTest, EachKindIsMadeByThePressItIsSetTo) {
    AppConfig config = DefaultConfig();
    config.screenshotTrigger = CreationTrigger::Off;
    config.drawingTrigger = CreationTrigger::Plain;
    StartWith(config);
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_FALSE(Canvases().CurrentOrNull()->items[0].hasBackground) << "a drawing";
    Drag(150.0f, 150.0f, 300.0f, 250.0f);  // into it, so it stays
    PressKey(ImGuiKey_Escape);

    DragWith(ImGuiMod_Ctrl, 600.0f, 300.0f, 900.0f, 500.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "Ctrl is set to nothing";
    DoubleClick(900.0f, 650.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    EXPECT_FALSE(Canvases().CurrentOrNull()->items[1].hasBackground) << "the fullscreen one is a drawing too";
}

// With Alt set to make screenshots, an Alt double-click is a double-click:
// a modified press is otherwise never half of one.
TEST_F(HeadlessAppTest, AnAltDoubleClickMakesAFullscreenOfWhatAltIsSetTo) {
    AppConfig config = DefaultConfig();
    config.screenshotTrigger = CreationTrigger::Alt;
    config.drawingTrigger = CreationTrigger::Plain;
    StartWith(config);
    ShowEditMode();
    StepFrame();
    With(ImGuiMod_Alt, [&] { DoubleClick(640.0f, 400.0f); });
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[0].isFullscreen);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[0].hasBackground);
}

// The filter a picture is drawn with, as the last frame's draw lists say:
// what the callback just before the draw of `texture` asked for, or
// nothing if there was none - the default sampler. Read from the windows
// rather than ImGui::GetDrawData, which with no renderer behind it lists
// none of them.
std::optional<platform::ImageFilter> FilterDrawnWith(uint64_t texture) {
    for (const ImGuiWindow* window : GImGui->Windows) {
        if (!window->Active) {
            continue;
        }
        const ImVector<ImDrawCmd>& cmds = window->DrawList->CmdBuffer;
        for (int i = 0; i < cmds.Size; ++i) {
            // TexRef's own id rather than GetTexID, which asserts on the
            // font atlas's commands - never uploaded, with no renderer.
            if (cmds[i].UserCallback != nullptr || cmds[i].TexRef._TexData != nullptr ||
                cmds[i].TexRef._TexID != static_cast<ImTextureID>(texture)) {
                continue;
            }
            if (i > 0 && cmds[i - 1].UserCallback == &FakeOverlayWindow::FakeImageFilterCallback) {
                return static_cast<platform::ImageFilter>(reinterpret_cast<intptr_t>(cmds[i - 1].UserCallbackData));
            }
            return std::nullopt;
        }
    }
    ADD_FAILURE() << "texture " << texture << " was not drawn";
    return std::nullopt;
}

// The texture `item`'s picture has this frame, 0 for none.
uint64_t PictureTextureOf(Session& session, ItemId item) {
    return session.Textures().Find(TextureKey{TextureKey::Kind::Picture, item}).value_or(0);
}

// A screenshot is drawn through the filter in settings, and the default
// adds nothing to the draw list - it is ImGui's own sampler.
TEST_F(HeadlessAppTest, AScreenshotIsDrawnThroughTheFilterInSettings) {
    AppConfig config = DefaultConfig();
    config.imageFilter = platform::ImageFilter::Lanczos;
    config.profileable.freezeScreen = true;  // the drag crops the frozen screen
    StartWith(config);
    host_.overlayWindow.captureReturnsWidth = static_cast<int>(kDisplayWidth);
    host_.overlayWindow.captureReturnsHeight = static_cast<int>(kDisplayHeight);
    host_.overlayWindow.captureReturnsPixelsRGBA.assign(static_cast<size_t>(kDisplayWidth * kDisplayHeight) * 4, 255);
    host_.overlayWindow.uploadsSucceed = true;
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    StepFrame();
    const uint64_t picture = PictureTextureOf(controller_->GetSession(), Canvases().CurrentOrNull()->items[0].id);
    ASSERT_NE(picture, 0u);
    EXPECT_EQ(FilterDrawnWith(picture), platform::ImageFilter::Lanczos);

    controller_->GetSettings().Set(setting::kImageFilter, platform::ImageFilter::Bilinear);
    StepFrame();
    EXPECT_EQ(FilterDrawnWith(picture), std::nullopt);
}

// A drawing that was not meant costs nothing: it goes as soon as the hand
// moves on from it without putting anything in. And the press that moves
// on from it is only that, with the modifier that makes a drawing held or
// not - the same modifier draws a rectangle in it, and one begun a little
// outside must not make another drawing.
TEST_F(HeadlessAppTest, AnUntouchedDrawingGoesWhenTheHandMovesOn) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 300.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    MakeADrawing(600.0f, 300.0f, 900.0f, 500.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "only moved on";
    EXPECT_TRUE(Canvases().CurrentOrNull()->items.empty()) << "gone, not merely deleted";
    EXPECT_FALSE(App().DrawingItem().has_value());

    MakeADrawing(600.0f, 300.0f, 900.0f, 500.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(Canvases().CurrentOrNull()->items[0].id))
        << "and the next press makes the next one";
}

// Exit is moving on too, with no next showing to notice: the drawing is
// not saved, to come back after a restart as an ordinary empty snippet.
TEST_F(HeadlessAppTest, AnUntouchedDrawingIsNotSavedOnTheWayOut) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 300.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    host_.TriggerTrayCommand(platform::TrayCommand::Exit);

    EXPECT_TRUE(Canvases().CurrentOrNull()->items.empty());
}

// Coming up settles what went some other way than being put away, in the
// order going away does: the stroke first, into the drawing it is on, and
// only then the drawing, no longer untouched. The other way round the
// drawing went for good with the stroke in flight on it.
TEST_F(HeadlessAppTest, ComingUpKeepsAStrokeInFlightOnANewDrawing) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 400.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    RawMouse(150.0f, 150.0f, platform::MouseEventKind::Down, platform::MouseButton::Left);
    StepFrame();
    RawMouse(250.0f, 200.0f, platform::MouseEventKind::Move, platform::MouseButton::Left);
    StepFrame();

    controller_->Overlay().OnOverlayShown();

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].strokes.size(), 1u);
}

// A paste made by key after a stray drawing is the most recent thing done,
// and the first undo takes the paste back, not the drawing.
TEST_F(HeadlessAppTest, UndoAfterAPasteTakesThePasteBackNotAnUntouchedDrawing) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressCtrlKey(ImGuiKey_C);
    RawClick(900.0f, 650.0f);  // empty canvas: the selection goes
    StepFrames(30);
    MakeADrawing(600.0f, 300.0f, 900.0f, 500.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    PressCtrlKey(ImGuiKey_V);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 3u);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "the paste taken back, and the stray drawing gone as moved on from";
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "and the step before the drawing is next";
}

TEST_F(HeadlessAppTest, ADrawingWithSomethingInItStays) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 400.0f, 400.0f);
    Drag(150.0f, 150.0f, 300.0f, 300.0f);  // a stroke into it
    RawClick(900.0f, 650.0f);              // moving on
    MakeADrawing(600.0f, 300.0f, 900.0f, 500.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u);
}

// A drawing something has gone into is no longer a stray click, even once
// an undo has taken that something back out: moving on leaves it there,
// empty, and the stroke can be redone into it.
TEST_F(HeadlessAppTest, ADrawingEmptiedByUndoIsKeptWhenTheHandMovesOn) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 400.0f, 400.0f);
    Drag(150.0f, 150.0f, 300.0f, 300.0f);  // a stroke into it
    const ItemId drawing = Canvases().CurrentOrNull()->items[0].id;
    ASSERT_EQ(Canvases().CurrentOrNull()->items[0].strokes.size(), 1u);

    PressCtrlKey(ImGuiKey_Z);
    ASSERT_TRUE(Canvases().CurrentOrNull()->items[0].strokes.empty()) << "the stroke, not the drawing";
    RawClick(900.0f, 650.0f);  // empty canvas: the hand moves on

    ASSERT_EQ(Canvases().CurrentOrNull()->items.size(), 1u) << "kept, empty";
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressCtrlKey(ImGuiKey_Y);
    const Item* item = Canvases().CurrentOrNull()->items.data();
    ASSERT_EQ(item->id, drawing);
    EXPECT_EQ(item->strokes.size(), 1u) << "and the stroke is redone into it";
}

TEST_F(HeadlessAppTest, UndoTakesBackAStrayDrawingWithoutLeavingItDeleted) {
    ShowEditMode();
    StepFrame();
    With(ImGuiMod_Ctrl, [&] { DoubleClick(640.0f, 400.0f); });
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items.empty()) << "erased, not kept deleted";
    EXPECT_FALSE(App().DrawingItem().has_value());
}

TEST_F(HeadlessAppTest, UndoDeletesAScreenshotAndRedoBringsItBack) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
    EXPECT_EQ(Canvases().CurrentOrNull()->items.size(), 1u) << "a capture is kept, deleted, where it can be found";

    PressCtrlKey(ImGuiKey_Y);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// ===== The clipboard: Ctrl+C, Ctrl+X, Ctrl+V =====

// A copy leaves the snippet where it is and pastes a second one beside
// it, selected, so it can be moved straight away.
TEST_F(HeadlessAppTest, CopyAndPasteLeaveTwoSnippetsWhereThereWasOne) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId original = Canvases().CurrentOrNull()->items[0].id;
    const Rect where = Canvases().CurrentOrNull()->items[0].rect;
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{original});

    PressCtrlKey(ImGuiKey_C);
    PressCtrlKey(ImGuiKey_V);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    ASSERT_EQ(App().Selection().size(), 1u);
    const ItemId copy = App().Selection().front();
    EXPECT_NE(copy, original) << "the copy is its own snippet";
    const Item* pasted = Canvases().CurrentOrNull()->items.back().id == copy
                              ? &Canvases().CurrentOrNull()->items.back()
                              : nullptr;
    ASSERT_NE(pasted, nullptr) << "and it is in front";
    EXPECT_GT(pasted->rect.x, where.x) << "offset off its source, or it would be invisible under it";
    EXPECT_GT(pasted->rect.y, where.y);

    // The clipboard still holds it: a copy can be pasted as often as
    // wanted.
    PressCtrlKey(ImGuiKey_V);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 3u);
}

// Several selected snippets are copied as one: they land together, each
// offset by the same step, so the shape of the group survives.
TEST_F(HeadlessAppTest, CopyingASelectionPastesAllOfItKeepingItsLayout) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    Drag(600.0f, 400.0f, 800.0f, 560.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    const Rect firstRect = Canvases().CurrentOrNull()->items[0].rect;
    const Rect secondRect = Canvases().CurrentOrNull()->items[1].rect;
    // Both selected: a click on one, Shift held for the other.
    KeyEvent(ImGuiMod_Shift, true);
    StepFrame();
    RawClick(firstRect.x + 40.0f, firstRect.y + 40.0f);
    KeyEvent(ImGuiMod_Shift, false);
    StepFrame();
    ASSERT_EQ(App().Selection().size(), 2u);

    PressCtrlKey(ImGuiKey_C);
    PressCtrlKey(ImGuiKey_V);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 4u);
    ASSERT_EQ(App().Selection().size(), 2u);
    // The copies, told apart by the size of the snippet each came from -
    // they are pasted in the order the two were selected in, which is the
    // order they were clicked, not the order they were made.
    const std::vector<Item>& items = Canvases().CurrentOrNull()->items;
    const Item* copyOfFirst = nullptr;
    const Item* copyOfSecond = nullptr;
    for (size_t at = 2; at < items.size(); ++at) {
        (items[at].rect.w == firstRect.w ? copyOfFirst : copyOfSecond) = &items[at];
    }
    ASSERT_NE(copyOfFirst, nullptr);
    ASSERT_NE(copyOfSecond, nullptr);
    EXPECT_FLOAT_EQ(copyOfSecond->rect.x - copyOfFirst->rect.x, secondRect.x - firstRect.x);
    EXPECT_FLOAT_EQ(copyOfSecond->rect.y - copyOfFirst->rect.y, secondRect.y - firstRect.y);
}

// A cut takes nothing away until the paste that moves it - and the paste
// moves the snippet itself, not a copy of it.
TEST_F(HeadlessAppTest, ACutSnippetStaysUntilItIsPastedSomewhereElse) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    CanvasManager& manager = test::Model(controller_->GetSession());
    const ItemId cut = manager.CurrentOrNull()->items[0].id;
    const CanvasId first = manager.CurrentCanvasId();

    PressCtrlKey(ImGuiKey_X);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "still there, waiting for the paste";

    const CanvasId second = manager.AddCanvas("Second");
    manager.SwitchToCanvas(second);
    StepFrame();
    PressCtrlKey(ImGuiKey_V);

    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(manager.CurrentOrNull()->items[0].id, cut) << "the snippet itself moved, keeping its id";
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{cut});
    const Canvas* wasOn = manager.FindCanvas(first);
    ASSERT_NE(wasOn, nullptr);
    EXPECT_TRUE(wasOn->items.empty()) << "and it is gone from the canvas it was cut on";

    // The clipboard is empty afterwards: those snippets have moved, and
    // pasting again would move them from where they now are.
    PressCtrlKey(ImGuiKey_V);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// A cut pasted back onto its own canvas is the snippet itself, put back
// where it was: nothing moved, so nothing is offset the way a copy is.
TEST_F(HeadlessAppTest, ACutPastedOntoItsOwnCanvasStaysWhereItWas) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId cut = Canvases().CurrentOrNull()->items[0].id;
    const Rect before = Canvases().CurrentOrNull()->items[0].rect;

    PressCtrlKey(ImGuiKey_X);
    PressCtrlKey(ImGuiKey_V);

    ASSERT_EQ(Canvases().CurrentOrNull()->items.size(), 1u);
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].id, cut);
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].rect, before) << "not offset: nothing is on top of anything";
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{cut});
}

// Undo right after a paste takes the paste back - and only that. It used
// to have no step of its own, so the undo reached past it and took back
// whatever came before, out of sight under the copy.
TEST_F(HeadlessAppTest, UndoTakesAPasteBackAndNothingBeforeIt) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    const ItemId original = Canvases().CurrentOrNull()->items[0].id;
    PressCtrlKey(ImGuiKey_C);
    PressCtrlKey(ImGuiKey_V);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_FALSE(Canvases().IsItemDeleted(original)) << "the copy went, not the original";
    PressCtrlKey(ImGuiKey_Y);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u);
}

TEST_F(HeadlessAppTest, UndoTakesADuplicateBack) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    const ItemId original = Canvases().CurrentOrNull()->items[0].id;
    PressCtrlKey(ImGuiKey_D);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_FALSE(Canvases().IsItemDeleted(original)) << "the copy went, not the original";
}

// A cut's paste undone sends the snippet back to the canvas it was cut
// on, and redone brings it again.
TEST_F(HeadlessAppTest, UndoSendsACutsPasteBackWhereItCameFrom) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    CanvasManager& manager = test::Model(controller_->GetSession());
    const ItemId cut = manager.CurrentOrNull()->items[0].id;
    const CanvasId first = manager.CurrentCanvasId();
    PressCtrlKey(ImGuiKey_X);
    const CanvasId second = manager.AddCanvas("Second");
    manager.SwitchToCanvas(second);
    StepFrame();
    PressCtrlKey(ImGuiKey_V);
    ASSERT_EQ(manager.CanvasHoldingItem(cut), std::optional<CanvasId>(second));

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(manager.CanvasHoldingItem(cut), std::optional<CanvasId>(first));
    EXPECT_TRUE(App().Selection().empty()) << "nothing selected that is not here";
    PressCtrlKey(ImGuiKey_Y);
    EXPECT_EQ(manager.CanvasHoldingItem(cut), std::optional<CanvasId>(second));
}

// The clipboard holds ids, so a paste asks for the snippets as they are
// now: one deleted in between is simply not pasted.
TEST_F(HeadlessAppTest, ASnippetDeletedAfterBeingCopiedIsNotPasted) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    PressCtrlKey(ImGuiKey_C);
    PressKey(ImGuiKey_Delete);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 0u);

    PressCtrlKey(ImGuiKey_V);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "nothing left to paste, and nothing resurrected";
}

// Escape calls a cut off: the snippets were never taken away, so there is
// only the waiting to stop.
TEST_F(HeadlessAppTest, EscapeCallsOffACutBeforeItClearsTheSelection) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId only = Canvases().CurrentOrNull()->items[0].id;

    PressCtrlKey(ImGuiKey_X);
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{only}) << "the first Escape was the cut's";

    PressCtrlKey(ImGuiKey_V);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "nothing on the clipboard to paste";
    PressKey(ImGuiKey_Escape);
    EXPECT_TRUE(App().Selection().empty()) << "the next one is the selection's";
}

// ===== Duplicate, and a canvas of their own =====

// Ctrl+D is Copy and Paste in one step: a copy of what is selected, on
// this canvas, offset off its source and selected in its place. What it
// must not do is go through the clipboard, which is why this copies one
// snippet first and pastes it afterwards - the duplicate in between would
// have quietly taken its place.
TEST_F(HeadlessAppTest, DuplicatingTheSelectionLeavesTheClipboardAlone) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);  // a wide one, onto the clipboard
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Rect copied = Canvases().CurrentOrNull()->items[0].rect;
    PressCtrlKey(ImGuiKey_C);

    Drag(600.0f, 400.0f, 700.0f, 620.0f);  // a tall one, selected now
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    const ItemId tall = App().Selection().front();
    const Rect tallRect = Canvases().CurrentOrNull()->items[1].rect;

    PressCtrlKey(ImGuiKey_D);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 3u) << "the selected snippet was duplicated";
    ASSERT_EQ(App().Selection().size(), 1u);
    const ItemId duplicate = App().Selection().front();
    EXPECT_NE(duplicate, tall) << "the copy is what is selected, ready to be dragged off";
    const Item& made = Canvases().CurrentOrNull()->items.back();
    ASSERT_EQ(made.id, duplicate);
    EXPECT_GT(made.rect.x, tallRect.x) << "offset off its source, or it would be invisible under it";
    EXPECT_GT(made.rect.y, tallRect.y);
    EXPECT_NEAR(made.rect.w, tallRect.w, 0.01f) << "and the same snippet otherwise";

    PressCtrlKey(ImGuiKey_V);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 4u);
    EXPECT_NEAR(Canvases().CurrentOrNull()->items.back().rect.w, copied.w, 0.01f)
        << "the paste is of the wide one, so Ctrl+D never touched the clipboard";
}

TEST_F(HeadlessAppTest, DuplicatingWithNothingSelectedMakesNothing) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    RawClick(900.0f, 650.0f);  // empty canvas: clears the selection
    ASSERT_TRUE(App().Selection().empty());

    PressCtrlKey(ImGuiKey_D);

    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// Ctrl+Shift+N: a new canvas that the selected snippets come along to,
// which is what "these belong somewhere of their own" otherwise costs a
// cut, two canvas switches and a paste.
TEST_F(HeadlessAppTest, TheSelectionCanBeTakenToANewCanvas) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);  // stays behind
    Drag(600.0f, 400.0f, 800.0f, 560.0f);  // goes
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    const CanvasId before = Canvases().CurrentOrNull()->id;
    const FolderId folder = Canvases().CurrentOrNull()->folderId;
    const ItemId staying = Canvases().CurrentOrNull()->items[0].id;
    const ItemId going = App().Selection().front();
    ASSERT_NE(going, staying);

    PressCtrlShiftKey(ImGuiKey_N);

    ASSERT_EQ(Canvases().Canvases().size(), 2u);
    const Canvas& made = Canvases().Canvases().back();
    EXPECT_NE(made.id, before);
    EXPECT_EQ(made.folderId, folder) << "in the folder the work was in";
    EXPECT_EQ(Canvases().CurrentOrNull()->id, made.id) << "and it is the canvas you are now on";
    ASSERT_EQ(made.items.size(), 1u);
    EXPECT_EQ(made.items[0].id, going) << "the snippet itself moved, rather than a copy of it";
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{going}) << "still selected, to be arranged";

    const Canvas& left = Canvases().Canvases().front();
    ASSERT_EQ(left.id, before);
    ASSERT_EQ(left.items.size(), 1u);
    EXPECT_EQ(left.items[0].id, staying) << "what was not selected stayed where it was";
}

// The browsed folder and the current canvas's are decoupled: browsing
// another folder in the Overview and closing it without switching leaves
// them apart. A canvas made from the canvas itself goes beside the work,
// not wherever the Overview was last looking.
TEST_F(HeadlessAppTest, ACanvasMadeFromTheCanvasGoesBesideTheCurrentOneNotTheBrowsedFolder) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    CanvasManager& manager = test::Model(controller_->GetSession());
    const FolderId workFolder = manager.CurrentOrNull()->folderId;
    const FolderId browsed = manager.AddFolder("Elsewhere");  // browsed, as the Overview would leave it
    ASSERT_EQ(manager.CurrentFolderId(), browsed);
    ASSERT_EQ(manager.CurrentOrNull()->folderId, workFolder);

    PressCtrlShiftKey(ImGuiKey_N);

    ASSERT_EQ(manager.Canvases().size(), 2u);
    EXPECT_EQ(manager.CurrentOrNull()->folderId, workFolder) << "beside the canvas the work was on";
    EXPECT_EQ(manager.CurrentFolderId(), workFolder) << "and the Overview follows";
    for (const Canvas& canvas : manager.Canvases()) {
        EXPECT_EQ(canvas.folderId, workFolder);
    }
}

TEST_F(HeadlessAppTest, TakingNothingToANewCanvasIsJustANewCanvas) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    RawClick(900.0f, 650.0f);  // empty canvas: clears the selection
    ASSERT_TRUE(App().Selection().empty());
    const CanvasId before = Canvases().CurrentOrNull()->id;

    PressCtrlShiftKey(ImGuiKey_N);

    ASSERT_EQ(Canvases().Canvases().size(), 2u) << "an empty selection is no reason to refuse the canvas";
    EXPECT_NE(Canvases().CurrentOrNull()->id, before);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items.empty());
    EXPECT_EQ(Canvases().Canvases().front().items.size(), 1u) << "and the snippet stayed behind";
}

// A shortcut can land while the button is held. What the hand was doing
// ends on the canvas it started on, before the switch: the stroke in
// flight is committed to its snippet rather than left dangling on a canvas
// nobody is looking at, with its undo entry filed under the new one.
TEST_F(HeadlessAppTest, AShortcutThatSwitchesCanvasEndsTheStrokeInFlightFirst) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 400.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    CanvasManager& manager = test::Model(controller_->GetSession());
    const CanvasId before = manager.CurrentCanvasId();
    const ItemId drawing = manager.CurrentOrNull()->items[0].id;

    // A stroke pressed and moved, not let go of - ImGui told about the
    // button too, since settling asks it whether the button is down.
    MouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(150.0f, 150.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    ASSERT_TRUE(AppSession().LiveLayer().ActiveStroke().has_value()) << "in flight";

    PressCtrlShiftKey(ImGuiKey_N);

    ASSERT_NE(manager.CurrentCanvasId(), before);
    const Item* item = manager.FindItemAnywhere(drawing);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->strokes.size(), 1u) << "ended on the canvas it started on, before the switch";
    EXPECT_FALSE(AppSession().LiveLayer().ActiveStroke().has_value());
    EXPECT_FALSE(AppSession().LiveLayer().ActiveStroke().has_value());

    // The real release comes later and finds nothing in flight.
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Up);
    MouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrames(2);
    EXPECT_EQ(manager.FindItemAnywhere(drawing)->strokes.size(), 1u);
}

// A press of the button that is down already: its release was lost on the
// way. The stroke it was drawing ends where it got to, and the new press
// starts a stroke of its own rather than drawing a line on from the last.
TEST_F(HeadlessAppTest, APressAfterALostReleaseEndsWhatThatButtonWasDoing) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 700.0f, 500.0f);
    CanvasManager& manager = test::Model(controller_->GetSession());
    const ItemId drawing = manager.CurrentOrNull()->items[0].id;

    MouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(150.0f, 150.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    // No Up - and the frames in between have the pointer where the next
    // press is, as the hover frames before it would.
    ImGui::GetIO().AddMousePosEvent(500.0f, 150.0f);
    StepFrame();
    RawMouse(500.0f, 150.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(600.0f, 150.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(600.0f, 150.0f, platform::MouseEventKind::Up);
    MouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrame();
    const Item* item = manager.FindItemAnywhere(drawing);
    ASSERT_EQ(item->strokes.size(), 2u);
    // The first ends where it got to, well short of where the second
    // starts - not with a straight tail to the next press.
    float firstRight = -1e9f;
    for (const StrokePoint& point : item->strokes[0].points) {
        firstRight = std::max(firstRight, point.x);
    }
    float secondLeft = 1e9f;
    for (const StrokePoint& point : item->strokes[1].points) {
        secondLeft = std::min(secondLeft, point.x);
    }
    EXPECT_LT(firstRight, secondLeft);
}

// A release lost with the overlay going away: the button is not taken for
// held when it comes back, and the other one is not ignored for it.
TEST_F(HeadlessAppTest, AReleaseLostWhileHiddenLeavesNoButtonHeld) {
    ShowEditMode();
    StepFrame();
    RawMouse(900.0f, 650.0f, platform::MouseEventKind::Down, platform::MouseButton::Right);
    StepFrame();
    ShowEditMode();  // away, the release lost on the way
    ShowEditMode();  // and back
    StepFrame();
    const size_t before = ItemCountOnCurrentCanvas();
    Drag(200.0f, 200.0f, 500.0f, 400.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), before + 1) << "the left button's drag, not ignored";
}

// The ignored button's release lost: pressed again, it is taken as that
// release and then as the press it is, not swallowed with its own release.
TEST_F(HeadlessAppTest, AnIgnoredButtonPressedAgainIsNotIgnoredStill) {
    ShowEditMode();
    StepFrame();
    MoveTo(900.0f, 650.0f);
    RawMouse(900.0f, 650.0f, platform::MouseEventKind::Down, platform::MouseButton::Left);
    StepFrame();
    RawMouse(900.0f, 650.0f, platform::MouseEventKind::Down, platform::MouseButton::Right);  // ignored
    StepFrame();
    RawMouse(900.0f, 650.0f, platform::MouseEventKind::Up, platform::MouseButton::Left);
    StepFrames(30);  // the right button's release lost
    RightClick(900.0f, 650.0f);
    EXPECT_TRUE(App().IsEmptyCanvasMenuOpen());
}

// Whatever the pointer is in the middle of - a stroke, a shape, an erase,
// a move or a resize, a box, a snippet being framed, a hold, a right
// click, with releases lost and the other button pressed on top - every
// command in the table (see ui/interaction/command.h), by its key, its
// hotkey, or dispatched as a menu row or a bar button would, ends it first
// (see Editor::Settle): straight after one that ran, nothing is in
// flight in the app or open on the session, and a stroke it interrupted
// is kept. In between, nothing is left open on the session that the hand
// has let go of. Once every button has been pressed and let go, the hand
// is at rest whatever came before. And every command ran somewhere.
TEST_F(HeadlessAppTest, EveryCommandSettlesTheHandWhateverItInterrupts) {
    using platform::MouseButton;
    using platform::MouseEventKind;
    constexpr uint32_t kSeeds = 16;
    constexpr int kSteps = 200;
    const ImGuiKey kModifiers[] = {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt};
    // How many strokes a command found in flight and was checked to keep -
    // enough that the check is not one that never runs - and which
    // commands ran at all.
    size_t strokesInterrupted = 0;
    std::vector<bool> ran(kCommandCount, false);
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        host_.overlayWindow.visible = false;  // the last seed's overlay went with its app
        StartWith(DefaultConfig());
        ShowEditMode();
        StepFrame();
        Drag(100.0f, 100.0f, 500.0f, 400.0f);           // a screenshot
        Drag(700.0f, 650.0f, 400.0f, 300.0f);           // another over it
        MakeADrawing(600.0f, 150.0f, 1100.0f, 600.0f);  // and a drawing over that, in drawing mode
        StepFrames(30);
        Session& session = controller_->GetSession();
        std::mt19937 rng(seed);
        const auto pick = [&rng](size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(rng); };
        // Where a press lands: on a snippet, mostly, and anywhere else.
        const auto somewhere = [&]() -> ImVec2 {
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            const Canvas* canvas = Canvases().CurrentOrNull();
            if (canvas != nullptr && !canvas->items.empty() && pick(4) != 0) {
                const Rect& rect = canvas->items[pick(canvas->items.size())].rect;
                return ImVec2(rect.x + rect.w * unit(rng), rect.y + rect.h * unit(rng));
            }
            return ImVec2(kDisplayWidth * unit(rng), kDisplayHeight * unit(rng));
        };
        ImVec2 pointer(640.0f, 400.0f);
        std::vector<MouseButton> held;    // down, as the hand on the mouse knows it
        std::vector<ImGuiKey> modifiers;  // likewise
        const auto imguiButton = [](MouseButton button) {
            return button == MouseButton::Left ? ImGuiMouseButton_Left : ImGuiMouseButton_Right;
        };
        // A key as a hand presses it: its modifiers, the key, and all of it
        // let go again.
        const auto press = [&](const platform::KeyCombo& key) {
            const std::pair<bool, ImGuiKey> mods[] = {
                {key.ctrl, ImGuiMod_Ctrl}, {key.alt, ImGuiMod_Alt}, {key.shift, ImGuiMod_Shift}};
            for (const auto& [down, mod] : mods) {
                if (down) {
                    KeyEvent(mod, true);
                }
            }
            KeyEvent(ui::ImGuiKeyForCombo(key), true);
            StepFrame();
            KeyEvent(ui::ImGuiKeyForCombo(key), false);
            for (const auto& [down, mod] : mods) {
                if (down) {
                    KeyEvent(mod, false);
                }
            }
            StepFrame();
        };
        // The stroke a command would find in flight, big enough to be kept
        // whatever its shape: the snippet it goes into, and how many
        // strokes that has now.
        const auto strokeToKeep = [&]() -> std::optional<std::pair<ItemId, size_t>> {
            const std::optional<Stroke>& active = session.LiveLayer().ActiveStroke();
            if (!active.has_value() || active->points.empty() || !App().DrawingItem().has_value()) {
                return std::nullopt;
            }
            float x0 = active->points[0].x;
            float x1 = x0;
            float y0 = active->points[0].y;
            float y1 = y0;
            for (const StrokePoint& point : active->points) {
                x0 = std::min(x0, point.x);
                x1 = std::max(x1, point.x);
                y0 = std::min(y0, point.y);
                y1 = std::max(y1, point.y);
            }
            const Item* item = Canvases().FindItemAnywhere(*App().DrawingItem());
            if (item == nullptr || std::max(x1 - x0, y1 - y0) < 30.0f) {
                return std::nullopt;
            }
            return std::pair{item->id, item->strokes.size()};
        };

        for (int step = 0; step < kSteps && !HasFailure(); ++step) {
            // Edit mode, which is where the hand does anything: a command
            // before may have put the overlay away or switched it to view.
            if (!host_.overlayWindow.visible || App().IsViewOnly()) {
                ShowEditMode();
                StepFrame();
            }
            // A panel a command opened is closed again, now and then, so
            // the canvas is not out of reach for most of a seed.
            if ((App().IsOverviewOpen() || App().IsCheatSheetOpen()) && pick(3) == 0) {
                PressKey(ImGuiKey_Escape);
            }
            const size_t what = pick(19);
            if (what >= 16 && held.empty()) {
                // A stroke begun on a snippet and carried some way, left in
                // flight for what comes next - into drawing mode first, the
                // way a hand gets there, if no snippet is in it.
                const Canvas* canvas = Canvases().CurrentOrNull();
                if (!App().DrawingItem().has_value() && canvas != nullptr && !canvas->items.empty()) {
                    for (const ImGuiKey modifier : modifiers) {
                        KeyEvent(modifier, false);
                    }
                    modifiers.clear();
                    const Rect& rect = canvas->items[pick(canvas->items.size())].rect;
                    DoubleClick(rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f);
                }
                if (const Item* item = App().DrawingItem().has_value()
                                           ? Canvases().FindItemAnywhere(*App().DrawingItem())
                                           : nullptr) {
                    pointer = ImVec2(item->rect.x + item->rect.w * 0.3f, item->rect.y + item->rect.h * 0.3f);
                    MoveTo(pointer.x, pointer.y);
                    StepFrame();
                    RawMouse(pointer.x, pointer.y, MouseEventKind::Down);
                    MouseButtonEvent(ImGuiMouseButton_Left, true);
                    held.push_back(MouseButton::Left);
                    for (int i = 0; i < 3; ++i) {
                        pointer = ImVec2(pointer.x + 20.0f, pointer.y + 15.0f);
                        RawMouse(pointer.x, pointer.y, MouseEventKind::Move);
                        StepFrame();
                    }
                }
            } else if (what < 2) {
                const MouseButton button = pick(3) == 0 ? MouseButton::Right : MouseButton::Left;
                pointer = somewhere();
                MoveTo(pointer.x, pointer.y);
                StepFrame();
                RawMouse(pointer.x, pointer.y, MouseEventKind::Down, button);
                MouseButtonEvent(imguiButton(button), true);
                if (std::find(held.begin(), held.end(), button) == held.end()) {
                    held.push_back(button);
                }
            } else if (what < 5) {
                std::uniform_real_distribution<float> by(-150.0f, 150.0f);
                pointer = ImVec2(std::clamp(pointer.x + by(rng), 0.0f, kDisplayWidth - 1.0f),
                                 std::clamp(pointer.y + by(rng), 0.0f, kDisplayHeight - 1.0f));
                const MouseButton button = held.empty() ? MouseButton::Left : held[pick(held.size())];
                RawMouse(pointer.x, pointer.y, MouseEventKind::Move, button);
            } else if (what < 7) {
                if (!held.empty()) {
                    const size_t which = pick(held.size());
                    const MouseButton button = held[which];
                    held.erase(held.begin() + static_cast<std::ptrdiff_t>(which));
                    if (pick(4) != 0) {  // and otherwise the release is lost on the way
                        RawMouse(pointer.x, pointer.y, MouseEventKind::Up, button);
                    }
                    MouseButtonEvent(imguiButton(button), false);
                }
            } else if (what == 7) {
                const ImGuiKey modifier = kModifiers[pick(3)];
                const auto it = std::find(modifiers.begin(), modifiers.end(), modifier);
                KeyEvent(modifier, it == modifiers.end());
                if (it == modifiers.end()) {
                    modifiers.push_back(modifier);
                } else {
                    modifiers.erase(it);
                }
            } else if (what == 8) {
                WheelEvent(pick(2) == 0 ? 1.0f : -1.0f);
            } else if (what == 9) {
                StepFrames(35);  // long enough for a hold to mature
            } else if (what < 16) {
                // A command, by whatever reaches it: its key, its global
                // hotkey, or - for one only a menu or the selection bar
                // reaches - dispatched the way those do, about a snippet
                // and a canvas picked at random.
                const auto id = static_cast<CommandId>(pick(kCommandCount));
                const CommandInfo& info = InfoFor(id);
                const std::vector<platform::KeyCombo> keys =
                    KeysFor(id, AppSettings().Stored(), AppSettings().Live().shortcuts);
                if (!info.hotkey.has_value() && !keys.empty()) {
                    // A key wants exactly its own modifiers, so the ones the
                    // hand holds are let go of first.
                    for (const ImGuiKey modifier : modifiers) {
                        KeyEvent(modifier, false);
                    }
                    modifiers.clear();
                    StepFrame();
                }
                const std::optional<std::pair<ItemId, size_t>> stroke = strokeToKeep();
                const uint64_t before = App().CommandsRun();
                if (info.hotkey.has_value()) {
                    if (!keys.empty()) {
                        TriggerHotkey(keys.front());
                    }
                } else if (!keys.empty()) {
                    press(keys[pick(keys.size())]);
                } else {
                    Command command{id};
                    const Canvas* canvas = Canvases().CurrentOrNull();
                    if (canvas != nullptr && !canvas->items.empty()) {
                        command.item = canvas->items[pick(canvas->items.size())].id;
                    }
                    command.canvas = Canvases().Canvases()[pick(Canvases().Canvases().size())].id;
                    command.at = platform::Vec2{pointer.x, pointer.y};
                    command.rect = Rect{pointer.x, pointer.y, 200.0f, 150.0f};
                    controller_->Overlay().Dispatch(command);
                }
                if (App().CommandsRun() > before) {
                    const CommandId last = *App().LastCommand();
                    const std::string name(InfoFor(last).name);
                    ran[static_cast<size_t>(last)] = true;
                    ASSERT_FALSE(session.LiveLayer().ActiveStroke().has_value()) << "step " << step << ", " << name;
                    if (IsNudge(last)) {
                        // A step of the burst it began or went on with,
                        // which is in the hand now, holding it open.
                        ASSERT_NE(App().InputStack().find("NudgeBurst"), std::string::npos)
                            << "step " << step << ", " << name;
                    } else {
                        ASSERT_TRUE(App().HandAtRest()) << "step " << step << ", " << name;
                        ASSERT_FALSE(HandGestureOpen(session)) << "step " << step << ", " << name;
                    }
                    // Kept - and an undo then takes it back, as the most
                    // recent thing done; clearing the drawing takes it along.
                    if (stroke.has_value() && last != CommandId::ClearDrawing) {
                        ++strokesInterrupted;
                        const Item* item = Canvases().FindItemAnywhere(stroke->first);
                        ASSERT_NE(item, nullptr) << "step " << step;
                        EXPECT_EQ(item->strokes.size(), stroke->second + (last == CommandId::Undo ? 0u : 1u))
                            << "step " << step << ", " << name;
                    }
                }
            }
            StepFrames(1 + static_cast<int>(pick(2)));
            // Nothing the hand has let go of is left open on the session.
            if (App().HandAtRest() && !App().EditingNote().has_value()) {
                ASSERT_FALSE(HandGestureOpen(session)) << "step " << step;
            }
        }

        // Every button let go of, and each pressed and let go of once more:
        // whatever was lost on the way, the hand is at rest.
        for (const MouseButton button : held) {
            RawMouse(pointer.x, pointer.y, MouseEventKind::Up, button);
            MouseButtonEvent(imguiButton(button), false);
        }
        for (const ImGuiKey modifier : modifiers) {
            KeyEvent(modifier, false);
        }
        StepFrame();
        if (!host_.overlayWindow.visible || App().IsViewOnly()) {
            ShowEditMode();
            StepFrame();
        }
        RawClick(20.0f, 20.0f);
        RightClick(20.0f, 20.0f);
        EXPECT_TRUE(App().HandAtRest());
    }
    EXPECT_GE(strokesInterrupted, 10u);
    for (const CommandInfo& info : kCommands) {
        EXPECT_TRUE(ran[static_cast<size_t>(info.id)]) << info.name << " never ran";
    }
}

// ===== The panels docked against the screen's edges =====

// Enough frames for the moment the panels come out when the overlay comes
// up (1.6 s), the linger after it, and the slide back in.
constexpr int kFramesPastAFlash = 150;

TEST_F(HeadlessAppTest, TheCanvasBarComesOutWithTheOverlayAndGoesAgain) {
    ShowEditMode();
    StepFrames(20);
    EXPECT_GE(App().CanvasBarReveal(), 1.0f) << "out for a moment, so it is seen";
    StepFrames(kFramesPastAFlash);
    EXPECT_EQ(App().CanvasBarReveal(), 0.0f);
}

TEST_F(HeadlessAppTest, ThePointerAtTheBottomEdgeBringsTheCanvasBarOut) {
    ShowEditMode();
    MoveTo(640.0f, 300.0f);
    StepFrames(kFramesPastAFlash);
    ASSERT_EQ(App().CanvasBarReveal(), 0.0f);

    MoveTo(640.0f, kDisplayHeight - 1.0f);
    StepFrames(20);
    EXPECT_GE(App().CanvasBarReveal(), 1.0f);

    MoveTo(640.0f, 300.0f);
    StepFrames(60);
    EXPECT_EQ(App().CanvasBarReveal(), 0.0f) << "and back in once the pointer has left it";
}

TEST_F(HeadlessAppTest, AChangeOfCanvasBringsTheBarOutForAMoment) {
    AppConfig config = DefaultConfig();
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::NewCanvas)] =
        platform::KeyCombo{/*ctrl=*/false, /*alt=*/false, /*shift=*/false, /*key=*/'C'};
    StartWith(std::move(config));
    ShowEditMode();
    MoveTo(640.0f, 300.0f);
    StepFrames(kFramesPastAFlash);
    ASSERT_EQ(App().CanvasBarReveal(), 0.0f);

    PressKey(ImGuiKey_C);
    StepFrames(20);
    EXPECT_GE(App().CanvasBarReveal(), 1.0f);
}

// A combo one of the app's own hotkeys already has never reaches the
// capture loop as a key press - Windows hands a registered combination
// to the hotkey and to nothing else - so the hotkey firing is the press:
// it completes the capture instead of doing its usual job, and the row
// that had the combo is left unbound.
TEST_F(HeadlessAppTest, AHotkeyPressedWhileCapturingBecomesTheCapturedCombo) {
    ShowEditMode();
    StepFrame();
    controller_->Overlay().ArmHotkeyCapture(HotkeySlot::EditMode);
    ASSERT_TRUE(App().IsCapturingHotkey());

    TriggerHotkey(config_.hotkeyViewMode);
    StepFrame();

    EXPECT_FALSE(App().IsCapturingHotkey());
    EXPECT_FALSE(App().IsViewOnly()) << "the press was captured, not acted on";
    EXPECT_TRUE(host_.overlayWindow.visible);
    const AppConfig& stored = controller_->GetSettings().Stored();
    EXPECT_EQ(stored.hotkeyEditMode, config_.hotkeyViewMode);
    EXPECT_FALSE(stored.hotkeyViewMode.IsValid()) << "taken from the view hotkey";
}

// A hotkey another application held at the start has its combo and no
// registration. Picking the same combo again, once it is free, registers
// it - rather than counting as no change.
TEST_F(HeadlessAppTest, PickingTheSameComboAgainRegistersAHotkeyThatFailedAtTheStart) {
    host_.registerHotkeySucceeds = false;
    StartWith(DefaultConfig());
    ASSERT_FALSE(controller_->UnregisteredHotkeys().empty());
    host_.registerHotkeySucceeds = true;

    controller_->Overlay().ArmHotkeyCapture(HotkeySlot::EditMode);
    controller_->Overlay().CompleteHotkeyCapture(config_.hotkeyEditMode);

    bool registered = false;
    for (const auto& [id, combo] : host_.registeredCombos) {
        registered = registered || combo == config_.hotkeyEditMode;
    }
    EXPECT_TRUE(registered);
}

// A hotkey row and a shortcut row wait for the next key on the same page,
// and one press bound it to both. Only the row armed last waits.
TEST_F(HeadlessAppTest, ArmingAHotkeyOrAShortcutCaptureDisarmsTheOther) {
    controller_->Overlay().ArmHotkeyCapture(HotkeySlot::ViewMode);
    controller_->Overlay().ArmShortcutCapture(ShortcutAction::CheatSheet);
    EXPECT_FALSE(App().IsCapturingHotkey());
    EXPECT_TRUE(App().IsCapturingShortcut());

    controller_->Overlay().ArmHotkeyCapture(HotkeySlot::ViewMode);
    EXPECT_TRUE(App().IsCapturingHotkey());
    EXPECT_FALSE(App().IsCapturingShortcut());
}

// A row waiting for a key takes the next one pressed, with its modifiers,
// from the input stream - over the Overview, which takes every other key.
// Escape on a shortcut's row binds nothing; on a hotkey's it only stops the
// row waiting, and neither closes the Overview.
TEST_F(HeadlessAppTest, AWaitingRowTakesTheNextKeyAndEscapeStopsIt) {
    ShowEditMode();
    StepFrame();
    Command settings{CommandId::Settings};
    ASSERT_TRUE(controller_->Overlay().Dispatch(settings));
    StepFrame();
    ASSERT_EQ(App().InputStack(), "Canvas / - / Overview / - / - / -");

    controller_->Overlay().ArmShortcutCapture(ShortcutAction::Copy);
    EXPECT_EQ(App().InputStack(), "Canvas / - / Overview / - / KeyCapture / -");
    KeyEvent(ImGuiMod_Ctrl, true);
    KeyEvent(ImGuiMod_Shift, true);
    PressKey(ImGuiKey_K);
    KeyEvent(ImGuiMod_Shift, false);
    KeyEvent(ImGuiMod_Ctrl, false);
    StepFrame();
    EXPECT_FALSE(App().IsCapturingShortcut());
    EXPECT_EQ(AppSettings().Stored().profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Copy)],
              (platform::KeyCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/true, 'K'}));

    controller_->Overlay().ArmShortcutCapture(ShortcutAction::Copy);
    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(AppSettings().Stored().profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Copy)].IsValid())
        << "Escape binds nothing";
    EXPECT_TRUE(App().IsOverviewOpen());

    const platform::KeyCombo hotkey = AppSettings().Stored().hotkeyEditMode;
    controller_->Overlay().ArmHotkeyCapture(HotkeySlot::EditMode);
    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().IsCapturingHotkey());
    EXPECT_EQ(AppSettings().Stored().hotkeyEditMode, hotkey) << "a hotkey's row only stops waiting";
    EXPECT_TRUE(App().IsOverviewOpen());

    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().IsOverviewOpen()) << "with nothing waiting, Escape closes the Overview";
}

// ===== The hand at rest: Select =====

// A marking tool is in hand only in drawing mode, so putting it down -
// Escape, or its own key again - is leaving drawing mode. The snippet
// stays selected, so the key picks it up again.
TEST_F(HeadlessAppTest, EscapeAndTheSameKeyAgainPutTheToolDown) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 600.0f, 500.0f);
    PressKey(ImGuiKey_E);
    ASSERT_EQ(App().ActiveTool(), Tool::Erase);
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(App().Selection().size(), 1u) << "still selected";

    PressKey(ImGuiKey_E);
    ASSERT_EQ(App().ActiveTool(), Tool::Erase);
    EXPECT_TRUE(App().DrawingItem().has_value());
    PressKey(ImGuiKey_E);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_FALSE(App().DrawingItem().has_value());
}

TEST_F(HeadlessAppTest, WithSelectInHandADragAnywhereOnASnippetPicksItUp) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 300.0f, 300.0f);  // a screenshot
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    ASSERT_EQ(App().ActiveTool(), Tool::Select);

    Drag(200.0f, 200.0f, 400.0f, 250.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_NEAR(item.rect.x, 300.0f, 1.0f);
    EXPECT_NEAR(item.rect.y, 150.0f, 1.0f);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u);
}

// A right click on a snippet - one not being drawn on - selects it and
// opens its context menu, and makes nothing and changes nothing else; on
// empty canvas a right click opens that menu instead (see the tests under
// "Making a snippet"). Escape then closes the menu, and the *next* Escape clears
// the selection: an open popup takes the first press (see
// Popup).
TEST_F(HeadlessAppTest, ARightClickOnASnippetSelectsItAndOpensItsContextMenu) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);  // a screenshot, fullscreen, to click on
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressKey(ImGuiKey_Escape);
    ASSERT_TRUE(App().Selection().empty());

    RightClick(640.0f, 400.0f);
    EXPECT_EQ(App().Selection().size(), 1u);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_TRUE(App().IsItemContextMenuOpen());
    EXPECT_EQ(App().ItemContextMenuItem(), App().Selection().front());

    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_EQ(App().Selection().size(), 1u) << "the menu took that press, not the selection";

    PressKey(ImGuiKey_Escape);
    EXPECT_TRUE(App().Selection().empty());
}

// The menu belongs to the click that never dragged: a right-drag is a
// resize (see ARightDragFromNearestEdgeResizes...) and must leave no menu
// hanging open over the snippet it just resized.
TEST_F(HeadlessAppTest, ARightDragResizesAndOpensNoContextMenu) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    PressKey(ImGuiKey_Escape);  // out of drawing mode, where right is the eraser
    ASSERT_FALSE(App().DrawingItem().has_value());
    const float before = Canvases().CurrentOrNull()->items[0].rect.w;

    Drag(690.0f, 420.0f, 780.0f, 420.0f, 10, platform::MouseButton::Right);
    EXPECT_GT(Canvases().CurrentOrNull()->items[0].rect.w, before + 1.0f);
    EXPECT_FALSE(App().IsItemContextMenuOpen());
}

// The menu follows the snippet it was opened over: delete that snippet
// while it is up and it closes itself rather than acting on nothing (see
// ContextMenu::Render, which reads an empty row list as "close").
TEST_F(HeadlessAppTest, TheContextMenuClosesWhenItsSnippetGoesAway) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressKey(ImGuiKey_Escape);

    RightClick(640.0f, 400.0f);
    ASSERT_TRUE(App().IsItemContextMenuOpen());

    test::Model(controller_->GetSession()).DeleteItem(*App().ItemContextMenuItem());
    StepFrames(2);
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_FALSE(App().ItemContextMenuItem().has_value());
}

// On the snippet being drawn on, a right-drag is the eraser, whatever tool
// is in hand - a quick correction without changing the tool - and a right
// click leaves drawing mode and opens nothing. Alt+right-drag still
// resizes.
TEST_F(HeadlessAppTest, ARightDragOnTheSnippetBeingDrawnOnErasesAndARightClickLeavesTheMode) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_TRUE(App().DrawingItem().has_value());
    const Rect before = Canvases().CurrentOrNull()->items[0].rect;
    Drag(350.0f, 400.0f, 650.0f, 400.0f);  // a stroke across it
    ASSERT_EQ(StrokeCountOnCurrentCanvas(), 1u);
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);

    // Right-dragged through the middle of the stroke: it is cut in two,
    // and the snippet is neither resized nor left.
    Drag(500.0f, 340.0f, 500.0f, 460.0f, 10, platform::MouseButton::Right);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 2u) << "the eraser cut the stroke";
    EXPECT_FLOAT_EQ(Canvases().CurrentOrNull()->items[0].rect.w, before.w);
    EXPECT_TRUE(App().DrawingItem().has_value());
    EXPECT_EQ(App().ActiveTool(), Tool::Draw) << "the pen is still in hand";

    // With Alt, the resize from the nearest edge.
    KeyEvent(ImGuiMod_Alt, true);
    StepFrame();
    Drag(680.0f, 330.0f, 780.0f, 330.0f, 10, platform::MouseButton::Right);
    KeyEvent(ImGuiMod_Alt, false);
    StepFrame();
    // Grew, rather than by exactly the 100px of drag: the press landed in
    // a corner, and an aspect-locked corner follows the pointer projected
    // onto the snippet's own diagonal (see ItemGeometryTest). Sideways
    // motion therefore counts for less than all of itself.
    EXPECT_GT(Canvases().CurrentOrNull()->items[0].rect.w, before.w + 1.0f);
    EXPECT_TRUE(App().DrawingItem().has_value());

    RightClick(500.0f, 425.0f);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_EQ(App().Selection().size(), 1u) << "still selected";
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 2u) << "a click erases nothing";
    EXPECT_FALSE(App().IsItemContextMenuOpen()) << "leaving the mode is all that click did";
}

// Undo with the pen still down: what it takes back is the stroke in
// flight, and the rest of the drag draws nothing. It used to take back the
// stroke before, while the one in flight went on and was kept.
TEST_F(HeadlessAppTest, UndoMidStrokeTakesBackTheStrokeInFlight) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    Drag(350.0f, 400.0f, 650.0f, 400.0f);
    ASSERT_EQ(Canvases().CurrentOrNull()->items[0].strokes.size(), 1u);
    const Stroke first = Canvases().CurrentOrNull()->items[0].strokes[0];

    MoveTo(350.0f, 480.0f);
    StepFrame();
    RawMouse(350.0f, 480.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(500.0f, 480.0f, platform::MouseEventKind::Move);
    StepFrame();
    PressCtrlKey(ImGuiKey_Z);
    RawMouse(650.0f, 480.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(650.0f, 480.0f, platform::MouseEventKind::Up);
    StepFrames(2);

    const Item& item = Canvases().CurrentOrNull()->items[0];
    ASSERT_EQ(item.strokes.size(), 1u) << "the rest of the drag drew nothing";
    EXPECT_EQ(item.strokes[0], first) << "and the stroke before is still there";
}

// Escape with the pen still down calls the stroke off: nothing of it is
// left and nothing is filed for it, the snippet stays in drawing mode, and
// the rest of the drag draws nothing (docs/INTERACTIONS.md, section 5).
TEST_F(HeadlessAppTest, EscapeMidStrokeCallsTheStrokeOff) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    const ItemId drawing = Canvases().CurrentOrNull()->items[0].id;
    Drag(350.0f, 400.0f, 650.0f, 400.0f);
    ASSERT_EQ(StrokeCountOnCurrentCanvas(), 1u);

    MoveTo(350.0f, 480.0f);
    StepFrame();
    RawMouse(350.0f, 480.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(500.0f, 480.0f, platform::MouseEventKind::Move);
    StepFrame();
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 1u) << "nothing of it left";
    RawMouse(650.0f, 480.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(650.0f, 480.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 1u) << "and the rest of the drag drew nothing";
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(drawing)) << "still drawing";

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u) << "the undo takes the stroke before";
}

// The same for a line and for the eraser: a line called off leaves no
// line, and an erase called off puts back what it had taken.
TEST_F(HeadlessAppTest, EscapeMidShapeOrEraseLeavesTheDrawingAsItWas) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 700.0f, 600.0f);
    Drag(150.0f, 300.0f, 650.0f, 300.0f);
    const std::vector<Stroke> drawn = Canvases().CurrentOrNull()->items[0].strokes;
    ASSERT_EQ(drawn.size(), 1u);
    const auto escapeMidDrag = [&](float fromX, float fromY, float toX, float toY) {
        MoveTo(fromX, fromY);
        StepFrame();
        RawMouse(fromX, fromY, platform::MouseEventKind::Down);
        StepFrame();
        RawMouse(toX, toY, platform::MouseEventKind::Move);
        StepFrame();
        PressKey(ImGuiKey_Escape);
        RawMouse(toX, toY, platform::MouseEventKind::Up);
        StepFrames(2);
    };

    KeyEvent(ImGuiMod_Shift, true);
    StepFrame();
    escapeMidDrag(200.0f, 450.0f, 600.0f, 450.0f);
    KeyEvent(ImGuiMod_Shift, false);
    StepFrame();
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].strokes, drawn) << "no line";
    EXPECT_FALSE(AppSession().LiveLayer().ActiveStroke().has_value());

    PressKey(ImGuiKey_E);
    escapeMidDrag(150.0f, 300.0f, 650.0f, 300.0f);
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].strokes, drawn) << "the erased stroke, whole again";
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u) << "nothing filed for either: the undo takes the stroke";
}

// A rectangle erase ended from outside erases nothing: its release is what
// erases, and nothing is erased that a release did not ask for.
TEST_F(HeadlessAppTest, ARectangleEraseEndedFromOutsideErasesNothing) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 700.0f, 600.0f);
    Drag(150.0f, 150.0f, 200.0f, 200.0f);
    ASSERT_EQ(StrokeCountOnCurrentCanvas(), 1u);
    PressKey(ImGuiKey_E);

    KeyEvent(ImGuiMod_Ctrl, true);
    MoveTo(120.0f, 120.0f);
    StepFrame();
    RawMouse(120.0f, 120.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    KeyEvent(ImGuiMod_Ctrl, false);
    PressKey(ImGuiKey_E);  // the eraser put down: drawing mode ends, and the erase with it
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 1u);
}

// Drawing mode is entered with the pen, whatever tool was used last time.
TEST_F(HeadlessAppTest, DrawingModeAlwaysStartsWithThePen) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    PressKey(ImGuiKey_E);
    ASSERT_EQ(App().ActiveTool(), Tool::Erase);
    PressKey(ImGuiKey_Escape);
    ASSERT_FALSE(App().DrawingItem().has_value());

    DoubleClick(500.0f, 425.0f);
    EXPECT_TRUE(App().DrawingItem().has_value());
    EXPECT_EQ(App().ActiveTool(), Tool::Draw);
}

// ===== Drawing mode =====

// Double-clicking a snippet is the way in: it is selected, outlined for
// drawing, the pen is in hand, and a drag on it draws. A click anywhere
// else is the way out, and does nothing more.
TEST_F(HeadlessAppTest, ADoubleClickOnASnippetEntersDrawingModeAndAClickElsewhereLeavesIt) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 700.0f, 600.0f);  // a screenshot
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId shot = Canvases().CurrentOrNull()->items[0].id;
    ASSERT_FALSE(App().DrawingItem().has_value());

    // A drag on it moves it - not in drawing mode yet.
    Drag(300.0f, 300.0f, 320.0f, 310.0f);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u);
    EXPECT_NEAR(Canvases().CurrentOrNull()->items[0].rect.x, 120.0f, 1.0f);

    DoubleClick(400.0f, 400.0f);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(shot));
    EXPECT_EQ(App().ActiveTool(), Tool::Draw);
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{shot});
    EXPECT_TRUE(App().SelectionBarButtonCenter(ChromeButton::Pen).has_value()) << "the drawing bar";
    EXPECT_FALSE(App().SelectionBarButtonCenter(ChromeButton::Close).has_value());

    Drag(300.0f, 300.0f, 500.0f, 450.0f);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 1u) << "a drag on it draws now";
    EXPECT_NEAR(Canvases().CurrentOrNull()->items[0].rect.x, 120.0f, 1.0f) << "and moves nothing";

    // Out: a click on empty canvas, which makes nothing and clears nothing.
    RawClick(1100.0f, 100.0f);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{shot}) << "the press was for leaving, nothing more";
    EXPECT_TRUE(App().SelectionBarButtonCenter(ChromeButton::Close).has_value()) << "the item bar again";
}

// The drawing bar's buttons switch the tool and open the color chooser.
TEST_F(HeadlessAppTest, TheDrawingBarSwitchesTheToolAndOpensTheColor) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);

    const std::optional<ImVec2> eraser = App().SelectionBarButtonCenter(ChromeButton::Eraser);
    ASSERT_TRUE(eraser.has_value());
    RawClick(eraser->x, eraser->y);
    EXPECT_EQ(App().ActiveTool(), Tool::Erase);
    EXPECT_TRUE(App().DrawingItem().has_value());

    const std::optional<ImVec2> color = App().SelectionBarButtonCenter(ChromeButton::Color);
    ASSERT_TRUE(color.has_value());
    RawClick(color->x, color->y);
    StepFrames(2);
    EXPECT_TRUE(App().IsColorChooserOpen());

    // Escape closes the chooser and nothing more: still drawing, same tool.
    PressKey(ImGuiKey_Escape);
    StepFrames(2);
    EXPECT_FALSE(App().IsColorChooserOpen());
    EXPECT_EQ(App().ActiveTool(), Tool::Erase);
    EXPECT_TRUE(App().DrawingItem().has_value());
}

// The tool already in hand is cycled through its shapes by its own button:
// pen, line, rectangle; eraser, rectangle eraser. A plain drag then makes
// that shape. Changing the tool puts the shape back.
TEST_F(HeadlessAppTest, ClickingTheActiveBarToolAgainCyclesItsShape) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 700.0f, 600.0f);
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);
    ASSERT_EQ(App().PenShape(), DrawShape::Freehand);

    const auto clickBar = [this](ChromeButton button) {
        const std::optional<ImVec2> center = App().SelectionBarButtonCenter(button);
        ASSERT_TRUE(center.has_value());
        RawClick(center->x, center->y);
    };

    clickBar(ChromeButton::Pen);
    EXPECT_EQ(App().PenShape(), DrawShape::Line);
    Drag(150.0f, 150.0f, 400.0f, 300.0f);
    clickBar(ChromeButton::Pen);
    EXPECT_EQ(App().PenShape(), DrawShape::Rectangle);
    Drag(200.0f, 200.0f, 500.0f, 450.0f);
    clickBar(ChromeButton::Pen);
    EXPECT_EQ(App().PenShape(), DrawShape::Freehand);
    Drag(250.0f, 250.0f, 600.0f, 500.0f, 6);
    EXPECT_EQ(App().ActiveTool(), Tool::Draw) << "the pen throughout";

    const Item& item = Canvases().CurrentOrNull()->items[0];
    ASSERT_EQ(item.strokes.size(), 3u);
    EXPECT_EQ(item.strokes[0].points.size(), 2u) << "a line";
    EXPECT_EQ(item.strokes[1].points.size(), 5u) << "a rectangle's outline, closed";
    EXPECT_GT(item.strokes[2].points.size(), 5u) << "freehand again: the drag's path, not a shape's corners";

    clickBar(ChromeButton::Eraser);
    EXPECT_EQ(App().ActiveTool(), Tool::Erase);
    EXPECT_EQ(App().EraserShape(), DrawShape::Freehand);
    clickBar(ChromeButton::Eraser);
    EXPECT_EQ(App().EraserShape(), DrawShape::Rectangle);
    Drag(120.0f, 120.0f, 650.0f, 580.0f);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u) << "the rectangle took all three";

    clickBar(ChromeButton::Pen);
    EXPECT_EQ(App().ActiveTool(), Tool::Draw);
    EXPECT_EQ(App().PenShape(), DrawShape::Freehand) << "back to plain with the tool change";
    clickBar(ChromeButton::Eraser);
    EXPECT_EQ(App().EraserShape(), DrawShape::Freehand) << "and so is the eraser";
}

// ===== A hold stands in for a double-click, for a finger or a pen =====

TEST_F(HeadlessAppTest, AHoldOnEmptyCanvasMakesAFullscreenScreenshot) {
    ShowEditMode();
    StepFrame();
    Hold(640.0f, 400.0f);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_TRUE(item.isFullscreen);
    EXPECT_TRUE(item.hasBackground);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_FALSE(App().ArmedCreation().has_value());
}

TEST_F(HeadlessAppTest, ACtrlHoldOnEmptyCanvasMakesAFullscreenDrawing) {
    ShowEditMode();
    StepFrame();
    With(ImGuiMod_Ctrl, [&] { Hold(640.0f, 400.0f); });

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_TRUE(item.isFullscreen);
    EXPECT_FALSE(item.hasBackground);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(item.id));
}

TEST_F(HeadlessAppTest, AShortPressOrADragOnEmptyCanvasIsNotAHold) {
    ShowEditMode();
    StepFrame();
    PressFor(640.0f, 400.0f, 10);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "a click, which makes nothing";

    // Moved and then held: a drag, which frames what it dragged.
    MoveTo(300.0f, 300.0f);
    StepFrame();
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(350.0f, 340.0f, platform::MouseEventKind::Move);
    StepFrames(35);
    RawMouse(500.0f, 450.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(500.0f, 450.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& item = Canvases().CurrentOrNull()->items[0];
    EXPECT_FALSE(item.isFullscreen);
    EXPECT_NEAR(item.rect.x, 300.0f, 1.0f);
    EXPECT_NEAR(item.rect.w, 200.0f, 1.0f);
}

TEST_F(HeadlessAppTest, AHoldOnASnippetEntersDrawingModeAndMovesNothing) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 700.0f, 600.0f);  // a screenshot
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId shot = Canvases().CurrentOrNull()->items[0].id;

    // A short press selects; a drag moves; neither is a hold.
    PressFor(400.0f, 400.0f, 10);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{shot});
    StepFrames(30);  // long enough that the next press is not a double-click's second
    MoveTo(400.0f, 400.0f);
    StepFrame();
    RawMouse(400.0f, 400.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(420.0f, 410.0f, platform::MouseEventKind::Move);
    StepFrames(35);
    RawMouse(420.0f, 410.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FALSE(App().DrawingItem().has_value()) << "a drag, held at its end, is still a drag";
    EXPECT_NEAR(Canvases().CurrentOrNull()->items[0].rect.x, 120.0f, 1.0f);

    Hold(400.0f, 400.0f);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(shot));
    EXPECT_EQ(App().ActiveTool(), Tool::Draw);
    EXPECT_NEAR(Canvases().CurrentOrNull()->items[0].rect.x, 120.0f, 1.0f) << "the release moved nothing";
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u) << "and drew nothing";

    Drag(300.0f, 300.0f, 500.0f, 450.0f);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 1u) << "a drag on it draws now";
}

// A key pressed during a hold is what the hand wants instead: Escape
// calls the press off - the selecting it did at once stays, and the next
// Escape is the one that clears it - and a tool key's tool stays in hand.
TEST_F(HeadlessAppTest, EscapePressedDuringAHoldCallsItOff) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 700.0f, 600.0f);  // a screenshot
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId shot = Canvases().CurrentOrNull()->items[0].id;
    StepFrames(30);

    MoveTo(400.0f, 400.0f);
    StepFrame();
    RawMouse(400.0f, 400.0f, platform::MouseEventKind::Down);
    StepFrame();
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{shot});
    PressKey(ImGuiKey_Escape);
    StepFrames(35);
    EXPECT_FALSE(App().DrawingItem().has_value()) << "Escape, and not drawing mode after it";
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{shot}) << "the press, called off; its selecting stays";
    RawMouse(400.0f, 400.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    PressKey(ImGuiKey_Escape);
    EXPECT_TRUE(App().Selection().empty());
}

TEST_F(HeadlessAppTest, AToolKeyPressedDuringAHoldKeepsItsTool) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 700.0f, 600.0f);  // a screenshot
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    StepFrames(30);

    MoveTo(400.0f, 400.0f);
    StepFrame();
    RawMouse(400.0f, 400.0f, platform::MouseEventKind::Down);
    StepFrame();
    ASSERT_FALSE(App().DrawingItem().has_value());
    PressKey(ImGuiKey_E);
    ASSERT_EQ(App().ActiveTool(), Tool::Erase);
    StepFrames(35);
    EXPECT_EQ(App().ActiveTool(), Tool::Erase) << "not the pen the hold would have picked";
    RawMouse(400.0f, 400.0f, platform::MouseEventKind::Up);
    StepFrames(2);
}

// A hold on another snippet moves drawing mode there, and one on empty
// canvas makes a fullscreen screenshot - the press leaves the mode and the
// hold then does what a double-click's second press would have.
TEST_F(HeadlessAppTest, AHoldWhileInDrawingModeIsWhatADoubleClickThereWouldBe) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);  // a screenshot
    Drag(600.0f, 400.0f, 900.0f, 600.0f);  // another
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    const ItemId first = Canvases().CurrentOrNull()->items[0].id;
    const ItemId second = Canvases().CurrentOrNull()->items[1].id;

    Hold(200.0f, 200.0f);
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(first));
    Hold(750.0f, 500.0f);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(second));
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{second});

    Hold(1100.0f, 100.0f);
    EXPECT_FALSE(App().DrawingItem().has_value());
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 3u);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[2].isFullscreen);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[2].hasBackground);
}

// What a touch screen sends for a held finger on Windows, as measured: a
// left press, 650 ms later a right press and release with the left still
// down, then the left release. The right click is ignored while the left
// is down, so the hold's work stands: drawing mode stays entered, and the
// fullscreen snippet a hold on empty canvas made is left as it is.
TEST_F(HeadlessAppTest, TheOtherButtonIsIgnoredWhileOneIsDown) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 700.0f, 600.0f);  // a screenshot
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId shot = Canvases().CurrentOrNull()->items[0].id;

    const auto heldTouchAt = [this](float x, float y) {
        MoveTo(x, y);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Down);
        StepFrames(35);
        RawMouse(x, y, platform::MouseEventKind::Down, platform::MouseButton::Right);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Up, platform::MouseButton::Right);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Up);
        StepFrames(2);
    };

    heldTouchAt(400.0f, 400.0f);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(shot)) << "the hold's work stands";

    heldTouchAt(1100.0f, 100.0f);
    EXPECT_FALSE(App().DrawingItem().has_value());
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[1].isFullscreen);

    // And the other way round: a left click during a right press is not a
    // click at all, and the right press ends as it would have.
    Drag(300.0f, 300.0f, 320.0f, 310.0f, 4, platform::MouseButton::Right);  // resizes the fullscreen snippet
    MoveTo(600.0f, 600.0f);
    StepFrame();
    RawMouse(600.0f, 600.0f, platform::MouseEventKind::Down, platform::MouseButton::Right);
    StepFrame();
    RawMouse(600.0f, 600.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(600.0f, 600.0f, platform::MouseEventKind::Up);
    StepFrame();
    RawMouse(600.0f, 600.0f, platform::MouseEventKind::Up, platform::MouseButton::Right);
    StepFrames(2);
    EXPECT_EQ(App().Selection().size(), 1u) << "the right click, on a snippet, selected it as ever";
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u);
}

// Escape leaves drawing mode first, and only then clears the selection.
// Delete does nothing to the snippet being drawn on.
TEST_F(HeadlessAppTest, EscapeLeavesDrawingModeBeforeClearingTheSelection) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_TRUE(App().DrawingItem().has_value());

    PressKey(ImGuiKey_Delete);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "not deleted from inside";

    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(App().Selection().size(), 1u);
    PressKey(ImGuiKey_Escape);
    EXPECT_TRUE(App().Selection().empty());
}

// A double-click on another snippet moves drawing mode to it.
TEST_F(HeadlessAppTest, ADoubleClickOnAnotherSnippetMovesDrawingModeThere) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);  // a screenshot
    Drag(600.0f, 400.0f, 900.0f, 600.0f);  // another
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    const ItemId first = Canvases().CurrentOrNull()->items[0].id;
    const ItemId second = Canvases().CurrentOrNull()->items[1].id;

    DoubleClick(200.0f, 200.0f);
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(first));
    DoubleClick(750.0f, 500.0f);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(second));
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{second});
}

// The first create action on an empty library has to mint a canvas to put
// the item on - the empty state is self-healing rather than a dead end.
TEST_F(HeadlessAppTest, CreatingAnItemWithNoCanvasMakesOne) {
    StartWithEmptyLibrary();
    ASSERT_FALSE(Canvases().HasCurrentCanvas());
    ShowEditMode();
    StepFrame();
    PressKey(ImGuiKey_D);
    Drag(300.0f, 300.0f, 500.0f, 400.0f);

    EXPECT_TRUE(Canvases().HasCurrentCanvas());
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// The HUD names what is in front and where it sits relative to us, which
// is the line that answers "nothing in this panel is moving". It has to
// cope with there being nothing to name: over a hidden or just-started
// desktop the foreground is unknown on both counts.
TEST_F(HeadlessAppTest, TheHudDrawsWithNothingIdentifiableInFront) {
    AppConfig config = DefaultConfig();
    config.showInputOptionsHud = true;
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{};
    StartWith(std::move(config));

    ShowEditMode();
    StepFrames(3);

    EXPECT_FALSE(AppSession().Manager().Canvases().empty());
    EXPECT_EQ(AppSettings().UnderlyingApplication().integrity, platform::ForegroundIntegrity::Unknown);
}

// An elevated application is named too, and is the case the line exists
// for: every number beside it is a readout of input that never arrived.
TEST_F(HeadlessAppTest, TheHudDrawsOverAnApplicationAboveUs) {
    AppConfig config = DefaultConfig();
    config.showInputOptionsHud = true;
    platform::ForegroundApp elevated;
    elevated.executable = "taskmgr.exe";
    elevated.title = "Task Manager";
    elevated.integrity = platform::ForegroundIntegrity::Above;
    host_.overlayWindow.underlyingApp = elevated;
    StartWith(std::move(config));

    ShowEditMode();
    StepFrames(3);

    EXPECT_EQ(AppSettings().UnderlyingApplication().integrity, platform::ForegroundIntegrity::Above);
    EXPECT_FALSE(host_.overlayWindow.editModeNoActivate) << "and focus was taken from it";
}

// A setting saved while the overlay is up over an elevated application
// leaves the focus taken from it: the settings change re-derives the same
// answer the way up did, rather than going back to the setting alone.
TEST_F(HeadlessAppTest, SavingASettingOverAnElevatedApplicationKeepsItsFocusTaken) {
    platform::ForegroundApp elevated;
    elevated.executable = "taskmgr.exe";
    elevated.integrity = platform::ForegroundIntegrity::Above;
    host_.overlayWindow.underlyingApp = elevated;
    ShowEditMode();
    StepFrame();
    ASSERT_FALSE(host_.overlayWindow.editModeNoActivate);
    const int noActivateCalls = host_.overlayWindow.setEditModeNoActivateCallCount;

    controller_->GetSettings().Set(setting::kShowItemBorders, !AppSettings().Stored().showItemBorders);
    StepFrame();  // the window hears of it after the frame

    EXPECT_FALSE(host_.overlayWindow.editModeNoActivate);
    EXPECT_EQ(host_.overlayWindow.setEditModeNoActivateCallCount, noActivateCalls) << "nothing to tell it";
}

// The HUD's number keys write into the profile that is running, and the two
// rows that need edit mode re-entered then restart the overlay. A restart
// that re-asked what was underneath - in the middle of hiding itself, when
// the answer can be "nothing identifiable" - would stop the profile
// matching, read the value from the defaults again, and make the option
// look as if it had switched itself back a moment after being pressed.
TEST_F(HeadlessAppTest, AHudToggleSurvivesTheRestartItAsksFor) {
    AppConfig config = DefaultConfig();
    config.showInputOptionsHud = true;
    Profile profile;
    profile.name = "Game";
    profile.match.executables.push_back("game.exe");
    config.profiles.push_back(profile);
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "Game"};
    host_.overlayWindow.forgetUnderlyingAppOnHide = true;
    StartWith(std::move(config));

    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(AppSettings().ActiveProfile().has_value());
    const bool before = AppSettings().Live().dontStealFocus;

    // Row 1 is "Don't steal focus" - one of the two that restart.
    PressKey(ImGuiKey_1);
    StepFrames(4);  // the restart waits for the key to be up, then runs

    // What the key asked for, still - and still the profile's, since the
    // restart is the same showing rather than a new one.
    EXPECT_NE(AppSettings().Live().dontStealFocus, before);
    ASSERT_TRUE(AppSettings().ActiveProfile().has_value());
    ASSERT_TRUE(AppSettings().Profiles()[0].overrides.dontStealFocus.has_value());
    EXPECT_EQ(*AppSettings().Profiles()[0].overrides.dontStealFocus, !before);
    // The defaults were not the ones written to.
    EXPECT_EQ(AppSettings().Base().dontStealFocus, before);
}

TEST_F(HeadlessAppTest, ShortcutsAreIgnoredInViewOnlyMode) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 600.0f, 500.0f);  // something a key could act on
    ShowViewMode();
    StepFrame();
    ASSERT_EQ(App().ActiveTool(), Tool::Select) << "view-only puts drawing mode down";
    PressKey(ImGuiKey_E);
    // View-only renders a read-only canvas and takes no input at all - the
    // frame callback returns before any of the edit-mode handling.
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
}

// ===== Nothing from before a showing counts as input to it =====

// The keyboard grab hands the overlay every keystroke while edit mode is
// up, the letter of the hotkey that ends edit mode included - and that one
// arrives after the last frame. Hidden, the app draws no frame at all, so
// ImGui's queue holds the press until the next showing. Bound to the
// screenshot tool, "S" came back armed to capture on the first click.
// Put away and brought back, the overlay is as it was left - drawing mode,
// a panel - with nothing in the hand; view-only mode ends everything above
// the canvas (docs/INTERACTIONS.md, section 4.2).
TEST_F(HeadlessAppTest, PutAwayKeepsWhatIsOpenAndViewOnlyEndsIt) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    Drag(350.0f, 400.0f, 650.0f, 400.0f);  // something in it, so it stays
    ASSERT_TRUE(App().DrawingItem().has_value());
    ASSERT_TRUE(controller_->Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrame();
    ASSERT_EQ(App().InputStack(), "Canvas / DrawingMode / CheatSheet / - / - / -");

    ShowEditMode();  // its own hotkey again: put away
    StepFrame();
    ASSERT_FALSE(host_.overlayWindow.visible);
    ShowEditMode();
    StepFrame();
    EXPECT_EQ(App().InputStack(), "Canvas / DrawingMode / CheatSheet / - / - / -") << "as it was left";

    ShowViewMode();
    StepFrame();
    EXPECT_EQ(App().InputStack(), "Canvas / - / - / - / - / -");
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_FALSE(App().IsCheatSheetOpen());
}

TEST_F(HeadlessAppTest, AKeyStrandedByHidingIsNotAPressOnTheWayBack) {
    ShowEditMode();
    StepFrame();
    ASSERT_EQ(App().ActiveTool(), Tool::Select);

    ShowEditMode();  // the same hotkey again puts the overlay away
    ASSERT_FALSE(host_.overlayWindow.visible);
    // No frame in between, which is the whole point: hidden, there are none.
    KeyEvent(ImGuiKey_S, true);
    KeyEvent(ImGuiKey_S, false);

    ShowEditMode();
    StepFrames(2);
    EXPECT_EQ(App().ActiveTool(), Tool::Select) << "that S belonged to the showing it ended";
}

// The same staleness the other way round: a modifier let go of while the
// overlay was away is heard of by nobody - ImGui still believes it held,
// and so would the input stream, had the window not said otherwise on the
// way back. Latched, it would make the exact-modifier test in
// Editor::CommandForKey refuse an ordinary key press.
TEST_F(HeadlessAppTest, AModifierHeldWhenTheOverlayWentAwayDoesNotOutliveIt) {
    ShowEditMode();
    KeyEvent(ImGuiMod_Ctrl, true);
    StepFrame();  // the frame that records it as held
    ASSERT_TRUE(ImGui::GetIO().KeyCtrl);

    ShowEditMode();                  // away, with Ctrl down
    KeyEvent(ImGuiMod_Ctrl, false);  // let go of while hidden
    ShowEditMode();                  // and back
    StepFrame();

    PressKey(ImGuiKey_S);
    EXPECT_EQ(App().ActiveTool(), Tool::NewScreenshot) << "a bare S, not Ctrl+S";
}

// The mouse the same: a button held as the overlay went away has its
// release go to whatever is underneath, and ImGui must not bring it back
// still down.
TEST_F(HeadlessAppTest, AButtonHeldWhenTheOverlayWentAwayIsNotHeldOnTheWayBack) {
    ShowEditMode();
    ImGui::GetIO().AddMousePosEvent(200.0f, 200.0f);
    MouseButtonEvent(ImGuiMouseButton_Left, true);
    StepFrame();
    ASSERT_TRUE(ImGui::IsMouseDown(ImGuiMouseButton_Left));

    ShowEditMode();  // away, with the button still down as far as ImGui knows
    ShowEditMode();  // and back
    StepFrame();
    EXPECT_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
}

// Long enough for a message to have faded: it lasts 2.2 seconds, and a
// frame here is a sixtieth of one.
constexpr int kFramesPastAToast = 200;

// The color is one setting, not a slot among favorites: the overlay comes
// up with the hand at rest, and draws in whatever color was last chosen.
TEST_F(HeadlessAppTest, StartupHoldsTheConfiguredColor) {
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_EQ(App().DrawColorRGBA(), AppSettings().Stored().strokeColorRGBA);
}

// A canvas or folder nobody has named is called for the moment it was
// made - "2026-09-07 22:36:14" - so the shape is what a test can assert
// without freezing a clock.
bool LooksLikeATimestampName(const std::string& name) {
    static constexpr const char* kShape = "0000-00-00 00:00:00";
    if (name.size() != std::char_traits<char>::length(kShape)) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const bool digitExpected = kShape[i] == '0';
        const bool isDigit = name[i] >= '0' && name[i] <= '9';
        if (digitExpected ? !isDigit : name[i] != kShape[i]) {
            return false;
        }
    }
    return true;
}

// Every capture gets a canvas of its own, at the end of its folder - a
// screen of identically-sized shots stacked on one canvas is unreadable,
// one per canvas is a row of tiles.
TEST_F(HeadlessAppTest, ACaptureMakesItsOwnCanvasAtTheEndOfTheFolder) {
    const CanvasId before = Canvases().CurrentOrNull()->id;
    const FolderId folder = Canvases().CurrentOrNull()->folderId;
    ASSERT_EQ(Canvases().Canvases().size(), 1u);

    TriggerHotkey(config_.hotkeySilentCapture);

    ASSERT_EQ(Canvases().Canvases().size(), 2u);
    const Canvas& made = Canvases().Canvases().back();
    EXPECT_NE(made.id, before);
    EXPECT_EQ(made.folderId, folder);
    EXPECT_TRUE(LooksLikeATimestampName(made.name)) << "named for when it was made, but was: " << made.name;
    EXPECT_EQ(made.items.size(), 1u);
    EXPECT_EQ(Canvases().CurrentOrNull()->id, made.id) << "and it is the canvas you are now on";
    EXPECT_TRUE(Canvases().Canvases().front().items.empty()) << "the canvas being worked on is left alone";

    TriggerHotkey(config_.hotkeySilentCapture);
    ASSERT_EQ(Canvases().Canvases().size(), 3u) << "a second capture gets its own canvas too";
    EXPECT_EQ(Canvases().Canvases().back().items.size(), 1u);
}

// "Current folder" is two things that are deliberately allowed to differ:
// the folder being browsed in the Overview, and the folder the canvas
// being worked on lives in. A capture belongs to the second - browsing
// elsewhere and never opening anything must not send the shot there.
TEST_F(HeadlessAppTest, ACaptureLandsInTheFolderOfTheCanvasBeingWorkedOn) {
    CanvasManagerSnapshot snapshot;
    Folder working;
    working.id = 1;
    working.name = "Working";
    Folder browsed;
    browsed.id = 2;
    browsed.name = "Browsed";
    snapshot.folders = {working, browsed};
    Canvas onlyCanvas;
    onlyCanvas.id = 10;
    onlyCanvas.name = "A1";
    onlyCanvas.folderId = working.id;
    snapshot.canvases = {onlyCanvas};
    snapshot.currentCanvasId = onlyCanvas.id;
    snapshot.currentFolderId = browsed.id;  // browsing the other one, still editing A1
    controller_->GetSession().ImportLibrary(snapshot);
    ASSERT_EQ(Canvases().CurrentFolderId(), browsed.id);

    TriggerHotkey(config_.hotkeySilentCapture);

    ASSERT_EQ(Canvases().Canvases().size(), 2u);
    EXPECT_EQ(Canvases().Canvases().back().folderId, working.id);
    EXPECT_EQ(Canvases().CurrentFolderId(), working.id) << "and the Overview follows the canvas we moved to";
}

// The silent capture: the shot lands, and the only thing that reaches the
// screen is the message saying so - on a window that takes no clicks and
// takes itself away again.
TEST_F(HeadlessAppTest, ASilentCaptureShowsAMessageWithoutOpeningTheOverlay) {
    ASSERT_FALSE(host_.overlayWindow.visible);
    const size_t before = ItemCountOnCurrentCanvas();

    TriggerHotkey(config_.hotkeySilentCapture);

    EXPECT_EQ(ItemCountOnCurrentCanvas(), before + 1);
    EXPECT_TRUE(host_.overlayWindow.visible);
    EXPECT_TRUE(host_.overlayWindow.inputPassthrough) << "a notice must let every click through";
    EXPECT_EQ(host_.overlayWindow.showClickThroughCallCount, 1)
        << "and must not take focus from whatever the user is typing in";
    EXPECT_EQ(host_.overlayWindow.showCallCount, 1) << "which is the *only* way it may be shown";
    EXPECT_TRUE(App().IsNoticeOnly());
    EXPECT_EQ(App().ActionToastText(), "Captured screenshot");

    StepFrames(kFramesPastAToast);

    EXPECT_FALSE(host_.overlayWindow.visible) << "the notice takes itself away when the message fades";
    EXPECT_FALSE(App().IsNoticeOnly());
}

// With messages-while-hidden off, the same hotkey leaves no trace at all -
// including later, when the overlay is next opened. That last part is the
// point of dismissing rather than letting it expire: the clock a message
// expires on only runs while frames do.
TEST_F(HeadlessAppTest, ASilentCaptureCanBeCompletelySilent) {
    AppConfig config = DefaultConfig();
    config.showToastsWhileHidden = false;
    StartWith(config);
    const size_t before = ItemCountOnCurrentCanvas();

    TriggerHotkey(config_.hotkeySilentCapture);

    EXPECT_EQ(ItemCountOnCurrentCanvas(), before + 1) << "the capture itself still happens";
    EXPECT_FALSE(host_.overlayWindow.visible);
    EXPECT_TRUE(App().ActionToastText().empty());

    ShowEditMode();
    StepFrames(3);
    EXPECT_TRUE(App().ActionToastText().empty()) << "and nothing surfaces when the overlay next comes up";
}

// The overlay already being up is the case the setting has no say over: a
// message costs a frame that was being drawn anyway. View-only especially,
// where there is otherwise nothing at all to show that the hotkey worked.
TEST_F(HeadlessAppTest, ASilentCaptureSaysSoInViewOnlyModeEvenWithMessagesOff) {
    AppConfig config = DefaultConfig();
    config.showToastsWhileHidden = false;
    StartWith(config);
    ShowViewMode();
    StepFrame();
    ASSERT_TRUE(App().IsViewOnly());
    const int hidesBefore = host_.overlayWindow.hideCallCount;

    TriggerHotkey(config_.hotkeySilentCapture);
    StepFrames(3);

    EXPECT_EQ(App().ActionToastText(), "Captured screenshot");
    EXPECT_TRUE(host_.overlayWindow.visible);
    EXPECT_TRUE(App().IsViewOnly());
    EXPECT_FALSE(App().IsNoticeOnly()) << "a session the user opened is not a notice";

    StepFrames(kFramesPastAToast);
    EXPECT_TRUE(host_.overlayWindow.visible) << "and the fading message must not close it";
    EXPECT_EQ(host_.overlayWindow.hideCallCount, hidesBefore);
}

// Opening the overlay while a notice happens to be up: the notice is over,
// but what the user asked for stays - the message fading a moment later
// must not take the overlay with it.
TEST_F(HeadlessAppTest, OpeningTheOverlayDuringANoticeKeepsItOpen) {
    TriggerHotkey(config_.hotkeySilentCapture);
    ASSERT_TRUE(App().IsNoticeOnly());

    ShowEditMode();
    StepFrames(kFramesPastAToast);

    EXPECT_TRUE(host_.overlayWindow.visible);
    EXPECT_FALSE(App().IsViewOnly());
    EXPECT_FALSE(App().IsNoticeOnly());
}

// The view hotkey during a notice asks for view mode, and gets it. It used
// to be indistinguishable from "view mode is already up, so hide" - which
// would have hidden the overlay because a window the user never asked for
// was on screen.
TEST_F(HeadlessAppTest, TheViewHotkeyDuringANoticeEntersViewMode) {
    TriggerHotkey(config_.hotkeySilentCapture);
    ASSERT_TRUE(App().IsNoticeOnly());

    ShowViewMode();
    StepFrames(kFramesPastAToast);

    EXPECT_TRUE(host_.overlayWindow.visible);
    EXPECT_TRUE(App().IsViewOnly());
    EXPECT_FALSE(App().IsNoticeOnly());
}

// Two items, the second overlapping the first, laid out so the back one's
// east border and its south-east corner run under the front one's body -
// the arrangement every "I grabbed the wrong window's edge" report was
// about, back when a snippet had an invisible grab margin around it.
struct OverlappingItems {
    Rect back;
    Rect front;
};
class OverlappingItemsTest : public HeadlessAppTest {
protected:
    // Two drawings; the second is in drawing mode afterwards, as a new
    // drawing is, unless a test leaves it.
    OverlappingItems MakeOverlappingItems() {
        PressKey(ImGuiKey_D);
        Drag(200.0f, 200.0f, 600.0f, 500.0f);
        PressKey(ImGuiKey_D);
        Drag(480.0f, 380.0f, 900.0f, 700.0f);
        StepFrames(2);
        const Canvas* canvas = Canvases().CurrentOrNull();
        backId_ = canvas->items[0].id;
        frontId_ = canvas->items[1].id;
        return OverlappingItems{canvas->items[0].rect, canvas->items[1].rect};
    }

    // The two by id rather than by index: selecting a snippet brings it to
    // the front by default, so "the back item" is not items[0] for long.
    const Item& BackItem() const { return ItemById(backId_); }
    const Item& FrontItem() const { return ItemById(frontId_); }
    const Item& ItemById(ItemId id) const {
        for (const Item& item : Canvases().CurrentOrNull()->items) {
            if (item.id == id) {
                return item;
            }
        }
        ADD_FAILURE() << "the snippet went missing";
        return Canvases().CurrentOrNull()->items.front();
    }
    ItemId backId_ = 0;
    ItemId frontId_ = 0;

    // Drawing mode left, and the back item selected by a click on the part
    // of it the front item is nowhere near.
    void SelectTheBackItem(const OverlappingItems& items) {
        PressKey(ImGuiKey_Escape);
        ASSERT_EQ(App().ActiveTool(), Tool::Select);
        ASSERT_FALSE(App().DrawingItem().has_value());
        RawClick(items.back.x + 60.0f, items.back.y + 100.0f);
        ASSERT_EQ(App().Selection().size(), 1u);
    }

    // A click with a modifier held from before the press until after the
    // release - ImGuiMod_Shift, ImGuiMod_Alt.
    void RawClickWith(ImGuiKey modifier, float x, float y) {
        KeyEvent(modifier, true);
        StepFrame();
        RawClick(x, y);
        KeyEvent(modifier, false);
        StepFrame();
    }
};

// A snippet's border is a line, not a grab margin: a press just outside
// the back item's east border, over the front item's body - which is in
// drawing mode, being the drawing just made - is a press on the front
// item's body, and draws into it. An invisible handle margin around every
// snippet, hit-tested by ImGui a frame late, would start a resize of the
// back item here even where the front item covers it.
// Handles exist only on a selected snippet, and nothing over the
// canvas asks ImGui who is under the pointer.
TEST_F(OverlappingItemsTest, APressBesideABorderDrawsRatherThanResizing) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    ASSERT_EQ(Canvases().CurrentOrNull()->items.size(), 2u);
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(frontId_));
    const size_t strokesBefore = StrokeCountOnCurrentCanvas();

    // Just outside the back item's east border, and well inside the front
    // item's body.
    const float x = items.back.x + items.back.w + 2.0f;
    const float y = items.front.y + 30.0f;
    RawMouse(x, y, platform::MouseEventKind::Down);
    StepFrame();
    for (int i = 1; i <= 10; ++i) {
        RawMouse(x - static_cast<float>(i) * 6.0f, y - static_cast<float>(i) * 6.0f, platform::MouseEventKind::Move);
        StepFrame();
    }
    RawMouse(x - 60.0f, y - 60.0f, platform::MouseEventKind::Up);
    StepFrames(2);

    const Rect after = BackItem().rect;
    EXPECT_FLOAT_EQ(after.w, items.back.w);
    EXPECT_FLOAT_EQ(after.h, items.back.h);
    EXPECT_FLOAT_EQ(after.x, items.back.x);
    EXPECT_FLOAT_EQ(after.y, items.back.y);
    EXPECT_GT(StrokeCountOnCurrentCanvas(), strokesBefore) << "the press should have drawn into the front item";
}

// The same one-frame lag, seen from the other side. ImGui decides
// io.WantCaptureMouse in NewFrame from the hovered window, and while a
// chrome window sat under the pointer that was a frame late at every pixel
// where two items overlapped: the pen blinked back to an arrow for that
// frame, which is what "the pointer flickers" was. With no chrome window
// left for ImGui to hover, the answer is the resolver's, every frame.
TEST_F(OverlappingItemsTest, ThePointerKeepsItsShapeCrossingAnOccludedBorder) {
    AppConfig config = DefaultConfig();
    // The app only asks the OS for a shape when it isn't drawing the
    // pointer itself, which is what makes the answer observable here.
    config.profileable.softwarePointer = false;
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(frontId_)) << "the pen is for the front item";

    // Over the front item's body throughout, crossing the back item's east
    // border on the way. One frame per pixel, which is what a moving
    // pointer gets.
    const float y = items.front.y + 30.0f;
    MoveTo(items.back.x + items.back.w - 12.0f, y);
    StepFrames(3);
    ASSERT_EQ(host_.overlayWindow.cursorShape, platform::CursorShape::Pen);
    for (float x = items.back.x + items.back.w - 11.0f; x < items.back.x + items.back.w + 12.0f; x += 1.0f) {
        MoveTo(x, y);
        StepFrame();
        EXPECT_EQ(host_.overlayWindow.cursorShape, platform::CursorShape::Pen)
            << "at x=" << x << ", which is " << (x - items.back.x - items.back.w) << "px past the border";
    }
}

// Leaving a resize handle gives the pen back.
//
// ApplyPointerShape only pushes a shape when the answer has moved, rather
// than every frame, and getting "moved" wrong shows up exactly here: ImGui's
// backend installs its own cursor from NewFrame by comparing against the
// shape it installed last, and it reads ImGui's answer *before* NewFrame
// resets it - so the install lands one frame after ImGui's cursor changed.
// A change check that only looked at the current frame's ImGui cursor
// therefore missed the frame the backend actually took the cursor away, and
// the arrow stayed over the canvas until something else happened to move.
// Caught by driving the real app, not by reading the code, so it is pinned
// here.
//
// The snippet in drawing mode keeps its handles, so the pen and a handle's
// arrow trade places over it without leaving the mode.
//
// What this covers is a *moving* pointer, which is the case that actually
// broke. ApplyPointerShape also re-asserts when the pointer is standing still
// and ImGui lets go of the cursor on its own - a real case (the app wants a
// tool cursor at a point ImGui has just stopped claiming) but one with no
// deterministic way to stage here, since every way of making ImGui release
// the cursor in this harness also moves the pointer. That half is argued in
// ApplyPointerShape's own comment rather than tested.
TEST_F(OverlappingItemsTest, ThePenComesBackAfterHoveringAResizeHandle) {
    AppConfig config = DefaultConfig();
    config.profileable.softwarePointer = false;  // see the test above
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();

    // Well inside the back item, in drawing mode, where the tool owns the
    // pointer.
    const float bodyY = items.back.y + 100.0f;
    const float bodyX = items.back.x + 60.0f;
    DoubleClick(bodyX, bodyY);
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(backId_));
    MoveTo(bodyX, bodyY);
    StepFrames(3);
    ASSERT_EQ(host_.overlayWindow.cursorShape, platform::CursorShape::Pen);

    // Its west handle - which the front item is nowhere near - asks ImGui
    // for a sizing cursor, so the app stands back and asks for Default.
    MoveTo(items.back.x, items.back.y + items.back.h * 0.5f);
    StepFrames(3);
    ASSERT_EQ(host_.overlayWindow.cursorShape, platform::CursorShape::Default)
        << "the handle should have handed the cursor to ImGui";

    // Back onto the body. Several frames, because the frame that has to be
    // got right is the one *after* ImGui lets go.
    MoveTo(bodyX, bodyY);
    StepFrames(5);
    EXPECT_EQ(host_.overlayWindow.cursorShape, platform::CursorShape::Pen)
        << "the pen never came back after the handle released the cursor";
}

// The other half of the same change: a pointer that is not moving over a
// picture that is not changing asks the OS for nothing at all. That call is
// not free - it costs a WindowFromPoint, a system-wide hit test, measured
// at ~57us of the ~82us an idle overlay's frame otherwise spends here.
TEST_F(OverlappingItemsTest, AStillPointerStopsAskingTheOsForACursor) {
    AppConfig config = DefaultConfig();
    config.profileable.softwarePointer = false;
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(frontId_));

    MoveTo(items.front.x + 60.0f, items.front.y + 60.0f);
    StepFrames(5);
    ASSERT_EQ(host_.overlayWindow.cursorShape, platform::CursorShape::Pen);

    const int callsBefore = host_.overlayWindow.setCursorShapeCallCount;
    StepFrames(30);
    EXPECT_EQ(host_.overlayWindow.setCursorShapeCallCount, callsBefore)
        << "thirty idle frames should have asked the OS for a cursor no times";
    EXPECT_EQ(host_.overlayWindow.cursorShape, platform::CursorShape::Pen)
        << "and the shape should still be the one it settled on";
}

// ===== The selection =====

TEST_F(OverlappingItemsTest, AClickSelectsAndShiftClickAddsAndRemoves) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    const ItemId backId = backId_;
    const ItemId frontId = frontId_;
    SelectTheBackItem(items);
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{backId});

    // On the front item, where the back item isn't.
    const float frontX = items.front.x + items.front.w - 30.0f;
    const float frontY = items.front.y + items.front.h - 30.0f;
    ASSERT_EQ(Canvases().CurrentOrNull()->items.back().id, backId) << "selecting it raised it";
    RawClickWith(ImGuiMod_Shift, frontX, frontY);
    EXPECT_EQ(App().Selection(), (std::vector<ItemId>{backId, frontId}));
    EXPECT_EQ(Canvases().CurrentOrNull()->items.back().id, backId) << "a Shift-click restacks nothing";
    RawClickWith(ImGuiMod_Shift, frontX, frontY);
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{backId});

    // A plain click on the other one replaces the selection.
    RawClick(frontX, frontY);
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{frontId});
}

// Empty canvas is what you click to select nothing: a click there makes
// no snippet (a drag or a double-click does).
TEST_F(OverlappingItemsTest, AClickOnEmptyCanvasClearsTheSelectionAndMakesNothing) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    RawClick(1100.0f, 100.0f);
    EXPECT_TRUE(App().Selection().empty());
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "nothing was made";
}

// Shift held, a drag from open canvas draws a box instead of framing a
// snippet, and everything the box touches is selected - touched, not
// enclosed, so a box drawn across a row catches the whole row.
TEST_F(OverlappingItemsTest, AShiftDragOverOpenCanvasSelectsWhatTheBoxTouches) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    // Escape in stages: out of drawing mode, then the selection it left.
    PressKey(ImGuiKey_Escape);
    PressKey(ImGuiKey_Escape);
    ASSERT_TRUE(App().Selection().empty());

    // From open canvas above the front snippet, back across the corner
    // of both.
    DragWith(ImGuiMod_Shift, 1000.0f, 100.0f, 500.0f, 450.0f);

    EXPECT_EQ(App().Selection(), (std::vector<ItemId>{backId_, frontId_}));
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "the box framed no snippet of its own";
}

// Shift is already the key that adds one snippet to the selection, so a
// box drawn with it held adds what it caught rather than replacing what
// was there.
TEST_F(OverlappingItemsTest, ABoxAddsToTheSelectionAndAShiftClickCatchesNothing) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{backId_});

    // A box over the front snippet's far corner only.
    DragWith(ImGuiMod_Shift, 1000.0f, 760.0f, items.front.x + items.front.w - 20.0f, items.front.y + items.front.h - 20.0f);
    EXPECT_EQ(App().Selection(), (std::vector<ItemId>{backId_, frontId_}));

    // A Shift-press on open canvas that never travels is a click, and a
    // click there with Shift held catches nothing and clears nothing.
    RawClickWith(ImGuiMod_Shift, 1100.0f, 120.0f);
    EXPECT_EQ(App().Selection(), (std::vector<ItemId>{backId_, frontId_}));
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u);
}

// Dragging a selected snippet moves it with the pointer. Selecting it
// brought it to the front, as a window manager raises a window you take
// hold of - the default (AppConfig::raiseSelectedSnippet).
TEST_F(OverlappingItemsTest, DraggingASelectedSnippetMovesItAndSelectingRaisedIt) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    const ItemId backId = backId_;
    SelectTheBackItem(items);
    ASSERT_EQ(Canvases().CurrentOrNull()->items.back().id, backId) << "selecting it raised it";

    const float x = items.back.x + 40.0f;
    const float y = items.back.y + 40.0f;
    Drag(x, y, x + 50.0f, y + 20.0f);

    const Canvas* canvas = Canvases().CurrentOrNull();
    ASSERT_EQ(canvas->items.back().id, backId);
    EXPECT_FLOAT_EQ(canvas->items.back().rect.x, items.back.x + 50.0f);
    EXPECT_FLOAT_EQ(canvas->items.back().rect.y, items.back.y + 20.0f);
    EXPECT_FLOAT_EQ(canvas->items.back().rect.w, items.back.w);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u);
}

// Taking hold of a multi-selection raises all of it, as a block and in
// its own order - not the one snippet under the pointer out of the group.
TEST_F(OverlappingItemsTest, DraggingAMultiSelectionRaisesItAsABlockInItsOwnOrder) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    const ItemId backId = backId_;
    const ItemId frontId = frontId_;
    SelectTheBackItem(items);
    RawClickWith(ImGuiMod_Shift, items.front.x + items.front.w - 30.0f, items.front.y + items.front.h - 30.0f);
    ASSERT_EQ(App().Selection().size(), 2u);
    // Something else made since, on top of both.
    const ItemId otherId = test::Model(controller_->GetSession()).CreateItem(false, Rect{1200.0f, 100.0f, 200.0f, 200.0f}, "Other");
    StepFrame();
    const auto order = [this] {
        std::vector<ItemId> ids;
        for (const Item& item : Canvases().CurrentOrNull()->items) {
            ids.push_back(item.id);
        }
        return ids;
    };
    // Selecting the back one raised it; the Shift-click that added the
    // other raised nothing.
    ASSERT_EQ(order(), (std::vector<ItemId>{frontId, backId, otherId}));

    // Taken hold of by the one in front of the two.
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    Drag(x, y, x + 30.0f, y + 10.0f);

    EXPECT_EQ(order(), (std::vector<ItemId>{otherId, frontId, backId}));
    EXPECT_FLOAT_EQ(ItemById(backId).rect.x, items.back.x + 30.0f);
    EXPECT_FLOAT_EQ(ItemById(frontId).rect.x, items.front.x + 30.0f) << "the group moved together";
}

// A drag of the selection is one undo for all of it, and a click that
// never moved anything is none.
TEST_F(OverlappingItemsTest, DraggingTheSelectionIsUndoneAndRedoneInOneStep) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    const ItemId backId = backId_;
    const ItemId frontId = frontId_;
    SelectTheBackItem(items);
    RawClickWith(ImGuiMod_Shift, items.front.x + items.front.w - 30.0f, items.front.y + items.front.h - 30.0f);
    ASSERT_EQ(App().Selection().size(), 2u);

    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    RawClick(x, y);  // a press and release that moves nothing
    StepFrames(60);  // not the first half of a double-click, which would draw
    Drag(x, y, x + 40.0f, y + 25.0f);
    ASSERT_FLOAT_EQ(ItemById(backId).rect.x, items.back.x + 40.0f);
    ASSERT_FLOAT_EQ(ItemById(frontId).rect.x, items.front.x + 40.0f);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(ItemById(backId).rect.x, items.back.x);
    EXPECT_FLOAT_EQ(ItemById(backId).rect.y, items.back.y);
    EXPECT_FLOAT_EQ(ItemById(frontId).rect.x, items.front.x) << "both, in the one step";
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "the click before it filed nothing to take a snippet back with";

    PressCtrlKey(ImGuiKey_Y);
    EXPECT_FLOAT_EQ(ItemById(backId).rect.x, items.back.x + 40.0f);
    EXPECT_FLOAT_EQ(ItemById(frontId).rect.y, items.front.y + 25.0f);
}

// A spin of the wheel is one undo, back to the size it began at.
TEST_F(OverlappingItemsTest, ASpinOfTheWheelIsUndoneInOneStep) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    const ItemId backId = backId_;
    SelectTheBackItem(items);
    MoveTo(items.back.x + 60.0f, items.back.y + 100.0f);
    StepFrame();
    Wheel(1.0f);
    Wheel(1.0f);
    Wheel(1.0f);
    ASSERT_GT(ItemById(backId).rect.w, items.back.w * 1.3f);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(ItemById(backId).rect.w, items.back.w);
    EXPECT_FLOAT_EQ(ItemById(backId).rect.x, items.back.x);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "one undo, not four";
}

// With the setting off, selecting leaves the stacking order alone - a
// drawing program's selection.
TEST_F(OverlappingItemsTest, WithRaisingOffSelectingLeavesTheOrderAlone) {
    AppConfig config = DefaultConfig();
    config.raiseSelectedSnippet = false;
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    const ItemId backId = backId_;
    SelectTheBackItem(items);

    const float x = items.back.x + 40.0f;
    const float y = items.back.y + 40.0f;
    Drag(x, y, x + 50.0f, y + 20.0f);

    const Canvas* canvas = Canvases().CurrentOrNull();
    ASSERT_EQ(canvas->items.front().id, backId) << "still at the back";
    EXPECT_FLOAT_EQ(canvas->items.front().rect.x, items.back.x + 50.0f);
}

// A snippet that isn't selected yet is selected by the press that drags
// it, so a drag from cold moves it too.
TEST_F(OverlappingItemsTest, ADragOnAnUnselectedSnippetSelectsAndMovesIt) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    const ItemId frontId = frontId_;
    PressKey(ImGuiKey_Escape);  // out of drawing mode on the front item
    PressKey(ImGuiKey_Escape);  // and nothing selected

    const float x = items.front.x + items.front.w - 30.0f;
    const float y = items.front.y + items.front.h - 30.0f;
    Drag(x, y, x + 30.0f, y + 30.0f);

    EXPECT_EQ(App().Selection(), std::vector<ItemId>{frontId});
    EXPECT_FLOAT_EQ(FrontItem().rect.x, items.front.x + 30.0f);
}

// A selected snippet's handles sit on its corners and the middles of its
// edges, and the debug readout says which one is found.
TEST_F(OverlappingItemsTest, HandlesSitOnTheCornersAndTheMiddlesOfTheEdges) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    MoveTo(items.back.x, items.back.y);
    StepFrames(2);
    EXPECT_EQ(App().DebugHoveredResizeHandle().substr(0, 2), "nw");

    MoveTo(items.back.x + items.back.w * 0.5f, items.back.y);
    StepFrames(2);
    EXPECT_EQ(App().DebugHoveredResizeHandle().substr(0, 2), "n ");

    // Between the two: the border, which is nothing to grab.
    MoveTo(items.back.x + 40.0f, items.back.y);
    StepFrames(2);
    EXPECT_TRUE(App().DebugHoveredResizeHandle().empty());
}

TEST_F(OverlappingItemsTest, ASelectedSnippetsHandleResizesIt) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    // The west handle, in the middle of the border the front item is
    // nowhere near, dragged 40px further out.
    const float x = items.back.x;
    const float y = items.back.y + items.back.h * 0.5f;
    Drag(x, y, x - 40.0f, y, /*steps=*/10);

    const Rect after = BackItem().rect;
    EXPECT_NEAR(after.w, items.back.w + 40.0f, 1.0f);
    EXPECT_NEAR(after.x, items.back.x - 40.0f, 1.0f);
    EXPECT_NEAR(after.h, items.back.h * after.w / items.back.w, 1.0f) << "a drawing keeps its shape";
}

// A right-drag resizes a snippet from whichever edge or corner is nearest
// the press, without aiming for a handle - with any tool in hand, here the
// pen in drawing mode on the other snippet. A right press that never
// drags is a right click, and resizes nothing.
TEST_F(OverlappingItemsTest, ARightDragResizesFromTheNearestEdgeAndARightClickResizesNothing) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);

    // In the back item's right third, above where the front item starts.
    const float x = items.back.x + items.back.w - 20.0f;
    const float y = items.back.y + 40.0f;
    Drag(x, y, x + 40.0f, y, 10, platform::MouseButton::Right);

    const Rect after = BackItem().rect;
    // The press was in the top *and* right thirds, so this is a corner:
    // the width follows the drag projected onto the snippet's diagonal,
    // which is less than the 40px moved - the amount is ItemGeometryTest's
    // to pin down, what matters here is which edges moved.
    EXPECT_GT(after.w, items.back.w + 1.0f) << "the right edge followed the drag";
    EXPECT_FLOAT_EQ(after.x, items.back.x) << "and the left edge stayed put";
    EXPECT_NEAR(after.h / after.w, items.back.h / items.back.w, 0.01f) << "keeping its shape";
    EXPECT_EQ(App().Selection().size(), 1u) << "resizing it selects it";
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u);

    RightClick(items.back.x + 60.0f, items.back.y + 100.0f);
    EXPECT_EQ(App().Selection().size(), 1u) << "a right click, with no drag, only selects";
    EXPECT_FLOAT_EQ(BackItem().rect.w, after.w) << "and resized nothing";
}

// A canvas switch in the middle of a right-drag resize ends it on the
// canvas it began on, as it ends a left-button gesture: the rest of the
// drag resizes nothing, and the resize is one undo there. Only the left
// button's gestures were ended, and the resize went on across the switch.
TEST_F(OverlappingItemsTest, ACanvasSwitchEndsARightDragResizeWhereItIs) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    CanvasManager& manager = test::Model(controller_->GetSession());
    const CanvasId home = manager.CurrentCanvasId();
    const ItemId back = backId_;
    const float x = items.back.x + items.back.w - 20.0f;
    const float y = items.back.y + 40.0f;

    MoveTo(x, y);
    StepFrame();
    RawMouse(x, y, platform::MouseEventKind::Down, platform::MouseButton::Right);
    StepFrame();
    RawMouse(x + 30.0f, y, platform::MouseEventKind::Move, platform::MouseButton::Right);
    StepFrame();
    const Rect mid = manager.FindItemAnywhere(back)->rect;
    ASSERT_GT(mid.w, items.back.w);

    TriggerHotkey(config_.hotkeyQuickCapture);  // onto a canvas of its own
    ASSERT_NE(manager.CurrentCanvasId(), home);
    RawMouse(x + 90.0f, y, platform::MouseEventKind::Move, platform::MouseButton::Right);
    StepFrame();
    RawMouse(x + 90.0f, y, platform::MouseEventKind::Up, platform::MouseButton::Right);
    StepFrames(2);
    EXPECT_EQ(manager.FindItemAnywhere(back)->rect, mid) << "the rest of the drag resized nothing";

    manager.SwitchToCanvas(home);
    StepFrame();
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(manager.FindItemAnywhere(back)->rect, items.back) << "one undo, on the canvas it began on";
}

// A fullscreen snippet right-clicked stays fullscreen; right-dragged, it
// leaves fullscreen and resizes from there.
TEST_F(HeadlessAppTest, ARightClickOnAFullscreenSnippetLeavesItFullscreen) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    ASSERT_TRUE(Canvases().CurrentOrNull()->items[0].isFullscreen);

    RightClick(640.0f, 400.0f);
    EXPECT_TRUE(Canvases().CurrentOrNull()->items[0].isFullscreen);

    // The click opened the snippet's context menu, and while a popup is up
    // ImGui claims the mouse (io.WantCaptureMouse) - so the next press
    // dismisses the menu and does nothing else, exactly as a context menu
    // behaves anywhere. Escape is the same dismissal without spending a
    // press on it.
    PressKey(ImGuiKey_Escape);
    ASSERT_FALSE(App().IsItemContextMenuOpen());

    Drag(1200.0f, 400.0f, 1100.0f, 400.0f, 10, platform::MouseButton::Right);
    EXPECT_FALSE(Canvases().CurrentOrNull()->items[0].isFullscreen);
}

// The selection's handles are drawn over every snippet, and are grabbable
// there too: the back item's south-east corner lies under the front item's
// body, and still resizes the back item rather than drawing into the front.
TEST_F(OverlappingItemsTest, ASelectedSnippetsHandleIsOnTopOfEveryOtherSnippet) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float cornerX = items.back.x + items.back.w;
    const float cornerY = items.back.y + items.back.h;
    ASSERT_GT(cornerX, items.front.x);
    ASSERT_GT(cornerY, items.front.y);

    Drag(cornerX, cornerY, cornerX + 40.0f, cornerY + 40.0f, /*steps=*/10);

    const Rect after = BackItem().rect;
    EXPECT_GT(after.w, items.back.w + 30.0f);
    EXPECT_GT(after.h, items.back.h + 30.0f);
    EXPECT_NEAR(after.w / after.h, items.back.w / items.back.h, 0.01f) << "a drawing keeps its shape";
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u) << "nothing was drawn into the front item";
}

// With more than one snippet selected, a handle drags the box around all
// of them: every snippet is scaled by one factor about the corner the drag
// leaves fixed, so the group keeps its shape and the spacing inside it.
TEST_F(OverlappingItemsTest, AHandleResizesTheWholeSelectionAtOnce) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    RawClickWith(ImGuiMod_Shift, items.front.x + items.front.w - 30.0f, items.front.y + items.front.h - 30.0f);
    ASSERT_EQ(App().Selection().size(), 2u);
    // The box around the two, and the corner opposite the one dragged.
    const float boxRight = std::max(items.back.x + items.back.w, items.front.x + items.front.w);
    const float boxBottom = std::max(items.back.y + items.back.h, items.front.y + items.front.h);

    // The back snippet's own north-west handle, out by a tenth of the box.
    Drag(items.back.x, items.back.y, items.back.x - 70.0f, items.back.y - 50.0f, /*steps=*/10);

    const Rect back = BackItem().rect;
    const Rect front = FrontItem().rect;
    EXPECT_NEAR(back.w / items.back.w, 1.1f, 0.02f);
    EXPECT_NEAR(front.w / items.front.w, 1.1f, 0.02f) << "the other snippet is scaled by the same factor";
    EXPECT_NEAR(front.h / items.front.h, 1.1f, 0.02f);
    EXPECT_NEAR(front.x - back.x, (items.front.x - items.back.x) * 1.1f, 2.0f) << "and the space between them with it";
    EXPECT_NEAR(front.x + front.w, boxRight, 2.0f) << "the corner opposite the drag stays where it was";
    EXPECT_NEAR(front.y + front.h, boxBottom, 2.0f);
}

// The smallest snippet is the floor for the whole group: it stops, and
// everything stops with it, rather than flattening against the minimum
// while the rest carry on shrinking.
TEST_F(OverlappingItemsTest, TheSmallestSnippetStopsTheWholeGroupShrinking) {
    ShowEditMode();
    StepFrame();
    PressKey(ImGuiKey_D);
    Drag(200.0f, 200.0f, 600.0f, 500.0f);
    PressKey(ImGuiKey_D);
    Drag(700.0f, 200.0f, 810.0f, 290.0f);
    PressKey(ImGuiKey_Escape);
    const Canvas* canvas = Canvases().CurrentOrNull();
    ASSERT_EQ(canvas->items.size(), 2u);
    const ItemId bigId = canvas->items[0].id;
    const ItemId smallId = canvas->items[1].id;
    const Rect big = canvas->items[0].rect;
    const Rect small = canvas->items[1].rect;
    RawClick(big.x + 40.0f, big.y + 40.0f);
    RawClickWith(ImGuiMod_Shift, small.x + 20.0f, small.y + 20.0f);
    ASSERT_EQ(App().Selection().size(), 2u);

    // Far enough in to take the small one well below its floor on its own.
    Drag(big.x, big.y, big.x + 300.0f, big.y + 200.0f, /*steps=*/10);

    const Rect smallNow = ItemById(smallId).rect;
    const Rect bigNow = ItemById(bigId).rect;
    EXPECT_GE(smallNow.w, kItemMinWidth - 0.5f);
    EXPECT_GE(smallNow.h, kItemMinHeight - 0.5f);
    EXPECT_NEAR(smallNow.w, kItemMinWidth, 1.0f) << "it stops exactly at its floor";
    EXPECT_NEAR(bigNow.w / big.w, smallNow.w / small.w, 0.02f) << "and the big one stops at the same factor";

    // And it goes no further: the same drag again changes nothing.
    Drag(bigNow.x, bigNow.y, bigNow.x + 300.0f, bigNow.y + 200.0f, /*steps=*/10);
    EXPECT_NEAR(ItemById(smallId).rect.w, smallNow.w, 1.0f);
    EXPECT_NEAR(ItemById(bigId).rect.w, bigNow.w, 1.0f);
}

// A screenshot is a picture and keeps its shape when resized; a drawing is
// a box and doesn't. Shift flips whichever the default is.
TEST_F(HeadlessAppTest, AScreenshotKeepsItsShapeWhenResizedAndShiftFreesIt) {
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 500.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    ASSERT_TRUE(Canvases().CurrentOrNull()->items[0].hasBackground);
    const Rect before = Canvases().CurrentOrNull()->items[0].rect;
    RawClick(300.0f, 200.0f);
    ASSERT_EQ(App().Selection().size(), 1u);

    // The east handle, dragged 100px out: the height follows.
    Drag(before.x + before.w, before.y + before.h * 0.5f, before.x + before.w + 100.0f, before.y + before.h * 0.5f, 10);
    const Rect locked = Canvases().CurrentOrNull()->items[0].rect;
    EXPECT_NEAR(locked.w, before.w + 100.0f, 1.0f);
    EXPECT_NEAR(locked.h, locked.w * before.h / before.w, 1.0f);

    // With Shift, only the width changes.
    DragWith(ImGuiMod_Shift, locked.x + locked.w, locked.y + locked.h * 0.5f, locked.x + locked.w + 50.0f,
             locked.y + locked.h * 0.5f);
    const Rect freed = Canvases().CurrentOrNull()->items[0].rect;
    EXPECT_NEAR(freed.w, locked.w + 50.0f, 1.0f);
    EXPECT_FLOAT_EQ(freed.h, locked.h);
}

TEST_F(OverlappingItemsTest, DeleteRemovesTheSelectionAndUndoBringsItBack) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    PressKey(ImGuiKey_Delete);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_TRUE(App().Selection().empty());

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u);
}

// Delete with the button still down, halfway through a drag: the drag
// stops there. It used to go on moving the hidden snippet and file that
// move after the delete, so the first undo did nothing to be seen and the
// snippet came back wherever the hand let go.
TEST_F(OverlappingItemsTest, DeletingMidDragStopsTheDragWhereItWas) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click

    RawMouse(x, y, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(x, y + 100.0f, platform::MouseEventKind::Move);
    StepFrame();
    PressKey(ImGuiKey_Delete);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    RawMouse(x, y + 250.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(x, y + 250.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 100.0f) << "not moved on after the delete";
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "and the rest of the drag made nothing";

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "the first undo brings it back";
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 100.0f);
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y) << "the second takes the drag back";
}

// Undo with a press on a snippet held still: the press is over, and so is
// the hold it might have become - it no longer enters drawing mode once
// the hold's time is up.
TEST_F(OverlappingItemsTest, UndoDuringAHeldPressCancelsTheHold) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click

    RawMouse(x, y, platform::MouseEventKind::Down);
    StepFrame();
    PressCtrlKey(ImGuiKey_Z);
    StepFrames(35);  // well past kHoldSeconds, still held
    RawMouse(x, y, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FALSE(App().DrawingItem().has_value());
}

// Undo with the button still down: what it takes back is the drag so far,
// and the rest of the drag leaves that alone. It used to undo the step
// before and let the drag write over it, losing that step unseen.
TEST_F(OverlappingItemsTest, UndoMidDragTakesBackTheDragSoFar) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click
    Drag(x, y, x, y + 50.0f);
    ASSERT_FLOAT_EQ(BackItem().rect.y, items.back.y + 50.0f);
    StepFrames(30);

    RawMouse(x, y + 50.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(x, y + 150.0f, platform::MouseEventKind::Move);
    StepFrame();
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 50.0f);
    RawMouse(x, y + 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(x, y + 300.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 50.0f) << "the rest of the drag moved nothing";

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y) << "the first move is still there to undo";
}

// Escape with the button still down calls the drag off: the snippet is
// back where the press found it, nothing is filed for it, and the rest of
// the drag moves nothing.
TEST_F(OverlappingItemsTest, EscapeMidDragPutsTheSnippetBack) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click
    Drag(x, y, x, y + 50.0f);
    ASSERT_FLOAT_EQ(BackItem().rect.y, items.back.y + 50.0f);
    StepFrames(30);

    RawMouse(x, y + 50.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(x, y + 150.0f, platform::MouseEventKind::Move);
    StepFrame();
    ASSERT_FLOAT_EQ(BackItem().rect.y, items.back.y + 150.0f);
    PressKey(ImGuiKey_Escape);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 50.0f) << "back where the press found it";
    RawMouse(x, y + 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(x, y + 300.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 50.0f) << "the rest of the drag moved nothing";

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y) << "the first move is the one to undo";
}

// A press on a snippet selects it at once and moves it only once it is a
// drag: until then nothing is open on the session (docs/INTERACTIONS.md,
// 6.2).
TEST_F(OverlappingItemsTest, APressOnASnippetBeginsNoMoveUntilItDrags) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    PressKey(ImGuiKey_Escape);  // out of drawing mode, where a press elsewhere is for leaving it
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);
    MoveTo(x, y);
    StepFrame();
    RawMouse(x, y, platform::MouseEventKind::Down);
    StepFrame();
    EXPECT_EQ(App().Selection().size(), 1u) << "selected at once";
    EXPECT_FALSE(HandGestureOpen(controller_->GetSession())) << "nothing to move yet";
    RawMouse(x, y + 2.0f, platform::MouseEventKind::Move);
    StepFrame();
    EXPECT_FALSE(HandGestureOpen(controller_->GetSession())) << "still a click";
    RawMouse(x, y + 40.0f, platform::MouseEventKind::Move);
    StepFrame();
    EXPECT_TRUE(HandGestureOpen(controller_->GetSession())) << "a drag now";
    RawMouse(x, y + 40.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 40.0f) << "moved the whole way from the press";
}

// A key pressed between two moves of a drag acts between them, however
// quickly they come - with no frame in between, as a fast hand manages.
// Keys were read at the frame, after every move that came before it, and
// the drag had gone on to the second move by the time the key ended it.
TEST_F(OverlappingItemsTest, AKeyBetweenTwoMovesOfADragActsBetweenThem) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click

    RawMouse(x, y, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(x, y + 100.0f, platform::MouseEventKind::Move);
    KeyEvent(ImGuiKey_RightArrow, true);  // ends the drag where it is, and nudges
    RawMouse(x, y + 250.0f, platform::MouseEventKind::Move);
    KeyEvent(ImGuiKey_RightArrow, false);
    StepFrame();
    RawMouse(x, y + 250.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 100.0f) << "the rest of the drag moved nothing";
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 1.0f);
}

TEST_F(OverlappingItemsTest, TheArrowKeysNudgeTheSelection) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    PressKey(ImGuiKey_RightArrow);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 1.0f);

    KeyEvent(ImGuiMod_Shift, true);
    PressKey(ImGuiKey_DownArrow);
    KeyEvent(ImGuiMod_Shift, false);
    StepFrame();
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 10.0f);
}

// A burst of nudges is one undo, but only while nothing comes between
// them: a nudge right after an undo, or after a drag of the same snippet,
// is a step of its own - not folded into the older entry left on top.
TEST_F(OverlappingItemsTest, ANudgeAfterAnUndoOrADragIsItsOwnStep) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click
    Drag(x, y, x + 30.0f, y);
    ASSERT_FLOAT_EQ(BackItem().rect.x, items.back.x + 30.0f);

    PressKey(ImGuiKey_RightArrow);
    PressCtrlKey(ImGuiKey_Z);
    ASSERT_FLOAT_EQ(BackItem().rect.x, items.back.x + 30.0f);
    PressKey(ImGuiKey_RightArrow);
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 30.0f) << "the nudge after the undo, not the drag with it";

    PressKey(ImGuiKey_RightArrow);
    StepFrames(30);  // still well inside a burst's second
    Drag(x + 31.0f, y, x + 61.0f, y);
    PressKey(ImGuiKey_RightArrow);
    ASSERT_FLOAT_EQ(BackItem().rect.x, items.back.x + 62.0f);
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 61.0f) << "the nudge after the drag, not the drag with it";
}

// A nudge against the screen's edge moves nothing and files nothing - and
// starts no run: the nudge after it is a step of its own, not folded into
// the drag before it.
TEST_F(OverlappingItemsTest, ANudgeThatMovesNothingStartsNoRun) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click
    Drag(x, y, x - 3000.0f, y);  // as far left as a snippet goes
    const float edge = BackItem().rect.x;
    PressKey(ImGuiKey_LeftArrow);
    ASSERT_FLOAT_EQ(BackItem().rect.x, edge) << "nowhere to go";
    PressKey(ImGuiKey_RightArrow);
    ASSERT_FLOAT_EQ(BackItem().rect.x, edge + 1.0f);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, edge) << "the nudge, not the drag with it";
}

// Mid-drag the wheel leaves the snippet to the drag: it is on the mouse
// holding the drag, and what it filed would be undone to a place the drag
// had since left.
TEST_F(OverlappingItemsTest, TheWheelWaitsForADragToEnd) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);  // past the double-click window: a press, not a second click

    RawMouse(x, y, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(x + 20.0f, y, platform::MouseEventKind::Move);
    StepFrame();
    Wheel(1.0f);
    RawMouse(x + 40.0f, y, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(x + 40.0f, y, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.w, items.back.w);
    ASSERT_FLOAT_EQ(BackItem().rect.x, items.back.x + 40.0f);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x) << "the drag, whole, in one step";
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x) << "and no step under it to a place mid-drag";
}

// An arrow key is a command, and a command ends the drag where it is (see
// Editor::Settle): the nudge is a step of its own after the drag,
// and the rest of the drag moves nothing.
TEST_F(OverlappingItemsTest, AnArrowKeyEndsADragAndNudgesAfterIt) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;
    StepFrames(30);

    RawMouse(x, y, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(x + 20.0f, y, platform::MouseEventKind::Move);
    StepFrame();
    PressKey(ImGuiKey_DownArrow);
    EXPECT_EQ(App().InputStack(), "Canvas / - / - / - / - / NudgeBurst") << "the drag is over";
    RawMouse(x + 40.0f, y, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(x + 40.0f, y, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 20.0f) << "the drag ended where the key found it";
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y + 1.0f);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y) << "the nudge first";
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 20.0f);
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x) << "then the drag";
}

// Escape works in stages: a selection clears before the tool goes down.
TEST_F(OverlappingItemsTest, EscapeClearsTheSelectionBeforePuttingTheToolDown) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    PressKey(ImGuiKey_E);  // the eraser, for the selected back item
    ASSERT_EQ(App().ActiveTool(), Tool::Erase);
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(backId_));

    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(App().Selection().size(), 1u);
    EXPECT_EQ(App().ActiveTool(), Tool::Select);

    PressKey(ImGuiKey_Escape);
    EXPECT_TRUE(App().Selection().empty());
}

// Alt is the way to pick the snippet up without leaving drawing mode: an
// Alt-drag on it moves it and draws nothing, and the drag after Alt is let
// go draws again.
TEST_F(OverlappingItemsTest, AltMovesTheSnippetBeingDrawnOn) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    DoubleClick(items.back.x + 60.0f, items.back.y + 100.0f);
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(backId_));
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);
    const float x = items.back.x + 60.0f;
    const float y = items.back.y + 100.0f;

    DragWith(ImGuiMod_Alt, x, y, x + 30.0f, y + 10.0f);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 30.0f);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 0u);
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(backId_)) << "still drawing on it";

    Drag(x + 30.0f, y + 10.0f, x + 80.0f, y + 40.0f);
    EXPECT_GT(StrokeCountOnCurrentCanvas(), 0u);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 30.0f) << "not moved again";
}

// The bar changes with the mode: the item buttons at rest, the drawing
// buttons in drawing mode - and the handles stay through both.
// What the bar carries is a setting: a button switched off is not on the
// bar and takes no press, and the order the setting gives is the order the
// bar is drawn in.
TEST_F(OverlappingItemsTest, TheBarCarriesWhatTheSettingSays) {
    AppConfig config = DefaultConfig();
    config.snippetBar = {{ChromeButton::Close, true}, {ChromeButton::More, true}, {ChromeButton::Pin, false},
                          {ChromeButton::Minimize, false}, {ChromeButton::Maximize, false}};
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    EXPECT_FALSE(App().SelectionBarButtonCenter(ChromeButton::Pin).has_value()) << "switched off, so not there";
    EXPECT_FALSE(App().SelectionBarButtonCenter(ChromeButton::Minimize).has_value());
    const std::optional<ImVec2> close = App().SelectionBarButtonCenter(ChromeButton::Close);
    const std::optional<ImVec2> more = App().SelectionBarButtonCenter(ChromeButton::More);
    ASSERT_TRUE(close.has_value());
    ASSERT_TRUE(more.has_value());
    EXPECT_LT(close->x, more->x) << "drawn in the order the setting gives, Close first";

    // And it still works: the one button left that acts on the selection.
    RawClick(close->x, close->y);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// Every button switched off is a bar with nothing on it, which is drawn as
// no bar at all rather than as an empty pill.
TEST_F(OverlappingItemsTest, ABarWithEveryButtonSwitchedOffIsNotThere) {
    AppConfig config = DefaultConfig();
    for (BarButtonSetting& entry : config.snippetBar) {
        entry.shown = false;
    }
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    for (const ChromeButton button : kSnippetBarButtons) {
        EXPECT_FALSE(App().SelectionBarButtonCenter(button).has_value());
    }
    // The drawing bar is its own setting and is untouched.
    DoubleClick(items.back.x + 60.0f, items.back.y + 100.0f);
    ASSERT_TRUE(App().DrawingItem().has_value());
    EXPECT_TRUE(App().SelectionBarButtonCenter(ChromeButton::Pen).has_value());
}

TEST_F(OverlappingItemsTest, TheBarShowsTheDrawingButtonsInDrawingMode) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    EXPECT_TRUE(App().SelectionBarButtonCenter(ChromeButton::Close).has_value());
    EXPECT_FALSE(App().SelectionBarButtonCenter(ChromeButton::Pen).has_value());

    PressKey(ImGuiKey_P);  // Draw, for the selected back item
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);
    ASSERT_EQ(App().DrawingItem(), std::optional<ItemId>(backId_));
    EXPECT_EQ(App().Selection().size(), 1u);
    EXPECT_FALSE(App().SelectionBarButtonCenter(ChromeButton::Close).has_value());
    EXPECT_TRUE(App().SelectionBarButtonCenter(ChromeButton::Pen).has_value());
    MoveTo(items.back.x, items.back.y);
    StepFrames(2);
    EXPECT_EQ(App().DebugHoveredResizeHandle().substr(0, 2), "nw") << "the handles stay in drawing mode";

    PressKey(ImGuiKey_Escape);
    PressKey(ImGuiKey_Delete);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// A bar button fires on the release, and only if the release lands on the
// same button it was pressed on - ImGui's own rule for a Button, kept now
// that the buttons are drawn and take their press from the raw pipeline.
TEST_F(OverlappingItemsTest, ABarButtonFiresOnReleaseOverItself) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const std::optional<ImVec2> minimize = App().SelectionBarButtonCenter(ChromeButton::Minimize);
    ASSERT_TRUE(minimize.has_value());

    // Pressed on the button, released off it: nothing.
    RawMouse(minimize->x, minimize->y, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(minimize->x, minimize->y + 80.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(minimize->x, minimize->y + 80.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FALSE(BackItem().minimized);

    RawClick(minimize->x, minimize->y);
    EXPECT_TRUE(BackItem().minimized);
    EXPECT_TRUE(App().Selection().empty()) << "a minimized snippet is off the screen, so out of the selection";
}

// Pin is the one toggle on the bar, and a second press takes the pin back
// out.
TEST_F(OverlappingItemsTest, ThePinButtonPinsASnippetAndUnpinsIt) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);
    const auto pinned = [&] { return BackItem().pinned; };
    const std::optional<ImVec2> pin = App().SelectionBarButtonCenter(ChromeButton::Pin);
    ASSERT_TRUE(pin.has_value());

    RawClick(pin->x, pin->y);
    EXPECT_TRUE(pinned());

    RawClick(pin->x, pin->y);
    EXPECT_FALSE(pinned());
}

// The bar floats above the selection, and below it when there is no room
// above - never off the screen.
TEST_F(HeadlessAppTest, TheBarFloatsAboveTheSelectionOrBelowItWhenThereIsNoRoom) {
    ShowEditMode();
    StepFrame();
    PressKey(ImGuiKey_D);
    Drag(300.0f, 200.0f, 700.0f, 500.0f);
    PressKey(ImGuiKey_D);
    Drag(300.0f, 10.0f, 700.0f, 150.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 2u);
    PressKey(ImGuiKey_Escape);  // out of drawing mode on the second

    RawClick(500.0f, 350.0f);
    std::optional<ImVec2> close = App().SelectionBarButtonCenter(ChromeButton::Close);
    ASSERT_TRUE(close.has_value());
    EXPECT_LT(close->y, 200.0f) << "above the snippet";

    RawClick(500.0f, 80.0f);
    close = App().SelectionBarButtonCenter(ChromeButton::Close);
    ASSERT_TRUE(close.has_value());
    EXPECT_GT(close->y, 150.0f) << "below the snippet, since there is no room above";
}

// A drawing a press on empty canvas made goes again once the hand moves on
// without putting anything into it - unless it was moved or resized first:
// a box someone has placed is a box they want, empty or not.
TEST_F(HeadlessAppTest, AnEmptyDrawingThatWasMovedStays) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressKey(ImGuiKey_Escape);  // out of drawing mode, still selected
    Drag(500.0f, 425.0f, 600.0f, 475.0f);

    // The hand moves on: a screenshot taken elsewhere.
    Drag(1000.0f, 200.0f, 1400.0f, 500.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "the moved drawing stayed";
    EXPECT_EQ(App().Selection(), std::vector<ItemId>{Canvases().CurrentOrNull()->items[1].id})
        << "the selection moved on to the screenshot just made";
}

// The same for one scaled with the wheel, made fullscreen or set back to
// its size: each files a step on the drawing itself, and an undo takes
// back that step alone - not the drawing, and not the step before it.
TEST_F(HeadlessAppTest, AnEmptyDrawingThatWasScaledStaysAndTheScalingIsUndoneAlone) {
    ShowEditMode();
    StepFrame();
    Drag(1000.0f, 100.0f, 1300.0f, 300.0f);  // a screenshot: the step before
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_TRUE(App().DrawingItem().has_value());
    const ItemId id = *App().DrawingItem();
    const Rect before = test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect;
    PressKey(ImGuiKey_Escape);  // out of drawing mode, still selected
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{id});
    MoveTo(500.0f, 400.0f);
    StepFrames(2);
    Wheel(2.0f);
    ASSERT_GT(test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect.w, before.w);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "the drawing and the screenshot, both";
    ASSERT_NE(test::Model(controller_->GetSession()).FindItemAnywhere(id), nullptr);
    EXPECT_NEAR(test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect.w, before.w, 0.5f) << "the scaling taken back";
}

TEST_F(HeadlessAppTest, AnEmptyDrawingThatWasOnlySelectedStillGoes) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    PressKey(ImGuiKey_Escape);
    RawClick(500.0f, 425.0f);
    ASSERT_EQ(App().Selection().size(), 1u);

    RawClick(1100.0f, 100.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "nothing was put into it, and nothing was done with it";
    EXPECT_TRUE(App().Selection().empty());
}

// Leaving drawing mode by a click elsewhere is the hand moving on too: an
// empty drawing goes with it.
TEST_F(HeadlessAppTest, AnEmptyDrawingGoesWhenDrawingModeIsLeftByAClickElsewhere) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    ASSERT_TRUE(App().DrawingItem().has_value());

    RawClick(1100.0f, 100.0f);
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
}

// The wheel's width change is kept in the config - once the size preview
// has faded, not per notch, so a burst of notches is one write.
TEST_F(HeadlessAppTest, TheWheelsPenWidthIsKeptOnceThePreviewFades) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_EQ(App().ActiveTool(), Tool::Draw);
    const float before = AppSettings().Stored().strokeWidth;

    MoveTo(500.0f, 400.0f);
    Wheel(2.0f);
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, before) << "not yet: the preview is still up";
    StepFrames(400);
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, before + 2.0f);
}

// Out of drawing mode the wheel is the selection's size: scaled about the
// middle, shape kept - and in drawing mode it stays the pen's, and the
// snippet under the pen keeps its size.
TEST_F(HeadlessAppTest, TheWheelScalesTheSelectionOutsideDrawingModeAndSizesThePenInIt) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_TRUE(App().DrawingItem().has_value());
    const ItemId id = *App().DrawingItem();
    const Rect before = test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect;
    const float widthBefore = AppSettings().Stored().strokeWidth;

    MoveTo(500.0f, 400.0f);
    Wheel(1.0f);
    EXPECT_FLOAT_EQ(test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect.w, before.w) << "drawing mode: the pen, not the snippet";
    StepFrames(400);
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, widthBefore + 1.0f);

    PressKey(ImGuiKey_Escape);  // out of drawing mode, still selected
    ASSERT_FALSE(App().DrawingItem().has_value());
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{id});
    Wheel(2.0f);
    const Rect after = test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect;
    EXPECT_NEAR(after.w, before.w * 1.21f, 0.5f);
    EXPECT_NEAR(after.h, before.h * 1.21f, 0.5f);
    EXPECT_NEAR(after.x + after.w * 0.5f, before.x + before.w * 0.5f, 0.5f) << "about its middle";
    EXPECT_NEAR(after.y + after.h * 0.5f, before.y + before.h * 0.5f, 0.5f);
    Wheel(-2.0f);
    EXPECT_NEAR(test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect.w, before.w, 0.5f) << "and back";
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, widthBefore + 1.0f) << "the pen untouched";

    StepFrames(90);            // the spin over: Escape now would take it back
    PressKey(ImGuiKey_Escape);  // nothing selected: the wheel has nothing to do
    Wheel(1.0f);
    EXPECT_NEAR(test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect.w, before.w, 0.5f);
}

// Ctrl with the wheel is the selection's background opacity, Shift its
// foreground's, five percent a notch, within the popover's ranges.
TEST_F(HeadlessAppTest, CtrlAndShiftWithTheWheelSetTheSelectionsOpacities) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 700.0f, 550.0f);  // a region: a screenshot, with a picture
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId id = Canvases().CurrentOrNull()->items[0].id;
    RawClick(500.0f, 400.0f);
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{id});
    ASSERT_FALSE(App().DrawingItem().has_value());
    const Rect before = test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect;

    const auto wheelWith = [this](ImGuiKey modifier, float notches) {
        KeyEvent(modifier, true);
        StepFrame();
        Wheel(notches);
        KeyEvent(modifier, false);
        StepFrame();
    };
    wheelWith(ImGuiMod_Ctrl, -2.0f);
    const Item* item = test::Model(controller_->GetSession()).FindItemAnywhere(id);
    EXPECT_FLOAT_EQ(item->picture.opacity, 0.9f);
    EXPECT_FLOAT_EQ(item->foregroundOpacity, 1.0f);

    wheelWith(ImGuiMod_Shift, -40.0f);
    EXPECT_FLOAT_EQ(test::Model(controller_->GetSession()).FindItemAnywhere(id)->foregroundOpacity, 0.1f) << "not below a tenth";
    wheelWith(ImGuiMod_Ctrl, 5.0f);
    EXPECT_FLOAT_EQ(test::Model(controller_->GetSession()).FindItemAnywhere(id)->picture.opacity, 1.0f) << "not above whole";
    EXPECT_FLOAT_EQ(test::Model(controller_->GetSession()).FindItemAnywhere(id)->rect.w, before.w) << "a modified wheel does not scale";
}

// ===== Draw and Erase: the shape comes from the modifier =====

TEST_F(HeadlessAppTest, ShiftDrawsALineAndCtrlARectangle) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 700.0f, 600.0f);  // a drawing to draw in
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    DragWith(ImGuiMod_Shift, 150.0f, 150.0f, 400.0f, 300.0f);
    DragWith(ImGuiMod_Ctrl, 200.0f, 200.0f, 500.0f, 450.0f);

    const Item& item = Canvases().CurrentOrNull()->items[0];
    ASSERT_EQ(item.strokes.size(), 2u);
    EXPECT_EQ(item.strokes[0].points.size(), 2u) << "a line";
    EXPECT_EQ(item.strokes[1].points.size(), 5u) << "a rectangle's outline, closed";
    EXPECT_EQ(App().ActiveTool(), Tool::Draw) << "no tool of its own for either";
}

TEST_F(HeadlessAppTest, CtrlWithEraseTakesOutARectangle) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(100.0f, 100.0f, 700.0f, 600.0f);
    Drag(150.0f, 150.0f, 200.0f, 200.0f);  // a stroke to erase
    Drag(550.0f, 450.0f, 650.0f, 550.0f);  // and one to leave
    ASSERT_EQ(StrokeCountOnCurrentCanvas(), 2u);

    PressKey(ImGuiKey_E);
    DragWith(ImGuiMod_Ctrl, 120.0f, 120.0f, 300.0f, 300.0f);

    EXPECT_EQ(StrokeCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(App().ActiveTool(), Tool::Erase);
}

// ===== Text: typed into a drawing, with the Text tool in drawing mode =====

AppConfig WithTextOnT() {
    AppConfig config = DefaultConfig();
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Text)] =
        platform::KeyCombo{/*ctrl=*/false, /*alt=*/false, /*shift=*/false, /*key=*/'T'};
    return config;
}

TEST_F(HeadlessAppTest, WithTextInHandAClickOnTheDrawingOpensItForTyping) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 500.0f);
    PressKey(ImGuiKey_T);
    ASSERT_EQ(App().ActiveTool(), Tool::Text);

    RawClick(400.0f, 400.0f);

    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const Item& note = Canvases().CurrentOrNull()->items[0];
    EXPECT_EQ(App().EditingNote(), std::optional<ItemId>(note.id)) << "open for typing";
}

// While a note is being typed into, every key is the note's - a tool's key
// types a letter rather than picking the tool, Delete deletes no snippet -
// and Escape ends the typing, keeping what was typed (docs/INTERACTIONS.md,
// decision 1); Undo then takes it back.
TEST_F(HeadlessAppTest, WhileANoteIsTypedEveryKeyIsTheNotesAndEscapeKeepsTheText) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 500.0f);
    PressKey(ImGuiKey_T);
    RawClick(400.0f, 400.0f);
    const ItemId note = Canvases().CurrentOrNull()->items[0].id;
    ASSERT_EQ(App().EditingNote(), std::optional<ItemId>(note));
    EXPECT_EQ(App().InputStack(), "Canvas / DrawingMode / - / - / TypingNote / -");

    ImGui::GetIO().AddInputCharacter('p');
    PressKey(ImGuiKey_P);
    EXPECT_EQ(App().ActiveTool(), Tool::Text) << "no tool picked under the field";
    PressKey(ImGuiKey_Delete);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "and no snippet deleted either";

    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().EditingNote().has_value());
    EXPECT_EQ(Canvases().FindItemAnywhere(note)->noteText, "p") << "Escape keeps what was typed";
    EXPECT_EQ(App().DrawingItem(), std::optional<ItemId>(note)) << "and ends only the typing";
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(Canvases().FindItemAnywhere(note)->noteText, "");
}

// The key for Text pressed halfway through a stroke: a command, so the
// stroke ends where the key found it and is kept (see
// Editor::Settle), and the rest of the drag draws nothing. Taken
// by Text instead, it stayed in flight, and nothing could be pressed after
// it.
TEST_F(HeadlessAppTest, TextPickedMidStrokeEndsTheStrokeWhereItIs) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 500.0f);
    CanvasManager& manager = test::Model(controller_->GetSession());
    const ItemId drawing = manager.CurrentOrNull()->items[0].id;

    MouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(350.0f, 350.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(450.0f, 400.0f, platform::MouseEventKind::Move);
    StepFrame();
    PressKey(ImGuiKey_T);
    ASSERT_EQ(App().ActiveTool(), Tool::Text);
    ASSERT_EQ(manager.FindItemAnywhere(drawing)->strokes.size(), 1u) << "kept as the key found it";
    const size_t points = manager.FindItemAnywhere(drawing)->strokes[0].points.size();
    EXPECT_FALSE(AppSession().LiveLayer().ActiveStroke().has_value());
    RawMouse(500.0f, 420.0f, platform::MouseEventKind::Move);
    RawMouse(500.0f, 420.0f, platform::MouseEventKind::Up);
    MouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrame();
    ASSERT_EQ(manager.FindItemAnywhere(drawing)->strokes.size(), 1u);
    EXPECT_EQ(manager.FindItemAnywhere(drawing)->strokes[0].points.size(), points) << "the rest drew nothing";

    RawClick(400.0f, 400.0f);
    EXPECT_EQ(App().EditingNote(), std::optional<ItemId>(drawing)) << "the next press is Text's";
}

TEST_F(HeadlessAppTest, ANoteNothingWasTypedIntoGoesWhenTheHandMovesOn) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 500.0f);
    PressKey(ImGuiKey_T);
    RawClick(400.0f, 400.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    ASSERT_TRUE(App().EditingNote().has_value());

    // A click elsewhere: it closes the note, leaves drawing mode, the empty
    // drawing goes, and the click makes nothing new of its own.
    RawClick(900.0f, 600.0f);

    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u);
    EXPECT_FALSE(App().EditingNote().has_value());
    EXPECT_FALSE(App().DrawingItem().has_value());
    EXPECT_TRUE(Canvases().CurrentOrNull()->items.empty()) << "erased, not kept deleted";
}

// Hiding and exiting both flush, and a note being typed is the note as it
// is typed (see Session::PreviewText) - but its edit is one step only once
// it is committed, which both do on the way out.
TEST_F(HeadlessAppTest, HidingByHotkeyCommitsTheNoteBeingTyped) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 620.0f, 440.0f);
    PressKey(ImGuiKey_T);
    RawClick(400.0f, 400.0f);
    ASSERT_TRUE(App().EditingNote().has_value());
    ImGui::GetIO().AddInputCharacter('a');
    ImGui::GetIO().AddInputCharacter('b');
    StepFrames(2);
    ASSERT_EQ(Canvases().CurrentOrNull()->items[0].noteText, "ab") << "the note is what has been typed";
    ASSERT_TRUE(AppSession().TextEditItem().has_value()) << "and the edit is still open";

    ShowEditMode();  // the edit hotkey again: put away, with the editor still open
    EXPECT_FALSE(host_.overlayWindow.IsVisible());
    EXPECT_FALSE(App().EditingNote().has_value());
    EXPECT_FALSE(AppSession().TextEditItem().has_value());
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].noteText, "ab");
    EXPECT_TRUE(AppSession().CanUndo()) << "committed, as one step";
}

// Switching to view-only in place: the editor is not drawn there, so it
// could never be told it was closed - the text stayed uncommitted, and
// back in edit mode a press on empty canvas made nothing.
TEST_F(HeadlessAppTest, SwitchingToViewOnlyCommitsTheNoteBeingTyped) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 620.0f, 440.0f);
    Drag(350.0f, 320.0f, 500.0f, 340.0f);  // a stroke first: a drawing with nothing in it settles its note anyway
    PressKey(ImGuiKey_T);
    RawClick(400.0f, 400.0f);
    ASSERT_TRUE(App().EditingNote().has_value());
    ImGui::GetIO().AddInputCharacter('a');
    StepFrames(2);

    ShowViewMode();
    StepFrames(2);
    EXPECT_TRUE(App().IsViewOnly());
    EXPECT_FALSE(App().EditingNote().has_value());
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].noteText, "a");
    EXPECT_EQ(host_.overlayWindow.releaseTextInputCallCount, host_.overlayWindow.requestTextInputCallCount)
        << "the keyboard is given back";
}

// The retention period runs at startup, while nobody is looking; what it
// deleted for good is said the next time the overlay comes up.
TEST_F(HeadlessAppTest, WhatTheRetentionPeriodDeletedIsSaidOnTheNextShow) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "spickzettel_headless_purge_said";
    std::filesystem::remove_all(dir);
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "F";
    snapshot.folders.push_back(folder);
    Canvas live;
    live.id = 2;
    live.name = "Live";
    live.folderId = 1;
    snapshot.canvases.push_back(live);
    Canvas old;
    old.id = 3;
    old.name = "Old";
    old.folderId = 1;
    old.deletedAt = static_cast<int64_t>(std::time(nullptr)) - 40 * 24 * 60 * 60;
    snapshot.canvases.push_back(old);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    ASSERT_TRUE(persistence::LibraryStore(dir / "library.db").Save(snapshot));
    host_.libraryPath = dir / "library.db";

    StartWith(DefaultConfig());
    EXPECT_EQ(Canvases().FindCanvas(3), nullptr);
    ShowEditMode();
    StepFrame();
    EXPECT_NE(App().ActionToastText().find("deleted permanently"), std::string::npos) << App().ActionToastText();

    Shutdown();
    std::filesystem::remove_all(dir);
}

TEST_F(HeadlessAppTest, ExitingFromTheTrayCommitsTheNoteBeingTyped) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 620.0f, 440.0f);
    PressKey(ImGuiKey_T);
    RawClick(400.0f, 400.0f);
    ASSERT_TRUE(App().EditingNote().has_value());
    ImGui::GetIO().AddInputCharacter('x');
    StepFrames(2);

    host_.TriggerTrayCommand(platform::TrayCommand::Exit);
    EXPECT_TRUE(host_.quitCalled);
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].noteText, "x");
}

// A note is whatever its record says it is. Opening a long one for
// editing must not cut it to the size of some buffer.
TEST_F(HeadlessAppTest, EditingALongNoteKeepsAllOfIt) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 620.0f, 440.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const std::string longNote(20000, 'a');
    test::Model(controller_->GetSession()).CurrentOrNull()->items[0].noteText = longNote;

    PressKey(ImGuiKey_T);
    RawClick(400.0f, 400.0f);
    ASSERT_TRUE(App().EditingNote().has_value());
    ImGui::GetIO().AddInputCharacter('b');
    StepFrames(2);
    // A click elsewhere that ImGui sees, which deactivates the field and so
    // closes the note - the drawing has text already, so it is not the
    // stray click a raw press alone would settle.
    Click(1100.0f, 100.0f);
    ASSERT_FALSE(App().EditingNote().has_value());

    const std::string& text = Canvases().CurrentOrNull()->items[0].noteText;
    EXPECT_EQ(text.size(), longNote.size() + 1) << "nothing cut off, one character typed";
    size_t as = 0;
    size_t bs = 0;
    for (const char c : text) {
        as += c == 'a' ? 1 : 0;
        bs += c == 'b' ? 1 : 0;
    }
    EXPECT_EQ(as, longNote.size()) << "every character of the long note survived";
    EXPECT_EQ(bs, 1u) << "wherever the caret was";
}

// A snippet with text in it, widened by 50px from its right-hand handle.
// What the handle does is the snippet's own setting, and `config` is where
// the one a new drawing gets comes from.
class TextBoxResizeTest : public HeadlessAppTest {
protected:
    struct Widened {
        Rect before;
        Rect after;
    };
    Widened MakeATextBoxAndWidenIt(AppConfig config) {
        StartWith(std::move(config));
        ShowEditMode();
        StepFrame();
        MakeADrawing(300.0f, 300.0f, 620.0f, 440.0f);
        PressKey(ImGuiKey_T);
        RawClick(400.0f, 400.0f);
        EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
        EXPECT_TRUE(App().EditingNote().has_value());
        ImGui::GetIO().AddInputCharacter('a');
        StepFrames(2);
        RawClick(1100.0f, 100.0f);  // closes the note and leaves drawing mode; kept, now that it has text
        EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
        const Rect before = Canvases().CurrentOrNull()->items[0].rect;
        EXPECT_FALSE(Canvases().CurrentOrNull()->items[0].noteText.empty());

        RawClick(before.x + before.w * 0.5f, before.y + before.h * 0.5f);
        EXPECT_EQ(App().Selection().size(), 1u);
        Drag(before.x + before.w, before.y + before.h * 0.5f, before.x + before.w + 50.0f,
             before.y + before.h * 0.5f, 10);
        return {before, Canvases().CurrentOrNull()->items[0].rect};
    }
};

// Typing into a snippet does not change what its handles do: it used to
// free its shape, so the same drag stretched a drawing the moment it had
// a caption in it.
TEST_F(TextBoxResizeTest, TextLeavesASnippetKeepingItsShape) {
    const auto [before, after] = MakeATextBoxAndWidenIt(WithTextOnT());
    EXPECT_NEAR(after.w, before.w + 50.0f, 1.0f);
    EXPECT_NEAR(after.h, before.h * after.w / before.w, 1.0f) << "the height followed the width";
}

// A drawing made not to keep its shape - Settings > Defaults, or its own
// popover - resizes freely: a box whose text wraps to its width.
TEST_F(TextBoxResizeTest, ASnippetSetNotToKeepItsShapeResizesFreely) {
    AppConfig config = WithTextOnT();
    config.drawingDefaults.keepAspect = false;
    const auto [before, after] = MakeATextBoxAndWidenIt(std::move(config));
    EXPECT_NEAR(after.w, before.w + 50.0f, 1.0f);
    EXPECT_FLOAT_EQ(after.h, before.h) << "only the width changed";
}

// ===== What a save that could not finish does next =====
//
// Driven through real frames against a real store in a temp directory,
// with the file held by another program so that nothing can be written.
class HeadlessSaveTest : public HeadlessAppTest {
protected:
    void SetUp() override {
        StartWith(DefaultConfig());
        root_ = std::filesystem::temp_directory_path() /
                (std::string("spickzettel_headless_save_") +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        if (controller_) {
            controller_->GetSession().SetLibraryStore(nullptr);
        }
        HeadlessAppTest::TearDown();
        store_.reset();  // the file is open until it goes
        std::filesystem::remove_all(root_);
    }

    // One drawing on the canvas, so there is something unsaved.
    void PlaceADrawing() {
        ShowEditMode();
        StepFrame();
        PressKey(ImGuiKey_D);
        Drag(200.0f, 200.0f, 800.0f, 600.0f);
        ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    }

    // A store in the temp directory, as TrayController attaches its own -
    // opened, so that a hold taken after it stops writes rather than the
    // store opening at all - with the library as it is written into it,
    // as a first run's is.
    void AttachStore() {
        store_ = std::make_unique<persistence::LibraryStore>(Library());
        ASSERT_EQ(store_->Open(), persistence::LibraryStore::OpenResult::Opened);
        controller_->GetSession().SetLibraryStore(store_.get());
        ASSERT_TRUE(controller_->GetSession().WriteWholeLibrary());
    }
    std::filesystem::path Library() const { return root_ / "library.db"; }

    std::filesystem::path root_;
    std::unique_ptr<persistence::LibraryStore> store_;
};

// Drawing mode left with the button still held - the key of the tool in
// hand, which puts it down - halfway through a stroke on a snippet whose
// earlier strokes are written: the stroke is ended there as a release
// would end it - one undo step, and written as it ends.
TEST_F(HeadlessSaveTest, LeavingDrawingModeMidStrokeKeepsAndSavesTheStroke) {
    AttachStore();
    PlaceADrawing();
    Session& session = controller_->GetSession();
    const ItemId drawing = Canvases().CurrentOrNull()->items[0].id;
    const auto strokeCount = [&] { return session.Manager().FindItemAnywhere(drawing)->strokes.size(); };

    // A first stroke, whole, and saved.
    MouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(400.0f, 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(400.0f, 300.0f, platform::MouseEventKind::Up);
    MouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrame();
    ASSERT_EQ(strokeCount(), 1u);

    // A second, left mid-way.
    MouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(300.0f, 500.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(500.0f, 500.0f, platform::MouseEventKind::Move);
    StepFrame();
    PressKey(ImGuiKey_P);
    ASSERT_FALSE(App().DrawingItem().has_value());
    RawMouse(500.0f, 500.0f, platform::MouseEventKind::Up);
    MouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrames(2);
    ASSERT_EQ(strokeCount(), 2u);

    persistence::LibraryStore reopened(Library());
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    size_t strokesOnDisk = 0;
    for (const Item& item : loaded->canvases[0].items) {
        strokesOnDisk = item.id == drawing ? item.strokes.size() : strokesOnDisk;
    }
    EXPECT_EQ(strokesOnDisk, 2u) << "the second stroke is on disk";

    // And it is one undo step of its own, taken back whole.
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(strokeCount(), 1u);
}

// A change whose write fails is not made, and that is said on screen until
// a write lands - what is drawn is what the library holds, and the line
// says why the change is not there.
TEST_F(HeadlessSaveTest, AWriteThatFailsIsSaidOnScreenUntilOneLands) {
    PlaceADrawing();
    AttachStore();
    EXPECT_TRUE(App().PersistenceWarning().empty()) << "nothing has failed yet";
    const size_t strokes = StrokeCountOnCurrentCanvas();
    {
        test::HeldLibrary held(Library(), /*readers=*/true);
        Drag(300.0f, 300.0f, 500.0f, 400.0f);  // a stroke, in drawing mode
    }
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), strokes) << "not made";
    const std::string warning = App().PersistenceWarning();
    EXPECT_NE(warning.find(Library().string()), std::string::npos) << warning;

    Drag(300.0f, 300.0f, 500.0f, 400.0f);
    EXPECT_EQ(StrokeCountOnCurrentCanvas(), strokes + 1);
    EXPECT_TRUE(App().PersistenceWarning().empty()) << "gone with the write that landed";
}

// ===== Deleted things, on the screen =====

// Deleting a snippet marks it deleted where it is - off the screen, and
// saved with the stamp on it - and Undo brings it back.
TEST_F(HeadlessSaveTest, DeletingASnippetHidesItInPlaceAndUndoBringsItBack) {
    PlaceADrawing();
    AttachStore();
    const Item drawing = Canvases().CurrentOrNull()->items[0];

    // Selected, and the Delete key.
    PressKey(ImGuiKey_Escape);
    RawClick(drawing.rect.x + drawing.rect.w * 0.5f, drawing.rect.y + drawing.rect.h * 0.5f);
    ASSERT_EQ(App().Selection().size(), 1u);
    PressKey(ImGuiKey_Delete);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "off the screen";
    ASSERT_EQ(Canvases().CurrentOrNull()->items.size(), 1u) << "but still in the library";
    EXPECT_NE(Canvases().CurrentOrNull()->items[0].deletedAt, 0);
    {
        const std::optional<CanvasManagerSnapshot> saved = persistence::LibraryStore(Library()).Load();
        ASSERT_TRUE(saved.has_value());
        ASSERT_EQ(saved->canvases[0].items.size(), 1u) << "saved, stamp and all";
        EXPECT_NE(saved->canvases[0].items[0].deletedAt, 0);
    }

    // Undo, by key.
    PressCtrlKey(ImGuiKey_Z);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].deletedAt, 0);
}

// The most opaque any vertex of the snippets' layer was drawn last frame -
// 0 with nothing on it.
unsigned MostOpaqueSnippetVertex() {
    const ImGuiWindow* layer = ImGui::FindWindowByName("##sz_items_layer");
    unsigned most = 0;
    if (layer != nullptr) {
        for (const ImDrawVert& vertex : layer->DrawList->VtxBuffer) {
            most = std::max(most, static_cast<unsigned>((vertex.col >> IM_COL32_A_SHIFT) & 0xFFu));
        }
    }
    return most;
}

// While a snippet is being made the others fade back, so what is being
// framed shows through them: with a creation tool in hand, and while a
// region is dragged out on empty canvas - but not for a press that has not
// moved, which may be a click and would only flicker.
TEST_F(HeadlessAppTest, SnippetsFadeBackWhileANewOneIsBeingMade) {
    ShowEditMode();
    StepFrame();
    PressKey(ImGuiKey_D);
    Drag(200.0f, 200.0f, 600.0f, 500.0f);
    PressKey(ImGuiKey_Escape);  // out of drawing mode
    PressKey(ImGuiKey_Escape);  // and nothing selected
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    ASSERT_FALSE(App().ItemsFadedForCreation());
    const unsigned faint = static_cast<unsigned>(255.0f * ui::kCreationFadeAlpha) + 1u;
    ASSERT_GT(MostOpaqueSnippetVertex(), faint) << "a border at full strength, at least";

    MoveTo(1000.0f, 100.0f);
    StepFrame();
    RawMouse(1000.0f, 100.0f, platform::MouseEventKind::Down);
    StepFrame();
    EXPECT_FALSE(App().ItemsFadedForCreation()) << "not yet a drag";
    RawMouse(1150.0f, 250.0f, platform::MouseEventKind::Move);
    StepFrame();
    EXPECT_TRUE(App().ItemsFadedForCreation());
    EXPECT_LE(MostOpaqueSnippetVertex(), faint);
    RawMouse(1150.0f, 250.0f, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u) << "the region was made";
    EXPECT_FALSE(App().ItemsFadedForCreation());
    EXPECT_GT(MostOpaqueSnippetVertex(), faint);

    PressKey(ImGuiKey_S);  // the screenshot tool, in hand before any press
    EXPECT_TRUE(App().ItemsFadedForCreation());
    EXPECT_LE(MostOpaqueSnippetVertex(), faint);
    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().ItemsFadedForCreation());
}

// ===== The cheat sheet =====

// Its key opens it and closes it again, and while it is up the canvas
// underneath takes none of the keys or presses meant for it: a tool key
// picks nothing, Escape closes the sheet and nothing else, and a press on
// the dimmed canvas makes no snippet.
TEST_F(HeadlessAppTest, CtrlHOpensTheCheatSheetAndTheCanvasWaitsUnderIt) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 600.0f, 500.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    ASSERT_EQ(App().Selection().size(), 1u);

    PressCtrlKey(ImGuiKey_H);
    ASSERT_TRUE(App().IsCheatSheetOpen());
    PressKey(ImGuiKey_E);  // the eraser, for the selected snippet - were the sheet not up
    EXPECT_EQ(App().ActiveTool(), Tool::Select);
    EXPECT_FALSE(App().DrawingItem().has_value());
    Drag(700.0f, 200.0f, 900.0f, 400.0f);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "a drag over the sheet's backdrop makes nothing";

    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(App().IsCheatSheetOpen());
    EXPECT_EQ(App().Selection().size(), 1u) << "Escape closed the sheet, and did not also deselect";

    PressCtrlKey(ImGuiKey_H);
    ASSERT_TRUE(App().IsCheatSheetOpen());
    PressCtrlKey(ImGuiKey_H);
    EXPECT_FALSE(App().IsCheatSheetOpen()) << "its own key closes it";

    PressCtrlKey(ImGuiKey_H);
    ASSERT_TRUE(App().IsCheatSheetOpen());
    Click(4.0f, 4.0f);  // the backdrop, in the corner, as far from the panel as it gets
    EXPECT_FALSE(App().IsCheatSheetOpen());
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);
}

// Undo and redo wait under a panel too: what they changed would be on a
// canvas nobody can see.
TEST_F(HeadlessAppTest, UndoWaitsUnderAPanel) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 600.0f, 500.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);

    PressCtrlKey(ImGuiKey_H);
    ASSERT_TRUE(App().IsCheatSheetOpen());
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u);

    PressKey(ImGuiKey_Escape);
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "and takes it back once the canvas is in sight";
}

// The welcome note names the key that brings the overlay back and the
// cheat sheet's, as they are bound, and no other hotkey: the sheet has
// the rest.
TEST_F(HeadlessAppTest, TheWelcomeNotePointsAtTheCheatSheet) {
    controller_->Overlay().RequestWelcomeNote();
    ShowEditMode();
    StepFrames(2);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 3u);
    const std::string& text = Canvases().CurrentOrNull()->items[0].noteText;
    EXPECT_NE(text.find("Ctrl+Alt+S"), std::string::npos) << text;
    EXPECT_NE(text.find("Ctrl+H"), std::string::npos) << text;
    EXPECT_EQ(text.find("Ctrl+Alt+V"), std::string::npos) << text;
}

TEST_F(HeadlessAppTest, WithTheCheatSheetUnboundTheWelcomeNoteSendsYouToTheMenu) {
    controller_->GetSettings().SetShortcut(ShortcutAction::CheatSheet, platform::KeyCombo{}, std::nullopt);
    controller_->Overlay().RequestWelcomeNote();
    ShowEditMode();
    StepFrames(2);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 3u);
    const std::string& text = Canvases().CurrentOrNull()->items[0].noteText;
    EXPECT_NE(text.find("Right-click empty space for the cheat sheet"), std::string::npos) << text;
    EXPECT_EQ(text.find("(none)"), std::string::npos) << text;
}

// Beside the welcome, the two things a new user must not skip: set the
// behavior up per program, and beware of anti-cheat. Larger and in red,
// side by side with the welcome rather than over it, and all on screen.
TEST_F(HeadlessAppTest, AFirstRunOpensWithTheWelcomeAndTwoWarnings) {
    controller_->Overlay().RequestWelcomeNote();
    ShowEditMode();
    StepFrames(2);
    const Canvas& canvas = *Canvases().CurrentOrNull();
    ASSERT_EQ(canvas.items.size(), 3u);
    EXPECT_EQ(canvas.items[0].name, strings::kWelcomeName);
    EXPECT_EQ(canvas.items[1].name, strings::kWelcomeBehaviorName);
    EXPECT_EQ(canvas.items[2].name, strings::kWelcomeAntiCheatName);
    EXPECT_NE(canvas.items[1].noteText.find("Settings > Behavior"), std::string::npos);
    EXPECT_NE(canvas.items[2].noteText.find("anti-cheat"), std::string::npos);

    const Item& welcome = canvas.items[0];
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    for (size_t i = 0; i < canvas.items.size(); ++i) {
        const Item& note = canvas.items[i];
        EXPECT_FALSE(note.hasBackground);
        EXPECT_GE(note.rect.x, 0.0f);
        EXPECT_GE(note.rect.y, 0.0f);
        EXPECT_LE(note.rect.x + note.rect.w, display.x);
        EXPECT_LE(note.rect.y + note.rect.h, display.y);
        for (size_t j = i + 1; j < canvas.items.size(); ++j) {
            const Rect& a = note.rect;
            const Rect& b = canvas.items[j].rect;
            const bool apart = a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
            EXPECT_TRUE(apart) << "notes " << i << " and " << j << " overlap";
        }
        if (i > 0) {
            EXPECT_GT(note.noteTextSizePx, welcome.noteTextSizePx) << "the warnings are the larger text";
            EXPECT_NE(note.noteTextColorRGBA, welcome.noteTextColorRGBA);
            EXPECT_GT(note.noteTextColorRGBA >> 24, (note.noteTextColorRGBA >> 16) & 0xFFu) << "red over green";
        }
    }
}

// The welcome notes are the app talking, and are made at the interface
// scale - text and all - though a note's text is otherwise its own size.
TEST_F(HeadlessAppTest, TheWelcomeIsMadeAtTheInterfaceScale) {
    host_.overlayWindow.scalePercent = 150;
    controller_->Overlay().RequestWelcomeNote();
    ShowEditMode();
    StepFrames(2);
    const Canvas& canvas = *Canvases().CurrentOrNull();
    ASSERT_EQ(canvas.items.size(), 3u);
    EXPECT_FLOAT_EQ(canvas.items[0].noteTextSizePx, 18.0f * 1.5f);
    EXPECT_FLOAT_EQ(canvas.items[0].rect.w, 400.0f * 1.5f);
    EXPECT_FLOAT_EQ(canvas.items[1].noteTextSizePx, 22.0f * 1.5f);
}

// A device the driver replaced takes every texture with it. The next frame
// lets go of each, and makes again the ones it draws: the frozen screen
// from the pixels kept, and a snippet's picture from the library.
TEST_F(HeadlessSaveTest, AfterALostDeviceEveryTextureIsMadeAgainBeforeItIsDrawn) {
    AppConfig config = DefaultConfig();
    config.profileable.freezeScreen = true;  // the drag crops the frozen screen
    StartWith(config);
    AttachStore();
    host_.overlayWindow.captureReturnsWidth = static_cast<int>(kDisplayWidth);
    host_.overlayWindow.captureReturnsHeight = static_cast<int>(kDisplayHeight);
    host_.overlayWindow.captureReturnsPixelsRGBA.assign(static_cast<size_t>(kDisplayWidth * kDisplayHeight) * 4, 255);
    host_.overlayWindow.uploadsSucceed = true;
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    StepFrame();
    Session& session = controller_->GetSession();
    const ItemId shot = Canvases().CurrentOrNull()->items[0].id;
    const uint64_t pictureBefore = PictureTextureOf(session, shot);
    const uint64_t frozenBefore = session.FrozenScreenTexture();
    ASSERT_TRUE(host_.overlayWindow.IsDrawable(pictureBefore));
    ASSERT_TRUE(host_.overlayWindow.IsDrawable(frozenBefore));

    ++host_.overlayWindow.textureGeneration;
    StepFrame();
    EXPECT_EQ(host_.overlayWindow.liveTextures.count(pictureBefore), 0u) << "let go of";
    EXPECT_EQ(host_.overlayWindow.liveTextures.count(frozenBefore), 0u) << "let go of";
    const uint64_t pictureAfter = PictureTextureOf(session, shot);
    EXPECT_TRUE(host_.overlayWindow.IsDrawable(pictureAfter)) << "read back from the library";
    EXPECT_TRUE(host_.overlayWindow.IsDrawable(session.FrozenScreenTexture())) << "from the pixels kept";
    EXPECT_TRUE(FilterDrawnWith(pictureAfter) == std::nullopt) << "drawn, with the new texture";
    EXPECT_EQ(host_.overlayWindow.badTextureUses, 0);
}

// Whatever happens - screenshots and drawings made, copied, sent, deleted,
// undone and redone, canvases switched, deleted and erased, the stroke
// renderer switched, the canvas bar's previews shown, the overlay put away,
// the device replaced and uploads failing - no frame draws a texture that
// is not live on the device there is, none is released twice or updated
// once gone, and every texture the window holds is one the cache holds.
// And at the end, on a canvas of nothing, nothing is held but the frozen
// screen being shown.
TEST_F(HeadlessSaveTest, EveryTextureDrawnIsLiveWhateverHappens) {
    constexpr uint32_t kSeeds = 6;
    constexpr int kSteps = 120;
    AppConfig config = DefaultConfig();
    config.profileable.freezeScreen = true;
    config.showCanvasBar = true;
    config.overviewShowsBitmaps = true;
    FakeOverlayWindow& window = host_.overlayWindow;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        if (controller_) {
            controller_->GetSession().SetLibraryStore(nullptr);
        }
        store_.reset();
        std::filesystem::remove(Library());
        window.visible = false;  // the last seed's overlay went with its app
        window.uploadsSucceed = true;
        window.captureReturnsWidth = 64;
        window.captureReturnsHeight = 48;
        window.captureReturnsPixelsRGBA.assign(64u * 48u * 4u, static_cast<uint8_t>(seed));
        StartWith(config);
        AttachStore();
        ShowEditMode();
        StepFrame();
        Session& session = controller_->GetSession();
        std::mt19937 rng(seed);
        const auto pick = [&rng](size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(rng); };
        const auto rect = [&rng] {
            std::uniform_real_distribution<float> at(0.0f, 60.0f);
            std::uniform_real_distribution<float> size(20.0f, 300.0f);
            return Rect{at(rng), at(rng), size(rng), size(rng)};
        };
        const auto itemsHere = [&session] {
            std::vector<ItemId> ids;
            if (const Canvas* canvas = session.Manager().CurrentOrNull()) {
                for (const Item& item : canvas->items) {
                    if (!session.Manager().IsDeleted(*canvas, item)) {
                        ids.push_back(item.id);
                    }
                }
            }
            return ids;
        };
        const auto canvases = [&session](bool deleted) {
            std::vector<CanvasId> ids;
            for (const Canvas& canvas : session.Manager().Canvases()) {
                if (canvas.id != session.Manager().CurrentCanvasId() && session.Manager().IsDeleted(canvas) == deleted) {
                    ids.push_back(canvas.id);
                }
            }
            return ids;
        };

        for (int step = 0; step < kSteps && !HasFailure(); ++step) {
            const std::vector<ItemId> here = itemsHere();
            switch (pick(18)) {
                case 0:
                case 1:
                case 2:
                    session.CreateItem(true, rect(), "Shot");
                    break;
                case 3: {
                    const Rect box = rect();
                    const ItemId drawing = session.CreateItem(false, box, "Drawing");
                    if (drawing != 0) {
                        session.LiveLayer().BeginStroke(StrokePoint{box.x + 2.0f, box.y + 2.0f}, 0xFF0000FFu, 3.0f);
                        session.LiveLayer().ExtendStroke(StrokePoint{box.x + box.w * 0.5f, box.y + box.h * 0.5f});
                        session.LiveLayer().EndStroke();
                        session.CommitLiveStroke(drawing);
                    }
                    break;
                }
                case 4:
                    if (const std::vector<CanvasId> live = canvases(false); !live.empty()) {
                        session.SwitchToCanvas(live[pick(live.size())]);
                    }
                    break;
                case 5:
                    session.SwitchToCanvas(session.AddCanvas("Another"));
                    break;
                case 6:
                    if (!here.empty()) {
                        session.DeleteItem(here[pick(here.size())]);
                    }
                    break;
                case 7:
                    session.Undo();
                    break;
                case 8:
                    session.Redo();
                    break;
                case 9:
                    if (!here.empty()) {
                        session.Duplicate({here[pick(here.size())]});
                    }
                    break;
                case 10:
                    if (const std::vector<CanvasId> live = canvases(false); !here.empty() && !live.empty()) {
                        session.SendItemsTo({here[pick(here.size())]}, live[pick(live.size())], pick(2) == 0);
                    }
                    break;
                case 11:
                    if (const std::vector<CanvasId> live = canvases(false); !live.empty()) {
                        session.Delete(live[pick(live.size())]);
                    }
                    break;
                case 12:
                    if (const std::vector<CanvasId> gone = canvases(true); !gone.empty()) {
                        const CanvasId target = gone[pick(gone.size())];
                        pick(2) == 0 ? session.Restore(target) : session.DeletePermanently(target);
                    }
                    break;
                case 13:
                    ++window.textureGeneration;  // the driver replaced the device
                    break;
                case 14:
                    window.uploadsSucceed = !window.uploadsSucceed;
                    break;
                case 15:
                    controller_->GetSettings().Set(setting::kStrokeRenderMode,
                                                   AppSettings().Stored().strokeRenderMode == StrokeRenderMode::Rasterized
                                                       ? StrokeRenderMode::Tessellated
                                                       : StrokeRenderMode::Rasterized);
                    break;
                case 16:
                    // Out to the bottom edge, where the canvas bar and its
                    // previews come out, or back up.
                    MoveTo(kDisplayWidth * 0.5f, pick(2) == 0 ? kDisplayHeight - 2.0f : kDisplayHeight * 0.5f);
                    break;
                case 17:
                    ShowEditMode();  // put away, frozen screen and all
                    ShowEditMode();  // and up again, frozen afresh
                    break;
            }
            const int frames = 1 + static_cast<int>(pick(3));
            for (int frame = 0; frame < frames && !HasFailure(); ++frame) {
                StepFrame();
                for (const uint64_t texture : TexturesDrawn()) {
                    ASSERT_TRUE(window.IsDrawable(texture)) << "step " << step << " drew texture " << texture;
                }
                ASSERT_EQ(window.badTextureUses, 0) << "step " << step;
                ASSERT_EQ(window.liveTextures.size(), session.Textures().Held()) << "step " << step;
            }
        }

        // The bar comes out for a while after a canvas switch, previews and
        // all; switched off, it is gone at once.
        window.uploadsSucceed = true;
        session.SwitchToCanvas(session.AddCanvas("Nothing"));
        controller_->GetSettings().Set(setting::kShowCanvasBar, false);
        StepFrames(3);
        const bool frozen = session.FrozenScreenTexture() != 0;
        EXPECT_EQ(window.liveTextures.size(), frozen ? 1u : 0u);
    }
}

}  // namespace
}  // namespace sz::test
