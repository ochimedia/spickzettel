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
    EXPECT_EQ(session.Manager().DeletedFolderAndCanvasCount(), 0u);
    EXPECT_TRUE(session.Manager().MarkedSnippets().empty());
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
    CanvasManager& manager = session.Manager();
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
    ItemId shot = 0;
    {
        persistence::LibraryStore store(dir);
        test::FakeOverlayWindow window;
        window.captureReturnsHandle = 7;
        window.captureReturnsWidth = 1;
        window.captureReturnsHeight = 1;
        window.captureReturnsPixelsRGBA = {10, 20, 30, 255};
        Session session;
        session.AttachWindow(&window);
        session.SetLibraryStore(&store);
        shot = session.Manager().CreateItem(true, Rect{0.0f, 0.0f, 1.0f, 1.0f}, "Shot");
        session.CaptureShotItem(*session.Manager().FindItemAnywhere(shot));
        const ItemId gone = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Gone");
        ASSERT_TRUE(session.Delete(gone));
        ASSERT_TRUE(session.Flush());
        session.SetLibraryStore(nullptr);
    }

    persistence::LibraryStore store(dir);
    std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    test::FakeOverlayWindow window;
    window.createTextureFromPixelsReturnsHandle = 0;  // no device yet
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    session.ImportLibrary(std::move(*loaded));
    ASSERT_TRUE(session.Manager().MarkedSnippets().empty()) << "the deleted snippet was erased";

    window.createTextureFromPixelsReturnsHandle = 9;  // the window is made, and the first frame runs
    session.EnsureTexturesForCurrentCanvas();
    const Item* item = session.Manager().FindItemAnywhere(shot);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->ImageLayer()->textureHandle, 9u) << "not a placeholder until the canvas is switched";

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}

// A painted layer whose file was there but could not be read when its
// canvas came back - held by another program, say; here, unreadable for a
// while. Painting must not start the layer over from blank, which the next
// save would write over the drawing; it waits until the file reads again,
// and then paints onto what is in it.
TEST(SessionTest, PaintingNeverStartsOverALayerWhoseFileCouldNotBeRead) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_paint_unreadable";
    std::filesystem::remove_all(dir);
    persistence::LibraryStore store(dir);
    test::FakeOverlayWindow window;
    window.createTextureFromPixelsReturnsHandle = 9;
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);
    const CanvasId home = session.Manager().CurrentCanvasId();
    const ItemId drawing = session.Manager().CreateItem(false, Rect{0.0f, 0.0f, 32.0f, 32.0f}, "Drawing");
    session.BeginPaint(drawing, 4.0f, 4.0f, 0xFF0000FFu, 4.0f);
    session.ExtendPaint(12.0f, 4.0f);
    session.EndPaint();
    ASSERT_TRUE(session.Flush());
    const auto paintedLayer = [&]() -> Layer& {
        return *Session::FindPaintedLayer(*session.Manager().FindItemAnywhere(drawing));
    };
    const std::vector<uint8_t> first = paintedLayer().painted->PixelsRGBA();
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        file = entry.path().filename() == paintedLayer().imageFile ? entry.path() : file;
    }
    ASSERT_FALSE(file.empty());

    // Away, so its pixels are let go of; the file unreadable; and back.
    session.Manager().SwitchToCanvas(session.Manager().AddCanvas("Elsewhere"));
    session.EnsureTexturesForCurrentCanvas();
    ASSERT_EQ(paintedLayer().painted, nullptr);
    const std::string saved = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "not a picture";
    session.Manager().SwitchToCanvas(home);
    session.EnsureTexturesForCurrentCanvas();
    ASSERT_EQ(paintedLayer().painted, nullptr) << "could not be read";

    session.BeginPaint(drawing, 4.0f, 20.0f, 0xFF0000FFu, 4.0f);
    session.EndPaint();
    EXPECT_EQ(paintedLayer().painted, nullptr) << "painted into blank pixels in its place";
    ASSERT_TRUE(session.Flush());

    // Readable again: painting goes onto the drawing that is there.
    std::ofstream(file, std::ios::binary | std::ios::trunc) << saved;
    session.BeginPaint(drawing, 4.0f, 20.0f, 0xFF0000FFu, 4.0f);
    session.EndPaint();
    ASSERT_NE(paintedLayer().painted, nullptr);
    ASSERT_TRUE(session.Flush());
    const std::optional<persistence::DecodedImage> onDisk = store.LoadImage(drawing, paintedLayer().imageFile);
    ASSERT_TRUE(onDisk.has_value());
    ASSERT_EQ(onDisk->pixelsRGBA.size(), first.size());
    for (size_t i = 3; i < first.size(); i += 4) {
        if (first[i] != 0) {
            ASSERT_NE(onDisk->pixelsRGBA[i], 0) << "the first stroke was painted over with blank";
        }
    }
    EXPECT_NE(onDisk->pixelsRGBA, first) << "and the second stroke is there too";

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}

