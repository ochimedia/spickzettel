#include "core/persistence/library_store.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <fstream>

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/util/slug.h"
#include "core/util/uid.h"

namespace sz::core::persistence {
namespace {

class LibraryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / (std::string("spickzettel_library_store_test_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
    std::filesystem::path ShotItemDir() const {
        return dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "shot-1-000004";
    }
    // Where the sample's canvas is set aside, should a save find it gone
    // (see LibraryStore::Save) - the same path it has under folders/, one
    // directory over.
    std::filesystem::path RetiredCanvasDir() const {
        return dir_ / "retired" / "folder-1-000001" / "canvas-1-000002";
    }
};

// The one obstruction that fails a write without failing everything around
// it: a directory at every temporary name the writer would try (see
// WriteFileAtomically, which passes over a name something is at). A
// directory at the first name alone is stepped around.
void ObstructEveryTemporaryName(const std::filesystem::path& destination) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::filesystem::path tmp = destination;
        tmp += attempt == 0 ? std::string(".tmp") : ".tmp" + std::to_string(attempt);
        std::filesystem::create_directories(tmp);
    }
}
void ClearTemporaryObstructions(const std::filesystem::path& destination) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::filesystem::path tmp = destination;
        tmp += attempt == 0 ? std::string(".tmp") : ".tmp" + std::to_string(attempt);
        std::filesystem::remove_all(tmp);
    }
}

CanvasManagerSnapshot MakeSampleSnapshot() {
    CanvasManagerSnapshot snapshot;

    Folder folder;
    folder.id = 1;
    folder.name = "Folder 1";
    snapshot.folders.push_back(folder);

    Canvas canvas;
    canvas.id = 2;
    canvas.name = "Canvas 1";
    canvas.folderId = 1;

    Item drawing;
    drawing.id = 3;
    drawing.hasBackground = false;
    drawing.name = "Drawing 1";
    drawing.rect = Rect{10, 20, 300, 200};
    drawing.nativeW = 300;
    drawing.nativeH = 200;
    drawing.foregroundOpacity = 0.75f;
    drawing.ImageLayer()->opacity = 0.3f;
    drawing.ImageLayer()->tintColorRGBA = 0xaabbccffu;
    drawing.isFullscreen = false;
    drawing.minimized = true;
    drawing.pinned = true;
    Stroke stroke;
    stroke.colorRGBA = 0x11223344u;
    stroke.width = 4.5f;
    stroke.points = {StrokePoint{1, 2}, StrokePoint{3, 4}, StrokePoint{5, 6}};
    drawing.noteText = "A caption, styled per snippet";
    drawing.noteTextColorRGBA = 0x4dd6b880u;  // teal at half alpha - text carries its own opacity
    drawing.noteTextSizePx = 34.0f;
    drawing.keepAspect = false;  // not the default, so a lost field would show
    drawing.strokes.push_back(stroke);
    canvas.items.push_back(drawing);

    Item shot;
    shot.id = 4;
    shot.hasBackground = true;
    shot.name = "Shot 1";
    shot.rect = Rect{0, 0, 1920, 1080};
    shot.nativeW = 1920;
    shot.nativeH = 1080;
    shot.isFullscreen = true;
    shot.isFullscreenStretch = true;
    shot.anchorRect = Rect{50, 50, 400, 300};
    shot.anchorDisplayWidth = 1920.0f;
    shot.anchorDisplayHeight = 1080.0f;
    shot.ImageLayer()->placeholderHue = 123.5f;
    shot.ImageLayer()->opacity = 0.9f;
    shot.ImageLayer()->tintColorRGBA = 0x112233ffu;
    shot.ImageLayer()->imageFile = "000004.qoi";
    canvas.items.push_back(shot);

    snapshot.canvases.push_back(canvas);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    return snapshot;
}

// ===== Writing a library by hand =====
//
// Two levels, because the tests below are about two different things. The
// ones that care what a *record* says use WriteLibraryTree and hand over a
// whole library in one convenient blob; the ones that care what the *tree*
// says place directories themselves, because that is the thing under test.

// An id as a test document spells it: a number, for brevity, or the
// base36 spelling records use. WriteLibraryTree writes either as the
// latter.
uint64_t IdOf(const nlohmann::json& record, const char* key = "id") {
    const auto it = record.find(key);
    if (it == record.end()) {
        return 0;
    }
    if (it->is_string()) {
        return ParseUid(it->get<std::string>()).value_or(0);
    }
    return it->get<uint64_t>();
}

// Splays a whole library, written in one piece, into the directories the
// store actually reads. Folders and canvases go where their ids say.
// The keys of `record` that hold an id, spelled as records spell them.
nlohmann::json WithIdsSpelled(nlohmann::json record) {
    for (const char* key : {"id", "folderId", "currentFolderId", "currentCanvasId"}) {
        if (const auto it = record.find(key); it != record.end() && it->is_number_unsigned()) {
            *it = FormatUid(it->get<uint64_t>());
        }
    }
    return record;
}

void WriteLibraryTree(const std::filesystem::path& root, const std::string& document) {
    nlohmann::json doc = nlohmann::json::parse(document);
    std::filesystem::create_directories(root);

    nlohmann::json globals = WithIdsSpelled(doc);
    globals.erase("folders");
    globals.erase("canvases");
    std::ofstream(root / "library.json") << globals.dump(2);

    for (const auto& folder : doc.value("folders", nlohmann::json::array())) {
        const std::string dirName = "f-" + FormatUid(IdOf(folder));
        const std::filesystem::path folderDir = root / "folders" / dirName;
        std::filesystem::create_directories(folderDir);
        std::ofstream(folderDir / "folder.json") << WithIdsSpelled(folder).dump(2);

        for (const auto& canvas : doc.value("canvases", nlohmann::json::array())) {
            if (IdOf(canvas, "folderId") != IdOf(folder)) {
                continue;
            }
            const std::filesystem::path canvasDir =
                folderDir / ("c-" + FormatUid(IdOf(canvas)));
            std::filesystem::create_directories(canvasDir);
            nlohmann::json canvasRecord = WithIdsSpelled(canvas);
            canvasRecord.erase("items");
            canvasRecord.erase("folderId");  // routing for this helper; the directory says it
            std::ofstream(canvasDir / "canvas.json") << canvasRecord.dump(2);

            // Each snippet is a directory of its own beside the canvas
            // record, in the order the document listed them.
            std::vector<std::string> itemOrder;
            for (const auto& item : canvas.value("items", nlohmann::json::array())) {
                const std::string itemDirName = "i-" + FormatUid(IdOf(item));
                std::filesystem::create_directories(canvasDir / itemDirName);
                std::ofstream(canvasDir / itemDirName / "item.json") << WithIdsSpelled(item).dump(2);
                itemOrder.push_back(FormatUid(IdOf(item)));
            }
            std::ofstream(canvasDir / "order.json") << nlohmann::json{{"items", itemOrder}}.dump(2);
        }
    }
}

// The tree, a directory at a time - for the tests that are about the layout
// itself, where hiding it behind a helper would hide the thing being tested.
std::filesystem::path PlaceFolder(const std::filesystem::path& root, const std::string& dirName,
                                   uint64_t id, const std::string& name) {
    const std::filesystem::path dir = root / "folders" / dirName;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "folder.json") << nlohmann::json{{"id", FormatUid(id)}, {"name", name}}.dump(2);
    return dir;
}

std::filesystem::path PlaceCanvas(const std::filesystem::path& folderDir, const std::string& dirName,
                                   uint64_t id, const std::string& name) {
    const std::filesystem::path dir = folderDir / dirName;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "canvas.json") << nlohmann::json{{"id", FormatUid(id)},
                                                          {"name", name}}
                                              .dump(2);
    return dir;
}

void PlaceOrderFile(const std::filesystem::path& dir, const char* key,
                     const std::vector<std::string>& names) {
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "order.json") << nlohmann::json{{key, names}}.dump(2);
}

// A picture with enough structure that a downscale is visibly a
// downscale rather than a flat fill.
std::vector<uint8_t> Checkerboard(int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t value = ((x / 8 + y / 8) % 2) == 0 ? 0 : 255;
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i] = value;
            pixels[i + 1] = value;
            pixels[i + 2] = value;
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

TEST_F(LibraryStoreTest, LoadReturnsNulloptWhenNothingSavedYet) {
    LibraryStore store(dir_);
    EXPECT_FALSE(store.Load().has_value());
}

// Sets library.json's version to `version`, or removes it for nullopt,
// keeping everything else in the file as the save wrote it.
void StampLibraryVersion(const std::filesystem::path& dir, std::optional<int> version) {
    std::ifstream in(dir / "library.json");
    nlohmann::json doc = nlohmann::json::parse(in);
    in.close();
    if (version) {
        doc["version"] = *version;
    } else {
        doc.erase("version");
    }
    std::ofstream(dir / "library.json") << doc.dump();
}

std::string FileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A library a newer build wrote is neither read nor written: Load reads
// nothing, and every write fails, leaving the files as they were - one
// saved back by this build would lose what the newer one put there.
TEST_F(LibraryStoreTest, ALibraryWrittenByANewerVersionIsNeitherReadNorWritten) {
    const CanvasManagerSnapshot original = MakeSampleSnapshot();
    ASSERT_TRUE(LibraryStore(dir_).Save(original));
    StampLibraryVersion(dir_, LibraryStore::kFormatVersion + 1);
    const std::string pointerFile = FileText(dir_ / "library.json");
    const std::string shotRecord = FileText(ShotItemDir() / "item.json");
    ASSERT_FALSE(shotRecord.empty());

    LibraryStore store(dir_);
    EXPECT_TRUE(store.WrittenByANewerVersion());
    EXPECT_FALSE(store.Load().has_value());

    CanvasManagerSnapshot changed = original;
    changed.canvases[0].name = "Renamed";
    changed.canvases[0].items[1].rect.x += 50.0f;
    EXPECT_FALSE(store.Save(changed));
    EXPECT_FALSE(store.Remove(4, changed));
    const uint8_t pixels[4] = {1, 2, 3, 255};
    EXPECT_FALSE(store.SaveImage(4, pixels, 1, 1).has_value());
    EXPECT_EQ(store.WriteGeneration(), 0u);

    EXPECT_EQ(FileText(dir_ / "library.json"), pointerFile);
    EXPECT_EQ(FileText(ShotItemDir() / "item.json"), shotRecord);
}

// This build's own version, and a library.json without one, open as usual.
TEST_F(LibraryStoreTest, ALibraryOfThisVersionOrWithoutOneOpens) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    for (const std::optional<int> version : {std::optional<int>(LibraryStore::kFormatVersion), std::optional<int>()}) {
        StampLibraryVersion(dir_, version);
        LibraryStore store(dir_);
        EXPECT_FALSE(store.WrittenByANewerVersion());
        ASSERT_TRUE(store.Load().has_value());
        EXPECT_TRUE(store.Save(MakeSampleSnapshot()));
    }
}

// A name that cannot be spelled in UTF-8 - a lone surrogate, which NTFS
// allows - is no name the store writes, and is passed over like any other
// file that is not the store's: Load, Save and a delete for good go on
// around it, and leave it where it is.
TEST_F(LibraryStoreTest, ANameThatIsNotUtf8IsLeftAloneAndStopsNothing) {
    const CanvasManagerSnapshot original = MakeSampleSnapshot();
    ASSERT_TRUE(LibraryStore(dir_).Save(original));
    const std::wstring loneSurrogate(1, static_cast<wchar_t>(0xD800));
    const std::filesystem::path strayFile = ShotItemDir() / (loneSurrogate + L"note.txt");
    const std::filesystem::path strayDir = ShotItemDir().parent_path() / (loneSurrogate + L"scans");
    std::ofstream(strayFile) << "someone else's";
    ASSERT_TRUE(std::filesystem::exists(strayFile)) << "the name is allowed where the test runs";
    std::filesystem::create_directories(strayDir);

    LibraryStore store(dir_);
    std::optional<CanvasManagerSnapshot> loaded;
    ASSERT_NO_THROW(loaded = store.Load());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases.size(), 1u);
    bool saved = false;
    ASSERT_NO_THROW(saved = store.Save(*loaded));
    EXPECT_TRUE(saved);

    CanvasManagerSnapshot without = *loaded;
    without.canvases[0].items.pop_back();  // the shot, deleted for good
    ASSERT_NO_THROW(store.Remove(4, without));
    EXPECT_TRUE(std::filesystem::exists(strayFile)) << "not the store's to delete";
    EXPECT_TRUE(std::filesystem::exists(strayDir));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "item.json"));
}

