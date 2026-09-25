#include "core/session/session.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/persistence/library_store.h"
#include "fakes/fake_platform_host.h"
#include "support/held_library.h"
#include "support/removed_at_end.h"
#include "support/session_test_access.h"

namespace sz::core {
namespace {

using test::HeldLibrary;
using test::Model;
using test::RemovedAtEnd;

// The session with nothing attached - no window, no stores - is still a
// whole model of what the app is working on: that is what lets a UI, or a
// test, drive it without either.

const Item* ItemById(const CanvasManager& manager, ItemId id) {
    for (const Canvas& canvas : manager.Canvases()) {
        for (const Item& item : canvas.items) {
            if (item.id == id) {
                return &item;
            }
        }
    }
    return nullptr;
}

// Draws a two-point stroke across `item` on the live layer and commits it,
// the way the pen does.
void DrawStrokeInto(Session& session, ItemId item) {
    session.LiveLayer().BeginStroke(StrokePoint{10.0f, 10.0f}, 0xFF0000FFu, 3.0f);
    session.LiveLayer().ExtendStroke(StrokePoint{60.0f, 60.0f});
    session.LiveLayer().EndStroke();
    session.CommitLiveStroke(item);
}

// A horizontal stroke at height `y` across `item`, for the tests about
// which stroke is which: an item made at {0,0,100,100} has native space
// equal to screen space, so the y comes back out unchanged.
void DrawLineInto(Session& session, ItemId item, float y) {
    session.LiveLayer().BeginStroke(StrokePoint{10.0f, y}, 0xFF0000FFu, 3.0f);
    session.LiveLayer().ExtendStroke(StrokePoint{90.0f, y});
    session.LiveLayer().EndStroke();
    session.CommitLiveStroke(item);
}

// The height of every stroke, in list order - the draw order, which is what
// the history has to keep.
std::vector<float> StrokeHeights(const CanvasManager& manager, ItemId id) {
    std::vector<float> heights;
    for (const Stroke& stroke : ItemById(manager, id)->strokes) {
        heights.push_back(stroke.points.front().y);
    }
    return heights;
}

TEST(SessionTest, StartsWithNothingDeleted) {
    Session session;
    EXPECT_EQ(Model(session).DeletedFolderAndCanvasCount(), 0u);
    EXPECT_TRUE(Model(session).MarkedSnippets().empty());
    EXPECT_TRUE(Model(session).HasCurrentCanvas());
    EXPECT_FALSE(session.CanUndo());
}

TEST(SessionTest, ADeletedCanvasStaysWhereItIsAndComesBack) {
    Session session;
    const CanvasId canvas = Model(session).CurrentCanvasId();
    const FolderId folder = Model(session).CurrentFolderId();

    ASSERT_TRUE(session.Delete(canvas));
    const Canvas* deleted = Model(session).FindCanvas(canvas);
    ASSERT_NE(deleted, nullptr) << "marked, not moved anywhere";
    EXPECT_NE(deleted->deletedAt, 0);
    EXPECT_EQ(deleted->folderId, folder);
    EXPECT_FALSE(Model(session).HasCurrentCanvas()) << "the only canvas in its folder, and hidden now";
    EXPECT_EQ(Model(session).CurrentFolderId(), folder) << "still browsing where it was";

    ASSERT_TRUE(session.Restore(canvas));
    EXPECT_EQ(Model(session).FindCanvas(canvas)->deletedAt, 0);
}

TEST(SessionTest, DeletingOrRestoringWhatIsNotThereDoesNothing) {
    Session session;
    EXPECT_FALSE(session.Delete(424242));
    EXPECT_FALSE(session.Restore(424242));
    EXPECT_FALSE(session.DeletePermanently(424242));
    EXPECT_FALSE(session.Restore(Model(session).CurrentCanvasId())) << "nothing deleted to restore";
}

// Undo is the only way a deleted snippet comes back, and no history
// outlives its session - so a library opened again has no use for one.
TEST(SessionTest, ALibraryOpenedErasesTheSnippetsMarkedDeleted) {
    CanvasManager source("First");
    const CanvasId first = source.CurrentCanvasId();
    const ItemId gone = source.CreateItem(false, Rect{0, 0, 100, 100}, "Gone");
    const ItemId kept = source.CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    const ItemId inDeletedCanvas = source.CreateItem(false, Rect{0, 0, 100, 100}, "InDeletedCanvas");
    const ItemId wentWithIt = source.CreateItem(false, Rect{0, 0, 100, 100}, "WentWithIt");
    ASSERT_TRUE(source.MarkDeleted(gone, 1));
    ASSERT_TRUE(source.MarkDeleted(inDeletedCanvas, 2));
    const CanvasId second = source.AddCanvas("Second");
    ASSERT_TRUE(source.MarkDeleted(first, 3));

    Session session;
    session.ImportLibrary(source.ExportSnapshot());
    CanvasManager& manager = Model(session);
    EXPECT_EQ(manager.FindItemAnywhere(gone), nullptr);
    EXPECT_EQ(manager.FindItemAnywhere(inDeletedCanvas), nullptr) << "restoring its canvas would not bring it back";
    EXPECT_NE(manager.FindItemAnywhere(kept), nullptr);
    EXPECT_NE(manager.FindItemAnywhere(wentWithIt), nullptr) << "comes back with its canvas";
    ASSERT_NE(manager.FindCanvas(first), nullptr) << "a deleted canvas is kept to be restored";
    EXPECT_NE(manager.FindCanvas(second), nullptr);
}

// The erasing happens at startup, before the overlay window - and with it
// anything to make a texture with - exists. It must not count as the
// current canvas's textures having been loaded: they could not have been,
// and nothing would try again until the canvas changed.
TEST(SessionTest, ErasingDeletedSnippetsOnOpenLeavesTheCanvasToLoadItsPicturesLater) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_erase_on_open_textures";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    ItemId shot = 0;
    {
        persistence::LibraryStore store(dir / "library.db");
        test::FakeOverlayWindow window;
        window.captureReturnsHandle = 7;
        window.captureReturnsWidth = 1;
        window.captureReturnsHeight = 1;
        window.captureReturnsPixelsRGBA = {10, 20, 30, 255};
        Session session;
        session.AttachWindow(&window);
        session.SetLibraryStore(&store);
        shot = session.CreateItem(true, Rect{0.0f, 0.0f, 1.0f, 1.0f}, "Shot");
        const ItemId gone = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Gone");
        ASSERT_TRUE(session.Delete(gone));
        ASSERT_TRUE(session.Flush());
        session.SetLibraryStore(nullptr);
    }

