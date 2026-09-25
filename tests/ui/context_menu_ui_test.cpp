// The snippet context menu, driven by row name - see ContextMenu and
// OverlayApp::BuildItemContextMenuRows.
//
// The menu is opened the way a hand opens it: a right click on the
// snippet, through the raw mouse pipeline, since that is where the press
// that asks for it arrives (see OverlayApp::HandleItemGesture). Only the
// rows themselves are clicked by name, which is the part worth asserting
// - that a row is there, that it is available or not, and that choosing
// it runs the action it promises.
#include "fakes/ui_test.h"

#include <cstring>

namespace sz::test {
namespace {

class ContextMenuUiTest : public UiTest {
protected:
    // A fullscreen screenshot to open the menu over, selected but with no
    // ink on it, and nothing else on the canvas.
    void MakeASnippet() {
        ShowEditMode();
        StepFrame();
        DoubleClick(640.0f, 400.0f);
        ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
        PressKey(ImGuiKey_Escape);  // out of the selection the creation left
    }

    void OpenTheMenu() {
        RightClick(640.0f, 400.0f);
        ASSERT_TRUE(App().IsItemContextMenuOpen());
    }

    // The menu is an anonymous popup, so it has no window name to reach it
    // by - "the focused window" is what a popup always is while it is up.
    void ClickRow(const char* id) {
        RunUi(id, [id](ImGuiTestContext* ctx) {
            ctx->SetRef("//$FOCUSED");
            ctx->ItemClick(id);
            ctx->Yield(2);
        });
    }
};

// A menu taller than the room above and below where it was opened - the
// snippet menu at a large interface scale, opened halfway down - is held
// on the screen rather than flipped off the top of it.
TEST_F(ContextMenuUiTest, AMenuThatFitsNeitherWayRoundStaysOnTheScreen) {
    host_.overlayWindow.scalePercent = 250;
    MakeASnippet();
    OpenTheMenu();
    StepFrame();

    const ImGuiContext& g = *ImGui::GetCurrentContext();
    ASSERT_FALSE(g.OpenPopupStack.empty());
    const ImGuiWindow* menu = g.OpenPopupStack.back().Window;
    ASSERT_NE(menu, nullptr);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ASSERT_GT(menu->Size.y, display.y - 400.0f) << "too short to fit below where it was opened";
    ASSERT_GT(menu->Size.y, 400.0f) << "too short to fit above it";
    EXPECT_GE(menu->Pos.y, 0.0f);
    EXPECT_LE(menu->Pos.y + menu->Size.y, display.y);
    EXPECT_GE(menu->Pos.x, 0.0f);
    EXPECT_LE(menu->Pos.x + menu->Size.x, display.x);
}

TEST_F(ContextMenuUiTest, DuplicateFromTheMenuMakesACopy) {
    MakeASnippet();
    OpenTheMenu();

    ClickRow("##menu_duplicate");
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 2u);
    EXPECT_FALSE(App().IsItemContextMenuOpen()) << "choosing a row closes the menu";
}

// The row that only had a shortcut until now (Ctrl+Shift+N): the snippet
// goes to a brand new canvas, and the app is left looking at it.
TEST_F(ContextMenuUiTest, MoveToNewCanvasFromTheMenuTakesTheSnippetThere) {
    MakeASnippet();
    const CanvasId from = Canvases().CurrentCanvasId();
    const size_t canvasesBefore = Canvases().Canvases().size();
    OpenTheMenu();

    ClickRow("##menu_move_to_new_canvas");
    EXPECT_EQ(Canvases().Canvases().size(), canvasesBefore + 1);
    EXPECT_NE(Canvases().CurrentCanvasId(), from);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 1u) << "the snippet came along";
}

// A row that cannot be chosen right now is grayed rather than dropped, so
// the menu keeps the same shape over every snippet. Clear drawing is the
// one that starts out unavailable on a fresh screenshot: there is no ink
// on it yet.
TEST_F(ContextMenuUiTest, ARowWithNothingToDoIsThereButDisabled) {
    MakeASnippet();
    OpenTheMenu();

    ImGuiItemFlags clearFlags = 0;
    ImGuiItemFlags duplicateFlags = 0;
    RunUi("read the rows", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//$FOCUSED");
        clearFlags = ctx->ItemInfo("##menu_clear_drawing").ItemFlags;
        duplicateFlags = ctx->ItemInfo("##menu_duplicate").ItemFlags;
    });
    EXPECT_NE(clearFlags & ImGuiItemFlags_Disabled, 0) << "nothing has been drawn on it";
    EXPECT_EQ(duplicateFlags & ImGuiItemFlags_Disabled, 0);
}