TEST_F(LibraryStoreTest, SaveThenLoadRoundTripsEverything) {
    LibraryStore store(dir_);
    const CanvasManagerSnapshot original = MakeSampleSnapshot();

    ASSERT_TRUE(store.Save(original));
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());

    EXPECT_EQ(loaded->currentFolderId, original.currentFolderId);
    EXPECT_EQ(loaded->currentCanvasId, original.currentCanvasId);
    ASSERT_EQ(loaded->folders.size(), 1u);
    EXPECT_EQ(loaded->folders[0].name, "Folder 1");
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].name, "Canvas 1");
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u);

    const Item& drawing = loaded->canvases[0].items[0];
    EXPECT_EQ(drawing.id, 3u);
    EXPECT_FALSE(drawing.hasBackground);
    EXPECT_FLOAT_EQ(drawing.rect.x, 10.0f);
    EXPECT_FLOAT_EQ(drawing.foregroundOpacity, 0.75f);
    EXPECT_EQ(drawing.noteText, "A caption, styled per snippet");
    EXPECT_EQ(drawing.noteTextColorRGBA, 0x4dd6b880u);
    EXPECT_FLOAT_EQ(drawing.noteTextSizePx, 34.0f);
    EXPECT_FALSE(drawing.keepAspect);
    EXPECT_FLOAT_EQ(drawing.ImageLayer()->opacity, 0.3f);
    EXPECT_EQ(drawing.ImageLayer()->tintColorRGBA, 0xaabbccffu);
    EXPECT_TRUE(drawing.minimized);
    EXPECT_TRUE(drawing.pinned);
    ASSERT_EQ(drawing.strokes.size(), 1u);
    EXPECT_EQ(drawing.strokes[0].colorRGBA, 0x11223344u);
    ASSERT_EQ(drawing.strokes[0].points.size(), 3u);
    EXPECT_FLOAT_EQ(drawing.strokes[0].points[1].x, 3.0f);
    EXPECT_EQ(drawing.ImageLayer()->textureHandle, 0u);  // never persisted/reloaded here

    const Item& shot = loaded->canvases[0].items[1];
    EXPECT_EQ(shot.id, 4u);
    EXPECT_TRUE(shot.isFullscreen);
    EXPECT_TRUE(shot.isFullscreenStretch);
    EXPECT_FLOAT_EQ(shot.anchorRect.x, 50.0f);
    EXPECT_FLOAT_EQ(shot.anchorDisplayWidth, 1920.0f);
    EXPECT_FLOAT_EQ(shot.anchorDisplayHeight, 1080.0f);
    EXPECT_FLOAT_EQ(shot.ImageLayer()->placeholderHue, 123.5f);
    EXPECT_FLOAT_EQ(shot.ImageLayer()->opacity, 0.9f);
    EXPECT_EQ(shot.ImageLayer()->tintColorRGBA, 0x112233ffu);
    EXPECT_EQ(shot.ImageLayer()->imageFile, "000004.qoi");
}

// An item is never layerless, however broken the record - everything
// downstream reaches for ImageLayer() without checking.
TEST_F(LibraryStoreTest, LoadGivesAnItemALayerEvenIfTheListIsEmpty) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": [
            {"id": 3, "name": "A", "layers": []}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items[0].layers.size(), 1u);
}

// More than one layer round-trips in order, which is the whole reason the
// list exists - nothing creates a second one yet, so this is what holds the
// door open for the painting work.
TEST_F(LibraryStoreTest, SaveThenLoadRoundTripsSeveralLayersInOrder) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Item& shot = snapshot.canvases[0].items[1];
    Layer overlay;
    overlay.opacity = 0.5f;
    overlay.tintColorRGBA = 0x00ff00ffu;
    overlay.imageFile = "over.qoi";
    shot.layers.push_back(overlay);

    ASSERT_TRUE(store.Save(snapshot));
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());

    const Item& reloaded = loaded->canvases[0].items[1];
    ASSERT_EQ(reloaded.layers.size(), 2u);
    EXPECT_EQ(reloaded.layers[0].imageFile, "000004.qoi");
    EXPECT_EQ(reloaded.layers[1].imageFile, "over.qoi");
    EXPECT_FLOAT_EQ(reloaded.layers[1].opacity, 0.5f);
}

// The image GC walks every layer, not just the first: a second layer's
// pixels are as live as the bottom one's.
TEST_F(LibraryStoreTest, SaveKeepsImagesReferencedByAnyLayer) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = {1, 2, 3, 255};
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 1, 1).has_value());
    ASSERT_TRUE(store.SaveImage(8, pixels.data(), 1, 1).has_value());

    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Item& shot = snapshot.canvases[0].items[1];
    shot.layers[0].imageFile = "000007.qoi";
    Layer overlay;
    overlay.imageFile = "000008.qoi";
    shot.layers.push_back(overlay);

    ASSERT_TRUE(store.Save(snapshot));

    // Both moved out of staging and in with the snippet that names them,
    // which is what makes a snippet one thing to move.
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000007.qoi"));
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000008.qoi"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "staging" / "000007.qoi")) << "staging is drained";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "staging")) << "and gone once empty";
}

TEST_F(LibraryStoreTest, LoadDefaultsAMissingItemAnchorToNotYetAnchored) {
    std::filesystem::create_directories(dir_);
    // A record without the anchor keys - they fall back to 0/a zero Rect,
    // which CanvasManager::SyncItemsToDisplaySize reads as "not yet
    // anchored" (see Item::anchorRect's own doc comment) rather than a
    // real 0x0 anchor - it just adopts the loaded rect as-is the first
    // time it runs.
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": [
            {"id": 3, "name": "A", "rect": {"x": 1, "y": 2, "w": 3, "h": 4}}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[0].anchorDisplayWidth, 0.0f);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[0].anchorDisplayHeight, 0.0f);
}

// Text styling is per item (see Item::noteTextColorRGBA/noteTextSizePx);
// a record with neither key reads back in the default style: opaque white
// at the UI font's own 17px.
TEST_F(LibraryStoreTest, LoadDefaultsAMissingNoteTextStyle) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": [
            {"id": 3, "name": "A", "noteText": "no style of its own"}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items[0].noteTextColorRGBA, 0xFFFFFFFFu);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[0].noteTextSizePx, 17.0f);
}

// Item::keepAspect is newer than the records, and one without it reads back
// doing what its handles did before there was a setting: keeping the shape
// until the snippet had text in it.
TEST_F(LibraryStoreTest, ARecordFromBeforeKeepAspectKeepsItsShapeUnlessItHasText) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": [
            {"id": 3, "name": "A"},
            {"id": 4, "name": "B", "noteText": "a caption"}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u);
    EXPECT_TRUE(loaded->canvases[0].items[0].keepAspect);
    EXPECT_FALSE(loaded->canvases[0].items[1].keepAspect);
}

// Out-of-band sizes are pulled into range rather than rejected outright -
// a hand-edited or corrupted 0 would otherwise render an invisible
// caption with no way to tell it from an empty one.
// 1e100 is valid JSON and infinite as a float, and one infinite coordinate
// poisons every bounding box it meets. Read as the field's default, with
// the rest of the record kept.
TEST_F(LibraryStoreTest, LoadReadsANumberTooLargeForAFloatAsTheDefault) {
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "folderId": 1, "name": "C", "items": [{
            "id": 3, "name": "Huge",
            "rect": {"x": 1e100, "y": 20, "w": 300, "h": -1e300},
            "nativeW": 1e6, "nativeH": 1e40, "foregroundOpacity": 7,
            "strokes": [{"width": 1e100, "points": [{"x": 1e100, "y": 5}, {"x": 10, "y": 6}]}],
            "layers": [{"opacity": 5, "resolutionScale": 100}]
        }]}]
    })");
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u) << "the record is kept";
    const Item& item = loaded->canvases[0].items[0];
    EXPECT_FLOAT_EQ(item.rect.x, 0.0f);
    EXPECT_FLOAT_EQ(item.rect.y, 20.0f) << "the finite fields are untouched";
    EXPECT_FLOAT_EQ(item.rect.h, 0.0f);
    EXPECT_FLOAT_EQ(item.nativeW, 65536.0f) << "finite and absurd: held to the top of the sensible range";
    EXPECT_FLOAT_EQ(item.nativeH, 0.0f) << "infinite: the default, not the top of the range";
    EXPECT_FLOAT_EQ(item.foregroundOpacity, 1.0f);
    ASSERT_EQ(item.strokes.size(), 1u);
    EXPECT_FLOAT_EQ(item.strokes[0].width, 3.0f);
    EXPECT_FLOAT_EQ(item.strokes[0].points[0].x, 0.0f);
    EXPECT_FLOAT_EQ(item.strokes[0].points[0].y, 5.0f);
    EXPECT_FLOAT_EQ(item.layers[0].opacity, 1.0f);
    EXPECT_FLOAT_EQ(item.layers[0].resolutionScale, 16.0f);
}

// ...and what was repaired reaches the disk with the next save. A record
// read back exactly as a save would write it is noted as written and left
// alone (see IncrementalSaveTest); one the read had to change is not.
TEST_F(LibraryStoreTest, TheNextSaveWritesBackWhatTheLoadRepaired) {
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "folderId": 1, "name": "C", "items": [{
            "id": 3, "name": "Huge", "nativeW": 1e6, "layers": [{"opacity": 5}]
        }]}]
    })");
    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(store.Save(*loaded));

    // Under whatever the save named the snippet's directory.
    nlohmann::json record;
    for (const auto& entry : std::filesystem::directory_iterator(dir_ / "folders" / "f-000001" / "c-000002")) {
        if (entry.is_directory()) {
            record = nlohmann::json::parse(std::ifstream(entry.path() / "item.json"));
        }
    }
    ASSERT_TRUE(record.is_object());
    EXPECT_FLOAT_EQ(record["nativeW"].get<float>(), 65536.0f) << "held to the range, on disk now";
    EXPECT_FLOAT_EQ(record["layers"][0]["opacity"].get<float>(), 1.0f);
}

TEST_F(LibraryStoreTest, LoadClampsAnOutOfRangeNoteTextSize) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": [
            {"id": 3, "name": "A", "noteText": "tiny", "noteTextSizePx": 0.0},
            {"id": 4, "name": "B", "noteText": "huge", "noteTextSizePx": 5000.0}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[0].noteTextSizePx, kNoteTextSizeMin);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[1].noteTextSizePx, kNoteTextSizeMax);
}

// Keys this version knows nothing about must not stop the rest of the
// library from loading.
TEST_F(LibraryStoreTest, UnknownKeysInLibraryJsonAreIgnored) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "someSetting": [4, 0, 1], "anotherOne": {"a": 1},
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": []}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases.size(), 1u);
}

TEST_F(LibraryStoreTest, LoadReturnsNulloptForGarbageJson) {
    std::filesystem::create_directories(dir_);
    std::ofstream(dir_ / "library.json") << "{ this is not valid json";

    LibraryStore store(dir_);
    EXPECT_FALSE(store.Load().has_value());
}

// Deleting the last canvas and the last folder is allowed (see
// CanvasManager's own class comment), so an entirely empty library is a
// state the app can genuinely be in and save. Refusing to load it back
// would quietly resurrect whatever was there before on the next start.
TEST_F(LibraryStoreTest, SaveThenLoadRoundTripsAnEmptyLibrary) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot empty;
    empty.currentFolderId = 0;
    empty.currentCanvasId = 0;
    ASSERT_TRUE(store.Save(empty));

    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->folders.empty());
    EXPECT_TRUE(loaded->canvases.empty());
    EXPECT_EQ(loaded->currentFolderId, 0u);
    EXPECT_EQ(loaded->currentCanvasId, 0u);
}

// Refusing the whole library over this would be the wrong answer for a
// tree: "the canvas that was current is no longer here" is what a supported
// gesture - moving its directory out while the app is closed - leaves
// behind. So it is repaired, not rejected.
TEST_F(LibraryStoreTest, LoadRepairsACurrentCanvasIdOfZeroWithCanvasesPresent) {
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 0,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": []}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->currentCanvasId, 2u) << "should fall back to a canvas that is actually there";
    EXPECT_EQ(loaded->currentFolderId, 1u);
}

