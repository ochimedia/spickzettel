// What a crash at any point of a save leaves the library as.
//
// Each test runs one change to the library - an edit, a move, a rename, a
// capture - with the disk stopping after every possible number of its
// changes (see ForEachCrashPoint), then restarts on what was left and
// checks what comes back against the few rules a crash must never break:
//
//   - nothing loads twice
//   - every picture a record names is there
//   - what loads is the library before the change or after it, snippet by
//     snippet: each one whole, from one side or the other
//   - with no crash at all, what loads is the library after the change
//   - a restart on what the crash left saves, and loads back the same

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/persistence/library_store.h"
#include "core/util/uid.h"
#include "support/crash_points.h"
#include "support/faulty_file_system.h"
#include "support/memory_file_system.h"

namespace sz::core::persistence {
namespace {

using fakes::FaultyFileSystem;
using fakes::ForEachCrashPoint;
using fakes::MemoryFileSystem;

// Only ever in memory; a path that would be valid on this machine.
std::filesystem::path Root() { return std::filesystem::temp_directory_path() / "spickzettel_crash_library"; }

constexpr uint64_t kFolder = 1;
constexpr uint64_t kOtherFolder = 2;
constexpr uint64_t kCanvasA = 10;
constexpr uint64_t kCanvasB = 11;
constexpr uint64_t kShot = 100;     // a capture on A, with a picture
constexpr uint64_t kDrawing = 101;  // strokes on A
constexpr uint64_t kNote = 102;     // on B
constexpr uint64_t kCapture = 103;  // taken during a scenario

std::vector<uint8_t> Pixels(uint8_t shade) {
    return std::vector<uint8_t>(4 * 4 * 4, shade);
}

Item MakeItem(uint64_t id, const std::string& name, int strokes) {
    Item item;
    item.id = id;
    item.name = name;
    item.rect = Rect{10, 20, 300, 200};
    item.nativeW = 300;
    item.nativeH = 200;
    for (int i = 0; i < strokes; ++i) {
        Stroke stroke;
        stroke.points = {StrokePoint{float(i), 1}, StrokePoint{float(i), 2}};
        item.strokes.push_back(stroke);
    }
    return item;
}

Canvas* FindCanvas(CanvasManagerSnapshot& library, uint64_t id) {
    for (Canvas& canvas : library.canvases) {
        if (canvas.id == id) {
            return &canvas;
        }
    }
    return nullptr;
}

// Two folders; canvases A and B in the first; a capture with its picture
// and a drawing on A, a note on B - saved, and then saved again with the
// picture named, the way a capture lands.
CanvasManagerSnapshot MakeLibrary() {
    CanvasManagerSnapshot library;
    for (const auto& [id, name] : {std::pair{kFolder, "Folder"}, std::pair{kOtherFolder, "Other"}}) {
        Folder folder;
        folder.id = id;
        folder.name = name;
        library.folders.push_back(folder);
    }
    for (const auto& [id, name] : {std::pair{kCanvasA, "A"}, std::pair{kCanvasB, "B"}}) {
        Canvas canvas;
        canvas.id = id;
        canvas.name = name;
        canvas.folderId = kFolder;
        library.canvases.push_back(canvas);
    }
    FindCanvas(library, kCanvasA)->items = {MakeItem(kShot, "Shot", 0), MakeItem(kDrawing, "Drawing", 3)};
    FindCanvas(library, kCanvasB)->items = {MakeItem(kNote, "Note", 1)};
    library.currentFolderId = kFolder;
    library.currentCanvasId = kCanvasA;
    return library;
}

void SetUpLibrary(FileSystem& fs) {
    CanvasManagerSnapshot library = MakeLibrary();
    LibraryStore store(Root(), fs);
    ASSERT_TRUE(store.Save(library));
    const std::vector<uint8_t> pixels = Pixels(0x40);
    const std::optional<std::string> file = store.SaveImage(kShot, pixels.data(), 4, 4);
    ASSERT_TRUE(file.has_value());
    FindCanvas(library, kCanvasA)->items[0].ImageLayer()->imageFile = *file;
    ASSERT_TRUE(store.Save(library));
}

// A scenario: a fresh start on the library - a load - then `change` to what
// loaded, then a save, as the app does.
void RunChange(FileSystem& fs, const std::function<void(LibraryStore&, CanvasManagerSnapshot&)>& change) {
    LibraryStore store(Root(), fs);
    std::optional<CanvasManagerSnapshot> library = store.Load();
    if (!library) {
        return;
    }
    change(store, *library);
    store.Save(*library);
}

// Everything a snippet is, for telling one version of it from another: the
// fields the scenarios change, and its pictures.
std::string Fingerprint(const Item& item) {
    std::string print = item.name + "|" + std::to_string(item.strokes.size()) + "|" +
                        std::to_string(item.rect.w) + "x" + std::to_string(item.rect.h);
    for (const Layer& layer : item.layers) {
        print += "|" + layer.imageFile;
    }
    return print;
}

// Where everything is and what it is, by id.
struct Placement {
    uint64_t parent;
    std::string print;  // a snippet's fingerprint, a canvas's or folder's name
    bool operator==(const Placement&) const = default;
};
using Layout = std::map<uint64_t, Placement>;

Layout LayoutOf(const CanvasManagerSnapshot& library) {
    Layout layout;
    for (const Folder& folder : library.folders) {
        layout[folder.id] = {0, folder.name};
    }
    for (const Canvas& canvas : library.canvases) {
        layout[canvas.id] = {canvas.folderId, canvas.name};
        for (const Item& item : canvas.items) {
            layout[item.id] = {canvas.id, Fingerprint(item)};
        }
    }
    return layout;
}

std::string Describe(const Layout& layout) {
    std::string out;
    for (const auto& [id, placement] : layout) {
        out += "  " + std::to_string(id) + " in " + std::to_string(placement.parent) + ": " + placement.print + "\n";
    }
    return out;
}

// The rules every restart is held to, whatever the scenario; returns what
// loaded, for the scenario's own.
Layout CheckRestart(MemoryFileSystem& disk, size_t crashedAfter) {
    SCOPED_TRACE("crashed after " + std::to_string(crashedAfter) + " changes");
    LibraryStore store(Root(), disk);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    EXPECT_TRUE(loaded.has_value()) << "the library did not load at all";
    if (!loaded) {
        return {};
    }

    std::multiset<uint64_t> ids;
    for (const Folder& folder : loaded->folders) {
        ids.insert(folder.id);
    }
    for (const Canvas& canvas : loaded->canvases) {
        ids.insert(canvas.id);
        for (const Item& item : canvas.items) {
            ids.insert(item.id);
            for (const Layer& layer : item.layers) {
                if (!layer.imageFile.empty()) {
                    EXPECT_TRUE(store.LoadImage(item.id, layer.imageFile).has_value())
                        << "snippet " << item.id << " names " << layer.imageFile << ", which does not load";
                }
            }
        }
    }
    for (const uint64_t id : ids) {
        EXPECT_EQ(ids.count(id), 1u) << id << " loaded more than once";
    }

    // And the restart gets going again: what it loaded saves, and loads
    // back as itself.
    const Layout layout = LayoutOf(*loaded);
    EXPECT_TRUE(store.Save(*loaded)) << "the first save after the restart failed";
    LibraryStore again(Root(), disk);
    const std::optional<CanvasManagerSnapshot> reloaded = again.Load();
    EXPECT_TRUE(reloaded.has_value());
    if (reloaded) {
        EXPECT_EQ(LayoutOf(*reloaded), layout) << "loaded:\n" << Describe(layout) << "then:\n"
                                                << Describe(LayoutOf(*reloaded));
    }
    return layout;
}

// What a scenario may leave: each id at one of the places, as one of the
// versions, that it had before the change or after it - or, for something
// the change created, absent.
void ExpectBeforeOrAfter(const Layout& loaded, const Layout& before, const Layout& after, size_t crashedAfter,
                         size_t changes) {
    SCOPED_TRACE("crashed after " + std::to_string(crashedAfter) + " of " + std::to_string(changes) + " changes");
    if (crashedAfter == changes) {
        EXPECT_EQ(loaded, after) << "with no crash, the library is not the one saved:\n" << Describe(loaded);
        return;
    }
    std::set<uint64_t> ids;
    for (const Layout* layout : {&loaded, &before, &after}) {
        for (const auto& [id, placement] : *layout) {
            (void)placement;
            ids.insert(id);
        }
    }
    for (const uint64_t id : ids) {
        const auto was = before.find(id);
        const auto will = after.find(id);
        const auto is = loaded.find(id);
        if (is == loaded.end()) {
            EXPECT_TRUE(was == before.end() || will == after.end())
                << id << " was there before and after, and is lost:\n" << Describe(loaded);
            continue;
        }
        const bool asBefore = was != before.end() && is->second == was->second;
        const bool asAfter = will != after.end() && is->second == will->second;
        EXPECT_TRUE(asBefore || asAfter) << id << " loaded as neither version: " << is->second.print << " in "
                                         << is->second.parent;
    }
}

// Whether `path` is the directory of `uid`, or inside it: a directory name
// ends in its uid.
bool IsOf(const std::filesystem::path& path, uint64_t uid) {
    const std::string suffix = "-" + FormatUid(uid);
    for (const std::filesystem::path& part : path) {
        const std::string name = part.string();
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

// A matcher for `file` - a record - in the directory of `uid`.
std::function<bool(const std::filesystem::path&)> RecordOf(uint64_t uid, const char* file) {
    return [uid, file](const std::filesystem::path& path) {
        return path.filename() == file && path.parent_path().filename().string().ends_with("-" + FormatUid(uid));
    };
}

// A matcher for the directory of `uid` itself.
std::function<bool(const std::filesystem::path&)> DirectoryOf(uint64_t uid) {
    return [uid](const std::filesystem::path& path) { return path.filename().string().ends_with("-" + FormatUid(uid)); };
}

// Nothing of what was deleted for good is anywhere in the library - not in
// the tree, not set aside in retired/ - once a restart has saved.
void ExpectNothingLeftOf(MemoryFileSystem& disk, const std::vector<uint64_t>& erased, size_t crashedAfter) {
    for (const auto& [path, text] : disk.FilesUnder(Root())) {
        (void)text;
        for (const uint64_t uid : erased) {
            EXPECT_FALSE(IsOf(path, uid)) << path << " is left of " << uid << ", deleted for good (crashed after "
                                          << crashedAfter << ")";
        }
    }
}

// The file a snippet's picture is in, wherever its directory is.
std::filesystem::path PictureOf(MemoryFileSystem& disk, uint64_t item) {
    for (const auto& [path, text] : disk.FilesUnder(Root() / "folders")) {
        (void)text;
        if (path.filename() == FormatUid(item) + ".qoi") {
            return path;
        }
    }
    return {};
}

// The whole check for a scenario that changes the library by `change`,
// deleting `erased` for good on the way (none by default). `holdPicturesOf`
// are held open, as a picture viewer would, from before the change until
// the restart.
void CheckEveryCrashPoint(const std::function<void(LibraryStore&, CanvasManagerSnapshot&)>& change,
                          const std::vector<uint64_t>& erased = {},
                          const std::vector<uint64_t>& holdPicturesOf = {}) {
    CanvasManagerSnapshot before = MakeLibrary();
    // `before` as it is on disk after SetUpLibrary, picture named; `after`
    // is the same change applied to it.
    {
        MemoryFileSystem disk;
        SetUpLibrary(disk);
        LibraryStore store(Root(), disk);
        before = *store.Load();
    }
    Layout beforeLayout = LayoutOf(before);
    Layout afterLayout;
    {
        MemoryFileSystem disk;
        SetUpLibrary(disk);
        RunChange(disk, change);
        LibraryStore store(Root(), disk);
        afterLayout = LayoutOf(*store.Load());
    }
    ASSERT_NE(beforeLayout, afterLayout) << "the scenario changes nothing";

    const size_t changes = ForEachCrashPoint(
        [&](FaultyFileSystem& fs, MemoryFileSystem& disk) {
            SetUpLibrary(fs);
            for (const uint64_t item : holdPicturesOf) {
                const std::filesystem::path picture = PictureOf(disk, item);
                ASSERT_FALSE(picture.empty());
                fs.Hold(picture);
            }
        },
        [&](FaultyFileSystem& fs) { RunChange(fs, change); },
        [&](MemoryFileSystem& disk, size_t crashedAfter, size_t of) {
            const Layout loaded = CheckRestart(disk, crashedAfter);
            ExpectBeforeOrAfter(loaded, beforeLayout, afterLayout, crashedAfter, of);
            // Recorded - or it would have loaded, whole, above - and so
            // finished by the restart's save.
            if (!erased.empty() && loaded.count(erased.front()) == 0) {
                ExpectNothingLeftOf(disk, erased, crashedAfter);
            }
        });
    EXPECT_GT(changes, 0u);
}

TEST(LibraryCrashTest, EditingASnippet) {
    CheckEveryCrashPoint([](LibraryStore&, CanvasManagerSnapshot& library) {
        Item& drawing = FindCanvas(library, kCanvasA)->items[1];
        drawing.name = "Drawing, edited";
        drawing.strokes.push_back(drawing.strokes.front());
        drawing.rect.w = 500;
    });
}

TEST(LibraryCrashTest, MovingASnippetWithItsPictureToAnotherCanvas) {
    CheckEveryCrashPoint([](LibraryStore&, CanvasManagerSnapshot& library) {
        std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
        FindCanvas(library, kCanvasB)->items.push_back(onA[0]);
        onA.erase(onA.begin());
    });
}

TEST(LibraryCrashTest, RenamingACanvas) {
    CheckEveryCrashPoint([](LibraryStore&, CanvasManagerSnapshot& library) {
        FindCanvas(library, kCanvasA)->name = "A, renamed";
    });
}

TEST(LibraryCrashTest, MovingACanvasToAnotherFolder) {
    CheckEveryCrashPoint([](LibraryStore&, CanvasManagerSnapshot& library) {
        FindCanvas(library, kCanvasA)->folderId = kOtherFolder;
    });
}

TEST(LibraryCrashTest, RenamingAFolderAndEditingASnippetInside) {
    CheckEveryCrashPoint([](LibraryStore&, CanvasManagerSnapshot& library) {
        library.folders[0].name = "Folder, renamed";
        FindCanvas(library, kCanvasB)->items[0].name = "Note, edited";
    });
}

// A capture is written the moment it is taken, before its snippet has a
// directory - into staging - and the save that follows brings it home.
TEST(LibraryCrashTest, CapturingANewSnippet) {
    CheckEveryCrashPoint([](LibraryStore& store, CanvasManagerSnapshot& library) {
        const std::vector<uint8_t> pixels = Pixels(0x80);
        Item capture = MakeItem(kCapture, "Capture", 0);
        capture.ImageLayer()->imageFile = store.SaveImage(kCapture, pixels.data(), 4, 4).value_or("");
        FindCanvas(library, kCanvasB)->items.push_back(capture);
    });
}

// Deleting for good, a snippet, a canvas and a folder. A crash before the
// removal is recorded leaves it as it was, whole, to be deleted again; one
// after never brings any of it back, and the restart finishes it.
TEST(LibraryCrashTest, DeletingASnippetForGood) {
    CheckEveryCrashPoint(
        [](LibraryStore& store, CanvasManagerSnapshot& library) {
            std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
            onA.erase(onA.begin());
            store.Remove({kShot}, library);
        },
        {kShot});
}

TEST(LibraryCrashTest, DeletingACanvasForGood) {
    CheckEveryCrashPoint(
        [](LibraryStore& store, CanvasManagerSnapshot& library) {
            library.canvases.erase(library.canvases.begin());
            store.Remove({kCanvasA, kShot, kDrawing}, library);
        },
        {kCanvasA, kShot, kDrawing});
}

TEST(LibraryCrashTest, DeletingAFolderForGood) {
    CheckEveryCrashPoint(
        [](LibraryStore& store, CanvasManagerSnapshot& library) {
            library.folders.erase(library.folders.begin());
            library.canvases.clear();
            store.Remove({kFolder, kCanvasA, kCanvasB, kShot, kDrawing, kNote}, library);
        },
        {kFolder, kCanvasA, kCanvasB, kShot, kDrawing, kNote});
}

// A snippet moved to another canvas and the canvas it left deleted for
// good, with its picture held open so that the move cannot land: the
// removal waits for it, and a crash or a restart meanwhile finds the
// snippet where it was moved to - never the deleted canvas back, and
// never the snippet lost with it.
TEST(LibraryCrashTest, DeletingForGoodACanvasASnippetWasJustMovedOutOfWhileItsPictureIsHeld) {
    CheckEveryCrashPoint(
        [](LibraryStore& store, CanvasManagerSnapshot& library) {
            std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
            FindCanvas(library, kCanvasB)->items.push_back(onA[0]);
            library.canvases.erase(library.canvases.begin());
            store.Remove({kCanvasA, kDrawing}, library);
            store.Save(library);  // the move, still held
        },
        {kCanvasA, kDrawing}, {kShot});
}

// The same a level up: a canvas moved to another folder and the folder
// deleted for good, a picture in the canvas held open.
TEST(LibraryCrashTest, DeletingForGoodAFolderACanvasWasJustMovedOutOfWhileAPictureInItIsHeld) {
    CheckEveryCrashPoint(
        [](LibraryStore& store, CanvasManagerSnapshot& library) {
            FindCanvas(library, kCanvasA)->folderId = kOtherFolder;
            library.folders.erase(library.folders.begin());
            library.canvases.erase(library.canvases.begin() + 1);  // B, in the folder
            store.Remove({kFolder, kCanvasB, kNote}, library);
            store.Save(library);
        },
        {kFolder, kCanvasB, kNote}, {kShot});
}

// A canvas deleted for good while a picture in one of its snippets is held
// open: the removal stops at the picture, having taken the snippet's record,
// and a save retries while it is still held. Once let go, nothing is left -
// the snippet's directory, with no record in it any more, is still known
// for what it is by its uid.
TEST(LibraryCrashTest, DeletingACanvasForGoodWhileOneOfItsPicturesIsHeld) {
    CheckEveryCrashPoint(
        [](LibraryStore& store, CanvasManagerSnapshot& library) {
            library.canvases.erase(library.canvases.begin());
            store.Remove({kCanvasA, kShot, kDrawing}, library);
            store.Save(library);  // the retry, still held
        },
        {kCanvasA, kShot, kDrawing}, {kShot});
}

// A snippet moved into a canvas and the canvas deleted for good, before any
// save: the snippet's directory is still under the canvas it came from, and
// goes too - not set aside in retired/ as something gone missing.
TEST(LibraryCrashTest, DeletingForGoodACanvasASnippetWasJustMovedInto) {
    CheckEveryCrashPoint(
        [](LibraryStore& store, CanvasManagerSnapshot& library) {
            std::vector<Item>& onB = FindCanvas(library, kCanvasB)->items;
            FindCanvas(library, kCanvasA)->items.push_back(onB[0]);
            onB.clear();
            library.canvases.erase(library.canvases.begin());
            store.Remove({kCanvasA, kShot, kDrawing, kNote}, library);
        },
        {kCanvasA, kShot, kDrawing, kNote});
}

// A directory that cannot be listed cannot be said to be emptied: the
// removal stays owed, and a restart does not read it back.
TEST(LibraryFaultTest, ARemovalThatCouldNotListADirectoryIsNotDone) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    FaultyFileSystem fs(disk);
    {
        LibraryStore store(Root(), fs);
        CanvasManagerSnapshot library = *store.Load();
        fs.FailWhen(FaultyFileSystem::Op::List, [](const std::filesystem::path& path) {
            return IsOf(path, kCanvasA) && !IsOf(path, kShot) && !IsOf(path, kDrawing);
        });
        library.canvases.erase(library.canvases.begin());
        EXPECT_FALSE(store.Remove({kCanvasA, kShot, kDrawing}, library));
        EXPECT_TRUE(store.HasPendingRemoval(kCanvasA));
    }
    fs.ClearFailures();
    LibraryStore restarted(Root(), fs);
    const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(LayoutOf(*loaded).count(kCanvasA), 0u) << "deleted for good, and back";
    EXPECT_TRUE(restarted.Save(*loaded));
    EXPECT_FALSE(restarted.HasPendingRemoval(kCanvasA));
    ExpectNothingLeftOf(disk, {kCanvasA, kShot, kDrawing}, 0);
}

// What a save that stopped partway leaves of a canvas whose own record
// never landed - its record's temporary, torn by a crash; or its order file
// and snippets, with no canvas record above them, one of them no more than
// a picture moved in from staging - is not loaded, and so
// not named when its folder is deleted for good. It goes with the folder
// all the same, rather than keeping the folder's directory standing.
TEST(LibraryFaultTest, WhatASaveLeftOfADirectoryWithoutItsRecordGoesWithWhatItIsIn) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    std::filesystem::path folderDir;
    for (const auto& [path, text] : disk.FilesUnder(Root() / "folders")) {
        (void)text;
        if (path.filename() == "folder.json" && IsOf(path, kFolder)) {
            folderDir = path.parent_path();
        }
    }
    ASSERT_FALSE(folderDir.empty());
    ASSERT_TRUE(disk.Put(folderDir / "torn-00000x" / "canvas.json.tmp", "{\"id\": \"00"));
    ASSERT_TRUE(disk.Put(folderDir / "unrecorded-00000y" / "order.json", "{\"items\": []}"));
    ASSERT_TRUE(disk.Put(folderDir / "unrecorded-00000y" / "snippet-00000z" / "item.json", "{\"id\": \"00000z\"}"));
    ASSERT_TRUE(disk.Put(folderDir / "unrecorded-00000y" / "shot-00000w" / "00000w.qoi", "pixels"));

    LibraryStore store(Root(), disk);
    CanvasManagerSnapshot library = *store.Load();
    library.folders.erase(library.folders.begin());
    library.canvases.clear();
    EXPECT_TRUE(store.Remove({kFolder, kCanvasA, kCanvasB, kShot, kDrawing, kNote}, library));
    EXPECT_EQ(disk.Status(folderDir), FileSystem::Kind::None) << "left standing";
}

// A crash between a removal's last step and the rewrite of pending.json
// leaves the file naming what is gone. That is owed too, until a save has
// brought the file in line - or nothing would ever rewrite it.
TEST(LibraryFaultTest, APendingRecordNamingWhatIsGoneIsBroughtInLine) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    ASSERT_TRUE(disk.Put(Root() / "pending.json", "{\"erased\": [\"zzzzzz\"]}"));

