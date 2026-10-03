#include "core/persistence/library_store.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/canvas/item_geometry.h"
#include "core/diagnostics/timeline.h"
#include "core/util/timestamp_name.h"

namespace sz::core::persistence {

namespace {

using nlohmann::json;

// "Sztl", as the file's application_id: what tells a library of ours from
// any other SQLite file that happens to be where the library should be.
constexpr int64_t kApplicationId = 0x537A746C;

// How long a statement waits for a lock another program holds on the file
// before it gives up - another copy of the app holding it, on another
// computer sharing a network folder, say. Short, because writes run on the
// render thread; a change whose write gives up is not made.
constexpr int kBusyTimeoutMs = 250;

// What SQLite puts before each page in the WAL.
constexpr int64_t kWalFrameHeaderBytes = 24;

// The whole schema. See the class comment for what each table is.
//
// Foreign keys, so that a canvas is always in a folder and a snippet on a
// canvas; a write puts what holds a thing in before the thing. The trigger
// is what takes a snippet's picture with it, however the snippet goes.
constexpr const char* kSchema = R"(
CREATE TABLE meta (
    key TEXT PRIMARY KEY,
    value INTEGER NOT NULL
);
CREATE TABLE folders (
    id INTEGER PRIMARY KEY,
    position INTEGER NOT NULL,
    name TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    deleted_at INTEGER NOT NULL
);
CREATE TABLE canvases (
    id INTEGER PRIMARY KEY,
    folder_id INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    name TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    deleted_at INTEGER NOT NULL
);
CREATE INDEX canvases_by_folder ON canvases(folder_id);
CREATE TABLE items (
    id INTEGER PRIMARY KEY,
    canvas_id INTEGER NOT NULL REFERENCES canvases(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    record TEXT NOT NULL,
    strokes BLOB NOT NULL
);
CREATE INDEX items_by_canvas ON items(canvas_id);
CREATE TABLE pictures (
    item_id INTEGER PRIMARY KEY,
    width INTEGER NOT NULL,
    height INTEGER NOT NULL,
    pixels BLOB NOT NULL,
    thumbnail BLOB
);
CREATE TRIGGER items_take_their_pictures AFTER DELETE ON items
BEGIN
    DELETE FROM pictures WHERE item_id = OLD.id;
END;
)";

// ===== SQLite, thinly =====

std::string Utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool Exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

// A write's transaction, rolled back unless it is committed: by a failure
// returning early, and by an exception on its way through, which had left
// it open - and every write after it failing to begin one.
class WriteTransaction {
public:
    explicit WriteTransaction(sqlite3* db) : db_(db), open_(Exec(db, "BEGIN IMMEDIATE")) {}
    ~WriteTransaction() {
        if (open_) {
            Exec(db_, "ROLLBACK");
        }
    }
    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;

    bool Begun() const { return open_; }
    // A commit that fails leaves it to be rolled back.
    bool Commit() {
        open_ = !Exec(db_, "COMMIT");
        return !open_;
    }

private:
    sqlite3* db_;
    bool open_;
};

// What the file itself is wrong with - not a database, or a damaged one -
// as against a moment's trouble reaching it.
bool IsDamage(int rc) {
    const int primary = rc & 0xFF;
    return primary == SQLITE_NOTADB || primary == SQLITE_CORRUPT;
}

// One prepared statement, finalized with it. Every column and parameter
// accessor is a thin pass-through; what they add is binding a uint64_t id
// and an empty blob (which sqlite3_bind_blob with a null pointer would
// store as NULL) the way the schema wants them.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void Bind(int index, int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
    void Bind(int index, uint64_t value) { sqlite3_bind_int64(stmt_, index, static_cast<int64_t>(value)); }
    void Bind(int index, int value) { sqlite3_bind_int64(stmt_, index, value); }
    void Bind(int index, std::string_view text) {
        sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    }
    void BindBlob(int index, const void* data, size_t size) {
        if (size == 0) {
            sqlite3_bind_zeroblob(stmt_, index, 0);
        } else {
            sqlite3_bind_blob64(stmt_, index, data, size, SQLITE_TRANSIENT);
        }
    }

    // SQLITE_ROW, SQLITE_DONE or an error; a statement that failed to
    // prepare answers as an error.
    int Step() { return stmt_ != nullptr ? sqlite3_step(stmt_) : SQLITE_ERROR; }
    // Steps a statement that returns no rows, and readies it to run again
    // with fresh parameters. One that failed to prepare fails here too:
    // sqlite3_clear_bindings, alone of what this class calls, does not
    // take a null statement in this build (no SQLITE_ENABLE_API_ARMOR),
    // and a table the file no longer had crashed the write naming it.
    bool Run() {
        if (stmt_ == nullptr) {
            return false;
        }
        const int rc = sqlite3_step(stmt_);
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
        return rc == SQLITE_DONE;
    }

    int64_t Int(int column) const { return sqlite3_column_int64(stmt_, column); }
    bool IsNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }
    std::string Text(int column) const {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, column));
        return text != nullptr ? std::string(text, static_cast<size_t>(sqlite3_column_bytes(stmt_, column)))
                               : std::string();
    }
    std::vector<uint8_t> Blob(int column) const {
        const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, column));
        const int size = sqlite3_column_bytes(stmt_, column);
        return data != nullptr && size > 0 ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>();
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// The first column of a query's first row, as an integer.
int ReadInt(sqlite3* db, const char* sql, int64_t& out) {
    Statement statement(db, sql);
    const int rc = statement.Step();
    if (rc == SQLITE_ROW) {
        out = statement.Int(0);
        return SQLITE_OK;
    }
    return rc == SQLITE_DONE ? SQLITE_OK : sqlite3_extended_errcode(db);
}