TEST_F(LibraryStoreTest, LoadRepairsACurrentCanvasIdThatNamesNothing) {
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 777, "currentCanvasId": 999,
        "folders": [{"id": 1, "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "folderId": 1, "items": []}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->currentCanvasId, 2u);
    EXPECT_EQ(loaded->currentFolderId, 1u);
}

// ...but an empty library still says "nothing is current" rather than
// inventing something, since there is nothing to fall back to.
TEST_F(LibraryStoreTest, LoadLeavesNothingCurrentWhenThereIsNothing) {
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 5, "currentCanvasId": 6,
        "folders": [], "canvases": []
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->currentFolderId, 0u);
    EXPECT_EQ(loaded->currentCanvasId, 0u);
}

// Ids are drawn at random and checked against what is actually loaded, so
// the property that matters is that a
// hand-written library, whatever ids it happens to name, cannot make the
// allocator hand out one that is already in use.
TEST_F(LibraryStoreTest, IdsMintedAfterLoadingAHandWrittenLibraryAvoidWhatItNames) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 5, "currentCanvasId": 6,
        "folders": [{"id": 5, "name": "F"}],
        "canvases": [{"id": 6, "name": "C", "folderId": 5,
                       "items": [{"id": 7, "name": "I"}]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());

    CanvasManager manager;
    manager.ImportSnapshot(*loaded);
    for (int i = 0; i < 50; ++i) {
        const CanvasId minted = manager.AddCanvas("x");
        EXPECT_NE(minted, 5u);
        EXPECT_NE(minted, 6u);
        EXPECT_NE(minted, 7u);
    }
}

// ===== The tree, rearranged by hand =====
//
// Everything below is somebody moving directories around in a file manager
// while the app is closed. None of it is an error case: the layout exists to
// make these gestures work, so each one has an expected outcome rather than
// a diagnostic.

TEST_F(LibraryStoreTest, SaveWritesADirectoryPerFolderAndCanvas) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));

    const std::filesystem::path folderDir = dir_ / "folders" / "folder-1-000001";
    EXPECT_TRUE(std::filesystem::exists(folderDir / "folder.json"));
    EXPECT_TRUE(std::filesystem::exists(folderDir / "canvas-1-000002" / "canvas.json"));
    EXPECT_TRUE(std::filesystem::exists(folderDir / "order.json"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "order.json"));
    // The globals file keeps only what has no other home - no folder or
    // canvas list, because the directories are that list.
    const std::optional<nlohmann::json> globals = [&] {
        std::ifstream in(dir_ / "library.json");
        return std::optional<nlohmann::json>(nlohmann::json::parse(in));
    }();
    ASSERT_TRUE(globals.has_value());
    EXPECT_FALSE(globals->contains("folders"));
    EXPECT_FALSE(globals->contains("canvases"));
    EXPECT_TRUE(globals->contains("currentCanvasId"));
}

TEST_F(LibraryStoreTest, ACanvasDirectoryMovedIntoAnotherFolderBelongsToThatFolder) {
    PlaceFolder(dir_, "work-000001", 1, "Work");
    const std::filesystem::path play = PlaceFolder(dir_, "play-000002", 2, "Play");
    // In Play's directory, as dragging it across from Work leaves it: the
    // directory decides, and nothing in the record says otherwise.
    PlaceCanvas(play, "notes-000003", 3, "Notes");
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].folderId, 2u) << "where it is decides";
}

TEST_F(LibraryStoreTest, ACopiedCanvasDirectoryBecomesACanvasOfItsOwn) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "notes-000003", 3, "Notes");
    // The same canvas.json under a second directory name: a copy-paste in a
    // file manager, which duplicates the id along with everything else.
    PlaceCanvas(work, "notes-copy-000003", 3, "Notes");
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 2u);
    EXPECT_NE(loaded->canvases[0].id, loaded->canvases[1].id) << "the copy has to get an id of its own";
    EXPECT_NE(loaded->canvases[0].id, 0u);
    EXPECT_NE(loaded->canvases[1].id, 0u);
}

// An order file lists uids - the same spelling the records and directory
// names use.
TEST_F(LibraryStoreTest, OrderFilesListUids) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    const nlohmann::json order = nlohmann::json::parse(std::ifstream(canvasDir / "order.json"));
    EXPECT_EQ(order["items"], (nlohmann::json::array({"000003", "000004"})));
    EXPECT_EQ(nlohmann::json::parse(std::ifstream(dir_ / "folders" / "order.json"))["folders"],
              (nlohmann::json::array({"000001"})));
}

TEST_F(LibraryStoreTest, AnOrderFileDecidesTheOrderOfWhatIsThere) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "a-000003", 3, "A");
    PlaceCanvas(work, "b-000004", 4, "B");
    PlaceCanvas(work, "c-000005", 5, "C");
    // Deliberately not alphabetical, so passing cannot be an accident of
    // directory enumeration order.
    PlaceOrderFile(work, "canvases", {"000005", "000003", "000004"});
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 3u);
    EXPECT_EQ(loaded->canvases[0].id, 5u);
    EXPECT_EQ(loaded->canvases[1].id, 3u);
    EXPECT_EQ(loaded->canvases[2].id, 4u);
}

TEST_F(LibraryStoreTest, AnOrderFileNamingSomethingGoneSimplySkipsIt) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "a-000003", 3, "A");
    PlaceOrderFile(work, "canvases", {"0000zz", "000003"});
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].id, 3u);
}

TEST_F(LibraryStoreTest, ACanvasTheOrderFileDoesNotKnowAboutGoesToTheEnd) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "a-000003", 3, "A");
    PlaceCanvas(work, "b-000004", 4, "B");
    // "a" is listed; "b" was dropped in afterwards and nothing knows it yet.
    PlaceOrderFile(work, "canvases", {"000003"});
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 2u);
    EXPECT_EQ(loaded->canvases[0].id, 3u);
    EXPECT_EQ(loaded->canvases[1].id, 4u) << "the newcomer goes last, not first";
}

// The readable half of a directory name is a label, and the trailing uid is
// the identity - so renaming the label by hand keeps the canvas's place.
TEST_F(LibraryStoreTest, RenamingTheReadableHalfOfADirectoryKeepsItsPlace) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "renamed-by-hand-000003", 3, "A");
    PlaceCanvas(work, "b-000004", 4, "B");
    PlaceOrderFile(work, "canvases", {"000003", "000004"});
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 2u);
    EXPECT_EQ(loaded->canvases[0].id, 3u) << "matched on the uid, not on the words in front of it";
}

TEST_F(LibraryStoreTest, ADirectoryWithNoRecordInItIsLeftAlone) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "a-000003", 3, "A");
    std::filesystem::create_directories(work / "something-else");        // not ours
    std::filesystem::create_directories(dir_ / "folders" / "stray-dir");  // nor this
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->folders.size(), 1u);
    EXPECT_EQ(loaded->canvases.size(), 1u);
    // Left alone by the load *and* by what follows it - see
    // ADirectoryWithNoRecordInItSurvivesASave.
}

// A rename in the app moves the directory rather than leaving the old one
// behind - the label is regenerated from the current name every save.
TEST_F(LibraryStoreTest, RenamingACanvasMovesItsDirectoryRatherThanLeavingTwo) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    const std::filesystem::path folderDir = dir_ / "folders" / "folder-1-000001";
    ASSERT_TRUE(std::filesystem::exists(folderDir / "canvas-1-000002"));

    snapshot.canvases[0].name = "Renamed";
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_TRUE(std::filesystem::exists(folderDir / "renamed-000002" / "canvas.json"));
    EXPECT_FALSE(std::filesystem::exists(folderDir / "canvas-1-000002"))
        << "the old directory should have been moved, not duplicated";
    // ...and the content came with it rather than being rebuilt empty.
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u);
}

TEST_F(LibraryStoreTest, MovingACanvasToAnotherFolderInTheAppMovesItsDirectory) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Folder second;
    second.id = 9;
    second.name = "Second";
    snapshot.folders.push_back(second);
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].folderId = 9;
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "second-000009" / "canvas-1-000002" / "canvas.json"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "canvas-1-000002"));
}

// A snapshot that lacks something nobody deleted for good - which no delete
// the app makes produces - takes it out of the library's tree,
// setting it aside rather than deleting it. The folder it was in stays.
TEST_F(LibraryStoreTest, ACanvasTheSnapshotLacksIsSetAsideOutOfTheTree) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    ASSERT_TRUE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "canvas-1-000002"));

    snapshot.canvases.clear();
    snapshot.currentCanvasId = 0;
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_FALSE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "canvas-1-000002"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "folder.json"))
        << "the folder itself outlives its last canvas";
    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "canvas.json")) << "set aside, not deleted";
}

TEST_F(LibraryStoreTest, AFolderTheSnapshotLacksIsSetAsideWithWhatWasInside) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.folders.clear();
    snapshot.canvases.clear();
    snapshot.currentFolderId = 0;
    snapshot.currentCanvasId = 0;
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_FALSE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001"));
    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "drawing-1-000003" / "item.json"));
}

// Saving twice with nothing changed must be a no-op on disk, or every
// autosave would churn directories that nobody touched.
TEST_F(LibraryStoreTest, SavingTwiceLeavesTheSameTree) {
    LibraryStore store(dir_);
    const CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    std::vector<std::string> first;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_)) {
        first.push_back(entry.path().lexically_relative(dir_).generic_string());
    }
    std::sort(first.begin(), first.end());

    ASSERT_TRUE(store.Save(snapshot));
    std::vector<std::string> second;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_)) {
        second.push_back(entry.path().lexically_relative(dir_).generic_string());
    }
    std::sort(second.begin(), second.end());

    EXPECT_EQ(first, second);
}

// ===== Snippets, one directory each =====

// An id in a record reads the same as the directory's trailing uid, so the
// two can be matched by eye.
TEST_F(LibraryStoreTest, IdsAreWrittenTheWayTheDirectoriesSpellThem) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    const std::filesystem::path folderDir = dir_ / "folders" / "folder-1-000001";
    const std::filesystem::path canvasDir = folderDir / "canvas-1-000002";

    EXPECT_EQ(nlohmann::json::parse(std::ifstream(folderDir / "folder.json"))["id"], "000001");
    EXPECT_EQ(nlohmann::json::parse(std::ifstream(canvasDir / "canvas.json"))["id"], "000002");
    EXPECT_EQ(nlohmann::json::parse(std::ifstream(canvasDir / "drawing-1-000003" / "item.json"))["id"], "000003");
    const nlohmann::json library = nlohmann::json::parse(std::ifstream(dir_ / "library.json"));
    EXPECT_EQ(library["currentFolderId"], "000001");
    EXPECT_EQ(library["currentCanvasId"], "000002");
}

TEST_F(LibraryStoreTest, SaveWritesADirectoryPerSnippet) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));

    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    EXPECT_TRUE(std::filesystem::exists(canvasDir / "drawing-1-000003" / "item.json"));
    EXPECT_TRUE(std::filesystem::exists(canvasDir / "shot-1-000004" / "item.json"));
    // The canvas record does not carry its snippets - the directories do.
    std::ifstream in(canvasDir / "canvas.json");
    const nlohmann::json canvasRecord = nlohmann::json::parse(in);
    EXPECT_FALSE(canvasRecord.contains("items"));
}

// The gesture the whole layout is for: one directory carries the snippet's
// record, its picture and its thumbnail, so moving it moves all three.
TEST_F(LibraryStoreTest, ASnippetDirectoryMovedToAnotherCanvasGoesWithItsPicture) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 64, 64).has_value());

    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000007.qoi";
    Canvas other;
    other.id = 8;
    other.name = "Other";
    other.folderId = 1;
    snapshot.canvases.push_back(other);
    ASSERT_TRUE(store.Save(snapshot));
    ASSERT_TRUE(std::filesystem::exists(ShotItemDir() / "000007.qoi"));

    // Drag the snippet's directory across, the way a file manager would.
    const std::filesystem::path folderDir = dir_ / "folders" / "folder-1-000001";
    std::filesystem::rename(ShotItemDir(), folderDir / "other-000008" / "shot-1-000004");

    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 2u);
    const Canvas& source = loaded->canvases[0].id == 2 ? loaded->canvases[0] : loaded->canvases[1];
    const Canvas& target = loaded->canvases[0].id == 8 ? loaded->canvases[0] : loaded->canvases[1];
    EXPECT_EQ(source.items.size(), 1u) << "it left the canvas it was in";
    ASSERT_EQ(target.items.size(), 1u) << "...and arrived in the one it was dropped on";
    EXPECT_EQ(target.items[0].id, 4u);
    // ...and its picture is still readable, from its new home.
    EXPECT_TRUE(reopened.LoadImage(4, "000007.qoi").has_value());
}

TEST_F(LibraryStoreTest, MovingASnippetInTheAppMovesItsDirectory) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Canvas other;
    other.id = 8;
    other.name = "Other";
    other.folderId = 1;
    snapshot.canvases.push_back(other);
    ASSERT_TRUE(store.Save(snapshot));
    ASSERT_TRUE(std::filesystem::exists(ShotItemDir() / "item.json"));

    // What MoveOrCopyItemToCanvas leaves behind: the same item, listed by a
    // different canvas.
    Item moved = snapshot.canvases[0].items[1];
    snapshot.canvases[0].items.pop_back();
    snapshot.canvases[1].items.push_back(moved);
    ASSERT_TRUE(store.Save(snapshot));

    const std::filesystem::path folderDir = dir_ / "folders" / "folder-1-000001";
    EXPECT_TRUE(std::filesystem::exists(folderDir / "other-000008" / "shot-1-000004" / "item.json"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()))
        << "moved, not copied and orphaned";
}

