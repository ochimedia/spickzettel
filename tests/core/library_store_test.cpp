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
    shot.ImageLayer()->imageFile = "000004.png";
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

// An id as a test document spells it: either a number, the way records
// were written before ids took the directories' base36 spelling, or that
// spelling itself.
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
void WriteLibraryTree(const std::filesystem::path& root, const std::string& document) {
    nlohmann::json doc = nlohmann::json::parse(document);
    std::filesystem::create_directories(root);

    nlohmann::json globals = doc;
    globals.erase("folders");
    globals.erase("canvases");
    std::ofstream(root / "library.json") << globals.dump(2);

    for (const auto& folder : doc.value("folders", nlohmann::json::array())) {
        const std::string dirName = "f-" + FormatUid(IdOf(folder));
        const std::filesystem::path folderDir = root / "folders" / dirName;
        std::filesystem::create_directories(folderDir);
        std::ofstream(folderDir / "folder.json") << folder.dump(2);

        for (const auto& canvas : doc.value("canvases", nlohmann::json::array())) {
            if (IdOf(canvas, "folderId") != IdOf(folder)) {
                continue;
            }
            const std::filesystem::path canvasDir =
                folderDir / ("c-" + FormatUid(IdOf(canvas)));
            std::filesystem::create_directories(canvasDir);
            nlohmann::json canvasRecord = canvas;
            canvasRecord.erase("items");
            std::ofstream(canvasDir / "canvas.json") << canvasRecord.dump(2);

            // Each snippet is a directory of its own beside the canvas
            // record, in the order the document listed them.
            std::vector<std::string> itemOrder;
            for (const auto& item : canvas.value("items", nlohmann::json::array())) {
                const std::string itemDirName = "i-" + FormatUid(IdOf(item));
                std::filesystem::create_directories(canvasDir / itemDirName);
                std::ofstream(canvasDir / itemDirName / "item.json") << item.dump(2);
                itemOrder.push_back(itemDirName);
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
    std::ofstream(dir / "folder.json") << nlohmann::json{{"id", id}, {"name", name}}.dump(2);
    return dir;
}

std::filesystem::path PlaceCanvas(const std::filesystem::path& folderDir, const std::string& dirName,
                                   uint64_t id, const std::string& name, uint64_t claimsFolderId = 0) {
    const std::filesystem::path dir = folderDir / dirName;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "canvas.json") << nlohmann::json{{"id", id},
                                                          {"name", name},
                                                          {"folderId", claimsFolderId},
                                                          {"items", nlohmann::json::array()}}
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
    EXPECT_EQ(shot.ImageLayer()->imageFile, "000004.png");
}

// The migration that matters: every library written before layers existed
// carries an item's one layer as five fields of the item's own. Those items
// have to come back with their screenshot, their opacity and their tint
// intact - losing them would be losing the picture.
TEST_F(LibraryStoreTest, LoadReadsAPreLayersItemAsOneLayer) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2, "nextId": 10,
        "folders": [{"id": 1, "name": "F", "slug": "f-1"}],
        "canvases": [{"id": 2, "name": "C", "slug": "c-2", "folderId": 1, "items": [
            {"id": 3, "name": "Shot", "hasBackground": true, "backgroundOpacity": 0.8,
             "backgroundColorRGBA": 305419896, "seedHue": 42.5, "shotImageFile": "000003.png"},
            {"id": 4, "name": "Drawing", "hasBackground": false}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u);

    const Item& shot = loaded->canvases[0].items[0];
    ASSERT_EQ(shot.layers.size(), 1u);
    EXPECT_EQ(shot.layers[0].kind, LayerKind::Image);
    EXPECT_FLOAT_EQ(shot.layers[0].opacity, 0.8f);
    EXPECT_EQ(shot.layers[0].tintColorRGBA, 305419896u);
    EXPECT_TRUE(shot.layers[0].showsPlaceholder);
    EXPECT_FLOAT_EQ(shot.layers[0].placeholderHue, 42.5f);
    EXPECT_EQ(shot.layers[0].imageFile, "000003.png");

    // A drawing had no background keys at all, and its layer falls back to
    // the same transparent default CanvasManager::CreateItem gives one.
    const Item& drawing = loaded->canvases[0].items[1];
    ASSERT_EQ(drawing.layers.size(), 1u);
    EXPECT_FLOAT_EQ(drawing.layers[0].opacity, 0.0f);
    EXPECT_FALSE(drawing.layers[0].showsPlaceholder);
    EXPECT_TRUE(drawing.layers[0].imageFile.empty());
}

// An item is never layerless, however broken the record - everything
// downstream reaches for ImageLayer() without checking.
TEST_F(LibraryStoreTest, LoadGivesAnItemALayerEvenIfTheListIsEmpty) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2, "nextId": 10,
        "folders": [{"id": 1, "name": "F", "slug": "f-1"}],
        "canvases": [{"id": 2, "name": "C", "slug": "c-2", "folderId": 1, "items": [
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
    EXPECT_EQ(reloaded.layers[0].imageFile, "000004.png");
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
    EXPECT_FALSE(std::filesystem::exists(dir_ / "images" / "000007.qoi")) << "staging is drained";
}

TEST_F(LibraryStoreTest, LoadDefaultsItemAnchorToNotYetAnchoredWhenLibraryPredatesThem) {
    std::filesystem::create_directories(dir_);
    // A library.json written before resolution-relative item sizing
    // existed - the missing keys fall back to 0/a zero Rect, which
    // CanvasManager::SyncItemsToDisplaySize already reads as "not yet
    // anchored" (see Item::anchorRect's own doc comment) rather than a
    // real 0x0 anchor - it just adopts the loaded rect as-is the first
    // time it runs.
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2, "nextId": 10,
        "folders": [{"id": 1, "name": "F", "slug": "f-1"}],
        "canvases": [{"id": 2, "name": "C", "slug": "c-2", "folderId": 1, "items": [
            {"id": 3, "type": "drawing", "name": "A", "rect": {"x": 1, "y": 2, "w": 3, "h": 4}}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[0].anchorDisplayWidth, 0.0f);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[0].anchorDisplayHeight, 0.0f);
}

// Text styling is per item (see Item::noteTextColorRGBA/noteTextSizePx),
// which means a library written before it existed has neither key - and
// has to read back as exactly the fixed look it was drawn with then:
// opaque white at the UI font's own 17px.
TEST_F(LibraryStoreTest, LoadDefaultsNoteTextStyleToTheOldFixedLookWhenLibraryPredatesIt) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2, "nextId": 10,
        "folders": [{"id": 1, "name": "F", "slug": "f-1"}],
        "canvases": [{"id": 2, "name": "C", "slug": "c-2", "folderId": 1, "items": [
            {"id": 3, "name": "A", "noteText": "written before text had a color"}
        ]}]
    })");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();

    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].items[0].noteTextColorRGBA, 0xFFFFFFFFu);
    EXPECT_FLOAT_EQ(loaded->canvases[0].items[0].noteTextSizePx, 17.0f);
}

// Out-of-band sizes are pulled into range rather than rejected outright -
// a hand-edited or corrupted 0 would otherwise render an invisible
// caption with no way to tell it from an empty one.
TEST_F(LibraryStoreTest, LoadClampsAnOutOfRangeNoteTextSize) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2, "nextId": 10,
        "folders": [{"id": 1, "name": "F", "slug": "f-1"}],
        "canvases": [{"id": 2, "name": "C", "slug": "c-2", "folderId": 1, "items": [
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

// Keys this version knows nothing about - a setting an older or newer
// build kept there - must not stop the rest of the library from loading.
TEST_F(LibraryStoreTest, UnknownKeysInLibraryJsonAreIgnored) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2, "nextId": 10,
        "favoriteToolIds": [4, 0, 1], "favoriteColorsRGBA": [4278190335, 0, 0],
        "favoriteCreateIds": [0, 1, 3], "favoriteGlobalIds": [2, 0, 3],
        "folders": [{"id": 1, "name": "F", "slug": "f-1"}],
        "canvases": [{"id": 2, "name": "C", "slug": "c-2", "folderId": 1, "items": []}]
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

// There is no "next id" left to heal - ids are drawn at random and checked
// against what is actually loaded - so the property that matters is that a
// hand-written library, whatever ids it happens to name, cannot make the
// allocator hand out one that is already in use.
TEST_F(LibraryStoreTest, IdsMintedAfterLoadingAHandWrittenLibraryAvoidWhatItNames) {
    std::filesystem::create_directories(dir_);
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 5, "currentCanvasId": 6,
        "folders": [{"id": 5, "name": "F", "slug": "f-000005"}],
        "canvases": [{"id": 6, "name": "C", "slug": "c-000006", "folderId": 5,
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
    // Physically in Play, but its record still claims Work - which is
    // exactly what dragging the directory across leaves behind.
    PlaceCanvas(play, "notes-000003", 3, "Notes", /*claimsFolderId=*/1);
    WriteLibraryTree(dir_, R"({"currentFolderId": 1, "currentCanvasId": 3})");

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].folderId, 2u) << "where it is beats what it says";
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
// names use - and one written before that, listing whole directory names,
// is still read.
TEST_F(LibraryStoreTest, OrderFilesListUidsAndStillReadTheOldNames) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    const nlohmann::json order = nlohmann::json::parse(std::ifstream(canvasDir / "order.json"));
    EXPECT_EQ(order["items"], (nlohmann::json::array({"000003", "000004"})));
    EXPECT_EQ(nlohmann::json::parse(std::ifstream(dir_ / "folders" / "order.json"))["folders"],
              (nlohmann::json::array({"000001"})));

    // The old form, reversed, decides the order just the same.
    PlaceOrderFile(canvasDir, "items", {"shot-1-000004", "drawing-1-000003"});
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->canvases[0].items.size(), 2u);
    EXPECT_EQ(loaded->canvases[0].items[0].id, 4u);
    EXPECT_EQ(loaded->canvases[0].items[1].id, 3u);
}

TEST_F(LibraryStoreTest, AnOrderFileDecidesTheOrderOfWhatIsThere) {
    const std::filesystem::path work = PlaceFolder(dir_, "work-000001", 1, "Work");
    PlaceCanvas(work, "a-000003", 3, "A");
    PlaceCanvas(work, "b-000004", 4, "B");
    PlaceCanvas(work, "c-000005", 5, "C");
    // Deliberately not alphabetical, so passing cannot be an accident of
    // directory enumeration order.
    PlaceOrderFile(work, "canvases", {"c-000005", "a-000003", "b-000004"});
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
    PlaceOrderFile(work, "canvases", {"deleted-0000zz", "a-000003"});
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
    PlaceOrderFile(work, "canvases", {"a-000003"});
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
    PlaceOrderFile(work, "canvases", {"a-000003", "b-000004"});
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
// the app makes produces any more - takes it out of the library's tree,
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
// two can be matched by eye - and a record from before that carries the
// same id as a number, which still reads and is rewritten on the first save.
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

    // The number form, as every record was written before.
    std::ofstream(folderDir / "folder.json") << R"({"id": 1, "name": "Folder 1"})";
    LibraryStore reopened(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->folders.size(), 1u);
    EXPECT_EQ(loaded->folders[0].id, 1u);
    ASSERT_TRUE(reopened.Save(*loaded));
    EXPECT_EQ(nlohmann::json::parse(std::ifstream(folderDir / "folder.json"))["id"], "000001")
        << "brought into the one spelling by the first save";
}

TEST_F(LibraryStoreTest, SaveWritesADirectoryPerSnippet) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));

    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";
    EXPECT_TRUE(std::filesystem::exists(canvasDir / "drawing-1-000003" / "item.json"));
    EXPECT_TRUE(std::filesystem::exists(canvasDir / "shot-1-000004" / "item.json"));
    // The canvas record no longer carries its snippets - the directories do.
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

// Deleting for good is Remove, at once - and the save after it agrees about
// what went, setting nothing aside.
TEST_F(LibraryStoreTest, RemoveDeletesASnippetForGoodAndTheNextSaveAgrees) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = Checkerboard(64, 64);
    ASSERT_TRUE(store.SaveImage(4, pixels.data(), 64, 64).has_value());
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000004.qoi";
    ASSERT_TRUE(store.Save(snapshot));

    ASSERT_TRUE(store.Remove(4));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir()));
    EXPECT_FALSE(store.LoadImage(4, "000004.qoi").has_value());
    EXPECT_FALSE(store.Remove(4)) << "nothing by that id left to remove";

    snapshot.canvases[0].items.pop_back();
    ASSERT_TRUE(store.Save(snapshot));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired"));
    const std::optional<CanvasManagerSnapshot> loaded = LibraryStore(dir_).Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->canvases[0].items.size(), 1u);
}