    LibraryStore store(Root(), disk);
    const std::optional<CanvasManagerSnapshot> library = store.Load();
    ASSERT_TRUE(library.has_value());
    EXPECT_EQ(LayoutOf(*library).size(), 7u) << "nothing it names is here to skip";
    EXPECT_TRUE(store.HasPendingRemovals());
    EXPECT_TRUE(store.Save(*library));
    EXPECT_FALSE(store.HasPendingRemovals());
    EXPECT_EQ(disk.Status(Root() / "pending.json"), FileSystem::Kind::None);
}

// A canvas moved into a folder that has never been saved, and every other
// folder deleted for good while the canvas's picture is held open, so that
// the move cannot land - and then the process stops. The folder the record
// says the canvas belongs to never reached the disk, and no other is left:
// the canvas is rescued into a new folder rather than left unread for the
// next save to sweep away with the folder it is still in.
TEST(LibraryFaultTest, ARescueWithNowhereToGoGetsAFolderOfItsOwn) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    FaultyFileSystem fs(disk);
    fs.Hold(PictureOf(disk, kShot));
    constexpr uint64_t kNewFolder = 3;
    {
        LibraryStore store(Root(), fs);
        CanvasManagerSnapshot library = *store.Load();
        Folder fresh;
        fresh.id = kNewFolder;
        fresh.name = "New";
        library.folders.push_back(fresh);
        FindCanvas(library, kCanvasA)->folderId = kNewFolder;
        // The new folder's directory is never made - the process is gone
        // before a save gets that far.
        fs.FailWhen(FaultyFileSystem::Op::MakeDirectory,
                    [](const std::filesystem::path& path) { return IsOf(path, kNewFolder); });
        library.folders.erase(library.folders.begin(), library.folders.begin() + 2);
        library.canvases.erase(library.canvases.begin() + 1);  // B
        EXPECT_FALSE(store.Remove({kFolder, kOtherFolder, kCanvasB, kNote}, library));
        EXPECT_FALSE(store.Save(library));
    }
    fs.ClearFailures();
    fs.ReleaseAll();