#if defined(_WIN32)
// Windows will not rename a directory while a file inside it is held open
// without delete sharing, which a picture viewer looking at a capture is.
// A move the disk refused is not a save that landed: the tree is what a
// load believes, and it would put the snippet back on the canvas it left.
TEST_F(LibraryStoreTest, AMoveWhoseDirectoryCannotBeRenamedFailsTheSaveUntilItCan) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    Canvas other;
    other.id = 8;
    other.name = "Other";
    other.folderId = 1;
    snapshot.canvases.push_back(other);
    ASSERT_TRUE(store.Save(snapshot));

    Item moved = snapshot.canvases[0].items[1];
    snapshot.canvases[0].items.pop_back();
    snapshot.canvases[1].items.push_back(moved);
    const std::filesystem::path movedTo = dir_ / "folders" / "folder-1-000001" / "other-000008" / "shot-1-000004";
    {
        std::ifstream held(ShotItemDir() / "000004.qoi");  // no delete sharing
        ASSERT_TRUE(held.is_open());
        EXPECT_FALSE(store.Save(snapshot)) << "the snippet is still under the canvas it left";
        EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "item.json")) << "and its record is kept there";
        EXPECT_FALSE(std::filesystem::exists(movedTo));
    }
    EXPECT_TRUE(store.Save(snapshot)) << "retried once the file was let go of";
    EXPECT_TRUE(std::filesystem::exists(movedTo / "item.json"));
    EXPECT_TRUE(std::filesystem::exists(movedTo / "000004.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
}

// The same held file, and then the canvas it is leaving deleted for good:
// the removal waits for the move, however many saves that takes, and never
// marks the directory the moved snippet is still in.
TEST_F(LibraryStoreTest, ARemovalWaitsForAMoveOutOfItThatCannotLandYet) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    Canvas other;
    other.id = 8;
    other.name = "Other";
    other.folderId = 1;
    snapshot.canvases.push_back(other);
    ASSERT_TRUE(store.Save(snapshot));

    Item moved = snapshot.canvases[0].items[1];
    snapshot.canvases[1].items.push_back(moved);
    snapshot.canvases.erase(snapshot.canvases.begin());
    const std::filesystem::path oldCanvasDir = ShotItemDir().parent_path();
    const std::filesystem::path movedTo = dir_ / "folders" / "folder-1-000001" / "other-000008" / "shot-1-000004";
    {
        std::ifstream held(ShotItemDir() / "000004.qoi");  // no delete sharing
        ASSERT_TRUE(held.is_open());
        EXPECT_FALSE(store.Remove(2, snapshot));
        EXPECT_TRUE(store.HasPendingRemoval(2));
        EXPECT_FALSE(store.Save(snapshot)) << "the move did not land";
        EXPECT_TRUE(store.HasPendingRemoval(2));
        EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi"));
        const std::string pending = FileText(dir_ / "pending.json");
        EXPECT_NE(pending.find("\"000002\""), std::string::npos) << "recorded";
        EXPECT_NE(pending.find("\"000004\": \"000008\""), std::string::npos)
            << "with where what was moved out of it belongs:\n" << pending;

        // A restart meanwhile finds the snippet where it was moved to, and
        // not the canvas it was moved out of.
        LibraryStore restarted(dir_);
        const std::optional<CanvasManagerSnapshot> loaded = restarted.Load();
        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->canvases.size(), 1u);
        EXPECT_EQ(loaded->canvases[0].id, 8u);
        ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
        EXPECT_EQ(loaded->canvases[0].items[0].id, 4u);
    }
    EXPECT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(store.HasPendingRemoval(2));
    EXPECT_FALSE(std::filesystem::exists(oldCanvasDir));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
    LibraryStore reopened(dir_);
    ASSERT_TRUE(reopened.Load().has_value());
    const std::optional<DecodedImage> reloaded = reopened.LoadImage(4, "000004.qoi");
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->pixelsRGBA, pixels);
    EXPECT_TRUE(std::filesystem::exists(movedTo / "000004.qoi"));
}

// A picture waiting in staging that could not be moved home when its
// snippet got a directory, and written again since - at home, where every
// write goes from then on. The waiting one is the older, and must not be
// moved over the newer once it can be.
TEST_F(LibraryStoreTest, AStagedPictureNeverReplacesANewerOneAtHome) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> older = Checkerboard(64, 64);
    std::vector<uint8_t> newer = older;
    std::reverse(newer.begin(), newer.end());
    ASSERT_TRUE(store.SaveImage(4, older.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    {
        std::ifstream held(dir_ / "staging" / "000004.qoi");  // no delete sharing
        ASSERT_TRUE(held.is_open());
        ASSERT_TRUE(store.Save(snapshot));
        ASSERT_TRUE(std::filesystem::exists(dir_ / "staging" / "000004.qoi")) << "the move was refused";
        ASSERT_TRUE(store.SaveImage(4, newer.data(), 64, 64).has_value());
        ASSERT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi")) << "written at home";
    }
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "staging" / "000004.qoi"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "retired" / "staging" / "000004.qoi")) << "set aside, not deleted";

    LibraryStore reopened(dir_);
    ASSERT_TRUE(reopened.Load().has_value());
    const std::optional<DecodedImage> image = reopened.LoadImage(4, "000004.qoi");
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->pixelsRGBA, newer);
}
#endif

// A snippet moved to another canvas, and the canvas it left deleted for
// good before the save that would have moved its directory: the directory
// is still inside the one being removed, and removing that now would take
// the snippet's pictures with it.
TEST_F(LibraryStoreTest, RemovingAParentSparesWhatWasMovedOutOfItAndNotYetSaved) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    Canvas other;
    other.id = 8;
    other.name = "Other";
    other.folderId = 1;
    snapshot.canvases.push_back(other);
    ASSERT_TRUE(store.Save(snapshot));

    Item moved = snapshot.canvases[0].items[1];
    snapshot.canvases[1].items.push_back(moved);
    snapshot.canvases.erase(snapshot.canvases.begin());
    EXPECT_FALSE(store.Remove(2, snapshot));
    EXPECT_TRUE(store.HasPendingRemoval(2)) << "owed until the move has landed";
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi"));

    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(store.HasPendingRemoval(2));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir().parent_path())) << "the canvas went";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired")) << "deleted, not set aside";
    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    const std::optional<DecodedImage> image = reopened.LoadImage(4, "000004.qoi");
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->pixelsRGBA, pixels);
}

// The same a level up: a canvas moved to another folder, then the folder.
TEST_F(LibraryStoreTest, RemovingAFolderSparesACanvasMovedOutOfItAndNotYetSaved) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    Folder second;
    second.id = 9;
    second.name = "Second";
    snapshot.folders.push_back(second);
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].folderId = 9;
    snapshot.folders.erase(snapshot.folders.begin());
    EXPECT_FALSE(store.Remove(1, snapshot));
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(store.HasPendingRemoval(1));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001"));
    LibraryStore reopened(dir_);
    ASSERT_TRUE(reopened.Load().has_value());
    const std::optional<DecodedImage> image = reopened.LoadImage(4, "000004.qoi");
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->pixelsRGBA, pixels);
}
#if defined(_WIN32)

// A rename that the disk refused is another matter: the uid is the
// identity, so the record under its old label is still exactly where a
// load looks for it, and the save counts.
TEST_F(LibraryStoreTest, ARenameWhoseDirectoryCannotBeRenamedStillCountsAsSaved) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].name = "Renamed";
    const std::filesystem::path oldCanvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    {
        std::ifstream held(ShotItemDir() / "000004.qoi");  // two levels down, and enough
        ASSERT_TRUE(held.is_open());
        EXPECT_TRUE(store.Save(snapshot)) << "a stale label is not a failed save";
        EXPECT_EQ(nlohmann::json::parse(std::ifstream(oldCanvasDir / "canvas.json"))["name"], "Renamed")
            << "the record is current where the directory is";
    }
    EXPECT_TRUE(store.Save(snapshot));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "renamed-000002" / "canvas.json"))
        << "relabeled once it could be";
}
#endif

TEST_F(LibraryStoreTest, ACopiedSnippetDirectoryBecomesASnippetOfItsOwn) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    std::filesystem::copy(canvasDir / "shot-1-000004", canvasDir / "shot-1-copy-000004",
                           std::filesystem::copy_options::recursive);

    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 3u);
    std::set<uint64_t> ids;
    for (const Item& item : loaded->canvases[0].items) {
        EXPECT_TRUE(ids.insert(item.id).second) << "the copy needs an id of its own";
    }
}

TEST_F(LibraryStoreTest, SnippetOrderIsZOrderAndANewcomerArrivesInFront) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    // Dropped in by hand, so the order file cannot know about it.
    std::filesystem::copy(canvasDir / "drawing-1-000003", canvasDir / "later-00000z",
                           std::filesystem::copy_options::recursive);

    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 3u);
    // Back to front, so last in the list is the one on top.
    EXPECT_EQ(loaded->canvases[0].items[0].id, 3u);
    EXPECT_EQ(loaded->canvases[0].items[1].id, 4u);
    EXPECT_EQ(loaded->canvases[0].items[2].name, "Drawing 1") << "the newcomer is in front";
}

TEST_F(LibraryStoreTest, ASnippetTheSnapshotLacksIsSetAsideWithItsPicture) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000007.qoi";
    ASSERT_TRUE(store.Save(snapshot));
    ASSERT_TRUE(std::filesystem::exists(ShotItemDir() / "000007.qoi"));

    snapshot.canvases[0].items.pop_back();
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "shot-1-000004" / "000007.qoi"))
        << "set aside, picture and all";
}

// ===== Deleted things, and deleting for good =====

// A delete is a stamp in the record and nothing else: the snippet stays
// where it is on disk, picture and all, reads back deleted after a restart,
// and loses the stamp again when it is restored.
TEST_F(LibraryStoreTest, ADeletedSnippetStaysInPlaceStampedAcrossARestart) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].items[1].deletedAt = 1234;
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_EQ(nlohmann::json::parse(std::ifstream(ShotItemDir() / "item.json"))["deletedAt"], 1234);
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired")) << "nothing went missing";

    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u);
    EXPECT_EQ(loaded->canvases[0].items[1].deletedAt, 1234);
    EXPECT_TRUE(reopened.LoadImage(4, "000004.qoi").has_value());

    snapshot.canvases[0].items[1].deletedAt = 0;
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(nlohmann::json::parse(std::ifstream(ShotItemDir() / "item.json")).contains("deletedAt"));
}

// A record larger than Load reads is not written, and the save says so:
// acknowledged, the snippet would be skipped at the next start with all of
// it gone. The last record that fitted stays, and loads.
TEST_F(LibraryStoreTest, ARecordTooLargeToReadBackIsNotWrittenAndFailsTheSave) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_TRUE(store.OversizedRecords().empty());

    snapshot.canvases[0].items[0].noteText.assign(std::size_t{65} << 20, 'a');
    snapshot.canvases[0].items[1].name = "Renamed alongside";
    EXPECT_FALSE(store.Save(snapshot));
    EXPECT_EQ(store.OversizedRecords(), std::set<uint64_t>{3});

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u) << "the snippet is still there to load";
    EXPECT_EQ(loaded->canvases[0].items[0].noteText, MakeSampleSnapshot().canvases[0].items[0].noteText);
    EXPECT_EQ(loaded->canvases[0].items[1].name, "Renamed alongside") << "the rest of the save landed";

    snapshot.canvases[0].items[0].noteText = "Short again";
    EXPECT_TRUE(store.Save(snapshot));
    EXPECT_TRUE(store.OversizedRecords().empty());
}

// Deleting for good is Remove, at once - and the save after it agrees about
// what went, setting nothing aside.
TEST_F(LibraryStoreTest, RemoveDeletesASnippetForGoodAndTheNextSaveAgrees) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].items.pop_back();
    ASSERT_TRUE(store.Remove(4, snapshot));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
    EXPECT_FALSE(store.LoadImage(4, "000004.qoi").has_value());
    EXPECT_FALSE(store.Remove(4, snapshot)) << "nothing by that id left to remove";

    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items.size(), 1u);
}

#if defined(_WIN32)
TEST_F(LibraryStoreTest, ARemoveThatCouldNotFinishIsRetriedByTheNextSaveNotSetAside) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));
    snapshot.canvases[0].items.pop_back();

    {
        std::ifstream held(ShotItemDir() / "000004.qoi");  // no delete sharing
        ASSERT_TRUE(held.is_open());
        EXPECT_FALSE(store.Remove(4, snapshot)) << "not gone, so not done";
        EXPECT_TRUE(store.HasPendingRemoval(4));
        EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi"));
        EXPECT_NE(FileText(dir_ / "pending.json").find("000004"), std::string::npos) << "the intent is on disk";

        // A save meanwhile neither sets the remains aside nor forgets them.
        ASSERT_TRUE(store.Save(snapshot));
        EXPECT_TRUE(store.HasPendingRemoval(4));
        EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
        EXPECT_TRUE(std::filesystem::exists(ShotItemDir()));
    }
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(store.HasPendingRemoval(4));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir())) << "finished once the file was let go of";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
}

