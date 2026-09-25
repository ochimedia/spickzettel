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
#include <string>

#include "core/canvas/item_geometry.h"  // kItemMinWidth/kItemMinHeight, for the group-resize floor
#include "core/persistence/library_store.h"
#include "core/util/uid.h"
#include "ui/overlay_app_internal.h"

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
    namespace theme = overlay_detail::theme;
    ShowEditMode();
    StepFrame();
    EXPECT_FLOAT_EQ(theme::Accent().y, 0x6C / 255.0f) << "teal by default";
    EXPECT_GT(theme::AccentInk().x, 0.5f) << "light ink on the dark default";

    controller_->GetSettings().Mutable().accentColorRGBA = 0xFF6A3DFFu;
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

    controller_->GetSettings().Mutable().uiScalePercent = 200;
    StepFrame();
    EXPECT_FLOAT_EQ(UiScale(), 2.0f) << "the setting, over Windows' 150";
    EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, padding * 2.0f) << "scaled from the base, not from 150%";

    controller_->GetSettings().Mutable().uiScalePercent = 0;
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
        EXPECT_FLOAT_EQ(shot.ImageLayer()->opacity, 0.75f);
        EXPECT_EQ(shot.ImageLayer()->tintColorRGBA, 0xFFFFFFFFu) << "a capture is not tinted by the drawing's color";
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
    EXPECT_FLOAT_EQ(drawing.ImageLayer()->opacity, 0.6f);
    EXPECT_EQ(drawing.ImageLayer()->tintColorRGBA, 0x112233FFu);
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