// The retention period: what has been deleted since before the cutoff goes
// for good, whatever else is deleted stays, and nothing live is touched.
TEST(SessionTest, ErasingWhatWasDeletedBeforeACutoffLeavesTheRest) {
    Session session;
    CanvasManager& manager = session.Manager();
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
    const FolderId folder = session.Manager().CurrentFolderId();
    const CanvasId first = session.Manager().CurrentCanvasId();
    const CanvasId second = session.Manager().AddCanvas("Second");
    const CanvasId third = session.Manager().AddCanvas("Third");
    EXPECT_EQ(session.DeleteMarkedCanvasesPermanently(folder), Session::Removal::NotFound) << "nothing deleted in it";
    ASSERT_TRUE(session.Delete(first));
    ASSERT_TRUE(session.Delete(third));

    EXPECT_EQ(session.DeleteMarkedCanvasesPermanently(folder), Session::Removal::Removed);
    EXPECT_EQ(session.Manager().FindCanvas(first), nullptr);
    EXPECT_EQ(session.Manager().FindCanvas(third), nullptr);
    ASSERT_NE(session.Manager().FindFolder(folder), nullptr);
    EXPECT_NE(session.Manager().FindCanvas(second), nullptr);
}

// A placement change is one entry for everything it moved, taken back and
// put back whole - fullscreen state and anchor with the rect - and nothing
// at all is filed for a change that changed nothing.
TEST(SessionTest, APlacementChangeIsUndoneAndRedoneWhole) {
    Session session;
    CanvasManager& manager = session.Manager();
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = manager.CreateItem(false, Rect{200, 0, 100, 100}, "B");
    ASSERT_FALSE(session.CanUndo());

    EXPECT_FALSE(session.RecordPlacements(session.PlacementsOf({a, b}))) << "nothing changed";
    EXPECT_FALSE(session.CanUndo());

    std::vector<Session::Placement> before = session.PlacementsOf({a, b});
    manager.FindItemAnywhere(a)->rect = Rect{10, 20, 100, 100};
    manager.FindItemAnywhere(b)->rect = Rect{210, 20, 100, 100};
    ASSERT_TRUE(session.RecordPlacements(std::move(before)));

    std::vector<Session::Placement> beforeFullscreen = session.PlacementsOf({a});
    manager.ToggleFullscreen(a, 1920.0f, 1080.0f, /*stretch=*/true);
    ASSERT_TRUE(session.RecordPlacements(std::move(beforeFullscreen)));

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
    CanvasManager& manager = session.Manager();
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    for (int i = 1; i <= 3; ++i) {
        std::vector<Session::Placement> before = session.PlacementsOf({a});
        manager.FindItemAnywhere(a)->rect.x = static_cast<float>(i);
        ASSERT_TRUE(session.RecordPlacements(std::move(before), /*merge=*/true));
    }
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.x, 0.0f) << "the whole burst, at once";
    EXPECT_FALSE(session.CanUndo());

    ASSERT_TRUE(session.Redo().has_value());
    const ItemId b = session.CreateItem(false, Rect{300, 0, 50, 50}, "B");  // something else filed
    ASSERT_NE(b, 0u);
    std::vector<Session::Placement> before = session.PlacementsOf({a});
    manager.FindItemAnywhere(a)->rect.x = 9.0f;
    ASSERT_TRUE(session.RecordPlacements(std::move(before), /*merge=*/true));
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.x, 3.0f) << "a new entry, not merged past the one between";
}