// The owed directory's parent may be renamed before the retry - the folder
// given a new name, the canvas too - and the removal has to follow it
// rather than look at the old spelling, find nothing, and call it done.
TEST_F(LibraryStoreTest, ARemovalStillOwedFollowsItsParentsRename) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));
    snapshot.canvases[0].items.pop_back();
    {
        std::ifstream held(ShotItemDir() / "000004.qoi");
        ASSERT_TRUE(held.is_open());
        EXPECT_FALSE(store.Remove(4, snapshot));
        ASSERT_TRUE(store.HasPendingRemoval(4));
    }
    // Let go - and the folder and the canvas renamed before the next save.
    snapshot.folders[0].name = "Renamed Folder";
    snapshot.canvases[0].name = "Renamed Canvas";
    ASSERT_TRUE(store.Save(snapshot));
    const std::filesystem::path movedTo =
        dir_ / "folders" / "renamed-folder-000001" / "renamed-canvas-000002" / "shot-1-000004";
    EXPECT_FALSE(std::filesystem::exists(movedTo)) << "left under the new name as a remainder";
    EXPECT_FALSE(store.HasPendingRemoval(4)) << "owed until it is gone, then not";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_)) {
        EXPECT_NE(entry.path().filename(), ".removed") << entry.path();
    }
    EXPECT_FALSE(std::filesystem::exists(dir_ / "pending.json")) << "nothing owed, nothing recorded";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
}

// The app may well be restarted while the file is still held - and the
// snippet the user deleted for good must not be back when it is.
TEST_F(LibraryStoreTest, ARemovalStillOwedSurvivesARestart) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].items.pop_back();
    std::ifstream held(ShotItemDir() / "000004.qoi");  // no delete sharing
    ASSERT_TRUE(held.is_open());
    EXPECT_FALSE(store.Remove(4, snapshot));
    ASSERT_NE(FileText(dir_ / "pending.json").find("000004"), std::string::npos);

    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items.size(), 1u) << "deleted for good is not brought back by a restart";
    EXPECT_TRUE(reopened.HasPendingRemoval(4)) << "and still owed";
    ASSERT_TRUE(reopened.Save(*loaded));
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi")) << "still held";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired")) << "owed, not lost";

    held.close();
    ASSERT_TRUE(reopened.Save(*loaded));
    EXPECT_FALSE(reopened.HasPendingRemoval(4));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
}
#endif

// A library an older build left a removal owed in: the directory carries
// the `.removed` mark instead of being named in pending.json. It is still
// not read, and the next save finishes it, mark and all.
TEST_F(LibraryStoreTest, AnOlderBuildsRemovedMarkIsStillAPendingRemoval) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    std::ofstream(ShotItemDir() / ".removed") << "Deleted for good.";

    LibraryStore store(dir_);
    std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items.size(), 1u) << "the marked snippet is not read";
    EXPECT_TRUE(store.HasPendingRemoval(4));

    ASSERT_TRUE(store.Save(*loaded));
    EXPECT_FALSE(store.HasPendingRemoval(4));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "pending.json"));
}

TEST_F(LibraryStoreTest, RemoveOfAFolderTakesEverythingInIt) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));

    ASSERT_TRUE(store.Remove(1, CanvasManagerSnapshot{}));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001"));
    ASSERT_TRUE(store.Save(CanvasManagerSnapshot{}));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired")) << "its canvas and snippets went with it, not astray";
}

// What someone else put beside a record is not the store's to delete: a
// permanent delete takes the records and pictures and leaves the rest, and
// the directory with it - which, holding no record, nothing reads back and
// nothing sets aside.
TEST_F(LibraryStoreTest, RemoveLeavesWhatIsNotTheLibrarysAndStillCountsAsDone) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));
    std::ofstream(ShotItemDir() / "notes.txt") << "mine";
    std::ofstream(ShotItemDir() / "item.json.tmp2") << "{";  // a temporary of ours a crash left
    std::filesystem::create_directories(ShotItemDir() / "scans");
    std::ofstream(ShotItemDir() / "scans" / "page.txt") << "mine too";

    snapshot.canvases[0].items.pop_back();
    EXPECT_TRUE(store.Remove(4, snapshot)) << "everything of the library's is gone";
    EXPECT_FALSE(store.HasPendingRemoval(4));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "item.json"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "item.json.tmp2"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "000004.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "000004.thumb.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / ".removed"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "pending.json"));
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "notes.txt"));
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "scans" / "page.txt"));

    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired")) << "not the store's, so not set aside either";
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "notes.txt"));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items.size(), 1u) << "a directory with no record is not a snippet";
}

// The same a level up: a folder deleted for good takes its canvases and
// snippets, and leaves a directory of someone else's inside it standing.
TEST_F(LibraryStoreTest, RemoveOfAFolderLeavesADirectoryThatIsNotTheLibrarys) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    const std::filesystem::path folderDir = dir_ / "folders" / "folder-1-000001";
    std::filesystem::create_directories(folderDir / "reference");
    std::ofstream(folderDir / "reference" / "map.txt") << "mine";

    ASSERT_TRUE(store.Remove(1, CanvasManagerSnapshot{}));
    EXPECT_FALSE(std::filesystem::exists(folderDir / "folder.json"));
    EXPECT_FALSE(std::filesystem::exists(folderDir / "order.json"));
    EXPECT_FALSE(std::filesystem::exists(folderDir / "canvas-1-000002")) << "empty once its own went, so gone";
    EXPECT_TRUE(std::filesystem::exists(folderDir / "reference" / "map.txt"));

    ASSERT_TRUE(store.Save(CanvasManagerSnapshot{}));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->folders.empty()) << "a directory with no record is not a folder";
}

// The net: whatever a snapshot lacks without a Remove is set aside whole,
// merging into what is set aside already, and never read back.
TEST_F(LibraryStoreTest, WhatASnapshotLacksIsSetAsideWholeAndNotReadBack) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));

    CanvasManagerSnapshot withoutTheShot = snapshot;
    withoutTheShot.canvases[0].items.pop_back();
    ASSERT_TRUE(store.Save(withoutTheShot));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "shot-1-000004" / "000004.qoi"));

    // Then the canvas it was on, into the place the snippet already took.
    CanvasManagerSnapshot withoutTheCanvas = snapshot;
    withoutTheCanvas.canvases.clear();
    withoutTheCanvas.currentCanvasId = 0;
    ASSERT_TRUE(store.Save(withoutTheCanvas));
    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "canvas.json"));
    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "drawing-1-000003" / "item.json"));
    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "shot-1-000004" / "000004.qoi"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "folder.json"));

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->canvases.empty()) << "nothing is read back out of retired/";
}

// ===== Moves are moves, whichever way round the canvases are visited =====
//
// A snippet moved from one canvas to another is, on disk, one canvas losing
// a directory and another gaining it. A save that collected strays canvas by
// canvas as it went would delete the snippet's directory - picture included
// - before reaching the canvas it moved to. Real pixels here, because the
// record can be rebuilt from memory and the picture cannot; only the picture
// tells the two apart.
TEST_F(LibraryStoreTest, MovingASnippetToALaterCanvasKeepsItsPicture) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    Canvas other;
    other.id = 8;
    other.name = "Other";
    other.folderId = 1;
    snapshot.canvases.push_back(other);
    ASSERT_TRUE(store.Save(snapshot));
    ASSERT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi"));

    Item moved = snapshot.canvases[0].items[1];
    snapshot.canvases[0].items.pop_back();
    snapshot.canvases[1].items.push_back(moved);
    ASSERT_TRUE(store.Save(snapshot));

    const std::filesystem::path newHome = dir_ / "folders" / "folder-1-000001" / "other-000008" / "shot-1-000004";
    EXPECT_TRUE(std::filesystem::exists(newHome / "000004.qoi")) << "the picture went with the record";
    EXPECT_TRUE(std::filesystem::exists(newHome / "000004.thumb.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
    // Readable through the store that did the move, and through a fresh one.
    EXPECT_TRUE(store.LoadImage(4, "000004.qoi").has_value());
    LibraryStore reopened(dir_);
    ASSERT_TRUE(reopened.Load().has_value());
    const std::optional<DecodedImage> decoded = reopened.LoadImage(4, "000004.qoi");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

TEST_F(LibraryStoreTest, MovingASnippetToAnEarlierCanvasKeepsItsPictureToo) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Canvas other;
    other.id = 8;
    other.name = "Other";
    other.folderId = 1;
    Item shot;
    shot.id = 9;
    shot.name = "Late shot";
    shot.rect = Rect{0, 0, 64, 64};
    ASSERT_TRUE(store.SaveImage(9, pixels.data(), 64, 64).has_value());
    shot.ImageLayer()->imageFile = "000009.qoi";
    other.items.push_back(shot);
    snapshot.canvases.push_back(other);
    ASSERT_TRUE(store.Save(snapshot));

    // From the canvas saved second to the one saved first.
    snapshot.canvases[0].items.push_back(snapshot.canvases[1].items[0]);
    snapshot.canvases[1].items.clear();
    ASSERT_TRUE(store.Save(snapshot));

    const std::filesystem::path newHome = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "late-shot-000009";
    EXPECT_TRUE(std::filesystem::exists(newHome / "000009.qoi"));
    EXPECT_TRUE(store.LoadImage(9, "000009.qoi").has_value());
    LibraryStore reopened(dir_);
    ASSERT_TRUE(reopened.Load().has_value());
    EXPECT_TRUE(reopened.LoadImage(9, "000009.qoi").has_value());
}

// ===== A picture is found through its snippet, wherever that is now =====
//
// A library-wide map from filename to directory falls behind on a rename:
// the picture is right there in the renamed directory, and the map goes on
// pointing at the old one. Lookup goes through the owning snippet instead.
TEST_F(LibraryStoreTest, RenamingASnippetKeepsItsPictureReadableThroughTheSameStore) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));
    ASSERT_TRUE(store.LoadImage(4, "000004.qoi").has_value());

    snapshot.canvases[0].items[1].name = "Renamed";
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_TRUE(store.LoadImage(4, "000004.qoi").has_value());
    EXPECT_TRUE(store.LoadThumbnail(4, "000004.qoi").has_value());
    // ...and a picture written *after* the rename lands in the renamed
    // directory, not in a ghost of the old one.
    ASSERT_TRUE(store.SaveLayerImage(4, 1, pixels.data(), 64, 64).has_value());
    const std::filesystem::path renamed = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "renamed-000004";
    EXPECT_TRUE(std::filesystem::exists(renamed / "000004_p1.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
}

// The same one level up: renaming a canvas or a folder moves everything
// inside it, and everything inside it has to know.
TEST_F(LibraryStoreTest, RenamingACanvasOrFolderKeepsTheSnippetsInsideReadable) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].name = "Renamed canvas";
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_TRUE(store.LoadImage(4, "000004.qoi").has_value()) << "after the canvas moved";

    snapshot.folders[0].name = "Renamed folder";
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_TRUE(store.LoadImage(4, "000004.qoi").has_value()) << "after the folder moved";

    // And the snippet itself, afterwards - which is only right if the
    // index followed both moves rather than the snippet being rebuilt.
    snapshot.canvases[0].items[1].name = "Renamed shot";
    ASSERT_TRUE(store.Save(snapshot));
    const std::filesystem::path home = dir_ / "folders" / "renamed-folder-000001" / "renamed-canvas-000002" /
                                       "renamed-shot-000004";
    EXPECT_TRUE(std::filesystem::exists(home / "000004.qoi"));
    EXPECT_TRUE(store.LoadImage(4, "000004.qoi").has_value());
    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u) << "nothing was left behind to be found twice";
}

// ===== What a save deletes is what it once read, and nothing else =====

TEST_F(LibraryStoreTest, ADirectoryWithNoRecordInItSurvivesASave) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "a-000003", 3, "A");
    std::filesystem::create_directories(work / "personal-notes");
    std::ofstream(work / "personal-notes" / "keep.txt") << "unrelated";
    std::filesystem::create_directories(dir_ / "folders" / "stray-dir");
    std::ofstream(dir_ / "folders" / "stray-dir" / "keep.txt") << "unrelated";
    std::filesystem::create_directories(work / "a-000003" / "not-a-snippet");
    std::ofstream(work / "a-000003" / "not-a-snippet" / "keep.txt") << "unrelated";
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(store.Save(*loaded));
    ASSERT_TRUE(store.Save(*loaded));  // and not on a second look either

    EXPECT_TRUE(std::filesystem::exists(work / "personal-notes" / "keep.txt"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "stray-dir" / "keep.txt"));
    EXPECT_TRUE(std::filesystem::exists(work / "a-000003" / "not-a-snippet" / "keep.txt"));
}

// Unreadable must not become deleted. A record that failed to parse is
// skipped by Load, and skipped is all it may be: the directory holds
// somebody's work, and the most likely reader to fix the record is the next
// version of this app.
TEST_F(LibraryStoreTest, ADirectoryWhoseRecordCannotBeReadSurvivesASave) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    const std::filesystem::path canvas = PlaceCanvas(work, "a-000003", 3, "A");
    std::filesystem::create_directories(canvas / "broken-000005");
    std::ofstream(canvas / "broken-000005" / "item.json") << "{ this is not json";
    std::ofstream(canvas / "broken-000005" / "000005.qoi") << "and this is the picture that matters";
    std::filesystem::create_directories(work / "broken-000006");
    std::ofstream(work / "broken-000006" / "canvas.json") << "[1, 2, 3]";  // valid JSON, not a record
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_TRUE(loaded->canvases[0].items.empty()) << "the broken record is not read as a snippet";
    ASSERT_TRUE(store.Save(*loaded));

    EXPECT_TRUE(std::filesystem::exists(canvas / "broken-000005" / "000005.qoi"));
    EXPECT_TRUE(std::filesystem::exists(canvas / "broken-000005" / "item.json"));
    EXPECT_TRUE(std::filesystem::exists(work / "broken-000006" / "canvas.json"));
}