    persistence::LibraryStore store(dir / "library.db");
    std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    test::FakeOverlayWindow window;
    window.createTextureFromPixelsReturnsHandle = 0;  // no device yet
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    session.ImportLibrary(std::move(*loaded));
    ASSERT_TRUE(Model(session).MarkedSnippets().empty()) << "the deleted snippet was erased";

    window.createTextureFromPixelsReturnsHandle = 9;  // the window is made, and the first frame runs
    session.EnsureTexturesForCurrentCanvas();
    const Item* item = Model(session).FindItemAnywhere(shot);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->picture.textureHandle, 9u) << "not a placeholder until the canvas is switched";

    session.SetLibraryStore(nullptr);
}

// The retention period: what has been deleted since before the cutoff goes
// for good, whatever else is deleted stays, and nothing live is touched.
TEST(SessionTest, ErasingWhatWasDeletedBeforeACutoffLeavesTheRest) {
    Session session;
    CanvasManager& manager = Model(session);
    const FolderId home = manager.CurrentFolderId();
    const CanvasId live = manager.CurrentCanvasId();
    const CanvasId old = manager.AddCanvas("Old");
    const CanvasId recent = manager.AddCanvas("Recent");
    const FolderId oldFolder = manager.AddFolder("Old folder");
    manager.SwitchToFolder(oldFolder);
    const CanvasId inOldFolder = manager.AddCanvas("In old folder");
    manager.SwitchToCanvas(live);
    ASSERT_TRUE(manager.MarkDeleted(old, 100));
    ASSERT_TRUE(manager.MarkDeleted(recent, 300));
    ASSERT_TRUE(manager.MarkDeleted(oldFolder, 150));

    EXPECT_EQ(session.EraseDeletedBefore(200), 2u) << "the old canvas, and the old folder with what is in it";
    EXPECT_EQ(manager.FindCanvas(old), nullptr);
    EXPECT_EQ(manager.FindFolder(oldFolder), nullptr);
    EXPECT_EQ(manager.FindCanvas(inOldFolder), nullptr);
    ASSERT_NE(manager.FindCanvas(recent), nullptr);
    EXPECT_NE(manager.FindCanvas(recent)->deletedAt, 0) << "still deleted, to be restored";
    EXPECT_NE(manager.FindCanvas(live), nullptr);
    EXPECT_NE(manager.FindFolder(home), nullptr);
    EXPECT_EQ(session.EraseDeletedBefore(200), 0u) << "nothing left that old";
}

TEST(SessionTest, ErasingAFoldersDeletedCanvasesLeavesTheFolderAndTheRest) {
    Session session;
    const FolderId folder = Model(session).CurrentFolderId();
    const CanvasId first = Model(session).CurrentCanvasId();
    const CanvasId second = Model(session).AddCanvas("Second");
    const CanvasId third = Model(session).AddCanvas("Third");
    EXPECT_FALSE(session.DeleteMarkedCanvasesPermanently(folder)) << "nothing deleted in it";
    ASSERT_TRUE(session.Delete(first));
    ASSERT_TRUE(session.Delete(third));

    EXPECT_TRUE(session.DeleteMarkedCanvasesPermanently(folder));
    EXPECT_EQ(Model(session).FindCanvas(first), nullptr);
    EXPECT_EQ(Model(session).FindCanvas(third), nullptr);
    ASSERT_NE(Model(session).FindFolder(folder), nullptr);
    EXPECT_NE(Model(session).FindCanvas(second), nullptr);
}

// A placement change is one entry for everything it moved, taken back and
// put back whole - fullscreen state and anchor with the rect - and nothing
// at all is filed for a change that changed nothing.
TEST(SessionTest, APlacementChangeIsUndoneAndRedoneWhole) {
    Session session;
    CanvasManager& manager = Model(session);
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = manager.CreateItem(false, Rect{200, 0, 100, 100}, "B");
    ASSERT_FALSE(session.CanUndo());

    session.SyncItemsToDisplaySize(1920.0f, 1080.0f);
    session.BeginPlacement({a, b});
    EXPECT_FALSE(session.EndPlacement()) << "nothing changed";
    EXPECT_FALSE(session.CanUndo());

    ASSERT_TRUE(session.SetRects({{a, Rect{10, 20, 100, 100}}, {b, Rect{210, 20, 100, 100}}}));
    session.ToggleFullscreen(a, /*stretch=*/true);

    const std::optional<Session::UndoStep> out = session.Undo();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->what, Session::UndoWhat::Placement);
    EXPECT_FALSE(manager.FindItemAnywhere(a)->isFullscreen);
    EXPECT_EQ(manager.FindItemAnywhere(a)->rect, (Rect{10, 20, 100, 100}));

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(manager.FindItemAnywhere(a)->rect, (Rect{0, 0, 100, 100})) << "both, in one step";
    EXPECT_EQ(manager.FindItemAnywhere(b)->rect, (Rect{200, 0, 100, 100}));

    ASSERT_TRUE(session.Redo().has_value());
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_TRUE(manager.FindItemAnywhere(a)->isFullscreen);
    EXPECT_EQ(manager.FindItemAnywhere(b)->rect, (Rect{210, 20, 100, 100}));
}

// A burst - wheel notches, arrow presses - folds into one entry that goes
// back to where the burst began; anything else filed in between ends it.
TEST(SessionTest, AMergedBurstOfPlacementChangesIsOneUndo) {
    Session session;
    CanvasManager& manager = Model(session);
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    for (int i = 1; i <= 3; ++i) {
        ASSERT_TRUE(session.SetRects({{a, Rect{static_cast<float>(i), 0, 100, 100}}}, /*merge=*/true));
    }
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.x, 0.0f) << "the whole burst, at once";
    EXPECT_FALSE(session.CanUndo());

    ASSERT_TRUE(session.Redo().has_value());
    const ItemId b = session.CreateItem(false, Rect{300, 0, 50, 50}, "B");  // something else filed
    ASSERT_NE(b, 0u);
    ASSERT_TRUE(session.SetRects({{a, Rect{9.0f, 0, 100, 100}}}, /*merge=*/true));
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.x, 3.0f) << "a new entry, not merged past the one between";
}

// A snippet that leaves the canvas takes its part of a group's move with
// it; the others' part stays to be taken back.
TEST(SessionTest, ForgettingOneSnippetLeavesTheRestOfAGroupMove) {
    Session session;
    CanvasManager& manager = Model(session);
    const CanvasId canvas = manager.CurrentCanvasId();
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = manager.CreateItem(false, Rect{200, 0, 100, 100}, "B");
    const CanvasId elsewhere = session.AddCanvas("Elsewhere");
    ASSERT_TRUE(session.SetRects({{a, Rect{0, 50, 100, 100}}, {b, Rect{200, 50, 100, 100}}}));

    ASSERT_EQ(session.SendItemsTo({a}, elsewhere, /*copy=*/false).items.size(), 1u);
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.y, 50.0f) << "forgotten";
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(b)->rect.y, 0.0f);

    ASSERT_EQ(session.SendItemsTo({b}, elsewhere, /*copy=*/false).items.size(), 1u);
    EXPECT_FALSE(session.CanRedo()) << "an entry naming nothing is gone";
    EXPECT_EQ(session.Manager().CurrentCanvasId(), canvas);
}

