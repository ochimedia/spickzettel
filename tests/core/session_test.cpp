#include "core/session/session.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/persistence/library_store.h"
#include "fakes/fake_platform_host.h"
#include "support/failing_writes.h"
#include "support/held_library.h"
#include "support/removed_at_end.h"
#include "support/session_test_access.h"
#include "support/temp_dir.h"

namespace sz::core {
namespace {

using test::FailingWrites;
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

// Empty trash: everything deleted goes for good, however and whenever it
// was deleted, and nothing live is touched.
TEST(SessionTest, EmptyingTheTrashErasesEverythingDeletedAndNothingElse) {
    Session session;
    CanvasManager& manager = Model(session);
    const FolderId home = manager.CurrentFolderId();
    const CanvasId live = manager.CurrentCanvasId();
    const CanvasId deletedCanvas = manager.AddCanvas("Deleted canvas");
    const FolderId deletedFolder = manager.AddFolder("Deleted folder");
    manager.SwitchToFolder(deletedFolder);
    const CanvasId inDeletedFolder = manager.AddCanvas("In deleted folder");
    manager.SwitchToCanvas(live);
    EXPECT_EQ(session.EmptyTrash(), 0u) << "nothing in it yet";
    ASSERT_TRUE(session.Delete(deletedCanvas));
    ASSERT_TRUE(session.Delete(deletedFolder));

    EXPECT_EQ(session.EmptyTrash(), 2u) << "the canvas, and the folder with what is in it";
    EXPECT_EQ(manager.FindCanvas(deletedCanvas), nullptr);
    EXPECT_EQ(manager.FindFolder(deletedFolder), nullptr);
    EXPECT_EQ(manager.FindCanvas(inDeletedFolder), nullptr);
    EXPECT_NE(manager.FindCanvas(live), nullptr);
    EXPECT_NE(manager.FindFolder(home), nullptr);
    EXPECT_EQ(manager.DeletedFolderAndCanvasCount(), 0u);
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

// A burst - wheel notches, arrow presses - is a placement held open, one
// entry that goes back to where the burst began; one for other snippets
// ends it, and anything filed on the way ends it too.
TEST(SessionTest, APlacementHeldOpenForABurstIsOneUndo) {
    Session session;
    CanvasManager& manager = Model(session);
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    for (int i = 1; i <= 3; ++i) {
        if (!session.Placing({a})) {
            session.BeginPlacement({a});
        }
        session.PreviewRect(a, Rect{static_cast<float>(i), 0, 100, 100});
    }
    EXPECT_FALSE(session.Placing({})) << "exactly the snippets it holds";
    ASSERT_TRUE(session.EndPlacement());
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.x, 0.0f) << "the whole burst, at once";
    EXPECT_FALSE(session.CanUndo());

    ASSERT_TRUE(session.Redo().has_value());
    session.BeginPlacement({a});
    session.PreviewRect(a, Rect{9.0f, 0, 100, 100});
    const ItemId b = session.CreateItem(false, Rect{300, 0, 50, 50}, "B");  // something else filed
    ASSERT_NE(b, 0u);
    EXPECT_FALSE(session.Placing({a})) << "ended by what was filed";
    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.x, 3.0f) << "filed before it, as a step of its own";
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

// An undo with the note open closes it first - an edit that changed
// nothing files nothing - and then takes back the edit before it.
TEST(SessionTest, AnUndoWithTheNoteOpenClosesItFirst) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    session.BeginTextEdit(item);
    session.EndTextEdit(std::string("first"));
    EXPECT_EQ(ItemById(Model(session), item)->noteText, "first");

    session.BeginTextEdit(item);
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::TextEdit);
    EXPECT_EQ(ItemById(Model(session), item)->noteText, "");
    EXPECT_FALSE(session.TextEditItem().has_value());
    EXPECT_FALSE(session.CanUndo());
}

// The eraser takes what it passed over between two of its positions, not
// only what lies under each: movement comes once a frame, and a quick
// stroke of the hand crosses a line between two of them. Here neither
// position touches the line, and the pass between them cuts it in two -
// one undo step, which puts it back.
TEST(SessionTest, TheEraserTakesWhatItPassedOverBetweenTwoPositions) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 50.0f);
    const std::vector<Stroke> drawn = ItemById(Model(session), item)->strokes;

    session.BeginErase(item, 50.0f, 20.0f, 10.0f);
    session.ExtendErase(50.0f, 80.0f, 10.0f);
    session.EndErase();

    const std::vector<Stroke>& left = ItemById(Model(session), item)->strokes;
    ASSERT_EQ(left.size(), 2u);
    EXPECT_NEAR(left[0].points.back().x, 45.0f, 1e-3f);
    EXPECT_NEAR(left[1].points.front().x, 55.0f, 1e-3f);

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemById(Model(session), item)->strokes, drawn);
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
    // fragment the first one left. Off the line and back between them, so
    // the eraser's pass takes nothing between the two.
    session.BeginErase(item, 30.0f, 30.0f, 10.0f);
    session.ExtendErase(30.0f, 60.0f, 10.0f);
    session.ExtendErase(70.0f, 60.0f, 10.0f);
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