// ===== A snippet's record =====
//
// Everything about a snippet but where it is (canvas_id, position), its
// strokes (their own blob, below) and whether it has a picture (a row in
// pictures) is one JSON object. A new field is a new key, read with a
// default, and needs no change to the schema.
//
// JSON has no infinity and no NaN, but it has 1e100, which becomes
// infinity the moment it is read as a float - and one infinite coordinate
// poisons every bounding box, tessellation and hit test it meets. Every
// float a record holds comes through here: a value that is not finite reads
// as the field's default, and the fields with a meaningful range are held
// inside it. `repaired` says a value had to be changed, so that the row is
// written back as it now reads.

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

// When a folder, canvas or snippet was deleted, as stored: 0 for not
// deleted, else seconds since the epoch, from the clock at the delete.
// Anything but 0 is deleted, whatever the time: whether it was is the
// stamp's meaning, and the time only says when. A time the C runtime
// cannot turn into a date - before 1970, or past the year 3000 - reads
// as `now`; the Show deleted tooltip used to end the app on one.
//
// One ahead of `now` is kept as it is. It is what a clock that runs
// behind at start makes of every recent delete, and an upper bound near
// `now` once read those as not deleted and wrote that back: fixing the
// clock did not undo it. Kept, such a stamp is purged by retention that
// much later, which is the side that loses nothing.
int64_t DeletionStamp(int64_t stored, int64_t now, bool& repaired) {
    // 3000-01-01 00:00:00 UTC.
    constexpr int64_t kLastDatable = 32503680000;
    if (stored == 0 || (stored > 0 && stored < kLastDatable)) {
        return stored;
    }
    repaired = true;
    return now;
}

json RectJson(const Rect& r) { return json{{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}}; }

// Held to the range a screen could have, as the native and anchor sizes
// beside it are: finite is not enough, since 1e30 is finite and places a
// snippet nowhere, and a size below zero is no size.
void ReadRect(const json& j, const char* key, Rect& out, bool& repaired) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return;
    }
    if (!it->is_object()) {
        repaired = true;
        return;
    }
    out.x = ClampedOr(*it, "x", 0.0f, -kMaxSensibleExtent, kMaxSensibleExtent, repaired);
    out.y = ClampedOr(*it, "y", 0.0f, -kMaxSensibleExtent, kMaxSensibleExtent, repaired);
    out.w = ClampedOr(*it, "w", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.h = ClampedOr(*it, "h", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
}

std::string ItemRecord(const Item& item) {
    // Whether there are pixels is the pictures table's to say.
    json picture{
        {"opacity", item.picture.opacity},
        {"tintColorRGBA", item.picture.tintColorRGBA},
        {"showsPlaceholder", item.picture.showsPlaceholder},
        {"placeholderHue", item.picture.placeholderHue},
    };
    const json j{
        {"name", item.name},
        {"createdAt", item.createdAt},
        {"deletedAt", item.deletedAt},
        {"hasBackground", item.hasBackground},
        {"rect", RectJson(item.rect)},
        {"nativeW", item.nativeW},
        {"nativeH", item.nativeH},
        {"foregroundOpacity", item.foregroundOpacity},
        {"keepAspect", item.keepAspect},
        {"isFullscreen", item.isFullscreen},
        {"isFullscreenStretch", item.isFullscreenStretch},
        {"anchorRect", RectJson(item.anchorRect)},
        {"anchorDisplayWidth", item.anchorDisplayWidth},
        {"anchorDisplayHeight", item.anchorDisplayHeight},
        {"minimized", item.minimized},
        {"pinned", item.pinned},
        {"picture", std::move(picture)},
        {"noteText", item.noteText},
        {"noteTextColorRGBA", item.noteTextColorRGBA},
        {"noteTextSizePx", item.noteTextSizePx},
    };
    // A name or note that is not UTF-8 is written with U+FFFD for what
    // cannot be read, as the settings file is: the strict dump throws, and
    // a snippet that could not be written would take every write with it.
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// A value of the wrong type is the default, not an exception: json::value
// throws on one, which is what `Value` is for.
template <typename T>
T Value(const json& j, const char* key, T fallback, bool& repaired) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return fallback;
    }
    try {
        return it->get<T>();
    } catch (const json::exception&) {
        repaired = true;
        return fallback;
    }
}