// A snippet that leaves the canvas takes its part of a group's move with
// it; the others' part stays to be taken back.
TEST(SessionTest, ForgettingOneSnippetLeavesTheRestOfAGroupMove) {
    Session session;
    CanvasManager& manager = session.Manager();
    const CanvasId canvas = manager.CurrentCanvasId();
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const ItemId b = manager.CreateItem(false, Rect{200, 0, 100, 100}, "B");
    std::vector<Session::Placement> before = session.PlacementsOf({a, b});
    manager.FindItemAnywhere(a)->rect.y = 50.0f;
    manager.FindItemAnywhere(b)->rect.y = 50.0f;
    ASSERT_TRUE(session.RecordPlacements(std::move(before)));

    session.ForgetHistoryOfItem(canvas, a);
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(a)->rect.y, 50.0f) << "forgotten";
    EXPECT_FLOAT_EQ(manager.FindItemAnywhere(b)->rect.y, 0.0f);

    session.ForgetHistoryOfItem(canvas, b);
    EXPECT_FALSE(session.CanRedo()) << "an entry naming nothing is gone";
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

// One Delete is one undo, however many snippets it took - more than the
// history holds entries included, where one entry each left the earliest
// beyond reach, and a deleted snippet only comes back by undo.
TEST(SessionTest, DeletingManySnippetsAtOnceIsOneUndo) {
    Session session;
    std::vector<ItemId> items;
    for (int i = 0; i < 60; ++i) {
        items.push_back(session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A"));
    }
    EXPECT_EQ(session.DeleteItems(items), 60u);
    EXPECT_EQ(session.DeleteItems(items), 0u) << "deleted already";

    ASSERT_TRUE(session.Undo().has_value());
    for (const ItemId item : items) {
        EXPECT_FALSE(session.Manager().IsItemDeleted(item));
    }
    EXPECT_FALSE(session.Undo().has_value()) << "one step, not sixty";

    ASSERT_TRUE(session.Redo().has_value());
    for (const ItemId item : items) {
        EXPECT_TRUE(session.Manager().IsItemDeleted(item));
    }
}

TEST(SessionTest, AnOpenTextEditIsNotUndoneFromUnderIt) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    session.BeginTextEdit(item);
    session.EndTextEdit(std::string("first"));
    EXPECT_EQ(ItemById(session.Manager(), item)->noteText, "first");

    // Undone with the note open: nothing happens, and the entry is kept -
    // unlike one that no longer applies, it still does, once the note is
    // closed.
    session.BeginTextEdit(item);
    EXPECT_FALSE(session.Undo().has_value()) << "the edit in progress would overwrite it anyway";
    EXPECT_EQ(ItemById(session.Manager(), item)->noteText, "first");
    EXPECT_TRUE(session.CanUndo()) << "kept for when the note is closed";
    session.EndTextEdit(std::nullopt);  // abandoned: nothing filed

    // Undone with the note closed, it goes back.
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::TextEdit);
    EXPECT_EQ(ItemById(session.Manager(), item)->noteText, "");
    EXPECT_FALSE(session.CanUndo());
}

// An erase over strokes and pixels both, undone while the pixels were not
// in memory: the strokes come back, and the pixels, once loaded again from
// what was saved, are not later swapped for the ones from before the erase.
TEST(SessionTest, AnEraseUndoneWithoutItsPixelsLeavesThemOutOfItsRedo) {
    test::FakeOverlayWindow window;
    window.createTextureFromPixelsReturnsHandle = 9;
    Session session;
    session.AttachWindow(&window);
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawStrokeInto(session, item);
    session.BeginPaint(item, 10.0f, 60.0f, 0xFF0000FFu, 6.0f);
    session.ExtendPaint(60.0f, 10.0f);
    session.EndPaint();
    session.BeginErase(item, 35.0f, 35.0f, 20.0f);
    session.EndErase();
    Layer* found = Session::FindPaintedLayer(*session.Manager().FindItemAnywhere(item));
    ASSERT_NE(found, nullptr);
    ASSERT_NE(found->painted, nullptr);
    Layer& layer = *found;
    const PaintedImage afterErase = *layer.painted;
    const size_t strokesAfterErase = ItemById(session.Manager(), item)->strokes.size();

    layer.painted.reset();
    ASSERT_TRUE(session.Undo().has_value()) << "the strokes are there to put back";
    EXPECT_NE(ItemById(session.Manager(), item)->strokes.size(), strokesAfterErase);

    layer.painted = std::make_shared<PaintedImage>(afterErase);  // as read back from disk
    ASSERT_TRUE(session.Redo().has_value());
    EXPECT_EQ(ItemById(session.Manager(), item)->strokes.size(), strokesAfterErase);
    EXPECT_EQ(layer.painted->PixelsRGBA(), afterErase.PixelsRGBA()) << "not the pixels from before the erase";
}