TEST(SessionTest, CapturingWithoutAWindowLeavesAPlaceholder) {
    Session session;
    const ItemId id = session.CreateItem(/*hasBackground=*/true, Rect{0, 0, 100, 100}, "Shot");
    Item* item = Model(session).FindItemAnywhere(id);
    ASSERT_NE(item, nullptr);
    EXPECT_FALSE(item->picture.stored);
    EXPECT_EQ(session.Textures().Size(), 0u);
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
    EXPECT_EQ(baked->strokes[0].corners, StrokeCorners::Sharp) << "and square at them";
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


// Deleting a snippet for good takes its own entries with it, and nothing
// else of its canvas's history.
TEST(SessionTest, DeletingASnippetPermanentlyLeavesItsCanvasHistoryAlone) {
    Session session;
    const ItemId kept = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    DrawStrokeInto(session, kept);
    const ItemId gone = Model(session).CreateItem(false, Rect{200, 0, 100, 100}, "Gone");
    ASSERT_TRUE(session.DeleteItem(gone));
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
    EXPECT_EQ(undone->intoDeletedCanvas, 0u);
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

// A copy of a fullscreen snippet, duplicated or pasted beside its source,
// is fullscreen as its source is, and what is offset is the place it goes
// back to: taken out of fullscreen, it is its source's size, a little down
// and to the right of it. Offset as a snippet on the canvas is, the
// fullscreen rect became the place it went back to.
TEST(SessionTest, ACopyOfAFullscreenSnippetGoesBackToItsSourcesSizeBesideIt) {
    for (const bool stretch : {false, true}) {
        for (const bool duplicate : {false, true}) {
            SCOPED_TRACE(std::string(duplicate ? "duplicate" : "paste") + (stretch ? ", stretched" : ", fitted"));
            Session session;
            session.SyncItemsToDisplaySize(1920.0f, 1080.0f);
            const ItemId source = Model(session).CreateItem(false, Rect{100, 100, 400, 200}, "Source");
            Model(session).CommitItemLayout(source);
            session.ToggleFullscreen(source, stretch);
            ASSERT_TRUE(Model(session).FindItemAnywhere(source)->isFullscreen);

            const Session::Placed placed =
                duplicate ? session.Duplicate({source}) : session.Paste({source}, /*cut=*/false);
            ASSERT_EQ(placed.items.size(), 1u);
            const ItemId copy = placed.items[0];
            const Item* made = Model(session).FindItemAnywhere(copy);
            EXPECT_TRUE(made->isFullscreen);
            EXPECT_EQ(made->rect, Model(session).FindItemAnywhere(source)->rect) << "fullscreen, as its source";

            session.ToggleFullscreen(copy, stretch);
            EXPECT_EQ(Model(session).FindItemAnywhere(copy)->rect, (Rect{124, 124, 400, 200}));
            session.SyncItemsToDisplaySize(1920.0f, 1080.0f);
            EXPECT_EQ(Model(session).FindItemAnywhere(copy)->rect, (Rect{124, 124, 400, 200})) << "and anchored there";
        }
    }
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

// With the screen frozen, a shot is cut out of the frozen picture rather
// than captured again - the live screen has moved on, and the user framed
// what they were looking at. The cut is a plain sub-rectangle, saved to
// the library like any other capture's pixels.
TEST(SessionTest, AShotIsCutOutOfTheFrozenScreen) {
    const std::filesystem::path dir =
        sz::test::TempDir() / "spickzettel_session_test_frozen_cut";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    test::FakeOverlayWindow window;
    window.uploadsSucceed = true;
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
    ASSERT_TRUE(session.WriteWholeLibrary());
    session.FreezeScreen(platform::DisplayInfo{"d", "D", 0, 0, 4, 3, true, 60, 100});
    ASSERT_EQ(window.captureCallCount, 1);
    ASSERT_NE(session.FrozenScreenTexture(), 0u);

    // The middle two columns of the bottom two rows.
    const ItemId id = session.CreateItem(true, Rect{1.0f, 1.0f, 2.0f, 2.0f}, "Shot");
    Item* shot = Model(session).FindItemAnywhere(id);
    ASSERT_NE(shot, nullptr);

    EXPECT_EQ(window.captureCallCount, 1) << "cut from the frozen screen, not captured again";
    const std::optional<uint64_t> texture = session.Textures().Find(TextureKey{TextureKey::Kind::Picture, id});
    ASSERT_TRUE(texture.has_value());
    EXPECT_TRUE(window.IsDrawable(*texture)) << "the cut's own texture, from its pixels";
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
    window.uploadsSucceed = false;
    const ItemId again = session.CreateItem(true, Rect{0.0f, 0.0f, 1.0f, 1.0f}, "Again");
    EXPECT_EQ(window.captureCallCount, 1);
    EXPECT_EQ(session.Textures().Find(TextureKey{TextureKey::Kind::Picture, again}), std::optional<uint64_t>(0));
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
        sz::test::TempDir() / "spickzettel_session_test_capture_no_device";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    test::FakeOverlayWindow window;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    ASSERT_TRUE(session.WriteWholeLibrary());

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

    // Not tried again on every frame - but a device again is a new one,
    // and on it everything is made again.
    window.uploadsSucceed = true;
    EXPECT_EQ(session.FrozenScreenTexture(), 0u);
    ++window.textureGeneration;
    EXPECT_TRUE(window.IsDrawable(session.FrozenScreenTexture()));

    session.SetLibraryStore(nullptr);
}

// A capture past what a picture may be is no picture: the snippet is made
// with its placeholder, rather than shown until the next start and then
// never again because the library would not read it back.
TEST(SessionTest, ACapturePastThePictureBudgetLeavesThePlaceholder) {
    const std::filesystem::path dir =
        sz::test::TempDir() / "spickzettel_session_test_capture_too_big";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    test::FakeOverlayWindow window;
    window.uploadsSucceed = true;
    window.captureReturnsWidth = persistence::kMaxImageExtent + 1;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA.assign(static_cast<size_t>(window.captureReturnsWidth) * 4, 128);
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    ASSERT_TRUE(session.WriteWholeLibrary());

    const ItemId id = session.CreateItem(true, Rect{0.0f, 0.0f, 100.0f, 1.0f}, "Shot");
    ASSERT_NE(id, 0u) << "the snippet is made all the same";
    const Item* shot = Model(session).FindItemAnywhere(id);
    ASSERT_NE(shot, nullptr);
    EXPECT_FALSE(shot->picture.stored);
    EXPECT_FALSE(session.Textures().Find(TextureKey{TextureKey::Kind::Picture, id}).has_value());
    EXPECT_FALSE(store.HasImage(id));

    session.SetLibraryStore(nullptr);
}

// A source whose picture is gone from the library gives its copy nothing,
// and says so, rather than quietly producing a copy that looks captured.
TEST(SessionTest, ACopyOfACaptureWhosePictureCannotBeReadSaysSo) {
    const std::filesystem::path dir =
        sz::test::TempDir() / "spickzettel_session_test_copy_unreadable";
    std::filesystem::remove_all(dir);
    const RemovedAtEnd cleanup(dir);
    persistence::LibraryStore store(dir / "library.db");
    Session session;
    session.SetLibraryStore(&store);
    ASSERT_TRUE(session.WriteWholeLibrary());
    const ItemId id = session.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    Model(session).FindItemAnywhere(id)->picture.stored = true;  // and yet there is none

    const Session::Placed made = session.Duplicate({id});
    ASSERT_EQ(made.items.size(), 1u);
    const ItemId copyId = made.items[0];
    EXPECT_TRUE(made.pictureLost);
    EXPECT_FALSE(Model(session).FindItemAnywhere(copyId)->picture.stored);

    session.SetLibraryStore(nullptr);
}

// ===== Every command written as it is made =====

// A library in a temporary directory, taken away at the end, and a window
// whose captures are one 2x1 picture.
class WrittenSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = sz::test::TempDir() /
               (std::string("spickzettel_session_written_") +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        cleanup_.emplace(dir_);
        store_.emplace(File());
        ASSERT_EQ(store_->Open(), persistence::LibraryStore::OpenResult::Opened);
        window_.captureReturnsWidth = 2;
        window_.captureReturnsHeight = 1;
        window_.captureReturnsPixelsRGBA = kPixels;
        window_.uploadsSucceed = true;
        session_.AttachWindow(&window_);
        session_.SetLibraryStore(&*store_);
        ASSERT_TRUE(session_.WriteWholeLibrary());
    }
    void TearDown() override {
        session_.SetLibraryStore(nullptr);
        store_.reset();
    }

    std::filesystem::path File() const { return dir_ / "library.db"; }
    // The library as the file holds it now.
    CanvasManagerSnapshot OnDisk() const {
        std::optional<CanvasManagerSnapshot> loaded = persistence::LibraryStore(File()).Load();
        EXPECT_TRUE(loaded.has_value());
        return loaded.value_or(CanvasManagerSnapshot{});
    }
    static size_t ItemsIn(const CanvasManagerSnapshot& snapshot) {
        size_t items = 0;
        for (const Canvas& canvas : snapshot.canvases) {
            items += canvas.items.size();
        }
        return items;
    }

    inline static const std::vector<uint8_t> kPixels = {10, 20, 30, 255, 40, 50, 60, 255};
    std::filesystem::path dir_;
    std::optional<RemovedAtEnd> cleanup_;
    std::optional<persistence::LibraryStore> store_;
    test::FakeOverlayWindow window_;
    Session session_;
};

// A screenshot is in the file, picture and all, the moment it is made.
TEST_F(WrittenSessionTest, ACaptureIsWrittenWithItsPictureAsItIsMade) {
    const ItemId id = session_.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    ASSERT_NE(id, 0u);
    EXPECT_TRUE(session_.Manager().FindItemAnywhere(id)->picture.stored);
    const CanvasManagerSnapshot disk = OnDisk();
    ASSERT_EQ(ItemsIn(disk), 1u);
    EXPECT_TRUE(disk.canvases[0].items[0].picture.stored);
    const std::optional<persistence::DecodedImage> picture = store_->LoadImage(id);
    ASSERT_TRUE(picture.has_value());
    EXPECT_EQ(picture->pixelsRGBA, kPixels);
}

// A capture whose write fails is not made: nothing on screen that looks
// captured and is not in the library, nothing on the history, and the
// texture it had given back once nothing has drawn it for a frame. The
// next one, once the file is free, is.
TEST_F(WrittenSessionTest, ACaptureThatCannotBeWrittenIsNotMade) {
    const size_t before = session_.Manager().CurrentOrNull()->items.size();
    {
        HeldLibrary held(File(), /*readers=*/true);
        EXPECT_EQ(session_.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot"), 0u);
    }
    EXPECT_EQ(session_.Manager().CurrentOrNull()->items.size(), before);
    EXPECT_TRUE(session_.LastWriteFailed());
    EXPECT_FALSE(session_.CanUndo());
    EXPECT_EQ(window_.liveTextures.size(), 1u);
    session_.Textures().BeginFrame();
    session_.Textures().BeginFrame();
    EXPECT_TRUE(window_.liveTextures.empty()) << "its texture";

    const ItemId id = session_.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    ASSERT_NE(id, 0u);
    EXPECT_FALSE(session_.LastWriteFailed());
    EXPECT_EQ(ItemsIn(OnDisk()), 1u);
}

// Any command whose write fails is not made, and nothing of it is filed;
// an undo whose write fails is put back where it was, to be taken again.
TEST_F(WrittenSessionTest, ACommandOrAnUndoThatCannotBeWrittenIsNotMade) {
    const ItemId item = session_.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session_, item);
    ASSERT_EQ(OnDisk().canvases[0].items[0].strokes.size(), 1u);
    {
        HeldLibrary held(File(), /*readers=*/true);
        DrawStrokeInto(session_, item);
        EXPECT_EQ(ItemById(session_.Manager(), item)->strokes.size(), 1u) << "not made";
        EXPECT_FALSE(session_.Undo().has_value());
        EXPECT_EQ(ItemById(session_.Manager(), item)->strokes.size(), 1u) << "not undone";
        EXPECT_TRUE(session_.LastWriteFailed());
    }
    const std::optional<Session::UndoStep> undone = session_.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke) << "the stroke's step, still there";
    EXPECT_TRUE(OnDisk().canvases[0].items[0].strokes.empty());
}

// A copy's picture is written with the copy; a copy whose write fails is
// not made.
TEST_F(WrittenSessionTest, ACopysPictureIsWrittenWithTheCopy) {
    const ItemId id = session_.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    {
        HeldLibrary held(File(), /*readers=*/true);
        EXPECT_TRUE(session_.Duplicate({id}).items.empty());
    }
    const Session::Placed made = session_.Duplicate({id});
    ASSERT_EQ(made.items.size(), 1u);
    EXPECT_FALSE(made.pictureLost);
    const CanvasManagerSnapshot disk = OnDisk();
    ASSERT_EQ(ItemsIn(disk), 2u);
    for (const Item& item : disk.canvases[0].items) {
        const std::optional<persistence::DecodedImage> picture = store_->LoadImage(item.id);
        ASSERT_TRUE(picture.has_value());
        EXPECT_EQ(picture->pixelsRGBA, kPixels);
    }
}

// A style preview about other snippets ends the edit open first. An end
// that cannot be written puts the library back as it was, moving every
// snippet in it, and the new preview is not begun: nothing is left open,
// and the next preview begins it as usual.
TEST_F(WrittenSessionTest, AStylePreviewAfterAnEditThatCannotBeWrittenIsNotBegun) {
    const ItemId a = session_.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = session_.CreateItem(false, Rect{200, 0, 100, 100}, "B");
    ItemStyle half = ItemStyle::Of(*ItemById(session_.Manager(), a));
    half.foregroundOpacity = 0.5f;
    session_.PreviewStyles({{a, half}, {b, half}});

    FailingWrites failing(File());
    failing.FailAll();
    ItemStyle faint = half;
    faint.foregroundOpacity = 0.2f;
    session_.PreviewStyles({{a, faint}});
    failing.Stop();
    EXPECT_TRUE(session_.LastWriteFailed());
    EXPECT_FALSE(session_.StyleEditOpen());
    EXPECT_FLOAT_EQ(ItemById(session_.Manager(), a)->foregroundOpacity, 1.0f);
    EXPECT_FLOAT_EQ(ItemById(session_.Manager(), b)->foregroundOpacity, 1.0f) << "the edit of both, not made";

    session_.PreviewStyles({{a, faint}});
    EXPECT_TRUE(session_.EndStyleEdit());
    const CanvasManagerSnapshot disk = OnDisk();
    ASSERT_EQ(ItemsIn(disk), 2u);
    EXPECT_FLOAT_EQ(disk.canvases[0].items[0].foregroundOpacity, 0.2f);
    EXPECT_FLOAT_EQ(disk.canvases[0].items[1].foregroundOpacity, 1.0f);
}

// Every command ends the gesture open first. One whose write fails has
// gone back as it was, and the command does nothing more: an undo would
// take back a second thing, the step before the drag, with the drag's
// failure never said.
TEST_F(WrittenSessionTest, ACommandAfterAGestureThatCannotBeWrittenDoesNothingMore) {
    const ItemId a = session_.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = session_.CreateItem(false, Rect{200, 0, 100, 100}, "B");
    DrawStrokeInto(session_, a);
    session_.BeginPlacement({b});
    session_.PreviewRect(b, Rect{200, 150, 100, 100});

    FailingWrites failing(File());
    failing.FailSnippet(b);
    EXPECT_FALSE(session_.Undo().has_value());
    failing.Stop();
    EXPECT_TRUE(session_.LastWriteFailed());
    EXPECT_EQ(ItemById(session_.Manager(), b)->rect, (Rect{200, 0, 100, 100})) << "the drag, not made";
    EXPECT_EQ(ItemById(session_.Manager(), a)->strokes.size(), 1u) << "and nothing else taken back";

    const std::optional<Session::UndoStep> undone = session_.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session_.Manager(), a)->strokes.empty());
}