void ReadItemRecord(std::string_view text, int64_t now, Item& out, bool& repaired) {
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        repaired = true;
        return;  // a snippet with every field at its default, rather than none
    }
    out.name = Value(j, "name", std::string(), repaired);
    out.createdAt = Value(j, "createdAt", int64_t{0}, repaired);
    out.deletedAt = DeletionStamp(Value(j, "deletedAt", int64_t{0}, repaired), now, repaired);
    out.hasBackground = Value(j, "hasBackground", false, repaired);
    ReadRect(j, "rect", out.rect, repaired);
    // A snippet with no size is one that cannot be seen or picked; it gets
    // the smallest one a snippet can have. Not the anchor's: a zero anchor
    // is "not yet anchored".
    if (out.rect.w <= 0.0f || out.rect.h <= 0.0f) {
        out.rect = GrowRectToMinimumSize(out.rect);
        repaired = true;
    }
    out.nativeW = ClampedOr(j, "nativeW", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.nativeH = ClampedOr(j, "nativeH", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.foregroundOpacity = ClampedOr(j, "foregroundOpacity", 1.0f, 0.0f, 1.0f, repaired);
    out.keepAspect = Value(j, "keepAspect", true, repaired);
    out.isFullscreen = Value(j, "isFullscreen", false, repaired);
    out.isFullscreenStretch = Value(j, "isFullscreenStretch", false, repaired);
    // A zero anchor is "not yet anchored" (see Item::anchorRect), which
    // CanvasManager::SyncItemsToDisplaySize fills in from `rect`.
    ReadRect(j, "anchorRect", out.anchorRect, repaired);
    out.anchorDisplayWidth = ClampedOr(j, "anchorDisplayWidth", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.anchorDisplayHeight = ClampedOr(j, "anchorDisplayHeight", 0.0f, 0.0f, kMaxSensibleExtent, repaired);
    out.minimized = Value(j, "minimized", false, repaired);
    out.pinned = Value(j, "pinned", false, repaired);
    if (const auto it = j.find("picture"); it != j.end()) {
        if (it->is_object()) {
            out.picture.opacity = ClampedOr(*it, "opacity", 0.0f, 0.0f, 1.0f, repaired);
            out.picture.tintColorRGBA = Value(*it, "tintColorRGBA", uint32_t{0xFFFFFFFF}, repaired);
            out.picture.showsPlaceholder = Value(*it, "showsPlaceholder", false, repaired);
            out.picture.placeholderHue = ClampedOr(*it, "placeholderHue", 0.0f, 0.0f, 360.0f, repaired);
        } else {
            repaired = true;
        }
    }
    out.noteText = Value(j, "noteText", std::string(), repaired);
    out.noteTextColorRGBA = Value(j, "noteTextColorRGBA", uint32_t{0xFFFFFFFF}, repaired);
    out.noteTextSizePx = ClampedOr(j, "noteTextSizePx", 17.0f, kNoteTextSizeMin, kNoteTextSizeMax, repaired);
}

// ===== A snippet's strokes =====
//
// One blob per snippet rather than a row per stroke or a JSON array: a
// stroke point as JSON is an object of two keys, and an ordinary canvas
// holds tens of thousands of them. Little-endian throughout:
//
//   u8  format (2)
//   u32 stroke count, then per stroke:
//       u32 colorRGBA, f32 width, u8 corners, u32 point count,
//       then per point: f32 x, f32 y
//
// Format 1, up to 0.2.1, had no corners byte: see CornersOfAFormat1Stroke.
static_assert(std::endian::native == std::endian::little, "the stroke blob is written as the host stores it");
constexpr uint8_t kStrokeBlobFormat = 2;
constexpr uint8_t kStrokeBlobFormat1 = 1;

template <typename T>
void Put(std::vector<uint8_t>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

std::vector<uint8_t> StrokesBlob(const std::vector<Stroke>& strokes) {
    size_t size = 1 + 4;
    for (const Stroke& stroke : strokes) {
        size += 13 + stroke.points.size() * 8;
    }
    std::vector<uint8_t> out;
    out.reserve(size);
    out.push_back(kStrokeBlobFormat);
    Put(out, static_cast<uint32_t>(strokes.size()));
    for (const Stroke& stroke : strokes) {
        Put(out, stroke.colorRGBA);
        Put(out, stroke.width);
        Put(out, static_cast<uint8_t>(stroke.corners));
        Put(out, static_cast<uint32_t>(stroke.points.size()));
        for (const StrokePoint& point : stroke.points) {
            Put(out, point.x);
            Put(out, point.y);
        }
    }
    return out;
}

class BlobReader {
public:
    explicit BlobReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    template <typename T>
    bool Get(T& out) {
        if (bytes_.size() - at_ < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, bytes_.data() + at_, sizeof(T));
        at_ += sizeof(T);
        return true;
    }
    size_t Left() const { return bytes_.size() - at_; }

private:
    const std::vector<uint8_t>& bytes_;
    size_t at_ = 0;
};

// A stroke from before strokes said what their corners are, when every
// turn below about 120 degrees was mitered: the rectangle tool's is the
// one whose corners were meant, and it is told by its shape - every
// segment exactly level or plumb, which a fitted freehand line with a
// turn in it never is. So is what the eraser left of one. A straight line
// matches as well, and has no corner to draw either way.
StrokeCorners CornersOfAFormat1Stroke(const std::vector<StrokePoint>& points) {
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (points[i].x != points[i + 1].x && points[i].y != points[i + 1].y) {
            return StrokeCorners::Round;
        }
    }
    return StrokeCorners::Sharp;
}

// What the blob holds, as far as it can be read: a blob cut short keeps the
// strokes before the cut, and a value that cannot be used is the default,
// as in a record.
std::vector<Stroke> ReadStrokes(const std::vector<uint8_t>& blob, bool& repaired) {
    std::vector<Stroke> strokes;
    BlobReader in(blob);
    uint8_t format = 0;
    uint32_t count = 0;
    if (!in.Get(format) || (format != kStrokeBlobFormat && format != kStrokeBlobFormat1) || !in.Get(count)) {
        repaired = true;
        return strokes;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Stroke stroke;
        uint8_t corners = 0;
        uint32_t points = 0;
        // A count past what is left cannot be true, and is not reserved for.
        if (!in.Get(stroke.colorRGBA) || !in.Get(stroke.width) ||
            (format != kStrokeBlobFormat1 && !in.Get(corners)) || !in.Get(points) || in.Left() / 8 < points) {
            repaired = true;
            break;
        }
        if (corners > static_cast<uint8_t>(StrokeCorners::Sharp)) {
            corners = 0;
            repaired = true;
        }
        stroke.corners = static_cast<StrokeCorners>(corners);
        if (!std::isfinite(stroke.width) || stroke.width <= 0.0f || stroke.width > kMaxSensibleExtent) {
            stroke.width = 3.0f;
            repaired = true;
        }
        stroke.points.resize(points);
        for (StrokePoint& point : stroke.points) {
            in.Get(point.x);
            in.Get(point.y);
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                point = StrokePoint{};
                repaired = true;
            }
        }
        if (format == kStrokeBlobFormat1) {
            stroke.corners = CornersOfAFormat1Stroke(stroke.points);
        }
        strokes.push_back(std::move(stroke));
    }
    return strokes;
}

}  // namespace

// ================= Opening =================

LibraryStore::LibraryStore(std::filesystem::path file) : file_(std::move(file)) {}