// The Properties popover keeps what describes a snippet and gives what is
// done to it to this menu: none of the old row of action buttons, and a
// picker for each color with white the one swatch left beside the
// background's.
TEST_F(UiTest, ThePropertiesPopoverHasPickersAndNoActionRow) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 700.0f, 550.0f);  // a screenshot, which has a background color
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    RawClick(500.0f, 420.0f);
    ASSERT_EQ(App().Selection().size(), 1u);
    const std::optional<ImVec2> more = App().SelectionBarButtonCenter(ChromeButton::More);
    ASSERT_TRUE(more.has_value());
    RawClick(more->x, more->y);

    bool hasBackgroundPicker = false;
    bool hasTextPicker = false;
    bool hasWhite = false;
    bool hasPreset = false;
    bool hasActionRow = false;
    RunUi("read the popover", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//$FOCUSED");
        hasBackgroundPicker = ctx->ItemExists("##bg_color_section/##bgcolor/##ColorButton");
        hasTextPicker = ctx->ItemExists("##note_text_section/##notetextcolor/##ColorButton");
        hasWhite = ctx->ItemExists("##bg_color_section/##swatch");
        hasPreset = ctx->ItemExists("##bg_color_section/$$0/##swatch") ||
                    ctx->ItemExists("##note_text_section/$$0/##swatch");
        hasActionRow = ctx->ItemExists("##fullscreen") || ctx->ItemExists("##copy_item") ||
                       ctx->ItemExists("##move_item");
    });
    EXPECT_TRUE(hasBackgroundPicker);
    EXPECT_TRUE(hasTextPicker);
    EXPECT_TRUE(hasWhite);
    EXPECT_FALSE(hasPreset);
    EXPECT_FALSE(hasActionRow);
}

// ===== The canvas bar's tiles =====

class CanvasBarMenuUiTest : public UiTest {
protected:
    // A second canvas, so that deleting one still leaves the bar with
    // something on it and the count is unambiguous.
    void MakeASecondCanvas() {
        ShowEditMode();
        StepFrame();
        controller_->GetSession().Manager().AddCanvas("Second");
        StepFrame();
        ASSERT_EQ(Canvases().Canvases().size(), 2u);
    }

    // The bar hides against the bottom edge; the pointer there brings it
    // out, as it does for a hand.
    static void RevealTheBar(ImGuiTestContext* ctx) {
        ctx->MouseMoveToPos(ImVec2(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f));
        ctx->Yield(30);
    }
};

// Right-clicking a tile opens its menu without switching to that canvas -
// a menu is opened to act on something, not to go to it.
TEST_F(CanvasBarMenuUiTest, ARightClickOnATileOpensItsMenuAndDoesNotSwitch) {
    MakeASecondCanvas();
    const CanvasId before = Canvases().CurrentCanvasId();

    RunUi("right-click a tile", [this](ImGuiTestContext* ctx) {
        RevealTheBar(ctx);
        IM_CHECK(App().CanvasBarReveal() >= 1.0f);
        ctx->SetRef("//##canvas_bar");
        ctx->ItemClick("##canvasbar_tile_0", ImGuiMouseButton_Right);
        ctx->Yield(2);
    });
    EXPECT_TRUE(App().IsCanvasContextMenuOpen());
    EXPECT_EQ(Canvases().CurrentCanvasId(), before) << "the right click switched canvas";
}

// Delete asks first, through the same confirmation the Overview's own
// delete button uses - a canvas takes every snippet on it along, and
// unlike a snippet there is no undo entry to take it back with.
TEST_F(CanvasBarMenuUiTest, DeleteFromTheTileMenuAsksAndThenDeletes) {
    MakeASecondCanvas();
    const size_t before = Canvases().Canvases().size();

    RunUi("delete a canvas from its tile", [this](ImGuiTestContext* ctx) {
        RevealTheBar(ctx);
        ctx->SetRef("//##canvas_bar");
        ctx->ItemClick("##canvasbar_tile_0", ImGuiMouseButton_Right);
        ctx->Yield(2);
        IM_CHECK(App().IsCanvasContextMenuOpen());

        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("##canvasmenu_delete");
        ctx->Yield(3);
        // Nothing is gone yet: the row only asked.
        IM_CHECK(Canvases().Canvases().size() == 2u);

        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("##confirmdelete");
        ctx->Yield(3);
    });
    // Deleted things stay in the library, marked, and can be restored with
    // Show deleted in the Overview - so the count is unchanged and what
    // changed is that one of them is now deleted.
    EXPECT_EQ(Canvases().Canvases().size(), before);
    size_t alive = 0;
    for (const Canvas& canvas : Canvases().Canvases()) {
        if (!Canvases().IsDeleted(canvas)) {
            ++alive;
        }
    }
    EXPECT_EQ(alive, 1u);
    EXPECT_FALSE(App().IsCanvasContextMenuOpen());
}

