#include "core/persistence/library_store.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <functional>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <system_error>
#include <thread>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "core/util/slug.h"
#include "core/util/timestamp_name.h"
#include "core/util/atomic_file.h"
#include "core/util/uid.h"

namespace sz::core::persistence {

namespace {

using nlohmann::json;

// An id in a record is spelled the way the directory names spell it: six
// base36 characters (see util/uid.h). One spelling everywhere, so that a
// record and the directory holding it, or a pointer and the thing it points
// at, can be matched by eye.
json IdJson(uint64_t id) { return FormatUid(id); }

uint64_t ReadId(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return 0;
    }
    if (it->is_string()) {
        return ParseUid(it->get<std::string>()).value_or(0);
    }
    return 0;  // anything else is no id, and 0 is "no id" everywhere
}

// ===== Numbers a record may carry =====
//
// JSON has no infinity and no NaN, but it has 1e100, which becomes
// infinity the moment it is read as a float - and one infinite coordinate
// poisons every bounding box, tessellation and hit test it meets. Every
// float a record holds comes through here: a value that is not finite
// reads as the field's default, and the fields with a meaningful range are
// held inside it. Clamped or defaulted, never refused: the rest of the
// record is still the user's, and a save writes the repaired value back -
// which is what `repaired` is for: a record the load had to change on the
// way in is not noted as already written (see ReadTree), so the next save
// writes it. A key that is simply absent reads as its default and is not
// a repair: written back, it reads the same next time.
float FiniteOr(const json& j, const char* key, float fallback, bool& repaired) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return fallback;
    }
    if (!it->is_number()) {
        repaired = true;
        return fallback;
    }
    const float value = it->get<float>();
    if (!std::isfinite(value)) {
        repaired = true;
        return fallback;
    }
    return value;
}

float ClampedOr(const json& j, const char* key, float fallback, float min, float max, bool& repaired) {
    const float value = FiniteOr(j, key, fallback, repaired);
    const float held = std::clamp(value, min, max);
    if (held != value) {
        repaired = true;
    }
    return held;
}

// A stroke's width or an item's native size: positive, and no wider than a
// screen is likely to be. Past that a value is a typo or an overflow, not
// a wish.
constexpr float kMaxSensibleExtent = 65536.0f;

json ToJson(const StrokePoint& p) { return json{{"x", p.x}, {"y", p.y}}; }

void FromJson(const json& j, StrokePoint& out, bool& repaired) {
    out.x = FiniteOr(j, "x", 0.0f, repaired);
    out.y = FiniteOr(j, "y", 0.0f, repaired);
}

json ToJson(const Stroke& s) {
    json points = json::array();
    for (const StrokePoint& p : s.points) {
        points.push_back(ToJson(p));
    }
    return json{{"colorRGBA", s.colorRGBA}, {"width", s.width}, {"points", std::move(points)}};
}

void FromJson(const json& j, Stroke& out, bool& repaired) {
    out.colorRGBA = j.value("colorRGBA", 0xFF0000FFu);
    out.width = FiniteOr(j, "width", 3.0f, repaired);
    if (out.width <= 0.0f || out.width > kMaxSensibleExtent) {
        out.width = 3.0f;
        repaired = true;
    }
    out.points.clear();
    if (const auto it = j.find("points"); it != j.end() && it->is_array()) {
        out.points.reserve(it->size());
        for (const auto& pointJson : *it) {
            StrokePoint point;
            FromJson(pointJson, point, repaired);
            out.points.push_back(point);
        }
    }
}

json ToJson(const Rect& r) { return json{{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}}; }

