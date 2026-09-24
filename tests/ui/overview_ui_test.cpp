// The Overview's canvas list, driven by widget name.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "fakes/ui_test.h"

namespace sz::test {
namespace {


// Where a window sits in ImGui's own display order - the list is front-to-
// back with the last entry on top, so "in front of" is ">".
int DisplayIndexOf(const ImGuiWindow* window) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.Windows.Size; ++i) {
        if (g.Windows[i] == window) {
            return i;
        }
    }
    return -1;
}

// What a color picker's z-order looks like from inside a running frame -
// sampled there rather than after the test ends, since the engine tidies up
// behind itself and an open popup does not survive that.
struct PickerOrder {
    int popupsOpen = 0;
    int pickerIndex = -1;
    int panelIndex = -1;
};

PickerOrder SamplePickerOrder() {
    PickerOrder order;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    order.popupsOpen = g.OpenPopupStack.Size;
    if (order.popupsOpen > 0) {
        order.pickerIndex = DisplayIndexOf(g.OpenPopupStack.back().Window);
    }
    order.panelIndex = DisplayIndexOf(ImGui::FindWindowByName("##overview_panel"));
    return order;
}

// A color swatch in the Settings panel opens ImGui's own picker, which -
// unlike every popover this app opens itself - has no Begin/End pair of
// ours to hang a per-frame "stay in front" on. The panel reasserts itself
// to the front every frame, so without help the picker is buried the frame
// after it opens: visible for one frame, then gone, with the clicks that
// follow landing on the panel. See OverlayApp::KeepChildPopupsInFront.
void ExpectPickerInFrontOfThePanel(const PickerOrder& order) {
    ASSERT_GT(order.popupsOpen, 0) << "the color picker never opened";
    ASSERT_GE(order.pickerIndex, 0);
    ASSERT_GE(order.panelIndex, 0);
    EXPECT_GT(order.pickerIndex, order.panelIndex) << "the color picker is behind the panel that opened it";
}

// The canvases a folder shows - a deleted one stays in Canvases(), hidden.
size_t CanvasesInFolder(const CanvasManager& canvases, FolderId folderId) {
    size_t count = 0;
    for (const Canvas& canvas : canvases.Canvases()) {
        if (canvas.folderId == folderId && !canvases.IsDeleted(canvas)) {
            ++count;
        }
    }
    return count;
}

// A rename field borrows the keyboard from the game and gives it back when
// ImGui deactivates the field. Switching to view mode closes the Overview
// from outside the frame, so the field is never rendered again and that
// deactivation never comes - the keyboard was left borrowed for the rest
// of the session. See OverlayApp::CloseOverview.
TEST_F(UiTest, SwitchingModeWhileRenamingGivesTheKeyboardBack) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    const int requestsBefore = host_.overlayWindow.requestTextInputCallCount;
    const int releasesBefore = host_.overlayWindow.releaseTextInputCallCount;

    RunUi("rename then switch mode", [this, requestsBefore](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemDoubleClick("**/##folderrow");
        ctx->Yield();
        IM_CHECK_EQ(host_.overlayWindow.requestTextInputCallCount, requestsBefore + 1);
        ShowViewMode();
        ctx->Yield();
        IM_CHECK(!App().IsOverviewOpen());
    });

    EXPECT_FALSE(App().IsOverviewOpen());
    EXPECT_EQ(host_.overlayWindow.requestTextInputCallCount, requestsBefore + 1);
    EXPECT_EQ(host_.overlayWindow.releaseTextInputCallCount, releasesBefore + 1) << "the keyboard stayed borrowed";
}

// Deleting the canvas you are working on stays in the folder it lived in
// rather than handing you whichever canvas is first in the whole library -
// in practice a folder you had not been in all session. When that was its
// last canvas the folder is simply empty: nothing is created to fill it, and
// nothing else becomes current.
TEST_F(UiTest, DeletingAFoldersOnlyCanvasLeavesTheFolderEmpty) {
    ShowEditMode();
    StepFrame();
    const FolderId firstFolder = Canvases().CurrentFolderId();
    OpenOverviewUi();

    // A second folder, which "New folder" creates with one canvas in it and
    // switches to - so the canvas about to be deleted is both the current
    // one and the only one in its folder, with another folder's canvas
    // sitting there as the thing a global fallback would jump to.
    RunUi("new folder", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##newfolder");
    });
    const FolderId workingFolder = Canvases().CurrentFolderId();
    ASSERT_NE(workingFolder, firstFolder);
    ASSERT_EQ(CanvasesInFolder(Canvases(), workingFolder), 1u);
    const CanvasId doomed = Canvases().CurrentOrNull()->id;

    RunUi("delete the canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##delcanvas");
        // The confirmation draws its own label, so the button is addressed
        // by its id rather than by "Delete" - see DangerButton.
        ctx->ItemClick("//$FOCUSED/##confirmdelete");
    });

    EXPECT_FALSE(Canvases().HasCurrentCanvas()) << "no canvas anywhere else may be adopted";
    EXPECT_EQ(Canvases().CurrentFolderId(), workingFolder) << "and the Overview stays where the user was";
    EXPECT_EQ(CanvasesInFolder(Canvases(), workingFolder), 0u);
    const Canvas* deleted = Canvases().FindCanvas(doomed);
    ASSERT_NE(deleted, nullptr) << "deleted in place, to be restored";
    EXPECT_NE(deleted->deletedAt, 0);
}

