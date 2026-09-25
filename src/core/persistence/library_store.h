#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "core/canvas/canvas_manager.h"
#include "core/persistence/image_codec.h"

struct sqlite3;

namespace sz::core::persistence {

// The library - folders, canvases, snippets and their pictures - in one
// SQLite file, and the sole persistence boundary for everything the overlay
// shows: there is no save or load anywhere else in the app (see Session's
// debounced autosave, which is what calls Save).
//
// One file, and every write to it is a transaction: a save lands whole or
// not at all, whatever stops it partway - a crash, a power cut, a disk
// that fills up. There is no state in between for a later start to find
// and reconcile, which is what the directory tree this replaces spent most
// of its code on (see docs/ARCHITECTURE.md, "Persistence").
//
// Tables (see the .cpp for the schema):
//   meta      - which folder and canvas are current
//   folders   - id, place in the list, name, when made and deleted
//   canvases  - the same, and which folder each is in
//   items     - which canvas each snippet is on and where in its stack, its
//               strokes as one packed blob, and everything else about it as
//               a JSON record
//   pictures  - a snippet's captured pixels as QOI, and a thumbnail of them
//
// Pictures are keyed by their snippet and live in the same file, so a
// snippet and its picture cannot disagree about where either is, and
// deleting the one deletes the other (a trigger does it).
//
// No OS dependency; fully exercised by linux-tests.
class LibraryStore {
public:
    // `file` need not exist, nor its directory: both are made at the first
    // Open.
    explicit LibraryStore(std::filesystem::path file);
    ~LibraryStore();
    LibraryStore(const LibraryStore&) = delete;
    LibraryStore& operator=(const LibraryStore&) = delete;

    // The shape of the library this build reads and writes, as the file's
    // user_version. It goes up once per release that changes what is
    // written - a field an older build would drop, a new table - and a
    // version covers everything since the release before it.
    // 1: the first library in a database.
    static constexpr int kFormatVersion = 1;

    // The longest edge a thumbnail is stored at. A canvas tile in the
    // Overview is 200x130 and an item inside one is smaller still; what this
    // buys is a ~40 KB picture that decodes in well under a millisecond
    // against the 8 MB a fullscreen capture decodes to.
    static constexpr int kThumbnailMaxExtent = 256;

    // What opening the file found. Everything below opens it on first use,
    // so this only needs calling to hear the answer - see
    // TrayController::Initialize, which refuses to start on either of the
    // last two.
    enum class OpenResult {
        Opened,
        // user_version is above kFormatVersion. Such a library is not this
        // build's to open - every row it rewrote would lose what the newer
        // build put there - so the store reads and writes nothing.
        WrittenByANewerVersion,
        // The file is there and could not be opened or read: another
        // program holding it, or not ours to read. Nothing is read or
        // written, so that a start over it cannot write an empty library
        // where it was.
        Unreadable,
    };
    OpenResult Open();
    const std::filesystem::path& File() const { return file_; }
    // Where a file that was not a library this store could read - not a
    // SQLite database, a damaged one, or someone else's - was set aside
    // when Open found it, so that a new library could start in its place
    // without losing it. Empty when nothing was.
    const std::filesystem::path& SetAsideAs() const { return setAsideAs_; }

    // The library, or nullopt when there is none to load: the file was made
    // by this Open (a first run), or could not be read. A library someone
    // emptied loads as an empty snapshot, which is not a first run. A value
    // a row carries that cannot be used - a coordinate that is not finite,
    // a stroke blob cut short - is repaired rather than refused, and the
    // repaired row is written back by the next save.
    std::optional<CanvasManagerSnapshot> Load();

    // Writes what changed since this store last read or wrote the library,
    // in one transaction: every folder and canvas that differs, every
    // snippet whose content or place differs, and the removal of everything
    // the library no longer holds (with its picture). True when it landed;
    // false when it did not, and then nothing did, and the next Save tries
    // all of it again.
    bool Save(const LibraryView& view);
    bool Save(const CanvasManagerSnapshot& snapshot) {
        return Save(LibraryView{snapshot.folders, snapshot.canvases, snapshot.currentFolderId,
                                snapshot.currentCanvasId});
    }

    // How many snippet rows the last Save that landed wrote - for a test to
    // see that an unchanged snippet is not written again.
    size_t ItemsWrittenByLastSave() const { return itemsWrittenByLastSave_; }

    // Stores `pixelsRGBA` (width*height*4 bytes, RGBA8, row-major, top-left
    // origin) as snippet `itemId`'s picture, with a thumbnail, replacing
    // any it had. At once rather than with the next save: a screenshot is
    // the one thing in the library that cannot be made again (see
    // Session::CaptureShotItem). A picture whose snippet is never saved is
    // removed at the next Load.
    bool SaveImage(uint64_t itemId, const uint8_t* pixelsRGBA, int width, int height);
    // Gives `toItemId` a copy of `fromItemId`'s picture, as stored - no
    // decoding. False when there is none to copy.
    bool CopyImage(uint64_t fromItemId, uint64_t toItemId);
    // A snippet's picture, decoded; nullopt when it has none or it cannot
    // be read.
    std::optional<DecodedImage> LoadImage(uint64_t itemId);
    std::optional<DecodedImage> LoadThumbnail(uint64_t itemId);
    // Whether the library holds a picture for `itemId`, without reading it.
    bool HasImage(uint64_t itemId);

    // Copies the library as it is in the file to `file`, which must not
    // exist - the start of a recovery copy (see Session::WriteRecoveryCopy).
    bool WriteCopyTo(const std::filesystem::path& file);

private:
    // What was last written for a folder, canvas or snippet - what Save
    // compares against to know whether a row needs writing.
    struct FolderRow {
        int64_t position = 0;
        std::string name;
        int64_t createdAt = 0;
        int64_t deletedAt = 0;
        bool operator==(const FolderRow&) const = default;
    };
    struct CanvasRow {
        uint64_t folderId = 0;
        int64_t position = 0;
        std::string name;
        int64_t createdAt = 0;
        int64_t deletedAt = 0;
        bool operator==(const CanvasRow&) const = default;
    };
    struct ItemRow {
        uint64_t canvasId = 0;
        int64_t position = 0;
        // See ItemContentHash in the .cpp. 0 for a row read back repaired,
        // which no content hashes to, so that it is written again.
        uint64_t hash = 0;
        bool operator==(const ItemRow&) const = default;
    };

    // Open, and whether the store may be used - opened, not a newer
    // library, and not broken by a Load that failed.
    bool Ready();
    OpenResult TryOpen();
    bool CreateSchema();
    // Moves the file aside (see SetAsideAs) and opens a new one in its
    // place.
    OpenResult SetAsideAndStartOver();
    void Close();
    std::optional<DecodedImage> LoadPictureColumn(uint64_t itemId, const char* column);

    std::filesystem::path file_;
    std::filesystem::path setAsideAs_;
    sqlite3* db_ = nullptr;
    std::optional<OpenResult> openResult_;
    // Whether this Open made the schema, which makes Load a first run.
    bool createdByOpen_ = false;
    // A Load that failed partway: the file is not what this store knows it
    // to be, so nothing is written over it.
    bool broken_ = false;

    std::optional<uint64_t> writtenCurrentFolderId_;
    std::optional<uint64_t> writtenCurrentCanvasId_;
    std::unordered_map<uint64_t, FolderRow> writtenFolders_;
    std::unordered_map<uint64_t, CanvasRow> writtenCanvases_;
    std::unordered_map<uint64_t, ItemRow> writtenItems_;
    size_t itemsWrittenByLastSave_ = 0;
};

}  // namespace sz::core::persistence