// An erase begun while another is still open ends that one first, filed
// whole: one undo puts back what each took.
TEST(SessionTest, AnEraseBegunOverAnOpenOneFilesThatOneFirst) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    DrawLineInto(session, item, 20.0f);
    DrawLineInto(session, item, 80.0f);
    const std::vector<Stroke> drawn = ItemById(session.Manager(), item)->strokes;

    session.BeginErase(item, 50.0f, 20.0f, 10.0f);
    session.BeginErase(item, 50.0f, 80.0f, 10.0f);
    session.EndErase();

    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_TRUE(session.Undo().has_value());
    EXPECT_EQ(ItemById(session.Manager(), item)->strokes, drawn);
}

// An eraser dragged over nothing but transparent pixels changes nothing,
// and files nothing: the next undo takes back what came before it.
TEST(SessionTest, AnErasePassThatChangesNothingIsNoStep) {
    test::FakeOverlayWindow window;
    window.createTextureFromPixelsReturnsHandle = 9;
    Session session;
    session.AttachWindow(&window);
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    session.BeginPaint(item, 10.0f, 10.0f, 0xFF0000FFu, 6.0f);
    session.EndPaint();
    Layer& layer = *Session::FindPaintedLayer(*session.Manager().FindItemAnywhere(item));
    layer.paintedDirty = false;  // as once saved

    session.BeginErase(item, 80.0f, 80.0f, 10.0f);
    session.ExtendErase(90.0f, 60.0f, 10.0f);
    session.EndErase();
    EXPECT_FALSE(layer.paintedDirty) << "nothing to write out again";

    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_NE(undone->what, Session::UndoWhat::Erase) << "the paint, not an erase of nothing";
    EXPECT_FALSE(session.CanUndo());
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

// A rectangle dragged flat is the line it looks like, not an outline that
// goes out and back over itself.
TEST(SessionTest, AFlatRectangleIsALine) {
    Session session;
    const ItemId item = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "A");
    session.BeginShape(item, Session::Shape::Rectangle, 10.0f, 40.0f, 0xFF0000FFu, 3.0f, false);
    session.EndShape(80.0f, 40.0f);
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
    const CanvasId first = session.Manager().CurrentCanvasId();
    session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Below");
    const ItemId moved = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Above");
    const CanvasId second = session.Manager().AddCanvas("Second");
    session.Manager().SwitchToCanvas(second);
    session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Resident");

    const std::optional<Session::Arrival> arrival = session.MoveItemHere(moved);
    ASSERT_TRUE(arrival.has_value());
    session.RecordArrivals({*arrival}, /*duplicate=*/false);
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
        const CanvasId first = session.Manager().CurrentCanvasId();
        std::vector<ItemId> stack;
        for (const char* name : {"A", "B", "C", "D"}) {
            stack.push_back(session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, name));
        }
        const CanvasId second = session.Manager().AddCanvas("Second");
        session.Manager().SwitchToCanvas(second);
        std::vector<Session::Arrival> arrivals;
        for (const size_t at : cutOrder) {
            arrivals.push_back(*session.MoveItemHere(stack[at]));
        }
        session.RecordArrivals(std::move(arrivals), /*duplicate=*/false);

        ASSERT_TRUE(session.Undo().has_value());
        std::vector<ItemId> after;
        for (const Item& item : session.Manager().FindCanvas(first)->items) {
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
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId stays = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Stays");
    const ItemId moved = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    DrawStrokeInto(session, stays);
    const CanvasId second = session.Manager().AddCanvas("Second");
    session.Manager().SwitchToCanvas(second);
    session.RecordArrivals({*session.MoveItemHere(moved)}, /*duplicate=*/false);
    ASSERT_TRUE(session.Undo().has_value());

    session.Manager().SwitchToCanvas(first);
    DrawStrokeInto(session, moved);
    session.Manager().SwitchToCanvas(second);
    ASSERT_TRUE(session.Redo().has_value());
    ASSERT_EQ(StackIndex(session, second, moved), 0);

    session.Manager().SwitchToCanvas(first);
    const std::optional<Session::UndoStep> undone = session.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->what, Session::UndoWhat::Stroke);
    EXPECT_TRUE(ItemById(session.Manager(), stays)->strokes.empty()) << "the stroke on what stayed";
    EXPECT_EQ(ItemById(session.Manager(), moved)->strokes.size(), 1u) << "not the one on what left";
}