// "$$n" is how the engine spells a PushID(n) - every sidebar row and grid
// tile is under its folder's or canvas's own.
std::string Under(uint64_t id, const char* item) { return "**/$$" + std::to_string(id) + "/" + item; }

// With Show deleted on, a deleted canvas is shown where it was, with a
// Restore of its own that clears its mark.
TEST_F(UiTest, ShowDeletedShowsADeletedCanvasWhereItWasAndRestoreBringsItBack) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("new folder", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##newfolder");
    });
    const CanvasId doomed = Canvases().CurrentOrNull()->id;
    const FolderId folder = Canvases().CurrentFolderId();

    RunUi("delete the canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##delcanvas");
        ctx->ItemClick("//$FOCUSED/##confirmdelete");
    });
    EXPECT_EQ(CanvasesInFolder(Canvases(), folder), 0u) << "hidden";
    ASSERT_NE(Canvases().FindCanvas(doomed), nullptr);
    EXPECT_NE(Canvases().FindCanvas(doomed)->deletedAt, 0);

    const std::string restore = Under(doomed, "##restore");
    RunUi("show deleted, and restore it", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###showdeleted");
        ctx->ItemClick(restore.c_str());
    });
    EXPECT_EQ(Canvases().FindCanvas(doomed)->deletedAt, 0);
    EXPECT_EQ(Canvases().FindCanvas(doomed)->folderId, folder);
    EXPECT_EQ(CanvasesInFolder(Canvases(), folder), 1u);
    EXPECT_EQ(Canvases().DeletedFolderAndCanvasCount(), 0u);
}

// Its other button deletes it for good, after asking.
TEST_F(UiTest, DeletingPermanentlyWithShowDeletedErasesIt) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("new folder", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##newfolder");
    });
    const CanvasId doomed = Canvases().CurrentOrNull()->id;

    RunUi("delete the canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##delcanvas");
        ctx->ItemClick("//$FOCUSED/##confirmdelete");
    });
    ASSERT_NE(Canvases().FindCanvas(doomed), nullptr);

    const std::string deleteForGood = Under(doomed, "##deleteforgood");
    RunUi("show deleted, and delete it for good", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###showdeleted");
        ctx->ItemClick(deleteForGood.c_str());
        ctx->ItemClick("//$FOCUSED/##confirmdelete");
    });
    EXPECT_EQ(Canvases().FindCanvas(doomed), nullptr);
    EXPECT_EQ(Canvases().DeletedFolderAndCanvasCount(), 0u);
}

// With both confirmations switched off in Settings > Behavior, each press
// does what it says at once: the delete marks it, and the second deletes it
// for good.
TEST_F(UiTest, WithoutConfirmationsEachDeleteHappensOnThePress) {
    controller_->GetSettings().Mutable().confirmDelete = false;
    controller_->GetSettings().Mutable().confirmDeleteForGood = false;
    controller_->GetSettings().Commit();
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("new folder", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##newfolder");
    });
    const CanvasId doomed = Canvases().CurrentOrNull()->id;

    RunUi("delete the canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##delcanvas");
    });
    ASSERT_NE(Canvases().FindCanvas(doomed), nullptr);
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindCanvas(doomed))) << "no dialog in between";

    const std::string deleteForGood = Under(doomed, "##deleteforgood");
    RunUi("show deleted, and delete it for good", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###showdeleted");
        ctx->ItemClick(deleteForGood.c_str());
    });
    EXPECT_EQ(Canvases().FindCanvas(doomed), nullptr);
}

// A new folder holding two canvases, the second current - what the tests
// below delete bits of.
struct TwoCanvasFolder {
    FolderId folder = 0;
    CanvasId first = 0;
    CanvasId second = 0;
};

class ShowDeletedUiTest : public UiTest {
protected:
    TwoCanvasFolder MakeTwoCanvasFolder() {
        RunUi("new folder and a second canvas", [](ImGuiTestContext* ctx) {
            ctx->SetRef("//##overview_panel");
            ctx->ItemClick("**/##newfolder");
            ctx->ItemClick("**/##newcanvas");
        });
        TwoCanvasFolder made;
        made.folder = Canvases().CurrentFolderId();
        made.second = Canvases().CurrentOrNull()->id;
        for (const Canvas& canvas : Canvases().Canvases()) {
            if (canvas.folderId == made.folder && canvas.id != made.second) {
                made.first = canvas.id;
            }
        }
        return made;
    }
};

