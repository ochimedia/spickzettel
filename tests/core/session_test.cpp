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

namespace sz::core {
namespace {

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
    Canvas* canvas = session.Manager().CurrentOrNull();
    ASSERT_NE(canvas, nullptr);
    canvas->liveLayer.BeginStroke(StrokePoint{10.0f, 10.0f}, 0xFF0000FFu, 3.0f);
    canvas->liveLayer.ExtendStroke(StrokePoint{60.0f, 60.0f});
    canvas->liveLayer.EndStroke();
    session.CommitLiveStroke(item);
}

// A horizontal stroke at height `y` across `item`, for the tests about
// which stroke is which: an item made at {0,0,100,100} has native space
// equal to screen space, so the y comes back out unchanged.
void DrawLineInto(Session& session, ItemId item, float y) {
    Canvas* canvas = session.Manager().CurrentOrNull();
    ASSERT_NE(canvas, nullptr);
    canvas->liveLayer.BeginStroke(StrokePoint{10.0f, y}, 0xFF0000FFu, 3.0f);
    canvas->liveLayer.ExtendStroke(StrokePoint{90.0f, y});
    canvas->liveLayer.EndStroke();
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
    EXPECT_TRUE(session.Manager().DeletedThings().empty());
    EXPECT_TRUE(session.Manager().HasCurrentCanvas());
    EXPECT_FALSE(session.CanUndo());
}

TEST(SessionTest, ADeletedCanvasStaysWhereItIsAndComesBack) {
    Session session;
    const CanvasId canvas = session.Manager().CurrentCanvasId();
    const FolderId folder = session.Manager().CurrentFolderId();

    ASSERT_TRUE(session.Delete(canvas));
    const Canvas* deleted = session.Manager().FindCanvas(canvas);
    ASSERT_NE(deleted, nullptr) << "marked, not moved anywhere";
    EXPECT_NE(deleted->deletedAt, 0);
    EXPECT_EQ(deleted->folderId, folder);
    EXPECT_FALSE(session.Manager().HasCurrentCanvas()) << "the only canvas in its folder, and hidden now";
    EXPECT_EQ(session.Manager().CurrentFolderId(), folder) << "still browsing where it was";

    ASSERT_TRUE(session.Restore(canvas));
    EXPECT_EQ(session.Manager().FindCanvas(canvas)->deletedAt, 0);
}

TEST(SessionTest, DeletingOrRestoringWhatIsNotThereDoesNothing) {
    Session session;
    EXPECT_FALSE(session.Delete(424242));
    EXPECT_FALSE(session.Restore(424242));
    EXPECT_EQ(session.DeletePermanently(424242), Session::Removal::NotFound);
    EXPECT_FALSE(session.Restore(session.Manager().CurrentCanvasId())) << "nothing deleted to restore";
}

TEST(SessionTest, AStrokeIsUndoneAndRedone) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    ASSERT_EQ(ItemById(session.Manager(), item)->strokes.size(), 1u);
    ASSERT_TRUE(session.CanUndo());

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(undone->undone);
    EXPECT_TRUE(ItemById(session.Manager(), item)->strokes.empty());
    EXPECT_FALSE(session.CanUndo());
    ASSERT_TRUE(session.CanRedo());

    const std::optional<Session::UndoStep> redone = session.Redo();
    ASSERT_TRUE(redone.has_value());
    EXPECT_FALSE(redone->undone);
    EXPECT_EQ(ItemById(session.Manager(), item)->strokes.size(), 1u);
}

TEST(SessionTest, ADeletedSnippetIsRestoredOnUndo) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    ASSERT_TRUE(session.DeleteItem(item));
    ASSERT_NE(ItemById(session.Manager(), item), nullptr) << "marked where it is";
    EXPECT_TRUE(session.Manager().IsItemDeleted(item));
    EXPECT_FALSE(session.DeleteItem(item)) << "a delete of something deleted is for good, and not this call";

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Delete);
    EXPECT_FALSE(session.Manager().IsItemDeleted(item));

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_TRUE(session.Manager().IsItemDeleted(item));
}