// The canvas a cut came from, deleted - marked, or for good - before the
// paste is undone: the snippet stays where it was pasted rather than go
// somewhere nobody can see it, or nowhere at all, and the step says so
// and is dropped, so that the next undo reaches the step before it.
TEST(SessionTest, APasteWhoseSourceCanvasIsDeletedIsNotUndoneAndSaysSo) {
    for (const bool forGood : {false, true}) {
        SCOPED_TRACE(forGood ? "deleted for good" : "deleted");
        Session session;
        const CanvasId first = session.Manager().CurrentCanvasId();
        const ItemId moved = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
        const CanvasId second = session.Manager().AddCanvas("Second");
        session.Manager().SwitchToCanvas(second);
        const ItemId made = session.CreateItem(false, Rect{0, 0, 100, 100}, "Made");
        session.RecordArrivals({*session.MoveItemHere(moved)}, /*duplicate=*/false);
        ASSERT_TRUE(session.Delete(first));
        if (forGood) {
            ASSERT_EQ(session.DeletePermanently(first), Session::Removal::Removed);
        }

        const std::optional<Session::UndoStep> refused = session.Undo();
        ASSERT_TRUE(refused.has_value());
        EXPECT_TRUE(refused->refused);
        EXPECT_EQ(refused->what, Session::UndoWhat::Paste);
        EXPECT_EQ(StackIndex(session, second, moved), 1) << "still where it was pasted, on top";
        EXPECT_FALSE(session.Manager().IsItemDeleted(moved));

        const std::optional<Session::UndoStep> next = session.Undo();
        ASSERT_TRUE(next.has_value());
        EXPECT_EQ(next->what, Session::UndoWhat::Create);
        EXPECT_TRUE(session.Manager().IsItemDeleted(made));
    }
}

// Sent back by an undo, then deleted there: nothing to bring again, and
// the redo says so rather than moving a deleted snippet here.
TEST(SessionTest, ARedoOfAPasteWhoseSnippetWasDeletedSinceIsRefused) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId moved = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    const CanvasId second = session.Manager().AddCanvas("Second");
    session.Manager().SwitchToCanvas(second);
    session.RecordArrivals({*session.MoveItemHere(moved)}, /*duplicate=*/false);
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
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId moved = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Moved");
    const CanvasId second = session.Manager().AddCanvas("Second");
    const CanvasId third = session.Manager().AddCanvas("Third");
    session.Manager().SwitchToCanvas(second);
    session.RecordArrivals({*session.MoveItemHere(moved)}, /*duplicate=*/false);
    ASSERT_TRUE(session.Undo().has_value());
    ASSERT_EQ(session.Manager().CanvasHoldingItem(moved), std::optional<CanvasId>(first));
    session.Manager().SwitchToCanvas(third);
    session.RecordArrivals({*session.MoveItemHere(moved)}, /*duplicate=*/false);
    session.Manager().SwitchToCanvas(second);

    const std::optional<Session::UndoStep> refused = session.Redo();
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->refused);
    EXPECT_EQ(session.Manager().CanvasHoldingItem(moved), std::optional<CanvasId>(third)) << "left where it is";
    session.Manager().SwitchToCanvas(third);
    EXPECT_TRUE(session.CanUndo()) << "with the third canvas's history of it";
}