    LibraryStore restarted(Root(), fs);
    std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->folders.size(), 1u);
    EXPECT_EQ(loaded->folders[0].name, "Recovered");
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].id, kCanvasA);
    EXPECT_EQ(loaded->canvases[0].folderId, loaded->folders[0].id);
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u) << "the capture and the drawing, with it";

    EXPECT_TRUE(restarted.Save(*loaded));
    ExpectNothingLeftOf(disk, {kFolder, kOtherFolder, kCanvasB, kNote}, 0);
    LibraryStore again(Root(), disk);
    const std::optional<CanvasManagerSnapshot> reloaded = again.Load();
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(LayoutOf(*reloaded), LayoutOf(*loaded));
    EXPECT_TRUE(again.LoadImage(kShot, reloaded->canvases[0].items[0].ImageLayer()->imageFile).has_value() ||
                again.LoadImage(kShot, reloaded->canvases[0].items[1].ImageLayer()->imageFile).has_value());
}

// A snippet moved out of a canvas that is then deleted for good, the
// process gone before the save that moves it - and at the restart, its
// record cannot be read for a moment, or the canvas it is still in cannot
// be listed (`op` failing on what `fault` matches). It is not rescued that
// start, and not deleted with the canvas either: unreadable is never
// deleted. The first start where it can be read puts it where it was moved
// to, and finishes the removal.
void CheckWhatWasMovedOutIsKeptWhileItCannotBeRead(FaultyFileSystem::Op op,
                                                  const std::function<bool(const std::filesystem::path&)>& fault) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    FaultyFileSystem fs(disk);
    {
        LibraryStore store(Root(), fs);
        CanvasManagerSnapshot library = *store.Load();
        std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
        FindCanvas(library, kCanvasB)->items.push_back(onA[0]);
        library.canvases.erase(library.canvases.begin());
        EXPECT_FALSE(store.Remove({kCanvasA, kDrawing}, library)) << "waits for the move";
    }  // gone before the save
    fs.FailWhen(op, fault);
    {
        LibraryStore restarted(Root(), fs);
        const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(LayoutOf(*loaded).count(kShot), 0u) << "not read this start";
        fs.ClearFailures();  // the other program lets go
        restarted.Save(*loaded);
        EXPECT_FALSE(PictureOf(disk, kShot).empty()) << "deleted with the canvas it was moved out of";
    }
    LibraryStore again(Root(), fs);
    const std::optional<CanvasManagerSnapshot> reloaded = again.Load();
    ASSERT_TRUE(reloaded.has_value());
    const Layout layout = LayoutOf(*reloaded);
    ASSERT_EQ(layout.count(kShot), 1u);
    EXPECT_EQ(layout.at(kShot).parent, kCanvasB);
    EXPECT_TRUE(again.Save(*reloaded));
    EXPECT_FALSE(again.HasPendingRemovals());
    ExpectNothingLeftOf(disk, {kCanvasA, kDrawing}, 0);
}

