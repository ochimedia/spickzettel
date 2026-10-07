// The view as docs/VIEW_LAYER.md describes it, where only a widget clicked
// by name reaches it: a popup ImGui opens inside a surface (section 3), what
// each of the Overview's and the dock's widgets asks for (section 6), and
// what closing a popup does (section 4). Written against the app as it was
// before that document's changes, so that each change shows it moved
// nothing it did not mean to.
#include <string>
#include <vector>

#include "fakes/ui_test.h"
#include "generated/ui_strings.h"
#include "support/session_test_access.h"
#include "support/view_stack.h"

namespace sz::test {
namespace {

// "$$n" is how the engine spells a PushID(n) - every sidebar row and grid
// tile is under its folder's or canvas's own.
std::string Under(uint64_t id, const char* item) { return "**/$$" + std::to_string(id) + "/" + item; }

class ViewLayerUiTest : public UiTest {
protected:
    CanvasManager& Model() { return test::Model(controller_->GetSession()); }

    // The canvases of `folder`, in its order, deleted ones left out.
    std::vector<CanvasId> CanvasesOf(FolderId folder) const {
        std::vector<CanvasId> ids;
        for (const Canvas& canvas : Canvases().Canvases()) {
            if (canvas.folderId == folder && !Canvases().IsDeleted(canvas)) {
                ids.push_back(canvas.id);
            }
        }
        return ids;
    }
    std::vector<FolderId> FolderOrder() const {
        std::vector<FolderId> ids;
        for (const Folder& folder : Canvases().Folders()) {
            ids.push_back(folder.id);
        }
        return ids;
    }
};

// ===== The stack: ImGui's popups inside a surface (section 3) =====

TEST_F(ViewLayerUiTest, APopupInsideTheOverviewSitsAboveIt) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    std::vector<std::string> stack;
    RunUi("open a picker in Settings", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##editbordercolor/##ColorButton");
        ctx->Yield(2);
        stack = SurfacesBackToFront();
    });
    const std::vector<std::string> expected = {"canvas",   "items",   "chrome", "overview backdrop",
                                               "overview", "popup in overview"};
    ASSERT_TRUE(InStackOrder(expected));
    EXPECT_EQ(Describe(stack), Describe(expected));
}

TEST_F(ViewLayerUiTest, APickerInsidePropertiesSitsAboveIt) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 700.0f, 550.0f);  // a screenshot, which has a background color
    const std::optional<ImVec2> more = App().SelectionBarButtonCenter(ChromeButton::More);
    ASSERT_TRUE(more.has_value());
    RawClick(more->x, more->y);
    std::vector<std::string> stack;
    RunUi("open the background picker", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("##bg_color_section/##bgcolor/##ColorButton");
        ctx->Yield(2);
        stack = SurfacesBackToFront("properties");
    });
    // The canvas bar is out for a moment after the overlay comes up, and
    // may or may not be by now; it sits below the popups either way.
    std::erase(stack, std::string("canvas bar"));
    const std::vector<std::string> expected = {"canvas", "items", "properties", "popup in properties", "chrome"};
    ASSERT_TRUE(InStackOrder(expected));
    EXPECT_EQ(Describe(stack), Describe(expected));
}

// ===== What the Overview's widgets ask for (section 6) =====

// A tile switches to its canvas, and the Overview goes.
TEST_F(ViewLayerUiTest, ATileInTheOverviewSwitchesToItsCanvasAndClosesIt) {
    ShowEditMode();
    StepFrame();
    const CanvasId other = Model().AddCanvas("Other");
    StepFrame();
    ASSERT_NE(Canvases().CurrentCanvasId(), other);
    OpenOverviewUi();

    const std::string tile = Under(other, "##tile");
    RunUi("click the tile", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick(tile.c_str());
        ctx->Yield(2);
    });
    EXPECT_EQ(Canvases().CurrentCanvasId(), other);
    EXPECT_FALSE(App().IsOverviewOpen());
}

// A folder row shows that folder, and the Overview stays.
TEST_F(ViewLayerUiTest, AFolderRowShowsItsFolder) {
    ShowEditMode();
    StepFrame();
    const FolderId first = Canvases().CurrentFolderId();
    const FolderId second = Model().AddFolder("Second");
    Model().SwitchToFolder(first);
    StepFrame();
    ASSERT_EQ(Canvases().CurrentFolderId(), first);
    OpenOverviewUi();

    const std::string row = Under(second, "##folderrow");
    RunUi("click the folder", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick(row.c_str());
        ctx->Yield(2);
    });
    EXPECT_EQ(Canvases().CurrentFolderId(), second);
    EXPECT_TRUE(App().IsOverviewOpen());
}