// A failure is said until a write lands. A command with nothing to write
// - a click that selected and moved nothing - lands nothing, and says
// nothing of whether the file can be written now.
TEST_F(WrittenSessionTest, AFailureStandsThroughACommandWithNothingToWrite) {
    const ItemId id = session_.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    {
        FailingWrites failing(File());
        failing.FailAll();
        session_.SetPinned({id}, true);
        failing.Stop();
    }
    ASSERT_TRUE(session_.LastWriteFailed());
    session_.BeginPlacement({id});
    EXPECT_FALSE(session_.EndPlacement());
    EXPECT_TRUE(session_.LastWriteFailed()) << "nothing landed";

    session_.SetPinned({id}, true);
    EXPECT_FALSE(session_.LastWriteFailed()) << "a write that landed";
}

// A stroke being drawn belongs to the canvas, not to the command whose
// write failed beside it: a note committed while the stroke was drawn - its
// field let go of by the press that began the stroke - and not written
// leaves the stroke to go on and be made.
TEST_F(WrittenSessionTest, AStrokeBeingDrawnOutlastsAFailedWriteBesideIt) {
    const ItemId note = session_.CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    const ItemId drawing = session_.CreateItem(false, Rect{200, 0, 100, 100}, "Drawing");
    session_.BeginTextEdit(note);
    session_.PreviewText("typed");
    session_.LiveLayer().BeginStroke(StrokePoint{210.0f, 10.0f}, 0xFF0000FFu, 3.0f);
    {
        FailingWrites failing(File());
        failing.FailAll();
        session_.EndTextEdit();
        failing.Stop();
    }
    ASSERT_TRUE(session_.LastWriteFailed());
    EXPECT_TRUE(ItemById(session_.Manager(), note)->noteText.empty()) << "the note, not made";
    ASSERT_TRUE(session_.LiveLayer().ActiveStroke().has_value()) << "the stroke, still being drawn";

    session_.LiveLayer().ExtendStroke(StrokePoint{260.0f, 60.0f});
    session_.LiveLayer().EndStroke();
    session_.CommitLiveStroke(drawing);
    EXPECT_EQ(ItemById(session_.Manager(), drawing)->strokes.size(), 1u);
    EXPECT_EQ(OnDisk().canvases[0].items[1].strokes.size(), 1u);
}