TEST(SessionTest, AnOpenTextEditIsNotUndoneFromUnderIt) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    session.BeginTextEdit(item);
    session.EndTextEdit(std::string("first"));
    EXPECT_EQ(ItemById(session.Manager(), item)->noteText, "first");

    // Undone with the note open: nothing happens, and - as for any entry
    // that no longer applies - it is dropped rather than kept for later.
    session.BeginTextEdit(item);
    EXPECT_FALSE(session.Undo().has_value()) << "the edit in progress would overwrite it anyway";
    session.EndTextEdit(std::nullopt);  // abandoned: nothing filed
    EXPECT_EQ(ItemById(session.Manager(), item)->noteText, "first");
    EXPECT_FALSE(session.CanUndo());

    // Undone with the note closed, it goes back.
    session.BeginTextEdit(item);
    session.EndTextEdit(std::string("second"));
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::TextEdit);
    EXPECT_EQ(ItemById(session.Manager(), item)->noteText, "first");
}

TEST(SessionTest, ClearingADrawingIsOneStep) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    DrawStrokeInto(session, item);
    ASSERT_TRUE(session.ClearDrawing(item));
    EXPECT_TRUE(ItemById(session.Manager(), item)->strokes.empty());
    EXPECT_FALSE(session.ClearDrawing(item)) << "nothing left to clear";

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Erase);
    EXPECT_EQ(ItemById(session.Manager(), item)->strokes.size(), 2u);
}

// ===== Erase history keeps the draw order =====

TEST(SessionTest, UndoingAnEraseRestoresTheStrokeWhereItWas) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    session.EraseRect(item, 0.0f, 20.0f, 100.0f, 40.0f);  // the first stroke, wholly
    ASSERT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{100.0f}));

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 100.0f}))
        << "back where it was, not at the end";

    // The next undo is of the second stroke, and takes off the second
    // stroke - not whichever one happens to be last.
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f}));

    ASSERT_TRUE(session.Redo().has_value());
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{100.0f}));
    EXPECT_FALSE(session.CanRedo());
}

TEST(SessionTest, FragmentsOfAnErasedStrokeStandWhereItStood) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    session.EraseRect(item, 40.0f, 0.0f, 60.0f, 50.0f);  // the middle out of the first stroke
    ASSERT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 30.0f, 100.0f}))
        << "two fragments, in the erased stroke's place";

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 100.0f}));
    EXPECT_EQ(ItemById(session.Manager(), item)->strokes[0].points.size(), 2u) << "the whole original";

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 30.0f, 100.0f}));
}

TEST(SessionTest, AStrokeClippedTwiceInOneDragComesBackWhole) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    // One drag, two bites out of the first stroke - the second bite clips a
    // fragment the first one left.
    session.BeginErase(item, 30.0f, 30.0f, 10.0f);
    session.ExtendErase(70.0f, 30.0f, 10.0f);
    session.EndErase();
    ASSERT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 30.0f, 30.0f, 100.0f}));

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 100.0f}));
    EXPECT_EQ(ItemById(session.Manager(), item)->strokes[0].points.size(), 2u) << "as before the drag";
    EXPECT_TRUE(session.CanRedo()) << "one entry for the drag";
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 30.0f, 30.0f, 100.0f}));
}

TEST(SessionTest, EqualStrokesAreToldApartByPosition) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 30.0f);
    DrawLineInto(session, item, 100.0f);
    DrawLineInto(session, item, 30.0f);  // equal to the first, by value
    session.EraseRect(item, 0.0f, 20.0f, 100.0f, 40.0f);  // both equal strokes, wholly
    ASSERT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{100.0f}));

    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 100.0f, 30.0f}))
        << "each equal stroke back in its own place";
    ASSERT_TRUE(session.Undo().has_value()) << "the third stroke";
    EXPECT_EQ(StrokeHeights(session.Manager(), item), (std::vector<float>{30.0f, 100.0f}));
}

// Nothing leaves the library when a canvas is deleted, so nothing of its
// history does either: restored, it can be undone into as before.
TEST(SessionTest, ACanvasDeletedAndRestoredKeepsItsHistory) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    ASSERT_TRUE(session.CanUndo());

    const CanvasId second = session.Manager().AddCanvas("Second");
    session.Manager().SwitchToCanvas(second);
    ASSERT_TRUE(session.Delete(first));
    ASSERT_TRUE(session.Restore(first));
    session.Manager().SwitchToCanvas(first);
    ASSERT_EQ(session.Manager().CurrentCanvasId(), first);
    EXPECT_TRUE(session.CanUndo());
}