TEST(LibraryFaultTest, WhatWasMovedOutOfSomethingDeletedForGoodIsKeptWhileItsRecordCannotBeRead) {
    CheckWhatWasMovedOutIsKeptWhileItCannotBeRead(FaultyFileSystem::Op::Read, RecordOf(kShot, "item.json"));
}

TEST(LibraryFaultTest, WhatWasMovedOutOfSomethingDeletedForGoodIsKeptWhileWhatItIsInCannotBeListed) {
    CheckWhatWasMovedOutIsKeptWhileItCannotBeRead(FaultyFileSystem::Op::List, DirectoryOf(kCanvasA));
}

// A snippet deleted for good, recorded, and the process stopped before
// anything was removed - and at the restart, the record of the canvas it is
// in cannot be read for a moment, or the canvas cannot be listed (`op`
// failing on what `fault` matches). The first save does not forget the
// removal: the next start where the canvas reads does not load the snippet
// back, and nothing of it is left once a save has run with nothing in the
// way.
void CheckARemovalUnderADirectoryThatCouldNotBeReadIsNotForgotten(
    FaultyFileSystem::Op op, const std::function<bool(const std::filesystem::path&)>& fault) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    FaultyFileSystem fs(disk);
    {
        LibraryStore store(Root(), fs);
        CanvasManagerSnapshot library = *store.Load();
        std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
        onA.erase(onA.begin());
        fs.CrashAfter(2);  // pending.json's temporary and its rename, then nothing
        store.Remove({kShot}, library);
    }
    fs.ClearCrash();
    ASSERT_FALSE(PictureOf(disk, kShot).empty());
    fs.FailWhen(op, fault);
    {
        LibraryStore restarted(Root(), fs);
        const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
        ASSERT_TRUE(loaded.has_value());
        restarted.Save(*loaded);
    }
    fs.ClearFailures();
    LibraryStore again(Root(), fs);
    const std::optional<CanvasManagerSnapshot> reloaded = again.Load();
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(LayoutOf(*reloaded).count(kShot), 0u) << "deleted for good, and back";
    EXPECT_TRUE(again.Save(*reloaded));
    EXPECT_FALSE(again.HasPendingRemovals());
    ExpectNothingLeftOf(disk, {kShot}, 0);
}