// A gesture called off writes nothing: the file holds what it held before
// the press, and what was filed before the gesture is still there.
TEST_F(WrittenSessionTest, ACanceledDragWritesNothing) {
    const ItemId id = session_.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    session_.BeginPlacement({id});
    session_.PreviewRect(id, Rect{50, 50, 100, 100});
    session_.CancelPlacement();
    EXPECT_EQ(session_.Manager().FindItemAnywhere(id)->rect, (Rect{0, 0, 100, 100}));
    const CanvasManagerSnapshot disk = OnDisk();
    ASSERT_EQ(ItemsIn(disk), 1u);
    EXPECT_EQ(disk.canvases[0].items[0].rect, (Rect{0, 0, 100, 100}));
    EXPECT_FALSE(session_.LastWriteFailed());
}

// A snippet sent to another canvas, and the canvas it left then deleted for
// good: it is where it was sent, picture and all.
TEST_F(WrittenSessionTest, ACanvasDeletedForGoodAfterASnippetLeftItKeepsTheSnippet) {
    const CanvasId left = session_.Manager().CurrentCanvasId();
    const ItemId id = session_.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    const CanvasId other = session_.AddCanvas("Other");
    ASSERT_EQ(session_.SendItemsTo({id}, other, /*copy=*/false).items.size(), 1u);
    ASSERT_TRUE(session_.Delete(left));
    ASSERT_TRUE(session_.DeletePermanently(left));

    const CanvasManagerSnapshot disk = OnDisk();
    ASSERT_EQ(disk.canvases.size(), 1u);
    ASSERT_EQ(disk.canvases[0].items.size(), 1u);
    EXPECT_EQ(disk.canvases[0].items[0].id, id);
    EXPECT_TRUE(store_->LoadImage(id).has_value());
}