TEST(SessionTest, AnItemMovedAwayTakesNoHistoryWithIt) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    session.ForgetHistoryOfItem(first, item);
    EXPECT_FALSE(session.CanUndo());
}

TEST(SessionTest, RestoringASnippetRestoresWhatHoldsIt) {
    Session session;
    const FolderId folder = session.Manager().CurrentFolderId();
    const CanvasId canvas = session.Manager().CurrentCanvasId();
    const ItemId kept = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    const ItemId back = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Back");
    ASSERT_TRUE(session.Delete(kept));
    ASSERT_TRUE(session.Delete(folder));
    EXPECT_TRUE(session.Manager().IsItemDeleted(back)) << "inside a deleted folder";

    ASSERT_TRUE(session.Restore(back));
    EXPECT_FALSE(session.Manager().IsItemDeleted(back));
    EXPECT_EQ(session.Manager().FindCanvas(canvas)->deletedAt, 0);
    EXPECT_EQ(session.Manager().FindFolder(folder)->deletedAt, 0) << "the folder came back with it";
    EXPECT_TRUE(session.Manager().IsItemDeleted(kept)) << "deleted on its own before, and still";
}

TEST(SessionTest, CapturingWithoutAWindowLeavesAPlaceholder) {
    Session session;
    const ItemId id = session.Manager().CreateItem(/*hasBackground=*/true, Rect{0, 0, 100, 100}, "Shot");
    Item* item = session.Manager().FindItemAnywhere(id);
    ASSERT_NE(item, nullptr);
    session.CaptureShotItem(*item);
    ASSERT_NE(item->ImageLayer(), nullptr);
    EXPECT_EQ(item->ImageLayer()->textureHandle, 0u);
    EXPECT_TRUE(item->ImageLayer()->imageFile.empty());
    EXPECT_EQ(session.FrozenScreenTexture(), 0u);
}

// Drags a shape from (10, 10) to (x, y), the way the mouse does.
void DragShape(Session& session, ItemId item, Session::Shape shape, float x, float y) {
    session.BeginShape(item, shape, 10.0f, 10.0f, 0xFF0000FFu, 3.0f, /*paintPixels=*/false);
    session.UpdateShape((10.0f + x) * 0.5f, (10.0f + y) * 0.5f);
    session.UpdateShape(x, y);
    session.EndShape(x, y);
}

TEST(SessionTest, AShapeIsBakedAsOneStrokeAndUndoneInOneStep) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DragShape(session, item, Session::Shape::Rectangle, 80.0f, 60.0f);

    const Item* baked = ItemById(session.Manager(), item);
    ASSERT_EQ(baked->strokes.size(), 1u);
    EXPECT_EQ(baked->strokes[0].points.size(), 5u) << "an outline closed back on its corner";
    const CanvasState& live = session.Manager().CurrentOrNull()->liveLayer;
    EXPECT_TRUE(live.Strokes().empty());
    EXPECT_FALSE(live.ActiveStroke().has_value());
    EXPECT_FALSE(session.IsDrawingShape());

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session.Manager(), item)->strokes.empty());
}

TEST(SessionTest, AShapeCanChangeWhileItIsDragged) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    session.BeginShape(item, Session::Shape::Rectangle, 10.0f, 10.0f, 0xFF0000FFu, 3.0f, false);
    session.UpdateShape(80.0f, 60.0f);
    session.SetShape(Session::Shape::Line);
    const CanvasState& live = session.Manager().CurrentOrNull()->liveLayer;
    ASSERT_TRUE(live.ActiveStroke().has_value());
    EXPECT_EQ(live.ActiveStroke()->points.size(), 2u) << "the preview follows at once";

    session.EndShape(80.0f, 60.0f);
    const Item* baked = ItemById(session.Manager(), item);
    ASSERT_EQ(baked->strokes.size(), 1u);
    EXPECT_EQ(baked->strokes[0].points.size(), 2u);
}

TEST(SessionTest, AShapeTooShortToBeMeantLeavesNothing) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DragShape(session, item, Session::Shape::Line, 15.0f, 15.0f);
    EXPECT_TRUE(ItemById(session.Manager(), item)->strokes.empty());
    EXPECT_FALSE(session.Manager().CurrentOrNull()->liveLayer.ActiveStroke().has_value());
    EXPECT_FALSE(session.CanUndo());
}

