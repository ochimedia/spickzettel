#include "core/persistence/library_store.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "core/canvas/item_geometry.h"
#include "core/diagnostics/timeline.h"
#include "support/failing_writes.h"
#include "support/temp_dir.h"

namespace sz::core::persistence {
namespace {

class LibraryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = sz::test::TempDir() /
               (std::string("spickzettel_library_store_test_") +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        file_ = dir_ / "library.db";
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
    std::filesystem::path file_;
};

// The file as another program would open it - to look inside, to hold a
// lock, or to change what a store wrote.
class RawConnection {
public:
    explicit RawConnection(const std::filesystem::path& file) {
        const std::u8string name = file.u8string();
        sqlite3_open(std::string(name.begin(), name.end()).c_str(), &db_);
    }
    ~RawConnection() { sqlite3_close(db_); }
    bool Exec(const std::string& sql) { return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK; }
    int64_t Int(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        int64_t value = -1;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            value = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
        return value;
    }
    bool SetBlob(const std::string& sql, const std::vector<uint8_t>& blob) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_blob(statement, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
        const bool done = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return done;
    }

private:
    sqlite3* db_ = nullptr;
};

CanvasManagerSnapshot MakeSampleSnapshot() {
    CanvasManagerSnapshot snapshot;

    Folder folder;
    folder.id = 1;
    folder.name = "Folder 1";
    folder.createdAt = 1700000000;
    snapshot.folders.push_back(folder);
    Folder deletedFolder;
    deletedFolder.id = 5;
    deletedFolder.name = "Gone";
    deletedFolder.deletedAt = 1700000100;
    snapshot.folders.push_back(deletedFolder);

    Canvas canvas;
    canvas.id = 2;
    canvas.name = "Canvas 1";
    canvas.folderId = 1;
    canvas.createdAt = 1700000001;

    Item drawing;
    drawing.id = 3;
    drawing.hasBackground = false;
    drawing.name = "Drawing 1";
    drawing.createdAt = 1700000002;
    drawing.rect = Rect{10, 20, 300, 200};
    drawing.nativeW = 300;
    drawing.nativeH = 200;
    drawing.foregroundOpacity = 0.75f;
    drawing.picture.opacity = 0.3f;
    drawing.picture.tintColorRGBA = 0xaabbccffu;
    drawing.minimized = true;
    drawing.pinned = true;
    drawing.noteText = "A caption, styled per snippet \xE2\x9C\x93";
    drawing.noteTextColorRGBA = 0x4dd6b880u;
    drawing.noteTextSizePx = 34.0f;
    drawing.keepAspect = false;  // not the default, so a lost field would show
    Stroke stroke;
    stroke.colorRGBA = 0x11223344u;
    stroke.width = 4.5f;
    stroke.points = {StrokePoint{1, 2}, StrokePoint{3, 4}, StrokePoint{5, 6}};
    drawing.strokes.push_back(stroke);
    stroke.points = {StrokePoint{-0.0f, 7.25f}};
    stroke.corners = StrokeCorners::Sharp;  // not the default either
    drawing.strokes.push_back(stroke);
    canvas.items.push_back(drawing);

    Item shot;
    shot.id = 4;
    shot.hasBackground = true;
    shot.name = "Shot 1";
    shot.deletedAt = 1700000200;
    shot.rect = Rect{0, 0, 1920, 1080};
    shot.nativeW = 1920;
    shot.nativeH = 1080;
    shot.isFullscreen = true;
    shot.isFullscreenStretch = true;
    shot.anchorRect = Rect{50, 50, 400, 300};
    shot.anchorDisplayWidth = 1920.0f;
    shot.anchorDisplayHeight = 1080.0f;
    shot.picture.placeholderHue = 123.5f;
    shot.picture.opacity = 0.9f;
    shot.picture.tintColorRGBA = 0x112233ffu;
    shot.picture.showsPlaceholder = true;
    canvas.items.push_back(shot);
    snapshot.canvases.push_back(canvas);

    Canvas second;
    second.id = 6;
    second.name = "Canvas 2";
    second.folderId = 5;
    second.deletedAt = 1700000300;
    snapshot.canvases.push_back(second);

    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    return snapshot;
}

void ExpectSameLibrary(const CanvasManagerSnapshot& expected, const CanvasManagerSnapshot& actual) {
    EXPECT_EQ(actual.folders, expected.folders);
    ASSERT_EQ(actual.canvases.size(), expected.canvases.size());
    for (size_t i = 0; i < expected.canvases.size(); ++i) {
        const Canvas& e = expected.canvases[i];
        const Canvas& a = actual.canvases[i];
        EXPECT_EQ(a.id, e.id);
        EXPECT_EQ(a.name, e.name);
        EXPECT_EQ(a.folderId, e.folderId);
        EXPECT_EQ(a.createdAt, e.createdAt);
        EXPECT_EQ(a.deletedAt, e.deletedAt);
        EXPECT_EQ(a.items, e.items) << "canvas " << e.id;
    }
    EXPECT_EQ(actual.currentFolderId, expected.currentFolderId);
    EXPECT_EQ(actual.currentCanvasId, expected.currentCanvasId);
}

std::vector<uint8_t> Checkerboard(int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            const bool dark = ((x / 8) + (y / 8)) % 2 == 0;
            pixels[i] = dark ? 20 : 230;
            pixels[i + 1] = static_cast<uint8_t>(x);
            pixels[i + 2] = static_cast<uint8_t>(y);
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

// ===== Opening, and what a first run is =====

TEST_F(LibraryStoreTest, AFileThatIsNotThereYetIsAFirstRun) {
    LibraryStore store(file_);
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Opened);
    EXPECT_TRUE(std::filesystem::exists(file_)) << "made, directory and all";
    EXPECT_FALSE(store.Load().has_value());
    EXPECT_TRUE(store.SetAsideAs().empty());
}

// Everything a snapshot holds comes back as it went in: every field of
// every snippet, the stamps, the order of folders, canvases and snippets,
// and which ones are current.
TEST_F(LibraryStoreTest, SaveThenLoadRoundTripsEverything) {
    const CanvasManagerSnapshot original = MakeSampleSnapshot();
    ASSERT_TRUE(LibraryStore(file_).Save(original));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    ExpectSameLibrary(original, *loaded);
}

// The schema is committed by the Open that makes it, before a first run's
// first write. A file whose first write failed holds tables and nothing in
// them, which is a first run still - not a library someone emptied, which
// keeps the rows saying which canvas is current (below).
TEST_F(LibraryStoreTest, ALibraryWhoseFirstWriteFailedIsAFirstRunAgain) {
    {
        LibraryStore store(file_);
        ASSERT_FALSE(store.Load().has_value());
        test::FailingWrites failing(file_);
        failing.FailAll();
        EXPECT_FALSE(store.Save(MakeSampleSnapshot()));
    }
    LibraryStore store(file_);
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Opened);
    EXPECT_FALSE(store.Load().has_value());
}

// A library someone emptied is still a library, not a first run - that
// person has met the app already.
TEST_F(LibraryStoreTest, AnEmptiedLibraryLoadsAsAnEmptyOne) {
    {
        LibraryStore store(file_);
        ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
        ASSERT_TRUE(store.Save(CanvasManagerSnapshot{}));
    }
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->folders.empty());
    EXPECT_TRUE(loaded->canvases.empty());
}

