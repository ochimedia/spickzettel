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

// A row that cannot be chosen right now is greyed rather than dropped, so
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

}  // namespace
}  // namespace sz::test