LibraryStore::~LibraryStore() { Close(); }

void LibraryStore::Close() {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

LibraryStore::OpenResult LibraryStore::Open() {
    if (!openResult_.has_value()) {
        openResult_ = TryOpen();
    }
    return *openResult_;
}

bool LibraryStore::Ready() { return Open() == OpenResult::Opened && db_ != nullptr; }

LibraryStore::OpenResult LibraryStore::TryOpen() {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (sqlite3_open_v2(Utf8(file_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
        Close();
        return OpenResult::Unreadable;
    }
    Configure();

    // Whose file it is, which version, and whether there is anything in it
    // at all - a file that does not exist yet opens as an empty database,
    // and so does one of no bytes.
    int64_t applicationId = 0;
    int64_t userVersion = 0;
    int64_t schemaObjects = 0;
    int rc = ReadInt(db_, "PRAGMA application_id", applicationId);
    if (rc == SQLITE_OK) {
        rc = ReadInt(db_, "PRAGMA user_version", userVersion);
    }
    if (rc == SQLITE_OK) {
        rc = ReadInt(db_, "SELECT count(*) FROM sqlite_schema", schemaObjects);
    }
    if (IsDamage(rc)) {
        return SetAsideAndStartOver();
    }
    if (rc != SQLITE_OK) {
        Close();
        return OpenResult::Unreadable;
    }
    if (schemaObjects == 0 && applicationId == 0 && userVersion == 0) {
        if (!CreateSchema()) {
            Close();
            return OpenResult::Unreadable;
        }
        EnterWal();
        return OpenResult::Opened;
    }
    if (applicationId != kApplicationId) {
        return SetAsideAndStartOver();  // some other program's database
    }
    if (userVersion > kFormatVersion) {
        Close();
        return OpenResult::WrittenByANewerVersion;
    }
    // An older version's library is read as it is - every version reads
    // what the ones before it wrote - and is this version's from here on:
    // what is written now, an older build would lose.
    if (userVersion < kFormatVersion &&
        !Exec(db_, ("PRAGMA user_version = " + std::to_string(kFormatVersion)).c_str())) {
        Close();
        return OpenResult::Unreadable;
    }
    Exec(db_, "PRAGMA foreign_keys = ON");
    EnterWal();
    return OpenResult::Opened;
}

void LibraryStore::MoveHereFrom(const std::filesystem::path& former) {
    std::error_code ec;
    if (db_ != nullptr || openResult_.has_value() || former.empty() || former == file_ ||
        std::filesystem::exists(file_, ec) || ec || !std::filesystem::exists(former, ec) || ec) {
        return;
    }
    // An open and a close play back a journal a crash left, or move a WAL
    // into the file; the first read is what does it.
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(Utf8(former).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
            int64_t version = 0;
            ReadInt(db, "PRAGMA user_version", version);
        }
        sqlite3_close_v2(db);
    }
    // Still there - the file could not be written, say - they are part of
    // the library, and it is not moved without them.
    for (const char* suffix : {"-journal", "-wal"}) {
        std::filesystem::path left = former;
        left += suffix;
        if (std::filesystem::exists(left, ec) || ec) {
            file_ = former;
            return;
        }
    }
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (!ec) {
        std::filesystem::rename(former, file_, ec);
        if (!ec) {
            return;
        }
        // Another drive - a roaming profile redirected to a server share:
        // copied, and made the library only once the copy is whole.
        std::filesystem::path moving = file_;
        moving += ".moving";
        std::filesystem::copy_file(former, moving, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::rename(moving, file_, ec);
        }
        if (!ec) {
            // Left where it was if it cannot be removed: the one here is the
            // library from now on.
            std::filesystem::remove(former, ec);
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(moving, ignored);
    }
    file_ = former;
}

void LibraryStore::Configure() {
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    // Held for this store alone from the first read to the close: nobody
    // else reads or writes the file meanwhile, another copy of the app on
    // another computer sharing the folder included - it is refused at its
    // start instead of writing over this one's changes. Before the first
    // read, too, for WAL: then SQLite keeps the WAL's index in this
    // process's memory instead of in a -shm file shared with others, which
    // does not work on a network drive.
    if (!lockShared_) {
        Exec(db_, "PRAGMA locking_mode = EXCLUSIVE");
    }
}

void LibraryStore::EnterWal() {
    std::string mode;
    {
        Statement statement(db_, "PRAGMA journal_mode = WAL");
        if (statement.Step() == SQLITE_ROW) {
            mode = statement.Text(0);
        }
    }
    wal_ = mode == "wal";
    if (!wal_) {
        return;
    }
    // A commit is in the WAL whole, each frame checksummed, and the WAL is
    // flushed before a checkpoint writes any of it into the file - so the
    // file is always a whole library. What a commit does not do is wait for
    // the disk: after a power cut, what was committed since the last
    // checkpoint may be gone.
    Exec(db_, "PRAGMA synchronous = NORMAL");
    ReadInt(db_, "PRAGMA page_size", pageSize_);
    // Setting a hook of our own also turns off SQLite's checkpoint at every
    // thousand pages, which would come inside a commit, on the render
    // thread - see Checkpoint for when this store makes one instead.
    sqlite3_wal_hook(db_, &LibraryStore::OnWalCommit, this);
    // A WAL left by a copy that did not close - a crash - is read at the
    // open and not moved into the file yet.
    walFrames_ = -1;
}

int LibraryStore::OnWalCommit(void* self, sqlite3* /*db*/, const char* /*name*/, int frames) {
    static_cast<LibraryStore*>(self)->walFrames_ = frames;
    return SQLITE_OK;
}

bool LibraryStore::Checkpoint() {
    if (db_ == nullptr || !wal_ || walFrames_ == 0) {
        return true;
    }
    TimelineScope marked(TimelineMark::Checkpoint);
    marked.SetBytes(UncheckpointedBytes());
    // TRUNCATE rather than the default: the WAL is left empty on disk too,
    // not the size of everything written since the open.
    if (sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }
    walFrames_ = 0;
    return true;
}

int64_t LibraryStore::UncheckpointedBytes() const {
    return walFrames_ < 0 ? -1 : walFrames_ * (pageSize_ + kWalFrameHeaderBytes);
}

bool LibraryStore::CreateSchema() {
    // Before the first table, or it has no effect: deleted rows give their
    // pages back to the file at the next Load's incremental_vacuum rather
    // than leaving it the size of the largest library it ever held.
    Exec(db_, "PRAGMA auto_vacuum = INCREMENTAL");
    const std::string versions = "PRAGMA application_id = " + std::to_string(kApplicationId) +
                                 "; PRAGMA user_version = " + std::to_string(kFormatVersion) + ";";
    if (!Exec(db_, "BEGIN IMMEDIATE")) {
        return false;
    }
    if (!Exec(db_, kSchema) || !Exec(db_, versions.c_str()) || !Exec(db_, "COMMIT")) {
        Exec(db_, "ROLLBACK");
        return false;
    }
    Exec(db_, "PRAGMA foreign_keys = ON");
    unwritten_ = true;
    return true;
}

LibraryStore::OpenResult LibraryStore::SetAsideAndStartOver() {
    Close();
    // "library-unreadable-2026-09-25-17-44-03.db", beside the library.
    std::string stamp = TimestampName();
    std::replace(stamp.begin(), stamp.end(), ' ', '-');
    std::replace(stamp.begin(), stamp.end(), ':', '-');
    const std::filesystem::path aside =
        file_.parent_path() / (file_.stem().string() + "-unreadable-" + stamp + file_.extension().string());
    std::error_code ec;
    std::filesystem::rename(file_, aside, ec);
    if (ec) {
        return OpenResult::Unreadable;
    }
    // A journal or a WAL left beside it belongs to it, and would be played
    // back into the new file otherwise.
    for (const char* suffix : {"-journal", "-wal"}) {
        std::filesystem::path left = file_;
        left += suffix;
        if (std::filesystem::exists(left, ec)) {
            std::filesystem::path asideLeft = aside;
            asideLeft += suffix;
            std::filesystem::rename(left, asideLeft, ec);
        }
    }
    setAsideAs_ = aside;
    if (sqlite3_open_v2(Utf8(file_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
        Close();
        return OpenResult::Unreadable;
    }
    Configure();
    if (!CreateSchema()) {
        Close();
        return OpenResult::Unreadable;
    }
    EnterWal();
    return OpenResult::Opened;
}

// ================= Loading =================

std::optional<CanvasManagerSnapshot> LibraryStore::Load() {
    if (!Ready() || unwritten_) {
        return std::nullopt;
    }
    CanvasManagerSnapshot snapshot;
    LibraryChanges repairs;
    std::optional<uint64_t> currentFolder;
    std::optional<uint64_t> currentCanvas;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    // One read transaction, so the library is read as of one moment.
    const auto read = [&]() -> int {
        {
            Statement statement(db_, "SELECT id, position, name, created_at, deleted_at FROM folders "
                                     "ORDER BY position, id");
            int rc = SQLITE_ROW;
            while ((rc = statement.Step()) == SQLITE_ROW) {
                Folder folder;
                folder.id = static_cast<uint64_t>(statement.Int(0));
                folder.name = statement.Text(2);
                folder.createdAt = statement.Int(3);
                bool repaired = false;
                folder.deletedAt = DeletionStamp(statement.Int(4), now, repaired);
                repairs.foldersAndCanvases = repairs.foldersAndCanvases || repaired;
                snapshot.folders.push_back(std::move(folder));
            }
            if (rc != SQLITE_DONE) {
                return sqlite3_extended_errcode(db_);
            }
        }
        std::unordered_map<uint64_t, size_t> canvasIndex;
        {
            Statement statement(db_, "SELECT id, folder_id, position, name, created_at, deleted_at FROM canvases "
                                     "ORDER BY position, id");
            int rc = SQLITE_ROW;
            while ((rc = statement.Step()) == SQLITE_ROW) {
                Canvas canvas;
                canvas.id = static_cast<uint64_t>(statement.Int(0));
                canvas.folderId = static_cast<uint64_t>(statement.Int(1));
                canvas.name = statement.Text(3);
                canvas.createdAt = statement.Int(4);
                bool repaired = false;
                canvas.deletedAt = DeletionStamp(statement.Int(5), now, repaired);
                repairs.foldersAndCanvases = repairs.foldersAndCanvases || repaired;
                canvasIndex[canvas.id] = snapshot.canvases.size();
                snapshot.canvases.push_back(std::move(canvas));
            }
            if (rc != SQLITE_DONE) {
                return sqlite3_extended_errcode(db_);
            }
        }
        {
            Statement statement(db_, "SELECT items.id, items.canvas_id, items.position, items.record, items.strokes, "
                                     "pictures.item_id IS NOT NULL FROM items "
                                     "LEFT JOIN pictures ON pictures.item_id = items.id "
                                     "ORDER BY items.canvas_id, items.position, items.id");
            int rc = SQLITE_ROW;
            while ((rc = statement.Step()) == SQLITE_ROW) {
                const uint64_t canvasId = static_cast<uint64_t>(statement.Int(1));
                const auto canvas = canvasIndex.find(canvasId);
                if (canvas == canvasIndex.end()) {
                    continue;  // the foreign key says this cannot be
                }
                Item item;
                item.id = static_cast<uint64_t>(statement.Int(0));
                bool repaired = false;
                const std::string record = statement.Text(3);
                const std::vector<uint8_t> strokes = statement.Blob(4);
                ReadItemRecord(record, now, item, repaired);
                item.strokes = ReadStrokes(strokes, repaired);
                item.picture.stored = statement.Int(5) != 0;
                if (repaired) {
                    repairs.items.push_back(item.id);
                }
                snapshot.canvases[canvas->second].items.push_back(std::move(item));
            }
            if (rc != SQLITE_DONE) {
                return sqlite3_extended_errcode(db_);
            }
        }
        {
            Statement statement(db_, "SELECT key, value FROM meta");
            int rc = SQLITE_ROW;
            while ((rc = statement.Step()) == SQLITE_ROW) {
                const std::string key = statement.Text(0);
                if (key == "current_folder") {
                    currentFolder = static_cast<uint64_t>(statement.Int(1));
                } else if (key == "current_canvas") {
                    currentCanvas = static_cast<uint64_t>(statement.Int(1));
                }
            }
            if (rc != SQLITE_DONE) {
                return sqlite3_extended_errcode(db_);
            }
        }
        return SQLITE_OK;
    };
    int rc = Exec(db_, "BEGIN") ? read() : sqlite3_extended_errcode(db_);
    Exec(db_, "COMMIT");
    if (rc != SQLITE_OK) {
        // Damage partway is what damage at the open is. Anything else - the
        // file held past the busy timeout, a read that failed - leaves a
        // library this store could not read, and not a first run: nothing
        // is read from it or written over it.
        if (IsDamage(rc)) {
            openResult_ = SetAsideAndStartOver();
        } else {
            Close();
            openResult_ = OpenResult::Unreadable;
        }
        return std::nullopt;
    }
    // Nothing at all, not even the rows saying which canvas is current,
    // which every whole write leaves and nothing takes out: a library made
    // and never written. The schema is committed by the Open that makes it,
    // before a first run's first write, and should that write fail, this
    // is what is left - which loaded as a library someone had emptied: no
    // folder, no canvas and no welcome, at every start from then on. A
    // first run still, and written whole by whatever write comes next.
    if (snapshot.folders.empty() && snapshot.canvases.empty() && !currentFolder && !currentCanvas) {
        unwritten_ = true;
        return std::nullopt;
    }

    // Which folder and canvas are current. None - no canvas on screen, or
    // no folder browsed - is a state the model has, and keeps: deleting a
    // folder's last canvas leaves nothing on screen and that folder
    // browsed (see CanvasManager::SettleOffDeleted). It was taken for a
    // pointer naming nothing, and a restart opened on the library's first
    // canvas - another folder's, or one in the trash - and wrote that
    // back. Only a pointer naming nothing - no row, or an id the library
    // does not hold - opens on something else: the first canvas and folder
    // not deleted, where the model would move to, and is written back so.
    const auto live = [](const Folder& f) { return f.deletedAt == 0; };
    const auto liveFolder = [&](FolderId id) {
        return std::any_of(snapshot.folders.begin(), snapshot.folders.end(),
                           [&](const Folder& f) { return f.id == id && live(f); });
    };
    const auto namesNothing = [](const std::optional<uint64_t>& pointer, const auto& things) {
        if (!pointer.has_value()) {
            return true;
        }
        return *pointer != 0 &&
               std::none_of(things.begin(), things.end(), [&](const auto& thing) { return thing.id == *pointer; });
    };
    snapshot.currentCanvasId = currentCanvas.value_or(0);
    if (namesNothing(currentCanvas, snapshot.canvases)) {
        const auto first = std::find_if(snapshot.canvases.begin(), snapshot.canvases.end(), [&](const Canvas& c) {
            return c.deletedAt == 0 && liveFolder(c.folderId);
        });
        snapshot.currentCanvasId = first != snapshot.canvases.end() ? first->id : 0;
    }
    snapshot.currentFolderId = currentFolder.value_or(0);
    if (namesNothing(currentFolder, snapshot.folders)) {
        const auto current = std::find_if(snapshot.canvases.begin(), snapshot.canvases.end(),
                                          [&](const Canvas& c) { return c.id == snapshot.currentCanvasId; });
        const auto first = std::find_if(snapshot.folders.begin(), snapshot.folders.end(), live);
        snapshot.currentFolderId = current != snapshot.canvases.end() ? current->folderId
                                   : first != snapshot.folders.end()  ? first->id
                                                                      : 0;
    }

    repairs.current = currentFolder != snapshot.currentFolderId || currentCanvas != snapshot.currentCanvasId;

    // What was repaired, written back as it now reads - best effort: a
    // write that fails leaves a row that is repaired again next time.
    Write(LibraryView{snapshot.folders, snapshot.canvases, snapshot.currentFolderId, snapshot.currentCanvasId},
          repairs);
    // Housekeeping, and a failure costs only space: a picture whose snippet
    // is not in the library - which only a library written before every
    // change was one transaction could hold - and the pages deleted rows
    // left, given back to the file system.
    Exec(db_, "DELETE FROM pictures WHERE item_id NOT IN (SELECT id FROM items)");
    Exec(db_, "PRAGMA incremental_vacuum");
    return snapshot;
}

// ================= Writing =================

bool LibraryStore::Write(const LibraryView& view, const LibraryChanges& changes, const PictureWrites& pictures) {
    if (changes.Empty() && pictures.Empty()) {
        return true;
    }
    // A library not yet written whole is written whole by the next write,
    // whatever it names: after a first run's Save failed, a snippet written
    // alone would be on a canvas the file does not have, and the foreign
    // key refused it - and every write after it, until one happened to
    // write the folders and canvases.
    LibraryChanges everything;
    everything.everything = true;
    if (!Ready()) {
        return false;
    }
    {
        TimelineScope marked(TimelineMark::Commit);
        WriteTransaction transaction(db_);
        if (!transaction.Begun() || !WriteRows(view, unwritten_ ? everything : changes, pictures) ||
            !transaction.Commit()) {
            return false;
        }
        marked.SetBytes(UncheckpointedBytes());
    }
    // What this store wrote is a library now, and a Load of it is not a
    // first run.
    unwritten_ = false;
    // An overlay left up long enough to fill the WAL - a capture is a few
    // megabytes - is not left to fill it without end. A checkpoint that
    // fails loses nothing, and is tried again at the next write.
    if (UncheckpointedBytes() >= checkpointAtBytes_) {
        Checkpoint();
    }
    return true;
}

bool LibraryStore::Save(const LibraryView& view) {
    LibraryChanges everything;
    everything.everything = true;
    return Write(view, everything);
}

bool LibraryStore::WriteRows(const LibraryView& view, const LibraryChanges& changes, const PictureWrites& pictures) {
    const bool all = changes.everything;
    // The rows the library has, by id - what a row absent from the view is
    // found among. Nothing for a read that stopped short: the rows past
    // where it stopped were left in, and the write committed without them
    // taken out.
    const auto idsIn = [this](const char* sql) -> std::optional<std::unordered_set<uint64_t>> {
        std::unordered_set<uint64_t> ids;
        Statement select(db_, sql);
        int rc = SQLITE_ROW;
        while ((rc = select.Step()) == SQLITE_ROW) {
            ids.insert(static_cast<uint64_t>(select.Int(0)));
        }
        if (rc != SQLITE_DONE) {
            return std::nullopt;
        }
        return ids;
    };
    const auto removeAbsent = [this](const char* sql, std::unordered_set<uint64_t> absent) {
        Statement remove(db_, sql);
        for (const uint64_t id : absent) {
            remove.Bind(1, id);
            if (!remove.Run()) {
                return false;
            }
        }
        return true;
    };

    // What holds a thing before the thing: a canvas's folder has to be there
    // for the canvas to be written, and a snippet's canvas for the snippet.
    // What is gone goes last, after every snippet has been written where it
    // is now: a canvas taken out takes what is still on it with it, which a
    // snippet moved off it in the same write must not be.
    const bool layout = all || changes.foldersAndCanvases;
    std::unordered_set<uint64_t> absentFolders;
    std::unordered_set<uint64_t> absentCanvases;
    if (layout) {
        std::optional<std::unordered_set<uint64_t>> folders = idsIn("SELECT id FROM folders");
        std::optional<std::unordered_set<uint64_t>> canvases = idsIn("SELECT id FROM canvases");
        if (!folders || !canvases) {
            return false;
        }
        absentFolders = std::move(*folders);
        absentCanvases = std::move(*canvases);
        Statement folder(db_, "INSERT INTO folders (id, position, name, created_at, deleted_at) "
                              "VALUES (?1, ?2, ?3, ?4, ?5) ON CONFLICT (id) DO UPDATE SET "
                              "position = excluded.position, name = excluded.name, "
                              "created_at = excluded.created_at, deleted_at = excluded.deleted_at");
        for (size_t i = 0; i < view.folders.size(); ++i) {
            const Folder& f = view.folders[i];
            folder.Bind(1, f.id);
            folder.Bind(2, static_cast<int64_t>(i));
            folder.Bind(3, f.name);
            folder.Bind(4, f.createdAt);
            folder.Bind(5, f.deletedAt);
            if (!folder.Run()) {
                return false;
            }
            absentFolders.erase(f.id);
        }
        Statement canvas(db_, "INSERT INTO canvases (id, folder_id, position, name, created_at, deleted_at) "
                              "VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT (id) DO UPDATE SET "
                              "folder_id = excluded.folder_id, position = excluded.position, "
                              "name = excluded.name, created_at = excluded.created_at, "
                              "deleted_at = excluded.deleted_at");
        for (size_t i = 0; i < view.canvases.size(); ++i) {
            const Canvas& c = view.canvases[i];
            canvas.Bind(1, c.id);
            canvas.Bind(2, c.folderId);
            canvas.Bind(3, static_cast<int64_t>(i));
            canvas.Bind(4, c.name);
            canvas.Bind(5, c.createdAt);
            canvas.Bind(6, c.deletedAt);
            if (!canvas.Run()) {
                return false;
            }
            absentCanvases.erase(c.id);
        }
    }

    // Where every snippet is in the view, to write it from.
    struct Place {
        const Canvas* canvas = nullptr;
        size_t index = 0;
    };
    std::unordered_map<uint64_t, Place> places;
    for (const Canvas& c : view.canvases) {
        for (size_t i = 0; i < c.items.size(); ++i) {
            places.emplace(c.items[i].id, Place{&c, i});
        }
    }
    {
        Statement remove(db_, "DELETE FROM items WHERE id = ?1");
        for (const uint64_t id : changes.erasedItems) {
            remove.Bind(1, id);
            if (!remove.Run()) {
                return false;
            }
        }
    }
    // The snippets written whole, and the order of every canvas one of them
    // is on: a place written alone could land beside a stale one, where an
    // earlier write took a snippet out from between them.
    std::unordered_set<uint64_t> orders(changes.itemOrders.begin(), changes.itemOrders.end());
    {
        Statement upsert(db_, "INSERT INTO items (id, canvas_id, position, record, strokes) "
                              "VALUES (?1, ?2, ?3, ?4, ?5) ON CONFLICT (id) DO UPDATE SET "
                              "canvas_id = excluded.canvas_id, position = excluded.position, "
                              "record = excluded.record, strokes = excluded.strokes");
        const auto write = [&](const Item& item, const Place& place) {
            const std::string record = ItemRecord(item);
            const std::vector<uint8_t> strokes = StrokesBlob(item.strokes);
            upsert.Bind(1, item.id);
            upsert.Bind(2, place.canvas->id);
            upsert.Bind(3, static_cast<int64_t>(place.index));
            upsert.Bind(4, record);
            upsert.BindBlob(5, strokes.data(), strokes.size());
            orders.insert(place.canvas->id);
            return upsert.Run();
        };
        if (all) {
            for (const auto& [id, place] : places) {
                if (!write(place.canvas->items[place.index], place)) {
                    return false;
                }
            }
        } else {
            for (const uint64_t id : changes.items) {
                const auto place = places.find(id);
                if (place != places.end() && !write(place->second.canvas->items[place->second.index], place->second)) {
                    return false;
                }
            }
        }
    }
    {
        Statement move(db_, "UPDATE items SET canvas_id = ?2, position = ?3 WHERE id = ?1");
        for (const Canvas& c : view.canvases) {
            if (orders.count(c.id) == 0) {
                continue;
            }
            for (size_t i = 0; i < c.items.size(); ++i) {
                move.Bind(1, c.items[i].id);
                move.Bind(2, c.id);
                move.Bind(3, static_cast<int64_t>(i));
                if (!move.Run()) {
                    return false;
                }
            }
        }
    }
    if (all) {
        std::optional<std::unordered_set<uint64_t>> absent = idsIn("SELECT id FROM items");
        if (!absent) {
            return false;
        }
        for (const auto& [id, place] : places) {
            absent->erase(id);
        }
        if (!removeAbsent("DELETE FROM items WHERE id = ?1", std::move(*absent))) {
            return false;
        }
    }
    // What the library no longer holds, with what is still on it: the
    // foreign keys take a canvas's snippets, and the trigger their pictures.
    if (layout && (!removeAbsent("DELETE FROM canvases WHERE id = ?1", std::move(absentCanvases)) ||
                   !removeAbsent("DELETE FROM folders WHERE id = ?1", std::move(absentFolders)))) {
        return false;
    }

    for (const NewPicture& picture : pictures.captured) {
        if (!SaveImage(picture.itemId, picture.pixelsRGBA, picture.width, picture.height)) {
            return false;
        }
    }
    {
        Statement copy(db_, "INSERT OR REPLACE INTO pictures (item_id, width, height, pixels, thumbnail) "
                            "SELECT ?2, width, height, pixels, thumbnail FROM pictures WHERE item_id = ?1");
        for (const auto& [from, to] : pictures.copies) {
            copy.Bind(1, from);
            copy.Bind(2, to);
            if (!copy.Run()) {
                return false;
            }
        }
    }

    if (all || changes.current) {
        Statement upsert(db_, "INSERT INTO meta (key, value) VALUES (?1, ?2) "
                              "ON CONFLICT (key) DO UPDATE SET value = excluded.value");
        upsert.Bind(1, std::string_view("current_folder"));
        upsert.Bind(2, view.currentFolderId);
        if (!upsert.Run()) {
            return false;
        }
        upsert.Bind(1, std::string_view("current_canvas"));
        upsert.Bind(2, view.currentCanvasId);
        if (!upsert.Run()) {
            return false;
        }
    }
    return true;
}

// ================= Pictures =================
// ================= Pictures =================

bool LibraryStore::SaveImage(uint64_t itemId, const uint8_t* pixelsRGBA, int width, int height) {
    if (!Ready() || pixelsRGBA == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> thumbnail;
    {
        TimelineScope marked(TimelineMark::Encode);
        pixels = EncodeQoi(pixelsRGBA, width, height);
        if (pixels.empty()) {
            return false;
        }
        // Taken from the caller's own pixels rather than through a
        // DecodedImage, which would mean copying eight megabytes to make
        // forty kilobytes. A thumbnail that could not be made is left out;
        // the Overview falls back to the picture itself.
        const DecodedImage small = DownscaleToFit(pixelsRGBA, width, height, kThumbnailMaxExtent);
        thumbnail = EncodeQoi(small.pixelsRGBA.data(), small.width, small.height);
        marked.SetBytes(static_cast<int64_t>(pixels.size() + thumbnail.size()));
    }
    Statement insert(db_, "INSERT OR REPLACE INTO pictures (item_id, width, height, pixels, thumbnail) "
                          "VALUES (?1, ?2, ?3, ?4, ?5)");
    insert.Bind(1, itemId);
    insert.Bind(2, width);
    insert.Bind(3, height);
    insert.BindBlob(4, pixels.data(), pixels.size());
    if (!thumbnail.empty()) {
        insert.BindBlob(5, thumbnail.data(), thumbnail.size());  // unbound, it is NULL
    }
    return insert.Run();
}

std::optional<DecodedImage> LibraryStore::LoadPictureColumn(uint64_t itemId, const char* column) {
    if (!Ready()) {
        return std::nullopt;
    }
    TimelineScope marked(TimelineMark::ReadPicture);
    const std::string sql = std::string("SELECT ") + column + " FROM pictures WHERE item_id = ?1";
    Statement select(db_, sql.c_str());
    select.Bind(1, itemId);
    if (select.Step() != SQLITE_ROW || select.IsNull(0)) {
        return std::nullopt;
    }
    const std::vector<uint8_t> bytes = select.Blob(0);
    marked.SetBytes(static_cast<int64_t>(bytes.size()));
    return DecodeQoi(bytes.data(), bytes.size());
}

std::optional<DecodedImage> LibraryStore::LoadImage(uint64_t itemId) { return LoadPictureColumn(itemId, "pixels"); }

std::optional<DecodedImage> LibraryStore::LoadThumbnail(uint64_t itemId) {
    return LoadPictureColumn(itemId, "thumbnail");
}

bool LibraryStore::HasImage(uint64_t itemId) {
    if (!Ready()) {
        return false;
    }
    Statement select(db_, "SELECT 1 FROM pictures WHERE item_id = ?1");
    select.Bind(1, itemId);
    return select.Step() == SQLITE_ROW;
}

}  // namespace sz::core::persistence
