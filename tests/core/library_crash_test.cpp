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
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/persistence/library_store.h"
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
            EXPECT_TRUE(was == before.end()) << id << " was there before and is lost:\n" << Describe(loaded);
            continue;
        }
        const bool asBefore = was != before.end() && is->second == was->second;
        const bool asAfter = will != after.end() && is->second == will->second;
        EXPECT_TRUE(asBefore || asAfter) << id << " loaded as neither version: " << is->second.print << " in "
                                         << is->second.parent;
    }
}

// The whole check for a scenario that changes the library by `change`.
void CheckEveryCrashPoint(const std::function<void(LibraryStore&, CanvasManagerSnapshot&)>& change) {
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
        [](FaultyFileSystem& fs) { SetUpLibrary(fs); }, [&](FaultyFileSystem& fs) { RunChange(fs, change); },
        [&](MemoryFileSystem& disk, size_t crashedAfter, size_t of) {
            const Layout loaded = CheckRestart(disk, crashedAfter);
            ExpectBeforeOrAfter(loaded, beforeLayout, afterLayout, crashedAfter, of);
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

// A picture written again over itself - a painted layer does, every save -
// is the old picture or the new one after a crash, never half of either.
TEST(LibraryCrashTest, RewritingAPictureLeavesTheOldOneOrTheNewOne) {
    size_t changes = ForEachCrashPoint(
        [](FaultyFileSystem& fs) { SetUpLibrary(fs); },
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

}  // namespace
}  // namespace sz::core::persistence
