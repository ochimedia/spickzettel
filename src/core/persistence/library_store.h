#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/canvas_manager.h"
#include "core/persistence/image_codec.h"

struct sqlite3;

namespace sz::core::persistence {

// The library - folders, canvases, snippets and their pictures - in one
// SQLite file, and the sole persistence boundary for everything the overlay
// shows: there is no save or load anywhere else in the app. Every change
// the session makes is written here as it is made, in one transaction (see
// Write), so the file always holds what the app holds.
//
// Every write lands whole or not at all, whatever stops it partway - a
// crash, a power cut, a disk that fills up. There is no state in between
// for a later start to find and reconcile, which is what the directory
// tree this replaces spent most of its code on (see docs/ARCHITECTURE.md,
// "Persistence").
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
        // where it was. What Open answers after a Load that could not read
        // the library through, for any reason but damage, as well.
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
    // by this Open (a first run), or could not be read - and then Open says
    // Unreadable from here on, so that the caller can tell the two apart. A
    // damaged one is set aside and a new one started, as Open does. A
    // library someone emptied loads as an empty snapshot, which is not a
    // first run. A value a row carries that cannot be used - a coordinate
    // that is not finite, a stroke blob cut short - is repaired rather than
    // refused, and the repaired row is written back as it now reads.
    std::optional<CanvasManagerSnapshot> Load();

    // Pictures written with a change: a screenshot's pixels (width*height*4
    // bytes, RGBA8, row-major, top-left origin), stored with a thumbnail,
    // and a copy's picture copied from its source's as stored.
    struct NewPicture {
        uint64_t itemId = 0;
        const uint8_t* pixelsRGBA = nullptr;
        int width = 0;
        int height = 0;
    };
    struct PictureWrites {
        std::vector<NewPicture> captured;
        std::vector<std::pair<uint64_t, uint64_t>> copies;  // from, to
        bool Empty() const { return captured.empty() && copies.empty(); }
    };

    // Writes one change to the library, in one transaction: the rows
    // `changes` names, read from `view`, and `pictures`. True when it
    // landed; false when it did not, and then nothing did - the caller puts
    // its model back (see CanvasManager::RollBack). Nothing to write is a
    // write that landed.
    bool Write(const LibraryView& view, const LibraryChanges& changes, const PictureWrites& pictures = {});
    // Writes the whole library: every row, and every one it does not hold
    // taken out. For a library made rather than changed - a test's.
    bool Save(const LibraryView& view);
    bool Save(const CanvasManagerSnapshot& snapshot) {
        return Save(LibraryView{snapshot.folders, snapshot.canvases, snapshot.currentFolderId,
                                snapshot.currentCanvasId});
    }

    // Stores a picture for `itemId` on its own - see NewPicture - replacing
    // any it had.
    bool SaveImage(uint64_t itemId, const uint8_t* pixelsRGBA, int width, int height);
    // A snippet's picture, decoded; nullopt when it has none or it cannot
    // be read.
    std::optional<DecodedImage> LoadImage(uint64_t itemId);
    std::optional<DecodedImage> LoadThumbnail(uint64_t itemId);
    // Whether the library holds a picture for `itemId`, without reading it.
    bool HasImage(uint64_t itemId);

private:
    // The rows of one Write, inside its transaction. False at the first
    // statement that fails.
    bool WriteRows(const LibraryView& view, const LibraryChanges& changes, const PictureWrites& pictures);

    // Open, and whether the store may be used - opened, and neither a
    // newer library nor one that could not be read.
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
};

}  // namespace sz::core::persistence