TEST_F(LibraryStoreTest, RemoveOfAFolderTakesEverythingInIt) {
    LibraryStore store(dir_);
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));

    ASSERT_TRUE(store.Remove(1));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001"));
    ASSERT_TRUE(store.Save(CanvasManagerSnapshot{}));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "retired")) << "its canvas and snippets went with it, not astray";
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
    std::ofstream(dir_ / "library.json") << R"({"currentFolderId": 1, "currentCanvasId": "wrong type",
                                                 "favoriteToolIds": "also wrong"})";

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
    std::ofstream(drawingDir / "item.json") << R"({"id": 3, "name": 12345, "rect": "not a rect"})";

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
// library.json holds pointers and preferences. Anything wrong with it is a
// reason to default those, never a reason to report the library absent
// while a tree is there - because "absent" starts the app fresh, and a
// fresh library's first save retires everything it finds. That is exactly
// what happened: an older build that could not read the pointer file wrote
// its own single-file library.json over it, and the next start of the
// current build lost the whole tree.

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

// The measured sequence, end to end at the store level: the tree survives
// the old-shaped file, the file is set aside, and a save afterwards leaves
// every snippet where it was.
TEST_F(LibraryStoreTest, AnOldSingleFileLibraryJsonBesideATreeCostsThePointersNotTheTree) {
    ASSERT_TRUE(LibraryStore(dir_).Save(MakeSampleSnapshot()));
    std::ofstream(dir_ / "library.json") << R"({"version": 1, "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "Old"}],
        "canvases": [{"id": 2, "folderId": 1, "name": "Old canvas", "items": [{"id": 4, "name": "Welcome"}]}]})";

    LibraryStore store(dir_);
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value()) << "the tree is the library";
    ASSERT_EQ(loaded->canvases.size(), 1u);
    EXPECT_EQ(loaded->canvases[0].name, "Canvas 1") << "read from the tree, not from the old file";
    EXPECT_EQ(loaded->canvases[0].items.size(), 2u);
    EXPECT_TRUE(std::filesystem::exists(dir_ / "library.json.v0"));

    ASSERT_TRUE(store.Save(*loaded));
    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "item.json")) << "nothing was retired";
    EXPECT_TRUE(std::filesystem::exists(dir_ / "folders" / "folder-1-000001" / "canvas-1-000002" /
                                         "drawing-1-000003" / "item.json"));
    const std::optional<CanvasManagerSnapshot> again = LibraryStore(dir_).Load();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->canvases[0].items.size(), 2u);
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