TEST(SessionTest, AStrokeIsUndoneAndRedone) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    ASSERT_EQ(ItemById(Model(session), item)->strokes.size(), 1u);
    ASSERT_TRUE(session.CanUndo());

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(undone->undone);
    EXPECT_TRUE(ItemById(Model(session), item)->strokes.empty());
    EXPECT_FALSE(session.CanUndo());
    ASSERT_TRUE(session.CanRedo());

    const std::optional<Session::UndoStep> redone = session.Redo();
    ASSERT_TRUE(redone.has_value());
    EXPECT_FALSE(redone->undone);
    EXPECT_EQ(ItemById(Model(session), item)->strokes.size(), 1u);
}

TEST(SessionTest, ADeletedSnippetIsRestoredOnUndo) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    ASSERT_TRUE(session.DeleteItem(item));
    ASSERT_NE(ItemById(Model(session), item), nullptr) << "marked where it is";
    EXPECT_TRUE(Model(session).IsItemDeleted(item));
    EXPECT_FALSE(session.DeleteItem(item)) << "a delete of something deleted is for good, and not this call";

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Delete);
    EXPECT_FALSE(Model(session).IsItemDeleted(item));

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_TRUE(Model(session).IsItemDeleted(item));
}

// One Delete is one undo, however many snippets it took - more than the
// history holds entries included, where one entry each left the earliest
// beyond reach, and a deleted snippet only comes back by undo.
TEST(SessionTest, DeletingManySnippetsAtOnceIsOneUndo) {
    Session session;
    std::vector<ItemId> items;
    for (int i = 0; i < 60; ++i) {
        items.push_back(Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A"));
    }
    EXPECT_EQ(session.DeleteItems(items), 60u);
    EXPECT_EQ(session.DeleteItems(items), 0u) << "deleted already";

    ASSERT_TRUE(session.Undo().has_value());
    for (const ItemId item : items) {
        EXPECT_FALSE(Model(session).IsItemDeleted(item));
    }
    EXPECT_FALSE(session.Undo().has_value()) << "one step, not sixty";

    ASSERT_TRUE(session.Redo().has_value());
    for (const ItemId item : items) {
        EXPECT_TRUE(Model(session).IsItemDeleted(item));
    }
}

TEST(SessionTest, AnOpenTextEditIsNotUndoneFromUnderIt) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    session.BeginTextEdit(item);
    session.EndTextEdit(std::string("first"));
    EXPECT_EQ(ItemById(Model(session), item)->noteText, "first");

    // Undone with the note open: nothing happens, and the entry is kept -
    // unlike one that no longer applies, it still does, once the note is
    // closed.
    session.BeginTextEdit(item);
    EXPECT_FALSE(session.Undo().has_value()) << "the edit in progress would overwrite it anyway";
    EXPECT_EQ(ItemById(Model(session), item)->noteText, "first");
    EXPECT_TRUE(session.CanUndo()) << "kept for when the note is closed";
    session.EndTextEdit(std::nullopt);  // abandoned: nothing filed

    // Undone with the note closed, it goes back.
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::TextEdit);
    EXPECT_EQ(ItemById(Model(session), item)->noteText, "");
    EXPECT_FALSE(session.CanUndo());
}

// An erase begun while another is still open ends that one first, filed
// whole: one undo puts back what each took.
TEST(SessionTest, AnEraseBegunOverAnOpenOneFilesThatOneFirst) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 20.0f);
    DrawLineInto(session, item, 80.0f);
    const std::vector<Stroke> drawn = ItemById(Model(session), item)->strokes;

    session.BeginErase(item, 50.0f, 20.0f, 10.0f);
    session.BeginErase(item, 50.0f, 80.0f, 10.0f);
    session.EndErase();

    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemById(Model(session), item)->strokes, drawn);
}

// The same for a rectangle erased while an erase is open: that one is
// filed first, whole, and ending it afterwards finds nothing left open.
TEST(SessionTest, ARectangleErasedOverAnOpenEraseFilesThatOneFirst) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 20.0f);
    DrawLineInto(session, item, 80.0f);
    const std::vector<Stroke> drawn = ItemById(Model(session), item)->strokes;

    session.BeginErase(item, 50.0f, 20.0f, 10.0f);
    session.EraseRect(item, 0.0f, 70.0f, 100.0f, 90.0f);
    session.EndErase();

    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemById(Model(session), item)->strokes, drawn);
}

// An eraser dragged over nothing but empty space changes nothing, and
// files nothing: the next undo takes back what came before it.
TEST(SessionTest, AnErasePassThatChangesNothingIsNoStep) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 10.0f);

    session.BeginErase(item, 80.0f, 80.0f, 10.0f);
    session.ExtendErase(90.0f, 60.0f, 10.0f);
    session.EndErase();

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke) << "the stroke, not an erase of nothing";
    EXPECT_FALSE(session.CanUndo());
}

TEST(SessionTest, ClearingADrawingIsOneStep) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    DrawStrokeInto(session, item);
    ASSERT_TRUE(session.ClearDrawing(item));
    EXPECT_TRUE(ItemById(Model(session), item)->strokes.empty());
    EXPECT_FALSE(session.ClearDrawing(item)) << "nothing left to clear";

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Erase);
    EXPECT_EQ(ItemById(Model(session), item)->strokes.size(), 2u);
}

// ===== Erase history keeps the draw order =====

TEST(SessionTest, UndoingAnEraseRestoresTheStrokeWhereItWas) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    session.EraseRect(item, 0.0f, 20.0f, 100.0f, 40.0f);  // the first stroke, wholly
    ASSERT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{100.0f}));

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 100.0f}))
        << "back where it was, not at the end";

    // The next undo is of the second stroke, and takes off the second
    // stroke - not whichever one happens to be last.
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f}));

    ASSERT_TRUE(session.Redo().has_value());
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{100.0f}));
    EXPECT_FALSE(session.CanRedo());
}

TEST(SessionTest, FragmentsOfAnErasedStrokeStandWhereItStood) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    session.EraseRect(item, 40.0f, 0.0f, 60.0f, 50.0f);  // the middle out of the first stroke
    ASSERT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 30.0f, 100.0f}))
        << "two fragments, in the erased stroke's place";

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 100.0f}));
    EXPECT_EQ(ItemById(Model(session), item)->strokes[0].points.size(), 2u) << "the whole original";

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 30.0f, 100.0f}));
}