// A folder row dropped on another takes its place.
TEST_F(ViewLayerUiTest, AFolderDraggedOntoAnotherMovesThere) {
    ShowEditMode();
    StepFrame();
    const FolderId a = Canvases().CurrentFolderId();
    const FolderId b = Model().AddFolder("B");
    const FolderId c = Model().AddFolder("C");
    StepFrame();
    ASSERT_EQ(FolderOrder(), (std::vector<FolderId>{a, b, c}));
    OpenOverviewUi();

    const std::string from = Under(c, "##folderrow");
    const std::string to = Under(a, "##folderrow");
    RunUi("drag the last folder onto the first", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemDragAndDrop(from.c_str(), to.c_str());
        ctx->Yield(2);
    });
    EXPECT_EQ(FolderOrder(), (std::vector<FolderId>{c, a, b}));
}

// A tile dropped on another in the grid takes its place; one dropped on a
// folder row goes to that folder.
TEST_F(ViewLayerUiTest, ATileDraggedInTheGridMovesInItsFolderOrToAnother) {
    ShowEditMode();
    StepFrame();
    const FolderId folder = Canvases().CurrentFolderId();
    const CanvasId a = Canvases().CurrentCanvasId();
    const CanvasId b = Model().AddCanvas("B");
    const CanvasId c = Model().AddCanvas("C");
    const FolderId other = Model().AddFolder("Other");
    Model().SwitchToFolder(folder);
    StepFrame();
    ASSERT_EQ(CanvasesOf(folder), (std::vector<CanvasId>{a, b, c}));
    OpenOverviewUi();

    const std::string tileA = Under(a, "##tile");
    const std::string tileC = Under(c, "##tile");
    RunUi("drag the last tile onto the first", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemDragAndDrop(tileC.c_str(), tileA.c_str());
        ctx->Yield(2);
    });
    EXPECT_EQ(CanvasesOf(folder), (std::vector<CanvasId>{c, a, b}));

    const std::string tileB = Under(b, "##tile");
    const std::string otherRow = Under(other, "##folderrow");
    RunUi("drag a tile onto the other folder", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemDragAndDrop(tileB.c_str(), otherRow.c_str());
        ctx->Yield(2);
    });
    EXPECT_EQ(Canvases().FindCanvas(b)->folderId, other);
    EXPECT_EQ(CanvasesOf(folder), (std::vector<CanvasId>{c, a}));
}

// A double-click on a folder's name or a canvas's opens it for typing, and
// what is typed is its name once the field lets go.
TEST_F(ViewLayerUiTest, AFolderAndACanvasAreRenamedWhereTheyAre) {
    ShowEditMode();
    StepFrame();
    const FolderId folder = Canvases().CurrentFolderId();
    const CanvasId canvas = Canvases().CurrentCanvasId();
    OpenOverviewUi();

    const std::string row = Under(folder, "##folderrow");
    const std::string folderField = Under(folder, "##renamefolder");
    RunUi("rename the folder", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemDoubleClick(row.c_str());
        ctx->Yield(2);
        ctx->ItemInputValue(folderField.c_str(), "Renamed folder");
        ctx->Yield(2);
    });
    EXPECT_EQ(Canvases().FindFolder(folder)->name, "Renamed folder");

    // The name is plain text under the tile, with no id of its own: it is
    // double-clicked where it is drawn.
    const std::string tile = Under(canvas, "##tile");
    const std::string canvasField = Under(canvas, "##renamecanvas");
    RunUi("rename the canvas", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        const ImRect rect = ctx->ItemInfo(tile.c_str()).RectFull;
        ctx->MouseMoveToPos(ImVec2(rect.Min.x + 12.0f,
                                   rect.Max.y + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight() * 0.5f));
        ctx->MouseDoubleClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
        ctx->ItemInputValue(canvasField.c_str(), "Renamed canvas");
        ctx->Yield(2);
    });
    EXPECT_EQ(Canvases().FindCanvas(canvas)->name, "Renamed canvas");
    EXPECT_TRUE(App().IsOverviewOpen());
}

// New canvas while picking where a snippet goes makes the canvas and sends
// the snippet there, and does not switch to it.
TEST_F(ViewLayerUiTest, NewCanvasWhilePickingSendsTheSnippetThere) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);  // a fullscreen screenshot
    ASSERT_EQ(ItemCountOnCurrentCanvas(), 1u);
    const ItemId snippet = Canvases().CurrentOrNull()->items[0].id;
    const CanvasId from = Canvases().CurrentCanvasId();
    Model().AddCanvas("Other");  // so that there is a canvas to move to
    StepFrame();
    PressKey(ImGuiKey_Escape);
    RightClick(640.0f, 400.0f);
    ASSERT_TRUE(App().IsItemContextMenuOpen());
    RunUi("move to canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("##menu_move_to_canvas");
        ctx->Yield(2);
    });
    ASSERT_TRUE(App().IsOverviewOpen());
    const size_t canvasesBefore = Canvases().Canvases().size();

    RunUi("new canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##newcanvas");
        ctx->Yield(2);
    });
    ASSERT_EQ(Canvases().Canvases().size(), canvasesBefore + 1);
    const CanvasId made = Canvases().Canvases().back().id;
    EXPECT_EQ(Model().CanvasHoldingItem(snippet), std::optional<CanvasId>(made));
    EXPECT_EQ(Canvases().CurrentCanvasId(), from) << "not switched to";
    EXPECT_TRUE(App().IsOverviewOpen());
}