// The single file that held everything before the tree. It was never
// shipped, so there is no migration - but reading it as an empty library
// and then overwriting it on the first save was the one way this format
// change could have lost a library, so it is set aside instead. Without a
// tree beside it the library starts fresh; with one, see above.
TEST_F(LibraryStoreTest, AnOldSingleFileLibraryIsSetAsideRatherThanReadAsEmpty) {
    std::filesystem::create_directories(dir_);
    const std::string old = R"({"version": 1, "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": 1, "name": "Old"}],
        "canvases": [{"id": 2, "folderId": 1, "name": "Old canvas", "items": [{"id": 4, "name": "Old note"}]}]})";
    std::ofstream(dir_ / "library.json") << old;

    LibraryStore store(dir_);
    EXPECT_FALSE(store.Load().has_value()) << "not read as an empty library";
    EXPECT_FALSE(std::filesystem::exists(dir_ / "library.json"));
    ASSERT_TRUE(std::filesystem::exists(dir_ / "library.json.v0"));
    std::ifstream in(dir_ / "library.json.v0");
    const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(kept, old);
    // A fresh library saves beside it without touching it.
    ASSERT_TRUE(store.Save(MakeSampleSnapshot()));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "library.json.v0"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "library.json"));
}

// ===== Nothing the store deletes or writes is outside the library =====
//
// Every path it mutates is built under its root from sanitised parts, with
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
    std::ofstream(outside / "item.json") << R"({"id": 77, "name": "Linked", "layers": []})";
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
    EXPECT_EQ(record["id"], 77) << "the record behind the link is untouched";
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
    EXPECT_EQ(record["id"], 77) << "nothing was moved into where the link pointed";
    EXPECT_TRUE(std::filesystem::exists(outside / "precious.txt"));
    std::filesystem::remove_all(outside);
}
#endif