// The picker sends the snippet from the current canvas, so nothing in it
// may change which canvas that is: making a folder switches to a canvas in
// it, and deleting the current canvas moves off it. Both are off while it
// is open; picking a canvas still moves the snippet there.
TEST_F(ContextMenuUiTest, ThePickerKeepsTheCanvasItSendsFrom) {
    MakeASnippet();
    const CanvasId other = controller_->GetSession().Manager().AddCanvas("Other");
    StepFrame();
    const ItemId snippet = Canvases().CurrentOrNull()->items[0].id;
    OpenTheMenu();
    ClickRow("##menu_move_to_canvas");
    ASSERT_TRUE(App().IsOverviewOpen());

    bool newFolderOff = false;
    bool deleteOff = false;
    RunUi("pick a canvas", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        newFolderOff = (ctx->ItemInfo("**/##newfolder").ItemFlags & ImGuiItemFlags_Disabled) != 0;
        deleteOff = (ctx->ItemInfo("**/##delcanvas").ItemFlags & ImGuiItemFlags_Disabled) != 0;
        const std::string tile = "**/$$" + std::to_string(other) + "/##tile";
        ctx->ItemClick(tile.c_str());
    });
    EXPECT_TRUE(newFolderOff);
    EXPECT_TRUE(deleteOff);
    EXPECT_EQ(controller_->GetSession().Manager().CanvasHoldingItem(snippet), std::optional<CanvasId>(other));
}

// "Move to canvas" is there when there is a canvas to move to - and a
// deleted one is not that: the picker does not show it.
TEST_F(ContextMenuUiTest, MoveToCanvasWantsACanvasThatIsNotDeleted) {
    MakeASnippet();
    CanvasManager& manager = controller_->GetSession().Manager();
    const CanvasId other = manager.AddCanvas("Other");
    ASSERT_TRUE(manager.MarkDeleted(other, 100));
    StepFrame();

    OpenTheMenu();
    bool available = true;
    RunUi("look at the row", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//$FOCUSED");
        available = (ctx->ItemInfo("##menu_move_to_canvas").ItemFlags & ImGuiItemFlags_Disabled) == 0;
    });
    EXPECT_FALSE(available);
}

// The picker's Cancel closes the Overview there and then, and nothing more
// of it is drawn that frame - not the Settings page it was last left on,
// which the body fell back to once there was nothing being picked.
TEST_F(ContextMenuUiTest, ThePickersCancelDrawsNothingMoreOfThePanel) {
    MakeASnippet();
    controller_->GetSession().Manager().AddCanvas("Other");
    StepFrame();
    OpenOverviewUi();
    RunUi("leave the overview on Settings", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
    });
    PressKey(ImGuiKey_Escape);
    ASSERT_FALSE(App().IsOverviewOpen());
    OpenTheMenu();
    ClickRow("##menu_move_to_canvas");
    ASSERT_TRUE(App().IsOverviewOpen());

    bool settingsDrawn = true;
    RunUi("cancel the pick", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        const int frame = ImGui::GetFrameCount();
        ctx->ItemClick("**/###pickercancel");
        ctx->Yield(2);
        // By name: a child window's path lookup finds nothing once its
        // parent is gone, and the panel is.
        const ImGuiWindow* body = nullptr;
        for (const ImGuiWindow* window : ctx->UiContext->Windows) {
            body = std::strstr(window->Name, "##settings_body") != nullptr ? window : body;
        }
        IM_CHECK(body != nullptr);  // drawn when Settings was open
        settingsDrawn = body->LastFrameActive >= frame;
    });
    EXPECT_FALSE(App().IsOverviewOpen());
    EXPECT_FALSE(settingsDrawn);
}

}  // namespace
}  // namespace sz::test