void FromJson(const json& j, Rect& out, bool& repaired) {
    out.x = FiniteOr(j, "x", 0.0f, repaired);
    out.y = FiniteOr(j, "y", 0.0f, repaired);
    out.w = FiniteOr(j, "w", 0.0f, repaired);
    out.h = FiniteOr(j, "h", 0.0f, repaired);
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
// capture or painted layer or a thumbnail beside one, all .qoi. What the
// per-snippet collection below is limited to.
bool IsPictureFilename(const std::string& name) {
    return name.size() >= 4 && std::string_view(name).substr(name.size() - 4) == ".qoi";
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

bool FromJson(const json& j, Layer& out, bool& repaired) {
    if (!j.is_object()) {
        return false;
    }
    out.kind = j.value("kind", std::string("image")) == "painted" ? LayerKind::Painted : LayerKind::Image;
    out.opacity = ClampedOr(j, "opacity", 0.0f, 0.0f, 1.0f, repaired);
    out.resolutionScale = ClampedOr(j, "resolutionScale", 1.0f, 0.05f, 16.0f, repaired);
    out.tintColorRGBA = j.value("tintColorRGBA", uint32_t{0xFFFFFFFF});
    out.showsPlaceholder = j.value("showsPlaceholder", false);
    out.placeholderHue = ClampedOr(j, "placeholderHue", 0.0f, 0.0f, 360.0f, repaired);
    out.imageFile = j.value("imageFile", std::string());
    if (!out.imageFile.empty() && !IsPlainFilename(out.imageFile)) {
        out.imageFile.clear();  // a layer with no picture, rather than one somewhere else
        repaired = true;
    }
    return true;
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
        {"keepAspect", item.keepAspect},
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
// pair of eyes on the same list of fields. A field added to the serializer
// and not to this is an edit that is silently never saved - the worst bug
// this file could have - so they are kept adjacent, the order of the fields
// is the same in both, and ItemContentHashTest asserts that every one of
// them moves the hash.
//
// A hash rather than the text it would have written, because avoiding the
// serialization is the whole point: a stroke point becomes a JSON object,
// and an ordinary canvas carries tens of thousands of them. This walks the
// same data as raw bytes and costs about a thousandth of building that
// document. (The exact text *is* what the small records are compared by -
// see writtenFileText_ - because for those the serialization is free and an
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
    h.Mix(item.keepAspect);
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
//
// `repaired` is set when a value the record carried had to be changed to
// be usable - see FiniteOr - and is what keeps the repaired record from
// being noted as already written (see ReadTree).
bool FromJson(const json& j, Item& out, bool& repaired) {
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
        FromJson(*it, out.rect, repaired);
    }
    out.nativeW = ClampedOr(j, "nativeW", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.nativeH = ClampedOr(j, "nativeH", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.foregroundOpacity = ClampedOr(j, "foregroundOpacity", 1.0f, 0.0f, 1.0f, repaired);
    out.isFullscreen = j.value("isFullscreen", false);
    out.isFullscreenStretch = j.value("isFullscreenStretch", false);
    out.minimized = j.value("minimized", false);
    out.pinned = j.value("pinned", false);
    out.layers.clear();
    if (const auto layersIt = j.find("layers"); layersIt != j.end() && layersIt->is_array()) {
        for (const json& layerJson : *layersIt) {
            Layer layer;
            if (FromJson(layerJson, layer, repaired)) {
                out.layers.push_back(std::move(layer));
            } else {
                repaired = true;  // an entry that was not a layer, left out
            }
        }
    }
    if (out.layers.empty()) {
        // A record with no list, an empty one, or one whose every entry
        // was unreadable. Item::layers is never empty - see its own comment
        // - so this is where that promise is kept for a loaded item.
        out.layers.push_back(Layer{});
        repaired = true;
    }
    out.noteText = j.value("noteText", std::string());
    out.noteTextColorRGBA = j.value("noteTextColorRGBA", uint32_t{0xFFFFFFFF});
    // Clamped, not rejected, and the fallback is the same default a fresh
    // Item carries: a record without these two fields reads back as a
    // note in the default style.
    out.noteTextSizePx = ClampedOr(j, "noteTextSizePx", 17.0f, kNoteTextSizeMin, kNoteTextSizeMax, repaired);
    // A record from before the field: what its handles did then, which was
    // to keep the shape until there was text.
    out.keepAspect = j.value("keepAspect", out.noteText.empty());
    // anchorRect defaults to a zero Rect (via FromJson(Rect)'s own
    // per-field 0.0f defaults) and anchorDisplayWidth/Height default to 0
    // - together, "not yet anchored" (see Item::anchorRect's own doc
    // comment), the correct fallback for a record without them.
    // CanvasManager::SyncItemsToDisplaySize adopts the loaded `rect` as the
    // anchor the first time it runs.
    if (const auto it = j.find("anchorRect"); it != j.end()) {
        FromJson(*it, out.anchorRect, repaired);
    }
    out.anchorDisplayWidth = ClampedOr(j, "anchorDisplayWidth", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.anchorDisplayHeight = ClampedOr(j, "anchorDisplayHeight", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.createdAt = j.value("createdAt", int64_t{0});
    out.deletedAt = j.value("deletedAt", int64_t{0});
    out.strokes.clear();
    if (const auto it = j.find("strokes"); it != j.end() && it->is_array()) {
        out.strokes.reserve(it->size());
        for (const auto& strokeJson : *it) {
            Stroke stroke;
            FromJson(strokeJson, stroke, repaired);
            out.strokes.push_back(std::move(stroke));
        }
    }
    return true;
}

// Only what the tree cannot say. No items: each one is a directory of its
// own beside this file, so that a snippet - its record, its picture and its
// thumbnail - is a single thing to move. No folderId: the directory this
// sits in says which folder it is in, and a copy of that here would be
// wrong from the moment someone dragged the directory somewhere else. No
// slug: the directory name is regenerated from the current name on every
// save, and a stored one would be stale from the first rename on. A
// record that repeats what the tree says can only agree with
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

// Every file this store writes goes through WriteFileAtomically (see
// core/util/atomic_file.h): a fresh temporary beside the destination,
// renamed over it once whole, so a reader never sees half a record.

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
constexpr const char* kStagingDir = "staging";
// Where a save sets aside a directory the library no longer holds and
// nobody deleted for good (see Save's retirement pass) - whole, record and
// pictures together, at the same path it had under folders/. Nothing reads
// it back: it is there for a person to recover something from, where a
// delete would have left nothing to recover.
constexpr const char* kRetiredDir = "retired";
// What was deleted for good and is not wholly gone from the disk yet, by
// uid - see LibraryStore::Remove. Written before anything is removed, so a
// removal that stops partway is still known at the next start.
constexpr const char* kPendingFile = "pending.json";
// The mark an older build left in a directory it could not wholly remove,
// written only once a removal had failed - so a crash partway through one
// reloaded what was left of it. Still read as a removal owed, and removed
// with the rest.
constexpr const char* kRemovedMarker = ".removed";

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

// The same for a snippet's record, which also says whether it had to be
// repaired on the way in - see FromJson(Item).
bool ReadItemRecord(const json& j, Item& out, bool& repaired) {
    try {
        return FromJson(j, out, repaired);
    } catch (const json::exception&) {
        return false;
    }
}

// The most a record may be before it is read at all. A snippet's record
// is a JSON object per stroke point, and a heavily drawn-on snippet runs to
// a few megabytes; 64 MB is some seven hundred thousand points on one
// snippet, far past anything a hand draws, and a file bigger than that in a
// record's place is not a record. Refused unread rather than parsed into
// memory to find out - and never written either (see Save), so that a
// record this store writes is always one it reads back.
constexpr uintmax_t kMaxRecordBytes = uintmax_t{64} << 20;

std::optional<std::string> ReadFileText(FileSystem& fs, const std::filesystem::path& path) {
    return fs.Read(path, kMaxRecordBytes);
}

// Whether `path` is a file that could be read as a record, were nothing
// holding it: a file, and no larger than a record may be. What cannot be
// read for any other reason - a directory in its place, a file far too big
// - will not be read by asking again.
bool CouldBeHeld(FileSystem& fs, const std::filesystem::path& path) {
    return fs.Status(path) == FileSystem::Kind::File && fs.FileSize(path).value_or(0) <= kMaxRecordBytes;
}

// `path`'s text, waiting out a hold: a file there that cannot be read - a
// backup, a sync client or a virus scanner holding it, the likely reasons
// - is asked again, five times over half a second, the likely cure.
std::optional<std::string> ReadWaitingOutAHold(FileSystem& fs, const std::filesystem::path& path) {
    std::optional<std::string> text = ReadFileText(fs, path);
    for (int attempt = 0; !text && CouldBeHeld(fs, path) && attempt < 5; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        text = ReadFileText(fs, path);
    }
    return text;
}

std::optional<json> ReadJsonFile(FileSystem& fs, const std::filesystem::path& path) {
    const std::optional<std::string> text = ReadFileText(fs, path);
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
// deleted. Load skips linked directories and does not look inside one
// (see SortedSubdirectories), so nothing behind a link ever enters the
// index; and every path this store creates, writes, moves or deletes goes
// through one check, LibraryStore::IsOurs, which walks the whole path from
// the root down for a link (see CrossesLink) - at the moment of the
// operation, whether the path was just made or has been indexed since the
// load. That includes the top-level directories - folders/, staging/,
// retired/ - which the first version of this took on trust and a junction
// at any of which had a save writing a whole tree outside the library; and
// it includes an indexed directory replaced by a junction between two
// saves, which the second version wrote its record through, since it had
// been real when indexed. A link standing where a save wants to
// put a directory makes that record unplaceable: the save reports failure
// and leaves the link alone. The one thing not checked is the library
// root itself: a root that is a junction is how a library is moved to
// another drive, and is supported.

// Whether `path` itself is a symlink or junction - a junction being what
// "mklink /J" makes, the reparse point Windows lets a user create without
// privileges, and one that looks like a directory to everything that does
// not ask.
bool IsLink(FileSystem& fs, const std::filesystem::path& path) {
    return fs.LinkStatus(path) == FileSystem::Kind::Link;
}

// Everything in `dir` whose name can be spelled in UTF-8, or nullopt when
// it cannot be listed. The rest - a lone surrogate, which NTFS allows in a
// name - are nothing the store wrote, its names being ASCII, and are left
// alone as anyone else's files are: turned into a std::string to be looked
// at, one threw std::system_error out of Save, Load and Remove alike, and
// took the app down.
std::optional<std::vector<FileSystem::Entry>> ListNamed(FileSystem& fs, const std::filesystem::path& dir) {
    std::optional<std::vector<FileSystem::Entry>> entries = fs.List(dir);
    if (!entries) {
        return std::nullopt;
    }
    std::erase_if(*entries, [](const FileSystem::Entry& entry) {
        try {
            (void)entry.name.string();
            return false;
        } catch (const std::system_error&) {
            return true;
        }
    });
    return entries;
}

// Everything in `dir` it is safe to look at (see ListNamed), or nothing
// when it cannot be listed.
std::vector<FileSystem::Entry> Listing(FileSystem& fs, const std::filesystem::path& dir) {
    std::optional<std::vector<FileSystem::Entry>> entries = ListNamed(fs, dir);
    return entries ? std::move(*entries) : std::vector<FileSystem::Entry>{};
}

// Whether any component of `path` from `root` down - `root` itself
// excluded, `path` itself included - is a link. Checked before anything
// under `path` is deleted or moved: a plain directory below a linked
// ancestor is physically somewhere else, whatever it is called here.
bool CrossesLink(FileSystem& fs, const std::filesystem::path& root, const std::filesystem::path& path) {
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
        if (IsLink(fs, walked)) {
            return true;
        }
    }
    return false;
}

// Deletes `path` with everything in it - or, for a link, the link alone,
// whatever it points at: a removal that walked into a junction would be
// emptying a directory outside the library (see FileSystem::RemoveAll).
bool RemoveTree(FileSystem& fs, const std::filesystem::path& path) {
    return fs.RemoveAll(path);
}

// ===== What a permanent delete may take =====
//
// A directory of the library's can hold what someone else put there - a
// note beside a record, a folder of scans beside a canvas - and none of it
// is the store's to delete. A permanent delete therefore takes what the
// store writes and nothing else: the records, the order files, the
// pictures and thumbnails (the same rule the per-snippet sweep in Save
// keeps, see IsPictureFilename), the removed mark, and a temporary a crash
// left one of those as. It recurses only into a directory that is the
// store's by Load's own rule - one holding a record, or the mark - and
// removes a directory only once it is empty. What is left standing
// afterwards holds no record, so Load does not read it and Save, which
// never indexed it, does not set it aside.

// `name` without the ".tmp" or ".tmp3" a temporary of WriteFileAtomically's
// ends in, or `name` itself if it has none.
std::string_view WithoutTemporarySuffix(std::string_view name) {
    if (const size_t tmp = name.rfind(".tmp"); tmp != std::string_view::npos) {
        const std::string_view suffix = name.substr(tmp + 4);
        if (std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return name.substr(0, tmp);
        }
    }
    return name;
}

// Whether `name` is a file this store writes beside a record, or a
// temporary of one.
bool IsOwnFilename(const std::string& name) {
    const std::string_view base = WithoutTemporarySuffix(name);
    for (const char* own : {kFolderFile, kCanvasFile, kItemFile, kOrderFile, kRemovedMarker}) {
        if (base == own) {
            return true;
        }
    }
    return IsPictureFilename(std::string(base));
}

std::optional<uint64_t> UidFromDirectoryName(const std::string& name);

// Whether `dir` is a directory of the store's, asked when deleting for
// good what it is inside of: it holds a record, the removed mark left in
// one whose record went before the rest could, an order file, a picture
// named after the directory's own uid, or a temporary of any of those - or
// a directory of the store's below it. All but the first two are what a
// save that stopped partway leaves of a directory whose own record never
// landed: its snippets written and not the canvas record above them, or a
// snippet's picture moved in from staging and not its record. Asked only
// about the record, such a directory was left standing inside one deleted
// for good, and kept it standing. Two levels down is as deep as the tree
// goes.
bool HoldsOwnRecord(FileSystem& fs, const std::filesystem::path& dir, int depth = 0) {
    const std::optional<uint64_t> uid = UidFromDirectoryName(dir.filename().string());
    const std::string pictureStem = uid ? FormatUid(*uid) : std::string();
    for (const FileSystem::Entry& entry : Listing(fs, dir)) {
        if (entry.kind == FileSystem::Kind::Directory) {
            if (depth < 2 && HoldsOwnRecord(fs, dir / entry.name, depth + 1)) {
                return true;
            }
            continue;
        }
        if (entry.kind != FileSystem::Kind::File) {
            continue;
        }
        const std::string name = entry.name.string();
        const std::string_view base = WithoutTemporarySuffix(name);
        for (const char* own : {kFolderFile, kCanvasFile, kItemFile, kOrderFile, kRemovedMarker}) {
            if (base == own) {
                return true;
            }
        }
        if (!pictureStem.empty() && base.substr(0, pictureStem.size()) == pictureStem &&
            IsPictureFilename(std::string(base))) {
            return true;
        }
    }
    return false;
}

// Takes everything of the store's out of `dir`, recursively, and `dir`
// itself once that leaves it empty. True once nothing of the store's is
// left under it - a file held open by another program keeps it false, and
// so does a directory that could not be listed: not looked at is not
// emptied, and calling it done left the record in it to load again.
bool RemoveOwnContents(FileSystem& fs, const std::filesystem::path& dir, const std::set<uint64_t>& erased) {
    const std::optional<std::vector<FileSystem::Entry>> entries = ListNamed(fs, dir);
    if (!entries) {
        return false;
    }
    bool clean = true;
    for (const FileSystem::Entry& entry : *entries) {
        const std::filesystem::path path = dir / entry.name;
        if (entry.kind == FileSystem::Kind::Link) {
            continue;  // never the store's, whatever it points at
        }
        if (entry.kind == FileSystem::Kind::Directory) {
            const std::optional<uint64_t> uid = UidFromDirectoryName(entry.name.string());
            if ((uid && erased.count(*uid) > 0) || HoldsOwnRecord(fs, path)) {
                clean = RemoveOwnContents(fs, path, erased) && clean;
            }
            continue;
        }
        const std::string name = entry.name.string();
        if (entry.kind != FileSystem::Kind::File || !IsOwnFilename(name) || name == kRemovedMarker) {
            continue;
        }
        fs.Remove(path);
        clean = clean && fs.LinkStatus(path) == FileSystem::Kind::None;
    }
    if (!clean) {
        return false;
    }
    // An older build's mark goes last, and only once everything it stood
    // for has.
    const std::filesystem::path marker = dir / kRemovedMarker;
    fs.Remove(marker);
    if (fs.LinkStatus(marker) != FileSystem::Kind::None) {
        return false;
    }
    fs.Remove(dir);  // refused when someone else's files keep it: theirs to keep
    return true;
}

// Sorted, so that what a directory holds is walked in the same order twice
// running. Enumeration order is not specified by the filesystem, and an
// unlisted member's position (see ApplyOrder) would otherwise wander.
// Real directories only: a linked one is not part of the tree, see above.
// `listed`, when given, goes false if `dir` could not be listed - which is
// not the same as it holding nothing.
std::vector<std::filesystem::path> SortedSubdirectories(FileSystem& fs, const std::filesystem::path& dir,
                                                        bool* listed = nullptr) {
    std::vector<std::filesystem::path> out;
    if (IsLink(fs, dir)) {
        return out;  // nothing behind a link is read - folders/ itself included
    }
    const std::optional<std::vector<FileSystem::Entry>> entries = ListNamed(fs, dir);
    if (!entries) {
        if (listed != nullptr && fs.LinkStatus(dir) != FileSystem::Kind::None) {
            *listed = false;
        }
        return out;
    }
    for (const FileSystem::Entry& entry : *entries) {
        if (entry.kind == FileSystem::Kind::Directory) {
            out.push_back(dir / entry.name);
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
// IdJson.
//
// The file's exact text comes back beside the entries, for Load to compare
// against what a save would write - see the baseline it establishes.
struct OrderFile {
    std::vector<std::string> names;
    std::optional<std::string> text;
};

OrderFile ReadOrderFile(FileSystem& fs, const std::filesystem::path& path, const char* key) {
    OrderFile file;
    file.text = ReadFileText(fs, path);
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

// Reorders `found` to match `order`, which is a list of uids.
//
// This is the whole reconciliation rule in one function, and it is
// deliberately forgiving in both directions: anything the order file names
// that is no longer there simply doesn't appear, and anything present that
// the order file doesn't name goes to the end. Neither is an error, because
// both are what a directory looks like after somebody moved something into
// or out of it while the app wasn't running - which is a thing this layout
// exists to allow.
//
// Matched on the directory's trailing uid, so renaming the readable half of
// a directory by hand doesn't lose its place. An entry that is not a uid
// names nothing.
template <typename T>
void ApplyOrder(std::vector<std::pair<std::string, T>>& found, const std::vector<std::string>& order) {
    std::vector<std::pair<std::string, T>> sorted;
    sorted.reserve(found.size());
    std::vector<bool> placed(found.size(), false);
    for (const std::string& wanted : order) {
        const std::optional<uint64_t> wantedUid = ParseUid(wanted);
        if (!wantedUid) {
            continue;
        }
        for (size_t i = 0; i < found.size(); ++i) {
            if (placed[i]) {
                continue;
            }
            if (UidFromDirectoryName(found[i].first) == wantedUid) {
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

// Puts each of `unread` back into `order` where `old` had it: after the
// nearest name before it in `old` that `order` still has, or first when
// there is none. Walked in `old`'s order, so two unread side by side stay
// that way.
void KeepUnreadPlaces(std::vector<std::string>& order, const std::vector<std::string>& old,
                      const std::set<std::string>& unread) {
    for (size_t i = 0; i < old.size(); ++i) {
        if (unread.count(old[i]) == 0 || std::find(order.begin(), order.end(), old[i]) != order.end()) {
            continue;
        }
        auto at = order.begin();
        for (size_t before = i; before-- > 0;) {
            if (const auto it = std::find(order.begin(), order.end(), old[before]); it != order.end()) {
                at = std::next(it);
                break;
            }
        }
        order.insert(at, old[i]);
    }
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
// {"erased": ["k3j9x2", ...], "moves": {"s7d2k1": "b4n6c3"}} - the uids
// spelled the way the directory names spell them; "moves" only while
// there are any.
json PendingJson(const std::set<uint64_t>& erased, const std::map<uint64_t, uint64_t>& moves) {
    json list = json::array();
    for (const uint64_t uid : erased) {
        list.push_back(FormatUid(uid));
    }
    json doc{{"erased", std::move(list)}};
    if (!moves.empty()) {
        json to = json::object();
        for (const auto& [uid, parent] : moves) {
            to[FormatUid(uid)] = FormatUid(parent);
        }
        doc["moves"] = std::move(to);
    }
    return doc;
}

json LibraryJson(FolderId currentFolderId, CanvasId currentCanvasId) {
    json doc;
    doc["version"] = LibraryStore::kFormatVersion;
    doc["currentFolderId"] = IdJson(currentFolderId);
    doc["currentCanvasId"] = IdJson(currentCanvasId);
    return doc;
}

// Moves everything inside `source` into `target`, which already exists,
// and removes `source`. A directory that exists on both sides is merged
// the same way; a file on both sides is the source's, which is the newer.
// What setting a directory aside needs when its place in retired/ is
// already taken - by a snippet set aside before its canvas was, say, or by
// an earlier retirement of the same uid.
bool MergeDirectoryInto(FileSystem& fs, const std::filesystem::path& source, const std::filesystem::path& target) {
    bool complete = true;
    for (const FileSystem::Entry& entry : Listing(fs, source)) {
        const std::filesystem::path from = source / entry.name;
        const std::filesystem::path to = target / entry.name;
        // A link on either side is a leaf: one in the source is moved as a
        // link, and one in the target is replaced as a link. Recursing into
        // either would be moving files into, or out of, wherever it points.
        const bool bothRealDirectories =
            entry.kind == FileSystem::Kind::Directory && fs.LinkStatus(to) == FileSystem::Kind::Directory;
        if (bothRealDirectories) {
            complete = MergeDirectoryInto(fs, from, to) && complete;
            continue;
        }
        if (fs.LinkStatus(to) != FileSystem::Kind::None) {
            RemoveTree(fs, to);
        }
        complete = fs.Rename(from, to) && complete;
    }
    if (complete) {
        fs.Remove(source);
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

// Every folder, canvas and snippet `view` holds, by id - ids are unique
// across all three (see util/uid.h).
std::unordered_set<uint64_t> IdsIn(const LibraryView& view) {
    std::unordered_set<uint64_t> ids;
    for (const Folder& folder : view.folders) {
        ids.insert(folder.id);
    }
    for (const Canvas& canvas : view.canvases) {
        ids.insert(canvas.id);
        for (const Item& item : canvas.items) {
            ids.insert(item.id);
        }
    }
    return ids;
}

bool IsUnder(const std::filesystem::path& path, const std::filesystem::path& dir) {
    const std::filesystem::path relative = path.lexically_relative(dir);
    return !relative.empty() && *relative.begin() != "..";
}

}  // namespace

LibraryStore::LibraryStore(std::filesystem::path rootDir, FileSystem& fs)
    : rootDir_(std::move(rootDir)), fs_(&fs) {}

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

bool LibraryStore::IsOurs(const std::filesystem::path& path) const {
    return WithinRoot(path) && !CrossesLink(*fs_, rootDir_, path);
}

bool LibraryStore::RemoveOwnDirectory(const std::filesystem::path& path, const std::set<uint64_t>& erased) const {
    // Nothing in the index is a link or below one - Load never indexes a
    // linked directory - so meeting one here means the tree was rearranged
    // under a running instance, and the directory is physically somewhere
    // this store has never read. Left alone.
    if (!IsOurs(path)) {
        return false;
    }
    if (fs_->LinkStatus(path) == FileSystem::Kind::None) {
        return true;  // gone already - by hand, or by an earlier run at it
    }
    // Whether nothing of the store's is left is the answer, not whether the
    // directory is gone: a file held open without delete sharing by another
    // process, on Windows, keeps it there and the removal owed, where a file
    // of someone else's beside the record keeps it there and is theirs.
    return RemoveOwnContents(*fs_, path, erased);
}

void LibraryStore::ForgetUnder(const std::filesystem::path& dir, const std::unordered_set<uint64_t>& keep) const {
    for (auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
        for (auto it = index->begin(); it != index->end();) {
            const bool forget = IsUnder(it->second, dir) && keep.count(it->first) == 0;
            it = forget ? index->erase(it) : std::next(it);
        }
    }
}

bool LibraryStore::HoldsAnyOf(const std::filesystem::path& dir, const std::unordered_set<uint64_t>& ids) const {
    for (const auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
        for (const auto& [id, path] : *index) {
            if (ids.count(id) > 0 && IsUnder(path, dir)) {
                return true;
            }
        }
    }
    return false;
}

bool LibraryStore::WrittenByANewerVersion() const {
    // Asked once, and the answer kept either way. Asked again by Load after
    // TrayController::Initialize had its answer, a library.json held for a
    // moment in between made Load report no library at all - a first run,
    // on a store that then refused every write.
    if (versionKnown_) {
        return writtenByANewerVersion_;
    }
    versionKnown_ = true;
    const std::filesystem::path path = rootDir_ / "library.json";
    const std::optional<std::string> text = ReadWaitingOutAHold(*fs_, path);
    if (!text) {
        // A file that could be read and still cannot is not this build's
        // to call its own. Anything else in its place - nothing, a
        // directory, a file far bigger than any record - is repaired as a
        // pointer file with nothing readable in it (see Load).
        if (CouldBeHeld(*fs_, path)) {
            writtenByANewerVersion_ = true;
            versionUnreadable_ = true;
        }
        return writtenByANewerVersion_;
    }
    const json doc = json::parse(*text, /*callback=*/nullptr, /*allow_exceptions=*/false);
    if (doc.is_object()) {
        const auto version = doc.find("version");
        writtenByANewerVersion_ =
            version != doc.end() && version->is_number_integer() && version->get<int64_t>() > kFormatVersion;
    }
    return writtenByANewerVersion_;
}

bool LibraryStore::Remove(const std::vector<uint64_t>& uids, const LibraryView& remaining) const {
    if (writtenByANewerVersion_) {
        return false;
    }
    const std::unordered_set<uint64_t> held = IdsIn(remaining);
    bool known = false;
    for (const uint64_t uid : uids) {
        std::filesystem::path dir;
        for (const auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
            if (const auto it = index->find(uid); it != index->end()) {
                dir = it->second;
                break;
            }
        }
        // Every path in the index was made under the root, so one that
        // isn't is a bug - and a bug here leaves the directory alone.
        if (dir.empty() || !WithinRoot(dir)) {
            continue;
        }
        known = true;
        pendingRemovals_[uid] = dir;
        // Whatever the index has inside it that the library no longer
        // holds goes with it, named or not. What it still holds - moved out
        // in the model, not yet on disk - stays indexed, and the removal
        // waits for it to be moved (see RunPendingRemovals).
        for (const auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
            for (const auto& [id, path] : *index) {
                if (id != uid && held.count(id) == 0 && IsUnder(path, dir)) {
                    pendingRemovals_[id] = path;
                }
            }
        }
        // Out of the index, so that nothing under it is placed, or taken
        // for something gone missing and set aside, meanwhile.
        ForgetUnder(dir, held);
    }
    if (!known) {
        return false;
    }
    ++writeGeneration_;
    RunPendingRemovals(remaining);
    return std::none_of(uids.begin(), uids.end(), [this](uint64_t uid) { return pendingRemovals_.count(uid) > 0; });
}

LibraryStore::PendingRecord LibraryStore::PendingFor(const LibraryView& library) const {
    // Where the library has each canvas and snippet.
    std::unordered_map<uint64_t, uint64_t> parentOf;
    for (const Canvas& canvas : library.canvases) {
        parentOf[canvas.id] = canvas.folderId;
        for (const Item& item : canvas.items) {
            parentOf[item.id] = canvas.id;
        }
    }
    PendingRecord record;
    for (const auto& [uid, dir] : pendingRemovals_) {
        record.erased.insert(uid);
        // Anything the library still holds directly inside - moved out in
        // the model, not yet on disk - and where it belongs. What is inside
        // that goes with it, so only the directory directly inside is named.
        // Named by the uid its directory's name ends in, which is what the
        // rescue at the next start looks for: the two differ for a copy
        // made by hand, which Load gave an id of its own.
        for (const auto* index : {&canvasDirs_, &itemDirs_}) {
            for (const auto& [id, path] : *index) {
                const auto parent = parentOf.find(id);
                if (parent != parentOf.end() && path.parent_path() == dir) {
                    record.moves[UidFromDirectoryName(path.filename().string()).value_or(id)] = parent->second;
                }
            }
        }
    }
    // ...and what the file said that this session could not look at.
    record.erased.insert(unobservedPending_.erased.begin(), unobservedPending_.erased.end());
    record.moves.insert(unobservedPending_.moves.begin(), unobservedPending_.moves.end());
    return record;
}

bool LibraryStore::HoldsUnrescued(const std::filesystem::path& dir, int depth) const {
    if (unobservedPending_.moves.empty()) {
        return false;
    }
    bool listed = true;
    for (const std::filesystem::path& inside : SortedSubdirectories(*fs_, dir, &listed)) {
        const std::optional<uint64_t> uid = UidFromDirectoryName(inside.filename().string());
        if ((uid && unobservedPending_.moves.count(*uid) > 0) || (depth < 1 && HoldsUnrescued(inside, depth + 1))) {
            return true;
        }
    }
    return !listed;
}

std::vector<std::filesystem::path> LibraryStore::WalkInto(const std::filesystem::path& dir) const {
    bool listed = true;
    std::vector<std::filesystem::path> inside = SortedSubdirectories(*fs_, dir, &listed);
    walkedEverything_ = walkedEverything_ && listed;
    return inside;
}

void LibraryStore::NoteUnobservedPending(const std::unordered_set<uint64_t>& seen, bool sawEverything) const {
    unobservedPending_ = {};
    // A walk that saw everything and did not find it shows it gone - by
    // hand, or by a removal that finished before the file was rewritten.
    if (sawEverything) {
        return;
    }
    for (const uint64_t uid : writtenPending_.erased) {
        if (pendingRemovals_.count(uid) == 0) {
            unobservedPending_.erased.insert(uid);
        }
    }
    for (const auto& [uid, parent] : writtenPending_.moves) {
        if (seen.count(uid) == 0) {
            unobservedPending_.moves.emplace(uid, parent);
        }
    }
}

bool LibraryStore::RunPendingRemovals(const LibraryView& library) const {
    // On disk before anything is touched: a removal that stops partway,
    // for a crash or a held file, is then still known at the next start -
    // and so is where whatever was moved out of it belongs, while it is
    // still inside. Only what is recorded is removed, so a record that
    // could not be written removes nothing new.
    const bool recorded = WritePendingFile(PendingFor(library));
    std::set<uint64_t> erased = writtenPending_.erased;
    for (const auto& [uid, dir] : pendingRemovals_) {
        erased.insert(uid);
    }
    // Ready: nothing the library holds is inside any more. One that still
    // holds something waits for the save that moves it out.
    const std::unordered_set<uint64_t> live = IdsIn(library);
    for (auto it = pendingRemovals_.begin(); it != pendingRemovals_.end();) {
        const bool ready = writtenPending_.erased.count(it->first) > 0 && !HoldsAnyOf(it->second, live) &&
                           !HoldsUnrescued(it->second);
        if (ready && RemoveOwnDirectory(it->second, erased)) {
            ForgetUnder(it->second);
            it = pendingRemovals_.erase(it);
        } else {
            ++it;
        }
    }
    // And the record brought in line with what is left. Best effort: one
    // still naming something gone costs the next load a look for it.
    WritePendingFile(PendingFor(library));
    return recorded;
}

bool LibraryStore::WritePendingFile(const PendingRecord& record) const {
    if (record == writtenPending_) {
        return true;
    }
    if (!pendingFileReadable_) {
        return false;  // what it names is unknown; not written over
    }
    const std::filesystem::path file = rootDir_ / kPendingFile;
    if (!IsOurs(file)) {
        return false;
    }
    const bool written = record.erased.empty() && record.moves.empty()
                             ? fs_->Remove(file)
                             : WriteFileAtomically(*fs_, file, PendingJson(record.erased, record.moves).dump(2));
    if (written) {
        writtenPending_ = record;
    }
    return written;
}

void LibraryStore::ReadPendingFile() const {
    writtenPending_ = {};
    pendingFileReadable_ = true;
    const std::filesystem::path file = rootDir_ / kPendingFile;
    if (fs_->LinkStatus(file) == FileSystem::Kind::None) {
        return;
    }
    const std::optional<json> doc = ReadJsonFile(*fs_, file);
    if (!doc) {
        pendingFileReadable_ = false;
        return;
    }
    if (const auto it = doc->find("erased"); it != doc->end() && it->is_array()) {
        for (const json& entry : *it) {
            if (!entry.is_string()) {
                continue;
            }
            if (const std::optional<uint64_t> uid = ParseUid(entry.get<std::string>())) {
                writtenPending_.erased.insert(*uid);
            }
        }
    }
    if (const auto it = doc->find("moves"); it != doc->end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            const std::optional<uint64_t> uid = ParseUid(key);
            const std::optional<uint64_t> parent =
                value.is_string() ? ParseUid(value.get<std::string>()) : std::nullopt;
            if (uid && parent) {
                writtenPending_.moves[*uid] = *parent;
            }
        }
    }
}

bool LibraryStore::NotePendingRemoval(const std::filesystem::path& dir) const {
    const std::optional<uint64_t> uid = UidFromDirectoryName(dir.filename().string());
    if (!uid || writtenPending_.erased.count(*uid) == 0) {
        if (!fs_->Exists(dir / kRemovedMarker)) {
            return false;
        }
        // An older build's mark. A name without a uid is left alone: not
        // read, since the mark says so, and not removed, since nothing
        // says whose it was.
        if (!uid) {
            return true;
        }
        // Recorded on disk as much as pending.json would record it; the
        // next save writes it there too. An older build marked a directory
        // only once nothing moved out of it was left inside, so whatever
        // is inside went with it.
        writtenPending_.erased.insert(*uid);
        for (const std::filesystem::path& inside : WalkInto(dir)) {
            if (const std::optional<uint64_t> insideUid = UidFromDirectoryName(inside.filename().string())) {
                writtenPending_.erased.insert(*insideUid);
            }
        }
    }
    // Owed under the uid its name ends in - what Remove keyed it by, since
    // the record inside may already be gone.
    pendingRemovals_[*uid] = dir;
    // ...and whatever inside it was deleted for good along with it, which
    // may have lost its own record to an earlier run at it.
    for (const std::filesystem::path& inside : WalkInto(dir)) {
        const std::optional<uint64_t> insideUid = UidFromDirectoryName(inside.filename().string());
        if (insideUid && writtenPending_.erased.count(*insideUid) > 0) {
            NotePendingRemoval(inside);
        }
    }
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
    if (!IsPlainFilename(filename)) {
        // Somewhere that cannot exist, inside the library, rather than
        // wherever the name was trying to point - see IsPlainFilename.
        return rootDir_ / kStagingDir / "not-a-filename";
    }
    if (const auto it = itemDirs_.find(itemId); it != itemDirs_.end()) {
        const std::filesystem::path inHome = it->second / filename;
        if (fs_->Exists(inHome)) {
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

    // The uids of directories in a container that were not read - for
    // unreadPlaces_, which keeps those the order file names.
    const auto noteUnread = [this](const std::string& key, const OrderFile& file,
                                   const std::set<std::string>& notRead) {
        UnreadPlaces places;
        for (const std::string& name : file.names) {
            if (notRead.count(name) > 0) {
                places.unread.insert(name);
            }
        }
        if (!places.unread.empty()) {
            places.order = file.names;
            unreadPlaces_[key] = std::move(places);
        }
    };
    const auto uidNameOf = [](const std::filesystem::path& dir) {
        const std::optional<uint64_t> uid = UidFromDirectoryName(dir.filename().string());
        return uid ? FormatUid(*uid) : std::string();
    };

    // What was deleted for good inside a folder or canvas whose record
    // cannot be read - by another program, for a moment - is owed all the
    // same, `levels` down. Not looked for, it was not in the next
    // pending.json, and loaded again at the first start where the record
    // could be read.
    const std::function<void(const std::filesystem::path&, int)> noteRemovalsInside =
        [&](const std::filesystem::path& dir, int levels) {
            for (const std::filesystem::path& inside : WalkInto(dir)) {
                if (!NotePendingRemoval(inside) && levels > 1) {
                    noteRemovalsInside(inside, levels - 1);
                }
            }
        };

    // A record that is there and was not read - held, for a moment - as
    // opposed to a directory with none, which is not ours.
    const auto noteUnreadRecord = [this](const std::filesystem::path& record) {
        if (fs_->LinkStatus(record) != FileSystem::Kind::None) {
            readEverything_ = false;
        }
    };

    std::vector<std::pair<std::string, Folder>> foundFolders;
    std::set<std::string> foldersNotRead;
    for (const std::filesystem::path& folderDir : WalkInto(foldersRoot)) {
        if (NotePendingRemoval(folderDir)) {
            continue;  // deleted for good; a removal still owed, not a folder
        }
        const std::optional<json> folderDoc = ReadJsonFile(*fs_, folderDir / kFolderFile);
        Folder folder;
        if (!folderDoc || !ReadRecord(*folderDoc, folder)) {
            // Not a folder of ours, or not one that can be read right now:
            // left alone rather than guessed at.
            foldersNotRead.insert(uidNameOf(folderDir));
            noteUnreadRecord(folderDir / kFolderFile);
            noteRemovalsInside(folderDir, 2);
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
    const OrderFile folderOrderFile = ReadOrderFile(*fs_, foldersRoot / kOrderFile, "folders");
    ApplyOrder(foundFolders, folderOrderFile.names);
    noteOrderFile("order:root", folderOrderFile, "folders", namesOf(foundFolders));
    noteUnread("order:root", folderOrderFile, foldersNotRead);

    // A snippet's directory, read, indexed, and noted if it came back
    // exactly as a save would write it.
    const auto readItem = [&](const std::filesystem::path& itemDir) -> std::optional<Item> {
        const std::optional<json> itemDoc = ReadJsonFile(*fs_, itemDir / kItemFile);
        if (!itemDoc) {
            return std::nullopt;
        }
        Item item;
        bool repaired = false;
        if (!ReadItemRecord(*itemDoc, item, repaired)) {
            return std::nullopt;
        }
        const uint64_t recordedItemId = item.id;
        item.id = claimId(item.id);
        // Every file beside the record belongs to it, whatever the record
        // calls them - which is what lets a snippet be moved by moving one
        // directory. Knowing where the directory is is therefore the whole
        // of image resolution (see FindImage); nothing inside it needs
        // listing.
        itemDirs_[item.id] = itemDir;
        // The hash of what was read, not a comparison of the text: the
        // record is where the bulk is, and serializing it to compare would
        // cost the load half a save. A key the record lacks reads as its
        // default and reads back the same next time, so it is no reason to
        // write; a value the read had to repair is (see FiniteOr) - left
        // unnoted so the next save writes it as it now is.
        if (item.id == recordedItemId && !repaired) {
            writtenItemHashes_[item.id] = HashItem(item);
        }
        return item;
    };
    // A canvas's directory, and the snippets in it, the same way - in
    // `folderId`, wherever the directory is.
    const auto readCanvas = [&](const std::filesystem::path& canvasDir, FolderId folderId) -> std::optional<Canvas> {
        const std::optional<json> canvasDoc = ReadJsonFile(*fs_, canvasDir / kCanvasFile);
        if (!canvasDoc) {
            return std::nullopt;
        }
        Canvas canvas;
        if (!ReadRecord(*canvasDoc, canvas)) {
            return std::nullopt;
        }
        const uint64_t recordedCanvasId = canvas.id;
        canvas.id = claimId(canvas.id);
        // Where it *is* beats what it says it belongs to. That is the whole
        // point of the tree: drag a canvas directory into another folder
        // while the app is closed and it is in that folder, with no record
        // anywhere left saying otherwise.
        canvas.folderId = folderId;
        canvasDirs_[canvas.id] = canvasDir;
        // A record that is not exactly what a save writes - a key it does
        // not write, say - is not noted here, and is rewritten.
        if (canvas.id == recordedCanvasId && ToJson(canvas) == *canvasDoc) {
            writtenFileText_["canvas:" + std::to_string(canvas.id)] = ToJson(canvas).dump(2);
        }

        // ...and the same, one level down, for the snippets inside it.
        std::vector<std::pair<std::string, Item>> foundItems;
        std::set<std::string> itemsNotRead;
        for (const std::filesystem::path& itemDir : WalkInto(canvasDir)) {
            if (NotePendingRemoval(itemDir)) {
                continue;
            }
            if (std::optional<Item> item = readItem(itemDir)) {
                foundItems.emplace_back(itemDir.filename().string(), std::move(*item));
            } else {
                itemsNotRead.insert(uidNameOf(itemDir));
                noteUnreadRecord(itemDir / kItemFile);
            }
        }
        // Item order is z-order, back to front - so a snippet dropped in by
        // hand, which the order file cannot know about, arrives in front
        // rather than buried.
        const OrderFile itemOrderFile = ReadOrderFile(*fs_, canvasDir / kOrderFile, "items");
        ApplyOrder(foundItems, itemOrderFile.names);
        noteOrderFile("order:canvas:" + std::to_string(canvas.id), itemOrderFile, "items", namesOf(foundItems));
        noteUnread("order:canvas:" + std::to_string(canvas.id), itemOrderFile, itemsNotRead);
        canvas.items.clear();
        canvas.items.reserve(foundItems.size());
        for (auto& [itemDirName, item] : foundItems) {
            canvas.items.push_back(std::move(item));
        }
        return canvas;
    };

    for (auto& [folderDirName, folder] : foundFolders) {
        const std::filesystem::path folderDir = foldersRoot / folderDirName;
        std::vector<std::pair<std::string, Canvas>> foundCanvases;
        std::set<std::string> canvasesNotRead;
        for (const std::filesystem::path& canvasDir : WalkInto(folderDir)) {
            if (NotePendingRemoval(canvasDir)) {
                continue;
            }
            if (std::optional<Canvas> canvas = readCanvas(canvasDir, folder.id)) {
                foundCanvases.emplace_back(canvasDir.filename().string(), std::move(*canvas));
            } else {
                canvasesNotRead.insert(uidNameOf(canvasDir));
                noteUnreadRecord(canvasDir / kCanvasFile);
                noteRemovalsInside(canvasDir, 1);
            }
        }
        const OrderFile canvasOrderFile = ReadOrderFile(*fs_, folderDir / kOrderFile, "canvases");
        ApplyOrder(foundCanvases, canvasOrderFile.names);
        noteOrderFile("order:folder:" + std::to_string(folder.id), canvasOrderFile, "canvases",
                      namesOf(foundCanvases));
        noteUnread("order:folder:" + std::to_string(folder.id), canvasOrderFile, canvasesNotRead);
        for (auto& [canvasDirName, canvas] : foundCanvases) {
            out.canvases.push_back(std::move(canvas));
        }
        out.folders.push_back(std::move(folder));
    }

    // ===== What was moved out of something deleted for good =====
    //
    // A canvas moved out of a folder, or a snippet out of a canvas, and the
    // folder or canvas then deleted for good before the save that moves the
    // directory: it is still inside the one being removed, and pending.json
    // says where it belongs (see RunPendingRemovals). It is read from where
    // it is and put there, and the next save moves its directory to match
    // - to the first folder or canvas not in the trash, should the one it
    // belongs to be gone too, and to a new one named "Recovered" should
    // there be none: a folder made, the canvas moved into it, and neither
    // saved before a crash leaves exactly that, and a rescue with nowhere
    // to go left the canvas unread for the next save to sweep away with the
    // folder it was in. Not into one in the trash: hidden there, what was
    // rescued was erased with it by the retention pass that follows the
    // load. Only what the record names: it is written, moves and all,
    // before the removal starts, so anything else inside went with what
    // was deleted.
    //
    // Canvases first, then snippets: a snippet can have been moved into a
    // canvas that is itself being rescued, and walked the other way round
    // - in the order of the uids - it found the canvas not there yet.
    //
    // One whose record is there and cannot be read now stays where it is,
    // and the walk counts as one that did not see everything - so that the
    // record keeps its move, and the removal waits for it (see
    // unobservedPending_). One with no record at all - a save stopped
    // before it landed - has nothing to rescue, and goes with what it is
    // in.
    const auto liveFolder = [&out](FolderId id) {
        return std::any_of(out.folders.begin(), out.folders.end(),
                           [id](const Folder& f) { return f.id == id && f.deletedAt == 0; });
    };
    const auto someFolder = [&]() -> FolderId {
        const auto live =
            std::find_if(out.folders.begin(), out.folders.end(), [](const Folder& f) { return f.deletedAt == 0; });
        if (live != out.folders.end()) {
            return live->id;
        }
        Folder folder;
        folder.id = claimId(0);
        folder.name = "Recovered";
        folder.createdAt = static_cast<int64_t>(std::time(nullptr));
        out.folders.push_back(std::move(folder));
        return out.folders.back().id;
    };
    const auto someCanvas = [&]() -> Canvas& {
        const auto live = std::find_if(out.canvases.begin(), out.canvases.end(), [&](const Canvas& c) {
            return c.deletedAt == 0 && liveFolder(c.folderId);
        });
        if (live != out.canvases.end()) {
            return *live;
        }
        Canvas canvas;
        canvas.id = claimId(0);
        canvas.name = "Recovered";
        canvas.folderId = someFolder();
        canvas.createdAt = static_cast<int64_t>(std::time(nullptr));
        out.canvases.push_back(std::move(canvas));
        return out.canvases.back();
    };
    // What `erasedDir`, `depth` below folders/, has inside that `moves` names.
    const auto rescueFrom = [&](const std::filesystem::path& erasedDir, std::ptrdiff_t depth) {
        for (const std::filesystem::path& inside : WalkInto(erasedDir)) {
            const std::optional<uint64_t> uid = UidFromDirectoryName(inside.filename().string());
            if (!uid || writtenPending_.erased.count(*uid) > 0) {
                continue;
            }
            const auto move = writtenPending_.moves.find(*uid);
            if (move == writtenPending_.moves.end()) {
                continue;
            }
            const uint64_t target = move->second;
            if (depth == 1) {
                if (std::optional<Canvas> canvas = readCanvas(inside, 0)) {
                    const bool there = std::any_of(out.folders.begin(), out.folders.end(),
                                                   [target](const Folder& f) { return f.id == target; });
                    canvas->folderId = there ? target : someFolder();
                    out.canvases.push_back(std::move(*canvas));
                } else if (fs_->LinkStatus(inside / kCanvasFile) != FileSystem::Kind::None) {
                    walkedEverything_ = false;
                }
            } else if (std::optional<Item> item = readItem(inside)) {
                const auto canvas = std::find_if(out.canvases.begin(), out.canvases.end(),
                                                 [target](const Canvas& c) { return c.id == target; });
                Canvas& home = canvas != out.canvases.end() ? *canvas : someCanvas();
                home.items.push_back(std::move(*item));  // in front, like anything unlisted
            } else if (fs_->LinkStatus(inside / kItemFile) != FileSystem::Kind::None) {
                walkedEverything_ = false;
            }
        }
    };
    const std::map<uint64_t, std::filesystem::path> erasedDirs = pendingRemovals_;
    for (const std::ptrdiff_t depth : {1, 2}) {  // a snippet's directory holds no others
        for (const auto& [erasedUid, erasedDir] : erasedDirs) {
            (void)erasedUid;
            const std::filesystem::path relative = erasedDir.lexically_relative(foldersRoot);
            if (std::distance(relative.begin(), relative.end()) == depth) {
                rescueFrom(erasedDir, depth);
            }
        }
    }
}

std::optional<CanvasManagerSnapshot> LibraryStore::Load() const {
    // The tree is the library, and library.json is a pointer file beside
    // it. That ordering decides everything below: whatever is wrong with
    // library.json - missing, not JSON, a field of the wrong type - is a
    // reason to default what it holds, and never a reason to report the
    // library absent while a tree is sitting there. Reporting it absent
    // starts the app fresh, and a fresh library's first save would then
    // retire every directory it found on disk as something the library no
    // longer holds.

    // Before anything is read: nothing of a newer library is this build's
    // to touch.
    if (WrittenByANewerVersion()) {
        return std::nullopt;
    }
    const std::filesystem::path foldersRoot = FoldersRoot();
    const bool treeExists = fs_->Status(foldersRoot) == FileSystem::Kind::Directory;

    json doc = json::object();
    const std::optional<std::string> text = ReadWaitingOutAHold(*fs_, rootDir_ / "library.json");
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

    // A load replaces everything this store believed about the tree. The walk
    // below refills the index, and re-establishes the write-state record by
    // record: each one that came back exactly as a save would write it is
    // noted as already written, so the first save afterwards costs what
    // changed, like every save after it. Anything the load had to repair -
    // an id reassigned, an order file that disagreed with the directories
    // beside it - is left out, and so is written. See writtenItemHashes_.
    folderDirs_.clear();
    canvasDirs_.clear();
    itemDirs_.clear();
    pendingRemovals_.clear();  // refilled from pending.json as the walk finds what it names
    unreadPlaces_.clear();     // refilled by the walk
    ReadPendingFile();
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

    walkedEverything_ = true;
    readEverything_ = true;
    ReadTree(foldersRoot, snapshot);
    NoteUnobservedPending(IdsIn(LibraryView{snapshot.folders, snapshot.canvases, 0, 0}), walkedEverything_);

    // Dangling means "repair", not "refuse". A tree that people are invited
    // to rearrange is routinely a little out of date, and refusing to open a library
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
    if (LibraryJson(snapshot.currentFolderId, snapshot.currentCanvasId) == doc) {
        writtenFileText_["library"] = LibraryJson(snapshot.currentFolderId, snapshot.currentCanvasId).dump(2);
    }

    return snapshot;
}

void LibraryStore::IndexTreeFromDisk(const std::filesystem::path& foldersRoot) const {
    folderDirs_.clear();
    canvasDirs_.clear();
    itemDirs_.clear();
    // What was deleted for good is not to be indexed - and pending.json is
    // not to be written over by a store that has not read it.
    ReadPendingFile();
    walkedEverything_ = true;
    for (const std::filesystem::path& folderDir : WalkInto(foldersRoot)) {
        if (NotePendingRemoval(folderDir)) {
            continue;
        }
        if (const std::optional<json> doc = ReadJsonFile(*fs_, folderDir / kFolderFile)) {
            if (const uint64_t id = ReadId(*doc, "id"); id != 0) {
                folderDirs_.emplace(id, folderDir);
            }
        }
        for (const std::filesystem::path& canvasDir : WalkInto(folderDir)) {
            if (NotePendingRemoval(canvasDir)) {
                continue;
            }
            if (const std::optional<json> doc = ReadJsonFile(*fs_, canvasDir / kCanvasFile)) {
                if (const uint64_t id = ReadId(*doc, "id"); id != 0) {
                    canvasDirs_.emplace(id, canvasDir);
                }
            }
            // Item directories too, and across every canvas rather than
            // only this one: moving a snippet to another canvas is a move
            // of its directory, and finding where it currently is is what
            // makes that a move rather than a copy plus an orphan.
            for (const std::filesystem::path& itemDir : WalkInto(canvasDir)) {
                if (NotePendingRemoval(itemDir)) {
                    continue;
                }
                if (const std::optional<json> doc = ReadJsonFile(*fs_, itemDir / kItemFile)) {
                    if (const uint64_t id = ReadId(*doc, "id"); id != 0) {
                        itemDirs_.emplace(id, itemDir);
                    }
                }
            }
        }
    }
    treeIndexed_ = true;
    // Nothing moved out of what was deleted for good is rescued here, with
    // no library to rescue it into: it is kept, and keeps the directory it
    // is in standing, as a store that never read the tree must leave it.
    std::unordered_set<uint64_t> seen;
    for (const auto* index : {&folderDirs_, &canvasDirs_, &itemDirs_}) {
        for (const auto& [id, path] : *index) {
            seen.insert(id);
        }
    }
    NoteUnobservedPending(seen, /*sawEverything=*/false);
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
bool LibraryStore::Save(const LibraryView& view) const {
    if (writtenByANewerVersion_) {
        return false;  // not this build's to write - see WrittenByANewerVersion
    }
    oversizedRecords_.clear();
    const std::filesystem::path foldersRoot = FoldersRoot();
    if (!IsOurs(foldersRoot)) {
        // folders/ is a junction: there is nowhere of the library's to
        // write the tree. Nothing is written, not library.json either -
        // a pointer file over a tree this store will not write is worth
        // nothing - and the save fails until the link is gone.
        return false;
    }
    fs_->CreateDirectories(foldersRoot);

    // What the library holds, by id - what the retirement pass below is
    // measured against, and what everything remembered is pruned to.
    std::unordered_set<uint64_t> liveFolderIds;
    std::unordered_set<uint64_t> liveCanvasIds;
    std::unordered_set<uint64_t> liveItemIds;
    // ...and by picture: which snippet each live picture (and its thumbnail)
    // belongs to, for the staging pass.
    std::unordered_map<std::string, uint64_t> pictureOwner;
    // ...and by folder, once, so that placing a folder's canvases below is
    // a walk of its own canvases rather than of every canvas per folder.
    std::unordered_map<uint64_t, std::vector<const Canvas*>> canvasesByFolder;
    for (const Folder& folder : view.folders) {
        liveFolderIds.insert(folder.id);
    }
    for (const Canvas& canvas : view.canvases) {
        liveCanvasIds.insert(canvas.id);
        canvasesByFolder[canvas.folderId].push_back(&canvas);
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
        // At the write, not only at placement: the directory was real when
        // it was indexed, and a junction put in its place since is a link
        // all the same. Checked only when there is something to write, so
        // that a no-op save costs no trip to the filesystem for it.
        if (!IsOurs(path) || !WriteFileAtomically(*fs_, path, text)) {
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
    bool wroteEverything = writeIfChanged("library", rootDir_ / "library.json",
                                          LibraryJson(view.currentFolderId, view.currentCanvasId).dump(2),
                                          /*force=*/false);

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
    // be created, or on the way there: what is behind a link is not the
    // store's to write into (see IsOurs), so the record - and everything
    // under it - is skipped, the save reports failure, and the next one
    // tries again.
    //
    // `misplaced` is true when the rename that failed was a *move* - to a
    // different parent: a snippet to another canvas, a canvas to another
    // folder. That is not a stale label. The tree is the index, so a record
    // left under its old parent is a move the next load undoes. The record
    // is still written where the directory is, so nothing in it is lost,
    // but the save reports failure so that it is retried and said on
    // screen rather than acknowledged.
    struct Placed {
        std::filesystem::path dir;
        bool kept;
        bool usable = true;
        bool misplaced = false;
    };
    const auto placeDirectory = [this](std::map<uint64_t, std::filesystem::path>& index, uint64_t id,
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
            // Where it is, whatever its name says - what a rename that does
            // not happen leaves. See Placed::misplaced for why a move that
            // did not happen is more than a stale label.
            const auto stayed = [&old, &wanted] {
                return Placed{old, true, /*usable=*/true, /*misplaced=*/old.parent_path() != wanted.parent_path()};
            };
            if (!IsOurs(wanted)) {
                // A link is in the way of where its name says. It stays
                // where it is, indexed and real, with a stale label - the
                // same as a rename that failed.
                return stayed();
            }
            fs_->CreateDirectories(wanted.parent_path());
            if (fs_->Rename(old, wanted)) {
                // Everything indexed inside it moved with it - and every
                // removal still owed inside it, which is not indexed but
                // is a path all the same. Left at its old spelling, the
                // next pass found nothing there and took the removal for
                // done, with the directory sitting under the new name.
                for (std::map<uint64_t, std::filesystem::path>* nested : inside) {
                    Rehome(*nested, old, wanted);
                }
                Rehome(pendingRemovals_, old, wanted);
                it->second = wanted;
                return Placed{wanted, true};
            }
            if (fs_->Exists(old)) {
                return stayed();
            }
            // Gone from where the index said - rearranged under a running
            // instance, which is undefined but shouldn't lose what is in
            // memory. Rebuilt from that, below.
        }
        if (!IsOurs(wanted)) {
            return Placed{wanted, false, /*usable=*/false};  // a link there, or on the way: never ours
        }
        fs_->CreateDirectories(wanted);
        index[id] = wanted;
        return Placed{wanted, false};
    };

    // ===== 1. Place and write everything the library holds =====

    // What Load could not read keeps its place in each order file - see
    // unreadPlaces_.
    const auto keepUnread = [this](const std::string& key, std::vector<std::string>& order) {
        if (const auto it = unreadPlaces_.find(key); it != unreadPlaces_.end()) {
            KeepUnreadPlaces(order, it->second.order, it->second.unread);
        }
    };
    std::vector<std::string> folderOrder;
    folderOrder.reserve(view.folders.size());
    const std::vector<const Canvas*> noCanvases;
    for (const Folder& folder : view.folders) {
        const Placed placedFolder =
            placeDirectory(folderDirs_, folder.id, foldersRoot / MakeSlug(folder.name, folder.id),
                           {&canvasDirs_, &itemDirs_});
        const std::filesystem::path& folderDir = placedFolder.dir;
        folderOrder.push_back(FormatUid(folder.id));
        if (!placedFolder.usable) {
            wroteEverything = false;
            continue;
        }
        wroteEverything &= !placedFolder.misplaced;
        const bool folderRecorded = writeIfChanged("folder:" + std::to_string(folder.id), folderDir / kFolderFile,
                                                   ToJson(folder).dump(2), !placedFolder.kept);
        wroteEverything &= folderRecorded;
        // A directory with no record in it at all - made fresh, and its
        // first record did not land - is not a place to move anything into.
        // Load does not read it, so what moved in would be lost to a
        // restart until the record landed: a snippet saved long before,
        // moved into a canvas just made. Everything that belongs under it
        // waits where it is for the save that writes the record; a record
        // that failed to be rewritten has its last version still there.
        if (!folderRecorded && !fs_->Exists(folderDir / kFolderFile)) {
            continue;
        }

        std::vector<std::string> canvasOrder;
        const auto inFolder = canvasesByFolder.find(folder.id);
        for (const Canvas* canvasPtr : inFolder != canvasesByFolder.end() ? inFolder->second : noCanvases) {
            const Canvas& canvas = *canvasPtr;
            const Placed placedCanvas = placeDirectory(
                canvasDirs_, canvas.id, folderDir / MakeSlug(canvas.name, canvas.id), {&itemDirs_});
            const std::filesystem::path& canvasDir = placedCanvas.dir;
            canvasOrder.push_back(FormatUid(canvas.id));
            if (!placedCanvas.usable) {
                wroteEverything = false;
                continue;
            }
            wroteEverything &= !placedCanvas.misplaced;
            const bool canvasRecorded = writeIfChanged("canvas:" + std::to_string(canvas.id), canvasDir / kCanvasFile,
                                                       ToJson(canvas).dump(2), !placedCanvas.kept);
            wroteEverything &= canvasRecorded;
            if (!canvasRecorded && !fs_->Exists(canvasDir / kCanvasFile)) {
                continue;  // see the folder's record above
            }

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
                wroteEverything &= !placedItem.misplaced;

                // The one place where skipping the work is worth real time,
                // and the only one that answers "changed?" without
                // serializing - see HashItem. Everything else in this
                // function is a handful of scalars.
                const uint64_t hash = HashItem(item);
                const auto known = writtenItemHashes_.find(item.id);
                if (placedItem.kept && known != writtenItemHashes_.end() && known->second == hash) {
                    continue;
                }
                // A record past what Load reads is not written: acknowledged,
                // it would vanish at the next start with everything in it,
                // where refused, the save fails and says so, and the last
                // record that fitted stays on disk to load.
                const std::string text = ToJson(item).dump(2);
                const bool fits = text.size() <= kMaxRecordBytes;
                if (!fits) {
                    oversizedRecords_.insert(item.id);
                }
                const bool recordWritten =
                    fits && IsOurs(itemDir / kItemFile) && WriteFileAtomically(*fs_, itemDir / kItemFile, text);
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
                if (!placedItem.kept || !recordWritten || !IsOurs(itemDir)) {
                    continue;  // a fresh directory holds nothing to collect
                }
                std::unordered_set<std::string> named;
                for (const Layer& layer : item.layers) {
                    if (!layer.imageFile.empty()) {
                        named.insert(layer.imageFile);
                        named.insert(ThumbnailFilename(layer.imageFile));
                    }
                }
                for (const FileSystem::Entry& entry : Listing(*fs_, itemDir)) {
                    if (entry.kind != FileSystem::Kind::File) {
                        continue;
                    }
                    const std::string name = entry.name.string();
                    // Only what this store writes: its own pictures and
                    // their thumbnails. Anything else beside the record - a
                    // note someone dropped in by hand, say - is not the
                    // store's to collect, whatever it is called.
                    if (name == kItemFile || named.count(name) > 0 || !IsPictureFilename(name)) {
                        continue;
                    }
                    fs_->Remove(itemDir / entry.name);
                }
            }
            keepUnread("order:canvas:" + std::to_string(canvas.id), itemOrder);
            wroteEverything &= writeIfChanged("order:canvas:" + std::to_string(canvas.id), canvasDir / kOrderFile,
                                               OrderFileJson("items", itemOrder).dump(2), !placedCanvas.kept);
        }
        keepUnread("order:folder:" + std::to_string(folder.id), canvasOrder);
        wroteEverything &= writeIfChanged("order:folder:" + std::to_string(folder.id), folderDir / kOrderFile,
                                           OrderFileJson("canvases", canvasOrder).dump(2), !placedFolder.kept);
    }
    keepUnread("order:root", folderOrder);
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
        // (see Remove) - a removal that waited for a move out of it, which
        // the placing above has made, included. One that cannot be recorded
        // fails the save, since nothing on disk then says it was asked for.
        // Not part of the index, so nothing under it is retired below: it
        // was deleted, not lost. The paths are kept current through every
        // rename and retirement of a parent (see Rehome), so "gone" there
        // means gone, not moved.
        wroteEverything &= RunPendingRemovals(view);

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
                if (!IsOurs(path)) {
                    continue;
                }
                // ...and where it would go: retired/ can be a junction as
                // easily as anything else, and a directory moved through
                // one has left the library.
                const std::filesystem::path destination = retiredRoot / path.lexically_relative(foldersRoot);
                if (!IsOurs(destination)) {
                    continue;
                }
                fs_->CreateDirectories(destination.parent_path());
                const bool moved = fs_->Exists(destination) ? MergeDirectoryInto(*fs_, path, destination)
                                                            : fs_->Rename(path, destination);
                // A move that failed leaves the directory where it is, and
                // indexed, for the next save to take another run at. One
                // that went takes the removals still owed inside it along:
                // retired/ is never read, but a directory deleted for good
                // is finished there too rather than left as a remainder.
                if (moved) {
                    Rehome(pendingRemovals_, path, destination);
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
    // retired/staging/ rather than deleted: it is the capture of a snippet
    // deleted for good before it was ever saved, which nobody wants back -
    // or the remains of a crash between writing a picture and saving the
    // record that names it, which is exactly the screenshot that cannot be
    // taken again, and the two cannot be told apart from here. A picture
    // is written the moment it is taken so that it survives such a crash;
    // deleting it at the next start would undo that promise. A move that
    // fails stays, readably - see FindImage.
    //
    // A file of the same name already at home is newer than the one
    // waiting, never older: a picture is written to staging only while its
    // snippet has no directory, and to the directory from the moment it
    // has one (see ImageHome). So the waiting one is what a move that
    // failed left behind - a file held open at the save that made the
    // directory - and the painting has been saved again at home since.
    // Moving it in would put the older pixels back over the newer, under a
    // layer that believes itself saved; it is set aside instead.
    const std::filesystem::path stagingDir = rootDir_ / kStagingDir;
    if (fs_->Exists(stagingDir) && IsOurs(stagingDir)) {
        for (const FileSystem::Entry& entry : Listing(*fs_, stagingDir)) {
            if (entry.kind != FileSystem::Kind::File) {
                continue;
            }
            const std::filesystem::path waiting = stagingDir / entry.name;
            const std::string name = entry.name.string();
            if (const auto owner = pictureOwner.find(name); owner != pictureOwner.end()) {
                const auto home = itemDirs_.find(owner->second);
                if (home == itemDirs_.end() || !IsOurs(home->second)) {
                    continue;
                }
                if (!fs_->Exists(home->second / name)) {
                    fs_->Rename(waiting, home->second / name);
                    continue;
                }
            } else if (!walkedEverything_ || !readEverything_) {
                // Perhaps the picture of a snippet whose record another
                // program held at the start - saved, and its picture not
                // yet moved in when the process stopped. Set aside, it was
                // missing from the snippet once the record could be read.
                continue;
            }
            const std::filesystem::path setAside = rootDir_ / kRetiredDir / kStagingDir;
            if (!IsOurs(setAside)) {
                continue;  // stays in staging, readably, rather than leaving the library
            }
            fs_->CreateDirectories(setAside);
            fs_->Rename(waiting, setAside / name);
        }
        // And gone once drained - a removal that only succeeds on an empty
        // directory, so whatever stayed keeps it. Left standing, it was an
        // empty folder in the library that a person looking in could only
        // wonder about.
        fs_->Remove(stagingDir);
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
    // Nothing is written into a library a newer build wrote - a painted
    // layer no more than a screenshot, which alone was refused.
    if (writtenByANewerVersion_) {
        return std::nullopt;
    }
    const std::filesystem::path home = ImageHome(itemId);
    if (!IsOurs(home)) {
        return std::nullopt;  // staging, or the snippet's directory, is behind a link: not written
    }
    ++writeGeneration_;
    if (!EncodeQoiToFile(*fs_, home / filename, pixelsRGBA, width, height)) {
        return std::nullopt;
    }
    // And a thumbnail beside it, so the Overview never has to decode the
    // full image to draw a 200px tile. Best-effort, and deliberately not
    // part of this function's success: the capture is the thing that must
    // survive, and it already has. Taken from the caller's own pixels
    // rather than through a DecodedImage, which would mean copying eight
    // megabytes to make forty kilobytes.
    const DecodedImage small = DownscaleToFit(pixelsRGBA, width, height, kThumbnailMaxExtent);
    EncodeQoiToFile(*fs_, home / ThumbnailFilename(filename), small.pixelsRGBA.data(), small.width, small.height);
    return filename;
}

std::string LibraryStore::ThumbnailFilename(const std::string& imageFilename) {
    const size_t dot = imageFilename.find_last_of('.');
    const std::string stem = dot == std::string::npos ? imageFilename : imageFilename.substr(0, dot);
    return stem + ".thumb.qoi";
}

bool LibraryStore::SaveThumbnail(uint64_t itemId, const std::string& imageFilename, const DecodedImage& image) const {
    if (writtenByANewerVersion_ || !IsPlainFilename(imageFilename) || image.width <= 0 || image.height <= 0) {
        return false;
    }
    const DecodedImage small = DownscaleToFit(image, kThumbnailMaxExtent);
    // Beside the image it belongs to, wherever that currently is - which for
    // a picture still in staging is staging, so the two travel together.
    const std::filesystem::path beside = FindImage(itemId, imageFilename).parent_path();
    if (!IsOurs(beside)) {
        return false;
    }
    ++writeGeneration_;
    return EncodeQoiToFile(*fs_, beside / ThumbnailFilename(imageFilename), small.pixelsRGBA.data(), small.width,
                           small.height);
}

std::optional<DecodedImage> LibraryStore::LoadThumbnail(uint64_t itemId, const std::string& imageFilename) const {
    if (imageFilename.empty()) {
        return std::nullopt;
    }
    return DecodeQoiFromFile(*fs_, FindImage(itemId, ThumbnailFilename(imageFilename)));
}

std::optional<DecodedImage> LibraryStore::LoadImage(uint64_t itemId, const std::string& filename) const {
    if (filename.empty()) {
        return std::nullopt;
    }
    return DecodeQoiFromFile(*fs_, FindImage(itemId, filename));
}

bool LibraryStore::HasImage(uint64_t itemId, const std::string& filename) const {
    return !filename.empty() && fs_->Exists(FindImage(itemId, filename));
}

}  // namespace sz::core::persistence