// The other way round: sent into a canvas deleted for good after, it goes
// with the canvas, picture and all.
TEST_F(WrittenSessionTest, ACanvasDeletedForGoodTakesWhatWasSentIntoIt) {
    const ItemId id = session_.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    const CanvasId other = session_.AddCanvas("Other");
    ASSERT_EQ(session_.SendItemsTo({id}, other, /*copy=*/false).items.size(), 1u);
    ASSERT_TRUE(session_.Delete(other));
    ASSERT_TRUE(session_.DeletePermanently(other));

    const CanvasManagerSnapshot disk = OnDisk();
    ASSERT_EQ(disk.canvases.size(), 1u);
    EXPECT_TRUE(disk.canvases[0].items.empty());
    EXPECT_FALSE(store_->HasImage(id));
}

// Deleting for good that cannot be written deletes nothing: the canvas and
// what is on it stay, with their history.
TEST_F(WrittenSessionTest, ADeleteForGoodThatCannotBeWrittenKeepsEverything) {
    const CanvasId first = session_.Manager().CurrentCanvasId();
    const ItemId id = session_.CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    const CanvasId other = session_.AddCanvas("Other");
    session_.SwitchToCanvas(other);
    ASSERT_TRUE(session_.Delete(first));
    {
        HeldLibrary held(File(), /*readers=*/true);
        EXPECT_FALSE(session_.DeletePermanently(first));
    }
    ASSERT_NE(session_.Manager().FindCanvas(first), nullptr);
    ASSERT_NE(session_.Manager().FindItemAnywhere(id), nullptr);
    ASSERT_TRUE(session_.Restore(first));
    session_.SwitchToCanvas(first);
    EXPECT_TRUE(session_.CanUndo()) << "its history too";
    EXPECT_EQ(ItemsIn(OnDisk()), 1u);
}

// ===== History that follows its snippets =====
//
// See history::History for the rules these hold it to: a snippet's undo
// changes are on the canvas holding it, a new change ends its redo future,
// and nothing on a stack is ever refused.

// Delete and Restore are for folders and canvases. A snippet's mark is the
// history's to change - set outside it, the next undo of that snippet's
// making or deleting would swap in a mark it did not expect.
TEST(SessionTest, DeleteAndRestoreAreForFoldersAndCanvasesOnly) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    EXPECT_FALSE(session.Delete(item));
    EXPECT_FALSE(session.Manager().IsItemDeleted(item));
    ASSERT_TRUE(session.DeleteItem(item));
    EXPECT_FALSE(session.Restore(item));
    EXPECT_TRUE(session.Manager().IsItemDeleted(item));
}

// A snippet sent to another canvas takes its history there: nothing of it
// is left to undo where it was, and there its send is the newest step and
// its stroke under it.
TEST(SessionTest, ASnippetSentAwayTakesItsHistoryWithIt) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    const CanvasId second = session.AddCanvas("Second");
    ASSERT_EQ(session.SendItemsTo({item}, second, /*copy=*/false).items.size(), 1u);
    EXPECT_EQ(session.Manager().CurrentCanvasId(), first);
    EXPECT_FALSE(session.CanUndo()) << "nothing of it left here";

    session.SwitchToCanvas(second);
    const std::optional<Session::UndoStep> send = session.Undo();
    ASSERT_TRUE(send.has_value());
    EXPECT_EQ(send->what, Session::UndoWhat::Move);
    EXPECT_EQ(session.Manager().CanvasHoldingItem(item), std::optional<CanvasId>(first)) << "sent back";
    EXPECT_FALSE(session.CanUndo()) << "and its stroke with it";

    session.SwitchToCanvas(first);
    const std::optional<Session::UndoStep> stroke = session.Undo();
    ASSERT_TRUE(stroke.has_value());
    EXPECT_EQ(stroke->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session.Manager(), item)->strokes.empty());
}

// A snippet sent from A to B, on to C and then to D, with C then deleted
// for good: the moves from and to C cannot be undone, and neither can the
// one from A to B before them - undone on D, it asked for the snippet on
// B. Everything else about it still undoes, and it stays on D.
TEST(SessionTest, ACanvasDeletedForGoodTakesTheMovesBeforeItsOwn) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const CanvasId b = session.AddCanvas("B");
    const CanvasId c = session.AddCanvas("C");
    const CanvasId d = session.AddCanvas("D");
    ASSERT_EQ(session.SendItemsTo({item}, b, /*copy=*/false).items.size(), 1u);
    session.SwitchToCanvas(b);
    DrawStrokeInto(session, item);
    ASSERT_EQ(session.SendItemsTo({item}, c, /*copy=*/false).items.size(), 1u);
    session.SwitchToCanvas(c);
    ASSERT_EQ(session.SendItemsTo({item}, d, /*copy=*/false).items.size(), 1u);
    session.SwitchToCanvas(d);
    ASSERT_TRUE(session.Delete(c));
    ASSERT_TRUE(session.DeletePermanently(c));

    const std::optional<Session::UndoStep> stroke = session.Undo();
    ASSERT_TRUE(stroke.has_value()) << "every step left applies";
    EXPECT_EQ(stroke->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session.Manager(), item)->strokes.empty());
    EXPECT_FALSE(session.CanUndo()) << "no move left to undo";
    EXPECT_EQ(session.Manager().CanvasHoldingItem(item), std::optional<CanvasId>(d));
}