TEST_F(LibraryStoreTest, OrderIsKeptWhenThingsAreRearranged) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));

    std::swap(snapshot.folders[0], snapshot.folders[1]);
    std::swap(snapshot.canvases[0], snapshot.canvases[1]);
    std::swap(snapshot.canvases[1].items[0], snapshot.canvases[1].items[1]);
    ASSERT_TRUE(store.Save(snapshot));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    ExpectSameLibrary(snapshot, *loaded);
}

// ===== Writing a change =====

LibraryView ViewOf(const CanvasManagerSnapshot& snapshot) {
    return LibraryView{snapshot.folders, snapshot.canvases, snapshot.currentFolderId, snapshot.currentCanvasId};
}

// A write writes the rows it is told to and no others: a snippet changed
// in the view but not named stays in the file as it was.
TEST_F(LibraryStoreTest, AWriteWritesTheRowsItNamesAndNoOthers) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].items[0].name = "Written";
    snapshot.canvases[0].items[1].name = "Not written";
    LibraryChanges changes;
    changes.items = {3};
    ASSERT_TRUE(store.Write(ViewOf(snapshot), changes));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items[0].name, "Written");
    EXPECT_EQ(loaded->canvases[0].items[1].name, "Shot 1");
    EXPECT_TRUE(store.Write(ViewOf(snapshot), LibraryChanges{})) << "nothing to write lands";
}

// A snippet written with its canvas's order: taken out of the middle of a
// stack and one put in at the bottom, the stack reads back as it is,
// rather than with the new one beside a stale place.
TEST_F(LibraryStoreTest, ASnippetIsWrittenWithItsCanvasesOrder) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Item third = snapshot.canvases[0].items[0];
    third.id = 9;
    snapshot.canvases[0].items.push_back(third);
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].items.erase(snapshot.canvases[0].items.begin() + 1);  // 4, from the middle
    LibraryChanges erased;
    erased.erasedItems = {4};
    ASSERT_TRUE(store.Write(ViewOf(snapshot), erased));
    Item bottom = third;
    bottom.id = 8;
    snapshot.canvases[0].items.insert(snapshot.canvases[0].items.begin(), bottom);
    LibraryChanges made;
    made.items = {8};
    ASSERT_TRUE(store.Write(ViewOf(snapshot), made));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    ExpectSameLibrary(snapshot, *loaded);
}

// Every field of a snippet is written with it, and comes back.
TEST_F(LibraryStoreTest, EveryFieldOfASnippetIsSaved) {
    const std::vector<std::function<void(Item&)>> changes = {
        [](Item& i) { i.name += "x"; },
        [](Item& i) { i.createdAt += 1; },
        [](Item& i) { i.deletedAt += 1; },
        [](Item& i) { i.hasBackground = !i.hasBackground; },
        [](Item& i) { i.rect.w += 1.0f; },
        [](Item& i) { i.nativeW += 1.0f; },
        [](Item& i) { i.nativeH += 1.0f; },
        [](Item& i) { i.foregroundOpacity = 0.5f; },
        [](Item& i) { i.keepAspect = !i.keepAspect; },
        [](Item& i) { i.isFullscreen = !i.isFullscreen; },
        [](Item& i) { i.isFullscreenStretch = !i.isFullscreenStretch; },
        [](Item& i) { i.anchorRect.h += 1.0f; },
        [](Item& i) { i.anchorDisplayWidth += 1.0f; },
        [](Item& i) { i.anchorDisplayHeight += 1.0f; },
        [](Item& i) { i.minimized = !i.minimized; },
        [](Item& i) { i.pinned = !i.pinned; },
        [](Item& i) { i.picture.opacity = 0.25f; },
        [](Item& i) { i.picture.tintColorRGBA ^= 0xFF00u; },
        [](Item& i) { i.picture.showsPlaceholder = !i.picture.showsPlaceholder; },
        [](Item& i) { i.picture.placeholderHue += 1.0f; },
        [](Item& i) { i.noteText += "!"; },
        [](Item& i) { i.noteTextColorRGBA ^= 0xFFu; },
        [](Item& i) { i.noteTextSizePx += 1.0f; },
        [](Item& i) { i.strokes[0].points[0].x += 1.0f; },
        [](Item& i) { i.strokes[0].colorRGBA ^= 0xFFu; },
        [](Item& i) { i.strokes[0].width += 1.0f; },
        [](Item& i) { i.strokes.pop_back(); },
    };
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));
    for (size_t n = 0; n < changes.size(); ++n) {
        changes[n](snapshot.canvases[0].items[0]);
        LibraryChanges written;
        written.items = {3};
        ASSERT_TRUE(store.Write(ViewOf(snapshot), written));
        const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->canvases[0].items[0], snapshot.canvases[0].items[0]) << "change " << n;
    }
}

// ===== What the library no longer holds =====