TEST(SessionTest, MakingASnippetIsUndoneIntoDeletedAndRedoneOutOfIt) {
    Session session;
    const ItemId item = session.CreateItem(/*hasBackground=*/true, Rect{0, 0, 100, 100}, "Shot");
    ASSERT_NE(ItemById(session.Manager(), item), nullptr);
    ASSERT_TRUE(session.CanUndo());

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Create);
    ASSERT_NE(ItemById(session.Manager(), item), nullptr) << "where a capture taken by mistake can still be found";
    EXPECT_TRUE(session.Manager().IsItemDeleted(item));

    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_FALSE(session.Manager().IsItemDeleted(item));
}

TEST(SessionTest, AnUntouchedSnippetIsDiscardedWithoutATrace) {
    Session session;
    const ItemId kept = session.CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    DrawStrokeInto(session, kept);
    const ItemId empty = session.CreateItem(false, Rect{200, 0, 100, 100}, "Empty");

    EXPECT_TRUE(session.DiscardIfUntouched(empty));
    EXPECT_EQ(ItemById(session.Manager(), empty), nullptr) << "erased, not marked deleted";
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
    EXPECT_NE(ItemById(session.Manager(), drawn), nullptr);
    EXPECT_NE(ItemById(session.Manager(), noted), nullptr);
    EXPECT_NE(ItemById(session.Manager(), shot), nullptr);
}

// Deleting a snippet for good takes its own entries with it, and nothing
// else of its canvas's history.
TEST(SessionTest, DeletingASnippetPermanentlyLeavesItsCanvasHistoryAlone) {
    Session session;
    const ItemId kept = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Kept");
    DrawStrokeInto(session, kept);
    const ItemId gone = session.Manager().CreateItem(false, Rect{200, 0, 100, 100}, "Gone");
    ASSERT_TRUE(session.Delete(gone));
    ASSERT_EQ(session.DeletePermanently(gone), Session::Removal::Removed);
    EXPECT_EQ(ItemById(session.Manager(), gone), nullptr);
    EXPECT_TRUE(session.CanUndo());
}

// With the screen frozen, a shot is cut out of the frozen picture rather
// than captured again - the live screen has moved on, and the user framed
// what they were looking at. The cut is a plain sub-rectangle, saved to
// the library like any other capture's pixels.
TEST(SessionTest, AShotIsCutOutOfTheFrozenScreen) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_frozen_cut";
    std::filesystem::remove_all(dir);
    persistence::LibraryStore store(dir);
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
    const ItemId id = session.Manager().CreateItem(true, Rect{1.0f, 1.0f, 2.0f, 2.0f}, "Shot");
    Item* shot = session.Manager().FindItemAnywhere(id);
    ASSERT_NE(shot, nullptr);
    session.CaptureShotItem(*shot);

    EXPECT_EQ(window.captureCallCount, 1) << "cut from the frozen screen, not captured again";
    ASSERT_NE(shot->ImageLayer(), nullptr);
    EXPECT_EQ(shot->ImageLayer()->textureHandle, 8u) << "the cut's own upload";
    ASSERT_FALSE(shot->ImageLayer()->imageFile.empty());
    const std::optional<persistence::DecodedImage> saved = store.LoadImage(id, shot->ImageLayer()->imageFile);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->width, 2);
    EXPECT_EQ(saved->height, 2);
    const std::vector<uint8_t> expected{5, 5, 5, 255, 6, 6, 6, 255, 9, 9, 9, 255, 10, 10, 10, 255};
    EXPECT_EQ(saved->pixelsRGBA, expected);

    // Without the upload the cut is dropped and the screen is captured
    // live, as it is with nothing frozen at all.
    window.createTextureFromPixelsReturnsHandle = 0;
    const ItemId again = session.Manager().CreateItem(true, Rect{0.0f, 0.0f, 1.0f, 1.0f}, "Again");
    session.CaptureShotItem(*session.Manager().FindItemAnywhere(again));
    EXPECT_EQ(window.captureCallCount, 2);
    EXPECT_FLOAT_EQ(window.lastCaptureRect.w, 1.0f);

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}