// A stroke drawn across two snippets is one step about both; one of them
// sent away takes its part with it. Undone where it was drawn, the stroke
// comes off the snippet still there and stays on the one sent. There, the
// send is undone first, and back home the snippet's part is undone after.
TEST(SessionTest, AStrokeAcrossTwoSnippetsSplitsWhenOneIsSentAway) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId a = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = Model(session).CreateItem(false, Rect{120, 0, 100, 100}, "B");
    session.LiveLayer().BeginStroke(StrokePoint{50.0f, 50.0f}, 0xFF0000FFu, 3.0f);
    session.LiveLayer().ExtendStroke(StrokePoint{170.0f, 50.0f});
    session.LiveLayer().EndStroke();
    session.CommitLiveStroke({a, b});
    ASSERT_EQ(ItemById(session.Manager(), a)->strokes.size(), 1u);
    ASSERT_EQ(ItemById(session.Manager(), b)->strokes.size(), 1u);

    const CanvasId second = session.AddCanvas("Second");
    ASSERT_EQ(session.SendItemsTo({b}, second, /*copy=*/false).items.size(), 1u);
    const std::optional<Session::UndoStep> here = session.Undo();
    ASSERT_TRUE(here.has_value());
    EXPECT_EQ(here->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session.Manager(), a)->strokes.empty());
    EXPECT_EQ(ItemById(session.Manager(), b)->strokes.size(), 1u) << "sent away, with its part";

    session.SwitchToCanvas(second);
    const std::optional<Session::UndoStep> send = session.Undo();
    ASSERT_TRUE(send.has_value());
    EXPECT_EQ(send->what, Session::UndoWhat::Move);
    EXPECT_FALSE(session.CanUndo()) << "its part went home with it";

    session.SwitchToCanvas(first);
    const std::optional<Session::UndoStep> home = session.Undo();
    ASSERT_TRUE(home.has_value());
    EXPECT_EQ(home->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session.Manager(), b)->strokes.empty());
    EXPECT_FALSE(session.CanUndo());
}

// A group move whose snippets part ways is split between them: each part
// is undone where its snippet is, and the part left behind still is.
TEST(SessionTest, ASnippetSentAwayTakesItsPartOfAGroupMove) {
    Session session;
    const CanvasId canvas = session.Manager().CurrentCanvasId();
    const ItemId a = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = Model(session).CreateItem(false, Rect{200, 0, 100, 100}, "B");
    const CanvasId elsewhere = session.AddCanvas("Elsewhere");
    ASSERT_TRUE(session.SetRects({{a, Rect{0, 50, 100, 100}}, {b, Rect{200, 50, 100, 100}}}));
    ASSERT_EQ(session.SendItemsTo({a}, elsewhere, /*copy=*/false).items.size(), 1u);

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(ItemById(session.Manager(), b)->rect.y, 0.0f) << "the part left here";
    EXPECT_FLOAT_EQ(ItemById(session.Manager(), a)->rect.y, 50.0f) << "went with its snippet";
    EXPECT_FALSE(session.CanUndo());

    session.SwitchToCanvas(elsewhere);
    ASSERT_TRUE(session.Undo().has_value()) << "the send";
    session.SwitchToCanvas(canvas);
    ASSERT_TRUE(session.Undo().has_value()) << "back with it, the part came back too";
    EXPECT_FLOAT_EQ(ItemById(session.Manager(), a)->rect.y, 0.0f);
}

// A group move whose middle snippet went on elsewhere and came back puts
// every snippet back where it stood, undone. Rejoined at the end, its part
// was undone out of turn, and two snippets swapped places in the stack.
TEST(SessionTest, AGroupMoveWhoseSnippetCameBackIsUndoneIntoTheOrderItLeft) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    std::vector<ItemId> ids;
    for (const char* name : {"X", "A", "Y", "B", "Z", "C"}) {
        ids.push_back(Model(session).CreateItem(false, Rect{0, 0, 100, 100}, name));
    }
    const ItemId a = ids[1];
    const ItemId b = ids[3];
    const ItemId c = ids[5];
    const CanvasId second = session.AddCanvas("Second");
    const CanvasId third = session.AddCanvas("Third");
    // A canvas's stack, bottom first, by name.
    const auto stack = [&session](CanvasId canvas) {
        std::string names;
        for (const Item& item : session.Manager().FindCanvas(canvas)->items) {
            names += item.name;
        }
        return names;
    };

    ASSERT_EQ(session.SendItemsTo({a, b, c}, second, /*copy=*/false).items.size(), 3u);
    session.SwitchToCanvas(second);
    ASSERT_EQ(session.SendItemsTo({b}, third, /*copy=*/false).items.size(), 1u);
    session.SwitchToCanvas(third);
    ASSERT_TRUE(session.Undo().has_value()) << "B's send taken back";
    session.SwitchToCanvas(second);
    ASSERT_TRUE(session.Undo().has_value()) << "the group move";
    EXPECT_EQ(stack(first), "XAYBZC");
    EXPECT_EQ(stack(second), "");
}

// A paste undone sends what it moved back with its history, and redone
// brings both again.
TEST(SessionTest, AnUndonePasteTakesTheHistoryBackAndARedoBringsItAgain) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    DrawStrokeInto(session, moved);
    const CanvasId second = session.AddCanvas("Second");
    session.SwitchToCanvas(second);
    ASSERT_EQ(session.Paste({moved}, /*cut=*/true).items.size(), 1u);

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FALSE(session.CanUndo()) << "the stroke went back with it";
    session.SwitchToCanvas(first);
    EXPECT_TRUE(session.CanUndo());
    session.SwitchToCanvas(second);
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StackIndex(session, second, moved), 0);

    ASSERT_TRUE(session.Undo().has_value()) << "the paste, again";
    session.SwitchToCanvas(first);
    const std::optional<Session::UndoStep> stroke = session.Undo();
    ASSERT_TRUE(stroke.has_value());
    EXPECT_EQ(stroke->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session.Manager(), moved)->strokes.empty());
}

// Sent back by an undo and changed where it landed, a snippet is not
// pasted again: the change is newer than the paste's redo, and ends it -
// as a new change ends any redo.
TEST(SessionTest, ASnippetChangedAfterItsPasteWasUndoneIsNotPastedAgain) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId stays = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Stays");
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    DrawStrokeInto(session, stays);
    const CanvasId second = session.AddCanvas("Second");
    session.SwitchToCanvas(second);
    session.Paste({moved}, /*cut=*/true);
    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_TRUE(session.CanRedo());

    session.SwitchToCanvas(first);
    DrawStrokeInto(session, moved);
    session.SwitchToCanvas(second);
    EXPECT_FALSE(session.CanRedo());

    session.SwitchToCanvas(first);
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_TRUE(ItemById(session.Manager(), moved)->strokes.empty()) << "the newest first";
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_TRUE(ItemById(session.Manager(), stays)->strokes.empty());
}