// A screenshot is drawn through the filter in settings, and the default
// adds nothing to the draw list - it is ImGui's own sampler.
TEST_F(HeadlessAppTest, AScreenshotIsDrawnThroughTheFilterInSettings) {
    AppConfig config = DefaultConfig();
    config.imageFilter = platform::ImageFilter::Lanczos;
    config.freezeScreenInEditMode = true;  // the drag crops the frozen screen
    StartWith(config);
    host_.overlayWindow.captureReturnsHandle = 7;
    host_.overlayWindow.captureReturnsWidth = static_cast<int>(kDisplayWidth);
    host_.overlayWindow.captureReturnsHeight = static_cast<int>(kDisplayHeight);
    host_.overlayWindow.captureReturnsPixelsRGBA.assign(static_cast<size_t>(kDisplayWidth * kDisplayHeight) * 4, 255);
    host_.overlayWindow.createTextureFromPixelsReturnsHandle = 9;  // the part of the frozen screen kept
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    StepFrame();
    EXPECT_EQ(FilterDrawnWith(9), platform::ImageFilter::Lanczos);

    controller_->GetSettings().Mutable().imageFilter = platform::ImageFilter::Bilinear;
    StepFrame();
    EXPECT_EQ(FilterDrawnWith(9), std::nullopt);
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
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    StepFrame();
    RawClick(firstRect.x + 40.0f, firstRect.y + 40.0f);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
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
    CanvasManager& manager = controller_->GetSession().Manager();
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
    CanvasManager& manager = controller_->GetSession().Manager();
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
    CanvasManager& manager = controller_->GetSession().Manager();
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
    CanvasManager& manager = controller_->GetSession().Manager();
    const CanvasId before = manager.CurrentCanvasId();
    const ItemId drawing = manager.CurrentOrNull()->items[0].id;

    // A stroke pressed and moved, not let go of - ImGui told about the
    // button too, since settling asks it whether the button is down.
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(150.0f, 150.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    ASSERT_TRUE(manager.FindCanvas(before)->liveLayer.ActiveStroke().has_value()) << "in flight";

    PressCtrlShiftKey(ImGuiKey_N);

    ASSERT_NE(manager.CurrentCanvasId(), before);
    const Item* item = manager.FindItemAnywhere(drawing);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->strokes.size(), 1u) << "ended on the canvas it started on, before the switch";
    EXPECT_FALSE(manager.FindCanvas(before)->liveLayer.ActiveStroke().has_value());
    EXPECT_FALSE(manager.CurrentOrNull()->liveLayer.ActiveStroke().has_value());

    // The real release comes later and finds nothing in flight.
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Up);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
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
    CanvasManager& manager = controller_->GetSession().Manager();
    const ItemId drawing = manager.CurrentOrNull()->items[0].id;

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(150.0f, 150.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    // No Up.
    RawMouse(500.0f, 150.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(600.0f, 150.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(600.0f, 150.0f, platform::MouseEventKind::Up);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrame();
    EXPECT_EQ(manager.FindItemAnywhere(drawing)->strokes.size(), 2u);
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
    config.toolShortcuts[ShortcutActionIndex(ShortcutAction::NewCanvas)] =
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
// the selection: an open popover takes the first press (see
// HandleSelectionKeys).
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

    controller_->GetSession().Manager().DeleteItem(*App().ItemContextMenuItem());
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
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
    StepFrame();
    Drag(680.0f, 330.0f, 780.0f, 330.0f, 10, platform::MouseButton::Right);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
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
// leaves the snippet let go of, and a tool key's tool stays in hand.
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
    EXPECT_TRUE(App().Selection().empty());
    RawMouse(400.0f, 400.0f, platform::MouseEventKind::Up);
    StepFrames(2);
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

    controller_->GetSettings().Mutable().showItemBorders = !AppSettings().Stored().showItemBorders;
    controller_->GetSettings().Commit();

    EXPECT_FALSE(host_.overlayWindow.editModeNoActivate);
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
TEST_F(HeadlessAppTest, AKeyStrandedByHidingIsNotAPressOnTheWayBack) {
    ShowEditMode();
    StepFrame();
    ASSERT_EQ(App().ActiveTool(), Tool::Select);

    ShowEditMode();  // the same hotkey again puts the overlay away
    ASSERT_FALSE(host_.overlayWindow.visible);
    // No frame in between, which is the whole point: hidden, there are none.
    ImGui::GetIO().AddKeyEvent(ImGuiKey_S, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_S, false);

    ShowEditMode();
    StepFrames(2);
    EXPECT_EQ(App().ActiveTool(), Tool::Select) << "that S belonged to the showing it ended";
}

// The same staleness the other way round: what ImGui believes is held is
// whatever the last frame before the hiding saw. A modifier latched that
// way would make the exact-modifier test in HandleToolShortcuts refuse an
// ordinary key press on the way back.
TEST_F(HeadlessAppTest, AModifierHeldWhenTheOverlayWentAwayDoesNotOutliveIt) {
    ShowEditMode();
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    StepFrame();  // the frame that records it as held
    ASSERT_TRUE(ImGui::GetIO().KeyCtrl);

    ShowEditMode();  // away, with Ctrl still down as far as ImGui knows
    ShowEditMode();  // and back
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
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
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
    EXPECT_EQ(host_.overlayWindow.showWithoutActivatingCallCount, 1)
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
        ImGui::GetIO().AddKeyEvent(modifier, true);
        StepFrame();
        RawClick(x, y);
        ImGui::GetIO().AddKeyEvent(modifier, false);
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
    config.editModeInput.useSoftwarePointer = false;
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
    config.editModeInput.useSoftwarePointer = false;  // see the test above
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
    config.editModeInput.useSoftwarePointer = false;
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
    const ItemId otherId = controller_->GetSession().Manager().CreateItem(false, Rect{1200.0f, 100.0f, 200.0f, 200.0f}, "Other");
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
    CanvasManager& manager = controller_->GetSession().Manager();
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

TEST_F(OverlappingItemsTest, TheArrowKeysNudgeTheSelection) {
    ShowEditMode();
    StepFrame();
    const OverlappingItems items = MakeOverlappingItems();
    SelectTheBackItem(items);

    PressKey(ImGuiKey_RightArrow);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x + 1.0f);

    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    PressKey(ImGuiKey_DownArrow);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
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

// Mid-drag the arrow keys and the wheel leave the snippet to the drag:
// what either filed would be undone to a place the drag had since left.
TEST_F(OverlappingItemsTest, ArrowKeysAndTheWheelWaitForADragToEnd) {
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
    PressKey(ImGuiKey_DownArrow);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
    StepFrame();
    RawMouse(x + 40.0f, y, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(x + 40.0f, y, platform::MouseEventKind::Up);
    StepFrames(2);
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y);
    EXPECT_FLOAT_EQ(BackItem().rect.w, items.back.w);
    ASSERT_FLOAT_EQ(BackItem().rect.x, items.back.x + 40.0f);

    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x) << "the drag, whole, in one step";
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FLOAT_EQ(BackItem().rect.x, items.back.x) << "and no step under it to a place mid-drag";
    EXPECT_FLOAT_EQ(BackItem().rect.y, items.back.y);
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
    const Rect before = controller_->GetSession().Manager().FindItemAnywhere(id)->rect;
    const float widthBefore = AppSettings().Stored().strokeWidth;

    MoveTo(500.0f, 400.0f);
    Wheel(1.0f);
    EXPECT_FLOAT_EQ(controller_->GetSession().Manager().FindItemAnywhere(id)->rect.w, before.w) << "drawing mode: the pen, not the snippet";
    StepFrames(400);
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, widthBefore + 1.0f);

    PressKey(ImGuiKey_Escape);  // out of drawing mode, still selected
    ASSERT_FALSE(App().DrawingItem().has_value());
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{id});
    Wheel(2.0f);
    const Rect after = controller_->GetSession().Manager().FindItemAnywhere(id)->rect;
    EXPECT_NEAR(after.w, before.w * 1.21f, 0.5f);
    EXPECT_NEAR(after.h, before.h * 1.21f, 0.5f);
    EXPECT_NEAR(after.x + after.w * 0.5f, before.x + before.w * 0.5f, 0.5f) << "about its middle";
    EXPECT_NEAR(after.y + after.h * 0.5f, before.y + before.h * 0.5f, 0.5f);
    Wheel(-2.0f);
    EXPECT_NEAR(controller_->GetSession().Manager().FindItemAnywhere(id)->rect.w, before.w, 0.5f) << "and back";
    EXPECT_FLOAT_EQ(AppSettings().Stored().strokeWidth, widthBefore + 1.0f) << "the pen untouched";

    PressKey(ImGuiKey_Escape);  // nothing selected: the wheel has nothing to do
    Wheel(1.0f);
    EXPECT_NEAR(controller_->GetSession().Manager().FindItemAnywhere(id)->rect.w, before.w, 0.5f);
}

// Ctrl with the wheel is the selection's background opacity, Shift its
// foreground's, five percent a notch, within the popover's ranges.
TEST_F(HeadlessAppTest, CtrlAndShiftWithTheWheelSetTheSelectionsOpacities) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 700.0f, 550.0f);  // a region: a screenshot, with a picture layer
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId id = Canvases().CurrentOrNull()->items[0].id;
    RawClick(500.0f, 400.0f);
    ASSERT_EQ(App().Selection(), std::vector<ItemId>{id});
    ASSERT_FALSE(App().DrawingItem().has_value());
    const Rect before = controller_->GetSession().Manager().FindItemAnywhere(id)->rect;

    const auto wheelWith = [this](ImGuiKey modifier, float notches) {
        ImGui::GetIO().AddKeyEvent(modifier, true);
        StepFrame();
        Wheel(notches);
        ImGui::GetIO().AddKeyEvent(modifier, false);
        StepFrame();
    };
    wheelWith(ImGuiMod_Ctrl, -2.0f);
    const Item* item = controller_->GetSession().Manager().FindItemAnywhere(id);
    ASSERT_NE(item->ImageLayer(), nullptr);
    EXPECT_FLOAT_EQ(item->ImageLayer()->opacity, 0.9f);
    EXPECT_FLOAT_EQ(item->foregroundOpacity, 1.0f);

    wheelWith(ImGuiMod_Shift, -40.0f);
    EXPECT_FLOAT_EQ(controller_->GetSession().Manager().FindItemAnywhere(id)->foregroundOpacity, 0.1f) << "not below a tenth";
    wheelWith(ImGuiMod_Ctrl, 5.0f);
    EXPECT_FLOAT_EQ(controller_->GetSession().Manager().FindItemAnywhere(id)->ImageLayer()->opacity, 1.0f) << "not above whole";
    EXPECT_FLOAT_EQ(controller_->GetSession().Manager().FindItemAnywhere(id)->rect.w, before.w) << "a modified wheel does not scale";
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
    config.toolShortcuts[ShortcutActionIndex(ShortcutAction::Text)] =
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