// A screenshot is the one thing in the library that cannot be remade, so
// a capture whose picture could not be written at capture time keeps its
// pixels and is written by the next save that can - and no save counts as
// landed until it has.
TEST(SessionTest, ACaptureWhosePictureCouldNotBeWrittenIsWrittenByTheNextSave) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_pending_capture";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "images") << "a file where the staging directory wants to be";
    persistence::LibraryStore store(dir);
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 7;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);

    const ItemId id = session.Manager().CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    session.CaptureShotItem(*session.Manager().FindItemAnywhere(id));
    const Item* shot = session.Manager().FindItemAnywhere(id);
    EXPECT_EQ(shot->ImageLayer()->textureHandle, 7u) << "on screen as captured";
    EXPECT_TRUE(shot->ImageLayer()->imageFile.empty()) << "but not on disk";
    EXPECT_TRUE(session.HasUnsavedChanges());

    // The records land; the picture does not, so the save does not count.
    EXPECT_FALSE(session.Flush());
    EXPECT_TRUE(session.HasUnsavedChanges());
    EXPECT_TRUE(session.LastSaveFailed());

    // Now the snippet has a directory of its own to be written into.
    EXPECT_TRUE(session.Flush());
    EXPECT_FALSE(session.HasUnsavedChanges());
    EXPECT_FALSE(session.LastSaveFailed());
    shot = session.Manager().FindItemAnywhere(id);
    ASSERT_FALSE(shot->ImageLayer()->imageFile.empty());

    persistence::LibraryStore reopened(dir);
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
    EXPECT_EQ(reloaded->ImageLayer()->imageFile, shot->ImageLayer()->imageFile);
    const std::optional<persistence::DecodedImage> saved = reopened.LoadImage(id, reloaded->ImageLayer()->imageFile);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->pixelsRGBA, window.captureReturnsPixelsRGBA);

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}

// A recovery copy is a library that opens on its own: every picture a
// record in it names is in it, whether the session still held the pixels
// or had to read them back from the real library - and one that could not
// be made whole says so.
TEST(SessionTest, ARecoveryCopyHoldsEveryPictureItsRecordsName) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "spickzettel_session_test_recovery";
    const std::filesystem::path whole = dir.parent_path() / "spickzettel_session_test_recovery_whole";
    const std::filesystem::path partial = dir.parent_path() / "spickzettel_session_test_recovery_partial";
    for (const auto& path : {dir, whole, partial}) {
        std::filesystem::remove_all(path);
    }
    persistence::LibraryStore store(dir);
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 7;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    const ItemId id = session.Manager().CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    session.CaptureShotItem(*session.Manager().FindItemAnywhere(id));
    ASSERT_TRUE(session.Flush());
    const std::string onDisk = session.Manager().FindItemAnywhere(id)->ImageLayer()->imageFile;
    ASSERT_FALSE(onDisk.empty()) << "on disk in the real library, not in memory";

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
    const std::optional<persistence::DecodedImage> picture = recovered.LoadImage(copy->id, copy->ImageLayer()->imageFile);
    ASSERT_TRUE(picture.has_value()) << "names a picture the copy does not hold";
    EXPECT_EQ(picture->pixelsRGBA, window.captureReturnsPixelsRGBA);
    EXPECT_TRUE(std::filesystem::is_regular_file(whole / "recovery.txt"));

    // The real library's picture gone: the copy cannot be whole, and says so.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().filename() == onDisk) {
            std::filesystem::remove(entry.path());
        }
    }
    EXPECT_FALSE(session.WriteRecoveryCopy(partial));
    {
        std::ifstream note(partial / "recovery.txt");
        const std::string text((std::istreambuf_iterator<char>(note)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("Incomplete"), std::string::npos) << text;
    }

    session.SetLibraryStore(nullptr);
    for (const auto& path : {dir, whole, partial}) {
        std::filesystem::remove_all(path);
    }
}