TEST(LibraryFaultTest, ARemovalUnderARecordThatCouldNotBeReadIsNotForgotten) {
    CheckARemovalUnderADirectoryThatCouldNotBeReadIsNotForgotten(FaultyFileSystem::Op::Read,
                                                                 RecordOf(kCanvasA, "canvas.json"));
}

TEST(LibraryFaultTest, ARemovalUnderADirectoryThatCouldNotBeListedIsNotForgotten) {
    CheckARemovalUnderADirectoryThatCouldNotBeReadIsNotForgotten(FaultyFileSystem::Op::List,
                                                                 DirectoryOf(kCanvasA));
}

// A capture saved, and its picture not yet moved in from staging when the
// process stopped - and at the restart, its record cannot be read for a
// moment. The save that session does not take the picture for one nothing
// names and set it aside: the next start where the record reads finds it.
TEST(LibraryFaultTest, APictureInStagingIsKeptWhileItsSnippetCannotBeRead) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    FaultyFileSystem fs(disk);
    const auto inStaging = [](const std::filesystem::path& path) {
        return path.parent_path().filename() == "staging";
    };
    {
        LibraryStore store(Root(), fs);
        CanvasManagerSnapshot library = *store.Load();
        Item capture = MakeItem(kCapture, "Capture", 0);
        const std::vector<uint8_t> pixels = Pixels(0x80);
        const std::optional<std::string> file = store.SaveImage(kCapture, pixels.data(), 4, 4);
        ASSERT_TRUE(file.has_value());
        capture.ImageLayer()->imageFile = *file;
        FindCanvas(library, kCanvasB)->items.push_back(capture);
        fs.FailWhen(FaultyFileSystem::Op::Rename, inStaging);  // the process gone before it moves
        EXPECT_TRUE(store.Save(library));
    }
    fs.ClearFailures();
    fs.FailWhen(FaultyFileSystem::Op::Read, RecordOf(kCapture, "item.json"));
    {
        LibraryStore restarted(Root(), fs);
        const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(LayoutOf(*loaded).count(kCapture), 0u);
        restarted.Save(*loaded);
    }
    fs.ClearFailures();
    LibraryStore again(Root(), fs);
    std::optional<CanvasManagerSnapshot> reloaded = again.Load();
    ASSERT_TRUE(reloaded.has_value());
    const Canvas* canvasB = FindCanvas(*reloaded, kCanvasB);
    ASSERT_NE(canvasB, nullptr);
    const auto capture = std::find_if(canvasB->items.begin(), canvasB->items.end(),
                                      [](const Item& item) { return item.id == kCapture; });
    ASSERT_NE(capture, canvasB->items.end());
    EXPECT_TRUE(again.LoadImage(kCapture, capture->ImageLayer()->imageFile).has_value()) << "set aside";
}