// Copies, pasted or duplicated, are undone into their deletion mark - as
// a new snippet is - and redone out of it; the source is not touched.
TEST(SessionTest, CopiesArriveAndGoWithOneUndo) {
    for (const bool duplicate : {false, true}) {
        SCOPED_TRACE(duplicate ? "duplicate" : "paste");
        Session session;
        const ItemId source = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Source");
        const ItemId copyA = session.Manager().DuplicateItem(source);
        const ItemId copyB = session.Manager().DuplicateItem(source);
        session.RecordArrivals({Session::Arrival{copyA}, Session::Arrival{copyB}}, duplicate);

        const std::optional<Session::UndoStep> undone = session.Undo();
        ASSERT_TRUE(undone.has_value());
        EXPECT_EQ(undone->what, duplicate ? Session::UndoWhat::Duplicate : Session::UndoWhat::Paste);
        EXPECT_TRUE(session.Manager().IsItemDeleted(copyA));
        EXPECT_TRUE(session.Manager().IsItemDeleted(copyB));
        EXPECT_FALSE(session.Manager().IsItemDeleted(source));

        ASSERT_TRUE(session.Redo().has_value());
        EXPECT_FALSE(session.Manager().IsItemDeleted(copyA));
        EXPECT_FALSE(session.Manager().IsItemDeleted(copyB));
    }
}

// A snippet of a paste moved on elsewhere since is taken out of the
// paste's entry; the rest of the paste is still one undo.
TEST(SessionTest, ForgettingOneSnippetLeavesTheRestOfAPaste) {
    Session session;
    const CanvasId first = session.Manager().CurrentCanvasId();
    const ItemId one = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "One");
    const ItemId other = session.Manager().CreateItem(false, Rect{0, 0, 100, 100}, "Other");
    const CanvasId second = session.Manager().AddCanvas("Second");
    session.Manager().SwitchToCanvas(second);
    session.RecordArrivals({*session.MoveItemHere(one), *session.MoveItemHere(other)}, /*duplicate=*/false);
    const CanvasId third = session.Manager().AddCanvas("Third");
    session.Manager().SwitchToCanvas(third);
    ASSERT_TRUE(session.MoveItemHere(one).has_value());
    session.Manager().SwitchToCanvas(second);

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
    std::ofstream(dir / "staging") << "a file where the staging directory wants to be";
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
    std::ofstream(dir / "staging") << "a file where the staging directory wants to be";
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
    EXPECT_TRUE(session.ClonePicturesForCopy(id, copyId)) << "the pixels are in the session";
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

// The ordinary paste across canvases: the source's painted pixels were let
// go of when its canvas stopped being current, so the copy has to be given
// them from the source's file - or it comes out blank, for good.
TEST(SessionTest, APasteAcrossCanvasesKeepsThePaintedLayer) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_painted_paste";
    std::filesystem::remove_all(dir);
    persistence::LibraryStore store(dir);
    test::FakeOverlayWindow window;
    window.createTextureFromPixelsReturnsHandle = 9;
    Session session;
    session.AttachWindow(&window);
    session.SetLibraryStore(&store);

    const std::vector<uint8_t> paint = {1, 2, 3, 255, 4, 5, 6, 255};
    const ItemId id = session.Manager().CreateItem(false, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Painted");
    {
        Layer painted;
        painted.kind = LayerKind::Painted;
        painted.painted = std::make_shared<PaintedImage>(PaintedImage::FromPixels(2, 1, paint));
        painted.paintedDirty = true;
        session.Manager().FindItemAnywhere(id)->layers.push_back(painted);
    }
    ASSERT_TRUE(session.Flush());
    const std::string paintedFile = session.Manager().FindItemAnywhere(id)->layers[1].imageFile;
    ASSERT_FALSE(paintedFile.empty());

    // Away to another canvas: the pixels are on disk, so they are let go of.
    const CanvasId other = session.Manager().AddCanvas("Other");
    session.Manager().SwitchToCanvas(other);
    session.SyncTexturesToCurrentCanvas();
    ASSERT_FALSE(session.Manager().FindItemAnywhere(id)->layers[1].HasPaintedPixels()) << "released, as it should be";

    // Paste here, the way PasteFromClipboard does it.
    const ItemId copyId = session.Manager().PlaceItemOnCanvas(id, other, /*copy=*/true);
    ASSERT_NE(copyId, 0u);
    EXPECT_TRUE(session.ClonePicturesForCopy(id, copyId));
    Item* copy = session.Manager().FindItemAnywhere(copyId);
    ASSERT_EQ(copy->layers.size(), 2u);
    EXPECT_TRUE(copy->layers[1].HasPaintedPixels()) << "pixels of its own";
    EXPECT_EQ(copy->layers[1].painted->PixelsRGBA(), paint);
    session.SyncTexturesToCurrentCanvas();
    EXPECT_NE(copy->layers[1].textureHandle, 0u) << "and on screen";
    ASSERT_TRUE(session.Flush());

    persistence::LibraryStore reopened(dir);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    size_t paintedLayers = 0;
    for (const Canvas& canvas : loaded->canvases) {
        for (const Item& item : canvas.items) {
            ASSERT_EQ(item.layers.size(), 2u);
            ASSERT_FALSE(item.layers[1].imageFile.empty()) << "every copy names its painted layer's file";
            const std::optional<persistence::DecodedImage> saved = reopened.LoadImage(item.id, item.layers[1].imageFile);
            ASSERT_TRUE(saved.has_value());
            EXPECT_EQ(saved->pixelsRGBA, paint);
            ++paintedLayers;
        }
    }
    EXPECT_EQ(paintedLayers, 2u);

    // A source whose painted file is gone gives its copy nothing there, and
    // says so.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().filename() == paintedFile && entry.path().string().find("painted") != std::string::npos) {
            std::filesystem::remove(entry.path());
        }
    }
    const ItemId another = session.Manager().PlaceItemOnCanvas(id, other, /*copy=*/true);
    ASSERT_NE(another, 0u);
    EXPECT_FALSE(session.ClonePicturesForCopy(id, another));

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
    EXPECT_FALSE(session.ClonePicturesForCopy(id, copyId));
    EXPECT_TRUE(session.Manager().FindItemAnywhere(copyId)->ImageLayer()->imageFile.empty());

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}