// ===== Nothing in the files can throw out of Load =====
//
// Parsing without exceptions protects the parse and nothing after it: a
// field that is present with the wrong type throws from nlohmann's value(),
// and there was nothing between Load and WinMain to catch it.

TEST_F(LibraryStoreTest, AFieldOfTheWrongTypeInLibraryJsonReadsAsItsDefault) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    std::ofstream(dir_ / "library.json") << R"({"currentFolderId": "000001", "currentCanvasId": 12,
                                                 "notAKnownKey": "ignored"})";

    LibraryStore store(dir_);
    std::optional<CanvasManagerSnapshot> loaded;
    ASSERT_NO_THROW(loaded = store.Load());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->currentCanvasId, 2u) << "defaulted, then repaired to a canvas that exists";
    EXPECT_EQ(loaded->canvases.size(), 1u) << "the tree is untouched by a bad pointer file";
}

TEST_F(LibraryStoreTest, ARecordWithAFieldOfTheWrongTypeIsSkippedAndLeftWhereItIs) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    const std::filesystem::path drawingDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "drawing-1-000003";
    std::ofstream(drawingDir / "item.json") << R"({"id": "000003", "name": 12345, "rect": "not a rect"})";

    LibraryStore store(dir_);
    std::optional<CanvasManagerSnapshot> loaded;
    ASSERT_NO_THROW(loaded = store.Load());
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u) << "the unreadable record is skipped";
    EXPECT_EQ(loaded->canvases[0].items[0].id, 4u);
    ASSERT_TRUE(store.Save(*loaded));
    EXPECT_TRUE(std::filesystem::exists(drawingDir / "item.json")) << "...and left for a reader that can";
}

// ===== The tree is the library, whatever the pointer file says =====
//
// library.json holds the format version and two pointers. Anything wrong
// with it is a reason to default those, never a reason to report the library absent
// while a tree is there - because "absent" starts the app fresh, and a
// fresh library's first save retires everything it finds.

TEST_F(LibraryStoreTest, AGarbagePointerFileDoesNotHideTheTree) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    std::ofstream(dir_ / "library.json") << "{ this is not valid json";

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value()) << "a tree with a broken pointer file is still a library";
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u);
    EXPECT_EQ(loaded->currentCanvasId, 2u) << "repaired from the tree";
    ASSERT_TRUE(store.Save(*loaded));
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "item.json"));
}

TEST_F(LibraryStoreTest, AMissingPointerFileDoesNotHideTheTree) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    std::filesystem::remove(dir_ / "library.json");

    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases.size(), 1u);
}

// The second line of defence, for whatever else might start a fresh
// library over an existing tree: a store that never read a directory has
// no standing to retire it. What it finds on disk is left exactly as found,
// and the next load reads both.
TEST_F(LibraryStoreTest, AStoreThatNeverLoadedRetiresNothingItFoundOnDisk) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));

    CanvasManagerSnapshot fresh;
    Folder folder;
    folder.id = 50;
    folder.name = "Fresh";
    fresh.folders.push_back(folder);
    Canvas canvas;
    canvas.id = 51;
    canvas.name = "Fresh canvas";
    canvas.folderId = 50;
    fresh.canvases.push_back(canvas);
    fresh.currentFolderId = 50;
    fresh.currentCanvasId = 51;
    const LibraryStore neverLoaded(dir_);
    ASSERT_TRUE(neverLoaded.Save(fresh));
    ASSERT_TRUE(neverLoaded.Save(fresh));  // and not on the second look either

    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "item.json")) << "the existing tree was retired";
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "fresh-00001e" / "fresh-canvas-00001f" / "canvas.json"));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->folders.size(), 2u) << "both libraries are there to be read";
    EXPECT_EQ(loaded->canvases.size(), 2u);
}

// ===== Nothing the store deletes or writes is outside the library =====
//
// Every path it mutates is built under its root from sanitized parts, with
// one exception: Layer::imageFile reaches the filesystem from the record
// verbatim, and records can be hand-edited.

TEST_F(LibraryStoreTest, AnImageFileThatNamesAPathIsDroppedOnLoad) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    const std::filesystem::path shotRecord = ShotItemDir() / "item.json";
    nlohmann::json record = nlohmann::json::parse(std::ifstream(shotRecord));
    record["layers"][0]["imageFile"] = "..\\..\\..\\outside.qoi";
    std::ofstream(shotRecord) << record.dump(2);

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->canvases[0].items[1].ImageLayer()->imageFile.empty())
        << "a name that is a path is no name at all";

    // ...and the same rule at the API, for a name that never went through a
    // record: nothing is read from, or written to, where it points.
    const std::vector<uint8_t> pixels = Checkerboard(16, 16);
    DecodedImage image;
    image.width = 16;
    image.height = 16;
    image.pixelsRGBA = pixels;
    EXPECT_FALSE(store.SaveThumbnail(4, "../../outside.qoi", image));
    EXPECT_FALSE(store.LoadImage(4, "../../outside.qoi").has_value());
    EXPECT_FALSE(std::filesystem::exists(dir_.parent_path() / "outside.thumb.qoi"));
}

#if defined(_WIN32)
// A directory renamed by hand to words outside the ANSI code page, and a
// file dropped in beside a record with one in its name, are read and
// written past like any other: a name the code page cannot spell used to
// throw out of every std::filesystem::path::string() that met it.
TEST_F(LibraryStoreTest, NamesOutsideTheCodePageAreReadAndSavedPast) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    std::ofstream(ShotItemDir() / L"\U0001F4F7 scan.png") << "not ours";
    const std::filesystem::path folderDir = dir_ / "folders" / "folder-1-000001";
    std::filesystem::rename(folderDir / "canvas-1-000002", folderDir / L"メモ-000002");

    LibraryStore store(dir_);
    std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u);

    loaded->canvases[0].items[1].rect.x += 5;  // rewrites the shot's record, and sweeps its directory
    EXPECT_TRUE(store.Save(*loaded));
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / L"\U0001F4F7 scan.png")) << "and leaves what is not ours";
}

// ===== Links are not part of the tree =====
//
// A junction is what "mklink /J" makes, needs no privilege, and looks like a
// directory to everything that does not ask. What is behind one is somewhere
// else on the disk and is never the library's: not read, not written, not
// swept, not retired, not deleted.

// A directory outside the library, with a snippet's worth of files in it,
// and a junction at `link` pointing at it. The caller removes `outside`.
std::filesystem::path MakeJunctionTo(const std::filesystem::path& link, const std::filesystem::path& outside) {
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    std::ofstream(outside / "item.json") << R"({"id": "000025", "name": "Linked", "layers": []})";
    std::ofstream(outside / "precious.txt") << "not the library's to delete";
    std::filesystem::create_directories(link.parent_path());
    const std::string command = "mklink /J \"" + link.string() + "\" \"" + outside.string() + "\" >nul";
    EXPECT_EQ(std::system(command.c_str()), 0) << "could not create the junction";
    EXPECT_TRUE(std::filesystem::exists(link / "precious.txt"));
    return outside;
}

TEST_F(LibraryStoreTest, AJunctionInTheTreeIsNeitherReadNorWrittenNorRetired) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    const std::filesystem::path link = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "linked-000025";
    const std::filesystem::path outside =
        MakeJunctionTo(link, dir_.parent_path() / (dir_.filename().string() + "_outside"));

    // Not read: the record behind the link is not a snippet of this canvas.
    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u) << "what is behind a link is not the library's";

    // Not retired, though the snapshot lacks it - and not swept either,
    // though the snapshot has changed.
    CanvasManagerSnapshot snapshot = *loaded;
    snapshot.canvases[0].items[1].name = "Edited";
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_TRUE(std::filesystem::exists(link)) << "left exactly where it was";
    EXPECT_TRUE(std::filesystem::exists(outside / "precious.txt"));
    EXPECT_TRUE(std::filesystem::exists(outside / "item.json"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired" / "folder-1-000001" / "canvas-1-000002" / "linked-000025"));
    std::filesystem::remove_all(outside);
}

TEST_F(LibraryStoreTest, ASaveDoesNotWriteThroughAJunctionSittingWhereItsDirectoryWouldBe) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    // A junction with exactly the name the snippet's directory would get.
    const std::filesystem::path link = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "linked-000025";
    const std::filesystem::path outside =
        MakeJunctionTo(link, dir_.parent_path() / (dir_.filename().string() + "_outside"));

    Item linked;
    linked.id = 77;  // "000025" in base36: the directory the junction is standing in for
    linked.name = "Linked";
    linked.noteText = "would land outside";
    snapshot.canvases[0].items.push_back(linked);
    EXPECT_FALSE(store.Save(snapshot)) << "the record has nowhere of the library's to go";

    nlohmann::json record = nlohmann::json::parse(std::ifstream(outside / "item.json"));
    EXPECT_EQ(record["id"], "000025") << "the record behind the link is untouched";
    EXPECT_FALSE(record.contains("noteText")) << "nothing was written through the link";
    EXPECT_TRUE(std::filesystem::exists(outside / "precious.txt"));

    // The rest of the library is saved as usual, and a restart sees it.
    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u);
    std::filesystem::remove_all(outside);
}

TEST_F(LibraryStoreTest, RetiringOntoAJunctionReplacesTheLinkNotWhatItPointsTo) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    // The canvas's place in retired/ is already taken, and inside it a
    // junction sits where the snippet's directory would merge to.
    const std::filesystem::path link = RetiredCanvasDir() / "shot-1-000004";
    const std::filesystem::path outside =
        MakeJunctionTo(link, dir_.parent_path() / (dir_.filename().string() + "_outside"));

    snapshot.canvases.clear();
    snapshot.currentCanvasId = 0;
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_TRUE(std::filesystem::exists(RetiredCanvasDir() / "shot-1-000004" / "item.json")) << "set aside";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "canvas-1-000002"));
    nlohmann::json record = nlohmann::json::parse(std::ifstream(outside / "item.json"));
    EXPECT_EQ(record["id"], "000025") << "nothing was moved into where the link pointed";
    EXPECT_TRUE(std::filesystem::exists(outside / "precious.txt"));
    std::filesystem::remove_all(outside);
}

// The top-level directories are directories like any other, and a junction
// at one of them is the cheapest way to point a whole tree's worth of
// writes somewhere else.
TEST_F(LibraryStoreTest, NothingIsWrittenOrReadThroughAJunctionAtFolders) {
    const std::filesystem::path outside =
        MakeJunctionTo(dir_ / "folders", dir_.parent_path() / (dir_.filename().string() + "_outside"));
    LibraryStore store(dir_);
    EXPECT_FALSE(store.Save(MakeSampleSnapshot())) << "nowhere of the library's to write the tree";
    EXPECT_FALSE(std::filesystem::exists(outside / "order.json"));
    EXPECT_FALSE(std::filesystem::exists(outside / "folder-1-000001"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "library.json")) << "no pointer file over a tree that was not written";

    // Not read either: a folder behind the link is not the library's.
    std::filesystem::create_directories(outside / "folder-1-000001");
    std::ofstream(outside / "folder-1-000001" / "folder.json") << R"({"id": "000001", "name": "Behind the link"})";
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->folders.empty());
    std::filesystem::remove_all(outside);
}

TEST_F(LibraryStoreTest, APictureIsNotWrittenThroughAJunctionAtStaging) {
    const std::filesystem::path outside =
        MakeJunctionTo(dir_ / "staging", dir_.parent_path() / (dir_.filename().string() + "_outside"));
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(8, 8);
    EXPECT_FALSE(store.SaveImage(4, pixels.data(), 8, 8).has_value()) << "refused, so the session keeps the pixels";
    EXPECT_FALSE(std::filesystem::exists(outside / "000004.qoi"));
    EXPECT_FALSE(std::filesystem::exists(outside / "000004.thumb.qoi"));
    std::filesystem::remove_all(outside);
}

TEST_F(LibraryStoreTest, NothingIsSetAsideThroughAJunctionAtRetired) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    const std::vector<uint8_t> pixels = Checkerboard(8, 8);
    ASSERT_TRUE(store.SaveImage(99, pixels.data(), 8, 8).has_value());  // nothing names it: an orphan in staging
    const std::filesystem::path outside =
        MakeJunctionTo(dir_ / "retired", dir_.parent_path() / (dir_.filename().string() + "_outside"));

    snapshot.canvases.clear();
    snapshot.currentCanvasId = 0;
    ASSERT_TRUE(store.Save(snapshot)) << "setting aside is best-effort; the records landed";
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "canvas.json"))
        << "left where it is rather than moved out of the library";
    EXPECT_TRUE(std::filesystem::exists(dir_ / "staging" / "00002r.qoi")) << "the orphan stays in staging, readably";
    EXPECT_FALSE(std::filesystem::exists(outside / "folder-1-000001"));
    EXPECT_FALSE(std::filesystem::exists(outside / "staging"));
    std::filesystem::remove_all(outside);
}

