#include "core/persistence/library_store.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/util/timestamp_name.h"

namespace sz::core::persistence {

namespace {

using nlohmann::json;

// "Sztl", as the file's application_id: what tells a library of ours from
// any other SQLite file that happens to be where the library should be.
constexpr int64_t kApplicationId = 0x537A746C;

// How long a statement waits for a lock another program holds on the file
// before it gives up - a backup tool reading it, say. Short, because writes
// run on the render thread; a change whose write gives up is not made.
constexpr int kBusyTimeoutMs = 250;

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
    // with fresh parameters.
    bool Run() {
        const int rc = Step();
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

json RectJson(const Rect& r) { return json{{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}}; }

void ReadRect(const json& j, const char* key, Rect& out, bool& repaired) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object()) {
        return;
    }
    out.x = FiniteOr(*it, "x", 0.0f, repaired);
    out.y = FiniteOr(*it, "y", 0.0f, repaired);
    out.w = FiniteOr(*it, "w", 0.0f, repaired);
    out.h = FiniteOr(*it, "h", 0.0f, repaired);
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
    return j.dump();
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

void ReadItemRecord(std::string_view text, Item& out, bool& repaired) {
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        repaired = true;
        return;  // a snippet with every field at its default, rather than none
    }
    out.name = Value(j, "name", std::string(), repaired);
    out.createdAt = Value(j, "createdAt", int64_t{0}, repaired);
    out.deletedAt = Value(j, "deletedAt", int64_t{0}, repaired);
    out.hasBackground = Value(j, "hasBackground", false, repaired);
    ReadRect(j, "rect", out.rect, repaired);
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
//   u8  format (1)
//   u32 stroke count, then per stroke:
//       u32 colorRGBA, f32 width, u32 point count, then per point: f32 x, f32 y
static_assert(std::endian::native == std::endian::little, "the stroke blob is written as the host stores it");
constexpr uint8_t kStrokeBlobFormat = 1;

template <typename T>
void Put(std::vector<uint8_t>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

std::vector<uint8_t> StrokesBlob(const std::vector<Stroke>& strokes) {
    size_t size = 1 + 4;
    for (const Stroke& stroke : strokes) {
        size += 12 + stroke.points.size() * 8;
    }
    std::vector<uint8_t> out;
    out.reserve(size);
    out.push_back(kStrokeBlobFormat);
    Put(out, static_cast<uint32_t>(strokes.size()));
    for (const Stroke& stroke : strokes) {
        Put(out, stroke.colorRGBA);
        Put(out, stroke.width);
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

// What the blob holds, as far as it can be read: a blob cut short keeps the
// strokes before the cut, and a value that cannot be used is the default,
// as in a record.
std::vector<Stroke> ReadStrokes(const std::vector<uint8_t>& blob, bool& repaired) {
    std::vector<Stroke> strokes;
    BlobReader in(blob);
    uint8_t format = 0;
    uint32_t count = 0;
    if (!in.Get(format) || format != kStrokeBlobFormat || !in.Get(count)) {
        repaired = true;
        return strokes;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Stroke stroke;
        uint32_t points = 0;
        // A count past what is left cannot be true, and is not reserved for.
        if (!in.Get(stroke.colorRGBA) || !in.Get(stroke.width) || !in.Get(points) ||
            in.Left() / 8 < points) {
            repaired = true;
            break;
        }
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

bool LibraryStore::Ready() { return Open() == OpenResult::Opened && !broken_ && db_ != nullptr; }

LibraryStore::OpenResult LibraryStore::TryOpen() {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (sqlite3_open_v2(Utf8(file_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
        Close();
        return OpenResult::Unreadable;
    }
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);

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
        return OpenResult::Opened;
    }
    if (applicationId != kApplicationId) {
        return SetAsideAndStartOver();  // some other program's database
    }
    if (userVersion > kFormatVersion) {
        Close();
        return OpenResult::WrittenByANewerVersion;
    }
    Exec(db_, "PRAGMA foreign_keys = ON");
    return OpenResult::Opened;
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
    createdByOpen_ = true;
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
    // A journal left beside it belongs to it, and would be played back into
    // the new file otherwise.
    std::filesystem::path journal = file_;
    journal += "-journal";
    if (std::filesystem::exists(journal, ec)) {
        std::filesystem::path asideJournal = aside;
        asideJournal += "-journal";
        std::filesystem::rename(journal, asideJournal, ec);
    }
    setAsideAs_ = aside;
    if (sqlite3_open_v2(Utf8(file_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
            SQLITE_OK ||
        !CreateSchema()) {
        Close();
        return OpenResult::Unreadable;
    }
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    return OpenResult::Opened;
}

// ================= Loading =================

std::optional<CanvasManagerSnapshot> LibraryStore::Load() {
    if (!Ready() || createdByOpen_) {
        return std::nullopt;
    }
    CanvasManagerSnapshot snapshot;
    LibraryChanges repairs;
    std::optional<uint64_t> currentFolder;
    std::optional<uint64_t> currentCanvas;

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
                folder.deletedAt = statement.Int(4);
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
                canvas.deletedAt = statement.Int(5);
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
                ReadItemRecord(record, item, repaired);
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
        if (IsDamage(rc)) {
            openResult_ = SetAsideAndStartOver();
        } else {
            broken_ = true;
        }
        return std::nullopt;
    }

    // Which folder and canvas are current, as far as they still name one -
    // a pointer naming nothing opens on something that exists rather than
    // on nothing, and is written back so.
    snapshot.currentCanvasId = currentCanvas.value_or(0);
    const bool canvasExists = std::any_of(snapshot.canvases.begin(), snapshot.canvases.end(),
                                          [&](const Canvas& c) { return c.id == snapshot.currentCanvasId; });
    if (!canvasExists) {
        snapshot.currentCanvasId = snapshot.canvases.empty() ? 0 : snapshot.canvases.front().id;
    }
    snapshot.currentFolderId = currentFolder.value_or(0);
    const bool folderExists = std::any_of(snapshot.folders.begin(), snapshot.folders.end(),
                                          [&](const Folder& f) { return f.id == snapshot.currentFolderId; });
    if (!folderExists) {
        const auto current = std::find_if(snapshot.canvases.begin(), snapshot.canvases.end(),
                                          [&](const Canvas& c) { return c.id == snapshot.currentCanvasId; });
        snapshot.currentFolderId = current != snapshot.canvases.end() ? current->folderId
                                   : snapshot.folders.empty()        ? 0
                                                                     : snapshot.folders.front().id;
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
    if (!Ready() || !Exec(db_, "BEGIN IMMEDIATE")) {
        return false;
    }
    if (!WriteRows(view, changes, pictures) || !Exec(db_, "COMMIT")) {
        Exec(db_, "ROLLBACK");
        return false;
    }
    // What this store wrote is a library now, and a Load of it is not a
    // first run.
    createdByOpen_ = false;
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
    // found among.
    const auto idsIn = [this](const char* sql) {
        std::unordered_set<uint64_t> ids;
        Statement select(db_, sql);
        while (select.Step() == SQLITE_ROW) {
            ids.insert(static_cast<uint64_t>(select.Int(0)));
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
        absentFolders = idsIn("SELECT id FROM folders");
        absentCanvases = idsIn("SELECT id FROM canvases");
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
        std::unordered_set<uint64_t> absent = idsIn("SELECT id FROM items");
        for (const auto& [id, place] : places) {
            absent.erase(id);
        }
        if (!removeAbsent("DELETE FROM items WHERE id = ?1", std::move(absent))) {
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
    const std::vector<uint8_t> pixels = EncodeQoi(pixelsRGBA, width, height);
    if (pixels.empty()) {
        return false;
    }
    // Taken from the caller's own pixels rather than through a
    // DecodedImage, which would mean copying eight megabytes to make forty
    // kilobytes. A thumbnail that could not be made is left out; the
    // Overview falls back to the picture itself.
    const DecodedImage small = DownscaleToFit(pixelsRGBA, width, height, kThumbnailMaxExtent);
    const std::vector<uint8_t> thumbnail = EncodeQoi(small.pixelsRGBA.data(), small.width, small.height);
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
    const std::string sql = std::string("SELECT ") + column + " FROM pictures WHERE item_id = ?1";
    Statement select(db_, sql.c_str());
    select.Bind(1, itemId);
    if (select.Step() != SQLITE_ROW || select.IsNull(0)) {
        return std::nullopt;
    }
    const std::vector<uint8_t> bytes = select.Blob(0);
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
