#include "core/persistence/library_store.h"

#include <algorithm>
#include <map>
#include <fstream>
#include <functional>
#include <sstream>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "core/util/slug.h"
#include "core/util/timestamp_name.h"
#include "core/util/uid.h"

namespace sz::core::persistence {

namespace {

using nlohmann::json;

constexpr int kLibraryFormatVersion = 1;

// An id in a record is spelled the way the directory names spell it: six
// base36 characters (see util/uid.h). One spelling everywhere, so that a
// record and the directory holding it, or a pointer and the thing it points
// at, can be matched by eye. Records written before this carry the same id
// as a number, and are read either way; the first save writes them back in
// this form.
json IdJson(uint64_t id) { return FormatUid(id); }

uint64_t ReadId(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return 0;
    }
    if (it->is_string()) {
        return ParseUid(it->get<std::string>()).value_or(0);
    }
    if (it->is_number_unsigned()) {
        return it->get<uint64_t>();
    }
    return 0;  // anything else is no id, and 0 is "no id" everywhere
}

json ToJson(const StrokePoint& p) { return json{{"x", p.x}, {"y", p.y}}; }

void FromJson(const json& j, StrokePoint& out) {
    out.x = j.value("x", 0.0f);
    out.y = j.value("y", 0.0f);
}

json ToJson(const Stroke& s) {
    json points = json::array();
    for (const StrokePoint& p : s.points) {
        points.push_back(ToJson(p));
    }
    return json{{"colorRGBA", s.colorRGBA}, {"width", s.width}, {"points", std::move(points)}};
}

void FromJson(const json& j, Stroke& out) {
    out.colorRGBA = j.value("colorRGBA", 0xFF0000FFu);
    out.width = j.value("width", 3.0f);
    out.points.clear();
    if (const auto it = j.find("points"); it != j.end() && it->is_array()) {
        out.points.reserve(it->size());
        for (const auto& pointJson : *it) {
            StrokePoint point;
            FromJson(pointJson, point);
            out.points.push_back(point);
        }
    }
}

json ToJson(const Rect& r) { return json{{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}}; }

void FromJson(const json& j, Rect& out) {
    out.x = j.value("x", 0.0f);
    out.y = j.value("y", 0.0f);
    out.w = j.value("w", 0.0f);
    out.h = j.value("h", 0.0f);
}

// A bare filename and nothing else: no separators, no ".", no "..", no
// drive letter. Layer::imageFile is the one string that reaches the
// filesystem from a record verbatim, and a record can be hand-edited, so
// this is what keeps "..\..\x.qoi" in a record from turning a picture read
// or a thumbnail write into one outside the library. Applied where the
// name enters (FromJson) and where it is used (see FindImage), so neither
// has to trust the other.
bool IsPlainFilename(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        if (c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

// Whether `name` is one this store could have written a picture under: a
// capture or painted layer (.qoi, or .png from before QOI) or a thumbnail
// beside one. What the per-snippet collection below is limited to.
bool IsPictureFilename(const std::string& name) {
    const auto endsWith = [&name](std::string_view suffix) {
        return name.size() >= suffix.size() &&
               std::string_view(name).substr(name.size() - suffix.size()) == suffix;
    };
    return endsWith(".qoi") || endsWith(".png");
}

json ToJson(const Layer& layer) {
    // textureHandle is deliberately absent: a GPU handle from a previous
    // run is never valid to reuse, and imageFile is what it is re-derived
    // from - see CanvasManager::SyncShotTexturesToCanvas.
    return json{
        {"kind", layer.kind == LayerKind::Painted ? "painted" : "image"},
        {"opacity", layer.opacity},
        {"resolutionScale", layer.resolutionScale},
        {"tintColorRGBA", layer.tintColorRGBA},
        {"showsPlaceholder", layer.showsPlaceholder},
        {"placeholderHue", layer.placeholderHue},
        {"imageFile", layer.imageFile},
    };
}

bool FromJson(const json& j, Layer& out) {
    if (!j.is_object()) {
        return false;
    }
    out.kind = j.value("kind", std::string("image")) == "painted" ? LayerKind::Painted : LayerKind::Image;
    out.opacity = j.value("opacity", 0.0f);
    out.resolutionScale = std::max(j.value("resolutionScale", 1.0f), 0.05f);
    out.tintColorRGBA = j.value("tintColorRGBA", uint32_t{0xFFFFFFFF});
    out.showsPlaceholder = j.value("showsPlaceholder", false);
    out.placeholderHue = j.value("placeholderHue", 0.0f);
    out.imageFile = j.value("imageFile", std::string());
    if (!IsPlainFilename(out.imageFile)) {
        out.imageFile.clear();  // a layer with no picture, rather than one somewhere else
    }
    return true;
}

// The one layer an item had before layers existed, read back out of the
// item's own record: backgroundOpacity, backgroundColorRGBA, hasBackground,
// seedHue and shotImageFile were exactly a single Image layer written
// longhand. Only used for a record with no "layers" key of its own, which
// is every item in a library written before this - they load unchanged
// rather than losing their screenshots, and are written back in the new
// shape on the next save.
Layer LayerFromPreLayersItemJson(const json& j, bool hasBackground) {
    Layer layer;
    layer.kind = LayerKind::Image;
    layer.opacity = j.value("backgroundOpacity", hasBackground ? 1.0f : 0.0f);
    layer.tintColorRGBA = j.value("backgroundColorRGBA", uint32_t{0xFFFFFFFF});
    layer.showsPlaceholder = hasBackground;
    layer.placeholderHue = j.value("seedHue", 0.0f);
    layer.imageFile = j.value("shotImageFile", std::string());
    if (!IsPlainFilename(layer.imageFile)) {
        layer.imageFile.clear();
    }
    return layer;
}

json ToJson(const Item& item) {
    json j{
        {"id", IdJson(item.id)},
        {"hasBackground", item.hasBackground},
        {"name", item.name},
        {"rect", ToJson(item.rect)},
        {"nativeW", item.nativeW},
        {"nativeH", item.nativeH},
        {"foregroundOpacity", item.foregroundOpacity},
        {"isFullscreen", item.isFullscreen},
        {"isFullscreenStretch", item.isFullscreenStretch},
        {"minimized", item.minimized},
        {"pinned", item.pinned},
        {"noteText", item.noteText},
        {"noteTextColorRGBA", item.noteTextColorRGBA},
        {"noteTextSizePx", item.noteTextSizePx},
        {"anchorRect", ToJson(item.anchorRect)},
        {"anchorDisplayWidth", item.anchorDisplayWidth},
        {"anchorDisplayHeight", item.anchorDisplayHeight},
    };
    json strokes = json::array();
    for (const Stroke& stroke : item.strokes) {
        strokes.push_back(ToJson(stroke));
    }
    j["strokes"] = std::move(strokes);
    json layers = json::array();
    for (const Layer& layer : item.layers) {
        layers.push_back(ToJson(layer));
    }
    j["layers"] = std::move(layers);
    // Written only when set: a record that was never stamped carries no
    // key, so a live record and a restored one read the same.
    if (item.createdAt != 0) {
        j["createdAt"] = item.createdAt;
    }
    if (item.deletedAt != 0) {
        j["deletedAt"] = item.deletedAt;
    }
    return j;
}

// ===== Has this item changed since it was last written? =====
//
// Directly above ToJson(Item), and it has to stay there: these two are one
// pair of eyes on the same list of fields. A field added to the serialiser
// and not to this is an edit that is silently never saved - the worst bug
// this file could have - so they are kept adjacent, the order of the fields
// is the same in both, and ItemContentHashTest asserts that every one of
// them moves the hash.
//
// A hash rather than the text it would have written, because avoiding the
// serialisation is the whole point: a stroke point becomes a JSON object,
// and an ordinary canvas carries tens of thousands of them. This walks the
// same data as raw bytes and costs about a thousandth of building that
// document. (The exact text *is* what the small records are compared by -
// see writtenFileText_ - because for those the serialisation is free and an
// exact answer is worth more.)
//
// The hash is only ever asked whether two things differ, never for identity,
// and both sides are produced by this same function in one process - so 64
// bits of FNV-1a with a shift-mix is far more than the job needs.
class ContentHash {
public:
    // One overload for every integer width and every enum, so a field's type
    // can change without silently picking the float or bool conversion
    // instead - which is what an overload set of plain uint64_t/float/bool
    // does, ambiguously.
    template <typename T>
    std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>> Mix(T value) {
        MixWord(static_cast<uint64_t>(value));
    }
    void Mix(float value) {
        // The bits, not the value: a hash that read -0.0f and 0.0f as equal
        // would be right about the number and wrong about the file, which is
        // the only question being asked here.
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        MixWord(static_cast<uint64_t>(bits));
    }
    void Mix(const std::string& value) {
        MixWord(value.size());
        for (const char c : value) {
            MixWord(static_cast<uint64_t>(static_cast<unsigned char>(c)));
        }
    }
    void Mix(const Rect& r) {
        Mix(r.x);
        Mix(r.y);
        Mix(r.w);
        Mix(r.h);
    }
    uint64_t Value() const { return hash_; }

private:
    void MixWord(uint64_t value) {
        hash_ ^= value;
        hash_ *= 1099511628211ull;
        hash_ ^= hash_ >> 29;
    }

    uint64_t hash_ = 1469598103934665603ull;  // FNV-1a offset basis
};

uint64_t HashItem(const Item& item) {
    ContentHash h;
    // Same order, same fields as ToJson(Item) above.
    h.Mix(item.id);
    h.Mix(item.hasBackground);
    h.Mix(item.name);
    h.Mix(item.rect);
    h.Mix(item.nativeW);
    h.Mix(item.nativeH);
    h.Mix(item.foregroundOpacity);
    h.Mix(item.isFullscreen);
    h.Mix(item.isFullscreenStretch);
    h.Mix(item.minimized);
    h.Mix(item.pinned);
    h.Mix(item.noteText);
    h.Mix(item.noteTextColorRGBA);
    h.Mix(item.noteTextSizePx);
    h.Mix(item.anchorRect);
    h.Mix(item.anchorDisplayWidth);
    h.Mix(item.anchorDisplayHeight);
    h.Mix(item.createdAt);
    h.Mix(item.deletedAt);
    // ...and the two arrays, whose length is mixed in as well so that
    // dropping a trailing element cannot leave the hash where it was.
    h.Mix(item.strokes.size());
    for (const Stroke& stroke : item.strokes) {
        h.Mix(stroke.colorRGBA);
        h.Mix(stroke.width);
        h.Mix(stroke.points.size());
        for (const StrokePoint& point : stroke.points) {
            h.Mix(point.x);
            h.Mix(point.y);
        }
    }
    h.Mix(item.layers.size());
    for (const Layer& layer : item.layers) {
        // Matches ToJson(Layer)'s own list - textureHandle is deliberately
        // absent from both, being a GPU handle that is never persisted.
        h.Mix(static_cast<uint64_t>(layer.kind));
        h.Mix(layer.imageFile);
        h.Mix(layer.opacity);
        h.Mix(layer.tintColorRGBA);
        h.Mix(layer.showsPlaceholder);
        h.Mix(layer.placeholderHue);
        h.Mix(layer.resolutionScale);
    }
    return h.Value();
}

// false means "not a usable item record" (no id, or not an object at
// all) - the caller skips it rather than adding a zeroed-out placeholder.
// Every layer's textureHandle is deliberately left at its default (0): a
// GPU handle from a previous run is never valid to reuse - see OverlayApp's
// own reload, which re-derives it from the layer's imageFile instead.
bool FromJson(const json& j, Item& out) {
    if (!j.is_object()) {
        return false;
    }
    out.id = ReadId(j, "id");
    if (out.id == 0) {
        return false;
    }
    out.hasBackground = j.value("hasBackground", false);
    out.name = j.value("name", std::string());
    if (const auto it = j.find("rect"); it != j.end()) {
        FromJson(*it, out.rect);
    }
    out.nativeW = j.value("nativeW", 0.0f);
    out.nativeH = j.value("nativeH", 0.0f);
    out.foregroundOpacity = j.value("foregroundOpacity", 1.0f);
    out.isFullscreen = j.value("isFullscreen", false);
    out.isFullscreenStretch = j.value("isFullscreenStretch", false);
    out.minimized = j.value("minimized", false);
    out.pinned = j.value("pinned", false);
    // A record from before layers existed carries its one layer as five
    // fields of its own - see LayerFromPreLayersItemJson. Told apart by the
    // key rather than by a version number: the absence of "layers" is
    // exactly the condition, and a version field would have to be invented
    // and then maintained to say the same thing.
    out.layers.clear();
    if (const auto layersIt = j.find("layers"); layersIt != j.end() && layersIt->is_array()) {
        for (const json& layerJson : *layersIt) {
            Layer layer;
            if (FromJson(layerJson, layer)) {
                out.layers.push_back(std::move(layer));
            }
        }
    } else {
        out.layers.push_back(LayerFromPreLayersItemJson(j, out.hasBackground));
    }
    if (out.layers.empty()) {
        // A record with an explicitly empty list, or one whose every entry
        // was unreadable. Item::layers is never empty - see its own comment
        // - so this is where that promise is kept for a loaded item.
        out.layers.push_back(Layer{});
    }
    out.noteText = j.value("noteText", std::string());
    out.noteTextColorRGBA = j.value("noteTextColorRGBA", uint32_t{0xFFFFFFFF});
    // Clamped, not rejected, and the fallback is the same default a fresh
    // Item carries - a library written before these two fields existed
    // reads back as a note styled exactly the way it was drawn then.
    out.noteTextSizePx = std::clamp(j.value("noteTextSizePx", 17.0f), kNoteTextSizeMin, kNoteTextSizeMax);
    // anchorRect defaults to a zero Rect (via FromJson(Rect)'s own
    // per-field 0.0f defaults) and anchorDisplayWidth/Height default to 0
    // - together, "not yet anchored" (see Item::anchorRect's own doc
    // comment), the correct fallback for a library saved before this
    // field existed. CanvasManager::SyncItemsToDisplaySize adopts the
    // loaded `rect` as the anchor the first time it runs.
    if (const auto it = j.find("anchorRect"); it != j.end()) {
        FromJson(*it, out.anchorRect);
    }
    out.anchorDisplayWidth = j.value("anchorDisplayWidth", 0.0f);
    out.anchorDisplayHeight = j.value("anchorDisplayHeight", 0.0f);
    out.createdAt = j.value("createdAt", int64_t{0});
    out.deletedAt = j.value("deletedAt", int64_t{0});
    out.strokes.clear();
    if (const auto it = j.find("strokes"); it != j.end() && it->is_array()) {
        out.strokes.reserve(it->size());
        for (const auto& strokeJson : *it) {
            Stroke stroke;
            FromJson(strokeJson, stroke);
            out.strokes.push_back(std::move(stroke));
        }
    }
    return true;
}

// Only what the tree cannot say. No items: each one is a directory of its
// own beside this file, so that a snippet - its record, its picture and its
// thumbnail - is a single thing to move. No folderId: the directory this
// sits in says which folder it is in, and a copy of that here was ignored
// on load and wrong on disk from the moment someone dragged the directory
// somewhere else. No slug: the directory name is regenerated from the
// current name on every save, and a stored one was stale from the first
// rename on. A record that repeats what the tree says can only agree with
// it or disagree with it, and there is nothing to do with the second case.
// The two timestamps every record can carry - see Folder::createdAt - are
// written only when set, so a record that was never stamped carries no key
// and a live record and a restored one read the same.
void WriteStamps(json& j, int64_t createdAt, int64_t deletedAt) {
    if (createdAt != 0) {
        j["createdAt"] = createdAt;
    }
    if (deletedAt != 0) {
        j["deletedAt"] = deletedAt;
    }
}

json ToJson(const Canvas& canvas) {
    json j{
        {"id", IdJson(canvas.id)},
        {"name", canvas.name},
    };
    WriteStamps(j, canvas.createdAt, canvas.deletedAt);
    return j;
}

bool FromJson(const json& j, Canvas& out) {
    if (!j.is_object()) {
        return false;
    }
    out.id = ReadId(j, "id");
    if (out.id == 0) {
        return false;
    }
    out.name = j.value("name", std::string());
    out.createdAt = j.value("createdAt", int64_t{0});
    out.deletedAt = j.value("deletedAt", int64_t{0});
    out.items.clear();
    if (const auto it = j.find("items"); it != j.end() && it->is_array()) {
        out.items.reserve(it->size());
        for (const auto& itemJson : *it) {
            Item item;
            if (FromJson(itemJson, item)) {
                out.items.push_back(std::move(item));
            }
        }
    }
    return true;
}

json ToJson(const Folder& folder) {
    json j{{"id", IdJson(folder.id)}, {"name", folder.name}};
    WriteStamps(j, folder.createdAt, folder.deletedAt);
    return j;
}

bool FromJson(const json& j, Folder& out) {
    if (!j.is_object()) {
        return false;
    }
    out.id = ReadId(j, "id");
    if (out.id == 0) {
        return false;
    }
    out.name = j.value("name", std::string());
    out.createdAt = j.value("createdAt", int64_t{0});
    out.deletedAt = j.value("deletedAt", int64_t{0});
    return true;
}

// Writes `content` to `path` via a temp-file-then-rename, so a reader
// (including this same process, if it crashes mid-write and restarts)
// never observes a partially-written file - rename() is atomic on the
// same filesystem on both Windows and Linux. Used for every file this
// store writes.
bool WriteFileAtomically(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::filesystem::path tmpPath = path;
    tmpPath += ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << content;
        out.flush();
        if (!out.good()) {
            return false;
        }
    }
    std::filesystem::rename(tmpPath, path, ec);
    return !ec;
}

// ===== The tree =====
//
// The directory structure is the library's structure, and the files in it
// describe themselves. See LibraryStore's own header comment for the layout
// and for why the filesystem, not an index, is what says where things are.

constexpr const char* kFoldersDir = "folders";
constexpr const char* kFolderFile = "folder.json";
constexpr const char* kCanvasFile = "canvas.json";
constexpr const char* kItemFile = "item.json";
constexpr const char* kOrderFile = "order.json";
// Where a freshly captured image lands before there is anywhere better to
// put it. A capture is written to disk the moment it is taken, which is
// deliberate (see SaveImage) and can happen before the item it belongs to
// has ever been saved - so it goes here, and the next Save moves it into
// the item's own directory. Nothing is meant to accumulate here.
constexpr const char* kStagingDir = "images";
// Where a save sets aside a directory the library no longer holds and
// nobody deleted for good (see Save's retirement pass) - whole, record and
// pictures together, at the same path it had under folders/. Nothing reads
// it back: it is there for a person to recover something from, where a
// delete would have left nothing to recover.
constexpr const char* kRetiredDir = "retired";

// ===== Reading a record without letting it throw =====
//
// Parsing with allow_exceptions=false protects the parse and nothing after
// it: nlohmann's value() throws on a field that is present with the wrong
// type - a currentCanvasId written as a string, say - and one such field in
// one file took the whole app down at startup, with nothing between Load
// and WinMain to catch it. A record this cannot read is treated exactly
// like one that is not JSON at all: skipped, and left where it is (see the
// header on what Save may retire).

template <typename T>
bool ReadRecord(const json& j, T& out) {
    try {
        return FromJson(j, out);
    } catch (const json::exception&) {
        return false;
    }
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<json> ReadJsonFile(const std::filesystem::path& path) {
    const std::optional<std::string> text = ReadFileText(path);
    if (!text) {
        return std::nullopt;
    }
    json doc = json::parse(*text, /*callback=*/nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        return std::nullopt;
    }
    return doc;
}

// ===== Links are not part of the tree =====
//
// A symlink or junction inside the library names something that may be
// anywhere on the disk. What is behind one is therefore never the store's:
// not read as a record, not written to, not swept, not retired, not
// deleted. Load skips linked directories (see SortedSubdirectories), so
// nothing behind a link ever enters the index, and every step that removes
// or moves something checks the whole path from the root down for a link
// (see CrossesLink) in case one appeared under a running instance. A link
// left where a save wants to put a directory is a name collision the save
// treats like any other: the record goes where its slug says and the link
// is left alone.

// Whether `path` itself is a symlink or junction - a junction being what
// "mklink /J" makes, the reparse point Windows lets a user create without
// privileges, and one that looks like a directory to everything that does
// not ask.
bool IsLink(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (std::filesystem::is_symlink(status)) {
        return true;
    }
#if defined(_MSC_VER)
    // file_type::junction is MSVC's own extension; other standard libraries
    // have no junctions to report.
    return status.type() == std::filesystem::file_type::junction;
#else
    return false;
#endif
}

// Whether any component of `path` from `root` down - `root` itself
// excluded, `path` itself included - is a link. Checked before anything
// under `path` is deleted or moved: a plain directory below a linked
// ancestor is physically somewhere else, whatever it is called here.
bool CrossesLink(const std::filesystem::path& root, const std::filesystem::path& path) {
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") {
        return true;  // not under the root at all - treated as not ours either
    }
    std::filesystem::path walked = root;
    for (const std::filesystem::path& component : relative) {
        if (component == ".") {
            continue;
        }
        walked /= component;
        if (IsLink(walked)) {
            return true;
        }
    }
    return false;
}

// Deletes `path` with everything in it - or, for a link, the link alone,
// whatever it points at. remove_all on a directory symlink already stops at
// the link; a junction is reported as its own kind and has to be asked
// about first, since a remove_all that walked into one would be emptying a
// directory outside the library.
bool RemoveTree(const std::filesystem::path& path) {
    std::error_code ec;
    if (IsLink(path)) {
        std::filesystem::remove(path, ec);
        return !ec;
    }
    std::filesystem::remove_all(path, ec);
    return !ec;
}

// Sorted, so that what a directory holds is walked in the same order twice
// running. Enumeration order is not specified by the filesystem, and an
// unlisted member's position (see ApplyOrder) would otherwise wander.
// Real directories only: a linked one is not part of the tree, see above.
std::vector<std::filesystem::path> SortedSubdirectories(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code isDirEc;
        if (entry.is_directory(isDirEc) && !IsLink(entry.path())) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// An order file lists the uids of the directories it expects to find beside
// it, in the order they should appear: {"canvases": ["cdrtsy", ...]}. The
// uid and nothing else, because the uid is what identifies a directory
// (its readable half follows the name and can be renamed by hand), and
// because it is the same spelling every record and pointer uses - see
// IdJson. Files written before this listed whole directory names; those
// are still read, matched on their trailing uid (see ApplyOrder).
//
// The file's exact text comes back beside the entries, for Load to compare
// against what a save would write - see the baseline it establishes.
struct OrderFile {
    std::vector<std::string> names;
    std::optional<std::string> text;
};

OrderFile ReadOrderFile(const std::filesystem::path& path, const char* key) {
    OrderFile file;
    file.text = ReadFileText(path);
    if (!file.text) {
        return file;
    }
    const json doc = json::parse(*file.text, /*callback=*/nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        return file;
    }
    if (const auto it = doc.find(key); it != doc.end() && it->is_array()) {
        for (const auto& entry : *it) {
            if (entry.is_string()) {
                file.names.push_back(entry.get<std::string>());
            }
        }
    }
    return file;
}

// The trailing "-<uid>" a slug carries, which is the part of a directory
// name that survives someone renaming the readable half of it by hand.
std::optional<uint64_t> UidFromDirectoryName(const std::string& name) {
    if (name.size() < kUidLength + 1) {
        return std::nullopt;
    }
    if (name[name.size() - kUidLength - 1] != '-') {
        return std::nullopt;
    }
    return ParseUid(std::string_view(name).substr(name.size() - kUidLength));
}

// Reorders `found` to match `order`, which is a list of uids - or, from an
// order file written before the lists held uids, of directory names.
//
// This is the whole reconciliation rule in one function, and it is
// deliberately forgiving in both directions: anything the order file names
// that is no longer there simply doesn't appear, and anything present that
// the order file doesn't name goes to the end. Neither is an error, because
// both are what a directory looks like after somebody moved something into
// or out of it while the app wasn't running - which is a thing this layout
// exists to allow.
//
// Matched on the uid - the entry itself, or the trailing uid of an old
// entry's directory name - so renaming the readable half of a directory by
// hand doesn't lose its place. The whole-name comparison is the last resort
// for an entry that carries no uid at all.
template <typename T>
void ApplyOrder(std::vector<std::pair<std::string, T>>& found, const std::vector<std::string>& order) {
    std::vector<std::pair<std::string, T>> sorted;
    sorted.reserve(found.size());
    std::vector<bool> placed(found.size(), false);
    for (const std::string& wanted : order) {
        const std::optional<uint64_t> wantedUid =
            wanted.size() == kUidLength ? ParseUid(wanted) : UidFromDirectoryName(wanted);
        for (size_t i = 0; i < found.size(); ++i) {
            if (placed[i]) {
                continue;
            }
            const std::optional<uint64_t> foundUid = UidFromDirectoryName(found[i].first);
            const bool matches = (wantedUid && foundUid) ? (*wantedUid == *foundUid) : (found[i].first == wanted);
            if (matches) {
                sorted.push_back(std::move(found[i]));
                placed[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < found.size(); ++i) {
        if (!placed[i]) {
            sorted.push_back(std::move(found[i]));
        }
    }
    found = std::move(sorted);
}

json OrderFileJson(const char* key, const std::vector<std::string>& names) {
    json list = json::array();
    for (const std::string& name : names) {
        list.push_back(name);
    }
    return json{{key, std::move(list)}};
}

// What library.json holds: the things with no other home. One function for
// both directions, so that what Load compares against is exactly what Save
// writes.
json LibraryJson(const CanvasManagerSnapshot& snapshot) {
    json doc;
    doc["version"] = kLibraryFormatVersion;
    doc["currentFolderId"] = IdJson(snapshot.currentFolderId);
    doc["currentCanvasId"] = IdJson(snapshot.currentCanvasId);
    return doc;
}

// Moves everything inside `source` into `target`, which already exists,
// and removes `source`. A directory that exists on both sides is merged
// the same way; a file on both sides is the source's, which is the newer.
// What setting a directory aside needs when its place in retired/ is
// already taken - by a snippet set aside before its canvas was, say, or by
// an earlier retirement of the same uid.
bool MergeDirectoryInto(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code ec;
    bool complete = true;
    for (const auto& entry : std::filesystem::directory_iterator(source, ec)) {
        const std::filesystem::path to = target / entry.path().filename();
        std::error_code kindEc;
        // A link on either side is a leaf: one in the source is moved as a
        // link, and one in the target is replaced as a link. Recursing into
        // either would be moving files into, or out of, wherever it points.
        const bool bothRealDirectories = entry.is_directory(kindEc) && !IsLink(entry.path()) &&
                                         std::filesystem::is_directory(to, kindEc) && !IsLink(to);
        if (bothRealDirectories) {
            complete = MergeDirectoryInto(entry.path(), to) && complete;
            continue;
        }
        if (std::filesystem::exists(std::filesystem::symlink_status(to, kindEc))) {
            RemoveTree(to);
        }
        std::filesystem::rename(entry.path(), to, kindEc);
        complete = complete && !kindEc;
    }
    if (complete) {
        std::filesystem::remove(source, ec);
    }
    return complete;
}

// Moves every indexed path under `oldRoot` to the same place under
// `newRoot` - what a folder or canvas directory carries with it when it is
// renamed. The index entries of everything inside it would otherwise still
// name the old location, and the next placement would find nothing there,
// rebuild every snippet from memory, and lose every picture on the way.
void Rehome(std::map<uint64_t, std::filesystem::path>& index, const std::filesystem::path& oldRoot,
            const std::filesystem::path& newRoot) {
    for (auto& [id, path] : index) {
        (void)id;
        const std::filesystem::path relative = path.lexically_relative(oldRoot);
        if (relative.empty() || relative == "." || *relative.begin() == "..") {
            continue;
        }
        path = newRoot / relative;
    }
}

}  // namespace

LibraryStore::LibraryStore(std::filesystem::path rootDir) : rootDir_(std::move(rootDir)) {}

std::filesystem::path LibraryStore::FoldersRoot() const { return rootDir_ / kFoldersDir; }

bool LibraryStore::WithinRoot(const std::filesystem::path& path) const {
    std::error_code ec;
    // Lexical, on the absolute forms: the question is what the path *names*,
    // not what it resolves to - a link inside the root that points outside
    // still names something inside, and is handled as a link below.
    const std::filesystem::path root = std::filesystem::absolute(rootDir_, ec).lexically_normal();
    const std::filesystem::path candidate = std::filesystem::absolute(path, ec).lexically_normal();
    const std::filesystem::path relative = candidate.lexically_relative(root);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

bool LibraryStore::RemoveOwnDirectory(const std::filesystem::path& path) const {
    // Nothing in the index is a link or below one - Load never indexes a
    // linked directory - so meeting one here means the tree was rearranged
    // under a running instance, and the directory is physically somewhere
    // this store has never read. Left alone.
    if (CrossesLink(rootDir_, path)) {
        return false;
    }
    RemoveTree(path);
    // Whether it is gone is the answer, not whether remove_all complained:
    // a recursive delete that met one file it could not remove - held open
    // without delete sharing by another process, on Windows - has removed
    // everything else and left the directory standing around that file.
    std::error_code ec;
    return !std::filesystem::exists(std::filesystem::symlink_status(path, ec));
}

void LibraryStore::ForgetUnder(const std::filesystem::path& dir) const {
    for (auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
        for (auto it = index->begin(); it != index->end();) {
            const std::filesystem::path relative = it->second.lexically_relative(dir);
            const bool under = !relative.empty() && *relative.begin() != "..";
            it = under ? index->erase(it) : std::next(it);
        }
    }
}

bool LibraryStore::Remove(uint64_t uid) const {
    std::filesystem::path dir;
    for (const auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
        if (const auto it = index->find(uid); it != index->end()) {
            dir = it->second;
            break;
        }
    }
    // Every path in the index was made under the root, so one that isn't is
    // a bug - and a bug here leaves the directory alone.
    if (dir.empty() || !WithinRoot(dir)) {
        return false;
    }
    ++writeGeneration_;
    if (!RemoveOwnDirectory(dir)) {
        // Still there, whole or in part. Kept indexed and marked, so that
        // the next save takes another run at removing it rather than
        // taking it for something gone missing and setting it aside - and
        // so that the caller can say the files are still there.
        pendingRemovals_.insert(uid);
        return false;
    }
    pendingRemovals_.erase(uid);
    ForgetUnder(dir);
    return true;
}

// Where a snippet's pictures are written: its own directory once it has one,
// staging until then. A capture is written the moment it is taken, before
// the item it belongs to has ever been saved, so there has to be somewhere
// for it to wait - and the next Save moves it home (see Save's staging pass).
std::filesystem::path LibraryStore::ImageHome(uint64_t itemId) const {
    const auto it = itemDirs_.find(itemId);
    return it != itemDirs_.end() ? it->second : rootDir_ / kStagingDir;
}

// Where a snippet's picture is *read* from: its own directory if the file is
// there, staging otherwise. The fallback covers the window between a capture
// and the save that moves it, and a move that failed - the picture is still
// readable from where it is, which is the whole of what matters.
//
// Keyed by the owning snippet, not by the filename alone. A library-wide
// name-to-directory map was tried and was wrong in two ways at once: it fell
// behind whenever a directory moved (rename a snippet and its picture could
// no longer be found, though it was right there), and two snippets can
// legitimately hold files of the same name, since copying a directory in a
// file manager is a supported way to duplicate one.
std::filesystem::path LibraryStore::FindImage(uint64_t itemId, const std::string& filename) const {
    std::error_code ec;
    if (!IsPlainFilename(filename)) {
        // Somewhere that cannot exist, inside the library, rather than
        // wherever the name was trying to point - see IsPlainFilename.
        return rootDir_ / kStagingDir / "not-a-filename";
    }
    if (const auto it = itemDirs_.find(itemId); it != itemDirs_.end()) {
        const std::filesystem::path inHome = it->second / filename;
        if (std::filesystem::exists(inHome, ec)) {
            return inHome;
        }
    }
    return rootDir_ / kStagingDir / filename;
}

void LibraryStore::ReadTree(const std::filesystem::path& foldersRoot, CanvasManagerSnapshot& out) const {
    // Everything below comes from the tree, not from library.json: the
    // directories are what say which folders and canvases exist and where
    // they live. library.json holds only what has no other home.
    std::unordered_set<uint64_t> seenIds;
    // A duplicate id is not corruption - it is what copying a directory in
    // a file manager produces, which this layout invites. The copy gets a
    // fresh id and is thereafter simply another canvas.
    const auto claimId = [&seenIds](uint64_t id) {
        if (id != 0 && seenIds.insert(id).second) {
            return id;
        }
        const uint64_t fresh = MakeUid([&seenIds](uint64_t candidate) { return seenIds.count(candidate) > 0; });
        seenIds.insert(fresh);
        return fresh;
    };

    std::vector<std::pair<std::string, Folder>> foundFolders;
    for (const std::filesystem::path& folderDir : SortedSubdirectories(foldersRoot)) {
        const std::optional<json> folderDoc = ReadJsonFile(folderDir / kFolderFile);
        if (!folderDoc) {
            continue;  // not a folder of ours; left alone rather than guessed at
        }
        Folder folder;
        if (!ReadRecord(*folderDoc, folder)) {
            continue;
        }
        const uint64_t recordedFolderId = folder.id;
        folder.id = claimId(folder.id);
        // Noted on the way past, so a later save knows where everything is
        // without going back to disk to find out - see folderDirs_.
        folderDirs_[folder.id] = folderDir;
        // ...and, if the record is exactly what a save would write, that it
        // needn't be. Compared as JSON rather than as text, so a hand-edit
        // that only changed the formatting is not a reason to rewrite it.
        if (folder.id == recordedFolderId && ToJson(folder) == *folderDoc) {
            writtenFileText_["folder:" + std::to_string(folder.id)] = ToJson(folder).dump(2);
        }
        foundFolders.emplace_back(folderDir.filename().string(), std::move(folder));
    }
    // An order file that agrees with the directories beside it, in the
    // order they came out, is one a save would write back unchanged.
    const auto noteOrderFile = [this](const std::string& key, const OrderFile& file, const char* listKey,
                                      const std::vector<std::string>& names) {
        const std::string wouldWrite = OrderFileJson(listKey, names).dump(2);
        if (file.text && *file.text == wouldWrite) {
            writtenFileText_[key] = wouldWrite;
        }
    };
    const auto namesOf = [](const auto& found) {
        std::vector<std::string> names;
        names.reserve(found.size());
        for (const auto& [dirName, record] : found) {
            (void)dirName;
            names.push_back(FormatUid(record.id));
        }
        return names;
    };
    const OrderFile folderOrderFile = ReadOrderFile(foldersRoot / kOrderFile, "folders");
    ApplyOrder(foundFolders, folderOrderFile.names);
    noteOrderFile("order:root", folderOrderFile, "folders", namesOf(foundFolders));

    for (auto& [folderDirName, folder] : foundFolders) {
        const std::filesystem::path folderDir = foldersRoot / folderDirName;
        std::vector<std::pair<std::string, Canvas>> foundCanvases;
        for (const std::filesystem::path& canvasDir : SortedSubdirectories(folderDir)) {
            const std::optional<json> canvasDoc = ReadJsonFile(canvasDir / kCanvasFile);
            if (!canvasDoc) {
                continue;
            }
            Canvas canvas;
            if (!ReadRecord(*canvasDoc, canvas)) {
                continue;
            }
            const uint64_t recordedCanvasId = canvas.id;
            canvas.id = claimId(canvas.id);
            // Where it *is* beats what it says it belongs to. That is the
            // whole point of the tree: drag a canvas directory into another
            // folder while the app is closed and it is in that folder, with
            // no record anywhere left saying otherwise.
            canvas.folderId = folder.id;
            canvasDirs_[canvas.id] = canvasDir;
            // A record whose folderId disagreed with where it sits is not
            // noted here, and is thereby rewritten to agree.
            if (canvas.id == recordedCanvasId && ToJson(canvas) == *canvasDoc) {
                writtenFileText_["canvas:" + std::to_string(canvas.id)] = ToJson(canvas).dump(2);
            }

            // ...and the same, one level down, for the snippets inside it.
            std::vector<std::pair<std::string, Item>> foundItems;
            for (const std::filesystem::path& itemDir : SortedSubdirectories(canvasDir)) {
                const std::optional<json> itemDoc = ReadJsonFile(itemDir / kItemFile);
                if (!itemDoc) {
                    continue;
                }
                Item item;
                if (!ReadRecord(*itemDoc, item)) {
                    continue;
                }
                const uint64_t recordedItemId = item.id;
                item.id = claimId(item.id);
                // Every file beside the record belongs to it, whatever the
                // record calls them - which is what lets a snippet be moved
                // by moving one directory. Knowing where the directory is
                // is therefore the whole of image resolution (see
                // FindImage); nothing inside it needs listing.
                itemDirs_[item.id] = itemDir;
                // The hash of what was read, not a comparison of the text:
                // the record is where the bulk is, and serialising it to
                // compare would cost the load half a save. What FromJson
                // normalises on the way in reads back the same next time,
                // so it is no reason to write; a record in the shape from
                // before layers existed is, and is left unnoted so the next
                // save brings it into the current one.
                if (item.id == recordedItemId && itemDoc->contains("layers")) {
                    writtenItemHashes_[item.id] = HashItem(item);
                }
                foundItems.emplace_back(itemDir.filename().string(), std::move(item));
            }
            // Item order is z-order, back to front - so a snippet dropped in
            // by hand, which the order file cannot know about, arrives in
            // front rather than buried.
            const OrderFile itemOrderFile = ReadOrderFile(canvasDir / kOrderFile, "items");
            ApplyOrder(foundItems, itemOrderFile.names);
            noteOrderFile("order:canvas:" + std::to_string(canvas.id), itemOrderFile, "items", namesOf(foundItems));
            canvas.items.clear();
            canvas.items.reserve(foundItems.size());
            for (auto& [itemDirName, item] : foundItems) {
                canvas.items.push_back(std::move(item));
            }

            foundCanvases.emplace_back(canvasDir.filename().string(), std::move(canvas));
        }
        const OrderFile canvasOrderFile = ReadOrderFile(folderDir / kOrderFile, "canvases");
        ApplyOrder(foundCanvases, canvasOrderFile.names);
        noteOrderFile("order:folder:" + std::to_string(folder.id), canvasOrderFile, "canvases",
                      namesOf(foundCanvases));
        for (auto& [canvasDirName, canvas] : foundCanvases) {
            out.canvases.push_back(std::move(canvas));
        }
        out.folders.push_back(std::move(folder));
    }
}

std::optional<CanvasManagerSnapshot> LibraryStore::Load() const {
    // The tree is the library, and library.json is a pointer file beside
    // it. That ordering decides everything below: whatever is wrong with
    // library.json - missing, not JSON, a shape from before the tree - is a
    // reason to default what it holds, and never a reason to report the
    // library absent while a tree is sitting there. Reporting it absent
    // starts the app fresh, and a fresh library's first save would then
    // retire every directory it found on disk as something the library no
    // longer holds - which is exactly how a whole tree once went missing.
    const std::filesystem::path foldersRoot = FoldersRoot();
    std::error_code ec;
    const bool treeExists = std::filesystem::is_directory(foldersRoot, ec);

    // Read and closed before anything below: the old-shape guard renames
    // this file, and Windows refuses to rename a file something holds open.
    json doc = json::object();
    const std::optional<std::string> text = ReadFileText(rootDir_ / "library.json");
    if (!text && !treeExists) {
        return std::nullopt;  // nothing here at all: a first run
    }
    if (text) {
        doc = json::parse(*text, /*callback=*/nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object()) {
            if (!treeExists) {
                return std::nullopt;
            }
            doc = json::object();  // a broken pointer file costs its pointers, not the tree
        }
    }

    // The single file that held everything before the tree did. This reader
    // takes folders and canvases from directories only, so the old shape
    // would read as no pointers at all; it is set aside under a name nothing
    // writes to, for the record, and the tree beside it - if there is one -
    // is read as usual. It was never shipped, so there is no migration:
    // without a tree the library starts fresh beside the file.
    if (const auto canvases = doc.find("canvases");
        canvases != doc.end() && canvases->is_array() && !canvases->empty()) {
        std::filesystem::rename(rootDir_ / "library.json", rootDir_ / "library.json.v0", ec);
        ++writeGeneration_;
        if (!treeExists) {
            return std::nullopt;
        }
        doc = json::object();
    }

    // A load replaces everything this store believed about the tree. The walk
    // below refills the index, and re-establishes the write-state record by
    // record: each one that came back exactly as a save would write it is
    // noted as already written, so the first save afterwards costs what
    // changed, like every save after it. Anything the load had to repair -
    // an id reassigned, a folderId corrected, an order file that disagreed
    // with the directories beside it, a record in an older shape - is left
    // out, and so is written. See writtenItemHashes_.
    folderDirs_.clear();
    canvasDirs_.clear();
    itemDirs_.clear();
    writtenItemHashes_.clear();
    writtenFileText_.clear();
    treeIndexed_ = true;

    CanvasManagerSnapshot snapshot;
    // Defaults for a field that is missing *or* unreadable: everything in
    // this file is a pointer or a preference, repaired or defaulted below
    // anyway, so a bad one costs a setting rather than the library.
    snapshot.currentFolderId = ReadId(doc, "currentFolderId");
    snapshot.currentCanvasId = ReadId(doc, "currentCanvasId");
    // Anything else in the file is ignored and left out by the next save.

    ReadTree(foldersRoot, snapshot);

    // Dangling now means "repair", not "refuse". The old single file could
    // only be right or corrupt; a tree that people are invited to rearrange
    // is routinely a little out of date, and refusing to open a library
    // because the canvas that was current has been moved out of it would be
    // the worst possible answer to a supported gesture.
    const auto folderExists = [&snapshot](FolderId id) {
        return std::any_of(snapshot.folders.begin(), snapshot.folders.end(),
                            [id](const Folder& f) { return f.id == id; });
    };
    const auto canvasExists = [&snapshot](CanvasId id) {
        return std::any_of(snapshot.canvases.begin(), snapshot.canvases.end(),
                            [id](const Canvas& c) { return c.id == id; });
    };
    if (!snapshot.folders.empty() && !folderExists(snapshot.currentFolderId)) {
        snapshot.currentFolderId = snapshot.folders.front().id;
    }
    if (!snapshot.canvases.empty() && !canvasExists(snapshot.currentCanvasId)) {
        snapshot.currentCanvasId = snapshot.canvases.front().id;
    }
    if (snapshot.folders.empty()) {
        snapshot.currentFolderId = 0;
    }
    if (snapshot.canvases.empty()) {
        snapshot.currentCanvasId = 0;
    }

    // And library.json itself, after the repairs above: one that named
    // nothing is not noted, and is rewritten naming something.
    if (LibraryJson(snapshot) == doc) {
        writtenFileText_["library"] = LibraryJson(snapshot).dump(2);
    }

    return snapshot;
}

void LibraryStore::IndexTreeFromDisk(const std::filesystem::path& foldersRoot) const {
    folderDirs_.clear();
    canvasDirs_.clear();
    itemDirs_.clear();
    for (const std::filesystem::path& folderDir : SortedSubdirectories(foldersRoot)) {
        if (const std::optional<json> doc = ReadJsonFile(folderDir / kFolderFile)) {
            if (const uint64_t id = ReadId(*doc, "id"); id != 0) {
                folderDirs_.emplace(id, folderDir);
            }
        }
        for (const std::filesystem::path& canvasDir : SortedSubdirectories(folderDir)) {
            if (const std::optional<json> doc = ReadJsonFile(canvasDir / kCanvasFile)) {
                if (const uint64_t id = ReadId(*doc, "id"); id != 0) {
                    canvasDirs_.emplace(id, canvasDir);
                }
            }
            // Item directories too, and across every canvas rather than
            // only this one: moving a snippet to another canvas is a move
            // of its directory, and finding where it currently is is what
            // makes that a move rather than a copy plus an orphan.
            for (const std::filesystem::path& itemDir : SortedSubdirectories(canvasDir)) {
                if (const std::optional<json> doc = ReadJsonFile(itemDir / kItemFile)) {
                    if (const uint64_t id = ReadId(*doc, "id"); id != 0) {
                        itemDirs_.emplace(id, itemDir);
                    }
                }
            }
        }
    }
    treeIndexed_ = true;
}

// A save is a plan in three parts, and the order of the parts is the whole
// of its safety: first every folder, canvas and snippet the library holds is
// *placed* - its directory found, or moved to where its current name says,
// or created - and its record written; then, and only then, whatever the
// index knows about that the library no longer holds is retired; and last
// the pictures waiting in staging are moved in with their snippets.
//
// Placing everything before deleting anything is what makes a move a move.
// A snippet dragged from canvas A to canvas B is, on disk, A's directory
// losing a subdirectory and B's gaining one, and the version of this that
// swept each canvas for strays as it went deleted the snippet out of A
// before reaching B - record, picture and all - and then rebuilt the record
// from memory, which is the one part that could be rebuilt.
//
// Retiring only what the index knows is what makes an unfamiliar directory
// safe. A directory with no record, or with a record that cannot be read,
// is skipped by Load and never enters the index, so it cannot be retired
// here; unreadable must not become deleted. Deletion is a statement about
// something this store once read, and nothing else.
bool LibraryStore::Save(const CanvasManagerSnapshot& snapshot) const {
    std::error_code ec;
    const std::filesystem::path foldersRoot = FoldersRoot();
    std::filesystem::create_directories(foldersRoot, ec);

    // What the library holds, by id - what the retirement pass below is
    // measured against, and what everything remembered is pruned to.
    std::unordered_set<uint64_t> liveFolderIds;
    std::unordered_set<uint64_t> liveCanvasIds;
    std::unordered_set<uint64_t> liveItemIds;
    // ...and by picture: which snippet each live picture (and its thumbnail)
    // belongs to, for the staging pass.
    std::unordered_map<std::string, uint64_t> pictureOwner;
    for (const Folder& folder : snapshot.folders) {
        liveFolderIds.insert(folder.id);
    }
    for (const Canvas& canvas : snapshot.canvases) {
        liveCanvasIds.insert(canvas.id);
        for (const Item& item : canvas.items) {
            liveItemIds.insert(item.id);
            for (const Layer& layer : item.layers) {
                if (!layer.imageFile.empty()) {
                    pictureOwner[layer.imageFile] = item.id;
                    pictureOwner[ThumbnailFilename(layer.imageFile)] = item.id;
                }
            }
        }
    }

    // Only when there is nothing to trust. This is the read-and-parse-
    // everything pass every save used to begin with; Load fills the same
    // index for free on the way in, and everything below keeps it current.
    //
    // A store that has never loaded adopts only the directories of what it
    // is about to save - so a snippet that already has a directory is
    // written into it rather than beside it - and forgets the rest. It has
    // not read them, so they are not its to retire: retirement is a
    // statement about something this store once read, and a fresh store
    // saving a fresh library over a tree it never looked at must leave that
    // tree exactly as it found it. The next Load reads both.
    if (!treeIndexed_) {
        IndexTreeFromDisk(foldersRoot);
        const auto adoptOnly = [](std::map<uint64_t, std::filesystem::path>& index,
                                  const std::unordered_set<uint64_t>& live) {
            for (auto it = index.begin(); it != index.end();) {
                it = live.count(it->first) > 0 ? std::next(it) : index.erase(it);
            }
        };
        adoptOnly(folderDirs_, liveFolderIds);
        adoptOnly(canvasDirs_, liveCanvasIds);
        adoptOnly(itemDirs_, liveItemIds);
    }

    // Writes only what differs from what this store last wrote there. `force`
    // is for a record whose directory had to be created fresh: the file is
    // not there at all then, however familiar its contents look.
    const auto writeIfChanged = [this](const std::string& key, const std::filesystem::path& path,
                                        std::string text, bool force) {
        if (!force) {
            const auto it = writtenFileText_.find(key);
            if (it != writtenFileText_.end() && it->second == text) {
                return true;
            }
        }
        if (!WriteFileAtomically(path, text)) {
            // Not recorded, so the next save tries again rather than
            // believing a file it never managed to write.
            writtenFileText_.erase(key);
            return false;
        }
        writtenFileText_[key] = std::move(text);
        return true;
    };

    // Every write this save is obliged to make lands in here. Best-effort
    // work - moving a picture, retiring a directory - does not, since none
    // of it loses anything if it waits for the next save; see the header.
    bool wroteEverything =
        writeIfChanged("library", rootDir_ / "library.json", LibraryJson(snapshot).dump(2), /*force=*/false);

    // The directory name is regenerated from the *current* name every time,
    // which is what keeps it worth reading. Identity is the trailing uid, so
    // a failed rename costs a stale label and nothing else - literally: the
    // record keeps living where it is, and the label is retried next save.
    //
    // `dir` is where the record's directory actually is now - `wanted`, or
    // the old place if the move lost - and `kept` is whether what was in it
    // is still there. A fresh empty directory is not kept, and everything
    // written into it has to be written whatever a hash says.
    //
    // `usable` is false when a link sits where the directory would have to
    // be created: what is behind a link is not the store's to write into
    // (see IsLink), so the record - and everything under it - is skipped,
    // the save reports failure, and the next one tries again.
    struct Placed {
        std::filesystem::path dir;
        bool kept;
        bool usable = true;
    };
    const auto placeDirectory = [&ec](std::map<uint64_t, std::filesystem::path>& index, uint64_t id,
                                       const std::filesystem::path& wanted,
                                       std::initializer_list<std::map<uint64_t, std::filesystem::path>*> inside) {
        const auto it = index.find(id);
        if (it != index.end() && it->second == wanted) {
            // Already exactly where it belongs, which is the overwhelmingly
            // common case. Returning here rather than falling through to
            // create_directories matters: that call is a trip to the
            // filesystem for an answer we have, and one per folder, canvas
            // and snippet adds up to most of what a no-op save costs.
            return Placed{wanted, true};
        }
        if (it != index.end()) {
            const std::filesystem::path old = it->second;
            std::filesystem::create_directories(wanted.parent_path(), ec);
            ec.clear();
            std::filesystem::rename(old, wanted, ec);
            if (!ec) {
                // Everything indexed inside it moved with it.
                for (std::map<uint64_t, std::filesystem::path>* nested : inside) {
                    Rehome(*nested, old, wanted);
                }
                it->second = wanted;
                return Placed{wanted, true};
            }
            ec.clear();
            if (std::filesystem::exists(old, ec)) {
                return Placed{old, true};
            }
            // Gone from where the index said - rearranged under a running
            // instance, which is undefined but shouldn't lose what is in
            // memory. Rebuilt from that, below.
        }
        ec.clear();
        if (IsLink(wanted)) {
            return Placed{wanted, false, /*usable=*/false};  // not indexed: never ours
        }
        std::filesystem::create_directories(wanted, ec);
        index[id] = wanted;
        return Placed{wanted, false};
    };

    // ===== 1. Place and write everything the library holds =====

    std::vector<std::string> folderOrder;
    folderOrder.reserve(snapshot.folders.size());
    for (const Folder& folder : snapshot.folders) {
        const Placed placedFolder =
            placeDirectory(folderDirs_, folder.id, foldersRoot / MakeSlug(folder.name, folder.id),
                           {&canvasDirs_, &itemDirs_});
        const std::filesystem::path& folderDir = placedFolder.dir;
        folderOrder.push_back(FormatUid(folder.id));
        if (!placedFolder.usable) {
            wroteEverything = false;
            continue;
        }
        wroteEverything &= writeIfChanged("folder:" + std::to_string(folder.id), folderDir / kFolderFile,
                                           ToJson(folder).dump(2), !placedFolder.kept);

        std::vector<std::string> canvasOrder;
        for (const Canvas& canvas : snapshot.canvases) {
            if (canvas.folderId != folder.id) {
                continue;
            }
            const Placed placedCanvas = placeDirectory(
                canvasDirs_, canvas.id, folderDir / MakeSlug(canvas.name, canvas.id), {&itemDirs_});
            const std::filesystem::path& canvasDir = placedCanvas.dir;
            canvasOrder.push_back(FormatUid(canvas.id));
            if (!placedCanvas.usable) {
                wroteEverything = false;
                continue;
            }
            wroteEverything &= writeIfChanged("canvas:" + std::to_string(canvas.id), canvasDir / kCanvasFile,
                                               ToJson(canvas).dump(2), !placedCanvas.kept);

            std::vector<std::string> itemOrder;
            for (const Item& item : canvas.items) {
                const Placed placedItem =
                    placeDirectory(itemDirs_, item.id, canvasDir / MakeSlug(item.name, item.id), {});
                const std::filesystem::path& itemDir = placedItem.dir;
                itemOrder.push_back(FormatUid(item.id));
                if (!placedItem.usable) {
                    wroteEverything = false;
                    continue;
                }

                // The one place where skipping the work is worth real time,
                // and the only one that answers "changed?" without
                // serialising - see HashItem. Everything else in this
                // function is a handful of scalars.
                const uint64_t hash = HashItem(item);
                const auto known = writtenItemHashes_.find(item.id);
                if (placedItem.kept && known != writtenItemHashes_.end() && known->second == hash) {
                    continue;
                }
                const bool recordWritten = WriteFileAtomically(itemDir / kItemFile, ToJson(item).dump(2));
                if (recordWritten) {
                    writtenItemHashes_[item.id] = hash;
                } else {
                    writtenItemHashes_.erase(item.id);
                    wroteEverything = false;
                }

                // Any picture in the directory its layers no longer name -
                // a painted layer that moved in the stack is written under
                // a new name, and this is what collects the old one. Only
                // on a changed record, because that is the only time a file
                // can have stopped being named; a no-op save lists nothing.
                //
                // Only once the record that stopped naming it is on disk.
                // Until then the record on disk is the old one, and the
                // pictures it names are what the library reloads to if this
                // process never manages to write the new one - collecting
                // them on the strength of a record that failed to land would
                // leave the old record pointing at nothing.
                if (!placedItem.kept || !recordWritten || CrossesLink(rootDir_, itemDir)) {
                    continue;  // a fresh directory holds nothing to collect
                }
                std::unordered_set<std::string> named;
                for (const Layer& layer : item.layers) {
                    if (!layer.imageFile.empty()) {
                        named.insert(layer.imageFile);
                        named.insert(ThumbnailFilename(layer.imageFile));
                    }
                }
                for (const auto& entry : std::filesystem::directory_iterator(itemDir, ec)) {
                    std::error_code isFileEc;
                    if (!entry.is_regular_file(isFileEc)) {
                        continue;
                    }
                    const std::string name = entry.path().filename().string();
                    // Only what this store writes: its own pictures and
                    // their thumbnails. Anything else beside the record - a
                    // note someone dropped in by hand, say - is not the
                    // store's to collect, whatever it is called.
                    if (name == kItemFile || named.count(name) > 0 || !IsPictureFilename(name)) {
                        continue;
                    }
                    std::filesystem::remove(entry.path(), ec);
                }
            }
            wroteEverything &= writeIfChanged("order:canvas:" + std::to_string(canvas.id), canvasDir / kOrderFile,
                                               OrderFileJson("items", itemOrder).dump(2), !placedCanvas.kept);
        }
        wroteEverything &= writeIfChanged("order:folder:" + std::to_string(folder.id), folderDir / kOrderFile,
                                           OrderFileJson("canvases", canvasOrder).dump(2), !placedFolder.kept);
    }
    wroteEverything &= writeIfChanged("order:root", foldersRoot / kOrderFile,
                                       OrderFileJson("folders", folderOrder).dump(2), /*force=*/false);

    // ===== 2. Set aside what the index knows and the library no longer holds =====
    //
    // A delete is a mark that leaves its record in the library, and deleting
    // for good removes the directory at the moment it is asked for (see
    // Remove), so by the time a save runs the index and the snapshot agree
    // about everything that went on purpose, and this pass finds nothing. It
    // is the net under a snapshot that lacks something nobody deleted - the
    // kind of disagreement that once emptied a library - and what it finds
    // is moved into retired/, whole, at the path it had under folders/,
    // rather than deleted. Nothing reads retired/ back, and nothing is lost
    // to it either.
    //
    // Folders first, then canvases, then snippets, so a canvas goes whole,
    // its snippets still inside it, and each move takes its contents' index
    // entries with it. Over a copy of the ids, since every move edits the
    // index it came from.
    {
        // First, another run at whatever a permanent delete could not remove
        // (see Remove). What still cannot go stays indexed and marked, and
        // nothing under it is retired below: it was deleted, not lost.
        std::vector<std::filesystem::path> stillPending;
        for (auto pending = pendingRemovals_.begin(); pending != pendingRemovals_.end();) {
            std::filesystem::path dir;
            for (const auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
                if (const auto it = index->find(*pending); it != index->end()) {
                    dir = it->second;
                }
            }
            if (dir.empty()) {
                pending = pendingRemovals_.erase(pending);  // nothing indexed to remove any more
                continue;
            }
            if (RemoveOwnDirectory(dir)) {
                ForgetUnder(dir);
                pending = pendingRemovals_.erase(pending);
                continue;
            }
            stillPending.push_back(dir);
            ++pending;
        }
        const auto underPendingRemoval = [&stillPending](const std::filesystem::path& path) {
            for (const std::filesystem::path& dir : stillPending) {
                const std::filesystem::path relative = path.lexically_relative(dir);
                if (!relative.empty() && *relative.begin() != "..") {
                    return true;
                }
            }
            return false;
        };

        const std::filesystem::path retiredRoot = rootDir_ / kRetiredDir;
        const auto retire = [&](const std::map<uint64_t, std::filesystem::path>& index,
                                const std::unordered_set<uint64_t>& live) {
            std::vector<uint64_t> gone;
            for (const auto& [id, path] : index) {
                (void)path;
                if (live.count(id) == 0) {
                    gone.push_back(id);
                }
            }
            for (const uint64_t id : gone) {
                const auto it = index.find(id);
                if (it == index.end()) {
                    continue;  // went with a parent retired just before it
                }
                // Should never be false, checked anyway: every path in the
                // index was made under the root, so one that isn't is a
                // bug, and a bug here leaves the directory alone.
                const std::filesystem::path path = it->second;
                if (!WithinRoot(path) || CrossesLink(rootDir_, path) || underPendingRemoval(path)) {
                    continue;
                }
                const std::filesystem::path destination = retiredRoot / path.lexically_relative(foldersRoot);
                std::error_code moveEc;
                std::filesystem::create_directories(destination.parent_path(), moveEc);
                bool moved = false;
                if (std::filesystem::exists(destination, moveEc)) {
                    moved = MergeDirectoryInto(path, destination);
                } else {
                    moveEc.clear();
                    std::filesystem::rename(path, destination, moveEc);
                    moved = !moveEc;
                }
                // A move that failed leaves the directory where it is, and
                // indexed, for the next save to take another run at.
                if (moved) {
                    ForgetUnder(path);
                }
            }
        };
        retire(folderDirs_, liveFolderIds);
        retire(canvasDirs_, liveCanvasIds);
        retire(itemDirs_, liveItemIds);
    }

    // Everything remembered about what a record *said* goes with a retired
    // record, though the directory itself stays indexed: a snippet undo
    // brings back has its record rewritten once, which is cheap and cannot
    // be wrong, where a remembered hash for an id that comes back would say
    // "unchanged" about a file that has moved. Forgetting is always safe;
    // remembering too long is not.
    for (auto it = writtenItemHashes_.begin(); it != writtenItemHashes_.end();) {
        it = liveItemIds.count(it->first) > 0 ? std::next(it) : writtenItemHashes_.erase(it);
    }
    for (auto it = writtenFileText_.begin(); it != writtenFileText_.end();) {
        const std::string& key = it->first;
        bool keep = key == "library" || key == "order:root";
        const auto idAfter = [&key](const char* prefix) -> std::optional<uint64_t> {
            const std::string_view view(key);
            if (view.rfind(prefix, 0) != 0) {
                return std::nullopt;
            }
            return std::strtoull(key.c_str() + std::strlen(prefix), nullptr, 10);
        };
        if (const std::optional<uint64_t> folderId = idAfter("folder:")) {
            keep = liveFolderIds.count(*folderId) > 0;
        } else if (const std::optional<uint64_t> canvasId = idAfter("canvas:")) {
            keep = liveCanvasIds.count(*canvasId) > 0;
        } else if (const std::optional<uint64_t> orderFolderId = idAfter("order:folder:")) {
            keep = liveFolderIds.count(*orderFolderId) > 0;
        } else if (const std::optional<uint64_t> orderCanvasId = idAfter("order:canvas:")) {
            keep = liveCanvasIds.count(*orderCanvasId) > 0;
        }
        it = keep ? std::next(it) : writtenFileText_.erase(it);
    }

    // ===== 3. Bring the pictures waiting in staging home =====
    //
    // Whatever a snippet in the library names is moved in with it - every
    // one has a directory by now, a deleted one included, since a delete
    // leaves it where it is. Whatever nothing names is set aside into
    // retired/images/ rather than deleted: it is the capture of a snippet
    // deleted for good before it was ever saved, which nobody wants back -
    // or the remains of a crash between writing a picture and saving the
    // record that names it, which is exactly the screenshot that cannot be
    // taken again, and the two cannot be told apart from here. A picture
    // is written the moment it is taken so that it survives such a crash;
    // deleting it at the next start would undo that promise. A move that
    // fails stays, readably - see FindImage.
    const std::filesystem::path stagingDir = rootDir_ / kStagingDir;
    if (std::filesystem::exists(stagingDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(stagingDir, ec)) {
            std::error_code isFileEc;
            if (!entry.is_regular_file(isFileEc)) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (const auto owner = pictureOwner.find(name); owner != pictureOwner.end()) {
                if (const auto home = itemDirs_.find(owner->second); home != itemDirs_.end()) {
                    std::filesystem::rename(entry.path(), home->second / name, ec);
                }
                continue;
            }
            const std::filesystem::path setAside = rootDir_ / kRetiredDir / kStagingDir;
            std::filesystem::create_directories(setAside, ec);
            std::filesystem::rename(entry.path(), setAside / name, ec);
        }
    }

    ++writeGeneration_;  // whatever landed, or half-landed, the tree is not what it was
    return wroteEverything;
}

std::optional<std::string> LibraryStore::SaveImage(uint64_t itemId, const uint8_t* pixelsRGBA, int width,
                                                     int height) const {
    // The same base36 spelling the slugs use, so an id reads the same
    // wherever it appears rather than being decimal in a filename and
    // base36 in the directory beside it.
    return WritePicture(itemId, FormatUid(itemId) + ".qoi", pixelsRGBA, width, height);
}

std::optional<std::string> LibraryStore::SaveLayerImage(uint64_t itemId, size_t layerIndex,
                                                         const uint8_t* pixelsRGBA, int width, int height) const {
    // The owner's uid first, then the layer's place in its stack - so two
    // painted layers on one snippet can't collide, and so the name says
    // whose it is even when no record does.
    return WritePicture(itemId, FormatUid(itemId) + "_p" + std::to_string(layerIndex) + ".qoi", pixelsRGBA,
                        width, height);
}

std::optional<std::string> LibraryStore::WritePicture(uint64_t itemId, const std::string& filename,
                                                       const uint8_t* pixelsRGBA, int width, int height) const {
    const std::filesystem::path home = ImageHome(itemId);
    ++writeGeneration_;
    if (!EncodeQoiToFile(home / filename, pixelsRGBA, width, height)) {
        return std::nullopt;
    }
    // And a thumbnail beside it, so the Overview never has to decode the
    // full image to draw a 200px tile. Best-effort, and deliberately not
    // part of this function's success: the capture is the thing that must
    // survive, and it already has. Taken from the caller's own pixels
    // rather than through a DecodedImage, which would mean copying eight
    // megabytes to make forty kilobytes.
    const DecodedImage small = DownscaleToFit(pixelsRGBA, width, height, kThumbnailMaxExtent);
    EncodeQoiToFile(home / ThumbnailFilename(filename), small.pixelsRGBA.data(), small.width, small.height);
    return filename;
}

std::string LibraryStore::ThumbnailFilename(const std::string& imageFilename) {
    const size_t dot = imageFilename.find_last_of('.');
    const std::string stem = dot == std::string::npos ? imageFilename : imageFilename.substr(0, dot);
    return stem + ".thumb.qoi";
}

bool LibraryStore::SaveThumbnail(uint64_t itemId, const std::string& imageFilename, const DecodedImage& image) const {
    if (!IsPlainFilename(imageFilename) || image.width <= 0 || image.height <= 0) {
        return false;
    }
    const DecodedImage small = DownscaleToFit(image, kThumbnailMaxExtent);
    // Beside the image it belongs to, wherever that currently is - which for
    // a picture still in staging is staging, so the two travel together.
    const std::filesystem::path beside = FindImage(itemId, imageFilename).parent_path();
    ++writeGeneration_;
    return EncodeQoiToFile(beside / ThumbnailFilename(imageFilename), small.pixelsRGBA.data(), small.width,
                            small.height);
}

std::optional<DecodedImage> LibraryStore::LoadThumbnail(uint64_t itemId, const std::string& imageFilename) const {
    if (imageFilename.empty()) {
        return std::nullopt;
    }
    return DecodeImageFromFile(FindImage(itemId, ThumbnailFilename(imageFilename)));
}

std::optional<DecodedImage> LibraryStore::LoadImage(uint64_t itemId, const std::string& filename) const {
    if (filename.empty()) {
        return std::nullopt;
    }
    return DecodeImageFromFile(FindImage(itemId, filename));
}

}  // namespace sz::core::persistence