TEST_F(LibraryStoreTest, WhatTheLibraryNoLongerHoldsIsGoneWithItsPicture) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    const std::vector<uint8_t> pixels = Checkerboard(16, 16);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 16, 16));
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].items.pop_back();  // the shot
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(store.LoadImage(4).has_value());

    snapshot.folders.pop_back();  // folder 5, and canvas 6 in it
    snapshot.canvases.pop_back();
    ASSERT_TRUE(store.Save(snapshot));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    ExpectSameLibrary(snapshot, *loaded);
    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT count(*) FROM pictures"), 0);
}

// A folder that goes takes its canvases and their snippets with it, and
// the snippets their pictures, by the schema's own cascade - whatever
// wrote the delete.
TEST_F(LibraryStoreTest, DeletingAFolderRowTakesEverythingInIt) {
    {
        LibraryStore store(file_);
        const std::vector<uint8_t> pixels = Checkerboard(8, 8);
        ASSERT_TRUE(store.SaveImage(4, pixels.data(), 8, 8));
        ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    }
    RawConnection raw(file_);
    ASSERT_TRUE(raw.Exec("PRAGMA foreign_keys = ON; DELETE FROM folders WHERE id = 1"));
    EXPECT_EQ(raw.Int("SELECT count(*) FROM canvases WHERE folder_id = 1"), 0);
    EXPECT_EQ(raw.Int("SELECT count(*) FROM items"), 0);
    EXPECT_EQ(raw.Int("SELECT count(*) FROM pictures"), 0);
}

// A snippet moved to another canvas, and the canvas it left then deleted,
// in one save: the snippet goes with the move, not with the canvas.
TEST_F(LibraryStoreTest, ASnippetMovedOffACanvasThatGoesIsKept) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    const std::vector<uint8_t> pixels = Checkerboard(8, 8);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 8, 8));
    ASSERT_TRUE(store.Save(snapshot));

    Item shot = snapshot.canvases[0].items[1];
    snapshot.canvases[1].items.push_back(shot);
    snapshot.canvases.erase(snapshot.canvases.begin());
    snapshot.currentCanvasId = 6;
    ASSERT_TRUE(store.Save(snapshot));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items[0].id, 4u);
    EXPECT_TRUE(store.LoadImage(4).has_value());
}

// ===== Pictures =====

TEST_F(LibraryStoreTest, APictureComesBackExactlyWithAThumbnail) {
    LibraryStore store(file_);
    const std::vector<uint8_t> pixels = Checkerboard(640, 360);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 640, 360));

    const std::optional<DecodedImage> picture = store.LoadImage(4);
    ASSERT_TRUE(picture.has_value());
    EXPECT_EQ(picture->width, 640);
    EXPECT_EQ(picture->height, 360);
    EXPECT_EQ(picture->pixelsRGBA, pixels);

    const std::optional<DecodedImage> thumbnail = store.LoadThumbnail(4);
    ASSERT_TRUE(thumbnail.has_value());
    EXPECT_EQ(thumbnail->width, LibraryStore::kThumbnailMaxExtent);
    EXPECT_EQ(thumbnail->height, 144);

    EXPECT_FALSE(store.LoadImage(99).has_value());
    EXPECT_FALSE(store.SaveImage(5, nullptr, 4, 4));
    EXPECT_FALSE(store.SaveImage(5, pixels.data(), 0, 4));
}

// Whether a snippet has a picture is the library's to say.
TEST_F(LibraryStoreTest, ASnippetWithAStoredPictureLoadsKnowingIt) {
    LibraryStore store(file_);
    const std::vector<uint8_t> pixels = Checkerboard(8, 8);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 8, 8));
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->canvases[0].items[0].picture.stored);
    EXPECT_TRUE(loaded->canvases[0].items[1].picture.stored);
}

// A snippet's picture is written with it - captured pixels, or a copy of
// another's as stored - in the same write.
TEST_F(LibraryStoreTest, PicturesAreWrittenWithTheirSnippets) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));
    Item copy = snapshot.canvases[0].items[1];
    copy.id = 7;
    snapshot.canvases[0].items.push_back(copy);
    const std::vector<uint8_t> pixels = Checkerboard(32, 32);
    LibraryChanges changes;
    changes.items = {7};
    LibraryStore::PictureWrites pictures;
    pictures.captured.push_back({4, pixels.data(), 32, 32});
    pictures.copies.emplace_back(4, 7);
    ASSERT_TRUE(store.Write(ViewOf(snapshot), changes, pictures));

    const std::optional<DecodedImage> copied = store.LoadImage(7);
    ASSERT_TRUE(copied.has_value());
    EXPECT_EQ(copied->pixelsRGBA, pixels);
    EXPECT_TRUE(store.LoadThumbnail(7).has_value());
}

// A picture whose snippet is not in the library - which only a library
// written before every change was one transaction could hold - goes at the
// next Load.
TEST_F(LibraryStoreTest, APictureWithNoSnippetGoesAtTheNextLoad) {
    {
        LibraryStore store(file_);
        const std::vector<uint8_t> pixels = Checkerboard(8, 8);
        ASSERT_TRUE(store.SaveImage(4, pixels.data(), 8, 8));
        ASSERT_TRUE(store.SaveImage(99, pixels.data(), 8, 8));
        ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    }
    LibraryStore store(file_);
    ASSERT_TRUE(store.Load().has_value());
    EXPECT_TRUE(store.LoadImage(4).has_value());
    EXPECT_FALSE(store.LoadImage(99).has_value());
}

// ===== When the file says no =====

// Another program holding the file mid-write: the write fails and changes
// nothing, and the next one writes everything the failed one would have.
TEST_F(LibraryStoreTest, AWriteThatCannotGetTheFileChangesNothing) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.folders[0].name = "Renamed";
    snapshot.canvases[0].items[0].strokes.clear();
    {
        RawConnection other(file_);
        ASSERT_TRUE(other.Exec("BEGIN EXCLUSIVE"));
        EXPECT_FALSE(store.Save(snapshot));
        other.Exec("ROLLBACK");
    }
    {
        const std::optional<CanvasManagerSnapshot> unchanged = LibraryStore(file_).Load();
        ASSERT_TRUE(unchanged.has_value());
        ExpectSameLibrary(MakeSampleSnapshot(), *unchanged);
    }
    ASSERT_TRUE(store.Save(snapshot));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    ExpectSameLibrary(snapshot, *loaded);
}

