#include "core/persistence/library_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "support/failing_writes.h"

namespace sz::core::persistence {
namespace {

class LibraryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
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
    RawConnection other(file_);
    ASSERT_TRUE(other.Exec("BEGIN EXCLUSIVE"));
    LibraryStore store(file_);
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Unreadable);
    EXPECT_FALSE(store.Load().has_value());
    EXPECT_FALSE(store.Save(CanvasManagerSnapshot{}));
    other.Exec("ROLLBACK");
    EXPECT_TRUE(LibraryStore(file_).Load().has_value());
}

// Opened, and then held by another program before it could be read: not a
// first run, which would start an empty library over it, but a file that
// cannot be read - and nothing is written over it afterwards, even once it
// could be.
TEST_F(LibraryStoreTest, ALibraryThatCannotBeReadThroughIsUnreadableRatherThanAFirstRun) {
    ASSERT_TRUE(LibraryStore(file_).Save(MakeSampleSnapshot()));
    LibraryStore store(file_);
    ASSERT_EQ(store.Open(), LibraryStore::OpenResult::Opened);
    RawConnection other(file_);
    ASSERT_TRUE(other.Exec("BEGIN EXCLUSIVE"));
    EXPECT_FALSE(store.Load().has_value());
    EXPECT_EQ(store.Open(), LibraryStore::OpenResult::Unreadable);
    other.Exec("ROLLBACK");
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
        std::vector<uint8_t> blob = {1, 2, 0, 0, 0};
        const auto put = [&blob](const auto value) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            blob.insert(blob.end(), bytes, bytes + sizeof(value));
        };
        put(uint32_t{0xFF0000FF});
        put(std::numeric_limits<float>::infinity());  // a width that cannot be
        put(uint32_t{1});
        put(1.0f);
        put(2.0f);
        put(uint32_t{0x00FF00FF});
        put(3.0f);
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
    EXPECT_EQ(item.strokes[0].points, (std::vector<StrokePoint>{{1.0f, 2.0f}}));

    RawConnection raw(file_);
    EXPECT_EQ(raw.Int("SELECT json_extract(record, '$.foregroundOpacity') FROM items WHERE id = 3"), 1)
        << "written back";
    EXPECT_EQ(raw.Int("SELECT length(strokes) FROM items WHERE id = 3"), 1 + 4 + 12 + 8) << "one stroke, whole";
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
    const std::filesystem::path file = dir_ / std::filesystem::path(u8"Bibliothek ü 日本") / "library.db";
    ASSERT_TRUE(LibraryStore(file).Save(MakeSampleSnapshot()));
    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_TRUE(LibraryStore(file).Load().has_value());
}

}  // namespace
}  // namespace sz::core::persistence