// The key for Text pressed halfway through a stroke: the stroke still gets
// its moves and its release, and ends as any other does. Taken by Text
// instead, it stayed in flight, and nothing could be pressed after it.
TEST_F(HeadlessAppTest, TextPickedMidStrokeLetsTheStrokeFinish) {
    StartWith(WithTextOnT());
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 500.0f);
    CanvasManager& manager = controller_->GetSession().Manager();
    const ItemId drawing = manager.CurrentOrNull()->items[0].id;

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(350.0f, 350.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(450.0f, 400.0f, platform::MouseEventKind::Move);
    StepFrame();
    PressKey(ImGuiKey_T);
    ASSERT_EQ(App().ActiveTool(), Tool::Text);
    RawMouse(500.0f, 420.0f, platform::MouseEventKind::Move);
    RawMouse(500.0f, 420.0f, platform::MouseEventKind::Up);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrame();
    EXPECT_EQ(manager.FindItemAnywhere(drawing)->strokes.size(), 1u);
    EXPECT_FALSE(manager.CurrentOrNull()->liveLayer.ActiveStroke().has_value());

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

// Hiding and exiting both flush, and a note being typed lives in the
// editor's buffer until it is committed - so both have to commit it first,
// or the flush writes the note as it was when the editor opened.
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
    ASSERT_TRUE(Canvases().CurrentOrNull()->items[0].noteText.empty()) << "typed, not yet committed";

    ShowEditMode();  // the edit hotkey again: put away, with the editor still open
    EXPECT_FALSE(host_.overlayWindow.IsVisible());
    EXPECT_FALSE(App().EditingNote().has_value());
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].noteText, "ab");
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
    ASSERT_TRUE(persistence::LibraryStore(dir).Save(snapshot));
    host_.dataDirectoryPath = dir;

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
    controller_->GetSession().Manager().CurrentOrNull()->items[0].noteText = longNote;

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
// with the disk made to fail in the two ways the app has to survive: a
// record that cannot be written, and a picture that cannot.
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

    // A store in the temp directory, as TrayController attaches its own.
    void AttachStore() {
        store_ = std::make_unique<persistence::LibraryStore>(root_);
        controller_->GetSession().SetLibraryStore(store_.get());
    }

    std::filesystem::path root_;
    std::unique_ptr<persistence::LibraryStore> store_;
};