// A copy taken of a capture whose write has not landed has the session's
// pixels to copy from, not a file - and must not come out pictureless.
TEST(SessionTest, ACopyOfACaptureStillWaitingToBeWrittenGetsItsOwnPicture) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_copy_of_pending";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "images") << "a file where the staging directory wants to be";
    persistence::LibraryStore store(dir);
    test::FakeOverlayWindow window;
    window.captureReturnsHandle = 7;
    window.captureReturnsWidth = 2;
    window.captureReturnsHeight = 1;
    window.captureReturnsPixelsRGBA = {10, 20, 30, 255, 40, 50, 60, 255};
    window.createTextureFromPixelsReturnsHandle = 9;
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);

    const ItemId id = session.Manager().CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    session.CaptureShotItem(*session.Manager().FindItemAnywhere(id));
    ASSERT_TRUE(session.Manager().FindItemAnywhere(id)->ImageLayer()->imageFile.empty()) << "not on disk";

    const ItemId copyId = session.Manager().DuplicateItem(id);
    ASSERT_NE(copyId, 0u);
    EXPECT_TRUE(session.CloneShotImageForCopy(id, copyId)) << "the pixels are in the session";
    EXPECT_NE(session.Manager().FindItemAnywhere(copyId)->ImageLayer()->textureHandle, 0u) << "on screen at once";

    EXPECT_FALSE(session.Flush()) << "staging is still blocked";
    EXPECT_TRUE(session.Flush()) << "both snippets have directories of their own now";
    persistence::LibraryStore reopened(dir);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    size_t pictures = 0;
    for (const Canvas& canvas : loaded->canvases) {
        for (const Item& item : canvas.items) {
            ASSERT_FALSE(item.ImageLayer()->imageFile.empty()) << "every copy names a picture";
            const std::optional<persistence::DecodedImage> saved =
                reopened.LoadImage(item.id, item.ImageLayer()->imageFile);
            ASSERT_TRUE(saved.has_value());
            EXPECT_EQ(saved->pixelsRGBA, window.captureReturnsPixelsRGBA);
            ++pictures;
        }
    }
    EXPECT_EQ(pictures, 2u);

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}

// A source whose picture is gone from the disk gives its copy nothing,
// and says so, rather than quietly producing a copy that looks captured.
TEST(SessionTest, ACopyOfACaptureWhosePictureCannotBeReadSaysSo) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_copy_unreadable";
    std::filesystem::remove_all(dir);
    persistence::LibraryStore store(dir);
    Session session;
    session.SetLibraryStore(&store);
    const ItemId id = session.Manager().CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
    session.Manager().FindItemAnywhere(id)->ImageLayer()->imageFile = "gone.qoi";

    const ItemId copyId = session.Manager().DuplicateItem(id);
    ASSERT_NE(copyId, 0u);
    EXPECT_FALSE(session.CloneShotImageForCopy(id, copyId));
    EXPECT_TRUE(session.Manager().FindItemAnywhere(copyId)->ImageLayer()->imageFile.empty());

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}

#if defined(_WIN32)
// Windows refuses to delete a file another handle holds open without
// delete sharing - which is what a picture viewer looking at a capture
// does. A permanent delete that meets one must not report a clean delete.
TEST(SessionTest, APermanentDeleteThatLeavesFilesBehindSaysSoAndFinishesLater) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_files_remain";
    std::filesystem::remove_all(dir);
    persistence::LibraryStore store(dir);
    Session session;
    session.SetLibraryStore(&store);
    const ItemId gone = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Gone");
    session.Flush();

    std::filesystem::path record;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().filename() == "item.json") {
            record = entry.path();
        }
    }
    ASSERT_FALSE(record.empty());
    {
        std::ifstream held(record);  // held open, no delete sharing
        ASSERT_TRUE(held.is_open());
        EXPECT_EQ(session.DeletePermanently(gone), Session::Removal::FilesRemain);
        EXPECT_EQ(ItemById(session.Manager(), gone), nullptr) << "gone from the library all the same";
        EXPECT_TRUE(std::filesystem::exists(record));

        // A save meanwhile counts - the records and the intent are on disk -
        // but the removal stays owed, so the autosave keeps asking.
        EXPECT_TRUE(session.Flush());
        EXPECT_TRUE(session.HasUnsavedChanges()) << "a removal is still owed";
        EXPECT_TRUE(std::filesystem::exists(record));
    }
    // Let go, the autosave's own clock finishes the delete with no edit
    // to prompt it - and sets nothing aside. The first tick is the one
    // that notices the generation moved; the quiet time counts from there.
    session.Tick(0.0f);
    session.Tick(3.0f);
    EXPECT_FALSE(session.HasUnsavedChanges());
    EXPECT_FALSE(std::filesystem::exists(record.parent_path()));
    EXPECT_FALSE(std::filesystem::exists(dir / "retired"));

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}
#endif

}  // namespace
}  // namespace sz::core