// A write that fails partway leaves none of itself behind: what it wrote
// before the failure is rolled back with it.
TEST_F(LibraryStoreTest, AWriteThatFailsPartwayLeavesNoneOfItself) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("CREATE TRIGGER refuse BEFORE UPDATE ON canvases WHEN NEW.name = 'refused' "
                             "BEGIN SELECT RAISE(ABORT, 'refused'); END"));
    }
    snapshot.folders[0].name = "Written first";
    snapshot.canvases[0].name = "refused";
    EXPECT_FALSE(store.Save(snapshot));
    {
        const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->folders[0].name, "Folder 1") << "rolled back with the rest";
    }
    snapshot.canvases[0].name = "Allowed";
    ASSERT_TRUE(store.Save(snapshot));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->folders[0].name, "Written first");
    EXPECT_EQ(loaded->canvases[0].name, "Allowed");
}

// A table the file no longer has - another program dropped it - fails the
// statements naming it as they are prepared, and a write that meets one
// fails, rolled back whole. It crashed: the statement that was never made
// was run all the same.
TEST_F(LibraryStoreTest, AWriteToATableTheFileNoLongerHasFails) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("DROP TABLE meta"));
    }
    snapshot.folders[0].createdAt = 1800000000;
    EXPECT_FALSE(store.Save(snapshot)) << "the current canvas is written into meta, last";
    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT created_at FROM folders WHERE id = 1"), 1700000000) << "rolled back with the rest";
}

// After a first write that failed, the next write writes the whole
// library, whatever it names: one snippet written alone would be on a
// canvas the file does not have, which its foreign key refuses - as it
// refused every write after, until one wrote the folders and canvases.
TEST_F(LibraryStoreTest, TheWriteAfterAFailedFirstOneWritesTheWholeLibrary) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_FALSE(store.Load().has_value());
    test::FailingWrites failing(file_);
    failing.FailAll();
    EXPECT_FALSE(store.Save(snapshot));
    failing.Stop();

    snapshot.canvases[0].items[0].name = "Changed";
    LibraryChanges changes;
    changes.items = {3};
    ASSERT_TRUE(store.Write(ViewOf(snapshot), changes));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    ExpectSameLibrary(snapshot, *loaded);

    // Written whole once, a write is of what it names again.
    snapshot.canvases[0].items[0].name = "Named";
    snapshot.canvases[0].items[1].name = "Not named";
    ASSERT_TRUE(store.Write(ViewOf(snapshot), changes));
    const std::optional<CanvasManagerSnapshot> after = LibraryStore(file_).Load();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->canvases[0].items[0].name, "Named");
    EXPECT_EQ(after->canvases[0].items[1].name, "Shot 1");
}

// Held by another program as the app starts: not opened, and not written
// over - the app refuses to start rather than begin an empty library there.
TEST_F(LibraryStoreTest, AFileAnotherProgramHoldsIsUnreadable) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        // A WAL file is shut to readers only by a hold of the file itself.
        RawConnection other(file_);
        ASSERT_TRUE(other.Exec("PRAGMA locking_mode = EXCLUSIVE; BEGIN EXCLUSIVE"));
        LibraryStore store(file_);
        EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Unreadable);
        EXPECT_FALSE(store.Load().has_value());
        EXPECT_FALSE(store.Save(CanvasManagerSnapshot{}));
    }
    EXPECT_TRUE(LibraryStore(file_).Load().has_value());
}

// Opened, and then not read through: not a first run, which would start an
// empty library over it, but a file that cannot be read - and nothing is
// written over it afterwards, even once it could be. A table gone for the
// moment stands in for the read that fails partway: another program can no
// longer come between the open and the read, since the store holds the
// file from its open (see TheAppsStoreHoldsItsFileForItself).
TEST_F(LibraryStoreTest, ALibraryThatCannotBeReadThroughIsUnreadableRatherThanAFirstRun) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    LibraryStore store(file_);
    ASSERT_EQ(store.Open(), LibraryStore::OpenResult::Opened);
    RawConnection other(file_);
    ASSERT_TRUE(other.Exec("ALTER TABLE items RENAME TO items_away"));
    EXPECT_FALSE(store.Load().has_value());
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Unreadable);
    ASSERT_TRUE(other.Exec("ALTER TABLE items_away RENAME TO items"));
    EXPECT_FALSE(store.Save(CanvasManagerSnapshot{}));
    EXPECT_EQ(other.Int("SELECT count(*) FROM items"), 2) << "untouched";
}

// A newer build's library is neither read nor written.
TEST_F(LibraryStoreTest, ALibraryANewerVersionWroteIsNeitherReadNorWritten) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("PRAGMA user_version = " + std::to_string(LibraryStore::kFormatVersion + 1)));
    }
    LibraryStore store(file_);
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::WrittenByANewerVersion);
    EXPECT_FALSE(store.Load().has_value());
    EXPECT_FALSE(store.Save(CanvasManagerSnapshot{}));
    const uint8_t pixel[4] = {1, 2, 3, 4};
    EXPECT_FALSE(store.SaveImage(3, pixel, 1, 1));

    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT count(*) FROM items"), 2) << "untouched";
}

// Something that is not a library of ours where the library should be - a
// file that is not a database, a damaged one, someone else's - is set
// aside, not lost, and a new library starts in its place.
TEST_F(LibraryStoreTest, AFileThatIsNotALibraryIsSetAsideAndANewOneStarted) {
    std::filesystem::create_directories(dir_);
    std::ofstream(file_, std::ios::binary) << "not a database, but somebody's text";

    LibraryStore store(file_);
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Opened);
    ASSERT_FALSE(store.SetAsideAs().empty());
    EXPECT_EQ(store.SetAsideAs().parent_path(), dir_);
    EXPECT_NE(store.SetAsideAs().filename().string().find("library-unreadable-"), std::string::npos);
    std::ifstream aside(store.SetAsideAs(), std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(aside), {}), "not a database, but somebody's text");

    EXPECT_FALSE(store.Load().has_value()) << "a first run";
    EXPECT_TRUE(store.Save(MakeSampleSnapshot()));
}