TEST(SessionTest, AStrokeClippedTwiceInOneDragComesBackWhole) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    // One drag, two bites out of the first stroke - the second bite clips a
    // fragment the first one left.
    session.BeginErase(item, 30.0f, 30.0f, 10.0f);
    session.ExtendErase(70.0f, 30.0f, 10.0f);
    session.EndErase();
    ASSERT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 30.0f, 30.0f, 100.0f}));

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 100.0f}));
    EXPECT_EQ(ItemById(Model(session), item)->strokes[0].points.size(), 2u) << "as before the drag";
    EXPECT_TRUE(session.CanRedo()) << "one entry for the drag";
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 30.0f, 30.0f, 100.0f}));
}

TEST(SessionTest, EqualStrokesAreToldApartByPosition) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    DrawLineInto(session, item, 30.0f);  // equal to the first, by value
    session.EraseRect(item, 0.0f, 20.0f, 100.0f, 40.0f);  // both equal strokes, wholly
    ASSERT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{100.0f}));

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 100.0f, 30.0f}))
        << "each equal stroke back in its own place";
    ASSERT_TRUE(session.Undo().has_value()) << "the third stroke";
    EXPECT_EQ(StrokeHeights(Model(session), item), (std::vector<float>{30.0f, 100.0f}));
}

// Nothing leaves the library when a canvas is deleted, so nothing of its
// history does either: restored, it can be undone into as before.
TEST(SessionTest, ACanvasDeletedAndRestoredKeepsItsHistory) {
    Session session;
    const CanvasId first = Model(session).CurrentCanvasId();
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    ASSERT_TRUE(session.CanUndo());

    const CanvasId second = Model(session).AddCanvas("Second");
    Model(session).SwitchToCanvas(second);
    ASSERT_TRUE(session.Delete(first));
    ASSERT_TRUE(session.Restore(first));
    Model(session).SwitchToCanvas(first);
    ASSERT_EQ(Model(session).CurrentCanvasId(), first);
    EXPECT_TRUE(session.CanUndo());
}

TEST(SessionTest, AnItemMovedAwayTakesNoHistoryWithIt) {
    Session session;
    const CanvasId first = Model(session).CurrentCanvasId();
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    const CanvasId second = session.AddCanvas("Second");
    ASSERT_EQ(session.SendItemsTo({item}, second, /*copy=*/false).items.size(), 1u);
    EXPECT_EQ(session.Manager().CurrentCanvasId(), first);
    EXPECT_FALSE(session.CanUndo());
}

TEST(SessionTest, RestoringASnippetRestoresWhatHoldsIt) {
    Session session;
    const FolderId folder = Model(session).CurrentFolderId();
    const CanvasId canvas = Model(session).CurrentCanvasId();
    const ItemId kept = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    const ItemId back = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Back");
    ASSERT_TRUE(session.Delete(kept));
    ASSERT_TRUE(session.Delete(folder));
    EXPECT_TRUE(Model(session).IsItemDeleted(back)) << "inside a deleted folder";

    ASSERT_TRUE(session.Restore(back));
    EXPECT_FALSE(Model(session).IsItemDeleted(back));
    EXPECT_EQ(Model(session).FindCanvas(canvas)->deletedAt, 0);
    EXPECT_EQ(Model(session).FindFolder(folder)->deletedAt, 0) << "the folder came back with it";
    EXPECT_TRUE(Model(session).IsItemDeleted(kept)) << "deleted on its own before, and still";
}

TEST(SessionTest, CapturingWithoutAWindowLeavesAPlaceholder) {
    Session session;
    const ItemId id = session.CreateItem(/*hasBackground=*/true, Rect{0, 0, 100, 100}, "Shot");
    Item* item = Model(session).FindItemAnywhere(id);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->picture.textureHandle, 0u);
    EXPECT_FALSE(item->picture.stored);
    EXPECT_EQ(session.FrozenScreenTexture(), 0u);
}

// Drags a shape from (10, 10) to (x, y), the way the mouse does.
void DragShape(Session& session, ItemId item, Session::Shape shape, float x, float y) {
    session.BeginShape(item, shape, 10.0f, 10.0f, 0xFF0000FFu, 3.0f);
    session.UpdateShape((10.0f + x) * 0.5f, (10.0f + y) * 0.5f);
    session.UpdateShape(x, y);
    session.EndShape(x, y);
}

TEST(SessionTest, AShapeIsBakedAsOneStrokeAndUndoneInOneStep) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DragShape(session, item, Session::Shape::Rectangle, 80.0f, 60.0f);

    const Item* baked = ItemById(Model(session), item);
    ASSERT_EQ(baked->strokes.size(), 1u);
    EXPECT_EQ(baked->strokes[0].points.size(), 5u) << "an outline closed back on its corner";
    const CanvasState& live = session.LiveLayer();
    EXPECT_TRUE(live.Strokes().empty());
    EXPECT_FALSE(live.ActiveStroke().has_value());
    EXPECT_FALSE(session.IsDrawingShape());

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(Model(session), item)->strokes.empty());
}

TEST(SessionTest, AShapeCanChangeWhileItIsDragged) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    session.BeginShape(item, Session::Shape::Rectangle, 10.0f, 10.0f, 0xFF0000FFu, 3.0f);
    session.UpdateShape(80.0f, 60.0f);
    session.SetShape(Session::Shape::Line);
    const CanvasState& live = session.LiveLayer();
    ASSERT_TRUE(live.ActiveStroke().has_value());
    EXPECT_EQ(live.ActiveStroke()->points.size(), 2u) << "the preview follows at once";

    session.EndShape(80.0f, 60.0f);
    const Item* baked = ItemById(Model(session), item);
    ASSERT_EQ(baked->strokes.size(), 1u);
    EXPECT_EQ(baked->strokes[0].points.size(), 2u);
}

// A rectangle dragged flat is the line it looks like, not an outline that
// goes out and back over itself.
TEST(SessionTest, AFlatRectangleIsALine) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    session.BeginShape(item, Session::Shape::Rectangle, 10.0f, 40.0f, 0xFF0000FFu, 3.0f);
    session.EndShape(80.0f, 40.0f);
    const Item* baked = ItemById(Model(session), item);
    ASSERT_EQ(baked->strokes.size(), 1u);
    EXPECT_EQ(baked->strokes[0].points.size(), 2u);
}