// Move a captured snippet to another canvas and delete the canvas it left
// for good, all before the autosave: the picture is still in the old
// canvas's directory on disk, and must not go with it.
TEST(SessionTest, APermanentDeleteRightAfterAMoveKeepsWhatWasMoved) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "spickzettel_session_test_delete_after_move";
    std::filesystem::remove_all(dir);
    const std::vector<uint8_t> pixels = {10, 20, 30, 255, 40, 50, 60, 255};
    ItemId id = 0;
    {
        persistence::LibraryStore store(dir);
        Session session;
        session.SetLibraryStore(&store);
        const CanvasId left = session.Manager().CurrentCanvasId();
        id = session.Manager().CreateItem(true, Rect{0.0f, 0.0f, 2.0f, 1.0f}, "Shot");
        const std::optional<std::string> file = store.SaveImage(id, pixels.data(), 2, 1);
        ASSERT_TRUE(file.has_value());
        session.Manager().FindItemAnywhere(id)->ImageLayer()->imageFile = *file;
        const CanvasId other = session.Manager().AddCanvas("Other");
        ASSERT_TRUE(session.Flush());

        ASSERT_NE(session.Manager().PlaceItemOnCanvas(id, other, /*copy=*/false), 0u);
        ASSERT_TRUE(session.Delete(left));
        EXPECT_EQ(session.DeletePermanently(left), Session::Removal::Removed);
        EXPECT_TRUE(session.Flush());
        session.SetLibraryStore(nullptr);
    }
    persistence::LibraryStore reopened(dir);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    const Item& item = loaded->canvases[0].items[0];
    ASSERT_EQ(item.id, id);
    const std::optional<persistence::DecodedImage> image = reopened.LoadImage(id, item.ImageLayer()->imageFile);
    ASSERT_TRUE(image.has_value()) << "the picture went with the canvas the snippet left";
    EXPECT_EQ(image->pixelsRGBA, pixels);
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

        // ...on a clock of its own, not every frame: two seconds of frames
        // while the file is still held are not two seconds of saves.
        const uint64_t writesBefore = store.WriteGeneration();
        for (int frame = 0; frame < 120; ++frame) {
            session.Tick(1.0f / 60.0f);
        }
        EXPECT_EQ(store.WriteGeneration(), writesBefore) << "retried on every frame";
    }
    // Let go, the removal's own clock finishes the delete with no edit to
    // prompt it - and sets nothing aside.
    session.Tick(10.0f);
    EXPECT_FALSE(session.HasUnsavedChanges());
    EXPECT_FALSE(std::filesystem::exists(record.parent_path()));
    EXPECT_FALSE(std::filesystem::exists(dir / "retired"));

    session.SetLibraryStore(nullptr);
    std::filesystem::remove_all(dir);
}
#endif

}  // namespace
}  // namespace sz::core