TEST_F(LibraryStoreTest, SomeoneElsesDatabaseIsSetAside) {
    {
        std::filesystem::create_directories(dir_);
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("CREATE TABLE theirs (x INTEGER)"));
    }
    LibraryStore store(file_);
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Opened);
    EXPECT_FALSE(store.SetAsideAs().empty());
    EXPECT_FALSE(store.Load().has_value());
}

// ===== Rows that cannot be used as they are =====

// A value that cannot be used is the field's default, the rest of the row
// is kept, and the repaired row is written back as it now reads.
TEST_F(LibraryStoreTest, ARowThatCannotBeUsedAsItIsIsRepairedAndWrittenBack) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("UPDATE items SET record = json_set(record, '$.nativeW', 1e6, "
                             "'$.foregroundOpacity', 7, '$.name', 12) WHERE id = 3"));
        // The second stroke cut short partway through its points.
        std::vector<uint8_t> blob = {2, 2, 0, 0, 0};
        const auto put = [&blob](const auto value) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            blob.insert(blob.end(), bytes, bytes + sizeof(value));
        };
        put(uint32_t{0xFF0000FF});
        put(std::numeric_limits<float>::infinity());  // a width that cannot be
        put(uint8_t{7});                              // corners that are not
        put(uint32_t{1});
        put(1.0f);
        put(2.0f);
        put(uint32_t{0x00FF00FF});
        put(3.0f);
        put(uint8_t{0});
        put(uint32_t{2});
        put(4.0f);
        ASSERT_TRUE(raw.SetBlob("UPDATE items SET strokes = ?1 WHERE id = 3", blob));
    }
    LibraryStore store(file_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    const Item& item = loaded->canvases[0].items[0];
    EXPECT_FLOAT_EQ(item.nativeW, 65536.0f) << "finite and absurd: held to the top of the range";
    EXPECT_FLOAT_EQ(item.foregroundOpacity, 1.0f);
    EXPECT_EQ(item.name, "");
    EXPECT_FLOAT_EQ(item.nativeH, 200.0f) << "the rest of the record is kept";
    ASSERT_EQ(item.strokes.size(), 1u) << "the stroke before the cut";
    EXPECT_FLOAT_EQ(item.strokes[0].width, 3.0f);
    EXPECT_EQ(item.strokes[0].corners, StrokeCorners::Round);
    EXPECT_EQ(item.strokes[0].points, (std::vector<StrokePoint>{{1.0f, 2.0f}}));

    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT json_extract(record, '$.foregroundOpacity') FROM items WHERE id = 3"), 1)
        << "written back";
    EXPECT_EQ(raw.Int("SELECT length(strokes) FROM items WHERE id = 3"), 1 + 4 + 13 + 8) << "one stroke, whole";
}

// A rectangle is held to where a screen could be, not just to finite
// numbers, and a snippet without a size gets the smallest one there is.
// A rectangle that is not one is repaired as well.
TEST_F(LibraryStoreTest, ARectangleOutOfAnyScreensRangeIsHeldToIt) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("UPDATE items SET record = json_set(record, '$.rect.x', -1e30, '$.rect.y', 1e30, "
                             "'$.rect.w', -50, '$.rect.h', 65536, '$.anchorRect', 'not a rectangle') "
                             "WHERE id = 3"));
    }
    LibraryStore store(file_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    const Item& item = loaded->canvases[0].items[0];
    EXPECT_FLOAT_EQ(item.rect.x, -65536.0f);
    EXPECT_FLOAT_EQ(item.rect.y, 65536.0f);
    EXPECT_FLOAT_EQ(item.rect.w, kItemMinWidth) << "below zero is no size; the smallest a snippet can have";
    EXPECT_FLOAT_EQ(item.rect.h, 65536.0f) << "at the top of the range, kept";
    EXPECT_EQ(item.anchorRect, Rect{}) << "not anchored";

    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT json_extract(record, '$.rect.w') FROM items WHERE id = 3"),
              static_cast<int>(kItemMinWidth))
        << "written back";
    EXPECT_EQ(raw.Int("SELECT json_type(record, '$.anchorRect') = 'object' FROM items WHERE id = 3"), 1);
}

// One partly off the screen at the smallest size, unanchored, is an
// ordinary one: read back as written.
TEST_F(LibraryStoreTest, AnOrdinaryRectangleIsReadBackAsWritten) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Item& item = snapshot.canvases[0].items[0];
    item.rect = Rect{-20.0f, 1500.5f, 16.0f, 900.0f};
    item.anchorRect = Rect{};
    ASSERT_TRUE(LibraryStore(file_).Save(snapshot));
    LibraryStore store(file_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items[0].rect, (Rect{-20.0f, 1500.5f, 16.0f, 900.0f}));
}

// Strokes as 0.2.1 wrote them, without their corners: a rectangle's, and
// what the eraser left of one, keep theirs square, as they were drawn then;
// a freehand line is round.
TEST_F(LibraryStoreTest, StrokesFromBeforeCornersKeepTheRectanglesSquare) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        RawConnection raw(file_);
        std::vector<uint8_t> blob = {1, 3, 0, 0, 0};
        const auto put = [&blob](const auto value) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            blob.insert(blob.end(), bytes, bytes + sizeof(value));
        };
        const auto stroke = [&](const std::vector<StrokePoint>& points) {
            put(uint32_t{0xFF0000FF});
            put(3.0f);
            put(static_cast<uint32_t>(points.size()));
            for (const StrokePoint& point : points) {
                put(point.x);
                put(point.y);
            }
        };
        stroke({{10, 10}, {80, 10}, {80, 60}, {10, 60}, {10, 10}});
        stroke({{80, 30}, {80, 60}, {10, 60}});
        stroke({{10, 10}, {40, 11}, {45, 30}});
        ASSERT_TRUE(raw.SetBlob("UPDATE items SET strokes = ?1 WHERE id = 3", blob));
    }
    LibraryStore store(file_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    const std::vector<Stroke>& strokes = loaded->canvases[0].items[0].strokes;
    ASSERT_EQ(strokes.size(), 3u);
    EXPECT_EQ(strokes[0].corners, StrokeCorners::Sharp) << "a rectangle";
    EXPECT_EQ(strokes[1].corners, StrokeCorners::Sharp) << "what the eraser left of one";
    EXPECT_EQ(strokes[2].corners, StrokeCorners::Round) << "a freehand line";
    EXPECT_EQ(strokes[2].points.size(), 3u);
}