// A deleted folder is looked into from the sidebar, and one canvas can be
// restored out of it: the folder comes back to hold it, and the canvas that
// went with the folder stays deleted, as of when the folder went.
TEST_F(ShowDeletedUiTest, RestoringOneCanvasOfADeletedFolderLeavesTheOtherDeleted) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    const TwoCanvasFolder made = MakeTwoCanvasFolder();
    ASSERT_NE(made.first, 0u);
    ASSERT_TRUE(controller_->GetSession().Delete(made.folder));
    const int64_t folderStamp = Canvases().FindFolder(made.folder)->deletedAt;
    ASSERT_NE(Canvases().CurrentFolderId(), made.folder) << "a deleted folder is not browsed";

    const std::string folderRow = Under(made.folder, "##folderrow");
    const std::string restoreFirst = Under(made.first, "##restore");
    RunUi("show deleted, open the folder, restore one canvas", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###showdeleted");
        ctx->ItemClick(folderRow.c_str());
        ctx->ItemClick(restoreFirst.c_str());
    });
    EXPECT_EQ(Canvases().FindFolder(made.folder)->deletedAt, 0);
    EXPECT_EQ(Canvases().FindCanvas(made.first)->deletedAt, 0);
    EXPECT_EQ(Canvases().FindCanvas(made.second)->deletedAt, folderStamp);
    EXPECT_EQ(Canvases().CurrentFolderId(), made.folder) << "still the folder on show, browsed now it is back";
}

// A folder's Restore brings back every deleted canvas in it, and the folder
// itself if it was deleted.
TEST_F(ShowDeletedUiTest, AFoldersRestoreBringsBackAllOfIt) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    const TwoCanvasFolder made = MakeTwoCanvasFolder();
    ASSERT_TRUE(controller_->GetSession().Delete(made.first));
    ASSERT_TRUE(controller_->GetSession().Delete(made.folder));

    const std::string restoreFolder = Under(made.folder, "##restore");
    RunUi("show deleted, restore the folder", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###showdeleted");
        ctx->ItemClick(restoreFolder.c_str());
    });
    EXPECT_EQ(Canvases().FindFolder(made.folder)->deletedAt, 0);
    EXPECT_EQ(CanvasesInFolder(Canvases(), made.folder), 2u) << "the canvas deleted on its own before too";
}

// A folder that is not deleted but holds a deleted canvas: its Delete
// permanently erases what is deleted in it, and nothing else.
TEST_F(ShowDeletedUiTest, AFoldersDeletePermanentlyErasesOnlyItsDeletedCanvases) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    const TwoCanvasFolder made = MakeTwoCanvasFolder();
    ASSERT_TRUE(controller_->GetSession().Delete(made.first));

    const std::string deleteInFolder = Under(made.folder, "##deleteforgood");
    RunUi("show deleted, delete the folder's deleted canvases", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###showdeleted");
        ctx->ItemClick(deleteInFolder.c_str());
        ctx->ItemClick("//$FOCUSED/##confirmdelete");
    });
    EXPECT_EQ(Canvases().FindCanvas(made.first), nullptr);
    ASSERT_NE(Canvases().FindFolder(made.folder), nullptr);
    EXPECT_EQ(Canvases().FindFolder(made.folder)->deletedAt, 0);
    EXPECT_NE(Canvases().FindCanvas(made.second), nullptr);
}

// The same delete with a neighbor to fall back on: no canvas is created,
// and the one *before* it in the folder takes over.
TEST_F(UiTest, DeletingTheCurrentCanvasFallsBackToItsNeighborInTheFolder) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("new folder and a second canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##newfolder");
        ctx->ItemClick("**/##newcanvas");
    });
    const FolderId workingFolder = Canvases().CurrentFolderId();
    ASSERT_EQ(CanvasesInFolder(Canvases(), workingFolder), 2u);
    // "New canvas" switches to what it made, which is the second one - the
    // new canvas goes to the end of its folder.
    const CanvasId second = Canvases().CurrentOrNull()->id;
    CanvasId first = 0;
    for (const Canvas& canvas : Canvases().Canvases()) {
        if (canvas.folderId == workingFolder && canvas.id != second) {
            first = canvas.id;
        }
    }
    ASSERT_NE(first, 0u);

    // Two tiles now, so the trash button is addressed under its own tile's
    // PushID(canvas id) - "$$n" is how the engine spells one of those.
    const std::string secondsDeleteButton = "**/$$" + std::to_string(second) + "/##delcanvas";
    RunUi("delete the second canvas", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick(secondsDeleteButton.c_str());
        ctx->ItemClick("//$FOCUSED/##confirmdelete");
    });

    ASSERT_TRUE(Canvases().HasCurrentCanvas());
    EXPECT_EQ(Canvases().CurrentOrNull()->id, first);
    EXPECT_EQ(CanvasesInFolder(Canvases(), workingFolder), 1u);
}