TEST(SessionTest, AShapeTooShortToBeMeantLeavesNothing) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DragShape(session, item, Session::Shape::Line, 15.0f, 15.0f);
    EXPECT_TRUE(ItemById(Model(session), item)->strokes.empty());
    EXPECT_FALSE(session.LiveLayer().ActiveStroke().has_value());
    EXPECT_FALSE(session.CanUndo());
}

TEST(SessionTest, MakingASnippetIsUndoneIntoDeletedAndRedoneOutOfIt) {
    Session session;
    const ItemId item = session.CreateItem(/*hasBackground=*/true, Rect{0, 0, 100, 100}, "Shot");
    ASSERT_NE(ItemById(Model(session), item), nullptr);
    ASSERT_TRUE(session.CanUndo());

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Create);
    ASSERT_NE(ItemById(Model(session), item), nullptr) << "where a capture taken by mistake can still be found";
    EXPECT_TRUE(Model(session).IsItemDeleted(item));

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_FALSE(Model(session).IsItemDeleted(item));
}

TEST(SessionTest, AnUntouchedSnippetIsDiscardedWithoutATrace) {
    Session session;
    const ItemId kept = session.CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    DrawStrokeInto(session, kept);
    const ItemId empty = session.CreateItem(false, Rect{200, 0, 100, 100}, "Empty");

    EXPECT_TRUE(session.DiscardIfUntouched(empty));
    EXPECT_EQ(ItemById(Model(session), empty), nullptr) << "erased, not marked deleted";
    // Its own making is off the history; the rest of the canvas's is not.
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
}

TEST(SessionTest, ASnippetWithAnythingInItIsNotDiscarded) {
    Session session;
    const ItemId drawn = session.CreateItem(false, Rect{0, 0, 100, 100}, "Drawn");
    DrawStrokeInto(session, drawn);
    const ItemId noted = session.CreateItem(false, Rect{200, 0, 100, 100}, "Noted");
    session.BeginTextEdit(noted);
    session.EndTextEdit(std::string("text"));
    const ItemId shot = session.CreateItem(true, Rect{400, 0, 100, 100}, "Shot");

    EXPECT_FALSE(session.DiscardIfUntouched(drawn));
    EXPECT_FALSE(session.DiscardIfUntouched(noted));
    EXPECT_FALSE(session.DiscardIfUntouched(shot)) << "a capture is content, even one that failed";
    EXPECT_FALSE(session.DiscardIfUntouched(424242));
    EXPECT_NE(ItemById(Model(session), drawn), nullptr);
    EXPECT_NE(ItemById(Model(session), noted), nullptr);
    EXPECT_NE(ItemById(Model(session), shot), nullptr);
}

// Deleting a snippet for good takes its own entries with it, and nothing
// else of its canvas's history.
TEST(SessionTest, DeletingASnippetPermanentlyLeavesItsCanvasHistoryAlone) {
    Session session;
    const ItemId kept = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    DrawStrokeInto(session, kept);
    const ItemId gone = Model(session).CreateItem(false, Rect{200, 0, 100, 100}, "Gone");
    ASSERT_TRUE(session.Delete(gone));
    ASSERT_TRUE(session.DeletePermanently(gone));
    EXPECT_EQ(ItemById(Model(session), gone), nullptr);
    EXPECT_TRUE(session.CanUndo());
}

// Where `id` stands in `canvasId`'s stack, or -1 if it is not on it.
int StackIndex(const Session& session, CanvasId canvasId, ItemId id) {
    const Canvas* canvas = session.Manager().FindCanvas(canvasId);
    for (size_t i = 0; canvas != nullptr && i < canvas->items.size(); ++i) {
        if (canvas->items[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// A cut's paste, undone, puts the snippet back where it stood in the
// stack it came from; redone, it comes back on top.
TEST(SessionTest, ACutsPasteIsUndoneBackToWhereItStood) {
    Session session;
    const CanvasId first = Model(session).CurrentCanvasId();
    Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Below");
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Above");
    const CanvasId second = Model(session).AddCanvas("Second");
    Model(session).SwitchToCanvas(second);
    Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Resident");

    ASSERT_EQ(session.Paste({moved}, /*cut=*/true).items.size(), 1u);
    ASSERT_EQ(StackIndex(session, second, moved), 1);

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Paste);
    EXPECT_FALSE(undone->refused);
    EXPECT_EQ(StackIndex(session, second, moved), -1);
    EXPECT_EQ(StackIndex(session, first, moved), 1) << "between the two it stood between";

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StackIndex(session, first, moved), -1);
    EXPECT_EQ(StackIndex(session, second, moved), 1) << "on top, as the paste put it";
}

// Several snippets cut from one stack come back into it as it was, in
// whichever order they were selected: undone first to last, two cut
// together came back swapped.
TEST(SessionTest, SeveralSnippetsCutFromOneStackGoBackInItsOrder) {
    for (const std::vector<size_t>& cutOrder : {std::vector<size_t>{1, 2}, std::vector<size_t>{3, 1},
                                                std::vector<size_t>{2, 0, 3}}) {
        Session session;
        const CanvasId first = Model(session).CurrentCanvasId();
        std::vector<ItemId> stack;
        for (const char* name : {"A", "B", "C", "D"}) {
            stack.push_back(Model(session).CreateItem(false, Rect{0, 0, 100, 100}, name));
        }
        const CanvasId second = Model(session).AddCanvas("Second");
        Model(session).SwitchToCanvas(second);
        std::vector<ItemId> cut;
        for (const size_t at : cutOrder) {
            cut.push_back(stack[at]);
        }
        ASSERT_EQ(session.Paste(cut, /*cut=*/true).items.size(), cut.size());

        ASSERT_TRUE(session.Undo().has_value());
        std::vector<ItemId> after;
        for (const Item& item : Model(session).FindCanvas(first)->items) {
            after.push_back(item.id);
        }
        EXPECT_EQ(after, stack) << "cut in order " << cutOrder[0] << "," << cutOrder[1];
    }
}

// Sent back by an undo and edited where it landed, a snippet brought here
// again by the redo takes none of that history with it: an undo there
// would edit a snippet that is no longer on the canvas.
TEST(SessionTest, ARedonePasteLeavesTheHistoryItGatheredMeanwhileBehind) {
    Session session;
    const CanvasId first = Model(session).CurrentCanvasId();
    const ItemId stays = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Stays");
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    DrawStrokeInto(session, stays);
    const CanvasId second = Model(session).AddCanvas("Second");
    Model(session).SwitchToCanvas(second);
    session.Paste({moved}, /*cut=*/true);
    ASSERT_TRUE(session.Undo().has_value());

    Model(session).SwitchToCanvas(first);
    DrawStrokeInto(session, moved);
    Model(session).SwitchToCanvas(second);
    ASSERT_TRUE(session.Redo().has_value());
    ASSERT_EQ(StackIndex(session, second, moved), 0);

    Model(session).SwitchToCanvas(first);
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(Model(session), stays)->strokes.empty()) << "the stroke on what stayed";
    EXPECT_EQ(ItemById(Model(session), moved)->strokes.size(), 1u) << "not the one on what left";
}

// The canvas a cut came from, deleted - marked, or for good - before the
// paste is undone: the snippet stays where it was pasted rather than go
// somewhere nobody can see it, or nowhere at all, and the step says so
// and is dropped, so that the next undo reaches the step before it.
TEST(SessionTest, APasteWhoseSourceCanvasIsDeletedIsNotUndoneAndSaysSo) {
    for (const bool forGood : {false, true}) {
        SCOPED_TRACE(forGood ? "deleted for good" : "deleted");
        Session session;
        const CanvasId first = Model(session).CurrentCanvasId();
        const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
        const CanvasId second = Model(session).AddCanvas("Second");
        Model(session).SwitchToCanvas(second);
        const ItemId made = session.CreateItem(false, Rect{0, 0, 100, 100}, "Made");
        session.Paste({moved}, /*cut=*/true);
        ASSERT_TRUE(session.Delete(first));
        if (forGood) {
            ASSERT_TRUE(session.DeletePermanently(first));
        }

        const std::optional<Session::UndoStep> refused = session.Undo();
        ASSERT_TRUE(refused.has_value());
        EXPECT_TRUE(refused->refused);
        EXPECT_EQ(refused->what, Session::UndoWhat::Paste);
        EXPECT_EQ(StackIndex(session, second, moved), 1) << "still where it was pasted, on top";
        EXPECT_FALSE(Model(session).IsItemDeleted(moved));

        const std::optional<Session::UndoStep> next = session.Undo();
        ASSERT_TRUE(next.has_value());
        EXPECT_EQ(next->what, Session::UndoWhat::Create);
        EXPECT_TRUE(Model(session).IsItemDeleted(made));
    }
}

// Sent back by an undo, then deleted there: nothing to bring again, and
// the redo says so rather than moving a deleted snippet here.
TEST(SessionTest, ARedoOfAPasteWhoseSnippetWasDeletedSinceIsRefused) {
    Session session;
    const CanvasId first = Model(session).CurrentCanvasId();
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    const CanvasId second = Model(session).AddCanvas("Second");
    Model(session).SwitchToCanvas(second);
    session.Paste({moved}, /*cut=*/true);
    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_TRUE(session.Delete(moved));

    const std::optional<Session::UndoStep> refused = session.Redo();
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->refused);
    EXPECT_EQ(StackIndex(session, first, moved), 0) << "left where it is";
    EXPECT_FALSE(session.CanRedo()) << "and the step is gone";
}