// A failed save that fell through to the quiet-period check, which a
// failure does nothing to reset, would be retried on the very next frame
// and every frame after, a full synchronous rewrite each time.
TEST_F(HeadlessSaveTest, AFailedSaveIsRetriedOnItsOwnClockNotEveryFrame) {
    PlaceADrawing();
    // A directory where library.json wants to be: the temp file is written
    // fine and the rename onto it fails, which is a save that fails late.
    std::filesystem::create_directories(root_ / "library.json");
    AttachStore();

    // Past the quiet period: exactly one attempt, which failed - and left
    // no temporary behind to show for it (see WriteFileAtomically), so the
    // session's own account of it is the evidence.
    StepFrames(130);
    ASSERT_TRUE(controller_->GetSession().LastSaveFailed()) << "no attempt was made";
    ASSERT_FALSE(std::filesystem::exists(root_ / "library.json.tmp")) << "nothing half-written left beside it";
    ASSERT_TRUE(std::filesystem::is_directory(root_ / "library.json"));

    // The obstruction goes away. Retried every frame, the next frame would
    // write the file; on its own clock, the retry is still most of two
    // seconds out.
    std::filesystem::remove_all(root_ / "library.json");
    StepFrames(30);
    EXPECT_FALSE(std::filesystem::exists(root_ / "library.json")) << "retried too eagerly";
    StepFrames(120);
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "library.json")) << "never retried";
}