TEST_F(UiTest, TheEditModeBorderColorPickerStaysInFrontOfThePanel) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    PickerOrder order;
    RunUi("open the edit-mode border picker", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##editbordercolor/##ColorButton");
        // Two frames, because the burial happens on the frame *after* the
        // one the picker opens on: that is when the panel reasserts itself.
        ctx->Yield();
        ctx->Yield();
        order = SamplePickerOrder();
    });
    ExpectPickerInFrontOfThePanel(order);
}

TEST_F(UiTest, TheSnippetColorPickersStayInFrontOfThePanel) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    PickerOrder order;
    RunUi("open a snippet color picker", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##snipcolfrontborder/##ColorButton");
        ctx->Yield();
        ctx->Yield();
        order = SamplePickerOrder();
    });
    ExpectPickerInFrontOfThePanel(order);
}

// A color swatch reports a change on every frame its picker is dragged, so
// committing on that return value wrote the whole of config.json to disk once
// per frame for as long as the drag lasted - and not atomically, which is a
// poor thing to be doing sixty times a second. It commits when the edit
// finishes instead, exactly as the sliders beside it always have.
//
// The risk in that change is the opposite failure: a ColorEdit4 whose value
// is changed from inside its own popup is not obviously "deactivated after
// edit" at all, and if that never fires the color would stop being saved.
// So this drives the picker for real and then reads the file back.
class ColorPickerPersistenceTest : public UiTest {
protected:
    void SetUp() override {
        UiTest::SetUp();
        configFile_ = std::filesystem::temp_directory_path() /
                       ("sz_color_picker_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                        ".json");
        std::filesystem::remove(configFile_);
        host_.configFilePath = configFile_;
        // With the new-text size already decided, as it is after any first
        // run: deciding it is a settings change of its own, saved on the
        // first frame, and this is about the one the picker makes.
        AppConfig config = DefaultConfig();
        config.noteTextSizePx = kDefaultNoteTextSizePx;
        StartWith(config);
    }

    void TearDown() override {
        UiTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(configFile_, ec);
    }

    std::string ConfigFileContents() const {
        std::ifstream in(configFile_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::filesystem::path configFile_;
};

TEST_F(ColorPickerPersistenceTest, ASnippetColorSurvivesToDiskWhenTheDragEnds) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();

    const uint32_t before = AppSettings().Stored().itemBorderColorFrontRGBA;
    EXPECT_FALSE(std::filesystem::exists(configFile_)) << "nothing should have been written yet";

    uint32_t duringDrag = before;
    bool wroteDuringDrag = true;
    RunUi("drag the frontmost-border color", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##snipcolfrontborder/##ColorButton");
        ctx->Yield();
        ctx->Yield();
        // Inside ImGui's own picker popup: the saturation/value square.
        // Driven by hand rather than with ItemDragWithDelta because the
        // interesting moment is *mid*-drag, with the button still held -
        // and that helper has already released it by the time it returns.
        ctx->SetRef("//$FOCUSED");
        ctx->MouseMove("**/##picker/sv");
        const ImVec2 start = ImGui::GetIO().MousePos;
        ctx->MouseDown(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(ImVec2(start.x - 40.0f, start.y + 40.0f));
        ctx->Yield();
        // The value has changed and the button is still down: the state a
        // commit-on-change swatch writes the whole of config.json in, once
        // per frame, for as long as the user keeps dragging.
        duringDrag = AppSettings().Stored().itemBorderColorFrontRGBA;
        wroteDuringDrag = std::filesystem::exists(configFile_);
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield();
    });

    EXPECT_NE(duringDrag, before) << "the drag did not change the color, so this test proves nothing";
    EXPECT_FALSE(wroteDuringDrag) << "config.json was written while the picker was still being dragged";

    const uint32_t after = AppSettings().Stored().itemBorderColorFrontRGBA;
    ASSERT_NE(after, before) << "the drag did not change the color, so this test proves nothing";
    ASSERT_TRUE(std::filesystem::exists(configFile_))
        << "the finished edit never reached disk - IsItemDeactivatedAfterEdit does not fire for this widget";

    char expected[16];
    std::snprintf(expected, sizeof(expected), "#%08X", after);
    EXPECT_NE(ConfigFileContents().find(expected), std::string::npos)
        << "config.json does not hold the color that was picked";
}

}  // namespace
}  // namespace sz::test