// Sent back by an undo, then moved on to a third canvas: it is that
// canvas's now, and the redo leaves it there.
TEST(SessionTest, ARedoOfAPasteWhoseSnippetMovedOnSinceIsRefused) {
    Session session;
    const CanvasId first = Model(session).CurrentCanvasId();
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    const CanvasId second = Model(session).AddCanvas("Second");
    const CanvasId third = Model(session).AddCanvas("Third");
    Model(session).SwitchToCanvas(second);
    session.Paste({moved}, /*cut=*/true);
    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_EQ(Model(session).CanvasHoldingItem(moved), std::optional<CanvasId>(first));
    Model(session).SwitchToCanvas(third);
    session.Paste({moved}, /*cut=*/true);
    Model(session).SwitchToCanvas(second);

    const std::optional<Session::UndoStep> refused = session.Redo();
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->refused);
    EXPECT_EQ(Model(session).CanvasHoldingItem(moved), std::optional<CanvasId>(third)) << "left where it is";
    Model(session).SwitchToCanvas(third);
    EXPECT_TRUE(session.CanUndo()) << "with the third canvas's history of it";
}

// Copies, pasted or duplicated, are undone into their deletion mark - as
// a new snippet is - and redone out of it; the source is not touched.
TEST(SessionTest, CopiesArriveAndGoWithOneUndo) {
    for (const bool duplicate : {false, true}) {
        SCOPED_TRACE(duplicate ? "duplicate" : "paste");
        Session session;
        const ItemId source = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Source");
        const Session::Placed placed =
            duplicate ? session.Duplicate({source, source}) : session.Paste({source, source}, /*cut=*/false);
        ASSERT_EQ(placed.items.size(), 2u);
        const ItemId copyA = placed.items[0];
        const ItemId copyB = placed.items[1];

        const std::optional<Session::UndoStep> undone = session.Undo();
        ASSERT_TRUE(undone.has_value());
        EXPECT_EQ(undone->what, duplicate ? Session::UndoWhat::Duplicate : Session::UndoWhat::Paste);
        EXPECT_TRUE(Model(session).IsItemDeleted(copyA));
        EXPECT_TRUE(Model(session).IsItemDeleted(copyB));
        EXPECT_FALSE(Model(session).IsItemDeleted(source));

        ASSERT_TRUE(session.Redo().has_value());
        EXPECT_FALSE(Model(session).IsItemDeleted(copyA));
        EXPECT_FALSE(Model(session).IsItemDeleted(copyB));
    }
}

// A snippet of a paste moved on elsewhere since is taken out of the
// paste's entry; the rest of the paste is still one undo.
TEST(SessionTest, ForgettingOneSnippetLeavesTheRestOfAPaste) {
    Session session;
    const CanvasId first = Model(session).CurrentCanvasId();
    const ItemId one = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "One");
    const ItemId other = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Other");
    const CanvasId second = Model(session).AddCanvas("Second");
    Model(session).SwitchToCanvas(second);
    session.Paste({one, other}, /*cut=*/true);
    const CanvasId third = Model(session).AddCanvas("Third");
    Model(session).SwitchToCanvas(third);
    ASSERT_EQ(session.Paste({one}, /*cut=*/true).items.size(), 1u);
    Model(session).SwitchToCanvas(second);

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StackIndex(session, first, other), 0) << "went back";
    EXPECT_EQ(StackIndex(session, third, one), 0) << "was not the paste's any more";
}