// A library an older version wrote is this version's once opened, so the
// older version, which would drop what this one writes, refuses it.
TEST_F(LibraryStoreTest, ALibraryAnOlderVersionWroteIsThisVersionsOnceOpened) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("PRAGMA user_version = 1"));
    }
    {
        LibraryStore store(file_);
        EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Opened);
        EXPECT_TRUE(store.Load().has_value());
    }
    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("PRAGMA user_version"), LibraryStore::kFormatVersion);
}

// A deletion stamp that is no date - before 1970, or past the year 3000 -
// stays deleted, and reads as the time of the load, written back so. Such
// a stamp was shown in the Show deleted tooltip, where a time the C
// runtime cannot make a date of ended the app.
TEST_F(LibraryStoreTest, ADeletionStampThatIsNoDateStaysDeletedAsOfNow) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("UPDATE folders SET deleted_at = -5 WHERE id = 5"));
        ASSERT_TRUE(raw.Exec("UPDATE canvases SET deleted_at = 99999999999 WHERE id = 6"));
        ASSERT_TRUE(raw.Exec("UPDATE items SET record = json_set(record, '$.deletedAt', -1) WHERE id = 4"));
    }
    const int64_t before = static_cast<int64_t>(std::time(nullptr));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    const int64_t after = static_cast<int64_t>(std::time(nullptr));
    ASSERT_TRUE(loaded.has_value());
    for (const int64_t stamp :
         {loaded->folders[1].deletedAt, loaded->canvases[1].deletedAt, loaded->canvases[0].items[1].deletedAt}) {
        EXPECT_GE(stamp, before);
        EXPECT_LE(stamp, after);
    }
    EXPECT_EQ(loaded->folders[0].deletedAt, 0) << "not deleted, and not touched";

    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT deleted_at FROM folders WHERE id = 5"), loaded->folders[1].deletedAt)
        << "written back";
    EXPECT_EQ(raw.Int("SELECT deleted_at FROM canvases WHERE id = 6"), loaded->canvases[1].deletedAt);
    EXPECT_EQ(raw.Int("SELECT json_extract(record, '$.deletedAt') FROM items WHERE id = 4"),
              loaded->canvases[0].items[1].deletedAt);
}

// A deletion stamp ahead of the clock is kept as it is, and so is the
// file. It is what a clock that runs behind at start makes of every
// recent delete. Read as not deleted, those came back out of the trash -
// snippets onto their canvases - and a fixed clock did not put them back.
TEST_F(LibraryStoreTest, ADeletionStampAheadOfTheClockIsKept) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t weekAhead = now + 7 * 24 * 60 * 60;
    const int64_t yearAgo = now - 365 * 24 * 60 * 60;
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("UPDATE folders SET deleted_at = " + std::to_string(weekAhead) + " WHERE id = 5"));
        ASSERT_TRUE(raw.Exec("UPDATE canvases SET deleted_at = " + std::to_string(yearAgo) + " WHERE id = 6"));
        ASSERT_TRUE(raw.Exec("UPDATE items SET record = json_set(record, '$.deletedAt', " +
                             std::to_string(weekAhead) + ") WHERE id = 4"));
    }
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->folders[1].deletedAt, weekAhead);
    EXPECT_EQ(loaded->canvases[1].deletedAt, yearAgo);
    EXPECT_EQ(loaded->canvases[0].items[1].deletedAt, weekAhead);

    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT deleted_at FROM folders WHERE id = 5"), weekAhead);
    EXPECT_EQ(raw.Int("SELECT json_extract(record, '$.deletedAt') FROM items WHERE id = 4"), weekAhead);
}

// Text that is not UTF-8 in a snippet's record is written with U+FFFD in
// place of what cannot be read. The record's JSON threw on it, and nothing
// caught that: the write, and the app with it, ended there. Once it was
// the one write known to throw, and what the transaction's rollback on an
// exception was tested with.
TEST_F(LibraryStoreTest, TextThatIsNotUtf8IsWrittenWithReplacementCharacters) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    LibraryStore store(file_);
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.folders[0].name = "Written with it";
    snapshot.canvases[0].items[0].name = "Bad \xFF name";
    snapshot.canvases[0].items[0].noteText = "cut short \xE2\x9C";
    LibraryChanges changes;
    changes.foldersAndCanvases = true;
    changes.items = {3};
    ASSERT_TRUE(store.Write(ViewOf(snapshot), changes));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items[0].name, "Bad \xEF\xBF\xBD name");
    EXPECT_EQ(loaded->canvases[0].items[0].noteText, "cut short \xEF\xBF\xBD");
    EXPECT_EQ(loaded->folders[0].name, "Written with it");
}

// Current pointers naming nothing open on something that exists and is not
// deleted - the deleted canvas and folder are first here, to be passed over.
TEST_F(LibraryStoreTest, ACurrentCanvasNamingNothingFallsBackToOneThatExists) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    std::swap(snapshot.canvases[0], snapshot.canvases[1]);
    std::swap(snapshot.folders[0], snapshot.folders[1]);
    snapshot.currentCanvasId = 77;
    snapshot.currentFolderId = 78;
    ASSERT_TRUE(LibraryStore(file_).Save(snapshot));
    {
        const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->currentCanvasId, 2u);
        EXPECT_EQ(loaded->currentFolderId, 1u);
    }

    // A folder naming nothing, with no canvas current: the first folder not
    // deleted.
    snapshot.currentCanvasId = 0;
    ASSERT_TRUE(LibraryStore(file_).Save(snapshot));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->currentCanvasId, 0u);
    EXPECT_EQ(loaded->currentFolderId, 1u);
}

// No canvas current is a state of its own, which deleting a folder's last
// canvas leaves, and a load keeps it: it is not a pointer naming nothing.
// It was taken for one, and opened on the library's first canvas - here
// the one just deleted - which was written back.
TEST_F(LibraryStoreTest, NoCanvasCurrentIsKept) {
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].deletedAt = 1700000400;
    snapshot.currentCanvasId = 0;
    ASSERT_TRUE(LibraryStore(file_).Save(snapshot));
    for (int load = 0; load < 2; ++load) {
        const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(file_).Load();
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->currentCanvasId, 0u) << "load " << load;
        EXPECT_EQ(loaded->currentFolderId, 1u) << "load " << load;
    }
}