// ===== A save that could not write everything says so =====

TEST_F(LibraryStoreTest, AnOrderFileThatCouldNotBeWrittenFailsTheSave) {
    LibraryStore store(dir_);
    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    ASSERT_TRUE(store.Save(snapshot));
    const std::filesystem::path canvasDir = dir_ / "folders" / "folder-1-000001" / "canvas-1-000002";

    // A directory where the temp file wants to be is the one obstruction
    // that fails the write without failing everything around it.
    std::filesystem::create_directories(canvasDir / "order.json.tmp");
    std::swap(snapshot.canvases[0].items[0], snapshot.canvases[0].items[1]);
    EXPECT_FALSE(store.Save(snapshot)) << "the z-order on disk is not the z-order in memory";

    std::filesystem::remove_all(canvasDir / "order.json.tmp");
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

TEST_F(LibraryStoreTest, SaveDeletesOrphanedImagesNoLongerReferencedByAnyItem) {
    LibraryStore store(dir_);
    const std::vector<uint8_t> pixels = {1, 2, 3, 255};
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 1, 1).has_value());   // will stay referenced
    ASSERT_TRUE(store.SaveImage(8, pixels.data(), 1, 1).has_value());   // will become orphaned

    CanvasManagerSnapshot snapshot = MakeSampleSnapshot();
    snapshot.canvases[0].items[1].ImageLayer()->imageFile = "000007.qoi";  // only item 4 references image 7

    ASSERT_TRUE(store.Save(snapshot));

    EXPECT_TRUE(std::filesystem::exists(ShotItemDir() / "000007.qoi"));
    // Nothing names image 8, so it is collected out of staging rather than
    // carried into a snippet that never claimed it.
    EXPECT_FALSE(std::filesystem::exists(dir_ / "images" / "000008.qoi"));
    EXPECT_FALSE(std::filesystem::exists(ShotItemDir() / "000008.qoi"));
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
    EXPECT_TRUE(std::filesystem::exists(dir_ / "images" / "000007.thumb.qoi"));

    const std::optional<DecodedImage> thumb = store.LoadThumbnail(7, *filename);
    ASSERT_TRUE(thumb.has_value());
    EXPECT_EQ(std::max(thumb->width, thumb->height), LibraryStore::kThumbnailMaxExtent);
    // Aspect kept, so a tile drawn from this is the same shape as one drawn
    // from the full image.
    EXPECT_EQ(thumb->height, thumb->width / 2);
    // And it is smaller on disk than the image it stands in for, which is
    // the entire reason it exists.
    EXPECT_LT(std::filesystem::file_size(dir_ / "images" / "000007.thumb.qoi"),
               std::filesystem::file_size(dir_ / "images" / "000007.qoi"));
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