// With the screen frozen, a shot is cut out of the frozen picture rather
// than captured again - the live screen has moved on, and the user framed
// what they were looking at. The cut is a plain sub-rectangle, saved to
// the library like any other capture's pixels.
TEST(SessionTest, AShotIsCutOutOfTheFrozenScreen) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_frozen_cut";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 7;
    window.createTextureFromPixelsReturnsHandle = 8;
    // A 4x3 screen with every pixel its own value, so the cut can be told
    // from the whole and from any other cut.
    window.captureReturnsWidth = 4;
    window.captureReturnsHeight = 3;
    for (uint8_t i = 0; i < 12; ++i) {
        window.captureReturnsPixelsRGBA.insert(window.captureReturnsPixelsRGBA.end(), {i, i, i, 255});
    }
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    session.FreezeScreen(platform::DisplayInfo{"d", "D", 0, 0, 4, 3, true, 60, 100});
    ASSERT_EQ(window.captureCallCount, 1);
    ASSERT_EQ(session.FrozenScreenTexture(), 7u);

    // The middle two columns of the bottom two rows.
    const ItemId id = session.CreateItem(true, Rect{1.0f, 1.0f, 2.0f, 2.0f}, "Shot");
    Item* shot = Model(session).FindItemAnywhere(id);
    ASSERT_NE(shot, nullptr);

    EXPECT_EQ(window.captureCallCount, 1) << "cut from the frozen screen, not captured again";
    EXPECT_EQ(shot->picture.textureHandle, 8u) << "the cut's own upload";
    ASSERT_TRUE(shot->picture.stored);
    const std::optional<persistence::DecodedImage> saved = store.LoadImage(id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->width, 2);
    EXPECT_EQ(saved->height, 2);
    const std::vector<uint8_t> expected{5, 5, 5, 255, 6, 6, 6, 255, 9, 9, 9, 255, 10, 10, 10, 255};
    EXPECT_EQ(saved->pixelsRGBA, expected);

    // Without the upload the cut is still what is saved - the device lost,
    // say - rather than the live screen, which has moved on: the snippet
    // gets its texture from the library once there is a device again.
    window.createTextureFromPixelsReturnsHandle = 0;
    const ItemId again = session.CreateItem(true, Rect{0.0f, 0.0f, 1.0f, 1.0f}, "Again");
    Item* againShot = Model(session).FindItemAnywhere(again);
    EXPECT_EQ(window.captureCallCount, 1);
    EXPECT_EQ(againShot->picture.textureHandle, 0u);
    const std::optional<persistence::DecodedImage> againSaved = store.LoadImage(again);
    ASSERT_TRUE(againSaved.has_value());
    EXPECT_EQ(againSaved->pixelsRGBA, (std::vector<uint8_t>{0, 0, 0, 255}));

    session.SetLibraryStore(nullptr);
}

// A capture whose upload failed - the device lost while the overlay was
// hidden, with no frame since to replace it - is a capture all the same:
// its pixels are saved, and a freeze keeps them to cut shots from and to
// show once the device is back.
TEST(SessionTest, ACaptureWhoseUploadFailedKeepsItsPixels) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_capture_no_device";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 0;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);

    const ItemId id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    Item* shot = Model(session).FindItemAnywhere(id);
    ASSERT_TRUE(shot->picture.stored);
    const std::optional<persistence::DecodedImage> saved = store.LoadImage(id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->pixelsRGBA, window.captureReturnsPixelsRGBA);

    session.FreezeScreen(platform::DisplayInfo{"d", "D", 0, 0, 2, 1, true, 60, 100});
    EXPECT_EQ(session.FrozenScreenTexture(), 0u);
    const ItemId cut = session.CreateItem(true, Rect{1.0f, 0.0f, 1.0f, 1.0f}, "Cut");
    EXPECT_EQ(window.captureCallCount, 2) << "cut from what was frozen";
    const std::optional<persistence::DecodedImage> cutSaved = store.LoadImage(cut);
    ASSERT_TRUE(cutSaved.has_value());
    EXPECT_EQ(cutSaved->pixelsRGBA, (std::vector<uint8_t>{40, 50, 60, 255}));

    window.createTextureFromPixelsReturnsHandle = 9;  // a device again
    session.ReplaceLostTextures();
    EXPECT_EQ(session.FrozenScreenTexture(), 9u);

    session.SetLibraryStore(nullptr);
}

// A screenshot is the one thing in the library that cannot be remade, so
// a capture whose picture could not be written at capture time keeps its
// pixels and is written by the next save that can - and no save counts as
// landed until it has.
TEST(SessionTest, ACaptureWhosePictureCouldNotBeWrittenIsWrittenByTheNextSave) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_pending_capture";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    ASSERT_EQ(store.Open(), persistence::LibraryStore::OpenResult::Opened);
    HeldLibrary held(dir / "library.db", /*readers=*/false);
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 7;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);

    const ItemId id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    const Item* shot = Model(session).FindItemAnywhere(id);
    EXPECT_EQ(shot->picture.textureHandle, 7u) << "on screen as captured";
    EXPECT_FALSE(shot->picture.stored) << "but not in the library";
    EXPECT_TRUE(session.HasUnsavedChanges());

    // Nothing lands while the file is held, and the save does not count.
    EXPECT_FALSE(session.Flush());
    EXPECT_TRUE(session.HasUnsavedChanges());
    EXPECT_TRUE(session.LastSaveFailed());

    held.Release();
    EXPECT_TRUE(session.Flush());
    EXPECT_FALSE(session.HasUnsavedChanges());
    EXPECT_FALSE(session.LastSaveFailed());
    shot = Model(session).FindItemAnywhere(id);
    ASSERT_TRUE(shot->picture.stored);

    persistence::LibraryStore reopened(dir / "library.db");
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    const Item* reloaded = nullptr;
    for (const Canvas& canvas : loaded->canvases) {
        for (const Item& item : canvas.items) {
            if (item.id == id) {
                reloaded = &item;
            }
        }
    }
    ASSERT_NE(reloaded, nullptr);
    EXPECT_TRUE(reloaded->picture.stored);
    const std::optional<persistence::DecodedImage> saved = reopened.LoadImage(id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->pixelsRGBA, window.captureReturnsPixelsRGBA);

    session.SetLibraryStore(nullptr);
}