// A save that fails is said on screen for as long as it stays failed -
// what is drawn looks saved whether or not it is.
// Escape with the button still held, halfway through a stroke of painted
// pixels on a layer whose earlier strokes are saved: the stroke is ended
// there as a release would end it - one undo step, and a change the next
// save writes. Dropped instead, the pixels stayed on screen with nothing
// recorded, so a flush wrote nothing and they were gone at the next start.
TEST_F(HeadlessSaveTest, LeavingDrawingModeMidPaintStrokeKeepsAndSavesTheStroke) {
    AppConfig config = DefaultConfig();
    config.paintPixelsInsteadOfStrokes = true;
    StartWith(config);
    host_.overlayWindow.createTextureFromPixelsReturnsHandle = 7;
    AttachStore();
    PlaceADrawing();
    Session& session = controller_->GetSession();
    const ItemId drawing = Canvases().CurrentOrNull()->items[0].id;
    const auto paintedPixels = [&]() -> std::vector<uint8_t> {
        for (const Layer& layer : session.Manager().FindItemAnywhere(drawing)->layers) {
            if (layer.kind == LayerKind::Painted && layer.painted) {
                return layer.painted->PixelsRGBA();
            }
        }
        return {};
    };

    // A first stroke, whole, and saved.
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(300.0f, 300.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(400.0f, 300.0f, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(400.0f, 300.0f, platform::MouseEventKind::Up);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrame();
    ASSERT_TRUE(session.Flush());
    ASSERT_FALSE(session.HasUnsavedChanges());
    const std::vector<uint8_t> afterFirst = paintedPixels();
    ASSERT_FALSE(afterFirst.empty());

    // A second, left mid-way.
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RawMouse(300.0f, 500.0f, platform::MouseEventKind::Down);
    StepFrame();
    RawMouse(500.0f, 500.0f, platform::MouseEventKind::Move);
    StepFrame();
    EXPECT_TRUE(session.HasUnsavedChanges()) << "painted pixels are unsaved work before the stroke ends";
    PressKey(ImGuiKey_Escape);
    ASSERT_FALSE(App().DrawingItem().has_value());
    RawMouse(500.0f, 500.0f, platform::MouseEventKind::Up);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrames(2);
    const std::vector<uint8_t> afterSecond = paintedPixels();
    ASSERT_NE(afterSecond, afterFirst);

    EXPECT_TRUE(session.HasUnsavedChanges());
    ASSERT_TRUE(session.Flush());
    persistence::LibraryStore reopened(root_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    std::string file;
    for (const Item& item : loaded->canvases[0].items) {
        for (const Layer& layer : item.layers) {
            file = item.id == drawing && layer.kind == LayerKind::Painted ? layer.imageFile : file;
        }
    }
    const std::optional<persistence::DecodedImage> onDisk = reopened.LoadImage(drawing, file);
    ASSERT_TRUE(onDisk.has_value());
    EXPECT_EQ(onDisk->pixelsRGBA, afterSecond) << "the second stroke is on disk";

    // And it is one undo step of its own, taken back whole.
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(paintedPixels(), afterFirst);
}

TEST_F(HeadlessSaveTest, AFailedSaveIsSaidOnScreenUntilItLands) {
    PlaceADrawing();
    std::filesystem::create_directories(root_ / "library.json");
    AttachStore();
    EXPECT_TRUE(App().PersistenceWarning().empty()) << "nothing has failed yet";

    StepFrames(130);  // past the quiet period: one attempt, which failed
    const std::string warning = App().PersistenceWarning();
    EXPECT_NE(warning.find(root_.string()), std::string::npos) << warning;

    std::filesystem::remove_all(root_ / "library.json");
    StepFrames(160);  // past the retry's own clock
    EXPECT_TRUE(App().PersistenceWarning().empty()) << "gone with the save that landed";
}

// A snippet too large to save is said as such: "retried" would promise a
// save that no retry can make.
TEST_F(HeadlessSaveTest, ASnippetTooLargeToSaveIsSaidAsSuch) {
    PlaceADrawing();
    controller_->GetSession().Manager().CurrentOrNull()->items[0].noteText.assign(std::size_t{65} << 20, 'a');
    controller_->GetSession().Manager().MarkChanged();
    AttachStore();
    StepFrames(130);  // past the quiet period: one attempt, which failed
    EXPECT_EQ(App().PersistenceWarning(), std::string(strings::kStatusRecordTooLarge));
}

// The record went through and the picture didn't: the save must not be
// acknowledged on the strength of the half that worked, or the picture
// waits for some unrelated edit to trigger the next save - and a hide or
// exit in the meantime flushes nothing, since nothing looks pending.
TEST_F(HeadlessSaveTest, APictureThatCouldNotBeWrittenKeepsTheSaveUnacknowledged) {
    PlaceADrawing();
    CanvasManagerSnapshot snapshot = Canvases().ExportSnapshot();
    Layer painted;
    painted.kind = LayerKind::Painted;
    painted.opacity = 1.0f;
    painted.painted = std::make_shared<PaintedImage>(16, 16);
    painted.paintedDirty = true;
    snapshot.canvases[0].items[0].layers.push_back(painted);
    const ItemId itemId = snapshot.canvases[0].items[0].id;
    controller_->GetSession().ImportLibrary(std::move(snapshot));
    // A file where the staging directory wants to be: no picture can be
    // written, while every record can.
    std::ofstream(root_ / "staging") << "in the way";
    AttachStore();

    StepFrames(3);
    controller_->GetSession().Flush();
    ASSERT_TRUE(std::filesystem::is_regular_file(root_ / "library.json")) << "the records should have gone through";
    const auto paintedLayer = [this] { return Canvases().CurrentOrNull()->items[0].layers.back(); };
    ASSERT_TRUE(paintedLayer().paintedDirty) << "the pixels can't have been written";

    // Out of the way again. Nothing else changes; the retry alone has to
    // bring the picture to disk.
    std::filesystem::remove(root_ / "staging");
    StepFrames(150);
    EXPECT_FALSE(paintedLayer().paintedDirty) << "the failed picture was never retried";
    EXPECT_TRUE(store_->LoadImage(itemId, FormatUid(itemId) + "_p1.qoi").has_value());
}

// ===== Deleted things, on the screen =====

namespace {
// Whether a directory named for `id` (see MakeSlug: "<name>-<uid>") exists
// anywhere under `root` - where a folder, canvas or snippet is on disk.
bool DirectoryFor(const std::filesystem::path& root, uint64_t id) {
    const std::string suffix = "-" + FormatUid(id);
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(ec) && name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}
}  // namespace

// Deleting a snippet marks it deleted where it is - off the screen, its
// directory staying put with the stamp in its record - and Undo brings it
// back.
TEST_F(HeadlessSaveTest, DeletingASnippetHidesItInPlaceAndUndoBringsItBack) {
    PlaceADrawing();
    AttachStore();
    controller_->GetSession().Flush();
    const Item drawing = Canvases().CurrentOrNull()->items[0];
    ASSERT_TRUE(DirectoryFor(root_ / "folders", drawing.id));

    // Selected, and the Delete key.
    PressKey(ImGuiKey_Escape);
    RawClick(drawing.rect.x + drawing.rect.w * 0.5f, drawing.rect.y + drawing.rect.h * 0.5f);
    ASSERT_EQ(App().Selection().size(), 1u);
    PressKey(ImGuiKey_Delete);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "off the screen";
    ASSERT_EQ(Canvases().CurrentOrNull()->items.size(), 1u) << "but still in the library";
    EXPECT_NE(Canvases().CurrentOrNull()->items[0].deletedAt, 0);
    controller_->GetSession().Flush();
    EXPECT_TRUE(DirectoryFor(root_ / "folders", drawing.id)) << "its directory stays where it is";
    EXPECT_FALSE(std::filesystem::exists(root_ / "retired"));

    // Undo, by key.
    PressCtrlKey(ImGuiKey_Z);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].deletedAt, 0);
}

// ===== The library tree HUD shows the disk, not the model =====

TEST_F(HeadlessSaveTest, TheLibraryTreeHudReadsTheDiskAgainAfterEveryWrite) {
    AppConfig config = DefaultConfig();
    config.showLibraryTreeHud = true;
    StartWith(std::move(config));
    PlaceADrawing();
    AttachStore();

    // Nothing written yet: an empty root shows as nothing.
    StepFrame();
    EXPECT_TRUE(App().LibraryTreeHudLines().empty());

    controller_->GetSession().Flush();
    StepFrame();
    const auto& lines = App().LibraryTreeHudLines();
    const auto find = [&lines](const std::string& name) {
        return std::find_if(lines.begin(), lines.end(),
                            [&name](const OverlayApp::LibraryTreeLine& line) { return line.name == name; });
    };
    ASSERT_NE(find("library.json"), lines.end()) << "the pointer file the save just wrote";
    EXPECT_FALSE(find("library.json")->isDirectory);
    EXPECT_EQ(find("library.json")->depth, 0);
    ASSERT_NE(find("folders"), lines.end());
    EXPECT_TRUE(find("folders")->isDirectory);
    ASSERT_NE(find("item.json"), lines.end()) << "the drawing's record, four levels down";
    EXPECT_EQ(find("item.json")->depth, 4);
    // Directories before files at every level, and the tree in one pass:
    // the record's directory sits directly above the record.
    const auto record = find("item.json");
    ASSERT_NE(record, lines.begin());
    EXPECT_TRUE(std::prev(record)->isDirectory);
    EXPECT_EQ(std::prev(record)->depth, 3);

    // A file that appeared behind the store's back is not seen until the
    // store writes again - the walk is per write, not per frame.
    std::ofstream(root_ / "stray.txt") << "put here by hand";
    StepFrames(3);
    EXPECT_EQ(find("stray.txt"), lines.end());
    ASSERT_TRUE(store_->Save(Canvases().ExportSnapshot()));
    StepFrame();
    EXPECT_NE(find("stray.txt"), lines.end()) << "the next write brought it into view";
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
    const unsigned faint = static_cast<unsigned>(255.0f * ui::overlay_detail::kCreationFadeAlpha) + 1u;
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
    controller_->GetSettings().SetShortcut(std::nullopt, ShortcutAction::CheatSheet, platform::KeyCombo{});
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
    config.freezeScreenInEditMode = true;  // the drag crops the frozen screen
    StartWith(config);
    AttachStore();
    host_.overlayWindow.captureReturnsHandle = 7;
    host_.overlayWindow.captureReturnsWidth = static_cast<int>(kDisplayWidth);
    host_.overlayWindow.captureReturnsHeight = static_cast<int>(kDisplayHeight);
    host_.overlayWindow.captureReturnsPixelsRGBA.assign(static_cast<size_t>(kDisplayWidth * kDisplayHeight) * 4, 255);
    host_.overlayWindow.createTextureFromPixelsReturnsHandle = 9;
    ShowEditMode();
    StepFrame();
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    StepFrame();
    Session& session = controller_->GetSession();
    ASSERT_TRUE(session.Flush());
    const Layer* picture = Canvases().CurrentOrNull()->items[0].ImageLayer();
    ASSERT_NE(picture, nullptr);
    ASSERT_EQ(picture->textureHandle, 9u);
    ASSERT_EQ(session.FrozenScreenTexture(), 7u);

    const int releasedBefore = host_.overlayWindow.releaseTextureCallCount;
    host_.overlayWindow.createTextureFromPixelsReturnsHandle = 11;
    host_.overlayWindow.textureGeneration = 1;
    StepFrame();
    EXPECT_EQ(host_.overlayWindow.releaseTextureCallCount - releasedBefore, 2) << "the picture and the frozen screen";
    EXPECT_EQ(Canvases().CurrentOrNull()->items[0].ImageLayer()->textureHandle, 11u);
    EXPECT_EQ(session.FrozenScreenTexture(), 11u);
    EXPECT_TRUE(FilterDrawnWith(11) == std::nullopt) << "drawn, with the new texture";
}

}  // namespace
}  // namespace sz::test