TEST_F(LibraryStoreTest, LoadThumbnailReturnsNulloptForALibraryThatHasNone) {
    LibraryStore store(dir_);
    // Exactly the state a library written before thumbnails existed is in:
    // the image is there, the sidecar isn't, and that is not an error.
    const std::vector<uint8_t> pixels = {1, 2, 3, 255};
    ASSERT_TRUE(store.SaveImage(7, pixels.data(), 1, 1).has_value());
    std::filesystem::remove(dir_ / "images" / "000007.thumb.qoi");

    EXPECT_FALSE(store.LoadThumbnail(7, "000007.qoi").has_value());
    EXPECT_FALSE(store.LoadThumbnail(7, "").has_value());
    EXPECT_TRUE(store.LoadImage(7, "000007.qoi").has_value());
}

TEST_F(LibraryStoreTest, ThumbnailsAreNamedForLegacyPngImagesToo) {
    // A library from before the QOI switch names its images .png; the
    // sidecar is still QOI, and still has to be found by the same rule.
    EXPECT_EQ(LibraryStore::ThumbnailFilename("000007.png"), "000007.thumb.qoi");
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
    EXPECT_FALSE(std::filesystem::exists(dir_ / "images" / "000008.qoi"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "images" / "000008.thumb.qoi"));
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
    std::filesystem::create_directories(ShotItemDir() / "item.json.tmp");
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

    std::filesystem::remove_all(ShotItemDir() / "item.json.tmp");
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

// Every field the serialiser writes has to move the hash, or an edit to it is
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
        {"strokes: colour", [](Item& i) { i.strokes[0].colorRGBA ^= 0xFFu; }},
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
// when it went, or the save that brings it back sees a hash it recognises,
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
    globals["currentCanvasId"] = 999;
    std::ofstream(dir_ / "library.json") << globals.dump(2);

    const LibraryStore store{dir_};
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->currentCanvasId, 2u);
    EXPECT_EQ(FilesTouchedBy([&] { store.Save(*loaded); }), std::vector<std::string>{"library.json"});
}