// A canvas moved out of a folder, a snippet moved from another canvas of
// that folder into it, and the folder deleted for good - the process gone
// before the save that moves either. The rescue puts the canvas back first,
// so that the snippet finds it: the other way round, which is how the uids
// sort here, it went to whichever canvas came first.
TEST(LibraryFaultTest, ASnippetIsRescuedIntoACanvasRescuedWithIt) {
    constexpr uint64_t kLateFolder = 50;  // sorts after the canvases in it
    MemoryFileSystem disk;
    {
        CanvasManagerSnapshot library = MakeLibrary();
        library.folders[0].id = kLateFolder;
        for (Canvas& canvas : library.canvases) {
            canvas.folderId = kLateFolder;
        }
        library.currentFolderId = kLateFolder;
        LibraryStore store(Root(), disk);
        ASSERT_TRUE(store.Save(library));
    }
    {
        LibraryStore store(Root(), disk);
        CanvasManagerSnapshot library = *store.Load();
        std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
        FindCanvas(library, kCanvasB)->items.push_back(onA[0]);
        FindCanvas(library, kCanvasB)->folderId = kOtherFolder;
        library.folders.erase(library.folders.begin());
        library.canvases.erase(library.canvases.begin());  // A, with the folder
        EXPECT_FALSE(store.Remove({kLateFolder, kCanvasA, kDrawing}, library));
    }
    LibraryStore restarted(Root(), disk);
    const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
    ASSERT_TRUE(loaded.has_value());
    const Layout layout = LayoutOf(*loaded);
    ASSERT_EQ(layout.count(kShot), 1u);
    EXPECT_EQ(layout.at(kShot).parent, kCanvasB);
    ASSERT_EQ(layout.count(kCanvasB), 1u);
    EXPECT_EQ(layout.at(kCanvasB).parent, kOtherFolder);
    EXPECT_EQ(loaded->canvases.size(), 1u) << "no canvas made up to hold it";
}

// A rescue whose target never reached the disk goes to the first folder or
// canvas that is not in the trash - not into one that is, hidden there,
// and erased with it by the retention pass that follows the load.
TEST(LibraryFaultTest, ARescueWithItsTargetGoneDoesNotGoIntoTheTrash) {
    MemoryFileSystem disk;
    {
        CanvasManagerSnapshot library = MakeLibrary();
        Canvas trashed;
        trashed.id = 9;  // first in its folder
        trashed.name = "Trashed";
        trashed.folderId = kFolder;
        trashed.deletedAt = 1700000000;
        library.canvases.insert(library.canvases.begin(), trashed);
        LibraryStore store(Root(), disk);
        ASSERT_TRUE(store.Save(library));
    }
    constexpr uint64_t kNewCanvas = 12;
    {
        LibraryStore store(Root(), disk);
        CanvasManagerSnapshot library = *store.Load();
        // Into a canvas just made, which the process is gone before saving.
        Canvas fresh;
        fresh.id = kNewCanvas;
        fresh.name = "New";
        fresh.folderId = kOtherFolder;
        std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
        fresh.items.push_back(onA[0]);
        library.canvases.push_back(fresh);
        library.canvases.erase(library.canvases.begin() + 1);  // A
        EXPECT_FALSE(store.Remove({kCanvasA, kDrawing}, library));
    }
    LibraryStore restarted(Root(), disk);
    const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
    ASSERT_TRUE(loaded.has_value());
    const Layout layout = LayoutOf(*loaded);
    ASSERT_EQ(layout.count(kShot), 1u);
    EXPECT_EQ(layout.at(kShot).parent, kCanvasB);
}