// A path with characters outside every code page: %APPDATA% is under the
// user's profile, whose name can be anything.
TEST_F(LibraryStoreTest, ALibraryUnderANameOutsideTheCodePageOpens) {
    const std::filesystem::path name(u8"Bibliothek ü 日本");
    // Read in the build machine's code page, the name was another one,
    // inside it.
    ASSERT_EQ(name.wstring(), L"Bibliothek \u00FC \u65E5\u672C") << "the source read as UTF-8 (see /utf-8)";
    const std::filesystem::path file = dir_ / name / "library.db";
    ASSERT_TRUE(LibraryStore(file).Save(MakeSampleSnapshot()));
    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_TRUE(LibraryStore(file).Load().has_value());
}

// ================= The WAL, and the hold =================

// The hold a store has in the app, for the tests of it: every other test
// shares the file (see tests/support/shared_library.cpp).
class ExclusiveLocking {
public:
    ExclusiveLocking() : shared_(LibraryStore::LocksSharedForTesting()) { LibraryStore::LockSharedForTesting(false); }
    ~ExclusiveLocking() { LibraryStore::LockSharedForTesting(shared_); }
    ExclusiveLocking(const ExclusiveLocking&) = delete;
    ExclusiveLocking& operator=(const ExclusiveLocking&) = delete;

private:
    bool shared_;
};

std::filesystem::path Beside(const std::filesystem::path& file, const char* suffix) {
    std::filesystem::path beside = file;
    beside += suffix;
    return beside;
}

// Pixels that do not compress: QOI stores them at about five bytes each.
std::vector<uint8_t> NoisePixels(int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    uint32_t state = 12345;
    for (uint8_t& value : pixels) {
        state = state * 1664525u + 1013904223u;
        value = static_cast<uint8_t>(state >> 24);
    }
    return pixels;
}

// A commit goes into the WAL and stays there, however much it holds - SQLite
// would move it into the file at every thousand pages, inside a commit -
// until a checkpoint is asked for, which leaves the WAL empty. The file is
// in WAL mode from then on.
TEST_F(LibraryStoreTest, WritesStayInTheWalUntilACheckpoint) {
    {
        LibraryStore store(file_);
        ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
        EXPECT_GT(store.UncheckpointedBytes(), 0);
        const std::vector<uint8_t> pixels = NoisePixels(1000, 1000);
        ASSERT_TRUE(store.SaveImage(3, pixels.data(), 1000, 1000));
        EXPECT_GT(store.UncheckpointedBytes(), int64_t{4} << 20) << "past SQLite's thousand pages";
        EXPECT_GT(std::filesystem::file_size(Beside(file_, "-wal")), uintmax_t{4} << 20);

        ASSERT_TRUE(store.Checkpoint());
        EXPECT_EQ(store.UncheckpointedBytes(), 0);
        EXPECT_EQ(std::filesystem::file_size(Beside(file_, "-wal")), 0u);
        EXPECT_TRUE(store.Checkpoint()) << "nothing to do";
    }
    EXPECT_FALSE(std::filesystem::exists(Beside(file_, "-wal"))) << "gone with the close";
    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT count(*) FROM items"), 2);
    EXPECT_EQ(raw.Int("SELECT count(*) FROM pictures"), 1);
    EXPECT_EQ(raw.Int("SELECT journal_mode = 'wal' FROM pragma_journal_mode"), 1);
}

// The frame graph's marks: a commit with the WAL's size after it, a
// picture's encoding, a checkpoint with what it moved, and a picture read
// back with its size in the file.
TEST_F(LibraryStoreTest, ItsWorkIsMarkedOnTheTimelineWhileItRecords) {
    Timeline& timeline = Timeline::Instance();
    timeline.SetRecording(true);
    {
        LibraryStore store(file_);
        ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
        const int64_t afterCommit = store.UncheckpointedBytes();
        const std::vector<uint8_t> pixels = NoisePixels(64, 64);
        ASSERT_TRUE(store.SaveImage(3, pixels.data(), 64, 64));
        const int64_t beforeCheckpoint = store.UncheckpointedBytes();
        ASSERT_TRUE(store.Checkpoint());
        ASSERT_TRUE(store.LoadImage(3).has_value());

        const Timeline::Spans& spans = timeline.RecordedSpans();
        std::vector<TimelineMark> marks;
        for (size_t i = 0; i < spans.Size(); ++i) {
            marks.push_back(spans[i].mark);
        }
        ASSERT_EQ(marks, (std::vector<TimelineMark>{TimelineMark::Commit, TimelineMark::Encode,
                                                    TimelineMark::Checkpoint, TimelineMark::ReadPicture}));
        EXPECT_EQ(spans[0].bytes, afterCommit);
        EXPECT_GT(spans[1].bytes, 0);
        EXPECT_EQ(spans[2].bytes, beforeCheckpoint);
        EXPECT_GT(spans[3].bytes, 0);
    }
    timeline.SetRecording(false);
}

// A WAL that has grown to the limit is checkpointed by the write that
// took it there, so an overlay left up does not grow it without end.
TEST_F(LibraryStoreTest, AWriteThatFillsTheWalCheckpointsIt) {
    LibraryStore store(file_);
    store.SetCheckpointAtBytesForTesting(64 * 1024);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    ASSERT_GT(store.UncheckpointedBytes(), 0) << "below the limit";
    ASSERT_LT(store.UncheckpointedBytes(), 64 * 1024);
    CanvasManagerSnapshot bigger = MakeSampleSnapshot();
    Stroke stroke;
    for (int i = 0; i < 20000; ++i) {
        stroke.points.push_back({static_cast<float>(i % 300), static_cast<float>(i % 200)});
    }
    bigger.canvases[0].items[0].strokes.push_back(stroke);
    ASSERT_TRUE(store.Save(bigger));
    EXPECT_EQ(store.UncheckpointedBytes(), 0);
    EXPECT_EQ(LibraryStore(file_).Load()->canvases[0].items[0].strokes.size(),
              bigger.canvases[0].items[0].strokes.size());
}