// The canvas bar's "+" and Overview buttons are the NewCanvas and Overview
// commands, and are counted as commands run.
TEST_F(ViewLayerUiTest, TheCanvasBarsButtonsRunTheirCommands) {
    ShowEditMode();
    StepFrame();
    const CanvasId first = Canvases().CurrentCanvasId();
    const uint64_t before = App().CommandsRun();
    RunUi("the bar's +", [this](ImGuiTestContext* ctx) {
        ctx->MouseMoveToPos(ImVec2(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f));
        ctx->Yield(30);
        ctx->SetRef("//##canvas_bar");
        ctx->ItemClick("##canvasbar_new");
        ctx->Yield(2);
    });
    EXPECT_EQ(App().CommandsRun(), before + 1);
    EXPECT_EQ(App().LastCommand(), std::optional<CommandId>(CommandId::NewCanvas));
    EXPECT_NE(Canvases().CurrentCanvasId(), first);

    RunUi("the bar's Overview", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##canvas_bar");
        ctx->ItemClick("##canvasbar_overview");
        ctx->Yield(2);
    });
    EXPECT_EQ(App().CommandsRun(), before + 2);
    EXPECT_EQ(App().LastCommand(), std::optional<CommandId>(CommandId::Overview));
    EXPECT_TRUE(App().IsOverviewOpen());
}

// A dock chip brings its snippet back.
TEST_F(ViewLayerUiTest, ADockChipRestoresItsSnippet) {
    ShowEditMode();
    StepFrame();
    Drag(300.0f, 300.0f, 600.0f, 500.0f);
    PressKey(ImGuiKey_Escape);
    const ItemId snippet = Canvases().CurrentOrNull()->items[0].id;
    controller_->GetSession().SetMinimized({snippet}, true);
    StepFrame();

    const std::string chip = "##dockchip" + std::to_string(snippet);
    RunUi("click the chip", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##dock");
        ctx->ItemClick(chip.c_str());
        ctx->Yield(2);
    });
    EXPECT_FALSE(Canvases().FindItemAnywhere(snippet)->minimized);
}

// A click on the dimmed screen around the Overview closes it.
TEST_F(ViewLayerUiTest, TheBackdropClosesTheOverview) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("click the backdrop", [](ImGuiTestContext* ctx) {
        ctx->MouseMoveToPos(ImVec2(4.0f, 4.0f));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
    });
    EXPECT_FALSE(App().IsOverviewOpen());
}

// ===== What closing a popup does (section 4) =====

// A row chosen: the menu is gone, and so is the snippet it was about.
TEST_F(ViewLayerUiTest, ChoosingASnippetMenuRowForgetsTheSnippet) {
    ShowEditMode();
    StepFrame();
    DoubleClick(640.0f, 400.0f);
    PressKey(ImGuiKey_Escape);
    RightClick(640.0f, 400.0f);
    ASSERT_TRUE(App().ItemContextMenuItem().has_value());
    RunUi("duplicate", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("##menu_duplicate");
        ctx->Yield(2);
    });
    EXPECT_FALSE(App().IsItemContextMenuOpen());
    EXPECT_FALSE(App().ItemContextMenuItem().has_value());
}

// The canvas tile menu, closed by Escape, forgets its canvas.
TEST_F(ViewLayerUiTest, TheCanvasTileMenuForgetsItsCanvasWhenItCloses) {
    ShowEditMode();
    StepFrame();
    RunUi("right-click a tile", [this](ImGuiTestContext* ctx) {
        ctx->MouseMoveToPos(ImVec2(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f));
        ctx->Yield(30);
        ctx->SetRef("//##canvas_bar");
        ctx->ItemClick("##canvasbar_tile_0", ImGuiMouseButton_Right);
        ctx->Yield(2);
    });
    ASSERT_TRUE(App().IsCanvasContextMenuOpen());
    ASSERT_TRUE(App().CanvasContextMenuCanvas().has_value());

    PressKey(ImGuiKey_Escape);
    StepFrame();
    EXPECT_FALSE(App().IsCanvasContextMenuOpen());
    EXPECT_FALSE(App().CanvasContextMenuCanvas().has_value());
}

// The delete confirmation's Cancel deletes nothing, and leaves the Overview
// it was asked from up.
TEST_F(ViewLayerUiTest, TheDeleteConfirmationsCancelDeletesNothing) {
    ShowEditMode();
    StepFrame();
    const CanvasId canvas = Canvases().CurrentCanvasId();
    OpenOverviewUi();
    const std::string cancel = std::string("**/###") + strings::kMoveCopyCancel;
    RunUi("delete, then cancel", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##delcanvas");
        ctx->Yield(2);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick(cancel.c_str());
        ctx->Yield(2);
    });
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindCanvas(canvas)));
    EXPECT_EQ(App().InputStack(), "Canvas / - / Overview / - / - / -");
}

}  // namespace
}  // namespace sz::test