// The same a level up: a canvas moved into a folder just made, and the
// folder it left deleted for good, with the only other folder in the
// trash. It gets a folder of its own rather than going into the trash.
TEST(LibraryFaultTest, ARescuedCanvasWithItsFolderGoneDoesNotGoIntoTheTrash) {
    MemoryFileSystem disk;
    {
        CanvasManagerSnapshot library = MakeLibrary();
        library.folders[1].deletedAt = 1700000000;
        LibraryStore store(Root(), disk);
        ASSERT_TRUE(store.Save(library));
    }
    constexpr uint64_t kNewFolder = 3;
    {
        LibraryStore store(Root(), disk);
        CanvasManagerSnapshot library = *store.Load();
        Folder fresh;
        fresh.id = kNewFolder;
        fresh.name = "New";
        library.folders.push_back(fresh);
        FindCanvas(library, kCanvasA)->folderId = kNewFolder;
        library.folders.erase(library.folders.begin());
        library.canvases.erase(library.canvases.begin() + 1);  // B, with the folder
        EXPECT_FALSE(store.Remove({kFolder, kCanvasB, kNote}, library));
    }
    LibraryStore restarted(Root(), disk);
    const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->folders.size(), 2u);
    ASSERT_EQ(loaded->canvases.size(), 1u);
    const Folder& home = loaded->folders[0].id == loaded->canvases[0].folderId ? loaded->folders[0]
                                                                                : loaded->folders[1];
    EXPECT_EQ(home.id, loaded->canvases[0].folderId);
    EXPECT_EQ(home.deletedAt, 0);
    EXPECT_EQ(home.name, "Recovered");
}

// A saved snippet moved into a canvas just made, and the save that would
// write both cannot write the canvas's record. The snippet is not moved
// into a directory Load would not read - it waits where it was, and a
// restart finds it there.
TEST(LibraryFaultTest, NothingIsMovedUnderADirectoryWhoseRecordDidNotLand) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    FaultyFileSystem fs(disk);
    constexpr uint64_t kNewCanvas = 12;
    {
        LibraryStore store(Root(), fs);
        CanvasManagerSnapshot library = *store.Load();
        Canvas fresh;
        fresh.id = kNewCanvas;
        fresh.name = "New";
        fresh.folderId = kFolder;
        std::vector<Item>& onA = FindCanvas(library, kCanvasA)->items;
        fresh.items.push_back(onA[0]);
        onA.erase(onA.begin());
        library.canvases.push_back(fresh);
        fs.FailWhen(FaultyFileSystem::Op::WriteNewFile, [](const std::filesystem::path& path) {
            return IsOf(path, kNewCanvas) && path.filename().string().rfind("canvas.json", 0) == 0;
        });
        EXPECT_FALSE(store.Save(library));
    }
    fs.ClearFailures();

    LibraryStore restarted(Root(), fs);
    const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
    ASSERT_TRUE(loaded.has_value());
    const Layout layout = LayoutOf(*loaded);
    ASSERT_EQ(layout.count(kShot), 1u) << "a saved snippet lost to a canvas whose record never landed";
    EXPECT_EQ(layout.at(kShot).parent, kCanvasA);
    CanvasManagerSnapshot copy = *loaded;
    for (const Item& item : FindCanvas(copy, kCanvasA)->items) {
        if (item.id == kShot) {
            EXPECT_TRUE(restarted.LoadImage(kShot, item.ImageLayer()->imageFile).has_value());
        }
    }
}

// A pending.json that cannot be written deletes nothing: the removal is owed
// in memory, and a restart finds everything as it was.
TEST(LibraryFaultTest, ARemovalThatCannotBeRecordedRemovesNothing) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    const std::map<std::filesystem::path, std::string> before = disk.FilesUnder(Root() / "folders");
    FaultyFileSystem fs(disk);
    fs.FailWhen(FaultyFileSystem::Op::WriteNewFile, [](const std::filesystem::path& path) {
        return path.filename().string().rfind("pending.json", 0) == 0;
    });
    LibraryStore store(Root(), fs);
    CanvasManagerSnapshot library = *store.Load();
    library.canvases.erase(library.canvases.begin());
    EXPECT_FALSE(store.Remove({kCanvasA, kShot, kDrawing}, library));
    EXPECT_TRUE(store.HasPendingRemoval(kCanvasA));
    EXPECT_EQ(disk.FilesUnder(Root() / "folders"), before);
    EXPECT_FALSE(store.Save(library)) << "a removal that cannot be recorded fails the save";

    fs.ClearFailures();
    EXPECT_TRUE(store.Save(library));
    EXPECT_FALSE(store.HasPendingRemoval(kCanvasA));
    ExpectNothingLeftOf(disk, {kCanvasA, kShot, kDrawing}, 0);
}

