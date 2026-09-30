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
// A commit is written ahead to the file's WAL and made durable later, at a
// checkpoint (see Checkpoint), rather than flushed to the disk each time:
// a flush waits for everything the disk has queued, and a commit that
// waited took a frame or several on a busy disk. The file is held for this
// store alone while it is open (see docs/ARCHITECTURE.md, "Persistence").
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
    // Before the first Open: the library at `former` - where builds up to
    // 0.2.0 kept it, in the roaming profile - moved to File(), unless File()
    // is there already. Whatever a crash left beside it, a journal or a
    // WAL, is played into it first, so that it is one file to move; moved
    // to another drive, it is copied and the copy made the library only
    // once it is whole. A library that cannot be moved is opened where it
    // is, and File() says so; the move is tried again at the next start.
    void MoveHereFrom(const std::filesystem::path& former);
    // Where a file that was not a library this store could read - not a
    // SQLite database, a damaged one, or someone else's - was set aside
    // when Open found it, so that a new library could start in its place
    // without losing it. Empty when nothing was.
    const std::filesystem::path& SetAsideAs() const { return setAsideAs_; }

    // The library, or nullopt when there is none to load: the file was made
    // by this Open (a first run), or was made and never written (a first
    // run whose first Save failed, and so a first run still), or could not
    // be read - and then Open says Unreadable from here on, so that the
    // caller can tell these apart. A damaged one is set aside and a new one
    // started, as Open does. A library someone emptied loads as an empty
    // snapshot, which is not a first run. A value a row carries that cannot
    // be used - a coordinate that is not finite, a stroke blob cut short -
    // is repaired rather than refused, and the repaired row is written back
    // as it now reads.
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
    // write that landed. Until a library has been written whole once - a
    // first run's first Save may fail - each write is of the whole of it,
    // whatever `changes` names.
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

    // Makes what was written since the last checkpoint durable: moves it
    // from the WAL into the file and flushes it to the disk. The one flush
    // the store makes, so it can stall on a busy disk - for when nobody is
    // looking, when the overlay goes away (see TrayController::Apply). A
    // write checkpoints by itself once the WAL holds kCheckpointAtBytes, so
    // an overlay left up does not grow it without end. Nothing to do when
    // nothing was written since, or the file is not open. False when the
    // checkpoint could not be made; what it was to move stays in the WAL,
    // as safe as before.
    bool Checkpoint();
    // What the WAL holds that no checkpoint has moved into the file yet, in
    // bytes; 0 when nothing, and -1 when a WAL found at the open may hold
    // something.
    int64_t UncheckpointedBytes() const;
    static constexpr int64_t kCheckpointAtBytes = int64_t{64} << 20;
    void SetCheckpointAtBytesForTesting(int64_t bytes) { checkpointAtBytes_ = bytes; }

    // Whether stores opened from now on hold their file for themselves
    // (the default) or share it with other connections. Tests share it:
    // they read a library back, or make its writes fail, through a
    // connection of their own while the store has it open.
    static void LockSharedForTesting(bool shared) { lockShared_ = shared; }
    static bool LocksSharedForTesting() { return lockShared_; }

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
    // What every connection is set up with, before it first reads the file.
    void Configure();
    // Puts the file in WAL mode, and commits into it unflushed. A file that
    // cannot be keeps the rollback journal, flushed at every commit.
    void EnterWal();
    // SQLite's word after a commit into the WAL: how many frames it holds.
    static int OnWalCommit(void* self, sqlite3* db, const char* name, int frames);
    void Close();
    std::optional<DecodedImage> LoadPictureColumn(uint64_t itemId, const char* column);

    std::filesystem::path file_;
    std::filesystem::path setAsideAs_;
    sqlite3* db_ = nullptr;
    std::optional<OpenResult> openResult_;
    // Whether the file holds a schema that has never been written whole:
    // made by this Open, or found so by Load. Load is then a first run, and
    // Write writes everything.
    bool unwritten_ = false;
    // Whether the file is in WAL mode, and what its WAL holds unmoved - see
    // UncheckpointedBytes.
    bool wal_ = false;
    int64_t walFrames_ = 0;
    int64_t pageSize_ = 4096;
    int64_t checkpointAtBytes_ = kCheckpointAtBytes;
    inline static bool lockShared_ = false;
};

}  // namespace sz::core::persistence