// The same when it is deleted, or moved on to a third canvas: whatever is
// done to a snippet ends the redo of a paste of it.
TEST(SessionTest, ASnippetDeletedOrMovedOnAfterItsPasteWasUndoneIsNotPastedAgain) {
    for (const bool movedOn : {false, true}) {
        SCOPED_TRACE(movedOn ? "moved on" : "deleted");
        Session session;
        const CanvasId first = session.Manager().CurrentCanvasId();
        const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
        const CanvasId second = session.AddCanvas("Second");
        const CanvasId third = session.AddCanvas("Third");
        session.SwitchToCanvas(second);
        session.Paste({moved}, /*cut=*/true);
        ASSERT_TRUE(session.Undo().has_value());

        if (movedOn) {
            session.SwitchToCanvas(third);
            ASSERT_EQ(session.Paste({moved}, /*cut=*/true).items.size(), 1u);
        } else {
            session.SwitchToCanvas(first);
            ASSERT_TRUE(session.DeleteItem(moved));
        }
        session.SwitchToCanvas(second);
        EXPECT_FALSE(session.CanRedo());
        EXPECT_FALSE(session.Redo().has_value());
    }
}

// Undone while the canvas it came from is deleted, a paste sends the
// snippet back into that canvas all the same - where restoring the canvas
// finds it - and says where it went. Redone, it comes out again.
TEST(SessionTest, AnUndonePasteGoesBackIntoItsDeletedCanvas) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    const CanvasId second = session.AddCanvas("Second");
    session.SwitchToCanvas(second);
    session.Paste({moved}, /*cut=*/true);
    ASSERT_TRUE(session.Delete(first));

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Paste);
    EXPECT_EQ(undone->intoDeletedCanvas, first);
    EXPECT_EQ(StackIndex(session, first, moved), 0);
    EXPECT_TRUE(session.Manager().IsItemDeleted(moved)) << "with the canvas it is on";

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StackIndex(session, second, moved), 0);
    EXPECT_FALSE(session.Manager().IsItemDeleted(moved));
}

// The canvas a paste came from deleted for good: there is nowhere to send
// the snippet back to, so the move is gone from the history at once - not
// found out at the undo - and the snippet is this canvas's for good. The
// undo reaches the step before.
TEST(SessionTest, APasteFromACanvasDeletedForGoodIsNoLongerUndone) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId moved = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    const CanvasId second = session.AddCanvas("Second");
    session.SwitchToCanvas(second);
    const ItemId made = session.CreateItem(false, Rect{0, 0, 100, 100}, "Made");
    DrawStrokeInto(session, made);
    session.Paste({moved}, /*cut=*/true);
    ASSERT_TRUE(session.Delete(first));
    ASSERT_TRUE(session.DeletePermanently(first));

    const std::optional<Session::UndoStep> next = session.Undo();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->what, Session::UndoWhat::Stroke);
    EXPECT_EQ(StackIndex(session, second, moved), 1) << "still where it was pasted";
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_TRUE(session.Manager().IsItemDeleted(made));
}

// A snippet of a paste moved on elsewhere takes its part of the paste
// with it; the rest of the paste is still one undo here.
TEST(SessionTest, ASnippetMovedOnTakesItsPartOfAPaste) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId one = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "One");
    const ItemId other = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Other");
    const CanvasId second = session.AddCanvas("Second");
    const CanvasId third = session.AddCanvas("Third");
    session.SwitchToCanvas(second);
    session.Paste({one, other}, /*cut=*/true);
    session.SwitchToCanvas(third);
    ASSERT_EQ(session.Paste({one}, /*cut=*/true).items.size(), 1u);
    session.SwitchToCanvas(second);

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StackIndex(session, first, other), 0) << "went back";
    EXPECT_EQ(StackIndex(session, third, one), 0) << "its part is the third canvas's";
    EXPECT_FALSE(session.CanUndo());

    session.SwitchToCanvas(third);
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StackIndex(session, second, one), 0);
    session.SwitchToCanvas(second);
    ASSERT_TRUE(session.Undo().has_value()) << "its part of the first paste came back with it";
    EXPECT_EQ(StackIndex(session, first, one), 0);
}

// A text edit, a placement and a style edit are gestures, and the next
// command of any other kind ends the one open - filed first, with what it
// had come to - so that nothing is filed in the middle of one.
TEST(SessionTest, TheNextCommandEndsTheGestureOpenAndFilesItFirst) {
    Session session;
    const ItemId note = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    session.BeginTextEdit(note);
    session.PreviewText("typed");
    EXPECT_EQ(ItemById(session.Manager(), note)->noteText, "typed") << "the note is what has been typed";
    DrawStrokeInto(session, note);
    EXPECT_FALSE(session.TextEditItem().has_value()) << "ended by the stroke";

    const std::optional<Session::UndoStep> stroke = session.Undo();
    ASSERT_TRUE(stroke.has_value());
    EXPECT_EQ(stroke->what, Session::UndoWhat::Stroke);
    const std::optional<Session::UndoStep> text = session.Undo();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->what, Session::UndoWhat::TextEdit);
    EXPECT_EQ(ItemById(session.Manager(), note)->noteText, "");

    session.BeginPlacement({note});
    session.PreviewRect(note, Rect{40, 40, 100, 100});
    ItemStyle style = ItemStyle::Of(*ItemById(session.Manager(), note));
    style.foregroundOpacity = 0.5f;
    session.PreviewStyle(note, style);
    session.EndStyleEdit();
    const std::optional<Session::UndoStep> styled = session.Undo();
    ASSERT_TRUE(styled.has_value());
    EXPECT_EQ(styled->what, Session::UndoWhat::Style);
    const std::optional<Session::UndoStep> placed = session.Undo();
    ASSERT_TRUE(placed.has_value());
    EXPECT_EQ(placed->what, Session::UndoWhat::Placement) << "the drag, ended by the style edit";
    EXPECT_EQ(ItemById(session.Manager(), note)->rect, (Rect{0, 0, 100, 100}));
}

// An undo ends the gesture open first, and then takes back what it was.
TEST(SessionTest, AnUndoMidGestureTakesBackTheGesture) {
    Session session;
    const ItemId note = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    session.BeginTextEdit(note);
    session.PreviewText("typed");
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::TextEdit);
    EXPECT_EQ(ItemById(session.Manager(), note)->noteText, "");
    EXPECT_FALSE(session.TextEditItem().has_value());
}