// A recovery copy is a library that opens on its own: every picture a
// record in it names is in it, whether the session still held the pixels
// or had to read them back from the real library - and one that could not
// be made whole says so.
TEST(SessionTest, ARecoveryCopyHoldsEveryPictureItsRecordsName) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "spickzettel_session_test_recovery";
    const std::filesystem::path whole = dir / "whole.db";
    const std::filesystem::path partial = dir / "partial.db";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 7;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    const ItemId id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    ASSERT_TRUE(session.Flush());
    ASSERT_TRUE(Model(session).FindItemAnywhere(id)->picture.stored)
        << "in the real library, not in memory";

    EXPECT_TRUE(session.WriteRecoveryCopy(whole));
    persistence::LibraryStore recovered(whole);
    const std::optional<CanvasManagerSnapshot> loaded = recovered.Load();
    ASSERT_TRUE(loaded.has_value());
    const Item* copy = nullptr;
    for (const Canvas& canvas : loaded->canvases) {
        for (const Item& item : canvas.items) {
            copy = &item;
        }
    }
    ASSERT_NE(copy, nullptr);
    EXPECT_TRUE(copy->picture.stored);
    const std::optional<persistence::DecodedImage> picture = recovered.LoadImage(copy->id);
    ASSERT_TRUE(picture.has_value()) << "a snippet with a picture, and the copy does not hold it";
    EXPECT_EQ(picture->pixelsRGBA, window.captureReturnsPixelsRGBA);
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "whole.db.txt"));

    // The real library held by another program: the copy has the records
    // from the session, but not the picture, and says so.
    {
        HeldLibrary held(dir / "library.db", /*readers=*/false);
        EXPECT_FALSE(session.WriteRecoveryCopy(partial));
    }
    {
        std::ifstream note(dir / "partial.db.txt");
        const std::string text((std::istreambuf_iterator<char>(note)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("Incomplete"), std::string::npos) << text;
    }
    persistence::LibraryStore partialCopy(partial);
    const std::optional<CanvasManagerSnapshot> partialLoaded = partialCopy.Load();
    ASSERT_TRUE(partialLoaded.has_value()) << "the records are there all the same";

    session.SetLibraryStore(nullptr);
}

// A copy taken of a capture whose write has not landed has the session's
// pixels to copy from, not a file - and must not come out pictureless.
TEST(SessionTest, ACopyOfACaptureStillWaitingToBeWrittenGetsItsOwnPicture) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_copy_of_pending";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    ASSERT_EQ(store.Open(), persistence::LibraryStore::OpenResult::Opened);
    HeldLibrary held(dir / "library.db", /*readers=*/false);
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 7;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    window.createTextureFromPixelsReturnsHandle = 9;
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);

    const ItemId id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    ASSERT_FALSE(Model(session).FindItemAnywhere(id)->picture.stored) << "not in the library";

    const Session::Placed made = session.Duplicate({id});
    ASSERT_EQ(made.items.size(), 1u);
    const ItemId copyId = made.items[0];
    EXPECT_FALSE(made.pictureLost) << "the pixels are in the session";
    EXPECT_NE(Model(session).FindItemAnywhere(copyId)->picture.textureHandle, 0u) << "on screen at once";

    EXPECT_FALSE(session.Flush()) << "the file is still held";
    held.Release();
    EXPECT_TRUE(session.Flush());
    persistence::LibraryStore reopened(dir / "library.db");
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    size_t pictures = 0;
    for (const Canvas& canvas : loaded->canvases) {
        for (const Item& item : canvas.items) {
            ASSERT_TRUE(item.picture.stored) << "every copy has a picture";
            const std::optional<persistence::DecodedImage> saved = reopened.LoadImage(item.id);
            ASSERT_TRUE(saved.has_value());
            EXPECT_EQ(saved->pixelsRGBA, window.captureReturnsPixelsRGBA);
            ++pictures;
        }
    }
    EXPECT_EQ(pictures, 2u);

    session.SetLibraryStore(nullptr);
}

// A source whose picture is gone from the library gives its copy nothing,
// and says so, rather than quietly producing a copy that looks captured.
TEST(SessionTest, ACopyOfACaptureWhosePictureCannotBeReadSaysSo) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_copy_unreadable";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    Session session;
    session.SetLibraryStore(&store);
    const ItemId id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    Model(session).FindItemAnywhere(id)->picture.stored = true;  // and yet there is none

    const Session::Placed made = session.Duplicate({id});
    ASSERT_EQ(made.items.size(), 1u);
    const ItemId copyId = made.items[0];
    EXPECT_TRUE(made.pictureLost);
    EXPECT_FALSE(Model(session).FindItemAnywhere(copyId)->picture.stored);

    session.SetLibraryStore(nullptr);
}

// Move a captured snippet to another canvas and delete the canvas it left
// for good, all before the autosave: the picture must not go with the
// canvas the snippet left.
TEST(SessionTest, APermanentDeleteRightAfterAMoveKeepsWhatWasMoved) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_delete_after_move";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    const std::vector<uint8_t> pixels = {10, 20, 30, 255, 40, 50, 60, 255};
    ItemId id = 0;
    {
        persistence::LibraryStore store(dir / "library.db");
        Session session;
        session.SetLibraryStore(&store);
        const CanvasId left = Model(session).CurrentCanvasId();
        id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
        ASSERT_TRUE(store.SaveImage(id, pixels.data(), 2, 1));
        Model(session).FindItemAnywhere(id)->picture.stored = true;
        const CanvasId other = Model(session).AddCanvas("Other");
        ASSERT_TRUE(session.Flush());

        ASSERT_NE(Model(session).PlaceItemOnCanvas(id, other, /*copy=*/false), 0u);
        ASSERT_TRUE(session.Delete(left));
        EXPECT_TRUE(session.DeletePermanently(left));
        EXPECT_FALSE(session.HasUnsavedChanges()) << "saved with the delete";
        session.SetLibraryStore(nullptr);
    }
    persistence::LibraryStore reopened(dir / "library.db");
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    const Item& item = loaded->canvases[0].items[0];
    ASSERT_EQ(item.id, id);
    const std::optional<persistence::DecodedImage> image = reopened.LoadImage(id);
    ASSERT_TRUE(image.has_value()) << "the picture went with the canvas the snippet left";
    EXPECT_EQ(image->pixelsRGBA, pixels);
}

// The other way round: a captured snippet moved into a canvas, and that
// canvas deleted for good, before the autosave. The snippet goes with the
// canvas it went to, picture and all.
TEST(SessionTest, APermanentDeleteRightAfterAMoveIntoTheCanvasTakesWhatWasMovedIn) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_delete_after_move_in";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    const std::vector<uint8_t> pixels = {10, 20, 30, 255, 40, 50, 60, 255};
    ItemId id = 0;
    {
        persistence::LibraryStore store(dir / "library.db");
        Session session;
        session.SetLibraryStore(&store);
        id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
        ASSERT_TRUE(store.SaveImage(id, pixels.data(), 2, 1));
        Model(session).FindItemAnywhere(id)->picture.stored = true;
        const CanvasId other = Model(session).AddCanvas("Other");
        ASSERT_TRUE(session.Flush());

        ASSERT_NE(Model(session).PlaceItemOnCanvas(id, other, /*copy=*/false), 0u);
        ASSERT_TRUE(session.Delete(other));
        EXPECT_TRUE(session.DeletePermanently(other));
        session.SetLibraryStore(nullptr);
    }
    persistence::LibraryStore reopened(dir / "library.db");
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_TRUE(loaded->canvases[0].items.empty());
    EXPECT_FALSE(reopened.HasImage(id));
}

}  // namespace
}  // namespace sz::core