// A record in the shape from before layers existed reads fine (see
// LoadReadsAPreLayersItemAsOneLayer) and is rewritten in the current shape
// by the first save - which is what keeps the compatibility path from having
// to be kept forever.
TEST_F(IncrementalSaveTest, TheFirstSaveBringsAPreLayersRecordUpToDate) {
    WriteLibraryTree(dir_, R"({
        "currentFolderId": 1, "currentCanvasId": 2,
        "folders": [{"id": "000001", "name": "F"}],
        "canvases": [{"id": 2, "name": "C", "slug": "c-2", "folderId": 1, "items": [
            {"id": 3, "name": "Shot", "hasBackground": true, "backgroundOpacity": 0.8,
             "backgroundColorRGBA": 305419896, "seedHue": 42.5, "shotImageFile": "000003.png"}
        ]}]
    })");
    const LibraryStore store{dir_};
    const std::optional<CanvasManagerSnapshot> loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());

    const std::vector<std::string> touched = FilesTouchedBy([&] { store.Save(*loaded); });
    // The snippet's directory is renamed to its slug on the way, so the
    // record appears under its new name.
    const std::string itemFile = "folders/f-000001/c-000002/shot-000003/item.json";
    EXPECT_NE(std::find(touched.begin(), touched.end(), itemFile), touched.end()) << "the record kept its old shape";
    EXPECT_EQ(std::find(touched.begin(), touched.end(), "folders/f-000001/folder.json"), touched.end())
        << "the folder record was already what a save writes";
    const nlohmann::json rewritten = nlohmann::json::parse(std::ifstream(dir_ / itemFile));
    EXPECT_TRUE(rewritten.contains("layers"));
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