// A directory that was real when the store indexed it and is a junction by
// the time there is something to write into it. Not a race: the tree was
// rearranged between two saves, and the second must notice at the write.
TEST_F(LibraryStoreTest, AJunctionPutInPlaceOfAnIndexedDirectoryIsNotWrittenThrough) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    std::filesystem::remove_all(ShotItemDir());
    const std::filesystem::path outside =
        MakeJunctionTo(ShotItemDir(), dir_.parent_path() / (dir_.filename().string() + "_outside"));

    snapshot.canvases[0].items[1].noteText = "would land outside";
    EXPECT_FALSE(store.Save(snapshot)) << "the record has nowhere of the library's to go";
    nlohmann::json record = nlohmann::json::parse(std::ifstream(outside / "item.json"));
    EXPECT_EQ(record["id"], "000025") << "the record behind the link is untouched";
    EXPECT_FALSE(record.contains("noteText"));
    EXPECT_TRUE(std::filesystem::exists(outside / "precious.txt"));

    // And a picture for that snippet is not written there either.
    const std::vector<uint8_t> pixels = Checkerboard(8, 8);
    EXPECT_FALSE(store.SaveImage(4, pixels.data(), 8, 8).has_value());
    EXPECT_FALSE(std::filesystem::exists(outside / "000004.qoi"));
    std::filesystem::remove_all(outside);
}
#endif

// ===== A save that could not write everything says so =====

TEST_F(LibraryStoreTest, AnOrderFileThatCouldNotBeWrittenFailsTheSave) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";

    ObstructEveryTemporaryName(canvasDir / "order.json");
    std::swap(snapshot.canvases[0].items[0], snapshot.canvases[0].items[1]);
    EXPECT_FALSE(store.Save(snapshot)) << "the z-order on disk is not the z-order in memory";

    ClearTemporaryObstructions(canvasDir / "order.json");
    ASSERT_TRUE(store.Save(snapshot)) << "retried, not remembered as written";
    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items[0].id, 4u);
    EXPECT_EQ(loaded->canvases[0].items[1].id, 3u);
}

TEST_F(LibraryStoreTest, SaveImageThenLoadImageRoundTrips) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = {10, 20, 30, 255, 40, 50, 60, 255};  // 2x1 RGBA

    const std::optional<std::string> filename = store.SaveImage(/*itemId=*/7, pixels.data(), 2, 1);
    ASSERT_TRUE(filename.has_value());
    EXPECT_EQ(*filename, "000007.qoi");

    const std::optional<DecodedImage> decoded = store.LoadImage(7, *filename);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 2);
    EXPECT_EQ(decoded->height, 1);
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

TEST_F(LibraryStoreTest, LoadImageReturnsNulloptForEmptyFilename) {
    LibraryStore store(dir_);
    EXPECT_FALSE(store.LoadImage(7, "").has_value());
}

TEST_F(LibraryStoreTest, SaveSetsAsideAStagedPictureNothingNames) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = {1, 2, 3, 255};
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 1, 1).has_value());   // will stay referenced
    ASSERT_TRUE(store.SaveImage(8, pixels.data(), 1, 1).has_value());   // will become orphaned

    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000007.qoi";  // only item 4 references image 7

    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000007.qoi"));
    // Nothing names image 8, so it leaves staging - but for retired/, not
    // for nowhere: it may be the capture a crash left without a record.
    EXPECT_FALSE(std::filesystem::exists(dir_ / "staging" / "000008.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "000008.qoi"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "retired" / "staging" / "000008.qoi"));
}

// ===== Sidecar thumbnails =====

// A picture big enough to actually be shrunk, and structured rather than
// flat so an averaged downscale can be told from a blank one.

TEST_F(LibraryStoreTest, SaveImageWritesAThumbnailBesideIt) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(800, 400);
    const std::optional<std::string> filename = store.SaveImage(/*itemId=*/7, pixels.data(), 800, 400);
    ASSERT_TRUE(filename.has_value());

    EXPECT_EQ(LibraryStore::ThumbnailFilename(*filename), "000007.thumb.qoi");
    EXPECT_TRUE(std::filesystem::exists(dir_ / "staging" / "000007.thumb.qoi"));

    const std::optional<DecodedImage> thumb = store.LoadThumbnail(7, *filename);
    ASSERT_TRUE(thumb.has_value());
    EXPECT_EQ(std::max(thumb->width, thumb->height), LibraryStore::kThumbnailMaxExtent);
    // Aspect kept, so a tile drawn from this is the same shape as one drawn
    // from the full image.
    EXPECT_EQ(thumb->height, thumb->width / 2);
    // And it is smaller on disk than the image it stands in for, which is
    // the entire reason it exists.
    EXPECT_LT(std::filesystem::file_size(dir_ / "staging" / "000007.thumb.qoi"),
               std::filesystem::file_size(dir_ / "staging" / "000007.qoi"));
}

TEST_F(LibraryStoreTest, AnImageThatIsAlreadySmallKeepsItsSize) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(32, 16);
    const std::optional<std::string> filename = store.SaveImage(/*itemId=*/7, pixels.data(), 32, 16);
    ASSERT_TRUE(filename.has_value());

    const std::optional<DecodedImage> thumb = store.LoadThumbnail(7, *filename);
    ASSERT_TRUE(thumb.has_value());
    EXPECT_EQ(thumb->width, 32);
    EXPECT_EQ(thumb->height, 16);
    EXPECT_EQ(thumb->pixelsRGBA, pixels);
}

TEST_F(LibraryStoreTest, LoadThumbnailReturnsNulloptForAPictureWithoutOne) {
    LibraryStore store(dir_);
    // The image is there, the sidecar isn't, and that is not an error.
    const std::vector<uint8_t> pixels = {1, 2, 3, 255};
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 1, 1).has_value());
    std::filesystem::remove(dir_ / "staging" / "000007.thumb.qoi");

    EXPECT_FALSE(store.LoadThumbnail(7, "000007.qoi").has_value());
    EXPECT_FALSE(store.LoadThumbnail(7, "").has_value());
    EXPECT_TRUE(store.LoadImage(7, "000007.qoi").has_value());
}

TEST_F(LibraryStoreTest, ThumbnailsAreNamedAfterTheirImage) {
    EXPECT_EQ(LibraryStore::ThumbnailFilename("000007.qoi"), "000007.thumb.qoi");
    EXPECT_EQ(LibraryStore::ThumbnailFilename("4_p1.qoi"), "4_p1.thumb.qoi");
}

TEST_F(LibraryStoreTest, SaveKeepsALiveImagesThumbnailAndCollectsADeadOnes) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 64, 64).has_value());  // will stay referenced
    ASSERT_TRUE(store.SaveImage(8, pixels.data(), 64, 64).has_value());  // will become orphaned

    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000007.qoi";

    ASSERT_TRUE(store.Save(snapshot));

    // A thumbnail is named by no layer, so the GC has to know it belongs to
    // its image - or every save would delete the whole set and the next
    // Overview visit would rebuild it.
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000007.thumb.qoi"))
        << "a thumbnail travels with the image it belongs to";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "staging" / "000008.qoi"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "staging" / "000008.thumb.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "000008.qoi"));
}

TEST_F(LibraryStoreTest, ARecordThatCouldNotBeWrittenKeepsThePicturesTheOldOneNames) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = {1, 2, 3, 255};
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 1, 1).has_value());
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));
    ASSERT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi"));

    // The layer comes to name a different picture, and the record saying
    // so cannot be written.
    ASSERT_TRUE(store.SaveLayerImage(4, 0, pixels.data(), 1, 1).has_value());
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004_p0.qoi";
    ObstructEveryTemporaryName(ShotItemDir() / "item.json");
    EXPECT_FALSE(store.Save(snapshot));

    // The record on disk is still the old one, and what it names must still
    // be there for it: a restart reloads exactly that.
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000004.qoi"))
        << "collected on the strength of a record that never landed";
    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items[1].ImageLayer()->imageFile, "000004.qoi");
    EXPECT_TRUE(reopened.LoadImage(4, "000004.qoi").has_value());

    ClearTemporaryObstructions(ShotItemDir() / "item.json");
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "000004.qoi"))
        << "collected once the record naming its replacement is on disk";
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000004_p0.qoi"));
}

TEST_F(LibraryStoreTest, SaveCollectsOnlyPicturesOutOfASnippetsDirectory) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    std::ofstream(ShotItemDir() / "notes.txt") << "not the store's";
    std::ofstream(ShotItemDir() / "stray.qoi") << "a picture nothing names";

    snapshot.canvases[0].items[1].name = "Renamed in place";
    snapshot.canvases[0].items[1].id = 4;  // the slug changes, the directory moves, the record is rewritten
    ASSERT_TRUE(store.Save(snapshot));

    const std::filesystem::path movedDir =
        dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "renamed-in-place-000004";
    ASSERT_TRUE(std::filesystem::exists(movedDir / "item.json"));
    EXPECT_TRUE(std::filesystem::exists(movedDir / "notes.txt")) << "only pictures are the store's to collect";
    EXPECT_FALSE(std::filesystem::exists(movedDir / "stray.qoi")) << "a picture nothing names is";
}

// ===== Saving only what changed =====
//
// A save costs what changed, not the whole library. These tests are about
// the two things that have to keep being true: nothing that changed may be
// missed, and nothing that didn't may be rewritten.
//
// They observe *which files the store touched*, which is the only way to see
// the difference from outside - a correct incremental save and a correct full
// save leave identical trees, and only the writes tell them apart.
class IncrementalSaveTest : public LibraryStoreTest {
protected:
    // Every file under the library, by last-write time. Compared before and
    // after a save to see exactly what it touched.
    std::map<std::string, std::filesystem::file_time_type> FileTimes() const {
        std::map<std::string, std::filesystem::file_time_type> times;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_)) {
            if (entry.is_regular_file()) {
                times[std::filesystem::relative(entry.path(), dir_).generic_string()] =
                    entry.last_write_time();
            }
        }
        return times;
    }

    // Which files a save rewrote. The clock a filesystem stamps files with is
    // coarse, so every save under test is preceded by stamping every file
    // back to a distinctly older time - then "touched" is unambiguous rather
    // than a question of whether two writes landed in the same tick.
    std::vector<std::string> FilesTouchedBy(const std::function<void()>& save) const {
        const auto ancient = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_)) {
            if (entry.is_regular_file()) {
                std::filesystem::last_write_time(entry.path(), ancient);
            }
        }
        const std::map<std::string, std::filesystem::file_time_type> before = FileTimes();
        save();
        std::vector<std::string> touched;
        for (const auto& [name, time] : FileTimes()) {
            const auto it = before.find(name);
            if (it == before.end() || it->second != time) {
                touched.push_back(name);
            }
        }
        std::sort(touched.begin(), touched.end());
        return touched;
    }
};

TEST_F(IncrementalSaveTest, SavingAnUnchangedLibraryWritesNothingAtAll) {
    const LibraryStore store{dir_};
    const CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_TRUE(FilesTouchedBy([&] { store.Save(snapshot); }).empty());
    // ...and again, so this isn't just the second save settling something.
    EXPECT_TRUE(FilesTouchedBy([&] { store.Save(snapshot); }).empty());
}

TEST_F(IncrementalSaveTest, ChangingOneItemRewritesOnlyThatItemsRecord) {
    const LibraryStore store{dir_};
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[0].items[0].strokes[0].points.push_back(StrokePoint{7, 8});
    const std::vector<std::string> touched = FilesTouchedBy([&] { store.Save(snapshot); });
    EXPECT_EQ(touched, std::vector<std::string>{
                            "folders/folder-1-000001/canvas-1-000002/drawing-1-000003/item.json"})
        << "the other snippet, both order files, the canvas and the folder were all unchanged";
}

// The point of the whole exercise, stated as a test: the work no longer
// scales with the library, only with what was touched.
TEST_F(IncrementalSaveTest, AnUntouchedCanvasCostsNothingWhenAnotherOneChanges) {
    const LibraryStore store{dir_};
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    Canvas second;
    second.id = 20;
    second.name = "Canvas 2";
    second.folderId = 1;
    Item item;
    item.id = 21;
    item.name = "Drawing 2";
    item.rect = Rect{0, 0, 100, 100};
    second.items.push_back(item);
    snapshot.canvases.push_back(second);
    ASSERT_TRUE(store.Save(snapshot));

    snapshot.canvases[1].items[0].rect.x += 5.0f;
    const std::vector<std::string> touched = FilesTouchedBy([&] { store.Save(snapshot); });
    EXPECT_EQ(touched, std::vector<std::string>{
                            "folders/folder-1-000001/canvas-2-00000k/drawing-2-00000l/item.json"});
}