// What a copy that never closed - a crash, a power cut after the WAL
// reached the disk - left in its WAL is read at the next open: every
// commit whole. Here the files are copied while the store has them open,
// which is the state such a copy leaves them in.
TEST_F(LibraryStoreTest, AWalLeftBehindIsReadAtTheOpen) {
    const std::filesystem::path copy = dir_ / "copy" / "library.db";
    CanvasManagerSnapshot later = MakeSampleSnapshot();
    later.canvases[0].name = "Renamed";
    {
        LibraryStore store(file_);
        ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
        ASSERT_TRUE(store.Checkpoint());
        ASSERT_TRUE(store.Save(later));
        ASSERT_GT(store.UncheckpointedBytes(), 0);
        std::filesystem::create_directories(copy.parent_path());
        std::filesystem::copy_file(file_, copy);
        std::filesystem::copy_file(Beside(file_, "-wal"), Beside(copy, "-wal"));
    }
    LibraryStore store(copy);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].name, "Renamed");
    EXPECT_NE(store.UncheckpointedBytes(), 0) << "not moved into the file yet";
}

// The app's store holds its file for itself from the open to the close:
// no other connection reads or writes it meanwhile - another copy of the
// app on another computer sharing the folder is refused at its start - and
// no -shm file is made for others to share its WAL's index through.
TEST_F(LibraryStoreTest, TheAppsStoreHoldsItsFileForItself) {
    ExclusiveLocking exclusive;
    {
        LibraryStore store(file_);
        ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
        EXPECT_FALSE(std::filesystem::exists(Beside(file_, "-shm")));
        {
            RawConnection other(file_);
            EXPECT_EQ(other.Int("SELECT count(*) FROM items"), -1) << "not read";
            EXPECT_FALSE(other.Exec("DELETE FROM items"));
        }
        LibraryStore second(file_);
        EXPECT_EQ(second.Open(), LibraryStore::OpenResult::Unreadable);
        EXPECT_TRUE(store.Save(MakeSampleSnapshot())) << "still the first one's";
    }
    LibraryStore after(file_);
    EXPECT_EQ(after.Open(), LibraryStore::OpenResult::Opened) << "let go at the close";
    EXPECT_TRUE(after.Load().has_value());
}

// ================= Moving from where 0.2.0 kept it =================

// A library where builds up to 0.2.0 kept it, in the roaming profile, is
// moved to where the store's file is now, before the first open.
TEST_F(LibraryStoreTest, ALibraryWhereItWasIsMovedHere) {
    const std::filesystem::path former = dir_ / "roaming" / "library.db";
    const std::filesystem::path here = dir_ / "local" / "library.db";
    ASSERT_TRUE(LibraryStore(former).Save(MakeSampleSnapshot()));
    LibraryStore store(here);
    store.MoveHereFrom(former);
    EXPECT_EQ(store.File(), here);
    EXPECT_FALSE(std::filesystem::exists(former));
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u);
}

// A library here already is the one: the one where it was, left by a
// build from before that ran again, say, is left alone.
TEST_F(LibraryStoreTest, ALibraryHereIsKeptOverOneWhereItWas) {
    const std::filesystem::path former = dir_ / "roaming" / "library.db";
    const std::filesystem::path here = dir_ / "local" / "library.db";
    ASSERT_TRUE(LibraryStore(former).Save(MakeSampleSnapshot()));
    ASSERT_TRUE(LibraryStore(here).Save(CanvasManagerSnapshot{}));
    LibraryStore store(here);
    store.MoveHereFrom(former);
    EXPECT_EQ(store.File(), here);
    EXPECT_TRUE(std::filesystem::exists(former));
    EXPECT_TRUE(store.Load()->canvases.empty());

    LibraryStore nothingThere(dir_ / "elsewhere" / "library.db");
    nothingThere.MoveHereFrom(dir_ / "nowhere" / "library.db");
    EXPECT_EQ(nothingThere.File(), dir_ / "elsewhere" / "library.db");
}

// A library that cannot be moved is opened where it is - here, the place
// it would go is under a file, not a directory.
TEST_F(LibraryStoreTest, ALibraryThatCannotBeMovedIsOpenedWhereItIs) {
    const std::filesystem::path former = dir_ / "roaming" / "library.db";
    ASSERT_TRUE(LibraryStore(former).Save(MakeSampleSnapshot()));
    std::ofstream(dir_ / "local") << "a file";
    LibraryStore store(dir_ / "local" / "library.db");
    store.MoveHereFrom(former);
    EXPECT_EQ(store.File(), former);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u);
}

// A journal a crash left beside it is played back before the move - here
// a copy taken in the middle of a transaction, as a crash would leave the
// files - so that what is moved is the library as its last commit left it,
// and nothing is left behind.
TEST_F(LibraryStoreTest, AJournalACrashLeftIsPlayedBackBeforeTheMove) {
    const std::filesystem::path former = dir_ / "roaming" / "library.db";
    const std::filesystem::path here = dir_ / "local" / "library.db";
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    {
        RawConnection raw(file_);
        ASSERT_TRUE(raw.Exec("PRAGMA journal_mode = DELETE"));  // as 0.2.0 kept it
    }
    {
        RawConnection crashing(file_);
        ASSERT_TRUE(crashing.Exec("PRAGMA cache_size = 1; BEGIN; DELETE FROM items; "
                                  "INSERT INTO pictures (item_id, width, height, pixels) "
                                  "VALUES (99, 1, 1, zeroblob(1000000))"));
        ASSERT_TRUE(std::filesystem::exists(Beside(file_, "-journal")));
        std::filesystem::create_directories(former.parent_path());
        std::filesystem::copy_file(file_, former);
        std::filesystem::copy_file(Beside(file_, "-journal"), Beside(former, "-journal"));
        crashing.Exec("ROLLBACK");
    }
    LibraryStore store(here);
    store.MoveHereFrom(former);
    EXPECT_EQ(store.File(), here);
    EXPECT_FALSE(std::filesystem::exists(Beside(former, "-journal")));
    EXPECT_FALSE(std::filesystem::exists(Beside(here, "-journal")));
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u) << "as last committed";
    EXPECT_FALSE(store.HasImage(99));
}

}  // namespace
}  // namespace sz::core::persistence