// A picture written again over itself - a painted layer does, every save -
// is the old picture or the new one after a crash, never half of either.
TEST(LibraryCrashTest, RewritingAPictureLeavesTheOldOneOrTheNewOne) {
    size_t changes = ForEachCrashPoint(
        [](FaultyFileSystem& fs, MemoryFileSystem&) { SetUpLibrary(fs); },
        [](FaultyFileSystem& fs) {
            LibraryStore store(Root(), fs);
            store.Load();
            const std::vector<uint8_t> pixels = Pixels(0xc0);
            store.SaveImage(kShot, pixels.data(), 4, 4);
        },
        [](MemoryFileSystem& disk, size_t crashedAfter, size_t) {
            SCOPED_TRACE("crashed after " + std::to_string(crashedAfter));
            LibraryStore store(Root(), disk);
            const std::optional<CanvasManagerSnapshot> loaded = store.Load();
            ASSERT_TRUE(loaded.has_value());
            const Item& shot = loaded->canvases[0].items[0];
            ASSERT_EQ(shot.id, kShot);
            const std::optional<DecodedImage> picture = store.LoadImage(kShot, shot.ImageLayer()->imageFile);
            ASSERT_TRUE(picture.has_value());
            ASSERT_FALSE(picture->pixelsRGBA.empty());
            const uint8_t shade = picture->pixelsRGBA[0];
            EXPECT_TRUE(shade == 0x40 || shade == 0xc0) << int(shade);
            EXPECT_TRUE(std::all_of(picture->pixelsRGBA.begin(), picture->pixelsRGBA.end(),
                                    [shade](uint8_t v) { return v == shade; }));
        });
    EXPECT_GT(changes, 0u);
}

// A library.json that cannot be read is not taken for an older one's: a
// moment's hold is waited out, and one that goes on is refused like a newer
// library, since the first save would write it back at this build's
// version, whatever it said.
TEST(LibraryCrashTest, AnUnreadableLibraryFileIsNotTakenForAnOlderOne) {
    MemoryFileSystem memory;
    SetUpLibrary(memory);
    FaultyFileSystem disk(memory);
    const auto isPointerFile = [](const std::filesystem::path& path) { return path.filename() == "library.json"; };

    disk.FailWhen(FaultyFileSystem::Op::Read, isPointerFile, 2);
    {
        LibraryStore store(Root(), disk);
        EXPECT_FALSE(store.WrittenByANewerVersion()) << "a moment's hold, waited out";
        EXPECT_TRUE(store.Load().has_value());
    }

    disk.FailWhen(FaultyFileSystem::Op::Read, isPointerFile);
    LibraryStore store(Root(), disk);
    EXPECT_TRUE(store.WrittenByANewerVersion());
    EXPECT_TRUE(store.VersionUnreadable());
    EXPECT_FALSE(store.Load().has_value());
    EXPECT_FALSE(store.Save(MakeLibrary()));
}

// The version is asked once. A library.json read at the tray's check and
// held when Load looks again is not a library gone - which started the app
// as a first run, on a store refusing every write.
TEST(LibraryCrashTest, ALibraryFileHeldAfterTheVersionCheckStillLoads) {
    MemoryFileSystem memory;
    SetUpLibrary(memory);
    const Layout expected = LayoutOf(*LibraryStore(Root(), memory).Load());
    FaultyFileSystem disk(memory);
    LibraryStore store(Root(), disk);
    ASSERT_FALSE(store.WrittenByANewerVersion());
    disk.FailWhen(FaultyFileSystem::Op::Read,
                  [](const std::filesystem::path& path) { return path.filename() == "library.json"; });
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(LayoutOf(*loaded), expected);
    disk.ClearFailures();
    EXPECT_TRUE(store.Save(*loaded));
}

// What in library.json's place cannot be read by asking again - a
// directory - is not taken for a newer library: the tree loads, and the
// pointers are repaired, as for a pointer file that is not JSON.
TEST(LibraryCrashTest, ADirectoryWhereTheLibraryFileBelongsIsNotANewerLibrary) {
    MemoryFileSystem disk;
    SetUpLibrary(disk);
    const Layout expected = LayoutOf(*LibraryStore(Root(), disk).Load());
    ASSERT_TRUE(disk.Remove(Root() / "library.json"));
    ASSERT_TRUE(disk.MakeDirectory(Root() / "library.json"));
    LibraryStore store(Root(), disk);
    EXPECT_FALSE(store.WrittenByANewerVersion());
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(LayoutOf(*loaded), expected);
}

// A record that could not be read at a start - another program holding
// it - is not in the library that session, and the save does not write it
// out of its order file: read again at the next start, the snippet is where
// it was in its canvas's stack, and the canvas where it was in its folder.
TEST(LibraryCrashTest, WhatALoadCouldNotReadKeepsItsPlace) {
    MemoryFileSystem memory;
    SetUpLibrary(memory);
    FaultyFileSystem disk(memory);
    // One start with `unreadable` failing to read, something else changed
    // and saved - and the next start's load.
    const auto startWithout = [&](const std::function<bool(const std::filesystem::path&)>& unreadable) {
        disk.FailWhen(FaultyFileSystem::Op::Read, unreadable);
        {
            LibraryStore store(Root(), disk);
            std::optional<CanvasManagerSnapshot> library = store.Load();
            EXPECT_TRUE(library.has_value());
            if (Canvas* canvasB = library ? FindCanvas(*library, kCanvasB) : nullptr) {
                canvasB->name += "+";  // something to save
            }
            EXPECT_TRUE(library && store.Save(*library));
        }
        disk.ClearFailures();
        return LibraryStore(Root(), disk).Load();
    };

    std::optional<CanvasManagerSnapshot> library = startWithout(RecordOf(kShot, "item.json"));
    ASSERT_TRUE(library.has_value());
    const Canvas* canvasA = FindCanvas(*library, kCanvasA);
    ASSERT_NE(canvasA, nullptr);
    ASSERT_EQ(canvasA->items.size(), 2u);
    EXPECT_EQ(canvasA->items[0].id, kShot) << "at the back of its canvas, as it was";

    library = startWithout(RecordOf(kCanvasA, "canvas.json"));
    ASSERT_TRUE(library.has_value());
    ASSERT_EQ(library->canvases.size(), 2u);
    EXPECT_EQ(library->canvases[0].id, kCanvasA) << "first in its folder, as it was";
}

}  // namespace
}  // namespace sz::core::persistence