// Every field the serializer writes has to move the hash, or an edit to it is
// silently never saved. Driven through Save rather than at HashItem directly:
// what matters is not that some hash changed but that the file was rewritten,
// and that is also what keeps this honest if the mechanism is ever replaced.
TEST_F(IncrementalSaveTest, EveryPersistedItemFieldCausesARewrite) {
    const std::string itemFile = "folders/folder-1-000001/canvas-1-000002/drawing-1-000003/item.json";
    // Each entry changes exactly one thing an item record carries.
    const std::vector<std::pair<std::string, std::function<void(Item&)>>> mutations = {
        {"hasBackground", [](Item& i) { i.hasBackground = !i.hasBackground; }},
        {"rect", [](Item& i) { i.rect.w += 1.0f; }},
        {"nativeW", [](Item& i) { i.nativeW += 1.0f; }},
        {"nativeH", [](Item& i) { i.nativeH += 1.0f; }},
        {"foregroundOpacity", [](Item& i) { i.foregroundOpacity = 0.125f; }},
        {"isFullscreen", [](Item& i) { i.isFullscreen = !i.isFullscreen; }},
        {"isFullscreenStretch", [](Item& i) { i.isFullscreenStretch = !i.isFullscreenStretch; }},
        {"minimized", [](Item& i) { i.minimized = !i.minimized; }},
        {"pinned", [](Item& i) { i.pinned = !i.pinned; }},
        {"noteText", [](Item& i) { i.noteText += "!"; }},
        {"noteTextColorRGBA", [](Item& i) { i.noteTextColorRGBA ^= 0xFFu; }},
        {"noteTextSizePx", [](Item& i) { i.noteTextSizePx += 1.0f; }},
        {"anchorRect", [](Item& i) { i.anchorRect.y += 1.0f; }},
        {"anchorDisplayWidth", [](Item& i) { i.anchorDisplayWidth += 1.0f; }},
        {"anchorDisplayHeight", [](Item& i) { i.anchorDisplayHeight += 1.0f; }},
        {"strokes: a point moved", [](Item& i) { i.strokes[0].points[1].x += 1.0f; }},
        {"strokes: a point added", [](Item& i) { i.strokes[0].points.push_back(StrokePoint{9, 9}); }},
        {"strokes: a point removed", [](Item& i) { i.strokes[0].points.pop_back(); }},
        {"strokes: color", [](Item& i) { i.strokes[0].colorRGBA ^= 0xFFu; }},
        {"strokes: width", [](Item& i) { i.strokes[0].width += 1.0f; }},
        {"strokes: one added", [](Item& i) { i.strokes.push_back(i.strokes[0]); }},
        {"strokes: all removed", [](Item& i) { i.strokes.clear(); }},
        {"layers: kind", [](Item& i) { i.layers[0].kind = LayerKind::Painted; }},
        {"layers: opacity", [](Item& i) { i.layers[0].opacity = 0.5f; }},
        {"layers: resolutionScale", [](Item& i) { i.layers[0].resolutionScale = 0.5f; }},
        {"layers: tintColorRGBA", [](Item& i) { i.layers[0].tintColorRGBA ^= 0xFFu; }},
        {"layers: showsPlaceholder", [](Item& i) { i.layers[0].showsPlaceholder = !i.layers[0].showsPlaceholder; }},
        {"layers: placeholderHue", [](Item& i) { i.layers[0].placeholderHue += 1.0f; }},
        {"layers: imageFile", [](Item& i) { i.layers[0].imageFile = "different.qoi"; }},
        {"layers: one added", [](Item& i) { i.layers.push_back(i.layers[0]); }},
        {"createdAt", [](Item& i) { i.createdAt += 1; }},
        {"deletedAt", [](Item& i) { i.deletedAt = 1700000000; }},
    };

    for (const auto& [what, mutate] : mutations) {
        std::filesystem::remove_all(dir_);
        const LibraryStore store{dir_};
        CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
        ASSERT_TRUE(store.Save(snapshot)) << what;
        mutate(snapshot.canvases[0].items[0]);
        const std::vector<std::string> touched = FilesTouchedBy([&] { store.Save(snapshot); });
        EXPECT_NE(std::find(touched.begin(), touched.end(), itemFile), touched.end())
            << "changing " << what << " did not rewrite the item's record - that edit would be lost";
    }
    // The item's *name* is the exception that proves the rule: it moves the
    // directory rather than changing the record, so it is checked separately.
    std::filesystem::remove_all(dir_);
    const LibraryStore store{dir_};
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    snapshot.canvases[0].items[0].name = "Renamed";
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" /
                                         "renamed-000003" / "item.json"));
}

// Undo, from the store's point of view: a snippet goes away and comes back
// exactly as it was. Anything remembered about it has to have been forgotten
// when it went, or the save that brings it back sees a hash it recognizes,
// skips the write, and the snippet is gone from disk for good.
TEST_F(IncrementalSaveTest, ASnippetDeletedAndBroughtBackIsWrittenAgain) {
    const LibraryStore store{dir_};
    const CanvasManagerSnapshot original = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(original));
    const std::filesystem::path itemFile =
        dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" / "drawing-1-000003" / "item.json";
    ASSERT_TRUE(std::filesystem::exists(itemFile));

    CanvasManagerSnapshot without = original;
    without.canvases[0].items.erase(without.canvases[0].items.begin());
    ASSERT_TRUE(store.Save(without));
    ASSERT_FALSE(std::filesystem::exists(itemFile)) << "the delete should have taken the directory with it";

    ASSERT_TRUE(store.Save(original));
    EXPECT_TRUE(std::filesystem::exists(itemFile)) << "undo restored the snippet but the file never came back";

    const LibraryStore reader{dir_};
    const std::optional<CanvasManagerSnapshot> reloaded = reader.Load();
    ASSERT_TRUE(reloaded.has_value());
    ASSERT_EQ(reloaded->canvases.size(), 1u);
    ASSERT_EQ(reloaded->canvases[0].items.size(), original.canvases[0].items.size());
    EXPECT_EQ(reloaded->canvases[0].items[0].strokes, original.canvases[0].items[0].strokes);
}

// ===== The first save of a session =====
//
// Load establishes what a save would write back unchanged, record by record,
// so the first save costs what the load had to repair - and for a tree that
// needed no repair, that is nothing at all.

TEST_F(IncrementalSaveTest, ACleanLoadFollowedByASaveWritesNothing) {
    {
        const LibraryStore writer{dir_};
        ASSERT_TRUE(writer.Save(MakeSampleSnapshot()));
    }
    const LibraryStore store{dir_};
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(FilesTouchedBy([&] { store.Save(*loaded); }).empty())
        << "nothing was repaired, so nothing should have been written";
}

// A snippet directory copied by hand: two records claim one id, the copy
// gets a fresh one, and the canvas's order file gains a name. Those two
// files are the whole of the first save; the other snippet, the canvas, the
// folder and library.json are exactly as a save would write them.
TEST_F(IncrementalSaveTest, TheFirstSaveWritesOnlyWhatTheLoadRepaired) {
    {
        const LibraryStore writer{dir_};
        ASSERT_TRUE(writer.Save(MakeSampleSnapshot()));
    }
    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    std::filesystem::copy(canvasDir / "drawing-1-000003", canvasDir / "drawing-1-copy-000003",
                           std::filesystem::copy_options::recursive);

    const LibraryStore store{dir_};
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 3u);
    uint64_t copyId = 0;
    for (const Item& item : loaded->canvases[0].items) {
        if (item.id != 3 && item.id != 4) {
            copyId = item.id;
        }
    }
    ASSERT_NE(copyId, 0u) << "the copy should have been given an id of its own";

    const std::vector<std::string> touched = FilesTouchedBy([&] { store.Save(*loaded); });
    EXPECT_EQ(touched, (std::vector<std::string>{
                           "folders/folder-1-000001/canvas-1-000002/" + MakeSlug("Drawing 1", copyId) + "/item.json",
                           "folders/folder-1-000001/canvas-1-000002/order.json"}));
}

TEST_F(IncrementalSaveTest, TheFirstSaveWritesALibraryRecordTheLoadHadToRepair) {
    {
        const LibraryStore writer{dir_};
        ASSERT_TRUE(writer.Save(MakeSampleSnapshot()));
    }
    // library.json names a canvas that is not there; Load falls back to one
    // that is, and that repair has to reach the disk.
    nlohmann::json globals = nlohmann::json::parse(std::ifstream(dir_ / "library.json"));
    globals["currentCanvasId"] = FormatUid(999);
    std::ofstream(dir_ / "library.json") << globals.dump(2);

    const LibraryStore store{dir_};
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->currentCanvasId, 2u);
    EXPECT_EQ(FilesTouchedBy([&] { store.Save(*loaded); }), std::vector<std::string>{"library.json"});
}

// The safety net under all of the above, and the one test that would catch a
// missed change whatever the mechanism: whatever sequence of edits happens,
// what comes back off disk is what was in memory. If any save ever skips
// something it should have written, these stop matching.
TEST_F(IncrementalSaveTest, WhateverHappensTheTreeStillReloadsToWhatWasInMemory) {
    const LibraryStore store{dir_};
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));

    // A deterministic walk through the kinds of change the app can make -
    // seeded, so a failure is reproducible rather than a story about one run.
    std::mt19937 random(20260910);
    uint64_t nextId = 100;
    for (int step = 0; step < 80; ++step) {
        Canvas& canvas = snapshot.canvases[random() % snapshot.canvases.size()];
        switch (random() % 8) {
            case 0: {  // add a snippet
                Item item;
                item.id = nextId++;
                item.name = "Added " + std::to_string(item.id);
                item.rect = Rect{1, 2, 30, 40};
                canvas.items.push_back(item);
                break;
            }
            case 1:  // remove one
                if (!canvas.items.empty()) {
                    canvas.items.erase(canvas.items.begin() +
                                        static_cast<long>(random() % canvas.items.size()));
                }
                break;
            case 2:  // draw on one
                if (!canvas.items.empty()) {
                    Stroke stroke;
                    stroke.colorRGBA = static_cast<uint32_t>(random());
                    stroke.points = {StrokePoint{1, 1}, StrokePoint{2, 2}};
                    canvas.items[random() % canvas.items.size()].strokes.push_back(stroke);
                }
                break;
            case 3:  // move one
                if (!canvas.items.empty()) {
                    canvas.items[random() % canvas.items.size()].rect.x += 3.0f;
                }
                break;
            case 4:  // rename one
                if (!canvas.items.empty()) {
                    canvas.items[random() % canvas.items.size()].name =
                        "Renamed " + std::to_string(random() % 1000);
                }
                break;
            case 5:  // reorder them
                if (canvas.items.size() >= 2) {
                    std::swap(canvas.items.front(), canvas.items.back());
                }
                break;
            case 6: {  // a new canvas
                Canvas fresh;
                fresh.id = nextId++;
                fresh.name = "Canvas " + std::to_string(fresh.id);
                fresh.folderId = snapshot.folders[0].id;
                snapshot.canvases.push_back(fresh);
                break;
            }
            case 7:  // move a snippet to another canvas
                if (snapshot.canvases.size() >= 2 && !canvas.items.empty()) {
                    Canvas& other = snapshot.canvases[(&canvas == &snapshot.canvases[0]) ? 1 : 0];
                    other.items.push_back(canvas.items.back());
                    canvas.items.pop_back();
                }
                break;
            default:
                break;
        }
        ASSERT_TRUE(store.Save(snapshot)) << "step " << step;

        // A fresh store, so this reads the tree rather than anything the
        // saving store remembers about it.
        const LibraryStore reader{dir_};
        const std::optional<CanvasManagerSnapshot> reloaded = reader.Load();
        ASSERT_TRUE(reloaded.has_value()) << "step " << step;
        ASSERT_EQ(reloaded->canvases.size(), snapshot.canvases.size()) << "step " << step;
        for (size_t c = 0; c < snapshot.canvases.size(); ++c) {
            const Canvas& expected = snapshot.canvases[c];
            const auto found = std::find_if(reloaded->canvases.begin(), reloaded->canvases.end(),
                                             [&](const Canvas& x) { return x.id == expected.id; });
            ASSERT_NE(found, reloaded->canvases.end()) << "step " << step << ", canvas " << expected.id;
            ASSERT_EQ(found->items.size(), expected.items.size())
                << "step " << step << ", canvas " << expected.id;
            for (size_t i = 0; i < expected.items.size(); ++i) {
                EXPECT_EQ(found->items[i].id, expected.items[i].id) << "step " << step;
                EXPECT_EQ(found->items[i].name, expected.items[i].name) << "step " << step;
                EXPECT_EQ(found->items[i].rect, expected.items[i].rect) << "step " << step;
                EXPECT_EQ(found->items[i].strokes, expected.items[i].strokes) << "step " << step;
            }
        }
    }
}

}  // namespace
}  // namespace sz::core::persistence