// Style changes are on the history, each a step of its own.
TEST(SessionTest, StyleChangesAreUndone) {
    Session session;
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemStyle start = ItemStyle::Of(*ItemById(session.Manager(), item));
    for (const float opacity : {0.9f, 0.8f, 0.7f}) {
        ItemStyle style = start;
        style.foregroundOpacity = opacity;
        ASSERT_TRUE(session.SetStyles({{item, style}}));
    }
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(ItemById(session.Manager(), item)->foregroundOpacity, 0.8f);
    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemStyle::Of(*ItemById(session.Manager(), item)), start);
    EXPECT_FALSE(session.CanUndo());
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_FLOAT_EQ(ItemById(session.Manager(), item)->foregroundOpacity, 0.9f);
}

// A style edit of several snippets - the opacity wheel spun over a
// selection - goes on while it is about the same ones, and is one step
// for all of them; a preview about others ends it, and a cancel puts every
// one back.
TEST(SessionTest, AStyleEditOfSeveralSnippetsIsOneStep) {
    Session session;
    const ItemId a = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = Model(session).CreateItem(false, Rect{200, 0, 100, 100}, "B");
    const ItemStyle start = ItemStyle::Of(*ItemById(session.Manager(), a));
    const auto faded = [&start](float opacity) {
        ItemStyle style = start;
        style.foregroundOpacity = opacity;
        return style;
    };
    for (const float opacity : {0.9f, 0.8f, 0.7f}) {
        session.PreviewStyles({{a, faded(opacity)}, {b, faded(opacity)}});
    }
    EXPECT_FALSE(session.CanUndo()) << "nothing filed while it goes on";
    ASSERT_TRUE(session.EndStyleEdit());
    EXPECT_FALSE(session.EndStyleEdit()) << "ended once";
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemStyle::Of(*ItemById(session.Manager(), a)), start);
    EXPECT_EQ(ItemStyle::Of(*ItemById(session.Manager(), b)), start);
    EXPECT_FALSE(session.CanUndo()) << "both, in the one step";

    session.PreviewStyles({{a, faded(0.5f)}, {b, faded(0.5f)}});
    session.PreviewStyles({{a, faded(0.4f)}});  // about other snippets: a step of its own
    session.CancelStyleEdit();
    EXPECT_FLOAT_EQ(ItemById(session.Manager(), a)->foregroundOpacity, 0.5f) << "back to where the second began";
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemStyle::Of(*ItemById(session.Manager(), a)), start);
    EXPECT_EQ(ItemStyle::Of(*ItemById(session.Manager(), b)), start);
}

// An erase gesture that has lost track of which fragment stands for which
// stroke still files what it did, as one replacement of the whole list:
// the strokes never change behind the history's back.
TEST(SessionTest, AnEraseThatLostTrackOfItsFragmentsIsStillUndoneExactly) {
    Session session;
    session.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const ItemId item = Model(session).CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 60.0f);
    const std::vector<Stroke> before = ItemById(session.Manager(), item)->strokes;
    session.BeginErase(item, 50.0f, 30.0f, 10.0f);
    // What only a bug could do mid-gesture: the strokes changed by
    // something other than the eraser.
    Model(session).FindItemAnywhere(item)->strokes.pop_back();
    session.ExtendErase(50.0f, 30.0f, 10.0f);
    session.EndErase();

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemById(session.Manager(), item)->strokes, before);
    ASSERT_TRUE(session.Undo().has_value()) << "and the strokes before it, as they were";
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f}));
}

// Every gesture can be called off, leaving the library exactly as the
// gesture found it - what Escape does in the middle of one. Nothing is
// filed, so there is nothing to undo.
TEST(SessionTest, ACanceledGestureLeavesNoTrace) {
    Session session;
    session.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    CanvasManager& manager = Model(session);
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = manager.CreateItem(false, Rect{200, 0, 100, 100}, "B");
    DrawLineInto(session, a, 30.0f);
    session.ToggleFullscreen(b, /*stretch=*/false);
    const std::vector<Stroke> strokes = ItemById(manager, a)->strokes;
    const Rect bFullscreen = ItemById(manager, b)->rect;
    const ItemStyle style = ItemStyle::Of(*ItemById(manager, a));

    // A drag of both, one of them taken out of fullscreen on the way.
    session.BeginPlacement({a, b});
    session.PreviewLeaveFullscreen(b);
    session.PreviewRect(a, Rect{40, 40, 100, 100});
    session.PreviewRect(b, Rect{240, 40, 100, 100});
    session.CancelPlacement();
    EXPECT_EQ(ItemById(manager, a)->rect, (Rect{0, 0, 100, 100}));
    EXPECT_TRUE(ItemById(manager, b)->isFullscreen);
    EXPECT_EQ(ItemById(manager, b)->rect, bFullscreen);

    session.BeginErase(a, 50.0f, 30.0f, 10.0f);
    session.ExtendErase(60.0f, 30.0f, 10.0f);
    ASSERT_NE(ItemById(manager, a)->strokes, strokes) << "the erase took something";
    session.CancelErase();
    EXPECT_EQ(ItemById(manager, a)->strokes, strokes);

    ItemStyle faded = style;
    faded.foregroundOpacity = 0.3f;
    session.PreviewStyle(a, faded);
    session.CancelStyleEdit();
    EXPECT_EQ(ItemStyle::Of(*ItemById(manager, a)), style);

    session.BeginShape(a, Session::Shape::Line, 10.0f, 10.0f, 0xFF0000FFu, 3.0f);
    session.UpdateShape(80.0f, 80.0f);
    session.CancelShape();
    EXPECT_TRUE(session.LiveLayer().Strokes().empty());
    EXPECT_EQ(ItemById(manager, a)->strokes, strokes);

    // Nothing of any of them was filed, and the gestures are closed: the
    // next command finds nothing open to file first.
    size_t filed = 0;
    while (session.Undo().has_value()) {
        ++filed;
    }
    EXPECT_EQ(filed, 2u) << "the stroke and the fullscreen from before, and nothing else";
}

}  // namespace
}  // namespace sz::core
